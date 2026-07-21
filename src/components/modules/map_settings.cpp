#include "std_include.hpp"
#include "components/common/toml.hpp"

namespace components
{
#define CATCH_ERR	catch (toml::type_error& err) { game::console(); printf("%s\n", err.what()); return; }

	void map_settings::set_settings_for_map(const std::string& map_name)
	{
		m_map_settings.mapname = !map_name.empty() ? map_name : interfaces::get()->m_engine->get_level_name();
		utils::replace_all(m_map_settings.mapname, std::string("maps/"), "");		// if sp map
		utils::replace_all(m_map_settings.mapname, std::string(".bsp"), "");

		parse_toml();

		// V20.9: merge the persistent per-map light database after the normal map
		// configuration has been parsed but before any Remix lights are spawned.
		// This makes <map>.toml authoritative on subsequent loads while preserving
		// the legacy map_settings TOML as the base layer.
		dynamic_lighting::load_persistent_map_lights_into_settings();

		static bool disable_map_configs = flags::has_flag("xo_disable_map_conf");
		if (remix_api::is_initialized())
		{
			if (!disable_map_configs)
			{
				// resets all modified variables back to rtx.conf level
				remix_vars::reset_all_modified();

				// auto apply {map_name}.conf (if it exists)
				open_and_set_var_config(m_map_settings.mapname + ".conf", true);

				// apply other manually defined configs
				for (const auto& f : m_map_settings.api_var_configs) {
					open_and_set_var_config(f);
				} 
			}

			main_module::cross_handle_map_and_game_settings();

			// lights are spawned manually in edit mode
			if (!imgui::get()->m_light_edit_mode) {
				remix_lights::get()->add_all_map_setting_lights_without_creation_trigger();
			}
		}

		m_map_settings.default_nocull_dist = game_settings::get()->default_nocull_distance.get_as<float>();

		// are we using any sound hashes or names to trigger configvar transitions?
		{
			if (!m_map_settings.remix_transitions.empty())
			{
				for (const auto& t : m_map_settings.remix_transitions)
				{
					if (t.trigger_type == TRANSITION_TRIGGER_TYPE::SOUND && t.sound_hash) {
						m_map_settings.using_any_transition_sound_hash = true;
					}

					if (t.trigger_type == TRANSITION_TRIGGER_TYPE::SOUND && !t.sound_name.empty()) {
						m_map_settings.using_any_transition_sound_name = true;
					}
				}
			}
		}

		// are we using any sound hashes to trigger light spawning?
		{
			if (!m_map_settings.remix_lights.empty())
			{
				for (const auto& l : m_map_settings.remix_lights)
				{
					if (l.trigger_sound_hash || l.kill_sound_hash)
					{
						m_map_settings.using_any_light_sound_hash = true;
						break;
					}
				}
			}
		}

		// are we using any sound hashes/names or choreo trigger on markers?
		{
			if (!m_map_settings.map_markers.empty())
			{
				for (const auto& m : m_map_settings.map_markers)
				{
					if (m.trigger_show.sound_hash || m.trigger_hide.sound_hash)
					{
						m_map_settings.using_any_marker_sound_hash = true;
						break;
					}

					if (!m.trigger_show.sound_name.empty() || !m.trigger_hide.sound_name.empty())
					{
						m_map_settings.using_any_marker_sound_name = true;
						break;
					}

					if (!m.trigger_show.choreo_name.empty() || !m.trigger_hide.choreo_name.empty())
					{
						m_map_settings.using_any_marker_choreo = true;
						break;
					}
				}
			}
		}

		// are we using any dynamic light event triggers?
		{
			for (const auto& e : m_map_settings.dynamic_light_events)
			{
				if (!e.enabled) {
					continue;
				}

				if (e.trigger_type == DYN_LIGHT_TRIGGER_SOUND)
				{
					if (e.sound_hash) {
						m_map_settings.using_any_dynamic_light_sound_hash = true;
					}
					if (!e.sound_name.empty()) {
						m_map_settings.using_any_dynamic_light_sound_name = true;
					}
				}
				else if (e.trigger_type == DYN_LIGHT_TRIGGER_CHOREO) {
					m_map_settings.using_any_dynamic_light_choreo = true;
				}
				else if (e.trigger_type == DYN_LIGHT_TRIGGER_LEAF) {
					m_map_settings.using_any_dynamic_light_leaf = true;
				}
			}
		}

		m_loaded = true;
	}

	// cannot be called in the current on_map_load stub (too early)
	// called from 'once_per_frame_cb()' instead
	void map_settings::spawn_markers_once()
	{
		if (m_spawned_markers) return;

		const auto now = std::chrono::steady_clock::now();
		if (m_marker_next_spawn_attempt.time_since_epoch().count() != 0 && now < m_marker_next_spawn_attempt) return;

		bool has_pending_normal_markers = false;
		for (const auto& marker : m_map_settings.map_markers)
		{
			if (!marker.no_cull && !marker.handle && !marker.runtime_nocull_fallback)
			{
				has_pending_normal_markers = true;
				break;
			}
		}

		if (!has_pending_normal_markers)
		{
			m_spawned_markers = true;
			m_marker_spawn_status = std::format("complete: {} entity markers, {} runtime fallbacks",
				std::count_if(m_map_settings.map_markers.begin(), m_map_settings.map_markers.end(), [](const marker_settings_s& marker) { return marker.handle != nullptr; }),
				marker_fallback_count());
			return;
		}

		const bool dependencies_ready = game::server_tools_available() && l4d2::mdl_cache &&
			!utils::memory::is_bad_read_ptr(l4d2::mdl_cache);
		void* mdlcache = nullptr;
		if (dependencies_ready)
		{
			mdlcache = reinterpret_cast<void*>(*reinterpret_cast<DWORD*>(l4d2::mdl_cache));
		}

		if (!dependencies_ready || !mdlcache || utils::memory::is_bad_read_ptr(mdlcache))
		{
			++m_marker_dependency_waits;
			const auto delay_ms = std::min<std::uint32_t>(2000u, 250u << std::min<std::uint32_t>(m_marker_dependency_waits - 1u, 3u));
			m_marker_next_spawn_attempt = now + std::chrono::milliseconds(delay_ms);
			m_marker_spawn_status = std::format("waiting for marker dependencies ({}/4); next retry {} ms",
				m_marker_dependency_waits, delay_ms);

			// Do not keep probing server interfaces forever from RenderView. After a short
			// grace period preserve authored markers through the deterministic no-cull path.
			if (m_marker_dependency_waits >= 4u)
			{
				for (auto& marker : m_map_settings.map_markers)
				{
					if (!marker.no_cull && !marker.handle) marker.runtime_nocull_fallback = true;
				}
				m_spawned_markers = true;
				m_marker_spawn_status = std::format("server marker path unavailable; {} markers switched to runtime no-cull fallback",
					marker_fallback_count());
			}
			return;
		}

		m_marker_dependency_waits = 0u;
		++m_marker_spawn_attempts;
		std::uint32_t processed_this_batch = 0u;
		constexpr std::uint32_t max_markers_per_batch = 2u;

		for (auto& marker : m_map_settings.map_markers)
		{
			if (marker.no_cull || marker.handle || marker.runtime_nocull_fallback) continue;
			if (processed_this_batch >= max_markers_per_batch) break;
			++processed_this_batch;
			++marker.runtime_spawn_attempts;

			const auto mdl_num = marker.index / 10u;
			const auto skin_num = marker.index % 10u;
			const auto model_name = utils::va("models/props_xo/mapmarker%03d.mdl", mdl_num * 10);

			utils::hook::call_virtual<26, void>(mdlcache); // IMDLCache::BeginLock
			const auto mdl_handle = utils::hook::call_virtual<6, std::uint16_t>(mdlcache, model_name);
			utils::hook::call_virtual<27, void>(mdlcache); // IMDLCache::EndLock

			bool spawn_failed = mdl_handle == 0xFFFF;
			if (!spawn_failed)
			{
				marker.handle = game::server_tools_create_entity("dynamic_prop");
				spawn_failed = marker.handle == nullptr;
			}

			if (!spawn_failed)
			{
				utils::hook::call_virtual<34, void>(marker.handle, "origin", utils::va("%.10f %.10f %.10f", marker.origin[0], marker.origin[1], marker.origin[2]));
				utils::hook::call_virtual<34, void>(marker.handle, "angles", utils::va("%.10f %.10f %.10f", marker.rotation[0], marker.rotation[1], marker.rotation[2]));
				utils::hook::call_virtual<34, void>(marker.handle, "model", model_name);
				utils::hook::call_virtual<34, void>(marker.handle, "solid", "0");
				utils::hook::call_virtual<34, void>(marker.handle, "rendercolor", utils::va("%d %d %d",
					static_cast<int>(std::clamp(marker.color.x, 0.0f, 1.0f) * 255.0f),
					static_cast<int>(std::clamp(marker.color.y, 0.0f, 1.0f) * 255.0f),
					static_cast<int>(std::clamp(marker.color.z, 0.0f, 1.0f) * 255.0f)));

				struct skin_offset
				{
					char pad[1092];
					int m_nSkin;
				}; STATIC_ASSERT_OFFSET(skin_offset, m_nSkin, 1092);
				static_cast<skin_offset*>(marker.handle)->m_nSkin = static_cast<int>(skin_num);

				utils::hook::call_virtual<26, void>(marker.handle); // CBaseEntity::Precache
				if (!game::server_tools_dispatch_spawn(static_cast<CBaseEntity*>(marker.handle)))
				{
					game::cbaseentity_remove(marker.handle);
					marker.handle = nullptr;
					spawn_failed = true;
				}
			}

			if (spawn_failed)
			{
				++m_marker_spawn_failures;
				if (marker.runtime_spawn_attempts >= 3u)
				{
					marker.runtime_nocull_fallback = true;
				}
				continue;
			}

			utils::hook::call_virtual<36, void>(marker.handle); // CBaseEntity::Activate
			if (!marker.visible || marker.is_hidden)
			{
				constexpr int EF_NODRAW = 0x20;
				auto& effects = *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(marker.handle) + 0xE0);
				effects |= EF_NODRAW;
			}
		}

		bool all_handled = true;
		for (const auto& marker : m_map_settings.map_markers)
		{
			if (!marker.no_cull && !marker.handle && !marker.runtime_nocull_fallback)
			{
				all_handled = false;
				break;
			}
		}

