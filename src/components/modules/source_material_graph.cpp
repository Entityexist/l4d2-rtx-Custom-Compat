#include "std_include.hpp"
#include "source_material_graph.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

namespace components::source_material_graph
{
    namespace
    {
        struct node
        {
            std::string key;
            std::optional<std::string> value;
            std::vector<node> children;
        };

        std::string normalize_key(std::string value)
        {
            std::replace(value.begin(), value.end(), '\\', '/');
            std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::vector<std::string> tokenize(std::string_view text)
        {
            std::vector<std::string> tokens;
            std::size_t cursor = 0u;
            while (cursor < text.size())
            {
                while (cursor < text.size() &&
                    std::isspace(static_cast<unsigned char>(text[cursor])) != 0)
                    ++cursor;
                if (cursor >= text.size()) break;

                if (cursor + 1u < text.size() && text[cursor] == '/' && text[cursor + 1u] == '/')
                {
                    cursor += 2u;
                    while (cursor < text.size() && text[cursor] != '\n') ++cursor;
                    continue;
                }

                if (text[cursor] == '{' || text[cursor] == '}')
                {
                    tokens.emplace_back(1u, text[cursor++]);
                    continue;
                }

                std::string token;
                if (text[cursor] == '"')
                {
                    ++cursor;
                    while (cursor < text.size())
                    {
                        const char c = text[cursor++];
                        if (c == '"') break;
                        if (c == '\\' && cursor < text.size())
                        {
                            const char escaped = text[cursor];
                            if (escaped == '"' || escaped == '\\')
                            {
                                token.push_back(escaped);
                                ++cursor;
                            }
                            else
                            {
                                // KeyValues paths may use a literal backslash. Preserve it
                                // instead of interpreting every path separator as an escape.
                                token.push_back(c);
                            }
                        }
                        else token.push_back(c);
                    }
                }
                else
                {
                    while (cursor < text.size() &&
                        std::isspace(static_cast<unsigned char>(text[cursor])) == 0 &&
                        text[cursor] != '{' && text[cursor] != '}')
                    {
                        if (cursor + 1u < text.size() && text[cursor] == '/' &&
                            text[cursor + 1u] == '/') break;
                        token.push_back(text[cursor++]);
                    }
                }
                if (!token.empty()) tokens.push_back(std::move(token));
            }
            return tokens;
        }

        std::vector<node> parse_nodes(const std::vector<std::string>& tokens,
            std::size_t& cursor, const bool stop_at_brace)
        {
            std::vector<node> nodes;
            while (cursor < tokens.size())
            {
                if (tokens[cursor] == "}")
                {
                    if (stop_at_brace) ++cursor;
                    break;
                }
                if (tokens[cursor] == "{")
                {
                    ++cursor;
                    const auto anonymous = parse_nodes(tokens, cursor, true);
                    nodes.insert(nodes.end(), anonymous.begin(), anonymous.end());
                    continue;
                }

                node entry;
                entry.key = tokens[cursor++];
                if (cursor >= tokens.size())
                {
                    nodes.push_back(std::move(entry));
                    break;
                }
                if (tokens[cursor] == "{")
                {
                    ++cursor;
                    entry.children = parse_nodes(tokens, cursor, true);
                }
                else if (tokens[cursor] != "}")
                {
                    entry.value = tokens[cursor++];
                }
                nodes.push_back(std::move(entry));
            }
            return nodes;
        }

        void flatten_operations(const std::vector<node>& nodes, const operation_mode mode,
            vmt_document& document)
        {
            for (const auto& entry : nodes)
            {
                const std::string key = normalize_key(entry.key);
                if (key == "proxies")
                {
                    for (const auto& proxy : entry.children)
                    {
                        const std::string name = normalize_key(proxy.key);
                        if (!name.empty() && std::find(document.proxies.begin(),
                            document.proxies.end(), name) == document.proxies.end())
                            document.proxies.push_back(name);
                    }
                    continue;
                }

                if (entry.value)
                {
                    document.operations.push_back({ mode, key, *entry.value });
                    continue;
                }

                const bool conditional = key.starts_with("dx") || key.starts_with("$dx") ||
                    key.starts_with(">=") || key == "hdr" || key == "ldr" ||
                    key == "$hdr" || key == "$ldr";
                if (conditional)
                {
                    document.warnings.push_back("Applied Source conditional block '" + key +
                        "' using the L4D2 DX9 authoring profile");
                    flatten_operations(entry.children, mode, document);
                }
            }
        }

        float clamp01(const float value)
        {
            return std::clamp(value, 0.0f, 1.0f);
        }

        std::string lower_copy(std::string_view text)
        {
            std::string result(text);
            std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
            return result;
        }
    }

