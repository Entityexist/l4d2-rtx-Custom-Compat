#pragma once
#include "map_settings.hpp"
#include "source_map_entities.hpp"

namespace components
{
	class dynamic_lighting : public component
	{
	public:
		dynamic_lighting();
		~dynamic_lighting() = default;

		static inline dynamic_lighting* p_this = nullptr;
		static dynamic_lighting* get() { return p_this; }

		static void on_map_load(const char* map_name);
		static void on_client_frame();
		static void on_sound_start(std::uint32_t hash, const std::string& sound_name, const Vector& origin);
		static void on_choreo_start(const std::string_view& name, const std::string_view& actor, const std::string_view& event, const std::string_view& param1);

		static void spawn_test_event(const char* preset, const char* animation);
		static void test_muzzle_flash();
		static bool spawn_anchor_preview(const map_settings::light_anchor_s& anchor);
		static std::string build_light_anchor_toml(const map_settings::light_anchor_s& anchor);
		static std::string build_anchor_event_toml(const std::string& anchor_name, const std::string& trigger_body, float duration, float cooldown, bool loop);
		static bool append_anchor_export(const map_settings::light_anchor_s& anchor, const std::string& event_toml);
		static bool auto_muzzle_flash_enabled() { return m_auto_muzzle_flash; }

		static inline bool m_small_radius_mode = true;
		static inline float m_small_radius_max = 16.0f;

		// Compatibility/performance profiles. These do not try to remove Remix/Source limits;
		// they keep the single-threaded compat path from doing too much work per frame.
		// 0 = Authoring, 1 = Balanced, 2 = Performance, 3 = Ultra Performance.
		static inline int m_compat_profile = 1;
		static inline bool m_runtime_budgets_enabled = true;
		static inline bool m_authoring_debug_tools = false;
		static inline std::uint32_t m_runtime_max_active_lights = 48u;
		static inline std::uint32_t m_runtime_max_pending_lights = 48u;
		static inline std::uint32_t m_runtime_max_spawns_per_second = 24u;
		static inline std::uint32_t m_runtime_max_muzzle_per_second = 24u;
		static inline std::uint32_t m_runtime_max_sound_hash_per_second = 4u;
		static inline float m_runtime_leaf_check_hz = 20.0f;
		static inline std::uint32_t m_runtime_skipped_budget = 0u;
		static inline std::uint32_t m_runtime_skipped_active_limit = 0u;
		static inline std::uint32_t m_runtime_skipped_pending_limit = 0u;
		static inline std::uint32_t m_runtime_skipped_muzzle_budget = 0u;
		static inline std::uint32_t m_runtime_skipped_hash_budget = 0u;
		static inline std::uint32_t m_runtime_spawned_this_second = 0u;
		static inline std::uint32_t m_runtime_muzzle_this_second = 0u;
		static inline std::uint32_t m_runtime_hash_this_second = 0u;
		static inline std::uint32_t m_runtime_active_lights_snapshot = 0u;

		// Retained for configuration-file compatibility with V21.1/V21.2. V21.2.2
		// restores the original per-frame flashlight lifecycle; these governor fields
		// are parsed and preserved but are not used to throttle or retain light handles.
		static inline bool m_flashlight_governor_enabled = false;
		static inline float m_flashlight_update_hz = 60.0f;
		static inline float m_flashlight_motion_epsilon = 0.10f;
		static inline float m_flashlight_direction_epsilon_degrees = 0.10f;
		static inline float m_flashlight_force_update_distance = 32.0f;
		static inline std::uint32_t m_flashlight_player_layer_limit = 4u;
		static inline std::uint32_t m_flashlight_bot_layer_limit = 2u;
		static inline std::uint32_t m_flashlight_budget_reserve = 6u;
		static inline bool m_flashlight_cull_distant_bots = true;
		static inline float m_flashlight_bot_cull_distance = 2200.0f;
		static inline bool m_flashlight_nearest_bot_priority = true;
		static inline bool m_flashlight_preserve_last_good_rig = true;
		static inline bool m_flashlight_verify_draw_results = false;
		static inline std::uint32_t m_flashlight_owner_grace_frames = 3u;
		static inline std::uint32_t m_flashlight_retry_base_ms = 100u;
		static inline std::uint32_t m_flashlight_retry_max_ms = 2000u;

		// CPU-skinning mitigation stub. This is intentionally conservative and optional:
		// it can skip far common infected / ragdoll-style animated meshes in performance profiles.
		static inline bool m_cpu_skin_throttle_enabled = false;
		static inline bool m_cpu_skin_skip_far_common = false;
		static inline bool m_cpu_skin_skip_far_ragdolls = false;
		static inline float m_cpu_skin_common_skip_distance = 1800.0f;
		static inline float m_cpu_skin_ragdoll_skip_distance = 1400.0f;
		static inline std::uint32_t m_cpu_skin_skipped_common = 0u;
		static inline std::uint32_t m_cpu_skin_skipped_ragdoll = 0u;

		static void apply_compat_profile(int profile);
		static const char* get_compat_profile_name(int profile = -1);
		static void apply_source_runtime_light_profile(int profile);
		static const char* get_source_runtime_light_profile_name(int profile = -1);
		static std::string get_event_light_summary();
		static std::string get_facing_poly_link_report();
		static void reset_runtime_budget_counters();
		static bool should_skip_model_for_cpu_skin_budget(const ModelRenderInfo_t& info);

		static inline bool m_sound_hash_library_enabled = false;
		static inline bool m_sound_hash_fire_enabled = true;
		static inline bool m_sound_hash_explosion_enabled = true;
		static inline bool m_sound_hash_energy_enabled = true;
		static inline bool m_sound_hash_weather_enabled = true;
		static inline bool m_sound_hash_alarm_enabled = false;
		static inline bool m_sound_hash_use_camera_for_zero_origin = true;
		static inline bool m_sound_hash_reject_near_player_origin = true;
		static inline float m_sound_hash_reject_near_player_distance = 160.0f;
		static inline float m_sound_hash_radius_scale = 0.020f;
		static inline float m_sound_hash_min_radius = 0.25f;
		static inline float m_sound_hash_max_radius = 12.0f;
		static inline bool m_sound_hash_draw_debug = false;
		static inline std::uint32_t m_sound_hash_library_matches = 0u;
		static inline std::uint32_t m_sound_hash_library_spawned = 0u;
		static inline std::uint32_t m_sound_hash_library_skipped = 0u;
		static inline std::uint32_t m_sound_hash_library_skipped_player_origin = 0u;
		static inline std::string m_sound_hash_last_category = "none";
		static inline std::string m_sound_hash_last_sound = "none";
		static inline std::uint32_t m_sound_hash_last_hash = 0u;
		static inline Vector m_sound_hash_last_origin = Vector(0.0f, 0.0f, 0.0f);