		m_spawned_markers = all_handled;
		if (all_handled)
		{
			m_marker_spawn_status = std::format("complete after {} batches: {} entity markers, {} runtime fallbacks, {} failures",
				m_marker_spawn_attempts,
				std::count_if(m_map_settings.map_markers.begin(), m_map_settings.map_markers.end(), [](const marker_settings_s& marker) { return marker.handle != nullptr; }),
				marker_fallback_count(), m_marker_spawn_failures);
		}
		else
		{
			const auto delay_ms = std::min<std::uint32_t>(1500u, 250u * std::max<std::uint32_t>(1u, m_marker_spawn_attempts));
			m_marker_next_spawn_attempt = now + std::chrono::milliseconds(delay_ms);
			m_marker_spawn_status = std::format("marker batch {} pending; {} failures; next retry {} ms",
				m_marker_spawn_attempts, m_marker_spawn_failures, delay_ms);
		}
	}

	std::uint32_t map_settings::marker_fallback_count()
	{
		return static_cast<std::uint32_t>(std::count_if(m_map_settings.map_markers.begin(), m_map_settings.map_markers.end(),
			[](const marker_settings_s& marker) { return marker.runtime_nocull_fallback; }));
	}

	void map_settings::destroy_markers()
	{
		// destroy active markers
		for (auto& m : m_map_settings.map_markers)
		{
			if (m.handle)
			{
				game::cbaseentity_remove(m.handle);
				m.handle = nullptr;
			}
		}

		m_map_settings.map_markers.clear();
		m_spawned_markers = false;
		m_marker_spawn_attempts = 0u;
		m_marker_spawn_failures = 0u;
		m_marker_dependency_waits = 0u;
		m_marker_spawn_status = "not started";
		m_marker_next_spawn_attempt = {};
	}

	bool map_settings::parse_toml()
	{
		try 
		{
			auto config = toml::parse(COMPMOD_ASSET_DIR "map_settings.toml", toml::spec::v(1, 1, 0));

			// #
			auto to_float = [](const toml::value& entry, const float default_val = 0.0f)
				{
					if (entry.is_floating()) {
						return static_cast<float>(entry.as_floating());
					}

					if (entry.is_integer()) {
						return static_cast<float>(entry.as_integer());
					}

					try { // this will fail and let the user know whats wrong
						return static_cast<float>(entry.as_floating());
					}
					catch (toml::type_error& err) {
						game::console(); printf("%s\n", err.what());
					}

					return default_val;
				};

			// #
			auto to_int = [](const toml::value& entry, const int default_val = 0)
				{
					if (entry.is_floating())  {
						return static_cast<int>(entry.as_floating());
					}

					if (entry.is_integer()) {
						return static_cast<int>(entry.as_integer());
					}

					try { // this will fail and let the user know whats wrong
						return static_cast<int>(entry.as_integer());
					}
					catch (toml::type_error& err) {
						game::console(); printf("%s\n", err.what());
					}

					return default_val;
				};

			auto to_uint = [](const toml::value& entry, const std::uint32_t default_val = 0u)
				{
					if (entry.is_floating()) {
						return static_cast<std::uint32_t>(entry.as_floating());
					}

					if (entry.is_integer()) {
						return static_cast<std::uint32_t>(entry.as_integer());
					}

					try { // this will fail and let the user know whats wrong
						return static_cast<std::uint32_t>(entry.as_integer());
					}
					catch (toml::type_error& err) {
						game::console(); printf("%s\n", err.what());
					}

					return default_val;
				};

			// #
			auto to_bool = [](const toml::value& entry, const bool default_setting = false)
				{
					if (entry.is_boolean()) {
						return static_cast<bool>(entry.as_boolean());
					}

					if (entry.is_integer()) {
						return static_cast<bool>(entry.as_integer());
					}

					try { // this will fail and let the user know whats wrong
						return static_cast<bool>(entry.as_boolean());
					}
					catch (toml::type_error& err) {
						game::console(); printf("%s\n", err.what());
					}

					return default_setting;
				};

			// ####################
			// parse 'FOG' table
			if (config.contains("FOG"))
			{
				auto& fog_table = config["FOG"];

				// try to find the loaded map
				if (fog_table.contains(m_map_settings.mapname))
				{
					if (const auto map = fog_table[m_map_settings.mapname];
						!map.is_empty())
					{
						const bool has_distance = map.contains("distance");
						const bool has_density = map.contains("density");

						if ((has_distance || has_density) && map.contains("color"))
						{
							if (has_distance) {
								m_map_settings.fog_dist = to_float(map.at("distance"));
							}
							else if (has_density) {
								m_map_settings.fog_density = to_float(map.at("density"));
							}

							if (const auto& color = map.at("color").as_array(); 
								color.size() == 3)
							{
								const auto r = static_cast<std::uint8_t>(to_int(color[0]));
								const auto g = static_cast<std::uint8_t>(to_int(color[1]));
								const auto b = static_cast<std::uint8_t>(to_int(color[2]));
								m_map_settings.fog_color = D3DCOLOR_XRGB(r, g, b);
							}
						}
					}
				}
			} // end 'FOG'


			// ####################
			// parse 'WATER' table
			if (config.contains("WATER"))
			{
				auto& water_table = config["WATER"];

				// try to find the loaded map
				if (water_table.contains(m_map_settings.mapname))
				{
					if (const auto map = water_table[m_map_settings.mapname];
						!map.is_empty())
					{
						if (map.contains("scale")) {
							m_map_settings.water_uv_scale = to_float(map.at("scale"), 1.0f);
						}

						if (map.contains("top_layer_offset")) {
							m_map_settings.water_offset_top = to_float(map.at("top_layer_offset"), 0.5f);
						}

						if (map.contains("bottom_layer_offset")) {
							m_map_settings.water_offset_bottom = to_float(map.at("bottom_layer_offset"), 0.0f);
						}
					}
				}
			} // end 'WATER'


			// ####################
			// parse 'CULL' table
			if (config.contains("CULL"))
			{
				auto& cull_table = config["CULL"];

				// #
				auto process_cull_entry = [to_uint, to_float](const toml::value& entry)
					{
						const auto contains_leafs = entry.contains("leafs");
						const auto contains_areas = entry.contains("areas");
						const auto contains_leaf_tweak = entry.contains("leaf_tweak");
						const auto contains_hidden_leafs = entry.contains("hide_leafs");
						const auto contains_hidden_areas = entry.contains("hide_areas");
						const auto contains_cull = entry.contains("cull");

						if (entry.contains("in_area"))
						{
							const auto area = to_uint(entry.at("in_area"));

							// forced leafs
							std::unordered_set<std::uint32_t> leaf_set;
							if (contains_leafs)
							{
								auto& leafs = entry.at("leafs").as_array();

								for (const auto& leaf : leafs) {
									leaf_set.insert(to_uint(leaf));
								}
							}

							// forced areas
							std::unordered_set<std::uint32_t> area_set;
							if (contains_areas)
							{
								auto& areas = entry.at("areas").as_array();

								for (const auto& a : areas) {
									area_set.insert(to_uint(a));
								}
							}

							// culling mode
							AREA_CULL_MODE cmode = imgui::get()->m_disable_cullnode ? map_settings::AREA_CULL_MODE_NO_FRUSTUM : map_settings::AREA_CULL_INFO_DEFAULT;
							if (contains_cull)
							{
								auto m = to_uint(entry.at("cull"));
								if (m >= AREA_CULL_INFO_COUNT) 
								{
									game::console(); printf("MapSettings: param 'cull' was out-of-range (%d)\n", m);
									m = 0u;
								}
								cmode = (AREA_CULL_MODE)(std::uint8_t)m;
							}

							// nocull dist for certain cull modes
							float temp_nocull_dist = game_settings::get()->default_nocull_distance.get_as<float>();
							if (entry.contains("nocull_dist")) {
								temp_nocull_dist = to_float(entry.at("nocull_dist"));
							}

							// hidden leafs
							std::unordered_set<std::uint32_t> hidden_leaf_set;
							if (contains_hidden_leafs)
							{
								auto& leafs = entry.at("hide_leafs").as_array();

								for (const auto& leaf : leafs) {
									hidden_leaf_set.insert(to_uint(leaf));
								}
							}

							// hidden areas
							std::vector<hide_area_s> temp_hidden_areas_set;
							if (contains_hidden_areas)
							{
								auto& hide_areas = entry.at("hide_areas").as_array();
								for (const auto& elem : hide_areas)
								{
									if (elem.contains("areas"))
									{
										std::unordered_set<std::uint32_t> temp_area_set;
										const auto& areas = elem.at("areas").as_array();

										for (const auto& a : areas) {
											temp_area_set.insert(to_uint(a));
										}

										std::unordered_set<std::uint32_t> temp_not_in_leaf_set;
										if (elem.contains("N_leafs"))
										{
											const auto& nleafs = elem.at("N_leafs").as_array();
											for (const auto& nl : nleafs) {
												temp_not_in_leaf_set.insert(to_uint(nl));
											}
										}

										temp_hidden_areas_set.emplace_back(std::move(temp_area_set), std::move(temp_not_in_leaf_set));
									}
								}
							}

							// leaf tweaks
							std::vector<leaf_tweak_s> temp_leaf_tweak_set;
							bool any_nocull_dist_overrides_in_leaf_tweaks = false;

							if (contains_leaf_tweak)
							{
								auto& leaf_tweak = entry.at("leaf_tweak").as_array();
								for (const auto& elem : leaf_tweak)
								{
									if (elem.contains("in_leafs"))
									{
										std::unordered_set<std::uint32_t> temp_in_leafs_set;
										const auto& in_leafs = elem.at("in_leafs").as_array();

										for (const auto& l : in_leafs) {
											temp_in_leafs_set.insert(to_uint(l));
										}

										std::unordered_set<std::uint32_t> temp_areas;
										if (elem.contains("areas"))
										{
											const auto& areas = elem.at("areas").as_array();
											for (const auto& a : areas) {
												temp_areas.insert(to_uint(a));
											}
										}

										std::unordered_set<std::uint32_t> temp_leafs;
										if (elem.contains("leafs"))
										{
											const auto& leafs = elem.at("leafs").as_array();
											for (const auto& l : leafs) {
												temp_leafs.insert(to_uint(l));
											}
										}

										// nocull dist for certain cull modes
										float temp_leaf_tweak_nocull_dist = 0.0f; // 0 = no override
										if (elem.contains("nocull_dist")) 
										{
											temp_leaf_tweak_nocull_dist = to_float(elem.at("nocull_dist"));
											any_nocull_dist_overrides_in_leaf_tweaks = true;
										}

										temp_leaf_tweak_set.emplace_back(
											std::move(temp_in_leafs_set), 
											std::move(temp_areas), 
											std::move(temp_leafs),
											temp_leaf_tweak_nocull_dist);
									}
								}
							}

							m_map_settings.area_settings.emplace(area,
								area_overrides_s 
								{
									std::move(leaf_set),
									std::move(area_set),
									std::move(hidden_leaf_set),
									std::move(temp_hidden_areas_set),
									std::move(temp_leaf_tweak_set),
									cmode,
									temp_nocull_dist,
									any_nocull_dist_overrides_in_leaf_tweaks,
									area
								});
						}
					};

				// try to find the loaded map
				if (cull_table.contains(m_map_settings.mapname))
				{
					if (const auto map = cull_table[m_map_settings.mapname]; 
						!map.is_empty() && !map.as_array().empty())
					{
						for (const auto& entry : map.as_array()) {
							process_cull_entry(entry);
						}
					}
				}
			} // end 'CULL'


			// ####################
			// parse 'HIDEMODEL' table
			if (config.contains("HIDEMODEL"))
			{
				// try to find the loaded map
				if (auto& hidemdl_table = config["HIDEMODEL"]; 
					hidemdl_table.contains(m_map_settings.mapname))
				{
					if (const auto map = hidemdl_table[m_map_settings.mapname];
						!map.is_empty())
					{
						if (map.contains("name"))
						{
							if (auto& names = map.at("name");
								!names.is_empty())
							{
								if (const auto& narray = map.at("name").as_array();
									!narray.empty())
								{
									for (auto& str : narray) {
										m_map_settings.hide_models.substrings.insert(str.as_string());
									}
								}
							}
						}

						if (map.contains("radius"))
						{
							if (auto& radii = map.at("radius");
								!radii.is_empty())
							{
								if (const auto& rarray = map.at("radius").as_array();
									!rarray.empty())
								{
									for (auto& r : rarray) {
										m_map_settings.hide_models.radii.insert(to_float(r, -1.0f));
									}
								}
							}
						}
					}
				}
			} // end 'HIDEMODEL'


			// ####################
			// parse 'UNBAKE' table
			if (config.contains("UNBAKE"))
			{
				auto& unbake_table = config["UNBAKE"];

				// try to find the loaded map
				if (unbake_table.contains(m_map_settings.mapname))
				{
					if (const auto map = unbake_table[m_map_settings.mapname];
						!map.is_empty())
					{
						if (map.contains("checksum"))
						{
							if (auto& checksum = map.at("checksum");
								!checksum.is_empty())
							{
								if (const auto& arr = checksum.as_array();
									!arr.empty())
								{
									for (auto& sum : arr) {
										m_map_settings.unbake_models.checksums.insert(to_int(sum, 0u));
									}
								}
							}
						}
					}
				}

				if (unbake_table.contains("ALL"))
				{
					if (auto& all = unbake_table.at("ALL");
						!all.is_empty())
					{
						if (all.contains("checksum"))
						{
							if (auto& checksum = all.at("checksum");
								!checksum.is_empty())
							{
								if (const auto& arr = checksum.as_array();
									!arr.empty())
								{
									for (auto& sum : arr) {
										m_map_settings.unbake_models.checksums.insert(to_uint(sum, 0u));
									}
								}
							}
						}
					}
				}
			} // end 'UNBAKE'


			// ####################
			// parse 'MARKER' table
			if (config.contains("MARKER"))
			{
				auto& marker_table = config["MARKER"];

				// #
				auto process_marker_entry = [to_bool, to_uint, to_int, to_float](const toml::value& entry)
					{
						bool temp_is_nocull_marker = false;
						std::uint32_t temp_marker_index = 0u;

						if (entry.contains("marker")) {
							temp_marker_index = static_cast<std::uint32_t>(to_int(entry.at("marker"), 0u));
						}
						else if (entry.contains("nocull")) 
						{
							temp_marker_index = static_cast<std::uint32_t>(to_int(entry.at("nocull"), 0u));
							temp_is_nocull_marker = true;
						}
						else
						{
							TOML_ERROR("[MARKER] #index", entry, "Marker did not define an index via 'marker' or 'nocull' -> skipping");
							return;
						}

						std::string temp_comment;
						if (!entry.comments().empty())
						{
							temp_comment = entry.comments().at(0);
							temp_comment.erase(0, 2); // rem '# '
						}

						std::string temp_name;
						if (entry.contains("name")) { try { temp_name = entry.at("name").as_string(); } CATCH_ERR; }
						const bool temp_visible = entry.contains("visible") ? to_bool(entry.at("visible"), true) : true;
						const float temp_visibility_range = entry.contains("range") ? std::max(0.0f, to_float(entry.at("range"), 0.0f)) : 0.0f;
						Vector temp_color = { 1.0f, 1.0f, 1.0f };
						if (entry.contains("color") && entry.at("color").is_array())
						{
							if (const auto& color = entry.at("color").as_array(); color.size() == 3) {
								temp_color = { std::clamp(to_float(color[0]), 0.0f, 1.0f), std::clamp(to_float(color[1]), 0.0f, 1.0f), std::clamp(to_float(color[2]), 0.0f, 1.0f) };
							}
						}

						if (entry.contains("position"))
						{
							if (const auto& pos = entry.at("position").as_array();
								pos.size() == 3)
							{
								Vector temp_rotation;
								Vector temp_scale = { 1.0, 1.0f, 1.0f };

								// optional
								if (entry.contains("rotation"))
								{
									if (const auto& rot = entry.at("rotation").as_array(); rot.size() == 3) {
										temp_rotation = { DEG2RAD(to_float(rot[0])), DEG2RAD(to_float(rot[1])), DEG2RAD(to_float(rot[2])) };
									} else { TOML_ERROR("[MARKER] #rotation", entry.at("rotation"), "expected a 3D vector but got => %d ", entry.at("rotation").as_array().size()); }
								}

								// optional
								if (entry.contains("scale"))
								{
									if (const auto& scale = entry.at("scale").as_array(); scale.size() == 3) {
										temp_scale = { to_float(scale[0]), to_float(scale[1]), to_float(scale[2]) };
									} else { TOML_ERROR("[MARKER] #scale", entry.at("scale"), "expected a 3D vector but got => %d ", entry.at("scale").as_array().size()); }
								}

								// optional
								std::unordered_set<std::uint32_t> temp_area_set;
								if (entry.contains("areas"))
								{
									if (const auto& areas = entry.at("areas").as_array(); !areas.empty()) 
									{
										for (const auto& a : areas) {
											temp_area_set.insert(to_int(a));
										}
									}
								}

								// optional
								std::unordered_set<std::uint32_t> temp_not_in_leaf_set;
								if (entry.contains("N_leafs"))
								{
									if (const auto& nleafs = entry.at("N_leafs").as_array(); !nleafs.empty())
									{
										for (const auto& nl : nleafs) {
											temp_not_in_leaf_set.insert(to_int(nl));
										}
									}
								}

								// optional
								marker_trigger_s temp_trigger_show = {};
								marker_trigger_s temp_trigger_hide = {};
								bool temp_trigger_always = false;
								bool temp_hide_by_default = false;

								if (entry.contains("trigger"))
								{
									const auto& trigger = entry.at("trigger");
									bool has_valid_trigger = false;

									// SHOW
									if (trigger.contains("show"))
									{
										const auto& show = trigger.at("show");
										if (show.contains("choreo"))
										{
											try { temp_trigger_show.choreo_name = show.at("choreo").as_string(); }
											CATCH_ERR;

											if (show.contains("actor"))
											{
												try { temp_trigger_show.choreo_actor = show.at("actor").as_string(); }
												CATCH_ERR;
											}

											if (show.contains("event"))
											{
												try { temp_trigger_show.choreo_event = show.at("event").as_string(); }
												CATCH_ERR;
											}

											if (show.contains("param1"))
											{
												try { temp_trigger_show.choreo_param1 = show.at("param1").as_string(); }
												CATCH_ERR;
											}

											has_valid_trigger = true;
										}
										// sound trigger
										else if (show.contains("sound"))
										{
											if (show.at("sound").type() == toml::value_t::integer) {
												temp_trigger_show.sound_hash = to_uint(show.at("sound"), 0u);
											}
											else
											{
												try { temp_trigger_show.sound_name = show.at("sound").as_string(); }
												CATCH_ERR;
											}

											has_valid_trigger = true;
										}

										if (has_valid_trigger)
										{
											temp_hide_by_default = true;

											if (show.contains("delay")) {
												temp_trigger_show.delay = to_float(show.at("delay"), 0.0f);
											}
										}
										else { TOML_ERROR("[MARKER] #trigger", show, "defined show trigger with no choreo / sound hash"); }
									}

									// HIDE
									if (trigger.contains("hide"))
									{
										const auto& hide = trigger.at("hide");
										if (hide.contains("choreo"))
										{
											try { temp_trigger_hide.choreo_name = hide.at("choreo").as_string(); }
											CATCH_ERR;

											if (hide.contains("actor"))
											{
												try { temp_trigger_hide.choreo_actor = hide.at("actor").as_string(); }
												CATCH_ERR;
											}

											if (hide.contains("event"))
											{
												try { temp_trigger_hide.choreo_event = hide.at("event").as_string(); }
												CATCH_ERR;
											}

											if (hide.contains("param1"))
											{
												try { temp_trigger_hide.choreo_param1 = hide.at("param1").as_string(); }
												CATCH_ERR;
											}

											has_valid_trigger = true;
										}

										// sound trigger
										else if (hide.contains("sound"))
										{
											if (hide.at("sound").type() == toml::value_t::integer) {
												temp_trigger_hide.sound_hash = to_uint(hide.at("sound"), 0u);
											}
											else 
											{
												try { temp_trigger_hide.sound_name = hide.at("sound").as_string(); }
												CATCH_ERR;
											}

											has_valid_trigger = true;
										}

										if (has_valid_trigger)
										{
											if (hide.contains("delay")) {
												temp_trigger_hide.delay = to_float(hide.at("delay"), 0.0f);
											}
										}
										else { TOML_ERROR("[MARKER] #trigger", hide, "defined hide trigger with no choreo / sound hash"); }
									}

									if (trigger.contains("always")) {
										temp_trigger_always = to_bool(trigger.at("always"), false);
									}
								}

								m_map_settings.map_markers.emplace_back(
									marker_settings_s
									{
										.index = temp_marker_index,
										.origin = { to_float(pos[0]), to_float(pos[1]), to_float(pos[2]) },
										.no_cull = temp_is_nocull_marker,
										.rotation = temp_rotation,
										.scale = temp_scale,
										.areas = std::move(temp_area_set),
										.when_not_in_leafs = std::move(temp_not_in_leaf_set),
										.trigger_show = std::move(temp_trigger_show),
										.trigger_hide = std::move(temp_trigger_hide),
										.trigger_always = temp_trigger_always,
										.comment = std::move(temp_comment),
										.name = std::move(temp_name),
										.color = temp_color,
										.visible = temp_visible,
										.visibility_range = temp_visibility_range,

										.is_hidden = temp_hide_by_default
									});
							}
							else { TOML_ERROR("[MARKER] #position", entry.at("position"), "expected a 3D vector but got => %d ", entry.at("position").as_array().size()); }
						}
					};

				// try to find the loaded map
				if (marker_table.contains(m_map_settings.mapname))
				{
					if (const auto map = marker_table[m_map_settings.mapname];
						!map.is_empty() && !map.as_array().empty())
					{
						for (const auto& entry : map.as_array()) {
							process_marker_entry(entry);
						}
					}
				}
			} // end 'MARKER'


			// ####################
			// parse 'CONFIGVARS' table
			{
				auto& configvar_table = config["CONFIGVARS"];

				auto process_transition_entry = [to_uint, to_int, to_float](const toml::value& entry)
					{
						// we NEED conf, leafs and duration or speed
						if (entry.contains("conf") && entry.contains("trigger") && (entry.contains("duration") || entry.contains("speed")))
						{
							std::string config_name;

							try { config_name = entry.at("conf").as_string(); }
							catch (toml::type_error& err) 
							{
								game::console(); printf("%s\n", err.what());
								return;
							}

							if (!config_name.empty()) 
							{
								std::uint8_t mode = 0u;
								remix_vars::EASE_TYPE ease = remix_vars::EASE_TYPE_LINEAR;
								float delay_in = 0.0f, delay_out = 0.0f, duration = 0.0f;

								if (entry.contains("mode")) {
									mode = (std::uint8_t)to_int(entry.at("mode"));
								}

								if (entry.contains("ease")) {
									ease = (remix_vars::EASE_TYPE)to_int(entry.at("ease"));
								}

								if (entry.contains("delay_in")) {
									delay_in = to_float(entry.at("delay_in"));
								}

								if (entry.contains("delay_out")) {
									delay_out = to_float(entry.at("delay_out"));
								}

								if (entry.contains("duration")) {
									duration = to_float(entry.at("duration"));
								}

								const auto& trigger = entry.at("trigger");

								// choreo trigger
								if (trigger.contains("choreo"))
								{
									std::string choreo_name;
									std::string choreo_actor;
									std::string choreo_event;
									std::string choreo_param1;

									try { choreo_name = trigger.at("choreo").as_string(); }
									CATCH_ERR;

									if (trigger.contains("actor"))
									{
										try { choreo_actor = trigger.at("actor").as_string(); }
										CATCH_ERR;
									}
									
									if (trigger.contains("event"))
									{
										try { choreo_event = trigger.at("event").as_string(); }
										CATCH_ERR;
									}

									if (trigger.contains("param1"))
									{
										try { choreo_param1 = trigger.at("param1").as_string(); }
										CATCH_ERR;
									}

									if (!choreo_name.empty())
									{
										const auto hash = utils::string_hash64(utils::va("%s%s%.2f", choreo_name.c_str(), config_name.c_str(), duration));
										m_map_settings.remix_transitions.emplace_back(
											TRANSITION_TRIGGER_TYPE::CHOREO,
											std::move(choreo_name),
											std::move(choreo_actor),
											std::move(choreo_event),
											std::move(choreo_param1),
											0u,
											"",
											std::unordered_set<std::uint32_t>(),
											config_name,
											(TRANSITION_MODE)mode,
											ease,
											delay_in,
											delay_out,
											duration,
											hash);
									}
								}

								// sound trigger
								else if (trigger.contains("sound"))
								{
									std::uint32_t temp_sound_hash = 0u;
									std::string temp_sound_name;

									if (trigger.at("sound").type() == toml::value_t::integer)
									{
										temp_sound_hash = to_uint(trigger.at("sound"), 0u);
									}
									else
									{
										try { temp_sound_name = trigger.at("sound").as_string(); }
										CATCH_ERR;
									}

									const auto hash = utils::string_hash64(utils::va("%d%s%s%.2f", temp_sound_hash, temp_sound_name.c_str(), config_name.c_str(), duration));
									m_map_settings.remix_transitions.emplace_back(
										TRANSITION_TRIGGER_TYPE::SOUND,
										"",
										"",
										"",
										"",
										temp_sound_hash,
										std::move(temp_sound_name),
										std::unordered_set<std::uint32_t>(),
										config_name,
										(TRANSITION_MODE)mode,
										ease,
										delay_in,
										delay_out,
										duration,
										hash);
								}

								// leaf trigger
								else if (trigger.contains("leafs") && trigger.at("leafs").is_array())
								{
									std::unordered_set<std::uint32_t> leaf_set;
									const auto& leafs = trigger.at("leafs").as_array();
									if (!leafs.empty())
									{
										for (const auto& leaf : leafs) {
											leaf_set.insert(to_int(leaf));
										}

										// create a unique hash for this transition
										std::uint32_t leaf_sum = 0;
										for (const auto& leaf : leaf_set) {
											leaf_sum += leaf;
										}

										const auto hash = utils::string_hash64(utils::va("%d%s%.2f", leaf_sum, config_name.c_str(), duration));
										m_map_settings.remix_transitions.emplace_back(
											TRANSITION_TRIGGER_TYPE::LEAF,
											"",
											"",
											"",
											"",
											0u,
											"",
											std::move(leaf_set),
											config_name,
											(TRANSITION_MODE)mode,
											ease,
											delay_in,
											delay_out,
											duration,
											hash);
									}
								}
							}
						}
					};

				// try to find the loaded map
				if (configvar_table.contains(m_map_settings.mapname))
				{
					if (const auto map = configvar_table[m_map_settings.mapname];
						!map.is_empty())
					{
						if (map.contains("startup"))
						{
							if (auto& startup = map.at("startup").as_array(); 
								!startup.empty())
							{
								for (const auto& conf : startup)
								{
									try {
										m_map_settings.api_var_configs.emplace_back(conf.as_string());
									}
									catch (toml::type_error& err) {
										game::console(); printf("%s\n", err.what());
									}
								}
							}
						}

						if (map.contains("transitions"))
						{
							if (auto& transitions = map.at("transitions").as_array();
								!transitions.empty())
							{
								for (const auto& entry : transitions) {
									process_transition_entry(entry);
								}
							}
						}
					}
				}
			} // end 'CONFIGVARS'


			// ####################
			// parse 'LIGHT_ANCHORS' table
			if (config.contains("LIGHT_ANCHORS"))
			{
				auto& light_anchor_table = config["LIGHT_ANCHORS"];

				auto read_vec3 = [to_float](const toml::value& value, const Vector& fallback, const char* label)
					{
						try
						{
							if (value.is_array())
							{
								const auto& arr = value.as_array();
								if (arr.size() == 3u) {
									return Vector(to_float(arr[0], fallback.x), to_float(arr[1], fallback.y), to_float(arr[2], fallback.z));
								}
							}
						}
						catch (toml::type_error& err) {
							game::console(); printf("%s\n", err.what());
						}

						game::console(); printf("[LIGHT_ANCHORS] %s expected a 3D vector\n", label);
						return fallback;
					};

				auto process_light_anchor_entry = [to_bool, to_float, read_vec3](const toml::value& entry)
					{
						light_anchor_s anchor = {};

						if (!entry.comments().empty())
						{
							anchor.comment = entry.comments().at(0);
							anchor.comment.erase(0, 2); // rem '# '
						}

						if (entry.contains("name")) { try { anchor.name = entry.at("name").as_string(); } CATCH_ERR; }
						if (entry.contains("preset")) { try { anchor.preset = entry.at("preset").as_string(); } CATCH_ERR; }
						if (entry.contains("animation")) { try { anchor.animation = entry.at("animation").as_string(); } CATCH_ERR; }
						if (entry.contains("position")) { anchor.position = read_vec3(entry.at("position"), anchor.position, "#position"); }
						if (entry.contains("offset")) { anchor.offset = read_vec3(entry.at("offset"), anchor.offset, "#offset"); }
						if (entry.contains("radiance")) { anchor.radiance = read_vec3(entry.at("radiance"), anchor.radiance, "#radiance"); }
						if (entry.contains("scalar")) { anchor.scalar = std::max(0.0f, to_float(entry.at("scalar"), anchor.scalar)); }
						if (entry.contains("radius")) { anchor.radius = std::max(0.0f, to_float(entry.at("radius"), anchor.radius)); }
						if (entry.contains("duration")) { anchor.duration = std::max(0.01f, to_float(entry.at("duration"), anchor.duration)); }
						if (entry.contains("speed")) { anchor.speed = std::max(0.01f, to_float(entry.at("speed"), anchor.speed)); }
						if (entry.contains("variation")) { anchor.variation = std::clamp(to_float(entry.at("variation"), anchor.variation), 0.0f, 1.0f); }
						if (entry.contains("volumetric_scale")) { anchor.volumetric_scale = std::max(0.0f, to_float(entry.at("volumetric_scale"), anchor.volumetric_scale)); }
						if (entry.contains("loop")) { anchor.loop = to_bool(entry.at("loop"), anchor.loop); }
						if (entry.contains("loop_smoothing")) { anchor.loop_smoothing = to_bool(entry.at("loop_smoothing"), anchor.loop_smoothing); }

						if (entry.contains("direction"))
						{
							anchor.direction = read_vec3(entry.at("direction"), anchor.direction, "#direction");
							anchor.direction.Normalize();
							anchor.use_shaping = true;
						}

						if (entry.contains("degrees"))
						{
							anchor.degrees = std::clamp(to_float(entry.at("degrees"), anchor.degrees), 0.0f, 180.0f);
							anchor.use_shaping = anchor.degrees != 180.0f;
						}

						if (entry.contains("softness")) { anchor.softness = std::clamp(to_float(entry.at("softness"), anchor.softness), 0.0f, M_PI); }
						if (entry.contains("exponent")) { anchor.exponent = std::max(0.0f, to_float(entry.at("exponent"), anchor.exponent)); }

						if (entry.contains("animation_axis"))
						{
							anchor.animation_axis = read_vec3(entry.at("animation_axis"), anchor.animation_axis, "#animation_axis");
							if (anchor.animation_axis.LengthSqr() > 0.0001f) { anchor.animation_axis.Normalize(); }
							else { anchor.animation_axis = Vector(0.0f, 0.0f, 1.0f); }
						}
						if (entry.contains("animation_degrees")) { anchor.animation_degrees = std::clamp(to_float(entry.at("animation_degrees"), anchor.animation_degrees), 0.0f, 1440.0f); }
						if (entry.contains("animation_phase")) { anchor.animation_phase = to_float(entry.at("animation_phase"), anchor.animation_phase); }

						if (entry.contains("comment")) { try { anchor.comment = entry.at("comment").as_string(); } CATCH_ERR; }

						if (anchor.name.empty())
						{
							anchor.name = utils::va("anchor_%.0f_%.0f_%.0f", anchor.position.x, anchor.position.y, anchor.position.z);
						}

						m_map_settings.light_anchors.push_back(std::move(anchor));
					};

				if (light_anchor_table.contains(m_map_settings.mapname))
				{
					if (const auto& map = light_anchor_table[m_map_settings.mapname];
						!map.is_empty() && map.is_array() && !map.as_array().empty())
					{
						for (const auto& entry : map.as_array()) {
							process_light_anchor_entry(entry);
						}
					}
				}
			} // end 'LIGHT_ANCHORS'

			// ####################
			// parse 'LIGHT_EVENTS' table
			if (config.contains("LIGHT_EVENTS"))
			{
				auto& light_event_table = config["LIGHT_EVENTS"];

				auto process_dynamic_light_event_entry = [to_bool, to_int, to_uint, to_float](const toml::value& entry)
					{
						if (!entry.contains("trigger")) {
							TOML_ERROR("[LIGHT_EVENTS] #trigger", entry, "dynamic light event needs a trigger");
							return;
						}

						dynamic_light_event_s ev = {};

						if (!entry.comments().empty())
						{
							ev.comment = entry.comments().at(0);
							ev.comment.erase(0, 2); // rem '# '
						}

						if (entry.contains("name")) {
							try { ev.name = entry.at("name").as_string(); }
							CATCH_ERR;
						}

						if (entry.contains("preset")) {
							try { ev.preset = entry.at("preset").as_string(); }
							CATCH_ERR;
						}

						if (entry.contains("animation")) {
							try { ev.animation = entry.at("animation").as_string(); }
							CATCH_ERR;
						}

						if (entry.contains("duration")) {
							ev.duration = std::max(0.01f, to_float(entry.at("duration"), ev.duration));
						}

						if (entry.contains("delay")) {
							ev.delay = std::max(0.0f, to_float(entry.at("delay"), ev.delay));
						}

						if (entry.contains("cooldown")) {
							ev.cooldown = std::max(0.0f, to_float(entry.at("cooldown"), ev.cooldown));
						}

						if (entry.contains("speed")) {
							ev.speed = std::max(0.01f, to_float(entry.at("speed"), ev.speed));
						}

						if (entry.contains("variation")) {
							ev.variation = std::clamp(to_float(entry.at("variation"), ev.variation), 0.0f, 1.0f);
						}

						if (entry.contains("once")) {
							ev.once = to_bool(entry.at("once"), ev.once);
						}

						if (entry.contains("loop")) {
							ev.loop = to_bool(entry.at("loop"), ev.loop);
						}

						if (entry.contains("loop_smoothing")) {
							ev.loop_smoothing = to_bool(entry.at("loop_smoothing"), ev.loop_smoothing);
						}

						if (entry.contains("enabled")) {
							ev.enabled = to_bool(entry.at("enabled"), ev.enabled);
						}

						if (entry.contains("use_source_origin")) {
							ev.use_source_origin = to_bool(entry.at("use_source_origin"), ev.use_source_origin);
						}

						if (entry.contains("use_camera_when_no_source")) {
							ev.use_camera_when_no_source = to_bool(entry.at("use_camera_when_no_source"), ev.use_camera_when_no_source);
						}

						if (entry.contains("activate_anchor"))
						{
							try { ev.activate_anchor = entry.at("activate_anchor").as_string(); }
							CATCH_ERR;
							ev.use_source_origin = false;
						}

						if (entry.contains("position"))
						{
							if (const auto& arr = entry.at("position").as_array(); arr.size() == 3u) {
								ev.position = Vector(to_float(arr[0]), to_float(arr[1]), to_float(arr[2]));
								ev.use_source_origin = false;
							}
							else { TOML_ERROR("[LIGHT_EVENTS] #position", entry.at("position"), "expected a 3D vector but got => %d ", entry.at("position").as_array().size()); }
						}

						if (entry.contains("offset"))
						{
							if (const auto& arr = entry.at("offset").as_array(); arr.size() == 3u) {
								ev.offset = Vector(to_float(arr[0]), to_float(arr[1]), to_float(arr[2]));
							}
							else { TOML_ERROR("[LIGHT_EVENTS] #offset", entry.at("offset"), "expected a 3D vector but got => %d ", entry.at("offset").as_array().size()); }
						}

						if (entry.contains("radiance"))
						{
							if (const auto& arr = entry.at("radiance").as_array(); arr.size() == 3u) {
								ev.radiance = Vector(to_float(arr[0], 1.0f), to_float(arr[1], 1.0f), to_float(arr[2], 1.0f));
							}
							else { TOML_ERROR("[LIGHT_EVENTS] #radiance", entry.at("radiance"), "expected a 3D vector but got => %d ", entry.at("radiance").as_array().size()); }
						}

						if (entry.contains("scalar")) {
							ev.scalar = std::max(0.0f, to_float(entry.at("scalar"), ev.scalar));
						}

						if (entry.contains("radius")) {
							ev.radius = std::max(0.0f, to_float(entry.at("radius"), ev.radius));
						}

						if (entry.contains("volumetric_scale")) {
							ev.volumetric_scale = std::max(0.0f, to_float(entry.at("volumetric_scale"), ev.volumetric_scale));
						}

						if (entry.contains("direction"))
						{
							if (const auto& arr = entry.at("direction").as_array(); arr.size() == 3u)
							{
								ev.direction = Vector(to_float(arr[0], 0.0f), to_float(arr[1], 0.0f), to_float(arr[2], 1.0f));
								ev.direction.Normalize();
								ev.use_shaping = true;
							}
							else { TOML_ERROR("[LIGHT_EVENTS] #direction", entry.at("direction"), "expected a 3D vector but got => %d ", entry.at("direction").as_array().size()); }
						}

						if (entry.contains("degrees"))
						{
							ev.degrees = std::clamp(to_float(entry.at("degrees"), ev.degrees), 0.0f, 180.0f);
							ev.use_shaping = ev.degrees != 180.0f;
						}

						if (entry.contains("softness")) {
							ev.softness = std::clamp(to_float(entry.at("softness"), ev.softness), 0.0f, M_PI);
						}

						if (entry.contains("exponent")) {
							ev.exponent = std::max(0.0f, to_float(entry.at("exponent"), ev.exponent));
						}

						if (entry.contains("animation_axis"))
						{
							if (const auto& arr = entry.at("animation_axis").as_array(); arr.size() == 3u)
							{
								ev.animation_axis = Vector(to_float(arr[0], 0.0f), to_float(arr[1], 0.0f), to_float(arr[2], 1.0f));
								if (ev.animation_axis.LengthSqr() > 0.0001f) { ev.animation_axis.Normalize(); }
								else { ev.animation_axis = Vector(0.0f, 0.0f, 1.0f); }
							}
							else { TOML_ERROR("[LIGHT_EVENTS] #animation_axis", entry.at("animation_axis"), "expected a 3D vector but got => %d ", entry.at("animation_axis").as_array().size()); }
						}
						if (entry.contains("animation_degrees")) {
							ev.animation_degrees = std::clamp(to_float(entry.at("animation_degrees"), ev.animation_degrees), 0.0f, 1440.0f);
						}
						if (entry.contains("animation_phase")) {
							ev.animation_phase = to_float(entry.at("animation_phase"), ev.animation_phase);
						}

						const auto& trigger = entry.at("trigger");
						bool has_valid_trigger = false;

						if (trigger.contains("sound"))
						{
							ev.trigger_type = DYN_LIGHT_TRIGGER_SOUND;
							if (trigger.at("sound").type() == toml::value_t::integer) {
								ev.sound_hash = to_uint(trigger.at("sound"), 0u);
							}
							else {
								try { ev.sound_name = trigger.at("sound").as_string(); }
								CATCH_ERR;
							}
							has_valid_trigger = ev.sound_hash || !ev.sound_name.empty();
						}
						else if (trigger.contains("choreo"))
						{
							ev.trigger_type = DYN_LIGHT_TRIGGER_CHOREO;
							try { ev.choreo_name = trigger.at("choreo").as_string(); }
							CATCH_ERR;

							if (trigger.contains("actor")) { try { ev.choreo_actor = trigger.at("actor").as_string(); } CATCH_ERR; }
							if (trigger.contains("event")) { try { ev.choreo_event = trigger.at("event").as_string(); } CATCH_ERR; }
							if (trigger.contains("param1")) { try { ev.choreo_param1 = trigger.at("param1").as_string(); } CATCH_ERR; }
							has_valid_trigger = !ev.choreo_name.empty();
						}
						else if (trigger.contains("leafs") && trigger.at("leafs").is_array())
						{
							ev.trigger_type = DYN_LIGHT_TRIGGER_LEAF;
							const auto& leafs = trigger.at("leafs").as_array();
							for (const auto& leaf : leafs) {
								ev.leafs.insert(to_uint(leaf));
							}
							has_valid_trigger = !ev.leafs.empty();
						}

						if (!has_valid_trigger) {
							TOML_ERROR("[LIGHT_EVENTS] #trigger", trigger, "defined trigger with no valid sound/choreo/leaf target");
							return;
						}

						if (ev.name.empty())
						{
							ev.name = utils::va("%s_%s", ev.preset.c_str(), ev.animation.c_str());
						}

						m_map_settings.dynamic_light_events.push_back(std::move(ev));
					};

				if (light_event_table.contains(m_map_settings.mapname))
				{
					if (const auto& map = light_event_table[m_map_settings.mapname];
						!map.is_empty() && map.is_array() && !map.as_array().empty())
					{
						for (const auto& entry : map.as_array()) {
							process_dynamic_light_event_entry(entry);
						}
					}
				}
			} // end 'LIGHT_EVENTS'

			// ####################
			// parse 'LIGHTS' table
			if (config.contains("LIGHTS"))
			{
				auto& light_table = config["LIGHTS"];

				// #
				auto process_light_entry = [to_bool, to_int, to_uint, to_float](const toml::value& entry)
					{
						if (entry.contains("points") && !entry.at("points").as_array().empty())
						{
							auto normalize_or = [](Vector value, const Vector& fallback) -> Vector
								{
									if (value.LengthSqr() <= 0.0001f) { value = fallback; }
									if (value.LengthSqr() <= 0.0001f) { value = Vector(0.0f, 0.0f, -1.0f); }
									value.Normalize();
									return value;
								};

							auto apply_pseudo_ies_profile = [normalize_or](std::string profile, Vector& radiance, float& scalar, float& radius, bool& shaping, Vector& direction, float& degrees, float& softness, float& exponent, const float strength, const float focus)
								{
									profile = utils::str_to_lower(profile);
									utils::replace_all(profile, "-", "_");
									utils::replace_all(profile, " ", "_");

									if (profile.empty() || profile == "none" || profile == "off" || profile == "stable") { return; }

									auto make_cone = [&](const float cone, const float soft, const float exp, const Vector& fallback_dir)
										{
											shaping = true;
											degrees = std::clamp(cone, 1.0f, 180.0f);
											softness = std::clamp(soft, 0.0f, static_cast<float>(M_PI));
											exponent = std::max(0.0f, exp * focus);
											direction = normalize_or(direction, fallback_dir);
										};

									const float s = std::max(0.0f, strength);
									if (profile == "omni_soft" || profile == "sphere_soft")
									{
										shaping = false; degrees = 180.0f; softness = 0.0f; exponent = 0.0f;
										scalar *= 0.85f * s;
									}
									else if (profile == "bulb_a19" || profile == "warm_bulb")
									{
										shaping = false; degrees = 180.0f; softness = 0.0f; exponent = 0.0f;
										radiance = Vector(1.0f, 0.72f, 0.42f);
										scalar *= 1.10f * s;
									}
									else if (profile == "fluorescent_tube" || profile == "fluorescent")
									{
										radiance = Vector(0.76f, 0.88f, 1.0f);
										make_cone(128.0f, 0.52f, 0.08f, Vector(0.0f, 0.0f, -1.0f));
										scalar *= 0.85f * s;
									}
									else if (profile == "recessed_downlight" || profile == "downlight")
									{
										make_cone(64.0f, 0.22f, 0.45f, Vector(0.0f, 0.0f, -1.0f));
										scalar *= 1.15f * s;
									}
									else if (profile == "spot_25" || profile == "spotlight_25")
									{
										make_cone(25.0f, 0.08f, 1.20f, Vector(0.0f, 1.0f, 0.0f));
										scalar *= 1.35f * s;
									}
									else if (profile == "street_cutoff" || profile == "street_lamp_cutoff")
									{
										make_cone(92.0f, 0.18f, 0.72f, Vector(0.0f, 0.0f, -1.0f));
										radiance = Vector(1.0f, 0.78f, 0.48f);
										scalar *= 1.05f * s;
									}
									else if (profile == "exit_sign_wallwash" || profile == "exit_sign")
									{
										make_cone(116.0f, 0.62f, 0.05f, Vector(0.0f, 1.0f, 0.0f));
										radiance = Vector(0.42f, 1.0f, 0.38f);
										scalar *= 0.55f * s;
									}
									else if (profile == "tv_panel" || profile == "monitor")
									{
										make_cone(135.0f, 0.70f, 0.04f, Vector(0.0f, 1.0f, 0.0f));
										radiance = Vector(0.48f, 0.68f, 1.0f);
										scalar *= 0.70f * s;
									}
									else if (profile == "fire_emitter" || profile == "fire")
									{
										shaping = false; degrees = 180.0f; softness = 0.0f; exponent = 0.0f;
										radiance = Vector(1.0f, 0.34f, 0.08f);
										radius = std::max(radius, 2.0f);
										scalar *= 1.10f * s;
									}
								};

							// - parse trigger

							std::string temp_trigger_choreo_name;
							std::string temp_trigger_choreo_actor;
							std::string temp_trigger_choreo_event;
							std::string temp_trigger_choreo_param1;

							std::uint32_t temp_trigger_sound = 0u;
							float temp_trigger_delay = 0.0f;
							bool temp_trigger_always = false;

							std::string temp_comment;
							if (!entry.comments().empty())
							{
								temp_comment = entry.comments().at(0);
								temp_comment.erase(0, 2); // rem '# '
							}

							bool temp_enabled = true;
							if (entry.contains("enabled")) {
								temp_enabled = to_bool(entry.at("enabled"), true);
							}
							if (entry.contains("disabled")) {
								temp_enabled = !to_bool(entry.at("disabled"), false);
							}

							std::string temp_group;
							if (entry.contains("group")) {
								try { temp_group = entry.at("group").as_string(); }
								CATCH_ERR;
							}

							if (entry.contains("trigger"))
							{
								bool has_valid_trigger = false;
								const auto& trigger = entry.at("trigger");

								// choreo trigger
								if (trigger.contains("choreo"))
								{
									try { temp_trigger_choreo_name = trigger.at("choreo").as_string(); }
									CATCH_ERR;

									if (trigger.contains("actor"))
									{
										try { temp_trigger_choreo_actor = trigger.at("actor").as_string(); }
										CATCH_ERR;
									}

									if (trigger.contains("event"))
									{
										try { temp_trigger_choreo_event = trigger.at("event").as_string(); }
										CATCH_ERR;
									}

									if (trigger.contains("param1"))
									{
										try { temp_trigger_choreo_param1 = trigger.at("param1").as_string(); }
										CATCH_ERR;
									}

									has_valid_trigger = true;
								}
								// sound trigger
								else if (trigger.contains("sound"))
								{
									temp_trigger_sound = to_uint(trigger.at("sound"), 0u);
									has_valid_trigger = true;
								}

								if (has_valid_trigger)
								{
									if (trigger.contains("delay")) {
										temp_trigger_delay = to_float(trigger.at("delay"), 0.0f);
									}

									if (trigger.contains("always")) {
										temp_trigger_always = to_bool(trigger.at("always"), false);
									}
								}
								else { TOML_ERROR("[LIGHTS] #trigger", trigger, "defined trigger with no choreo / sound hash"); }
							}

							// - parse kill

							std::string temp_kill_choreo_name;
							std::uint32_t temp_kill_sound = 0u;
							float temp_kill_delay = 0.0f;

							if (entry.contains("kill"))
							{
								bool has_valid_kill_trigger = false;
								const auto& kill = entry.at("kill");

								// choreo
								if (kill.contains("choreo"))
								{
									try { temp_kill_choreo_name = kill.at("choreo").as_string(); }
									CATCH_ERR;

									has_valid_kill_trigger = true;
								}
								// sound
								else if (kill.contains("sound"))
								{
									temp_kill_sound = to_uint(kill.at("sound"), 0u);
									has_valid_kill_trigger = true;
								}

								if (has_valid_kill_trigger)
								{
									if (kill.contains("delay")) {
										temp_kill_delay = to_float(kill.at("delay"), 0.0f);
									}
								}
								else { TOML_ERROR("[LIGHTS] #trigger", kill, "defined kill trigger with no choreo / sound hash"); }
							}

							// - parse the explicit light backend and its native/fake IES defaults.
							auto parse_light_rig_mode = [](std::string mode)
							{
								mode = utils::str_to_lower(std::move(mode));
								utils::replace_all(mode, "-", "_");
								utils::replace_all(mode, " ", "_");
								if (mode == "native_ies" || mode == "ies" || mode == "ies_profile" || mode == "ies_profile_lights")
									return remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES;
								if (mode == "fake_ies" || mode == "pseudo_ies" || mode == "helper_ies" || mode == "fake_ies_profile_rig")
									return remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES;
								return remix_light_settings_s::LIGHT_RIG_MODE_LEGACY;
							};


							auto parse_light_authoring_shape = [](std::string shape)
							{
								shape = utils::str_to_lower(std::move(shape));
								utils::replace_all(shape, "-", "_");
								utils::replace_all(shape, " ", "_");
								if (shape == "point" || shape == "sphere" || shape == "omni") return remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT;
								if (shape == "spot" || shape == "cone") return remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT;
								if (shape == "disk" || shape == "disc") return remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK;
								if (shape == "rect" || shape == "rectangle" || shape == "panel") return remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT;
								if (shape == "tube" || shape == "cylinder" || shape == "line") return remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE;
								if (shape == "distant" || shape == "directional" || shape == "sun") return remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT;
								return remix_light_settings_s::LIGHT_AUTHORING_SHAPE_AUTO;
							};

							int entry_authoring_shape = remix_light_settings_s::LIGHT_AUTHORING_SHAPE_AUTO;
							float entry_authoring_width = 2.0f;
							float entry_authoring_height = 2.0f;
							float entry_authoring_length = 4.0f;
							float entry_authoring_range = 128.0f;
							if (entry.contains("editor_shape")) { try { entry_authoring_shape = parse_light_authoring_shape(entry.at("editor_shape").as_string()); } CATCH_ERR; }
							else if (entry.contains("light_shape")) { try { entry_authoring_shape = parse_light_authoring_shape(entry.at("light_shape").as_string()); } CATCH_ERR; }
							if (entry.contains("editor_width")) entry_authoring_width = std::clamp(to_float(entry.at("editor_width"), 2.0f), 0.001f, 8192.0f);
							if (entry.contains("editor_height")) entry_authoring_height = std::clamp(to_float(entry.at("editor_height"), 2.0f), 0.001f, 8192.0f);
							if (entry.contains("editor_length")) entry_authoring_length = std::clamp(to_float(entry.at("editor_length"), 4.0f), 0.001f, 8192.0f);
							if (entry.contains("editor_range")) entry_authoring_range = std::clamp(to_float(entry.at("editor_range"), 128.0f), 0.001f, 65536.0f);

							int entry_light_rig_mode = remix_light_settings_s::LIGHT_RIG_MODE_LEGACY;
							bool entry_light_rig_mode_explicit = false;
							if (entry.contains("light_mode")) { try { entry_light_rig_mode = parse_light_rig_mode(entry.at("light_mode").as_string()); entry_light_rig_mode_explicit = true; } CATCH_ERR; }
							else if (entry.contains("rig_mode")) { try { entry_light_rig_mode = parse_light_rig_mode(entry.at("rig_mode").as_string()); entry_light_rig_mode_explicit = true; } CATCH_ERR; }

							std::string entry_ies_profile;
							std::string entry_ies_file;
							float entry_ies_axis_rotation = 0.0f;
							float entry_ies_angle_scale = 1.0f;
							float entry_ies_intensity_scale = 1.0f;
							bool entry_ies_normalize = true;
							float entry_ies_strength = 1.0f;
							float entry_ies_focus = 1.0f;
							if (entry.contains("ies_profile")) { try { entry_ies_profile = entry.at("ies_profile").as_string(); } CATCH_ERR; }
							else if (entry.contains("ies")) { try { entry_ies_profile = entry.at("ies").as_string(); } CATCH_ERR; }
							if (entry.contains("ies_file")) { try { entry_ies_file = entry.at("ies_file").as_string(); } CATCH_ERR; }
							if (entry.contains("ies_axis_rotation")) { entry_ies_axis_rotation = std::clamp(to_float(entry.at("ies_axis_rotation"), 0.0f), -360.0f, 360.0f); }
							if (entry.contains("ies_angle_scale")) { entry_ies_angle_scale = std::clamp(to_float(entry.at("ies_angle_scale"), 1.0f), 0.01f, 8.0f); }
							if (entry.contains("ies_intensity_scale")) { entry_ies_intensity_scale = std::clamp(to_float(entry.at("ies_intensity_scale"), 1.0f), 0.0f, 32.0f); }
							if (entry.contains("ies_normalize")) { entry_ies_normalize = to_bool(entry.at("ies_normalize"), true); }
							if (entry.contains("ies_strength")) { entry_ies_strength = std::clamp(to_float(entry.at("ies_strength"), 1.0f), 0.0f, 8.0f); }
							if (entry.contains("ies_focus")) { entry_ies_focus = std::clamp(to_float(entry.at("ies_focus"), 1.0f), 0.05f, 8.0f); }

							// Optional default pseudo-IES cluster emulation for all points in this light.
							bool entry_ies_emulation = false;
							int entry_ies_emulation_samples = 0;
							float entry_ies_emulation_spread = 0.35f;
							float entry_ies_emulation_radius_scale = 0.45f;
							float entry_ies_emulation_intensity_scale = 0.35f;
							float entry_ies_emulation_forward_offset = 0.08f;
							std::string entry_ies_emulation_pattern = "spiral";
							float entry_ies_emulation_aspect = 1.0f;
							float entry_ies_emulation_twist = 0.0f;
							auto parse_ies_emulation_table = [to_bool, to_int, to_float](const toml::value& src, bool& enabled, int& samples, float& spread, float& radius_scale, float& intensity_scale, float& forward_offset, std::string& pattern, float& aspect, float& twist)
							{
								if (src.contains("enabled")) { enabled = to_bool(src.at("enabled"), enabled); }
								if (src.contains("mode"))
								{
									try
									{
										auto mode = utils::str_to_lower(src.at("mode").as_string());
										utils::replace_all(mode, "-", "_");
										enabled = mode == "cluster" || mode == "multi_point" || mode == "multipoint" || mode == "helpers";
									}
									CATCH_ERR;
								}
								if (src.contains("samples")) { samples = std::clamp(to_int(src.at("samples"), samples), 0, 24); }
								if (src.contains("spread")) { spread = std::clamp(to_float(src.at("spread"), spread), 0.0f, 4.0f); }
								if (src.contains("radius_scale")) { radius_scale = std::clamp(to_float(src.at("radius_scale"), radius_scale), 0.01f, 4.0f); }
								if (src.contains("intensity_scale")) { intensity_scale = std::clamp(to_float(src.at("intensity_scale"), intensity_scale), 0.0f, 4.0f); }
								if (src.contains("forward_offset")) { forward_offset = std::clamp(to_float(src.at("forward_offset"), forward_offset), -4.0f, 4.0f); }
								if (src.contains("pattern"))
								{
									try
									{
										pattern = utils::str_to_lower(src.at("pattern").as_string());
										utils::replace_all(pattern, "-", "_");
									}
									CATCH_ERR;
								}
								if (src.contains("aspect")) { aspect = std::clamp(to_float(src.at("aspect"), aspect), 0.1f, 8.0f); }
								if (src.contains("twist")) { twist = std::clamp(to_float(src.at("twist"), twist), -360.0f, 360.0f); }
								if (src.contains("twist_degrees")) { twist = std::clamp(to_float(src.at("twist_degrees"), twist), -360.0f, 360.0f); }
								if (samples > 0) { enabled = true; }
							};
							if (entry.contains("ies_emulation")) { parse_ies_emulation_table(entry.at("ies_emulation"), entry_ies_emulation, entry_ies_emulation_samples, entry_ies_emulation_spread, entry_ies_emulation_radius_scale, entry_ies_emulation_intensity_scale, entry_ies_emulation_forward_offset, entry_ies_emulation_pattern, entry_ies_emulation_aspect, entry_ies_emulation_twist); }
							if (entry.contains("ies_cluster_samples")) { entry_ies_emulation_samples = std::clamp(to_int(entry.at("ies_cluster_samples"), entry_ies_emulation_samples), 0, 24); entry_ies_emulation = entry_ies_emulation_samples > 0; }
							if (entry.contains("ies_cluster_spread")) { entry_ies_emulation_spread = std::clamp(to_float(entry.at("ies_cluster_spread"), entry_ies_emulation_spread), 0.0f, 4.0f); }
							if (entry.contains("ies_cluster_radius_scale")) { entry_ies_emulation_radius_scale = std::clamp(to_float(entry.at("ies_cluster_radius_scale"), entry_ies_emulation_radius_scale), 0.01f, 4.0f); }
							if (entry.contains("ies_cluster_intensity_scale")) { entry_ies_emulation_intensity_scale = std::clamp(to_float(entry.at("ies_cluster_intensity_scale"), entry_ies_emulation_intensity_scale), 0.0f, 4.0f); }
							if (entry.contains("ies_cluster_forward_offset")) { entry_ies_emulation_forward_offset = std::clamp(to_float(entry.at("ies_cluster_forward_offset"), entry_ies_emulation_forward_offset), -4.0f, 4.0f); }
							if (entry.contains("ies_cluster_pattern")) { try { entry_ies_emulation_pattern = utils::str_to_lower(entry.at("ies_cluster_pattern").as_string()); utils::replace_all(entry_ies_emulation_pattern, "-", "_"); } CATCH_ERR; }
							if (entry.contains("ies_cluster_aspect")) { entry_ies_emulation_aspect = std::clamp(to_float(entry.at("ies_cluster_aspect"), entry_ies_emulation_aspect), 0.1f, 8.0f); }
							if (entry.contains("ies_cluster_twist")) { entry_ies_emulation_twist = std::clamp(to_float(entry.at("ies_cluster_twist"), entry_ies_emulation_twist), -360.0f, 360.0f); }

							// Backward-compatible inference for configurations authored before explicit light_mode.
							if (!entry_light_rig_mode_explicit)
							{
								if (!entry_ies_file.empty()) entry_light_rig_mode = remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES;
								else if (entry_ies_emulation || entry_ies_emulation_samples > 0) entry_light_rig_mode = remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES;
							}
							entry_ies_emulation = entry_light_rig_mode == remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES && entry_ies_emulation_samples > 0;

							// - parse points

							const auto& parray = entry.at("points").as_array();
							std::vector<remix_light_settings_s::point_s> temp_points;

							// for each point
							for (auto i = 0u; i < parray.size(); i++)
							{
								bool point_has_valid_position = false;

								const auto& p = parray[i];
								if (p.contains("position"))
								{
									if (const auto& positions = p.at("position").as_array(); positions.size() == 3) {
										point_has_valid_position = true;
									}
									else { TOML_ERROR("[LIGHTS] #position", p.at("position"), "expected a 3D vector but got => %d ", p.at("position").as_array().size()); }
								}

								if (!i && !point_has_valid_position) // first point needs to define a position
								{
									TOML_ERROR("[LIGHTS] #position", p, "first point needs to define a position! Ignoring light");
									break;
								}

								Vector temp_radiance = { 10.0f, 10.0f, 10.0f };
								if (p.contains("radiance"))
								{
									if (const auto& radiance = p.at("radiance").as_array(); radiance.size() == 3)
									{
										temp_radiance = Vector(to_float(radiance[0], 10.0f), to_float(radiance[1], 10.0f), to_float(radiance[2], 10.0f));
									}
									else { TOML_ERROR("[LIGHTS] #radiance", p.at("radiance"), "expected a 3D vector but got => %d ", p.at("radiance").as_array().size()); }
								}

								float temp_radiance_scalar = 1.0f;
								if (p.contains("scalar")) {
									temp_radiance_scalar = to_float(p.at("scalar"), 1.0f);
								}

								float temp_radius = 1.0f;
								if (p.contains("radius")) {
									temp_radius = to_float(p.at("radius"), 1.0f);
								}

								float temp_timepoint = 0.0f;
								if (i && p.contains("timepoint")) { // do not set timepoint for first point
									temp_timepoint = to_float(p.at("timepoint"), 0.0f);
								}

								float temp_smoothness = 0.5f;
								if (p.contains("smoothness"))
								{
									temp_smoothness = to_float(p.at("smoothness"), 0.5f);
									temp_smoothness = std::clamp<float>(temp_smoothness, 0.0f, 10.0f);
								}


								// shaping

								Vector temp_direction = { 0.0f, 0.0f, 1.0f };
								if (p.contains("direction"))
								{
									if (const auto& direction = p.at("direction").as_array(); direction.size() == 3)
									{
										temp_direction = Vector(to_float(direction[0], 0.0f), to_float(direction[1], 0.0f), to_float(direction[2], 1.0f));
										temp_direction.Normalize();
									}
									else { TOML_ERROR("[LIGHTS] #direction", p.at("direction"), "expected a 3D vector but got => %d ", p.at("direction").as_array().size()); }
								}

								Vector temp_angle_offset_attached = { 0.0f, 0.0f, 0.0f };
								if (p.contains("angle_offset_attached"))
								{
									if (const auto& angle_offset_attached = p.at("angle_offset_attached").as_array(); angle_offset_attached.size() == 3)
									{
										temp_angle_offset_attached = Vector(to_float(angle_offset_attached[0], 0.0f), to_float(angle_offset_attached[1], 0.0f), to_float(angle_offset_attached[2], 0.0f));
										utils::vector::angle_normalize(temp_angle_offset_attached);
									}
									else { TOML_ERROR("[LIGHTS] #angle_offset_attached", p.at("angle_offset_attached"), "expected a 3D vector but got => %d ", p.at("angle_offset_attached").as_array().size()); }
								}

								bool temp_shaping_enabled = false;
								float temp_degrees = 180.0f;
								if (p.contains("degrees"))
								{
									temp_degrees = to_float(p.at("degrees"), 180.0f);
									temp_degrees = std::clamp<float>(temp_degrees, 0.0f, 180.0f);
									temp_shaping_enabled = temp_degrees != 180.0f;
								}

								float temp_softness = 0.0f;
								if (p.contains("softness"))
								{
									temp_softness = to_float(p.at("softness"), 0.0f);
									temp_softness = std::clamp<float>(temp_softness, 0.0f, M_PI);
								}

								float temp_exponent = 0.0f;
								if (p.contains("exponent")) {
									temp_exponent = to_float(p.at("exponent"), 0.0f);
								}

								// volumetrics
								float temp_volumetric = 1.0f;
								if (p.contains("volumetric_scale")) { // volumetricRadianceScale
									temp_volumetric = to_float(p.at("volumetric_scale"), 1.0f);
								}


								int temp_authoring_shape = entry_authoring_shape;
								float temp_authoring_width = entry_authoring_width;
								float temp_authoring_height = entry_authoring_height;
								float temp_authoring_length = entry_authoring_length;
								float temp_authoring_range = entry_authoring_range;
								if (p.contains("editor_shape")) { try { temp_authoring_shape = parse_light_authoring_shape(p.at("editor_shape").as_string()); } CATCH_ERR; }
								else if (p.contains("light_shape")) { try { temp_authoring_shape = parse_light_authoring_shape(p.at("light_shape").as_string()); } CATCH_ERR; }
								if (p.contains("editor_width")) temp_authoring_width = std::clamp(to_float(p.at("editor_width"), temp_authoring_width), 0.001f, 8192.0f);
								if (p.contains("editor_height")) temp_authoring_height = std::clamp(to_float(p.at("editor_height"), temp_authoring_height), 0.001f, 8192.0f);
								if (p.contains("editor_length")) temp_authoring_length = std::clamp(to_float(p.at("editor_length"), temp_authoring_length), 0.001f, 8192.0f);
								if (p.contains("editor_range")) temp_authoring_range = std::clamp(to_float(p.at("editor_range"), temp_authoring_range), 0.001f, 65536.0f);

								// Pseudo-IES / photometric profile metadata. The profile is translated into
								// Remix sphere light shaping so it works through the existing add-light path.
								int temp_light_rig_mode = entry_light_rig_mode;
								bool temp_light_rig_mode_explicit = entry_light_rig_mode_explicit;
								std::string temp_ies_profile = entry_ies_profile;
								std::string temp_ies_file = entry_ies_file;
								float temp_ies_axis_rotation = entry_ies_axis_rotation;
								float temp_ies_angle_scale = entry_ies_angle_scale;
								float temp_ies_intensity_scale = entry_ies_intensity_scale;
								bool temp_ies_normalize = entry_ies_normalize;
								float temp_ies_strength = entry_ies_strength;
								float temp_ies_focus = entry_ies_focus;
								bool temp_ies_emulation = entry_ies_emulation;
								int temp_ies_emulation_samples = entry_ies_emulation_samples;
								float temp_ies_emulation_spread = entry_ies_emulation_spread;
								float temp_ies_emulation_radius_scale = entry_ies_emulation_radius_scale;
								float temp_ies_emulation_intensity_scale = entry_ies_emulation_intensity_scale;
								float temp_ies_emulation_forward_offset = entry_ies_emulation_forward_offset;
								std::string temp_ies_emulation_pattern = entry_ies_emulation_pattern;
								float temp_ies_emulation_aspect = entry_ies_emulation_aspect;
								float temp_ies_emulation_twist = entry_ies_emulation_twist;
								if (p.contains("light_mode")) { try { temp_light_rig_mode = parse_light_rig_mode(p.at("light_mode").as_string()); temp_light_rig_mode_explicit = true; } CATCH_ERR; }
								else if (p.contains("rig_mode")) { try { temp_light_rig_mode = parse_light_rig_mode(p.at("rig_mode").as_string()); temp_light_rig_mode_explicit = true; } CATCH_ERR; }
								if (p.contains("ies_profile")) { try { temp_ies_profile = p.at("ies_profile").as_string(); } CATCH_ERR; }
								else if (p.contains("ies")) { try { temp_ies_profile = p.at("ies").as_string(); } CATCH_ERR; }
								if (p.contains("ies_file")) { try { temp_ies_file = p.at("ies_file").as_string(); } CATCH_ERR; }
								if (p.contains("ies_axis_rotation")) { temp_ies_axis_rotation = std::clamp(to_float(p.at("ies_axis_rotation"), entry_ies_axis_rotation), -360.0f, 360.0f); }
								if (p.contains("ies_angle_scale")) { temp_ies_angle_scale = std::clamp(to_float(p.at("ies_angle_scale"), entry_ies_angle_scale), 0.01f, 8.0f); }
								if (p.contains("ies_intensity_scale")) { temp_ies_intensity_scale = std::clamp(to_float(p.at("ies_intensity_scale"), entry_ies_intensity_scale), 0.0f, 32.0f); }
								if (p.contains("ies_normalize")) { temp_ies_normalize = to_bool(p.at("ies_normalize"), entry_ies_normalize); }
								if (p.contains("ies_strength")) { temp_ies_strength = std::clamp(to_float(p.at("ies_strength"), entry_ies_strength), 0.0f, 8.0f); }
								if (p.contains("ies_focus")) { temp_ies_focus = std::clamp(to_float(p.at("ies_focus"), entry_ies_focus), 0.05f, 8.0f); }
								if (p.contains("ies_emulation")) { parse_ies_emulation_table(p.at("ies_emulation"), temp_ies_emulation, temp_ies_emulation_samples, temp_ies_emulation_spread, temp_ies_emulation_radius_scale, temp_ies_emulation_intensity_scale, temp_ies_emulation_forward_offset, temp_ies_emulation_pattern, temp_ies_emulation_aspect, temp_ies_emulation_twist); }
								if (p.contains("ies_cluster_samples")) { temp_ies_emulation_samples = std::clamp(to_int(p.at("ies_cluster_samples"), temp_ies_emulation_samples), 0, 24); temp_ies_emulation = temp_ies_emulation_samples > 0; }
								if (p.contains("ies_cluster_spread")) { temp_ies_emulation_spread = std::clamp(to_float(p.at("ies_cluster_spread"), temp_ies_emulation_spread), 0.0f, 4.0f); }
								if (p.contains("ies_cluster_radius_scale")) { temp_ies_emulation_radius_scale = std::clamp(to_float(p.at("ies_cluster_radius_scale"), temp_ies_emulation_radius_scale), 0.01f, 4.0f); }
								if (p.contains("ies_cluster_intensity_scale")) { temp_ies_emulation_intensity_scale = std::clamp(to_float(p.at("ies_cluster_intensity_scale"), temp_ies_emulation_intensity_scale), 0.0f, 4.0f); }
								if (p.contains("ies_cluster_forward_offset")) { temp_ies_emulation_forward_offset = std::clamp(to_float(p.at("ies_cluster_forward_offset"), temp_ies_emulation_forward_offset), -4.0f, 4.0f); }
								if (p.contains("ies_cluster_pattern")) { try { temp_ies_emulation_pattern = utils::str_to_lower(p.at("ies_cluster_pattern").as_string()); utils::replace_all(temp_ies_emulation_pattern, "-", "_"); } CATCH_ERR; }
								if (p.contains("ies_cluster_aspect")) { temp_ies_emulation_aspect = std::clamp(to_float(p.at("ies_cluster_aspect"), temp_ies_emulation_aspect), 0.1f, 8.0f); }
								if (p.contains("ies_cluster_twist")) { temp_ies_emulation_twist = std::clamp(to_float(p.at("ies_cluster_twist"), temp_ies_emulation_twist), -360.0f, 360.0f); }

								if (!temp_light_rig_mode_explicit)
								{
									if (!temp_ies_file.empty()) temp_light_rig_mode = remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES;
									else if (temp_ies_emulation || temp_ies_emulation_samples > 0) temp_light_rig_mode = remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES;
								}
								temp_ies_emulation = temp_light_rig_mode == remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES && temp_ies_emulation_samples > 0;

								// Named pseudo profiles are destructive only in Fake IES mode. Native IES reads the file directly.
								if (temp_light_rig_mode == remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES && !temp_ies_profile.empty())
								{
									apply_pseudo_ies_profile(temp_ies_profile, temp_radiance, temp_radiance_scalar, temp_radius, temp_shaping_enabled, temp_direction, temp_degrees, temp_softness, temp_exponent, temp_ies_strength, temp_ies_focus);
								}

								// to avoid code duplication
								Vector pt;

								// using either position defined in current point or previous position
								if (point_has_valid_position)
								{
									const auto& positions = p.at("position").as_array();
									pt = Vector(to_float(positions[0]), to_float(positions[1]), to_float(positions[2]));
								}
								else {
									pt = temp_points.back().position; // pos of previous point
								}

								temp_points.emplace_back(
									remix_light_settings_s::point_s{
										.position = pt,
										.radiance = temp_radiance,
										.radiance_scalar = temp_radiance_scalar,
										.radius = temp_radius,
										.timepoint = temp_timepoint,
										.smoothness = temp_smoothness,
										.use_shaping = temp_shaping_enabled,
										.direction = temp_direction,
										.angle_offset_attached = temp_angle_offset_attached,
										.degrees = temp_degrees,
										.softness = temp_softness,
										.exponent = temp_exponent,
										.volumetric_scale = temp_volumetric,
										.authoring_shape = temp_authoring_shape,
										.authoring_width = temp_authoring_width,
										.authoring_height = temp_authoring_height,
										.authoring_length = temp_authoring_length,
										.authoring_range = temp_authoring_range,
										.light_rig_mode = temp_light_rig_mode,
										.ies_profile = std::move(temp_ies_profile),
										.ies_file = std::move(temp_ies_file),
										.ies_axis_rotation = temp_ies_axis_rotation,
										.ies_angle_scale = temp_ies_angle_scale,
										.ies_intensity_scale = temp_ies_intensity_scale,
										.ies_normalize = temp_ies_normalize,
										.ies_strength = temp_ies_strength,
										.ies_focus = temp_ies_focus,
										.ies_emulation = temp_ies_emulation,
										.ies_emulation_samples = temp_ies_emulation_samples,
										.ies_emulation_spread = temp_ies_emulation_spread,
										.ies_emulation_radius_scale = temp_ies_emulation_radius_scale,
										.ies_emulation_intensity_scale = temp_ies_emulation_intensity_scale,
										.ies_emulation_forward_offset = temp_ies_emulation_forward_offset,
										.ies_emulation_pattern = temp_ies_emulation_pattern,
										.ies_emulation_aspect = temp_ies_emulation_aspect,
										.ies_emulation_twist = temp_ies_emulation_twist }
										);
							}


							// V21.14.5 split animation metadata. Exported files also keep explicit points,
							// so loading never needs to destructively regenerate them here.
							std::string temp_property_animation = "stable";
							float temp_property_animation_duration = 2.0f;
							float temp_property_animation_speed = 1.0f;
							float temp_property_animation_variation = 0.0f;
							float temp_property_animation_intensity = temp_points.empty() ? -1.0f : temp_points.front().radiance_scalar;
							std::string temp_movement_animation = "none";
							float temp_movement_animation_duration = 2.0f;
							float temp_movement_animation_speed = 1.0f;
							Vector temp_movement_animation_axis = Vector(0.0f, 0.0f, 1.0f);
							float temp_movement_animation_degrees = 110.0f;
							float temp_movement_animation_phase = 0.0f;
							float temp_movement_animation_distance = 64.0f;
							const bool has_split_animation_metadata = entry.contains("property_animation") || entry.contains("movement_animation");
							if (entry.contains("property_animation")) { try { temp_property_animation = utils::str_to_lower(entry.at("property_animation").as_string()); } CATCH_ERR; utils::replace_all(temp_property_animation, "-", "_"); }
							if (entry.contains("property_animation_duration")) temp_property_animation_duration = std::max(0.05f, to_float(entry.at("property_animation_duration"), 2.0f));
							if (entry.contains("property_animation_speed")) temp_property_animation_speed = std::max(0.05f, to_float(entry.at("property_animation_speed"), 1.0f));
							if (entry.contains("property_animation_variation")) temp_property_animation_variation = std::clamp(to_float(entry.at("property_animation_variation"), 0.0f), 0.0f, 1.0f);
							if (entry.contains("property_animation_intensity")) temp_property_animation_intensity = std::max(0.0f, to_float(entry.at("property_animation_intensity"), temp_property_animation_intensity));
							if (entry.contains("movement_animation")) { try { temp_movement_animation = utils::str_to_lower(entry.at("movement_animation").as_string()); } CATCH_ERR; utils::replace_all(temp_movement_animation, "-", "_"); }
							if (entry.contains("movement_animation_duration")) temp_movement_animation_duration = std::max(0.05f, to_float(entry.at("movement_animation_duration"), 2.0f));
							if (entry.contains("movement_animation_speed")) temp_movement_animation_speed = std::max(0.05f, to_float(entry.at("movement_animation_speed"), 1.0f));
							if (entry.contains("movement_animation_axis") && entry.at("movement_animation_axis").is_array())
							{
								const auto& arr = entry.at("movement_animation_axis").as_array();
								if (arr.size() == 3u)
								{
									temp_movement_animation_axis = Vector(to_float(arr[0], 0.0f), to_float(arr[1], 0.0f), to_float(arr[2], 1.0f));
									if (temp_movement_animation_axis.LengthSqr() > 0.0001f) temp_movement_animation_axis.Normalize();
									else temp_movement_animation_axis = Vector(0.0f, 0.0f, 1.0f);
								}
							}
							if (entry.contains("movement_animation_degrees")) temp_movement_animation_degrees = std::clamp(to_float(entry.at("movement_animation_degrees"), 110.0f), 0.0f, 1440.0f);
							if (entry.contains("movement_animation_phase")) temp_movement_animation_phase = to_float(entry.at("movement_animation_phase"), 0.0f);
							if (entry.contains("movement_animation_distance")) temp_movement_animation_distance = std::max(0.0f, to_float(entry.at("movement_animation_distance"), 64.0f));

							// Optional procedural animation for regular [LIGHTS].
							// This keeps the original remix_lights/add-light path intact: the animation is expanded
							// into ordinary light points, so update_all_active_lights() and the existing interpolator
							// handle it exactly like a manually authored animated light.
							bool temp_animation_forces_loop = false;
							std::string temp_animation = "stable";
							float temp_animation_duration = 1.0f;
							float temp_animation_speed = 1.0f;
							float temp_animation_variation = 0.0f;
							Vector temp_animation_axis = Vector(0.0f, 0.0f, 1.0f);
							float temp_animation_degrees = 360.0f;
							float temp_animation_phase = 0.0f;
							if (!temp_points.empty() && entry.contains("animation"))
							{
								try { temp_animation = utils::str_to_lower(entry.at("animation").as_string()); }
								CATCH_ERR;
								utils::replace_all(temp_animation, "-", "_");

								if (!temp_animation.empty() && temp_animation != "stable" && temp_animation != "none" && temp_animation != "off")
								{
									auto normalize_or_anim = [](Vector value, const Vector& fallback) -> Vector
									{
										if (value.LengthSqr() <= 0.0001f) { value = fallback; }
										if (value.LengthSqr() <= 0.0001f) { value = Vector(0.0f, 0.0f, 1.0f); }
										value.Normalize();
										return value;
									};

									auto rotate_axis = [normalize_or_anim](Vector value, Vector axis, const float degrees) -> Vector
									{
										value = normalize_or_anim(value, Vector(0.0f, 1.0f, 0.0f));
										axis = normalize_or_anim(axis, Vector(0.0f, 0.0f, 1.0f));
										const float radians = degrees * static_cast<float>(M_PI / 180.0);
										const float c = std::cos(radians);
										const float sinv = std::sin(radians);
										const float d = value.Dot(axis);
										return normalize_or_anim(value * c + axis.Cross(value) * sinv + axis * (d * (1.0f - c)), value);
									};

									if (entry.contains("animation_axis") && entry.at("animation_axis").is_array())
									{
										const auto& arr = entry.at("animation_axis").as_array();
										if (arr.size() == 3u) {
											temp_animation_axis = normalize_or_anim(Vector(to_float(arr[0], 0.0f), to_float(arr[1], 0.0f), to_float(arr[2], 1.0f)), Vector(0.0f, 0.0f, 1.0f));
										}
									}

									temp_animation_degrees = entry.contains("animation_degrees") ? std::clamp(to_float(entry.at("animation_degrees"), 360.0f), 0.0f, 1440.0f) : 360.0f;
									temp_animation_phase = entry.contains("animation_phase") ? to_float(entry.at("animation_phase"), 0.0f) : 0.0f;
									temp_animation_speed = entry.contains("speed") ? std::max(0.05f, to_float(entry.at("speed"), 1.0f)) : 1.0f;
									temp_animation_variation = entry.contains("variation") ? std::clamp(to_float(entry.at("variation"), 0.0f), 0.0f, 1.0f) : 0.0f;
									temp_animation_duration = entry.contains("animation_duration") ? std::max(0.05f, to_float(entry.at("animation_duration"), 1.0f)) : std::max(0.05f, 1.0f / temp_animation_speed);
									const float cycle_time = temp_animation_duration;

									auto base = temp_points.front();
									base.timepoint = 0.0f;
									const float base_scalar = base.radiance_scalar;
									const Vector base_radiance = base.radiance;
									const Vector base_dir = normalize_or_anim(base.direction, Vector(0.0f, 1.0f, 0.0f));
									temp_points.clear();

									auto add_point = [&](float t, float scalar_mul, Vector dir, Vector radiance_override = Vector(-1.0f, -1.0f, -1.0f))
									{
										auto pt = base;
										pt.timepoint = t;
										pt.radiance_scalar = base_scalar * std::max(0.0f, scalar_mul);
										pt.direction = normalize_or_anim(dir, base_dir);
										if (radiance_override.x >= 0.0f) { pt.radiance = radiance_override; }
										temp_points.push_back(pt);
									};

									const float vlow = 1.0f - temp_animation_variation;
									const float vhigh = 1.0f + temp_animation_variation;

									if (temp_animation == "pulse_slow" || temp_animation == "pulse_fast" || temp_animation == "breathing")
									{
										const float low = temp_animation == "breathing" ? 0.30f : 0.18f;
										const float high = temp_animation == "pulse_fast" ? 1.35f : 1.08f;
										add_point(0.0f, low * vlow, base_dir);
										add_point(cycle_time * 0.5f, high * vhigh, base_dir);
										add_point(cycle_time, low * vlow, base_dir);
										temp_animation_forces_loop = true;
									}
									else if (temp_animation == "soft_flicker" || temp_animation == "broken_fluorescent" || temp_animation == "tv_noise" || temp_animation == "generator_stutter")
									{
										const float vals[] = { 0.20f, 1.10f, 0.55f, 1.30f, 0.08f, 0.95f, 0.35f, 1.15f, 0.20f };
										const int count = static_cast<int>(sizeof(vals) / sizeof(vals[0]));
										for (int i = 0; i < count; ++i) {
											add_point(cycle_time * (static_cast<float>(i) / static_cast<float>(count - 1)), vals[i] * vhigh, base_dir);
										}
										temp_animation_forces_loop = true;
									}
									else if (temp_animation == "fire_pulse" || temp_animation == "flame_small" || temp_animation == "flame_large")
									{
										const float vals[] = { 0.68f, 1.18f, 0.84f, 1.35f, 0.74f, 1.08f, 0.62f };
										const int count = static_cast<int>(sizeof(vals) / sizeof(vals[0]));
										for (int i = 0; i < count; ++i)
										{
											const float u = static_cast<float>(i) / static_cast<float>(count - 1);
											const Vector fire_color = Vector(1.0f, 0.30f + 0.20f * ((i % 3) / 2.0f), 0.05f + 0.08f * u);
											add_point(cycle_time * u, vals[i] * vhigh, base_dir, fire_color);
										}
										temp_animation_forces_loop = true;
									}
									else if (temp_animation == "candle_flicker" || temp_animation == "unstable_bulb" || temp_animation == "fluorescent_random")
									{
										const float* vals = nullptr;
										int count = 0;
										const float candle_vals[] = { 0.62f, 1.05f, 0.72f, 1.18f, 0.66f };
										const float bulb_vals[] = { 0.92f, 0.28f, 1.22f, 0.48f, 1.08f, 0.88f };
										const float fl_vals[] = { 0.05f, 1.25f, 0.02f, 0.70f, 0.10f, 1.05f, 0.36f };
										if (temp_animation == "candle_flicker") { vals = candle_vals; count = static_cast<int>(sizeof(candle_vals) / sizeof(candle_vals[0])); }
										else if (temp_animation == "unstable_bulb") { vals = bulb_vals; count = static_cast<int>(sizeof(bulb_vals) / sizeof(bulb_vals[0])); }
										else { vals = fl_vals; count = static_cast<int>(sizeof(fl_vals) / sizeof(fl_vals[0])); }
										for (int i = 0; i < count; ++i) {
											Vector color = base_radiance;
											if (temp_animation == "candle_flicker" && (i % 2)) { color = Vector(1.0f, 0.46f + 0.04f * static_cast<float>(i), 0.10f); }
											add_point(cycle_time * (static_cast<float>(i) / static_cast<float>(std::max(1, count - 1))), vals[i] * vhigh, base_dir, color);
										}
										temp_animation_forces_loop = true;
									}
									else if (temp_animation == "strobe_fast" || temp_animation == "strobe_slow")
									{
										const int flashes = temp_animation == "strobe_fast" ? 5 : 2;
										for (int i = 0; i <= flashes * 2; ++i) {
											add_point(cycle_time * (static_cast<float>(i) / static_cast<float>(flashes * 2)), (i % 2) ? 1.8f * vhigh : 0.02f, base_dir);
										}
										temp_animation_forces_loop = true;
									}
									else if (temp_animation == "rotating_yaw" || temp_animation == "rotating_pitch" || temp_animation == "rotating_roll" || temp_animation == "rotating_beacon" || temp_animation == "warning_beacon" || temp_animation == "police_red_blue" || temp_animation == "lighthouse_sweep" || temp_animation == "disc_spin" || temp_animation == "spot_axis_spin")
									{
										if (temp_animation == "rotating_pitch") { temp_animation_axis = Vector(1.0f, 0.0f, 0.0f); }
										else if (temp_animation == "rotating_roll") { temp_animation_axis = Vector(0.0f, 1.0f, 0.0f); }
										base.use_shaping = true;
										if (base.degrees >= 179.9f) { base.degrees = 38.0f; }
										const int steps = temp_animation == "police_red_blue" ? 8 : 9;
										for (int i = 0; i < steps; ++i)
										{
											const float u = static_cast<float>(i) / static_cast<float>(steps - 1);
											const float angle = temp_animation_phase + temp_animation_degrees * u;
											float scalar = (temp_animation == "warning_beacon" || temp_animation == "rotating_beacon") ? (0.25f + 1.10f * (0.5f + 0.5f * std::sin(u * static_cast<float>(M_PI) * 2.0f))) : 1.0f;
											Vector color = base_radiance;
											if (temp_animation == "police_red_blue") { color = (i % 2) ? Vector(0.25f, 0.35f, 1.0f) : Vector(1.0f, 0.08f, 0.03f); scalar = 1.25f; }
											add_point(cycle_time * u, scalar * vhigh, rotate_axis(base_dir, temp_animation_axis, angle), color);
										}
										temp_animation_forces_loop = true;
									}
									else if (temp_animation == "searchlight_sweep" || temp_animation == "pendulum_sweep" || temp_animation == "spot_axis_sweep")
									{
										base.use_shaping = true;
										if (base.degrees >= 179.9f) { base.degrees = 35.0f; }
										const float half = std::max(0.0f, temp_animation_degrees) * 0.5f;
										add_point(0.0f, 0.85f * vlow, rotate_axis(base_dir, temp_animation_axis, temp_animation_phase - half));
										add_point(cycle_time * 0.5f, 1.15f * vhigh, rotate_axis(base_dir, temp_animation_axis, temp_animation_phase + half));
										add_point(cycle_time, 0.85f * vlow, rotate_axis(base_dir, temp_animation_axis, temp_animation_phase - half));
										temp_animation_forces_loop = true;
									}
									else
									{
										// Unknown animation preset: keep the light stable instead of failing map load.
										temp_points.push_back(base);
									}
								}
							}

							// Migrate legacy metadata into one of the two editable modules without changing
							// the already generated points.
							if (!has_split_animation_metadata && !temp_animation.empty() && temp_animation != "stable" && temp_animation != "none" && temp_animation != "off")
							{
								if (temp_animation == "rotating_yaw") temp_movement_animation = "rotate_yaw";
								else if (temp_animation == "disc_spin" || temp_animation == "spot_axis_spin") temp_movement_animation = "axis_spin";
								else if (temp_animation == "spot_axis_sweep" || temp_animation == "searchlight_sweep") temp_movement_animation = "sweep";
								else if (temp_animation == "pendulum_sweep") temp_movement_animation = "pendulum";
								else temp_property_animation = temp_animation;
								if (temp_movement_animation != "none")
								{
									temp_movement_animation_duration = temp_animation_duration;
									temp_movement_animation_speed = temp_animation_speed;
									temp_movement_animation_axis = temp_animation_axis;
									temp_movement_animation_degrees = temp_animation_degrees;
									temp_movement_animation_phase = temp_animation_phase;
								}
								else
								{
									temp_property_animation_duration = temp_animation_duration;
									temp_property_animation_speed = temp_animation_speed;
									temp_property_animation_variation = temp_animation_variation;
								}
							}

							// attach settings

							float temp_attach_prop_radius = 0.0f;
							std::string temp_attach_prop_str;
							Vector temp_attach_prop_bounds_min;
							Vector temp_attach_prop_bounds_max;

							int temp_attach_bone_index = -1;
							std::string temp_attach_bone_str;

							if (entry.contains("attach"))
							{
								bool has_valid_attach = false;
								const auto& attach = entry.at("attach");

								if (attach.contains("radius"))
								{
									temp_attach_prop_radius = to_float(attach.at("radius"), 0.0f);
									has_valid_attach = true;
								}
								else if (attach.contains("name"))
								{
									try { temp_attach_prop_str = attach.at("name").as_string(); }
									CATCH_ERR;

									has_valid_attach = true;
								}

								if (has_valid_attach)
								{
									m_map_settings.using_any_light_attached_to_prop = true;

									if (attach.contains("bounds"))
									{
										if (const auto& bounds = attach.at("bounds").as_array();
											bounds.size() == 6u)
										{
											temp_attach_prop_bounds_min = Vector(to_float(bounds[0]), to_float(bounds[1]), to_float(bounds[2]));
											temp_attach_prop_bounds_max = Vector(to_float(bounds[3]), to_float(bounds[4]), to_float(bounds[5]));
										}
									}

									if (attach.contains("bone_index")) {
										temp_attach_bone_index = to_int(attach.at("bone_index"), -1);
									}

									if (attach.contains("bone_name"))
									{
										try { temp_attach_bone_str = attach.at("bone_name").as_string(); }
										CATCH_ERR;
									}
								}
							}

							// - parse general settings

							if (!temp_points.empty())
							{
								bool temp_run_once = false;
								if (entry.contains("run_once")) {
									temp_run_once = to_bool(entry.at("run_once"), false);
								}

								bool temp_loop = temp_animation_forces_loop;
								if (entry.contains("loop")) {
									temp_loop = to_bool(entry.at("loop"), temp_animation_forces_loop);
								}

								bool temp_loop_smoothing = false;
								if (entry.contains("loop_smoothing")) {
									temp_loop_smoothing = to_bool(entry.at("loop_smoothing"), false);
								}

								m_map_settings.remix_lights.push_back(
									remix_light_settings_s{
										.points = std::move(temp_points),
										.run_once = temp_run_once,
										.loop = temp_loop,
										.loop_smoothing = temp_loop_smoothing,
										.animation = std::move(temp_animation),
										.animation_duration = temp_animation_duration,
										.animation_speed = temp_animation_speed,
										.animation_variation = temp_animation_variation,
										.animation_axis = temp_animation_axis,
										.animation_degrees = temp_animation_degrees,
										.animation_phase = temp_animation_phase,
										.property_animation = std::move(temp_property_animation),
										.property_animation_duration = temp_property_animation_duration,
										.property_animation_speed = temp_property_animation_speed,
										.property_animation_variation = temp_property_animation_variation,
										.property_animation_intensity = temp_property_animation_intensity,
										.movement_animation = std::move(temp_movement_animation),
										.movement_animation_duration = temp_movement_animation_duration,
										.movement_animation_speed = temp_movement_animation_speed,
										.movement_animation_axis = temp_movement_animation_axis,
										.movement_animation_degrees = temp_movement_animation_degrees,
										.movement_animation_phase = temp_movement_animation_phase,
										.movement_animation_distance = temp_movement_animation_distance,
										.trigger_always = temp_trigger_always,

										.trigger_choreo_name = std::move(temp_trigger_choreo_name),
										.trigger_choreo_actor = std::move(temp_trigger_choreo_actor),
										.trigger_choreo_event = std::move(temp_trigger_choreo_event),
										.trigger_choreo_param1 = std::move(temp_trigger_choreo_param1),
										.trigger_sound_hash = temp_trigger_sound,
										.trigger_delay = temp_trigger_delay,

										.kill_choreo_name = std::move(temp_kill_choreo_name),
										.kill_sound_hash = temp_kill_sound,
										.kill_delay = temp_kill_delay,

										.attach_prop_radius = temp_attach_prop_radius,
										.attach_prop_name = std::move(temp_attach_prop_str),
										.attach_prop_mins = temp_attach_prop_bounds_min,
										.attach_prop_maxs = temp_attach_prop_bounds_max,
										.attach_bone_index = temp_attach_bone_index,
										.attach_bone_name = temp_attach_bone_str,

										.comment = std::move(temp_comment),
										.enabled = temp_enabled,
										.group = std::move(temp_group)
									});
							}
						}
						else { TOML_ERROR("[LIGHTS] #points", entry, "needs at least one point to define a light"); }
					};

				// try to find the loaded map
				if (light_table.contains(m_map_settings.mapname))
				{
					if (const auto& map = light_table[m_map_settings.mapname];
						!map.is_empty() && !map.as_array().empty())
					{
						for (const auto& entry : map.as_array()) {
							process_light_entry(entry);
						}
					}
				}
			} // end 'LIGHTS'
		}

		catch (const toml::syntax_error& err)
		{
			game::console();
			printf("%s\n", err.what());
			return false;
		}

		return true;
	}

	bool map_settings::matches_map_name()
	{
		return utils::str_to_lower(m_args[0]) == m_map_settings.mapname;
	}

	void map_settings::open_and_set_var_config(const std::string& config, const bool no_error, const bool ignore_hashes, const char* custom_path)
	{
		std::string path = COMPMOD_ASSET_DIR "map_configs";
		if (custom_path)
		{
			path = custom_path;
		}

		std::ifstream file;
		if (utils::open_file_homepath(path, config, file))
		{
			std::string input;
			while (std::getline(file, input))
			{
				if (utils::starts_with(input, "#")) {
					continue;
				}

				if (auto pair = utils::split(input, '=');
					pair.size() == 2u)
				{
					utils::trim(pair[0]);
					utils::trim(pair[1]);

					if (ignore_hashes && pair[1].starts_with("0x")) {
						continue;
					}

					if (pair[1].empty()) {
						continue;
					}

					if (const auto o = remix_vars::get_option(pair[0].c_str()); o)
					{
						const auto& v = remix_vars::string_to_option_value(o->second.type, pair[1]);
						remix_vars::set_option(o, v, true);
					}
				}
			}

			file.close();
		}
		else if (!no_error)
		{
			game::console();
			printf("[MapSettings] Failed to find config: \"%s\" in %s \n", config.c_str(), custom_path ? custom_path : "\"" COMPMOD_ASSET_DIR "map_configs\"");
		}
	}


	std::string map_settings::serialize_current_map_authoring_data()
	{
		if (m_map_settings.mapname.empty()) return {};
		return std::string("[MARKER]\n") + common::toml::build_map_marker_string_for_current_map(m_map_settings.map_markers) +
			"\n[CULL]\n" + common::toml::build_culling_overrides_string_for_current_map(m_map_settings.area_settings);
	}

	bool map_settings::replace_map_section_entry(std::string& document, const std::string& section, const std::string& map_name, const std::string& assignment)
	{
		const std::string header = "[" + section + "]";
		auto is_line_start = [&document](const std::size_t pos) { return pos == 0u || document[pos - 1u] == '\n'; };
		std::size_t header_pos = document.find(header);
		while (header_pos != std::string::npos && !is_line_start(header_pos)) header_pos = document.find(header, header_pos + 1u);

		if (header_pos == std::string::npos)
		{
			if (!document.empty() && document.back() != '\n') document.push_back('\n');
			document += "\n" + header + "\n" + assignment + "\n";
			return true;
		}

		const auto header_line_end = document.find('\n', header_pos);
		const std::size_t section_begin = header_line_end == std::string::npos ? document.size() : header_line_end + 1u;
		std::size_t section_end = document.size();
		for (std::size_t pos = section_begin; pos < document.size();)
		{
			const auto line_end = document.find('\n', pos);
			const auto logical_end = line_end == std::string::npos ? document.size() : line_end;
			std::size_t first = pos;
			while (first < logical_end && (document[first] == ' ' || document[first] == '\t' || document[first] == '\r')) ++first;
			if (first < logical_end && document[first] == '[') {
				section_end = pos;
				break;
			}
			if (line_end == std::string::npos) break;
			pos = line_end + 1u;
		}

		std::size_t entry_start = std::string::npos;
		for (std::size_t pos = section_begin; pos < section_end;)
		{
			const auto line_end = document.find('\n', pos);
			const auto logical_end = std::min(line_end == std::string::npos ? document.size() : line_end, section_end);
			std::size_t first = pos;
			while (first < logical_end && (document[first] == ' ' || document[first] == '\t' || document[first] == '\r')) ++first;
			if (first < logical_end && document[first] != '#')
			{
				if (document.compare(first, map_name.size(), map_name) == 0)
				{
					std::size_t after = first + map_name.size();
					while (after < logical_end && (document[after] == ' ' || document[after] == '\t')) ++after;
					if (after < logical_end && document[after] == '=') {
						entry_start = pos;
						break;
					}
				}
			}
			if (line_end == std::string::npos) break;
			pos = line_end + 1u;
		}

		if (entry_start == std::string::npos)
		{
			std::string insertion;
			if (section_end > 0u && document[section_end - 1u] != '\n') insertion.push_back('\n');
			insertion += assignment;
			if (!insertion.empty() && insertion.back() != '\n') insertion.push_back('\n');
			document.insert(section_end, insertion);
			return true;
		}

		const auto equals = document.find('=', entry_start);
		const auto array_start = equals == std::string::npos ? std::string::npos : document.find('[', equals);
		if (array_start == std::string::npos || array_start >= section_end) return false;

		int depth = 0;
		bool in_string = false;
		bool escape = false;
		bool in_comment = false;
		std::size_t entry_end = std::string::npos;
		for (std::size_t i = array_start; i < document.size(); ++i)
		{
			const char c = document[i];
			if (in_comment) {
				if (c == '\n') in_comment = false;
				continue;
			}
			if (in_string) {
				if (escape) escape = false;
				else if (c == '\\') escape = true;
				else if (c == '"') in_string = false;
				continue;
			}
			if (c == '#') { in_comment = true; continue; }
			if (c == '"') { in_string = true; continue; }
			if (c == '[') ++depth;
			else if (c == ']') {
				--depth;
				if (depth == 0) {
					entry_end = i + 1u;
					while (entry_end < document.size() && (document[entry_end] == ' ' || document[entry_end] == '\t' || document[entry_end] == '\r')) ++entry_end;
					if (entry_end < document.size() && document[entry_end] == '\n') ++entry_end;
					break;
				}
			}
		}
		if (entry_end == std::string::npos) return false;

		std::string replacement = assignment;
		if (!replacement.empty() && replacement.back() != '\n') replacement.push_back('\n');
		document.replace(entry_start, entry_end - entry_start, replacement);
		return true;
	}

	void map_settings::mark_map_data_dirty(const char* reason)
	{
		if (!m_map_autosave_ready || m_map_settings.mapname.empty()) return;
		m_map_data_dirty = true;
		m_map_last_change_time = std::chrono::steady_clock::now();
		m_map_dirty_reason = reason && *reason ? reason : "map authoring change";
		m_map_save_status = std::format("Pending map auto-save ({})", m_map_dirty_reason);
	}

	bool map_settings::save_current_map_authoring_data(const bool force)
	{
		if (m_map_settings.mapname.empty() || (!force && !m_map_data_dirty)) return false;

		const auto path = std::filesystem::path(game::root_path + COMPMOD_ASSET_DIR "map_settings.toml");
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);
		std::string document;
		{
			std::ifstream input(path, std::ios::binary);
			if (input.is_open()) document.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		}

		const auto markers = common::toml::build_map_marker_string_for_current_map(m_map_settings.map_markers);
		const auto culling = common::toml::build_culling_overrides_string_for_current_map(m_map_settings.area_settings);
		if (!replace_map_section_entry(document, "MARKER", m_map_settings.mapname, markers) ||
			!replace_map_section_entry(document, "CULL", m_map_settings.mapname, culling))
		{
			m_map_save_status = "Map auto-save failed: malformed target section";
			m_map_last_change_time = std::chrono::steady_clock::now();
			return false;
		}

		const auto temp = std::filesystem::path(path.wstring() + L".tmp");
		std::ofstream output(temp, std::ios::binary | std::ios::trunc);
		if (!output.is_open()) {
			m_map_save_status = "Map auto-save failed: cannot open temporary file";
			m_map_last_change_time = std::chrono::steady_clock::now();
			return false;
		}
		output.write(document.data(), static_cast<std::streamsize>(document.size()));
		output.flush();
		const bool stream_ok = output.good();
		output.close();
		if (!stream_ok)
		{
			std::filesystem::remove(temp, ec);
			m_map_save_status = "Map auto-save failed: temporary file write error";
			m_map_last_change_time = std::chrono::steady_clock::now();
			return false;
		}

		if (!MoveFileExW(temp.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			const auto error = GetLastError();
			std::filesystem::remove(temp, ec);
			m_map_save_status = std::format("Map auto-save failed: atomic replace error {}", error);
			m_map_last_change_time = std::chrono::steady_clock::now();
			return false;
		}

		m_map_last_serialized = serialize_current_map_authoring_data();
		m_map_data_dirty = false;
		++m_map_save_generation;
		m_map_save_status = std::format("Map data saved: {} (generation {})", m_map_settings.mapname, m_map_save_generation);
		return true;
	}

	void map_settings::on_frame_autosave()
	{
		if (!m_map_autosave_ready || m_map_settings.mapname.empty()) return;
		using clock = std::chrono::steady_clock;
		static auto next_probe = clock::now();
		const auto now = clock::now();
		const auto* im = imgui::get();
		if (!m_map_data_dirty)
		{
			// Serialising every marker/culling entry on the render thread was another
			// periodic hitch. Probe only while the authoring UI is open.
			if (!im || !im->m_menu_active || now < next_probe) return;
			next_probe = now + std::chrono::seconds(1);
			const auto serialized = serialize_current_map_authoring_data();
			if (serialized != m_map_last_serialized) mark_map_data_dirty("markers/culling changed");
		}
		if (!m_map_data_dirty) return;
		if (now - m_map_last_change_time < m_map_autosave_debounce) return;
		save_current_map_authoring_data(false);
	}

	void map_settings::on_map_load(const std::string& map_name)
	{
		if (m_loaded) {
			get()->clear_map_settings();
		}

		get()->set_settings_for_map(map_name);
		m_map_last_serialized = serialize_current_map_authoring_data();
		m_map_data_dirty = false;
		m_map_autosave_ready = true;
		m_map_save_status = std::format("Map data loaded: {}", get_map_name());

		is_level.reset();
		is_level.update(get_map_name());
	}

	void map_settings::on_map_unload()
	{
		if (m_map_data_dirty) save_current_map_authoring_data(true);
		get()->clear_map_settings();
	}

	void map_settings::clear_map_settings()
	{
		if (m_map_data_dirty && m_map_autosave_ready) save_current_map_authoring_data(true);
		m_map_autosave_ready = false;
		remix_lights::get()->destroy_and_clear_all_active_lights();
		m_map_settings.remix_lights.clear();
		m_map_settings.using_any_light_sound_hash = false;
		m_map_settings.using_any_transition_sound_hash = false;
		m_map_settings.using_any_transition_sound_name = false;


		m_map_settings.area_settings.clear();
		m_map_settings.hide_models.substrings.clear();
		m_map_settings.hide_models.radii.clear();
		m_map_settings.unbake_models.checksums.clear();
		m_map_settings.remix_transitions.clear();

		destroy_markers();
		m_map_settings.map_markers.clear();

		m_map_settings.api_var_configs.clear();
		m_map_settings = {};
		m_loaded = false;
		m_map_data_dirty = false;
		m_map_last_serialized.clear();
		m_map_save_status = "Map data unloaded";

		main_module::trigger_vis_logic();
	}

	ConCommand xo_mapsettings_update {};
	void map_settings::reload()
	{
		get()->clear_map_settings();
		get()->set_settings_for_map("");
		m_map_last_serialized = serialize_current_map_authoring_data();
		m_map_data_dirty = false;
		m_map_autosave_ready = true;
		m_map_save_status = std::format("Map data reloaded: {}", get_map_name());
		imgui::get()->m_light_edit_mode = false;
	}

	map_settings::map_settings()
	{
		p_this = this;
		game::con_add_command(&xo_mapsettings_update, "xo_mapsettings_update", map_settings::reload, "Reloads the map_settings.toml file + map.conf");
	}

#undef CATCH_ERR
}