    vmt_document parse_vmt(const std::string_view text)
    {
        vmt_document result;
        const auto tokens = tokenize(text);
        if (tokens.empty())
        {
            result.warnings.emplace_back("Empty VMT document");
            return result;
        }

        std::size_t cursor = 0u;
        result.shader = tokens[cursor++];
        result.patch = normalize_key(result.shader) == "patch";
        if (cursor >= tokens.size() || tokens[cursor] != "{")
        {
            result.warnings.emplace_back("VMT root block is missing");
            return result;
        }
        ++cursor;
        const auto root = parse_nodes(tokens, cursor, true);

        if (!result.patch)
        {
            flatten_operations(root, operation_mode::assign, result);
            return result;
        }

        for (const auto& entry : root)
        {
            const std::string key = normalize_key(entry.key);
            if ((key == "include" || key == "$include") && entry.value)
            {
                result.include_path = *entry.value;
            }
            else if (key == "insert")
            {
                flatten_operations(entry.children, operation_mode::insert_if_missing, result);
            }
            else if (key == "replace")
            {
                flatten_operations(entry.children, operation_mode::replace, result);
            }
            else if (key == "proxies")
            {
                flatten_operations({ entry }, operation_mode::assign, result);
            }
            else if (!entry.children.empty() &&
                (key.starts_with("dx") || key.starts_with("$dx") || key.starts_with(">=") ||
                 key == "hdr" || key == "ldr" || key == "$hdr" || key == "$ldr"))
            {
                result.warnings.push_back("Applied Source conditional block '" + key +
                    "' using the L4D2 DX9 authoring profile");
                flatten_operations(entry.children, operation_mode::assign, result);
            }
            else if (entry.value)
            {
                result.operations.push_back({ operation_mode::assign, key, *entry.value });
            }
        }
        if (result.include_path.empty())
            result.warnings.emplace_back("Patch VMT has no include target");
        return result;
    }

    void apply_operations(std::map<std::string, std::string>& effective_values,
        const vmt_document& document)
    {
        for (const auto& operation : document.operations)
        {
            const std::string key = normalize_key(operation.key);
            if (operation.mode == operation_mode::insert_if_missing)
            {
                effective_values.try_emplace(key, operation.value);
            }
            else if (operation.mode == operation_mode::replace)
            {
                // Valve Patch/replace modifies values inherited from the include.
                // A missing target is deliberately not promoted into a new field;
                // authors can use insert for that behavior.
                if (const auto it = effective_values.find(key); it != effective_values.end())
                    it->second = operation.value;
            }
            else
            {
                effective_values[key] = operation.value;
            }
        }
    }

    const char* detail_role_name(const detail_role role)
    {
        switch (role)
        {
        case detail_role::microdetail_albedo: return "Microdetail albedo modulation";
        case detail_role::color_overlay: return "Color overlay";
        case detail_role::emissive: return "Unlit emissive detail";
        case detail_role::dual_pattern: return "Dual-pattern modulation";
        case detail_role::ao_modulation: return "AO/albedo multiplication";
        case detail_role::ssbump_ao: return "SSBump-derived AO";
        case detail_role::unsupported: return "Unsupported L4D2 detail mode";
        default: return "None";
        }
    }

