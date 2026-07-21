#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace components::source_material_graph
{
    enum class operation_mode
    {
        assign,
        insert_if_missing,
        replace
    };

    struct vmt_operation
    {
        operation_mode mode = operation_mode::assign;
        std::string key;
        std::string value;
    };

    struct vmt_document
    {
        std::string shader;
        bool patch = false;
        std::string include_path;
        std::vector<vmt_operation> operations;
        std::vector<std::string> proxies;
        std::vector<std::string> warnings;
    };

    vmt_document parse_vmt(std::string_view text);
    void apply_operations(std::map<std::string, std::string>& effective_values,
        const vmt_document& document);

    enum class detail_role
    {
        none,
        microdetail_albedo,
        color_overlay,
        emissive,
        dual_pattern,
        ao_modulation,
        ssbump_ao,
        unsupported
    };

    const char* detail_role_name(detail_role role);
    detail_role classify_l4d2_detail_mode(int mode);

    struct specular_input
    {
        bool phong_enabled = false;
        bool envmap_enabled = false;
        bool has_mask = false;
        bool exponent_texture = false;
        bool known_metal = false;
        float phong_exponent = 16.0f;
        float phong_boost = 1.0f;
        float envmap_contrast = 0.0f;
        float envmap_saturation = 1.0f;
        float envmap_tint_luminance = 1.0f;
    };

    struct specular_result
    {
        float roughness = 0.5f;
        float reflection_weight = 0.0f;
        float dielectric_f0 = 0.04f;
        float metallic_hint = 0.0f;
        std::string workflow;
    };

    specular_result calibrate_specular(const specular_input& input);

    struct emissive_link_result
    {
        bool candidate = false;
        std::string reason;
    };

    emissive_link_result classify_event_emissive_candidate(
        std::string_view material_name,
        bool emission_enabled,
        const std::vector<std::string>& proxies);
}