		static inline bool m_auto_muzzle_flash = true;
		static inline int m_muzzle_flash_origin_mode = 0; // 0 = player flashlight, 1 = camera/view, 2 = sound origin, 3 = sound origin + eye height
		static inline int m_muzzle_flash_shape_mode = 0; // 0 = sphere, 1 = cone/spot shaping
		static inline int m_muzzle_weapon_profile_mode = 0; // 0 = auto, 1.. = forced weapon profile
		static inline bool m_muzzle_weapon_profiles_enabled = true;
		static inline float m_muzzle_flash_scalar = 0.07f;
		static inline float m_muzzle_flash_radius = 0.02f;
		static inline float m_muzzle_flash_duration = 0.300f;
		static inline float m_muzzle_flash_fade = 0.090f;
		static inline float m_muzzle_flash_delay = 0.000f;
		static inline float m_muzzle_flash_cooldown = 0.018f;
		static inline Vector m_muzzle_flash_color = Vector(1.0f, 0.68f, 0.30f);
		static inline Vector m_muzzle_flash_offset = Vector(18.0f, -2.0f, -3.0f); // F/H/V: forward, height/up, right
		static inline bool m_muzzle_draw_debug = false;
		// V21.13.2 uses a balanced classifier by default. Explicit fire/gunfire tokens always win
		// over ambiguous handling words (for example pump_shotgun_fire), while strict mode remains
		// available for authoring and only disables the guarded weapon-family fallback.
		static inline bool m_muzzle_strict_fire_tokens = false;
		static inline bool m_muzzle_reject_weapon_handling = true;
		static inline bool m_muzzle_allow_weapon_family_fallback = true;
		static inline std::uint32_t m_muzzle_candidate_sounds = 0u;
		static inline std::uint32_t m_muzzle_accepted_sounds = 0u;
		static inline std::uint32_t m_muzzle_rejected_non_fire = 0u;
		static inline std::uint32_t m_muzzle_rejected_hard = 0u;
		static inline std::uint32_t m_muzzle_rejected_strict = 0u;
		static inline std::uint32_t m_muzzle_skipped_cooldown = 0u;
		static inline std::uint32_t m_muzzle_skipped_budget = 0u;
		static inline std::uint32_t m_muzzle_spawn_attempts = 0u;
		static inline std::string m_muzzle_last_accept_reason = "none";
		static inline std::string m_muzzle_last_reject_reason = "none";
		static inline std::string m_muzzle_last_profile = "none";
		static inline std::string m_muzzle_last_sound = "none";
		static inline Vector m_muzzle_last_origin = Vector(0.0f, 0.0f, 0.0f);
		static inline Vector m_muzzle_last_forward = Vector(0.0f, 1.0f, 0.0f);

		struct bsp_light_candidate_s
		{
			std::uint32_t source_index = 0u;
			std::string classname;
			std::string targetname;
			std::string comment;
			Vector origin = Vector(0.0f, 0.0f, 0.0f);
			Vector radiance = Vector(1.0f, 0.85f, 0.55f);
			float scalar = 1.0f;
			float radius = 1.0f;
			bool shaped = false;
			Vector direction = Vector(0.0f, 0.0f, -1.0f);
			float degrees = 180.0f;
			float softness = 0.0f;
			float exponent = 0.0f;
			bool distant = false;
			float distant_angular_diameter = 0.53f;
			std::string distant_source;
			bool selected = true;
			bool from_worldlight = false;
			bool hdr_worldlight = false;
			std::int32_t source_type = -1;
			std::int32_t style = 0;
			std::int32_t flags = 0;
			std::int32_t owner = -1;
			std::int32_t texinfo = -1;
			std::string surface_material_name;
			bool surface_texinfo_linked = false;
			bool surface_event_proxy = false;
			bool surface_pbr_emissive_linked = false;
			std::string surface_pbr_emissive_reason;
			std::string surface_link_reason;
			Vector owner_local_origin = Vector(0.0f, 0.0f, 0.0f);
			Vector owner_local_direction = Vector(0.0f, 0.0f, -1.0f);
			bool owner_relative = false;
			std::int32_t map_light_entity_index = -1;
			std::int32_t map_owner_entity_index = -1;
			std::int32_t map_hammer_id = -1;
			std::string map_targetname;
			std::string map_light_classname;
			Vector map_light_origin = Vector(0.0f, 0.0f, 0.0f);
			bool map_light_starts_disabled = false;
			bool map_light_runtime_trackable = false;
			std::uint32_t map_light_control_group = 0u;
			bool map_light_io_killable = false;
			bool map_light_io_group_kill_all = false;
			std::string map_light_control_summary;
			std::string map_parentname;
			std::string map_owner_classname;
			std::string map_owner_targetname;
			std::string map_owner_model;
			std::string map_owner_attachment;
			std::int32_t map_owner_hammer_id = -1;
			std::int32_t map_owner_initial_health = -1;
			bool map_owner_breakable = false;
			std::string binding_reason;
			Vector map_owner_origin = Vector(0.0f, 0.0f, 0.0f);
			Vector map_owner_angles = Vector(0.0f, 0.0f, 0.0f);
			bool graph_matched = false;
			bool graph_owner_bound = false;
			bool origin_extruded = false;
			bool near_camera = false;
			float camera_distance = 0.0f;
			bool previously_streamed = false;
			bool override_applied = false;
			bool override_disabled = false;
			std::string override_summary;
			bool surface_cluster = false;
			int surface_cluster_samples = 0;
			float surface_cluster_spread = 0.65f;
			float surface_cluster_aspect = 1.0f;
			float surface_cluster_intensity = 0.30f;
			float surface_cluster_radius_scale = 0.45f;
			std::string surface_cluster_pattern = "cross";

			// Snapshot used by the in-game editor. It represents the last value loaded from
			// the map/override file, so unsaved edits can be reverted without rescanning the BSP.
			bool editor_baseline_valid = false;
			bool editor_selected = true;
			Vector editor_origin = Vector(0.0f, 0.0f, 0.0f);
			Vector editor_radiance = Vector(1.0f, 1.0f, 1.0f);
			float editor_scalar = 1.0f;
			float editor_radius = 1.0f;
			bool editor_shaped = false;
			Vector editor_direction = Vector(0.0f, 0.0f, -1.0f);
			float editor_degrees = 180.0f;
			float editor_softness = 0.0f;
			float editor_exponent = 0.0f;
			float editor_distant_angular_diameter = 0.53f;
			bool editor_surface_cluster = false;
			int editor_surface_cluster_samples = 0;
			float editor_surface_cluster_spread = 0.65f;
			float editor_surface_cluster_aspect = 1.0f;
			float editor_surface_cluster_intensity = 0.30f;
			float editor_surface_cluster_radius_scale = 0.45f;
			std::string editor_surface_cluster_pattern = "cross";
		};

