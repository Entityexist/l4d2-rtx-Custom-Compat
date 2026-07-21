#pragma once

#include <array>
#include <vector>

#include "source_material_bridge_api.hpp"

namespace components
{
    class material_exporter : public component
    {
    public:
        struct channel_view_s
        {
            std::string name;
            std::string source;
            std::string declared_texture;
            std::string source_kind;
            std::string provenance;
            std::string quality_status;
            std::string status;
            float confidence = 0.0f;
            float minimum = 0.0f;
            float maximum = 0.0f;
            float mean = 0.0f;
            float standard_deviation = 0.0f;
            float normal_mean_length = 0.0f;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t unique_values = 0;
            int sampler_slot = -1;
            int match_score = 0;
            bool ready = false;
            bool generated = false;
            bool derived = false;
            bool constant = false;
            bool suspicious = false;
            bool rejected = false;
        };

        struct record_view_s
        {
            std::uint64_t remix_hash = 0;
            std::string material_name;
            std::string shader_name;
            std::string shader_profile;
            std::string texture_group;
            std::string surface_prop;
            std::string category;
            std::string identity_key;
            std::string export_directory;
            std::uint32_t draw_count = 0;
            std::uint32_t texture_count = 0;
            std::uint32_t declared_texture_count = 0;
            std::uint32_t resolved_texture_asset_count = 0;
            std::uint32_t missing_texture_asset_count = 0;
            std::uint32_t live_texture_param_count = 0;
            std::uint64_t source_vmt_hash = 0;
            std::uint64_t source_vmt_byte_size = 0;
            std::string source_vmt_status;
            std::string source_asset_grade;
            std::string resolved_vmt_path;
            std::string source_shader_family;
            std::string specular_semantics;
            std::string emission_semantics;
            std::string alpha_semantics;
            std::string animation_semantics;
            std::string material_graph_summary;
            std::string patch_semantics;
            std::string detail_semantics;
            std::string layer_semantics;
            std::string event_emissive_semantics;
            std::string calibrated_specular_workflow;
            std::vector<std::string> model_material_search_paths;
            std::vector<std::string> vmt_include_chain;
            std::vector<std::string> vmt_patch_warnings;
            std::vector<std::string> semantic_conflicts;
            std::vector<std::string> detected_proxies;
            std::vector<std::string> missing_source_assets;
            std::uint32_t model_candidate_count = 0;
            std::uint32_t vtf_animated_count = 0;
            std::uint32_t vtf_alpha_count = 0;
            std::uint32_t vtf_normal_flag_count = 0;
            std::uint32_t vtf_ssbump_flag_count = 0;
            std::uint32_t vtf_srgb_count = 0;
            std::uint32_t vtf_cubemap_count = 0;
            std::uint32_t vtf_unsupported_count = 0;
            bool resolved_via_model_search_path = false;
            bool shader_semantics_valid = false;
            bool animated_or_proxy_driven = false;
            bool unlit_fullbright = false;
            bool patch_material = false;
            bool patch_include_resolved = false;
            bool detail_composition_ready = false;
            bool world_vertex_transition_preserved = false;
            bool event_emissive_candidate = false;
            float calibrated_roughness = 0.5f;
            float calibrated_reflection_weight = 0.0f;
            float calibrated_dielectric_f0 = 0.04f;
            float calibrated_metallic_hint = 0.0f;
            bool source_vmt_resolved = false;
            bool source_vmt_from_vpk = false;
            bool source_albedo_asset_resolved = false;
            bool asset_backed_material = false;
            float roughness = 0.5f;
            float metallic = 0.0f;
            bool has_albedo = false;
            bool has_normal = false;
            bool emissive = false;
            bool translucent = false;
            bool exported = false;
            bool override_applied = false;
            bool dual_layer = false;
            bool detail_texture = false;
            bool glass_material = false;
            bool dynamic_material = false;
            std::uint32_t warning_count = 0;
            std::vector<std::string> warnings;
            std::array<channel_view_s, 8> channels{};
            std::uint32_t dds_ready_count = 0;
            bool texconv_used = false;
            bool conversion_pending = false;
            std::string conversion_status;
            std::string albedo_source;
            std::string normal_source;
            std::string roughness_source;
            std::string metallic_source;
            std::string height_source;
            std::string emissive_source;
            std::uint32_t preview_ready_count = 0;
            std::uint32_t observed_sampler_candidates = 0;
            bool water_material = false;
            float confidence = 0.0f;
            std::string material_grade;
            std::string activation_reason;
            bool auto_activated = false;
            bool quarantined = false;
            bool opacity_exported = false;
            bool detail_exported = false;
        };

        material_exporter();
        ~material_exporter();

        static inline material_exporter* p_this = nullptr;
        static material_exporter* get() { return p_this; }

        // Called once for every Source material render pass. Returns a stable 64-bit
        // material hash suitable for RS150/RS153 in the matching Remix runtime.
        static std::uint64_t capture_draw(IMaterialInternal* material,
            IShaderAPIDX8* shader_api, const BufferedState_t& state);