    detail_role classify_l4d2_detail_mode(const int mode)
    {
        switch (mode)
        {
        case 0: return detail_role::microdetail_albedo;
        case 2:
        case 3: return detail_role::color_overlay;
        case 5: return detail_role::emissive;
        case 7: return detail_role::dual_pattern;
        case 8: return detail_role::ao_modulation;
        case 11: return detail_role::ssbump_ao;
        case 1:
        case 4:
        case 6:
        case 9:
        case 10: return detail_role::unsupported;
        default: return detail_role::none;
        }
    }

    specular_result calibrate_specular(const specular_input& input)
    {
        specular_result result;
        const float exponent = std::clamp(input.phong_exponent > 0.0f ?
            input.phong_exponent : 16.0f, 1.0f, 8192.0f);
        result.roughness = std::clamp(std::sqrt(2.0f / (exponent + 2.0f)), 0.035f, 1.0f);

        const float tint = clamp01(input.envmap_tint_luminance);
        const float saturation_guard = std::clamp(input.envmap_saturation, 0.0f, 2.0f);
        const float contrast_gain = 1.0f + std::clamp(input.envmap_contrast, 0.0f, 4.0f) * 0.12f;
        const float boost_gain = std::sqrt(std::clamp(input.phong_boost, 0.0f, 64.0f));
        const float authority = input.has_mask ? 1.0f : 0.65f;

        if (input.phong_enabled)
        {
            result.reflection_weight = clamp01(0.18f * boost_gain * contrast_gain *
                std::max(0.2f, tint) * authority);
            result.workflow = input.exponent_texture
                ? "Phong exponent texture -> roughness; Phong mask/boost -> reflection weight"
                : "Phong exponent -> roughness; Phong mask/boost -> reflection weight";
        }
        else if (input.envmap_enabled)
        {
            result.roughness = std::clamp(0.48f - input.envmap_contrast * 0.045f, 0.08f, 0.92f);
            result.reflection_weight = clamp01(0.28f * contrast_gain *
                std::max(0.2f, tint) * authority * std::max(0.5f, saturation_guard));
            result.workflow = "Envmap mask/tint/contrast -> reflection response; dielectric roughness retained";
        }
        else
        {
            result.reflection_weight = 0.04f;
            result.workflow = "No explicit Source specular model; dielectric defaults";
        }

        result.metallic_hint = input.known_metal ? clamp01(0.65f + result.reflection_weight * 0.35f) : 0.0f;
        result.dielectric_f0 = input.known_metal ? 0.0f :
            std::clamp(0.035f + result.reflection_weight * 0.025f, 0.025f, 0.08f);
        return result;
    }

    emissive_link_result classify_event_emissive_candidate(
        const std::string_view material_name, const bool emission_enabled,
        const std::vector<std::string>& proxies)
    {
        emissive_link_result result;
        if (!emission_enabled) return result;

        for (const auto& proxy : proxies)
        {
            const std::string key = lower_copy(proxy);
            if (key == "animatedtexture" || key == "toggletexture" || key == "sine" ||
                key == "linearramp" || key == "texturescroll" || key == "entityrandom")
            {
                result.candidate = true;
                result.reason = "Source emissive material is controlled by proxy '" + key + "'";
                return result;
            }
        }

        const std::string name = lower_copy(material_name);
        static constexpr std::string_view hints[] = {
            "alarm", "light", "lamp", "led", "glow", "screen", "display", "clock", "button"
        };
        for (const auto hint : hints)
        {
            if (name.find(hint) != std::string::npos)
            {
                result.candidate = true;
                result.reason = "Emissive material name suggests a map event/facing-poly light source ('" +
                    std::string(hint) + "')";
                return result;
            }
        }
        result.reason = "Static emissive material without an event/proxy hint";
        return result;
    }
}