		static inline bool m_source_bsp_scan_spawn_helpers = false;
		static inline bool m_source_bsp_scan_clear_previous_preview = true;
		static inline bool m_source_bsp_scan_preview_near_camera = true;
		static inline bool m_source_bsp_scan_include_point = true;
		static inline bool m_source_bsp_scan_include_spot = true;
		static inline bool m_source_bsp_scan_include_projected = true;
		static inline bool m_source_bsp_scan_include_sprite = true;
		static inline float m_source_bsp_scan_scalar_scale = 1.0f;
		static inline float m_source_bsp_scan_radius_scale = 1.0f;
		static inline float m_source_bsp_scan_min_scalar = 0.25f;
		static inline float m_source_bsp_scan_max_scalar = 120.0f;
		static inline bool m_source_bsp_preview_force_sphere = true;
		static inline bool m_source_bsp_preview_boost = true;
		static inline float m_source_bsp_preview_boost_scalar = 4.0f;
		static inline float m_source_bsp_preview_boost_radius = 1.75f;
		static inline int m_source_bsp_scan_preview_limit = 96;
		static inline float m_source_bsp_scan_preview_max_distance = 2400.0f;
		static inline std::uint32_t m_source_bsp_preview_generation = 0u;
		static inline std::uint32_t m_source_bsp_preview_live_count = 0u;
		static inline std::string m_source_bsp_scan_status = "not scanned";
		static inline std::uint32_t m_source_bsp_scan_entities = 0u;
		static inline std::uint32_t m_source_bsp_scan_lights = 0u;
		static inline std::uint32_t m_source_bsp_scan_previewed = 0u;
		static inline std::uint32_t m_source_bsp_scan_skipped_by_filter = 0u;
		static inline std::uint32_t m_source_bsp_scan_skipped_by_distance = 0u;
		static inline std::uint32_t m_source_bsp_preview_create_success = 0u;
		static inline std::uint32_t m_source_bsp_preview_create_failed = 0u;
		static inline std::string m_source_bsp_scan_output_path;
		static inline std::vector<bsp_light_candidate_s> m_source_bsp_candidates = {};
		static inline int m_source_bsp_selected_index = -1;
		static inline bool m_source_bsp_import_selected_only = true;
		static inline bool m_source_bsp_import_near_camera_only = false;
		static inline int m_source_bsp_import_limit = 64;
		static inline std::string m_source_bsp_import_status = "not imported";
		static bool scan_current_bsp_light_entities();
		static void clear_bsp_preview_lights();
		static std::uint32_t get_selected_bsp_candidate_count();
		static std::uint32_t get_near_camera_bsp_candidate_count();
		static void select_all_bsp_candidates(bool selected);
		static void select_near_camera_bsp_candidates(bool selected);
		static void select_nearest_bsp_candidate_to_camera();
		static bool preview_bsp_candidate(int index);
		static bool preview_selected_bsp_candidate();
		static bool spawn_bsp_camera_probe_light();
		static std::string build_bsp_candidate_light_toml(const bsp_light_candidate_s& candidate);
		static std::string build_bsp_import_toml(bool selected_only, bool near_camera_only, std::uint32_t limit);
		static bool append_bsp_import_export(bool selected_only, bool near_camera_only, std::uint32_t limit);

		// Native Source BSP WORLDLIGHTS importer. Unlike the entity-lump workbench scanner,
		// this reads the post-VRAD light records from LUMP_WORLDLIGHTS_HDR / LUMP_WORLDLIGHTS.
		static inline bool m_bsp_worldlight_auto_import = true;
		static inline bool m_bsp_worldlight_prefer_engine_memory = true;
		static inline bool m_bsp_worldlight_prefer_hdr = true;
		static inline bool m_bsp_worldlight_near_camera_only = true;
		static inline bool m_bsp_worldlight_stream_nearby = true;
		static inline float m_bsp_worldlight_stream_interval = 2.0f;
		static inline float m_bsp_worldlight_stream_move_threshold = 1000.0f;
		static inline bool m_bsp_worldlight_prefer_previous_stream_set = true;
		static inline float m_bsp_worldlight_stream_hysteresis = 700.0f;
		static inline bool m_bsp_worldlight_include_surface = true;
		static inline bool m_bsp_worldlight_include_point = true;
		static inline bool m_bsp_worldlight_include_spot = true;
		static inline bool m_bsp_worldlight_include_quake = true;
		// Preserve light_environment / skyambient records in the Source Direct registry.
		// They stay disabled as local Remix helpers by default because a sphere is not a
		// physically correct replacement for a directional/environment light.
		static inline bool m_bsp_worldlight_include_environment_records = true;
		static inline bool m_bsp_worldlight_enable_environment_helpers = false;

		// V20.3 Source environment resolver. VRAD treats the first light_environment as a
		// directional skylight plus a separate skyambient source. The resolver mirrors those
		// rules, compares HDR/LDR compiled sets, matches the canonical entity to its skylight
		// record, and creates exactly one native Remix Distant light.
		static inline bool m_source_distant_light_import = true;
		static inline bool m_source_distant_prefer_hdr = true;
		static inline bool m_source_distant_use_worldlight_direction = true;
		static inline bool m_source_distant_allow_entity_fallback = true;
		static inline bool m_source_distant_compare_bsp_sets = true;
		static inline bool m_source_distant_follow_vrad_first_environment = true;
		static inline bool m_source_distant_match_worldlight_to_entity = true;
		static inline bool m_source_distant_use_target_direction = true;
		static inline bool m_source_distant_linearize_entity_color = true;
		static inline bool m_source_distant_spread_is_radius = true;
		static inline bool m_source_environment_use_shadow_control = true;
		static inline bool m_source_environment_use_env_sun = true;
		static inline bool m_source_environment_use_env_cascade = true;
		static inline bool m_source_environment_force_direction_only = true;
		// 0 = automatic Source/VRAD propagation convention, 1 = trust normal, 2 = reverse.
		static inline int m_source_distant_direction_mode = 0;
		static inline float m_source_distant_intensity_scale = 96.0f;
		static inline float m_source_distant_default_angular_diameter = 0.53f;
		static inline float m_source_distant_min_angular_diameter = 0.01f;
		static inline float m_source_distant_max_angular_diameter = 180.0f;
		static inline float m_source_distant_max_pair_angle = 75.0f;
		static inline float m_source_distant_sign_flip_min_improvement = 8.0f;
		static inline float m_source_distant_min_confidence = 0.25f;
		static inline float m_source_environment_default_strength = 1.0f;
		static inline std::uint32_t m_source_distant_entities_found = 0u;
		static inline std::uint32_t m_source_environment_light_environment_found = 0u;
		static inline std::uint32_t m_source_environment_light_directional_found = 0u;
		static inline std::uint32_t m_source_environment_cascade_found = 0u;
		static inline std::uint32_t m_source_environment_shadow_control_found = 0u;
		static inline std::uint32_t m_source_environment_env_sun_found = 0u;
		static inline std::uint32_t m_source_distant_worldlights_found = 0u;
		static inline std::uint32_t m_source_distant_skyambient_found = 0u;
		static inline std::uint32_t m_source_distant_hdr_sets_found = 0u;
		static inline std::uint32_t m_source_distant_ldr_sets_found = 0u;
		static inline std::uint32_t m_source_distant_candidates_rejected = 0u;
		static inline float m_source_distant_selected_confidence = 0.0f;
		static inline float m_source_distant_direction_disagreement = 0.0f;
		static inline bool m_source_distant_selected_hdr = false;
		static inline bool m_source_distant_sign_flipped = false;
		static inline bool m_source_distant_detected = false;
		static inline bool m_source_distant_imported = false;
		static inline std::string m_source_distant_status = "waiting for map environment";
		static inline std::string m_source_distant_diagnostics;
		static inline std::string m_source_environment_selected_alias = "none";

