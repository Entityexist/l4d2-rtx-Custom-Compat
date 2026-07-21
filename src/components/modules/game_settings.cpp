#include "std_include.hpp"
#include "components/common/toml.hpp"
#include "material_exporter.hpp"

namespace components
{
	namespace
	{
		constexpr std::uint64_t k_fnv_offset = 14695981039346656037ull;
		constexpr std::uint64_t k_fnv_prime = 1099511628211ull;

		void hash_bytes(std::uint64_t& hash, const void* data, const std::size_t size)
		{
			const auto* bytes = static_cast<const std::uint8_t*>(data);
			for (std::size_t i = 0; i < size; ++i) {
				hash = (hash ^ bytes[i]) * k_fnv_prime;
			}
		}

		template <typename T>
		void hash_value(std::uint64_t& hash, const T& value)
		{
			hash_bytes(hash, &value, sizeof(value));
		}

		bool read_bool(const toml::value& entry, const bool fallback)
		{
			try
			{
				if (entry.is_boolean()) return entry.as_boolean();
				if (entry.is_integer()) return entry.as_integer() != 0;
			}
			catch (const toml::type_error& err) {
				game::console();
				printf("[GameSettings] %s\n", err.what());
			}
			return fallback;
		}

		int read_int(const toml::value& entry, const int fallback)
		{
			try
			{
				if (entry.is_boolean()) return entry.as_boolean() ? 1 : 0;
				if (entry.is_integer()) return static_cast<int>(entry.as_integer());
				if (entry.is_floating()) return static_cast<int>(entry.as_floating());
			}
			catch (const toml::type_error& err) {
				game::console();
				printf("[GameSettings] %s\n", err.what());
			}
			return fallback;
		}

		float read_float(const toml::value& entry, const float fallback)
		{
			try
			{
				if (entry.is_integer()) return static_cast<float>(entry.as_integer());
				if (entry.is_floating()) return static_cast<float>(entry.as_floating());
			}
			catch (const toml::type_error& err) {
				game::console();
				printf("[GameSettings] %s\n", err.what());
			}
			return fallback;
		}

		std::vector<float> read_vector(const toml::value& entry, const std::size_t expected, const float* fallback)
		{
			std::vector<float> result(expected, 0.0f);
			for (std::size_t i = 0; i < expected; ++i) result[i] = fallback ? fallback[i] : 0.0f;
			try
			{
				if (!entry.is_array() || entry.as_array().size() != expected) return result;
				for (std::size_t i = 0; i < expected; ++i) {
					const auto& value = entry.as_array()[i];
					if (value.is_integer()) result[i] = static_cast<float>(value.as_integer());
					else if (value.is_floating()) result[i] = static_cast<float>(value.as_floating());
				}
			}
			catch (const toml::type_error& err) {
				game::console();
				printf("[GameSettings] %s\n", err.what());
			}
			return result;
		}

		std::filesystem::path settings_path()
		{
			return std::filesystem::path(game::root_path + COMPMOD_ASSET_DIR "game_settings.toml");
		}

		const toml::value* runtime_ui_table(const toml::value& config)
		{
			if (!config.contains("runtime_ui")) return nullptr;
			const auto& table = config.at("runtime_ui");
			return table.is_table() ? &table : nullptr;
		}
	}