        // DrawModelExecute supplies model/skin/body context for the material passes
        // issued inside the original Source model render call. The bridge publishes
        // a bounded packet and passes its id through RS214 for the matching draw.
        static void begin_model_draw(const ModelRenderInfo_t& info);
        static void end_model_draw();
        static std::uint32_t current_model_draw_packet_id();
        static std::uint32_t current_model_draw_packet_flags();

        // Registers model material names together with the search paths compiled from
        // QC $cdmaterials. The resolver uses these candidates when an IMaterial name
        // is only a mesh-local basename rather than a canonical materials/... path.
        static void register_model_material_paths(const studiohdr_t* studio_header);

        static void on_map_load(const char* map_name);
        static void begin_capture();
        static void clear_registry();
        static bool export_all();
        static bool export_material(std::uint64_t hash);
        static bool rescan_source_assets();
        static bool rescan_source_asset(std::uint64_t hash);
        static bool rebuild_usda();
        static bool initialize_legacy_mod();
        static bool reload_overrides();
        static bool write_override_template();
        static bool write_audit_report();
        static bool query_event_emissive_material(const std::string& material_name,
            std::string* reason = nullptr);
        static std::size_t get_override_count();
        static std::size_t get_installed_material_count();
        static std::vector<record_view_s> get_records();
        // Lightweight runtime metadata registry consumed by the optional DXVK
        // Source Material Bridge. No DDS export or CPU texture readback occurs here.
        static std::uint64_t get_source_bridge_registry_revision();
        static bool query_source_bridge_material(std::uint64_t hash, RemixSourceMaterialInfoV1* output);
        static bool get_source_bridge_status(RemixSourceMaterialBridgeStatusV1* output);
        static bool query_source_bridge_draw_packet(std::uint32_t packet_id, RemixSourceModelDrawInfoV2* output);
        static std::uint32_t get_latest_source_bridge_draw_packet_id();
        static std::string get_status();
        static std::filesystem::path get_export_root();
        static std::filesystem::path get_overrides_path();
        static std::filesystem::path get_audit_report_path();

        static inline bool m_capture_enabled = false;
        // OFF by default: ordinary Toolkit captures and existing replacements are keyed
        // by the native albedo texture hash. Enable only when authoring a mod that
        // intentionally uses the Source VMT stable hashes emitted by this exporter.
        static inline bool m_inject_stable_hashes = false;
        // Source Engine -> DXVK metadata bridge. Enabled by default in V21.14 so
        // world and Studio model draws share one material registry. Native capture
        // identity remains available by disabling publication and stable hashes.
        static inline bool m_source_material_bridge_enabled = true;
        static inline bool m_model_material_packets_enabled = true;
        static inline bool m_auto_export_new_materials = false;
        static inline bool m_export_raw_vtf = true;
        static inline bool m_export_runtime_dds = true;
        static inline bool m_dump_all_bound_slots = false;
        static inline bool m_prefer_texconv = true;
        static inline bool m_background_bc_conversion = false;
        static inline bool m_invert_normal_y = false;
        static inline bool m_generate_mask_textures = true;
        static inline bool m_export_debug_png = true;
        static inline bool m_generate_roughness_from_albedo = true;
        static inline bool m_generate_ssbump_height = false;
        static inline bool m_generate_water_albedo = true;
        static inline bool m_overwrite_existing_dds = true;
        static inline bool m_enable_quality_gate = true;
        static inline bool m_reject_suspicious_native_maps = true;
        static inline bool m_apply_material_overrides = true;
        static inline bool m_write_html_audit = true;
        static inline bool m_clean_export_before_export = true;
        static inline bool m_include_ui_materials = false;
        static inline bool m_include_error_materials = false;
        static inline bool m_export_opacity_masks = true;
        static inline bool m_export_detail_authoring_assets = true;
        static inline bool m_quarantine_low_confidence = true;
        static inline bool m_auto_activate_review_materials = false;
        // V21.8 Source-semantic PBR policy. When enabled, automatic material
        // activation requires a real Source VMT plus a resolvable base VTF from
        // loose files or mounted VPKs. Runtime sampler captures remain available
        // as diagnostics/fallback authoring data but no longer masquerade as a
        // fully asset-backed material.
        static inline bool m_resolve_source_assets_during_capture = true;
        static inline bool m_require_asset_backed_activation = true;
        static inline bool m_allow_live_sampler_fallback = true;
        static inline bool m_rescan_unresolved_assets_periodically = true;
        static inline bool m_enable_source_shader_semantics = true;
        static inline bool m_resolve_model_cdmaterials = true;
        static inline bool m_inspect_vtf_metadata = true;
        static inline bool m_preserve_dynamic_materials_for_review = true;
        static inline bool m_allow_envmapmask_conflict_fallback = false;
        static inline bool m_use_detail_as_pbr_microdetail = true;
        static inline bool m_export_semantic_manifest = true;
        static inline bool m_enable_patch_vmt_resolution = true;
        static inline bool m_enable_calibrated_specular = true;
        static inline bool m_compose_detail_layers = true;
        static inline bool m_preserve_world_vertex_transition = true;
        static inline bool m_link_event_emissive_materials = true;
        static inline float m_minimum_auto_activation_confidence = 0.62f;
    };
}