		static inline bool m_bsp_worldlight_include_styled = true;
		static inline bool m_bsp_worldlight_merge_duplicates = true;
		static inline bool m_bsp_worldlight_surface_clusters = true;
		static inline int m_bsp_worldlight_surface_cluster_samples = 4;
		static inline float m_bsp_worldlight_surface_cluster_spread = 0.65f;
		static inline float m_bsp_worldlight_surface_cluster_aspect = 1.0f;
		static inline float m_bsp_worldlight_surface_cluster_intensity = 0.30f;
		static inline float m_bsp_worldlight_surface_cluster_radius_scale = 0.45f;
		static inline bool m_map_light_overrides_enabled = true;
		static inline bool m_map_light_auto_save_config = true;
		static inline bool m_map_light_auto_sync_editor = true;
		static inline bool m_persistent_map_light_database = true;
		static inline bool m_persistent_map_light_auto_save = true;
		static inline float m_persistent_map_light_save_delay = 0.75f;
		static inline std::string m_persistent_map_light_status = "waiting for map settings";
		static inline std::uint32_t m_persistent_map_light_loaded = 0u;
		static inline std::uint32_t m_persistent_map_light_materialized = 0u;
		static inline std::uint32_t m_persistent_map_light_live_linked = 0u;
		static inline std::uint32_t m_persistent_map_light_tombstones = 0u;
		static inline std::uint32_t m_persistent_map_light_autosaves = 0u;
		static inline int m_bsp_worldlight_import_limit = 192;
		static inline float m_bsp_worldlight_max_distance = 3500.0f;
		static inline float m_bsp_worldlight_intensity_scale = 96.0f;
		static inline float m_bsp_worldlight_radius_scale = 0.0078125f;
		static inline float m_bsp_worldlight_min_intensity = 0.01f;
		static inline float m_bsp_worldlight_min_radius = 0.35f;
		static inline float m_bsp_worldlight_max_radius = 14.0f;
		static inline std::string m_bsp_worldlight_status = "not loaded";
		static inline std::string m_bsp_worldlight_backend = "none";
		static inline std::string m_bsp_worldlight_diagnostics;
		static inline std::string m_bsp_worldlight_source;
		static inline std::string m_map_light_override_path;
		static inline std::string m_map_light_override_status = "not loaded";
		static inline std::uint32_t m_bsp_worldlight_records = 0u;
		static inline std::uint32_t m_bsp_worldlight_candidates = 0u;
		static inline std::uint32_t m_bsp_worldlight_imported = 0u;
		static inline std::uint32_t m_bsp_worldlight_untracked_active = 0u;
		static inline std::uint32_t m_bsp_worldlight_create_failed = 0u;
		static inline std::uint32_t m_bsp_worldlight_skipped_type = 0u;
		static inline std::uint32_t m_bsp_worldlight_skipped_style = 0u;
		static inline std::uint32_t m_bsp_worldlight_skipped_intensity = 0u;
		static inline std::uint32_t m_bsp_worldlight_skipped_distance = 0u;
		static inline std::uint32_t m_bsp_worldlight_merged_duplicates = 0u;
		static inline std::uint32_t m_bsp_worldlight_stream_refreshes = 0u;
		static inline std::uint32_t m_bsp_worldlight_stream_retained = 0u;
		static inline std::uint32_t m_bsp_worldlight_stream_evicted = 0u;
		static inline std::uint32_t m_bsp_worldlight_surface_clustered = 0u;
		static inline std::uint32_t m_map_light_overrides_loaded = 0u;
		static inline std::uint32_t m_map_light_overrides_applied = 0u;
		static inline std::uint32_t m_map_light_overrides_disabled = 0u;
		static inline std::uint32_t m_map_light_override_invalid_lines = 0u;
		static bool scan_current_bsp_world_lights();
		static bool import_scanned_bsp_world_lights();
		static void clear_bsp_worldlight_lights();
		static bool reload_map_light_overrides();
		static bool reload_current_map_lights();
		static bool apply_selected_map_light_edits();
		static bool save_selected_map_light_edits();
		static bool save_all_map_lights_to_config();
		static bool sync_imported_map_lights_to_light_editor();
		static bool save_light_editor_map_lights_to_config();
		static bool load_persistent_map_lights_into_settings();
		static bool reload_persistent_map_lights_from_file();
		static void notify_light_editor_changed();
		static std::uint32_t get_light_editor_imported_count();
		static bool rotate_selected_map_light_direction(int axis, float degrees, bool apply_to_all_spots);
		static bool reset_selected_map_light_edits();
		static bool remove_selected_map_light_override();
		static std::string get_selected_map_light_runtime_details();
		static std::string get_selected_map_light_io_group_details();
		static std::string get_selected_map_light_io_scheduler_details();
		static bool simulate_selected_map_light_io(source_map_entities::io_action_kind kind);
		static std::string get_map_light_transition_log_text();
		static void clear_map_light_transition_log();
		static const std::string& get_bsp_worldlight_pending_map() { return m_bsp_worldlight_pending_map; }
		static std::uint32_t get_bsp_worldlight_auto_import_retries() { return m_bsp_worldlight_auto_import_retries; }
		static std::uint32_t get_bsp_worldlight_session_generation() { return m_bsp_worldlight_session_generation; }

		// Runtime Source light import. VEngineEffects supplies the active dlight lifetime,
		// while the client-entity pass catches env_projectedtexture and owner-bound map lights.
		static inline bool m_map_light_runtime_tracking = true;
		static inline bool m_source_dlight_import = true;
		static inline bool m_source_alloc_hooks_enabled = true;
		static inline bool m_source_projectedtexture_import = true;
		// V21.6 policy layer: Source runtime lights are classified before submission so
		// character helper effects cannot masquerade as authored map illumination.
		static inline int m_source_runtime_profile = 1; // 0 raw, 1 balanced, 2 world-only, 3 diagnostics
		static inline bool m_source_runtime_allow_world = true;
		static inline bool m_source_runtime_allow_survivors = false;
		static inline bool m_source_runtime_allow_infected = false;
		static inline bool m_source_runtime_allow_unknown_characters = false;
		static inline bool m_source_runtime_allow_dlights = true;
		static inline bool m_source_runtime_allow_elights = true;
		static inline bool m_source_runtime_allow_entity_dynamic = true;
		static inline bool m_source_runtime_allow_projected = true;
		static inline bool m_source_runtime_allow_point_spotlight = true;
		static inline float m_source_runtime_world_intensity_scale = 1.0f;
		static inline float m_source_runtime_character_intensity_scale = 0.20f;
		static inline float m_source_runtime_world_radius_scale = 1.0f;
		static inline float m_source_runtime_character_radius_scale = 0.25f;
		static inline float m_source_runtime_max_radius = 8.0f;
		static inline float m_source_runtime_character_max_radius = 1.25f;
		static inline float m_source_runtime_character_spot_max_angle = 55.0f;
		static inline bool m_source_runtime_show_rejected_in_registry = true;
		static inline std::uint32_t m_source_runtime_rejected_survivor = 0u;
		static inline std::uint32_t m_source_runtime_rejected_infected = 0u;
		static inline std::uint32_t m_source_runtime_rejected_unknown_character = 0u;
		static inline std::uint32_t m_source_runtime_rejected_class = 0u;
		static inline std::uint32_t m_source_runtime_world_accepted = 0u;
		static inline std::uint32_t m_source_runtime_character_accepted = 0u;
		static inline std::string m_source_runtime_last_policy = "waiting for Source runtime light";

		// Event-driven map lights: this high-level policy gates the already existing
		// entity graph, AcceptInput capture, lightstyle animation and scheduled I/O.
		static inline bool m_map_event_lights_enabled = true;
		static inline bool m_map_event_accept_input = true;
		static inline bool m_map_event_follow_lightstyles = true;
		static inline bool m_map_event_include_sprite_proxies = true;
		static inline bool m_map_event_honor_starts_disabled = true;
		static inline bool m_map_event_surface_material_linkage = true;
		static inline std::uint32_t m_map_event_controlled_candidates = 0u;
		static inline std::uint32_t m_map_event_sprite_candidates = 0u;
		static inline std::uint32_t m_map_event_styled_candidates = 0u;
		static inline std::uint32_t m_surface_material_linked = 0u;
		static inline std::uint32_t m_surface_material_unresolved = 0u;
		static inline std::uint32_t m_surface_event_linked = 0u;
		static inline std::string m_surface_material_status = "waiting for BSP TEXINFO";