	bool game_settings::atomic_replace_file(const std::filesystem::path& temp_path, const std::filesystem::path& final_path)
	{
		const auto temp_w = temp_path.wstring();
		const auto final_w = final_path.wstring();
		return MoveFileExW(temp_w.c_str(), final_w.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
	}

	void game_settings::write_runtime_ui_settings(std::ostream& file)
	{
		file << std::setprecision(9);
		file << "\n[runtime_ui]\n";
		file << "schema_version = 31\n";
		file << "autosave_debounce_ms = " << m_autosave_debounce.count() << "\n\n";

		file << "# Muzzle Flash\n";
		file << "muzzle_enabled = " << (dynamic_lighting::m_auto_muzzle_flash ? "true" : "false") << '\n';
		file << "muzzle_origin_mode = " << dynamic_lighting::m_muzzle_flash_origin_mode << '\n';
		file << "muzzle_shape_mode = " << dynamic_lighting::m_muzzle_flash_shape_mode << '\n';
		file << "muzzle_weapon_profile = " << dynamic_lighting::m_muzzle_weapon_profile_mode << '\n';
		file << "muzzle_weapon_profiles_enabled = " << (dynamic_lighting::m_muzzle_weapon_profiles_enabled ? "true" : "false") << '\n';
		file << "muzzle_intensity = " << dynamic_lighting::m_muzzle_flash_scalar << '\n';
		file << "muzzle_radius = " << dynamic_lighting::m_muzzle_flash_radius << '\n';
		file << "muzzle_duration = " << dynamic_lighting::m_muzzle_flash_duration << '\n';
		file << "muzzle_fade = " << dynamic_lighting::m_muzzle_flash_fade << '\n';
		file << "muzzle_delay = " << dynamic_lighting::m_muzzle_flash_delay << '\n';
		file << "muzzle_color = [" << dynamic_lighting::m_muzzle_flash_color.x << ", " << dynamic_lighting::m_muzzle_flash_color.y << ", " << dynamic_lighting::m_muzzle_flash_color.z << "]\n";
		file << "muzzle_cooldown = " << dynamic_lighting::m_muzzle_flash_cooldown << '\n';
		file << "muzzle_offset = [" << dynamic_lighting::m_muzzle_flash_offset.x << ", " << dynamic_lighting::m_muzzle_flash_offset.y << ", " << dynamic_lighting::m_muzzle_flash_offset.z << "]\n";
		file << "muzzle_debug = " << (dynamic_lighting::m_muzzle_draw_debug ? "true" : "false") << '\n';
		file << "muzzle_strict_fire_tokens = " << (dynamic_lighting::m_muzzle_strict_fire_tokens ? "true" : "false") << '\n';
		file << "muzzle_reject_weapon_handling = " << (dynamic_lighting::m_muzzle_reject_weapon_handling ? "true" : "false") << '\n';
		file << "muzzle_allow_weapon_family_fallback = " << (dynamic_lighting::m_muzzle_allow_weapon_family_fallback ? "true" : "false") << "\n\n";

		file << "# Performance / compatibility\n";
		file << "performance_profile = " << dynamic_lighting::m_compat_profile << '\n';
		file << "runtime_budgets_enabled = " << (dynamic_lighting::m_runtime_budgets_enabled ? "true" : "false") << '\n';
		file << "authoring_debug_tools = " << (dynamic_lighting::m_authoring_debug_tools ? "true" : "false") << '\n';
		file << "max_active_lights = " << dynamic_lighting::m_runtime_max_active_lights << '\n';
		file << "max_pending_lights = " << dynamic_lighting::m_runtime_max_pending_lights << '\n';
		file << "max_spawns_per_second = " << dynamic_lighting::m_runtime_max_spawns_per_second << '\n';
		file << "max_muzzle_per_second = " << dynamic_lighting::m_runtime_max_muzzle_per_second << '\n';
		file << "max_sound_hash_per_second = " << dynamic_lighting::m_runtime_max_sound_hash_per_second << '\n';
		file << "leaf_check_hz = " << dynamic_lighting::m_runtime_leaf_check_hz << '\n';
		file << "cpu_skin_throttle = " << (dynamic_lighting::m_cpu_skin_throttle_enabled ? "true" : "false") << '\n';
		file << "cpu_skin_skip_common = " << (dynamic_lighting::m_cpu_skin_skip_far_common ? "true" : "false") << '\n';
		file << "cpu_skin_skip_ragdolls = " << (dynamic_lighting::m_cpu_skin_skip_far_ragdolls ? "true" : "false") << '\n';
		file << "cpu_skin_common_distance = " << dynamic_lighting::m_cpu_skin_common_skip_distance << '\n';
		file << "cpu_skin_ragdoll_distance = " << dynamic_lighting::m_cpu_skin_ragdoll_skip_distance << '\n';

		file << "\n# Layered flashlight runtime governor\n";
		file << "flashlight_governor_enabled = " << (dynamic_lighting::m_flashlight_governor_enabled ? "true" : "false") << '\n';
		file << "flashlight_update_hz = " << dynamic_lighting::m_flashlight_update_hz << '\n';
		file << "flashlight_motion_epsilon = " << dynamic_lighting::m_flashlight_motion_epsilon << '\n';
		file << "flashlight_direction_epsilon_degrees = " << dynamic_lighting::m_flashlight_direction_epsilon_degrees << '\n';
		file << "flashlight_force_update_distance = " << dynamic_lighting::m_flashlight_force_update_distance << '\n';
		file << "flashlight_player_layer_limit = " << dynamic_lighting::m_flashlight_player_layer_limit << '\n';
		file << "flashlight_bot_layer_limit = " << dynamic_lighting::m_flashlight_bot_layer_limit << '\n';
		file << "flashlight_budget_reserve = " << dynamic_lighting::m_flashlight_budget_reserve << '\n';
		file << "flashlight_cull_distant_bots = " << (dynamic_lighting::m_flashlight_cull_distant_bots ? "true" : "false") << '\n';
		file << "flashlight_bot_cull_distance = " << dynamic_lighting::m_flashlight_bot_cull_distance << '\n';
		file << "flashlight_nearest_bot_priority = " << (dynamic_lighting::m_flashlight_nearest_bot_priority ? "true" : "false") << '\n';
		file << "flashlight_preserve_last_good_rig = " << (dynamic_lighting::m_flashlight_preserve_last_good_rig ? "true" : "false") << '\n';
		file << "flashlight_verify_draw_results = " << (dynamic_lighting::m_flashlight_verify_draw_results ? "true" : "false") << '\n';
		file << "flashlight_owner_grace_frames = " << dynamic_lighting::m_flashlight_owner_grace_frames << '\n';
		file << "flashlight_retry_base_ms = " << dynamic_lighting::m_flashlight_retry_base_ms << '\n';
		file << "flashlight_retry_max_ms = " << dynamic_lighting::m_flashlight_retry_max_ms << '\n';

		file << "\n# Source runtime light policy\n";
		file << "source_runtime_profile = " << dynamic_lighting::m_source_runtime_profile << '\n';
		file << "source_runtime_allow_world = " << (dynamic_lighting::m_source_runtime_allow_world ? "true" : "false") << '\n';
		file << "source_runtime_allow_survivors = " << (dynamic_lighting::m_source_runtime_allow_survivors ? "true" : "false") << '\n';
		file << "source_runtime_allow_infected = " << (dynamic_lighting::m_source_runtime_allow_infected ? "true" : "false") << '\n';
		file << "source_runtime_allow_unknown_characters = " << (dynamic_lighting::m_source_runtime_allow_unknown_characters ? "true" : "false") << '\n';
		file << "source_runtime_allow_dlights = " << (dynamic_lighting::m_source_runtime_allow_dlights ? "true" : "false") << '\n';
		file << "source_runtime_allow_elights = " << (dynamic_lighting::m_source_runtime_allow_elights ? "true" : "false") << '\n';
		file << "source_runtime_allow_entity_dynamic = " << (dynamic_lighting::m_source_runtime_allow_entity_dynamic ? "true" : "false") << '\n';
		file << "source_runtime_allow_projected = " << (dynamic_lighting::m_source_runtime_allow_projected ? "true" : "false") << '\n';
		file << "source_runtime_allow_point_spotlight = " << (dynamic_lighting::m_source_runtime_allow_point_spotlight ? "true" : "false") << '\n';
		file << "source_runtime_world_intensity_scale = " << dynamic_lighting::m_source_runtime_world_intensity_scale << '\n';
		file << "source_runtime_character_intensity_scale = " << dynamic_lighting::m_source_runtime_character_intensity_scale << '\n';
		file << "source_runtime_world_radius_scale = " << dynamic_lighting::m_source_runtime_world_radius_scale << '\n';
		file << "source_runtime_character_radius_scale = " << dynamic_lighting::m_source_runtime_character_radius_scale << '\n';
		file << "source_runtime_max_radius = " << dynamic_lighting::m_source_runtime_max_radius << '\n';
		file << "source_runtime_character_max_radius = " << dynamic_lighting::m_source_runtime_character_max_radius << '\n';
		file << "source_runtime_character_spot_max_angle = " << dynamic_lighting::m_source_runtime_character_spot_max_angle << '\n';
		file << "source_runtime_show_rejected = " << (dynamic_lighting::m_source_runtime_show_rejected_in_registry ? "true" : "false") << '\n';

		file << "\n# Event-driven map light recovery\n";
		file << "map_event_lights_enabled = " << (dynamic_lighting::m_map_event_lights_enabled ? "true" : "false") << '\n';
		file << "map_event_accept_input = " << (dynamic_lighting::m_map_event_accept_input ? "true" : "false") << '\n';
		file << "map_event_follow_lightstyles = " << (dynamic_lighting::m_map_event_follow_lightstyles ? "true" : "false") << '\n';
		file << "map_event_include_sprite_proxies = " << (dynamic_lighting::m_map_event_include_sprite_proxies ? "true" : "false") << '\n';
		file << "map_event_honor_starts_disabled = " << (dynamic_lighting::m_map_event_honor_starts_disabled ? "true" : "false") << '\n';
		file << "map_event_surface_material_linkage = " << (dynamic_lighting::m_map_event_surface_material_linkage ? "true" : "false") << '\n';

		file << "\n# Source material asset recovery\n";
		file << "material_resolve_source_assets = " << (material_exporter::m_resolve_source_assets_during_capture ? "true" : "false") << '\n';
		file << "material_require_asset_backed_activation = " << (material_exporter::m_require_asset_backed_activation ? "true" : "false") << '\n';
		file << "material_allow_live_sampler_fallback = " << (material_exporter::m_allow_live_sampler_fallback ? "true" : "false") << '\n';
		file << "material_rescan_unresolved_periodically = " << (material_exporter::m_rescan_unresolved_assets_periodically ? "true" : "false") << '\n';
		file << "material_export_original_vtf = " << (material_exporter::m_export_raw_vtf ? "true" : "false") << '\n';
		file << "material_source_bridge_enabled = " << (material_exporter::m_source_material_bridge_enabled ? "true" : "false") << '\n';
		file << "material_model_draw_packets_enabled = " << (material_exporter::m_model_material_packets_enabled ? "true" : "false") << '\n';
		file << "material_source_shader_semantics = " << (material_exporter::m_enable_source_shader_semantics ? "true" : "false") << '\n';
		file << "material_resolve_model_cdmaterials = " << (material_exporter::m_resolve_model_cdmaterials ? "true" : "false") << '\n';
		file << "material_inspect_vtf_metadata = " << (material_exporter::m_inspect_vtf_metadata ? "true" : "false") << '\n';
		file << "material_preserve_dynamic_for_review = " << (material_exporter::m_preserve_dynamic_materials_for_review ? "true" : "false") << '\n';
		file << "material_allow_envmapmask_conflict_fallback = " << (material_exporter::m_allow_envmapmask_conflict_fallback ? "true" : "false") << '\n';
		file << "material_use_detail_as_pbr_microdetail = " << (material_exporter::m_use_detail_as_pbr_microdetail ? "true" : "false") << '\n';
		file << "material_export_semantic_manifest = " << (material_exporter::m_export_semantic_manifest ? "true" : "false") << '\n';
		file << "material_patch_vmt_resolution = " << (material_exporter::m_enable_patch_vmt_resolution ? "true" : "false") << '\n';
		file << "material_calibrated_specular = " << (material_exporter::m_enable_calibrated_specular ? "true" : "false") << '\n';
		file << "material_compose_detail_layers = " << (material_exporter::m_compose_detail_layers ? "true" : "false") << '\n';
		file << "material_preserve_world_vertex_transition = " << (material_exporter::m_preserve_world_vertex_transition ? "true" : "false") << '\n';
		file << "material_link_event_emissive = " << (material_exporter::m_link_event_emissive_materials ? "true" : "false") << '\n';
	}

	void game_settings::parse_runtime_ui_settings(const toml::value& config)
	{
		const auto* runtime = runtime_ui_table(config);
		if (!runtime) return;

		auto contains = [runtime](const char* key) { return runtime->contains(key); };
		auto value = [runtime](const char* key) -> const toml::value& { return runtime->at(key); };

		if (contains("muzzle_enabled")) dynamic_lighting::m_auto_muzzle_flash = read_bool(value("muzzle_enabled"), dynamic_lighting::m_auto_muzzle_flash);
		if (contains("muzzle_origin_mode")) dynamic_lighting::m_muzzle_flash_origin_mode = std::clamp(read_int(value("muzzle_origin_mode"), dynamic_lighting::m_muzzle_flash_origin_mode), 0, 3);
		if (contains("muzzle_shape_mode")) dynamic_lighting::m_muzzle_flash_shape_mode = std::clamp(read_int(value("muzzle_shape_mode"), dynamic_lighting::m_muzzle_flash_shape_mode), 0, 1);
		if (contains("muzzle_weapon_profile")) dynamic_lighting::m_muzzle_weapon_profile_mode = std::clamp(read_int(value("muzzle_weapon_profile"), dynamic_lighting::m_muzzle_weapon_profile_mode), 0, 6);
		if (contains("muzzle_weapon_profiles_enabled")) dynamic_lighting::m_muzzle_weapon_profiles_enabled = read_bool(value("muzzle_weapon_profiles_enabled"), dynamic_lighting::m_muzzle_weapon_profiles_enabled);
		if (contains("muzzle_intensity")) dynamic_lighting::m_muzzle_flash_scalar = std::max(0.0f, read_float(value("muzzle_intensity"), dynamic_lighting::m_muzzle_flash_scalar));
		if (contains("muzzle_radius")) dynamic_lighting::m_muzzle_flash_radius = std::max(0.001f, read_float(value("muzzle_radius"), dynamic_lighting::m_muzzle_flash_radius));
		if (contains("muzzle_duration")) dynamic_lighting::m_muzzle_flash_duration = std::max(0.025f, read_float(value("muzzle_duration"), dynamic_lighting::m_muzzle_flash_duration));
		if (contains("muzzle_fade")) dynamic_lighting::m_muzzle_flash_fade = std::max(0.005f, read_float(value("muzzle_fade"), dynamic_lighting::m_muzzle_flash_fade));
		if (contains("muzzle_delay")) dynamic_lighting::m_muzzle_flash_delay = std::max(0.0f, read_float(value("muzzle_delay"), dynamic_lighting::m_muzzle_flash_delay));
		if (contains("muzzle_color")) {
			const float fallback[3] = { dynamic_lighting::m_muzzle_flash_color.x, dynamic_lighting::m_muzzle_flash_color.y, dynamic_lighting::m_muzzle_flash_color.z };
			const auto v = read_vector(value("muzzle_color"), 3u, fallback);
			dynamic_lighting::m_muzzle_flash_color = Vector(std::max(0.0f, v[0]), std::max(0.0f, v[1]), std::max(0.0f, v[2]));
		}
		if (contains("muzzle_cooldown")) dynamic_lighting::m_muzzle_flash_cooldown = std::max(0.0f, read_float(value("muzzle_cooldown"), dynamic_lighting::m_muzzle_flash_cooldown));
		if (contains("muzzle_offset")) {
			const float fallback[3] = { dynamic_lighting::m_muzzle_flash_offset.x, dynamic_lighting::m_muzzle_flash_offset.y, dynamic_lighting::m_muzzle_flash_offset.z };
			const auto v = read_vector(value("muzzle_offset"), 3u, fallback);
			dynamic_lighting::m_muzzle_flash_offset = Vector(v[0], v[1], v[2]);
		}
		if (contains("muzzle_debug")) dynamic_lighting::m_muzzle_draw_debug = read_bool(value("muzzle_debug"), dynamic_lighting::m_muzzle_draw_debug);
		if (contains("muzzle_strict_fire_tokens")) dynamic_lighting::m_muzzle_strict_fire_tokens = read_bool(value("muzzle_strict_fire_tokens"), dynamic_lighting::m_muzzle_strict_fire_tokens);
		if (contains("muzzle_reject_weapon_handling")) dynamic_lighting::m_muzzle_reject_weapon_handling = read_bool(value("muzzle_reject_weapon_handling"), dynamic_lighting::m_muzzle_reject_weapon_handling);
		if (contains("muzzle_allow_weapon_family_fallback")) dynamic_lighting::m_muzzle_allow_weapon_family_fallback = read_bool(value("muzzle_allow_weapon_family_fallback"), dynamic_lighting::m_muzzle_allow_weapon_family_fallback);

		if (contains("performance_profile")) dynamic_lighting::apply_compat_profile(std::clamp(read_int(value("performance_profile"), dynamic_lighting::m_compat_profile), 0, 3));
		if (contains("runtime_budgets_enabled")) dynamic_lighting::m_runtime_budgets_enabled = read_bool(value("runtime_budgets_enabled"), dynamic_lighting::m_runtime_budgets_enabled);
		if (contains("authoring_debug_tools")) dynamic_lighting::m_authoring_debug_tools = read_bool(value("authoring_debug_tools"), dynamic_lighting::m_authoring_debug_tools);
		if (contains("max_active_lights")) dynamic_lighting::m_runtime_max_active_lights = static_cast<std::uint32_t>(std::max(0, read_int(value("max_active_lights"), static_cast<int>(dynamic_lighting::m_runtime_max_active_lights))));
		if (contains("max_pending_lights")) dynamic_lighting::m_runtime_max_pending_lights = static_cast<std::uint32_t>(std::max(0, read_int(value("max_pending_lights"), static_cast<int>(dynamic_lighting::m_runtime_max_pending_lights))));
		if (contains("max_spawns_per_second")) dynamic_lighting::m_runtime_max_spawns_per_second = static_cast<std::uint32_t>(std::max(0, read_int(value("max_spawns_per_second"), static_cast<int>(dynamic_lighting::m_runtime_max_spawns_per_second))));
		if (contains("max_muzzle_per_second")) dynamic_lighting::m_runtime_max_muzzle_per_second = static_cast<std::uint32_t>(std::max(0, read_int(value("max_muzzle_per_second"), static_cast<int>(dynamic_lighting::m_runtime_max_muzzle_per_second))));
		if (contains("max_sound_hash_per_second")) dynamic_lighting::m_runtime_max_sound_hash_per_second = static_cast<std::uint32_t>(std::max(0, read_int(value("max_sound_hash_per_second"), static_cast<int>(dynamic_lighting::m_runtime_max_sound_hash_per_second))));
		if (contains("leaf_check_hz")) dynamic_lighting::m_runtime_leaf_check_hz = std::clamp(read_float(value("leaf_check_hz"), dynamic_lighting::m_runtime_leaf_check_hz), 0.0f, 60.0f);
		if (contains("cpu_skin_throttle")) dynamic_lighting::m_cpu_skin_throttle_enabled = read_bool(value("cpu_skin_throttle"), dynamic_lighting::m_cpu_skin_throttle_enabled);
		if (contains("cpu_skin_skip_common")) dynamic_lighting::m_cpu_skin_skip_far_common = read_bool(value("cpu_skin_skip_common"), dynamic_lighting::m_cpu_skin_skip_far_common);
		if (contains("cpu_skin_skip_ragdolls")) dynamic_lighting::m_cpu_skin_skip_far_ragdolls = read_bool(value("cpu_skin_skip_ragdolls"), dynamic_lighting::m_cpu_skin_skip_far_ragdolls);
		if (contains("cpu_skin_common_distance")) dynamic_lighting::m_cpu_skin_common_skip_distance = std::max(0.0f, read_float(value("cpu_skin_common_distance"), dynamic_lighting::m_cpu_skin_common_skip_distance));
		if (contains("cpu_skin_ragdoll_distance")) dynamic_lighting::m_cpu_skin_ragdoll_skip_distance = std::max(0.0f, read_float(value("cpu_skin_ragdoll_distance"), dynamic_lighting::m_cpu_skin_ragdoll_skip_distance));

		if (contains("flashlight_governor_enabled")) dynamic_lighting::m_flashlight_governor_enabled = read_bool(value("flashlight_governor_enabled"), dynamic_lighting::m_flashlight_governor_enabled);
		if (contains("flashlight_update_hz")) dynamic_lighting::m_flashlight_update_hz = std::clamp(read_float(value("flashlight_update_hz"), dynamic_lighting::m_flashlight_update_hz), 1.0f, 240.0f);
		if (contains("flashlight_motion_epsilon")) dynamic_lighting::m_flashlight_motion_epsilon = std::max(0.0f, read_float(value("flashlight_motion_epsilon"), dynamic_lighting::m_flashlight_motion_epsilon));
		if (contains("flashlight_direction_epsilon_degrees")) dynamic_lighting::m_flashlight_direction_epsilon_degrees = std::clamp(read_float(value("flashlight_direction_epsilon_degrees"), dynamic_lighting::m_flashlight_direction_epsilon_degrees), 0.0f, 45.0f);
		if (contains("flashlight_force_update_distance")) dynamic_lighting::m_flashlight_force_update_distance = std::max(0.0f, read_float(value("flashlight_force_update_distance"), dynamic_lighting::m_flashlight_force_update_distance));
		if (contains("flashlight_player_layer_limit")) dynamic_lighting::m_flashlight_player_layer_limit = static_cast<std::uint32_t>(std::clamp(read_int(value("flashlight_player_layer_limit"), static_cast<int>(dynamic_lighting::m_flashlight_player_layer_limit)), 0, 4));
		if (contains("flashlight_bot_layer_limit")) dynamic_lighting::m_flashlight_bot_layer_limit = static_cast<std::uint32_t>(std::clamp(read_int(value("flashlight_bot_layer_limit"), static_cast<int>(dynamic_lighting::m_flashlight_bot_layer_limit)), 0, 4));
		if (contains("flashlight_budget_reserve")) dynamic_lighting::m_flashlight_budget_reserve = static_cast<std::uint32_t>(std::clamp(read_int(value("flashlight_budget_reserve"), static_cast<int>(dynamic_lighting::m_flashlight_budget_reserve)), 0, 128));
		if (contains("flashlight_cull_distant_bots")) dynamic_lighting::m_flashlight_cull_distant_bots = read_bool(value("flashlight_cull_distant_bots"), dynamic_lighting::m_flashlight_cull_distant_bots);
		if (contains("flashlight_bot_cull_distance")) dynamic_lighting::m_flashlight_bot_cull_distance = std::max(0.0f, read_float(value("flashlight_bot_cull_distance"), dynamic_lighting::m_flashlight_bot_cull_distance));
		if (contains("flashlight_nearest_bot_priority")) dynamic_lighting::m_flashlight_nearest_bot_priority = read_bool(value("flashlight_nearest_bot_priority"), dynamic_lighting::m_flashlight_nearest_bot_priority);
		if (contains("flashlight_preserve_last_good_rig")) dynamic_lighting::m_flashlight_preserve_last_good_rig = read_bool(value("flashlight_preserve_last_good_rig"), dynamic_lighting::m_flashlight_preserve_last_good_rig);
		if (contains("flashlight_verify_draw_results")) dynamic_lighting::m_flashlight_verify_draw_results = read_bool(value("flashlight_verify_draw_results"), dynamic_lighting::m_flashlight_verify_draw_results);
		if (contains("flashlight_owner_grace_frames")) dynamic_lighting::m_flashlight_owner_grace_frames = static_cast<std::uint32_t>(std::clamp(read_int(value("flashlight_owner_grace_frames"), static_cast<int>(dynamic_lighting::m_flashlight_owner_grace_frames)), 0, 30));
		if (contains("flashlight_retry_base_ms")) dynamic_lighting::m_flashlight_retry_base_ms = static_cast<std::uint32_t>(std::clamp(read_int(value("flashlight_retry_base_ms"), static_cast<int>(dynamic_lighting::m_flashlight_retry_base_ms)), 10, 5000));
		if (contains("flashlight_retry_max_ms")) dynamic_lighting::m_flashlight_retry_max_ms = static_cast<std::uint32_t>(std::clamp(read_int(value("flashlight_retry_max_ms"), static_cast<int>(dynamic_lighting::m_flashlight_retry_max_ms)), 10, 30000));
		if (dynamic_lighting::m_flashlight_retry_max_ms < dynamic_lighting::m_flashlight_retry_base_ms) dynamic_lighting::m_flashlight_retry_max_ms = dynamic_lighting::m_flashlight_retry_base_ms;

		if (contains("source_runtime_profile")) dynamic_lighting::m_source_runtime_profile = std::clamp(read_int(value("source_runtime_profile"), dynamic_lighting::m_source_runtime_profile), 0, 3);
		if (contains("source_runtime_allow_world")) dynamic_lighting::m_source_runtime_allow_world = read_bool(value("source_runtime_allow_world"), dynamic_lighting::m_source_runtime_allow_world);
		if (contains("source_runtime_allow_survivors")) dynamic_lighting::m_source_runtime_allow_survivors = read_bool(value("source_runtime_allow_survivors"), dynamic_lighting::m_source_runtime_allow_survivors);
		if (contains("source_runtime_allow_infected")) dynamic_lighting::m_source_runtime_allow_infected = read_bool(value("source_runtime_allow_infected"), dynamic_lighting::m_source_runtime_allow_infected);
		if (contains("source_runtime_allow_unknown_characters")) dynamic_lighting::m_source_runtime_allow_unknown_characters = read_bool(value("source_runtime_allow_unknown_characters"), dynamic_lighting::m_source_runtime_allow_unknown_characters);
		if (contains("source_runtime_allow_dlights")) dynamic_lighting::m_source_runtime_allow_dlights = read_bool(value("source_runtime_allow_dlights"), dynamic_lighting::m_source_runtime_allow_dlights);
		if (contains("source_runtime_allow_elights")) dynamic_lighting::m_source_runtime_allow_elights = read_bool(value("source_runtime_allow_elights"), dynamic_lighting::m_source_runtime_allow_elights);
		if (contains("source_runtime_allow_entity_dynamic")) dynamic_lighting::m_source_runtime_allow_entity_dynamic = read_bool(value("source_runtime_allow_entity_dynamic"), dynamic_lighting::m_source_runtime_allow_entity_dynamic);
		if (contains("source_runtime_allow_projected")) dynamic_lighting::m_source_runtime_allow_projected = read_bool(value("source_runtime_allow_projected"), dynamic_lighting::m_source_runtime_allow_projected);
		if (contains("source_runtime_allow_point_spotlight")) dynamic_lighting::m_source_runtime_allow_point_spotlight = read_bool(value("source_runtime_allow_point_spotlight"), dynamic_lighting::m_source_runtime_allow_point_spotlight);
		if (contains("source_runtime_world_intensity_scale")) dynamic_lighting::m_source_runtime_world_intensity_scale = std::clamp(read_float(value("source_runtime_world_intensity_scale"), dynamic_lighting::m_source_runtime_world_intensity_scale), 0.0f, 8.0f);
		if (contains("source_runtime_character_intensity_scale")) dynamic_lighting::m_source_runtime_character_intensity_scale = std::clamp(read_float(value("source_runtime_character_intensity_scale"), dynamic_lighting::m_source_runtime_character_intensity_scale), 0.0f, 8.0f);
		if (contains("source_runtime_world_radius_scale")) dynamic_lighting::m_source_runtime_world_radius_scale = std::clamp(read_float(value("source_runtime_world_radius_scale"), dynamic_lighting::m_source_runtime_world_radius_scale), 0.0f, 8.0f);
		if (contains("source_runtime_character_radius_scale")) dynamic_lighting::m_source_runtime_character_radius_scale = std::clamp(read_float(value("source_runtime_character_radius_scale"), dynamic_lighting::m_source_runtime_character_radius_scale), 0.0f, 8.0f);
		if (contains("source_runtime_max_radius")) dynamic_lighting::m_source_runtime_max_radius = std::clamp(read_float(value("source_runtime_max_radius"), dynamic_lighting::m_source_runtime_max_radius), 0.10f, 64.0f);
		if (contains("source_runtime_character_max_radius")) dynamic_lighting::m_source_runtime_character_max_radius = std::clamp(read_float(value("source_runtime_character_max_radius"), dynamic_lighting::m_source_runtime_character_max_radius), 0.10f, 32.0f);
		if (contains("source_runtime_character_spot_max_angle")) dynamic_lighting::m_source_runtime_character_spot_max_angle = std::clamp(read_float(value("source_runtime_character_spot_max_angle"), dynamic_lighting::m_source_runtime_character_spot_max_angle), 1.0f, 179.0f);
		if (contains("source_runtime_show_rejected")) dynamic_lighting::m_source_runtime_show_rejected_in_registry = read_bool(value("source_runtime_show_rejected"), dynamic_lighting::m_source_runtime_show_rejected_in_registry);
		if (contains("map_event_lights_enabled")) dynamic_lighting::m_map_event_lights_enabled = read_bool(value("map_event_lights_enabled"), dynamic_lighting::m_map_event_lights_enabled);
		if (contains("map_event_accept_input")) dynamic_lighting::m_map_event_accept_input = read_bool(value("map_event_accept_input"), dynamic_lighting::m_map_event_accept_input);
		if (contains("map_event_follow_lightstyles")) dynamic_lighting::m_map_event_follow_lightstyles = read_bool(value("map_event_follow_lightstyles"), dynamic_lighting::m_map_event_follow_lightstyles);
		if (contains("map_event_include_sprite_proxies")) dynamic_lighting::m_map_event_include_sprite_proxies = read_bool(value("map_event_include_sprite_proxies"), dynamic_lighting::m_map_event_include_sprite_proxies);
		if (contains("map_event_honor_starts_disabled")) dynamic_lighting::m_map_event_honor_starts_disabled = read_bool(value("map_event_honor_starts_disabled"), dynamic_lighting::m_map_event_honor_starts_disabled);
		if (contains("map_event_surface_material_linkage")) dynamic_lighting::m_map_event_surface_material_linkage = read_bool(value("map_event_surface_material_linkage"), dynamic_lighting::m_map_event_surface_material_linkage);

		if (contains("material_resolve_source_assets")) material_exporter::m_resolve_source_assets_during_capture = read_bool(value("material_resolve_source_assets"), material_exporter::m_resolve_source_assets_during_capture);
		if (contains("material_require_asset_backed_activation")) material_exporter::m_require_asset_backed_activation = read_bool(value("material_require_asset_backed_activation"), material_exporter::m_require_asset_backed_activation);
		if (contains("material_allow_live_sampler_fallback")) material_exporter::m_allow_live_sampler_fallback = read_bool(value("material_allow_live_sampler_fallback"), material_exporter::m_allow_live_sampler_fallback);
		if (contains("material_rescan_unresolved_periodically")) material_exporter::m_rescan_unresolved_assets_periodically = read_bool(value("material_rescan_unresolved_periodically"), material_exporter::m_rescan_unresolved_assets_periodically);
		if (contains("material_export_original_vtf")) material_exporter::m_export_raw_vtf = read_bool(value("material_export_original_vtf"), material_exporter::m_export_raw_vtf);
		if (contains("material_source_bridge_enabled")) material_exporter::m_source_material_bridge_enabled = read_bool(value("material_source_bridge_enabled"), material_exporter::m_source_material_bridge_enabled);
		if (contains("material_model_draw_packets_enabled")) material_exporter::m_model_material_packets_enabled = read_bool(value("material_model_draw_packets_enabled"), material_exporter::m_model_material_packets_enabled);
		if (contains("material_source_shader_semantics")) material_exporter::m_enable_source_shader_semantics = read_bool(value("material_source_shader_semantics"), material_exporter::m_enable_source_shader_semantics);
		if (contains("material_resolve_model_cdmaterials")) material_exporter::m_resolve_model_cdmaterials = read_bool(value("material_resolve_model_cdmaterials"), material_exporter::m_resolve_model_cdmaterials);
		if (contains("material_inspect_vtf_metadata")) material_exporter::m_inspect_vtf_metadata = read_bool(value("material_inspect_vtf_metadata"), material_exporter::m_inspect_vtf_metadata);
		if (contains("material_preserve_dynamic_for_review")) material_exporter::m_preserve_dynamic_materials_for_review = read_bool(value("material_preserve_dynamic_for_review"), material_exporter::m_preserve_dynamic_materials_for_review);
		if (contains("material_allow_envmapmask_conflict_fallback")) material_exporter::m_allow_envmapmask_conflict_fallback = read_bool(value("material_allow_envmapmask_conflict_fallback"), material_exporter::m_allow_envmapmask_conflict_fallback);
		if (contains("material_use_detail_as_pbr_microdetail")) material_exporter::m_use_detail_as_pbr_microdetail = read_bool(value("material_use_detail_as_pbr_microdetail"), material_exporter::m_use_detail_as_pbr_microdetail);
		if (contains("material_export_semantic_manifest")) material_exporter::m_export_semantic_manifest = read_bool(value("material_export_semantic_manifest"), material_exporter::m_export_semantic_manifest);
		if (contains("material_patch_vmt_resolution")) material_exporter::m_enable_patch_vmt_resolution = read_bool(value("material_patch_vmt_resolution"), material_exporter::m_enable_patch_vmt_resolution);
		if (contains("material_calibrated_specular")) material_exporter::m_enable_calibrated_specular = read_bool(value("material_calibrated_specular"), material_exporter::m_enable_calibrated_specular);
		if (contains("material_compose_detail_layers")) material_exporter::m_compose_detail_layers = read_bool(value("material_compose_detail_layers"), material_exporter::m_compose_detail_layers);
		if (contains("material_preserve_world_vertex_transition")) material_exporter::m_preserve_world_vertex_transition = read_bool(value("material_preserve_world_vertex_transition"), material_exporter::m_preserve_world_vertex_transition);
		if (contains("material_link_event_emissive")) material_exporter::m_link_event_emissive_materials = read_bool(value("material_link_event_emissive"), material_exporter::m_link_event_emissive_materials);
	}

	std::uint64_t game_settings::calculate_settings_fingerprint()
	{
		std::uint64_t hash = k_fnv_offset;
		static_assert(sizeof(var_definitions) % sizeof(variable) == 0, "game_settings variables must remain contiguous");
		const std::uint32_t setting_count = sizeof(var_definitions) / sizeof(variable);
		for (std::uint32_t i = 0u; i < setting_count; ++i)
		{
			const auto* var = reinterpret_cast<const variable*>(reinterpret_cast<const char*>(&vars) + i * sizeof(variable));
			const auto name = std::string_view(var->m_name ? var->m_name : "");
			hash_bytes(hash, name.data(), name.size());
			const char* value_ptr = var->get_str_value();
			const auto value = std::string_view(value_ptr ? value_ptr : "");
			hash_bytes(hash, value.data(), value.size());
		}

		hash_value(hash, dynamic_lighting::m_auto_muzzle_flash);
		hash_value(hash, dynamic_lighting::m_muzzle_flash_origin_mode);
		hash_value(hash, dynamic_lighting::m_muzzle_flash_shape_mode);
		hash_value(hash, dynamic_lighting::m_muzzle_weapon_profile_mode);
		hash_value(hash, dynamic_lighting::m_muzzle_weapon_profiles_enabled);
		hash_value(hash, dynamic_lighting::m_muzzle_flash_scalar);
		hash_value(hash, dynamic_lighting::m_muzzle_flash_radius);
		hash_value(hash, dynamic_lighting::m_muzzle_flash_duration);
		hash_value(hash, dynamic_lighting::m_muzzle_flash_fade);
		hash_value(hash, dynamic_lighting::m_muzzle_flash_delay);
		hash_value(hash, dynamic_lighting::m_muzzle_flash_color);
		hash_value(hash, dynamic_lighting::m_muzzle_flash_cooldown);
		hash_value(hash, dynamic_lighting::m_muzzle_flash_offset);
		hash_value(hash, dynamic_lighting::m_muzzle_draw_debug);
		hash_value(hash, dynamic_lighting::m_muzzle_strict_fire_tokens);
		hash_value(hash, dynamic_lighting::m_muzzle_reject_weapon_handling);
		hash_value(hash, dynamic_lighting::m_muzzle_allow_weapon_family_fallback);
		hash_value(hash, dynamic_lighting::m_compat_profile);
		hash_value(hash, dynamic_lighting::m_runtime_budgets_enabled);
		hash_value(hash, dynamic_lighting::m_authoring_debug_tools);
		hash_value(hash, dynamic_lighting::m_runtime_max_active_lights);
		hash_value(hash, dynamic_lighting::m_runtime_max_pending_lights);
		hash_value(hash, dynamic_lighting::m_runtime_max_spawns_per_second);
		hash_value(hash, dynamic_lighting::m_runtime_max_muzzle_per_second);
		hash_value(hash, dynamic_lighting::m_runtime_max_sound_hash_per_second);
		hash_value(hash, dynamic_lighting::m_runtime_leaf_check_hz);
		hash_value(hash, dynamic_lighting::m_cpu_skin_throttle_enabled);
		hash_value(hash, dynamic_lighting::m_cpu_skin_skip_far_common);
		hash_value(hash, dynamic_lighting::m_cpu_skin_skip_far_ragdolls);
		hash_value(hash, dynamic_lighting::m_cpu_skin_common_skip_distance);
		hash_value(hash, dynamic_lighting::m_cpu_skin_ragdoll_skip_distance);
		hash_value(hash, dynamic_lighting::m_flashlight_governor_enabled);
		hash_value(hash, dynamic_lighting::m_flashlight_update_hz);
		hash_value(hash, dynamic_lighting::m_flashlight_motion_epsilon);
		hash_value(hash, dynamic_lighting::m_flashlight_direction_epsilon_degrees);
		hash_value(hash, dynamic_lighting::m_flashlight_force_update_distance);
		hash_value(hash, dynamic_lighting::m_flashlight_player_layer_limit);
		hash_value(hash, dynamic_lighting::m_flashlight_bot_layer_limit);
		hash_value(hash, dynamic_lighting::m_flashlight_budget_reserve);
		hash_value(hash, dynamic_lighting::m_flashlight_cull_distant_bots);
		hash_value(hash, dynamic_lighting::m_flashlight_bot_cull_distance);
		hash_value(hash, dynamic_lighting::m_flashlight_nearest_bot_priority);
		hash_value(hash, dynamic_lighting::m_flashlight_preserve_last_good_rig);
		hash_value(hash, dynamic_lighting::m_flashlight_verify_draw_results);
		hash_value(hash, dynamic_lighting::m_flashlight_owner_grace_frames);
		hash_value(hash, dynamic_lighting::m_flashlight_retry_base_ms);
		hash_value(hash, dynamic_lighting::m_flashlight_retry_max_ms);
		hash_value(hash, dynamic_lighting::m_source_runtime_profile);
		hash_value(hash, dynamic_lighting::m_source_runtime_allow_world);
		hash_value(hash, dynamic_lighting::m_source_runtime_allow_survivors);
		hash_value(hash, dynamic_lighting::m_source_runtime_allow_infected);
		hash_value(hash, dynamic_lighting::m_source_runtime_allow_unknown_characters);
		hash_value(hash, dynamic_lighting::m_source_runtime_allow_dlights);
		hash_value(hash, dynamic_lighting::m_source_runtime_allow_elights);
		hash_value(hash, dynamic_lighting::m_source_runtime_allow_entity_dynamic);
		hash_value(hash, dynamic_lighting::m_source_runtime_allow_projected);
		hash_value(hash, dynamic_lighting::m_source_runtime_allow_point_spotlight);
		hash_value(hash, dynamic_lighting::m_source_runtime_world_intensity_scale);
		hash_value(hash, dynamic_lighting::m_source_runtime_character_intensity_scale);
		hash_value(hash, dynamic_lighting::m_source_runtime_world_radius_scale);
		hash_value(hash, dynamic_lighting::m_source_runtime_character_radius_scale);
		hash_value(hash, dynamic_lighting::m_source_runtime_max_radius);
		hash_value(hash, dynamic_lighting::m_source_runtime_character_max_radius);
		hash_value(hash, dynamic_lighting::m_source_runtime_character_spot_max_angle);
		hash_value(hash, dynamic_lighting::m_source_runtime_show_rejected_in_registry);
		hash_value(hash, dynamic_lighting::m_map_event_lights_enabled);
		hash_value(hash, dynamic_lighting::m_map_event_accept_input);
		hash_value(hash, dynamic_lighting::m_map_event_follow_lightstyles);
		hash_value(hash, dynamic_lighting::m_map_event_include_sprite_proxies);
		hash_value(hash, dynamic_lighting::m_map_event_honor_starts_disabled);
		hash_value(hash, dynamic_lighting::m_map_event_surface_material_linkage);
		hash_value(hash, material_exporter::m_resolve_source_assets_during_capture);
		hash_value(hash, material_exporter::m_require_asset_backed_activation);
		hash_value(hash, material_exporter::m_allow_live_sampler_fallback);
		hash_value(hash, material_exporter::m_rescan_unresolved_assets_periodically);
		hash_value(hash, material_exporter::m_export_raw_vtf);
		hash_value(hash, material_exporter::m_source_material_bridge_enabled);
		hash_value(hash, material_exporter::m_model_material_packets_enabled);
		hash_value(hash, material_exporter::m_enable_source_shader_semantics);
		hash_value(hash, material_exporter::m_resolve_model_cdmaterials);
		hash_value(hash, material_exporter::m_inspect_vtf_metadata);
		hash_value(hash, material_exporter::m_preserve_dynamic_materials_for_review);
		hash_value(hash, material_exporter::m_allow_envmapmask_conflict_fallback);
		hash_value(hash, material_exporter::m_use_detail_as_pbr_microdetail);
		hash_value(hash, material_exporter::m_export_semantic_manifest);
		hash_value(hash, material_exporter::m_enable_patch_vmt_resolution);
		hash_value(hash, material_exporter::m_enable_calibrated_specular);
		hash_value(hash, material_exporter::m_compose_detail_layers);
		hash_value(hash, material_exporter::m_preserve_world_vertex_transition);
		hash_value(hash, material_exporter::m_link_event_emissive_materials);
		return hash;
	}

	void game_settings::mark_dirty(const char* reason)
	{
		const auto now = std::chrono::steady_clock::now();
		if (!m_dirty) m_dirty_since = now;
		m_dirty = true;
		m_last_change_time = now;
		m_last_dirty_reason = reason && *reason ? reason : "runtime UI change";
		m_last_save_status = std::format("Pending auto-save ({})", m_last_dirty_reason);
	}

	void game_settings::apply_persistent_quick_actions()
	{
		using clock = std::chrono::steady_clock;
		static std::string active_map;
		static clock::time_point director_due = clock::now();
		static clock::time_point bot_kick_due = clock::now();

		const auto* intf = interfaces::get();
		if (!intf || !intf->m_engine || !intf->m_engine->is_connected() || !intf->m_engine->is_in_game())
		{
			active_map.clear();
			return;
		}

		const auto now = clock::now();
		const std::string current_map = map_settings::get_map_name();
		if (current_map != active_map)
		{
			active_map = current_map;
			director_due = now + std::chrono::milliseconds(800);
			bot_kick_due = now + std::chrono::milliseconds(2500);
		}

		if (vars.quick_auto_stop_director.get_as<bool>() && now >= director_due)
		{
			intf->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; director_stop");
			director_due = now + std::chrono::seconds(5);
		}

		if (vars.quick_auto_kick_bots.get_as<bool>() && now >= bot_kick_due)
		{
			intf->m_engine->execute_client_cmd_unrestricted(
				"sv_cheats 1; kick rochelle; kick coach; kick ellis; kick nick; kick louis; kick zoey; kick francis; kick bill");
			bot_kick_due = now + std::chrono::seconds(8);
		}
	}

	void game_settings::on_frame()
	{
		if (!m_loaded) return;
		apply_persistent_quick_actions();
		using clock = std::chrono::steady_clock;
		static auto next_fingerprint_check = clock::now();
		const auto now = clock::now();

		// A complete fingerprint formats every variable. Running that scan on every
		// render frame caused a regular hitch which was most visible with the F5 menu
		// open. Poll only while the UI can edit settings and use a one-second cadence.
		const auto* im = imgui::get();
		if (im && im->m_menu_active && now >= next_fingerprint_check)
		{
			next_fingerprint_check = now + std::chrono::seconds(1);
			const auto fingerprint = calculate_settings_fingerprint();
			if (fingerprint != m_last_fingerprint)
			{
				m_last_fingerprint = fingerprint;
				mark_dirty("detected UI/runtime change");
			}
		}

		if (!m_dirty) return;
		if (now - m_last_change_time < m_autosave_debounce) return;
		write_toml();
	}

	void game_settings::write_toml()
	{
		const auto final_path = settings_path();
		const auto temp_path = final_path.wstring() + L".tmp";
		std::error_code ec;
		std::filesystem::create_directories(final_path.parent_path(), ec);

		std::ofstream file(std::filesystem::path(temp_path), std::ios::binary | std::ios::trunc);
		if (!file.is_open())
		{
			m_last_save_succeeded = false;
			m_last_save_status = "Auto-save failed: cannot open temporary file";
			m_last_change_time = std::chrono::steady_clock::now();
			return;
		}

		// V21.8 compatibility baseline marker: L4D2 RTX Compatibility Mod V21.8
		file << "# L4D2 RTX Compatibility Mod V21.11\n";
		file << "# Auto-generated with debounced atomic persistence. Valid manual edits remain supported.\n\n";

		static_assert(sizeof(var_definitions) % sizeof(variable) == 0, "game_settings variables must remain contiguous");
		const std::uint32_t setting_count = sizeof(var_definitions) / sizeof(variable);
		for (std::uint32_t i = 0u; i < setting_count; ++i)
		{
			auto* var = reinterpret_cast<variable*>(reinterpret_cast<char*>(&vars) + i * sizeof(variable));
			file << "# " << var->m_desc << '\n';
			file << "# Type: " << var->get_str_type() << " || Default: " << var->get_str_value(true) << '\n';
			file << var->m_name << " = " << var->get_str_value() << "\n\n";
		}
		write_runtime_ui_settings(file);
		file.flush();
		const bool stream_ok = file.good();
		file.close();

		if (!stream_ok)
		{
			std::filesystem::remove(std::filesystem::path(temp_path), ec);
			m_last_save_succeeded = false;
			m_last_save_status = "Auto-save failed: temporary file write error";
			m_last_change_time = std::chrono::steady_clock::now();
			return;
		}

		if (!atomic_replace_file(std::filesystem::path(temp_path), final_path))
		{
			const auto error = GetLastError();
			std::filesystem::remove(std::filesystem::path(temp_path), ec);
			m_last_save_succeeded = false;
			m_last_save_status = std::format("Auto-save failed: atomic replace error {}", error);
			m_last_change_time = std::chrono::steady_clock::now();
			return;
		}

		m_dirty = false;
		m_last_save_succeeded = true;
		++m_save_generation;
		m_last_save_status = std::format("Saved atomically (generation {})", m_save_generation);
		m_last_fingerprint = calculate_settings_fingerprint();
	}

	void game_settings::restore_defaults_no_save()
	{
		static_assert(sizeof(var_definitions) % sizeof(variable) == 0, "game_settings variables must remain contiguous");
		const std::uint32_t setting_count = sizeof(var_definitions) / sizeof(variable);
		for (std::uint32_t i = 0u; i < setting_count; ++i) {
			auto* var = reinterpret_cast<variable*>(reinterpret_cast<char*>(&vars) + i * sizeof(variable));
			var->reset_to_default(true);
		}

		dynamic_lighting::m_auto_muzzle_flash = true;
		dynamic_lighting::m_muzzle_flash_origin_mode = 0;
		dynamic_lighting::m_muzzle_flash_shape_mode = 0;
		dynamic_lighting::m_muzzle_weapon_profile_mode = 0;
		dynamic_lighting::m_muzzle_weapon_profiles_enabled = true;
		dynamic_lighting::m_muzzle_flash_scalar = 0.07f;
		dynamic_lighting::m_muzzle_flash_radius = 0.02f;
		dynamic_lighting::m_muzzle_flash_duration = 0.300f;
		dynamic_lighting::m_muzzle_flash_fade = 0.090f;
		dynamic_lighting::m_muzzle_flash_delay = 0.000f;
		dynamic_lighting::m_muzzle_flash_color = Vector(1.0f, 0.68f, 0.30f);
		dynamic_lighting::m_muzzle_flash_cooldown = 0.018f;
		dynamic_lighting::m_muzzle_flash_offset = Vector(18.0f, -2.0f, -3.0f);
		dynamic_lighting::m_muzzle_draw_debug = false;
		dynamic_lighting::m_muzzle_strict_fire_tokens = false;
		dynamic_lighting::m_muzzle_reject_weapon_handling = true;
		dynamic_lighting::m_muzzle_allow_weapon_family_fallback = true;
		dynamic_lighting::apply_compat_profile(1);
		dynamic_lighting::m_flashlight_governor_enabled = false;
		dynamic_lighting::m_flashlight_update_hz = 60.0f;
		dynamic_lighting::m_flashlight_motion_epsilon = 0.10f;
		dynamic_lighting::m_flashlight_direction_epsilon_degrees = 0.10f;
		dynamic_lighting::m_flashlight_force_update_distance = 32.0f;
		dynamic_lighting::m_flashlight_player_layer_limit = 4u;
		dynamic_lighting::m_flashlight_bot_layer_limit = 2u;
		dynamic_lighting::m_flashlight_budget_reserve = 6u;
		dynamic_lighting::m_flashlight_cull_distant_bots = true;
		dynamic_lighting::m_flashlight_bot_cull_distance = 2200.0f;
		dynamic_lighting::m_flashlight_nearest_bot_priority = true;
		dynamic_lighting::m_flashlight_preserve_last_good_rig = true;
		dynamic_lighting::m_flashlight_verify_draw_results = false;
		dynamic_lighting::m_flashlight_owner_grace_frames = 3u;
		dynamic_lighting::m_flashlight_retry_base_ms = 100u;
		dynamic_lighting::m_flashlight_retry_max_ms = 2000u;
		dynamic_lighting::m_source_runtime_profile = 1;
		dynamic_lighting::m_source_runtime_allow_world = true;
		dynamic_lighting::m_source_runtime_allow_survivors = false;
		dynamic_lighting::m_source_runtime_allow_infected = false;
		dynamic_lighting::m_source_runtime_allow_unknown_characters = false;
		dynamic_lighting::m_source_runtime_allow_dlights = true;
		dynamic_lighting::m_source_runtime_allow_elights = true;
		dynamic_lighting::m_source_runtime_allow_entity_dynamic = true;
		dynamic_lighting::m_source_runtime_allow_projected = true;
		dynamic_lighting::m_source_runtime_allow_point_spotlight = true;
		dynamic_lighting::m_source_runtime_world_intensity_scale = 1.0f;
		dynamic_lighting::m_source_runtime_character_intensity_scale = 0.20f;
		dynamic_lighting::m_source_runtime_world_radius_scale = 1.0f;
		dynamic_lighting::m_source_runtime_character_radius_scale = 0.25f;
		dynamic_lighting::m_source_runtime_max_radius = 8.0f;
		dynamic_lighting::m_source_runtime_character_max_radius = 1.25f;
		dynamic_lighting::m_source_runtime_character_spot_max_angle = 55.0f;
		dynamic_lighting::m_source_runtime_show_rejected_in_registry = true;
		dynamic_lighting::m_map_event_lights_enabled = true;
		dynamic_lighting::m_map_event_accept_input = true;
		dynamic_lighting::m_map_event_follow_lightstyles = true;
		dynamic_lighting::m_map_event_include_sprite_proxies = true;
		dynamic_lighting::m_map_event_honor_starts_disabled = true;
		dynamic_lighting::m_map_event_surface_material_linkage = true;
		material_exporter::m_resolve_source_assets_during_capture = true;
		material_exporter::m_require_asset_backed_activation = true;
		material_exporter::m_allow_live_sampler_fallback = true;
		material_exporter::m_rescan_unresolved_assets_periodically = true;
		material_exporter::m_export_raw_vtf = true;
		material_exporter::m_source_material_bridge_enabled = true;
		material_exporter::m_model_material_packets_enabled = true;
		material_exporter::m_enable_source_shader_semantics = true;
		material_exporter::m_resolve_model_cdmaterials = true;
		material_exporter::m_inspect_vtf_metadata = true;
		material_exporter::m_preserve_dynamic_materials_for_review = true;
		material_exporter::m_allow_envmapmask_conflict_fallback = false;
		material_exporter::m_use_detail_as_pbr_microdetail = true;
		material_exporter::m_export_semantic_manifest = true;
		material_exporter::m_enable_patch_vmt_resolution = true;
		material_exporter::m_enable_calibrated_specular = true;
		material_exporter::m_compose_detail_layers = true;
		material_exporter::m_preserve_world_vertex_transition = true;
		material_exporter::m_link_event_emissive_materials = true;
	}

	bool game_settings::parse_toml()
	{
		m_dirty = false;
		restore_defaults_no_save();
		const auto path = settings_path();
		if (std::filesystem::exists(path))
		{
			try
			{
				auto config = toml::parse(path.string());
				int loaded_runtime_schema = 0;
				if (const auto* runtime = runtime_ui_table(config); runtime && runtime->contains("schema_version"))
				{
					loaded_runtime_schema = read_int(runtime->at("schema_version"), 0);
				}
				static_assert(sizeof(var_definitions) % sizeof(variable) == 0, "game_settings variables must remain contiguous");
				const std::uint32_t setting_count = sizeof(var_definitions) / sizeof(variable);
				for (std::uint32_t i = 0u; i < setting_count; ++i)
				{
					auto* var = reinterpret_cast<variable*>(reinterpret_cast<char*>(&vars) + i * sizeof(variable));
					if (!config.contains(var->m_name)) continue;
					const auto& entry = config.at(var->m_name);
					switch (var->get_type())
					{
					case var_type_boolean:
						var->set_var(read_bool(entry, var->get_as<bool>()), true);
						break;
					case var_type_integer:
						var->set_var(read_int(entry, var->get_as<int>()), true);
						break;
					case var_type_value:
						var->set_var(read_float(entry, var->get_as<float>()), true);
						break;
					case var_type_vec2:
					case var_type_vec3:
					case var_type_vec4:
					{
						const std::size_t count = var->get_type() == var_type_vec2 ? 2u : var->get_type() == var_type_vec3 ? 3u : 4u;
						const auto vec = read_vector(entry, count, var->get_as<float*>());
						var->set_vec(vec.data(), true);
						break;
					}
					}
				}
				parse_runtime_ui_settings(config);

				// V21.4 migration: V21.3.1 shipped an oversized Core Sphere default
				// (radius 0.6, intensity 50000). Only migrate that exact legacy pair,
				// preserving intentionally authored custom point-light settings.
				if (loaded_runtime_schema < 25 &&
					std::abs(vars.flashlight_inner_radius.get_as<float>() - 0.6f) < 0.0001f &&
					std::abs(vars.flashlight_inner_intensity.get_as<float>() - 50000.0f) < 1.0f)
				{
					vars.flashlight_inner_radius.set_var(0.10f, true);
					vars.flashlight_inner_intensity.set_var(9000.0f, true);
					mark_dirty("migrate legacy flashlight core point radius");
				}

				// V21.13.2 migration: V21.13.0/1 enabled strict token-only detection by default.
				// That rejected valid Source samples such as pump_shotgun_fire before the positive
				// token was evaluated. Move untouched legacy configs to the balanced classifier.
				if (loaded_runtime_schema < 30 && dynamic_lighting::m_muzzle_strict_fire_tokens &&
					dynamic_lighting::m_muzzle_reject_weapon_handling)
				{
					dynamic_lighting::m_muzzle_strict_fire_tokens = false;
					dynamic_lighting::m_muzzle_allow_weapon_family_fallback = true;
					mark_dirty("migrate muzzle detection to balanced classifier");
				}

				// V21.14 migration: model materials now carry draw context through ABI v2.
				// Enable the bridge for existing untouched profiles so character, weapon
				// and viewmodel materials no longer stop at world-surface Auto PBR.
				if (loaded_runtime_schema < 31)
				{
					material_exporter::m_source_material_bridge_enabled = true;
					material_exporter::m_model_material_packets_enabled = true;
					mark_dirty("enable Studio model material bridge v2");
				}
			}
			catch (const toml::syntax_error& err)
			{
				game::console();
				printf("%s\n", err.what());
				const auto corrupt = path.wstring() + L".corrupt." + std::to_wstring(GetTickCount64());
				CopyFileW(path.wstring().c_str(), corrupt.c_str(), FALSE);
				restore_defaults_no_save();
				m_last_save_status = "Damaged config backed up; defaults restored";
				m_last_save_succeeded = false;
				mark_dirty("repair damaged configuration");
			}
			catch (const std::exception& err)
			{
				game::console();
				printf("[GameSettings] %s\n", err.what());
				const auto corrupt = path.wstring() + L".corrupt." + std::to_wstring(GetTickCount64());
				CopyFileW(path.wstring().c_str(), corrupt.c_str(), FALSE);
				restore_defaults_no_save();
				m_last_save_status = "Unreadable config backed up; defaults restored";
				m_last_save_succeeded = false;
				mark_dirty("repair unreadable configuration");
			}
		}
		else
		{
			mark_dirty("create initial configuration");
		}

		m_loaded = true;
		m_last_fingerprint = calculate_settings_fingerprint();
		if (m_dirty) write_toml();
		else {
			m_last_save_succeeded = true;
			m_last_save_status = "Loaded existing configuration";
		}
		return true;
	}

	void game_settings::reset_all_to_defaults()
	{
		restore_defaults_no_save();
		mark_dirty("reset all settings");
	}

	ConCommand xo_gamesettings_update {};
	void game_settings::xo_gamesettings_update_fn()
	{
		parse_toml();
		main_module::cross_handle_map_and_game_settings();
	}

	game_settings::game_settings()
	{
		parse_toml();
		game::con_add_command(&xo_gamesettings_update, "xo_gamesettings_update", xo_gamesettings_update_fn, "Reloads the game_settings.toml file");
	}
}
