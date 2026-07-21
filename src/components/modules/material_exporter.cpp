#include "std_include.hpp"
#include "source_material_graph.hpp"
#include "../../source_compat.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <thread>
#include <cctype>
#include <chrono>
#include <deque>
#include <iomanip>
#include <limits>
#include <optional>
#include <numeric>
#include <unordered_map>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace components
{
    namespace
    {
        enum material_var_type_t : unsigned char
        {
            matvar_float = 0,
            matvar_string = 1,
            matvar_vector = 2,
            matvar_texture = 3,
            matvar_int = 4,
            matvar_fourcc = 5,
            matvar_undefined = 6,
            matvar_matrix = 7,
            matvar_material = 8,
        };

        enum class texture_semantic_t
        {
            albedo,
            normal,
            scalar_mask,
            emission,
            height,
        };

        enum class pixel_transform_t
        {
            copy_color,
            normal_xy,
            normal_dxt5nm,
            normal_ssbump,
            scalar_red,
            scalar_alpha,
            scalar_luminance,
            phong_exponent_to_roughness,
            specular_mask_to_roughness,
            alpha_specular_mask_to_roughness,
            albedo_luminance_to_roughness,
            ssbump_energy_to_height,
            emission_color,
            emission_from_alpha,
        };

        enum class channel_provenance_t
        {
            missing,
            native_map,
            converted_map,
            inferred_map,
            constant_fallback,
            rejected,
        };

        enum class material_grade_t
        {
            rejected,
            needs_review,
            usable,
            verified,
        };

        enum class source_shader_family_t
        {
            unknown,
            vertex_lit_model,
            lightmapped_world,
            world_vertex_transition,
            unlit,
            refract_or_glass,
            water,
            decal,
            particle_or_sprite,
            special,
        };

        enum class source_mask_semantics_t
        {
            none,
            dedicated_texture,
            base_texture_alpha,
            normal_map_alpha,
            base_texture_luminance,
            phong_exponent_texture,
            detail_glow,
            emissive_blend,
            fullbright,
            conflicted,
        };

        struct image_quality_s
        {
            bool valid = false;
            bool constant = false;
            bool suspicious = false;
            float minimum = 0.0f;
            float maximum = 0.0f;
            float mean = 0.0f;
            float standard_deviation = 0.0f;
            float normal_mean_length = 0.0f;
            std::uint32_t unique_values = 0;
            std::string status = "not analysed";
        };

        struct material_override_s
        {
            std::optional<float> roughness;
            std::optional<float> metalness;
            std::optional<bool> disable_height;
            std::optional<bool> disable_emission;
            std::optional<bool> invert_normal_y;
            std::optional<bool> force_ssbump;
            std::string normal_source = "auto";
            std::string roughness_source = "auto";
            std::string height_source = "auto";
        };

        struct texture_param_s
        {
            std::string var_name;
            std::string source_name;
            ITexture* texture = nullptr;
            int width = 0;
            int height = 0;
            int frames = 1;
            bool is_normal = false;
            bool is_cube = false;
            bool raw_exported = false;
        };

        struct rgba_image_s
        {
            UINT width = 0;
            UINT height = 0;
            std::vector<std::uint8_t> pixels; // RGBA8
            std::uint8_t alpha_min = 255;
            std::uint8_t alpha_max = 0;
            std::uint8_t red_min = 255;
            std::uint8_t red_max = 0;
        };

        struct sampler_candidate_s
        {
            IDirect3DBaseTexture9* texture = nullptr;
            std::uint32_t hits = 0;
            std::uint32_t first_draw = 0;
            std::uint32_t last_draw = 0;
        };

        struct bound_texture_choice_s
        {
            IDirect3DBaseTexture9* texture = nullptr;
            int slot = -1;
            int candidate_index = -1;
            int score = std::numeric_limits<int>::min();
            std::uint32_t hits = 0;
            D3DSURFACE_DESC desc{};

            explicit operator bool() const { return texture != nullptr; }
        };

        struct channel_export_s
        {
            std::string filename;
            std::string preview_filename;
            std::string source_parameter;
            std::string source_texture_name;
            std::string source_kind = "missing";
            std::string requested_format;
            std::string actual_status;
            channel_provenance_t provenance = channel_provenance_t::missing;
            image_quality_s quality;
            std::vector<std::string> warnings;
            float confidence = 0.0f;
            int sampler_slot = -1;
            int sampler_candidate = -1;
            int match_score = std::numeric_limits<int>::min();
            std::uint32_t source_hits = 0;
            std::uint32_t source_width = 0;
            std::uint32_t source_height = 0;
            bool ready = false;
            bool preview_ready = false;
            bool generated = false;
            bool derived = false;
            bool texconv_used = false;
            bool fallback_dds = false;
            bool rejected = false;
        };

        struct material_record_s
        {
            std::uint64_t remix_hash = 0;
            std::uint64_t bridge_revision = 0;
            std::string canonical_name;
            std::string shader_name;
            std::string texture_group;
            std::string surface_prop;
            std::string category;
            std::string shader_profile = "generic";
            std::string identity_key;
            std::string runtime_vmt;
            std::string export_relative_dir;
            std::vector<texture_param_s> texture_params;
            bool source_vmt_resolved = false;
            bool source_vmt_from_vpk = false;
            bool source_albedo_asset_resolved = false;
            bool asset_backed_material = false;
            std::uint64_t source_vmt_hash = 0u;
            std::uint64_t source_vmt_byte_size = 0u;
            std::uint32_t declared_texture_count = 0u;
            std::uint32_t resolved_texture_asset_count = 0u;
            std::uint32_t missing_texture_asset_count = 0u;
            std::uint32_t live_texture_param_count = 0u;
            std::string source_vmt_status = "not scanned";
            std::string source_asset_grade = "Unresolved";
            std::string resolved_vmt_path;
            std::string source_shader_name;
            std::string source_shader_family = "Unknown";
            std::string specular_semantics = "None";
            std::string emission_semantics = "None";
            std::string alpha_semantics = "Opaque";
            std::string animation_semantics = "Static";
            std::string material_graph_summary = "Unbuilt";
            std::string patch_semantics = "Direct VMT";
            std::string detail_semantics = "None";
            std::string layer_semantics = "Single layer";
            std::string event_emissive_semantics = "Not linked";
            std::string calibrated_specular_workflow = "Not calibrated";
            std::vector<std::string> model_material_search_paths;
            std::vector<std::string> vmt_include_chain;
            std::vector<std::string> vmt_patch_warnings;
            std::map<std::string, std::string> effective_vmt_values;
            std::vector<std::string> semantic_conflicts;
            std::vector<std::string> detected_proxies;
            std::vector<std::string> missing_source_assets;
            std::uint32_t model_candidate_count = 0u;
            std::uint32_t vtf_animated_count = 0u;
            std::uint32_t vtf_alpha_count = 0u;
            std::uint32_t vtf_normal_flag_count = 0u;
            std::uint32_t vtf_ssbump_flag_count = 0u;
            std::uint32_t vtf_srgb_count = 0u;
            std::uint32_t vtf_cubemap_count = 0u;
            std::uint32_t vtf_unsupported_count = 0u;
            bool resolved_via_model_search_path = false;
            bool shader_semantics_valid = false;
            bool animated_or_proxy_driven = false;
            bool unlit_fullbright = false;
            std::array<IDirect3DBaseTexture9*, 16> bound_textures{};
            std::array<std::uint32_t, 16> bound_texture_hits{};
            std::array<IDirect3DBaseTexture9*, 16> contender_textures{};
            std::array<std::uint32_t, 16> contender_texture_hits{};
            std::array<std::vector<sampler_candidate_s>, 16> sampler_candidates{};
            std::uint32_t draw_count = 0;
            float roughness = 0.5f;
            float metallic = 0.0f;
            float inferred_roughness = 0.5f;
            float inferred_metallic = 0.0f;
            float emissive_intensity = 0.0f;
            float phong_exponent = -1.0f;
            float phong_boost = 1.0f;
            float envmap_contrast = 0.0f;
            float envmap_saturation = 1.0f;
            float envmap_tint_luminance = 1.0f;
            float calibrated_roughness = 0.5f;
            float calibrated_reflection_weight = 0.0f;
            float calibrated_dielectric_f0 = 0.04f;
            float calibrated_metallic_hint = 0.0f;
            float confidence = 0.35f;
            bool translucent = false;
            bool alpha_tested = false;
            bool emissive = false;
            bool has_normal = false;
            bool phong_enabled = false;
            bool ssbump = false;
            bool base_alpha_phong_mask = false;
            bool base_alpha_envmap_mask = false;
            bool normal_alpha_envmap_mask = false;
            bool base_luminance_phong_mask = false;
            bool selfillum_enabled = false;
            bool selfillum_envmapmask_alpha = false;
            bool emissiveblend_enabled = false;
            bool lightwarp_enabled = false;
            bool invert_phong_mask = false;
            bool parallax_enabled = false;
            bool envmap_enabled = false;
            bool water_material = false;
            bool glass_material = false;
            bool dual_layer = false;
            bool detail_texture = false;
            bool additive = false;
            bool nocull = false;
            bool dynamic_material = false;
            bool patch_material = false;
            bool patch_include_resolved = false;
            bool detail_composition_ready = false;
            bool world_vertex_transition_preserved = false;
            bool event_emissive_candidate = false;
            float alpha_test_reference = 0.5f;
            float detail_scale = 1.0f;
            float detail_blend_factor = 1.0f;
            int detail_blend_mode = 0;
            bool override_applied = false;
            bool override_disable_height = false;
            bool override_disable_emission = false;
            bool override_invert_normal_y = false;
            bool override_force_ssbump = false;
            std::string override_normal_source = "auto";
            std::string override_roughness_source = "auto";
            std::string override_height_source = "auto";
            std::vector<std::string> warnings;
            std::vector<std::string> ignored_unresolved_texture_params;
            std::array<float, 3> water_tint{ 0.12f, 0.20f, 0.22f };
            bool exported = false;
            bool albedo_exported = false;
            bool albedo_layer1_exported = false;
            bool normal_exported = false;
            bool normal_layer1_exported = false;
            bool blend_modulate_exported = false;
            bool roughness_exported = false;
            bool metallic_exported = false;
            bool height_exported = false;
            bool emissive_exported = false;
            bool opacity_exported = false;
            bool detail_exported = false;
            bool texconv_used = false;
            bool conversion_pending = false;
            std::string conversion_status = "not exported";
            material_grade_t material_grade = material_grade_t::rejected;
            bool auto_activated = false;
            bool quarantined = false;
            std::string activation_reason = "not evaluated";
            channel_export_s albedo_channel;
            channel_export_s albedo_layer1_channel;
            channel_export_s normal_channel;
            channel_export_s normal_layer1_channel;
            channel_export_s blend_modulate_channel;
            channel_export_s roughness_channel;
            channel_export_s metallic_channel;
            channel_export_s height_channel;
            channel_export_s emissive_channel;
            channel_export_s opacity_channel;
            channel_export_s detail_channel;
        };

        std::mutex g_material_mutex;
        std::map<std::uint64_t, material_record_s> g_materials;
        std::atomic<std::uint64_t> g_source_bridge_registry_revision{ 1u };
        std::atomic<std::uint64_t> g_source_bridge_query_count{ 0u };
        std::atomic<std::uint64_t> g_source_bridge_query_hit_count{ 0u };
        std::atomic<std::uint32_t> g_source_bridge_latest_draw_packet{ 0u };
        constexpr std::size_t k_source_bridge_draw_packet_limit = 2048u;
        struct source_bridge_draw_packet_slot_s
        {
            std::uint32_t packet_id = 0u;
            RemixSourceModelDrawInfoV2 packet{};
        };
        // Fixed ring: Studio models can issue thousands of material draws per frame.
        // A std::map/deque packet registry allocated and freed nodes on the render
        // thread, creating the exact periodic hitch this bridge is intended to avoid.
        std::array<source_bridge_draw_packet_slot_s, k_source_bridge_draw_packet_limit>
            g_source_bridge_draw_packets{};
        std::uint32_t g_source_bridge_draw_packet_count = 0u;

        // Direct-mapped per-thread identity cache. With the bridge enabled every
        // material draw reaches capture_draw; rebuilding std::string identities on
        // every pass produced avoidable allocator traffic and visible frame spikes.
        // Source IMaterial pointers are stable within a map, while the generation
        // invalidates pointer reuse across map transitions.
        constexpr std::size_t k_runtime_material_identity_cache_size = 256u;
        struct runtime_material_identity_cache_s
        {
            const IMaterialInternal* material = nullptr;
            std::uint64_t generation = 0u;
            std::uint64_t hash = 0u;
            bool ignored = false;
            bool water_or_refract = false;
            std::string name;
            std::string shader;
            std::string normalized_shader;
        };
        thread_local std::array<runtime_material_identity_cache_s,
            k_runtime_material_identity_cache_size> g_runtime_material_identity_cache{};
        std::atomic<std::uint64_t> g_runtime_material_identity_generation{ 1u };

        struct active_model_draw_s
        {
            bool active = false;
            std::string model_name;
            std::uint64_t model_hash = 0u;
            std::uint64_t instance_hash = 0u;
            std::int32_t entity_index = -1;
            std::int32_t skin = 0;
            std::int32_t body = 0;
            std::int32_t hitbox_set = 0;
            std::uint32_t source_instance = 0u;
            std::uint32_t material_ordinal = 0u;
            std::uint32_t last_packet_id = 0u;
            std::uint32_t last_packet_flags = 0u;
            std::uint32_t draw_class = REMIX_SOURCE_DRAW_UNKNOWN;
        };
        thread_local active_model_draw_s g_active_model_draw;

        // key: mesh-local material name and basename; value: ordered canonical VMT
        // candidates derived from the model's compiled QC $cdmaterials paths.
        std::unordered_map<std::string, std::vector<std::string>> g_model_material_candidates;
        std::unordered_map<std::string, material_override_s> g_material_overrides;
        std::string g_current_map = "menu";
        std::string g_status = "Press 1. Capture Materials to create a fresh list.";
        std::filesystem::path g_export_root;

        std::string lower_slashes(std::string value)
        {
            std::replace(value.begin(), value.end(), '\\', '/');
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            while (value.starts_with("materials/")) value.erase(0, 10);
            if (value.ends_with(".vmt")) value.resize(value.size() - 4);
            return value;
        }

        std::string material_basename(std::string value)
        {
            value = lower_slashes(std::move(value));
            const auto slash = value.find_last_of('/');
            return slash == std::string::npos ? value : value.substr(slash + 1u);
        }

        std::string normalize_cdmaterial_path(std::string value)
        {
            std::replace(value.begin(), value.end(), '\\', '/');
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            while (!value.empty() && value.front() == '/') value.erase(value.begin());
            while (value.starts_with("materials/")) value.erase(0u, 10u);
            while (!value.empty() && value.back() == '/') value.pop_back();
            if (!value.empty()) value.push_back('/');
            return value;
        }

        void append_unique(std::vector<std::string>& values, std::string value,
            const std::size_t limit = 64u)
        {
            if (value.empty() || values.size() >= limit) return;
            if (std::find(values.begin(), values.end(), value) == values.end())
                values.push_back(std::move(value));
        }

        std::string normalize_map(std::string value)
        {
            value = lower_slashes(std::move(value));
            const auto slash = value.find_last_of('/');
            if (slash != std::string::npos) value.erase(0, slash + 1);
            if (value.ends_with(".bsp")) value.resize(value.size() - 4);
            if (value.empty()) value = "menu";
            return value;
        }

        std::string safe_component(std::string value)
        {
            value = lower_slashes(std::move(value));
            std::string out;
            out.reserve(value.size());
            bool underscore = false;
            for (const unsigned char c : value)
            {
                const bool valid = std::isalnum(c) || c == '-' || c == '_' || c == '.';
                if (valid)
                {
                    out.push_back(static_cast<char>(c));
                    underscore = false;
                }
                else if (!underscore)
                {
                    out.push_back('_');
                    underscore = true;
                }
            }
            while (!out.empty() && (out.front() == '_' || out.front() == '.')) out.erase(out.begin());
            while (!out.empty() && (out.back() == '_' || out.back() == '.')) out.pop_back();
            if (out.empty()) out = "material";
            if (out.size() > 96) out.resize(96);
            return out;
        }

        std::string hash_hex(const std::uint64_t hash)
        {
            std::ostringstream out;
            out << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << hash;
            return out.str();
        }

        std::uint64_t stable_material_hash(const std::string& identity)
        {
            // 0 disables the override and 0xfefefefe is DXVK's untouched-state
            // sentinel. Deterministically salt the identity in the extremely rare
            // case that either DWORD would collide with a reserved value.
            for (std::uint32_t salt = 0; salt < 32; ++salt)
            {
                const std::string candidate = salt == 0
                    ? identity
                    : identity + "#rs150-rs151-" + std::to_string(salt);
                const std::uint64_t hash = utils::string_hash64(candidate);
                const std::uint32_t low = static_cast<std::uint32_t>(hash);
                const std::uint32_t high = static_cast<std::uint32_t>(hash >> 32u);
                if (low != 0u && low != 0xfefefefeu && high != 0xfefefefeu)
                    return hash;
            }
            return 1u;
        }

        std::uint64_t bytes_hash64(const std::vector<std::uint8_t>& bytes)
        {
            std::uint64_t hash = 14695981039346656037ull;
            for (const std::uint8_t value : bytes)
            {
                hash ^= value;
                hash *= 1099511628211ull;
            }
            return hash;
        }

        std::string json_escape(std::string_view value)
        {
            std::string out;
            out.reserve(value.size() + 8);
            for (const char c : value)
            {
                switch (c)
                {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out.push_back(c); break;
                }
            }
            return out;
        }

        std::string html_escape(std::string_view value)
        {
            std::string out;
            out.reserve(value.size() + 16u);
            for (const char c : value)
            {
                switch (c)
                {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                case '\'': out += "&#39;"; break;
                default: out.push_back(c); break;
                }
            }
            return out;
        }

        std::string usda_asset(std::string value)
        {
            std::replace(value.begin(), value.end(), '\\', '/');
            std::string out;
            out.reserve(value.size());
            for (const char c : value)
            {
                if (c != '@') out.push_back(c);
            }
            return out;
        }

        std::filesystem::path game_root()
        {
            if (game::root_path.empty())
            {
                char path[MAX_PATH]{};
                GetModuleFileNameA(nullptr, path, MAX_PATH);
                std::filesystem::path exe(path);
                game::root_path = exe.parent_path().string() + "\\";
            }
            return std::filesystem::path(game::root_path);
        }

        std::filesystem::path mods_root_unlocked()
        {
            return game_root() / "rtx-remix" / "mods";
        }

        void resolve_export_layout_unlocked()
        {
            // V20.2.4 is intentionally isolated. Never inspect, select, patch or write
            // files belonging to another Remix project.
            g_export_root = mods_root_unlocked() / "LegacyMaterials";
        }

        std::filesystem::path export_root_unlocked()
        {
            if (g_export_root.empty()) resolve_export_layout_unlocked();
            return g_export_root;
        }

        std::filesystem::path materials_root_unlocked()
        {
            return export_root_unlocked() / "Materials";
        }

        std::filesystem::path conversion_cache_root_unlocked()
        {
            return export_root_unlocked() / "conversion_cache";
        }

        std::filesystem::path tools_root_unlocked()
        {
            return export_root_unlocked() / "tools";
        }

        std::filesystem::path manifests_root_unlocked()
        {
            return export_root_unlocked() / "manifests";
        }

        std::filesystem::path overrides_path_unlocked()
        {
            return export_root_unlocked() / "config" / "material_overrides.toml";
        }

        std::filesystem::path audit_report_path_unlocked()
        {
            return manifests_root_unlocked() / "material_audit.html";
        }

        std::string trim_copy(std::string value)
        {
            const auto not_space = [](const unsigned char c) { return std::isspace(c) == 0; };
            value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
            value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
            return value;
        }

        std::string unquote(std::string value)
        {
            value = trim_copy(std::move(value));
            if (value.size() >= 2u &&
                ((value.front() == '"' && value.back() == '"') ||
                 (value.front() == '\'' && value.back() == '\'')))
                value = value.substr(1u, value.size() - 2u);
            return lower_slashes(std::move(value));
        }

        std::optional<bool> parse_bool_value(const std::string& raw)
        {
            const std::string value = lower_slashes(trim_copy(raw));
            if (value == "true" || value == "1" || value == "yes" || value == "on") return true;
            if (value == "false" || value == "0" || value == "no" || value == "off") return false;
            return std::nullopt;
        }

        std::optional<float> parse_float_value(const std::string& raw)
        {
            try
            {
                std::size_t consumed = 0u;
                const float value = std::stof(trim_copy(raw), &consumed);
                if (consumed == 0u || !std::isfinite(value)) return std::nullopt;
                return value;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        bool load_overrides_unlocked()
        {
            g_material_overrides.clear();
            if (!material_exporter::m_apply_material_overrides) return true;

            const auto path = overrides_path_unlocked();
            std::ifstream input(path);
            if (!input.is_open()) return true;

            material_override_s* current = nullptr;
            for (std::string line; std::getline(input, line); )
            {
                const auto comment = line.find('#');
                if (comment != std::string::npos) line.resize(comment);
                line = trim_copy(std::move(line));
                if (line.empty()) continue;

                if (line.front() == '[' && line.back() == ']')
                {
                    std::string section = trim_copy(line.substr(1u, line.size() - 2u));
                    constexpr std::string_view prefix = "materials.";
                    if (!section.starts_with(prefix))
                    {
                        current = nullptr;
                        continue;
                    }

                    std::string name = trim_copy(section.substr(prefix.size()));
                    name = unquote(std::move(name));
                    if (name.starts_with("materials/")) name.erase(0u, 10u);
                    if (name.ends_with(".vmt")) name.resize(name.size() - 4u);
                    current = name.empty() ? nullptr : &g_material_overrides[name];
                    continue;
                }

                if (!current) continue;
                const auto equals = line.find('=');
                if (equals == std::string::npos) continue;
                const std::string key = lower_slashes(trim_copy(line.substr(0u, equals)));
                const std::string value = trim_copy(line.substr(equals + 1u));

                if (key == "roughness") current->roughness = parse_float_value(value);
                else if (key == "metalness" || key == "metallic") current->metalness = parse_float_value(value);
                else if (key == "disable_height") current->disable_height = parse_bool_value(value);
                else if (key == "disable_emission") current->disable_emission = parse_bool_value(value);
                else if (key == "invert_normal_y") current->invert_normal_y = parse_bool_value(value);
                else if (key == "force_ssbump") current->force_ssbump = parse_bool_value(value);
                else if (key == "normal_source") current->normal_source = unquote(value);
                else if (key == "roughness_source") current->roughness_source = unquote(value);
                else if (key == "height_source") current->height_source = unquote(value);
            }
            return true;
        }

        bool write_override_template_unlocked()
        {
            const auto path = overrides_path_unlocked();
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) return false;
            if (std::filesystem::is_regular_file(path, ec)) return true;

            std::ofstream out(path, std::ios::trunc);
            if (!out.is_open()) return false;
            out << "# LegacyMaterials V21.8 per-material overrides.\n";
            out << "# Section key is the Source VMT path without materials/ and .vmt.\n";
            out << "# Accepted source modes: auto, none, albedo, constant, $bumpmap,\n";
            out << "# $normalmap, $envmapmask, $phongexponenttexture, bump_alpha, ssbump.\n\n";
            out << "[materials.\"de_nuke/nukmetwallhab_wet\"]\n";
            out << "roughness = 0.18\n";
            out << "metalness = 0.70\n";
            out << "normal_source = \"$bumpmap\"\n";
            out << "roughness_source = \"$envmapmask\"\n";
            out << "height_source = \"none\"\n";
            out << "invert_normal_y = false\n";
            out << "force_ssbump = false\n";
            out << "disable_height = true\n";
            out << "disable_emission = false\n";
            return out.good();
        }

        void apply_override(material_record_s& record)
        {
            record.roughness = record.inferred_roughness;
            record.metallic = record.inferred_metallic;
            record.override_applied = false;
            record.override_disable_height = false;
            record.override_disable_emission = false;
            record.override_invert_normal_y = false;
            record.override_force_ssbump = false;
            record.override_normal_source = "auto";
            record.override_roughness_source = "auto";
            record.override_height_source = "auto";

            if (!material_exporter::m_apply_material_overrides) return;
            const auto found = g_material_overrides.find(record.canonical_name);
            if (found == g_material_overrides.end()) return;

            const auto& value = found->second;
            record.override_applied = true;
            if (value.roughness) record.roughness = std::clamp(*value.roughness, 0.02f, 1.0f);
            if (value.metalness) record.metallic = std::clamp(*value.metalness, 0.0f, 1.0f);
            record.override_disable_height = value.disable_height.value_or(false);
            record.override_disable_emission = value.disable_emission.value_or(false);
            record.override_invert_normal_y = value.invert_normal_y.value_or(false);
            record.override_force_ssbump = value.force_ssbump.value_or(false);
            record.override_normal_source = value.normal_source;
            record.override_roughness_source = value.roughness_source;
            record.override_height_source = value.height_source;
        }

        const char* provenance_name(const channel_provenance_t value)
        {
            switch (value)
            {
            case channel_provenance_t::native_map: return "Native";
            case channel_provenance_t::converted_map: return "Converted";
            case channel_provenance_t::inferred_map: return "Inferred";
            case channel_provenance_t::constant_fallback: return "Constant fallback";
            case channel_provenance_t::rejected: return "Rejected";
            default: return "Missing";
            }
        }

        const char* material_grade_name(const material_grade_t value)
        {
            switch (value)
            {
            case material_grade_t::verified: return "Verified";
            case material_grade_t::usable: return "Usable";
            case material_grade_t::needs_review: return "Needs review";
            default: return "Rejected";
            }
        }

        bool should_ignore_material(const std::string& material_name, const std::string& shader)
        {
            if (!material_exporter::m_include_error_materials &&
                (material_name == "error" || material_name.contains("error") || shader.empty()))
                return true;

            if (!material_exporter::m_include_ui_materials &&
                (material_name.starts_with("vgui/") || material_name == "vgui_white" ||
                 material_name == "__fontpage" || material_name.starts_with("dev/")))
                return true;

            return material_name.empty();
        }

        IMaterialVar* find_var(IMaterialInternal* material, const char* name)
        {
            if (!material || !name) return nullptr;
            bool found = false;
            auto* value = material->vftable->FindVar(material, nullptr, name, &found, false);
            return found ? value : nullptr;
        }

        float get_float(IMaterialInternal* material, const char* name, const float fallback)
        {
            if (auto* value = find_var(material, name); value && value->vftable->IsDefined(value))
                return value->vftable->GetFloatValueInternal(value);
            return fallback;
        }

        int get_int(IMaterialInternal* material, const char* name, const int fallback)
        {
            if (auto* value = find_var(material, name); value && value->vftable->IsDefined(value))
                return value->vftable->GetIntValueInternal(value);
            return fallback;
        }

        std::string get_string(IMaterialInternal* material, const char* name)
        {
            if (auto* value = find_var(material, name); value && value->vftable->IsDefined(value))
            {
                const char* str = value->vftable->GetStringValue(value);
                return str ? str : "";
            }
            return {};
        }

        std::array<float, 3> get_vector3(IMaterialInternal* material, const char* name,
            const std::array<float, 3>& fallback)
        {
            auto result = fallback;
            auto* value = find_var(material, name);
            if (!value || !value->vftable->IsDefined(value)) return result;
            const float* vector = value->vftable->GetVecValueInternal1(value);
            if (!vector) return result;
            const int count = std::clamp(value->vftable->VectorSizeInternal(value), 1, 4);
            for (int index = 0; index < std::min(count, 3); ++index)
                result[static_cast<std::size_t>(index)] = std::clamp(vector[index], 0.0f, 1.0f);
            return result;
        }

        bool contains_any(const std::string& text, std::initializer_list<const char*> values)
        {
            for (const auto* value : values)
                if (text.contains(value)) return true;
            return false;
        }

        std::string classify_material(const material_record_s& record)
        {
            const std::string probe = record.canonical_name + " " + record.surface_prop + " " + record.shader_name;
            if (contains_any(probe, {"water", "slime"})) return "water";
            if (record.translucent && contains_any(probe, {"glass", "window"})) return "glass";
            if (record.emissive) return "emissive";
            if (contains_any(probe, {"flesh", "skin", "infected", "survivor"})) return "organic";
            if (contains_any(probe, {"wood", "wooden"})) return "wood";
            if (contains_any(probe, {"fabric", "cloth", "carpet"})) return "fabric";
            if (contains_any(probe, {"rubber", "tire"})) return "rubber";
            if (contains_any(probe, {"metal", "steel", "chrome", "chain", "grate"})) return "metal";
            if (contains_any(probe, {"concrete", "brick", "rock", "stone", "plaster"})) return "masonry";
            if (contains_any(probe, {"grass", "foliage", "leaf", "plant"})) return "foliage";
            if (record.shader_name.starts_with("Unlit")) return "unlit";
            return "generic";
        }

        void infer_pbr(material_record_s& record, IMaterialInternal* material)
        {
            const float direct_roughness = get_float(material, "$roughness", -1.0f);
            const float direct_metalness = get_float(material, "$metalness", -1.0f);
            record.phong_exponent = get_float(material, "$phongexponent", -1.0f);
            record.phong_boost = std::max(0.0f, get_float(material, "$phongboost", 1.0f));
            record.phong_enabled = get_int(material, "$phong", 0) != 0;
            record.ssbump = get_int(material, "$ssbump", 0) != 0;
            record.base_alpha_phong_mask = get_int(material, "$basemapalphaphongmask", 0) != 0;
            record.base_luminance_phong_mask = get_int(material, "$basemapluminancephongmask", 0) != 0;
            record.base_alpha_envmap_mask = get_int(material, "$basealphaenvmapmask", 0) != 0;
            record.normal_alpha_envmap_mask = get_int(material, "$normalmapalphaenvmapmask", 0) != 0;
            record.selfillum_envmapmask_alpha = get_int(material, "$selfillum_envmapmask_alpha", 0) != 0;
            record.lightwarp_enabled = !get_string(material, "$lightwarptexture").empty();
            record.emissiveblend_enabled = get_int(material, "$emissiveblendenabled", 0) != 0 ||
                get_int(material, "$emissiveblend", 0) != 0 ||
                !get_string(material, "$emissiveblendtexture").empty();
            record.invert_phong_mask = get_int(material, "$invertphongmask", 0) != 0;
            record.parallax_enabled = get_int(material, "$parallax", 0) != 0 ||
                get_int(material, "$parallaxcorrect", 0) != 0 ||
                !get_string(material, "$parallaxmap").empty();

            const bool envmap = !get_string(material, "$envmap").empty();
            record.envmap_enabled = envmap;
            const bool selfillum = get_int(material, "$selfillum", 0) != 0 ||
                material->vftable->GetMaterialVarFlag(material, nullptr, MATERIAL_VAR_SELFILLUM);
            record.selfillum_enabled = selfillum;

            record.surface_prop = lower_slashes(get_string(material, "$surfaceprop"));
            record.translucent = material->vftable->IsTranslucent(material);
            record.alpha_tested = material->vftable->IsAlphaTested(material);
            record.alpha_test_reference = std::clamp(
                get_float(material, "$alphatestreference", 0.5f), 0.0f, 1.0f);
            record.additive = get_int(material, "$additive", 0) != 0;
            record.nocull = get_int(material, "$nocull", 0) != 0;
            record.detail_scale = std::max(0.001f, get_float(material, "$detailscale", 1.0f));
            record.detail_blend_factor = std::clamp(
                get_float(material, "$detailblendfactor", 1.0f), 0.0f, 8.0f);
            record.detail_blend_mode = get_int(material, "$detailblendmode", 0);
            record.emissive = selfillum || record.selfillum_envmapmask_alpha ||
                record.emissiveblend_enabled;
            record.emissive_intensity = record.emissive
                ? std::max(1.0f, get_float(material, "$selfillumscale", 1.0f))
                : 0.0f;
            record.water_material = record.shader_name.starts_with("Water") ||
                contains_any(record.canonical_name + " " + record.surface_prop, { "water", "slime" });
            record.water_tint = get_vector3(material, "$refracttint", record.water_tint);

            const std::string probe = record.canonical_name + " " + record.surface_prop;
            if (record.water_material)
            {
                const float reflect_amount = std::clamp(get_float(material, "$reflectamount", 0.8f), 0.0f, 1.0f);
                record.roughness = std::clamp(0.18f - reflect_amount * 0.10f, 0.045f, 0.22f);
                record.confidence = 0.82f;
            }
            else if (direct_roughness >= 0.0f)
            {
                record.roughness = std::clamp(direct_roughness, 0.02f, 1.0f);
                record.confidence = 0.98f;
            }
            else if (record.phong_exponent > 0.0f)
            {
                record.roughness = std::clamp(
                    std::sqrt(2.0f / (record.phong_exponent + 2.0f)), 0.04f, 1.0f);
                record.confidence = 0.86f;
            }
            else if (record.phong_enabled || envmap)
            {
                record.roughness = record.phong_enabled ? 0.32f : 0.42f;
                record.confidence = 0.68f;
            }
            else if (contains_any(probe, {"glass", "chrome"})) record.roughness = 0.12f;
            else if (contains_any(probe, {"metal", "steel"})) record.roughness = 0.35f;
            else if (contains_any(probe, {"plastic", "tile"})) record.roughness = 0.48f;
            else if (contains_any(probe, {"wood"})) record.roughness = 0.68f;
            else if (contains_any(probe, {"concrete", "brick", "rock"})) record.roughness = 0.82f;
            else if (contains_any(probe, {"fabric", "cloth", "carpet"})) record.roughness = 0.92f;
            else record.roughness = 0.65f;

            if (record.water_material)
            {
                record.metallic = 0.0f;
            }
            else if (direct_metalness >= 0.0f)
            {
                record.metallic = std::clamp(direct_metalness, 0.0f, 1.0f);
                record.confidence = std::max(record.confidence, 0.98f);
            }
            else if (contains_any(probe, {"paint", "rust", "oxid", "dirty_metal"}))
                record.metallic = 0.0f;
            else if (contains_any(probe, {"chrome", "bare_metal", "raw_metal", "polished_metal"}))
                record.metallic = 1.0f;
            else if (contains_any(probe, {"metal", "steel", "iron", "chain", "grate"}))
                // Source envmaps are not proof of a metal workflow. Most L4D2 metal
                // surfaces are painted, oxidised or dirty dielectrics. Keep automatic
                // metalness conservative and require an explicit map/name/override for 1.0.
                record.metallic = 0.0f;
            else
                record.metallic = 0.0f;

            record.category = classify_material(record);
            record.inferred_roughness = record.roughness;
            record.inferred_metallic = record.metallic;
            record.glass_material = record.translucent &&
                contains_any(record.canonical_name + " " + record.shader_name, { "glass", "window", "refract" });
        }

        std::string var_to_text(IMaterialVar* value)
        {
            if (!value || !value->vftable->IsDefined(value)) return "undefined";
            std::ostringstream out;
            switch (value->m_Type)
            {
            case matvar_float:
                out << value->vftable->GetFloatValueInternal(value); break;
            case matvar_int:
                out << value->vftable->GetIntValueInternal(value); break;
            case matvar_vector:
            {
                const int count = std::clamp(value->vftable->VectorSizeInternal(value), 1, 4);
                const float* vec = value->vftable->GetVecValueInternal1(value);
                out << "\"[";
                for (int i = 0; i < count; ++i) { if (i) out << ' '; out << vec[i]; }
                out << "]\"";
                break;
            }
            case matvar_texture:
            case matvar_string:
            {
                const char* str = value->vftable->GetStringValue(value);
                out << '"' << (str ? str : "") << '"';
                break;
            }
            default:
                out << "<type " << static_cast<int>(value->m_Type) << '>'; break;
            }
            return out.str();
        }

        void observe_sampler_candidate(material_record_s& record, const int slot,
            IDirect3DBaseTexture9* texture)
        {
            if (!texture || slot < 0 || slot >= static_cast<int>(record.sampler_candidates.size())) return;
            auto& candidates = record.sampler_candidates[slot];
            const auto found = std::find_if(candidates.begin(), candidates.end(),
                [&](const sampler_candidate_s& candidate) { return candidate.texture == texture; });
            if (found != candidates.end())
            {
                ++found->hits;
                found->last_draw = record.draw_count;
                return;
            }

            constexpr std::size_t max_candidates_per_sampler = 16;
            if (candidates.size() >= max_candidates_per_sampler)
            {
                const auto weakest = std::min_element(candidates.begin(), candidates.end(),
                    [](const sampler_candidate_s& a, const sampler_candidate_s& b) {
                        return a.hits < b.hits;
                    });
                if (weakest == candidates.end() || weakest->hits > 1u) return;
                if (weakest->texture) weakest->texture->Release();
                candidates.erase(weakest);
            }

            texture->AddRef();
            candidates.push_back({ texture, 1u, record.draw_count, record.draw_count });
        }

        void retain_bound_textures(material_record_s& record, IShaderAPIDX8* shader_api, const BufferedState_t& state)
        {
            if (!shader_api) return;
            for (std::size_t i = 0; i < record.bound_textures.size(); ++i)
            {
                if (state.m_BoundTexture[i] < 0) continue;
                auto* texture = shader_api->vtbl->GetD3DTexture(shader_api, nullptr, state.m_BoundTexture[i]);
                if (!texture) continue;
                observe_sampler_candidate(record, static_cast<int>(i), texture);

                if (record.bound_textures[i] == texture)
                {
                    ++record.bound_texture_hits[i];
                    continue;
                }

                if (record.contender_textures[i] == texture)
                {
                    ++record.contender_texture_hits[i];
                    // Promote a texture only after it becomes the dominant binding for the
                    // sampler. This avoids capturing depth/flashlight/helper passes as PBR input.
                    if (record.contender_texture_hits[i] > record.bound_texture_hits[i] + 2u)
                    {
                        if (record.bound_textures[i]) record.bound_textures[i]->Release();
                        record.bound_textures[i] = record.contender_textures[i];
                        record.bound_texture_hits[i] = record.contender_texture_hits[i];
                        record.contender_textures[i] = nullptr;
                        record.contender_texture_hits[i] = 0u;
                    }
                    continue;
                }

                if (!record.bound_textures[i])
                {
                    texture->AddRef();
                    record.bound_textures[i] = texture;
                    record.bound_texture_hits[i] = 1u;
                    continue;
                }

                if (record.contender_textures[i]) record.contender_textures[i]->Release();
                texture->AddRef();
                record.contender_textures[i] = texture;
                record.contender_texture_hits[i] = 1u;
            }
        }

        void release_record(material_record_s& record)
        {
            for (auto& texture : record.texture_params)
            {
                if (texture.texture && texture.texture->vftable)
                    texture.texture->vftable->DecrementReferenceCount(texture.texture);
                texture.texture = nullptr;
            }
            for (std::size_t i = 0; i < record.bound_textures.size(); ++i)
            {
                if (record.bound_textures[i]) record.bound_textures[i]->Release();
                if (record.contender_textures[i]) record.contender_textures[i]->Release();
                record.bound_textures[i] = nullptr;
                record.contender_textures[i] = nullptr;
                record.bound_texture_hits[i] = 0u;
                record.contender_texture_hits[i] = 0u;
                for (auto& candidate : record.sampler_candidates[i])
                    if (candidate.texture) candidate.texture->Release();
                record.sampler_candidates[i].clear();
            }
        }

        bool is_probable_texture_parameter(const std::string& name)
        {
            static constexpr std::array<std::string_view, 26> tokens = {
                "basetexture", "albedo", "bumpmap", "normalmap", "detail",
                "roughness", "metalness", "metallic", "heightmap", "displacement",
                "parallax", "phongexponenttexture", "envmapmask", "selfillummask",
                "emissive", "blendmodulatetexture", "flowmap", "lightwarptexture",
                "phongwarptexture", "dudvmap", "refracttexture", "iris", "corneatexture",
                "opacity", "translucencymask", "alphamask"
            };
            return std::any_of(tokens.begin(), tokens.end(), [&](const std::string_view token) {
                return name.find(token) != std::string::npos;
            });
        }

        bool valid_texture_reference(const std::string& value)
        {
            std::string normalized = lower_slashes(value);
            const auto first = normalized.find_first_not_of(" \t\r\n\"");
            if (first == std::string::npos) return false;
            const auto last = normalized.find_last_not_of(" \t\r\n\"");
            normalized = normalized.substr(first, last - first + 1u);

            // Source shader tables expose many optional texture variables as placeholder
            // ITexture objects named <undefined>. They are not material assets and must
            // never be linked to whichever unrelated texture happened to occupy a sampler.
            if (normalized.empty() || normalized == "env_cubemap" ||
                normalized == "undefined" || normalized == "<undefined>" ||
                normalized == "null" || normalized == "<null>" ||
                normalized == "error" || normalized == "<error>" ||
                normalized.find("undefined") != std::string::npos)
                return false;
            if (normalized.front() == '[' || normalized.front() == '{' ||
                normalized.front() == '<')
                return false;
            return std::any_of(normalized.begin(), normalized.end(), [](const unsigned char c) {
                return std::isalpha(c) != 0;
            });
        }

        bool texture_parameter_has_runtime_source(const texture_param_s& source)
        {
            return valid_texture_reference(source.source_name) &&
                (source.texture != nullptr || (source.width > 0 && source.height > 0));
        }

        texture_param_s& upsert_texture_param(material_record_s& record,
            const std::string& var_name, const std::string& source_name)
        {
            auto found = std::find_if(record.texture_params.begin(), record.texture_params.end(),
                [&](const texture_param_s& param) { return param.var_name == var_name; });
            if (found == record.texture_params.end())
            {
                texture_param_s param;
                param.var_name = var_name;
                param.source_name = source_name;
                record.texture_params.push_back(std::move(param));
                return record.texture_params.back();
            }
            if (!source_name.empty() && found->source_name != source_name)
            {
                found->source_name = source_name;
                found->raw_exported = false;
            }
            return *found;
        }

        void merge_declared_vmt_params(material_record_s& record);
        void interpret_source_semantics(material_record_s& record);
        void materialize_effective_vmt(material_record_s& record);

        void capture_material_params(material_record_s& record, IMaterialInternal* material)
        {
            std::vector<std::string> ignored_unresolved;
            const int count = std::max(0, material->vftable->ShaderParamCount(material));
            IMaterialVar** vars = material->vftable->GetShaderParams(material);
            std::ostringstream vmt;
            vmt << record.shader_name << "\n{\n";
            for (int i = 0; vars && i < count; ++i)
            {
                IMaterialVar* value = vars[i];
                if (!value || !value->vftable) continue;
                const char* name_ptr = value->vftable->GetName(value);
                if (!name_ptr || !*name_ptr) continue;
                const std::string name = lower_slashes(name_ptr);
                vmt << "    \"" << name << "\" " << var_to_text(value) << "\n";

                if (value->m_Type == matvar_texture && value->vftable->IsDefined(value))
                {
                    // Preserve the VMT-declared path even when Source substituted the error
                    // texture or skipped loading the bump because mat_fastnobump/full FFP is
                    // active. V20.2.7 discarded the declaration before reading GetStringValue,
                    // which made every packed-in-VPK normal fall through to a flat fallback.
                    ITexture* texture = value->vftable->GetTextureValue(value);
                    const bool live_texture_valid =
                        texture && texture->vftable && !texture->vftable->IsError(texture);
                    const char* texture_name = live_texture_valid
                        ? texture->vftable->GetName(texture) : nullptr;
                    const char* fallback_name = value->vftable->GetStringValue(value);

                    std::string source_name = lower_slashes(
                        texture_name && *texture_name
                            ? texture_name
                            : (fallback_name ? fallback_name : ""));
                    if (!valid_texture_reference(source_name) && fallback_name && *fallback_name)
                        source_name = lower_slashes(fallback_name);

                    if (!valid_texture_reference(source_name))
                    {
                        if (is_probable_texture_parameter(name))
                            ignored_unresolved.push_back(name);
                        continue;
                    }

                    auto& slot = upsert_texture_param(record, name, source_name);
                    slot.is_normal = name.contains("bump") || name.contains("normal");
                    record.has_normal |= slot.is_normal;

                    if (!live_texture_valid)
                    {
                        // The path is still sufficient for loose/VPK extraction.
                        continue;
                    }

                    if (slot.texture != texture)
                    {
                        if (slot.texture && slot.texture->vftable)
                            slot.texture->vftable->DecrementReferenceCount(slot.texture);
                        texture->vftable->IncrementReferenceCount(texture);
                        slot.texture = texture;
                        slot.raw_exported = false;
                    }
                    slot.width = texture->vftable->GetActualWidth(texture);
                    slot.height = texture->vftable->GetActualHeight(texture);
                    slot.frames = std::max(1, texture->vftable->GetNumAnimationFrames(texture));
                    slot.is_normal = texture->vftable->IsNormalMap(texture) || slot.is_normal;
                    slot.is_cube = texture->vftable->IsCubeMap(texture);
                    continue;
                }

                // A few Source shader parameters remain strings until their texture is
                // downloaded or until a proxy updates them. Preserve the declared path so
                // the runtime sampler linker can still associate the VMT channel later.
                if ((value->m_Type == matvar_string || value->m_Type == matvar_undefined) &&
                    is_probable_texture_parameter(name))
                {
                    const char* raw = value->vftable->GetStringValue(value);
                    const std::string source_name = lower_slashes(raw ? raw : "");
                    if (valid_texture_reference(source_name))
                    {
                        auto& slot = upsert_texture_param(record, name, source_name);
                        slot.is_normal = name.contains("bump") || name.contains("normal");
                        record.has_normal |= slot.is_normal;
                    }
                    else if (!source_name.empty())
                    {
                        ignored_unresolved.push_back(name);
                    }
                }
            }
            // The live IMaterial table is not authoritative when full FFP or
            // mat_fastnobump replaced texture variables with error textures. Read the
            // original VMT (including VPK-packed files) and merge its declarations.
            merge_declared_vmt_params(record);

            // Raw VMT recovery may have resolved parameters that appeared undefined in
            // the runtime table. Keep only genuinely unresolved diagnostics.
            ignored_unresolved.erase(std::remove_if(ignored_unresolved.begin(),
                ignored_unresolved.end(), [&](const std::string& unresolved)
                {
                    return std::any_of(record.texture_params.begin(),
                        record.texture_params.end(), [&](const texture_param_s& param)
                        {
                            return param.var_name == unresolved &&
                                valid_texture_reference(param.source_name);
                        });
                }), ignored_unresolved.end());

            std::sort(ignored_unresolved.begin(), ignored_unresolved.end());
            ignored_unresolved.erase(std::unique(ignored_unresolved.begin(), ignored_unresolved.end()),
                ignored_unresolved.end());
            record.ignored_unresolved_texture_params = std::move(ignored_unresolved);
            vmt << "}\n";
            record.runtime_vmt = vmt.str();
        }

        void refresh_material_features(material_record_s& record)
        {
            const std::string shader = lower_slashes(record.shader_name);
            if (shader.starts_with("worldvertextransition"))
                record.shader_profile = "world vertex transition";
            else if (shader.starts_with("infected"))
                record.shader_profile = "infected coloring shader";
            else if (shader.starts_with("eyerefract") || shader.starts_with("eyes"))
                record.shader_profile = "eye refract";
            else if (shader.starts_with("sprite") || shader.contains("spritecard") ||
                shader.contains("particle"))
                record.shader_profile = "particle / sprite";
            else if (shader.starts_with("cable") || shader.contains("rope"))
                record.shader_profile = "cable / rope";
            else if (shader.starts_with("sky"))
                record.shader_profile = "sky";
            else if (shader.starts_with("water"))
                record.shader_profile = "water";
            else if (shader.starts_with("refract") || record.glass_material)
                record.shader_profile = "glass / refract";
            else if (shader.starts_with("vertexlit"))
                record.shader_profile = "vertex lit model";
            else if (shader.starts_with("lightmapped"))
                record.shader_profile = "lightmapped world";
            else if (shader.starts_with("unlit"))
                record.shader_profile = "unlit / emissive";
            else if (shader.contains("decal"))
                record.shader_profile = "decal";
            else
                record.shader_profile = "generic Source shader";

            record.dual_layer = shader.starts_with("worldvertextransition");
            record.detail_texture = false;
            // Model shaders are submitted through dynamic/skinned draw paths even
            // when their VMT has no animation proxy. Publish that distinction so
            // DXVK can enable its dynamic-safe Auto PBR path for characters,
            // weapons, viewmodels, eyes and movable props.
            record.dynamic_material = record.water_material || shader.starts_with("refract")
                || shader.starts_with("vertexlit") || shader.starts_with("infected")
                || shader.starts_with("eyerefract") || shader.starts_with("eyes");

            for (const auto& texture : record.texture_params)
            {
                record.dual_layer |= texture.var_name == "$basetexture2" ||
                    texture.var_name == "$bumpmap2" ||
                    texture.var_name == "$blendmodulatetexture";
                record.detail_texture |= texture.var_name == "$detail" ||
                    texture.var_name == "$detailnormalmap";
                record.dynamic_material |= texture.var_name == "$flowmap" ||
                    texture.var_name == "$flow_normal_texture" ||
                    texture.var_name == "$refracttexture" ||
                    texture.var_name == "$reflecttexture";
            }

            if (material_exporter::m_enable_source_shader_semantics)
                interpret_source_semantics(record);
            materialize_effective_vmt(record);
            for (const auto& warning : record.vmt_patch_warnings)
                append_unique(record.semantic_conflicts, warning, 64u);

            const std::string signature = record.canonical_name + "|" +
                lower_slashes(record.shader_name) + "|" + record.runtime_vmt + "|" +
                record.resolved_vmt_path + "|" + record.specular_semantics + "|" +
                record.emission_semantics + "|" + record.animation_semantics + "|" +
                record.patch_semantics + "|" + record.detail_semantics + "|" +
                record.layer_semantics + "|" + record.calibrated_specular_workflow;
            record.identity_key = hash_hex(stable_material_hash(signature));
        }

        bool texture_desc(IDirect3DBaseTexture9* base, D3DSURFACE_DESC& desc)
        {
            desc = {};
            if (!base || base->GetType() != D3DRTYPE_TEXTURE) return false;
            return SUCCEEDED(static_cast<IDirect3DTexture9*>(base)->GetLevelDesc(0, &desc)) &&
                desc.Width > 0u && desc.Height > 0u;
        }

        bool format_is_two_channel_normal(const D3DFORMAT format)
        {
            const auto fourcc = static_cast<DWORD>(format);
            return fourcc == MAKEFOURCC('A', 'T', 'I', '2') ||
                fourcc == MAKEFOURCC('B', 'C', '5', 'U');
        }

        bool format_looks_like_normal(const D3DFORMAT format)
        {
            return format == D3DFMT_DXT5 || format_is_two_channel_normal(format);
        }

        int semantic_slot_bonus(const material_record_s& record, const int slot,
            const texture_semantic_t semantic)
        {
            switch (semantic)
            {
            case texture_semantic_t::albedo:
                return slot == 0 ? 90 : (slot == 1 ? 30 : 0);
            case texture_semantic_t::normal:
                if (record.shader_name.starts_with("VertexLit"))
                    return slot == 3 ? 55 : (slot == 4 ? 45 : 0);
                return slot == 4 ? 55 : (slot == 3 ? 45 : 0);
            case texture_semantic_t::scalar_mask:
                return slot >= 4 ? 20 : 0;
            case texture_semantic_t::emission:
                return slot >= 2 ? 20 : 0;
            case texture_semantic_t::height:
                return slot >= 3 ? 20 : 0;
            }
            return 0;
        }

        int score_bound_candidate(const material_record_s& record, const texture_param_s& source,
            const int slot, const sampler_candidate_s& candidate, const texture_semantic_t semantic,
            const bool excluded)
        {
            if (!candidate.texture) return std::numeric_limits<int>::min();
            D3DSURFACE_DESC desc{};
            if (!texture_desc(candidate.texture, desc)) return std::numeric_limits<int>::min();

            int score = semantic_slot_bonus(record, slot, semantic);
            if (source.width > 0 && source.height > 0)
            {
                if (static_cast<int>(desc.Width) == source.width &&
                    static_cast<int>(desc.Height) == source.height)
                {
                    score += 180;
                }
                else
                {
                    const float expected_area = static_cast<float>(source.width) * source.height;
                    const float actual_area = static_cast<float>(desc.Width) * desc.Height;
                    const float area_ratio = expected_area > 0.0f
                        ? std::max(expected_area, actual_area) / std::max(1.0f, std::min(expected_area, actual_area))
                        : 1.0f;
                    const float expected_aspect = static_cast<float>(source.width) /
                        std::max(1, source.height);
                    const float actual_aspect = static_cast<float>(desc.Width) /
                        std::max(1u, desc.Height);
                    const float aspect_delta = std::abs(expected_aspect - actual_aspect);
                    if (area_ratio <= 1.05f) score += 90;
                    else if (area_ratio <= 2.1f) score += 45;
                    else if (area_ratio >= 16.0f) score -= 45;
                    if (aspect_delta < 0.02f) score += 25;
                    if (static_cast<int>(desc.Width) == source.width ||
                        static_cast<int>(desc.Height) == source.height) score += 20;
                }
            }
            else
            {
                // String-only material variables do not expose dimensions. Keep them
                // linkable using shader sampler conventions and observed persistence.
                score += 10;
            }

            if (desc.Width < 8u || desc.Height < 8u) score -= 80;
            if (desc.Width == 1u && desc.Height == 1u) score -= 100;

            // Reflection/refraction/shadow render targets are transient frame data, not
            // source material maps. They are especially common in Water and projected
            // texture passes, so keep them out of static PBR exports.
            if ((desc.Usage & D3DUSAGE_RENDERTARGET) != 0u) score -= 420;
            if ((desc.Usage & D3DUSAGE_DEPTHSTENCIL) != 0u) score -= 600;

            const bool explicit_normal_parameter = source.is_normal ||
                source.var_name.contains("bump") || source.var_name.contains("normal");
            if (semantic == texture_semantic_t::normal)
            {
                if (format_is_two_channel_normal(desc.Format)) score += 90;
                else if (desc.Format == D3DFMT_DXT5 && explicit_normal_parameter) score += 45;
                if (explicit_normal_parameter) score += 35;
                if (slot == 0 && !record.water_material) score -= 55;
            }
            else if (semantic == texture_semantic_t::albedo)
            {
                if (slot == 0) score += 55;
                if (format_is_two_channel_normal(desc.Format)) score -= 80;
            }
            else if (format_is_two_channel_normal(desc.Format))
            {
                score -= 25;
            }

            if (source.var_name == "$basetexture" && slot == 0) score += 70;
            if (source.var_name == "$flowmap" && slot == 4) score += 100;
            if (source.var_name.contains("bump") || source.var_name.contains("normal"))
            {
                if (record.water_material)
                {
                    if (slot == 1 || slot == 3) score += 60;
                    else if (slot == 0 || slot == 4) score += 30;
                }
                else if (record.shader_name.starts_with("Lightmapped"))
                {
                    if (slot == 2) score += 85;
                    else if (slot == 4) score += 70;
                    else if (slot == 3) score += 55;
                    else if (slot == 1) score += 25;
                }
                else if (record.shader_name.starts_with("VertexLit"))
                {
                    if (slot == 3) score += 85;
                    else if (slot == 2) score += 70;
                    else if (slot == 4) score += 55;
                    else if (slot == 1) score += 25;
                }
            }

            score += static_cast<int>(std::min<std::uint32_t>(candidate.hits, 80u));
            if (candidate.last_draw + 240u >= record.draw_count) score += 8;
            if (excluded) score -= 120;
            return score;
        }

        bound_texture_choice_s choose_bound_texture(const material_record_s& record,
            const texture_param_s& source, const texture_semantic_t semantic,
            const std::unordered_set<IDirect3DBaseTexture9*>& excluded,
            const bool allow_reuse = false)
        {
            // Scalar masks, displacement and emission are optional channels. Linking a
            // string-only/default shader variable by sampler position is too ambiguous
            // and was the source of cross-material face/noise maps. Wait until Source has
            // resolved the variable to a real runtime texture with dimensions.
            const bool sensitive_semantic = semantic == texture_semantic_t::scalar_mask ||
                semantic == texture_semantic_t::emission ||
                semantic == texture_semantic_t::height;
            if (sensitive_semantic && !texture_parameter_has_runtime_source(source))
                return {};

            bound_texture_choice_s best;
            for (int slot = 0; slot < static_cast<int>(record.sampler_candidates.size()); ++slot)
            {
                const auto& candidates = record.sampler_candidates[slot];
                for (int index = 0; index < static_cast<int>(candidates.size()); ++index)
                {
                    const auto& candidate = candidates[index];
                    const bool is_excluded = excluded.contains(candidate.texture);
                    if (is_excluded && !allow_reuse) continue;
                    const int score = score_bound_candidate(record, source, slot, candidate,
                        semantic, is_excluded);
                    if (score <= best.score) continue;
                    D3DSURFACE_DESC desc{};
                    if (!texture_desc(candidate.texture, desc)) continue;
                    best.texture = candidate.texture;
                    best.slot = slot;
                    best.candidate_index = index;
                    best.score = score;
                    best.hits = candidate.hits;
                    best.desc = desc;
                }
            }

            const int threshold = source.width > 0 && source.height > 0 ? 55 : 25;
            if (best.score < threshold) return {};
            return best;
        }

        bound_texture_choice_s best_candidate_in_slot(const material_record_s& record,
            const int slot, const bool reject_normal_format)
        {
            bound_texture_choice_s best;
            if (slot < 0 || slot >= static_cast<int>(record.sampler_candidates.size())) return best;
            const auto& candidates = record.sampler_candidates[slot];
            for (int index = 0; index < static_cast<int>(candidates.size()); ++index)
            {
                const auto& candidate = candidates[index];
                D3DSURFACE_DESC desc{};
                if (!texture_desc(candidate.texture, desc)) continue;
                if ((desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0u) continue;
                if (reject_normal_format && format_looks_like_normal(desc.Format)) continue;
                int score = static_cast<int>(std::min<std::uint32_t>(candidate.hits, 100u));
                if (desc.Width < 8u || desc.Height < 8u) score -= 100;
                if (candidate.texture == record.bound_textures[slot]) score += 20;
                if (score <= best.score) continue;
                best.texture = candidate.texture;
                best.slot = slot;
                best.candidate_index = index;
                best.score = score;
                best.hits = candidate.hits;
                best.desc = desc;
            }
            return best;
        }

        const texture_param_s* find_exact_texture_param(const material_record_s& record,
            std::initializer_list<const char*> names)
        {
            for (const auto* name : names)
                for (const auto& texture : record.texture_params)
                    if (!texture.is_cube && texture.var_name == name &&
                        valid_texture_reference(texture.source_name)) return &texture;
            return nullptr;
        }

        const texture_param_s* find_texture_param_by_name(const material_record_s& record,
            std::string name)
        {
            name = lower_slashes(std::move(name));
            if (name.empty() || name == "auto" || name == "none" ||
                name == "albedo" || name == "constant" ||
                name == "bump_alpha" || name == "ssbump")
                return nullptr;
            if (!name.starts_with("$")) name.insert(name.begin(), '$');
            for (const auto& texture : record.texture_params)
                if (!texture.is_cube && texture.var_name == name &&
                    valid_texture_reference(texture.source_name)) return &texture;
            return nullptr;
        }

        void append_channel_warnings(material_record_s& record, const char* label,
            const channel_export_s& channel)
        {
            if (!channel.ready)
                record.warnings.push_back(std::format("{} missing: {}", label,
                    channel.actual_status.empty() ? "no usable source" : channel.actual_status));
            if (channel.rejected)
                record.warnings.push_back(std::format("{} rejected by quality gate", label));
            if (channel.quality.suspicious)
                record.warnings.push_back(std::format("{} quality: {}", label,
                    channel.quality.status));
            if (channel.provenance == channel_provenance_t::constant_fallback)
                record.warnings.push_back(std::format("{} uses a constant fallback", label));
            if (channel.confidence > 0.0f && channel.confidence < 0.45f)
                record.warnings.push_back(std::format("{} has low source confidence ({:.0f}%)",
                    label, channel.confidence * 100.0f));
            for (const auto& warning : channel.warnings)
                if (!warning.empty())
                    record.warnings.push_back(std::format("{}: {}", label, warning));
        }

        bound_texture_choice_s choose_albedo_texture(const material_record_s& record,
            const texture_param_s** selected_source = nullptr)
        {
            const texture_param_s* source = find_exact_texture_param(record,
                { "$basetexture", "$albedo", "$basetexture2" });
            if (!source)
            {
                for (const auto& texture : record.texture_params)
                {
                    if (!texture.is_cube && texture.var_name.contains("basetexture"))
                    {
                        source = &texture;
                        break;
                    }
                }
            }
            if (selected_source) *selected_source = source;

            if (source)
            {
                const std::unordered_set<IDirect3DBaseTexture9*> none;
                auto scored = choose_bound_texture(record, *source,
                    texture_semantic_t::albedo, none, true);
                if (scored) return scored;
            }

            // Water shaders commonly bind reflection/refraction render targets where an
            // ordinary material would bind $basetexture. Never mistake the current scene
            // render target for a static albedo; V20.2.4 generates a neutral water base from
            // $refracttint instead.
            if (record.water_material && !source) return {};

            // Source convention: sampler 0 is normally the primary base texture.
            auto slot_zero = best_candidate_in_slot(record, 0, false);
            if (slot_zero) return slot_zero;

            bound_texture_choice_s largest;
            std::uint64_t best_area = 0;
            for (int slot = 1; slot < static_cast<int>(record.sampler_candidates.size()); ++slot)
            {
                const auto candidate = best_candidate_in_slot(record, slot, true);
                if (!candidate) continue;
                const std::uint64_t area = static_cast<std::uint64_t>(candidate.desc.Width) *
                    candidate.desc.Height;
                if (area <= best_area) continue;
                best_area = area;
                largest = candidate;
            }
            return largest;
        }

        bool save_dds(IDirect3DBaseTexture9* texture, const std::filesystem::path& path)
        {
            if (!texture) return false;
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            return SUCCEEDED(D3DXSaveTextureToFileA(path.string().c_str(), D3DXIFF_DDS, texture, nullptr));
        }

        bool write_solid_tga(const std::filesystem::path& path, const UINT width,
            const UINT height, const std::uint8_t red, const std::uint8_t green,
            const std::uint8_t blue)
        {
            if (width == 0u || height == 0u || width > 65535u || height > 65535u) return false;
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return false;

            std::array<std::uint8_t, 18> header{};
            header[2] = 2; // uncompressed true-color
            header[12] = static_cast<std::uint8_t>(width & 0xffu);
            header[13] = static_cast<std::uint8_t>((width >> 8u) & 0xffu);
            header[14] = static_cast<std::uint8_t>(height & 0xffu);
            header[15] = static_cast<std::uint8_t>((height >> 8u) & 0xffu);
            header[16] = 24;
            header[17] = 0x20; // top-left origin
            out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
            const std::array<std::uint8_t, 3> pixel{ blue, green, red };
            for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(width) * height; ++i)
                out.write(reinterpret_cast<const char*>(pixel.data()), static_cast<std::streamsize>(pixel.size()));
            return out.good();
        }

        bool write_gray_tga(const std::filesystem::path& path, const UINT width,
            const UINT height, const std::uint8_t value)
        {
            if (width == 0u || height == 0u || width > 65535u || height > 65535u) return false;
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return false;

            std::array<std::uint8_t, 18> header{};
            header[2] = 3; // uncompressed grayscale
            header[12] = static_cast<std::uint8_t>(width & 0xffu);
            header[13] = static_cast<std::uint8_t>((width >> 8u) & 0xffu);
            header[14] = static_cast<std::uint8_t>(height & 0xffu);
            header[15] = static_cast<std::uint8_t>((height >> 8u) & 0xffu);
            header[16] = 8;
            header[17] = 0x20;
            out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
            std::array<std::uint8_t, 4096> block{};
            block.fill(value);
            std::uint64_t remaining = static_cast<std::uint64_t>(width) * height;
            while (remaining > 0)
            {
                const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, block.size()));
                out.write(reinterpret_cast<const char*>(block.data()), count);
                remaining -= static_cast<std::uint64_t>(count);
            }
            return out.good();
        }

        bool decode_texture_rgba(IDirect3DBaseTexture9* base, rgba_image_s& image)
        {
            image = {};
            D3DSURFACE_DESC desc{};
            if (!texture_desc(base, desc)) return false;

            IDirect3DDevice9* device = nullptr;
            IDirect3DSurface9* source = nullptr;
            IDirect3DSurface9* system = nullptr;
            auto cleanup = [&]()
            {
                if (system) system->Release();
                if (source) source->Release();
                if (device) device->Release();
            };

            if (FAILED(base->GetDevice(&device)) || !device)
            {
                cleanup();
                return false;
            }
            if (FAILED(static_cast<IDirect3DTexture9*>(base)->GetSurfaceLevel(0, &source)) || !source)
            {
                cleanup();
                return false;
            }
            if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height,
                D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &system, nullptr)) || !system)
            {
                cleanup();
                return false;
            }
            if (FAILED(D3DXLoadSurfaceFromSurface(system, nullptr, nullptr, source, nullptr, nullptr,
                D3DX_FILTER_NONE, 0u)))
            {
                cleanup();
                return false;
            }

            D3DLOCKED_RECT locked{};
            if (FAILED(system->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            {
                cleanup();
                return false;
            }

            image.width = desc.Width;
            image.height = desc.Height;
            image.pixels.resize(static_cast<std::size_t>(desc.Width) * desc.Height * 4u);
            for (UINT y = 0; y < desc.Height; ++y)
            {
                const auto* row = static_cast<const std::uint8_t*>(locked.pBits) +
                    static_cast<std::size_t>(y) * locked.Pitch;
                for (UINT x = 0; x < desc.Width; ++x)
                {
                    const auto* bgra = row + static_cast<std::size_t>(x) * 4u;
                    auto* rgba = image.pixels.data() +
                        (static_cast<std::size_t>(y) * desc.Width + x) * 4u;
                    rgba[0] = bgra[2];
                    rgba[1] = bgra[1];
                    rgba[2] = bgra[0];
                    rgba[3] = bgra[3];
                    image.alpha_min = std::min(image.alpha_min, rgba[3]);
                    image.alpha_max = std::max(image.alpha_max, rgba[3]);
                    image.red_min = std::min(image.red_min, rgba[0]);
                    image.red_max = std::max(image.red_max, rgba[0]);
                }
            }
            system->UnlockRect();
            cleanup();
            return true;
        }


        struct vtf_decode_result_s
        {
            rgba_image_s image;
            ImageFormat format = IMAGE_FORMAT_UNKNOWN;
            std::uint32_t flags = 0u;
            std::uint32_t version_major = 0u;
            std::uint32_t version_minor = 0u;
            std::filesystem::path source_path;
            std::string status;
        };

        std::uint16_t read_le16(const std::vector<std::uint8_t>& bytes, const std::size_t offset)
        {
            if (offset + 2u > bytes.size()) return 0u;
            return static_cast<std::uint16_t>(bytes[offset]) |
                static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1u]) << 8u);
        }

        std::uint32_t read_le32(const std::vector<std::uint8_t>& bytes, const std::size_t offset)
        {
            if (offset + 4u > bytes.size()) return 0u;
            return static_cast<std::uint32_t>(bytes[offset]) |
                (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
                (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
                (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
        }

        std::size_t vtf_level_size(const ImageFormat format, const UINT width, const UINT height)
        {
            const std::size_t w = std::max<UINT>(1u, width);
            const std::size_t h = std::max<UINT>(1u, height);
            const std::size_t blocks = ((w + 3u) / 4u) * ((h + 3u) / 4u);
            switch (format)
            {
            case IMAGE_FORMAT_DXT1:
            case IMAGE_FORMAT_DXT1_ONEBITALPHA:
            case IMAGE_FORMAT_ATI1N:
                return blocks * 8u;
            case IMAGE_FORMAT_DXT3:
            case IMAGE_FORMAT_DXT5:
            case IMAGE_FORMAT_ATI2N:
                return blocks * 16u;
            case IMAGE_FORMAT_RGBA8888:
            case IMAGE_FORMAT_ABGR8888:
            case IMAGE_FORMAT_ARGB8888:
            case IMAGE_FORMAT_BGRA8888:
            case IMAGE_FORMAT_BGRX8888:
            case IMAGE_FORMAT_RGBX8888:
                return w * h * 4u;
            case IMAGE_FORMAT_RGB888:
            case IMAGE_FORMAT_BGR888:
            case IMAGE_FORMAT_RGB888_BLUESCREEN:
            case IMAGE_FORMAT_BGR888_BLUESCREEN:
                return w * h * 3u;
            case IMAGE_FORMAT_UV88:
            case IMAGE_FORMAT_IA88:
                return w * h * 2u;
            case IMAGE_FORMAT_I8:
            case IMAGE_FORMAT_A8:
                return w * h;
            default:
                return 0u;
            }
        }

        std::array<std::uint8_t, 4> decode_565(const std::uint16_t value)
        {
            return {
                static_cast<std::uint8_t>(((value >> 11u) & 31u) * 255u / 31u),
                static_cast<std::uint8_t>(((value >> 5u) & 63u) * 255u / 63u),
                static_cast<std::uint8_t>((value & 31u) * 255u / 31u),
                255u
            };
        }

        void write_vtf_pixel(rgba_image_s& image, const UINT x, const UINT y,
            const std::array<std::uint8_t, 4>& color)
        {
            if (x >= image.width || y >= image.height) return;
            auto* dst = image.pixels.data() +
                (static_cast<std::size_t>(y) * image.width + x) * 4u;
            std::copy(color.begin(), color.end(), dst);
            image.alpha_min = std::min(image.alpha_min, color[3]);
            image.alpha_max = std::max(image.alpha_max, color[3]);
            image.red_min = std::min(image.red_min, color[0]);
            image.red_max = std::max(image.red_max, color[0]);
        }

        std::array<std::uint8_t, 8> decode_bc4_palette(const std::uint8_t a0,
            const std::uint8_t a1)
        {
            std::array<std::uint8_t, 8> palette{};
            palette[0] = a0;
            palette[1] = a1;
            if (a0 > a1)
            {
                for (int i = 1; i <= 6; ++i)
                    palette[static_cast<std::size_t>(i + 1)] = static_cast<std::uint8_t>(
                        ((7 - i) * a0 + i * a1 + 3) / 7);
            }
            else
            {
                for (int i = 1; i <= 4; ++i)
                    palette[static_cast<std::size_t>(i + 1)] = static_cast<std::uint8_t>(
                        ((5 - i) * a0 + i * a1 + 2) / 5);
                palette[6] = 0u;
                palette[7] = 255u;
            }
            return palette;
        }

        std::array<std::uint8_t, 16> decode_bc4_block(const std::uint8_t* block)
        {
            std::array<std::uint8_t, 16> values{};
            const auto palette = decode_bc4_palette(block[0], block[1]);
            std::uint64_t indices = 0u;
            for (int i = 0; i < 6; ++i)
                indices |= static_cast<std::uint64_t>(block[2 + i]) << (8u * i);
            for (int i = 0; i < 16; ++i)
                values[static_cast<std::size_t>(i)] =
                    palette[static_cast<std::size_t>((indices >> (3u * i)) & 7u)];
            return values;
        }

        bool decode_vtf_pixels(const std::uint8_t* data, const std::size_t data_size,
            const ImageFormat format, const UINT width, const UINT height,
            rgba_image_s& image, std::string& status)
        {
            image = {};
            image.width = width;
            image.height = height;
            image.pixels.assign(static_cast<std::size_t>(width) * height * 4u, 255u);
            const auto required = vtf_level_size(format, width, height);
            if (required == 0u || required > data_size)
            {
                status = std::format("unsupported/truncated VTF format {} (need {}, have {})",
                    static_cast<int>(format), required, data_size);
                return false;
            }

            if (format == IMAGE_FORMAT_DXT1 || format == IMAGE_FORMAT_DXT1_ONEBITALPHA ||
                format == IMAGE_FORMAT_DXT3 || format == IMAGE_FORMAT_DXT5)
            {
                const bool dxt1 = format == IMAGE_FORMAT_DXT1 ||
                    format == IMAGE_FORMAT_DXT1_ONEBITALPHA;
                const std::size_t block_size = dxt1 ? 8u : 16u;
                const UINT blocks_x = (width + 3u) / 4u;
                const UINT blocks_y = (height + 3u) / 4u;
                for (UINT by = 0; by < blocks_y; ++by)
                for (UINT bx = 0; bx < blocks_x; ++bx)
                {
                    const auto* block = data +
                        (static_cast<std::size_t>(by) * blocks_x + bx) * block_size;
                    const auto* color_block = block + (dxt1 ? 0u : 8u);
                    const std::uint16_t c0 = static_cast<std::uint16_t>(color_block[0]) |
                        static_cast<std::uint16_t>(static_cast<std::uint16_t>(color_block[1]) << 8u);
                    const std::uint16_t c1 = static_cast<std::uint16_t>(color_block[2]) |
                        static_cast<std::uint16_t>(static_cast<std::uint16_t>(color_block[3]) << 8u);
                    std::array<std::array<std::uint8_t, 4>, 4> colors{};
                    colors[0] = decode_565(c0);
                    colors[1] = decode_565(c1);
                    if (!dxt1 || c0 > c1)
                    {
                        for (int c = 0; c < 3; ++c)
                        {
                            colors[2][c] = static_cast<std::uint8_t>(
                                (2u * colors[0][c] + colors[1][c] + 1u) / 3u);
                            colors[3][c] = static_cast<std::uint8_t>(
                                (colors[0][c] + 2u * colors[1][c] + 1u) / 3u);
                        }
                        colors[2][3] = colors[3][3] = 255u;
                    }
                    else
                    {
                        for (int c = 0; c < 3; ++c)
                            colors[2][c] = static_cast<std::uint8_t>(
                                (colors[0][c] + colors[1][c]) / 2u);
                        colors[2][3] = 255u;
                        colors[3] = { 0u, 0u, 0u, 0u };
                    }
                    const std::uint32_t color_indices =
                        static_cast<std::uint32_t>(color_block[4]) |
                        (static_cast<std::uint32_t>(color_block[5]) << 8u) |
                        (static_cast<std::uint32_t>(color_block[6]) << 16u) |
                        (static_cast<std::uint32_t>(color_block[7]) << 24u);
                    std::array<std::uint8_t, 16> alpha{};
                    alpha.fill(255u);
                    if (format == IMAGE_FORMAT_DXT3)
                    {
                        for (int pixel = 0; pixel < 16; ++pixel)
                        {
                            const auto nibble = static_cast<std::uint8_t>(
                                (block[pixel / 2] >> ((pixel & 1) * 4)) & 0x0fu);
                            alpha[static_cast<std::size_t>(pixel)] =
                                static_cast<std::uint8_t>(nibble * 17u);
                        }
                    }
                    else if (format == IMAGE_FORMAT_DXT5)
                    {
                        alpha = decode_bc4_block(block);
                    }
                    for (UINT py = 0; py < 4u; ++py)
                    for (UINT px = 0; px < 4u; ++px)
                    {
                        const UINT pixel = py * 4u + px;
                        auto color = colors[(color_indices >> (pixel * 2u)) & 3u];
                        color[3] = alpha[pixel];
                        write_vtf_pixel(image, bx * 4u + px, by * 4u + py, color);
                    }
                }
                status = std::format("decoded block-compressed VTF format {}",
                    static_cast<int>(format));
                return true;
            }

            if (format == IMAGE_FORMAT_ATI1N || format == IMAGE_FORMAT_ATI2N)
            {
                const UINT blocks_x = (width + 3u) / 4u;
                const UINT blocks_y = (height + 3u) / 4u;
                const std::size_t block_size = format == IMAGE_FORMAT_ATI2N ? 16u : 8u;
                for (UINT by = 0; by < blocks_y; ++by)
                for (UINT bx = 0; bx < blocks_x; ++bx)
                {
                    const auto* block = data +
                        (static_cast<std::size_t>(by) * blocks_x + bx) * block_size;
                    const auto red = decode_bc4_block(block);
                    std::array<std::uint8_t, 16> green{};
                    green.fill(128u);
                    if (format == IMAGE_FORMAT_ATI2N) green = decode_bc4_block(block + 8u);
                    for (UINT py = 0; py < 4u; ++py)
                    for (UINT px = 0; px < 4u; ++px)
                    {
                        const UINT pixel = py * 4u + px;
                        write_vtf_pixel(image, bx * 4u + px, by * 4u + py,
                            { red[pixel], green[pixel], 255u, 255u });
                    }
                }
                status = format == IMAGE_FORMAT_ATI2N ?
                    "decoded ATI2N/BC5 VTF" : "decoded ATI1N/BC4 VTF";
                return true;
            }

            const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
            for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
            {
                const UINT x = static_cast<UINT>(pixel % width);
                const UINT y = static_cast<UINT>(pixel / width);
                switch (format)
                {
                case IMAGE_FORMAT_RGBA8888:
                {
                    const auto* src = data + pixel * 4u;
                    write_vtf_pixel(image, x, y, { src[0], src[1], src[2], src[3] });
                    break;
                }
                case IMAGE_FORMAT_ABGR8888:
                {
                    const auto* src = data + pixel * 4u;
                    write_vtf_pixel(image, x, y, { src[3], src[2], src[1], src[0] });
                    break;
                }
                case IMAGE_FORMAT_ARGB8888:
                {
                    const auto* src = data + pixel * 4u;
                    write_vtf_pixel(image, x, y, { src[1], src[2], src[3], src[0] });
                    break;
                }
                case IMAGE_FORMAT_BGRA8888:
                case IMAGE_FORMAT_BGRX8888:
                {
                    const auto* src = data + pixel * 4u;
                    write_vtf_pixel(image, x, y, { src[2], src[1], src[0],
                        static_cast<std::uint8_t>(format == IMAGE_FORMAT_BGRA8888 ? src[3] : 255u) });
                    break;
                }
                case IMAGE_FORMAT_RGBX8888:
                {
                    const auto* src = data + pixel * 4u;
                    write_vtf_pixel(image, x, y, { src[0], src[1], src[2], 255u });
                    break;
                }
                case IMAGE_FORMAT_RGB888:
                case IMAGE_FORMAT_RGB888_BLUESCREEN:
                {
                    const auto* src = data + pixel * 3u;
                    write_vtf_pixel(image, x, y, { src[0], src[1], src[2], 255u });
                    break;
                }
                case IMAGE_FORMAT_BGR888:
                case IMAGE_FORMAT_BGR888_BLUESCREEN:
                {
                    const auto* src = data + pixel * 3u;
                    write_vtf_pixel(image, x, y, { src[2], src[1], src[0], 255u });
                    break;
                }
                case IMAGE_FORMAT_I8:
                    write_vtf_pixel(image, x, y,
                        { data[pixel], data[pixel], data[pixel], 255u });
                    break;
                case IMAGE_FORMAT_A8:
                    write_vtf_pixel(image, x, y, { 255u, 255u, 255u, data[pixel] });
                    break;
                case IMAGE_FORMAT_IA88:
                {
                    const auto* src = data + pixel * 2u;
                    write_vtf_pixel(image, x, y, { src[0], src[0], src[0], src[1] });
                    break;
                }
                case IMAGE_FORMAT_UV88:
                {
                    const auto* src = data + pixel * 2u;
                    write_vtf_pixel(image, x, y, { src[0], src[1], 255u, 255u });
                    break;
                }
                default:
                    status = std::format("VTF format {} is not supported by the direct resolver",
                        static_cast<int>(format));
                    return false;
                }
            }
            status = std::format("decoded uncompressed VTF format {}", static_cast<int>(format));
            return true;
        }

        bool decode_vtf_top_mip(const std::filesystem::path& path, vtf_decode_result_s& result)
        {
            result = {};
            result.source_path = path;
            std::ifstream input(path, std::ios::binary);
            if (!input.is_open())
            {
                result.status = "VTF file could not be opened";
                return false;
            }
            input.seekg(0, std::ios::end);
            const std::streamoff length = input.tellg();
            input.seekg(0, std::ios::beg);
            if (length < 64)
            {
                result.status = "VTF file is smaller than the base header";
                return false;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
            if (input.gcount() != static_cast<std::streamsize>(length))
            {
                result.status = "VTF file read failed";
                return false;
            }
            if (bytes[0] != 'V' || bytes[1] != 'T' || bytes[2] != 'F' || bytes[3] != 0u)
            {
                result.status = "invalid VTF signature";
                return false;
            }

            result.version_major = read_le32(bytes, 4u);
            result.version_minor = read_le32(bytes, 8u);
            const std::uint32_t header_size = read_le32(bytes, 12u);
            const UINT width = read_le16(bytes, 16u);
            const UINT height = read_le16(bytes, 18u);
            result.flags = read_le32(bytes, 20u);
            const UINT frames = std::max<UINT>(1u, read_le16(bytes, 24u));
            result.format = static_cast<ImageFormat>(read_le32(bytes, 52u));
            const UINT mip_count = std::max<UINT>(1u, bytes[56u]);
            const auto low_format = static_cast<ImageFormat>(read_le32(bytes, 57u));
            const UINT low_width = bytes[61u];
            const UINT low_height = bytes[62u];
            const UINT depth = result.version_minor >= 2u && bytes.size() >= 65u
                ? std::max<UINT>(1u, read_le16(bytes, 63u)) : 1u;
            if (result.version_major != 7u || width == 0u || height == 0u ||
                header_size < 64u || header_size > bytes.size())
            {
                result.status = std::format("unsupported VTF {}.{} header", result.version_major,
                    result.version_minor);
                return false;
            }

            std::size_t image_offset = 0u;
            if (result.version_minor >= 3u && bytes.size() >= 80u)
            {
                const UINT resource_count = read_le32(bytes, 68u);
                for (UINT index = 0; index < resource_count; ++index)
                {
                    const std::size_t entry = 80u + static_cast<std::size_t>(index) * 8u;
                    if (entry + 8u > bytes.size()) break;
                    const std::uint32_t tag = static_cast<std::uint32_t>(bytes[entry]) |
                        (static_cast<std::uint32_t>(bytes[entry + 1u]) << 8u) |
                        (static_cast<std::uint32_t>(bytes[entry + 2u]) << 16u);
                    const std::uint8_t resource_flags = bytes[entry + 3u];
                    if (tag == 0x30u && (resource_flags & 0x02u) == 0u)
                    {
                        image_offset = read_le32(bytes, entry + 4u);
                        break;
                    }
                }
            }
            if (image_offset == 0u)
            {
                const std::size_t low_size = low_width > 0u && low_height > 0u
                    ? vtf_level_size(low_format, low_width, low_height) : 0u;
                image_offset = static_cast<std::size_t>(header_size) + low_size;
            }

            constexpr std::uint32_t textureflags_envmap = 0x00004000u;
            const UINT faces = (result.flags & textureflags_envmap) != 0u ? 6u : 1u;
            for (int mip = static_cast<int>(mip_count) - 1; mip > 0; --mip)
            {
                const UINT mip_width = std::max<UINT>(1u, width >> mip);
                const UINT mip_height = std::max<UINT>(1u, height >> mip);
                const UINT mip_depth = std::max<UINT>(1u, depth >> mip);
                const auto level_size = vtf_level_size(result.format, mip_width, mip_height);
                if (level_size == 0u)
                {
                    result.status = std::format("cannot size VTF format {}",
                        static_cast<int>(result.format));
                    return false;
                }
                image_offset += level_size * frames * faces * mip_depth;
            }
            const auto top_size = vtf_level_size(result.format, width, height);
            if (top_size == 0u || image_offset + top_size > bytes.size())
            {
                result.status = std::format("VTF top mip outside file (offset {}, size {}, file {})",
                    image_offset, top_size, bytes.size());
                return false;
            }
            if (!decode_vtf_pixels(bytes.data() + image_offset, top_size,
                result.format, width, height, result.image, result.status))
                return false;
            result.status = std::format("VTF {}.{} {}x{} format {}: {}",
                result.version_major, result.version_minor, width, height,
                static_cast<int>(result.format), result.status);
            return true;
        }

        bool source_declares_ssbump(const material_record_s& record,
            const texture_param_s& source, const std::uint32_t vtf_flags = 0u)
        {
            constexpr std::uint32_t textureflags_ssbump = 0x08000000u;
            const std::string probe = lower_slashes(source.source_name);
            return record.override_force_ssbump || record.ssbump ||
                probe.contains("ssbump") ||
                (vtf_flags & textureflags_ssbump) != 0u;
        }

        std::filesystem::path normalize_vtf_relative_path(std::string source_name)
        {
            source_name = lower_slashes(std::move(source_name));
            while (!source_name.empty() && source_name.front() == '/') source_name.erase(source_name.begin());
            if (source_name.starts_with("materials/")) source_name.erase(0u, 10u);
            std::filesystem::path relative(source_name);
            if (lower_slashes(relative.extension().string()) != ".vtf") relative += ".vtf";
            return relative;
        }


        struct vpk_entry_s
        {
            std::uint16_t archive_index = 0xffffu;
            std::uint32_t entry_offset = 0u;
            std::uint32_t entry_length = 0u;
            std::vector<std::uint8_t> preload;
        };

        struct vpk_index_s
        {
            bool parsed = false;
            bool valid = false;
            std::filesystem::path directory_file;
            std::uint32_t version = 0u;
            std::uint32_t tree_size = 0u;
            std::size_t file_data_base = 0u;
            std::unordered_map<std::string, vpk_entry_s> entries;
            std::string status;
        };

        std::unordered_map<std::string, vpk_index_s> g_vpk_indexes;

        std::string normalize_source_asset_path(std::string path)
        {
            path = lower_slashes(std::move(path));
            while (!path.empty() && path.front() == '/') path.erase(path.begin());
            while (path.starts_with("./")) path.erase(0u, 2u);
            std::string compact;
            compact.reserve(path.size());
            bool previous_slash = false;
            for (char c : path)
            {
                if (c == '/')
                {
                    if (previous_slash) continue;
                    previous_slash = true;
                }
                else previous_slash = false;
                compact.push_back(c);
            }
            return compact;
        }

        bool read_null_terminated(const std::vector<std::uint8_t>& bytes,
            std::size_t& cursor, const std::size_t end, std::string& value)
        {
            value.clear();
            if (cursor >= end) return false;
            const std::size_t begin = cursor;
            while (cursor < end && bytes[cursor] != 0u) ++cursor;
            if (cursor >= end) return false;
            value.assign(reinterpret_cast<const char*>(bytes.data() + begin),
                cursor - begin);
            ++cursor;
            return true;
        }

        bool parse_vpk_index(vpk_index_s& index)
        {
            index.parsed = true;
            index.valid = false;
            index.entries.clear();

            std::ifstream input(index.directory_file, std::ios::binary);
            if (!input.is_open())
            {
                index.status = "VPK directory could not be opened";
                return false;
            }
            input.seekg(0, std::ios::end);
            const std::streamoff length = input.tellg();
            input.seekg(0, std::ios::beg);
            if (length < 12)
            {
                index.status = "VPK directory is smaller than its header";
                return false;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
            input.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
            {
                index.status = "VPK directory read failed";
                return false;
            }

            constexpr std::uint32_t vpk_signature = 0x55aa1234u;
            if (read_le32(bytes, 0u) != vpk_signature)
            {
                index.status = "invalid VPK signature";
                return false;
            }
            index.version = read_le32(bytes, 4u);
            index.tree_size = read_le32(bytes, 8u);
            const std::size_t header_size = index.version == 1u
                ? 12u : (index.version == 2u ? 28u : 0u);
            if (header_size == 0u ||
                header_size + static_cast<std::size_t>(index.tree_size) > bytes.size())
            {
                index.status = std::format("unsupported or truncated VPK version {}",
                    index.version);
                return false;
            }
            const std::size_t tree_end = header_size + index.tree_size;
            index.file_data_base = tree_end;
            std::size_t cursor = header_size;

            while (cursor < tree_end)
            {
                std::string extension;
                if (!read_null_terminated(bytes, cursor, tree_end, extension))
                {
                    index.status = "VPK extension tree is truncated";
                    return false;
                }
                if (extension.empty()) break;

                while (cursor < tree_end)
                {
                    std::string directory;
                    if (!read_null_terminated(bytes, cursor, tree_end, directory))
                    {
                        index.status = "VPK directory tree is truncated";
                        return false;
                    }
                    if (directory.empty()) break;
                    if (directory == " ") directory.clear();

                    while (cursor < tree_end)
                    {
                        std::string filename;
                        if (!read_null_terminated(bytes, cursor, tree_end, filename))
                        {
                            index.status = "VPK filename tree is truncated";
                            return false;
                        }
                        if (filename.empty()) break;
                        if (cursor + 18u > tree_end)
                        {
                            index.status = "VPK file entry is truncated";
                            return false;
                        }

                        // CRC is retained by VPK but not required for extraction.
                        const std::uint16_t preload_bytes = read_le16(bytes, cursor + 4u);
                        vpk_entry_s entry;
                        entry.archive_index = read_le16(bytes, cursor + 6u);
                        entry.entry_offset = read_le32(bytes, cursor + 8u);
                        entry.entry_length = read_le32(bytes, cursor + 12u);
                        const std::uint16_t terminator = read_le16(bytes, cursor + 16u);
                        cursor += 18u;
                        if (terminator != 0xffffu ||
                            cursor + preload_bytes > tree_end)
                        {
                            index.status = "invalid VPK file-entry terminator/preload";
                            return false;
                        }
                        entry.preload.assign(bytes.begin() + cursor,
                            bytes.begin() + cursor + preload_bytes);
                        cursor += preload_bytes;

                        std::string key;
                        if (!directory.empty()) key = directory + "/";
                        key += filename + "." + extension;
                        index.entries.emplace(normalize_source_asset_path(std::move(key)),
                            std::move(entry));
                    }
                }
            }

            index.valid = true;
            index.status = std::format("indexed {} assets from VPK v{}",
                index.entries.size(), index.version);
            return true;
        }

        vpk_index_s* get_vpk_index(const std::filesystem::path& directory_file)
        {
            const std::string key = lower_slashes(
                std::filesystem::absolute(directory_file).generic_string());
            auto [it, inserted] = g_vpk_indexes.try_emplace(key);
            auto& index = it->second;
            if (inserted)
                index.directory_file = directory_file;
            if (!index.parsed)
                parse_vpk_index(index);
            return index.valid ? &index : nullptr;
        }

        std::filesystem::path vpk_archive_path(const std::filesystem::path& directory_file,
            const std::uint16_t archive_index)
        {
            std::string filename = directory_file.filename().string();
            const std::string lowered = lower_slashes(filename);
            const std::size_t suffix = lowered.rfind("_dir.vpk");
            if (suffix == std::string::npos) return {};
            std::ostringstream archive;
            archive << filename.substr(0u, suffix) << "_" << std::setfill('0')
                << std::setw(3) << archive_index << ".vpk";
            return directory_file.parent_path() / archive.str();
        }

        bool read_vpk_entry(const vpk_index_s& index, const vpk_entry_s& entry,
            std::vector<std::uint8_t>& output, std::string& status)
        {
            output = entry.preload;
            if (entry.entry_length == 0u)
            {
                status = "read VPK preload-only asset";
                return true;
            }

            std::filesystem::path source;
            std::uint64_t offset = entry.entry_offset;
            if (entry.archive_index == 0x7fffu)
            {
                source = index.directory_file;
                offset += index.file_data_base;
            }
            else
            {
                source = vpk_archive_path(index.directory_file, entry.archive_index);
                if (source.empty())
                {
                    status = "could not derive numbered VPK archive path";
                    return false;
                }
            }

            std::ifstream input(source, std::ios::binary);
            if (!input.is_open())
            {
                status = "VPK archive could not be opened: " + source.filename().string();
                return false;
            }
            input.seekg(0, std::ios::end);
            const std::uint64_t file_size =
                static_cast<std::uint64_t>(input.tellg());
            if (offset + entry.entry_length > file_size)
            {
                status = std::format("VPK entry outside archive (offset {}, size {}, file {})",
                    offset, entry.entry_length, file_size);
                return false;
            }
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            const std::size_t begin = output.size();
            output.resize(begin + entry.entry_length);
            input.read(reinterpret_cast<char*>(output.data() + begin),
                static_cast<std::streamsize>(entry.entry_length));
            if (input.gcount() != static_cast<std::streamsize>(entry.entry_length))
            {
                output.clear();
                status = "VPK asset data read failed";
                return false;
            }
            status = std::format("extracted {} bytes from {}",
                output.size(), source.filename().string());
            return true;
        }

        std::vector<std::filesystem::path> source_vpk_directories()
        {
            const auto root = game_root();
            // Follow the practical Source mount priority: local/addon overrides first,
            // then update/DLC content, then the base game.
            const std::array<std::filesystem::path, 8> game_dirs{
                root / "left4dead2" / "addons" / "workshop",
                root / "left4dead2" / "addons",
                root / "update",
                root / "left4dead2_dlc3",
                root / "left4dead2_dlc2",
                root / "left4dead2_dlc1",
                root / "left4dead2",
                root,
            };
            std::vector<std::filesystem::path> result;
            std::error_code ec;
            for (const auto& directory : game_dirs)
            {
                const auto preferred = directory / "pak01_dir.vpk";
                if (std::filesystem::is_regular_file(preferred, ec))
                    result.push_back(preferred);
                ec.clear();

                if (!std::filesystem::is_directory(directory, ec))
                {
                    ec.clear();
                    continue;
                }
                std::vector<std::filesystem::path> discovered;
                for (std::filesystem::directory_iterator it(directory, ec), end;
                    !ec && it != end; it.increment(ec))
                {
                    if (!it->is_regular_file(ec)) continue;
                    const std::string filename =
                        lower_slashes(it->path().filename().string());
                    if (!filename.ends_with(".vpk")) continue;

                    // Numbered chunks are raw data archives and contain no directory tree.
                    // Parse *_dir.vpk plus single-file addon/workshop VPKs only.
                    const bool directory_vpk = filename.ends_with("_dir.vpk");
                    const bool numbered_chunk = !directory_vpk &&
                        filename.size() >= 8u &&
                        filename[filename.size() - 8u] == '_' &&
                        std::isdigit(static_cast<unsigned char>(
                            filename[filename.size() - 7u])) != 0 &&
                        std::isdigit(static_cast<unsigned char>(
                            filename[filename.size() - 6u])) != 0 &&
                        std::isdigit(static_cast<unsigned char>(
                            filename[filename.size() - 5u])) != 0;
                    if (numbered_chunk) continue;
                    discovered.push_back(it->path());
                }
                std::sort(discovered.begin(), discovered.end());
                for (const auto& path : discovered)
                {
                    if (std::find(result.begin(), result.end(), path) == result.end())
                        result.push_back(path);
                }
                ec.clear();
            }
            return result;
        }

        bool extract_source_asset_from_vpk(const std::string& relative_asset,
            std::vector<std::uint8_t>& bytes, std::string& status)
        {
            const std::string key = normalize_source_asset_path(relative_asset);
            std::string last_status = "asset not found in indexed VPKs";
            for (const auto& directory_file : source_vpk_directories())
            {
                auto* index = get_vpk_index(directory_file);
                if (!index)
                {
                    last_status = "failed to parse " + directory_file.filename().string();
                    continue;
                }
                const auto found = index->entries.find(key);
                if (found == index->entries.end()) continue;
                if (read_vpk_entry(*index, found->second, bytes, last_status))
                {
                    status = directory_file.parent_path().filename().string() + "/" +
                        directory_file.filename().string() + ": " + last_status;
                    return true;
                }
            }
            status = last_status;
            return false;
        }

        bool write_binary_file(const std::filesystem::path& path,
            const std::vector<std::uint8_t>& bytes)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output.is_open()) return false;
            output.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            return output.good();
        }


        std::optional<std::filesystem::path> find_loose_source_asset(
            const std::string& relative_asset)
        {
            const std::string normalized = normalize_source_asset_path(relative_asset);
            const auto root = game_root();
            const std::array<std::filesystem::path, 7> roots{
                root / "left4dead2_dlc3",
                root / "left4dead2_dlc2",
                root / "left4dead2_dlc1",
                root / "update",
                root / "left4dead2",
                root,
                std::filesystem::current_path(),
            };
            std::error_code ec;
            for (const auto& base : roots)
            {
                const auto candidate = base / std::filesystem::path(normalized);
                if (std::filesystem::is_regular_file(candidate, ec))
                    return candidate;
                ec.clear();
            }
            return std::nullopt;
        }

        bool read_source_asset_bytes(const std::string& relative_asset,
            std::vector<std::uint8_t>& bytes, std::string& status)
        {
            if (const auto loose = find_loose_source_asset(relative_asset))
            {
                std::ifstream input(*loose, std::ios::binary);
                if (!input.is_open())
                {
                    status = "loose Source asset could not be opened";
                    return false;
                }
                input.seekg(0, std::ios::end);
                const std::streamoff length = input.tellg();
                input.seekg(0, std::ios::beg);
                if (length < 0)
                {
                    status = "loose Source asset length query failed";
                    return false;
                }
                bytes.resize(static_cast<std::size_t>(length));
                if (!bytes.empty())
                    input.read(reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
                if (!input.good() && !input.eof())
                {
                    bytes.clear();
                    status = "loose Source asset read failed";
                    return false;
                }
                status = "resolved loose Source asset";
                return true;
            }
            return extract_source_asset_from_vpk(relative_asset, bytes, status);
        }

        std::vector<std::string> tokenize_vmt(std::string_view text)
        {
            std::vector<std::string> tokens;
            std::size_t cursor = 0u;
            while (cursor < text.size())
            {
                while (cursor < text.size() &&
                    std::isspace(static_cast<unsigned char>(text[cursor])) != 0)
                    ++cursor;
                if (cursor >= text.size()) break;

                if (cursor + 1u < text.size() && text[cursor] == '/' &&
                    text[cursor + 1u] == '/')
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
                            const char escaped = text[cursor++];
                            token.push_back(escaped);
                        }
                        else token.push_back(c);
                    }
                }
                else
                {
                    const std::size_t begin = cursor;
                    while (cursor < text.size() &&
                        std::isspace(static_cast<unsigned char>(text[cursor])) == 0 &&
                        text[cursor] != '{' && text[cursor] != '}')
                    {
                        if (cursor + 1u < text.size() && text[cursor] == '/' &&
                            text[cursor + 1u] == '/')
                            break;
                        ++cursor;
                    }
                    token.assign(text.substr(begin, cursor - begin));
                }
                if (!token.empty()) tokens.push_back(std::move(token));
            }
            return tokens;
        }

        bool parse_vmt_bool(std::string value)
        {
            value = lower_slashes(std::move(value));
            return value == "1" || value == "true" || value == "yes" ||
                value == "on";
        }

        std::vector<std::string> source_vmt_candidates(material_record_s& record)
        {
            std::vector<std::string> candidates;
            if (!record.resolved_vmt_path.empty())
                append_unique(candidates, record.resolved_vmt_path);
            append_unique(candidates, "materials/" + record.canonical_name + ".vmt");
            if (material_exporter::m_resolve_model_cdmaterials)
            {
                const std::array<std::string, 2> keys{
                    lower_slashes(record.canonical_name), material_basename(record.canonical_name)
                };
                for (const auto& key : keys)
                {
                    const auto it = g_model_material_candidates.find(key);
                    if (it == g_model_material_candidates.end()) continue;
                    for (const auto& candidate : it->second)
                        append_unique(candidates, candidate, 64u);
                }
            }
            record.model_material_search_paths = candidates;
            record.model_candidate_count = static_cast<std::uint32_t>(candidates.size());
            return candidates;
        }

        bool resolve_record_source_vmt(material_record_s& record,
            std::vector<std::uint8_t>& bytes, std::string& status, std::string& resolved_path)
        {
            bytes.clear();
            status.clear();
            resolved_path.clear();
            const std::string direct = "materials/" + record.canonical_name + ".vmt";
            std::string last_status;
            for (const auto& candidate : source_vmt_candidates(record))
            {
                std::vector<std::uint8_t> candidate_bytes;
                std::string candidate_status;
                if (read_source_asset_bytes(candidate, candidate_bytes, candidate_status) &&
                    !candidate_bytes.empty())
                {
                    bytes = std::move(candidate_bytes);
                    status = candidate_status;
                    resolved_path = candidate;
                    record.resolved_via_model_search_path = candidate != direct;
                    return true;
                }
                if (!candidate_status.empty()) last_status = candidate + ": " + candidate_status;
            }
            status = last_status.empty() ? "no Source VMT candidate resolved" : last_status;
            record.resolved_via_model_search_path = false;
            return false;
        }

        const texture_param_s* find_texture_param(const material_record_s& record,
            const std::string_view parameter)
        {
            const std::string key = lower_slashes(std::string(parameter));
            const auto it = std::find_if(record.texture_params.begin(), record.texture_params.end(),
                [&](const texture_param_s& value) { return value.var_name == key; });
            return it == record.texture_params.end() ? nullptr : &*it;
        }

        source_shader_family_t classify_source_shader_family(const material_record_s& record)
        {
            const std::string shader = lower_slashes(record.source_shader_name.empty()
                ? record.shader_name : record.source_shader_name);
            if (shader.starts_with("vertexlitgeneric") || shader.starts_with("infected"))
                return source_shader_family_t::vertex_lit_model;
            if (shader.starts_with("lightmappedgeneric") || shader.starts_with("lightmappedreflective"))
                return source_shader_family_t::lightmapped_world;
            if (shader.starts_with("worldvertextransition"))
                return source_shader_family_t::world_vertex_transition;
            if (shader.starts_with("unlitgeneric")) return source_shader_family_t::unlit;
            if (shader.starts_with("water")) return source_shader_family_t::water;
            if (shader.starts_with("refract") || shader.contains("glass"))
                return source_shader_family_t::refract_or_glass;
            if (shader.contains("decal")) return source_shader_family_t::decal;
            if (shader.contains("sprite") || shader.contains("particle"))
                return source_shader_family_t::particle_or_sprite;
            if (!shader.empty()) return source_shader_family_t::special;
            return source_shader_family_t::unknown;
        }

        const char* source_shader_family_name(const source_shader_family_t family)
        {
            switch (family)
            {
            case source_shader_family_t::vertex_lit_model: return "VertexLit model";
            case source_shader_family_t::lightmapped_world: return "Lightmapped world";
            case source_shader_family_t::world_vertex_transition: return "WorldVertexTransition";
            case source_shader_family_t::unlit: return "Unlit / fullbright";
            case source_shader_family_t::refract_or_glass: return "Refract / glass";
            case source_shader_family_t::water: return "Water";
            case source_shader_family_t::decal: return "Decal";
            case source_shader_family_t::particle_or_sprite: return "Particle / sprite";
            case source_shader_family_t::special: return "Special Source shader";
            default: return "Unknown";
            }
        }

        void interpret_source_semantics(material_record_s& record)
        {
            record.semantic_conflicts.clear();
            const auto family = classify_source_shader_family(record);
            record.source_shader_family = source_shader_family_name(family);
            record.shader_semantics_valid = family != source_shader_family_t::unknown;
            record.unlit_fullbright = family == source_shader_family_t::unlit;

            if (record.additive) record.alpha_semantics = "Additive";
            else if (record.alpha_tested) record.alpha_semantics = "Alpha test / cutout";
            else if (record.translucent) record.alpha_semantics = "Translucent";
            else record.alpha_semantics = "Opaque";

            const bool has_bump = find_texture_param(record, "$bumpmap") ||
                find_texture_param(record, "$normalmap");
            const bool has_envmap_mask = find_texture_param(record, "$envmapmask") != nullptr;
            const bool has_phong_exponent_texture =
                find_texture_param(record, "$phongexponenttexture") != nullptr;

            record.specular_semantics = "None";
            if (record.phong_enabled)
            {
                if (record.base_alpha_phong_mask)
                    record.specular_semantics = "$basetexture alpha -> Phong/specular";
                else if (record.base_luminance_phong_mask)
                    record.specular_semantics = "$basetexture luminance -> Phong/specular";
                else if (has_bump)
                    record.specular_semantics = "$bumpmap alpha -> Phong/specular";
                else
                    record.specular_semantics = "Phong enabled without resolved mask";
                if (has_phong_exponent_texture)
                    record.specular_semantics += " + $phongexponenttexture roughness";
            }
            else if (record.envmap_enabled)
            {
                if (record.normal_alpha_envmap_mask)
                    record.specular_semantics = "$bumpmap/$normalmap alpha -> env reflection";
                else if (record.base_alpha_envmap_mask)
                {
                    record.specular_semantics =
                        "$basetexture alpha -> explicit inverted L4D2 env reflection mask";
                    if (record.translucent || record.alpha_tested)
                        append_unique(record.semantic_conflicts,
                            "$basealphaenvmapmask shares base alpha with transparency/cutout semantics");
                }
                else if (has_envmap_mask)
                {
                    const bool conflict = has_bump || record.lightwarp_enabled;
                    if (conflict && !material_exporter::m_allow_envmapmask_conflict_fallback)
                    {
                        record.specular_semantics = "$envmapmask rejected by Source shader conflict";
                        append_unique(record.semantic_conflicts,
                            "$envmapmask conflicts with $bumpmap/$lightwarptexture in the L4D2 Source shader path");
                    }
                    else
                    {
                        record.specular_semantics = conflict
                            ? "$envmapmask compatibility fallback (review)"
                            : "$envmapmask -> env reflection/specular";
                    }
                }
                else if (record.vtf_alpha_count > 0u && !record.translucent &&
                    !record.alpha_tested)
                    record.specular_semantics =
                        "$basetexture alpha -> automatic L4D2 env reflection mask";
                else
                    record.specular_semantics = "Unmasked $envmap reflection";
            }

            record.emission_semantics = "None";
            if (record.emissiveblend_enabled)
                record.emission_semantics = "$emissiveblend texture/mask";
            else if (record.selfillum_envmapmask_alpha)
                record.emission_semantics = "$envmapmask alpha -> self illumination";
            else if (find_texture_param(record, "$selfillummask"))
                record.emission_semantics = "$selfillummask texture";
            else if (record.selfillum_enabled)
                record.emission_semantics = "$basetexture alpha -> self illumination";
            else if (record.detail_texture && record.detail_blend_mode == 5)
            {
                record.emission_semantics = "$detail blend mode 5 -> unlit additive glow";
                record.emissive = true;
                record.emissive_intensity = std::max(record.emissive_intensity, 1.0f);
            }
            else if (record.unlit_fullbright)
            {
                record.emission_semantics = "Unlit fullbright (visible glow, not physical light)";
                record.emissive = true;
                record.emissive_intensity = std::max(record.emissive_intensity, 1.0f);
            }

            static constexpr std::array<int, 5> unsupported_l4d2_detail_modes{ 1, 4, 6, 9, 10 };
            if (record.detail_texture && std::find(unsupported_l4d2_detail_modes.begin(),
                unsupported_l4d2_detail_modes.end(), record.detail_blend_mode) !=
                unsupported_l4d2_detail_modes.end())
            {
                append_unique(record.semantic_conflicts, std::format(
                    "$detailblendmode {} is unsupported in Left 4 Dead 2",
                    record.detail_blend_mode));
            }
            if (record.lightwarp_enabled && record.detail_texture)
                append_unique(record.semantic_conflicts,
                    "$lightwarptexture and $detail can be mutually incompatible on Source shader branches");
            if (record.selfillum_enabled && record.translucent)
                append_unique(record.semantic_conflicts,
                    "$selfillum on a translucent material requires a Source-specific workaround");

            const bool animated_texture = std::any_of(record.texture_params.begin(),
                record.texture_params.end(), [](const texture_param_s& texture)
                {
                    return texture.frames > 1;
                });
            record.animated_or_proxy_driven = animated_texture || !record.detected_proxies.empty();
            if (!record.detected_proxies.empty())
                record.animation_semantics = "Proxy-driven: " + record.detected_proxies.front();
            else if (animated_texture)
                record.animation_semantics = "Animated VTF frames";
            else
                record.animation_semantics = "Static";
            record.dynamic_material |= record.animated_or_proxy_driven;

            if (material_exporter::m_preserve_dynamic_materials_for_review &&
                record.animated_or_proxy_driven)
            {
                record.confidence = std::min(record.confidence, 0.58f);
                append_unique(record.semantic_conflicts,
                    "Dynamic proxy/frame behavior cannot be represented by a single static PBR export");
            }
        }

        std::vector<float> parse_vmt_vector(std::string value)
        {
            for (auto& c : value)
            {
                if (c == '[' || c == ']' || c == '{' || c == '}' || c == ',') c = ' ';
            }
            std::istringstream stream(value);
            std::vector<float> result;
            float component = 0.0f;
            while (stream >> component) result.push_back(component);
            return result;
        }

        float vector_luminance(const std::string& value, const float fallback = 1.0f)
        {
            const auto components = parse_vmt_vector(value);
            if (components.empty()) return fallback;
            if (components.size() == 1u) return std::max(0.0f, components.front());
            const float r = components[0];
            const float g = components.size() > 1u ? components[1] : r;
            const float b = components.size() > 2u ? components[2] : g;
            return std::max(0.0f, r * 0.2126f + g * 0.7152f + b * 0.0722f);
        }

        bool effective_vmt_bool(const material_record_s& record, const std::string_view key,
            const bool fallback = false)
        {
            const auto it = record.effective_vmt_values.find(lower_slashes(std::string(key)));
            return it == record.effective_vmt_values.end() ? fallback : parse_vmt_bool(it->second);
        }

        std::optional<float> effective_vmt_float(const material_record_s& record,
            const std::string_view key)
        {
            const auto it = record.effective_vmt_values.find(lower_slashes(std::string(key)));
            if (it == record.effective_vmt_values.end()) return std::nullopt;
            return parse_float_value(it->second);
        }

        void materialize_effective_vmt(material_record_s& record)
        {
            const auto has = [&](const std::string_view key)
            {
                return record.effective_vmt_values.contains(lower_slashes(std::string(key)));
            };
            const auto value = [&](const std::string_view key) -> std::string
            {
                const auto it = record.effective_vmt_values.find(lower_slashes(std::string(key)));
                return it == record.effective_vmt_values.end() ? std::string{} : it->second;
            };

            if (has("$surfaceprop")) record.surface_prop = lower_slashes(value("$surfaceprop"));
            record.ssbump = effective_vmt_bool(record, "$ssbump", record.ssbump);
            record.phong_enabled = effective_vmt_bool(record, "$phong", record.phong_enabled);
            record.base_alpha_phong_mask = effective_vmt_bool(record,
                "$basemapalphaphongmask", record.base_alpha_phong_mask);
            record.base_luminance_phong_mask = effective_vmt_bool(record,
                "$basemapluminancephongmask", record.base_luminance_phong_mask);
            record.base_alpha_envmap_mask = effective_vmt_bool(record,
                "$basealphaenvmapmask", record.base_alpha_envmap_mask);
            record.normal_alpha_envmap_mask = effective_vmt_bool(record,
                "$normalmapalphaenvmapmask", record.normal_alpha_envmap_mask);
            record.invert_phong_mask = effective_vmt_bool(record,
                "$invertphongmask", record.invert_phong_mask);
            record.selfillum_enabled = effective_vmt_bool(record,
                "$selfillum", record.selfillum_enabled);
            record.selfillum_envmapmask_alpha = effective_vmt_bool(record,
                "$selfillum_envmapmask_alpha", record.selfillum_envmapmask_alpha);
            record.emissiveblend_enabled = effective_vmt_bool(record,
                "$emissiveblendenabled", record.emissiveblend_enabled) ||
                effective_vmt_bool(record, "$emissiveblend", false);
            record.alpha_tested = effective_vmt_bool(record, "$alphatest", record.alpha_tested);
            record.translucent = effective_vmt_bool(record, "$translucent", record.translucent);
            record.additive = effective_vmt_bool(record, "$additive", record.additive);
            record.nocull = effective_vmt_bool(record, "$nocull", record.nocull);

            if (const auto parsed = effective_vmt_float(record, "$phongexponent"))
                record.phong_exponent = *parsed;
            if (const auto parsed = effective_vmt_float(record, "$phongboost"))
                record.phong_boost = std::max(0.0f, *parsed);
            if (const auto parsed = effective_vmt_float(record, "$envmapcontrast"))
                record.envmap_contrast = std::max(0.0f, *parsed);
            if (const auto parsed = effective_vmt_float(record, "$envmapsaturation"))
                record.envmap_saturation = std::max(0.0f, *parsed);
            if (has("$envmaptint"))
                record.envmap_tint_luminance = vector_luminance(value("$envmaptint"), 1.0f);
            if (const auto parsed = effective_vmt_float(record, "$alphatestreference"))
                record.alpha_test_reference = std::clamp(*parsed, 0.0f, 1.0f);
            if (const auto parsed = effective_vmt_float(record, "$detailscale"))
                record.detail_scale = std::max(0.001f, *parsed);
            if (const auto parsed = effective_vmt_float(record, "$detailblendfactor"))
                record.detail_blend_factor = std::clamp(*parsed, 0.0f, 8.0f);
            if (const auto parsed = effective_vmt_float(record, "$detailblendmode"))
                record.detail_blend_mode = static_cast<int>(*parsed);
            if (const auto parsed = effective_vmt_float(record, "$emissiveblendstrength"))
                record.emissive_intensity = std::max(record.emissive_intensity, *parsed);

            for (const auto& [key, raw_value] : record.effective_vmt_values)
            {
                if (!is_probable_texture_parameter(key)) continue;
                const std::string source_name = lower_slashes(raw_value);
                if (!valid_texture_reference(source_name)) continue;
                auto& param = upsert_texture_param(record, key, source_name);
                param.is_normal = key.contains("bump") || key.contains("normal");
                record.has_normal |= param.is_normal;
            }

            record.lightwarp_enabled = has("$lightwarptexture");
            record.envmap_enabled = has("$envmap") &&
                lower_slashes(value("$envmap")) != "none";
            record.detail_texture = has("$detail") || record.detail_texture;
            record.dual_layer = has("$basetexture2") || has("$bumpmap2") ||
                classify_source_shader_family(record) == source_shader_family_t::world_vertex_transition;
            record.emissiveblend_enabled = record.emissiveblend_enabled ||
                has("$emissiveblendtexture") || has("$emissiveblendbasetexture") ||
                has("$emissiveblendflowtexture");
            record.selfillum_enabled = record.selfillum_enabled || has("$selfillummask");
            record.emissive = record.selfillum_enabled || record.selfillum_envmapmask_alpha ||
                record.emissiveblend_enabled;

            for (const auto frame_key : { "$frame", "$frame2", "$bumpframe", "$detailframe",
                "$envmapmaskframe" })
            {
                if (const auto parsed = effective_vmt_float(record, frame_key); parsed && *parsed != 0.0f)
                    record.animated_or_proxy_driven = true;
            }

            const auto detail_role = source_material_graph::classify_l4d2_detail_mode(
                record.detail_blend_mode);
            record.detail_semantics = record.detail_texture
                ? source_material_graph::detail_role_name(detail_role) : "None";
            record.detail_composition_ready = material_exporter::m_compose_detail_layers &&
                record.detail_texture &&
                detail_role != source_material_graph::detail_role::unsupported &&
                detail_role != source_material_graph::detail_role::none;
            record.layer_semantics = record.dual_layer
                ? (material_exporter::m_preserve_world_vertex_transition
                    ? "Two Source layers preserved with blend authoring assets"
                    : "Two Source layers detected; flattened export policy")
                : "Single Source layer";
            record.world_vertex_transition_preserved = record.dual_layer &&
                material_exporter::m_preserve_world_vertex_transition;

            const bool known_metal = record.surface_prop.contains("metal") ||
                record.canonical_name.contains("metal") || record.canonical_name.contains("chrome");
            source_material_graph::specular_input specular_input;
            specular_input.phong_enabled = record.phong_enabled;
            specular_input.envmap_enabled = record.envmap_enabled;
            specular_input.has_mask = has("$envmapmask") || record.base_alpha_envmap_mask ||
                record.normal_alpha_envmap_mask || record.base_alpha_phong_mask ||
                record.base_luminance_phong_mask || has("$phongexponenttexture");
            specular_input.exponent_texture = has("$phongexponenttexture");
            specular_input.known_metal = known_metal;
            specular_input.phong_exponent = record.phong_exponent;
            specular_input.phong_boost = record.phong_boost;
            specular_input.envmap_contrast = record.envmap_contrast;
            specular_input.envmap_saturation = record.envmap_saturation;
            specular_input.envmap_tint_luminance = record.envmap_tint_luminance;
            const auto calibrated = source_material_graph::calibrate_specular(specular_input);
            record.calibrated_roughness = calibrated.roughness;
            record.calibrated_reflection_weight = calibrated.reflection_weight;
            record.calibrated_dielectric_f0 = calibrated.dielectric_f0;
            record.calibrated_metallic_hint = calibrated.metallic_hint;
            record.calibrated_specular_workflow = calibrated.workflow;
            if (material_exporter::m_enable_calibrated_specular &&
                (record.phong_enabled || record.envmap_enabled))
            {
                record.roughness = calibrated.roughness;
                if (known_metal) record.metallic = std::max(record.metallic,
                    calibrated.metallic_hint);
            }

            const auto event_link = source_material_graph::classify_event_emissive_candidate(
                record.canonical_name, record.emissive, record.detected_proxies);
            record.event_emissive_candidate = material_exporter::m_link_event_emissive_materials &&
                event_link.candidate;
            record.event_emissive_semantics = event_link.reason;
            record.material_graph_summary = std::format(
                "{} | {} | detail: {} | layers: {} | event emissive: {}",
                record.source_shader_family, calibrated.workflow, record.detail_semantics,
                record.dual_layer ? 2 : 1, record.event_emissive_candidate ? "candidate" : "no");
        }

        bool merge_vmt_asset(material_record_s& record,
            const std::string& relative_vmt, std::unordered_set<std::string>& visited,
            const unsigned depth)
        {
            if (depth > 16u)
            {
                append_unique(record.vmt_patch_warnings, "VMT include depth exceeded 16 levels");
                return false;
            }
            std::string normalized = normalize_source_asset_path(relative_vmt);
            if (!normalized.starts_with("materials/")) normalized = "materials/" + normalized;
            if (!normalized.ends_with(".vmt")) normalized += ".vmt";
            if (!visited.insert(normalized).second)
            {
                append_unique(record.vmt_patch_warnings, "VMT include cycle/repeat: " + normalized);
                return true;
            }

            std::vector<std::uint8_t> bytes;
            std::string status;
            if (!read_source_asset_bytes(normalized, bytes, status) || bytes.empty())
            {
                append_unique(record.vmt_patch_warnings,
                    "Unable to resolve VMT include '" + normalized + "': " + status);
                return false;
            }

            append_unique(record.vmt_include_chain, normalized, 32u);
            const std::string text(bytes.begin(), bytes.end());
            const auto document = source_material_graph::parse_vmt(text);
            for (const auto& warning : document.warnings)
                append_unique(record.vmt_patch_warnings, normalized + ": " + warning, 64u);
            for (const auto& proxy : document.proxies)
            {
                append_unique(record.detected_proxies, proxy, 32u);
                if (proxy == "animatedtexture" || proxy == "toggletexture" ||
                    proxy == "texturescroll" || proxy == "texturetransform" ||
                    proxy == "sine" || proxy == "linearramp" ||
                    proxy == "playerproximity" || proxy == "entityrandom")
                    record.animated_or_proxy_driven = true;
            }

            bool include_ok = true;
            if (document.patch)
            {
                record.patch_material = true;
                record.patch_semantics = "Patch VMT with include/insert/replace merge";
                if (material_exporter::m_enable_patch_vmt_resolution)
                {
                    if (!document.include_path.empty())
                        include_ok = merge_vmt_asset(record, document.include_path, visited, depth + 1u);
                    else include_ok = false;
                }
                else
                {
                    include_ok = false;
                    append_unique(record.vmt_patch_warnings,
                        "Patch VMT resolution disabled; include base was not merged");
                }
                record.patch_include_resolved |= include_ok;
            }
            else if (!document.shader.empty())
            {
                record.source_shader_name = document.shader;
            }

            source_material_graph::apply_operations(record.effective_vmt_values, document);
            return include_ok;
        }

        void merge_declared_vmt_params(material_record_s& record)
        {
            if (record.canonical_name.empty()) return;
            std::vector<std::uint8_t> bytes;
            std::string status;
            std::string resolved_path;
            if (!resolve_record_source_vmt(record, bytes, status, resolved_path))
                return;
            record.resolved_vmt_path = resolved_path;
            record.effective_vmt_values.clear();
            record.vmt_include_chain.clear();
            record.vmt_patch_warnings.clear();
            record.patch_material = false;
            record.patch_include_resolved = false;
            record.patch_semantics = "Direct VMT";
            std::unordered_set<std::string> visited;
            merge_vmt_asset(record, resolved_path, visited, 0u);
            materialize_effective_vmt(record);
            if (material_exporter::m_enable_source_shader_semantics)
                interpret_source_semantics(record);
            materialize_effective_vmt(record);
            for (const auto& warning : record.vmt_patch_warnings)
                append_unique(record.semantic_conflicts, warning, 64u);
        }

        bool is_base_color_texture_parameter(const std::string& var_name)
        {
            const std::string key = lower_slashes(var_name);
            return key == "$basetexture" || key == "$basetexture2" ||
                key == "$iris" || key == "$corneatexture" ||
                key == "$flow_color_texture";
        }

        void accumulate_vtf_metadata(material_record_s& record,
            const std::vector<std::uint8_t>& bytes)
        {
            if (!material_exporter::m_inspect_vtf_metadata) return;
            if (bytes.size() < 64u || bytes[0] != 'V' || bytes[1] != 'T' ||
                bytes[2] != 'F' || bytes[3] != 0u)
            {
                ++record.vtf_unsupported_count;
                return;
            }
            const std::uint32_t version_major = read_le32(bytes, 4u);
            const std::uint32_t version_minor = read_le32(bytes, 8u);
            const std::uint32_t header_size = read_le32(bytes, 12u);
            const std::uint32_t width = read_le16(bytes, 16u);
            const std::uint32_t height = read_le16(bytes, 18u);
            const std::uint32_t flags = read_le32(bytes, 20u);
            const std::uint32_t frames = std::max<std::uint32_t>(1u, read_le16(bytes, 24u));
            const auto format = static_cast<ImageFormat>(read_le32(bytes, 52u));
            constexpr std::uint32_t textureflags_srgb = 0x00000040u;
            constexpr std::uint32_t textureflags_normal = 0x00000080u;
            constexpr std::uint32_t textureflags_onebitalpha = 0x00001000u;
            constexpr std::uint32_t textureflags_eightbitalpha = 0x00002000u;
            constexpr std::uint32_t textureflags_envmap = 0x00004000u;
            constexpr std::uint32_t textureflags_ssbump = 0x08000000u;
            if (frames > 1u) ++record.vtf_animated_count;
            if ((flags & (textureflags_onebitalpha | textureflags_eightbitalpha)) != 0u)
                ++record.vtf_alpha_count;
            if ((flags & textureflags_normal) != 0u) ++record.vtf_normal_flag_count;
            if ((flags & textureflags_ssbump) != 0u) ++record.vtf_ssbump_flag_count;
            if ((flags & textureflags_srgb) != 0u) ++record.vtf_srgb_count;
            if ((flags & textureflags_envmap) != 0u) ++record.vtf_cubemap_count;

            const bool supported_version = version_major == 7u && version_minor <= 6u;
            const bool valid_header = header_size >= 64u && header_size <= bytes.size();
            const bool valid_dimensions = width > 0u && height > 0u &&
                width <= 65536u && height <= 65536u;
            const bool known_format = vtf_level_size(format, 4u, 4u) != 0u;
            const bool block_compressed = format == IMAGE_FORMAT_DXT1 ||
                format == IMAGE_FORMAT_DXT1_ONEBITALPHA ||
                format == IMAGE_FORMAT_DXT3 || format == IMAGE_FORMAT_DXT5;
            const bool valid_block_dimensions = !block_compressed ||
                ((width % 4u) == 0u && (height % 4u) == 0u);
            if (!supported_version || !valid_header || !valid_dimensions ||
                !known_format || !valid_block_dimensions)
                ++record.vtf_unsupported_count;
        }

        void refresh_source_asset_inventory(material_record_s& record)
        {
            record.source_vmt_resolved = false;
            record.source_vmt_from_vpk = false;
            record.source_albedo_asset_resolved = false;
            record.asset_backed_material = false;
            record.source_vmt_hash = 0u;
            record.source_vmt_byte_size = 0u;
            record.declared_texture_count = 0u;
            record.resolved_texture_asset_count = 0u;
            record.missing_texture_asset_count = 0u;
            record.live_texture_param_count = 0u;
            record.vtf_animated_count = 0u;
            record.vtf_alpha_count = 0u;
            record.vtf_normal_flag_count = 0u;
            record.vtf_ssbump_flag_count = 0u;
            record.vtf_srgb_count = 0u;
            record.vtf_cubemap_count = 0u;
            record.vtf_unsupported_count = 0u;
            record.missing_source_assets.clear();

            if (record.canonical_name.empty())
            {
                record.source_vmt_status = "material has no canonical Source VMT name";
                record.source_asset_grade = "Unresolved";
                return;
            }

            std::vector<std::uint8_t> vmt_bytes;
            std::string vmt_status;
            std::string resolved_vmt;
            record.source_vmt_resolved = resolve_record_source_vmt(
                record, vmt_bytes, vmt_status, resolved_vmt);
            record.source_vmt_status = vmt_status;
            record.resolved_vmt_path = resolved_vmt;
            if (record.source_vmt_resolved)
            {
                record.source_vmt_hash = bytes_hash64(vmt_bytes);
                record.source_vmt_byte_size = static_cast<std::uint64_t>(vmt_bytes.size());
                const std::string lower_status = lower_slashes(vmt_status);
                record.source_vmt_from_vpk = lower_status.contains("vpk");
                record.effective_vmt_values.clear();
                record.vmt_include_chain.clear();
                record.vmt_patch_warnings.clear();
                record.patch_material = false;
                record.patch_include_resolved = false;
                record.patch_semantics = "Direct VMT";
                std::unordered_set<std::string> visited;
                merge_vmt_asset(record, resolved_vmt, visited, 0u);
                materialize_effective_vmt(record);
            }
            else
            {
                const auto candidates = source_vmt_candidates(record);
                if (!candidates.empty()) record.missing_source_assets.push_back(candidates.front());
            }

            std::unordered_set<std::string> scanned_assets;
            for (const auto& texture : record.texture_params)
            {
                if (texture.texture && texture.texture->vftable &&
                    !texture.texture->vftable->IsError(texture.texture))
                    ++record.live_texture_param_count;

                if (texture.is_cube || !valid_texture_reference(texture.source_name))
                    continue;

                const std::string asset = "materials/" +
                    normalize_vtf_relative_path(texture.source_name).generic_string();
                if (!scanned_assets.insert(asset).second)
                    continue;
                ++record.declared_texture_count;

                std::vector<std::uint8_t> bytes;
                std::string status;
                if (read_source_asset_bytes(asset, bytes, status) && !bytes.empty())
                {
                    ++record.resolved_texture_asset_count;
                    if (material_exporter::m_inspect_vtf_metadata)
                        accumulate_vtf_metadata(record, bytes);
                    if (is_base_color_texture_parameter(texture.var_name))
                        record.source_albedo_asset_resolved = true;
                }
                else
                {
                    ++record.missing_texture_asset_count;
                    if (record.missing_source_assets.size() < 32u)
                        record.missing_source_assets.push_back(asset);
                }
            }

            if (material_exporter::m_enable_source_shader_semantics)
                interpret_source_semantics(record);
            materialize_effective_vmt(record);
            for (const auto& warning : record.vmt_patch_warnings)
                append_unique(record.semantic_conflicts, warning, 64u);
            if (record.vtf_unsupported_count > 0u)
                append_unique(record.semantic_conflicts,
                    "one or more VTF assets use an unsupported version, format, header or block-compressed dimension layout");

            record.asset_backed_material = record.source_vmt_resolved &&
                record.source_albedo_asset_resolved;

            if (record.asset_backed_material && record.missing_texture_asset_count == 0u &&
                record.semantic_conflicts.empty() && record.vtf_unsupported_count == 0u)
                record.source_asset_grade = "Asset-backed semantic";
            else if (record.asset_backed_material && record.shader_semantics_valid)
                record.source_asset_grade = "Asset-backed partial / review";
            else if (record.source_vmt_resolved && record.live_texture_param_count > 0u)
                record.source_asset_grade = "Hybrid live fallback";
            else if (record.live_texture_param_count > 0u ||
                std::any_of(record.sampler_candidates.begin(), record.sampler_candidates.end(),
                    [](const auto& candidates) { return !candidates.empty(); }))
                record.source_asset_grade = "Live-only";
            else
                record.source_asset_grade = "Unresolved";
        }

        std::optional<std::filesystem::path> find_loose_source_vtf(const std::string& source_name)
        {
            const auto relative = normalize_vtf_relative_path(source_name);
            const auto root = game_root();
            const std::array<std::filesystem::path, 7> roots{
                root / "left4dead2_dlc3" / "materials",
                root / "left4dead2_dlc2" / "materials",
                root / "left4dead2_dlc1" / "materials",
                root / "left4dead2" / "materials",
                root / "update" / "materials",
                root / "materials",
                std::filesystem::current_path() / "materials",
            };
            std::error_code ec;
            for (const auto& base : roots)
            {
                const auto candidate = base / relative;
                if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
                ec.clear();
            }
            return std::nullopt;
        }

        bool acquire_declared_vtf(const texture_param_s& source,
            const std::filesystem::path& cache_dir, std::filesystem::path& path,
            std::string& status)
        {
            if (const auto loose = find_loose_source_vtf(source.source_name))
            {
                path = *loose;
                status = "resolved loose VTF from the exact VMT path";
                return true;
            }
            std::error_code ec;
            std::filesystem::create_directories(cache_dir, ec);
            path = cache_dir / (safe_component(source.var_name + "__" + source.source_name) + ".vtf");

            // Installed L4D2 assets normally live inside pak01_*.vpk. Full fixed-function
            // world rendering and mat_fastnobump can leave no usable live ITexture, so
            // resolve the exact VMT path directly from the VPK directory tree.
            std::vector<std::uint8_t> packed_bytes;
            std::string packed_status;
            const std::string packed_asset =
                "materials/" + normalize_vtf_relative_path(source.source_name).generic_string();
            if (extract_source_asset_from_vpk(packed_asset, packed_bytes, packed_status))
            {
                if (write_binary_file(path, packed_bytes))
                {
                    status = "resolved exact declared VTF from VPK: " + packed_status;
                    return true;
                }
                packed_status += "; cache write failed";
            }

            if (!source.texture || !source.texture->vftable ||
                source.texture->vftable->IsError(source.texture))
            {
                status = "declared VMT texture has no live ITexture/loose VTF; VPK resolver: " +
                    packed_status;
                return false;
            }
            if (std::filesystem::is_regular_file(path, ec) && !material_exporter::m_overwrite_existing_dds)
            {
                status = "reused cached VTF exported from Source ITexture";
                return true;
            }
            ec.clear();
            if (source.texture->vftable->SaveToFile(source.texture, path.string().c_str()))
            {
                status = "exported exact declared ITexture to VTF cache";
                return true;
            }
            source.texture->vftable->Download(source.texture, nullptr, 0);
            if (source.texture->vftable->SaveToFile(source.texture, path.string().c_str()))
            {
                status = "downloaded and exported exact declared ITexture to VTF cache";
                return true;
            }
            status = "ITexture::SaveToFile failed";
            return false;
        }

        IDirect3DTexture9* create_runtime_texture_from_rgba(const rgba_image_s& image)
        {
            if (image.width == 0u || image.height == 0u ||
                image.pixels.size() != static_cast<std::size_t>(image.width) * image.height * 4u)
                return nullptr;
            auto* device = game::get_d3d_device();
            if (!device) return nullptr;
            IDirect3DTexture9* texture = nullptr;
            if (FAILED(device->CreateTexture(image.width, image.height, 1u, 0u,
                D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)) || !texture)
                return nullptr;
            D3DLOCKED_RECT locked{};
            if (FAILED(texture->LockRect(0u, &locked, nullptr, 0u)))
            {
                texture->Release();
                return nullptr;
            }
            for (UINT y = 0; y < image.height; ++y)
            {
                auto* dst = static_cast<std::uint8_t*>(locked.pBits) +
                    static_cast<std::size_t>(y) * locked.Pitch;
                const auto* src = image.pixels.data() +
                    static_cast<std::size_t>(y) * image.width * 4u;
                for (UINT x = 0; x < image.width; ++x)
                {
                    dst[x * 4u + 0u] = src[x * 4u + 2u];
                    dst[x * 4u + 1u] = src[x * 4u + 1u];
                    dst[x * 4u + 2u] = src[x * 4u + 0u];
                    dst[x * 4u + 3u] = src[x * 4u + 3u];
                }
            }
            texture->UnlockRect(0u);
            return texture;
        }

        bool write_rgba_tga(const std::filesystem::path& path, const rgba_image_s& image)
        {
            if (image.width == 0u || image.height == 0u ||
                image.pixels.size() != static_cast<std::size_t>(image.width) * image.height * 4u)
                return false;

            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return false;

            std::array<std::uint8_t, 18> header{};
            header[2] = 2;
            header[12] = static_cast<std::uint8_t>(image.width & 0xffu);
            header[13] = static_cast<std::uint8_t>((image.width >> 8u) & 0xffu);
            header[14] = static_cast<std::uint8_t>(image.height & 0xffu);
            header[15] = static_cast<std::uint8_t>((image.height >> 8u) & 0xffu);
            header[16] = 32;
            header[17] = 0x28; // top-left origin, 8 alpha bits
            out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
            for (std::size_t i = 0; i < image.pixels.size(); i += 4u)
            {
                const std::array<std::uint8_t, 4> bgra{
                    image.pixels[i + 2u], image.pixels[i + 1u], image.pixels[i], image.pixels[i + 3u]
                };
                out.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
            }
            return out.good();
        }

        bool write_dynamic_gray_tga(const std::filesystem::path& path, const UINT width,
            const UINT height, const std::vector<std::uint8_t>& pixels)
        {
            if (width == 0u || height == 0u || pixels.size() != static_cast<std::size_t>(width) * height)
                return false;
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return false;
            std::array<std::uint8_t, 18> header{};
            header[2] = 3;
            header[12] = static_cast<std::uint8_t>(width & 0xffu);
            header[13] = static_cast<std::uint8_t>((width >> 8u) & 0xffu);
            header[14] = static_cast<std::uint8_t>(height & 0xffu);
            header[15] = static_cast<std::uint8_t>((height >> 8u) & 0xffu);
            header[16] = 8;
            header[17] = 0x20;
            out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
            out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
            return out.good();
        }

        std::uint8_t to_byte(const float value)
        {
            return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }

        float luminance(const std::uint8_t* rgba)
        {
            return (0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2]) / 255.0f;
        }

        image_quality_s analyse_scalar_pixels(const std::vector<std::uint8_t>& pixels)
        {
            image_quality_s result;
            if (pixels.empty())
            {
                result.status = "empty image";
                result.suspicious = true;
                return result;
            }

            std::array<bool, 256> histogram{};
            double sum = 0.0;
            double sum_sq = 0.0;
            std::uint8_t minimum = 255u;
            std::uint8_t maximum = 0u;
            const std::size_t stride = std::max<std::size_t>(1u, pixels.size() / 65536u);
            std::size_t samples = 0u;
            for (std::size_t index = 0u; index < pixels.size(); index += stride)
            {
                const std::uint8_t value = pixels[index];
                histogram[value] = true;
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
                const double normalized = value / 255.0;
                sum += normalized;
                sum_sq += normalized * normalized;
                ++samples;
            }

            result.valid = samples > 0u;
            result.minimum = minimum / 255.0f;
            result.maximum = maximum / 255.0f;
            result.mean = static_cast<float>(sum / std::max<std::size_t>(1u, samples));
            const double variance = std::max(0.0,
                sum_sq / std::max<std::size_t>(1u, samples) -
                static_cast<double>(result.mean) * result.mean);
            result.standard_deviation = static_cast<float>(std::sqrt(variance));
            result.unique_values = static_cast<std::uint32_t>(
                std::count(histogram.begin(), histogram.end(), true));
            result.constant = result.unique_values <= 2u ||
                static_cast<int>(maximum) - static_cast<int>(minimum) <= 2;
            result.suspicious = result.constant;
            result.status = result.constant
                ? "constant or near-constant scalar map"
                : std::format("usable scalar range {:.3f}-{:.3f}, sigma {:.3f}",
                    result.minimum, result.maximum, result.standard_deviation);
            return result;
        }

        image_quality_s analyse_rgba_pixels(const rgba_image_s& image,
            const texture_semantic_t semantic)
        {
            image_quality_s result;
            if (image.width == 0u || image.height == 0u || image.pixels.empty())
            {
                result.status = "empty image";
                result.suspicious = true;
                return result;
            }

            std::vector<std::uint8_t> scalar;
            scalar.reserve(static_cast<std::size_t>(image.width) * image.height);
            double normal_quality_sum = 0.0;
            std::size_t normal_samples = 0u;
            const std::size_t pixel_count = static_cast<std::size_t>(image.width) * image.height;
            const std::size_t stride = std::max<std::size_t>(1u, pixel_count / 65536u);
            for (std::size_t pixel = 0u; pixel < pixel_count; pixel += stride)
            {
                const auto* rgba = image.pixels.data() + pixel * 4u;
                if (semantic == texture_semantic_t::normal)
                {
                    const float x = rgba[0] / 127.5f - 1.0f;
                    const float y = rgba[1] / 127.5f - 1.0f;
                    const float xy_sq = x * x + y * y;
                    normal_quality_sum += xy_sq <= 1.08f ? 1.0 : 0.0;
                    ++normal_samples;
                    scalar.push_back(static_cast<std::uint8_t>(
                        std::clamp(std::lround(std::sqrt(std::min(xy_sq, 1.0f)) * 255.0f),
                            0l, 255l)));
                }
                else if (semantic == texture_semantic_t::scalar_mask ||
                    semantic == texture_semantic_t::height)
                {
                    scalar.push_back(rgba[0]);
                }
                else
                {
                    scalar.push_back(to_byte(luminance(rgba)));
                }
            }

            result = analyse_scalar_pixels(scalar);
            if (semantic == texture_semantic_t::normal)
            {
                result.normal_mean_length = normal_samples > 0u
                    ? static_cast<float>(normal_quality_sum / normal_samples)
                    : 0.0f;
                if (result.normal_mean_length < 0.92f)
                {
                    result.suspicious = true;
                    result.status = std::format(
                        "normal XY outside unit disk for {:.1f}% of samples",
                        (1.0f - result.normal_mean_length) * 100.0f);
                }
                else if (result.constant)
                {
                    result.status = "flat or near-flat normal map";
                }
                else
                {
                    result.status = std::format(
                        "normal map valid, XY validity {:.1f}%, sigma {:.3f}",
                        result.normal_mean_length * 100.0f,
                        result.standard_deviation);
                }
            }
            return result;
        }

        void set_generated_quality(channel_export_s& channel, const float value,
            const bool flat_normal)
        {
            channel.quality.valid = true;
            channel.quality.constant = true;
            channel.quality.suspicious = false;
            channel.quality.minimum = value;
            channel.quality.maximum = value;
            channel.quality.mean = value;
            channel.quality.standard_deviation = 0.0f;
            channel.quality.unique_values = 1u;
            channel.quality.normal_mean_length = flat_normal ? 1.0f : 0.0f;
            channel.quality.status = flat_normal
                ? "intentional generated flat normal"
                : "intentional generated constant fallback";
        }

        bool transform_texture_to_tga(IDirect3DBaseTexture9* texture,
            const std::filesystem::path& staging, const pixel_transform_t transform,
            const float base_roughness, const bool invert_y, const bool invert_mask,
            std::string& detail, image_quality_s* quality)
        {
            rgba_image_s source;
            if (!decode_texture_rgba(texture, source))
            {
                detail = "runtime texture could not be decoded to RGBA8";
                return false;
            }

            const bool alpha_has_signal = source.alpha_min < 250u ||
                static_cast<int>(source.alpha_max) - static_cast<int>(source.alpha_min) > 3;
            const bool red_has_variation =
                static_cast<int>(source.red_max) - static_cast<int>(source.red_min) > 3;

            if (transform == pixel_transform_t::normal_xy ||
                transform == pixel_transform_t::normal_dxt5nm ||
                transform == pixel_transform_t::normal_ssbump ||
                transform == pixel_transform_t::copy_color ||
                transform == pixel_transform_t::emission_color ||
                transform == pixel_transform_t::emission_from_alpha)
            {
                rgba_image_s output = source;
                for (std::size_t i = 0; i < source.pixels.size(); i += 4u)
                {
                    const auto* src = source.pixels.data() + i;
                    auto* dst = output.pixels.data() + i;
                    if (transform == pixel_transform_t::normal_xy ||
                        transform == pixel_transform_t::normal_dxt5nm)
                    {
                        dst[0] = transform == pixel_transform_t::normal_dxt5nm ? src[3] : src[0];
                        dst[1] = src[1];
                        if (invert_y) dst[1] = static_cast<std::uint8_t>(255u - dst[1]);
                        dst[2] = 255u;
                        dst[3] = 255u;
                    }
                    else if (transform == pixel_transform_t::normal_ssbump)
                    {
                        const float w0 = src[0] / 255.0f;
                        const float w1 = src[1] / 255.0f;
                        const float w2 = src[2] / 255.0f;
                        float nx = 0.81649658f * w0 - 0.40824829f * w1 - 0.40824829f * w2;
                        float ny = 0.70710678f * w1 - 0.70710678f * w2;
                        float nz = 0.57735027f * (w0 + w1 + w2);
                        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                        if (length > 0.00001f)
                        {
                            nx /= length;
                            ny /= length;
                        }
                        if (invert_y) ny = -ny;
                        dst[0] = to_byte(nx * 0.5f + 0.5f);
                        dst[1] = to_byte(ny * 0.5f + 0.5f);
                        dst[2] = 255u;
                        dst[3] = 255u;
                    }
                    else if (transform == pixel_transform_t::emission_from_alpha)
                    {
                        if (!alpha_has_signal)
                        {
                            detail = "base texture alpha has no self-illumination signal";
                            return false;
                        }
                        const float mask = src[3] / 255.0f;
                        dst[0] = to_byte((src[0] / 255.0f) * mask);
                        dst[1] = to_byte((src[1] / 255.0f) * mask);
                        dst[2] = to_byte((src[2] / 255.0f) * mask);
                        dst[3] = 255u;
                    }
                    else if (transform == pixel_transform_t::emission_color)
                    {
                        dst[3] = 255u;
                    }
                }
                detail = transform == pixel_transform_t::normal_dxt5nm ? "DXT5nm A/G decoded" :
                    (transform == pixel_transform_t::normal_ssbump ? "SSBump basis converted to tangent normal" :
                    (transform == pixel_transform_t::emission_from_alpha ? "RGB multiplied by base alpha self-illumination mask" :
                    "RGBA source preserved"));
                if (quality)
                {
                    const auto semantic = (transform == pixel_transform_t::normal_xy ||
                        transform == pixel_transform_t::normal_dxt5nm ||
                        transform == pixel_transform_t::normal_ssbump)
                        ? texture_semantic_t::normal
                        : (transform == pixel_transform_t::emission_color ||
                           transform == pixel_transform_t::emission_from_alpha
                            ? texture_semantic_t::emission
                            : texture_semantic_t::albedo);
                    *quality = analyse_rgba_pixels(output, semantic);
                }
                return write_rgba_tga(staging, output);
            }

            if ((transform == pixel_transform_t::scalar_alpha ||
                transform == pixel_transform_t::alpha_specular_mask_to_roughness) && !alpha_has_signal)
            {
                detail = "alpha channel has no usable signal";
                return false;
            }

            std::vector<std::uint8_t> output(static_cast<std::size_t>(source.width) * source.height);
            for (std::size_t pixel = 0; pixel < output.size(); ++pixel)
            {
                const auto* src = source.pixels.data() + pixel * 4u;
                float scalar = 0.0f;
                switch (transform)
                {
                case pixel_transform_t::scalar_red:
                    scalar = src[0] / 255.0f;
                    break;
                case pixel_transform_t::scalar_alpha:
                    scalar = src[3] / 255.0f;
                    break;
                case pixel_transform_t::scalar_luminance:
                    scalar = luminance(src);
                    break;
                case pixel_transform_t::phong_exponent_to_roughness:
                {
                    const float encoded = red_has_variation
                        ? src[0] / 255.0f
                        : (alpha_has_signal ? src[3] / 255.0f : src[0] / 255.0f);
                    const float exponent = std::max(1.0f, encoded * 255.0f);
                    scalar = std::clamp(std::sqrt(2.0f / (exponent + 2.0f)) * 1.08f,
                        0.035f, 1.0f);
                    break;
                }
                case pixel_transform_t::specular_mask_to_roughness:
                case pixel_transform_t::alpha_specular_mask_to_roughness:
                {
                    float mask = transform == pixel_transform_t::alpha_specular_mask_to_roughness
                        ? src[3] / 255.0f
                        : luminance(src);
                    if (invert_mask) mask = 1.0f - mask;
                    const float rough_at_zero = std::clamp(base_roughness + 0.24f, 0.08f, 1.0f);
                    const float rough_at_one = std::clamp(base_roughness - 0.20f, 0.035f, 0.92f);
                    scalar = rough_at_zero + (rough_at_one - rough_at_zero) * mask;
                    break;
                }
                case pixel_transform_t::albedo_luminance_to_roughness:
                {
                    const UINT x = static_cast<UINT>(pixel % source.width);
                    const UINT y = static_cast<UINT>(pixel / source.width);
                    float local_sum = 0.0f;
                    int local_count = 0;
                    for (int oy = -1; oy <= 1; ++oy)
                    {
                        const UINT sy = static_cast<UINT>(std::clamp<int>(static_cast<int>(y) + oy,
                            0, static_cast<int>(source.height) - 1));
                        for (int ox = -1; ox <= 1; ++ox)
                        {
                            const UINT sx = static_cast<UINT>(std::clamp<int>(static_cast<int>(x) + ox,
                                0, static_cast<int>(source.width) - 1));
                            local_sum += luminance(source.pixels.data() +
                                (static_cast<std::size_t>(sy) * source.width + sx) * 4u);
                            ++local_count;
                        }
                    }
                    const float lum = luminance(src);
                    const float local_average = local_sum / std::max(1, local_count);
                    const float micro_detail = std::abs(lum - local_average);
                    scalar = std::clamp(base_roughness + micro_detail * 0.90f +
                        (0.5f - lum) * 0.10f, 0.04f, 1.0f);
                    break;
                }
                case pixel_transform_t::ssbump_energy_to_height:
                {
                    const float energy = (src[0] + src[1] + src[2]) / (3.0f * 255.0f);
                    scalar = std::clamp((energy - 0.18f) / 0.82f, 0.0f, 1.0f);
                    break;
                }
                default:
                    scalar = luminance(src);
                    break;
                }
                output[pixel] = to_byte(scalar);
            }

            if (transform == pixel_transform_t::phong_exponent_to_roughness)
                detail = "Phong exponent converted to GGX roughness";
            else if (transform == pixel_transform_t::specular_mask_to_roughness ||
                transform == pixel_transform_t::alpha_specular_mask_to_roughness)
                detail = "Source specular mask remapped around inferred base roughness";
            else if (transform == pixel_transform_t::albedo_luminance_to_roughness)
                detail = "albedo luminance and local contrast converted to inferred roughness";
            else if (transform == pixel_transform_t::ssbump_energy_to_height)
                detail = "SSBump directional energy converted to diagnostic pseudo-height";
            else if (transform == pixel_transform_t::scalar_alpha)
                detail = "alpha channel extracted";
            else if (transform == pixel_transform_t::scalar_red)
                detail = "red channel extracted";
            else
                detail = "luminance extracted";
            if (quality) *quality = analyse_scalar_pixels(output);
            return write_dynamic_gray_tga(staging, source.width, source.height, output);
        }

        std::filesystem::path safe_material_relative_path(const std::string& canonical_name)
        {
            std::filesystem::path out;
            std::string normalized = lower_slashes(canonical_name);
            std::size_t begin = 0;
            while (begin < normalized.size())
            {
                const auto end = normalized.find('/', begin);
                const auto part = normalized.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
                if (!part.empty() && part != "." && part != "..") out /= safe_component(part);
                if (end == std::string::npos) break;
                begin = end + 1;
            }
            if (out.empty()) out = "material";
            return out;
        }

        std::filesystem::path material_dir(const material_record_s& record)
        {
            return materials_root_unlocked() / std::filesystem::path(record.export_relative_dir);
        }

        std::string material_file_stem(const material_record_s& record)
        {
            auto path = safe_material_relative_path(record.canonical_name);
            std::string stem = path.filename().string();
            if (stem.empty()) stem = "material_" + hash_hex(record.remix_hash);
            return safe_component(stem);
        }

        const texture_param_s* find_texture_param(const material_record_s& record,
            std::initializer_list<const char*> tokens)
        {
            for (const auto& texture : record.texture_params)
            {
                if (texture.is_cube || !valid_texture_reference(texture.source_name)) continue;
                for (const auto* token : tokens)
                    if (texture.var_name.contains(token)) return &texture;
            }
            return nullptr;
        }

        std::filesystem::path locate_texconv()
        {
            std::array<std::filesystem::path, 4> candidates{
                tools_root_unlocked() / "texconv.exe",
                game_root() / "l4d2-rtx" / "tools" / "texconv.exe",
                game_root() / "texconv.exe",
                export_root_unlocked() / "texconv.exe"
            };
            std::error_code ec;
            for (const auto& path : candidates)
                if (std::filesystem::is_regular_file(path, ec)) return path;

            wchar_t resolved[MAX_PATH]{};
            if (SearchPathW(nullptr, L"texconv.exe", nullptr, MAX_PATH, resolved, nullptr) > 0)
                return std::filesystem::path(resolved);
            return {};
        }

        std::wstring quoted(const std::filesystem::path& path)
        {
            std::wstring value = path.wstring();
            std::wstring out = L"\"";
            for (const wchar_t c : value)
            {
                if (c == L'\"') out += L"\\\"";
                else out.push_back(c);
            }
            out += L"\"";
            return out;
        }


        bool run_hidden_process(const std::wstring& command_line, const DWORD timeout_ms = 600000)
        {
            std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
            mutable_command.push_back(L'\0');
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
                return false;

            const DWORD wait = WaitForSingleObject(process.hProcess, timeout_ms);
            DWORD exit_code = 1;
            if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exit_code);
            else TerminateProcess(process.hProcess, 2);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return wait == WAIT_OBJECT_0 && exit_code == 0;
        }

        bool convert_with_texconv(const std::filesystem::path& source,
            const std::filesystem::path& target, const std::string& format,
            const std::wstring& extra_arguments)
        {
            if (!material_exporter::m_prefer_texconv) return false;
            const auto texconv = locate_texconv();
            if (texconv.empty()) return false;

            std::error_code ec;
            std::filesystem::create_directories(target.parent_path(), ec);
            const std::wstring command = quoted(texconv) + L" -nologo -y -dx10 -m 0 -f " +
                std::wstring(format.begin(), format.end()) + L" " + extra_arguments +
                L" -o " + quoted(target.parent_path()) + L" " + quoted(source);
            if (!run_hidden_process(command)) return false;

            auto produced = target.parent_path() / source.filename();
            produced.replace_extension(".dds");
            if (!std::filesystem::is_regular_file(produced, ec)) return false;
            if (produced != target)
            {
                std::filesystem::remove(target, ec);
                ec.clear();
                std::filesystem::rename(produced, target, ec);
                if (ec)
                {
                    ec.clear();
                    std::filesystem::copy_file(produced, target,
                        std::filesystem::copy_options::overwrite_existing, ec);
                    if (!ec) std::filesystem::remove(produced, ec);
                }
            }
            return std::filesystem::is_regular_file(target, ec);
        }

        bool convert_preview_png(const std::filesystem::path& source,
            const std::filesystem::path& target)
        {
            if (!material_exporter::m_export_debug_png) return false;
            const auto texconv = locate_texconv();
            if (texconv.empty() || !std::filesystem::is_regular_file(source)) return false;

            std::error_code ec;
            std::filesystem::create_directories(target.parent_path(), ec);
            const std::wstring command = quoted(texconv) +
                L" -nologo -y -m 1 -ft png -o " + quoted(target.parent_path()) +
                L" " + quoted(source);
            if (!run_hidden_process(command)) return false;

            auto produced = target.parent_path() / source.filename();
            produced.replace_extension(".png");
            if (!std::filesystem::is_regular_file(produced, ec)) return false;
            if (produced != target)
            {
                std::filesystem::remove(target, ec);
                ec.clear();
                std::filesystem::rename(produced, target, ec);
                if (ec)
                {
                    ec.clear();
                    std::filesystem::copy_file(produced, target,
                        std::filesystem::copy_options::overwrite_existing, ec);
                    if (!ec) std::filesystem::remove(produced, ec);
                }
            }
            return std::filesystem::is_regular_file(target, ec);
        }

        void fill_choice_metadata(channel_export_s& channel,
            const bound_texture_choice_s& choice, const texture_param_s* source,
            const std::string& source_kind)
        {
            channel.source_kind = source_kind;
            channel.source_texture_name = source ? source->source_name : "";
            channel.sampler_slot = choice.slot;
            channel.sampler_candidate = choice.candidate_index;
            channel.match_score = choice.score;
            channel.source_hits = choice.hits;
            channel.source_width = choice.desc.Width;
            channel.source_height = choice.desc.Height;

            const float score_confidence = std::clamp(
                (static_cast<float>(choice.score) - 20.0f) / 180.0f, 0.0f, 1.0f);
            const float hit_confidence = std::clamp(
                std::log2(static_cast<float>(choice.hits) + 1.0f) / 7.0f, 0.0f, 1.0f);
            channel.confidence = std::max(channel.confidence,
                0.45f + score_confidence * 0.35f + hit_confidence * 0.20f);
        }

        std::pair<UINT, UINT> fallback_dimensions(const bound_texture_choice_s& albedo,
            const texture_param_s* albedo_source)
        {
            UINT width = albedo.desc.Width;
            UINT height = albedo.desc.Height;
            if ((width == 0u || height == 0u) && albedo_source)
            {
                width = static_cast<UINT>(std::max(0, albedo_source->width));
                height = static_cast<UINT>(std::max(0, albedo_source->height));
            }
            if (width == 0u || height == 0u)
            {
                width = 256u;
                height = 256u;
            }
            width = std::clamp(width, 4u, 4096u);
            height = std::clamp(height, 4u, 4096u);
            return { width, height };
        }


        struct dds_info_s
        {
            bool valid = false;
            bool dx10 = false;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t mip_count = 0;
            std::uint32_t dxgi_format = 0;
        };

        dds_info_s read_dds_info(const std::filesystem::path& path)
        {
            dds_info_s info;
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) return info;
            std::array<unsigned char, 148> header{};
            in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
            if (in.gcount() < 128) return info;
            if (std::memcmp(header.data(), "DDS ", 4) != 0) return info;

            std::uint32_t header_size = 0;
            std::memcpy(&header_size, header.data() + 4, sizeof(header_size));
            if (header_size != 124u) return info;

            std::memcpy(&info.height, header.data() + 12, sizeof(info.height));
            std::memcpy(&info.width, header.data() + 16, sizeof(info.width));
            std::memcpy(&info.mip_count, header.data() + 28, sizeof(info.mip_count));
            if (info.mip_count == 0u) info.mip_count = 1u;
            info.valid = info.width > 0u && info.height > 0u;
            info.dx10 = std::memcmp(header.data() + 84, "DX10", 4) == 0;
            if (info.dx10 && in.gcount() >= 132)
                std::memcpy(&info.dxgi_format, header.data() + 128, sizeof(info.dxgi_format));
            return info;
        }

        std::optional<std::uint32_t> expected_dxgi_format(const std::string& format)
        {
            if (format == "BC4_UNORM") return 80u;
            if (format == "BC5_UNORM") return 83u;
            if (format == "BC7_UNORM") return 98u;
            if (format == "BC7_UNORM_SRGB") return 99u;
            return std::nullopt;
        }

        bool target_has_expected_dds(const std::filesystem::path& target, const std::string& format)
        {
            const auto expected = expected_dxgi_format(format);
            if (!expected) return false;
            const auto info = read_dds_info(target);
            const bool has_required_mips = (info.width <= 1u && info.height <= 1u) || info.mip_count > 1u;
            return info.valid && info.dx10 && info.dxgi_format == *expected && has_required_mips;
        }

        void quarantine_invalid_target(const std::filesystem::path& target)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(target, ec)) return;
            const auto rejected = conversion_cache_root_unlocked() / "rejected" /
                hash_hex(utils::string_hash64(target.generic_string()));
            std::filesystem::create_directories(rejected, ec);
            ec.clear();
            auto destination = rejected / target.filename();
            for (unsigned index = 1; std::filesystem::exists(destination, ec); ++index)
            {
                ec.clear();
                destination = rejected / (target.stem().string() + "_" + std::to_string(index) + target.extension().string());
            }
            ec.clear();
            std::filesystem::rename(target, destination, ec);
            if (ec)
            {
                ec.clear();
                std::filesystem::copy_file(target, destination,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec) std::filesystem::remove(target, ec);
            }
        }

        channel_export_s save_channel(IDirect3DBaseTexture9* texture,
            const std::filesystem::path& target, const std::filesystem::path& preview_target,
            const std::string& source_parameter, const std::string& format,
            const std::wstring& extra_arguments = {})
        {
            channel_export_s result;
            result.filename = target.filename().string();
            result.source_parameter = source_parameter;
            result.requested_format = format;
            result.provenance = channel_provenance_t::native_map;
            result.confidence = 0.88f;

            std::error_code existing_ec;
            if (std::filesystem::is_regular_file(target, existing_ec))
            {
                if (target_has_expected_dds(target, format) &&
                    !material_exporter::m_overwrite_existing_dds)
                {
                    result.ready = true;
                    result.texconv_used = true;
                    result.actual_status = "existing validated " + format + " DDS preserved";
                    result.preview_ready = convert_preview_png(target, preview_target);
                    if (result.preview_ready) result.preview_filename = preview_target.filename().string();
                    return result;
                }

                // V19 installed the raw D3D9 capture into the live mod before conversion.
                // Remix rejects those files (e.g. formats 201/207), so never keep them active.
                quarantine_invalid_target(target);
            }

            if (!texture)
            {
                result.actual_status = "source sampler unavailable";
                return result;
            }

            if (texture->GetType() != D3DRTYPE_TEXTURE)
            {
                result.actual_status = "non-2D Source texture skipped";
                result.provenance = channel_provenance_t::rejected;
                result.rejected = true;
                return result;
            }

            rgba_image_s decoded_quality_source;
            if (decode_texture_rgba(texture, decoded_quality_source))
            {
                result.quality = analyse_rgba_pixels(decoded_quality_source,
                    texture_semantic_t::albedo);
                if (result.quality.suspicious)
                    result.warnings.push_back(result.quality.status);
            }

            const auto staging = conversion_cache_root_unlocked() / "staging" /
                hash_hex(utils::string_hash64(target.generic_string())) / target.filename();
            if (!save_dds(texture, staging))
            {
                result.actual_status = "D3D9 DDS staging capture failed";
                return result;
            }
            result.preview_ready = convert_preview_png(staging, preview_target);
            if (result.preview_ready) result.preview_filename = preview_target.filename().string();

            // Synchronous mode installs only a verified Remix-ready DDS.
            if (!material_exporter::m_background_bc_conversion &&
                convert_with_texconv(staging, target, format, extra_arguments) &&
                target_has_expected_dds(target, format))
            {
                result.ready = true;
                result.texconv_used = true;
                result.actual_status = format + " with full mip chain";
                std::error_code ec;
                std::filesystem::remove(staging, ec);
                return result;
            }

            result.ready = false;
            result.fallback_dds = true;
            result.actual_status = locate_texconv().empty()
                ? "texconv.exe missing; DDS conversion not performed"
                : "texconv conversion failed";
            return result;
        }


        channel_export_s save_generated_channel(const std::filesystem::path& target,
            const std::filesystem::path& preview_target, const std::string& source_parameter,
            const std::string& format, const std::uint8_t value, const UINT width,
            const UINT height, const bool flat_normal = false)
        {
            channel_export_s result;
            result.filename = target.filename().string();
            result.source_parameter = source_parameter;
            result.requested_format = format;
            result.provenance = channel_provenance_t::constant_fallback;
            result.confidence = flat_normal ? 0.55f : 0.30f;
            set_generated_quality(result, flat_normal ? 0.5f : value / 255.0f, flat_normal);

            std::error_code ec;
            if (std::filesystem::is_regular_file(target, ec) &&
                target_has_expected_dds(target, format) &&
                !material_exporter::m_overwrite_existing_dds)
            {
                result.ready = true;
                result.texconv_used = true;
                result.generated = true;
                result.source_kind = "generated constant";
                result.source_width = width;
                result.source_height = height;
                result.actual_status = "existing validated " + format + " DDS preserved";
                result.preview_ready = convert_preview_png(target, preview_target);
                if (result.preview_ready) result.preview_filename = preview_target.filename().string();
                return result;
            }
            if (std::filesystem::is_regular_file(target, ec)) quarantine_invalid_target(target);

            const auto staging_dir = conversion_cache_root_unlocked() / "staging" /
                hash_hex(utils::string_hash64(target.generic_string()));
            auto source = staging_dir / target.filename();
            source.replace_extension(".tga");
            const bool written = flat_normal
                ? write_solid_tga(source, width, height, 128u, 128u, 255u)
                : write_gray_tga(source, width, height, value);
            if (!written)
            {
                result.actual_status = "generated PBR staging image failed";
                return result;
            }

            result.generated = true;
            result.source_kind = "generated constant";
            result.source_width = width;
            result.source_height = height;
            result.preview_ready = convert_preview_png(source, preview_target);
            if (result.preview_ready) result.preview_filename = preview_target.filename().string();

            if (convert_with_texconv(source, target, format, {}) &&
                target_has_expected_dds(target, format))
            {
                result.ready = true;
                result.texconv_used = true;
                result.actual_status = format + " generated with full mip chain";
                std::filesystem::remove(source, ec);
                return result;
            }

            result.fallback_dds = true;
            result.actual_status = locate_texconv().empty()
                ? "texconv.exe missing; generated PBR channel not exported"
                : "generated PBR conversion failed";
            return result;
        }


        channel_export_s save_generated_color_channel(const std::filesystem::path& target,
            const std::filesystem::path& preview_target, const std::string& source_parameter,
            const std::string& format, const std::uint8_t red, const std::uint8_t green,
            const std::uint8_t blue, const UINT width, const UINT height)
        {
            channel_export_s result;
            result.filename = target.filename().string();
            result.source_parameter = source_parameter;
            result.requested_format = format;
            result.generated = true;
            result.provenance = channel_provenance_t::constant_fallback;
            result.confidence = 0.45f;
            set_generated_quality(result,
                (0.2126f * red + 0.7152f * green + 0.0722f * blue) / 255.0f, false);
            result.source_kind = "generated color";
            result.source_width = width;
            result.source_height = height;

            std::error_code ec;
            if (std::filesystem::is_regular_file(target, ec) &&
                target_has_expected_dds(target, format) &&
                !material_exporter::m_overwrite_existing_dds)
            {
                result.ready = true;
                result.texconv_used = true;
                result.actual_status = "existing validated " + format + " DDS preserved";
                result.preview_ready = convert_preview_png(target, preview_target);
                if (result.preview_ready) result.preview_filename = preview_target.filename().string();
                return result;
            }
            if (std::filesystem::is_regular_file(target, ec)) quarantine_invalid_target(target);

            const auto staging_dir = conversion_cache_root_unlocked() / "staging" /
                hash_hex(utils::string_hash64(target.generic_string()));
            auto source = staging_dir / target.filename();
            source.replace_extension(".tga");
            if (!write_solid_tga(source, width, height, red, green, blue))
            {
                result.actual_status = "generated color staging image failed";
                return result;
            }

            result.preview_ready = convert_preview_png(source, preview_target);
            if (result.preview_ready) result.preview_filename = preview_target.filename().string();
            if (convert_with_texconv(source, target, format, {}) &&
                target_has_expected_dds(target, format))
            {
                result.ready = true;
                result.texconv_used = true;
                result.actual_status = format + " generated color with full mip chain";
                std::filesystem::remove(source, ec);
                return result;
            }

            result.fallback_dds = true;
            result.actual_status = locate_texconv().empty()
                ? "texconv.exe missing; generated color channel not exported"
                : "generated color conversion failed";
            return result;
        }


        channel_export_s save_transformed_channel(IDirect3DBaseTexture9* texture,
            const std::filesystem::path& target, const std::filesystem::path& preview_target,
            const std::string& source_parameter, const std::string& format,
            const pixel_transform_t transform,
            const float base_roughness = 0.5f, const bool invert_y = false,
            const bool invert_mask = false)
        {
            channel_export_s result;
            result.filename = target.filename().string();
            result.source_parameter = source_parameter;
            result.requested_format = format;
            result.provenance = channel_provenance_t::converted_map;
            result.confidence = 0.78f;

            std::error_code ec;
            if (std::filesystem::is_regular_file(target, ec) &&
                target_has_expected_dds(target, format) &&
                !material_exporter::m_overwrite_existing_dds)
            {
                result.ready = true;
                result.texconv_used = true;
                result.actual_status = "existing validated " + format + " DDS preserved";
                result.preview_ready = convert_preview_png(target, preview_target);
                if (result.preview_ready) result.preview_filename = preview_target.filename().string();
                return result;
            }
            if (std::filesystem::is_regular_file(target, ec)) quarantine_invalid_target(target);
            if (!texture)
            {
                result.actual_status = "source sampler unavailable";
                return result;
            }

            const auto staging_dir = conversion_cache_root_unlocked() / "staging" /
                hash_hex(utils::string_hash64(target.generic_string()));
            auto staging = staging_dir / target.filename();
            staging.replace_extension(".tga");
            std::string transform_detail;
            if (!transform_texture_to_tga(texture, staging, transform, base_roughness,
                invert_y, invert_mask, transform_detail, &result.quality))
            {
                result.actual_status = "semantic decode failed: " + transform_detail;
                return result;
            }

            const bool normal_transform = transform == pixel_transform_t::normal_xy ||
                transform == pixel_transform_t::normal_dxt5nm ||
                transform == pixel_transform_t::normal_ssbump;
            if (material_exporter::m_enable_quality_gate &&
                material_exporter::m_reject_suspicious_native_maps &&
                normal_transform && result.quality.suspicious &&
                !result.quality.constant)
            {
                result.provenance = channel_provenance_t::rejected;
                result.rejected = true;
                result.actual_status = "quality gate rejected normal map: " + result.quality.status;
                std::filesystem::remove(staging, ec);
                return result;
            }

            result.preview_ready = convert_preview_png(staging, preview_target);
            if (result.preview_ready) result.preview_filename = preview_target.filename().string();

            if (convert_with_texconv(staging, target, format, {}) &&
                target_has_expected_dds(target, format))
            {
                result.ready = true;
                result.texconv_used = true;
                result.actual_status = format + " semantic conversion: " + transform_detail;
                std::filesystem::remove(staging, ec);
                return result;
            }

            result.fallback_dds = true;
            result.actual_status = locate_texconv().empty()
                ? "texconv.exe missing after semantic decode"
                : "texconv failed after semantic decode";
            return result;
        }

        channel_export_s save_declared_vtf_channel(const texture_param_s& source,
            const std::filesystem::path& cache_dir,
            const std::filesystem::path& target,
            const std::filesystem::path& preview_target,
            const std::string& format,
            const pixel_transform_t transform,
            const float base_roughness = 0.5f,
            const bool invert_y = false,
            const bool invert_mask = false)
        {
            channel_export_s result;
            result.filename = target.filename().string();
            result.requested_format = format;
            result.source_parameter = source.var_name;
            result.source_texture_name = source.source_name;
            result.source_kind = "declared VMT -> direct VTF";

            std::filesystem::path declared_vtf;
            std::string status;
            if (!acquire_declared_vtf(source, cache_dir, declared_vtf, status))
            {
                result.actual_status = "direct VTF acquisition failed: " + status;
                return result;
            }

            vtf_decode_result_s decoded;
            if (!decode_vtf_top_mip(declared_vtf, decoded))
            {
                result.actual_status = "direct VTF decode failed: " + decoded.status;
                return result;
            }

            auto* direct_texture = create_runtime_texture_from_rgba(decoded.image);
            if (!direct_texture)
            {
                result.actual_status = "direct VTF decoded but temporary D3D texture creation failed";
                return result;
            }

            result = save_transformed_channel(direct_texture, target, preview_target,
                std::format("{} '{}' -> direct VTF", source.var_name, source.source_name),
                format, transform, base_roughness, invert_y, invert_mask);
            direct_texture->Release();

            result.source_parameter = source.var_name;
            result.source_texture_name = source.source_name;
            result.source_kind = "declared VMT -> direct VTF";
            result.source_width = decoded.image.width;
            result.source_height = decoded.image.height;
            result.match_score = 1000;
            result.derived = transform != pixel_transform_t::copy_color;
            result.confidence = 0.98f;
            if (!result.actual_status.empty())
                result.actual_status += "; " + status + "; " + decoded.status;
            return result;
        }


        void refresh_channel_state(channel_export_s& channel, const std::filesystem::path& directory)
        {
            if (channel.filename.empty()) return;
            const auto path = directory / channel.filename;
            std::error_code ec;
            channel.ready = std::filesystem::is_regular_file(path, ec);
            if (!channel.ready)
            {
                channel.texconv_used = false;
                channel.fallback_dds = false;
                channel.actual_status = "DDS missing";
                return;
            }

            const auto actual = read_dds_info(path);
            const auto expected = expected_dxgi_format(channel.requested_format);
            const bool has_required_mips = (actual.width <= 1u && actual.height <= 1u) || actual.mip_count > 1u;
            const bool requested_bc = actual.valid && actual.dx10 && expected &&
                actual.dxgi_format == *expected && has_required_mips;
            channel.ready = requested_bc;
            channel.texconv_used = requested_bc;
            channel.fallback_dds = !requested_bc;
            if (!requested_bc) quarantine_invalid_target(path);

            if (requested_bc)
            {
                const auto file_size = std::filesystem::file_size(path, ec);
                const bool dimension_mismatch = channel.source_width > 0u &&
                    channel.source_height > 0u &&
                    (actual.width != channel.source_width || actual.height != channel.source_height);
                const bool implausibly_small = !ec && actual.width * actual.height >= 16384u &&
                    file_size < 1536u;
                if (dimension_mismatch || implausibly_small)
                {
                    channel.quality.suspicious = true;
                    const std::string issue = dimension_mismatch
                        ? std::format("output DDS size {}x{} differs from resolved source {}x{}",
                            actual.width, actual.height,
                            channel.source_width, channel.source_height)
                        : std::format("output DDS is implausibly small ({} bytes for {}x{})",
                            file_size, actual.width, actual.height);
                    channel.quality.status = channel.quality.status == "not analysed"
                        ? issue
                        : channel.quality.status + "; " + issue;
                }
            }

            channel.actual_status = requested_bc
                ? channel.requested_format + " with full mip chain"
                : "DDS rejected until BC conversion completes";
        }

        void refresh_record_conversion_state(material_record_s& record)
        {
            const auto directory = material_dir(record);
            refresh_channel_state(record.albedo_channel, directory);
            if (record.albedo_layer1_channel.derived || record.albedo_layer1_channel.generated ||
                record.albedo_layer1_channel.provenance == channel_provenance_t::native_map)
                refresh_channel_state(record.albedo_layer1_channel, directory);
            else
                record.albedo_layer1_channel.ready = false;
            refresh_channel_state(record.normal_channel, directory);
            if (record.normal_layer1_channel.derived || record.normal_layer1_channel.generated)
                refresh_channel_state(record.normal_layer1_channel, directory);
            else
                record.normal_layer1_channel.ready = false;
            if (record.blend_modulate_channel.derived || record.blend_modulate_channel.generated ||
                record.blend_modulate_channel.provenance == channel_provenance_t::native_map ||
                record.blend_modulate_channel.provenance == channel_provenance_t::converted_map)
                refresh_channel_state(record.blend_modulate_channel, directory);
            else
                record.blend_modulate_channel.ready = false;
            refresh_channel_state(record.roughness_channel, directory);
            refresh_channel_state(record.metallic_channel, directory);

            // Height and emission are optional. Only refresh them when this export pass
            // actually resolved/generated the channel. Otherwise an old DDS left on disk
            // must not turn a rejected or missing semantic back into an active channel.
            if (record.height_channel.derived || record.height_channel.generated)
                refresh_channel_state(record.height_channel, directory);
            else
            {
                record.height_channel.ready = false;
                record.height_channel.texconv_used = false;
                record.height_channel.fallback_dds = false;
            }
            if (record.emissive_channel.derived || record.emissive_channel.generated)
                refresh_channel_state(record.emissive_channel, directory);
            else
            {
                record.emissive_channel.ready = false;
                record.emissive_channel.texconv_used = false;
                record.emissive_channel.fallback_dds = false;
            }
            if (record.opacity_channel.derived || record.opacity_channel.generated)
                refresh_channel_state(record.opacity_channel, directory);
            else
            {
                record.opacity_channel.ready = false;
                record.opacity_channel.texconv_used = false;
                record.opacity_channel.fallback_dds = false;
            }
            if (record.detail_channel.derived || record.detail_channel.generated)
                refresh_channel_state(record.detail_channel, directory);
            else
            {
                record.detail_channel.ready = false;
                record.detail_channel.texconv_used = false;
                record.detail_channel.fallback_dds = false;
            }

            record.albedo_exported = record.albedo_channel.ready;
            record.albedo_layer1_exported = record.albedo_layer1_channel.ready;
            record.normal_exported = record.normal_channel.ready;
            record.normal_layer1_exported = record.normal_layer1_channel.ready;
            record.blend_modulate_exported = record.blend_modulate_channel.ready;
            record.roughness_exported = record.roughness_channel.ready;
            record.metallic_exported = record.metallic_channel.ready;
            record.height_exported = record.height_channel.ready;
            record.emissive_exported = record.emissive_channel.ready;
            record.opacity_exported = record.opacity_channel.ready;
            record.detail_exported = record.detail_channel.ready;
            record.texconv_used = record.albedo_channel.texconv_used || record.albedo_layer1_channel.texconv_used ||
                record.normal_channel.texconv_used || record.normal_layer1_channel.texconv_used ||
                record.blend_modulate_channel.texconv_used || record.roughness_channel.texconv_used || record.metallic_channel.texconv_used ||
                record.height_channel.texconv_used || record.emissive_channel.texconv_used ||
                record.opacity_channel.texconv_used || record.detail_channel.texconv_used;
            record.conversion_pending = record.albedo_channel.fallback_dds || record.albedo_layer1_channel.fallback_dds ||
                record.normal_channel.fallback_dds || record.normal_layer1_channel.fallback_dds ||
                record.blend_modulate_channel.fallback_dds || record.roughness_channel.fallback_dds || record.metallic_channel.fallback_dds ||
                record.height_channel.fallback_dds || record.emissive_channel.fallback_dds ||
                record.opacity_channel.fallback_dds || record.detail_channel.fallback_dds;
            record.conversion_status = record.conversion_pending
                ? "Invalid live DDS quarantined; BC conversion is still pending"
                : (record.texconv_used ? "DDS ready in requested BC formats" : "No translated DDS activated");
        }

        std::string relative_asset_from_mod(const std::filesystem::path& path)
        {
            std::error_code ec;
            auto relative = std::filesystem::relative(path, export_root_unlocked(), ec);
            return "./" + usda_asset((ec ? path : relative).generic_string());
        }

        bool has_complete_pbr(const material_record_s& record)
        {
            return record.albedo_exported && record.normal_exported && record.roughness_exported;
        }

        bool channel_blocks_activation(const channel_export_s& channel)
        {
            return channel.rejected || channel.quality.suspicious ||
                (channel.ready && channel.confidence > 0.0f && channel.confidence < 0.35f);
        }

        void evaluate_material_activation(material_record_s& record)
        {
            record.material_grade = material_grade_t::rejected;
            record.auto_activated = false;
            record.quarantined = false;

            if (!has_complete_pbr(record))
            {
                record.activation_reason = "required albedo/normal/roughness DDS set is incomplete";
                record.quarantined = material_exporter::m_quarantine_low_confidence;
                return;
            }

            if (material_exporter::m_require_asset_backed_activation &&
                !record.asset_backed_material)
            {
                record.material_grade = material_grade_t::needs_review;
                record.auto_activated = material_exporter::m_auto_activate_review_materials;
                record.quarantined = material_exporter::m_quarantine_low_confidence &&
                    !record.auto_activated;
                record.activation_reason = record.source_vmt_resolved
                    ? "Source VMT resolved, but its declared base VTF could not be recovered from loose files or mounted VPKs"
                    : "Source VMT could not be recovered from loose files or mounted VPKs; live sampler data is authoring fallback only";
                return;
            }

            if (material_exporter::m_enable_source_shader_semantics &&
                !record.semantic_conflicts.empty())
            {
                record.material_grade = material_grade_t::needs_review;
                record.auto_activated = material_exporter::m_auto_activate_review_materials;
                record.quarantined = material_exporter::m_quarantine_low_confidence &&
                    !record.auto_activated;
                record.activation_reason = std::format(
                    "Source shader semantics require review: {}",
                    record.semantic_conflicts.front());
                return;
            }

            if (material_exporter::m_preserve_dynamic_materials_for_review &&
                record.animated_or_proxy_driven)
            {
                record.material_grade = material_grade_t::needs_review;
                record.auto_activated = material_exporter::m_auto_activate_review_materials;
                record.quarantined = material_exporter::m_quarantine_low_confidence &&
                    !record.auto_activated;
                record.activation_reason = std::format(
                    "dynamic Source material preserved for review ({})",
                    record.animation_semantics);
                return;
            }

            const bool required_suspicious = channel_blocks_activation(record.albedo_channel) ||
                channel_blocks_activation(record.normal_channel) ||
                channel_blocks_activation(record.roughness_channel);
            if (required_suspicious)
            {
                record.material_grade = material_grade_t::needs_review;
                record.activation_reason = "required channel failed the semantic quality policy";
                record.quarantined = material_exporter::m_quarantine_low_confidence;
                record.auto_activated = material_exporter::m_auto_activate_review_materials;
                return;
            }

            const bool has_declared_normal = find_exact_texture_param(record,
                { "$bumpmap", "$normalmap", "$bumpmap2", "$normalmap2",
                  "$detailnormalmap", "$flow_normal_texture" }) != nullptr;
            const bool intentional_neutral_normal = !has_declared_normal &&
                record.normal_channel.provenance == channel_provenance_t::constant_fallback &&
                record.normal_channel.ready && !record.normal_channel.rejected;
            const bool recognised_surface_profile = record.category != "generic" &&
                record.category != "unlit" && !record.surface_prop.empty();
            const bool grounded_inferred_roughness =
                record.roughness_channel.provenance == channel_provenance_t::inferred_map &&
                recognised_surface_profile;

            const float effective_normal_confidence = intentional_neutral_normal
                ? std::max(record.normal_channel.confidence, 0.82f)
                : record.normal_channel.confidence;
            const float effective_roughness_confidence = grounded_inferred_roughness
                ? std::max(record.roughness_channel.confidence, 0.62f)
                : record.roughness_channel.confidence;
            const float minimum_required_confidence = std::min({
                record.albedo_channel.confidence,
                effective_normal_confidence,
                effective_roughness_confidence });

            const bool required_native_or_converted =
                record.albedo_channel.provenance != channel_provenance_t::constant_fallback &&
                (record.normal_channel.provenance != channel_provenance_t::constant_fallback ||
                    intentional_neutral_normal) &&
                (record.roughness_channel.provenance != channel_provenance_t::constant_fallback ||
                    grounded_inferred_roughness);

            if (required_native_or_converted && !intentional_neutral_normal &&
                !grounded_inferred_roughness && minimum_required_confidence >= 0.84f)
                record.material_grade = material_grade_t::verified;
            else if (minimum_required_confidence >= material_exporter::m_minimum_auto_activation_confidence)
                record.material_grade = material_grade_t::usable;
            else
                record.material_grade = material_grade_t::needs_review;

            // Static RTX materials cannot faithfully replace draw-time composition used by
            // these Source shaders without a dedicated runtime bridge or a geometry-aware bake.
            // Export their assets and metadata, but keep them out of Materials_Auto.usda by
            // default so the automatic compiler never destroys WVT blending, Infected coloring,
            // refraction, flow water, animated particles or sky semantics.
            const bool runtime_composition_required = record.dual_layer || record.water_material ||
                record.glass_material || record.dynamic_material ||
                record.shader_profile == "world vertex transition" ||
                record.shader_profile == "infected coloring shader" ||
                record.shader_profile == "eye refract" ||
                record.shader_profile == "particle / sprite" ||
                record.shader_profile == "sky" ||
                record.shader_profile == "generic Source shader";

            if (runtime_composition_required)
            {
                record.material_grade = material_grade_t::needs_review;
                record.auto_activated = material_exporter::m_auto_activate_review_materials;
                record.quarantined = material_exporter::m_quarantine_low_confidence &&
                    !record.auto_activated;
                record.activation_reason = std::format(
                    "shader profile '{}' requires runtime composition or geometry-aware baking; assets exported for authoring only",
                    record.shader_profile);
                return;
            }

            if (record.material_grade == material_grade_t::verified ||
                record.material_grade == material_grade_t::usable)
            {
                record.auto_activated = true;
                record.activation_reason = std::format("{} material passed required-channel policy (min confidence {:.0f}%)",
                    material_grade_name(record.material_grade), minimum_required_confidence * 100.0f);
            }
            else
            {
                record.auto_activated = material_exporter::m_auto_activate_review_materials;
                record.quarantined = material_exporter::m_quarantine_low_confidence &&
                    !record.auto_activated;
                record.activation_reason = std::format(
                    "material requires review (min confidence {:.0f}%, shader profile {})",
                    minimum_required_confidence * 100.0f, record.shader_profile);
            }
        }

        void write_material_block(std::ostream& out, const material_record_s& record,
            const bool require_auto_activation = true)
        {
            if (!has_complete_pbr(record) ||
                (require_auto_activation && !record.auto_activated)) return;
            const auto dir = material_dir(record);
            const std::string prim = "mat_" + hash_hex(record.remix_hash);
            out << "        # materials/" << record.canonical_name << ".vmt | " << record.shader_name << "\n";
            out << "        over \"" << prim << "\"\n";
            out << "        {\n";
            out << "            over \"Shader\"\n";
            out << "            {\n";
            out << "                asset inputs:diffuse_texture = @"
                << relative_asset_from_mod(dir / record.albedo_channel.filename) << "@\n";
            out << "                custom asset inputs:normalmap_texture = @"
                << relative_asset_from_mod(dir / record.normal_channel.filename) << "@\n";
            out << "                custom asset inputs:reflectionroughness_texture = @"
                << relative_asset_from_mod(dir / record.roughness_channel.filename) << "@\n";
            if (record.metallic_exported)
                out << "                custom asset inputs:metallic_texture = @"
                    << relative_asset_from_mod(dir / record.metallic_channel.filename) << "@\n";
            if (record.height_exported)
            {
                const float displacement_scale = record.height_channel.source_kind.contains("pseudo-height")
                    ? 0.015f : 0.05f;
                out << "                custom float inputs:displace_in = " << displacement_scale << "\n";
                out << "                custom float inputs:displace_out = 0\n";
                out << "                custom asset inputs:height_texture = @"
                    << relative_asset_from_mod(dir / record.height_channel.filename) << "@\n";
            }
            if (record.emissive_exported)
            {
                out << "                custom float inputs:emissive_intensity = "
                    << std::max(1.0f, record.emissive_intensity) << "\n";
                out << "                custom asset inputs:emissive_mask_texture = @"
                    << relative_asset_from_mod(dir / record.emissive_channel.filename) << "@\n";
                out << "                custom bool inputs:enable_emission = 1\n";
            }
            if (record.opacity_exported)
            {
                out << "                custom bool inputs:enable_opacity = 1\n";
                out << "                custom asset inputs:opacity_texture = @"
                    << relative_asset_from_mod(dir / record.opacity_channel.filename) << "@\n";
                if (record.alpha_tested)
                    out << "                custom float inputs:opacity_threshold = "
                        << record.alpha_test_reference << "\n";
            }
            out << "            }\n";
            out << "        }\n";
        }

        bool export_record_files(material_record_s& record)
        {
            record.warnings.clear();
            apply_override(record);
            const auto dir = material_dir(record);
            const auto raw_dir = dir / "source_vtf";
            const auto resolver_vtf_cache = material_exporter::m_export_raw_vtf
                ? raw_dir
                : conversion_cache_root_unlocked() / "source_vtf" / record.identity_key;
            const auto preview_dir = dir / "debug_png";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::filesystem::create_directories(resolver_vtf_cache, ec);
            if (material_exporter::m_export_raw_vtf)
                std::filesystem::create_directories(raw_dir, ec);
            if (material_exporter::m_export_debug_png)
                std::filesystem::create_directories(preview_dir, ec);
            if (ec) return false;

            const std::string stem = material_file_stem(record);
            record.albedo_channel = {};
            record.albedo_layer1_channel = {};
            record.normal_channel = {};
            record.normal_layer1_channel = {};
            record.blend_modulate_channel = {};
            record.roughness_channel = {};
            record.metallic_channel = {};
            record.height_channel = {};
            record.emissive_channel = {};
            record.opacity_channel = {};
            record.detail_channel = {};
            record.albedo_channel.filename = stem + "_Albedo.a.rtex.dds";
            record.albedo_channel.requested_format = "BC7_UNORM";
            record.albedo_layer1_channel.filename = stem + "_Layer1_Albedo.a.rtex.dds";
            record.albedo_layer1_channel.requested_format = "BC7_UNORM";
            record.normal_channel.filename = stem + "_OTH_Normal.n.rtex.dds";
            record.normal_channel.requested_format = "BC5_UNORM";
            record.normal_layer1_channel.filename = stem + "_Layer1_OTH_Normal.n.rtex.dds";
            record.normal_layer1_channel.requested_format = "BC5_UNORM";
            record.blend_modulate_channel.filename = stem + "_BlendModulate.r.rtex.dds";
            record.blend_modulate_channel.requested_format = "BC4_UNORM";
            record.roughness_channel.filename = stem + "_Roughness.r.rtex.dds";
            record.roughness_channel.requested_format = "BC4_UNORM";
            record.metallic_channel.filename = stem + "_Metalness.m.rtex.dds";
            record.metallic_channel.requested_format = "BC4_UNORM";
            record.height_channel.filename = stem + "_Displacement.a.rtex.dds";
            record.height_channel.requested_format = "BC4_UNORM";
            record.emissive_channel.filename = stem + "_Emission.e.rtex.dds";
            record.emissive_channel.requested_format = "BC7_UNORM";
            record.opacity_channel.filename = stem + "_Opacity.o.rtex.dds";
            record.opacity_channel.requested_format = "BC4_UNORM";
            record.detail_channel.filename = stem + "_Detail.a.rtex.dds";
            record.detail_channel.requested_format = "BC7_UNORM";

            const auto preview_path = [&](const char* suffix)
            {
                return preview_dir / (stem + suffix + ".png");
            };

            // Optional channels may disappear after a stricter semantic re-evaluation.
            // Delete their previous files before resolving so refresh_channel_state cannot
            // resurrect stale false-positive height/emission maps from an older export.
            if (material_exporter::m_overwrite_existing_dds)
            {
                std::filesystem::remove(dir / record.height_channel.filename, ec);
                std::filesystem::remove(dir / record.emissive_channel.filename, ec);
                std::filesystem::remove(dir / record.normal_layer1_channel.filename, ec);
                std::filesystem::remove(dir / record.albedo_layer1_channel.filename, ec);
                std::filesystem::remove(dir / record.blend_modulate_channel.filename, ec);
                std::filesystem::remove(dir / record.opacity_channel.filename, ec);
                std::filesystem::remove(dir / record.detail_channel.filename, ec);
                std::filesystem::remove(preview_path("_Height"), ec);
                std::filesystem::remove(preview_path("_Emission"), ec);
                std::filesystem::remove(preview_path("_Layer1_Normal"), ec);
                std::filesystem::remove(preview_path("_Layer1_Albedo"), ec);
                std::filesystem::remove(preview_path("_BlendModulate"), ec);
                std::filesystem::remove(preview_path("_Opacity"), ec);
                std::filesystem::remove(preview_path("_Detail"), ec);
                ec.clear();
            }

            {
                std::ofstream vmt(dir / "source_runtime.vmt", std::ios::trunc);
                vmt << "// Reconstructed from the live Source IMaterial table.\n";
                const std::string original_vmt_asset = record.resolved_vmt_path.empty()
                    ? "materials/" + record.canonical_name + ".vmt"
                    : record.resolved_vmt_path;
                vmt << "// Original: " << original_vmt_asset << "\n";
                vmt << record.runtime_vmt;

                std::vector<std::uint8_t> original_vmt;
                std::string original_vmt_status;
                if (read_source_asset_bytes(original_vmt_asset, original_vmt,
                    original_vmt_status) && !original_vmt.empty())
                {
                    write_binary_file(dir / "source_original.vmt", original_vmt);
                }
            }

            if (material_exporter::m_export_raw_vtf)
            {
                for (auto& texture : record.texture_params)
                {
                    if (texture.raw_exported || texture.is_cube ||
                        !valid_texture_reference(texture.source_name))
                        continue;
                    const auto filename = safe_component(texture.var_name + "__" + texture.source_name) + ".vtf";
                    const auto target = raw_dir / filename;
                    std::error_code raw_ec;
                    if (std::filesystem::is_regular_file(target, raw_ec))
                    {
                        texture.raw_exported = true;
                        continue;
                    }

                    std::vector<std::uint8_t> original_vtf;
                    std::string original_vtf_status;
                    const std::string original_vtf_asset = "materials/" +
                        normalize_vtf_relative_path(texture.source_name).generic_string();
                    if (read_source_asset_bytes(original_vtf_asset, original_vtf,
                        original_vtf_status) && !original_vtf.empty())
                    {
                        texture.raw_exported = write_binary_file(target, original_vtf);
                    }
                    else if (material_exporter::m_allow_live_sampler_fallback &&
                        texture.texture && texture.texture->vftable &&
                        !texture.texture->vftable->IsError(texture.texture))
                    {
                        texture.raw_exported =
                            texture.texture->vftable->SaveToFile(texture.texture, target.string().c_str());
                    }
                }
            }

            std::unordered_set<IDirect3DBaseTexture9*> used;
            const texture_param_s* albedo_source = nullptr;
            const auto albedo_choice = choose_albedo_texture(record, &albedo_source);
            auto [fallback_width, fallback_height] = fallback_dimensions(albedo_choice, albedo_source);

            if (!albedo_choice)
            {
                std::uint64_t best_area = 0;
                for (const auto& candidates : record.sampler_candidates)
                {
                    for (const auto& candidate : candidates)
                    {
                        D3DSURFACE_DESC desc{};
                        if (!texture_desc(candidate.texture, desc)) continue;
                        const std::uint64_t area = static_cast<std::uint64_t>(desc.Width) * desc.Height;
                        if (area <= best_area || desc.Width < 8u || desc.Height < 8u) continue;
                        best_area = area;
                        fallback_width = std::clamp(desc.Width, 4u, 4096u);
                        fallback_height = std::clamp(desc.Height, 4u, 4096u);
                    }
                }
            }

            if (albedo_choice)
            {
                record.albedo_channel = save_channel(albedo_choice.texture,
                    dir / record.albedo_channel.filename, preview_path("_Albedo"),
                    std::format("{} '{}' -> runtime sampler {} candidate {}",
                        albedo_source ? albedo_source->var_name : "$basetexture fallback",
                        albedo_source ? albedo_source->source_name : "",
                        albedo_choice.slot, albedo_choice.candidate_index),
                    "BC7_UNORM");
                fill_choice_metadata(record.albedo_channel, albedo_choice, albedo_source,
                    "declared VMT texture linked to runtime sampler");
                used.insert(albedo_choice.texture);
            }
            else if (albedo_source)
            {
                record.albedo_channel = save_declared_vtf_channel(*albedo_source,
                    resolver_vtf_cache, dir / record.albedo_channel.filename,
                    preview_path("_Albedo"), "BC7_UNORM", pixel_transform_t::copy_color);
                record.albedo_channel.provenance = record.albedo_channel.ready
                    ? channel_provenance_t::native_map
                    : record.albedo_channel.provenance;
                record.albedo_channel.confidence = record.albedo_channel.ready ? 1.0f : 0.0f;
                record.albedo_channel.source_kind = "declared VMT -> direct VTF albedo";
            }
            else if (record.water_material && material_exporter::m_generate_water_albedo)
            {
                record.albedo_channel = save_generated_color_channel(
                    dir / record.albedo_channel.filename, preview_path("_Albedo"),
                    "$refracttint -> generated neutral water albedo",
                    "BC7_UNORM",
                    to_byte(record.water_tint[0]), to_byte(record.water_tint[1]),
                    to_byte(record.water_tint[2]), fallback_width, fallback_height);
            }
            else
            {
                record.albedo_channel.source_parameter = "$basetexture unavailable";
                record.albedo_channel.actual_status = "no usable albedo sampler observed";
            }
            record.albedo_exported = record.albedo_channel.ready;

            const texture_param_s* normal_source = nullptr;
            if (record.override_normal_source != "none")
            {
                normal_source = find_texture_param_by_name(record, record.override_normal_source);
                if (!normal_source && record.override_normal_source == "auto")
                    normal_source = find_exact_texture_param(record,
                        { "$bumpmap", "$normalmap", "$bumpmap2", "$flow_normal_texture", "$normalmap2" });
                if (!normal_source && record.override_normal_source == "auto")
                    normal_source = find_texture_param(record, { "bump", "normal", "flow_normal" });
                if (!normal_source && record.override_normal_source != "auto")
                    record.warnings.push_back(std::format(
                        "Override normal_source '{}' did not resolve to a captured VMT texture.",
                        record.override_normal_source));
            }
            bound_texture_choice_s normal_choice;
            bool direct_vtf_decoded = false;
            std::string direct_vtf_status;
            if (normal_source)
            {
                std::filesystem::path declared_vtf;
                if (acquire_declared_vtf(*normal_source, resolver_vtf_cache, declared_vtf, direct_vtf_status))
                {
                    vtf_decode_result_s decoded;
                    if (decode_vtf_top_mip(declared_vtf, decoded))
                    {
                        if (auto* direct_texture = create_runtime_texture_from_rgba(decoded.image))
                        {
                            const bool use_ssbump = source_declares_ssbump(record, *normal_source, decoded.flags);
                            const pixel_transform_t transform = use_ssbump
                                ? pixel_transform_t::normal_ssbump
                                : (decoded.format == IMAGE_FORMAT_DXT5
                                    ? pixel_transform_t::normal_dxt5nm
                                    : pixel_transform_t::normal_xy);
                            const std::string decoder = use_ssbump
                                ? "SSBump basis -> tangent normal"
                                : (decoded.format == IMAGE_FORMAT_DXT5
                                    ? "DXT5nm A/G -> tangent normal"
                                    : (decoded.format == IMAGE_FORMAT_ATI2N
                                        ? "ATI2N/BC5 R/G -> tangent normal"
                                        : "R/G -> tangent normal"));
                            record.normal_channel = save_transformed_channel(direct_texture,
                                dir / record.normal_channel.filename, preview_path("_Normal"),
                                std::format("{} '{}' -> direct VTF -> {}",
                                    normal_source->var_name, normal_source->source_name, decoder),
                                "BC5_UNORM", transform, record.roughness,
                                material_exporter::m_invert_normal_y || record.override_invert_normal_y, false);
                            direct_texture->Release();
                            direct_vtf_decoded = true;
                            record.normal_channel.derived = true;
                            record.normal_channel.provenance = channel_provenance_t::converted_map;
                            record.normal_channel.confidence = use_ssbump ? 1.0f : 0.98f;
                            record.normal_channel.source_parameter = normal_source->var_name;
                            record.normal_channel.source_texture_name = normal_source->source_name;
                            record.normal_channel.source_kind = use_ssbump
                                ? "declared VMT -> direct VTF -> SSBump"
                                : "declared VMT -> direct VTF normal";
                            record.normal_channel.source_width = decoded.image.width;
                            record.normal_channel.source_height = decoded.image.height;
                            record.normal_channel.match_score = 1000;
                            direct_vtf_status += "; " + decoded.status;
                        }
                        else direct_vtf_status += "; temporary D3D texture creation failed";
                    }
                    else direct_vtf_status += "; " + decoded.status;
                }
                if (!direct_vtf_decoded)
                    normal_choice = choose_bound_texture(record, *normal_source,
                        texture_semantic_t::normal, used, false);
            }

            if (!direct_vtf_decoded && normal_choice)
            {
                const bool use_ssbump = source_declares_ssbump(record, *normal_source);
                const pixel_transform_t transform = use_ssbump
                    ? pixel_transform_t::normal_ssbump
                    : (normal_choice.desc.Format == D3DFMT_DXT5
                        ? pixel_transform_t::normal_dxt5nm
                        : pixel_transform_t::normal_xy);
                const std::string decoder = use_ssbump
                    ? "SSBump basis -> tangent normal"
                    : (normal_choice.desc.Format == D3DFMT_DXT5
                        ? "DXT5nm A/G -> tangent normal"
                        : "R/G -> tangent normal");
                record.normal_channel = save_transformed_channel(normal_choice.texture,
                    dir / record.normal_channel.filename, preview_path("_Normal"),
                    std::format("{} '{}' -> runtime sampler fallback {} candidate {} -> {}",
                        normal_source->var_name, normal_source->source_name,
                        normal_choice.slot, normal_choice.candidate_index, decoder),
                    "BC5_UNORM", transform, record.roughness,
                    material_exporter::m_invert_normal_y || record.override_invert_normal_y, false);
                record.normal_channel.derived = true;
                record.normal_channel.provenance = channel_provenance_t::converted_map;
                record.normal_channel.confidence = use_ssbump ? 0.86f : 0.92f;
                fill_choice_metadata(record.normal_channel, normal_choice, normal_source,
                    use_ssbump ? "SSBump runtime sampler fallback" :
                        "Source normal runtime sampler fallback");
                used.insert(normal_choice.texture);
            }
            if (normal_source && !direct_vtf_decoded && !direct_vtf_status.empty())
                record.warnings.push_back("Direct VTF normal resolver: " + direct_vtf_status);
            if (!record.normal_channel.ready)
            {
                if (record.normal_channel.rejected)
                    record.warnings.push_back("Rejected Source normal candidate: " +
                        record.normal_channel.actual_status);
                record.normal_channel = save_generated_channel(
                    dir / record.normal_channel.filename, preview_path("_Normal"),
                    "generated full-resolution flat normal because no decodable Source bump texture was linked",
                    "BC5_UNORM", 128u, fallback_width, fallback_height, true);
            }
            record.normal_exported = record.normal_channel.ready;

            // WorldVertexTransition can carry an independent second normal in $bumpmap2.
            // It cannot be destructively merged without the BSP vertex blend weights, so
            // export it as a separate authoring asset and describe it in material.json.
            if (record.dual_layer)
            {
                if (const auto* layer1_albedo = find_exact_texture_param(record,
                    { "$basetexture2" }))
                {
                    record.albedo_layer1_channel = save_declared_vtf_channel(*layer1_albedo,
                        resolver_vtf_cache, dir / record.albedo_layer1_channel.filename,
                        preview_path("_Layer1_Albedo"), "BC7_UNORM",
                        pixel_transform_t::copy_color);
                    if (record.albedo_layer1_channel.ready)
                    {
                        record.albedo_layer1_channel.provenance = channel_provenance_t::native_map;
                        record.albedo_layer1_channel.source_kind =
                            "WorldVertexTransition layer 1 albedo direct VTF";
                    }
                }

                if (const auto* blend_modulate = find_exact_texture_param(record,
                    { "$blendmodulatetexture" }))
                {
                    record.blend_modulate_channel = save_declared_vtf_channel(*blend_modulate,
                        resolver_vtf_cache, dir / record.blend_modulate_channel.filename,
                        preview_path("_BlendModulate"), "BC4_UNORM",
                        pixel_transform_t::scalar_luminance);
                    if (record.blend_modulate_channel.ready)
                        record.blend_modulate_channel.source_kind =
                            "WorldVertexTransition blend-modulate direct VTF";
                }

                if (const auto* layer1_normal = find_exact_texture_param(record,
                    { "$bumpmap2", "$normalmap2" });
                    layer1_normal && layer1_normal != normal_source)
                {
                    std::filesystem::path declared_vtf;
                    std::string status;
                    if (acquire_declared_vtf(*layer1_normal, resolver_vtf_cache, declared_vtf, status))
                    {
                        vtf_decode_result_s decoded;
                        if (decode_vtf_top_mip(declared_vtf, decoded))
                        {
                            if (auto* direct_texture = create_runtime_texture_from_rgba(decoded.image))
                            {
                                const bool use_ssbump = source_declares_ssbump(record, *layer1_normal, decoded.flags);
                                const pixel_transform_t transform = use_ssbump
                                    ? pixel_transform_t::normal_ssbump
                                    : (decoded.format == IMAGE_FORMAT_DXT5
                                        ? pixel_transform_t::normal_dxt5nm
                                        : pixel_transform_t::normal_xy);
                                record.normal_layer1_channel = save_transformed_channel(direct_texture,
                                    dir / record.normal_layer1_channel.filename,
                                    preview_path("_Layer1_Normal"),
                                    std::format("{} '{}' -> direct VTF layer 1",
                                        layer1_normal->var_name, layer1_normal->source_name),
                                    "BC5_UNORM", transform, record.roughness,
                                    material_exporter::m_invert_normal_y ||
                                        record.override_invert_normal_y, false);
                                direct_texture->Release();
                                record.normal_layer1_channel.derived = true;
                                record.normal_layer1_channel.provenance =
                                    channel_provenance_t::converted_map;
                                record.normal_layer1_channel.confidence = use_ssbump ? 1.0f : 0.98f;
                                record.normal_layer1_channel.source_parameter = layer1_normal->var_name;
                                record.normal_layer1_channel.source_texture_name =
                                    layer1_normal->source_name;
                                record.normal_layer1_channel.source_kind = use_ssbump
                                    ? "WorldVertexTransition layer 1 direct VTF SSBump"
                                    : "WorldVertexTransition layer 1 direct VTF normal";
                                record.normal_layer1_channel.source_width = decoded.image.width;
                                record.normal_layer1_channel.source_height = decoded.image.height;
                                record.normal_layer1_channel.match_score = 1000;
                            }
                            else status += "; temporary D3D texture creation failed";
                        }
                        else status += "; " + decoded.status;
                    }
                    if (!record.normal_layer1_channel.ready)
                        record.warnings.push_back("Layer 1 normal resolver: " + status);
                }
            }
            record.albedo_layer1_exported = record.albedo_layer1_channel.ready;
            record.normal_layer1_exported = record.normal_layer1_channel.ready;
            record.blend_modulate_exported = record.blend_modulate_channel.ready;

            const std::string roughness_mode = lower_slashes(record.override_roughness_source);
            const bool roughness_force_constant = roughness_mode == "constant" ||
                roughness_mode == "none";
            const bool roughness_force_albedo = roughness_mode == "albedo";
            const bool roughness_auto = roughness_mode.empty() || roughness_mode == "auto";

            if (!roughness_force_constant && !roughness_force_albedo &&
                (roughness_auto || roughness_mode == "$roughness" ||
                 roughness_mode == "$roughnessmap"))
            {
                if (const auto* native_roughness = find_exact_texture_param(record,
                    { "$roughness", "$roughnessmap" }))
                {
                    record.roughness_channel = save_declared_vtf_channel(*native_roughness,
                        resolver_vtf_cache, dir / record.roughness_channel.filename,
                        preview_path("_Roughness"), "BC4_UNORM",
                        pixel_transform_t::scalar_luminance, record.roughness);
                    if (record.roughness_channel.ready)
                    {
                        record.roughness_channel.provenance = channel_provenance_t::native_map;
                        record.roughness_channel.source_kind = "native Source roughness direct VTF";
                    }
                    else
                    {
                        const auto choice = choose_bound_texture(record, *native_roughness,
                            texture_semantic_t::scalar_mask, used, false);
                        if (choice)
                        {
                            record.roughness_channel = save_transformed_channel(choice.texture,
                                dir / record.roughness_channel.filename, preview_path("_Roughness"),
                                std::format("{} '{}' -> sampler {} -> luminance",
                                    native_roughness->var_name, native_roughness->source_name, choice.slot),
                                "BC4_UNORM", pixel_transform_t::scalar_luminance);
                            record.roughness_channel.derived = true;
                            fill_choice_metadata(record.roughness_channel, choice, native_roughness,
                                "native Source roughness texture runtime fallback");
                            used.insert(choice.texture);
                        }
                    }
                }
            }

            if (!record.roughness_channel.ready && !roughness_force_constant &&
                !roughness_force_albedo &&
                (roughness_auto || roughness_mode == "$phongexponenttexture"))
            {
                const auto* exponent = find_exact_texture_param(record, { "$phongexponenttexture" });
                if (!exponent) exponent = find_texture_param(record, { "phongexponent" });
                if (exponent)
                {
                    record.roughness_channel = save_declared_vtf_channel(*exponent,
                        resolver_vtf_cache, dir / record.roughness_channel.filename,
                        preview_path("_Roughness"), "BC4_UNORM",
                        pixel_transform_t::phong_exponent_to_roughness, record.roughness);
                    if (record.roughness_channel.ready)
                    {
                        record.roughness_channel.source_kind =
                            "Phong exponent direct VTF -> GGX roughness";
                    }
                    else
                    {
                        const auto choice = choose_bound_texture(record, *exponent,
                            texture_semantic_t::scalar_mask, used, false);
                        if (choice)
                        {
                            record.roughness_channel = save_transformed_channel(choice.texture,
                                dir / record.roughness_channel.filename, preview_path("_Roughness"),
                                std::format("{} '{}' -> sampler {} -> Phong exponent to GGX roughness",
                                    exponent->var_name, exponent->source_name, choice.slot),
                                "BC4_UNORM", pixel_transform_t::phong_exponent_to_roughness,
                                record.roughness, false, false);
                            record.roughness_channel.derived = true;
                            fill_choice_metadata(record.roughness_channel, choice, exponent,
                                "Phong exponent texture runtime fallback");
                            used.insert(choice.texture);
                        }
                    }
                }
            }

            if (!record.roughness_channel.ready && !roughness_force_constant &&
                !roughness_force_albedo &&
                (roughness_auto || roughness_mode == "$envmapmask"))
            {
                const auto* env_mask = find_exact_texture_param(record, { "$envmapmask" });
                const bool explicit_envmapmask_override = roughness_mode == "$envmapmask";
                const bool envmapmask_semantically_allowed =
                    !record.specular_semantics.contains("rejected") ||
                    material_exporter::m_allow_envmapmask_conflict_fallback ||
                    explicit_envmapmask_override;
                if (env_mask && envmapmask_semantically_allowed)
                {
                    record.roughness_channel = save_declared_vtf_channel(*env_mask,
                        resolver_vtf_cache, dir / record.roughness_channel.filename,
                        preview_path("_Roughness"), "BC4_UNORM",
                        pixel_transform_t::specular_mask_to_roughness, record.roughness);
                    if (record.roughness_channel.ready)
                    {
                        record.roughness_channel.source_kind =
                            "Source envmap mask direct VTF -> inverse roughness";
                    }
                    else
                    {
                        const auto choice = choose_bound_texture(record, *env_mask,
                            texture_semantic_t::scalar_mask, used, false);
                        if (choice)
                        {
                            record.roughness_channel = save_transformed_channel(choice.texture,
                                dir / record.roughness_channel.filename, preview_path("_Roughness"),
                                std::format("$envmapmask '{}' -> sampler {} -> inverse specular roughness",
                                    env_mask->source_name, choice.slot),
                                "BC4_UNORM", pixel_transform_t::specular_mask_to_roughness,
                                record.roughness, false, false);
                            record.roughness_channel.derived = true;
                            fill_choice_metadata(record.roughness_channel, choice, env_mask,
                                "Source envmap mask runtime fallback");
                            used.insert(choice.texture);
                        }
                    }
                }
            }

            if (!record.roughness_channel.ready && roughness_auto &&
                find_exact_texture_param(record, { "$envmapmask" }) &&
                record.specular_semantics.contains("rejected") &&
                !material_exporter::m_allow_envmapmask_conflict_fallback)
            {
                append_unique(record.warnings,
                    "$envmapmask was not converted to roughness because Source shader semantics reject it with the active bump/lightwarp path");
            }

            if (!record.roughness_channel.ready && roughness_auto && albedo_source &&
                (record.base_alpha_phong_mask || record.base_alpha_envmap_mask ||
                 record.specular_semantics.contains("automatic L4D2 env")))
            {
                record.roughness_channel = save_declared_vtf_channel(*albedo_source,
                    resolver_vtf_cache, dir / record.roughness_channel.filename,
                    preview_path("_Roughness"), "BC4_UNORM",
                    pixel_transform_t::alpha_specular_mask_to_roughness,
                    record.roughness, false,
                    (record.base_alpha_phong_mask && record.invert_phong_mask) ||
                    record.base_alpha_envmap_mask);
                if (record.roughness_channel.ready)
                {
                    record.roughness_channel.source_kind = record.base_alpha_phong_mask
                        ? "$basetexture alpha direct VTF -> Phong roughness"
                        : (record.base_alpha_envmap_mask
                            ? "$basetexture alpha direct VTF -> explicit inverted L4D2 envmap roughness"
                            : "$basetexture alpha direct VTF -> automatic L4D2 envmap roughness");
                    record.roughness_channel.confidence = 0.96f;
                }
            }
            if (!record.roughness_channel.ready && roughness_auto && albedo_choice &&
                (record.base_alpha_phong_mask || record.base_alpha_envmap_mask ||
                 record.specular_semantics.contains("automatic L4D2 env")))
            {
                record.roughness_channel = save_transformed_channel(albedo_choice.texture,
                    dir / record.roughness_channel.filename, preview_path("_Roughness"),
                    std::format("$basetexture alpha sampler {} -> {} mask -> roughness",
                        albedo_choice.slot,
                        record.base_alpha_phong_mask ? "Phong" : "envmap"),
                    "BC4_UNORM", pixel_transform_t::alpha_specular_mask_to_roughness,
                    record.roughness, false,
                    (record.base_alpha_phong_mask && record.invert_phong_mask) ||
                    record.base_alpha_envmap_mask);
                record.roughness_channel.derived = true;
                fill_choice_metadata(record.roughness_channel, albedo_choice, albedo_source,
                    "base alpha specular mask runtime fallback");
            }

            if (!record.roughness_channel.ready && roughness_auto && albedo_source &&
                record.phong_enabled && record.base_luminance_phong_mask)
            {
                record.roughness_channel = save_declared_vtf_channel(*albedo_source,
                    resolver_vtf_cache, dir / record.roughness_channel.filename,
                    preview_path("_Roughness"), "BC4_UNORM",
                    pixel_transform_t::specular_mask_to_roughness,
                    record.roughness, false, record.invert_phong_mask);
                if (record.roughness_channel.ready)
                {
                    record.roughness_channel.source_kind =
                        "$basetexture luminance direct VTF -> Phong roughness";
                    record.roughness_channel.provenance = channel_provenance_t::inferred_map;
                    record.roughness_channel.confidence = 0.78f;
                }
            }
            if (!record.roughness_channel.ready && roughness_auto && albedo_choice &&
                record.phong_enabled && record.base_luminance_phong_mask)
            {
                record.roughness_channel = save_transformed_channel(albedo_choice.texture,
                    dir / record.roughness_channel.filename, preview_path("_Roughness"),
                    std::format("$basetexture sampler {} luminance -> Phong mask -> roughness",
                        albedo_choice.slot),
                    "BC4_UNORM", pixel_transform_t::specular_mask_to_roughness,
                    record.roughness, false, record.invert_phong_mask);
                record.roughness_channel.derived = true;
                fill_choice_metadata(record.roughness_channel, albedo_choice, albedo_source,
                    "base luminance Phong mask runtime fallback");
                record.roughness_channel.provenance = channel_provenance_t::inferred_map;
                record.roughness_channel.confidence = 0.72f;
            }

            if (!record.roughness_channel.ready && roughness_auto && normal_source &&
                record.phong_enabled && !record.base_alpha_phong_mask &&
                !record.base_luminance_phong_mask)
            {
                record.roughness_channel = save_declared_vtf_channel(*normal_source,
                    resolver_vtf_cache, dir / record.roughness_channel.filename,
                    preview_path("_Roughness"), "BC4_UNORM",
                    pixel_transform_t::alpha_specular_mask_to_roughness,
                    record.roughness, false, record.invert_phong_mask);
                if (record.roughness_channel.ready)
                {
                    record.roughness_channel.source_kind =
                        "$bumpmap alpha direct VTF -> default Phong roughness";
                    record.roughness_channel.provenance = channel_provenance_t::converted_map;
                    record.roughness_channel.confidence = 0.94f;
                }
            }
            if (!record.roughness_channel.ready && roughness_auto && normal_choice &&
                record.phong_enabled && !record.base_alpha_phong_mask &&
                !record.base_luminance_phong_mask)
            {
                record.roughness_channel = save_transformed_channel(normal_choice.texture,
                    dir / record.roughness_channel.filename, preview_path("_Roughness"),
                    std::format("$bumpmap alpha sampler {} -> default Phong mask -> roughness",
                        normal_choice.slot),
                    "BC4_UNORM", pixel_transform_t::alpha_specular_mask_to_roughness,
                    record.roughness, false, record.invert_phong_mask);
                record.roughness_channel.derived = true;
                fill_choice_metadata(record.roughness_channel, normal_choice, normal_source,
                    "default bump alpha Phong mask runtime fallback");
                record.roughness_channel.provenance = channel_provenance_t::inferred_map;
                record.roughness_channel.confidence = 0.80f;
            }

            if (!record.roughness_channel.ready && roughness_auto && normal_source &&
                record.normal_alpha_envmap_mask)
            {
                record.roughness_channel = save_declared_vtf_channel(*normal_source,
                    resolver_vtf_cache, dir / record.roughness_channel.filename,
                    preview_path("_Roughness"), "BC4_UNORM",
                    pixel_transform_t::alpha_specular_mask_to_roughness,
                    record.roughness, false, false);
                if (record.roughness_channel.ready)
                {
                    record.roughness_channel.source_kind =
                        "$bumpmap alpha direct VTF -> envmap roughness";
                    record.roughness_channel.confidence = 0.96f;
                }
            }
            if (!record.roughness_channel.ready && roughness_auto && normal_choice &&
                record.normal_alpha_envmap_mask)
            {
                record.roughness_channel = save_transformed_channel(normal_choice.texture,
                    dir / record.roughness_channel.filename, preview_path("_Roughness"),
                    std::format("$bumpmap alpha sampler {} -> envmap mask -> roughness",
                        normal_choice.slot),
                    "BC4_UNORM", pixel_transform_t::alpha_specular_mask_to_roughness,
                    record.roughness, false, false);
                record.roughness_channel.derived = true;
                fill_choice_metadata(record.roughness_channel, normal_choice, normal_source,
                    "normal alpha envmap mask runtime fallback");
            }

            if (!record.roughness_channel.ready && !roughness_force_constant &&
                (roughness_auto || roughness_force_albedo) && albedo_choice &&
                material_exporter::m_generate_roughness_from_albedo && !record.water_material)
            {
                record.roughness_channel = save_transformed_channel(albedo_choice.texture,
                    dir / record.roughness_channel.filename, preview_path("_Roughness"),
                    std::format("$basetexture sampler {} -> grayscale/local-detail inferred roughness",
                        albedo_choice.slot),
                    "BC4_UNORM", pixel_transform_t::albedo_luminance_to_roughness,
                    record.roughness, false, false);
                record.roughness_channel.derived = true;
                fill_choice_metadata(record.roughness_channel, albedo_choice, albedo_source,
                    record.envmap_enabled
                        ? "albedo-derived roughness with envmap-informed base"
                        : "albedo-derived diagnostic roughness");
                record.roughness_channel.provenance = channel_provenance_t::inferred_map;
                record.roughness_channel.confidence = 0.42f;
            }

            if (!record.roughness_channel.ready)
            {
                const auto roughness_value = to_byte(record.roughness);
                record.roughness_channel = save_generated_channel(
                    dir / record.roughness_channel.filename, preview_path("_Roughness"),
                    std::format("generated full-resolution roughness from VMT/Phong/envmap inference ({:.3f})",
                        record.roughness),
                    "BC4_UNORM", roughness_value, fallback_width, fallback_height);
            }
            record.roughness_exported = record.roughness_channel.ready;

            const texture_param_s* metallic_source = find_exact_texture_param(record,
                { "$metalness", "$metallic", "$metalnessmap", "$metallicmap" });
            if (!metallic_source) metallic_source = find_texture_param(record,
                { "metalness", "metallic" });
            if (metallic_source)
            {
                record.metallic_channel = save_declared_vtf_channel(*metallic_source,
                    resolver_vtf_cache, dir / record.metallic_channel.filename,
                    preview_path("_Metalness"), "BC4_UNORM",
                    pixel_transform_t::scalar_luminance);
                if (record.metallic_channel.ready)
                {
                    record.metallic_channel.provenance = channel_provenance_t::native_map;
                    record.metallic_channel.source_kind = "native Source metalness direct VTF";
                }
                else
                {
                    const auto choice = choose_bound_texture(record, *metallic_source,
                        texture_semantic_t::scalar_mask, used, false);
                    if (choice)
                    {
                        record.metallic_channel = save_transformed_channel(choice.texture,
                            dir / record.metallic_channel.filename, preview_path("_Metalness"),
                            std::format("{} '{}' -> sampler {} -> luminance",
                                metallic_source->var_name, metallic_source->source_name, choice.slot),
                            "BC4_UNORM", pixel_transform_t::scalar_luminance);
                        record.metallic_channel.derived = true;
                        fill_choice_metadata(record.metallic_channel, choice, metallic_source,
                            "native Source metalness texture runtime fallback");
                        used.insert(choice.texture);
                    }
                }
            }
            if (!record.metallic_channel.ready &&
                (record.metallic > 0.01f || material_exporter::m_generate_mask_textures))
            {
                const auto metallic_value = to_byte(record.metallic);
                record.metallic_channel = save_generated_channel(
                    dir / record.metallic_channel.filename, preview_path("_Metalness"),
                    std::format("generated full-resolution metalness from $surfaceprop/category/envmap inference ({:.3f})",
                        record.metallic),
                    "BC4_UNORM", metallic_value, fallback_width, fallback_height);
            }
            record.metallic_exported = record.metallic_channel.ready;

            const std::string height_mode = lower_slashes(record.override_height_source);
            const bool height_disabled = record.override_disable_height || height_mode == "none";
            const bool height_auto = height_mode.empty() || height_mode == "auto";
            const texture_param_s* height_source = nullptr;
            if (!height_disabled)
            {
                height_source = find_texture_param_by_name(record, height_mode);
                if (!height_source && height_auto)
                    height_source = find_exact_texture_param(record,
                        { "$heightmap", "$displacementmap", "$parallaxmap" });
                if (!height_source && height_auto)
                    height_source = find_texture_param(record,
                        { "height", "displacement", "parallax" });
            }
            if (height_source)
            {
                record.height_channel = save_declared_vtf_channel(*height_source,
                    resolver_vtf_cache, dir / record.height_channel.filename,
                    preview_path("_Height"), "BC4_UNORM",
                    pixel_transform_t::scalar_luminance);
                if (record.height_channel.ready)
                {
                    record.height_channel.source_kind =
                        "explicit Source height/displacement direct VTF";
                }
                else if (texture_parameter_has_runtime_source(*height_source))
                {
                    const auto choice = choose_bound_texture(record, *height_source,
                        texture_semantic_t::height, used, false);
                    if (choice)
                    {
                        record.height_channel = save_transformed_channel(choice.texture,
                            dir / record.height_channel.filename, preview_path("_Height"),
                            std::format("{} '{}' -> sampler {} -> height luminance",
                                height_source->var_name, height_source->source_name, choice.slot),
                            "BC4_UNORM", pixel_transform_t::scalar_luminance);
                        record.height_channel.derived = true;
                        fill_choice_metadata(record.height_channel, choice, height_source,
                            "explicit Source height/displacement runtime fallback");
                        used.insert(choice.texture);
                    }
                }
            }
            if (!height_disabled && !record.height_channel.ready &&
                (height_auto || height_mode == "bump_alpha") &&
                record.parallax_enabled && normal_choice &&
                !record.normal_alpha_envmap_mask)
            {
                record.height_channel = save_transformed_channel(normal_choice.texture,
                    dir / record.height_channel.filename, preview_path("_Height"),
                    std::format("$bumpmap alpha sampler {} -> parallax height", normal_choice.slot),
                    "BC4_UNORM", pixel_transform_t::scalar_alpha);
                record.height_channel.derived = true;
                fill_choice_metadata(record.height_channel, normal_choice, normal_source,
                    "normal alpha parallax height");
            }
            if (!height_disabled && !record.height_channel.ready &&
                (height_auto || height_mode == "ssbump") &&
                (record.ssbump || record.override_force_ssbump) && normal_choice &&
                material_exporter::m_generate_ssbump_height && !record.water_material)
            {
                record.height_channel = save_transformed_channel(normal_choice.texture,
                    dir / record.height_channel.filename, preview_path("_Height"),
                    std::format("$ssbump sampler {} -> diagnostic pseudo-height", normal_choice.slot),
                    "BC4_UNORM", pixel_transform_t::ssbump_energy_to_height);
                record.height_channel.derived = true;
                fill_choice_metadata(record.height_channel, normal_choice, normal_source,
                    "SSBump directional-energy pseudo-height (not original geometric height)");
                record.height_channel.provenance = channel_provenance_t::inferred_map;
                record.height_channel.confidence = 0.25f;
            }
            if (height_disabled)
            {
                record.height_channel.source_parameter = "disabled by material override";
                record.height_channel.actual_status = "height export disabled";
            }
            record.height_exported = record.height_channel.ready;

            const texture_param_s* emissive_source = find_exact_texture_param(record,
                { "$emissiveblendbasetexture", "$emissiveblendtexture", "$selfillummask",
                  "$emissivemask", "$emissive" });
            if (!emissive_source) emissive_source = find_texture_param(record,
                { "selfillum", "emissive" });
            if (!record.override_disable_emission && emissive_source && !record.emissive)
            {
                record.emissive_channel.source_parameter =
                    emissive_source->var_name + " '" + emissive_source->source_name + "'";
                record.emissive_channel.actual_status =
                    "emissive texture declared but Source self-illumination/emissive blend is inactive";
            }
            if (!record.override_disable_emission && record.emissive && emissive_source)
            {
                record.emissive_channel = save_declared_vtf_channel(*emissive_source,
                    resolver_vtf_cache, dir / record.emissive_channel.filename,
                    preview_path("_Emission"), "BC7_UNORM",
                    pixel_transform_t::emission_color);
                if (record.emissive_channel.ready)
                {
                    record.emissive_channel.source_kind = "explicit Source emissive direct VTF";
                }
                else if (texture_parameter_has_runtime_source(*emissive_source))
                {
                    const auto choice = choose_bound_texture(record, *emissive_source,
                        texture_semantic_t::emission, used, false);
                    if (choice)
                    {
                        record.emissive_channel = save_transformed_channel(choice.texture,
                            dir / record.emissive_channel.filename, preview_path("_Emission"),
                            std::format("{} '{}' -> sampler {} -> emission color",
                                emissive_source->var_name, emissive_source->source_name, choice.slot),
                            "BC7_UNORM", pixel_transform_t::emission_color);
                        record.emissive_channel.derived = true;
                        fill_choice_metadata(record.emissive_channel, choice, emissive_source,
                            "explicit Source emissive texture runtime fallback");
                        used.insert(choice.texture);
                    }
                }
            }
            if (!record.override_disable_emission &&
                !record.emissive_channel.ready && record.selfillum_envmapmask_alpha)
            {
                if (const auto* env_mask = find_exact_texture_param(record, { "$envmapmask" }))
                {
                    record.emissive_channel = save_declared_vtf_channel(*env_mask,
                        resolver_vtf_cache, dir / record.emissive_channel.filename,
                        preview_path("_Emission"), "BC7_UNORM",
                        pixel_transform_t::emission_from_alpha);
                    if (record.emissive_channel.ready)
                    {
                        record.emissive_channel.source_kind =
                            "$envmapmask alpha -> Source self illumination";
                        record.emissive_channel.confidence = 0.92f;
                    }
                }
            }

            if (!record.override_disable_emission &&
                !record.emissive_channel.ready && record.detail_texture &&
                record.detail_blend_mode == 5)
            {
                if (const auto* detail_glow = find_exact_texture_param(record, { "$detail" }))
                {
                    record.emissive_channel = save_declared_vtf_channel(*detail_glow,
                        resolver_vtf_cache, dir / record.emissive_channel.filename,
                        preview_path("_Emission"), "BC7_UNORM",
                        pixel_transform_t::emission_color);
                    if (record.emissive_channel.ready)
                    {
                        record.emissive_channel.source_kind =
                            "$detail blend mode 5 -> unlit additive Source glow";
                        record.emissive_channel.confidence = 0.88f;
                    }
                }
            }

            if (!record.override_disable_emission &&
                !record.emissive_channel.ready && record.unlit_fullbright && albedo_source)
            {
                record.emissive_channel = save_declared_vtf_channel(*albedo_source,
                    resolver_vtf_cache, dir / record.emissive_channel.filename,
                    preview_path("_Emission"), "BC7_UNORM",
                    pixel_transform_t::emission_color);
                if (record.emissive_channel.ready)
                {
                    record.emissive_channel.source_kind =
                        "UnlitGeneric base color direct VTF -> emissive appearance";
                    record.emissive_channel.confidence = 0.95f;
                }
            }

            if (!record.override_disable_emission &&
                !record.emissive_channel.ready && record.emissive && albedo_choice)
            {
                record.emissive_channel = save_transformed_channel(albedo_choice.texture,
                    dir / record.emissive_channel.filename, preview_path("_Emission"),
                    std::format("$selfillum -> $basetexture sampler {} RGB x alpha",
                        albedo_choice.slot),
                    "BC7_UNORM", pixel_transform_t::emission_from_alpha);
                record.emissive_channel.derived = true;
                fill_choice_metadata(record.emissive_channel, albedo_choice, albedo_source,
                    "base color alpha self-illumination");
                record.emissive_channel.provenance = channel_provenance_t::inferred_map;
                record.emissive_channel.confidence = 0.58f;
            }
            if (record.override_disable_emission)
            {
                record.emissive_channel.source_parameter = "disabled by material override";
                record.emissive_channel.actual_status = "emission export disabled";
            }
            record.emissive_exported = record.emissive_channel.ready;

            // Preserve Source detail as a separate authoring asset. Source detail blend
            // modes cannot be represented exactly by a single Remix texture, so V20.3.0
            // avoids destructive baking and records the blend metadata in material.json.
            if (material_exporter::m_export_detail_authoring_assets &&
                material_exporter::m_use_detail_as_pbr_microdetail)
            {
                if (const auto* detail_source = find_exact_texture_param(record, { "$detail" }))
                {
                    record.detail_channel = save_declared_vtf_channel(*detail_source,
                        resolver_vtf_cache, dir / record.detail_channel.filename,
                        preview_path("_Detail"), "BC7_UNORM", pixel_transform_t::copy_color);
                    if (record.detail_channel.ready)
                    {
                        record.detail_channel.provenance = channel_provenance_t::native_map;
                        record.detail_channel.source_kind = "Source detail authoring asset direct VTF";
                    }
                    else if (texture_parameter_has_runtime_source(*detail_source))
                    {
                        const auto choice = choose_bound_texture(record, *detail_source,
                            texture_semantic_t::albedo, used, true);
                        if (choice)
                        {
                            record.detail_channel = save_channel(choice.texture,
                                dir / record.detail_channel.filename, preview_path("_Detail"),
                                std::format("$detail '{}' -> runtime sampler {}",
                                    detail_source->source_name, choice.slot), "BC7_UNORM");
                            fill_choice_metadata(record.detail_channel, choice, detail_source,
                                "Source detail runtime fallback");
                            used.insert(choice.texture);
                        }
                    }
                }
            }
            record.detail_exported = record.detail_channel.ready;

            for (const auto& conflict : record.semantic_conflicts)
                append_unique(record.warnings, "Source semantic conflict: " + conflict);
            if (record.animated_or_proxy_driven)
                append_unique(record.warnings,
                    "Dynamic Source material exported for review; a static RTX material cannot preserve all frame/proxy behavior");

            // Cutout/translucency is semantically different from emission. Export a
            // dedicated BC4 opacity mask from an explicit mask or from base-texture alpha.
            if (material_exporter::m_export_opacity_masks &&
                (record.alpha_tested || record.translucent || record.additive))
            {
                const auto* opacity_source = find_exact_texture_param(record,
                    { "$opacity", "$opacitytexture", "$translucencymask", "$alphamask" });
                if (opacity_source)
                {
                    record.opacity_channel = save_declared_vtf_channel(*opacity_source,
                        resolver_vtf_cache, dir / record.opacity_channel.filename,
                        preview_path("_Opacity"), "BC4_UNORM",
                        pixel_transform_t::scalar_luminance);
                    if (record.opacity_channel.ready)
                        record.opacity_channel.source_kind = "explicit Source opacity direct VTF";
                }

                if (!record.opacity_channel.ready && albedo_source)
                {
                    record.opacity_channel = save_declared_vtf_channel(*albedo_source,
                        resolver_vtf_cache, dir / record.opacity_channel.filename,
                        preview_path("_Opacity"), "BC4_UNORM", pixel_transform_t::scalar_alpha);
                    if (record.opacity_channel.ready)
                    {
                        record.opacity_channel.source_kind = "base texture alpha direct VTF opacity";
                        record.opacity_channel.confidence = 0.96f;
                    }
                }
                if (!record.opacity_channel.ready && albedo_choice)
                {
                    record.opacity_channel = save_transformed_channel(albedo_choice.texture,
                        dir / record.opacity_channel.filename, preview_path("_Opacity"),
                        std::format("$basetexture sampler {} alpha -> opacity", albedo_choice.slot),
                        "BC4_UNORM", pixel_transform_t::scalar_alpha);
                    fill_choice_metadata(record.opacity_channel, albedo_choice, albedo_source,
                        "base texture alpha runtime opacity fallback");
                    record.opacity_channel.confidence = 0.82f;
                }
            }
            record.opacity_exported = record.opacity_channel.ready;

            refresh_record_conversion_state(record);
            evaluate_material_activation(record);
            record.exported = has_complete_pbr(record);
            record.conversion_pending = !record.exported || record.quarantined;
            record.conversion_status = record.auto_activated
                ? "V21.8 Source-semantic PBR material is active in Materials_Auto.usda"
                : (record.exported
                    ? "resolved assets exported but material is quarantined for review"
                    : "incomplete: albedo, normal and roughness DDS are required");

            if (record.override_applied)
                record.warnings.push_back("Manual material_overrides.toml rules were applied.");
            if (record.dual_layer)
                record.warnings.push_back(
                    "Dual-layer/WorldVertexTransition material detected. Layer references are reported, but vertex blend weights cannot be baked from a single runtime draw.");
            if (record.detail_texture)
                record.warnings.push_back(
                    record.detail_exported
                        ? "Detail texture exported as a separate authoring asset; Source blend metadata is preserved without destructive baking."
                        : "Detail texture detected but could not be exported; it was not destructively baked into albedo.");
            if (record.quarantined)
                record.warnings.push_back("Material exported to disk but excluded from automatic USD activation: " +
                    record.activation_reason);
            if (!record.source_vmt_resolved)
                record.warnings.push_back("Original Source VMT was not recovered from loose files or mounted VPKs; runtime IMaterial data is a reconstruction only.");
            if (record.missing_texture_asset_count > 0u)
                record.warnings.push_back(std::format("{} declared Source VTF asset(s) could not be recovered; see provenance_graph.json for exact paths.",
                    record.missing_texture_asset_count));
            if (!record.asset_backed_material && record.live_texture_param_count > 0u)
                record.warnings.push_back("Live ITexture/sampler data was retained as a labelled fallback and is not considered proof of the original game asset.");
            if (!record.ignored_unresolved_texture_params.empty())
            {
                std::ostringstream ignored;
                ignored << "Ignored unresolved Source texture placeholders:";
                const std::size_t limit = std::min<std::size_t>(
                    record.ignored_unresolved_texture_params.size(), 8u);
                for (std::size_t i = 0; i < limit; ++i)
                    ignored << (i == 0 ? " " : ", ")
                        << record.ignored_unresolved_texture_params[i];
                if (record.ignored_unresolved_texture_params.size() > limit)
                    ignored << " and "
                        << (record.ignored_unresolved_texture_params.size() - limit)
                        << " more";
                ignored << ". No sampler was assigned to these channels.";
                record.warnings.push_back(ignored.str());
            }
            if (record.dynamic_material)
                record.warnings.push_back(
                    "Dynamic Source shader inputs detected; exported PBR is a static Remix approximation.");

            append_channel_warnings(record, "Albedo", record.albedo_channel);
            append_channel_warnings(record, "Normal", record.normal_channel);
            if (record.normal_layer1_channel.derived || record.normal_layer1_channel.generated)
                append_channel_warnings(record, "Layer 1 normal", record.normal_layer1_channel);
            append_channel_warnings(record, "Roughness", record.roughness_channel);
            if (record.metallic > 0.01f || material_exporter::m_generate_mask_textures)
                append_channel_warnings(record, "Metalness", record.metallic_channel);
            if (!height_disabled && (height_source || record.parallax_enabled || record.ssbump ||
                record.override_height_source != "auto"))
                append_channel_warnings(record, "Height", record.height_channel);
            if (!record.override_disable_emission && (record.emissive || emissive_source))
                append_channel_warnings(record, "Emission", record.emissive_channel);
            if (record.alpha_tested || record.translucent || record.additive)
                append_channel_warnings(record, "Opacity", record.opacity_channel);
            if (record.detail_texture && material_exporter::m_export_detail_authoring_assets)
                append_channel_warnings(record, "Detail", record.detail_channel);

            std::sort(record.warnings.begin(), record.warnings.end());
            record.warnings.erase(std::unique(record.warnings.begin(), record.warnings.end()),
                record.warnings.end());

            const auto write_channel_json = [&](std::ostream& json, const char* name,
                const channel_export_s& channel, const bool trailing_comma)
            {
                json << "    \"" << name << "\": {\n";
                json << "      \"file\": \"" << json_escape(channel.filename) << "\",\n";
                json << "      \"preview_png\": \"" << json_escape(channel.preview_filename) << "\",\n";
                json << "      \"source\": \"" << json_escape(channel.source_parameter) << "\",\n";
                json << "      \"declared_texture\": \"" << json_escape(channel.source_texture_name) << "\",\n";
                json << "      \"source_kind\": \"" << json_escape(channel.source_kind) << "\",\n";
                json << "      \"provenance\": \"" << provenance_name(channel.provenance) << "\",\n";
                json << "      \"confidence\": " << channel.confidence << ",\n";
                json << "      \"quality\": {\n";
                json << "        \"status\": \"" << json_escape(channel.quality.status) << "\",\n";
                json << "        \"valid\": " << (channel.quality.valid ? "true" : "false") << ",\n";
                json << "        \"constant\": " << (channel.quality.constant ? "true" : "false") << ",\n";
                json << "        \"suspicious\": " << (channel.quality.suspicious ? "true" : "false") << ",\n";
                json << "        \"minimum\": " << channel.quality.minimum << ",\n";
                json << "        \"maximum\": " << channel.quality.maximum << ",\n";
                json << "        \"mean\": " << channel.quality.mean << ",\n";
                json << "        \"standard_deviation\": " << channel.quality.standard_deviation << ",\n";
                json << "        \"unique_values\": " << channel.quality.unique_values << ",\n";
                json << "        \"normal_xy_valid_fraction\": " << channel.quality.normal_mean_length << "\n";
                json << "      },\n";
                json << "      \"sampler_slot\": " << channel.sampler_slot << ",\n";
                json << "      \"candidate_index\": " << channel.sampler_candidate << ",\n";
                json << "      \"match_score\": " << channel.match_score << ",\n";
                json << "      \"hits\": " << channel.source_hits << ",\n";
                json << "      \"source_size\": [" << channel.source_width << ", "
                    << channel.source_height << "],\n";
                json << "      \"ready\": " << (channel.ready ? "true" : "false") << ",\n";
                json << "      \"generated\": " << (channel.generated ? "true" : "false") << ",\n";
                json << "      \"derived\": " << (channel.derived ? "true" : "false") << ",\n";
                json << "      \"rejected\": " << (channel.rejected ? "true" : "false") << ",\n";
                json << "      \"status\": \"" << json_escape(channel.actual_status) << "\"\n";
                json << "    }" << (trailing_comma ? "," : "") << "\n";
            };

            {
                std::ofstream json(dir / "material.json", std::ios::trunc);
                json << "{\n";
                json << "  \"schema\": \"LegacyMaterialsV21.8\",\n";
                json << "  \"source_vmt\": \"" << json_escape(record.resolved_vmt_path.empty()
                    ? "materials/" + record.canonical_name + ".vmt" : record.resolved_vmt_path) << "\",\n";
                json << "  \"shader\": \"" << json_escape(record.shader_name) << "\",\n";
                json << "  \"remix_hash\": \"" << hash_hex(record.remix_hash) << "\",\n";
                json << "  \"category\": \"" << json_escape(record.category) << "\",\n";
                json << "  \"shader_profile\": \"" << json_escape(record.shader_profile) << "\",\n";
                json << "  \"identity_key\": \"" << json_escape(record.identity_key) << "\",\n";
                json << "  \"surfaceprop\": \"" << json_escape(record.surface_prop) << "\",\n";
                json << "  \"source_asset_integrity\": {\n";
                json << "    \"grade\": \"" << json_escape(record.source_asset_grade) << "\",\n";
                json << "    \"asset_backed\": " << (record.asset_backed_material ? "true" : "false") << ",\n";
                json << "    \"vmt_resolved\": " << (record.source_vmt_resolved ? "true" : "false") << ",\n";
                json << "    \"vmt_from_vpk\": " << (record.source_vmt_from_vpk ? "true" : "false") << ",\n";
                json << "    \"vmt_status\": \"" << json_escape(record.source_vmt_status) << "\",\n";
                json << "    \"vmt_content_hash\": \"" << (record.source_vmt_resolved ? hash_hex(record.source_vmt_hash) : std::string{}) << "\",\n";
                json << "    \"vmt_byte_size\": " << record.source_vmt_byte_size << ",\n";
                json << "    \"base_albedo_asset_resolved\": " << (record.source_albedo_asset_resolved ? "true" : "false") << ",\n";
                json << "    \"declared_vtf_count\": " << record.declared_texture_count << ",\n";
                json << "    \"resolved_vtf_count\": " << record.resolved_texture_asset_count << ",\n";
                json << "    \"missing_vtf_count\": " << record.missing_texture_asset_count << ",\n";
                json << "    \"live_texture_param_count\": " << record.live_texture_param_count << "\n";
                json << "  },\n";
                json << "  \"resolved_required_pbr\": " << (record.exported ? "true" : "false") << ",\n";
                json << "  \"material_grade\": \"" << material_grade_name(record.material_grade) << "\",\n";
                json << "  \"auto_activated\": " << (record.auto_activated ? "true" : "false") << ",\n";
                json << "  \"quarantined\": " << (record.quarantined ? "true" : "false") << ",\n";
                json << "  \"activation_reason\": \"" << json_escape(record.activation_reason) << "\",\n";
                json << "  \"override_applied\": " << (record.override_applied ? "true" : "false") << ",\n";
                json << "  \"features\": {\n";
                json << "    \"dual_layer\": " << (record.dual_layer ? "true" : "false") << ",\n";
                json << "    \"detail_texture\": " << (record.detail_texture ? "true" : "false") << ",\n";
                json << "    \"glass_material\": " << (record.glass_material ? "true" : "false") << ",\n";
                json << "    \"dynamic_material\": " << (record.dynamic_material ? "true" : "false") << ",\n";
                json << "    \"opacity_exported\": " << (record.opacity_exported ? "true" : "false") << ",\n";
                json << "    \"detail_exported\": " << (record.detail_exported ? "true" : "false") << "\n";
                json << "  },\n";
                if (material_exporter::m_export_semantic_manifest)
                {
                    json << "  \"source_semantics\": {\n";
                    json << "    \"source_shader\": \"" << json_escape(record.source_shader_name.empty() ? record.shader_name : record.source_shader_name) << "\",\n";
                    json << "    \"shader_family\": \"" << json_escape(record.source_shader_family) << "\",\n";
                    json << "    \"resolved_vmt\": \"" << json_escape(record.resolved_vmt_path) << "\",\n";
                    json << "    \"resolved_via_model_search_path\": " << (record.resolved_via_model_search_path ? "true" : "false") << ",\n";
                    json << "    \"model_candidate_count\": " << record.model_candidate_count << ",\n";
                    json << "    \"material_graph\": \"" << json_escape(record.material_graph_summary) << "\",\n";
                    json << "    \"patch_semantics\": \"" << json_escape(record.patch_semantics) << "\",\n";
                    json << "    \"patch_material\": " << (record.patch_material ? "true" : "false") << ",\n";
                    json << "    \"patch_include_resolved\": " << (record.patch_include_resolved ? "true" : "false") << ",\n";
                    json << "    \"detail_semantics\": \"" << json_escape(record.detail_semantics) << "\",\n";
                    json << "    \"layer_semantics\": \"" << json_escape(record.layer_semantics) << "\",\n";
                    json << "    \"event_emissive_semantics\": \"" << json_escape(record.event_emissive_semantics) << "\",\n";
                    json << "    \"event_emissive_candidate\": " << (record.event_emissive_candidate ? "true" : "false") << ",\n";
                    json << "    \"calibrated_specular\": {\"workflow\": \"" << json_escape(record.calibrated_specular_workflow)
                        << "\", \"roughness\": " << record.calibrated_roughness
                        << ", \"reflection_weight\": " << record.calibrated_reflection_weight
                        << ", \"dielectric_f0\": " << record.calibrated_dielectric_f0
                        << ", \"metallic_hint\": " << record.calibrated_metallic_hint << "},\n";
                    json << "    \"include_chain\": [";
                    for (std::size_t i = 0; i < record.vmt_include_chain.size(); ++i)
                    {
                        if (i) json << ", ";
                        json << "\"" << json_escape(record.vmt_include_chain[i]) << "\"";
                    }
                    json << "],\n";
                    json << "    \"specular\": \"" << json_escape(record.specular_semantics) << "\",\n";
                    json << "    \"emission\": \"" << json_escape(record.emission_semantics) << "\",\n";
                    json << "    \"alpha\": \"" << json_escape(record.alpha_semantics) << "\",\n";
                    json << "    \"animation\": \"" << json_escape(record.animation_semantics) << "\",\n";
                    json << "    \"vtf_metadata\": {\"animated\": " << record.vtf_animated_count
                        << ", \"alpha\": " << record.vtf_alpha_count
                        << ", \"normal\": " << record.vtf_normal_flag_count
                        << ", \"ssbump\": " << record.vtf_ssbump_flag_count
                        << ", \"srgb\": " << record.vtf_srgb_count
                        << ", \"cubemap\": " << record.vtf_cubemap_count
                        << ", \"unsupported\": " << record.vtf_unsupported_count << "},\n";
                    json << "    \"proxies\": [";
                    for (std::size_t i = 0; i < record.detected_proxies.size(); ++i)
                    {
                        if (i) json << ", ";
                        json << "\"" << json_escape(record.detected_proxies[i]) << "\"";
                    }
                    json << "],\n";
                    json << "    \"conflicts\": [";
                    for (std::size_t i = 0; i < record.semantic_conflicts.size(); ++i)
                    {
                        if (i) json << ", ";
                        json << "\"" << json_escape(record.semantic_conflicts[i]) << "\"";
                    }
                    json << "]\n";
                    json << "  },\n";
                }
                json << "  \"flags\": {\n";
                json << "    \"ssbump\": " << (record.ssbump ? "true" : "false") << ",\n";
                json << "    \"envmap\": " << (record.envmap_enabled ? "true" : "false") << ",\n";
                json << "    \"water\": " << (record.water_material ? "true" : "false") << ",\n";
                json << "    \"base_alpha_phong_mask\": " << (record.base_alpha_phong_mask ? "true" : "false") << ",\n";
                json << "    \"base_alpha_envmap_mask\": " << (record.base_alpha_envmap_mask ? "true" : "false") << ",\n";
                json << "    \"normal_alpha_envmap_mask\": " << (record.normal_alpha_envmap_mask ? "true" : "false") << ",\n";
                json << "    \"alpha_tested\": " << (record.alpha_tested ? "true" : "false") << ",\n";
                json << "    \"translucent\": " << (record.translucent ? "true" : "false") << ",\n";
                json << "    \"additive\": " << (record.additive ? "true" : "false") << ",\n";
                json << "    \"nocull\": " << (record.nocull ? "true" : "false") << ",\n";
                json << "    \"alpha_test_reference\": " << record.alpha_test_reference << "\n";
                json << "  },\n";
                json << "  \"detail_blend\": {\n";
                json << "    \"mode\": " << record.detail_blend_mode << ",\n";
                json << "    \"factor\": " << record.detail_blend_factor << ",\n";
                json << "    \"scale\": " << record.detail_scale << "\n";
                json << "  },\n";
                json << "  \"inferred\": {\n";
                json << "    \"roughness\": " << record.roughness << ",\n";
                json << "    \"metalness\": " << record.metallic << ",\n";
                json << "    \"reflection_weight\": " << record.calibrated_reflection_weight << ",\n";
                json << "    \"dielectric_f0\": " << record.calibrated_dielectric_f0 << ",\n";
                json << "    \"confidence\": " << record.confidence << "\n";
                json << "  },\n";
                json << "  \"channels\": {\n";
                write_channel_json(json, "albedo", record.albedo_channel, true);
                write_channel_json(json, "albedo_layer1", record.albedo_layer1_channel, true);
                write_channel_json(json, "normal", record.normal_channel, true);
                write_channel_json(json, "normal_layer1", record.normal_layer1_channel, true);
                write_channel_json(json, "blend_modulate", record.blend_modulate_channel, true);
                write_channel_json(json, "roughness", record.roughness_channel, true);
                write_channel_json(json, "metalness", record.metallic_channel, true);
                write_channel_json(json, "height", record.height_channel, true);
                write_channel_json(json, "emission", record.emissive_channel, true);
                write_channel_json(json, "opacity", record.opacity_channel, true);
                write_channel_json(json, "detail", record.detail_channel, false);
                json << "  },\n";
                json << "  \"warnings\": [\n";
                for (std::size_t warning_index = 0u; warning_index < record.warnings.size(); ++warning_index)
                {
                    json << "    \"" << json_escape(record.warnings[warning_index]) << "\"";
                    if (warning_index + 1u != record.warnings.size()) json << ",";
                    json << "\n";
                }
                json << "  ],\n";
                json << "  \"conversion_status\": \"" << json_escape(record.conversion_status) << "\"\n";
                json << "}\n";
            }

            {
                std::ofstream graph(dir / "provenance_graph.json", std::ios::trunc);
                graph << "{\n";
                graph << "  \"schema\": \"LegacyMaterialsProvenanceGraphV21.8\",\n";
                graph << "  \"material\": \"materials/" << json_escape(record.canonical_name)
                    << ".vmt\",\n";
                graph << "  \"identity\": {\n";
                graph << "    \"source_vmt_hash\": \"" << hash_hex(record.remix_hash) << "\",\n";
                graph << "    \"identity_key\": \"" << json_escape(record.identity_key) << "\",\n";
                graph << "    \"runtime_hash_aliases\": [\"" << hash_hex(record.remix_hash)
                    << "\"]\n";
                graph << "  },\n";
                const std::string source_vmt_asset = record.resolved_vmt_path.empty()
                    ? "materials/" + record.canonical_name + ".vmt"
                    : record.resolved_vmt_path;
                std::vector<std::uint8_t> source_vmt_bytes;
                std::string source_vmt_status;
                const bool source_vmt_resolved = read_source_asset_bytes(
                    source_vmt_asset, source_vmt_bytes, source_vmt_status);
                graph << "  \"source_vmt\": {\"asset\": \""
                    << json_escape(source_vmt_asset) << "\", \"resolved\": "
                    << (source_vmt_resolved ? "true" : "false")
                    << ", \"resolution\": \"" << json_escape(source_vmt_status)
                    << "\", \"byte_size\": " << source_vmt_bytes.size()
                    << ", \"content_hash\": \""
                    << (source_vmt_resolved ? hash_hex(bytes_hash64(source_vmt_bytes)) : std::string{})
                    << "\"},\n";
                graph << "  \"declared_assets\": [\n";
                for (std::size_t texture_index = 0; texture_index < record.texture_params.size(); ++texture_index)
                {
                    const auto& texture = record.texture_params[texture_index];
                    const std::string relative_asset = "materials/" +
                        normalize_vtf_relative_path(texture.source_name).generic_string();
                    std::vector<std::uint8_t> bytes;
                    std::string status;
                    const bool resolved = read_source_asset_bytes(relative_asset, bytes, status);
                    graph << "    {\"parameter\": \"" << json_escape(texture.var_name)
                        << "\", \"asset\": \"" << json_escape(relative_asset)
                        << "\", \"resolved\": " << (resolved ? "true" : "false")
                        << ", \"resolution\": \"" << json_escape(status) << "\""
                        << ", \"byte_size\": " << bytes.size()
                        << ", \"content_hash\": \""
                        << (resolved ? hash_hex(bytes_hash64(bytes)) : std::string{}) << "\"}";
                    if (texture_index + 1u != record.texture_params.size()) graph << ',';
                    graph << "\n";
                }
                graph << "  ],\n";
                graph << "  \"outputs\": [\n";
                const std::array<std::pair<const char*, const channel_export_s*>, 11> outputs{{
                    {"albedo", &record.albedo_channel}, {"albedo_layer1", &record.albedo_layer1_channel},
                    {"normal", &record.normal_channel}, {"normal_layer1", &record.normal_layer1_channel},
                    {"blend_modulate", &record.blend_modulate_channel},
                    {"roughness", &record.roughness_channel}, {"metalness", &record.metallic_channel},
                    {"height", &record.height_channel}, {"emission", &record.emissive_channel},
                    {"opacity", &record.opacity_channel}, {"detail", &record.detail_channel}
                }};
                for (std::size_t output_index = 0; output_index < outputs.size(); ++output_index)
                {
                    const auto& [name, channel] = outputs[output_index];
                    graph << "    {\"channel\": \"" << name << "\", \"file\": \""
                        << json_escape(channel->filename) << "\", \"provenance\": \""
                        << provenance_name(channel->provenance) << "\", \"confidence\": "
                        << channel->confidence << ", \"source_kind\": \""
                        << json_escape(channel->source_kind) << "\", \"ready\": "
                        << (channel->ready ? "true" : "false") << "}";
                    if (output_index + 1u != outputs.size()) graph << ',';
                    graph << "\n";
                }
                graph << "  ]\n";
                graph << "}\n";
            }

            if (record.dual_layer)
            {
                const auto* base0 = find_exact_texture_param(record, { "$basetexture" });
                const auto* base1 = find_exact_texture_param(record, { "$basetexture2" });
                const auto* bump0 = find_exact_texture_param(record, { "$bumpmap", "$normalmap" });
                const auto* bump1 = find_exact_texture_param(record, { "$bumpmap2", "$normalmap2" });
                const auto* blend = find_exact_texture_param(record, { "$blendmodulatetexture" });
                const auto source_name = [](const texture_param_s* texture)
                {
                    return texture ? texture->source_name : std::string{};
                };
                std::ofstream layers(dir / "material_layers.json", std::ios::trunc);
                layers << "{\n";
                layers << "  \"schema\": \"LegacyMaterialsWorldVertexTransitionV21.8\",\n";
                layers << "  \"note\": \"Layer blend weights live in BSP vertex data; assets are exported separately and are not destructively baked.\",\n";
                layers << "  \"layer0\": {\n";
                layers << "    \"albedo_source\": \"" << json_escape(source_name(base0)) << "\",\n";
                layers << "    \"normal_source\": \"" << json_escape(source_name(bump0)) << "\",\n";
                layers << "    \"normal_output\": \"" << json_escape(record.normal_channel.filename) << "\"\n";
                layers << "  },\n";
                layers << "  \"layer1\": {\n";
                layers << "    \"albedo_source\": \"" << json_escape(source_name(base1)) << "\",\n";
                layers << "    \"albedo_output\": \"" << json_escape(record.albedo_layer1_channel.filename) << "\",\n";
                layers << "    \"albedo_ready\": " << (record.albedo_layer1_channel.ready ? "true" : "false") << ",\n";
                layers << "    \"normal_source\": \"" << json_escape(source_name(bump1)) << "\",\n";
                layers << "    \"normal_output\": \"" << json_escape(record.normal_layer1_channel.filename) << "\",\n";
                layers << "    \"normal_ready\": " << (record.normal_layer1_channel.ready ? "true" : "false") << "\n";
                layers << "  },\n";
                layers << "  \"blend_modulate_source\": \"" << json_escape(source_name(blend)) << "\",\n";
                layers << "  \"blend_modulate_output\": \"" << json_escape(record.blend_modulate_channel.filename) << "\",\n";
                layers << "  \"blend_modulate_ready\": " << (record.blend_modulate_channel.ready ? "true" : "false") << "\n";
                layers << "}\n";
            }

            if (record.detail_texture)
            {
                std::ofstream detail(dir / "detail_composition.json", std::ios::trunc);
                detail << "{\n";
                detail << "  \"schema\": \"LegacyMaterialsDetailCompositionV21.8\",\n";
                detail << "  \"role\": \"" << json_escape(record.detail_semantics) << "\",\n";
                detail << "  \"composition_enabled\": " << (material_exporter::m_compose_detail_layers ? "true" : "false") << ",\n";
                detail << "  \"composition_ready\": " << (record.detail_composition_ready ? "true" : "false") << ",\n";
                detail << "  \"blend_mode\": " << record.detail_blend_mode << ",\n";
                detail << "  \"blend_factor\": " << record.detail_blend_factor << ",\n";
                detail << "  \"scale\": " << record.detail_scale << ",\n";
                detail << "  \"detail_output\": \"" << json_escape(record.detail_channel.filename) << "\",\n";
                detail << "  \"detail_ready\": " << (record.detail_channel.ready ? "true" : "false") << ",\n";
                detail << "  \"policy\": \"Supported roles remain separate authoring inputs; unsupported L4D2 blend modes are quarantined instead of destructively baked.\"\n";
                detail << "}\n";
            }

            if (record.event_emissive_candidate)
            {
                std::ofstream event_link(dir / "event_emissive_link.json", std::ios::trunc);
                event_link << "{\n";
                event_link << "  \"schema\": \"LegacyMaterialsEventEmissiveLinkV21.8\",\n";
                event_link << "  \"material\": \"materials/" << json_escape(record.canonical_name) << ".vmt\",\n";
                event_link << "  \"candidate\": true,\n";
                event_link << "  \"reason\": \"" << json_escape(record.event_emissive_semantics) << "\",\n";
                event_link << "  \"emission_output\": \"" << json_escape(record.emissive_channel.filename) << "\",\n";
                event_link << "  \"emission_ready\": " << (record.emissive_channel.ready ? "true" : "false") << ",\n";
                event_link << "  \"runtime_link\": \"BSP facing-poly material lookup -> Auto PBR event emissive query -> Source event/lightstyle state\",\n";
                event_link << "  \"proxies\": [";
                for (std::size_t i = 0; i < record.detected_proxies.size(); ++i)
                {
                    if (i) event_link << ", ";
                    event_link << "\"" << json_escape(record.detected_proxies[i]) << "\"";
                }
                event_link << "]\n";
                event_link << "}\n";
            }

            {
                std::ofstream report(dir / "sampler_link_report.txt", std::ios::trunc);
                report << "LegacyMaterials V21.8 Source-semantic PBR compiler and provenance audit\n";
                report << (record.resolved_vmt_path.empty()
                    ? "materials/" + record.canonical_name + ".vmt" : record.resolved_vmt_path) << " | "
                    << record.shader_name << " | hash " << hash_hex(record.remix_hash) << "\n\n";
                report << "Grade: " << material_grade_name(record.material_grade)
                    << " | auto activated=" << (record.auto_activated ? "yes" : "no")
                    << " | quarantined=" << (record.quarantined ? "yes" : "no") << "\n";
                report << "Activation: " << record.activation_reason << "\n";
                report << "Source asset grade: " << record.source_asset_grade << "\n";
                report << "Original VMT: " << (record.source_vmt_resolved ? record.source_vmt_status : "missing")
                    << " | bytes " << record.source_vmt_byte_size
                    << " | hash " << (record.source_vmt_resolved ? hash_hex(record.source_vmt_hash) : std::string("none")) << "\n";
                report << "Declared VTF assets: " << record.resolved_texture_asset_count << "/"
                    << record.declared_texture_count << " resolved | missing "
                    << record.missing_texture_asset_count << " | live params "
                    << record.live_texture_param_count << "\n\n";
                report << "Declared VMT texture parameters:\n";
                for (const auto& texture : record.texture_params)
                {
                    report << "  " << texture.var_name << " = " << texture.source_name
                        << " | " << texture.width << "x" << texture.height
                        << " | normal=" << (texture.is_normal ? "yes" : "no") << "\n";
                }
                report << "\nObserved runtime sampler candidates:\n";
                for (std::size_t slot = 0; slot < record.sampler_candidates.size(); ++slot)
                {
                    const auto& candidates = record.sampler_candidates[slot];
                    for (std::size_t index = 0; index < candidates.size(); ++index)
                    {
                        D3DSURFACE_DESC desc{};
                        texture_desc(candidates[index].texture, desc);
                        report << "  sampler " << slot << " candidate " << index
                            << " | hits=" << candidates[index].hits
                            << " | draws=" << candidates[index].first_draw << "-"
                            << candidates[index].last_draw
                            << " | " << desc.Width << "x" << desc.Height
                            << " | format=" << static_cast<unsigned long>(desc.Format) << "\n";
                    }
                }
                report << "\nSelected channels:\n";
                const auto write_selected = [&](const char* name, const channel_export_s& channel)
                {
                    report << "  " << name << ": " << channel.source_parameter
                        << " | kind=" << channel.source_kind
                        << " | slot=" << channel.sampler_slot
                        << " | candidate=" << channel.sampler_candidate
                        << " | score=" << channel.match_score
                        << " | provenance=" << provenance_name(channel.provenance)
                        << " | confidence=" << channel.confidence
                        << " | quality=" << channel.quality.status
                        << " | ready=" << (channel.ready ? "yes" : "no")
                        << " | status=" << channel.actual_status << "\n";
                };
                write_selected("albedo", record.albedo_channel);
                write_selected("albedo_layer1", record.albedo_layer1_channel);
                write_selected("normal", record.normal_channel);
                write_selected("normal_layer1", record.normal_layer1_channel);
                write_selected("blend_modulate", record.blend_modulate_channel);
                write_selected("roughness", record.roughness_channel);
                write_selected("metalness", record.metallic_channel);
                write_selected("height", record.height_channel);
                write_selected("emission", record.emissive_channel);
                write_selected("opacity", record.opacity_channel);
                write_selected("detail", record.detail_channel);
                report << "\nWarnings:\n";
                if (record.warnings.empty()) report << "  none\n";
                for (const auto& warning : record.warnings)
                    report << "  - " << warning << "\n";
            }

            {
                std::ofstream identity(dir / "SOURCE_ID.txt", std::ios::trunc);
                identity << (record.resolved_vmt_path.empty()
                    ? "materials/" + record.canonical_name + ".vmt" : record.resolved_vmt_path) << "\n";
                identity << record.shader_name << "\n";
                identity << record.shader_profile << "\n";
                identity << hash_hex(record.remix_hash) << "\n";
                identity << record.identity_key << "\n";
            }

            {
                std::set<std::string> hashes;
                std::ifstream existing(dir / "hashes.txt");
                for (std::string line; std::getline(existing, line); )
                    if (!line.empty()) hashes.insert(line);
                hashes.insert(hash_hex(record.remix_hash));
                std::ofstream out(dir / "hashes.txt", std::ios::trunc);
                for (const auto& hash : hashes) out << hash << '\n';
            }

            return record.exported;
        }


        bool write_text_if_missing(const std::filesystem::path& path, const std::string& text)
        {
            std::error_code ec;
            if (std::filesystem::exists(path, ec)) return true;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::trunc);
            if (!out.is_open()) return false;
            out << text;
            return out.good();
        }

        bool write_atomic_text(const std::filesystem::path& path, const std::string& text)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            const auto temp = path.string() + ".tmp";
            {
                std::ofstream out(temp, std::ios::trunc | std::ios::binary);
                if (!out.is_open()) return false;
                out << text;
                if (!out.good()) return false;
            }
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(temp, path, ec);
            if (!ec) return true;
            ec.clear();
            std::filesystem::copy_file(temp, path, std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) std::filesystem::remove(temp, ec);
            return !ec;
        }

        bool write_audit_report_unlocked()
        {
            if (!material_exporter::m_write_html_audit) return true;
            std::error_code ec;
            std::filesystem::create_directories(manifests_root_unlocked(), ec);
            if (ec) return false;

            std::size_t resolved = 0u;
            std::size_t activated = 0u;
            std::size_t quarantined = 0u;
            std::size_t warning_total = 0u;
            std::size_t fallback_total = 0u;
            for (const auto& [hash, record] : g_materials)
            {
                if (has_complete_pbr(record)) ++resolved;
                if (record.auto_activated) ++activated;
                if (record.quarantined) ++quarantined;
                warning_total += record.warnings.size();
                const std::array<const channel_export_s*, 11> channels{
                    &record.albedo_channel, &record.albedo_layer1_channel,
                    &record.normal_channel, &record.normal_layer1_channel,
                    &record.blend_modulate_channel, &record.roughness_channel,
                    &record.metallic_channel, &record.height_channel,
                    &record.emissive_channel, &record.opacity_channel,
                    &record.detail_channel
                };
                for (const auto* channel : channels)
                    if (channel->provenance == channel_provenance_t::constant_fallback)
                        ++fallback_total;
            }

            std::ostringstream html;
            html << "<!doctype html><html><head><meta charset=\"utf-8\">";
            html << "<title>LegacyMaterials V21.8 audit</title>";
            html << "<style>";
            html << "body{font-family:Segoe UI,Arial,sans-serif;background:#11151b;color:#e7edf5;margin:24px}";
            html << "h1{margin-bottom:4px}.summary{display:flex;gap:12px;flex-wrap:wrap;margin:16px 0}";
            html << ".card{background:#1b222c;border:1px solid #344050;border-radius:8px;padding:10px 14px}";
            html << "table{border-collapse:collapse;width:100%;font-size:12px;background:#171d25}";
            html << "th,td{border:1px solid #303a48;padding:7px;vertical-align:top}th{position:sticky;top:0;background:#222b37}";
            html << "tr.warn{background:#251d17}tr.bad{background:#29181b}.thumb{width:96px;height:96px;object-fit:contain;background:#0b0e12}";
            html << ".native{color:#70d6a4}.converted{color:#7fb5ff}.inferred{color:#ffc76b}.fallback{color:#ff8e72}.missing{color:#a6adba}";
            html << "code{white-space:pre-wrap}.warnings{max-width:360px}a{color:#8fc7ff}";
            html << "</style></head><body>";
            html << "<h1>LegacyMaterials V21.8 — Source Semantic PBR Compiler</h1>";
            html << "<div>Map: <b>" << html_escape(g_current_map) << "</b></div>";
            html << "<div class=\"summary\">";
            html << "<div class=\"card\">Captured: <b>" << g_materials.size() << "</b></div>";
            html << "<div class=\"card\">Resolved PBR: <b>" << resolved << "</b></div>";
            html << "<div class=\"card\">Auto-active: <b>" << activated << "</b></div>";
            html << "<div class=\"card\">Quarantined: <b>" << quarantined << "</b></div>";
            html << "<div class=\"card\">Warnings: <b>" << warning_total << "</b></div>";
            const auto asset_backed_total = static_cast<std::size_t>(std::count_if(g_materials.begin(), g_materials.end(),
                [](const auto& item) { return item.second.asset_backed_material; }));
            const auto vmt_resolved_total = static_cast<std::size_t>(std::count_if(g_materials.begin(), g_materials.end(),
                [](const auto& item) { return item.second.source_vmt_resolved; }));
            html << "<div class=\"card\">Asset-backed: <b>" << asset_backed_total << "</b></div>";
            html << "<div class=\"card\">Original VMT resolved: <b>" << vmt_resolved_total << "</b></div>";
            html << "<div class=\"card\">Fallback channels: <b>" << fallback_total << "</b></div>";
            html << "<div class=\"card\">Overrides: <b>" << g_material_overrides.size() << "</b></div>";
            html << "</div>";
            html << "<table><thead><tr><th>Material</th><th>Profile / Grade</th><th>Source assets</th><th>Albedo</th><th>Normal</th>";
            html << "<th>Roughness</th><th>Metalness</th><th>Height</th><th>Emission</th>";
            html << "<th>Opacity</th><th>Detail</th><th>Warnings</th></tr></thead><tbody>";

            const auto channel_cell = [&](const material_record_s& record,
                const channel_export_s& channel)
            {
                std::ostringstream cell;
                std::string css = "missing";
                if (channel.provenance == channel_provenance_t::native_map) css = "native";
                else if (channel.provenance == channel_provenance_t::converted_map) css = "converted";
                else if (channel.provenance == channel_provenance_t::inferred_map) css = "inferred";
                else if (channel.provenance == channel_provenance_t::constant_fallback) css = "fallback";
                const std::string base = "../Materials/" + record.export_relative_dir + "/debug_png/";
                if (!channel.preview_filename.empty())
                    cell << "<img class=\"thumb\" loading=\"lazy\" src=\""
                         << html_escape(base + channel.preview_filename) << "\"><br>";
                cell << "<span class=\"" << css << "\"><b>"
                     << provenance_name(channel.provenance) << "</b></span><br>";
                cell << "confidence " << static_cast<int>(std::lround(channel.confidence * 100.0f))
                     << "%<br>";
                cell << html_escape(channel.quality.status) << "<br>";
                cell << "<small>" << html_escape(channel.source_parameter) << "</small>";
                return cell.str();
            };

            for (const auto& [hash, record] : g_materials)
            {
                const char* row_class = record.quarantined || !has_complete_pbr(record) ? "bad" :
                    (!record.warnings.empty() ? "warn" : "");
                html << "<tr class=\"" << row_class << "\">";
                const std::string material_link = "../Materials/" + record.export_relative_dir + "/material.json";
                html << "<td><a href=\"" << html_escape(material_link) << "\"><b>"
                     << html_escape(record.canonical_name) << "</b></a><br>"
                     << html_escape(record.shader_name) << "<br><code>"
                     << hash_hex(hash) << "</code>";
                if (record.override_applied) html << "<br><b>override</b>";
                html << "</td>";
                html << "<td>" << html_escape(record.shader_profile) << "<br>"
                     << html_escape(record.category) << "<br><b>"
                     << material_grade_name(record.material_grade) << "</b><br>"
                     << (record.auto_activated ? "auto-active" :
                         (record.quarantined ? "quarantined" : "not active"))
                     << "<br><small>" << html_escape(record.activation_reason) << "</small></td>";
                html << "<td><b>" << html_escape(record.source_asset_grade) << "</b><br>"
                     << "VMT: " << (record.source_vmt_resolved ? html_escape(record.source_vmt_status) : "missing") << "<br>"
                     << "VTF: " << record.resolved_texture_asset_count << "/" << record.declared_texture_count
                     << "<br>missing: " << record.missing_texture_asset_count
                     << "<br>base albedo: " << (record.source_albedo_asset_resolved ? "resolved" : "missing")
                     << "<hr><b>" << html_escape(record.source_shader_family) << "</b><br>"
                     << html_escape(record.specular_semantics) << "<br>"
                     << html_escape(record.emission_semantics) << "<br><small>"
                     << html_escape(record.animation_semantics) << "</small></td>";
                html << "<td>" << channel_cell(record, record.albedo_channel) << "</td>";
                html << "<td>" << channel_cell(record, record.normal_channel) << "</td>";
                html << "<td>" << channel_cell(record, record.roughness_channel) << "</td>";
                html << "<td>" << channel_cell(record, record.metallic_channel) << "</td>";
                html << "<td>" << channel_cell(record, record.height_channel) << "</td>";
                html << "<td>" << channel_cell(record, record.emissive_channel) << "</td>";
                html << "<td>" << channel_cell(record, record.opacity_channel) << "</td>";
                html << "<td>" << channel_cell(record, record.detail_channel) << "</td>";
                html << "<td class=\"warnings\">";
                if (record.warnings.empty()) html << "none";
                else
                    for (const auto& warning : record.warnings)
                        html << "• " << html_escape(warning) << "<br>";
                html << "</td></tr>";
            }
            html << "</tbody></table></body></html>";
            return write_atomic_text(audit_report_path_unlocked(), html.str());
        }

        bool rebuild_usda_unlocked()
        {
            resolve_export_layout_unlocked();
            const auto root = export_root_unlocked();
            std::error_code ec;
            std::filesystem::create_directories(materials_root_unlocked(), ec);
            std::filesystem::create_directories(conversion_cache_root_unlocked() / "staging", ec);
            std::filesystem::create_directories(tools_root_unlocked(), ec);
            std::filesystem::create_directories(manifests_root_unlocked(), ec);
            if (ec) return false;

            std::ostringstream generated;
            generated << "#usda 1.0\n\n";
            generated << "over \"RootNode\"\n{\n    over \"Looks\"\n    {\n";
            std::ostringstream review;
            review << "#usda 1.0\n\n";
            review << "# Resolved materials excluded from automatic activation.\n";
            review << "# This layer is intentionally not referenced by Materials.usda.\n";
            review << "over \"RootNode\"\n{\n    over \"Looks\"\n    {\n";
            std::size_t resolved_count = 0;
            std::size_t active_count = 0;
            std::size_t quarantine_count = 0;
            for (auto& [hash, record] : g_materials)
            {
                refresh_record_conversion_state(record);
                record.exported = has_complete_pbr(record);
                evaluate_material_activation(record);
                if (record.exported) ++resolved_count;
                if (record.quarantined) ++quarantine_count;
                if (record.auto_activated)
                {
                    generated << '\n';
                    write_material_block(generated, record);
                    ++active_count;
                }
                else if (record.exported)
                {
                    review << '\n';
                    write_material_block(review, record, false);
                }
            }
            generated << "    }\n}\n";
            review << "    }\n}\n";
            const bool auto_ok = write_atomic_text(root / "Materials_Auto.usda", generated.str());
            const bool review_ok = write_atomic_text(root / "Materials_Review.usda", review.str());

            const bool user_ok = write_text_if_missing(root / "Materials_User.usda",
                "#usda 1.0\n\n"
                "# Manual material corrections belong here. This file is never overwritten.\n"
                "over \"RootNode\"\n{\n    over \"Looks\"\n    {\n    }\n}\n");

            std::ostringstream materials;
            materials << "#usda 1.0\n(\n";
            materials << "    subLayers = [\n";
            materials << "        @./Materials_User.usda@,\n";
            materials << "        @./Materials_Auto.usda@\n";
            materials << "    ]\n";
            materials << ")\n";
            const bool materials_ok = write_atomic_text(root / "Materials.usda", materials.str());

            std::filesystem::remove(root / "LegacyMaterials.usda", ec);

            std::ostringstream mod;
            mod << "#usda 1.0\n(\n";
            mod << "    customLayerData = {\n";
            mod << "        string lightspeed_game_name = \"Left 4 Dead 2\"\n";
            mod << "        string lightspeed_layer_type = \"replacement\"\n";
            mod << "        string lightspeed_mod_name = \"LegacyMaterials\"\n";
            mod << "        string lightspeed_mod_notes = \"Source shader semantics, model $cdmaterials resolution, VTF integrity, provenance and protected user layer\"\n";
            mod << "        string lightspeed_mod_version = \"21.8\"\n";
            mod << "    }\n";
            mod << "    subLayers = [ @./Materials.usda@ ]\n";
            mod << "    metersPerUnit = 1\n";
            mod << "    timeCodesPerSecond = 24\n";
            mod << "    upAxis = \"Z\"\n";
            mod << ")\n";
            const bool mod_ok = write_atomic_text(root / "mod.usda", mod.str());

            std::ofstream index(manifests_root_unlocked() / "materials_index.csv", std::ios::trunc);
            index << "remix_hash,identity_key,source_vmt,shader,shader_profile,category,surfaceprop,source_asset_grade,asset_backed,source_vmt_resolved,source_vmt_from_vpk,source_vmt_hash,source_vmt_bytes,declared_vtf_count,resolved_vtf_count,missing_vtf_count,live_texture_params,resolved_vmt_path,source_shader_family,specular_semantics,emission_semantics,alpha_semantics,animation_semantics,semantic_conflict_count,detected_proxy_count,vtf_animated,vtf_alpha,vtf_normal,vtf_ssbump,vtf_srgb,vtf_cubemap,vtf_unsupported,resolved_required_pbr,material_grade,auto_activated,quarantined,activation_reason,override_applied,warning_count,material_directory,albedo_provenance,normal_provenance,roughness_provenance,metalness_provenance,height_provenance,emission_provenance,opacity_provenance,detail_provenance,albedo_source,normal_source,roughness_source,metalness_source,height_source,emission_source,opacity_source,detail_source,preview_png_count,sampler_candidates\n";
            for (const auto& [hash, record] : g_materials)
            {
                std::uint32_t preview_count = static_cast<std::uint32_t>(record.albedo_channel.preview_ready) +
                    static_cast<std::uint32_t>(record.normal_channel.preview_ready) +
                    static_cast<std::uint32_t>(record.roughness_channel.preview_ready) +
                    static_cast<std::uint32_t>(record.metallic_channel.preview_ready) +
                    static_cast<std::uint32_t>(record.height_channel.preview_ready) +
                    static_cast<std::uint32_t>(record.emissive_channel.preview_ready) +
                    static_cast<std::uint32_t>(record.opacity_channel.preview_ready) +
                    static_cast<std::uint32_t>(record.detail_channel.preview_ready) +
                    static_cast<std::uint32_t>(record.albedo_layer1_channel.preview_ready) +
                    static_cast<std::uint32_t>(record.normal_layer1_channel.preview_ready) +
                    static_cast<std::uint32_t>(record.blend_modulate_channel.preview_ready);
                std::uint32_t candidate_count = 0u;
                for (const auto& candidates : record.sampler_candidates)
                    candidate_count += static_cast<std::uint32_t>(candidates.size());
                index << hash_hex(hash) << ",\"" << record.identity_key
                    << "\",\"" << json_escape(record.resolved_vmt_path.empty()
                        ? "materials/" + record.canonical_name + ".vmt" : record.resolved_vmt_path) << "\",\""
                    << record.shader_name << "\",\"" << record.shader_profile << "\",\""
                    << record.category << "\",\"" << record.surface_prop << "\",\""
                    << json_escape(record.source_asset_grade) << "\","
                    << (record.asset_backed_material ? "true" : "false") << ","
                    << (record.source_vmt_resolved ? "true" : "false") << ","
                    << (record.source_vmt_from_vpk ? "true" : "false") << ",\""
                    << (record.source_vmt_resolved ? hash_hex(record.source_vmt_hash) : std::string{}) << "\","
                    << record.source_vmt_byte_size << ','
                    << record.declared_texture_count << ','
                    << record.resolved_texture_asset_count << ','
                    << record.missing_texture_asset_count << ','
                    << record.live_texture_param_count << ",\""
                    << json_escape(record.resolved_vmt_path) << "\",\""
                    << json_escape(record.source_shader_family) << "\",\""
                    << json_escape(record.specular_semantics) << "\",\""
                    << json_escape(record.emission_semantics) << "\",\""
                    << json_escape(record.alpha_semantics) << "\",\""
                    << json_escape(record.animation_semantics) << "\","
                    << record.semantic_conflicts.size() << ','
                    << record.detected_proxies.size() << ','
                    << record.vtf_animated_count << ','
                    << record.vtf_alpha_count << ','
                    << record.vtf_normal_flag_count << ','
                    << record.vtf_ssbump_flag_count << ','
                    << record.vtf_srgb_count << ','
                    << record.vtf_cubemap_count << ','
                    << record.vtf_unsupported_count << ','
                    << (has_complete_pbr(record) ? "true" : "false") << ","
                    << "\"" << material_grade_name(record.material_grade) << "\","
                    << (record.auto_activated ? "true" : "false") << ","
                    << (record.quarantined ? "true" : "false") << ",\""
                    << json_escape(record.activation_reason) << "\","
                    << (record.override_applied ? "true" : "false") << ","
                    << record.warnings.size() << ",\"Materials/" << record.export_relative_dir << "\",\""
                    << provenance_name(record.albedo_channel.provenance) << "\",\""
                    << provenance_name(record.normal_channel.provenance) << "\",\""
                    << provenance_name(record.roughness_channel.provenance) << "\",\""
                    << provenance_name(record.metallic_channel.provenance) << "\",\""
                    << provenance_name(record.height_channel.provenance) << "\",\""
                    << provenance_name(record.emissive_channel.provenance) << "\",\""
                    << provenance_name(record.opacity_channel.provenance) << "\",\""
                    << provenance_name(record.detail_channel.provenance) << "\",\""
                    << json_escape(record.albedo_channel.source_parameter) << "\",\""
                    << json_escape(record.normal_channel.source_parameter) << "\",\""
                    << json_escape(record.roughness_channel.source_parameter) << "\",\""
                    << json_escape(record.metallic_channel.source_parameter) << "\",\""
                    << json_escape(record.height_channel.source_parameter) << "\",\""
                    << json_escape(record.emissive_channel.source_parameter) << "\",\""
                    << json_escape(record.opacity_channel.source_parameter) << "\",\""
                    << json_escape(record.detail_channel.source_parameter) << "\","
                    << preview_count << ',' << candidate_count << "\n";
            }

            std::ofstream aliases(manifests_root_unlocked() / "identity_aliases.csv", std::ios::trunc);
            aliases << "source_vmt,source_vmt_hash,identity_key,material_directory,auto_activated,grade\n";
            for (const auto& [hash, record] : g_materials)
            {
                aliases << "\"" << json_escape(record.resolved_vmt_path.empty()
                    ? "materials/" + record.canonical_name + ".vmt" : record.resolved_vmt_path) << "\","
                    << hash_hex(hash) << ",\"" << record.identity_key << "\",\"Materials/"
                    << record.export_relative_dir << "\"," << (record.auto_activated ? "true" : "false")
                    << ",\"" << material_grade_name(record.material_grade) << "\"\n";
            }

            std::ofstream event_registry(manifests_root_unlocked() / "event_emissive_registry.json", std::ios::trunc);
            event_registry << "{\n  \"schema\": \"LegacyMaterialsEventEmissiveRegistryV21.8\",\n  \"materials\": [\n";
            bool first_event_material = true;
            for (const auto& [hash, record] : g_materials)
            {
                if (!record.event_emissive_candidate) continue;
                if (!first_event_material) event_registry << ",\n";
                first_event_material = false;
                event_registry << "    {\"hash\": \"" << hash_hex(hash)
                    << "\", \"material\": \"materials/" << json_escape(record.canonical_name)
                    << ".vmt\", \"reason\": \"" << json_escape(record.event_emissive_semantics)
                    << "\", \"emission_ready\": " << (record.emissive_channel.ready ? "true" : "false")
                    << ", \"dynamic\": " << (record.animated_or_proxy_driven ? "true" : "false") << "}";
            }
            event_registry << "\n  ]\n}\n";

            std::ofstream readme(root / "README_LEGACY_MATERIALS.txt", std::ios::trunc);
            readme << "LegacyMaterials V21.8 - Source Semantic PBR Compiler\n\n";
            readme << "1. Press Capture Materials in the in-game menu.\n";
            readme << "2. Press Export PBR to LegacyMaterials.\n\n";
            readme << "Final material wrapper: Materials.usda\n";
            readme << "Generated layer: Materials_Auto.usda\n";
            readme << "Protected manual layer: Materials_User.usda (never overwritten)\n";
            readme << "Inactive review layer: Materials_Review.usda (not loaded automatically)\n";
            readme << "Final textures: Materials/<Source VMT path>/\n";
            readme << "Only materials passing the semantic grade policy are written to Materials_Auto.usda.\n";
            readme << "Resolved but low-confidence materials remain on disk and are marked quarantined for review.\n";
            readme << "V21.8 treats Source shader semantics and real VMT/VTF assets as the primary material authority, records loose/VPK provenance and content hashes, quarantines live-only captures by default, exports original source assets, and preserves runtime samplers only as labelled fallbacks.\n";
            readme << "Overrides: config/material_overrides.toml\n";
            readme << "Audit: manifests/material_audit.html\n";
            readme << "Identity aliases: manifests/identity_aliases.csv\n";
            readme << "Event emissive registry: manifests/event_emissive_registry.json\n";
            readme << "This standalone exporter never edits another Remix project.\n";
            readme << "Resolved materials in current export: " << resolved_count << "\n";
            readme << "Auto-activated materials: " << active_count << "\n";
            readme << "Quarantined materials: " << quarantine_count << "\n";

            std::ofstream get_texconv(root / "GET_TEXCONV.bat", std::ios::trunc);
            get_texconv << "@echo off\nsetlocal EnableExtensions\n";
            get_texconv << "if not exist \"%~dp0tools\" mkdir \"%~dp0tools\"\n";
            get_texconv << "set \"WINGET=winget.exe\"\n";
            get_texconv << "where winget.exe >nul 2>nul || set \"WINGET=%LOCALAPPDATA%\\Microsoft\\WindowsApps\\winget.exe\"\n";
            get_texconv << "if exist \"%WINGET%\" \"%WINGET%\" install --id Microsoft.DirectXTex.Texconv -e --accept-source-agreements --accept-package-agreements\n";
            get_texconv << "set \"PATH=%PATH%;%LOCALAPPDATA%\\Microsoft\\WinGet\\Links;%LOCALAPPDATA%\\Microsoft\\WindowsApps\"\n";
            get_texconv << "for /f \"delims=\" %%I in ('where texconv.exe 2^>nul') do copy /y \"%%I\" \"%~dp0tools\\texconv.exe\" >nul\n";
            get_texconv << "if exist \"%~dp0tools\\texconv.exe\" (echo Texconv ready.) else (echo Texconv not found.)\n";
            get_texconv << "pause\n";

            std::ofstream conf(root / "rtx.conf.fragment", std::ios::trunc);
            conf << "rtx.useUnusedRenderstates = True\n";

            std::ofstream version(manifests_root_unlocked() / "generator_version.txt", std::ios::trunc);
            version << "L4D2-RTX ASI V21.8\n";
            version << "layout=SourceMaterialGraphCompiler5_0\n";
            version << "resolved_materials=" << resolved_count << '\n';
            version << "active_materials=" << active_count << '\n';
            version << "quarantined_materials=" << quarantine_count << '\n';
            version << "captured_materials=" << g_materials.size() << '\n';
            version << "last_map=" << g_current_map << '\n';
            version << "overrides=" << g_material_overrides.size() << '\n';

            const bool overrides_ok = write_override_template_unlocked();
            const bool audit_ok = write_audit_report_unlocked();
            return auto_ok && review_ok && user_ok && materials_ok && mod_ok && overrides_ok && audit_ok;
        }
    } // namespace

    material_exporter::material_exporter()
    {
        p_this = this;
        std::scoped_lock lock(g_material_mutex);
        resolve_export_layout_unlocked();
        std::error_code ec;
        std::filesystem::create_directories(materials_root_unlocked(), ec);
        std::filesystem::create_directories(tools_root_unlocked(), ec);
        std::filesystem::create_directories(overrides_path_unlocked().parent_path(), ec);
        write_override_template_unlocked();
        load_overrides_unlocked();
        write_text_if_missing(export_root_unlocked() / "GET_TEXCONV.bat",
            "@echo off\nsetlocal EnableExtensions\n"
            "if not exist \"%~dp0tools\" mkdir \"%~dp0tools\"\n"
            "set \"WINGET=winget.exe\"\n"
            "where winget.exe >nul 2>nul || set \"WINGET=%LOCALAPPDATA%\\Microsoft\\WindowsApps\\winget.exe\"\n"
            "if exist \"%WINGET%\" \"%WINGET%\" install --id Microsoft.DirectXTex.Texconv -e --accept-source-agreements --accept-package-agreements\n"
            "set \"PATH=%PATH%;%LOCALAPPDATA%\\Microsoft\\WinGet\\Links;%LOCALAPPDATA%\\Microsoft\\WindowsApps\"\n"
            "for /f \"delims=\" %%I in ('where texconv.exe 2^>nul') do copy /y \"%%I\" \"%~dp0tools\\texconv.exe\" >nul\n"
            "pause\n");
        g_status = "Press 1. Capture Materials to create a fresh list.";
    }

    material_exporter::~material_exporter()
    {
        clear_registry();
        p_this = nullptr;
    }

    void material_exporter::register_model_material_paths(const studiohdr_t* studio_header)
    {
        if (!m_resolve_model_cdmaterials || !studio_header ||
            studio_header->length <= 0 || studio_header->length > (128 * 1024 * 1024) ||
            studio_header->numtextures < 0 || studio_header->numtextures > 512 ||
            studio_header->numcdtextures < 0 || studio_header->numcdtextures > 32)
            return;

        std::vector<std::string> search_paths;
        search_paths.reserve(static_cast<std::size_t>(studio_header->numcdtextures) + 1u);
        for (int index = 0; index < studio_header->numcdtextures; ++index)
        {
            const char* raw = studio_header->pCdtexture(index);
            if (!raw || !*raw) continue;
            append_unique(search_paths, normalize_cdmaterial_path(raw), 32u);
        }
        if (search_paths.empty()) search_paths.emplace_back();

        std::scoped_lock lock(g_material_mutex);
        for (int index = 0; index < studio_header->numtextures; ++index)
        {
            const auto* texture = studio_header->pTexture(index);
            const char* raw_name = texture ? texture->pszName() : nullptr;
            if (!raw_name || !*raw_name) continue;
            const std::string material_name = lower_slashes(raw_name);
            if (material_name.empty() || material_name.size() > 260u) continue;

            std::vector<std::string> candidates;
            if (material_name.contains('/'))
                append_unique(candidates, "materials/" + material_name + ".vmt");
            for (const auto& path : search_paths)
                append_unique(candidates, "materials/" + path + material_name + ".vmt");
            append_unique(candidates, "materials/" + material_name + ".vmt");

            const std::array<std::string, 2> keys{ material_name, material_basename(material_name) };
            for (const auto& key : keys)
            {
                auto& cached = g_model_material_candidates[key];
                for (const auto& candidate : candidates)
                    append_unique(cached, candidate, 64u);
            }
        }
    }


    namespace
    {
        template <std::size_t N>
        void copy_bridge_text(char (&destination)[N], const std::string& source)
        {
            static_assert(N > 0u);
            const std::size_t count = std::min(source.size(), N - 1u);
            std::memcpy(destination, source.data(), count);
            destination[count] = '\0';
        }

        std::uint64_t bridge_semantic_signature(const material_record_s& record)
        {
            std::uint64_t signature = 1469598103934665603ull;
            const auto mix = [&signature](const std::uint64_t value)
            {
                signature ^= value;
                signature *= 1099511628211ull;
            };
            const auto mix_float = [&mix](const float value)
            {
                std::uint32_t bits = 0u;
                std::memcpy(&bits, &value, sizeof(bits));
                mix(bits);
            };
            mix_float(record.calibrated_roughness);
            mix_float(record.calibrated_metallic_hint);
            mix_float(record.calibrated_dielectric_f0);
            mix_float(record.calibrated_reflection_weight);
            mix_float(record.phong_exponent);
            mix_float(record.phong_boost);
            mix_float(record.emissive_intensity);
            mix_float(record.alpha_test_reference);
            mix_float(record.detail_scale);
            mix_float(record.detail_blend_factor);
            mix(record.has_normal); mix(record.ssbump); mix(record.translucent);
            mix(record.alpha_tested); mix(record.emissive); mix(record.additive);
            mix(record.nocull); mix(record.water_material); mix(record.glass_material);
            mix(record.dynamic_material); mix(record.detail_texture); mix(record.dual_layer);
            mix(record.envmap_enabled); mix(record.parallax_enabled);
            mix(record.base_alpha_phong_mask); mix(record.base_alpha_envmap_mask);
            mix(record.normal_alpha_envmap_mask); mix(record.base_luminance_phong_mask);
            mix(record.selfillum_enabled); mix(record.emissiveblend_enabled);
            mix(record.unlit_fullbright); mix(record.asset_backed_material);
            return signature;
        }

        std::uint32_t classify_bridge_material(const material_record_s& record)
        {
            const std::string category = lower_slashes(record.category + "/" + record.surface_prop + "/" + record.canonical_name);
            if (record.water_material) return REMIX_SOURCE_CLASS_WATER;
            if (record.glass_material) return REMIX_SOURCE_CLASS_GLASS;
            if (record.unlit_fullbright) return REMIX_SOURCE_CLASS_UNLIT;
            if (record.emissive) return REMIX_SOURCE_CLASS_EMISSIVE;
            if (category.find("metal") != std::string::npos) return REMIX_SOURCE_CLASS_METAL;
            if (category.find("wood") != std::string::npos) return REMIX_SOURCE_CLASS_WOOD;
            if (category.find("cloth") != std::string::npos || category.find("fabric") != std::string::npos) return REMIX_SOURCE_CLASS_FABRIC;
            if (category.find("rubber") != std::string::npos || category.find("tire") != std::string::npos) return REMIX_SOURCE_CLASS_RUBBER;
            if (category.find("flesh") != std::string::npos || category.find("skin") != std::string::npos || category.find("organic") != std::string::npos) return REMIX_SOURCE_CLASS_ORGANIC;
            if (category.find("foliage") != std::string::npos || category.find("grass") != std::string::npos || category.find("leaf") != std::string::npos) return REMIX_SOURCE_CLASS_FOLIAGE;
            if (category.find("concrete") != std::string::npos || category.find("brick") != std::string::npos || category.find("plaster") != std::string::npos || category.find("stone") != std::string::npos) return REMIX_SOURCE_CLASS_MASONRY;
            return REMIX_SOURCE_CLASS_UNKNOWN;
        }

        std::uint32_t bridge_specular_mask_mode(const material_record_s& record)
        {
            if (record.base_alpha_phong_mask || record.base_alpha_envmap_mask) return REMIX_SOURCE_MASK_BASE_ALPHA;
            if (record.normal_alpha_envmap_mask) return REMIX_SOURCE_MASK_NORMAL_ALPHA;
            if (record.base_luminance_phong_mask) return REMIX_SOURCE_MASK_BASE_LUMINANCE;
            if (find_texture_param(record, "$phongexponenttexture") != nullptr)
                return REMIX_SOURCE_MASK_PHONG_EXPONENT_TEXTURE;
            if (find_texture_param(record, "$envmapmask") != nullptr
                || find_texture_param(record, "$phongwarptexture") != nullptr)
                return REMIX_SOURCE_MASK_DEDICATED_TEXTURE;
            return REMIX_SOURCE_MASK_NONE;
        }

        std::uint32_t bridge_emission_mask_mode(const material_record_s& record)
        {
            if (record.emissiveblend_enabled) return REMIX_SOURCE_MASK_EMISSIVE_BLEND;
            if (record.selfillum_envmapmask_alpha) return REMIX_SOURCE_MASK_NORMAL_ALPHA;
            if (record.selfillum_enabled) return REMIX_SOURCE_MASK_BASE_ALPHA;
            if (record.detail_texture && record.emissive) return REMIX_SOURCE_MASK_DETAIL;
            return REMIX_SOURCE_MASK_NONE;
        }

        std::uint32_t classify_model_draw(const std::string& model_name, const int entity_index)
        {
            const auto model = lower_slashes(model_name);
            if (model.contains("models/weapons/v_") || model.contains("/v_models/")) return REMIX_SOURCE_DRAW_VIEWMODEL;
            if (model.contains("models/weapons/w_") || model.contains("/w_models/")) return REMIX_SOURCE_DRAW_WEAPON_WORLD;
            if (model.contains("infected") || model.contains("common_male") || model.contains("common_female")) return REMIX_SOURCE_DRAW_INFECTED;
            if (model.contains("survivor") || model.contains("models/player/")) return REMIX_SOURCE_DRAW_PLAYER;
            if (model.contains("ragdoll")) return REMIX_SOURCE_DRAW_RAGDOLL;
            return entity_index < 0 ? REMIX_SOURCE_DRAW_STATIC_PROP : REMIX_SOURCE_DRAW_DYNAMIC_PROP;
        }

        const std::string* declared_texture_name(const material_record_s& record,
            const std::initializer_list<const char*> names)
        {
            for (const auto* name : names)
            {
                if (const auto* param = find_texture_param(record, name); param && !param->source_name.empty())
                    return &param->source_name;
            }
            return nullptr;
        }

        template <std::size_t N>
        void copy_declared_texture(char (&destination)[N], const material_record_s& record,
            const std::initializer_list<const char*> names)
        {
            if (const auto* value = declared_texture_name(record, names)) copy_bridge_text(destination, *value);
            else destination[0] = '\0';
        }

        std::uint32_t publish_model_draw_packet(const material_record_s& record, const BufferedState_t& state)
        {
            if (!material_exporter::m_source_material_bridge_enabled ||
                !material_exporter::m_model_material_packets_enabled || !g_active_model_draw.active)
                return 0u;

            RemixSourceModelDrawInfoV2 packet{};
            packet.structSize = sizeof(packet);
            packet.abiVersion = REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V2;
            packet.packetId = g_source_bridge_latest_draw_packet.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (packet.packetId == 0u) packet.packetId = g_source_bridge_latest_draw_packet.fetch_add(1u, std::memory_order_relaxed) + 1u;
            packet.drawClass = g_active_model_draw.draw_class;
            packet.materialHash = record.remix_hash;
            packet.materialRevision = record.bridge_revision;
            packet.modelHash = g_active_model_draw.model_hash;
            packet.instanceHash = g_active_model_draw.instance_hash;
            packet.entityIndex = g_active_model_draw.entity_index;
            packet.skin = g_active_model_draw.skin;
            packet.body = g_active_model_draw.body;
            packet.hitboxSet = g_active_model_draw.hitbox_set;
            packet.sourceInstance = g_active_model_draw.source_instance;
            packet.materialOrdinal = g_active_model_draw.material_ordinal++;
            packet.albedoSampler = record.albedo_channel.sampler_slot;
            packet.normalSampler = record.normal_channel.sampler_slot;
            packet.roughnessSampler = record.roughness_channel.sampler_slot;
            packet.emissiveSampler = record.emissive_channel.sampler_slot;
            packet.flags = REMIX_SOURCE_DRAW_HAS_MODEL_CONTEXT;
            if (packet.drawClass != REMIX_SOURCE_DRAW_STATIC_PROP) packet.flags |= REMIX_SOURCE_DRAW_SKINNED;
            if (packet.drawClass == REMIX_SOURCE_DRAW_VIEWMODEL) packet.flags |= REMIX_SOURCE_DRAW_VIEWMODEL_FLAG;
            if (packet.drawClass == REMIX_SOURCE_DRAW_PLAYER) packet.flags |= REMIX_SOURCE_DRAW_PLAYER_FLAG;
            if (packet.drawClass == REMIX_SOURCE_DRAW_INFECTED) packet.flags |= REMIX_SOURCE_DRAW_INFECTED_FLAG;
            if (packet.drawClass == REMIX_SOURCE_DRAW_VIEWMODEL || packet.drawClass == REMIX_SOURCE_DRAW_WEAPON_WORLD)
                packet.flags |= REMIX_SOURCE_DRAW_WEAPON_FLAG;
            if (record.dynamic_material) packet.flags |= REMIX_SOURCE_DRAW_DYNAMIC_MATERIAL;
            if (record.asset_backed_material) packet.flags |= REMIX_SOURCE_DRAW_ASSET_BACKED;

            for (std::uint32_t i = 0u; i < 16u; ++i)
            {
                const auto source_texture_id = state.m_BoundTexture[i];
                packet.boundTextureIds[i] = source_texture_id > 0 ? static_cast<std::uint32_t>(source_texture_id) : 0u;
                if (packet.boundTextureIds[i] != 0u) packet.boundTextureMask |= 1u << i;
            }
            if ((packet.boundTextureMask & 1u) != 0u) packet.flags |= REMIX_SOURCE_DRAW_HAS_ALBEDO_BINDING;
            if ((packet.boundTextureMask & ~1u) != 0u && record.has_normal) packet.flags |= REMIX_SOURCE_DRAW_HAS_NORMAL_BINDING;
            const auto normalized_shader = lower_slashes(record.shader_name);
            if ((packet.flags & REMIX_SOURCE_DRAW_HAS_ALBEDO_BINDING) != 0u &&
                !normalized_shader.starts_with("infected") &&
                !normalized_shader.starts_with("refract") &&
                !normalized_shader.starts_with("water"))
                packet.flags |= REMIX_SOURCE_DRAW_EXPLICIT_ALBEDO;

            copy_bridge_text(packet.modelName, g_active_model_draw.model_name);
            copy_bridge_text(packet.materialName, record.canonical_name);
            copy_bridge_text(packet.shaderName, record.shader_name);
            copy_declared_texture(packet.albedoTexture, record, { "$basetexture", "$basetexture2" });
            copy_declared_texture(packet.normalTexture, record, { "$bumpmap", "$normalmap", "$detailnormalmap" });
            copy_declared_texture(packet.roughnessTexture, record, { "$phongexponenttexture", "$envmapmask" });
            copy_declared_texture(packet.emissiveTexture, record, { "$selfillummask", "$emissiveblendbasetexture", "$detail" });

            auto& slot = g_source_bridge_draw_packets[packet.packetId % k_source_bridge_draw_packet_limit];
            slot.packet = packet;
            slot.packet_id = packet.packetId;
            g_source_bridge_draw_packet_count = std::min<std::uint32_t>(
                g_source_bridge_draw_packet_count + 1u,
                static_cast<std::uint32_t>(k_source_bridge_draw_packet_limit));
            g_active_model_draw.last_packet_id = packet.packetId;
            g_active_model_draw.last_packet_flags = static_cast<std::uint32_t>(packet.flags & 0xffffffffull);
            return packet.packetId;
        }
    }

    std::uint64_t material_exporter::capture_draw(IMaterialInternal* material,
        IShaderAPIDX8* shader_api, const BufferedState_t& state)
    {
        // World/HUD passes can follow a model draw in the same thread. Never carry a
        // previous Studio packet into a non-model primitive through RS214/RS215.
        if (!g_active_model_draw.active)
        {
            g_active_model_draw.last_packet_id = 0u;
            g_active_model_draw.last_packet_flags = 0u;
        }
        if (!material || !material->vftable) return 0;

        const auto generation = g_runtime_material_identity_generation.load(std::memory_order_relaxed);
        const auto cache_index = (reinterpret_cast<std::uintptr_t>(material) >> 4u) &
            (k_runtime_material_identity_cache_size - 1u);
        auto& identity_cache = g_runtime_material_identity_cache[cache_index];
        if (identity_cache.material != material || identity_cache.generation != generation)
        {
            const char* name_ptr = material->vftable->GetName(material);
            const char* shader_ptr = material->vftable->GetShaderName(material);
            identity_cache.material = material;
            identity_cache.generation = generation;
            identity_cache.name = lower_slashes(name_ptr ? name_ptr : "");
            identity_cache.shader = shader_ptr ? shader_ptr : "";
            identity_cache.normalized_shader = lower_slashes(identity_cache.shader);
            identity_cache.ignored = should_ignore_material(identity_cache.name, identity_cache.shader);
            identity_cache.water_or_refract = identity_cache.normalized_shader.starts_with("water") ||
                identity_cache.normalized_shader.starts_with("refract");
            const std::string identity = "source1/materials/" + identity_cache.name + ".vmt|" +
                identity_cache.normalized_shader;
            identity_cache.hash = stable_material_hash(identity);
        }
        if (identity_cache.ignored) return 0;

        const auto& name = identity_cache.name;
        const auto& shader = identity_cache.shader;
        const bool runtime_water_or_refract = identity_cache.water_or_refract;
        const std::uint64_t hash = identity_cache.hash;

        // Stable hash injection and metadata publication are independent. With both
        // capture and the bridge disabled, this remains the original zero-overhead path.
        const bool runtime_bridge = m_source_material_bridge_enabled && !runtime_water_or_refract;
        if (!m_capture_enabled && !runtime_bridge)
            return runtime_water_or_refract && !m_inject_stable_hashes ? 0u : hash;

        {
            std::scoped_lock lock(g_material_mutex);
            auto [it, inserted] = g_materials.try_emplace(hash);
            auto& record = it->second;
            ++record.draw_count;
            const std::uint64_t old_signature = inserted ? 0u : bridge_semantic_signature(record);
            const bool refresh_runtime_metadata = inserted || record.draw_count <= 8u || (record.draw_count % 120u) == 0u;
            if (inserted)
            {
                record.remix_hash = hash;
                record.canonical_name = name;
                record.shader_name = shader;
                const char* group = material->vftable->GetTextureGroupName(material);
                record.texture_group = group ? group : "";
                record.export_relative_dir = safe_material_relative_path(name).generic_string();
                infer_pbr(record, material);
                record.exported = false;
            }
            if (refresh_runtime_metadata)
            {
                // Lightweight VMT/IMaterial metadata only. This path intentionally does
                // not export DDS files, run texconv or read textures back to the CPU.
                capture_material_params(record, material);
                refresh_material_features(record);
                if (m_capture_enabled && m_resolve_source_assets_during_capture)
                {
                    if (inserted || (m_rescan_unresolved_assets_periodically &&
                        !record.asset_backed_material && (record.draw_count % 120u) == 0u))
                        refresh_source_asset_inventory(record);
                }
                const std::uint64_t new_signature = bridge_semantic_signature(record);
                if (inserted || new_signature != old_signature)
                {
                    record.bridge_revision = g_source_bridge_registry_revision.fetch_add(1u, std::memory_order_relaxed) + 1u;
                }
            }
            if (m_capture_enabled)
            {
                retain_bound_textures(record, shader_api, state);
                g_status = std::format("Capturing materials: {} unique VMT/hash pairs collected on map '{}'.",
                    g_materials.size(), g_current_map);
            }
            else if (runtime_bridge && (inserted || (record.draw_count % 240u) == 0u))
            {
                g_status = std::format("Source Material Bridge: {} runtime material records, {} Studio packets on map '{}'.",
                    g_materials.size(), g_source_bridge_draw_packet_count, g_current_map);
            }
            if (g_active_model_draw.active) publish_model_draw_packet(record, state);
        }
        // Runtime metadata never takes ownership of Water/Refract. Their original
        // Source rendering remains in the compatibility mod. Explicit stable-hash
        // authoring can still opt in through m_inject_stable_hashes.
        return runtime_water_or_refract && !m_inject_stable_hashes ? 0u : hash;
    }

    void material_exporter::on_map_load(const char* map_name)
    {
        g_runtime_material_identity_generation.fetch_add(1u, std::memory_order_relaxed);
        std::scoped_lock lock(g_material_mutex);
        for (auto& [hash, record] : g_materials) release_record(record);
        g_materials.clear();
        g_model_material_candidates.clear();
        for (auto& slot : g_source_bridge_draw_packets) slot = {};
        g_source_bridge_draw_packet_count = 0u;
        g_source_bridge_latest_draw_packet.store(0u, std::memory_order_relaxed);
        g_active_model_draw = {};
        g_source_bridge_registry_revision.fetch_add(1u, std::memory_order_relaxed);
        g_current_map = normalize_map(map_name ? map_name : "");
        m_capture_enabled = false;
        g_status = std::format("Map '{}' loaded. Press 1. Capture Materials when the scene is ready.", g_current_map);
    }

    void material_exporter::begin_capture()
    {
        std::scoped_lock lock(g_material_mutex);
        for (auto& [hash, record] : g_materials) release_record(record);
        g_materials.clear();
        for (auto& slot : g_source_bridge_draw_packets) slot = {};
        g_source_bridge_draw_packet_count = 0u;
        g_source_bridge_latest_draw_packet.store(0u, std::memory_order_relaxed);
        g_source_bridge_registry_revision.fetch_add(1u, std::memory_order_relaxed);
        m_capture_enabled = true;
        m_auto_export_new_materials = false;
        m_background_bc_conversion = false;
        g_status = "Capture started. Move through the scene or rotate the camera; the list updates from real render calls.";
    }

    void material_exporter::clear_registry()
    {
        std::scoped_lock lock(g_material_mutex);
        for (auto& [hash, record] : g_materials) release_record(record);
        g_materials.clear();
        for (auto& slot : g_source_bridge_draw_packets) slot = {};
        g_source_bridge_draw_packet_count = 0u;
        g_source_bridge_latest_draw_packet.store(0u, std::memory_order_relaxed);
        g_source_bridge_registry_revision.fetch_add(1u, std::memory_order_relaxed);
        m_capture_enabled = false;
        g_status = "Capture list cleared. Existing LegacyMaterials files were preserved.";
    }

    bool material_exporter::export_material(const std::uint64_t hash)
    {
        std::scoped_lock lock(g_material_mutex);
        const auto it = g_materials.find(hash);
        if (it == g_materials.end()) return false;
        m_capture_enabled = false;
        load_overrides_unlocked();
        refresh_source_asset_inventory(it->second);
        const bool files_ok = export_record_files(it->second);
        const bool mod_ok = rebuild_usda_unlocked();
        g_status = files_ok && mod_ok
            ? std::format("Exported semantic PBR assets for materials/{}.vmt as mat_{}: grade {}, {}.",
                it->second.canonical_name, hash_hex(hash),
                material_grade_name(it->second.material_grade),
                it->second.auto_activated ? "active in Materials_Auto.usda" :
                    (it->second.quarantined ? "quarantined for review" : "not auto-active"))
            : std::format("Could not resolve required PBR assets for materials/{}.vmt. Check texconv.exe, provenance_graph.json and the HTML audit.",
                it->second.canonical_name);
        return files_ok && mod_ok;
    }

    bool material_exporter::export_all()
    {
        std::scoped_lock lock(g_material_mutex);
        if (g_materials.empty())
        {
            g_status = "Nothing to export. Press 1. Capture Materials first.";
            return false;
        }
        if (locate_texconv().empty())
        {
            g_status = std::format("texconv.exe was not found. Run {} before exporting.",
                (export_root_unlocked() / "GET_TEXCONV.bat").string());
            return false;
        }

        m_capture_enabled = false;
        m_background_bc_conversion = false;
        load_overrides_unlocked();

        std::error_code ec;
        if (m_clean_export_before_export)
        {
            std::filesystem::remove_all(materials_root_unlocked(), ec);
            ec.clear();
            std::filesystem::remove_all(export_root_unlocked() / "materials", ec);
            ec.clear();
            std::filesystem::remove_all(conversion_cache_root_unlocked(), ec);
            ec.clear();
        }
        std::filesystem::create_directories(materials_root_unlocked(), ec);
        std::filesystem::create_directories(conversion_cache_root_unlocked() / "staging", ec);

        std::size_t exported = 0;
        for (auto& [hash, record] : g_materials)
        {
            refresh_source_asset_inventory(record);
            if (export_record_files(record)) ++exported;
        }

        const bool mod_ok = rebuild_usda_unlocked();
        const auto active = static_cast<std::size_t>(std::count_if(g_materials.begin(), g_materials.end(),
            [](const auto& item) { return item.second.auto_activated; }));
        const auto quarantined = static_cast<std::size_t>(std::count_if(g_materials.begin(), g_materials.end(),
            [](const auto& item) { return item.second.quarantined; }));
        g_status = std::format(
            "Semantic PBR export complete: {}/{} materials resolved, {} auto-active, {} quarantined. Output: {}.",
            exported, g_materials.size(), active, quarantined, materials_root_unlocked().string());
        return exported > 0 && mod_ok;
    }

    bool material_exporter::rescan_source_assets()
    {
        std::scoped_lock lock(g_material_mutex);
        if (g_materials.empty())
        {
            g_status = "Nothing to rescan. Capture materials first.";
            return false;
        }

        std::size_t asset_backed = 0u;
        std::size_t vmt_resolved = 0u;
        for (auto& [hash, record] : g_materials)
        {
            refresh_source_asset_inventory(record);
            if (record.source_vmt_resolved) ++vmt_resolved;
            if (record.asset_backed_material) ++asset_backed;
            if (record.exported) evaluate_material_activation(record);
        }
        g_status = std::format(
            "Source asset rescan complete: {}/{} VMTs resolved, {}/{} materials have a real VMT + base VTF.",
            vmt_resolved, g_materials.size(), asset_backed, g_materials.size());
        return asset_backed > 0u;
    }

    bool material_exporter::rescan_source_asset(const std::uint64_t hash)
    {
        std::scoped_lock lock(g_material_mutex);
        const auto it = g_materials.find(hash);
        if (it == g_materials.end()) return false;
        refresh_source_asset_inventory(it->second);
        if (it->second.exported) evaluate_material_activation(it->second);
        g_status = std::format(
            "Source asset rescan for materials/{}.vmt: {} (VMT {}, textures {}/{}).",
            it->second.canonical_name, it->second.source_asset_grade,
            it->second.source_vmt_resolved ? "resolved" : "missing",
            it->second.resolved_texture_asset_count,
            it->second.declared_texture_count);
        return it->second.asset_backed_material;
    }

    bool material_exporter::rebuild_usda()
    {
        std::scoped_lock lock(g_material_mutex);
        const bool ok = rebuild_usda_unlocked();
        const auto resolved = static_cast<std::size_t>(std::count_if(g_materials.begin(), g_materials.end(),
            [](const auto& item) { return has_complete_pbr(item.second); }));
        const auto active = static_cast<std::size_t>(std::count_if(g_materials.begin(), g_materials.end(),
            [](const auto& item) { return item.second.auto_activated; }));
        g_status = ok
            ? std::format("Rebuilt protected material stack: {} resolved, {} auto-active.", resolved, active)
            : "Materials.usda / Materials_Auto.usda rebuild failed.";
        return ok;
    }

    bool material_exporter::initialize_legacy_mod()
    {
        std::scoped_lock lock(g_material_mutex);
        g_export_root.clear();
        resolve_export_layout_unlocked();
        write_override_template_unlocked();
        load_overrides_unlocked();
        const bool ok = rebuild_usda_unlocked();
        g_status = ok
            ? std::format("LegacyMaterials V21.8 initialized at {}.", export_root_unlocked().string())
            : "LegacyMaterials initialization failed.";
        return ok;
    }

    bool material_exporter::reload_overrides()
    {
        std::scoped_lock lock(g_material_mutex);
        const bool ok = load_overrides_unlocked();
        g_status = ok
            ? std::format("Reloaded {} material override section(s) from {}.",
                g_material_overrides.size(), overrides_path_unlocked().string())
            : "Could not reload material overrides.";
        return ok;
    }

    bool material_exporter::write_override_template()
    {
        std::scoped_lock lock(g_material_mutex);
        const bool ok = write_override_template_unlocked();
        if (ok) load_overrides_unlocked();
        g_status = ok
            ? std::format("Material override file is ready at {}.", overrides_path_unlocked().string())
            : "Could not create material_overrides.toml.";
        return ok;
    }

    bool material_exporter::write_audit_report()
    {
        std::scoped_lock lock(g_material_mutex);
        const bool ok = write_audit_report_unlocked();
        g_status = ok
            ? std::format("Material audit written to {}.", audit_report_path_unlocked().string())
            : "Could not write material audit.";
        return ok;
    }

    bool material_exporter::query_event_emissive_material(const std::string& material_name,
        std::string* reason)
    {
        if (!m_link_event_emissive_materials || material_name.empty())
        {
            if (reason) *reason = "event emissive linkage disabled or material name empty";
            return false;
        }

        std::scoped_lock lock(g_material_mutex);
        const std::string normalized = lower_slashes(material_name);
        for (const auto& [hash, record] : g_materials)
        {
            (void)hash;
            if (record.canonical_name == normalized ||
                material_basename(record.canonical_name) == material_basename(normalized))
            {
                if (reason) *reason = record.event_emissive_semantics;
                return record.event_emissive_candidate;
            }
        }

        // BSP/facing-poly discovery can run before the material has produced a draw.
        // Resolve the Source VMT on demand without inserting a synthetic renderer record.
        material_record_s probe;
        probe.canonical_name = normalized;
        probe.shader_name.clear();
        std::vector<std::uint8_t> bytes;
        std::string status;
        std::string resolved_path;
        if (!resolve_record_source_vmt(probe, bytes, status, resolved_path))
        {
            if (reason) *reason = "VMT not resolved for facing-poly material: " + status;
            return false;
        }
        probe.resolved_vmt_path = resolved_path;
        std::unordered_set<std::string> visited;
        merge_vmt_asset(probe, resolved_path, visited, 0u);
        materialize_effective_vmt(probe);
        if (m_enable_source_shader_semantics) interpret_source_semantics(probe);
        materialize_effective_vmt(probe);
        if (reason) *reason = probe.event_emissive_semantics;
        return probe.event_emissive_candidate;
    }

    std::size_t material_exporter::get_override_count()
    {
        std::scoped_lock lock(g_material_mutex);
        return g_material_overrides.size();
    }

    std::size_t material_exporter::get_installed_material_count()
    {
        std::scoped_lock lock(g_material_mutex);
        return static_cast<std::size_t>(std::count_if(g_materials.begin(), g_materials.end(),
            [](const auto& item) { return has_complete_pbr(item.second); }));
    }


    void material_exporter::begin_model_draw(const ModelRenderInfo_t& info)
    {
        g_active_model_draw = {};
        if (!m_model_material_packets_enabled || !info.pModel) return;
        g_active_model_draw.active = true;
        g_active_model_draw.model_name = info.pModel->szPathName;
        g_active_model_draw.model_hash = stable_material_hash(lower_slashes(g_active_model_draw.model_name));
        g_active_model_draw.entity_index = info.entity_index;
        g_active_model_draw.skin = info.skin;
        g_active_model_draw.body = info.body;
        g_active_model_draw.hitbox_set = info.hitboxset;
        g_active_model_draw.source_instance = info.instance;
        g_active_model_draw.draw_class = classify_model_draw(g_active_model_draw.model_name, info.entity_index);
        const std::string identity = lower_slashes(g_active_model_draw.model_name) + "|" +
            std::to_string(info.entity_index) + "|" + std::to_string(info.skin) + "|" +
            std::to_string(info.body) + "|" + std::to_string(info.hitboxset) + "|" +
            std::to_string(info.instance);
        g_active_model_draw.instance_hash = stable_material_hash(identity);
    }

    void material_exporter::end_model_draw()
    {
        g_active_model_draw.active = false;
        g_active_model_draw.material_ordinal = 0u;
        g_active_model_draw.last_packet_id = 0u;
        g_active_model_draw.last_packet_flags = 0u;
    }

    std::uint32_t material_exporter::current_model_draw_packet_id()
    {
        return g_active_model_draw.last_packet_id;
    }

    std::uint32_t material_exporter::current_model_draw_packet_flags()
    {
        return g_active_model_draw.last_packet_flags;
    }

    std::uint64_t material_exporter::get_source_bridge_registry_revision()
    {
        return g_source_bridge_registry_revision.load(std::memory_order_relaxed);
    }

    bool material_exporter::query_source_bridge_material(const std::uint64_t hash,
        RemixSourceMaterialInfoV1* output)
    {
        g_source_bridge_query_count.fetch_add(1u, std::memory_order_relaxed);
        if (!m_source_material_bridge_enabled || !output ||
            output->structSize < sizeof(RemixSourceMaterialInfoV1) ||
            output->abiVersion != REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V1)
            return false;

        std::scoped_lock lock(g_material_mutex);
        const auto it = g_materials.find(hash);
        if (it == g_materials.end()) return false;
        const auto& record = it->second;
        const std::string normalized_shader = lower_slashes(record.shader_name);
        if (record.water_material || normalized_shader.starts_with("water") ||
            normalized_shader.starts_with("refract"))
            return false;

        RemixSourceMaterialInfoV1 info{};
        info.structSize = sizeof(info);
        info.abiVersion = REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V1;
        info.materialHash = hash;
        info.materialRevision = record.bridge_revision;
        info.materialClass = classify_bridge_material(record);
        info.specularMaskMode = bridge_specular_mask_mode(record);
        info.emissionMaskMode = bridge_emission_mask_mode(record);
        info.roughness = std::clamp(record.calibrated_roughness, 0.02f, 1.0f);
        info.metallic = std::clamp(record.calibrated_metallic_hint, 0.0f, 1.0f);
        info.dielectricF0 = std::clamp(record.calibrated_dielectric_f0, 0.0f, 1.0f);
        info.reflectionWeight = std::clamp(record.calibrated_reflection_weight, 0.0f, 1.0f);
        info.phongExponent = record.phong_exponent;
        info.phongBoost = std::max(record.phong_boost, 0.0f);
        info.emissiveIntensity = std::max(record.emissive_intensity, 0.0f);
        info.alphaTestReference = std::clamp(record.alpha_test_reference, 0.0f, 1.0f);
        info.detailScale = std::max(record.detail_scale, 0.0f);
        info.detailBlendFactor = std::clamp(record.detail_blend_factor, 0.0f, 1.0f);
        info.roughnessConfidence = record.shader_semantics_valid ? std::clamp(record.confidence, 0.0f, 1.0f) : 0.35f;
        info.normalConfidence = record.has_normal ? 1.0f : (record.ssbump ? 0.75f : 0.0f);
        info.materialConfidence = std::clamp(record.confidence, 0.0f, 1.0f);

        const bool has_albedo = !record.texture_params.empty() || record.source_albedo_asset_resolved;
        if (has_albedo) info.flags |= REMIX_SOURCE_MATERIAL_HAS_ALBEDO;
        if (record.has_normal) info.flags |= REMIX_SOURCE_MATERIAL_HAS_NORMAL;
        if (record.roughness_channel.ready && !record.roughness_channel.generated) info.flags |= REMIX_SOURCE_MATERIAL_HAS_ROUGHNESS;
        if (record.metallic_channel.ready && !record.metallic_channel.generated) info.flags |= REMIX_SOURCE_MATERIAL_HAS_METALLIC;
        if (info.specularMaskMode != REMIX_SOURCE_MASK_NONE) info.flags |= REMIX_SOURCE_MATERIAL_HAS_SPECULAR_MASK;
        if (find_texture_param(record, "$phongexponenttexture") != nullptr)
            info.flags |= REMIX_SOURCE_MATERIAL_HAS_PHONG_EXPONENT_TEXTURE;
        if (record.height_channel.ready && !record.height_channel.generated) info.flags |= REMIX_SOURCE_MATERIAL_HAS_HEIGHT;
        if (record.ssbump) info.flags |= REMIX_SOURCE_MATERIAL_SSBUMP;
        if (record.selfillum_enabled || record.emissive) info.flags |= REMIX_SOURCE_MATERIAL_SELF_ILLUM;
        if (record.alpha_tested) info.flags |= REMIX_SOURCE_MATERIAL_ALPHA_TEST;
        if (record.translucent) info.flags |= REMIX_SOURCE_MATERIAL_TRANSLUCENT;
        if (record.additive) info.flags |= REMIX_SOURCE_MATERIAL_ADDITIVE;
        if (record.nocull) info.flags |= REMIX_SOURCE_MATERIAL_TWO_SIDED;
        if (record.water_material) info.flags |= REMIX_SOURCE_MATERIAL_WATER;
        if (record.glass_material) info.flags |= REMIX_SOURCE_MATERIAL_GLASS;
        if (record.unlit_fullbright) info.flags |= REMIX_SOURCE_MATERIAL_UNLIT;
        if (record.dynamic_material) info.flags |= REMIX_SOURCE_MATERIAL_DYNAMIC;
        if (record.detail_texture) info.flags |= REMIX_SOURCE_MATERIAL_DETAIL;
        if (record.dual_layer) info.flags |= REMIX_SOURCE_MATERIAL_DUAL_LAYER;
        if (record.envmap_enabled) info.flags |= REMIX_SOURCE_MATERIAL_ENV_MAP;
        if (record.parallax_enabled) info.flags |= REMIX_SOURCE_MATERIAL_PARALLAX;
        if (record.event_emissive_candidate) info.flags |= REMIX_SOURCE_MATERIAL_EVENT_EMISSIVE;
        if (record.asset_backed_material) info.flags |= REMIX_SOURCE_MATERIAL_ASSET_BACKED;
        if (record.patch_include_resolved) info.flags |= REMIX_SOURCE_MATERIAL_PATCH_RESOLVED;

        copy_bridge_text(info.materialName, record.canonical_name);
        copy_bridge_text(info.shaderName, record.shader_name);
        copy_bridge_text(info.surfaceProp, record.surface_prop);
        copy_bridge_text(info.providerName, "L4D2-RTX Source Compat V21.14");
        *output = info;
        g_source_bridge_query_hit_count.fetch_add(1u, std::memory_order_relaxed);
        return true;
    }

    bool material_exporter::get_source_bridge_status(RemixSourceMaterialBridgeStatusV1* output)
    {
        if (!output || output->structSize < sizeof(RemixSourceMaterialBridgeStatusV1) ||
            output->abiVersion != REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V1)
            return false;
        RemixSourceMaterialBridgeStatusV1 status{};
        status.structSize = sizeof(status);
        status.abiVersion = REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V1;
        status.enabled = m_source_material_bridge_enabled ? 1u : 0u;
        {
            std::scoped_lock lock(g_material_mutex);
            status.materialCount = static_cast<std::uint32_t>(g_materials.size());
            const auto bridge_status = std::format("{} | Studio packets: {} (latest {}).", g_status,
                g_source_bridge_draw_packet_count,
                g_source_bridge_latest_draw_packet.load(std::memory_order_relaxed));
            copy_bridge_text(status.status, bridge_status);
        }
        status.registryRevision = get_source_bridge_registry_revision();
        status.queryCount = g_source_bridge_query_count.load(std::memory_order_relaxed);
        status.queryHitCount = g_source_bridge_query_hit_count.load(std::memory_order_relaxed);
        copy_bridge_text(status.gameName, source_compat::game_name());
        *output = status;
        return true;
    }

    bool material_exporter::query_source_bridge_draw_packet(const std::uint32_t packet_id,
        RemixSourceModelDrawInfoV2* output)
    {
        if (!m_source_material_bridge_enabled || !m_model_material_packets_enabled || !output ||
            output->structSize < sizeof(RemixSourceModelDrawInfoV2) ||
            output->abiVersion != REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V2)
            return false;
        std::scoped_lock lock(g_material_mutex);
        const auto& slot = g_source_bridge_draw_packets[packet_id % k_source_bridge_draw_packet_limit];
        if (slot.packet_id != packet_id) return false;
        *output = slot.packet;
        return true;
    }

    std::uint32_t material_exporter::get_latest_source_bridge_draw_packet_id()
    {
        return g_source_bridge_latest_draw_packet.load(std::memory_order_relaxed);
    }

    std::vector<material_exporter::record_view_s> material_exporter::get_records()
    {
        std::scoped_lock lock(g_material_mutex);
        std::vector<record_view_s> out;
        out.reserve(g_materials.size());
        for (auto& [hash, record] : g_materials)
        {
            if (record.exported)
            {
                refresh_record_conversion_state(record);
                evaluate_material_activation(record);
            }
            record_view_s view;
            view.remix_hash = hash;
            view.material_name = record.canonical_name;
            view.shader_name = record.shader_name;
            view.shader_profile = record.shader_profile;
            view.texture_group = record.texture_group;
            view.surface_prop = record.surface_prop;
            view.category = record.category;
            view.identity_key = record.identity_key;
            view.export_directory = record.export_relative_dir;
            view.draw_count = record.draw_count;
            view.texture_count = static_cast<std::uint32_t>(record.texture_params.size());
            view.declared_texture_count = record.declared_texture_count;
            view.resolved_texture_asset_count = record.resolved_texture_asset_count;
            view.missing_texture_asset_count = record.missing_texture_asset_count;
            view.live_texture_param_count = record.live_texture_param_count;
            view.source_vmt_hash = record.source_vmt_hash;
            view.source_vmt_byte_size = record.source_vmt_byte_size;
            view.source_vmt_status = record.source_vmt_status;
            view.source_asset_grade = record.source_asset_grade;
            view.resolved_vmt_path = record.resolved_vmt_path;
            view.source_shader_family = record.source_shader_family;
            view.specular_semantics = record.specular_semantics;
            view.emission_semantics = record.emission_semantics;
            view.alpha_semantics = record.alpha_semantics;
            view.animation_semantics = record.animation_semantics;
            view.material_graph_summary = record.material_graph_summary;
            view.patch_semantics = record.patch_semantics;
            view.detail_semantics = record.detail_semantics;
            view.layer_semantics = record.layer_semantics;
            view.event_emissive_semantics = record.event_emissive_semantics;
            view.calibrated_specular_workflow = record.calibrated_specular_workflow;
            view.model_material_search_paths = record.model_material_search_paths;
            view.vmt_include_chain = record.vmt_include_chain;
            view.vmt_patch_warnings = record.vmt_patch_warnings;
            view.semantic_conflicts = record.semantic_conflicts;
            view.detected_proxies = record.detected_proxies;
            view.missing_source_assets = record.missing_source_assets;
            view.model_candidate_count = record.model_candidate_count;
            view.vtf_animated_count = record.vtf_animated_count;
            view.vtf_alpha_count = record.vtf_alpha_count;
            view.vtf_normal_flag_count = record.vtf_normal_flag_count;
            view.vtf_ssbump_flag_count = record.vtf_ssbump_flag_count;
            view.vtf_srgb_count = record.vtf_srgb_count;
            view.vtf_cubemap_count = record.vtf_cubemap_count;
            view.vtf_unsupported_count = record.vtf_unsupported_count;
            view.resolved_via_model_search_path = record.resolved_via_model_search_path;
            view.shader_semantics_valid = record.shader_semantics_valid;
            view.animated_or_proxy_driven = record.animated_or_proxy_driven;
            view.unlit_fullbright = record.unlit_fullbright;
            view.patch_material = record.patch_material;
            view.patch_include_resolved = record.patch_include_resolved;
            view.detail_composition_ready = record.detail_composition_ready;
            view.world_vertex_transition_preserved = record.world_vertex_transition_preserved;
            view.event_emissive_candidate = record.event_emissive_candidate;
            view.calibrated_roughness = record.calibrated_roughness;
            view.calibrated_reflection_weight = record.calibrated_reflection_weight;
            view.calibrated_dielectric_f0 = record.calibrated_dielectric_f0;
            view.calibrated_metallic_hint = record.calibrated_metallic_hint;
            view.source_vmt_resolved = record.source_vmt_resolved;
            view.source_vmt_from_vpk = record.source_vmt_from_vpk;
            view.source_albedo_asset_resolved = record.source_albedo_asset_resolved;
            view.asset_backed_material = record.asset_backed_material;
            view.roughness = record.roughness;
            view.metallic = record.metallic;
            view.has_albedo = std::any_of(record.sampler_candidates.begin(), record.sampler_candidates.end(),
                [](const auto& candidates) { return !candidates.empty(); });
            view.has_normal = record.has_normal;
            view.emissive = record.emissive;
            view.translucent = record.translucent;
            view.exported = record.exported;
            view.override_applied = record.override_applied;
            view.dual_layer = record.dual_layer;
            view.detail_texture = record.detail_texture;
            view.glass_material = record.glass_material;
            view.dynamic_material = record.dynamic_material;
            view.warning_count = static_cast<std::uint32_t>(record.warnings.size());
            view.warnings = record.warnings;

            const auto copy_channel = [](const char* name,
                const channel_export_s& source, channel_view_s& target)
            {
                target.name = name;
                target.source = source.source_parameter;
                target.declared_texture = source.source_texture_name;
                target.source_kind = source.source_kind;
                target.provenance = provenance_name(source.provenance);
                target.quality_status = source.quality.status;
                target.status = source.actual_status;
                target.confidence = source.confidence;
                target.minimum = source.quality.minimum;
                target.maximum = source.quality.maximum;
                target.mean = source.quality.mean;
                target.standard_deviation = source.quality.standard_deviation;
                target.normal_mean_length = source.quality.normal_mean_length;
                target.width = source.source_width;
                target.height = source.source_height;
                target.unique_values = source.quality.unique_values;
                target.sampler_slot = source.sampler_slot;
                target.match_score = source.match_score;
                target.ready = source.ready;
                target.generated = source.generated;
                target.derived = source.derived;
                target.constant = source.quality.constant;
                target.suspicious = source.quality.suspicious;
                target.rejected = source.rejected;
            };
            copy_channel("Albedo", record.albedo_channel, view.channels[0]);
            copy_channel("Normal", record.normal_channel, view.channels[1]);
            copy_channel("Roughness", record.roughness_channel, view.channels[2]);
            copy_channel("Metalness", record.metallic_channel, view.channels[3]);
            copy_channel("Height", record.height_channel, view.channels[4]);
            copy_channel("Emission", record.emissive_channel, view.channels[5]);
            copy_channel("Opacity", record.opacity_channel, view.channels[6]);
            copy_channel("Detail", record.detail_channel, view.channels[7]);

            view.dds_ready_count = static_cast<std::uint32_t>(record.albedo_exported) +
                static_cast<std::uint32_t>(record.normal_exported) +
                static_cast<std::uint32_t>(record.roughness_exported) +
                static_cast<std::uint32_t>(record.metallic_exported) +
                static_cast<std::uint32_t>(record.height_exported) +
                static_cast<std::uint32_t>(record.emissive_exported) +
                static_cast<std::uint32_t>(record.opacity_exported) +
                static_cast<std::uint32_t>(record.detail_exported);
            view.texconv_used = record.texconv_used;
            view.conversion_pending = record.conversion_pending;
            view.conversion_status = record.conversion_status;
            view.albedo_source = record.albedo_channel.source_parameter;
            view.normal_source = record.normal_channel.source_parameter;
            view.roughness_source = record.roughness_channel.source_parameter;
            view.metallic_source = record.metallic_channel.source_parameter;
            view.height_source = record.height_channel.source_parameter;
            view.emissive_source = record.emissive_channel.source_parameter;
            view.preview_ready_count = static_cast<std::uint32_t>(record.albedo_channel.preview_ready) +
                static_cast<std::uint32_t>(record.normal_channel.preview_ready) +
                static_cast<std::uint32_t>(record.roughness_channel.preview_ready) +
                static_cast<std::uint32_t>(record.metallic_channel.preview_ready) +
                static_cast<std::uint32_t>(record.height_channel.preview_ready) +
                static_cast<std::uint32_t>(record.emissive_channel.preview_ready) +
                static_cast<std::uint32_t>(record.opacity_channel.preview_ready) +
                static_cast<std::uint32_t>(record.detail_channel.preview_ready);
            view.observed_sampler_candidates = 0u;
            for (const auto& candidates : record.sampler_candidates)
                view.observed_sampler_candidates += static_cast<std::uint32_t>(candidates.size());
            view.water_material = record.water_material;
            view.confidence = record.confidence;
            view.material_grade = material_grade_name(record.material_grade);
            view.activation_reason = record.activation_reason;
            view.auto_activated = record.auto_activated;
            view.quarantined = record.quarantined;
            view.opacity_exported = record.opacity_exported;
            view.detail_exported = record.detail_exported;
            out.push_back(std::move(view));
        }
        return out;
    }

    std::string material_exporter::get_status()
    {
        std::scoped_lock lock(g_material_mutex);
        return g_status;
    }

    std::filesystem::path material_exporter::get_export_root()
    {
        std::scoped_lock lock(g_material_mutex);
        return export_root_unlocked();
    }

    std::filesystem::path material_exporter::get_overrides_path()
    {
        std::scoped_lock lock(g_material_mutex);
        return overrides_path_unlocked();
    }

    std::filesystem::path material_exporter::get_audit_report_path()
    {
        std::scoped_lock lock(g_material_mutex);
        return audit_report_path_unlocked();
    }
}