		static inline bool m_source_direct_deduplicate_runtime = true;
		static inline float m_source_direct_duplicate_distance = 24.0f;
		static inline float m_source_direct_duplicate_direction_dot = 0.90f;
		static inline int m_source_direct_snapshot_limit = 32;
		static inline bool m_bsp_worldlight_bind_owners = true;
		static inline bool m_bsp_worldlight_destroy_with_owner = true;
		static inline bool m_map_entity_graph_enabled = true;
		static inline bool m_map_light_match_entities = true;
		static inline bool m_map_light_auto_bind_nearby_owners = true;
		static inline bool m_map_light_extrude_from_models = true;
		static inline bool m_map_light_model_aware_binding = true;
		static inline bool m_map_light_use_raw_owner_indices = false;
		static inline bool m_map_light_unique_owner_claims = true;
		static inline bool m_map_light_follow_parent_attachments = true;
		static inline bool m_map_light_attachment_cache_enabled = true;
		static inline bool m_map_light_rebind_from_last_pose = true;
		static inline bool m_map_light_follow_runtime_light_state = true;
		static inline bool m_map_light_io_group_fallback = true;
		static inline bool m_map_light_io_group_propagation = true;
		static inline bool m_map_light_io_group_consensus = true;
		static inline bool m_map_light_io_action_scheduler = true;
		static inline bool m_server_accept_input_capture_enabled = true;
		static inline bool m_map_light_io_relative_delays = true;
		static inline bool m_map_light_io_honor_max_fires = true;
		static inline bool m_map_light_io_toggle_exact = true;
		static inline bool m_map_light_io_kill_is_terminal = true;
		static inline bool m_map_light_debounce_runtime_state = true;
		static inline bool m_map_light_keep_dormant_owners = true;
		static inline bool m_map_light_smooth_state_changes = true;
		static inline bool m_map_light_destroy_missing_breakables = true;
		static inline bool m_map_light_destroy_removed_props = true;
		static inline bool m_source_projected_pose_smoothing = true;
		static inline bool m_source_projected_hold_missing = true;
		// V20.2 multi-source light inference. The resolver scores every usable Source axis
		// (forward/right/up), endpoint targets, temporal history and nearby compiled WORLDLIGHTS.
		static inline bool m_source_light_axis_resolver = true;
		static inline bool m_source_light_basis_recovery = true;
		static inline bool m_source_light_temporal_axis_prior = true;
		static inline bool m_source_light_compiled_axis_prior = true;
		static inline bool m_source_light_shape_hysteresis = true;
		static inline bool m_source_light_parameter_sanity = true;
		static inline float m_map_light_entity_match_distance = 96.0f;
		static inline float m_map_light_owner_bind_distance = 80.0f;
		static inline float m_map_light_owner_resolve_distance = 192.0f;
		static inline float m_map_light_model_surface_offset = 3.0f;
		static inline float m_map_light_owner_missing_grace = 1.5f;
		static inline float m_map_light_owner_rebind_timeout = 8.0f;
		static inline float m_map_light_owner_rebind_poll_seconds = 1.0f;
		static inline float m_map_light_attachment_hold_seconds = 1.25f;
		static inline float m_map_light_runtime_light_resolve_distance = 128.0f;
		static inline float m_map_light_io_group_hold_seconds = 2.0f;
		static inline float m_map_light_io_group_confirm_seconds = 0.10f;
		static inline float m_map_light_io_group_majority_ratio = 0.60f;
		static inline int m_map_light_io_group_min_observations = 1;
		static inline int m_map_light_io_max_pending_actions = 512;
		static inline float m_map_light_io_scheduler_epsilon_seconds = 0.002f;
		static inline float m_map_light_state_confirm_seconds = 0.08f;
		static inline float m_map_light_state_fade_seconds = 0.18f;
		static inline float m_map_light_removed_prop_destroy_seconds = 2.0f;
		static inline float m_map_light_runtime_update_hz = 30.0f;
		static inline float m_source_dlight_release_hold_seconds = 0.045f;
		static inline float m_source_projected_smoothing_seconds = 0.08f;
		static inline float m_source_projected_missing_hold_seconds = 0.20f;
		static inline float m_source_light_axis_switch_margin = 0.10f;
		static inline float m_source_light_max_axis_jump_degrees = 72.0f;
		static inline float m_source_light_compiled_prior_distance = 72.0f;
		static inline int m_source_light_spot_confirm_frames = 3;
		static inline int m_source_light_sphere_confirm_frames = 3;
		static inline float m_source_dlight_intensity_scale = 90000.0f;
		static inline float m_source_dlight_radius_scale = 0.0078125f;
		static inline float m_source_projected_intensity = 90000.0f;
		static inline float m_source_projected_radius = 3.5f;
		static inline std::uint32_t m_source_dlight_active = 0u;
		static inline std::uint32_t m_source_dlight_updates = 0u;
		static inline std::uint32_t m_source_dlight_release_holds = 0u;
		static inline std::uint32_t m_source_elight_active = 0u;
		static inline std::uint32_t m_source_elight_updates = 0u;
		static inline std::uint32_t m_source_direct_duplicates_suppressed = 0u;
		static inline std::uint32_t m_source_direct_snapshots_created = 0u;
		static inline std::uint32_t m_source_direct_snapshot_lights = 0u;
		static inline std::string m_source_direct_status = "waiting for Source direct lights";
		static inline std::uint32_t m_source_alloc_dlight_calls = 0u;
		static inline std::uint32_t m_source_alloc_elight_calls = 0u;
		static inline bool m_source_alloc_hooks_installed = false;
		static inline std::uint32_t m_source_alloc_hook_attempts = 0u;
		static inline std::uint32_t m_source_alloc_hook_failures = 0u;
		static inline std::string m_source_alloc_hook_status = "not installed";
		static inline std::uint32_t m_source_projected_active = 0u;
		static inline std::uint32_t m_source_projected_updates = 0u;
		static inline std::uint32_t m_source_projected_pose_smoothed = 0u;
		static inline std::uint32_t m_source_projected_target_updates = 0u;
		static inline std::uint32_t m_source_projected_missing_holds = 0u;
		static inline std::uint32_t m_source_projected_generation_resets = 0u;
		static inline std::uint32_t m_source_light_axis_corrections = 0u;
		static inline std::uint32_t m_source_light_axis_switches = 0u;
		static inline std::uint32_t m_source_light_basis_recoveries = 0u;
		static inline std::uint32_t m_source_light_shape_holds = 0u;
		static inline std::uint32_t m_source_entity_dynamic_active = 0u;
		static inline std::uint32_t m_source_entity_dynamic_updates = 0u;
		static inline std::uint32_t m_map_entity_graph_entities = 0u;
		static inline std::uint32_t m_map_entity_graph_lights = 0u;
		static inline std::uint32_t m_map_entity_graph_owners = 0u;
		static inline std::uint32_t m_map_entity_graph_matched_lights = 0u;
		static inline std::uint32_t m_map_entity_graph_bound_owners = 0u;
		static inline std::uint32_t m_map_entity_graph_extruded = 0u;
		static inline std::uint32_t m_map_entity_graph_attachment_links = 0u;
		static inline std::uint32_t m_map_entity_graph_io_links = 0u;
		static inline std::uint32_t m_map_entity_graph_io_resolved_links = 0u;
		static inline std::uint32_t m_map_entity_graph_io_light_links = 0u;
		static inline std::uint32_t m_map_entity_graph_io_kill_links = 0u;
		static inline std::uint32_t m_map_entity_graph_io_transitive_links = 0u;
		static inline std::uint32_t m_map_entity_graph_io_relay_hops = 0u;
		static inline std::uint32_t m_map_entity_graph_io_cycles = 0u;
		static inline std::uint32_t m_map_entity_graph_io_depth_limited = 0u;
		static inline std::uint32_t m_map_entity_graph_io_multi_manager_links = 0u;
		static inline std::uint32_t m_map_entity_graph_io_relay_entities = 0u;
		static inline std::uint32_t m_map_entity_graph_controlled_lights = 0u;
		static inline std::uint32_t m_map_entity_graph_light_groups = 0u;
		static inline std::uint32_t m_map_light_io_group_observations = 0u;
		static inline std::uint32_t m_map_light_io_group_fallbacks = 0u;
		static inline std::uint32_t m_map_light_io_group_changes = 0u;
		static inline std::uint32_t m_map_light_io_group_terminal_kills = 0u;
		static inline std::uint32_t m_map_light_io_group_consensus_commits = 0u;
		static inline std::uint32_t m_map_light_io_group_conflicts = 0u;
		static inline std::uint32_t m_map_light_io_group_candidate_holds = 0u;
		static inline std::uint32_t m_map_light_io_group_split_observations = 0u;
		static inline std::uint32_t m_map_light_io_pending_count = 0u;
		static inline std::uint32_t m_map_light_io_actions_scheduled = 0u;
		static inline std::uint32_t m_map_light_io_actions_executed = 0u;
		static inline std::uint32_t m_map_light_io_actions_coalesced = 0u;
		static inline std::uint32_t m_map_light_io_actions_dropped = 0u;
		static inline std::uint32_t m_map_light_io_actions_maxfires_blocked = 0u;
		static inline std::uint32_t m_map_light_io_actions_toggle = 0u;
		static inline std::uint32_t m_map_light_io_actions_kill = 0u;
		static inline std::uint32_t m_map_light_io_actions_manual = 0u;
		static inline bool m_server_accept_input_hooks_installed = false;
		static inline bool m_server_accept_input_hook_scan_complete = false;
		static inline std::uint32_t m_server_accept_input_hook_attempts = 0u;
		static inline std::uint32_t m_server_accept_input_hook_failures = 0u;
		static inline std::uint32_t m_server_accept_input_vtables = 0u;
		static inline std::uint32_t m_server_accept_input_calls = 0u;
		static inline std::uint32_t m_server_accept_input_accepted = 0u;
		static inline std::uint32_t m_server_accept_input_direct_actions = 0u;
		static inline std::uint32_t m_server_accept_input_routed_actions = 0u;
		static inline std::uint32_t m_server_accept_input_unresolved = 0u;
		static inline std::uint32_t m_server_accept_input_dropped = 0u;
		static inline std::string m_server_accept_input_hook_status = "not installed";
		static inline std::string m_server_accept_input_last_event = "none";
		static inline std::uint32_t m_map_light_model_rejects = 0u;
		static inline std::uint32_t m_map_light_duplicate_owner_rejects = 0u;
		static inline std::uint32_t m_map_light_attachment_updates = 0u;
		static inline std::uint32_t m_map_light_destroyed_by_health = 0u;
		static inline std::uint32_t m_map_light_destroyed_by_removal = 0u;
		static inline std::uint32_t m_map_light_dormant_retained = 0u;
		static inline std::uint32_t m_map_light_owner_rebinds = 0u;
		static inline std::uint32_t m_map_light_rebind_deferred = 0u;
		static inline std::uint32_t m_map_light_attachment_fallbacks = 0u;
		static inline std::uint32_t m_map_light_attachment_cache_hits = 0u;
		static inline std::uint32_t m_map_light_handle_generation_rejects = 0u;
		static inline std::uint32_t m_map_light_predicted_rebinds = 0u;
		static inline std::uint32_t m_map_runtime_light_state_active = 0u;
		static inline std::uint32_t m_map_runtime_light_state_hidden = 0u;
		static inline std::uint32_t m_map_runtime_light_state_unresolved = 0u;
		static inline std::uint32_t m_map_runtime_light_state_updates = 0u;
		static inline std::uint32_t m_map_runtime_light_state_pending = 0u;
		static inline std::uint32_t m_map_runtime_light_state_missing_held = 0u;
		static inline std::uint32_t m_map_runtime_light_state_confirmed_changes = 0u;
		static inline std::uint32_t m_map_light_reason_transitions = 0u;
		static inline std::uint32_t m_map_light_transition_log_dropped = 0u;
		static inline std::string m_map_light_last_transition = "none";
		static inline std::string m_map_entity_graph_status = "not loaded";
		static inline std::uint32_t m_owned_worldlights_active = 0u;
		static inline std::uint32_t m_owned_worldlights_hidden = 0u;
		static inline std::uint32_t m_owned_worldlights_unresolved = 0u;
		static inline std::uint32_t m_owned_worldlights_updated = 0u;
		static inline std::string m_map_light_runtime_status = "waiting for map";
		static void clear_runtime_imported_lights();
		static bool rebuild_map_entity_graph();

		static inline bool m_d3d_light_capture_enabled = false;
		static inline bool m_d3d_light_spawn_enabled = false;
		static inline bool m_d3d_hooks_installed = false;
		static inline std::uint32_t m_d3d_hook_attempts = 0u;
		static inline std::uint32_t m_d3d_hook_failures = 0u;
		static inline std::string m_d3d_hook_status = "not installed";
		static inline float m_d3d_light_scalar = 1.0f;
		static inline float m_d3d_light_radius_scale = 1.0f;
		static inline float m_d3d_light_duration = 0.10f;
		static inline float m_d3d_light_cooldown = 0.035f;
		static inline std::uint32_t m_d3d_setlight_calls = 0u;
		static inline std::uint32_t m_d3d_lightenable_calls = 0u;
		static inline std::uint32_t m_d3d_spawned_lights = 0u;
		static inline std::uint32_t m_d3d_last_index = 0u;
		static inline Vector m_d3d_last_origin = Vector(0.0f, 0.0f, 0.0f);

		static void on_d3d_set_light(DWORD index, const D3DLIGHT9* light);
		static void on_d3d_light_enable(DWORD index, BOOL enable);


		struct source_direct_light_info_s
		{
			std::string kind;
			std::string classname;
			std::string comment;
			Vector position = Vector(0.0f, 0.0f, 0.0f);
			Vector direction = Vector(1.0f, 0.0f, 0.0f);
			Vector radiance = Vector(1.0f, 1.0f, 1.0f);
			float intensity = 0.0f;
			float radius = 0.0f;
			float outer_angle = 180.0f;
			float inner_angle = 180.0f;
			float near_z = 0.0f;
			float far_z = 0.0f;
			std::int32_t source_index = -1;
			std::int32_t runtime_entity_index = -1;
			std::int32_t source_key = 0;
			std::uint64_t capture_id = 0u;
			std::int32_t style = 0;
			float style_value = 1.0f;
			std::int32_t owner = -1;
			std::int32_t matched_compiled_source = -1;
			bool enabled = false;
			bool transient = false;
			bool shaped = false;
			bool shadows = false;
			bool duplicate_suppressed = false;
			std::string direction_source;
			std::string shape_reason;
			float direction_confidence = 0.0f;
			float shape_confidence = 0.0f;
			float axis_disagreement_degrees = 0.0f;
			float shape_evidence = 0.0f;
			std::uint32_t axis_stable_frames = 0u;
			std::uint32_t axis_switches = 0u;
			std::string axis_candidates;
			std::string parameter_sources;
			bool direction_corrected = false;
			bool basis_recovered = false;
			std::string owner_category;
			std::string owner_classname;
			std::string owner_model;
			bool policy_allowed = true;
			std::string policy_reason;
			std::string surface_material_name;
			bool event_controlled = false;
		};

		static std::vector<source_direct_light_info_s> get_source_direct_light_registry();
		static std::string get_source_direct_light_report();
		static bool snapshot_active_source_direct_lights_to_editor();
		static bool snapshot_source_direct_light_to_editor(std::uint64_t capture_id);
		static bool clear_source_direct_light_snapshots();

		static inline bool m_draw_debug = false;
		static inline float m_debug_last_trigger_time = 0.0f;
		static inline std::string m_debug_last_trigger_name;
		static inline Vector m_debug_last_trigger_origin = Vector(0.0f, 0.0f, 0.0f);

	private:
		struct pending_light_s
		{
			map_settings::remix_light_settings_s def;
			float delay = 0.0f;
		};

		static inline std::vector<pending_light_s> m_pending_lights = {};
		static inline float m_runtime_budget_window_start = -99999.0f;
		static inline float m_next_leaf_check_time = 0.0f;
		static inline float m_last_auto_muzzle_time = -99999.0f;
		static inline std::unordered_map<std::uint32_t, float> m_sound_hash_last_trigger_times = {};
		static inline bool m_bsp_worldlight_auto_import_pending = false;
		static inline float m_bsp_worldlight_auto_import_delay = 0.0f;
		static inline std::uint32_t m_bsp_worldlight_auto_import_retries = 0u;
		static inline std::string m_bsp_worldlight_pending_map;
		static inline std::string m_bsp_worldlight_loaded_map;
		static inline float m_bsp_worldlight_watchdog_next_check = 0.0f;
		static inline float m_bsp_worldlight_last_client_time = -1.0f;
		static inline bool m_bsp_worldlight_session_probe_pending = false;
		static inline double m_bsp_worldlight_last_map_load_signal = -99999.0;
		static inline std::uint32_t m_bsp_worldlight_session_generation = 0u;
		static inline float m_bsp_worldlight_next_stream_check = 0.0f;
		static inline Vector m_bsp_worldlight_last_import_origin = Vector(0.0f, 0.0f, 0.0f);
		static inline bool m_bsp_worldlight_have_import_origin = false;
		static inline float m_map_light_runtime_next_update = 0.0f;
		static inline bool m_map_light_config_dirty = false;
		static inline float m_map_light_config_save_at = 0.0f;
		static inline std::uint32_t m_map_light_editor_synced_generation = 0xffffffffu;
		static inline bool m_persistent_map_light_dirty = false;
		static inline float m_persistent_map_light_save_at = 0.0f;
		static inline std::uint64_t m_persistent_map_light_editor_fingerprint = 0u;
		static inline bool m_persistent_map_light_fingerprint_valid = false;
		static inline std::unordered_set<std::string> m_persistent_map_light_known_ids = {};
		static inline std::unordered_set<std::int32_t> m_persistent_map_light_materialized_sources = {};

		struct runtime_source_light_s
		{
			map_settings::remix_light_settings_s def;
			std::uint64_t signature = 0u;
			Vector smoothed_position = Vector(0.0f, 0.0f, 0.0f);
			Vector smoothed_direction = Vector(1.0f, 0.0f, 0.0f);
			std::uint32_t source_handle_raw = 0xffffffffu;
			float last_seen_time = -99999.0f;
			bool seen = false;
			bool pose_initialized = false;
			bool last_enabled = false;
			bool entity_light = false;
			bool transient = false;
			bool shadows = false;
			bool duplicate_suppressed = false;
			int runtime_entity_index = -1;
			int source_key = 0;
			int source_style = 0;
			int source_owner = -1;
			int matched_compiled_source = -1;
			float source_radius = 0.0f;
			float source_brightness = 1.0f;
			float source_outer_angle = 180.0f;
			float source_inner_angle = 180.0f;
			float source_near_z = 0.0f;
			float source_far_z = 0.0f;
			std::uint8_t spot_evidence_frames = 0u;
			std::uint8_t sphere_evidence_frames = 0u;
			bool stable_shaped = false;
			bool shape_initialized = false;
			Vector last_resolved_direction = Vector(1.0f, 0.0f, 0.0f);
			bool axis_initialized = false;
			std::uint32_t axis_stable_frames = 0u;
			std::uint32_t axis_switches = 0u;
			std::string last_axis_source;
			std::string runtime_kind;
			std::string runtime_classname;
			std::string direction_source;
			std::string shape_reason;
			float direction_confidence = 0.0f;
			float shape_confidence = 0.0f;
			float axis_disagreement_degrees = 0.0f;
			float shape_evidence = 0.0f;
			std::string axis_candidates;
			std::string parameter_sources;
			bool direction_corrected = false;
			bool basis_recovered = false;
			std::string owner_category = "world";
			std::string owner_classname;
			std::string owner_model;
			bool policy_allowed = true;
			std::string policy_reason = "allowed";
		};

		struct owned_worldlight_binding_s
		{
			std::uint32_t source_index = 0u;
			int owner_index = -1;
			int runtime_owner_index = -1;
			int map_owner_source_index = -1;
			int style = 0;
			Vector local_origin = Vector(0.0f, 0.0f, 0.0f);
			Vector local_direction = Vector(0.0f, 0.0f, -1.0f);
			Vector authored_world_origin = Vector(0.0f, 0.0f, 0.0f);
			Vector authored_world_direction = Vector(0.0f, 0.0f, -1.0f);
			Vector attachment_local_origin = Vector(0.0f, 0.0f, 0.0f);
			Vector attachment_local_direction = Vector(0.0f, 0.0f, -1.0f);
			Vector map_owner_origin = Vector(0.0f, 0.0f, 0.0f);
			Vector map_owner_angles = Vector(0.0f, 0.0f, 0.0f);
			Vector last_owner_origin = Vector(0.0f, 0.0f, 0.0f);
			Vector last_owner_angles = Vector(0.0f, 0.0f, 0.0f);
			Vector cached_attachment_origin = Vector(0.0f, 0.0f, 0.0f);
			Vector cached_attachment_angles = Vector(0.0f, 0.0f, 0.0f);
			Vector map_light_origin = Vector(0.0f, 0.0f, 0.0f);
			std::string owner_class_hint;
			std::string owner_targetname;
			std::string owner_model_hint;
			std::string owner_attachment;
			std::string map_light_class_hint;
			std::string map_light_targetname;
			std::int32_t owner_hammer_id = -1;
			std::int32_t owner_initial_health = -1;
			bool owner_breakable = false;
			std::string binding_reason;
			std::string io_control_summary;
			map_settings::remix_light_settings_s def;
			std::uint64_t signature = 0u;
			std::uintptr_t owner_entity_identity = 0u;
			std::uintptr_t owner_class_identity = 0u;
			std::uint32_t owner_handle_raw = 0xffffffffu;
			std::uint32_t runtime_light_handle_raw = 0xffffffffu;
			int map_light_source_index = -1;
			int runtime_light_entity_index = -1;
			std::uint32_t io_control_group = 0u;
			bool active = false;
			bool owner_resolved_once = false;
			bool owner_destroyed = false;
			bool graph_bound = false;
			bool local_transform_valid = false;
			bool extrude_from_owner = false;
			bool extrusion_applied = false;
			bool attachment_applied = false;
			bool attachment_local_valid = false;
			bool attachment_cache_valid = false;
			bool last_owner_pose_valid = false;
			bool runtime_light_trackable = false;
			bool runtime_light_resolved_once = false;
			bool runtime_light_last_enabled = true;
			bool runtime_light_state_initialized = false;
			bool runtime_light_pending_enabled = true;
			bool runtime_light_pending_valid = false;
			bool map_light_starts_disabled = false;
			bool io_killable = false;
			bool io_group_kill_all = false;
			bool io_group_terminal = false;
			bool io_scheduled_state_valid = false;
			bool io_scheduled_enabled = true;
			bool io_scheduled_terminal = false;
			bool owner_health_seen_positive = false;
			int owner_last_health = -1;
			float state_scalar = 0.0f;
			float last_state_update_time = -99999.0f;
			float owner_last_seen_time = -99999.0f;
			float attachment_last_seen_time = -99999.0f;
			float runtime_light_last_seen_time = -99999.0f;
			float runtime_light_pending_since = -99999.0f;
			float owner_last_rebind_attempt_time = -99999.0f;
			float io_last_action_time = -99999.0f;
			float last_transition_time = -99999.0f;
			std::uint64_t io_last_action_serial = 0u;
			std::uint32_t transition_count = 0u;
			std::uint32_t owner_missing_updates = 0u;
			std::string owner_state_reason = "unresolved";
			std::string runtime_light_state_reason = "not tracked";
			std::string io_scheduled_reason = "no scheduled map I/O action";
			std::string final_state_reason = "not evaluated";
		};

		struct io_group_runtime_state_s
		{
			bool initialized = false;
			bool enabled = true;
			bool terminal = false;
			bool candidate_valid = false;
			bool candidate_enabled = true;
			bool conflict = false;
			float last_observed_time = -99999.0f;
			float candidate_since = -99999.0f;
			std::uint32_t observations = 0u;
			std::uint32_t last_enabled_votes = 0u;
			std::uint32_t last_disabled_votes = 0u;
			std::uint32_t last_terminal_votes = 0u;
			std::uint64_t transition_serial = 0u;
		};


		struct map_light_io_exact_state_s
		{
			bool enabled = true;
			bool terminal = false;
			float changed_at = -99999.0f;
			std::uint64_t serial = 0u;
			std::string reason;
		};

		struct map_light_io_scheduled_action_s
		{
			float execute_time = 0.0f;
			float authored_delay = 0.0f;
			std::uint64_t serial = 0u;
			std::uint32_t source_index = 0u;
			std::uint32_t control_group = 0u;
			std::uint32_t root_action_id = 0u;
			std::size_t graph_action_index = 0u;
			source_map_entities::io_action_kind kind = source_map_entities::io_action_kind::unknown;
			std::string path;
			bool manual = false;
			bool runtime_captured = false;
		};

		struct map_light_transition_s
		{
			float time = 0.0f;
			std::uint32_t source_index = 0u;
			std::uint32_t control_group = 0u;
			std::string from_reason;
			std::string to_reason;
		};

		static inline std::unordered_map<std::uint32_t, io_group_runtime_state_s> m_map_light_io_group_states = {};
		static inline std::unordered_map<std::uint32_t, map_light_io_exact_state_s> m_map_light_io_exact_states = {};
		static inline std::vector<map_light_io_scheduled_action_s> m_map_light_io_pending_actions = {};
		static inline std::unordered_map<std::uint32_t, std::uint32_t> m_map_light_io_root_fire_counts = {};
		static inline std::uint64_t m_map_light_io_next_serial = 1u;
		static inline std::deque<map_light_transition_s> m_map_light_transition_log = {};
		static inline std::unordered_map<std::uintptr_t, runtime_source_light_s> m_runtime_dlights = {};
		static inline std::unordered_map<int, runtime_source_light_s> m_runtime_projected_lights = {};
		static inline std::vector<owned_worldlight_binding_s> m_owned_worldlight_bindings = {};
		static inline std::unordered_set<std::uint32_t> m_bsp_worldlight_active_sources = {};

		static void update_source_runtime_lights(float curtime);
		static void update_source_dlights();
		static void update_projected_texture_entities();
		static void update_owned_worldlights();
		static void process_scheduled_map_io_actions(float curtime);
		static void schedule_map_io_group_transition(std::uint32_t group_id, bool old_enabled, bool new_enabled, float curtime, bool manual = false);
		static void seed_map_io_group_state(std::uint32_t group_id, bool enabled, float curtime);
		static bool schedule_map_io_action(std::uint32_t source_index, std::uint32_t group_id,
			std::size_t graph_action_index, float execute_time, float authored_delay, bool manual,
			bool runtime_captured = false);
		static void process_server_accept_input_events(float curtime);
		static bool apply_direct_server_input(std::uint32_t map_source_index,
			source_map_entities::io_action_kind kind, float curtime, std::string_view reason);

		static void queue_or_spawn(map_settings::remix_light_settings_s&& def, float delay);
		static void update_budget_window(float curtime);
		static bool can_spawn_runtime_light(const map_settings::remix_light_settings_s& def, bool delayed);
		static bool can_process_sound_hash_runtime_event();
		static bool can_process_muzzle_runtime_event();
		static void trigger_event(map_settings::dynamic_light_event_s& ev, const Vector* source_origin = nullptr, const Vector* source_forward = nullptr);
		static const map_settings::light_anchor_s* find_light_anchor(const std::string& name);
		static map_settings::dynamic_light_event_s build_event_from_anchor(const map_settings::light_anchor_s& anchor, const map_settings::dynamic_light_event_s* trigger_ev = nullptr);
		static float clamp_workflow_radius(float radius);
		static map_settings::remix_light_settings_s build_light_from_event(const map_settings::dynamic_light_event_s& ev, const Vector& origin, const Vector& forward);
		struct d3d_light_cache_s
		{
			D3DLIGHT9 light = {};
			bool enabled = false;
			float last_spawn_time = -99999.0f;
		};

		static inline std::unordered_map<DWORD, d3d_light_cache_s> m_d3d_lights = {};

		static bool is_likely_muzzle_sound(const std::string& sound_name);
		static bool get_auto_muzzle_source(const Vector& sound_origin, Vector& out_origin, Vector& out_forward);
		static const char* get_muzzle_profile_name_for_ui();
		static bool get_player_flashlight_source(Vector& out_origin, Vector& out_forward);
		static Vector apply_muzzle_offset(const Vector& origin, const Vector& forward, const Vector& right, const Vector& up);
		static void try_spawn_from_d3d_light(DWORD index, const D3DLIGHT9& light);
		static bool try_spawn_from_sound_hash_library(std::uint32_t hash, const std::string& sound_name, const Vector& origin);
		static bool is_zero_origin(const Vector& origin);
	};
}
