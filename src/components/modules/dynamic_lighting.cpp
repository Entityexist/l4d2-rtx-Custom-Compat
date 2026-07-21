#include "std_include.hpp"
#include "components/common/muzzle_sound_classifier.hpp"
#include "material_exporter.hpp"
#include "source_bsp_lights.hpp"
#include "source_map_entities.hpp"
#include "source_map_light_overrides.hpp"

#include <limits>
#include <sstream>

namespace components
{
	extern int g_current_leaf;

	namespace
	{
		source_map_entities::graph_s g_map_entity_graph = {};
		std::string g_map_entity_graph_map;
		bool g_map_entity_graph_loaded = false;
		// Distant import must remain available when the broader owner/I/O entity graph is
		// disabled. Keep a private BSP entity parse so enabling sunlight does not silently
		// re-enable unrelated owner binding or accepted-input hooks.
		source_map_entities::graph_s g_source_environment_graph = {};
		std::string g_source_environment_graph_map;
		bool g_source_environment_graph_loaded = false;
		source_map_light_overrides::load_result_s g_map_light_overrides = {};
		std::uint64_t g_persistent_next_user_light_id = 1u;

		std::string normalize_map_session_name(std::string map)
		{
			utils::trim(map);
			std::replace(map.begin(), map.end(), '\\', '/');
			map = utils::str_to_lower(map);
			const auto slash = map.find_last_of('/');
			if (slash != std::string::npos) map.erase(0u, slash + 1u);
			if (map.ends_with(".bsp")) map.resize(map.size() - 4u);
			return map;
		}

		struct captured_engine_light_s
		{
			components::dlight_t* light = nullptr;
			int key = 0;
			bool entity_light = false;
			float allocated_at = 0.0f;
		};

		std::unordered_map<std::uintptr_t, captured_engine_light_s> g_captured_engine_lights = {};
		using alloc_engine_light_fn = components::dlight_t* (__thiscall*)(sdk::engine_effects*, int);
		alloc_engine_light_fn g_alloc_dlight_original = nullptr;
		alloc_engine_light_fn g_alloc_elight_original = nullptr;

		std::uintptr_t engine_light_tracking_key(const components::dlight_t* light, const bool entity_light)
		{
			const auto address = reinterpret_cast<std::uintptr_t>(light);
			return entity_light ? (address | static_cast<std::uintptr_t>(1u)) : address;
		}

		void remember_allocated_engine_light(components::dlight_t* light, const int key, const bool entity_light)
		{
			if (!light || !dynamic_lighting::m_source_alloc_hooks_enabled) return;
			const auto* intf = interfaces::get();
			const float curtime = intf && intf->m_globals ? intf->m_globals->curtime : 0.0f;
			g_captured_engine_lights[engine_light_tracking_key(light, entity_light)] = { light, key, entity_light, curtime };
			if (entity_light) ++dynamic_lighting::m_source_alloc_elight_calls;
			else ++dynamic_lighting::m_source_alloc_dlight_calls;
		}

		components::dlight_t* __fastcall alloc_dlight_hk(sdk::engine_effects* self, void*, const int key)
		{
			auto* light = g_alloc_dlight_original ? g_alloc_dlight_original(self, key) : nullptr;
			remember_allocated_engine_light(light, key, false);
			return light;
		}

		components::dlight_t* __fastcall alloc_elight_hk(sdk::engine_effects* self, void*, const int key)
		{
			auto* light = g_alloc_elight_original ? g_alloc_elight_original(self, key) : nullptr;
			remember_allocated_engine_light(light, key, true);
			return light;
		}

		bool install_source_light_allocation_hooks()
		{
			if (dynamic_lighting::m_source_alloc_hooks_installed) return true;
			++dynamic_lighting::m_source_alloc_hook_attempts;
			const auto* intf = interfaces::get();
			if (!intf || !intf->m_effects)
			{
				dynamic_lighting::m_source_alloc_hook_status = "waiting for VEngineEffects001";
				return false;
			}

			auto** table = *reinterpret_cast<void***>(intf->m_effects);
			if (!table)
			{
				++dynamic_lighting::m_source_alloc_hook_failures;
				dynamic_lighting::m_source_alloc_hook_status = "VEngineEffects001 has no vtable";
				return false;
			}

			// IVEfx / VEngineEffects001: CL_AllocDlight = 4, CL_AllocElight = 5,
			// CL_GetActiveDLights = 6. The existing interface wrapper already uses slot 6.
			void* dlight_target = table[4];
			void* elight_target = table[5];
			if (!dlight_target || !elight_target)
			{
				++dynamic_lighting::m_source_alloc_hook_failures;
				dynamic_lighting::m_source_alloc_hook_status = "allocation vtable slots unavailable";
				return false;
			}

			const auto dlight_status = MH_CreateHook(dlight_target, alloc_dlight_hk, reinterpret_cast<void**>(&g_alloc_dlight_original));
			const auto elight_status = MH_CreateHook(elight_target, alloc_elight_hk, reinterpret_cast<void**>(&g_alloc_elight_original));
			const bool dlight_created = dlight_status == MH_OK ||
				(dlight_status == MH_ERROR_ALREADY_CREATED && g_alloc_dlight_original != nullptr);
			const bool elight_created = elight_status == MH_OK ||
				(elight_status == MH_ERROR_ALREADY_CREATED && g_alloc_elight_original != nullptr);
			if (!dlight_created || !elight_created)
			{
				++dynamic_lighting::m_source_alloc_hook_failures;
				dynamic_lighting::m_source_alloc_hook_status = std::format("hook create failed: dlight={} elight={}",
					static_cast<int>(dlight_status), static_cast<int>(elight_status));
				return false;
			}

			const auto dlight_enable = MH_EnableHook(dlight_target);
			const auto elight_enable = MH_EnableHook(elight_target);
			const bool dlight_enabled = dlight_enable == MH_OK || dlight_enable == MH_ERROR_ENABLED;
			const bool elight_enabled = elight_enable == MH_OK || elight_enable == MH_ERROR_ENABLED;
			if (!dlight_enabled || !elight_enabled)
			{
				++dynamic_lighting::m_source_alloc_hook_failures;
				dynamic_lighting::m_source_alloc_hook_status = std::format("hook enable failed: dlight={} elight={}",
					static_cast<int>(dlight_enable), static_cast<int>(elight_enable));
				return false;
			}

			dynamic_lighting::m_source_alloc_hooks_installed = true;
			dynamic_lighting::m_source_alloc_hook_status = "CL_AllocDlight / CL_AllocElight hooks installed";
			return true;
		}


		using server_accept_input_fn = bool(__thiscall*)(CBaseEntity*, const char*,
			CBaseEntity*, CBaseEntity*, int, int);

		struct captured_server_input_s
		{
			float captured_at = 0.0f;
			std::int32_t hammer_id = -1;
			std::string classname;
			std::string targetname;
			std::string input;
		};

		std::unordered_map<CBaseEntity_vtbl*, server_accept_input_fn> g_server_accept_input_originals = {};
		std::unordered_set<std::string> g_server_accept_input_attempted_classes = {};
		std::deque<captured_server_input_s> g_server_accept_input_events = {};
		std::mutex g_server_accept_input_mutex;
		bool g_installing_server_accept_input_hooks = false;

		std::string copy_server_string(const char* value, const std::size_t max_length = 192u)
		{
			if (!value || IsBadStringPtrA(value, static_cast<UINT_PTR>(max_length))) return {};
			std::size_t length = 0u;
			while (length < max_length && value[length] != '\0') ++length;
			return std::string(value, length);
		}

		float capture_server_curtime()
		{
			const auto* intf = interfaces::get();
			return intf && intf->m_globals ? intf->m_globals->curtime : 0.0f;
		}

		bool __fastcall server_accept_input_hk(CBaseEntity* self, void*, const char* input_name,
			CBaseEntity* activator, CBaseEntity* caller, const int value, const int output_id)
		{
			server_accept_input_fn original = nullptr;
			{
				std::scoped_lock lock(g_server_accept_input_mutex);
				if (self && self->vtbl)
				{
					if (const auto found = g_server_accept_input_originals.find(self->vtbl);
						found != g_server_accept_input_originals.end())
					{
						original = found->second;
					}
				}
			}
			if (!original) return false;

			captured_server_input_s captured = {};
			const bool capture = dynamic_lighting::m_map_event_lights_enabled &&
				dynamic_lighting::m_map_event_accept_input &&
				dynamic_lighting::m_server_accept_input_capture_enabled &&
				!g_installing_server_accept_input_hooks;
			if (capture && self)
			{
				captured.captured_at = capture_server_curtime();
				captured.hammer_id = self->m_iHammerID;
				captured.classname = copy_server_string(self->m_iClassname);
				captured.targetname = copy_server_string(self->m_iName);
				captured.input = copy_server_string(input_name, 96u);
				++dynamic_lighting::m_server_accept_input_calls;
			}

			const bool accepted = original(self, input_name, activator, caller, value, output_id);
			if (!capture || !accepted || captured.input.empty()) return accepted;

			++dynamic_lighting::m_server_accept_input_accepted;
			std::scoped_lock lock(g_server_accept_input_mutex);
			constexpr std::size_t event_limit = 1024u;
			if (g_server_accept_input_events.size() >= event_limit)
			{
				g_server_accept_input_events.pop_front();
				++dynamic_lighting::m_server_accept_input_dropped;
			}
			g_server_accept_input_events.emplace_back(std::move(captured));
			return accepted;
		}

		bool patch_server_accept_input_vtable(CBaseEntity_vtbl* table)
		{
			if (!table || !table->AcceptInput) return false;
			std::scoped_lock lock(g_server_accept_input_mutex);
			if (g_server_accept_input_originals.contains(table)) return true;
			if (table->AcceptInput == reinterpret_cast<server_accept_input_fn>(&server_accept_input_hk)) return false;

			DWORD old_protect = 0u;
			if (!VirtualProtect(&table->AcceptInput, sizeof(table->AcceptInput), PAGE_EXECUTE_READWRITE, &old_protect))
			{
				return false;
			}
			const auto original = table->AcceptInput;
			table->AcceptInput = reinterpret_cast<server_accept_input_fn>(&server_accept_input_hk);
			DWORD ignored = 0u;
			VirtualProtect(&table->AcceptInput, sizeof(table->AcceptInput), old_protect, &ignored);
			FlushInstructionCache(GetCurrentProcess(), &table->AcceptInput, sizeof(table->AcceptInput));
			g_server_accept_input_originals.emplace(table, original);
			return true;
		}

		bool should_probe_accept_input_class(const source_map_entities::entity_s& entity)
		{
			if (entity.classname.empty() || entity.classname == "worldspawn" || entity.classname.starts_with("info_")) return false;
			return entity.is_light || entity.is_bindable_owner || !entity.outputs.empty() ||
				entity.classname.starts_with("logic_") || entity.classname.starts_with("math_") ||
				entity.classname.starts_with("trigger_") || entity.classname.starts_with("func_button") ||
				entity.classname == "multi_manager" || entity.classname == "multi_manager_ext";
		}

		bool install_server_accept_input_hooks()
		{
			if (dynamic_lighting::m_server_accept_input_hook_scan_complete)
			{
				return dynamic_lighting::m_server_accept_input_hooks_installed;
			}

			++dynamic_lighting::m_server_accept_input_hook_attempts;
			if (!dynamic_lighting::m_server_accept_input_capture_enabled)
			{
				dynamic_lighting::m_server_accept_input_hook_status = "disabled";
				dynamic_lighting::m_server_accept_input_hook_scan_complete = true;
				return false;
			}
			if (!game::server_module || !g_map_entity_graph_loaded)
			{
				dynamic_lighting::m_server_accept_input_hook_status = "waiting for server.dll and map entity graph";
				return false;
			}
			if (!game::server_tools_available())
			{
				dynamic_lighting::m_server_accept_input_hook_status = "waiting for IServerTools; no prototype scan performed";
				return false;
			}

			std::vector<std::string> classes;
			classes.reserve(64u);
			std::unordered_set<std::string> unique;
			for (const auto& entity : g_map_entity_graph.entities)
			{
				if (!should_probe_accept_input_class(entity) || !unique.insert(entity.classname).second) continue;
				classes.push_back(entity.classname);
				if (classes.size() >= 96u) break;
			}

			constexpr std::uint32_t max_classes_per_frame = 4u;
			std::uint32_t installed_now = 0u;
			std::uint32_t failed_now = 0u;
			std::uint32_t processed_now = 0u;
			std::uint32_t remaining = 0u;
			g_installing_server_accept_input_hooks = true;
			for (const auto& classname : classes)
			{
				if (g_server_accept_input_attempted_classes.contains(classname)) continue;
				if (processed_now >= max_classes_per_frame)
				{
					continue;
				}

				// Mark every class before probing. A class that cannot be instantiated is a
				// terminal result for this process and must not be recreated every second.
				g_server_accept_input_attempted_classes.insert(classname);
				++processed_now;
				auto* prototype = game::server_tools_create_entity(classname.c_str());
				if (!prototype || !prototype->vtbl)
				{
					++failed_now;
					if (prototype) game::cbaseentity_remove(prototype);
					continue;
				}
				if (patch_server_accept_input_vtable(prototype->vtbl)) ++installed_now;
				else ++failed_now;
				game::cbaseentity_remove(prototype);
			}
			g_installing_server_accept_input_hooks = false;

			for (const auto& classname : classes)
			{
				if (!g_server_accept_input_attempted_classes.contains(classname)) ++remaining;
			}

			dynamic_lighting::m_server_accept_input_vtables =
				static_cast<std::uint32_t>(g_server_accept_input_originals.size());
			dynamic_lighting::m_server_accept_input_hook_failures += failed_now;
			dynamic_lighting::m_server_accept_input_hooks_installed =
				!g_server_accept_input_originals.empty();
			dynamic_lighting::m_server_accept_input_hook_scan_complete = remaining == 0u;
			dynamic_lighting::m_server_accept_input_hook_status = std::format(
				"AcceptInput capture: {} vtables ({} new, {} failures, {} classes remaining{})",
				g_server_accept_input_originals.size(), installed_now, failed_now, remaining,
				dynamic_lighting::m_server_accept_input_hook_scan_complete ? ", scan complete" : "");
			return dynamic_lighting::m_server_accept_input_hooks_installed;
		}


		struct dyn_light_preset_s
		{
			const char* name;
			Vector radiance;
			float scalar;
			float radius;
			float volumetric;
			bool shaped;
			float degrees;
			float softness;
			float exponent;
		};

		static const dyn_light_preset_s DYN_LIGHT_PRESETS[] =
		{
			// Radii are intentionally small Remix/helper-light units. Older iterations used
			// Source-style radii like 100-600, which made every dynamic light flood huge areas.
			{ "soft_flash",       { 1.00f, 0.88f, 0.62f },  85000.0f,  2.25f, 1.00f, false, 180.0f, 0.00f, 0.00f },
			{ "alarm_red",        { 1.00f, 0.035f, 0.015f }, 90000.0f,  3.00f, 1.25f, false, 180.0f, 0.00f, 0.00f },
			{ "fire",             { 1.00f, 0.42f, 0.10f },   65000.0f,  3.60f, 1.45f, false, 180.0f, 0.00f, 0.00f },
			{ "explosion",        { 1.00f, 0.72f, 0.34f },  380000.0f, 6.80f, 1.75f, false, 180.0f, 0.00f, 0.00f },
			{ "lightning",        { 0.66f, 0.76f, 1.00f },  520000.0f, 10.0f, 0.80f, false, 180.0f, 0.00f, 0.00f },
			{ "tv_glow",          { 0.42f, 0.62f, 1.00f },   45000.0f,  2.50f, 0.85f, false, 180.0f, 0.00f, 0.00f },
			{ "safehouse_warm",   { 1.00f, 0.72f, 0.38f },   52000.0f,  4.00f, 1.05f, false, 180.0f, 0.00f, 0.00f },
			{ "muzzle_flash",     { 1.00f, 0.68f, 0.30f },  180000.0f,  1.00f, 0.35f, false, 180.0f, 0.00f, 0.00f },
			{ "spot_flash",       { 1.00f, 0.88f, 0.62f }, 140000.0f,  3.00f, 0.75f, true,   42.0f, 0.12f, 0.65f },
		};

		struct muzzle_weapon_profile_s
		{
			const char* name;
			const char* match_a;
			const char* match_b;
			float scalar;
			float radius;
			float duration;
			float cooldown;
			Vector offset; // F/H/V
		};

		static const muzzle_weapon_profile_s MUZZLE_WEAPON_PROFILES[] =
		{
			{ "Auto",        "",        "",        1.00f,  74.0f, 0.075f, 0.018f, Vector(18.0f, -2.0f, -3.0f) },
			{ "Pistol",      "pistol",  "magnum",  0.78f,  58.0f, 0.052f, 0.065f, Vector(15.0f, -2.5f, -2.5f) },
			{ "SMG",         "smg",     "uzi",     0.70f,  54.0f, 0.040f, 0.020f, Vector(17.0f, -2.3f, -2.8f) },
			{ "Rifle",       "rifle",   "ak47",    1.00f,  74.0f, 0.055f, 0.035f, Vector(20.0f, -2.0f, -3.2f) },
			{ "Shotgun",     "shotgun", "autoshot",1.45f, 112.0f, 0.075f, 0.180f, Vector(24.0f, -3.5f, -4.0f) },
			{ "Sniper",      "sniper",  "hunting", 1.35f,  96.0f, 0.068f, 0.170f, Vector(25.0f, -2.0f, -3.2f) },
			{ "Mounted Gun", "mounted", "50cal",   1.90f, 142.0f, 0.080f, 0.030f, Vector(38.0f, -4.0f, -7.0f) },
		};


		enum sound_hash_category_e : std::uint8_t
		{
			SOUND_HASH_CAT_FIRE = 0,
			SOUND_HASH_CAT_EXPLOSION,
			SOUND_HASH_CAT_ALARM,
			SOUND_HASH_CAT_WEATHER,
			SOUND_HASH_CAT_ENERGY,
		};

		struct sound_hash_trigger_s
		{
			std::uint32_t hash;
			sound_hash_category_e category;
			const char* preset;
			const char* animation;
			float duration;
			float cooldown;
			float scalar;
			float radius;
			bool use_camera_when_zero_origin;
			bool require_valid_origin;
			const char* source_name;
		};

		static const sound_hash_trigger_s SOUND_HASH_TRIGGERS[] =
		{
			{ 0x36a5b5c3u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_idle_loop_1.wav" },
			{ 0x6c0765dau, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_idle_loop_1.wav" },
			{ 0x24a0b107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0x54573107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0x61853107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0x65e0b107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0x6e4c0107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0x88699107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0x9bf72107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0xa2703107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0xb8482107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0xc0dc2107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0xd1b10107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0xe4b68107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0xe82d7107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0xe92e3107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0xe9bdb107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0xf0f52107u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_1.wav" },
			{ 0x954b7951u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_2.wav" },
			{ 0x0338ea5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x0993ba5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x1afbaa5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x223bca5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x25755a5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x475c4a5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x5c507a5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x65075a5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x6df34a5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x746aea5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x7d26ca5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x8466ba5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x8cbbea5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x975eba5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x99d71a5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x9c5eba5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0xa02f4a5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0xa56f9a5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0xb8b35a5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0xbfe65a5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0xc8052a5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0xd8a9ca5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0xe2a38a5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0xfed15a5bu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_4.wav" },
			{ 0x0f8f9b2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0x10735b2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0x2016cb2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0x2c95eb2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0x3dc11b2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0x43b02b2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0x46605b2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0x51c7cb2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0x53444b2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0x55c1bb2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0x65fa0b2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0x8d728b2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0xa3d30b2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0xa66f3b2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0xab73ab2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0xac7dfb2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0xc1e35b2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0xca4bcb2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0xce8cdb2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0xd65dcb2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0xdfdb3b2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0xf1187b2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0xf646eb2cu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_ignite_5.wav" },
			{ 0x12f0e2b3u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_loop_1.wav" },
			{ 0x88a502b3u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_loop_1.wav" },
			{ 0xb088c2b3u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_loop_1.wav" },
			{ 0xffcc82b3u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_loop_1.wav" },
			{ 0xcfb7b3b1u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_loop_fadeout_01.wav" },
			{ 0xfbb893b1u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/fire_loop_fadeout_01.wav" },
			{ 0x37962b2eu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/molotov_detonate_3.wav" },
			{ 0xc75ecb2eu, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/molotov_detonate_3.wav" },
			{ 0x4d54ab74u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/molotov_detonate_swt_01.wav" },
			{ 0xa1f54b74u, SOUND_HASH_CAT_FIRE, "fire", "fire_pulse", 1.250f, 0.350f, 0.850f, 180.0f, false, true, "weapons/molotov/molotov_detonate_swt_01.wav" },
			{ 0x47d134c1u, SOUND_HASH_CAT_EXPLOSION, "explosion", "lightning_flash", 0.180f, 0.450f, 1.000f, 360.0f, true, false, "ambient/random_amb_sfx/dist_explosion_02.wav" },
			{ 0x9c415609u, SOUND_HASH_CAT_EXPLOSION, "explosion", "lightning_flash", 0.180f, 0.450f, 1.000f, 360.0f, true, false, "ambient/random_amb_sfx/dist_explosion_02.wav" },
			{ 0xb4fa4f66u, SOUND_HASH_CAT_EXPLOSION, "explosion", "lightning_flash", 0.180f, 0.450f, 1.000f, 360.0f, true, false, "ambient/random_amb_sfx/dist_explosion_02.wav" },
			{ 0x7cb37bf1u, SOUND_HASH_CAT_EXPLOSION, "explosion", "lightning_flash", 0.180f, 0.450f, 1.000f, 360.0f, true, false, "ambient/random_amb_sfx/dist_explosion_03.wav" },
			{ 0x834f3576u, SOUND_HASH_CAT_EXPLOSION, "explosion", "lightning_flash", 0.180f, 0.450f, 1.000f, 360.0f, true, false, "ambient/random_amb_sfx/dist_explosion_03.wav" },
			{ 0xe85c23d3u, SOUND_HASH_CAT_EXPLOSION, "explosion", "lightning_flash", 0.180f, 0.450f, 1.000f, 360.0f, true, false, "ambient/random_amb_sfx/dist_explosion_03.wav" },
			{ 0x01f29f55u, SOUND_HASH_CAT_EXPLOSION, "explosion", "lightning_flash", 0.180f, 0.450f, 1.000f, 360.0f, true, false, "ambient/random_amb_sfx/dist_explosion_04.wav" },
			{ 0x4bfe13d8u, SOUND_HASH_CAT_EXPLOSION, "explosion", "lightning_flash", 0.180f, 0.450f, 1.000f, 360.0f, true, false, "ambient/random_amb_sfx/dist_explosion_04.wav" },
			{ 0x860c8d9cu, SOUND_HASH_CAT_EXPLOSION, "explosion", "lightning_flash", 0.180f, 0.450f, 1.000f, 360.0f, true, false, "ambient/random_amb_sfx/dist_explosion_04.wav" },
			{ 0x0d65ac21u, SOUND_HASH_CAT_EXPLOSION, "explosion", "lightning_flash", 0.180f, 0.450f, 1.000f, 360.0f, true, false, "weapons/hegrenade/explode3.wav" },
			{ 0x93bf6c21u, SOUND_HASH_CAT_EXPLOSION, "explosion", "lightning_flash", 0.180f, 0.450f, 1.000f, 360.0f, true, false, "weapons/hegrenade/explode3.wav" },
			{ 0x45e774a6u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.080f, 0.650f, 0.100f, 70.0f, true, false, "ambient/spacial_loops/blinkingalarmclock.wav" },
			{ 0x4da73f6du, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.080f, 0.650f, 0.100f, 70.0f, true, false, "ambient/spacial_loops/blinkingalarmclock.wav" },
			{ 0x139cd6e7u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0x14df9c74u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0x2613618cu, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0x28195cb3u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0x41559700u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0x49f3e7c1u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0x4dcb121cu, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0x6ab8244cu, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0x82a6f700u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0x8f2b6173u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0x96b82700u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0xb0013ea0u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0xb43a2a20u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0xc0139f81u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0xcdb6876eu, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0xd2d36d51u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0xd405e9d5u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0xe84107f9u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0xeac4c48cu, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0xf57c17b6u, SOUND_HASH_CAT_ALARM, "alarm_red", "alarm_pulse", 0.120f, 0.160f, 0.180f, 92.0f, false, true, "weapons/hegrenade/beep.wav" },
			{ 0x10650e87u, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/lightning_strike_1.wav" },
			{ 0x8f958687u, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/lightning_strike_1.wav" },
			{ 0x67bc9f29u, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/lightning_strike_2.wav" },
			{ 0x7b5c0729u, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/lightning_strike_2.wav" },
			{ 0xef8031feu, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/lightning_strike_4.wav" },
			{ 0xefa7b9feu, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/lightning_strike_4.wav" },
			{ 0x4b3f41c8u, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/thunder_1.wav" },
			{ 0xa54ec1c8u, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/thunder_1.wav" },
			{ 0x44475d0au, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/thunder_2.wav" },
			{ 0xbb3c5d0au, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/thunder_2.wav" },
			{ 0x2a1b8e67u, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/thunder_3.wav" },
			{ 0x3b8d8e67u, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/thunder_3.wav" },
			{ 0x66ae3fe8u, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/thunder_far_away_1.wav" },
			{ 0xaec837e8u, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/thunder_far_away_1.wav" },
			{ 0x023ded0cu, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/thunder_far_away_2.wav" },
			{ 0x2927650cu, SOUND_HASH_CAT_WEATHER, "lightning", "lightning_flash", 0.380f, 1.250f, 0.750f, 720.0f, true, false, "ambient/weather/thunderstorm/thunder_far_away_2.wav" },
			{ 0x4946cdceu, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap1.wav" },
			{ 0x5a80effcu, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap1.wav" },
			{ 0xb924c430u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap1.wav" },
			{ 0xd598a40bu, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap1.wav" },
			{ 0xd7e498dbu, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap1.wav" },
			{ 0xfc6d1f41u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap1.wav" },
			{ 0x1db99678u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap2.wav" },
			{ 0x27fbfb34u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap2.wav" },
			{ 0x29a08d81u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap2.wav" },
			{ 0x5d666074u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap2.wav" },
			{ 0x92c2dcfdu, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap2.wav" },
			{ 0xc046f4dcu, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap2.wav" },
			{ 0xf91c9bbau, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap2.wav" },
			{ 0x01171403u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap3.wav" },
			{ 0x23f43ca6u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap3.wav" },
			{ 0x35341352u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap3.wav" },
			{ 0x6f590b6au, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap3.wav" },
			{ 0x84d18b31u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap3.wav" },
			{ 0xc2a59ddbu, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap3.wav" },
			{ 0xce3a9ccfu, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap3.wav" },
			{ 0xd8ca150du, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap3.wav" },
			{ 0x5ef6d8c0u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap7.wav" },
			{ 0x60c77dd4u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap7.wav" },
			{ 0x78341408u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap7.wav" },
			{ 0x7b266e3au, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap7.wav" },
			{ 0x98e69837u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap7.wav" },
			{ 0xeb6b5400u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap7.wav" },
			{ 0xee02069cu, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap7.wav" },
			{ 0x173eb192u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap8.wav" },
			{ 0x29335077u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap8.wav" },
			{ 0x47b1cae2u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap8.wav" },
			{ 0x551f3c4cu, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap8.wav" },
			{ 0x66fe5f71u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap8.wav" },
			{ 0x83ed2887u, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap8.wav" },
			{ 0x91063faeu, SOUND_HASH_CAT_ENERGY, "tv_glow", "tv_noise", 0.160f, 0.080f, 0.420f, 140.0f, false, true, "ambient/energy/zap8.wav" },
		};

		const char* sound_hash_category_name(const sound_hash_category_e category)
		{
			switch (category)
			{
			case SOUND_HASH_CAT_FIRE: return "fire/molotov";
			case SOUND_HASH_CAT_EXPLOSION: return "explosion";
			case SOUND_HASH_CAT_ALARM: return "alarm/beep";
			case SOUND_HASH_CAT_WEATHER: return "weather/lightning";
			case SOUND_HASH_CAT_ENERGY: return "energy/zap";
			}

			return "unknown";
		}

		bool sound_hash_category_enabled(const sound_hash_category_e category)
		{
			switch (category)
			{
			case SOUND_HASH_CAT_FIRE: return dynamic_lighting::m_sound_hash_fire_enabled;
			case SOUND_HASH_CAT_EXPLOSION: return dynamic_lighting::m_sound_hash_explosion_enabled;
			case SOUND_HASH_CAT_ALARM: return dynamic_lighting::m_sound_hash_alarm_enabled;
			case SOUND_HASH_CAT_WEATHER: return dynamic_lighting::m_sound_hash_weather_enabled;
			case SOUND_HASH_CAT_ENERGY: return dynamic_lighting::m_sound_hash_energy_enabled;
			}

			return false;
		}

		bool sound_hash_requires_world_origin(const sound_hash_category_e category)
		{
			// These categories are only useful when the sound origin is a world object/effect.
			// L4D2 often emits weapon/effect sounds from the local survivor entity, which makes
			// helper lights spawn on the player. Weather is intentionally excluded because
			// camera/worldwide flashes are useful for cinematic lightning.
			switch (category)
			{
			case SOUND_HASH_CAT_FIRE:
			case SOUND_HASH_CAT_EXPLOSION:
			case SOUND_HASH_CAT_ALARM:
			case SOUND_HASH_CAT_ENERGY:
				return true;
			case SOUND_HASH_CAT_WEATHER:
			default:
				return false;
			}
		}

		float safe_sound_hash_radius(const float radius)
		{
			const float min_radius = std::max(0.0f, dynamic_lighting::m_sound_hash_min_radius);
			const float max_radius = std::max(min_radius, dynamic_lighting::m_sound_hash_max_radius);
			const float scaled = radius * std::max(0.0f, dynamic_lighting::m_sound_hash_radius_scale);
			return std::clamp(scaled, min_radius, max_radius);
		}

		bool sound_origin_is_near_player_view(const Vector& origin)
		{
			const auto* view_origin = game::get_current_view_origin();
			if (!view_origin) {
				return false;
			}

			const Vector delta = origin - *view_origin;
			const float limit = std::max(0.0f, dynamic_lighting::m_sound_hash_reject_near_player_distance);
			return delta.LengthSqr() <= (limit * limit);
		}

		const muzzle_weapon_profile_s& detect_muzzle_profile_from_sound(const std::string& sound_name)
		{
			const auto s = utils::str_to_lower(sound_name);
			for (std::size_t i = 1u; i < (sizeof(MUZZLE_WEAPON_PROFILES) / sizeof(MUZZLE_WEAPON_PROFILES[0])); ++i)
			{
				const auto& profile = MUZZLE_WEAPON_PROFILES[i];
				if ((profile.match_a && *profile.match_a && s.find(profile.match_a) != std::string::npos) ||
					(profile.match_b && *profile.match_b && s.find(profile.match_b) != std::string::npos))
				{
					return profile;
				}
			}

			return MUZZLE_WEAPON_PROFILES[0];
		}

		const muzzle_weapon_profile_s& get_selected_muzzle_profile(const std::string& sound_name)
		{
			if (dynamic_lighting::m_muzzle_weapon_profile_mode > 0 &&
				dynamic_lighting::m_muzzle_weapon_profile_mode < static_cast<int>((sizeof(MUZZLE_WEAPON_PROFILES) / sizeof(MUZZLE_WEAPON_PROFILES[0]))))
			{
				return MUZZLE_WEAPON_PROFILES[dynamic_lighting::m_muzzle_weapon_profile_mode];
			}

			return detect_muzzle_profile_from_sound(sound_name);
		}

		const dyn_light_preset_s& find_dyn_light_preset(const std::string& preset_name)
		{
			const auto lower = utils::str_to_lower(preset_name);
			for (const auto& preset : DYN_LIGHT_PRESETS)
			{
				if (lower == preset.name) {
					return preset;
				}
			}

			return DYN_LIGHT_PRESETS[0];
		}

		float now()
		{
			return interfaces::get()->m_globals ? interfaces::get()->m_globals->curtime : 0.0f;
		}

		float frame_time()
		{
			return interfaces::get()->m_globals ? interfaces::get()->m_globals->absoluteframetime : 0.0f;
		}

		float deterministic_variation(const std::string& name, const float variation)
		{
			if (variation <= 0.0f) {
				return 1.0f;
			}

			const auto h = utils::string_hash64(name);
			const float t = static_cast<float>((h % 10000u) / 10000.0);
			return 1.0f + ((t * 2.0f) - 1.0f) * variation;
		}

		map_settings::remix_light_settings_s::point_s make_point(
			const Vector& origin,
			const Vector& radiance,
			const float scalar,
			const float radius,
			const float timepoint,
			const float volumetric,
			const bool shaped,
			const Vector& direction,
			const float degrees,
			const float softness,
			const float exponent)
		{
			return map_settings::remix_light_settings_s::point_s{
				.position = origin,
				.radiance = radiance,
				.radiance_scalar = scalar,
				.radius = radius,
				.timepoint = timepoint,
				.smoothness = 0.0f,
				.use_shaping = shaped,
				.direction = direction,
				.angle_offset_attached = Vector(0.0f, 0.0f, 0.0f),
				.degrees = degrees,
				.softness = softness,
				.exponent = exponent,
				.volumetric_scale = volumetric
			};
		}

		Vector normalized_or(Vector value, const Vector& fallback)
		{
			if (value.LengthSqr() <= 0.0001f) {
				value = fallback;
			}

			if (value.LengthSqr() <= 0.0001f) {
				value = Vector(0.0f, 0.0f, 1.0f);
			}

			value.Normalize();
			return value;
		}

		Vector rotate_vector_axis(Vector value, Vector axis, const float degrees)
		{
			value = normalized_or(value, Vector(0.0f, 1.0f, 0.0f));
			axis = normalized_or(axis, Vector(0.0f, 0.0f, 1.0f));

			const float radians = degrees * static_cast<float>(M_PI / 180.0);
			const float c = std::cos(radians);
			const float s = std::sin(radians);
			const float d = value.Dot(axis);

			Vector rotated = value * c + axis.Cross(value) * s + axis * (d * (1.0f - c));
			return normalized_or(rotated, value);
		}

		bool animation_is_rotator(const std::string& animation)
		{
			return animation == "rotating_yaw" || animation == "rotating_pitch" || animation == "rotating_roll" ||
				animation == "rotating_beacon" || animation == "warning_beacon" || animation == "police_red_blue" ||
				animation == "lighthouse_sweep" || animation == "disc_spin" || animation == "spot_axis_spin";
		}

		bool animation_is_sweep(const std::string& animation)
		{
			return animation == "searchlight_sweep" || animation == "pendulum_sweep" || animation == "spot_axis_sweep";
		}

		Vector axis_for_animation(const std::string& animation, const Vector& custom_axis, const Vector& forward)
		{
			(void)forward;

			if (animation == "rotating_pitch") {
				return Vector(1.0f, 0.0f, 0.0f);
			}
			if (animation == "rotating_roll") {
				return Vector(0.0f, 1.0f, 0.0f);
			}

			return normalized_or(custom_axis, Vector(0.0f, 0.0f, 1.0f));
		}

		void push_anim_point(
			std::vector<map_settings::remix_light_settings_s::point_s>& points,
			const Vector& origin,
			const Vector& radiance,
			const float scalar,
			const float radius,
			const float timepoint,
			const float volumetric,
			const bool shaped,
			const Vector& direction,
			const float degrees,
			const float softness,
			const float exponent)
		{
			points.push_back(make_point(origin, radiance, scalar, radius, timepoint, volumetric, shaped, direction, degrees, softness, exponent));
		}

		using set_light_fn = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, const D3DLIGHT9*);
		set_light_fn set_light_original = nullptr;
		HRESULT __stdcall set_light_hk(IDirect3DDevice9* device, DWORD index, const D3DLIGHT9* light)
		{
			const auto result = set_light_original(device, index, light);
			dynamic_lighting::on_d3d_set_light(index, light);
			return result;
		}

		using light_enable_fn = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, BOOL);
		light_enable_fn light_enable_original = nullptr;
		HRESULT __stdcall light_enable_hk(IDirect3DDevice9* device, DWORD index, BOOL enable)
		{
			const auto result = light_enable_original(device, index, enable);
			dynamic_lighting::on_d3d_light_enable(index, enable);
			return result;
		}

		bool install_d3d_light_hooks()
		{
			if (dynamic_lighting::m_d3d_hooks_installed) {
				return true;
			}

			++dynamic_lighting::m_d3d_hook_attempts;
			const auto dev = game::get_d3d_device();
			if (!dev)
			{
				dynamic_lighting::m_d3d_hook_status = "waiting for D3D device";
				return false;
			}

			auto get_virtual = [](void* _class, unsigned int index) -> void* {
				return reinterpret_cast<void*>((*reinterpret_cast<std::uintptr_t**>(_class))[index]);
			};

			void* set_light_target = get_virtual(dev, 51);
			void* light_enable_target = get_virtual(dev, 53);

			const auto set_light_status = MH_CreateHook(set_light_target, set_light_hk, reinterpret_cast<void**>(&set_light_original));
			const auto light_enable_status = MH_CreateHook(light_enable_target, light_enable_hk, reinterpret_cast<void**>(&light_enable_original));

			const bool set_light_ok = set_light_status == MH_OK || set_light_status == MH_ERROR_ALREADY_CREATED;
			const bool light_enable_ok = light_enable_status == MH_OK || light_enable_status == MH_ERROR_ALREADY_CREATED;
			if (!set_light_ok || !light_enable_ok)
			{
				++dynamic_lighting::m_d3d_hook_failures;
				dynamic_lighting::m_d3d_hook_status = std::format("hook create failed: SetLight={} LightEnable={}", static_cast<int>(set_light_status), static_cast<int>(light_enable_status));
				return false;
			}

			const auto enable_set_light_status = MH_EnableHook(set_light_target);
			const auto enable_light_enable_status = MH_EnableHook(light_enable_target);
			const bool enable_set_light_ok = enable_set_light_status == MH_OK || enable_set_light_status == MH_ERROR_ENABLED;
			const bool enable_light_enable_ok = enable_light_enable_status == MH_OK || enable_light_enable_status == MH_ERROR_ENABLED;
			if (!enable_set_light_ok || !enable_light_enable_ok)
			{
				++dynamic_lighting::m_d3d_hook_failures;
				dynamic_lighting::m_d3d_hook_status = std::format("hook enable failed: SetLight={} LightEnable={}", static_cast<int>(enable_set_light_status), static_cast<int>(enable_light_enable_status));
				return false;
			}

			dynamic_lighting::m_d3d_hooks_installed = true;
			dynamic_lighting::m_d3d_hook_status = "installed; waiting for calls";
			return true;
		}

		struct recv_prop_location_s
		{
			std::ptrdiff_t offset = -1;
			sdk::send_prop_type type = sdk::send_prop_type::_int;
		};

		bool find_recv_prop_recursive_single(
			const sdk::recv_table* table,
			const char* name,
			recv_prop_location_s& out,
			const std::ptrdiff_t parent_offset = 0,
			const int depth = 0)
		{
			if (!table || !table->props || table->props_count <= 0 || !name || depth > 12) {
				return false;
			}

			for (int i = 0; i < table->props_count; ++i)
			{
				const auto& prop = table->props[i];
				if (prop.prop_name && std::strcmp(prop.prop_name, name) == 0)
				{
					out.offset = parent_offset + prop.offset;
					out.type = prop.prop_type;
					return true;
				}

				if (prop.data_table && prop.data_table != table &&
					find_recv_prop_recursive_single(prop.data_table, name, out, parent_offset + prop.offset, depth + 1)) {
					return true;
				}
			}

			return false;
		}

		bool find_recv_prop_recursive(
			const sdk::recv_table* table,
			const std::initializer_list<const char*>& names,
			recv_prop_location_s& out)
		{
			// Respect the caller's preference order. The previous implementation searched
			// the RecvTable first, so an unrelated fallback name could win over the
			// class-specific property requested first.
			for (const char* name : names)
			{
				recv_prop_location_s candidate = {};
				if (find_recv_prop_recursive_single(table, name, candidate))
				{
					out = candidate;
					return true;
				}
			}
			return false;
		}

		template <typename T>
		bool read_entity_prop(const sdk::c_base_entity* entity, const sdk::c_client_class* cls,
			const std::initializer_list<const char*>& names, T& out)
		{
			if (!entity || !cls || !cls->recvtable_ptr) {
				return false;
			}

			recv_prop_location_s location = {};
			if (!find_recv_prop_recursive(cls->recvtable_ptr, names, location) || location.offset < 0) {
				return false;
			}

			const auto* bytes = reinterpret_cast<const std::uint8_t*>(entity);
			std::memcpy(&out, bytes + location.offset, sizeof(T));
			return true;
		}

		bool read_entity_bool_prop(const sdk::c_base_entity* entity, const sdk::c_client_class* cls,
			const std::initializer_list<const char*>& names, bool& out)
		{
			if (!entity || !cls || !cls->recvtable_ptr) {
				return false;
			}

			recv_prop_location_s location = {};
			if (!find_recv_prop_recursive(cls->recvtable_ptr, names, location) || location.offset < 0) {
				return false;
			}

			const auto* bytes = reinterpret_cast<const std::uint8_t*>(entity);
			if (location.type == sdk::send_prop_type::_int)
			{
				const std::uint8_t byte_value = *(bytes + location.offset);
				out = byte_value != 0u;
				return true;
			}

			return false;
		}

		bool read_entity_float_prop(const sdk::c_base_entity* entity, const sdk::c_client_class* cls,
			const std::initializer_list<const char*>& names, float& out)
		{
			if (!entity || !cls || !cls->recvtable_ptr) return false;
			recv_prop_location_s location = {};
			if (!find_recv_prop_recursive(cls->recvtable_ptr, names, location) || location.offset < 0 ||
				location.type != sdk::send_prop_type::_float) {
				return false;
			}
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(entity);
			std::memcpy(&out, bytes + location.offset, sizeof(out));
			return std::isfinite(out);
		}

		bool finite_source_vector(const Vector& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool normalize_source_direction(Vector& value)
		{
			if (!finite_source_vector(value)) return false;
			const float length_sqr = value.LengthSqr();
			if (!std::isfinite(length_sqr) || length_sqr <= 0.0001f || length_sqr > 1000000.0f) return false;
			value.Normalize();
			return finite_source_vector(value);
		}

		std::string runtime_entity_class_name(const sdk::c_client_class* cls)
		{
			return cls && cls->network_name ? utils::str_to_lower(cls->network_name) : std::string{};
		}

		bool runtime_entity_is_rendered(sdk::c_base_entity* entity, const sdk::c_client_class* cls)
		{
			if (!entity || !cls || entity->is_dormant()) {
				return false;
			}

			constexpr int EF_NODRAW = 0x20;
			if ((entity->effect_flags() & EF_NODRAW) != 0) {
				return false;
			}

			bool disabled = false;
			if (read_entity_bool_prop(entity, cls, { "m_bDisabled", "m_iDisabled" }, disabled) && disabled) {
				return false;
			}

			const auto class_name = runtime_entity_class_name(cls);
			if (class_name.find("breakable") != std::string::npos ||
				class_name.find("physicsprop") != std::string::npos ||
				class_name.find("prop_physics") != std::string::npos)
			{
				int health = 1;
				if (read_entity_prop(entity, cls, { "m_iHealth" }, health) && health <= 0) {
					return false;
				}
			}

			return true;
		}

		bool runtime_light_entity_is_enabled(sdk::c_base_entity* entity, const sdk::c_client_class* cls)
		{
			if (!runtime_entity_is_rendered(entity, cls)) {
				return false;
			}

			bool value = false;
			if (read_entity_bool_prop(entity, cls, { "m_bState", "m_bEnabled", "m_bOn" }, value) && !value) {
				return false;
			}
			return true;
		}

		bool owner_class_is_terminal_when_removed(const std::string& class_name)
		{
			const auto lowered = utils::str_to_lower(class_name);
			return lowered.find("prop_physics") != std::string::npos ||
				lowered.find("physicsprop") != std::string::npos ||
				lowered.find("prop_dynamic") != std::string::npos ||
				lowered.find("breakable") != std::string::npos;
		}

		float smoothing_alpha(const float dt, const float response_seconds)
		{
			if (response_seconds <= 0.0001f) return 1.0f;
			return std::clamp(1.0f - std::exp(-std::max(0.0f, dt) / response_seconds), 0.0f, 1.0f);
		}

		Vector smooth_direction(Vector current, Vector target, const float alpha)
		{
			if (current.LengthSqr() <= 0.0001f) current = target;
			if (target.LengthSqr() <= 0.0001f) target = current;
			current.Normalize();
			target.Normalize();
			Vector result = current + (target - current) * std::clamp(alpha, 0.0f, 1.0f);
			if (result.LengthSqr() <= 0.0001f) result = target;
			result.Normalize();
			return result;
		}


		float source_direction_angle_degrees(Vector lhs, Vector rhs)
		{
			if (!normalize_source_direction(lhs) || !normalize_source_direction(rhs)) return 180.0f;
			constexpr float radians_to_degrees = 57.295779513082320876f;
			return std::acos(std::clamp(lhs.Dot(rhs), -1.0f, 1.0f)) * radians_to_degrees;
		}

		struct compiled_axis_prior_s
		{
			Vector direction = Vector(1.0f, 0.0f, 0.0f);
			int source_index = -1;
			float distance = 0.0f;
			bool valid = false;
		};

		compiled_axis_prior_s find_compiled_axis_prior(const Vector& origin, const bool shaped)
		{
			compiled_axis_prior_s best = {};
			if (!dynamic_lighting::m_source_light_compiled_axis_prior || !shaped) return best;
			const float max_distance = std::max(0.0f, dynamic_lighting::m_source_light_compiled_prior_distance);
			const float max_distance_sqr = max_distance * max_distance;
			float best_distance_sqr = std::numeric_limits<float>::max();
			for (const auto& candidate : dynamic_lighting::m_source_bsp_candidates)
			{
				if (!candidate.from_worldlight || !candidate.selected || candidate.override_disabled || !candidate.shaped) continue;
				if (candidate.source_type != static_cast<std::int32_t>(source_bsp_lights::emit_type::spotlight)) continue;
				Vector candidate_direction = candidate.direction;
				if (!normalize_source_direction(candidate_direction)) continue;
				const float distance_sqr = candidate.origin.DistToSqr(origin);
				if (!std::isfinite(distance_sqr) || distance_sqr > max_distance_sqr || distance_sqr >= best_distance_sqr) continue;
				best_distance_sqr = distance_sqr;
				best.direction = candidate_direction;
				best.source_index = static_cast<int>(candidate.source_index);
				best.distance = std::sqrt(std::max(0.0f, distance_sqr));
				best.valid = true;
			}
			return best;
		}

		struct source_axis_resolution_s
		{
			Vector direction = Vector(1.0f, 0.0f, 0.0f);
			std::string source = "fallback +X";
			std::string candidates;
			float confidence = 0.05f;
			float disagreement_degrees = 0.0f;
			bool corrected = false;
			bool basis_recovered = false;
			bool switched = false;
		};

		struct source_axis_candidate_s
		{
			Vector direction = Vector(1.0f, 0.0f, 0.0f);
			std::string name;
			float score = 0.0f;
			bool basis_axis = false;
			bool temporal = false;
			bool compiled = false;
			bool target = false;
		};

		source_axis_resolution_s resolve_source_light_axis(
			const bool projected, const bool point_spotlight, const bool dynamic_light, const bool camera_space,
			const Vector& angle_forward, const bool angle_forward_valid,
			const Vector& angle_right, const bool angle_right_valid,
			const Vector& angle_up, const bool angle_up_valid,
			const Vector& target_direction, const bool target_valid,
			const compiled_axis_prior_s& compiled_prior,
			const Vector& previous_direction, const bool previous_valid,
			const std::string& previous_source)
		{
			source_axis_resolution_s result = {};
			std::vector<source_axis_candidate_s> candidates;
			candidates.reserve(10u);
			auto add_candidate = [&](Vector direction, std::string name, const float base_score,
				const bool basis_axis = false, const bool temporal = false,
				const bool compiled = false, const bool target = false)
			{
				if (!normalize_source_direction(direction)) return;
				candidates.push_back({ direction, std::move(name), base_score, basis_axis, temporal, compiled, target });
			};

			if (angle_forward_valid)
			{
				// Live engine dlights expose m_Direction directly, so their forward vector is
				// stronger evidence than a generic entity angle but remains below an authored
				// point_spotlight or camera-space projected texture. Keeping this distinction
				// also makes the dynamic_light classifier input meaningful under /WX.
				const float base = camera_space ? 0.96f
					: (point_spotlight ? 1.00f : (dynamic_light ? 0.90f : (projected ? 0.72f : 0.80f)));
				add_candidate(angle_forward, dynamic_light ? "dynamic light direction" : "angles forward", base);
			}
			if (target_valid && !camera_space)
			{
				// env_projectedtexture owns a real target axis. point_spotlight endpoints are
				// validation/fallback data only: a stale endpoint must not beat valid Source
				// angles and recreate the historical ~90-degree beam rotation.
				const float base = projected ? 0.98f : (point_spotlight ? (angle_forward_valid ? 0.48f : 0.86f) : 0.72f);
				add_candidate(target_direction, point_spotlight ? "spotlight endpoint" : "target entity", base, false, false, false, true);
			}
			if (compiled_prior.valid)
			{
				add_candidate(compiled_prior.direction,
					std::format("WORLDLIGHT {} prior", compiled_prior.source_index), 0.94f, false, false, true, false);
			}
			if (dynamic_lighting::m_source_light_temporal_axis_prior && previous_valid)
			{
				add_candidate(previous_direction, "temporal stable axis", 0.55f, false, true, false, false);
			}

			// Some Source entities expose an orientation whose intended emission axis is a local side/up axis.
			// These candidates start with a deliberately low score and can only win when a target, compiled
			// WORLDLIGHT or stable temporal direction strongly validates the 90-degree basis correction.
			if (dynamic_lighting::m_source_light_basis_recovery && !camera_space && (point_spotlight || projected))
			{
				if (angle_right_valid)
				{
					add_candidate(angle_right, "angles local right", 0.08f, true);
					add_candidate(angle_right * -1.0f, "angles local left", 0.08f, true);
				}
				if (angle_up_valid)
				{
					add_candidate(angle_up, "angles local up", 0.08f, true);
					add_candidate(angle_up * -1.0f, "angles local down", 0.08f, true);
				}
			}

			if (candidates.empty()) return result;
			Vector normalized_target = target_direction;
			const bool have_target = target_valid && normalize_source_direction(normalized_target);
			Vector normalized_previous = previous_direction;
			const bool have_previous = previous_valid && normalize_source_direction(normalized_previous);

			for (auto& candidate : candidates)
			{
				float support = 0.0f;
				if (have_previous && !candidate.temporal)
				{
					const float dot = std::clamp(candidate.direction.Dot(normalized_previous), -1.0f, 1.0f);
					support += 0.20f * std::max(0.0f, dot);
					if (dot < 0.15f) support -= 0.10f;
				}
				if (compiled_prior.valid && !candidate.compiled)
				{
					const float dot = std::clamp(candidate.direction.Dot(compiled_prior.direction), -1.0f, 1.0f);
					support += 0.26f * std::max(0.0f, dot);
					if (dot < 0.10f) support -= 0.18f;
				}
				if (have_target && !candidate.target && !camera_space)
				{
					const float dot = std::clamp(candidate.direction.Dot(normalized_target), -1.0f, 1.0f);
					support += (point_spotlight ? 0.20f : 0.12f) * std::max(0.0f, dot);
					if (candidate.basis_axis && dot > 0.96f) support += 0.70f;
				}
				if (candidate.basis_axis)
				{
					float best_validator_dot = -1.0f;
					if (have_target) best_validator_dot = std::max(best_validator_dot, candidate.direction.Dot(normalized_target));
					if (compiled_prior.valid) best_validator_dot = std::max(best_validator_dot, candidate.direction.Dot(compiled_prior.direction));
					if (have_previous) best_validator_dot = std::max(best_validator_dot, candidate.direction.Dot(normalized_previous));
					if (best_validator_dot < 0.93f) support -= 0.30f;
				}
				candidate.score = std::clamp(candidate.score + support, -1.0f, 1.5f);
			}

			auto best_it = std::max_element(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs)
			{
				return lhs.score < rhs.score;
			});
			if (best_it == candidates.end()) return result;

			// Direction hysteresis: a new axis must beat a current, history-aligned candidate by a margin
			// before a large angular jump is accepted. This prevents target handles and network fields from
			// alternating between two perpendicular interpretations from frame to frame.
			if (dynamic_lighting::m_source_light_axis_resolver && have_previous &&
				source_direction_angle_degrees(best_it->direction, normalized_previous) >
				std::max(0.0f, dynamic_lighting::m_source_light_max_axis_jump_degrees))
			{
				auto stable_it = std::max_element(candidates.begin(), candidates.end(), [&](const auto& lhs, const auto& rhs)
				{
					const float lhs_alignment = lhs.direction.Dot(normalized_previous);
					const float rhs_alignment = rhs.direction.Dot(normalized_previous);
					return lhs_alignment < rhs_alignment;
				});
				if (stable_it != candidates.end() && stable_it->direction.Dot(normalized_previous) > 0.85f &&
					best_it->score < stable_it->score + std::max(0.0f, dynamic_lighting::m_source_light_axis_switch_margin))
				{
					best_it = stable_it;
				}
			}

			result.direction = best_it->direction;
			result.source = best_it->name;
			result.confidence = std::clamp((best_it->score + 0.10f) / 1.35f, 0.05f, 1.0f);
			result.basis_recovered = best_it->basis_axis;
			result.corrected = best_it->basis_axis || best_it->compiled ||
				(point_spotlight && best_it->target) || (projected && !camera_space && !best_it->target && target_valid);
			result.switched = have_previous && !previous_source.empty() && best_it->name != previous_source &&
				source_direction_angle_degrees(best_it->direction, normalized_previous) > 2.0f;
			result.disagreement_degrees = angle_forward_valid && target_valid
				? source_direction_angle_degrees(angle_forward, target_direction) : 0.0f;

			std::stable_sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs)
			{
				return lhs.score > rhs.score;
			});
			std::ostringstream summary;
			for (std::size_t i = 0u; i < std::min<std::size_t>(candidates.size(), 5u); ++i)
			{
				if (i) summary << "; ";
				summary << candidates[i].name << '=' << std::fixed << std::setprecision(2) << candidates[i].score;
			}
			result.candidates = summary.str();
			return result;
		}

		bool resolve_source_spot_shape(const float evidence, const bool force_spot, const bool force_sphere,
			std::uint8_t& spot_frames, std::uint8_t& sphere_frames, bool& initialized, bool& stable_shaped)
		{
			if (force_spot)
			{
				spot_frames = 255u;
				sphere_frames = 0u;
				stable_shaped = true;
				initialized = true;
				return true;
			}
			if (force_sphere)
			{
				spot_frames = 0u;
				sphere_frames = 255u;
				stable_shaped = false;
				initialized = true;
				return false;
			}

			const bool spot_vote = evidence >= 0.72f;
			const bool sphere_vote = evidence <= 0.42f;
			spot_frames = spot_vote ? static_cast<std::uint8_t>(std::min(254u, static_cast<unsigned int>(spot_frames) + 1u)) : 0u;
			sphere_frames = sphere_vote ? static_cast<std::uint8_t>(std::min(254u, static_cast<unsigned int>(sphere_frames) + 1u)) : 0u;
			if (!dynamic_lighting::m_source_light_shape_hysteresis)
			{
				stable_shaped = spot_vote;
				initialized = true;
				return stable_shaped;
			}
			if (!initialized)
			{
				stable_shaped = false;
				initialized = true;
			}
			const auto spot_required = static_cast<std::uint8_t>(std::clamp(dynamic_lighting::m_source_light_spot_confirm_frames, 1, 30));
			const auto sphere_required = static_cast<std::uint8_t>(std::clamp(dynamic_lighting::m_source_light_sphere_confirm_frames, 1, 30));
			if (!stable_shaped && spot_frames >= spot_required) stable_shaped = true;
			else if (stable_shaped && sphere_frames >= sphere_required) stable_shaped = false;
			return stable_shaped;
		}

		Vector rotate_source_local_vector(const Vector& local, const Vector& angles)
		{
			Vector forward(1.0f, 0.0f, 0.0f);
			Vector right(0.0f, -1.0f, 0.0f);
			Vector up(0.0f, 0.0f, 1.0f);
			utils::vector::AngleVectors(angles, &forward, &right, &up);
			return (forward * local.x) - (right * local.y) + (up * local.z);
		}

		Vector inverse_rotate_source_vector(const Vector& world, const Vector& angles)
		{
			Vector forward(1.0f, 0.0f, 0.0f);
			Vector right(0.0f, -1.0f, 0.0f);
			Vector up(0.0f, 0.0f, 1.0f);
			utils::vector::AngleVectors(angles, &forward, &right, &up);
			return Vector(world.Dot(forward), -world.Dot(right), world.Dot(up));
		}

		bool get_runtime_entity_basic(const int entity_index, sdk::c_base_entity*& out_entity,
			const sdk::c_client_class*& out_class)
		{
			out_entity = nullptr;
			out_class = nullptr;
			const auto* intf = interfaces::get();
			if (!intf || !intf->m_entity_list || entity_index <= 0 || entity_index >= intf->m_entity_list->get_max_entity()) {
				return false;
			}
			auto* entity = intf->m_entity_list->get_client_entity(entity_index);
			if (!entity) return false;
			const auto* cls = entity->client_class();
			if (!cls) return false;
			out_entity = entity;
			out_class = cls;
			return true;
		}

		std::string normalize_model_name(std::string value)
		{
			value = utils::str_to_lower(value);
			std::replace(value.begin(), value.end(), '\\', '/');
			while (value.starts_with("./")) value.erase(0u, 2u);
			return value;
		}

		std::string runtime_entity_model_name(sdk::c_base_entity* entity)
		{
			if (!entity) return {};
			const auto* model = entity->get_model();
			if (!model || model->szPathName[0] == '\0') return {};
			return normalize_model_name(model->szPathName);
		}


		enum class runtime_light_owner_category_e : std::uint8_t
		{
			world,
			survivor,
			infected,
			unknown_character,
		};

		struct runtime_light_owner_info_s
		{
			runtime_light_owner_category_e category = runtime_light_owner_category_e::world;
			std::string category_name = "world/map";
			std::string classname;
			std::string model;
			int entity_index = -1;
		};

		bool contains_any_token(const std::string& text, const std::initializer_list<const char*>& tokens)
		{
			for (const char* token : tokens) if (text.find(token) != std::string::npos) return true;
			return false;
		}

		runtime_light_owner_info_s classify_runtime_light_owner(const int entity_index)
		{
			runtime_light_owner_info_s info = {};
			info.entity_index = entity_index;
			if (entity_index <= 0) return info;
			const auto* intf = interfaces::get();
			if (!intf || !intf->m_entity_list || entity_index >= intf->m_entity_list->get_max_entity()) return info;
			auto* entity = intf->m_entity_list->get_client_entity(entity_index);
			if (!entity) return info;
			const auto* cls = entity->client_class();
			info.classname = runtime_entity_class_name(cls);
			info.model = runtime_entity_model_name(entity);
			const std::string evidence = info.classname + " " + info.model;
			if (contains_any_token(evidence, { "models/survivors/", "survivor_", "survivor", "teenangst", "manager", "biker", "namvet", "gambler", "producer", "coach", "mechanic" }))
			{
				info.category = runtime_light_owner_category_e::survivor;
				info.category_name = "survivor/player";
			}
			else if (contains_any_token(evidence, { "models/infected/", "infected", "smoker", "boomer", "hunter", "spitter", "jockey", "charger", "witch", "tank", "zombie" }))
			{
				info.category = runtime_light_owner_category_e::infected;
				info.category_name = "infected/special";
			}
			else if (entity->is_player() || contains_any_token(info.classname, { "terrorplayer", "player" }))
			{
				info.category = runtime_light_owner_category_e::unknown_character;
				info.category_name = "unknown character";
			}
			return info;
		}

		runtime_light_owner_info_s classify_runtime_light_owner_with_proximity(const int entity_index, const Vector& origin, const float max_distance)
		{
			auto direct = classify_runtime_light_owner(entity_index);
			if (direct.category != runtime_light_owner_category_e::world || !direct.classname.empty() || max_distance <= 0.0f) return direct;
			const auto* intf = interfaces::get();
			if (!intf || !intf->m_entity_list) return direct;
			const float max_distance_sq = max_distance * max_distance;
			float best_distance_sq = max_distance_sq;
			runtime_light_owner_info_s best = direct;
			const int max_entities = intf->m_entity_list->get_max_entity();
			for (int index = 1; index < max_entities; ++index)
			{
				auto candidate = classify_runtime_light_owner(index);
				if (candidate.category == runtime_light_owner_category_e::world) continue;
				auto* entity = intf->m_entity_list->get_client_entity(index);
				if (!entity) continue;
				const float distance_sq = entity->get_absolute_origin().DistToSqr(origin);
				if (!std::isfinite(distance_sq) || distance_sq > best_distance_sq) continue;
				best_distance_sq = distance_sq;
				best = std::move(candidate);
			}
			if (best.category != runtime_light_owner_category_e::world) best.category_name += " (proximity inferred)";
			return best;
		}

		bool source_runtime_class_allowed(const bool entity_light, const bool is_dynamic, const bool is_projected,
			const bool is_point_spotlight, std::string& reason)
		{
			if (entity_light && !dynamic_lighting::m_source_runtime_allow_elights) { reason = "elight class disabled"; return false; }
			if (!entity_light && !is_dynamic && !is_projected && !is_point_spotlight && !dynamic_lighting::m_source_runtime_allow_dlights)
			{
				reason = "dlight class disabled";
				return false;
			}
			if (is_dynamic && !dynamic_lighting::m_source_runtime_allow_entity_dynamic) { reason = "light_dynamic class disabled"; return false; }
			if (is_projected && !dynamic_lighting::m_source_runtime_allow_projected) { reason = "env_projectedtexture class disabled"; return false; }
			if (is_point_spotlight && !dynamic_lighting::m_source_runtime_allow_point_spotlight) { reason = "point_spotlight class disabled"; return false; }
			return true;
		}

		bool source_runtime_owner_allowed(const runtime_light_owner_info_s& owner, std::string& reason)
		{
			switch (owner.category)
			{
			case runtime_light_owner_category_e::survivor:
				if (!dynamic_lighting::m_source_runtime_allow_survivors) { reason = "survivor/player helper light filtered"; return false; }
				break;
			case runtime_light_owner_category_e::infected:
				if (!dynamic_lighting::m_source_runtime_allow_infected) { reason = "infected/special helper light filtered"; return false; }
				break;
			case runtime_light_owner_category_e::unknown_character:
				if (!dynamic_lighting::m_source_runtime_allow_unknown_characters) { reason = "unknown character helper light filtered"; return false; }
				break;
			case runtime_light_owner_category_e::world:
				if (!dynamic_lighting::m_source_runtime_allow_world) { reason = "world/map runtime lights disabled"; return false; }
				break;
			}
			return true;
		}

		void count_runtime_policy_result(const runtime_light_owner_info_s& owner, const bool allowed)
		{
			if (allowed)
			{
				if (owner.category == runtime_light_owner_category_e::world) ++dynamic_lighting::m_source_runtime_world_accepted;
				else ++dynamic_lighting::m_source_runtime_character_accepted;
				return;
			}
			switch (owner.category)
			{
			case runtime_light_owner_category_e::survivor: ++dynamic_lighting::m_source_runtime_rejected_survivor; break;
			case runtime_light_owner_category_e::infected: ++dynamic_lighting::m_source_runtime_rejected_infected; break;
			case runtime_light_owner_category_e::unknown_character: ++dynamic_lighting::m_source_runtime_rejected_unknown_character; break;
			default: ++dynamic_lighting::m_source_runtime_rejected_class; break;
			}
		}

		float runtime_owner_intensity_scale(const runtime_light_owner_info_s& owner)
		{
			return owner.category == runtime_light_owner_category_e::world
				? std::max(0.0f, dynamic_lighting::m_source_runtime_world_intensity_scale)
				: std::max(0.0f, dynamic_lighting::m_source_runtime_character_intensity_scale);
		}

		float runtime_owner_radius_scale(const runtime_light_owner_info_s& owner)
		{
			return owner.category == runtime_light_owner_category_e::world
				? std::max(0.0f, dynamic_lighting::m_source_runtime_world_radius_scale)
				: std::max(0.0f, dynamic_lighting::m_source_runtime_character_radius_scale);
		}

		float runtime_owner_max_radius(const runtime_light_owner_info_s& owner)
		{
			return owner.category == runtime_light_owner_category_e::world
				? std::max(0.10f, dynamic_lighting::m_source_runtime_max_radius)
				: std::max(0.10f, dynamic_lighting::m_source_runtime_character_max_radius);
		}

		bool runtime_model_matches(const std::string& expected_model, const std::string& runtime_model)
		{
			if (expected_model.empty()) return true;
			const auto expected = normalize_model_name(expected_model);
			const auto runtime = normalize_model_name(runtime_model);
			if (runtime.empty()) return false;
			if (expected == runtime) return true;
			if (!expected.empty() && expected.front() == '*') return expected == runtime;
			const auto expected_slash = expected.find_last_of('/');
			const auto runtime_slash = runtime.find_last_of('/');
			const auto expected_base = expected.substr(expected_slash == std::string::npos ? 0u : expected_slash + 1u);
			const auto runtime_base = runtime.substr(runtime_slash == std::string::npos ? 0u : runtime_slash + 1u);
			return !expected_base.empty() && expected_base == runtime_base;
		}

		bool find_runtime_entity_for_map_owner(const std::string& map_classname, const std::string& map_model,
			const Vector& map_origin, const float max_distance, const std::unordered_set<int>* claimed_indices,
			int& out_index, sdk::c_base_entity*& out_entity, const sdk::c_client_class*& out_class)
		{
			out_index = -1;
			out_entity = nullptr;
			out_class = nullptr;
			const auto* intf = interfaces::get();
			if (!intf || !intf->m_entity_list || map_classname.empty()) return false;

			const float radius = std::max(8.0f, max_distance);
			const float radius_sq = radius * radius;
			float best_score = -FLT_MAX;
			const int max_entities = intf->m_entity_list->get_max_entity();
			for (int index = 1; index < max_entities; ++index)
			{
				if (dynamic_lighting::m_map_light_unique_owner_claims && claimed_indices && claimed_indices->contains(index))
				{
					++dynamic_lighting::m_map_light_duplicate_owner_rejects;
					continue;
				}
				auto* entity = intf->m_entity_list->get_client_entity(index);
				if (!entity || entity->is_player()) continue;
				const auto* cls = entity->client_class();
				if (!cls || !source_map_entities::runtime_class_matches_map_class(runtime_entity_class_name(cls), map_classname)) continue;
				const Vector origin = entity->get_absolute_origin();
				const float distance_sq = origin.DistToSqr(map_origin);
				if (!std::isfinite(distance_sq) || distance_sq > radius_sq) continue;

				float score = radius - std::sqrt(std::max(0.0f, distance_sq));
				if (dynamic_lighting::m_map_light_model_aware_binding && !map_model.empty())
				{
					const auto runtime_model = runtime_entity_model_name(entity);
					if (!runtime_model_matches(map_model, runtime_model))
					{
						++dynamic_lighting::m_map_light_model_rejects;
						continue;
					}
					score += 256.0f;
				}
				if (!entity->is_dormant()) score += 12.0f;
				if (score <= best_score) continue;
				best_score = score;
				out_index = index;
				out_entity = entity;
				out_class = cls;
			}
			return out_entity != nullptr;
		}


		bool find_runtime_light_entity_for_map_light(const std::string& map_classname, const Vector& map_origin,
			const float max_distance, const std::unordered_set<int>* excluded_runtime_indices, int& out_index,
			sdk::c_base_entity*& out_entity, const sdk::c_client_class*& out_class)
		{
			out_index = -1;
			out_entity = nullptr;
			out_class = nullptr;
			const auto* intf = interfaces::get();
			if (!intf || !intf->m_entity_list || map_classname.empty()) return false;

			const float radius = std::max(8.0f, max_distance);
			const float radius_sq = radius * radius;
			float best_score = -FLT_MAX;
			const int max_entities = intf->m_entity_list->get_max_entity();
			for (int index = 1; index < max_entities; ++index)
			{
				if (excluded_runtime_indices && excluded_runtime_indices->contains(index)) continue;
				auto* entity = intf->m_entity_list->get_client_entity(index);
				if (!entity || entity->is_player()) continue;
				const auto* cls = entity->client_class();
				if (!cls || !source_map_entities::runtime_class_matches_map_class(runtime_entity_class_name(cls), map_classname)) continue;
				const Vector origin = entity->get_absolute_origin();
				const float distance_sq = origin.DistToSqr(map_origin);
				if (!std::isfinite(distance_sq) || distance_sq > radius_sq) continue;

				float score = radius - std::sqrt(std::max(0.0f, distance_sq));
				if (!entity->is_dormant()) score += 16.0f;
				if (runtime_light_entity_is_enabled(entity, cls)) score += 4.0f;
				if (score <= best_score) continue;
				best_score = score;
				out_index = index;
				out_entity = entity;
				out_class = cls;
			}
			return out_entity != nullptr;
		}

		const source_map_entities::entity_s* map_entity_from_owner_reference(const int owner_reference)
		{
			if (!g_map_entity_graph_loaded || owner_reference < 0) return nullptr;
			const std::array<int, 2> indices = { owner_reference, owner_reference - 1 };
			for (const int index : indices)
			{
				if (index >= 0 && static_cast<std::size_t>(index) < g_map_entity_graph.entities.size())
				{
					const auto& entity = g_map_entity_graph.entities[static_cast<std::size_t>(index)];
					if (entity.is_bindable_owner) return &entity;
				}
			}
			for (const auto& entity : g_map_entity_graph.entities)
			{
				if (entity.is_bindable_owner && static_cast<int>(entity.source_index) == owner_reference) return &entity;
			}
			return nullptr;
		}

		bool extrude_local_light_from_owner(sdk::c_base_entity* owner, const Vector& local_origin,
			Vector local_direction, const float surface_offset, Vector& out_local_origin)
		{
			out_local_origin = local_origin;
			if (!owner) return false;
			auto* collideable = owner->get_collideable();
			if (!collideable) return false;
			const Vector mins = collideable->mins();
			const Vector maxs = collideable->maxs();
			if (!std::isfinite(mins.x) || !std::isfinite(mins.y) || !std::isfinite(mins.z) ||
				!std::isfinite(maxs.x) || !std::isfinite(maxs.y) || !std::isfinite(maxs.z) ||
				mins.x >= maxs.x || mins.y >= maxs.y || mins.z >= maxs.z) {
				return false;
			}

			const bool inside = local_origin.x >= mins.x && local_origin.x <= maxs.x &&
				local_origin.y >= mins.y && local_origin.y <= maxs.y &&
				local_origin.z >= mins.z && local_origin.z <= maxs.z;
			if (!inside) return false;

			if (local_direction.LengthSqr() <= 0.0001f) {
				local_direction = Vector(0.0f, 0.0f, 1.0f);
			}
			local_direction.Normalize();
			float exit_distance = FLT_MAX;
			auto test_axis = [&](const float position, const float direction, const float lo, const float hi)
			{
				if (direction > 0.0001f) exit_distance = std::min(exit_distance, (hi - position) / direction);
				else if (direction < -0.0001f) exit_distance = std::min(exit_distance, (lo - position) / direction);
			};
			test_axis(local_origin.x, local_direction.x, mins.x, maxs.x);
			test_axis(local_origin.y, local_direction.y, mins.y, maxs.y);
			test_axis(local_origin.z, local_direction.z, mins.z, maxs.z);
			if (!std::isfinite(exit_distance) || exit_distance < 0.0f || exit_distance == FLT_MAX) return false;
			out_local_origin = local_origin + local_direction * (exit_distance + std::max(0.0f, surface_offset));
			return true;
		}

		bool get_runtime_owner_state(const int owner_index, Vector& out_origin, Vector& out_angles,
			bool& out_visible, sdk::c_base_entity** out_entity = nullptr, const sdk::c_client_class** out_class = nullptr)
		{
			out_visible = false;
			const auto* intf = interfaces::get();
			if (!intf || !intf->m_entity_list || owner_index <= 0 || owner_index >= intf->m_entity_list->get_max_entity()) {
				return false;
			}

			auto* entity = intf->m_entity_list->get_client_entity(owner_index);
			if (!entity) {
				return false;
			}

			const auto* cls = entity->client_class();
			if (!cls) {
				return false;
			}

			out_origin = entity->get_absolute_origin();
			out_angles = entity->get_absolute_angles();
			out_visible = runtime_entity_is_rendered(entity, cls);
			if (out_entity) *out_entity = entity;
			if (out_class) *out_class = cls;
			return true;
		}

		enum class runtime_owner_state_e
		{
			missing,
			active,
			dormant,
			hidden,
			destroyed,
		};

		bool get_runtime_attachment_transform(sdk::c_base_entity* entity, const std::string& attachment,
			Vector& out_origin, Vector& out_angles)
		{
			if (!entity || attachment.empty()) return false;
			auto* renderable = reinterpret_cast<components::IClientRenderable*>(reinterpret_cast<std::uintptr_t>(entity) + 4u);
			if (!renderable || !renderable->vftable_iclientrenderable ||
				!renderable->vftable_iclientrenderable->LookupAttachment ||
				!renderable->vftable_iclientrenderable->GetAttachment0) return false;
			const int index = renderable->vftable_iclientrenderable->LookupAttachment(renderable, attachment.c_str());
			if (index <= 0) return false;
			matrix3x4_t transform = {};
			if (!renderable->vftable_iclientrenderable->GetAttachment0(renderable, index, &transform)) return false;
			out_origin = Vector(transform.m_flMatVal[0][3], transform.m_flMatVal[1][3], transform.m_flMatVal[2][3]);
			utils::matrix_angles(transform, &out_angles);
			return std::isfinite(out_origin.x) && std::isfinite(out_origin.y) && std::isfinite(out_origin.z);
		}

		runtime_owner_state_e evaluate_runtime_owner_state(sdk::c_base_entity* entity, const sdk::c_client_class* cls,
			const bool expected_breakable, const int authored_health, bool& health_seen_positive, int& last_health)
		{
			if (!entity || !cls) return runtime_owner_state_e::missing;

			bool broken = false;
			if (read_entity_bool_prop(entity, cls, { "m_bIsBroken", "m_bBroken" }, broken) && broken) {
				return runtime_owner_state_e::destroyed;
			}

			int health = -1;
			if (read_entity_prop(entity, cls, { "m_iHealth" }, health))
			{
				last_health = health;
				if (health > 0) health_seen_positive = true;
				if (expected_breakable && health <= 0 && (health_seen_positive || authored_health > 0)) {
					return runtime_owner_state_e::destroyed;
				}
			}

			constexpr int EF_NODRAW = 0x20;
			if ((entity->effect_flags() & EF_NODRAW) != 0) return runtime_owner_state_e::hidden;
			bool disabled = false;
			if (read_entity_bool_prop(entity, cls, { "m_bDisabled", "m_iDisabled" }, disabled) && disabled) {
				return runtime_owner_state_e::hidden;
			}
			if (entity->is_dormant()) return runtime_owner_state_e::dormant;
			return runtime_owner_state_e::active;
		}

		float move_towards(const float current, const float target, const float max_delta)
		{
			if (current < target) return std::min(target, current + max_delta);
			if (current > target) return std::max(target, current - max_delta);
			return target;
		}

		std::uint64_t runtime_hash_bytes(std::uint64_t hash, const void* data, const std::size_t size)
		{
			const auto* bytes = static_cast<const std::uint8_t*>(data);
			for (std::size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ull;
			}
			return hash;
		}

		std::uint64_t runtime_light_signature(const map_settings::remix_light_settings_s& def, const bool enabled)
		{
			std::uint64_t hash = 1469598103934665603ull;
			hash = runtime_hash_bytes(hash, &enabled, sizeof(enabled));
			if (!def.points.empty())
			{
				const auto& point = def.points.front();
				hash = runtime_hash_bytes(hash, &point.position, sizeof(point.position));
				hash = runtime_hash_bytes(hash, &point.radiance, sizeof(point.radiance));
				hash = runtime_hash_bytes(hash, &point.radiance_scalar, sizeof(point.radiance_scalar));
				hash = runtime_hash_bytes(hash, &point.radius, sizeof(point.radius));
				hash = runtime_hash_bytes(hash, &point.use_shaping, sizeof(point.use_shaping));
				hash = runtime_hash_bytes(hash, &point.direction, sizeof(point.direction));
				hash = runtime_hash_bytes(hash, &point.degrees, sizeof(point.degrees));
				hash = runtime_hash_bytes(hash, &point.softness, sizeof(point.softness));
				hash = runtime_hash_bytes(hash, &point.exponent, sizeof(point.exponent));
				hash = runtime_hash_bytes(hash, &point.authoring_shape, sizeof(point.authoring_shape));
				hash = runtime_hash_bytes(hash, &point.authoring_width, sizeof(point.authoring_width));
				hash = runtime_hash_bytes(hash, &point.authoring_height, sizeof(point.authoring_height));
				hash = runtime_hash_bytes(hash, &point.authoring_length, sizeof(point.authoring_length));
			}
			return hash;
		}

		map_settings::remix_light_settings_s make_runtime_source_light_def(
			const std::string& comment,
			const std::string& group,
			const Vector& origin,
			const Vector& radiance,
			const float scalar,
			const float radius,
			const bool shaped,
			Vector direction,
			const float degrees,
			const float softness,
			const float exponent)
		{
			if (direction.LengthSqr() <= 0.0001f) {
				direction = Vector(1.0f, 0.0f, 0.0f);
			}
			direction.Normalize();

			map_settings::remix_light_settings_s def = {};
			def.enabled = true;
			def.group = group;
			def.comment = comment;
			map_settings::remix_light_settings_s::point_s point = {};
			point.position = origin;
			point.radiance = radiance;
			point.radiance_scalar = std::clamp(scalar, 0.0f, 250000.0f);
			point.radius = std::max(0.01f, radius);
			point.use_shaping = shaped;
			point.direction = direction;
			point.degrees = shaped ? std::clamp(degrees, 1.0f, 179.0f) : 180.0f;
			point.softness = shaped ? std::clamp(softness, 0.0f, 1.0f) : 0.0f;
			point.exponent = shaped ? std::clamp(exponent, 0.0f, 16.0f) : 0.0f;
			def.points.emplace_back(point);
			return def;
		}

		int find_compiled_direct_light_duplicate(const Vector& origin, Vector direction, const bool shaped)
		{
			if (!dynamic_lighting::m_source_direct_deduplicate_runtime ||
				dynamic_lighting::m_source_bsp_candidates.empty()) return -1;
			if (direction.LengthSqr() > 0.0001f) direction.Normalize();

			const float distance = std::max(0.0f, dynamic_lighting::m_source_direct_duplicate_distance);
			const float distance_sqr = distance * distance;
			const float direction_dot = std::clamp(dynamic_lighting::m_source_direct_duplicate_direction_dot, -1.0f, 1.0f);
			float best_distance_sqr = std::numeric_limits<float>::max();
			int best_source = -1;
			for (const auto& candidate : dynamic_lighting::m_source_bsp_candidates)
			{
				if (!candidate.from_worldlight || !candidate.selected || candidate.override_disabled) continue;
				if (candidate.source_type == static_cast<std::int32_t>(source_bsp_lights::emit_type::surface) ||
					candidate.source_type == static_cast<std::int32_t>(source_bsp_lights::emit_type::skylight) ||
					candidate.source_type == static_cast<std::int32_t>(source_bsp_lights::emit_type::skyambient)) continue;
				if (candidate.shaped != shaped) continue;

				const float candidate_distance_sqr = candidate.origin.DistToSqr(origin);
				if (candidate_distance_sqr > distance_sqr || candidate_distance_sqr >= best_distance_sqr) continue;
				if (shaped && candidate.direction.LengthSqr() > 0.0001f && direction.LengthSqr() > 0.0001f)
				{
					Vector candidate_direction = candidate.direction;
					candidate_direction.Normalize();
					if (candidate_direction.Dot(direction) < direction_dot) continue;
				}

				best_distance_sqr = candidate_distance_sqr;
				best_source = static_cast<int>(candidate.source_index);
			}
			return best_source;
		}

		Vector decode_source_color(const components::ColorRGBExp32& color, float& out_peak)
		{
			const float scale = std::ldexp(1.0f, static_cast<int>(color.exponent));
			const Vector linear(
				(static_cast<float>(color.r) / 255.0f) * scale,
				(static_cast<float>(color.g) / 255.0f) * scale,
				(static_cast<float>(color.b) / 255.0f) * scale);
			out_peak = std::max({ linear.x, linear.y, linear.z });
			if (!std::isfinite(out_peak) || out_peak <= 0.000001f) {
				out_peak = 0.0f;
				return Vector(1.0f, 1.0f, 1.0f);
			}
			return linear / out_peak;
		}

		Vector unpack_color32(const std::uint32_t packed, float& out_peak)
		{
			const float r = static_cast<float>(packed & 0xffu) / 255.0f;
			const float g = static_cast<float>((packed >> 8u) & 0xffu) / 255.0f;
			const float b = static_cast<float>((packed >> 16u) & 0xffu) / 255.0f;
			out_peak = std::max({ r, g, b });
			if (out_peak <= 0.000001f) {
				out_peak = 1.0f;
				return Vector(1.0f, 1.0f, 1.0f);
			}
			return Vector(r / out_peak, g / out_peak, b / out_peak);
		}

	}

	static void apply_runtime_entity_override(map_settings::remix_light_settings_s& def, bool& enabled,
		const source_map_light_overrides::light_override_s& entry);

	dynamic_lighting::dynamic_lighting()
	{
		p_this = this;
		// V21.0: game_settings loads the persisted profile and its explicit runtime
		// overrides before this component is constructed. Do not re-apply a preset here,
		// otherwise custom limits are silently overwritten on every launch.
		m_compat_profile = std::clamp(m_compat_profile, 0, 3);
		// SetLight hooks are intentionally lazy now. L4D2 usually does not expose useful
		// baked/dynamic map lights through D3D SetLight, so avoid installing hooks unless
		// the user explicitly enables Capture SetLight or Mirror To Remix in Advanced UI.
	}

	const char* dynamic_lighting::get_compat_profile_name(const int profile)
	{
		switch (profile >= 0 ? profile : m_compat_profile)
		{
		case 0: return "Authoring";
		case 1: return "Balanced";
		case 2: return "Performance";
		case 3: return "Ultra Performance";
		default: return "Custom";
		}
	}


	const char* dynamic_lighting::get_source_runtime_light_profile_name(const int profile)
	{
		switch (profile >= 0 ? profile : m_source_runtime_profile)
		{
		case 0: return "Raw Source";
		case 1: return "Balanced Map Lighting";
		case 2: return "World / Events Only";
		case 3: return "Diagnostics";
		default: return "Custom";
		}
	}

	void dynamic_lighting::apply_source_runtime_light_profile(int profile)
	{
		profile = std::clamp(profile, 0, 3);
		m_source_runtime_profile = profile;
		m_source_runtime_allow_world = true;
		m_source_runtime_allow_dlights = true;
		m_source_runtime_allow_elights = true;
		m_source_runtime_allow_entity_dynamic = true;
		m_source_runtime_allow_projected = true;
		m_source_runtime_allow_point_spotlight = true;
		m_source_runtime_show_rejected_in_registry = true;
		switch (profile)
		{
		case 0: // Preserve all raw Source effects for research.
			m_source_runtime_allow_survivors = true;
			m_source_runtime_allow_infected = true;
			m_source_runtime_allow_unknown_characters = true;
			m_source_runtime_world_intensity_scale = 1.0f;
			m_source_runtime_character_intensity_scale = 1.0f;
			m_source_runtime_world_radius_scale = 1.0f;
			m_source_runtime_character_radius_scale = 1.0f;
			m_source_runtime_max_radius = 32.0f;
			m_source_runtime_character_max_radius = 8.0f;
			m_source_runtime_character_spot_max_angle = 120.0f;
			break;
		case 1: // Gameplay default: keep map effects, filter player/zombie helpers.
			m_source_runtime_allow_survivors = false;
			m_source_runtime_allow_infected = false;
			m_source_runtime_allow_unknown_characters = false;
			m_source_runtime_world_intensity_scale = 1.0f;
			m_source_runtime_character_intensity_scale = 0.20f;
			m_source_runtime_world_radius_scale = 1.0f;
			m_source_runtime_character_radius_scale = 0.25f;
			m_source_runtime_max_radius = 8.0f;
			m_source_runtime_character_max_radius = 1.25f;
			m_source_runtime_character_spot_max_angle = 55.0f;
			break;
		case 2: // Only authored/world effects and event-driven map lights.
			m_source_runtime_allow_survivors = false;
			m_source_runtime_allow_infected = false;
			m_source_runtime_allow_unknown_characters = false;
			m_source_runtime_allow_elights = false;
			m_source_runtime_world_intensity_scale = 0.85f;
			m_source_runtime_character_intensity_scale = 0.0f;
			m_source_runtime_world_radius_scale = 0.85f;
			m_source_runtime_character_radius_scale = 0.0f;
			m_source_runtime_max_radius = 6.0f;
			m_source_runtime_character_max_radius = 0.25f;
			m_source_runtime_character_spot_max_angle = 45.0f;
			break;
		case 3: // Keep every record visible but submit only world/map lights.
			m_source_runtime_allow_survivors = false;
			m_source_runtime_allow_infected = false;
			m_source_runtime_allow_unknown_characters = false;
			m_source_runtime_world_intensity_scale = 1.0f;
			m_source_runtime_character_intensity_scale = 0.0f;
			m_source_runtime_world_radius_scale = 1.0f;
			m_source_runtime_character_radius_scale = 0.0f;
			m_source_runtime_max_radius = 12.0f;
			m_source_runtime_character_max_radius = 0.25f;
			m_source_runtime_character_spot_max_angle = 45.0f;
			break;
		}
		m_source_runtime_last_policy = std::format("profile applied: {}", get_source_runtime_light_profile_name(profile));
		game_settings::mark_dirty("Source runtime light profile");
	}

	std::string dynamic_lighting::get_event_light_summary()
	{
		return std::format(
			"event system {} | controlled {} | sprite/glow proxies {} | styled/blinking {} | pending actions {} | executed {} | transitions {} | AcceptInput {} ({})",
			m_map_event_lights_enabled ? "enabled" : "disabled",
			m_map_event_controlled_candidates, m_map_event_sprite_candidates, m_map_event_styled_candidates,
			m_map_light_io_pending_count, m_map_light_io_actions_executed, m_map_light_reason_transitions,
			(m_map_event_accept_input && m_server_accept_input_capture_enabled) ? "enabled" : "disabled",
			m_server_accept_input_hook_status);
	}

	std::string dynamic_lighting::get_facing_poly_link_report()
	{
		std::ostringstream out;
		out << "Facing-poly / emit_surface linkage\n";
		out << "material-linked: " << m_surface_material_linked << "\n";
		out << "material-unresolved: " << m_surface_material_unresolved << "\n";
		out << "event-linked sprite/glow proxies: " << m_surface_event_linked << "\n";
		for (const auto& candidate : m_source_bsp_candidates)
		{
			if (candidate.source_type != static_cast<std::int32_t>(source_bsp_lights::emit_type::surface)) continue;
			out << "#" << candidate.source_index << " texinfo=" << candidate.texinfo
				<< " material=" << (candidate.surface_material_name.empty() ? "<unresolved>" : candidate.surface_material_name)
				<< " event=" << (candidate.surface_event_proxy ? "yes" : "no")
				<< " mapClass=" << candidate.map_light_classname
				<< " group=" << candidate.map_light_control_group
				<< " reason=" << candidate.surface_link_reason << "\n";
		}
		return out.str();
	}

	void dynamic_lighting::reset_runtime_budget_counters()
	{
		m_runtime_skipped_budget = 0u;
		m_runtime_skipped_active_limit = 0u;
		m_runtime_skipped_pending_limit = 0u;
		m_runtime_skipped_muzzle_budget = 0u;
		m_runtime_skipped_hash_budget = 0u;
		m_runtime_spawned_this_second = 0u;
		m_runtime_muzzle_this_second = 0u;
		m_runtime_hash_this_second = 0u;
		m_runtime_active_lights_snapshot = remix_lights::get() ? static_cast<std::uint32_t>(remix_lights::get()->get_active_light_count()) : 0u;
		m_cpu_skin_skipped_common = 0u;
		m_cpu_skin_skipped_ragdoll = 0u;
		remix_api::reset_flashlight_runtime_stats();
	}

	void dynamic_lighting::apply_compat_profile(int profile)
	{
		profile = std::clamp(profile, 0, 3);
		m_compat_profile = profile;
		m_runtime_budgets_enabled = true;
		m_small_radius_mode = true;
		m_d3d_light_capture_enabled = false;
		m_d3d_light_spawn_enabled = false;
		m_sound_hash_library_enabled = false;
		m_sound_hash_alarm_enabled = false;
		m_source_bsp_scan_spawn_helpers = false;
		m_draw_debug = false;
		m_sound_hash_draw_debug = false;
		m_muzzle_draw_debug = false;
		m_flashlight_governor_enabled = false;
		m_flashlight_motion_epsilon = 0.10f;
		m_flashlight_direction_epsilon_degrees = 0.10f;
		m_flashlight_force_update_distance = 32.0f;
		m_flashlight_cull_distant_bots = true;
		m_flashlight_nearest_bot_priority = true;
		m_flashlight_preserve_last_good_rig = true;
		m_flashlight_verify_draw_results = false;
		m_flashlight_owner_grace_frames = 3u;
		m_flashlight_retry_base_ms = 100u;
		m_flashlight_retry_max_ms = 2000u;

		switch (profile)
		{
		case 0: // Authoring: all tools available, high budgets.
			m_authoring_debug_tools = true;
			m_runtime_max_active_lights = 128u;
			m_runtime_max_pending_lights = 128u;
			m_runtime_max_spawns_per_second = 64u;
			m_runtime_max_muzzle_per_second = 48u;
			m_runtime_max_sound_hash_per_second = 16u;
			m_runtime_leaf_check_hz = 60.0f;
			m_small_radius_max = 24.0f;
			m_source_bsp_scan_preview_limit = 96;
			m_cpu_skin_throttle_enabled = false;
			m_cpu_skin_skip_far_common = false;
			m_cpu_skin_skip_far_ragdolls = false;
			m_flashlight_update_hz = 120.0f;
			m_flashlight_player_layer_limit = 4u;
			m_flashlight_bot_layer_limit = 4u;
			m_flashlight_budget_reserve = 8u;
			m_flashlight_cull_distant_bots = false;
			m_flashlight_bot_cull_distance = 3000.0f;
			break;
		case 1: // Balanced gameplay defaults.
			m_authoring_debug_tools = false;
			m_runtime_max_active_lights = 48u;
			m_runtime_max_pending_lights = 48u;
			m_runtime_max_spawns_per_second = 24u;
			m_runtime_max_muzzle_per_second = 24u;
			m_runtime_max_sound_hash_per_second = 4u;
			m_runtime_leaf_check_hz = 20.0f;
			m_small_radius_max = 16.0f;
			m_source_bsp_scan_preview_limit = 32;
			m_cpu_skin_throttle_enabled = false;
			m_cpu_skin_skip_far_common = false;
			m_cpu_skin_skip_far_ragdolls = false;
			m_flashlight_update_hz = 60.0f;
			m_flashlight_player_layer_limit = 4u;
			m_flashlight_bot_layer_limit = 2u;
			m_flashlight_budget_reserve = 6u;
			m_flashlight_bot_cull_distance = 2200.0f;
			break;
		case 2: // Performance: fewer dynamic updates and optional far animated mesh skips.
			m_authoring_debug_tools = false;
			m_runtime_max_active_lights = 28u;
			m_runtime_max_pending_lights = 24u;
			m_runtime_max_spawns_per_second = 12u;
			m_runtime_max_muzzle_per_second = 12u;
			m_runtime_max_sound_hash_per_second = 2u;
			m_runtime_leaf_check_hz = 10.0f;
			m_small_radius_max = 10.0f;
			m_source_bsp_scan_preview_limit = 16;
			m_cpu_skin_throttle_enabled = true;
			m_cpu_skin_skip_far_common = true;
			m_cpu_skin_skip_far_ragdolls = false;
			m_cpu_skin_common_skip_distance = 2200.0f;
			m_cpu_skin_ragdoll_skip_distance = 1600.0f;
			m_flashlight_update_hz = 45.0f;
			m_flashlight_player_layer_limit = 3u;
			m_flashlight_bot_layer_limit = 2u;
			m_flashlight_budget_reserve = 5u;
			m_flashlight_bot_cull_distance = 1800.0f;
			m_flashlight_owner_grace_frames = 4u;
			break;
		case 3: // Ultra Performance: aggressive gameplay mode.
			m_authoring_debug_tools = false;
			m_runtime_max_active_lights = 16u;
			m_runtime_max_pending_lights = 12u;
			m_runtime_max_spawns_per_second = 6u;
			m_runtime_max_muzzle_per_second = 5u;
			m_runtime_max_sound_hash_per_second = 0u;
			m_runtime_leaf_check_hz = 5.0f;
			m_small_radius_max = 6.0f;
			m_source_bsp_scan_preview_limit = 8;
			m_sound_hash_library_enabled = false;
			m_cpu_skin_throttle_enabled = true;
			m_cpu_skin_skip_far_common = true;
			m_cpu_skin_skip_far_ragdolls = true;
			m_cpu_skin_common_skip_distance = 1500.0f;
			m_cpu_skin_ragdoll_skip_distance = 1100.0f;
			m_flashlight_update_hz = 30.0f;
			m_flashlight_player_layer_limit = 2u;
			m_flashlight_bot_layer_limit = 1u;
			m_flashlight_budget_reserve = 4u;
			m_flashlight_bot_cull_distance = 1400.0f;
			m_flashlight_owner_grace_frames = 5u;
			m_flashlight_retry_base_ms = 150u;
			break;
		}

		reset_runtime_budget_counters();
	}

	bool dynamic_lighting::should_skip_model_for_cpu_skin_budget(const ModelRenderInfo_t& info)
	{
		if (!m_cpu_skin_throttle_enabled || !info.pModel || !game::get_current_view_origin()) {
			return false;
		}

		const std::string_view model_path(info.pModel->szPathName);
		const bool likely_common = model_path.contains("infected") || model_path.contains("common") || model_path.contains("zombie");
		const bool likely_ragdoll = model_path.contains("ragdoll") || model_path.contains("gibs") || model_path.contains("corpse") || model_path.contains("dead");
		const float dist_sqr = game::get_current_view_origin()->DistToSqr(info.origin);

		if (m_cpu_skin_skip_far_ragdolls && likely_ragdoll)
		{
			const float limit = std::max(0.0f, m_cpu_skin_ragdoll_skip_distance);
			if (dist_sqr > limit * limit)
			{
				++m_cpu_skin_skipped_ragdoll;
				return true;
			}
		}

		if (m_cpu_skin_skip_far_common && likely_common)
		{
			const float limit = std::max(0.0f, m_cpu_skin_common_skip_distance);
			if (dist_sqr > limit * limit)
			{
				++m_cpu_skin_skipped_common;
				return true;
			}
		}

		return false;
	}


	bool dynamic_lighting::rebuild_map_entity_graph()
	{
		g_map_entity_graph = {};
		g_map_entity_graph_loaded = false;
		g_map_entity_graph_map.clear();
		m_server_accept_input_hook_scan_complete = false;
		m_server_accept_input_hook_status = "waiting for map entity graph";
		m_map_light_io_group_states.clear();
		m_map_light_io_exact_states.clear();
		{
			std::scoped_lock lock(g_server_accept_input_mutex);
			g_server_accept_input_events.clear();
		}
		m_server_accept_input_last_event = "none";
		m_map_light_io_pending_actions.clear();
		m_map_light_io_pending_count = 0u;
		m_map_light_io_root_fire_counts.clear();
		m_map_light_io_next_serial = 1u;
		m_map_entity_graph_entities = 0u;
		m_map_entity_graph_lights = 0u;
		m_map_entity_graph_owners = 0u;
		m_map_entity_graph_matched_lights = 0u;
		m_map_entity_graph_bound_owners = 0u;
		m_map_entity_graph_extruded = 0u;
		m_map_entity_graph_attachment_links = 0u;
		m_map_entity_graph_io_links = 0u;
		m_map_entity_graph_io_resolved_links = 0u;
		m_map_entity_graph_io_light_links = 0u;
		m_map_entity_graph_io_kill_links = 0u;
		m_map_entity_graph_io_transitive_links = 0u;
		m_map_entity_graph_io_relay_hops = 0u;
		m_map_entity_graph_io_cycles = 0u;
		m_map_entity_graph_io_depth_limited = 0u;
		m_map_entity_graph_io_multi_manager_links = 0u;
		m_map_entity_graph_io_relay_entities = 0u;
		m_map_entity_graph_controlled_lights = 0u;
		m_map_entity_graph_light_groups = 0u;

		if (!m_map_entity_graph_enabled)
		{
			m_map_entity_graph_status = "entity graph disabled";
			return false;
		}

		std::string map = normalize_map_session_name(m_bsp_worldlight_pending_map);
		if (map.empty()) map = normalize_map_session_name(map_settings::get_map_name());
		if (map.empty())
		{
			m_map_entity_graph_status = "no current map name";
			return false;
		}

		source_bsp_lights::map_file_s map_file = {};
		std::string error;
		if (!source_bsp_lights::load_map_file(game::root_path, map, map_file, error))
		{
			m_map_entity_graph_status = error.empty() ? "BSP unavailable for entity graph" : error;
			return false;
		}

		std::string entity_lump;
		if (!source_bsp_lights::read_entity_lump(map_file, entity_lump, error))
		{
			m_map_entity_graph_status = error.empty() ? "entity lump unavailable" : error;
			return false;
		}

		if (!source_map_entities::parse_entity_lump(entity_lump, g_map_entity_graph, error))
		{
			m_map_entity_graph_status = error.empty() ? "entity graph parse failed" : error;
			return false;
		}

		g_map_entity_graph_loaded = true;
		g_map_entity_graph_map = map;
		m_map_entity_graph_entities = static_cast<std::uint32_t>(g_map_entity_graph.entities.size());
		m_map_entity_graph_lights = g_map_entity_graph.light_entities;
		m_map_entity_graph_owners = g_map_entity_graph.bindable_owners;
		m_map_entity_graph_attachment_links = g_map_entity_graph.attachment_links;
		m_map_entity_graph_io_links = g_map_entity_graph.io_links;
		m_map_entity_graph_io_resolved_links = g_map_entity_graph.io_resolved_links;
		m_map_entity_graph_io_light_links = g_map_entity_graph.io_light_links;
		m_map_entity_graph_io_kill_links = g_map_entity_graph.io_kill_links;
		m_map_entity_graph_io_transitive_links = g_map_entity_graph.io_transitive_light_links;
		m_map_entity_graph_io_relay_hops = g_map_entity_graph.io_relay_hops;
		m_map_entity_graph_io_cycles = g_map_entity_graph.io_cycles_skipped;
		m_map_entity_graph_io_depth_limited = g_map_entity_graph.io_depth_limited;
		m_map_entity_graph_io_multi_manager_links = g_map_entity_graph.io_multi_manager_links;
		m_map_entity_graph_io_relay_entities = g_map_entity_graph.io_relay_entities;
		m_map_entity_graph_controlled_lights = g_map_entity_graph.controlled_light_entities;
		m_map_entity_graph_light_groups = g_map_entity_graph.light_control_groups;
		m_map_entity_graph_status = std::format("loaded {} entities, {} lights, {} owners, {} attachments; I/O links {}/{}, leaf/transitive {}/{}, relay hops {}, controlled lights {}, groups {}, kill links {}, cycles/depth {}/{}",
			m_map_entity_graph_entities, m_map_entity_graph_lights, m_map_entity_graph_owners, m_map_entity_graph_attachment_links,
			m_map_entity_graph_io_resolved_links, m_map_entity_graph_io_links,
			m_map_entity_graph_io_light_links, m_map_entity_graph_io_transitive_links, m_map_entity_graph_io_relay_hops,
			m_map_entity_graph_controlled_lights, m_map_entity_graph_light_groups, m_map_entity_graph_io_kill_links,
			m_map_entity_graph_io_cycles, m_map_entity_graph_io_depth_limited);
		return true;
	}

	void dynamic_lighting::clear_runtime_imported_lights()
	{
		if (remix_lights::get())
		{
			remix_lights::get()->destroy_lights_with_comment_prefix("Map runtime dlight:");
			remix_lights::get()->destroy_lights_with_comment_prefix("Map runtime elight:");
			remix_lights::get()->destroy_lights_with_comment_prefix("Map runtime projected:");
			remix_lights::get()->destroy_lights_with_comment_prefix("Map runtime entity light:");
		}
		m_runtime_dlights.clear();
		m_runtime_projected_lights.clear();
		g_captured_engine_lights.clear();
		m_source_dlight_active = 0u;
		m_source_dlight_release_holds = 0u;
		m_source_elight_active = 0u;
		m_source_projected_active = 0u;
		m_source_projected_pose_smoothed = 0u;
		m_source_projected_target_updates = 0u;
		m_source_projected_missing_holds = 0u;
		m_source_projected_generation_resets = 0u;
		m_source_entity_dynamic_active = 0u;
		m_source_direct_duplicates_suppressed = 0u;
		m_source_light_axis_corrections = 0u;
		m_source_light_axis_switches = 0u;
		m_source_light_basis_recoveries = 0u;
		m_source_light_shape_holds = 0u;
		m_map_light_runtime_status = "runtime map lights cleared";
	}


	std::vector<dynamic_lighting::source_direct_light_info_s> dynamic_lighting::get_source_direct_light_registry()
	{
		const auto& editor_lights = map_settings::get_map_settings().remix_lights;
		m_source_direct_snapshot_lights = static_cast<std::uint32_t>(std::count_if(editor_lights.begin(), editor_lights.end(),
			[](const auto& def) { return def.generated_source_transient; }));

		auto evaluate_style = [](const std::int32_t style)
		{
			if (style <= 0) return 1.0f;
			const auto* intf = interfaces::get();
			constexpr std::int32_t max_source_lightstyles = 64;
			float value = style < max_source_lightstyles && intf && intf->m_engine
				? intf->m_engine->light_style_value(style) : 1.0f;
			if (!std::isfinite(value)) value = 1.0f;
			return std::clamp(value, 0.0f, 4.0f);
		};

		std::vector<source_direct_light_info_s> result;
		result.reserve(m_source_bsp_candidates.size() + m_runtime_projected_lights.size() + m_runtime_dlights.size());

		for (const auto& candidate : m_source_bsp_candidates)
		{
			source_direct_light_info_s info = {};
			info.kind = candidate.distant ? "bsp_distant" : std::format("bsp_{}", source_bsp_lights::emit_type_name(
				static_cast<source_bsp_lights::emit_type>(candidate.source_type)));
			info.classname = candidate.map_light_classname.empty() ? candidate.classname : candidate.map_light_classname;
			info.comment = candidate.comment;
			info.position = candidate.origin;
			info.direction = candidate.direction;
			info.radiance = candidate.radiance;
			info.intensity = candidate.scalar;
			info.radius = candidate.radius;
			info.outer_angle = candidate.distant ? candidate.distant_angular_diameter : candidate.degrees;
			info.inner_angle = candidate.distant ? candidate.distant_angular_diameter :
				candidate.degrees * (1.0f - std::clamp(candidate.softness, 0.0f, 1.0f));
			info.source_index = static_cast<std::int32_t>(candidate.source_index);
			info.style = candidate.style;
			info.style_value = evaluate_style(candidate.style);
			info.owner = candidate.owner;
			info.enabled = candidate.selected && !candidate.override_disabled;
			info.transient = false;
			info.shaped = candidate.shaped;
			info.owner_category = candidate.graph_owner_bound ? "map owner/entity" : "world/map";
			info.owner_classname = candidate.map_owner_classname;
			info.surface_material_name = candidate.surface_material_name;
			info.event_controlled = candidate.map_light_control_group > 0u || candidate.style > 0 || candidate.surface_event_proxy;
			info.policy_allowed = true;
			info.policy_reason = info.event_controlled ? "compiled event-linked map light" : "compiled static map light";
			if (candidate.distant)
			{
				info.direction_source = candidate.distant_source;
				info.shape_reason = "native Remix Distant singleton";
				info.direction_confidence = 1.0f;
				info.shape_confidence = 1.0f;
				info.shape_evidence = 1.0f;
				info.axis_candidates = candidate.distant_source;
				info.parameter_sources = std::format("Source light_environment + VRAD skylight; angular diameter {:.2f} deg", candidate.distant_angular_diameter);
			}
			else
			{
				info.direction_source = candidate.shaped ? "BSP WORLDLIGHTS normal" : "BSP WORLDLIGHTS position-only record";
				info.shape_reason = std::format("compiled emit type: {}", source_bsp_lights::emit_type_name(
					static_cast<source_bsp_lights::emit_type>(candidate.source_type)));
				info.direction_confidence = candidate.shaped ? 1.0f : 0.0f;
				info.shape_confidence = 1.0f;
				info.shape_evidence = candidate.shaped ? 1.0f : 0.0f;
				info.axis_candidates = candidate.shaped ? "compiled WORLDLIGHT normal=1.00" : "position-only record";
				info.parameter_sources = "compiled BSP WORLDLIGHTS/VRAD";
			}
			result.emplace_back(std::move(info));
		}

		auto append_runtime = [&](const runtime_source_light_s& tracked, const std::uint64_t capture_id)
		{
			if (!tracked.policy_allowed && !m_source_runtime_show_rejected_in_registry) return;
			if (tracked.def.points.empty()) return;
			const auto& point = tracked.def.points.front();
			source_direct_light_info_s info = {};
			info.kind = tracked.runtime_kind.empty() ? (tracked.entity_light ? "elight" : "dlight") : tracked.runtime_kind;
			info.classname = tracked.runtime_classname;
			info.comment = tracked.def.comment;
			info.position = point.position;
			info.direction = point.direction;
			info.radiance = point.radiance;
			info.intensity = point.radiance_scalar;
			info.radius = point.radius;
			info.outer_angle = tracked.source_outer_angle;
			info.inner_angle = tracked.source_inner_angle;
			info.near_z = tracked.source_near_z;
			info.far_z = tracked.source_far_z;
			info.runtime_entity_index = tracked.runtime_entity_index;
			info.source_key = tracked.source_key;
			info.capture_id = capture_id;
			info.style = tracked.source_style;
			info.style_value = evaluate_style(tracked.source_style);
			info.owner = tracked.source_owner;
			info.matched_compiled_source = tracked.matched_compiled_source;
			info.enabled = tracked.last_enabled && tracked.policy_allowed;
			info.transient = tracked.transient;
			info.owner_category = tracked.owner_category;
			info.owner_classname = tracked.owner_classname;
			info.owner_model = tracked.owner_model;
			info.policy_allowed = tracked.policy_allowed;
			info.policy_reason = tracked.policy_reason;
			info.shaped = point.use_shaping;
			info.shadows = tracked.shadows;
			info.duplicate_suppressed = tracked.duplicate_suppressed;
			info.direction_source = tracked.direction_source;
			info.shape_reason = tracked.shape_reason;
			info.direction_confidence = tracked.direction_confidence;
			info.shape_confidence = tracked.shape_confidence;
			info.axis_disagreement_degrees = tracked.axis_disagreement_degrees;
			info.shape_evidence = tracked.shape_evidence;
			info.axis_stable_frames = tracked.axis_stable_frames;
			info.axis_switches = tracked.axis_switches;
			info.axis_candidates = tracked.axis_candidates;
			info.parameter_sources = tracked.parameter_sources;
			info.direction_corrected = tracked.direction_corrected;
			info.basis_recovered = tracked.basis_recovered;
			result.emplace_back(std::move(info));
		};

		for (const auto& [index, tracked] : m_runtime_projected_lights) append_runtime(tracked, static_cast<std::uint64_t>(index));
		for (const auto& [key, tracked] : m_runtime_dlights) append_runtime(tracked, static_cast<std::uint64_t>(key));
		std::stable_sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs)
		{
			if (lhs.transient != rhs.transient) return !lhs.transient;
			if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
			if (lhs.source_index != rhs.source_index) return lhs.source_index < rhs.source_index;
			if (lhs.runtime_entity_index != rhs.runtime_entity_index) return lhs.runtime_entity_index < rhs.runtime_entity_index;
			return lhs.source_key < rhs.source_key;
		});
		return result;
	}

	std::string dynamic_lighting::get_source_direct_light_report()
	{
		const auto registry = get_source_direct_light_registry();
		std::ostringstream out;
		out << "Source Direct Light Registry\n";
		out << "kind\tstate\tid\tclass\tstyle/value\towner\towner_category\towner_class/model\tpolicy/reason\tsurface_material\tevent\tposition\tdirection\tdirection_source/confidence/stability\taxis_candidates\tradiance\tintensity\tradius\tshape/reason/confidence/evidence\tparameters\tnear/far\tflags\tmatch\tcomment\n";
		for (const auto& light : registry)
		{
			std::string id;
			if (light.source_index >= 0) id = std::format("source:{}", light.source_index);
			else if (light.runtime_entity_index >= 0) id = std::format("entity:{}", light.runtime_entity_index);
			else id = std::format("key:{}", light.source_key);
			const std::string flags = std::format("{}{}{}",
				light.transient ? "transient " : "", light.shadows ? "shadows " : "",
				light.duplicate_suppressed ? "dedup" : "");
			out << light.kind << '\t'
				<< (light.enabled ? "on" : "off") << (light.duplicate_suppressed ? "/dedup" : "") << '\t'
				<< id << '\t' << light.classname << '\t'
				<< std::format("{}/{:.3f}", light.style, light.style_value) << '\t' << light.owner << '\t'
				<< light.owner_category << '\t'
				<< std::format("{} / {}", light.owner_classname, light.owner_model) << '\t'
				<< std::format("{} / {}", light.policy_allowed ? "allowed" : "blocked", light.policy_reason) << '\t'
				<< (light.surface_material_name.empty() ? "-" : light.surface_material_name) << '\t'
				<< (light.event_controlled ? "yes" : "no") << '\t'
				<< std::format("{:.2f} {:.2f} {:.2f}", light.position.x, light.position.y, light.position.z) << '\t'
				<< std::format("{:.3f} {:.3f} {:.3f}", light.direction.x, light.direction.y, light.direction.z) << '\t'
				<< std::format("{} / {:.2f} / stable {} / switches {}{}{} / disagreement {:.1f}deg",
					light.direction_source, light.direction_confidence, light.axis_stable_frames, light.axis_switches,
					light.direction_corrected ? " / corrected" : "", light.basis_recovered ? " / basis-recovered" : "",
					light.axis_disagreement_degrees) << '\t'
				<< light.axis_candidates << '\t'
				<< std::format("{:.3f} {:.3f} {:.3f}", light.radiance.x, light.radiance.y, light.radiance.z) << '\t'
				<< std::format("{:.2f}", light.intensity) << '\t' << std::format("{:.2f}", light.radius) << '\t'
				<< std::format("{} / {} / {:.2f} / evidence {:.2f}",
					light.shaped ? std::format("spot {:.1f}/{:.1f}", light.inner_angle, light.outer_angle) : "sphere",
					light.shape_reason, light.shape_confidence, light.shape_evidence) << '\t'
				<< light.parameter_sources << '\t'
				<< std::format("{:.2f}/{:.2f}", light.near_z, light.far_z) << '\t' << flags << '\t'
				<< (light.matched_compiled_source >= 0 ? std::to_string(light.matched_compiled_source) : "-") << '\t'
				<< light.comment << '\n';
		}
		return out.str();
	}

	bool dynamic_lighting::snapshot_active_source_direct_lights_to_editor()
	{
		auto& editor_lights = map_settings::get_map_settings().remix_lights;
		struct candidate_s
		{
			const runtime_source_light_s* tracked = nullptr;
			std::uint64_t capture_id = 0u;
			float distance_sqr = 0.0f;
		};
		std::vector<candidate_s> candidates;
		const Vector camera = game::get_current_view_origin() ? *game::get_current_view_origin() : Vector(0.0f, 0.0f, 0.0f);
		for (const auto& [key, tracked] : m_runtime_dlights)
		{
			if (!tracked.last_enabled || !tracked.policy_allowed || tracked.def.points.empty()) continue;
			candidates.push_back({ &tracked, static_cast<std::uint64_t>(key), tracked.def.points.front().position.DistToSqr(camera) });
		}
		std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) { return lhs.distance_sqr < rhs.distance_sqr; });
		const std::size_t limit = static_cast<std::size_t>(std::clamp(m_source_direct_snapshot_limit, 1, 128));
		if (candidates.size() > limit) candidates.resize(limit);

		std::uint32_t created = 0u;
		std::uint32_t preserved = 0u;
		for (const auto& candidate : candidates)
		{
			const auto& tracked = *candidate.tracked;
			const auto existing = std::find_if(editor_lights.begin(), editor_lights.end(), [&](const auto& def)
			{
				return def.generated_source_transient && def.generated_source_capture_id == candidate.capture_id;
			});
			if (existing != editor_lights.end())
			{
				++preserved;
				continue;
			}

			auto def = tracked.def;
			def.group = "Captured Source direct lights";
			def.comment = std::format("Source direct snapshot: {} key={} | {}", tracked.runtime_kind,
				tracked.source_key, tracked.def.comment);
			def.generated_map_light = false;
			def.generated_source_kind = tracked.runtime_kind;
			def.generated_source_style = tracked.source_style;
			def.generated_source_owner = tracked.source_owner;
			def.generated_source_key = tracked.source_key;
			def.generated_source_capture_id = candidate.capture_id;
			def.generated_source_transient = true;
			def.generated_source_live_link = false;
			editor_lights.emplace_back(std::move(def));
			++created;
		}

		m_source_direct_snapshots_created += created;
		m_source_direct_snapshot_lights = static_cast<std::uint32_t>(std::count_if(editor_lights.begin(), editor_lights.end(),
			[](const auto& def) { return def.generated_source_transient; }));
		m_source_direct_status = std::format("captured {} active d/e lights into Light Editor ({} existing snapshots preserved; {} total)",
			created, preserved, m_source_direct_snapshot_lights);
		return created > 0u || preserved > 0u;
	}

	bool dynamic_lighting::snapshot_source_direct_light_to_editor(const std::uint64_t capture_id)
	{
		const auto it = m_runtime_dlights.find(static_cast<std::uintptr_t>(capture_id));
		if (it == m_runtime_dlights.end() || !it->second.last_enabled || !it->second.policy_allowed || it->second.def.points.empty())
		{
			m_source_direct_status = "selected transient Source light is no longer active";
			return false;
		}

		auto& editor_lights = map_settings::get_map_settings().remix_lights;
		const auto existing = std::find_if(editor_lights.begin(), editor_lights.end(), [&](const auto& def)
		{
			return def.generated_source_transient && def.generated_source_capture_id == capture_id;
		});
		if (existing != editor_lights.end())
		{
			m_source_direct_status = "selected transient Source light is already frozen in Light Editor";
			return true;
		}

		const auto& tracked = it->second;
		auto def = tracked.def;
		def.group = "Captured Source direct lights";
		def.comment = std::format("Source direct snapshot: {} key={} capture=0x{:x} | {}",
			tracked.runtime_kind, tracked.source_key, capture_id, tracked.def.comment);
		def.generated_map_light = false;
		def.generated_source_kind = tracked.runtime_kind;
		def.generated_source_style = tracked.source_style;
		def.generated_source_owner = tracked.source_owner;
		def.generated_source_key = tracked.source_key;
		def.generated_source_capture_id = capture_id;
		def.generated_source_transient = true;
		def.generated_source_live_link = false;
		editor_lights.emplace_back(std::move(def));
		++m_source_direct_snapshots_created;
		m_source_direct_snapshot_lights = static_cast<std::uint32_t>(std::count_if(editor_lights.begin(), editor_lights.end(),
			[](const auto& light) { return light.generated_source_transient; }));
		m_source_direct_status = std::format("frozen selected {} key={} into Light Editor ({} total)",
			tracked.runtime_kind, tracked.source_key, m_source_direct_snapshot_lights);
		return true;
	}

	bool dynamic_lighting::clear_source_direct_light_snapshots()
	{
		auto& editor_lights = map_settings::get_map_settings().remix_lights;
		const auto old_size = editor_lights.size();
		std::erase_if(editor_lights, [](const auto& def) { return def.generated_source_transient; });
		const auto removed = old_size - editor_lights.size();
		m_source_direct_snapshot_lights = 0u;
		m_source_direct_status = std::format("removed {} frozen Source direct-light snapshots from Light Editor", removed);
		return removed > 0u;
	}

	void dynamic_lighting::update_source_dlights()
	{
		m_source_dlight_release_holds = 0u;
		for (auto& [key, light] : m_runtime_dlights) {
			light.seen = false;
		}

		const auto* intf = interfaces::get();
		if (!intf || !intf->m_effects || !remix_lights::get())
		{
			m_source_dlight_active = 0u;
			m_source_elight_active = 0u;
			return;
		}

		const float curtime = now();
		std::unordered_set<std::uintptr_t> active_dlight_keys;
		std::unordered_set<std::uintptr_t> active_elight_keys;
		auto process_source_light = [&](components::dlight_t* source, const bool entity_light, const int allocation_key) -> bool
		{
			if (!source || !std::isfinite(source->origin.x) || !std::isfinite(source->origin.y) ||
				!std::isfinite(source->origin.z) || !std::isfinite(source->radius) || source->radius <= 0.01f) {
				return false;
			}
			if (std::isfinite(source->die) && source->die > 0.0f && source->die + 0.001f < curtime) {
				return false;
			}

			float color_peak = 0.0f;
			const Vector radiance = decode_source_color(source->color, color_peak);
			if (color_peak <= 0.0f) return false;

			const std::uintptr_t tracking_key = engine_light_tracking_key(source, entity_light);
			auto& tracked = m_runtime_dlights[tracking_key];
			const auto pointer32 = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(source));
			const int source_key = allocation_key != 0 ? allocation_key : source->key;

			Vector raw_direction = source->m_Direction;
			const bool raw_direction_valid = normalize_source_direction(raw_direction);
			const bool outer_valid = std::isfinite(source->m_OuterAngle) &&
				source->m_OuterAngle > 0.01f && source->m_OuterAngle < 179.0f;
			const bool inner_valid = std::isfinite(source->m_InnerAngle) && source->m_InnerAngle >= 0.0f &&
				(!outer_valid || source->m_InnerAngle <= source->m_OuterAngle + 0.5f);
			constexpr int source_dlight_no_world_illumination = 0x1;
			constexpr int source_dlight_no_model_illumination = 0x2;
			const bool endpoint_hotspot = source_key < 0 &&
				(source->flags & source_dlight_no_model_illumination) != 0 &&
				(source->flags & source_dlight_no_world_illumination) == 0;
			const bool strong_spot_evidence =
				(source->flags & source_dlight_no_world_illumination) != 0;

			float shape_evidence = 0.0f;
			if (outer_valid) shape_evidence += 0.38f;
			if (inner_valid && outer_valid) shape_evidence += 0.22f;
			if (raw_direction_valid) shape_evidence += 0.18f;
			if (strong_spot_evidence) shape_evidence += 0.34f;
			if (outer_valid && source->m_OuterAngle >= 170.0f) shape_evidence -= 0.35f;
			shape_evidence = std::clamp(shape_evidence, 0.0f, 1.0f);
			const bool raw_spot_vote = shape_evidence >= 0.72f;
			const bool shaped = resolve_source_spot_shape(shape_evidence,
				strong_spot_evidence && outer_valid && inner_valid && raw_direction_valid && !endpoint_hotspot,
				endpoint_hotspot, tracked.spot_evidence_frames, tracked.sphere_evidence_frames,
				tracked.shape_initialized, tracked.stable_shaped);
			if (shaped != raw_spot_vote && !endpoint_hotspot) ++m_source_light_shape_holds;

			const compiled_axis_prior_s no_compiled_prior = {};
			auto axis = resolve_source_light_axis(false, false, true, false,
				raw_direction, raw_direction_valid, Vector(), false, Vector(), false,
				Vector(), false, no_compiled_prior,
				tracked.last_resolved_direction, tracked.axis_initialized,
				tracked.last_axis_source);
			if (axis.source == "dynamic light direction") axis.source = "dlight.m_Direction";
			if (axis.source.starts_with("temporal dynamic light direction")) axis.source = "temporal dlight.m_Direction";
			if (axis.switched)
			{
				++tracked.axis_switches;
				++m_source_light_axis_switches;
				tracked.axis_stable_frames = 0u;
			}
			else ++tracked.axis_stable_frames;
			tracked.last_resolved_direction = axis.direction;
			tracked.last_axis_source = axis.source;
			tracked.axis_initialized = true;
			if (axis.corrected && (axis.switched || tracked.axis_stable_frames == 1u)) ++m_source_light_axis_corrections;
			if (axis.basis_recovered && (axis.switched || tracked.axis_stable_frames == 1u)) ++m_source_light_basis_recoveries;

			const int owner_index = source_key > 0 ? source_key : -1;
			const auto owner_info = classify_runtime_light_owner_with_proximity(owner_index, source->origin, 32.0f);
			std::string policy_reason = "accepted by Source runtime light policy";
			const bool class_allowed = source_runtime_class_allowed(entity_light, false, false, false, policy_reason);
			const bool owner_allowed = class_allowed && source_runtime_owner_allowed(owner_info, policy_reason);
			const bool policy_allowed = class_allowed && owner_allowed;
			count_runtime_policy_result(owner_info, policy_allowed);
			m_source_runtime_last_policy = std::format("{} {}: {}", entity_light ? "elight" : "dlight", owner_info.category_name, policy_reason);

			Vector direction = axis.direction;
			float outer = shaped ? std::clamp(source->m_OuterAngle, 1.0f, 179.0f) : 180.0f;
			if (shaped && owner_info.category != runtime_light_owner_category_e::world)
				outer = std::min(outer, std::clamp(m_source_runtime_character_spot_max_angle, 1.0f, 179.0f));
			const float inner = shaped ? std::clamp(source->m_InnerAngle, 0.0f, outer) : outer;
			const float softness = shaped && outer > 0.001f
				? std::clamp((outer - inner) / outer, 0.02f, 1.0f)
				: 0.0f;
			const float radius = std::clamp(source->radius * std::max(0.00001f, m_source_dlight_radius_scale) *
				runtime_owner_radius_scale(owner_info), 0.10f, runtime_owner_max_radius(owner_info));
			const float scalar = std::clamp(color_peak * std::max(0.0f, m_source_dlight_intensity_scale) *
				runtime_owner_intensity_scale(owner_info), 1.0f, 250000.0f);
			const std::string direction_source = axis.source;
			const std::string shape_reason = endpoint_hotspot ? "sphere: dynamic spotlight endpoint hotspot" :
				(shaped ? (strong_spot_evidence ? "spot: Source flags + coherent cone fields" :
					std::format("spot: temporal evidence {:.2f} ({}/{} frames)", shape_evidence,
						tracked.spot_evidence_frames, std::max(1, m_source_light_spot_confirm_frames))) :
					(!outer_valid ? "sphere: m_OuterAngle is zero/invalid" :
						(!inner_valid ? "sphere: incoherent inner/outer angles" :
							(!raw_direction_valid ? "sphere: invalid direction" :
								std::format("sphere: cone evidence {:.2f}, hysteresis hold {}/{}", shape_evidence,
									tracked.sphere_evidence_frames, std::max(1, m_source_light_sphere_confirm_frames))))));
			const float direction_confidence = axis.confidence;
			const float shape_confidence = endpoint_hotspot ? 1.0f :
				std::clamp(shaped ? 0.60f + shape_evidence * 0.40f : 1.0f - shape_evidence * 0.45f, 0.50f, 1.0f);
			const std::string parameter_sources = std::format("color=ColorRGBExp32; radius=dlight.radius; cone={}; style=dlight.style",
				outer_valid && inner_valid ? "m_InnerAngle/m_OuterAngle" : "sphere fallback");


			const std::string comment = entity_light
				? std::format("Map runtime elight: key={} ptr=0x{:08x}", source_key, pointer32)
				: std::format("Map runtime dlight: key={} ptr=0x{:08x}", source_key, pointer32);
			auto def = make_runtime_source_light_def(comment,
				entity_light ? "map_runtime_elight" : "map_runtime_dynamic",
				source->origin, radiance, scalar, radius, shaped, direction, outer, softness, 0.75f);
			tracked.seen = true;
			tracked.last_seen_time = curtime;
			tracked.last_enabled = true;
			tracked.entity_light = entity_light;
			tracked.transient = true;
			tracked.runtime_kind = entity_light ? "elight" : "dlight";
			tracked.runtime_classname = entity_light ? "source_elight" : "source_dlight";
			tracked.source_key = source_key;
			tracked.source_style = source->style;
			tracked.source_owner = owner_index;
			tracked.owner_category = owner_info.category_name;
			tracked.owner_classname = owner_info.classname;
			tracked.owner_model = owner_info.model;
			tracked.policy_allowed = policy_allowed;
			tracked.policy_reason = policy_reason;
			tracked.source_radius = source->radius;
			tracked.source_brightness = color_peak;
			tracked.source_outer_angle = outer;
			tracked.source_inner_angle = inner;
			tracked.source_near_z = 0.0f;
			tracked.source_far_z = source->radius;
			tracked.shadows = false;
			tracked.direction_source = direction_source;
			tracked.shape_reason = shape_reason;
			tracked.direction_confidence = direction_confidence;
			tracked.shape_confidence = shape_confidence;
			tracked.axis_disagreement_degrees = axis.disagreement_degrees;
			tracked.shape_evidence = shape_evidence;
			tracked.axis_candidates = axis.candidates;
			tracked.parameter_sources = parameter_sources;
			tracked.direction_corrected = axis.corrected;
			tracked.basis_recovered = axis.basis_recovered;
			tracked.duplicate_suppressed = false;
			tracked.matched_compiled_source = -1;
			const bool effective_enabled = policy_allowed;
			const std::uint64_t policy_signature = runtime_light_signature(def, effective_enabled);
			if (tracked.signature != policy_signature || tracked.def.comment != comment ||
				(effective_enabled && !remix_lights::get()->has_light_with_exact_comment(comment)))
			{
				if (!tracked.def.comment.empty() && tracked.def.comment != comment) {
					remix_lights::get()->upsert_runtime_light(tracked.def, false);
				}
				tracked.def = def;
				tracked.signature = policy_signature;
				if (remix_lights::get()->upsert_runtime_light(tracked.def, effective_enabled))
				{
					if (entity_light) ++m_source_elight_updates;
					else ++m_source_dlight_updates;
				}
			}

			if (entity_light) active_elight_keys.insert(tracking_key);
			else active_dlight_keys.insert(tracking_key);
			return true;
		};

		components::dlight_t* active[sdk::engine_effects::max_dynamic_lights] = {};
		const int count = std::clamp(intf->m_effects->get_active_dlights(active), 0, sdk::engine_effects::max_dynamic_lights);
		for (int i = 0; i < count; ++i) {
			process_source_light(active[i], false, active[i] ? active[i]->key : 0);
		}

		if (m_source_alloc_hooks_enabled)
		{
			for (auto it = g_captured_engine_lights.begin(); it != g_captured_engine_lights.end();)
			{
				auto& captured = it->second;
				const bool expired = captured.light && std::isfinite(captured.light->die) &&
					captured.light->die > 0.0f && captured.light->die + 0.001f < curtime;
				const bool submitted = !expired && process_source_light(captured.light, captured.entity_light, captured.key);
				const bool never_became_valid = !submitted && curtime - captured.allocated_at > 0.50f;
				if (expired || never_became_valid) it = g_captured_engine_lights.erase(it);
				else ++it;
			}
		}
		else {
			g_captured_engine_lights.clear();
		}

		for (auto it = m_runtime_dlights.begin(); it != m_runtime_dlights.end();)
		{
			if (!it->second.seen)
			{
				const float unseen_for = std::max(0.0f, curtime - it->second.last_seen_time);
				if (unseen_for <= std::max(0.0f, m_source_dlight_release_hold_seconds))
				{
					++m_source_dlight_release_holds;
					++it;
					continue;
				}
				remix_lights::get()->upsert_runtime_light(it->second.def, false);
				it = m_runtime_dlights.erase(it);
			}
			else ++it;
		}
		m_source_dlight_active = static_cast<std::uint32_t>(active_dlight_keys.size());
		m_source_elight_active = static_cast<std::uint32_t>(active_elight_keys.size());
	}

	void dynamic_lighting::update_projected_texture_entities()
	{
		m_source_projected_pose_smoothed = 0u;
		m_source_projected_target_updates = 0u;
		m_source_projected_missing_holds = 0u;
		for (auto& [index, light] : m_runtime_projected_lights) {
			light.seen = false;
		}

		const auto* intf = interfaces::get();
		if (!intf || !intf->m_entity_list || !remix_lights::get())
		{
			m_source_projected_active = 0u;
			m_source_entity_dynamic_active = 0u;
			return;
		}

		const float curtime = now();
		const float dt = 1.0f / std::clamp(m_map_light_runtime_update_hz, 1.0f, 120.0f);
		std::uint32_t projected_active = 0u;
		std::uint32_t dynamic_active = 0u;
		const int max_entities = intf->m_entity_list->get_max_entity();
		for (int index = 0; index < max_entities; ++index)
		{
			auto* entity = intf->m_entity_list->get_client_entity(index);
			if (!entity) continue;
			const auto* cls = entity->client_class();
			if (!cls) continue;

			const std::string class_name = runtime_entity_class_name(cls);
			const bool is_projected = class_name.find("projectedtexture") != std::string::npos;
			const bool is_point_spotlight = class_name.find("pointspotlight") != std::string::npos;
			const bool is_dynamic_light = class_name.find("dynamiclight") != std::string::npos ||
				class_name.find("lightdynamic") != std::string::npos;
			if (!is_projected && !is_point_spotlight && !is_dynamic_light) continue;

			bool enabled = runtime_light_entity_is_enabled(entity, cls);
			Vector origin = entity->get_absolute_origin();
			const std::uint32_t handle_raw = entity->get_ref_handle_raw();
			auto existing_track = m_runtime_projected_lights.find(index);
			const bool temporal_prior_valid = existing_track != m_runtime_projected_lights.end() &&
				existing_track->second.axis_initialized &&
				(existing_track->second.source_handle_raw == 0xffffffffu || handle_raw == 0xffffffffu ||
					existing_track->second.source_handle_raw == handle_raw);

			Vector angle_forward(1.0f, 0.0f, 0.0f);
			Vector engine_right(0.0f, -1.0f, 0.0f);
			Vector angle_up(0.0f, 0.0f, 1.0f);
			utils::vector::AngleVectors(entity->get_absolute_angles(), &angle_forward, &engine_right, &angle_up);
			Vector angle_right = engine_right * -1.0f; // Source local +right, matching rotate_source_local_vector().
			const bool angle_forward_valid = normalize_source_direction(angle_forward);
			const bool angle_right_valid = normalize_source_direction(angle_right);
			const bool angle_up_valid = normalize_source_direction(angle_up);

			bool camera_space = false;
			if (is_projected) read_entity_bool_prop(entity, cls, { "m_bCameraSpace" }, camera_space);
			components::CBaseHandle target_handle = {};
			bool target_prop_found = false;
			if (is_projected) target_prop_found = read_entity_prop(entity, cls, { "m_hTargetEntity" }, target_handle);
			else if (is_point_spotlight) target_prop_found = read_entity_prop(entity, cls, { "m_hSpotlightTarget", "m_hTargetEntity" }, target_handle);

			Vector target_direction(1.0f, 0.0f, 0.0f);
			bool target_direction_valid = false;
			float target_distance = 0.0f;
			if (target_prop_found && target_handle.m_Index != 0xffffffffu)
			{
				if (auto* target_raw = intf->m_entity_list->get_client_entity_from_handle(target_handle))
				{
					auto* target = reinterpret_cast<sdk::c_base_entity*>(target_raw);
					target_direction = target->get_absolute_origin() - origin;
					target_distance = target_direction.Length();
					target_direction_valid = normalize_source_direction(target_direction);
				}
			}

			float color_peak = 1.0f;
			Vector radiance(1.0f, 1.0f, 1.0f);
			std::string color_source = "packed render color";
			if (is_dynamic_light)
			{
				components::ColorRGBExp32 source_color = { 255u, 255u, 255u, 0 };
				if (read_entity_prop(entity, cls, { "m_LightColor" }, source_color))
				{
					radiance = decode_source_color(source_color, color_peak);
					color_source = "ColorRGBExp32 m_LightColor";
				}
			}
			else
			{
				std::uint32_t packed_color = 0xffffffffu;
				if (read_entity_prop(entity, cls, { "m_LightColor", "m_clrRender", "m_Color" }, packed_color))
				{
					radiance = unpack_color32(packed_color, color_peak);
					color_source = "packed m_LightColor/m_clrRender";
				}
			}

			float brightness = 1.0f;
			const bool brightness_read = read_entity_float_prop(entity, cls,
				{ "m_flBrightnessScale", "m_flBrightness" }, brightness);
			if (!brightness_read || !std::isfinite(brightness)) brightness = 1.0f;
			brightness = std::clamp(brightness, 0.0f, 16.0f);
			enabled = enabled && brightness > 0.001f && color_peak > 0.000001f;

			float beam_length = 0.0f;
			float beam_width = 0.0f;
			const bool beam_length_read = read_entity_float_prop(entity, cls,
				{ "m_flSpotlightMaxLength", "m_flSpotlightCurLength", "m_flSpotlightLength", "m_flFarZ" }, beam_length);
			const bool beam_width_read = read_entity_float_prop(entity, cls,
				{ "m_flSpotlightGoalWidth", "m_flSpotlightCurWidth", "m_flSpotlightWidth" }, beam_width);
			if ((!beam_length_read || beam_length <= 0.01f) && target_distance > 0.01f) beam_length = target_distance;

			float outer_angle = is_projected ? 55.0f : (is_point_spotlight ? 35.0f : 180.0f);
			bool outer_angle_read = false;
			std::string cone_source = "class default";
			if (is_projected) outer_angle_read = read_entity_float_prop(entity, cls, { "m_flLightFOV", "m_flFOV" }, outer_angle);
			else if (is_dynamic_light) outer_angle_read = read_entity_float_prop(entity, cls, { "m_OuterAngle", "m_flOuterAngle" }, outer_angle);
			else if (is_point_spotlight) outer_angle_read = read_entity_float_prop(entity, cls, { "m_flLightFOV", "m_flFOV" }, outer_angle);
			if (outer_angle_read) cone_source = is_dynamic_light ? "typed inner/outer angles" : "typed light FOV";
			const bool raw_outer_valid = std::isfinite(outer_angle) && outer_angle > 0.01f && outer_angle < 179.0f;
			if (m_source_light_parameter_sanity && (!outer_angle_read || !raw_outer_valid) &&
				is_point_spotlight && beam_length > 1.0f && beam_width > 0.01f)
			{
				constexpr float radians_to_degrees = 57.295779513082320876f;
				const float geometry_fov = 2.0f * std::atan((beam_width * 0.5f) / beam_length) * radians_to_degrees;
				if (std::isfinite(geometry_fov) && geometry_fov >= 1.0f && geometry_fov <= 140.0f)
				{
					outer_angle = geometry_fov;
					outer_angle_read = true;
					cone_source = "beam width/length geometry";
				}
			}
			if (!std::isfinite(outer_angle) || outer_angle <= 0.01f || outer_angle >= 179.0f)
			{
				outer_angle = is_dynamic_light ? 180.0f : (is_point_spotlight ? 35.0f : 55.0f);
				cone_source = "sanitized class default";
			}

			float inner_angle = outer_angle;
			const bool inner_angle_read = is_dynamic_light &&
				read_entity_float_prop(entity, cls, { "m_InnerAngle", "m_flInnerAngle" }, inner_angle);
			const bool dynamic_cone_fields_valid = is_dynamic_light && outer_angle_read && inner_angle_read &&
				outer_angle > 0.01f && outer_angle < 179.0f && inner_angle >= 0.0f && inner_angle <= outer_angle + 0.5f;
			const bool expected_shaped = is_projected || is_point_spotlight || dynamic_cone_fields_valid;
			const auto compiled_prior = find_compiled_axis_prior(origin, expected_shaped);

			const Vector previous_direction = temporal_prior_valid ? existing_track->second.last_resolved_direction : Vector(1.0f, 0.0f, 0.0f);
			const std::string previous_source = temporal_prior_valid ? existing_track->second.last_axis_source : std::string{};
			auto axis = m_source_light_axis_resolver
				? resolve_source_light_axis(is_projected, is_point_spotlight, is_dynamic_light, camera_space,
					angle_forward, angle_forward_valid, angle_right, angle_right_valid, angle_up, angle_up_valid,
					target_direction, target_direction_valid, compiled_prior,
					previous_direction, temporal_prior_valid, previous_source)
				: resolve_source_light_axis(is_projected, is_point_spotlight, is_dynamic_light, camera_space,
					angle_forward, angle_forward_valid, Vector(), false, Vector(), false,
					(is_projected && !camera_space) ? target_direction : Vector(),
					is_projected && !camera_space && target_direction_valid, compiled_axis_prior_s{},
					Vector(), false, {});
			Vector direction = axis.direction;
			if (axis.source == "angles forward") axis.source = "absolute Source angles (forward)";
			if (axis.source == "target entity" && is_projected) ++m_source_projected_target_updates;

			float source_radius = 0.0f;
			const bool radius_read = read_entity_float_prop(entity, cls,
				{ "m_Radius", "m_flRadius", "m_SpotRadius" }, source_radius);
			float source_exponent = is_projected ? 0.90f : 0.75f;
			const bool exponent_read = read_entity_float_prop(entity, cls,
				{ "m_flLightExponent", "m_flSpotlightExponent", "m_Exponent", "m_flExponent" }, source_exponent);
			if (!std::isfinite(source_exponent)) source_exponent = is_projected ? 0.90f : 0.75f;
			source_exponent = std::clamp(source_exponent, 0.0f, 16.0f);

			float shape_evidence = 0.0f;
			if (is_projected || is_point_spotlight) shape_evidence = 1.0f;
			else
			{
				if (outer_angle_read && outer_angle > 0.01f && outer_angle < 170.0f) shape_evidence += 0.34f;
				if (inner_angle_read && inner_angle >= 0.0f && inner_angle <= outer_angle + 0.5f) shape_evidence += 0.24f;
				if (axis.confidence >= 0.65f) shape_evidence += 0.18f;
				if (radius_read && source_radius > 0.01f) shape_evidence += 0.08f;
				if (exponent_read && source_exponent > 0.01f) shape_evidence += 0.08f;
				if (compiled_prior.valid) shape_evidence += 0.20f;
				shape_evidence = std::clamp(shape_evidence, 0.0f, 1.0f);
			}

			std::uint8_t shape_spot_frames = temporal_prior_valid ? existing_track->second.spot_evidence_frames : 0u;
			std::uint8_t shape_sphere_frames = temporal_prior_valid ? existing_track->second.sphere_evidence_frames : 0u;
			bool shape_initialized = temporal_prior_valid && existing_track->second.shape_initialized;
			bool stable_shaped = temporal_prior_valid && existing_track->second.stable_shaped;
			const bool shaped = resolve_source_spot_shape(shape_evidence, is_projected || is_point_spotlight, false,
				shape_spot_frames, shape_sphere_frames, shape_initialized, stable_shaped);
			const bool raw_spot_vote = is_projected || is_point_spotlight || shape_evidence >= 0.72f;
			if (shaped != raw_spot_vote) ++m_source_light_shape_holds;

			if (shaped)
			{
				outer_angle = std::clamp(outer_angle, 1.0f, 179.0f);
				if (is_dynamic_light && dynamic_cone_fields_valid) inner_angle = std::clamp(inner_angle, 0.0f, outer_angle);
				else inner_angle = outer_angle * (is_projected ? 0.84f : 0.78f);
			}
			else outer_angle = inner_angle = 180.0f;

			const std::string shape_reason = is_projected ? "spot: env_projectedtexture class + multi-source axis resolver" :
				(is_point_spotlight ? "spot: point_spotlight class + endpoint/WORLDLIGHT validation" :
					(shaped ? std::format("spot: dynamic-light evidence {:.2f} ({}/{} frames)", shape_evidence,
						shape_spot_frames, std::max(1, m_source_light_spot_confirm_frames)) :
						std::format("sphere: dynamic-light evidence {:.2f} ({}/{} release frames)", shape_evidence,
							shape_sphere_frames, std::max(1, m_source_light_sphere_confirm_frames))));
			const float shape_confidence = std::clamp(shaped ? 0.65f + shape_evidence * 0.35f :
				1.0f - shape_evidence * 0.45f, 0.50f, 1.0f);

			int source_style = 0;
			read_entity_prop(entity, cls, { "m_nLightStyle", "m_iLightStyle", "m_nStyle", "m_iStyle" }, source_style);
			float near_z = 0.0f;
			float far_z = source_radius;
			read_entity_float_prop(entity, cls, { "m_flNearZ", "m_flNearPlane" }, near_z);
			read_entity_float_prop(entity, cls, { "m_flFarZ", "m_flFarPlane" }, far_z);
			if (!std::isfinite(near_z) || near_z < 0.0f) near_z = 0.0f;
			if ((!std::isfinite(far_z) || far_z <= near_z) && beam_length > near_z) far_z = beam_length;
			if (!std::isfinite(far_z) || far_z <= near_z) far_z = std::max(source_radius, near_z + 1.0f);
			bool shadows = false;
			read_entity_bool_prop(entity, cls, { "m_bEnableShadows", "m_bShadowsEnabled" }, shadows);

			int source_owner = -1;
			components::CBaseHandle owner_handle = {};
			if (read_entity_prop(entity, cls, { "m_hOwnerEntity", "m_hMoveParent" }, owner_handle) &&
				owner_handle.m_Index != 0xffffffffu)
			{
				source_owner = static_cast<int>(owner_handle.m_Index & 0x0fffu);
			}

			const auto owner_info = classify_runtime_light_owner_with_proximity(source_owner > 0 ? source_owner : -1, origin,
				(is_projected || is_point_spotlight) ? 96.0f : 32.0f);
			std::string policy_reason = "accepted by Source runtime light policy";
			const bool class_allowed = source_runtime_class_allowed(false, is_dynamic_light, is_projected, is_point_spotlight, policy_reason);
			const bool owner_allowed = class_allowed && source_runtime_owner_allowed(owner_info, policy_reason);
			const bool policy_allowed = class_allowed && owner_allowed;
			count_runtime_policy_result(owner_info, policy_allowed);
			m_source_runtime_last_policy = std::format("{} {}: {}", is_dynamic_light ? "light_dynamic" : (is_point_spotlight ? "point_spotlight" : "projected"),
				owner_info.category_name, policy_reason);
			if (shaped && owner_info.category != runtime_light_owner_category_e::world)
			{
				outer_angle = std::min(outer_angle, std::clamp(m_source_runtime_character_spot_max_angle, 1.0f, 179.0f));
				inner_angle = std::min(inner_angle, outer_angle);
			}
			const float radius = source_radius > 1.0f
				? std::clamp(source_radius * std::max(0.00001f, m_source_dlight_radius_scale) * runtime_owner_radius_scale(owner_info),
					0.15f, runtime_owner_max_radius(owner_info))
				: std::min(runtime_owner_max_radius(owner_info), std::max(0.10f,
					(is_dynamic_light ? 1.0f : m_source_projected_radius) * runtime_owner_radius_scale(owner_info)));
			const float intensity_scale = is_dynamic_light ? m_source_dlight_intensity_scale : m_source_projected_intensity;
			const float scalar = std::clamp(color_peak * brightness * std::max(0.0f, intensity_scale) *
				runtime_owner_intensity_scale(owner_info), 1.0f, 250000.0f);
			const float softness = shaped && outer_angle > 0.001f
				? std::clamp((outer_angle - inner_angle) / outer_angle, 0.02f, 1.0f)
				: 0.0f;
			const std::string direction_source = axis.source;
			const float direction_confidence = axis.confidence;
			const std::string parameter_sources = std::format(
				"color={}; brightness={}; cone={}; length={}; width={}; radius={}; exponent={}",
				color_source, brightness_read ? "typed" : "default", cone_source,
				beam_length_read || target_distance > 0.01f ? "typed/target" : "none",
				beam_width_read ? "typed" : "none", radius_read ? "typed" : "default",
				exponent_read ? "typed" : "class default");


			auto [tracked_it, inserted_runtime_entity] = m_runtime_projected_lights.try_emplace(index);
			auto& tracked = tracked_it->second;
			if (inserted_runtime_entity)
			{
				m_map_light_config_dirty = true;
				m_map_light_config_save_at = curtime + 0.75f;
			}
			const bool generation_changed = tracked.source_handle_raw != 0xffffffffu && handle_raw != 0xffffffffu &&
				tracked.source_handle_raw != handle_raw;
			if (generation_changed)
			{
				if (!tracked.def.comment.empty()) remix_lights::get()->upsert_runtime_light(tracked.def, false);
				tracked = {};
				m_map_light_config_dirty = true;
				m_map_light_config_save_at = curtime + 0.75f;
				++m_source_projected_generation_resets;
			}
			tracked.source_handle_raw = handle_raw;
			tracked.spot_evidence_frames = shape_spot_frames;
			tracked.sphere_evidence_frames = shape_sphere_frames;
			tracked.shape_initialized = shape_initialized;
			tracked.stable_shaped = stable_shaped;
			if (axis.switched)
			{
				++tracked.axis_switches;
				++m_source_light_axis_switches;
				tracked.axis_stable_frames = 0u;
			}
			else ++tracked.axis_stable_frames;
			tracked.last_resolved_direction = axis.direction;
			tracked.last_axis_source = axis.source;
			tracked.axis_initialized = true;
			if (axis.corrected && (axis.switched || tracked.axis_stable_frames == 1u)) ++m_source_light_axis_corrections;
			if (axis.basis_recovered && (axis.switched || tracked.axis_stable_frames == 1u)) ++m_source_light_basis_recoveries;

			if (m_source_projected_pose_smoothing && !is_dynamic_light)
			{
				if (!tracked.pose_initialized)
				{
					tracked.smoothed_position = origin;
					tracked.smoothed_direction = direction;
					tracked.pose_initialized = true;
				}
				else
				{
					const float alpha = smoothing_alpha(dt, std::max(0.0f, m_source_projected_smoothing_seconds));
					tracked.smoothed_position += (origin - tracked.smoothed_position) * alpha;
					tracked.smoothed_direction = smooth_direction(tracked.smoothed_direction, direction, alpha);
					++m_source_projected_pose_smoothed;
				}
				origin = tracked.smoothed_position;
				direction = tracked.smoothed_direction;
			}
			else
			{
				tracked.smoothed_position = origin;
				tracked.smoothed_direction = direction;
				tracked.pose_initialized = true;
			}

			const std::string comment = is_dynamic_light
				? std::format("Map runtime entity light: {} {}", index, class_name)
				: std::format("Map runtime projected: {} {}", index, class_name);
			const std::string runtime_kind = is_dynamic_light ? "entity_dynamic" :
				(is_point_spotlight ? "point_spotlight" : "projected_texture");
			auto def = make_runtime_source_light_def(comment,
				is_dynamic_light ? "map_runtime_entity_dynamic" : "map_runtime_projected",
				origin, radiance, scalar, radius, shaped, direction, outer_angle, softness, source_exponent);
			tracked.runtime_entity_index = index;
			tracked.runtime_kind = runtime_kind;
			tracked.runtime_classname = class_name;
			tracked.transient = false;
			tracked.source_key = index;
			tracked.source_style = source_style;
			tracked.source_owner = source_owner;
			tracked.owner_category = owner_info.category_name;
			tracked.owner_classname = owner_info.classname;
			tracked.owner_model = owner_info.model;
			tracked.policy_allowed = policy_allowed;
			tracked.policy_reason = policy_reason;
			tracked.source_radius = source_radius;
			tracked.source_brightness = brightness;
			tracked.source_outer_angle = outer_angle;
			tracked.source_inner_angle = inner_angle;
			tracked.source_near_z = near_z;
			tracked.source_far_z = far_z;
			tracked.shadows = shadows;
			tracked.direction_source = direction_source;
			tracked.shape_reason = shape_reason;
			tracked.direction_confidence = direction_confidence;
			tracked.shape_confidence = shape_confidence;
			tracked.axis_disagreement_degrees = axis.disagreement_degrees;
			tracked.shape_evidence = shape_evidence;
			tracked.axis_candidates = axis.candidates;
			tracked.parameter_sources = parameter_sources;
			tracked.direction_corrected = axis.corrected;
			tracked.basis_recovered = axis.basis_recovered;
			tracked.matched_compiled_source = -1;
			tracked.duplicate_suppressed = false;
			bool authored_runtime_override = false;
			if (m_map_light_overrides_enabled)
			{
				if (const auto* override_data = source_map_light_overrides::find_runtime_entity(
					g_map_light_overrides, index, runtime_kind, class_name))
				{
					authored_runtime_override = !override_data->snapshot_only;
					apply_runtime_entity_override(def, enabled, *override_data);
				}
			}
			// Deduplicate the final editor/config-adjusted pose. An authored offset can therefore
			// intentionally separate a runtime source from its compiled WORLDLIGHT counterpart.
			// A full authored runtime override also explicitly wins over automatic deduplication.
			if (!authored_runtime_override && !def.points.empty())
			{
				const auto& final_point = def.points.front();
				tracked.matched_compiled_source = find_compiled_direct_light_duplicate(
					final_point.position, final_point.direction, final_point.use_shaping);
				tracked.duplicate_suppressed = tracked.matched_compiled_source >= 0;
			}
			const bool effective_enabled = enabled && policy_allowed && !tracked.duplicate_suppressed;
			if (tracked.duplicate_suppressed) ++m_source_direct_duplicates_suppressed;
			const std::uint64_t signature = runtime_light_signature(def, effective_enabled);
			tracked.seen = true;
			tracked.last_seen_time = curtime;
			tracked.last_enabled = enabled;
			tracked.entity_light = false;
			if (tracked.signature != signature || (effective_enabled && !remix_lights::get()->has_light_with_exact_comment(comment)))
			{
				if (!tracked.def.comment.empty() && tracked.def.comment != comment) {
					remix_lights::get()->upsert_runtime_light(tracked.def, false);
				}
				tracked.def = def;
				tracked.signature = signature;
				if (remix_lights::get()->upsert_runtime_light(tracked.def, effective_enabled))
				{
					if (is_dynamic_light) ++m_source_entity_dynamic_updates;
					else ++m_source_projected_updates;
				}
			}
			if (enabled)
			{
				if (is_dynamic_light) ++dynamic_active;
				else ++projected_active;
			}
		}

		for (auto it = m_runtime_projected_lights.begin(); it != m_runtime_projected_lights.end();)
		{
			if (!it->second.seen)
			{
				const float missing_for = std::max(0.0f, curtime - it->second.last_seen_time);
				if (m_source_projected_hold_missing && it->second.last_enabled &&
					missing_for <= std::max(0.0f, m_source_projected_missing_hold_seconds))
				{
					++m_source_projected_missing_holds;
					++it;
					continue;
				}
				remix_lights::get()->upsert_runtime_light(it->second.def, false);
				it = m_runtime_projected_lights.erase(it);
			}
			else ++it;
		}
		m_source_projected_active = projected_active;
		m_source_entity_dynamic_active = dynamic_active;
	}

	bool dynamic_lighting::schedule_map_io_action(const std::uint32_t source_index, const std::uint32_t group_id,
		const std::size_t graph_action_index, const float execute_time, const float authored_delay, const bool manual,
		const bool runtime_captured)
	{
		if (source_index == 0u || graph_action_index >= g_map_entity_graph.resolved_light_actions.size()) return false;
		const auto& action = g_map_entity_graph.resolved_light_actions[graph_action_index];
		const int limit = std::clamp(m_map_light_io_max_pending_actions, 16, 4096);
		const float epsilon = std::max(0.0f, m_map_light_io_scheduler_epsilon_seconds);
		for (const auto& pending : m_map_light_io_pending_actions)
		{
			if (pending.source_index == source_index && pending.control_group == group_id &&
				pending.root_action_id == action.root_action_id && pending.kind == action.kind &&
				std::abs(pending.execute_time - execute_time) <= epsilon)
			{
				++m_map_light_io_actions_coalesced;
				return true;
			}
		}
		if (static_cast<int>(m_map_light_io_pending_actions.size()) >= limit)
		{
			++m_map_light_io_actions_dropped;
			return false;
		}

		map_light_io_scheduled_action_s pending = {};
		pending.execute_time = execute_time;
		pending.authored_delay = authored_delay;
		pending.serial = m_map_light_io_next_serial++;
		pending.source_index = source_index;
		pending.control_group = group_id;
		pending.root_action_id = action.root_action_id;
		pending.graph_action_index = graph_action_index;
		pending.kind = action.kind;
		pending.path = action.path;
		pending.manual = manual;
		pending.runtime_captured = runtime_captured;
		m_map_light_io_pending_actions.emplace_back(std::move(pending));
		m_map_light_io_pending_count = static_cast<std::uint32_t>(m_map_light_io_pending_actions.size());
		++m_map_light_io_actions_scheduled;
		return true;
	}

	void dynamic_lighting::seed_map_io_group_state(const std::uint32_t group_id, const bool enabled, const float curtime)
	{
		if (group_id == 0u) return;
		for (auto& binding : m_owned_worldlight_bindings)
		{
			if (binding.io_control_group != group_id || binding.io_scheduled_terminal) continue;
			binding.io_scheduled_state_valid = true;
			binding.io_scheduled_enabled = enabled;
			binding.io_last_action_time = curtime;
			binding.io_scheduled_reason = std::format("map I/O group {} initial {}", group_id,
				enabled ? "enabled" : "disabled");
		}
	}

	void dynamic_lighting::schedule_map_io_group_transition(const std::uint32_t group_id,
		const bool old_enabled, const bool new_enabled, const float curtime, const bool manual)
	{
		if (group_id == 0u || old_enabled == new_enabled || !m_map_light_io_action_scheduler)
		{
			if (group_id > 0u && old_enabled != new_enabled) seed_map_io_group_state(group_id, new_enabled, curtime);
			return;
		}
		const auto* action_indices = source_map_entities::find_control_group_actions(g_map_entity_graph, group_id);
		if (!action_indices || action_indices->empty())
		{
			seed_map_io_group_state(group_id, new_enabled, curtime);
			return;
		}

		struct root_family_s
		{
			std::uint32_t root_action_id = 0u;
			std::vector<std::size_t> actions;
			std::unordered_set<std::uint32_t> targets;
			std::uint32_t direct_count = 0u;
			std::uint32_t toggle_count = 0u;
			float min_delay = std::numeric_limits<float>::max();
			std::int32_t max_fires = -1;
		};
		std::unordered_map<std::uint32_t, root_family_s> families;
		for (const auto action_index : *action_indices)
		{
			if (action_index >= g_map_entity_graph.resolved_light_actions.size()) continue;
			const auto& action = g_map_entity_graph.resolved_light_actions[action_index];
			const bool direct = new_enabled
				? action.kind == source_map_entities::io_action_kind::enable
				: (action.kind == source_map_entities::io_action_kind::disable ||
					action.kind == source_map_entities::io_action_kind::kill);
			const bool toggle = action.kind == source_map_entities::io_action_kind::toggle;
			if (!direct && !toggle) continue;
			auto& family = families[action.root_action_id];
			family.root_action_id = action.root_action_id;
			family.actions.push_back(action_index);
			if (direct) ++family.direct_count;
			else ++family.toggle_count;
			family.min_delay = std::min(family.min_delay, std::max(0.0f, action.delay));
			family.max_fires = action.max_fires;
			for (const auto target : action.resolved_target_source_indices) family.targets.insert(target);
		}

		const root_family_s* best_family = nullptr;
		float best_score = -1.0e9f;
		for (const auto& [root_id, family] : families)
		{
			(void)root_id;
			const float score = static_cast<float>(family.direct_count) * 1000.0f +
				static_cast<float>(family.targets.size()) * 20.0f +
				static_cast<float>(family.toggle_count) * 2.0f -
				(std::isfinite(family.min_delay) ? family.min_delay : 0.0f);
			if (!best_family || score > best_score)
			{
				best_family = &family;
				best_score = score;
			}
		}
		if (!best_family)
		{
			seed_map_io_group_state(group_id, new_enabled, curtime);
			return;
		}

		if (m_map_light_io_honor_max_fires && best_family->max_fires > 0)
		{
			const auto fired = m_map_light_io_root_fire_counts[best_family->root_action_id];
			if (fired >= static_cast<std::uint32_t>(best_family->max_fires))
			{
				++m_map_light_io_actions_maxfires_blocked;
				return;
			}
		}

		std::unordered_map<std::uint32_t, std::size_t> selected_by_target;
		for (const auto action_index : best_family->actions)
		{
			const auto& action = g_map_entity_graph.resolved_light_actions[action_index];
			for (const auto target : action.resolved_target_source_indices)
			{
				const auto found = selected_by_target.find(target);
				if (found == selected_by_target.end())
				{
					selected_by_target.emplace(target, action_index);
					continue;
				}
				const auto& previous = g_map_entity_graph.resolved_light_actions[found->second];
				const bool current_direct = action.kind != source_map_entities::io_action_kind::toggle;
				const bool previous_direct = previous.kind != source_map_entities::io_action_kind::toggle;
				if ((current_direct && !previous_direct) ||
					(current_direct == previous_direct && action.delay < previous.delay))
				{
					found->second = action_index;
				}
			}
		}
		if (selected_by_target.empty())
		{
			seed_map_io_group_state(group_id, new_enabled, curtime);
			return;
		}

		float base_delay = std::numeric_limits<float>::max();
		for (const auto& [target, action_index] : selected_by_target)
		{
			(void)target;
			const auto& action = g_map_entity_graph.resolved_light_actions[action_index];
			base_delay = std::min(base_delay, std::max(0.0f, action.delay));
		}
		if (!std::isfinite(base_delay)) base_delay = 0.0f;

		bool scheduled_any = false;
		for (const auto& [target, action_index] : selected_by_target)
		{
			const auto& action = g_map_entity_graph.resolved_light_actions[action_index];
			const float authored_delay = std::max(0.0f, action.delay);
			const float relative_delay = m_map_light_io_relative_delays
				? std::max(0.0f, authored_delay - base_delay)
				: authored_delay;
			scheduled_any |= schedule_map_io_action(target, group_id, action_index,
				curtime + relative_delay, authored_delay, manual);
		}
		if (scheduled_any)
		{
			++m_map_light_io_root_fire_counts[best_family->root_action_id];
		}
	}


	bool dynamic_lighting::apply_direct_server_input(const std::uint32_t map_source_index,
		const source_map_entities::io_action_kind kind, const float curtime, const std::string_view reason)
	{
		if (map_source_index == 0u || kind == source_map_entities::io_action_kind::unknown) return false;
		bool default_enabled = true;
		if (static_cast<std::size_t>(map_source_index) <= g_map_entity_graph.entities.size())
		{
			default_enabled = !g_map_entity_graph.entities[static_cast<std::size_t>(map_source_index - 1u)].starts_disabled;
		}

		auto& exact = m_map_light_io_exact_states[map_source_index];
		const bool had_state = exact.serial != 0u;
		const bool old_enabled = had_state ? exact.enabled : default_enabled;
		if (exact.terminal && kind != source_map_entities::io_action_kind::kill) return false;

		bool new_enabled = old_enabled;
		bool terminal = exact.terminal;
		switch (kind)
		{
		case source_map_entities::io_action_kind::enable:
			new_enabled = true;
			break;
		case source_map_entities::io_action_kind::disable:
			new_enabled = false;
			break;
		case source_map_entities::io_action_kind::toggle:
			new_enabled = !old_enabled;
			++m_map_light_io_actions_toggle;
			break;
		case source_map_entities::io_action_kind::kill:
			new_enabled = false;
			terminal = true;
			++m_map_light_io_actions_kill;
			break;
		default:
			return false;
		}

		exact.enabled = new_enabled;
		exact.terminal = terminal;
		exact.changed_at = curtime;
		exact.serial = m_map_light_io_next_serial++;
		exact.reason = std::string(reason);

		const float epsilon = std::max(0.0f, m_map_light_io_scheduler_epsilon_seconds);
		const auto old_pending_size = m_map_light_io_pending_actions.size();
		std::erase_if(m_map_light_io_pending_actions, [&](const map_light_io_scheduled_action_s& pending)
		{
			return pending.runtime_captured && pending.source_index == map_source_index &&
				pending.kind == kind && pending.execute_time <= curtime + epsilon;
		});
		if (m_map_light_io_pending_actions.size() != old_pending_size)
		{
			m_map_light_io_actions_coalesced += static_cast<std::uint32_t>(
				old_pending_size - m_map_light_io_pending_actions.size());
			m_map_light_io_pending_count = static_cast<std::uint32_t>(m_map_light_io_pending_actions.size());
		}

		bool applied = false;
		for (auto& binding : m_owned_worldlight_bindings)
		{
			if (binding.map_light_source_index != static_cast<int>(map_source_index)) continue;
			if (binding.io_scheduled_terminal && kind != source_map_entities::io_action_kind::kill) continue;
			const bool binding_old = binding.io_scheduled_state_valid
				? binding.io_scheduled_enabled : !binding.map_light_starts_disabled;
			binding.io_scheduled_state_valid = true;
			binding.io_scheduled_enabled = new_enabled;
			binding.io_scheduled_terminal = terminal;
			if (terminal) binding.io_group_terminal = true;
			binding.io_last_action_time = curtime;
			binding.io_last_action_serial = exact.serial;
			binding.io_scheduled_reason = exact.reason;
			applied = true;

			if (binding_old != new_enabled || terminal)
			{
				if (m_map_light_transition_log.size() >= 96u)
				{
					m_map_light_transition_log.pop_front();
					++m_map_light_transition_log_dropped;
				}
				m_map_light_transition_log.push_back({
					curtime,
					map_source_index,
					binding.io_control_group,
					binding_old ? "enabled" : "disabled",
					std::format("{}: {}", new_enabled ? "enabled" : "disabled", exact.reason)
				});
			}
		}
		++m_server_accept_input_direct_actions;
		return applied || !had_state || old_enabled != new_enabled || terminal;
	}

	void dynamic_lighting::process_server_accept_input_events(const float curtime)
	{
		if (!m_server_accept_input_capture_enabled)
		{
			std::scoped_lock lock(g_server_accept_input_mutex);
			g_server_accept_input_events.clear();
			return;
		}
		std::deque<captured_server_input_s> events;
		{
			std::scoped_lock lock(g_server_accept_input_mutex);
			events.swap(g_server_accept_input_events);
		}
		if (events.empty()) return;

		auto resolve_map_entity = [](const captured_server_input_s& event) -> const source_map_entities::entity_s*
		{
			if (event.hammer_id >= 0)
			{
				if (const auto* by_hammer = source_map_entities::find_by_hammer_id(g_map_entity_graph, event.hammer_id)) {
					return by_hammer;
				}
			}
			if (!event.targetname.empty())
			{
				if (const auto* by_name = source_map_entities::find_by_targetname(g_map_entity_graph, event.targetname)) {
					return by_name;
				}
			}
			const source_map_entities::entity_s* unique = nullptr;
			for (const auto& entity : g_map_entity_graph.entities)
			{
				if (!event.classname.empty() && !source_map_entities::runtime_class_matches_map_class(
					event.classname, entity.classname)) continue;
				if (!(entity.is_light || entity.is_bindable_owner || !entity.outputs.empty())) continue;
				if (unique) return nullptr;
				unique = &entity;
			}
			return unique;
		};

		for (const auto& event : events)
		{
			const float event_time = event.captured_at > 0.0f ? event.captured_at : curtime;
			const auto* map_entity = resolve_map_entity(event);
			if (!map_entity)
			{
				++m_server_accept_input_unresolved;
				m_server_accept_input_last_event = std::format("{} {} target={} hammer={} (unresolved)",
					event.classname, event.input, event.targetname, event.hammer_id);
				continue;
			}

			bool handled = false;
			const auto direct_kind = source_map_entities::classify_io_input(event.input);
			if (direct_kind != source_map_entities::io_action_kind::unknown && map_entity->is_light)
			{
				handled |= apply_direct_server_input(map_entity->source_index, direct_kind, event_time,
					std::format("captured AcceptInput {} on {}",
						source_map_entities::io_action_kind_name(direct_kind),
						map_entity->targetname.empty() ? map_entity->classname : map_entity->targetname));
			}

			if (direct_kind == source_map_entities::io_action_kind::kill && map_entity->is_bindable_owner)
			{
				for (auto& binding : m_owned_worldlight_bindings)
				{
					if (binding.map_owner_source_index != static_cast<int>(map_entity->source_index)) continue;
					binding.owner_destroyed = true;
					binding.io_scheduled_state_valid = true;
					binding.io_scheduled_enabled = false;
					binding.io_scheduled_terminal = true;
					binding.io_group_terminal = true;
					binding.owner_state_reason = "captured owner Kill/Break input";
					binding.io_scheduled_reason = binding.owner_state_reason;
					binding.io_last_action_time = event_time;
					binding.io_last_action_serial = m_map_light_io_next_serial++;
					handled = true;
				}
			}

			const auto routed_outputs = source_map_entities::routed_outputs_for_input(map_entity->classname, event.input);
			std::unordered_set<std::string> output_set(routed_outputs.begin(), routed_outputs.end());
			std::unordered_set<std::uint32_t> scheduled_roots;
			std::unordered_set<std::uint32_t> blocked_roots;
			for (std::size_t action_index = 0u;
				action_index < g_map_entity_graph.resolved_light_actions.size(); ++action_index)
			{
				const auto& action = g_map_entity_graph.resolved_light_actions[action_index];
				if (action.root_source_entity_index != map_entity->source_index) continue;
				if (!output_set.empty() && !output_set.contains(action.root_output_name)) continue;
				if (output_set.empty()) continue;

				if (m_map_light_io_honor_max_fires && action.max_fires > 0)
				{
					const auto fired = m_map_light_io_root_fire_counts[action.root_action_id];
					if (fired >= static_cast<std::uint32_t>(action.max_fires))
					{
						if (blocked_roots.insert(action.root_action_id).second) {
							++m_map_light_io_actions_maxfires_blocked;
						}
						continue;
					}
				}

				for (const auto target_source : action.resolved_target_source_indices)
				{
					const auto group_id = source_map_entities::find_light_control_group(
						g_map_entity_graph, target_source);
					if (schedule_map_io_action(target_source, group_id, action_index,
						event_time + std::max(0.0f, action.delay),
						std::max(0.0f, action.delay), false, true))
					{
						handled = true;
						scheduled_roots.insert(action.root_action_id);
						++m_server_accept_input_routed_actions;
					}
				}
			}
			for (const auto root_id : scheduled_roots) ++m_map_light_io_root_fire_counts[root_id];

			m_server_accept_input_last_event = std::format("{} {} target={} hammer={} -> {}",
				event.classname, event.input,
				event.targetname.empty() ? "-" : event.targetname,
				event.hammer_id, handled ? "handled" : "no mapped light action");
			if (!handled) ++m_server_accept_input_unresolved;
		}
	}


	void dynamic_lighting::process_scheduled_map_io_actions(const float curtime)
	{
		if (!m_map_light_io_action_scheduler)
		{
			m_map_light_io_pending_actions.clear();
			m_map_light_io_pending_count = 0u;
			return;
		}
		if (m_map_light_io_pending_actions.empty()) return;
		std::stable_sort(m_map_light_io_pending_actions.begin(), m_map_light_io_pending_actions.end(),
			[](const auto& lhs, const auto& rhs)
			{
				if (lhs.execute_time != rhs.execute_time) return lhs.execute_time < rhs.execute_time;
				return lhs.serial < rhs.serial;
			});

		std::size_t processed = 0u;
		while (processed < m_map_light_io_pending_actions.size())
		{
			const auto pending = m_map_light_io_pending_actions[processed];
			if (pending.execute_time > curtime + std::max(0.0f, m_map_light_io_scheduler_epsilon_seconds)) break;
			bool applied = false;
			for (auto& binding : m_owned_worldlight_bindings)
			{
				if (binding.map_light_source_index != static_cast<int>(pending.source_index)) continue;
				if (binding.io_scheduled_terminal && pending.kind != source_map_entities::io_action_kind::kill) continue;

				const bool old_enabled = binding.io_scheduled_state_valid
					? binding.io_scheduled_enabled
					: !binding.map_light_starts_disabled;
				bool new_enabled = old_enabled;
				switch (pending.kind)
				{
				case source_map_entities::io_action_kind::enable:
					new_enabled = true;
					break;
				case source_map_entities::io_action_kind::disable:
					new_enabled = false;
					break;
				case source_map_entities::io_action_kind::toggle:
					new_enabled = m_map_light_io_toggle_exact ? !old_enabled :
						(binding.io_control_group > 0u && m_map_light_io_group_states.contains(binding.io_control_group)
							? m_map_light_io_group_states[binding.io_control_group].enabled : !old_enabled);
					++m_map_light_io_actions_toggle;
					break;
				case source_map_entities::io_action_kind::kill:
					new_enabled = false;
					binding.io_scheduled_terminal = true;
					binding.io_group_terminal = true;
					++m_map_light_io_actions_kill;
					break;
				default:
					continue;
				}

				binding.io_scheduled_state_valid = true;
				binding.io_scheduled_enabled = new_enabled;
				binding.io_last_action_time = curtime;
				binding.io_last_action_serial = pending.serial;
				binding.io_scheduled_reason = std::format("{} {} via {} (authored {:.2f}s)",
					pending.runtime_captured ? "captured runtime" : "scheduled",
					source_map_entities::io_action_kind_name(pending.kind),
					pending.path.empty() ? "resolved map I/O" : pending.path, pending.authored_delay);
				if (pending.manual) ++m_map_light_io_actions_manual;
				applied = true;

				if (old_enabled != new_enabled || pending.kind == source_map_entities::io_action_kind::kill)
				{
					if (m_map_light_transition_log.size() >= 96u)
					{
						m_map_light_transition_log.pop_front();
						++m_map_light_transition_log_dropped;
					}
					m_map_light_transition_log.push_back({ curtime, binding.source_index,
						binding.io_control_group,
						std::format("scheduled state {}", old_enabled ? "enabled" : "disabled"),
						binding.io_scheduled_reason });
					m_map_light_last_transition = std::format("WORLDLIGHT #{}: {}",
						binding.source_index, binding.io_scheduled_reason);
					++m_map_light_reason_transitions;
				}
			}
			if (applied) ++m_map_light_io_actions_executed;
			else ++m_map_light_io_actions_dropped;
			++processed;
		}
		if (processed > 0u)
		{
			m_map_light_io_pending_actions.erase(m_map_light_io_pending_actions.begin(),
				m_map_light_io_pending_actions.begin() + static_cast<std::ptrdiff_t>(processed));
		}
		m_map_light_io_pending_count = static_cast<std::uint32_t>(m_map_light_io_pending_actions.size());
	}

	void dynamic_lighting::update_owned_worldlights()
	{
		m_owned_worldlights_active = 0u;
		m_owned_worldlights_hidden = 0u;
		m_owned_worldlights_unresolved = 0u;
		m_map_light_model_rejects = 0u;
		m_map_light_duplicate_owner_rejects = 0u;
		m_map_light_attachment_updates = 0u;
		m_map_light_destroyed_by_health = 0u;
		m_map_light_destroyed_by_removal = 0u;
		m_map_light_dormant_retained = 0u;
		m_map_light_owner_rebinds = 0u;
		m_map_light_rebind_deferred = 0u;
		m_map_light_attachment_fallbacks = 0u;
		m_map_light_attachment_cache_hits = 0u;
		m_map_light_handle_generation_rejects = 0u;
		m_map_light_predicted_rebinds = 0u;
		m_map_runtime_light_state_active = 0u;
		m_map_runtime_light_state_hidden = 0u;
		m_map_runtime_light_state_unresolved = 0u;
		m_map_runtime_light_state_updates = 0u;
		m_map_runtime_light_state_pending = 0u;
		m_map_runtime_light_state_missing_held = 0u;
		m_map_light_io_group_observations = 0u;
		m_map_light_io_group_fallbacks = 0u;
		if (!remix_lights::get()) return;

		const float curtime = now();
		if (m_map_event_lights_enabled)
		{
			if (m_map_event_accept_input && m_server_accept_input_capture_enabled) process_server_accept_input_events(curtime);
			if (m_map_light_io_action_scheduler) process_scheduled_map_io_actions(curtime);
		}
		struct frame_group_observation_s
		{
			std::uint32_t enabled_votes = 0u;
			std::uint32_t disabled_votes = 0u;
			std::uint32_t terminal_votes = 0u;
		};
		std::unordered_map<std::uint32_t, frame_group_observation_s> frame_group_observations;
		std::unordered_map<int, int> resolved_map_owners;
		std::unordered_set<int> claimed_runtime_indices;
		std::unordered_map<int, int> resolved_map_lights;
		std::unordered_set<int> claimed_runtime_light_indices;
		for (auto& binding : m_owned_worldlight_bindings)
		{
			auto def = binding.def;
			bool owner_target_enabled = true;
			binding.owner_state_reason = "not owner-bound";
			binding.runtime_light_state_reason = binding.runtime_light_trackable ? "runtime light unresolved" : "not runtime-tracked";
			const bool wants_owner = m_bsp_worldlight_bind_owners && (binding.owner_index > 0 || binding.graph_bound);
			if (wants_owner) binding.owner_state_reason = "owner unresolved";

			if (wants_owner)
			{
				Vector owner_origin(0.0f, 0.0f, 0.0f);
				Vector owner_angles(0.0f, 0.0f, 0.0f);
				sdk::c_base_entity* owner_entity = nullptr;
				const sdk::c_client_class* owner_class = nullptr;
				bool owner_found = false;
				bool shared_owner_cache = false;
				bool resolved_this_update = false;

				if (binding.graph_bound && !(binding.owner_destroyed && m_bsp_worldlight_destroy_with_owner))
				{
					if (const auto cached = resolved_map_owners.find(binding.map_owner_source_index);
						cached != resolved_map_owners.end())
					{
						binding.runtime_owner_index = cached->second;
						owner_found = get_runtime_entity_basic(binding.runtime_owner_index, owner_entity, owner_class);
						shared_owner_cache = owner_found;
						resolved_this_update = owner_found;
					}

					if (!owner_found && binding.runtime_owner_index > 0)
					{
						const bool claimed_by_other_owner = m_map_light_unique_owner_claims &&
							claimed_runtime_indices.contains(binding.runtime_owner_index) && !shared_owner_cache;
						if (claimed_by_other_owner)
						{
							++m_map_light_duplicate_owner_rejects;
						}
						else if (get_runtime_entity_basic(binding.runtime_owner_index, owner_entity, owner_class))
						{
							const auto runtime_class = runtime_entity_class_name(owner_class);
							const bool class_matches = source_map_entities::runtime_class_matches_map_class(runtime_class, binding.owner_class_hint);
							const bool model_matches = !m_map_light_model_aware_binding || runtime_model_matches(binding.owner_model_hint,
								runtime_entity_model_name(owner_entity));
							owner_found = class_matches && model_matches;
						}
					}

					if (!owner_found)
					{
						const int previous_index = binding.runtime_owner_index;
						binding.runtime_owner_index = -1;
						owner_entity = nullptr;
						owner_class = nullptr;
						const float missing_for = binding.owner_resolved_once
							? std::max(0.0f, curtime - binding.owner_last_seen_time)
							: FLT_MAX;
						const bool breakable_is_terminal = binding.owner_resolved_once && binding.owner_breakable &&
							m_map_light_destroy_missing_breakables && m_bsp_worldlight_destroy_with_owner;
						const bool removed_prop_is_terminal = binding.owner_resolved_once && m_map_light_destroy_removed_props &&
							owner_class_is_terminal_when_removed(binding.owner_class_hint) && m_bsp_worldlight_destroy_with_owner;
						const bool removal_is_terminal = breakable_is_terminal || removed_prop_is_terminal;
						const bool rebind_timeout_elapsed = !binding.owner_resolved_once ||
							missing_for >= std::max(0.0f, m_map_light_owner_rebind_timeout);
						const bool rebind_poll_elapsed = curtime - binding.owner_last_rebind_attempt_time >=
							std::max(0.05f, m_map_light_owner_rebind_poll_seconds);
						const bool can_resolve = !removal_is_terminal && rebind_timeout_elapsed && rebind_poll_elapsed;

						if (can_resolve)
						{
							binding.owner_last_rebind_attempt_time = curtime;
							int resolved_index = -1;
							bool resolved = false;
							if (m_map_light_rebind_from_last_pose && binding.last_owner_pose_valid)
							{
								resolved = find_runtime_entity_for_map_owner(binding.owner_class_hint, binding.owner_model_hint,
									binding.last_owner_origin, m_map_light_owner_resolve_distance,
									m_map_light_unique_owner_claims ? &claimed_runtime_indices : nullptr,
									resolved_index, owner_entity, owner_class);
								if (resolved) ++m_map_light_predicted_rebinds;
							}
							if (!resolved)
							{
								resolved = find_runtime_entity_for_map_owner(binding.owner_class_hint, binding.owner_model_hint,
									binding.map_owner_origin, m_map_light_owner_resolve_distance,
									m_map_light_unique_owner_claims ? &claimed_runtime_indices : nullptr,
									resolved_index, owner_entity, owner_class);
							}
							if (resolved)
							{
								binding.runtime_owner_index = resolved_index;
								owner_found = true;
								resolved_this_update = true;
								if (binding.owner_resolved_once && previous_index != resolved_index) ++m_map_light_owner_rebinds;
							}
						}
						else if (binding.owner_resolved_once && !removal_is_terminal)
						{
							++m_map_light_rebind_deferred;
						}
					}

					if (owner_found)
					{
						resolved_map_owners[binding.map_owner_source_index] = binding.runtime_owner_index;
						claimed_runtime_indices.insert(binding.runtime_owner_index);
					}
				}
				else if (binding.owner_index > 0 && !(binding.owner_destroyed && m_bsp_worldlight_destroy_with_owner))
				{
					owner_found = get_runtime_entity_basic(binding.owner_index, owner_entity, owner_class);
				}

				if (owner_found && owner_entity && owner_class)
				{
					const std::uint32_t handle_raw = owner_entity->get_ref_handle_raw();
					const bool handle_valid = handle_raw != 0xffffffffu;
					const bool generation_changed = binding.owner_resolved_once && handle_valid &&
						binding.owner_handle_raw != 0xffffffffu && binding.owner_handle_raw != handle_raw;
					if (generation_changed && (binding.owner_breakable || !resolved_this_update))
					{
						++m_map_light_handle_generation_rejects;
						if (binding.owner_breakable && m_bsp_worldlight_destroy_with_owner) binding.owner_destroyed = true;
						owner_found = false;
						binding.runtime_owner_index = -1;
					}
				}

				if (owner_found && owner_entity && owner_class)
				{
					const auto entity_identity = reinterpret_cast<std::uintptr_t>(owner_entity);
					const auto class_identity = reinterpret_cast<std::uintptr_t>(owner_class);
					const std::uint32_t handle_raw = owner_entity->get_ref_handle_raw();
					if (!binding.owner_resolved_once)
					{
						binding.owner_entity_identity = entity_identity;
						binding.owner_class_identity = class_identity;
						binding.owner_handle_raw = handle_raw;
						binding.owner_resolved_once = true;
						binding.owner_destroyed = false;
					}
					else if (binding.owner_entity_identity != entity_identity || binding.owner_class_identity != class_identity)
					{
						if (binding.graph_bound && !binding.owner_breakable)
						{
							binding.owner_entity_identity = entity_identity;
							binding.owner_class_identity = class_identity;
							binding.owner_handle_raw = handle_raw;
							binding.attachment_local_valid = false;
							binding.attachment_cache_valid = false;
							binding.extrusion_applied = false;
							++m_map_light_owner_rebinds;
						}
						else binding.owner_destroyed = true;
					}

					const auto owner_state = evaluate_runtime_owner_state(owner_entity, owner_class,
						binding.owner_breakable, binding.owner_initial_health,
						binding.owner_health_seen_positive, binding.owner_last_health);
					if (owner_state != runtime_owner_state_e::missing)
					{
						binding.owner_last_seen_time = curtime;
						binding.owner_missing_updates = 0u;
						if (owner_state != runtime_owner_state_e::dormant)
						{
							const Vector pose_origin = owner_entity->get_absolute_origin();
							const Vector pose_angles = owner_entity->get_absolute_angles();
							if (std::isfinite(pose_origin.x) && std::isfinite(pose_origin.y) && std::isfinite(pose_origin.z))
							{
								binding.last_owner_origin = pose_origin;
								binding.last_owner_angles = pose_angles;
								binding.last_owner_pose_valid = true;
							}
						}
					}

					switch (owner_state)
					{
					case runtime_owner_state_e::active:
						owner_target_enabled = !binding.owner_destroyed;
						binding.owner_state_reason = binding.owner_destroyed ? "owner destroyed" : "owner active";
						break;
					case runtime_owner_state_e::dormant:
						owner_target_enabled = m_map_light_keep_dormant_owners && !binding.owner_destroyed;
						binding.owner_state_reason = owner_target_enabled ? "owner dormant, cached pose retained" : "owner dormant";
						if (owner_target_enabled) ++m_map_light_dormant_retained;
						break;
					case runtime_owner_state_e::hidden:
						owner_target_enabled = false;
						binding.owner_state_reason = "owner hidden or disabled";
						break;
					case runtime_owner_state_e::destroyed:
						owner_target_enabled = false;
						binding.owner_state_reason = "owner broken or health depleted";
						if (!binding.owner_destroyed) ++m_map_light_destroyed_by_health;
						binding.owner_destroyed = true;
						break;
					default:
						owner_target_enabled = false;
						binding.owner_state_reason = "owner missing";
						break;
					}

					if (!binding.owner_destroyed && owner_state != runtime_owner_state_e::dormant &&
						!def.points.empty() && binding.local_transform_valid)
					{
						owner_origin = owner_entity->get_absolute_origin();
						owner_angles = owner_entity->get_absolute_angles();
						bool attachment_transform = false;
						Vector transform_local_origin = binding.local_origin;
						Vector transform_local_direction = binding.local_direction;
						if (m_map_light_follow_parent_attachments && !binding.owner_attachment.empty())
						{
							Vector attachment_origin = owner_origin;
							Vector attachment_angles = owner_angles;
							attachment_transform = get_runtime_attachment_transform(owner_entity, binding.owner_attachment,
								attachment_origin, attachment_angles);
							if (attachment_transform)
							{
								owner_origin = attachment_origin;
								owner_angles = attachment_angles;
								binding.cached_attachment_origin = attachment_origin;
								binding.cached_attachment_angles = attachment_angles;
								binding.attachment_last_seen_time = curtime;
								binding.attachment_cache_valid = true;
							}
							else if (m_map_light_attachment_cache_enabled && binding.attachment_cache_valid &&
								curtime - binding.attachment_last_seen_time <= std::max(0.0f, m_map_light_attachment_hold_seconds))
							{
								owner_origin = binding.cached_attachment_origin;
								owner_angles = binding.cached_attachment_angles;
								attachment_transform = true;
								++m_map_light_attachment_cache_hits;
							}

							if (attachment_transform)
							{
								if (!binding.attachment_local_valid)
								{
									binding.attachment_local_origin = inverse_rotate_source_vector(
										binding.authored_world_origin - owner_origin, owner_angles);
									binding.attachment_local_direction = inverse_rotate_source_vector(
										binding.authored_world_direction, owner_angles);
									if (binding.attachment_local_direction.LengthSqr() > 0.0001f)
										binding.attachment_local_direction.Normalize();
									binding.attachment_local_valid = true;
								}
								transform_local_origin = binding.attachment_local_origin;
								transform_local_direction = binding.attachment_local_direction;
								binding.attachment_applied = true;
								++m_map_light_attachment_updates;
							}
							else
							{
								++m_map_light_attachment_fallbacks;
							}
						}

						if (!attachment_transform && binding.extrude_from_owner && m_map_light_extrude_from_models && !binding.extrusion_applied)
						{
							Vector extruded = transform_local_origin;
							if (extrude_local_light_from_owner(owner_entity, transform_local_origin, transform_local_direction,
								m_map_light_model_surface_offset, extruded))
							{
								binding.local_origin = extruded;
								transform_local_origin = extruded;
								binding.extrusion_applied = true;
								++m_map_entity_graph_extruded;
							}
						}

						def.points.front().position = owner_origin + rotate_source_local_vector(transform_local_origin, owner_angles);
						Vector direction = rotate_source_local_vector(transform_local_direction, owner_angles);
						if (direction.LengthSqr() > 0.0001f)
						{
							direction.Normalize();
							def.points.front().direction = direction;
						}
						binding.def.points.front().position = def.points.front().position;
						binding.def.points.front().direction = def.points.front().direction;
					}
				}
				else
				{
					++binding.owner_missing_updates;
					if (!binding.owner_resolved_once)
					{
						owner_target_enabled = false;
						binding.owner_state_reason = "owner never resolved";
						++m_owned_worldlights_unresolved;
					}
					else
					{
						const float missing_for = std::max(0.0f, curtime - binding.owner_last_seen_time);
						owner_target_enabled = missing_for <= std::max(0.0f, m_map_light_owner_missing_grace) && !binding.owner_destroyed;
						binding.owner_state_reason = owner_target_enabled ? "owner temporarily missing, grace retained" : "owner missing";
						const bool destroy_missing_breakable = binding.owner_breakable && m_map_light_destroy_missing_breakables &&
							missing_for >= std::max(m_map_light_owner_missing_grace, m_map_light_owner_rebind_timeout);
						const bool destroy_removed_prop = m_map_light_destroy_removed_props &&
							owner_class_is_terminal_when_removed(binding.owner_class_hint) &&
							missing_for >= std::max(m_map_light_owner_missing_grace, m_map_light_removed_prop_destroy_seconds);
						if (!binding.owner_destroyed && (destroy_missing_breakable || destroy_removed_prop) &&
							m_bsp_worldlight_destroy_with_owner)
						{
							binding.owner_destroyed = true;
							owner_target_enabled = false;
							if (destroy_removed_prop && !binding.owner_breakable)
							{
								binding.owner_state_reason = "owner removed, terminal";
								++m_map_light_destroyed_by_removal;
							}
							else
							{
								binding.owner_state_reason = "breakable owner removed, terminal";
								++m_map_light_destroyed_by_health;
							}
						}
					}
				}

				if (binding.owner_destroyed && m_bsp_worldlight_destroy_with_owner) owner_target_enabled = false;
			}

			bool map_light_target_enabled = true;
			bool runtime_light_observed_this_update = false;
			if (m_map_light_follow_runtime_light_state && binding.runtime_light_trackable)
			{
				sdk::c_base_entity* light_entity = nullptr;
				const sdk::c_client_class* light_class = nullptr;
				bool light_found = false;
				bool shared_light_cache = false;
				bool resolved_light_this_update = false;

				if (binding.map_light_source_index >= 0)
				{
					if (const auto cached = resolved_map_lights.find(binding.map_light_source_index);
						cached != resolved_map_lights.end())
					{
						binding.runtime_light_entity_index = cached->second;
						light_found = get_runtime_entity_basic(binding.runtime_light_entity_index, light_entity, light_class) &&
							source_map_entities::runtime_class_matches_map_class(runtime_entity_class_name(light_class), binding.map_light_class_hint);
						shared_light_cache = light_found;
						resolved_light_this_update = light_found;
					}
				}

				if (!light_found && binding.runtime_light_entity_index > 0)
				{
					const bool claimed_by_other_light = claimed_runtime_light_indices.contains(binding.runtime_light_entity_index) &&
						!shared_light_cache;
					if (!claimed_by_other_light &&
						get_runtime_entity_basic(binding.runtime_light_entity_index, light_entity, light_class) &&
						source_map_entities::runtime_class_matches_map_class(runtime_entity_class_name(light_class), binding.map_light_class_hint))
					{
						const std::uint32_t handle_raw = light_entity->get_ref_handle_raw();
						light_found = !binding.runtime_light_resolved_once || binding.runtime_light_handle_raw == 0xffffffffu ||
							handle_raw == 0xffffffffu || binding.runtime_light_handle_raw == handle_raw;
					}
				}

				if (!light_found)
				{
					binding.runtime_light_entity_index = -1;
					light_entity = nullptr;
					light_class = nullptr;
					int resolved_index = -1;
					if (find_runtime_light_entity_for_map_light(binding.map_light_class_hint, binding.map_light_origin,
						m_map_light_runtime_light_resolve_distance, &claimed_runtime_light_indices, resolved_index, light_entity, light_class))
					{
						binding.runtime_light_entity_index = resolved_index;
						binding.runtime_light_handle_raw = light_entity->get_ref_handle_raw();
						binding.runtime_light_resolved_once = true;
						light_found = true;
						resolved_light_this_update = true;
					}
				}

				if (light_found && light_entity && light_class)
				{
					const std::uint32_t handle_raw = light_entity->get_ref_handle_raw();
					const bool generation_changed = binding.runtime_light_resolved_once &&
						binding.runtime_light_handle_raw != 0xffffffffu && handle_raw != 0xffffffffu &&
						binding.runtime_light_handle_raw != handle_raw;
					if (generation_changed && !resolved_light_this_update)
					{
						++m_map_light_handle_generation_rejects;
						light_found = false;
						binding.runtime_light_entity_index = -1;
					}
					else
					{
						binding.runtime_light_handle_raw = handle_raw;
						binding.runtime_light_resolved_once = true;
						if (binding.map_light_source_index >= 0)
							resolved_map_lights[binding.map_light_source_index] = binding.runtime_light_entity_index;
						claimed_runtime_light_indices.insert(binding.runtime_light_entity_index);
					}
				}

				if (light_found && light_entity && light_class)
				{
					runtime_light_observed_this_update = true;
					const bool raw_enabled = runtime_light_entity_is_enabled(light_entity, light_class);
					binding.runtime_light_last_seen_time = curtime;
					if (!binding.runtime_light_state_initialized)
					{
						binding.runtime_light_last_enabled = raw_enabled;
						binding.runtime_light_pending_valid = false;
						binding.runtime_light_state_initialized = true;
					}
					else if (!m_map_light_debounce_runtime_state)
					{
						if (binding.runtime_light_last_enabled != raw_enabled)
						{
							++m_map_runtime_light_state_updates;
							++m_map_runtime_light_state_confirmed_changes;
						}
						binding.runtime_light_last_enabled = raw_enabled;
						binding.runtime_light_pending_valid = false;
					}
					else if (raw_enabled == binding.runtime_light_last_enabled)
					{
						binding.runtime_light_pending_valid = false;
					}
					else
					{
						if (!binding.runtime_light_pending_valid || binding.runtime_light_pending_enabled != raw_enabled)
						{
							binding.runtime_light_pending_enabled = raw_enabled;
							binding.runtime_light_pending_since = curtime;
							binding.runtime_light_pending_valid = true;
						}
						const float pending_for = std::max(0.0f, curtime - binding.runtime_light_pending_since);
						if (pending_for >= std::max(0.0f, m_map_light_state_confirm_seconds))
						{
							binding.runtime_light_last_enabled = raw_enabled;
							binding.runtime_light_pending_valid = false;
							++m_map_runtime_light_state_updates;
							++m_map_runtime_light_state_confirmed_changes;
						}
						else
						{
							++m_map_runtime_light_state_pending;
						}
					}

					map_light_target_enabled = binding.runtime_light_last_enabled;
					if (binding.runtime_light_pending_valid)
					{
						binding.runtime_light_state_reason = binding.runtime_light_pending_enabled
							? "runtime light enabling, awaiting confirmation"
							: "runtime light disabling, awaiting confirmation";
					}
					else
					{
						binding.runtime_light_state_reason = map_light_target_enabled
							? "runtime light enabled"
							: "runtime light disabled";
					}
					if (map_light_target_enabled) ++m_map_runtime_light_state_active;
					else ++m_map_runtime_light_state_hidden;
				}
				else
				{
					++m_map_runtime_light_state_unresolved;
					if (binding.runtime_light_resolved_once)
					{
						const float missing_for = std::max(0.0f, curtime - binding.runtime_light_last_seen_time);
						const bool hold_previous = missing_for <= std::max(0.0f, m_map_light_owner_missing_grace);
						map_light_target_enabled = hold_previous ? binding.runtime_light_last_enabled : false;
						binding.runtime_light_state_reason = hold_previous
							? "runtime light temporarily missing, previous state retained"
							: "runtime light missing";
						if (hold_previous) ++m_map_runtime_light_state_missing_held;
					}
					else
					{
						map_light_target_enabled = !m_map_event_honor_starts_disabled || !binding.map_light_starts_disabled;
						binding.runtime_light_state_reason = (m_map_event_honor_starts_disabled && binding.map_light_starts_disabled)
							? "runtime light unresolved, authored start disabled"
							: "runtime light unresolved, authored/default start enabled";
					}
				}
			}

			if (binding.io_control_group > 0u && m_map_light_io_group_propagation)
			{
				auto& group_state = m_map_light_io_group_states[binding.io_control_group];
				if (runtime_light_observed_this_update)
				{
					auto& frame = frame_group_observations[binding.io_control_group];
					if (map_light_target_enabled) ++frame.enabled_votes;
					else ++frame.disabled_votes;
				}
				else if (binding.runtime_light_resolved_once && binding.io_killable &&
					m_map_light_io_kill_is_terminal && !binding.io_group_terminal)
				{
					const float missing_for = std::max(0.0f, curtime - binding.runtime_light_last_seen_time);
					if (missing_for >= std::max(m_map_light_owner_missing_grace, m_map_light_io_group_hold_seconds))
					{
						binding.io_group_terminal = true;
						if (binding.io_group_kill_all) ++frame_group_observations[binding.io_control_group].terminal_votes;
						++m_map_light_io_group_terminal_kills;
					}
				}

				if (group_state.terminal || binding.io_group_terminal)
				{
					map_light_target_enabled = false;
					binding.runtime_light_state_reason = group_state.terminal
						? std::format("map I/O group {} terminal kill", binding.io_control_group)
						: "map I/O light terminal kill";
				}
				else if (!runtime_light_observed_this_update && m_map_light_io_group_fallback && group_state.initialized)
				{
					const float age = std::max(0.0f, curtime - group_state.last_observed_time);
					if (age <= std::max(0.0f, m_map_light_io_group_hold_seconds))
					{
						if (m_map_light_io_action_scheduler && binding.io_scheduled_state_valid)
						{
							map_light_target_enabled = binding.io_scheduled_enabled;
							binding.runtime_light_state_reason = binding.io_scheduled_reason;
						}
						else
						{
							map_light_target_enabled = group_state.enabled;
							binding.runtime_light_state_reason = group_state.conflict
								? std::format("map I/O group {} conflict, retained {}",
									binding.io_control_group, group_state.enabled ? "enabled" : "disabled")
								: std::format("map I/O group {} propagated {}",
									binding.io_control_group, group_state.enabled ? "enabled" : "disabled");
						}
						++m_map_light_io_group_fallbacks;
					}
				}
			}

			if (binding.io_scheduled_terminal)
			{
				map_light_target_enabled = false;
				binding.runtime_light_state_reason = binding.io_scheduled_reason;
			}
			else if (!runtime_light_observed_this_update && m_map_light_io_action_scheduler &&
				binding.io_scheduled_state_valid)
			{
				map_light_target_enabled = binding.io_scheduled_enabled;
				binding.runtime_light_state_reason = binding.io_scheduled_reason;
			}

			if (!m_map_event_lights_enabled)
			{
				map_light_target_enabled = true;
				binding.runtime_light_pending_valid = false;
				binding.runtime_light_state_reason = "event automation bypassed; compiled light kept active";
			}

			const bool owner_gate_enabled = owner_target_enabled;
			owner_target_enabled = owner_gate_enabled && map_light_target_enabled;

			float style_value = 1.0f;
			if (m_map_event_lights_enabled && m_map_event_follow_lightstyles && binding.style > 0)
			{
				const auto* intf = interfaces::get();
				constexpr int max_source_lightstyles = 64;
				style_value = binding.style < max_source_lightstyles && intf && intf->m_engine
					? intf->m_engine->light_style_value(binding.style)
					: 1.0f;
				if (!std::isfinite(style_value)) style_value = 1.0f;
				style_value = std::clamp(style_value, 0.0f, 4.0f);
			}

			std::string final_reason;
			if (!owner_gate_enabled) final_reason = binding.owner_state_reason;
			else if (!map_light_target_enabled) final_reason = binding.runtime_light_state_reason;
			else if (style_value <= 0.001f) final_reason = "lightstyle evaluated to zero";
			else if (binding.runtime_light_pending_valid) final_reason = binding.runtime_light_state_reason;
			else final_reason = "active";

			if (binding.final_state_reason != final_reason)
			{
				m_map_light_last_transition = std::format("WORLDLIGHT #{} group {}: {} -> {}",
					binding.source_index, binding.io_control_group, binding.final_state_reason, final_reason);
				if (m_map_light_transition_log.size() >= 96u)
				{
					m_map_light_transition_log.pop_front();
					++m_map_light_transition_log_dropped;
				}
				m_map_light_transition_log.push_back({ curtime, binding.source_index, binding.io_control_group,
					binding.final_state_reason, final_reason });
				binding.final_state_reason = final_reason;
				binding.last_transition_time = curtime;
				++binding.transition_count;
				++m_map_light_reason_transitions;
			}

			const float target_state = owner_target_enabled ? 1.0f : 0.0f;
			const float dt = binding.last_state_update_time < -90000.0f
				? (1.0f / std::max(1.0f, m_map_light_runtime_update_hz))
				: std::clamp(curtime - binding.last_state_update_time, 0.0f, 0.25f);
			binding.last_state_update_time = curtime;
			if (m_map_light_smooth_state_changes)
			{
				const float fade_time = std::max(0.001f, m_map_light_state_fade_seconds);
				binding.state_scalar = move_towards(binding.state_scalar, target_state, dt / fade_time);
			}
			else binding.state_scalar = target_state;

			const float effective_scalar = binding.state_scalar * style_value;
			const bool enabled = effective_scalar > 0.001f;
			if (!def.points.empty()) def.points.front().radiance_scalar = binding.def.points.front().radiance_scalar * effective_scalar;

			const std::uint64_t signature = runtime_light_signature(def, enabled);
			if (binding.signature != signature || (enabled && !remix_lights::get()->has_light_with_exact_comment(def.comment)))
			{
				binding.signature = signature;
				binding.def.comment = def.comment;
				if (remix_lights::get()->upsert_runtime_light(def, enabled)) ++m_owned_worldlights_updated;
			}
			binding.active = enabled;
			if (enabled) ++m_owned_worldlights_active;
			else ++m_owned_worldlights_hidden;
		}

		for (const auto& [group_id, frame] : frame_group_observations)
		{
			auto& state = m_map_light_io_group_states[group_id];
			state.last_enabled_votes = frame.enabled_votes;
			state.last_disabled_votes = frame.disabled_votes;
			state.last_terminal_votes = frame.terminal_votes;
			const std::uint32_t total_votes = frame.enabled_votes + frame.disabled_votes;
			m_map_light_io_group_observations += total_votes;

			if (frame.terminal_votes > 0u)
			{
				if (!state.terminal)
				{
					const auto from = state.initialized
						? std::format("group consensus {}", state.enabled ? "enabled" : "disabled")
						: std::string("group uninitialized");
					const std::string to = "group terminal kill";
					if (m_map_light_transition_log.size() >= 96u)
					{
						m_map_light_transition_log.pop_front();
						++m_map_light_transition_log_dropped;
					}
					m_map_light_transition_log.push_back({ curtime, 0u, group_id, from, to });
					m_map_light_last_transition = std::format("I/O group {}: {} -> {}", group_id, from, to);
					++m_map_light_reason_transitions;
				}
				state.initialized = true;
				state.enabled = false;
				state.terminal = true;
				state.conflict = false;
				state.candidate_valid = false;
				state.last_observed_time = curtime;
				++state.transition_serial;
				for (auto& binding : m_owned_worldlight_bindings)
				{
					if (binding.io_control_group != group_id) continue;
					binding.io_scheduled_state_valid = true;
					binding.io_scheduled_enabled = false;
					binding.io_scheduled_terminal = true;
					binding.io_scheduled_reason = std::format("map I/O group {} terminal kill", group_id);
					binding.io_last_action_time = curtime;
				}
				continue;
			}
			if (state.terminal) continue;

			if (total_votes < static_cast<std::uint32_t>(std::max(1, m_map_light_io_group_min_observations))) continue;
			state.last_observed_time = curtime;
			state.observations += total_votes;
			if (frame.enabled_votes > 0u && frame.disabled_votes > 0u) ++m_map_light_io_group_split_observations;

			const std::uint32_t majority_votes = std::max(frame.enabled_votes, frame.disabled_votes);
			const float majority_ratio = total_votes > 0u
				? static_cast<float>(majority_votes) / static_cast<float>(total_votes)
				: 0.0f;
			const float required_ratio = std::clamp(m_map_light_io_group_majority_ratio, 0.50f, 1.0f);
			if (m_map_light_io_group_consensus && majority_ratio + 0.0001f < required_ratio)
			{
				state.conflict = true;
				state.candidate_valid = false;
				++m_map_light_io_group_conflicts;
				continue;
			}

			const bool proposed_enabled = frame.enabled_votes >= frame.disabled_votes;
			state.conflict = false;
			if (!state.initialized)
			{
				state.initialized = true;
				state.enabled = proposed_enabled;
				state.candidate_valid = false;
				seed_map_io_group_state(group_id, proposed_enabled, curtime);
				continue;
			}

			if (state.enabled == proposed_enabled)
			{
				state.candidate_valid = false;
				continue;
			}

			if (!m_map_light_io_group_consensus)
			{
				state.candidate_valid = true;
				state.candidate_enabled = proposed_enabled;
				state.candidate_since = curtime - std::max(0.0f, m_map_light_io_group_confirm_seconds);
			}
			else if (!state.candidate_valid || state.candidate_enabled != proposed_enabled)
			{
				state.candidate_valid = true;
				state.candidate_enabled = proposed_enabled;
				state.candidate_since = curtime;
			}

			const float candidate_age = std::max(0.0f, curtime - state.candidate_since);
			if (candidate_age < std::max(0.0f, m_map_light_io_group_confirm_seconds))
			{
				++m_map_light_io_group_candidate_holds;
				continue;
			}

			const bool old_enabled = state.enabled;
			state.enabled = proposed_enabled;
			state.candidate_valid = false;
			++state.transition_serial;
			schedule_map_io_group_transition(group_id, old_enabled, proposed_enabled, curtime);
			++m_map_light_io_group_changes;
			++m_map_light_io_group_consensus_commits;
			const auto from = std::format("group consensus {}", old_enabled ? "enabled" : "disabled");
			const auto to = std::format("group consensus {} ({}/{})", proposed_enabled ? "enabled" : "disabled",
				majority_votes, total_votes);
			if (m_map_light_transition_log.size() >= 96u)
			{
				m_map_light_transition_log.pop_front();
				++m_map_light_transition_log_dropped;
			}
			m_map_light_transition_log.push_back({ curtime, 0u, group_id, from, to });
			m_map_light_last_transition = std::format("I/O group {}: {} -> {}", group_id, from, to);
			++m_map_light_reason_transitions;
		}
		m_bsp_worldlight_imported = m_bsp_worldlight_untracked_active + m_owned_worldlights_active;
	}

	void dynamic_lighting::update_source_runtime_lights(const float curtime)
	{
		if (!m_map_light_runtime_tracking)
		{
			if (!m_runtime_dlights.empty() || !m_runtime_projected_lights.empty()) {
				clear_runtime_imported_lights();
			}
			m_map_light_runtime_status = "runtime tracking disabled";
			return;
		}

		if (curtime < m_map_light_runtime_next_update) {
			return;
		}
		const float hz = std::clamp(m_map_light_runtime_update_hz, 1.0f, 120.0f);
		m_map_light_runtime_next_update = curtime + (1.0f / hz);
		m_source_direct_duplicates_suppressed = 0u;
		m_source_runtime_rejected_survivor = 0u;
		m_source_runtime_rejected_infected = 0u;
		m_source_runtime_rejected_unknown_character = 0u;
		m_source_runtime_rejected_class = 0u;
		m_source_runtime_world_accepted = 0u;
		m_source_runtime_character_accepted = 0u;

		if (m_source_dlight_import) {
			update_source_dlights();
		}
		else if (!m_runtime_dlights.empty() || !g_captured_engine_lights.empty())
		{
			for (auto& [key, light] : m_runtime_dlights) {
				remix_lights::get()->upsert_runtime_light(light.def, false);
			}
			m_runtime_dlights.clear();
			g_captured_engine_lights.clear();
			m_source_dlight_active = 0u;
			m_source_dlight_release_holds = 0u;
			m_source_elight_active = 0u;
		}

		if (m_source_projectedtexture_import) {
			update_projected_texture_entities();
		}
		else if (!m_runtime_projected_lights.empty())
		{
			for (auto& [index, light] : m_runtime_projected_lights) {
				remix_lights::get()->upsert_runtime_light(light.def, false);
			}
			m_runtime_projected_lights.clear();
			m_source_projected_active = 0u;
			m_source_projected_pose_smoothed = 0u;
			m_source_projected_target_updates = 0u;
			m_source_projected_missing_holds = 0u;
			m_source_entity_dynamic_active = 0u;
		}

		update_owned_worldlights();
		m_map_light_runtime_status = std::format(
			"dlight {}/{} + hold {} | elight {}/{} | entity dynamic {}/{} | projected {}/{} smooth/target/hold {}/{}/{} | WORLDLIGHTS active/hidden/unresolved {}/{}/{} | authored state active/hidden/unresolved/pending/held {}/{}/{}/{}/{} | destroyed health/removal {}/{} | attachment/fallback/cache {}/{}/{} | rebind/deferred/predicted {}/{}/{} | handle rejects {} | dormant {}",
			m_source_dlight_active, m_source_dlight_updates, m_source_dlight_release_holds,
			m_source_elight_active, m_source_elight_updates,
			m_source_entity_dynamic_active, m_source_entity_dynamic_updates,
			m_source_projected_active, m_source_projected_updates,
			m_source_projected_pose_smoothed, m_source_projected_target_updates, m_source_projected_missing_holds,
			m_owned_worldlights_active, m_owned_worldlights_hidden, m_owned_worldlights_unresolved,
			m_map_runtime_light_state_active, m_map_runtime_light_state_hidden, m_map_runtime_light_state_unresolved,
			m_map_runtime_light_state_pending, m_map_runtime_light_state_missing_held,
			m_map_light_destroyed_by_health, m_map_light_destroyed_by_removal,
			m_map_light_attachment_updates, m_map_light_attachment_fallbacks, m_map_light_attachment_cache_hits,
			m_map_light_owner_rebinds, m_map_light_rebind_deferred, m_map_light_predicted_rebinds,
			m_map_light_handle_generation_rejects, m_map_light_dormant_retained);
		m_map_light_runtime_status += std::format(" | runtime policy {} world/character accepted {}/{} rejected survivor/infected/unknown/class {}/{}/{}/{}",
			get_source_runtime_light_profile_name(), m_source_runtime_world_accepted, m_source_runtime_character_accepted,
			m_source_runtime_rejected_survivor, m_source_runtime_rejected_infected,
			m_source_runtime_rejected_unknown_character, m_source_runtime_rejected_class);
		m_map_light_runtime_status += std::format(" | direct dedup {} | frozen snapshots {} | axis corrections/basis/switches {}/{}/{} | shape holds {}",
			m_source_direct_duplicates_suppressed, m_source_direct_snapshot_lights,
			m_source_light_axis_corrections, m_source_light_basis_recoveries, m_source_light_axis_switches,
			m_source_light_shape_holds);
		m_source_direct_status = std::format(
			"compiled {} | stable runtime {} | transient d/e {} | deduplicated {} | frozen {} | axis fixes {} (basis {}) | switches {} | shape holds {}",
			m_source_bsp_candidates.size(), m_runtime_projected_lights.size(), m_runtime_dlights.size(),
			m_source_direct_duplicates_suppressed, m_source_direct_snapshot_lights,
			m_source_light_axis_corrections, m_source_light_basis_recoveries, m_source_light_axis_switches,
			m_source_light_shape_holds);
		m_map_light_runtime_status += std::format(" | I/O groups observations/fallbacks/changes/terminal {}/{}/{}/{} consensus/conflict/hold/split {}/{}/{}/{}",
			m_map_light_io_group_observations, m_map_light_io_group_fallbacks,
			m_map_light_io_group_changes, m_map_light_io_group_terminal_kills,
			m_map_light_io_group_consensus_commits, m_map_light_io_group_conflicts,
			m_map_light_io_group_candidate_holds, m_map_light_io_group_split_observations);
	}

	void dynamic_lighting::on_map_load(const char* map_name)
	{
		const std::string normalized_map = normalize_map_session_name(map_name ? map_name : "");
		const double signal_time = static_cast<double>(GetTickCount64()) * 0.001;
		m_bsp_worldlight_session_probe_pending = false;
		if (!normalized_map.empty() && normalized_map == m_bsp_worldlight_pending_map &&
			signal_time - m_bsp_worldlight_last_map_load_signal < 0.35)
		{
			// CModelLoader can report the same map more than once while its world model is
			// still being assembled. Do not keep wiping the pending importer on duplicates.
			m_bsp_worldlight_auto_import_pending = m_bsp_worldlight_auto_import;
			m_bsp_worldlight_auto_import_delay = std::max(m_bsp_worldlight_auto_import_delay, 0.50f);
			m_bsp_worldlight_last_map_load_signal = signal_time;
			return;
		}

		m_bsp_worldlight_last_map_load_signal = signal_time;
		++m_bsp_worldlight_session_generation;
		m_map_light_config_dirty = false;
		m_map_light_config_save_at = 0.0f;
		m_map_light_editor_synced_generation = 0xffffffffu;
		m_persistent_map_light_dirty = false;
		m_persistent_map_light_save_at = 0.0f;
		m_persistent_map_light_fingerprint_valid = false;
		m_persistent_map_light_known_ids.clear();
		m_persistent_map_light_materialized_sources.clear();
		m_persistent_map_light_loaded = 0u;
		m_persistent_map_light_materialized = 0u;
		m_persistent_map_light_live_linked = 0u;
		m_persistent_map_light_tombstones = 0u;
		m_persistent_map_light_status = "waiting for map settings merge";
		g_persistent_next_user_light_id = 1u;
		{
			auto& editor_lights = map_settings::get_map_settings().remix_lights;
			std::erase_if(editor_lights, [](const auto& def) { return def.generated_source_transient; });
			m_source_direct_snapshot_lights = 0u;
			m_source_direct_status = "new map: transient Source direct-light snapshots cleared";
		}
		clear_bsp_worldlight_lights();
		clear_runtime_imported_lights();
		m_owned_worldlight_bindings.clear();
		m_pending_lights.clear();
		m_d3d_lights.clear();
		m_sound_hash_last_trigger_times.clear();
		m_last_auto_muzzle_time = -99999.0f;
		m_debug_last_trigger_name.clear();
		m_debug_last_trigger_origin = Vector(0.0f, 0.0f, 0.0f);
		m_debug_last_trigger_time = 0.0f;
		m_d3d_setlight_calls = 0u;
		m_d3d_lightenable_calls = 0u;
		m_d3d_spawned_lights = 0u;
		m_d3d_last_index = 0u;
		m_d3d_last_origin = Vector(0.0f, 0.0f, 0.0f);
		m_muzzle_candidate_sounds = 0u;
		m_muzzle_accepted_sounds = 0u;
		m_muzzle_rejected_non_fire = 0u;
		m_muzzle_rejected_hard = 0u;
		m_muzzle_rejected_strict = 0u;
		m_muzzle_skipped_cooldown = 0u;
		m_muzzle_skipped_budget = 0u;
		m_muzzle_spawn_attempts = 0u;
		m_muzzle_last_accept_reason = "none";
		m_muzzle_last_reject_reason = "none";
		m_muzzle_last_profile = "none";
		m_muzzle_last_sound = "none";
		m_muzzle_last_origin = Vector(0.0f, 0.0f, 0.0f);
		m_muzzle_last_forward = Vector(0.0f, 1.0f, 0.0f);
		m_sound_hash_library_matches = 0u;
		m_sound_hash_library_spawned = 0u;
		m_sound_hash_library_skipped = 0u;
		m_sound_hash_library_skipped_player_origin = 0u;
		m_sound_hash_last_category = "none";
		m_sound_hash_last_sound = "none";
		m_sound_hash_last_hash = 0u;
		m_sound_hash_last_origin = Vector(0.0f, 0.0f, 0.0f);
		m_source_bsp_scan_status = "not scanned";
		m_source_bsp_scan_entities = 0u;
		m_source_bsp_scan_lights = 0u;
		m_source_bsp_scan_previewed = 0u;
		m_source_bsp_scan_skipped_by_filter = 0u;
		m_source_bsp_scan_skipped_by_distance = 0u;
		m_source_bsp_preview_create_success = 0u;
		m_source_bsp_preview_create_failed = 0u;
		m_source_bsp_preview_live_count = 0u;
		m_source_bsp_scan_output_path.clear();
		m_source_bsp_candidates.clear();
		m_source_bsp_selected_index = -1;
		m_source_bsp_import_status = "not imported";

		m_bsp_worldlight_status = "waiting for map load";
		m_bsp_worldlight_backend = "none";
		m_bsp_worldlight_diagnostics.clear();
		m_bsp_worldlight_source.clear();
		m_bsp_worldlight_records = 0u;
		m_bsp_worldlight_candidates = 0u;
		m_bsp_worldlight_imported = 0u;
		m_bsp_worldlight_untracked_active = 0u;
		m_bsp_worldlight_create_failed = 0u;
		m_bsp_worldlight_skipped_type = 0u;
		m_bsp_worldlight_skipped_style = 0u;
		m_bsp_worldlight_skipped_intensity = 0u;
		m_bsp_worldlight_skipped_distance = 0u;
		m_bsp_worldlight_merged_duplicates = 0u;
		m_bsp_worldlight_stream_refreshes = 0u;
		m_bsp_worldlight_stream_retained = 0u;
		m_bsp_worldlight_stream_evicted = 0u;
		m_bsp_worldlight_surface_clustered = 0u;
		m_bsp_worldlight_active_sources.clear();
		g_map_light_overrides = {};
		m_map_light_override_path.clear();
		m_map_light_override_status = "waiting for map";
		m_map_light_overrides_loaded = 0u;
		m_map_light_overrides_applied = 0u;
		m_map_light_overrides_disabled = 0u;
		m_map_light_override_invalid_lines = 0u;
		m_bsp_worldlight_pending_map = normalized_map;
		m_bsp_worldlight_loaded_map.clear();
		m_bsp_worldlight_auto_import_pending = m_bsp_worldlight_auto_import && !m_bsp_worldlight_pending_map.empty();
		m_bsp_worldlight_auto_import_delay = 0.75f;
		m_bsp_worldlight_auto_import_retries = 0u;
		m_bsp_worldlight_watchdog_next_check = 0.0f;
		m_bsp_worldlight_last_client_time = -1.0f;
		m_bsp_worldlight_session_probe_pending = false;
		m_bsp_worldlight_next_stream_check = 0.0f;
		m_bsp_worldlight_last_import_origin = Vector(0.0f, 0.0f, 0.0f);
		m_bsp_worldlight_have_import_origin = false;
		m_map_light_runtime_next_update = 0.0f;
		m_source_dlight_active = 0u;
		m_source_dlight_updates = 0u;
		m_source_dlight_release_holds = 0u;
		m_source_elight_active = 0u;
		m_source_elight_updates = 0u;
		m_source_alloc_dlight_calls = 0u;
		m_source_alloc_elight_calls = 0u;
		g_captured_engine_lights.clear();
		m_source_projected_active = 0u;
		m_source_projected_updates = 0u;
		m_source_projected_pose_smoothed = 0u;
		m_source_projected_target_updates = 0u;
		m_source_projected_missing_holds = 0u;
		m_source_projected_generation_resets = 0u;
		m_source_entity_dynamic_active = 0u;
		m_source_entity_dynamic_updates = 0u;
		g_map_entity_graph = {};
		g_map_entity_graph_map.clear();
		g_map_entity_graph_loaded = false;
		g_source_environment_graph = {};
		g_source_environment_graph_map.clear();
		g_source_environment_graph_loaded = false;
		m_map_entity_graph_entities = 0u;
		m_map_entity_graph_lights = 0u;
		m_map_entity_graph_owners = 0u;
		m_map_entity_graph_matched_lights = 0u;
		m_map_entity_graph_bound_owners = 0u;
		m_map_entity_graph_extruded = 0u;
		m_map_entity_graph_attachment_links = 0u;
		m_map_entity_graph_io_links = 0u;
		m_map_entity_graph_io_resolved_links = 0u;
		m_map_entity_graph_io_light_links = 0u;
		m_map_entity_graph_io_kill_links = 0u;
		m_map_entity_graph_io_transitive_links = 0u;
		m_map_entity_graph_io_relay_hops = 0u;
		m_map_entity_graph_io_cycles = 0u;
		m_map_entity_graph_io_depth_limited = 0u;
		m_map_entity_graph_io_multi_manager_links = 0u;
		m_map_entity_graph_io_relay_entities = 0u;
		m_map_entity_graph_controlled_lights = 0u;
		m_map_entity_graph_light_groups = 0u;
		m_map_light_io_group_observations = 0u;
		m_map_light_io_group_fallbacks = 0u;
		m_map_light_io_group_changes = 0u;
		m_map_light_io_group_terminal_kills = 0u;
		m_map_light_io_group_consensus_commits = 0u;
		m_map_light_io_group_conflicts = 0u;
		m_map_light_io_group_candidate_holds = 0u;
		m_map_light_io_group_split_observations = 0u;
		m_map_light_io_actions_scheduled = 0u;
		m_map_light_io_actions_executed = 0u;
		m_map_light_io_actions_coalesced = 0u;
		m_map_light_io_actions_dropped = 0u;
		m_map_light_io_actions_maxfires_blocked = 0u;
		m_map_light_io_actions_toggle = 0u;
		m_map_light_io_actions_kill = 0u;
		m_map_light_io_actions_manual = 0u;
		m_map_light_io_group_states.clear();
		m_map_light_io_exact_states.clear();
		{
			std::scoped_lock lock(g_server_accept_input_mutex);
			g_server_accept_input_events.clear();
		}
		m_server_accept_input_calls = 0u;
		m_server_accept_input_accepted = 0u;
		m_server_accept_input_direct_actions = 0u;
		m_server_accept_input_routed_actions = 0u;
		m_server_accept_input_unresolved = 0u;
		m_server_accept_input_dropped = 0u;
		m_server_accept_input_last_event = "none";
		m_map_light_io_pending_actions.clear();
		m_map_light_io_pending_count = 0u;
		m_map_light_io_root_fire_counts.clear();
		m_map_light_io_next_serial = 1u;
		m_map_light_model_rejects = 0u;
		m_map_light_duplicate_owner_rejects = 0u;
		m_map_light_attachment_updates = 0u;
		m_map_light_destroyed_by_health = 0u;
		m_map_light_destroyed_by_removal = 0u;
		m_map_light_dormant_retained = 0u;
		m_map_light_owner_rebinds = 0u;
		m_map_light_rebind_deferred = 0u;
		m_map_light_attachment_fallbacks = 0u;
		m_map_light_attachment_cache_hits = 0u;
		m_map_light_handle_generation_rejects = 0u;
		m_map_light_predicted_rebinds = 0u;
		m_map_runtime_light_state_active = 0u;
		m_map_runtime_light_state_hidden = 0u;
		m_map_runtime_light_state_unresolved = 0u;
		m_map_runtime_light_state_updates = 0u;
		m_map_runtime_light_state_pending = 0u;
		m_map_runtime_light_state_missing_held = 0u;
		m_map_runtime_light_state_confirmed_changes = 0u;
		m_map_light_reason_transitions = 0u;
		clear_map_light_transition_log();
		m_map_entity_graph_status = m_map_entity_graph_enabled ? "waiting for BSP entity graph" : "entity graph disabled";
		m_owned_worldlights_active = 0u;
		m_owned_worldlights_hidden = 0u;
		m_owned_worldlights_unresolved = 0u;
		m_owned_worldlights_updated = 0u;
		m_map_light_runtime_status = "waiting for runtime Source lights";

		m_runtime_budget_window_start = -99999.0f;
		m_next_leaf_check_time = 0.0f;
		reset_runtime_budget_counters();
	}

	Vector dynamic_lighting::apply_muzzle_offset(const Vector& origin, const Vector& forward, const Vector& right, const Vector& up)
	{
		return origin + (forward * m_muzzle_flash_offset.x) + (up * m_muzzle_flash_offset.y) + (right * m_muzzle_flash_offset.z);
	}

	bool dynamic_lighting::get_player_flashlight_source(Vector& out_origin, Vector& out_forward)
	{
		if (!remix_api::is_initialized()) {
			return false;
		}

		for (const auto& [name, fl] : remix_api::get()->m_flashlights)
		{
			if (!fl.is_player) {
				continue;
			}

			const auto gs = game_settings::get();
			const Vector flashlight_offset = gs->flashlight_offset_player.get_as<float*>();
			const Vector flashlight_origin = fl.def.pos + (fl.def.fwd * flashlight_offset.x) + (fl.def.up * flashlight_offset.y) + (fl.def.rt * flashlight_offset.z);

			out_origin = apply_muzzle_offset(flashlight_origin, fl.def.fwd, fl.def.rt, fl.def.up);
			out_forward = fl.def.fwd;
			if (out_forward.LengthSqr() <= 0.0001f) {
				out_forward = Vector(0.0f, 1.0f, 0.0f);
			}
			out_forward.Normalize();
			return true;
		}

		return false;
	}

	bool dynamic_lighting::get_auto_muzzle_source(const Vector& sound_origin, Vector& out_origin, Vector& out_forward)
	{
		Vector camera_origin = sound_origin;
		Vector forward(0.0f, 1.0f, 0.0f);
		Vector right(1.0f, 0.0f, 0.0f);
		Vector up(0.0f, 0.0f, 1.0f);
		game::get_current_view_basis(camera_origin, forward, right, up);

		out_forward = forward;

		// Best mode for first-person L4D2: the sound event is often emitted from the player
		// entity origin, which places the flash around feet/ground. The Remix flashlight already
		// tracks eye/weapon-view direction, so use it as the default muzzle anchor.
		if (m_muzzle_flash_origin_mode == 0 && get_player_flashlight_source(out_origin, out_forward)) {
			return true;
		}

		if (m_muzzle_flash_origin_mode == 1 || m_muzzle_flash_origin_mode == 0)
		{
			out_origin = apply_muzzle_offset(camera_origin, forward, right, up);
			return true;
		}

		if (m_muzzle_flash_origin_mode == 3)
		{
			// Useful for non-local survivors/bots when the sound origin is valid but low.
			out_origin = apply_muzzle_offset(sound_origin + Vector(0.0f, 0.0f, 46.0f), forward, right, up);
			return true;
		}

		out_origin = sound_origin;
		return true;
	}

	const char* dynamic_lighting::get_muzzle_profile_name_for_ui()
	{
		if (m_muzzle_weapon_profile_mode > 0 &&
			m_muzzle_weapon_profile_mode < static_cast<int>((sizeof(MUZZLE_WEAPON_PROFILES) / sizeof(MUZZLE_WEAPON_PROFILES[0]))))
		{
			return MUZZLE_WEAPON_PROFILES[m_muzzle_weapon_profile_mode].name;
		}

		return m_muzzle_last_profile.c_str();
	}

	void dynamic_lighting::test_muzzle_flash()
	{
		const auto& profile = m_muzzle_weapon_profiles_enabled ? get_selected_muzzle_profile("rifle") : MUZZLE_WEAPON_PROFILES[0];
		const Vector old_offset = m_muzzle_flash_offset;
		if (m_muzzle_weapon_profiles_enabled) {
			m_muzzle_flash_offset = profile.offset;
		}

		Vector muzzle_origin = Vector(0.0f, 0.0f, 0.0f);
		Vector muzzle_forward = Vector(0.0f, 1.0f, 0.0f);
		Vector camera_origin = {};
		Vector camera_forward = {}, camera_right = {}, camera_up = {};
		if (!game::get_current_view_basis(camera_origin, camera_forward, camera_right, camera_up)) {
			camera_origin = Vector(0.0f, 0.0f, 0.0f);
		}
		get_auto_muzzle_source(camera_origin, muzzle_origin, muzzle_forward);

		if (m_muzzle_weapon_profiles_enabled) {
			m_muzzle_flash_offset = old_offset;
		}

		map_settings::dynamic_light_event_s ev = {};
		ev.name = "test_muzzle_flash_v2";
		ev.preset = "muzzle_flash";
		ev.animation = "muzzle_flash";
		ev.duration = std::max(0.025f, profile.duration * (m_muzzle_flash_duration / 0.075f));
		ev.delay = std::max(0.0f, m_muzzle_flash_delay);
		ev.cooldown = 0.0f;
		ev.radiance = m_muzzle_flash_color;
		ev.scalar = m_muzzle_flash_scalar * (m_muzzle_weapon_profiles_enabled ? profile.scalar : 1.0f);
		ev.radius = std::max(0.001f, profile.radius * m_muzzle_flash_radius);
		ev.use_source_origin = true;
		ev.use_camera_when_no_source = true;
		ev.use_shaping = m_muzzle_flash_shape_mode == 1;
		ev.degrees = ev.use_shaping ? 62.0f : 180.0f;
		ev.softness = ev.use_shaping ? 0.22f : 0.0f;
		ev.exponent = ev.use_shaping ? 0.65f : 0.0f;
		ev.comment = "manual muzzle v2 test";
		trigger_event(ev, &muzzle_origin, &muzzle_forward);

		m_muzzle_last_profile = profile.name;
		m_muzzle_last_sound = "manual test";
		m_muzzle_last_origin = muzzle_origin;
		m_muzzle_last_forward = muzzle_forward;
	}

	static std::string read_bsp_entity_lump(const std::string& path, std::string& error)
	{
		struct bsp_lump_s
		{
			std::int32_t fileofs;
			std::int32_t filelen;
			std::int32_t version;
			char fourcc[4];
		};

		struct bsp_header_s
		{
			std::int32_t ident;
			std::int32_t version;
			bsp_lump_s lumps[64];
			std::int32_t map_revision;
		};

		std::ifstream file(path, std::ios::binary);
		if (!file.is_open())
		{
			error = "could not open BSP";
			return {};
		}

		bsp_header_s header = {};
		file.read(reinterpret_cast<char*>(&header), sizeof(header));
		if (!file || header.ident != 0x50534256) // 'VBSP'
		{
			error = "invalid VBSP header";
			return {};
		}

		const auto& ent_lump = header.lumps[0];
		if (ent_lump.fileofs <= 0 || ent_lump.filelen <= 0 || ent_lump.filelen > 16 * 1024 * 1024)
		{
			error = "invalid entity lump";
			return {};
		}

		std::string data(static_cast<std::size_t>(ent_lump.filelen), '\0');
		file.seekg(ent_lump.fileofs, std::ios::beg);
		file.read(data.data(), ent_lump.filelen);
		if (!file)
		{
			error = "failed to read entity lump";
			return {};
		}

		return data;
	}

	static std::vector<std::string> split_entity_blocks(const std::string& entity_lump)
	{
		std::vector<std::string> blocks;
		std::size_t pos = 0u;
		while (true)
		{
			const auto open = entity_lump.find('{', pos);
			if (open == std::string::npos) {
				break;
			}

			const auto close = entity_lump.find('}', open + 1u);
			if (close == std::string::npos) {
				break;
			}

			blocks.emplace_back(entity_lump.substr(open + 1u, close - open - 1u));
			pos = close + 1u;
		}

		return blocks;
	}

	static std::unordered_map<std::string, std::string> parse_entity_key_values(const std::string& block)
	{
		std::unordered_map<std::string, std::string> kv;
		std::size_t pos = 0u;
		while (true)
		{
			const auto key_begin = block.find('"', pos);
			if (key_begin == std::string::npos) {
				break;
			}

			const auto key_end = block.find('"', key_begin + 1u);
			if (key_end == std::string::npos) {
				break;
			}

			const auto val_begin = block.find('"', key_end + 1u);
			if (val_begin == std::string::npos) {
				break;
			}

			const auto val_end = block.find('"', val_begin + 1u);
			if (val_end == std::string::npos) {
				break;
			}

			kv[block.substr(key_begin + 1u, key_end - key_begin - 1u)] = block.substr(val_begin + 1u, val_end - val_begin - 1u);
			pos = val_end + 1u;
		}

		return kv;
	}

	static bool parse_vec3_string(const std::string& text, Vector& out)
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		if (sscanf(text.c_str(), "%f %f %f", &x, &y, &z) == 3)
		{
			out = Vector(x, y, z);
			return true;
		}

		return false;
	}

	static std::string toml_escape_inline_string_dyn(std::string value)
	{
		utils::replace_all(value, "\\", "\\\\");
		utils::replace_all(value, "\"", "\\\"");
		utils::replace_all(value, "\r", " ");
		utils::replace_all(value, "\n", " ");
		return value;
	}

	static Vector direction_from_source_angles(const Vector& angles)
	{
		const float pitch = DEG2RADF(angles.x);
		const float yaw = DEG2RADF(angles.y);
		const float sp = std::sin(pitch);
		const float cp = std::cos(pitch);
		const float sy = std::sin(yaw);
		const float cy = std::cos(yaw);
		Vector fwd(cp * cy, cp * sy, -sp);
		if (fwd.LengthSqr() <= 0.0001f) {
			fwd = Vector(0.0f, 0.0f, -1.0f);
		}
		fwd.Normalize();
		return fwd;
	}

	static bool parse_source_float_key(const std::unordered_map<std::string, std::string>& kv, const char* key, float& out)
	{
		const auto it = kv.find(key);
		if (it == kv.end()) {
			return false;
		}

		char* end = nullptr;
		const float value = std::strtof(it->second.c_str(), &end);
		if (!end || end == it->second.c_str()) {
			return false;
		}

		out = value;
		return true;
	}

	static void parse_source_light_value(const std::unordered_map<std::string, std::string>& kv, Vector& radiance, float& scalar)
	{
		radiance = Vector(10.0f, 8.0f, 5.0f);
		scalar = 12.0f;

		auto it = kv.find("_light");
		if (it == kv.end()) {
			it = kv.find("rendercolor");
		}

		float r = 255.0f, g = 220.0f, b = 170.0f, intensity = 200.0f;
		if (it != kv.end())
		{
			const int count = sscanf(it->second.c_str(), "%f %f %f %f", &r, &g, &b, &intensity);
			if (count >= 3)
			{
				if (count < 4) {
					intensity = 200.0f;
				}

				float hdr_scale = 1.0f;
				parse_source_float_key(kv, "HDRColorScale", hdr_scale);
				hdr_scale = std::clamp(hdr_scale, 0.05f, 16.0f);

				radiance = Vector(
					std::max(0.0f, r / 255.0f * 12.0f * hdr_scale),
					std::max(0.0f, g / 255.0f * 12.0f * hdr_scale),
					std::max(0.0f, b / 255.0f * 12.0f * hdr_scale));
				scalar = std::clamp(intensity / 8.0f, 2.0f, 90.0f);
			}
		}
	}

	static bool source_light_class_allowed(const std::string& cls)
	{
		if (cls == "light") {
			return dynamic_lighting::m_source_bsp_scan_include_point;
		}
		if (cls == "light_spot" || cls == "point_spotlight") {
			return dynamic_lighting::m_source_bsp_scan_include_spot;
		}
		if (cls == "env_projectedtexture") {
			return dynamic_lighting::m_source_bsp_scan_include_projected;
		}
		if (cls == "env_sprite") {
			return dynamic_lighting::m_source_bsp_scan_include_sprite;
		}
		return false;
	}

	static float source_light_radius_from_keys(const std::unordered_map<std::string, std::string>& kv, const bool shaped)
	{
		float dist = 0.0f;
		if (parse_source_float_key(kv, "_zero_percent_distance", dist) ||
			parse_source_float_key(kv, "distance", dist) ||
			parse_source_float_key(kv, "spotlight_length", dist))
		{
			// Source distances are world units. Remix light radius here is authored much smaller
			// in existing map_settings, so treat this as a rough authoring seed.
			return std::clamp(dist / (shaped ? 420.0f : 260.0f), shaped ? 0.55f : 0.85f, shaped ? 8.0f : 12.0f);
		}

		return shaped ? 0.75f : 2.35f;
	}

	static std::string source_targetname_comment(const std::unordered_map<std::string, std::string>& kv)
	{
		std::string out;
		if (const auto it = kv.find("targetname"); it != kv.end() && !it->second.empty()) {
			out += std::format(" targetname={}", it->second);
		}
		if (const auto it = kv.find("style"); it != kv.end() && !it->second.empty()) {
			out += std::format(" style={}", it->second);
		}
		return out;
	}

	static std::string source_targetname_value(const std::unordered_map<std::string, std::string>& kv)
	{
		if (const auto it = kv.find("targetname"); it != kv.end()) {
			return it->second;
		}
		return {};
	}


	static Vector normalize_map_light_animation_vector(Vector value, const Vector& fallback)
	{
		if (value.LengthSqr() <= 0.0001f) value = fallback;
		if (value.LengthSqr() <= 0.0001f) value = Vector(0.0f, 0.0f, 1.0f);
		value.Normalize();
		return value;
	}

	static Vector rotate_map_light_animation_vector(Vector value, Vector axis, const float degrees)
	{
		value = normalize_map_light_animation_vector(value, Vector(0.0f, 1.0f, 0.0f));
		axis = normalize_map_light_animation_vector(axis, Vector(0.0f, 0.0f, 1.0f));
		const float radians = DEG2RADF(degrees);
		const float c = std::cos(radians);
		const float sn = std::sin(radians);
		const float d = value.Dot(axis);
		return normalize_map_light_animation_vector(value * c + axis.Cross(value) * sn + axis * (d * (1.0f - c)), value);
	}

	static std::vector<map_settings::remix_light_settings_s::point_s> build_map_light_override_animation_points(
		const map_settings::remix_light_settings_s::point_s& source_point,
		std::string animation, float cycle_time, float variation, Vector animation_axis,
		float animation_degrees, float animation_phase)
	{
		auto name = utils::str_to_lower(std::move(animation));
		utils::replace_all(name, "-", "_");
		auto base = source_point;
		base.timepoint = 0.0f;
		cycle_time = std::max(0.05f, cycle_time);
		variation = std::clamp(variation, 0.0f, 1.0f);
		animation_axis = normalize_map_light_animation_vector(animation_axis, Vector(0.0f, 0.0f, 1.0f));
		const Vector base_direction = normalize_map_light_animation_vector(base.direction, Vector(0.0f, 1.0f, 0.0f));
		const float base_scalar = base.radiance_scalar;

		std::vector<map_settings::remix_light_settings_s::point_s> points;
		auto add = [&](const float t, const float scalar_mul, const Vector& direction)
		{
			auto point = base;
			point.timepoint = t;
			point.radiance_scalar = std::max(0.0f, base_scalar * scalar_mul);
			point.direction = normalize_map_light_animation_vector(direction, base_direction);
			points.emplace_back(std::move(point));
		};
		auto add_intensity_curve = [&](const std::vector<float>& values)
		{
			if (values.empty()) return;
			for (std::size_t i = 0u; i < values.size(); ++i)
			{
				const float u = values.size() <= 1u ? 0.0f : static_cast<float>(i) / static_cast<float>(values.size() - 1u);
				const float jitter = 1.0f + (((i & 1u) ? variation : -variation) * 0.25f);
				add(cycle_time * u, values[i] * jitter, base_direction);
			}
		};

		if (name.empty() || name == "stable" || name == "none" || name == "off")
		{
			points.push_back(base);
		}
		else if (name == "pulse_slow" || name == "pulse_fast" || name == "breathing")
		{
			const float low = name == "breathing" ? 0.42f : 0.22f;
			const float high = name == "pulse_fast" ? 1.30f : 1.08f;
			add_intensity_curve({ low, 0.72f, high, 0.72f, low });
		}
		else if (name == "fire_pulse" || name == "candle_flicker")
		{
			add_intensity_curve(name == "candle_flicker"
				? std::vector<float>{ 0.86f, 1.08f, 0.78f, 1.18f, 0.94f, 1.05f, 0.86f }
				: std::vector<float>{ 0.72f, 1.20f, 0.88f, 1.32f, 0.76f, 1.08f, 0.72f });
		}
		else if (name == "soft_flicker" || name == "tv_noise" || name == "unstable_bulb" || name == "generator_stutter")
		{
			add_intensity_curve((name == "unstable_bulb" || name == "generator_stutter")
				? std::vector<float>{ 1.0f, 0.18f, 0.05f, 1.28f, 0.62f, 1.05f, 1.0f }
				: std::vector<float>{ 0.88f, 1.08f, 0.82f, 1.14f, 0.91f, 1.04f, 0.88f });
		}
		else if (name == "broken_fluorescent" || name == "fluorescent_random")
		{
			add_intensity_curve({ 1.0f, 0.0f, 0.0f, 1.18f, 0.0f, 0.74f, 1.0f });
		}
		else if (name == "strobe_fast" || name == "strobe_slow")
		{
			const int flashes = name == "strobe_fast" ? 4 : 2;
			for (int i = 0; i <= flashes * 2; ++i)
			{
				const float u = static_cast<float>(i) / static_cast<float>(flashes * 2);
				add(cycle_time * u, (i & 1) ? 0.0f : 1.35f, base_direction);
			}
		}
		else if (name == "rotating_yaw" || name == "disc_spin" || name == "spot_axis_spin")
		{
			base.use_shaping = true;
			base.degrees = std::clamp(base.degrees <= 0.0f ? 35.0f : base.degrees, 1.0f, 179.0f);
			for (int i = 0; i <= 8; ++i)
			{
				const float u = static_cast<float>(i) / 8.0f;
				add(cycle_time * u, 1.0f, rotate_map_light_animation_vector(base_direction, animation_axis,
					animation_phase + animation_degrees * u));
			}
		}
		else if (name == "searchlight_sweep" || name == "pendulum_sweep" || name == "spot_axis_sweep")
		{
			base.use_shaping = true;
			base.degrees = std::clamp(base.degrees <= 0.0f ? 35.0f : base.degrees, 1.0f, 179.0f);
			const float half = std::max(1.0f, std::abs(animation_degrees)) * 0.5f;
			for (int i = 0; i <= 8; ++i)
			{
				const float u = static_cast<float>(i) / 8.0f;
				const float wave = std::sin((u * 2.0f - 0.5f) * static_cast<float>(M_PI));
				add(cycle_time * u, 1.0f, rotate_map_light_animation_vector(base_direction, animation_axis,
					animation_phase + wave * half));
			}
		}
		else
		{
			points.push_back(base);
		}

		if (points.empty()) points.push_back(base);
		return points;
	}

	static void apply_map_light_animation_override(map_settings::remix_light_settings_s& def,
		const source_map_light_overrides::light_override_s& entry)
	{
		if (!entry.has_animation || def.points.empty()) return;
		auto name = utils::str_to_lower(entry.animation);
		utils::replace_all(name, "-", "_");
		const float duration = std::max(0.05f, entry.has_animation_duration ? entry.animation_duration : 1.0f);
		const float speed = std::max(0.05f, entry.has_animation_speed ? entry.animation_speed : 1.0f);
		const float variation = std::clamp(entry.has_animation_variation ? entry.animation_variation : 0.0f, 0.0f, 1.0f);
		const Vector axis = normalize_map_light_animation_vector(entry.has_animation_axis ? entry.animation_axis : Vector(0.0f, 0.0f, 1.0f), Vector(0.0f, 0.0f, 1.0f));
		const float degrees = std::clamp(entry.has_animation_degrees ? entry.animation_degrees : 360.0f, 0.0f, 1440.0f);
		const float phase = entry.has_animation_phase ? entry.animation_phase : 0.0f;
		const auto base = def.points.front();
		def.points = build_map_light_override_animation_points(base, name, duration / speed, variation, axis, degrees, phase);
		def.animation = name.empty() ? "stable" : name;
		def.animation_duration = duration;
		def.animation_speed = speed;
		def.animation_variation = variation;
		def.animation_axis = axis;
		def.animation_degrees = degrees;
		def.animation_phase = phase;
		def.loop = !(name.empty() || name == "stable" || name == "none" || name == "off");
		def.loop_smoothing = false;
	}

	static map_settings::remix_light_settings_s make_light_def_from_bsp_candidate(const dynamic_lighting::bsp_light_candidate_s& candidate, const char* comment_prefix)
	{
		map_settings::remix_light_settings_s def = {};
		def.points.emplace_back(make_point(candidate.origin, candidate.radiance, candidate.scalar, candidate.radius, 0.0f, 1.0f, candidate.shaped, candidate.direction, candidate.degrees, candidate.softness, candidate.exponent));
		if (candidate.distant && !def.points.empty())
		{
			auto& point = def.points.front();
			point.authoring_shape = map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT;
			point.authoring_width = std::clamp(candidate.distant_angular_diameter, 0.01f, 180.0f);
			point.authoring_height = point.authoring_width;
			point.authoring_length = 1.0f;
			point.use_shaping = false;
			point.direction = candidate.direction;
			point.degrees = 180.0f;
			point.softness = 0.0f;
			point.exponent = 0.0f;
		}
		if (candidate.surface_cluster && !def.points.empty())
		{
			auto& point = def.points.front();
			point.ies_emulation = true;
			point.ies_emulation_samples = std::clamp(candidate.surface_cluster_samples, 1, 16);
			point.ies_emulation_spread = std::clamp(candidate.surface_cluster_spread, 0.0f, 4.0f);
			point.ies_emulation_aspect = std::clamp(candidate.surface_cluster_aspect, 0.1f, 8.0f);
			point.ies_emulation_intensity_scale = std::clamp(candidate.surface_cluster_intensity, 0.0f, 4.0f);
			point.ies_emulation_radius_scale = std::clamp(candidate.surface_cluster_radius_scale, 0.01f, 4.0f);
			point.ies_emulation_forward_offset = 0.04f;
			point.ies_emulation_pattern = candidate.surface_cluster_pattern.empty() ? "cross" : candidate.surface_cluster_pattern;
		}
		def.run_once = false;
		def.loop = true;
		def.loop_smoothing = true;
		def.comment = std::format("{}{}", comment_prefix ? comment_prefix : "", candidate.comment);
		if (dynamic_lighting::m_map_light_overrides_enabled)
		{
			if (const auto* animation_override = source_map_light_overrides::find_best(g_map_light_overrides,
				static_cast<std::int32_t>(candidate.source_index), candidate.map_hammer_id, candidate.map_targetname,
				candidate.map_light_classname.empty() ? candidate.classname : candidate.map_light_classname))
			{
				apply_map_light_animation_override(def, *animation_override);
			}
		}
		return def;
	}

	static map_settings::remix_light_settings_s make_bsp_preview_light_def(const dynamic_lighting::bsp_light_candidate_s& candidate, const char* comment_prefix)
	{
		auto def = make_light_def_from_bsp_candidate(candidate, comment_prefix);
		if (!def.points.empty())
		{
			auto& pt = def.points.front();
			if (dynamic_lighting::m_source_bsp_preview_force_sphere)
			{
				// Preview should answer one question first: "is there a visible Remix light here?"
				// Source spot directions are easy to convert incorrectly, so keep preview spherical by default.
				pt.use_shaping = false;
				pt.direction = Vector(0.0f, 0.0f, -1.0f);
				pt.degrees = 180.0f;
				pt.softness = 0.0f;
				pt.exponent = 0.0f;
			}

			if (dynamic_lighting::m_source_bsp_preview_boost)
			{
				pt.radiance_scalar *= std::max(0.0f, dynamic_lighting::m_source_bsp_preview_boost_scalar);
				pt.radius *= std::max(0.01f, dynamic_lighting::m_source_bsp_preview_boost_radius);
			}
		}

		return def;
	}

	void dynamic_lighting::clear_bsp_preview_lights()
	{
		if (remix_lights::get())
		{
			remix_lights::get()->destroy_lights_with_comment_prefix("BSP preview:");
		}

		m_source_bsp_preview_live_count = 0u;
		if (!m_source_bsp_scan_status.empty())
		{
			m_source_bsp_scan_status = "cleared BSP preview lights";
		}
	}

	std::uint32_t dynamic_lighting::get_selected_bsp_candidate_count()
	{
		return static_cast<std::uint32_t>(std::count_if(m_source_bsp_candidates.begin(), m_source_bsp_candidates.end(), [](const auto& candidate) {
			return candidate.selected;
		}));
	}

	std::uint32_t dynamic_lighting::get_near_camera_bsp_candidate_count()
	{
		return static_cast<std::uint32_t>(std::count_if(m_source_bsp_candidates.begin(), m_source_bsp_candidates.end(), [](const auto& candidate) {
			return candidate.near_camera;
		}));
	}

	void dynamic_lighting::select_all_bsp_candidates(const bool selected)
	{
		for (auto& candidate : m_source_bsp_candidates) {
			candidate.selected = selected;
		}
		m_source_bsp_import_status = selected ? "selected all BSP candidates" : "deselected all BSP candidates";
	}

	void dynamic_lighting::select_near_camera_bsp_candidates(const bool selected)
	{
		for (auto& candidate : m_source_bsp_candidates)
		{
			if (candidate.near_camera) {
				candidate.selected = selected;
			}
		}
		m_source_bsp_import_status = selected ? "selected near-camera BSP candidates" : "deselected near-camera BSP candidates";
	}

	void dynamic_lighting::select_nearest_bsp_candidate_to_camera()
	{
		if (m_source_bsp_candidates.empty())
		{
			m_source_bsp_selected_index = -1;
			m_source_bsp_import_status = "no BSP candidates";
			return;
		}

		const auto* view_origin = game::get_current_view_origin();
		if (!view_origin)
		{
			m_source_bsp_import_status = "no camera origin for nearest candidate";
			return;
		}

		float best_dist_sqr = FLT_MAX;
		int best_index = -1;
		for (std::size_t i = 0u; i < m_source_bsp_candidates.size(); ++i)
		{
			const float dist_sqr = view_origin->DistToSqr(m_source_bsp_candidates[i].origin);
			if (dist_sqr < best_dist_sqr)
			{
				best_dist_sqr = dist_sqr;
				best_index = static_cast<int>(i);
			}
		}

		m_source_bsp_selected_index = best_index;
		m_source_bsp_import_status = best_index >= 0 ? std::format("selected nearest BSP candidate #{}", best_index) : "no nearest BSP candidate";
	}

	bool dynamic_lighting::preview_bsp_candidate(const int index)
	{
		if (index < 0 || index >= static_cast<int>(m_source_bsp_candidates.size()))
		{
			m_source_bsp_import_status = "invalid BSP candidate index";
			return false;
		}

		if (!remix_api::is_initialized() || !remix_lights::get())
		{
			m_source_bsp_import_status = "Remix API not ready for BSP candidate preview";
			return false;
		}

		auto def = make_bsp_preview_light_def(m_source_bsp_candidates[index], "BSP preview: selected: ");
		const bool spawned = remix_lights::get()->add_single_map_setting_light_report(&def);
		if (spawned)
		{
			++m_source_bsp_preview_live_count;
			++m_source_bsp_preview_create_success;
			m_source_bsp_import_status = std::format("previewed BSP candidate #{}", index);
		}
		else
		{
			++m_source_bsp_preview_create_failed;
			m_source_bsp_import_status = std::format("failed to preview BSP candidate #{}", index);
		}
		return spawned;
	}

	bool dynamic_lighting::preview_selected_bsp_candidate()
	{
		return preview_bsp_candidate(m_source_bsp_selected_index);
	}

	bool dynamic_lighting::spawn_bsp_camera_probe_light()
	{
		if (!remix_api::is_initialized() || !remix_lights::get())
		{
			m_source_bsp_import_status = "Remix API not ready for BSP camera probe";
			++m_source_bsp_preview_create_failed;
			return false;
		}

		const auto* view_origin = game::get_current_view_origin();
		const auto* view_forward = game::get_current_view_forward();
		if (!view_origin || !view_forward)
		{
			m_source_bsp_import_status = "no camera origin/forward for BSP camera probe";
			++m_source_bsp_preview_create_failed;
			return false;
		}

		map_settings::remix_light_settings_s def = {};
		const Vector origin = *view_origin + (*view_forward * 96.0f);
		def.points.emplace_back(make_point(origin, Vector(10.0f, 7.5f, 3.5f), 95.0f, 5.0f, 0.0f, 1.0f, false, *view_forward, 180.0f, 0.0f, 0.0f));
		def.run_once = false;
		def.loop = true;
		def.loop_smoothing = true;
		def.comment = "BSP preview: camera visibility probe";

		const bool spawned = remix_lights::get()->add_single_map_setting_light_report(&def);
		if (spawned)
		{
			++m_source_bsp_preview_live_count;
			++m_source_bsp_preview_create_success;
			m_source_bsp_import_status = "spawned bright camera BSP preview probe";
		}
		else
		{
			++m_source_bsp_preview_create_failed;
			m_source_bsp_import_status = "failed to spawn bright camera BSP preview probe";
		}

		return spawned;
	}

	std::string dynamic_lighting::build_bsp_candidate_light_toml(const bsp_light_candidate_s& candidate)
	{
		std::string text = std::format("        # {}\n", candidate.comment);
		text += std::format("        {{ points = [ {{ position = [{:.3f}, {:.3f}, {:.3f}], radiance = [{:.3f}, {:.3f}, {:.3f}], scalar = {:.3f}, radius = {:.3f}, smoothness = 0.0",
			candidate.origin.x, candidate.origin.y, candidate.origin.z,
			candidate.radiance.x, candidate.radiance.y, candidate.radiance.z,
			candidate.scalar, candidate.radius);
		if (candidate.shaped)
		{
			text += std::format(", direction = [{:.4f}, {:.4f}, {:.4f}], degrees = {:.2f}, softness = {:.2f}, exponent = {:.2f}",
				candidate.direction.x, candidate.direction.y, candidate.direction.z,
				candidate.degrees, candidate.softness, candidate.exponent);
		}
		text += std::format(" }} ], comment = \"{}\" }},", toml_escape_inline_string_dyn(candidate.comment));
		return text;
	}

	std::string dynamic_lighting::build_bsp_import_toml(const bool selected_only, const bool near_camera_only, const std::uint32_t limit)
	{
		const auto map = map_settings::get_map_name();
		std::string text;
		text += std::format("# BSP Light Importer export for {}\n", map.empty() ? "<unknown map>" : map);
		text += "# Paste the entries into [LIGHTS] for the current map after reviewing them.\n";
		text += "# This export is intentionally conservative: it contains only filtered/selected candidates.\n\n";
		text += "[LIGHTS]\n";
		text += std::format("    {} = [\n", map.empty() ? "current_map" : map);

		std::uint32_t emitted = 0u;
		for (const auto& candidate : m_source_bsp_candidates)
		{
			if (selected_only && !candidate.selected) {
				continue;
			}
			if (near_camera_only && !candidate.near_camera) {
				continue;
			}
			if (limit > 0u && emitted >= limit) {
				break;
			}

			text += build_bsp_candidate_light_toml(candidate);
			text += "\n\n";
			++emitted;
		}

		text += "    ]\n";
		text += std::format("# Emitted {} BSP light candidates.\n", emitted);
		return text;
	}

	bool dynamic_lighting::append_bsp_import_export(const bool selected_only, const bool near_camera_only, const std::uint32_t limit)
	{
		const std::string out_dir = game::root_path + COMPMOD_ASSET_DIR "logs\\";
		std::filesystem::create_directories(out_dir);
		const std::string out_path = out_dir + "mapsettings_workbench_export.toml";

		std::ofstream out(out_path, std::ios::out | std::ios::app);
		if (!out.is_open())
		{
			m_source_bsp_import_status = "could not open mapsettings_workbench_export.toml";
			return false;
		}

		out << "\n\n# -----------------------------------------------------------------------------\n";
		out << "# BSP Light Importer export\n";
		out << "# Map: " << map_settings::get_map_name() << "\n";
		out << "# selected_only=" << (selected_only ? "true" : "false")
			<< " near_camera_only=" << (near_camera_only ? "true" : "false")
			<< " limit=" << limit << "\n";
		out << build_bsp_import_toml(selected_only, near_camera_only, limit) << "\n";

		m_source_bsp_import_status = std::format("appended BSP import to {}", out_path);
		return true;
	}

	bool dynamic_lighting::scan_current_bsp_light_entities()
	{
		m_source_bsp_scan_entities = 0u;
		m_source_bsp_scan_lights = 0u;
		m_source_bsp_scan_previewed = 0u;
		m_source_bsp_scan_skipped_by_filter = 0u;
		m_source_bsp_scan_skipped_by_distance = 0u;
		m_source_bsp_preview_create_success = 0u;
		m_source_bsp_preview_create_failed = 0u;
		m_source_bsp_scan_output_path.clear();
		m_source_bsp_candidates.clear();
		m_source_bsp_selected_index = -1;
		m_source_bsp_import_status = "not imported";

		if (m_source_bsp_scan_spawn_helpers && m_source_bsp_scan_clear_previous_preview)
		{
			clear_bsp_preview_lights();
		}

		if (m_source_bsp_scan_spawn_helpers)
		{
			++m_source_bsp_preview_generation;
		}

		const auto map = map_settings::get_map_name();
		if (map.empty())
		{
			m_source_bsp_scan_status = "no current map name";
			return false;
		}

		source_bsp_lights::map_file_s map_file = {};
		std::string error;
		if (!source_bsp_lights::load_map_file(game::root_path, map, map_file, error))
		{
			m_source_bsp_scan_status = error.empty() ? "BSP not found" : error;
			return false;
		}

		std::string entity_lump;
		if (!source_bsp_lights::read_entity_lump(map_file, entity_lump, error))
		{
			m_source_bsp_scan_status = error.empty() ? "empty entity lump" : error;
			return false;
		}

		const auto blocks = split_entity_blocks(entity_lump);
		m_source_bsp_scan_entities = static_cast<std::uint32_t>(blocks.size());

		const std::string out_dir = game::root_path + COMPMOD_ASSET_DIR "logs\\";
		std::filesystem::create_directories(out_dir);
		const std::string out_path = out_dir + "bsp_light_candidates_" + map + ".toml";
		std::ofstream out(out_path, std::ios::out | std::ios::trunc);
		if (!out.is_open())
		{
			m_source_bsp_scan_status = "could not open output file";
			return false;
		}

		out << "# Auto-generated Source BSP light entity candidates for " << map << "\n";
		out << "# This is a starting point only: paste useful entries into [LIGHTS] or convert to [LIGHT_EVENTS].\n";
		out << "# Source baked lightmaps are not copied here; only entity lump lights are scanned.\n";
		out << "# Scan settings: scalar_scale=" << m_source_bsp_scan_scalar_scale
			<< " radius_scale=" << m_source_bsp_scan_radius_scale
			<< " min_scalar=" << m_source_bsp_scan_min_scalar
			<< " max_scalar=" << m_source_bsp_scan_max_scalar << "\n";
		out << "# Filters: point=" << (m_source_bsp_scan_include_point ? "true" : "false")
			<< " spot=" << (m_source_bsp_scan_include_spot ? "true" : "false")
			<< " projected=" << (m_source_bsp_scan_include_projected ? "true" : "false")
			<< " sprite=" << (m_source_bsp_scan_include_sprite ? "true" : "false") << "\n";
		out << "# Preview: enabled=" << (m_source_bsp_scan_spawn_helpers ? "true" : "false")
			<< " clear_previous=" << (m_source_bsp_scan_clear_previous_preview ? "true" : "false")
			<< " near_camera=" << (m_source_bsp_scan_preview_near_camera ? "true" : "false")
			<< " max_distance=" << m_source_bsp_scan_preview_max_distance
			<< " limit=" << m_source_bsp_scan_preview_limit
			<< " force_sphere=" << (m_source_bsp_preview_force_sphere ? "true" : "false")
			<< " boost=" << (m_source_bsp_preview_boost ? "true" : "false")
			<< " boost_scalar=" << m_source_bsp_preview_boost_scalar
			<< " boost_radius=" << m_source_bsp_preview_boost_radius << "\n\n";
		out << "[LIGHTS]\n";
		out << "    " << map << " = [\n";

		const auto max_preview = static_cast<std::uint32_t>(std::max(0, m_source_bsp_scan_preview_limit));
		std::uint32_t source_index = 0u;
		for (const auto& block : blocks)
		{
			const auto kv = parse_entity_key_values(block);
			auto cls_it = kv.find("classname");
			if (cls_it == kv.end()) {
				continue;
			}

			const auto cls = utils::str_to_lower(cls_it->second);
			const bool is_light = cls == "light" || cls == "light_spot" || cls == "point_spotlight" || cls == "env_projectedtexture" || cls == "env_sprite";
			if (!is_light) {
				continue;
			}

			++source_index;
			if (!source_light_class_allowed(cls))
			{
				++m_source_bsp_scan_skipped_by_filter;
				continue;
			}

			Vector origin = Vector(0.0f, 0.0f, 0.0f);
			auto origin_it = kv.find("origin");
			if (origin_it == kv.end() || !parse_vec3_string(origin_it->second, origin)) {
				continue;
			}

			Vector radiance;
			float scalar;
			parse_source_light_value(kv, radiance, scalar);
			scalar *= std::max(0.0f, m_source_bsp_scan_scalar_scale);
			scalar = std::clamp(scalar, std::max(0.0f, m_source_bsp_scan_min_scalar), std::max(m_source_bsp_scan_min_scalar, m_source_bsp_scan_max_scalar));

			bool shaped = false;
			Vector direction = Vector(0.0f, 0.0f, -1.0f);
			float degrees = 180.0f;
			float softness = 0.0f;
			float exponent = 0.0f;
			if (cls == "light_spot" || cls == "point_spotlight" || cls == "env_projectedtexture")
			{
				shaped = true;
				Vector angles = Vector(90.0f, 0.0f, 0.0f);
				auto angles_it = kv.find("angles");
				if (angles_it != kv.end()) {
					parse_vec3_string(angles_it->second, angles);
				}
				direction = direction_from_source_angles(angles);

				auto cone_it = kv.find("_cone");
				if (cone_it != kv.end()) {
					degrees = std::clamp(strtof(cone_it->second.c_str(), nullptr), 1.0f, 179.0f);
				}
				else {
					degrees = cls == "env_projectedtexture" ? 58.0f : 70.0f;
				}
				softness = cls == "env_projectedtexture" ? 0.22f : 0.16f;
				exponent = cls == "env_projectedtexture" ? 0.95f : 0.75f;
			}
			else if (cls == "env_sprite")
			{
				// Sprites/glows are usually a visual hint for emissive helpers. Keep them soft and modest.
				scalar *= 0.38f;
			}

			float radius = source_light_radius_from_keys(kv, shaped) * std::max(0.01f, m_source_bsp_scan_radius_scale);
			radius = std::clamp(radius, shaped ? 0.35f : 0.50f, shaped ? 16.0f : 18.0f);

			const std::string source_comment = std::format("Source entity #{}: {}{}", source_index, cls, source_targetname_comment(kv));

			bool is_near_camera = false;
			float camera_distance = 0.0f;
			if (const auto* view_origin = game::get_current_view_origin())
			{
				camera_distance = view_origin->DistTo(origin);
				is_near_camera = camera_distance <= std::max(0.0f, m_source_bsp_scan_preview_max_distance);
			}

			bsp_light_candidate_s candidate = {};
			candidate.source_index = source_index;
			candidate.classname = cls;
			candidate.targetname = source_targetname_value(kv);
			candidate.comment = source_comment;
			candidate.origin = origin;
			candidate.radiance = radiance;
			candidate.scalar = scalar;
			candidate.radius = radius;
			candidate.shaped = shaped;
			candidate.direction = direction;
			candidate.degrees = degrees;
			candidate.softness = softness;
			candidate.exponent = exponent;
			candidate.selected = cls != "env_sprite"; // sprites/glows are useful, but too noisy for default import.
			candidate.near_camera = is_near_camera;
			candidate.camera_distance = camera_distance;
			m_source_bsp_candidates.emplace_back(candidate);
			if (m_source_bsp_selected_index < 0) {
				m_source_bsp_selected_index = 0;
			}

			out << "        # " << source_comment << "\n";

			out << std::format("        {{ points = [ {{ position = [{:.3f}, {:.3f}, {:.3f}], radiance = [{:.3f}, {:.3f}, {:.3f}], scalar = {:.3f}, radius = {:.3f}, smoothness = 0.0",
				origin.x, origin.y, origin.z, radiance.x, radiance.y, radiance.z, scalar, radius);
			if (shaped)
			{
				out << std::format(", direction = [{:.4f}, {:.4f}, {:.4f}], degrees = {:.2f}, softness = {:.2f}, exponent = {:.2f}",
					direction.x, direction.y, direction.z, degrees, softness, exponent);
			}
			out << " } ], comment = \"" << toml_escape_inline_string_dyn(source_comment) << "\" },\n\n";

			if (m_source_bsp_scan_spawn_helpers && remix_api::is_initialized() && remix_lights::get() && m_source_bsp_scan_previewed < max_preview)
			{
				bool can_preview = true;
				if (m_source_bsp_scan_preview_near_camera)
				{
					const auto* view_origin = game::get_current_view_origin();
					const float max_dist = std::max(0.0f, m_source_bsp_scan_preview_max_distance);
					can_preview = view_origin && view_origin->DistToSqr(origin) <= max_dist * max_dist;
					if (!can_preview) {
						++m_source_bsp_scan_skipped_by_distance;
					}
				}

				if (can_preview)
				{
					auto def = make_bsp_preview_light_def(candidate, std::format("BSP preview: gen {}: ", m_source_bsp_preview_generation).c_str());
					const bool spawned = remix_lights::get()->add_single_map_setting_light_report(&def);
					if (spawned)
					{
						++m_source_bsp_scan_previewed;
						++m_source_bsp_preview_live_count;
						++m_source_bsp_preview_create_success;
					}
					else
					{
						++m_source_bsp_preview_create_failed;
					}
				}
			}

			++m_source_bsp_scan_lights;
		}

		out << "    ]\n";
		out.close();

		m_source_bsp_scan_output_path = out_path;
		m_source_bsp_scan_status = std::format("scanned {} entities, exported {} candidates, previewed {} (CreateLight ok {}, failed {}), skipped {} by filter, skipped {} by distance",
			m_source_bsp_scan_entities, m_source_bsp_scan_lights, m_source_bsp_scan_previewed, m_source_bsp_preview_create_success, m_source_bsp_preview_create_failed, m_source_bsp_scan_skipped_by_filter, m_source_bsp_scan_skipped_by_distance);
		return m_source_bsp_scan_lights > 0u;
	}


	static bool bsp_worldlight_type_allowed(const source_bsp_lights::emit_type type)
	{
		switch (type)
		{
		case source_bsp_lights::emit_type::surface:
			return dynamic_lighting::m_bsp_worldlight_include_surface;
		case source_bsp_lights::emit_type::point:
			return dynamic_lighting::m_bsp_worldlight_include_point;
		case source_bsp_lights::emit_type::spotlight:
			return dynamic_lighting::m_bsp_worldlight_include_spot;
		case source_bsp_lights::emit_type::quakelight:
			return dynamic_lighting::m_bsp_worldlight_include_quake;
		case source_bsp_lights::emit_type::skylight:
		case source_bsp_lights::emit_type::skyambient:
			// Keep environment records visible in the Source Direct registry. Their local
			// helper remains disabled unless explicitly requested by the user.
			return dynamic_lighting::m_bsp_worldlight_include_environment_records;
		default:
			return false;
		}
	}

	static bool bsp_candidate_is_environment_record(const dynamic_lighting::bsp_light_candidate_s& candidate)
	{
		const auto type = static_cast<source_bsp_lights::emit_type>(candidate.source_type);
		return type == source_bsp_lights::emit_type::skylight || type == source_bsp_lights::emit_type::skyambient;
	}

	static bool bsp_candidate_is_registry_only_environment(const dynamic_lighting::bsp_light_candidate_s& candidate)
	{
		return bsp_candidate_is_environment_record(candidate) && !candidate.selected && !candidate.override_applied;
	}

	static void capture_candidate_editor_baseline(dynamic_lighting::bsp_light_candidate_s& candidate);
	static void apply_candidate_override(dynamic_lighting::bsp_light_candidate_s& candidate,
		const source_map_light_overrides::light_override_s& override_data);

	constexpr std::uint32_t k_source_distant_candidate_index = 0x7f000001u;

	static bool ensure_source_environment_graph(const std::string& map)
	{
		if (g_map_entity_graph_loaded && g_map_entity_graph_map == map) return true;
		if (g_source_environment_graph_loaded && g_source_environment_graph_map == map) return true;

		g_source_environment_graph = {};
		g_source_environment_graph_map.clear();
		g_source_environment_graph_loaded = false;
		source_bsp_lights::map_file_s map_file = {};
		std::string error;
		if (!source_bsp_lights::load_map_file(game::root_path, map, map_file, error)) return false;
		std::string entity_lump;
		if (!source_bsp_lights::read_entity_lump(map_file, entity_lump, error)) return false;
		if (!source_map_entities::parse_entity_lump(entity_lump, g_source_environment_graph, error)) return false;
		g_source_environment_graph_loaded = true;
		g_source_environment_graph_map = map;
		return true;
	}

	static const source_map_entities::graph_s* source_environment_graph()
	{
		std::string map = normalize_map_session_name(dynamic_lighting::get_bsp_worldlight_pending_map());
		if (map.empty()) map = normalize_map_session_name(map_settings::get_map_name());
		if (g_map_entity_graph_loaded && (map.empty() || g_map_entity_graph_map == map)) return &g_map_entity_graph;
		if (g_source_environment_graph_loaded && (map.empty() || g_source_environment_graph_map == map))
			return &g_source_environment_graph;
		return nullptr;
	}

	static const std::string* source_entity_value(const source_map_entities::entity_s& entity, const std::string_view key)
	{
		for (auto it = entity.keyvalues.rbegin(); it != entity.keyvalues.rend(); ++it)
		{
			if (it->first == key) return &it->second;
		}
		return nullptr;
	}

	static bool parse_source_entity_float(const source_map_entities::entity_s& entity, const std::string_view key, float& out)
	{
		const auto* value = source_entity_value(entity, key);
		if (!value || value->empty()) return false;
		char* end = nullptr;
		const float parsed = std::strtof(value->c_str(), &end);
		if (end != value->c_str() + value->size() || !std::isfinite(parsed)) return false;
		out = parsed;
		return true;
	}

	static bool parse_source_entity_vector(const source_map_entities::entity_s& entity,
		const std::string_view key, Vector& out)
	{
		const auto* value = source_entity_value(entity, key);
		if (!value || value->empty()) return false;
		float x = 0.0f, y = 0.0f, z = 0.0f;
		if (std::sscanf(value->c_str(), "%f %f %f", &x, &y, &z) != 3) return false;
		if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
		out = Vector(x, y, z);
		return out.LengthSqr() > 0.000001f;
	}

	struct source_sun_color_s
	{
		Vector color = Vector(1.0f, 1.0f, 1.0f);
		float strength = 0.0f;
		std::string source;
		bool used_hdr = false;
		bool valid = false;
	};

	static bool parse_source_sun_color_text(const std::string& text, Vector& out_color, float& out_strength)
	{
		float r = 0.0f;
		float g = 0.0f;
		float b = 0.0f;
		float brightness = 0.0f;
		const int parsed = std::sscanf(text.c_str(), "%f %f %f %f", &r, &g, &b, &brightness);
		if (parsed < 3 || !std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) ||
			r < 0.0f || g < 0.0f || b < 0.0f) return false;
		if (parsed >= 4 && (!std::isfinite(brightness) || brightness <= 0.0f)) return false;

		// VRAD's LightForKey converts the authored 0..255 RGB channels to linear space and,
		// when a fourth value exists, treats it as an intensity scaler. WORLDLIGHTS are later
		// exported with a 1/255 scale, so reproducing that exact normalization keeps the entity
		// fallback in the same magnitude domain as the compiled skylight record.
		auto decode_channel = [](const float value)
		{
			const float normalized = std::max(0.0f, value / 255.0f);
			return dynamic_lighting::m_source_distant_linearize_entity_color
				? std::pow(normalized, 2.2f) : normalized;
		};

		Vector linear(decode_channel(r), decode_channel(g), decode_channel(b));
		if (parsed >= 4) linear *= brightness / 255.0f;
		const float peak = std::max({ linear.x, linear.y, linear.z });
		if (!std::isfinite(peak) || peak <= 0.000001f) return false;
		out_color = linear * (1.0f / peak);
		out_strength = peak;
		return true;
	}

	static source_sun_color_s resolve_source_entity_color(const source_map_entities::entity_s& entity,
		const bool prefer_hdr, const std::string_view ldr_key, const std::string_view hdr_key,
		const std::string_view hdr_scale_key, const std::string_view label)
	{
		source_sun_color_s result = {};
		auto try_key = [&](const std::string_view key, const bool hdr)
		{
			const auto* value = source_entity_value(entity, key);
			Vector color;
			float strength = 0.0f;
			if (!value || !parse_source_sun_color_text(*value, color, strength)) return false;
			float scale = 1.0f;
			if (hdr)
			{
				if (!parse_source_entity_float(entity, hdr_scale_key, scale))
				{
					// Several Source branches and older FGD files use the reversed spelling.
					if (hdr_scale_key == "_lightscalehdr") parse_source_entity_float(entity, "_lighthdrscale", scale);
					else if (hdr_scale_key == "_ambientscalehdr") parse_source_entity_float(entity, "_ambienthdrscale", scale);
				}
			}
			result.color = color;
			result.strength = strength * std::max(0.0f, scale);
			result.source = std::format("{} {} {}", entity.classname, label, hdr ? "HDR" : "LDR");
			result.used_hdr = hdr;
			result.valid = std::isfinite(result.strength) && result.strength > 0.0f;
			return result.valid;
		};

		if (prefer_hdr && try_key(hdr_key, true)) return result;
		if (try_key(ldr_key, false)) return result;
		if (!prefer_hdr) try_key(hdr_key, true);
		return result;
	}

	static source_sun_color_s resolve_source_sun_entity_color(const source_map_entities::entity_s& entity,
		const bool prefer_hdr)
	{
		auto result = resolve_source_entity_color(entity, prefer_hdr, "_light", "_lighthdr", "_lightscalehdr", "direct");
		if (result.valid) return result;

		auto try_color_alias = [&](const std::string_view key, const std::string_view label)
		{
			const auto* value = source_entity_value(entity, key);
			Vector color;
			float strength = 0.0f;
			if (!value || !parse_source_sun_color_text(*value, color, strength)) return false;
			float hdr_scale = 1.0f;
			if (prefer_hdr) parse_source_entity_float(entity, "hdrcolorscale", hdr_scale);
			result.color = color;
			result.strength = strength * std::max(0.0f, hdr_scale);
			result.source = std::format("{} {}", entity.classname, label);
			result.used_hdr = prefer_hdr;
			result.valid = result.strength > 0.000001f;
			return result.valid;
		};

		if (entity.classname == "env_cascade_light")
		{
			if (try_color_alias("lightcolor", "lightcolor")) return result;
			if (try_color_alias("color", "color")) return result;
		}
		if (entity.classname == "env_sun")
		{
			if (try_color_alias("rendercolor", "visual rendercolor")) return result;
		}
		return result;
	}

	static source_sun_color_s resolve_source_ambient_entity_color(const source_map_entities::entity_s& entity,
		const bool prefer_hdr)
	{
		auto result = resolve_source_entity_color(entity, prefer_hdr, "_ambient", "_ambienthdr",
			"_ambientscalehdr", "ambient");
		if (result.valid) return result;

		// Stock VRAD falls back to half of the direct light when no explicit ambient value exists.
		const auto direct = resolve_source_sun_entity_color(entity, prefer_hdr);
		if (!direct.valid) return result;
		result = direct;
		result.strength *= 0.5f;
		result.source = std::format("{} ambient fallback (0.5 x direct)", entity.classname);
		return result;
	}

	static bool resolve_source_sun_entity_direction(const source_map_entities::entity_s& entity,
		const source_map_entities::graph_s* graph, Vector& out_direction, std::string& out_source)
	{
		for (const auto key : { "direction", "sun_direction", "shadowdirection", "shadow_direction" })
		{
			if (parse_source_entity_vector(entity, key, out_direction) && normalize_source_direction(out_direction))
			{
				out_source = std::format("{} {}", entity.classname, key);
				return true;
			}
		}

		if (dynamic_lighting::m_source_distant_use_target_direction && graph && entity.has_origin && !entity.target.empty())
		{
			if (const auto* target = source_map_entities::find_by_targetname(*graph, entity.target);
				target && target->has_origin)
			{
				// env_sun positions its visual disk on the ray from target toward the entity.
				// Actual light entities use the normal entity-to-target convention.
				out_direction = entity.classname == "env_sun"
					? entity.origin - target->origin : target->origin - entity.origin;
				if (normalize_source_direction(out_direction))
				{
					out_source = std::format("{} target '{}'", entity.classname, entity.target);
					return true;
				}
			}
		}

		Vector angles = entity.has_angles ? entity.angles : Vector(0.0f, 0.0f, 0.0f);
		bool has_direction_property = entity.has_angles;
		float legacy_angle = 0.0f;
		if (parse_source_entity_float(entity, "angle", legacy_angle))
		{
			has_direction_property = true;
			if (std::fabs(legacy_angle + 1.0f) <= 0.001f)
			{
				out_direction = Vector(0.0f, 0.0f, 1.0f);
				out_source = std::format("{} legacy ANGLE_UP", entity.classname);
				return true;
			}
			if (std::fabs(legacy_angle + 2.0f) <= 0.001f)
			{
				out_direction = Vector(0.0f, 0.0f, -1.0f);
				out_source = std::format("{} legacy ANGLE_DOWN", entity.classname);
				return true;
			}
			angles.y = legacy_angle;
		}

		float special_pitch = 0.0f;
		if (parse_source_entity_float(entity, "pitch", special_pitch))
		{
			// light_environment uses the inverse of normal Source pitch: -90 points down.
			angles.x = -special_pitch;
			has_direction_property = true;
			out_source = std::format("{} pitch + yaw", entity.classname);
		}
		else if (has_direction_property)
		{
			out_source = std::format("{} angles", entity.classname);
		}
		else
		{
			return false;
		}

		utils::vector::AngleVectors(angles, &out_direction);
		return normalize_source_direction(out_direction);
	}

	static float source_color_similarity(Vector a, Vector b)
	{
		if (!normalize_source_direction(a) || !normalize_source_direction(b)) return 0.0f;
		return std::clamp(a.Dot(b), 0.0f, 1.0f);
	}

	struct source_compiled_environment_record_s
	{
		source_bsp_lights::world_light_s light;
		std::string source;
		bool used_hdr = false;
		bool runtime = false;
	};

	static bool same_environment_record(const source_compiled_environment_record_s& lhs,
		const source_compiled_environment_record_s& rhs)
	{
		if (lhs.light.type != rhs.light.type || lhs.light.style != rhs.light.style ||
			lhs.used_hdr != rhs.used_hdr) return false;
		return (lhs.light.normal - rhs.light.normal).LengthSqr() <= 0.000001f &&
			(lhs.light.intensity - rhs.light.intensity).LengthSqr() <= 0.000001f;
	}

	static std::vector<source_compiled_environment_record_s> collect_source_environment_records(
		const source_bsp_lights::world_light_result_s& primary)
	{
		std::vector<source_compiled_environment_record_s> records;
		dynamic_lighting::m_source_distant_hdr_sets_found = 0u;
		dynamic_lighting::m_source_distant_ldr_sets_found = 0u;

		auto append_result = [&](const source_bsp_lights::world_light_result_s& set, const std::string_view label)
		{
			bool has_environment = false;
			for (const auto& light : set.lights)
			{
				if (light.type != source_bsp_lights::emit_type::skylight &&
					light.type != source_bsp_lights::emit_type::skyambient) continue;
				has_environment = true;
				source_compiled_environment_record_s record = {};
				record.light = light;
				record.used_hdr = set.used_hdr;
				record.runtime = set.backend == source_bsp_lights::source_backend::engine_memory;
				record.source = std::format("{} {}", label, set.used_hdr ? "HDR" : "LDR");
				auto duplicate = std::find_if(records.begin(), records.end(),
					[&](const auto& existing) { return same_environment_record(existing, record); });
				if (duplicate == records.end())
				{
					records.emplace_back(std::move(record));
				}
				else if (duplicate->runtime && !record.runtime)
				{
					*duplicate = std::move(record);
				}
			}
			if (has_environment)
			{
				if (set.used_hdr) ++dynamic_lighting::m_source_distant_hdr_sets_found;
				else ++dynamic_lighting::m_source_distant_ldr_sets_found;
			}
		};

		append_result(primary, primary.backend == source_bsp_lights::source_backend::engine_memory
			? "engine WORLDLIGHTS" : "primary BSP WORLDLIGHTS");

		if (dynamic_lighting::m_source_distant_compare_bsp_sets)
		{
			std::string map = normalize_map_session_name(dynamic_lighting::get_bsp_worldlight_pending_map());
			if (map.empty()) map = normalize_map_session_name(map_settings::get_map_name());
			source_bsp_lights::map_file_s map_file = {};
			std::string error;
			if (!map.empty() && source_bsp_lights::load_map_file(game::root_path, map, map_file, error))
			{
				source_bsp_lights::world_light_result_s hdr = {};
				if (source_bsp_lights::read_world_lights(map_file, true, hdr) && hdr.used_hdr)
					append_result(hdr, "BSP WORLDLIGHTS");
				source_bsp_lights::world_light_result_s ldr = {};
				if (source_bsp_lights::read_world_lights(map_file, false, ldr) && !ldr.used_hdr)
					append_result(ldr, "BSP WORLDLIGHTS");
			}
		}
		return records;
	}

	struct source_distant_resolution_s
	{
		Vector origin = Vector(0.0f, 0.0f, 0.0f);
		Vector direction = Vector(0.0f, 0.0f, -1.0f);
		Vector color = Vector(1.0f, 1.0f, 1.0f);
		Vector ambient_color = Vector(1.0f, 1.0f, 1.0f);
		float scalar = 0.0f;
		float ambient_scalar = 0.0f;
		float angular_diameter = 0.53f;
		float confidence = 0.0f;
		std::uint32_t entity_source_index = 0u;
		std::int32_t hammer_id = -1;
		std::string classname = "light_environment";
		std::string targetname;
		std::string direction_source;
		std::string color_source;
		std::string spread_source;
		std::string ambient_source;
		std::string worldlight_source;
		float direction_disagreement = 0.0f;
		float color_similarity = 0.0f;
		bool used_worldlight = false;
		bool used_entity = false;
		bool used_hdr = false;
		bool worldlight_flipped = false;
		bool ambient_valid = false;
		bool synthetic_radiance = false;
		bool valid = false;
	};

	static source_distant_resolution_s resolve_source_distant_light(const source_bsp_lights::world_light_result_s& parsed)
	{
		source_distant_resolution_s result = {};
		dynamic_lighting::m_source_distant_entities_found = 0u;
		dynamic_lighting::m_source_environment_light_environment_found = 0u;
		dynamic_lighting::m_source_environment_light_directional_found = 0u;
		dynamic_lighting::m_source_environment_cascade_found = 0u;
		dynamic_lighting::m_source_environment_shadow_control_found = 0u;
		dynamic_lighting::m_source_environment_env_sun_found = 0u;
		dynamic_lighting::m_source_environment_selected_alias = "none";
		dynamic_lighting::m_source_distant_worldlights_found = 0u;
		dynamic_lighting::m_source_distant_skyambient_found = 0u;
		dynamic_lighting::m_source_distant_candidates_rejected = 0u;
		dynamic_lighting::m_source_distant_selected_confidence = 0.0f;
		dynamic_lighting::m_source_distant_direction_disagreement = 0.0f;
		dynamic_lighting::m_source_distant_selected_hdr = false;
		dynamic_lighting::m_source_distant_sign_flipped = false;
		dynamic_lighting::m_source_distant_diagnostics.clear();

		const auto* environment_graph = source_environment_graph();
		const source_map_entities::entity_s* first_environment = nullptr;
		const source_map_entities::entity_s* best_entity = nullptr;
		float best_entity_score = -1.0e9f;
		if (environment_graph)
		{
			for (const auto& entity : environment_graph->entities)
			{
				if (!source_map_entities::class_is_environment_light(entity.classname)) continue;
				float score = -1.0e9f;
				if (entity.classname == "light_environment")
				{
					++dynamic_lighting::m_source_environment_light_environment_found;
					if (!first_environment) first_environment = &entity;
					score = 200.0f + (&entity == first_environment ? 100.0f : 0.0f);
				}
				else if (entity.classname == "light_directional")
				{
					++dynamic_lighting::m_source_environment_light_directional_found;
					score = 150.0f;
				}
				else if (entity.classname == "env_cascade_light")
				{
					++dynamic_lighting::m_source_environment_cascade_found;
					if (!dynamic_lighting::m_source_environment_use_env_cascade) continue;
					score = 110.0f;
				}
				else if (entity.classname == "shadow_control")
				{
					++dynamic_lighting::m_source_environment_shadow_control_found;
					if (!dynamic_lighting::m_source_environment_use_shadow_control) continue;
					score = 75.0f;
				}
				else if (entity.classname == "env_sun")
				{
					++dynamic_lighting::m_source_environment_env_sun_found;
					if (!dynamic_lighting::m_source_environment_use_env_sun) continue;
					score = 45.0f;
				}
				else continue;

				++dynamic_lighting::m_source_distant_entities_found;
				if (entity.has_angles || source_entity_value(entity, "pitch") || source_entity_value(entity, "angle") ||
					source_entity_value(entity, "direction") || source_entity_value(entity, "shadowdirection")) score += 20.0f;
				if (!entity.target.empty()) score += 12.0f;
				if (source_entity_value(entity, "_lighthdr") || source_entity_value(entity, "_light") ||
					source_entity_value(entity, "lightcolor") || source_entity_value(entity, "rendercolor")) score += 20.0f;
				if (entity.starts_disabled && entity.classname != "light_environment") score -= 25.0f;
				if (dynamic_lighting::m_source_distant_follow_vrad_first_environment && first_environment &&
					entity.classname == "light_environment" && &entity != first_environment) score -= 150.0f;
				if (score > best_entity_score)
				{
					best_entity_score = score;
					best_entity = &entity;
				}
			}
		}
		if (dynamic_lighting::m_source_distant_follow_vrad_first_environment && first_environment)
			best_entity = first_environment;
		if (best_entity) dynamic_lighting::m_source_environment_selected_alias = best_entity->classname;

		Vector entity_direction;
		std::string entity_direction_source;
		const bool entity_direction_valid = best_entity && dynamic_lighting::m_source_distant_allow_entity_fallback &&
			resolve_source_sun_entity_direction(*best_entity, environment_graph, entity_direction, entity_direction_source);

		const auto records = collect_source_environment_records(parsed);
		std::vector<const source_compiled_environment_record_s*> skylights;
		std::vector<const source_compiled_environment_record_s*> skyambient;
		for (const auto& record : records)
		{
			if (record.light.type == source_bsp_lights::emit_type::skylight) skylights.push_back(&record);
			else if (record.light.type == source_bsp_lights::emit_type::skyambient) skyambient.push_back(&record);
		}
		dynamic_lighting::m_source_distant_worldlights_found = static_cast<std::uint32_t>(skylights.size());
		dynamic_lighting::m_source_distant_skyambient_found = static_cast<std::uint32_t>(skyambient.size());

		const source_compiled_environment_record_s* best_sky = nullptr;
		Vector best_world_direction;
		bool best_world_flipped = false;
		float best_world_score = -1.0e9f;
		float best_world_angle = 180.0f;
		float best_color_similarity = 0.0f;
		std::uint32_t valid_skylight_candidates = 0u;
		for (const auto* candidate : skylights)
		{
			if (!candidate) continue;
			Vector direction = candidate->light.normal;
			if (!normalize_source_direction(direction))
			{
				++dynamic_lighting::m_source_distant_candidates_rejected;
				continue;
			}
			const float peak = std::max({ candidate->light.intensity.x, candidate->light.intensity.y,
				candidate->light.intensity.z });
			if (!std::isfinite(peak) || peak <= 0.000001f)
			{
				++dynamic_lighting::m_source_distant_candidates_rejected;
				continue;
			}
			++valid_skylight_candidates;

			bool flipped = dynamic_lighting::m_source_distant_direction_mode == 2;
			float angle = 0.0f;
			if (dynamic_lighting::m_source_distant_direction_mode == 0 && entity_direction_valid)
			{
				const float direct_angle = source_direction_angle_degrees(direction, entity_direction);
				const float flipped_angle = source_direction_angle_degrees(direction * -1.0f, entity_direction);
				const float required_improvement = std::clamp(
					dynamic_lighting::m_source_distant_sign_flip_min_improvement, 0.0f, 90.0f);
				flipped = direct_angle - flipped_angle >= required_improvement;
				angle = flipped ? flipped_angle : direct_angle;
			}
			else if (entity_direction_valid)
			{
				angle = source_direction_angle_degrees(flipped ? direction * -1.0f : direction, entity_direction);
			}
			if (flipped) direction *= -1.0f;

			float score = 80.0f;
			if (candidate->used_hdr == dynamic_lighting::m_source_distant_prefer_hdr) score += 45.0f;
			if (candidate->light.style == 0) score += 15.0f; else score -= 25.0f;
			if (!candidate->runtime) score += 8.0f;
			score += std::clamp(std::log2(1.0f + peak) * 4.0f, 0.0f, 20.0f);

			float similarity = 0.0f;
			if (best_entity)
			{
				const auto entity_color = resolve_source_sun_entity_color(*best_entity, candidate->used_hdr);
				if (entity_color.valid)
				{
					Vector compiled_color(
						std::max(0.0f, candidate->light.intensity.x) / peak,
						std::max(0.0f, candidate->light.intensity.y) / peak,
						std::max(0.0f, candidate->light.intensity.z) / peak);
					similarity = source_color_similarity(compiled_color, entity_color.color);
					score += similarity * 24.0f;
				}
			}
			if (entity_direction_valid && dynamic_lighting::m_source_distant_match_worldlight_to_entity)
			{
				const float max_pair = std::max(1.0f, dynamic_lighting::m_source_distant_max_pair_angle);
				score += 90.0f * (1.0f - std::clamp(angle / max_pair, 0.0f, 1.0f));
				if (angle > max_pair) score -= 45.0f;
			}

			if (score > best_world_score)
			{
				best_world_score = score;
				best_sky = candidate;
				best_world_direction = direction;
				best_world_flipped = flipped;
				best_world_angle = angle;
				best_color_similarity = similarity;
			}
		}

		if (best_sky && dynamic_lighting::m_source_distant_use_worldlight_direction)
		{
			result.direction = best_world_direction;
			result.direction_source = best_world_flipped
				? std::format("{} normal (reversed)", best_sky->source)
				: std::format("{} normal", best_sky->source);
			result.used_worldlight = true;
			result.worldlight_flipped = best_world_flipped;
			result.worldlight_source = best_sky->source;
		}
		else if (entity_direction_valid)
		{
			result.direction = entity_direction;
			result.direction_source = entity_direction_source;
		}
		else if (best_sky)
		{
			result.direction = best_world_direction;
			result.direction_source = best_world_flipped
				? std::format("{} normal fallback (reversed)", best_sky->source)
				: std::format("{} normal fallback", best_sky->source);
			result.used_worldlight = true;
			result.worldlight_flipped = best_world_flipped;
			result.worldlight_source = best_sky->source;
		}
		else
		{
			return result;
		}

		result.direction_disagreement = entity_direction_valid && best_sky ? best_world_angle : 0.0f;
		result.color_similarity = best_color_similarity;
		result.used_hdr = best_sky ? best_sky->used_hdr : false;

		if (best_entity)
		{
			result.used_entity = true;
			result.entity_source_index = best_entity->source_index;
			result.hammer_id = best_entity->hammer_id;
			result.classname = best_entity->classname;
			result.targetname = best_entity->targetname;
			if (best_entity->has_origin) result.origin = best_entity->origin;

			const auto entity_color = resolve_source_sun_entity_color(*best_entity,
				best_sky ? result.used_hdr : dynamic_lighting::m_source_distant_prefer_hdr);
			if (entity_color.valid)
			{
				if (!best_sky) result.used_hdr = entity_color.used_hdr;
				result.color = entity_color.color;
				result.scalar = entity_color.strength * std::max(0.0f, dynamic_lighting::m_source_distant_intensity_scale);
				result.color_source = entity_color.source;
			}

			float spread = 0.0f;
			if (parse_source_entity_float(*best_entity, "sunspreadangle", spread) && spread > 0.0001f)
			{
				result.angular_diameter = dynamic_lighting::m_source_distant_spread_is_radius ? spread * 2.0f : spread;
				result.spread_source = dynamic_lighting::m_source_distant_spread_is_radius
					? "SunSpreadAngle radius -> diameter" : "SunSpreadAngle";
			}
		}

		if (best_sky)
		{
			const float peak = std::max({ best_sky->light.intensity.x, best_sky->light.intensity.y,
				best_sky->light.intensity.z });
			if (peak > 0.000001f)
			{
				result.color = Vector(
					std::max(0.0f, best_sky->light.intensity.x) / peak,
					std::max(0.0f, best_sky->light.intensity.y) / peak,
					std::max(0.0f, best_sky->light.intensity.z) / peak);
				result.scalar = peak * std::max(0.0f, dynamic_lighting::m_source_distant_intensity_scale);
				result.color_source = std::format("{} compiled intensity", best_sky->source);
				result.used_worldlight = true;
			}
		}

		const source_compiled_environment_record_s* best_ambient = nullptr;
		float best_ambient_peak = -1.0f;
		for (const auto* ambient : skyambient)
		{
			if (!ambient || ambient->used_hdr != result.used_hdr) continue;
			const float peak = std::max({ ambient->light.intensity.x, ambient->light.intensity.y,
				ambient->light.intensity.z });
			if (std::isfinite(peak) && peak > best_ambient_peak)
			{
				best_ambient_peak = peak;
				best_ambient = ambient;
			}
		}
		if (best_ambient && best_ambient_peak > 0.000001f)
		{
			result.ambient_color = Vector(
				std::max(0.0f, best_ambient->light.intensity.x) / best_ambient_peak,
				std::max(0.0f, best_ambient->light.intensity.y) / best_ambient_peak,
				std::max(0.0f, best_ambient->light.intensity.z) / best_ambient_peak);
			result.ambient_scalar = best_ambient_peak * std::max(0.0f, dynamic_lighting::m_source_distant_intensity_scale);
			result.ambient_source = std::format("{} skyambient (diagnostic only)", best_ambient->source);
			result.ambient_valid = true;
		}
		else if (best_entity)
		{
			const auto ambient = resolve_source_ambient_entity_color(*best_entity, result.used_hdr);
			if (ambient.valid)
			{
				result.ambient_color = ambient.color;
				result.ambient_scalar = ambient.strength * std::max(0.0f, dynamic_lighting::m_source_distant_intensity_scale);
				result.ambient_source = std::format("{} (diagnostic only)", ambient.source);
				result.ambient_valid = true;
			}
		}

		if ((result.scalar <= 0.0f || !std::isfinite(result.scalar)) && best_entity &&
			dynamic_lighting::m_source_environment_force_direction_only)
		{
			// shadow_control and env_sun can carry the only surviving direction in a shipped map.
			// They do not define physical radiance, so create a conservative neutral fallback
			// rather than dropping the global light completely.
			result.color = Vector(1.0f, 0.96f, 0.90f);
			result.scalar = std::max(0.0001f, dynamic_lighting::m_source_environment_default_strength) *
				std::max(0.0f, dynamic_lighting::m_source_distant_intensity_scale);
			result.color_source = std::format("{} synthetic neutral fallback", best_entity->classname);
			result.synthetic_radiance = true;
		}
		if (result.scalar <= 0.0f || !std::isfinite(result.scalar)) return result;
		if (result.spread_source.empty())
		{
			result.angular_diameter = dynamic_lighting::m_source_distant_default_angular_diameter;
			result.spread_source = "default solar angular diameter";
		}
		result.angular_diameter = std::clamp(result.angular_diameter,
			std::max(0.001f, dynamic_lighting::m_source_distant_min_angular_diameter),
			std::max(dynamic_lighting::m_source_distant_min_angular_diameter,
				dynamic_lighting::m_source_distant_max_angular_diameter));

		if (best_sky && best_entity)
		{
			result.confidence = 0.76f;
			if (result.direction_disagreement <= 5.0f) result.confidence += 0.16f;
			else if (result.direction_disagreement <= 20.0f) result.confidence += 0.10f;
			else if (result.direction_disagreement > dynamic_lighting::m_source_distant_max_pair_angle) result.confidence -= 0.20f;
			result.confidence += result.color_similarity * 0.05f;
		}
		else if (best_sky) result.confidence = 0.74f;
		else if (best_entity)
		{
			if (best_entity->classname == "light_environment") result.confidence = 0.62f;
			else if (best_entity->classname == "light_directional") result.confidence = 0.58f;
			else if (best_entity->classname == "env_cascade_light") result.confidence = 0.50f;
			else if (best_entity->classname == "shadow_control") result.confidence = 0.38f;
			else if (best_entity->classname == "env_sun") result.confidence = 0.30f;
			else result.confidence = 0.25f;
			if (result.synthetic_radiance) result.confidence = std::max(result.confidence, 0.30f);
		}
		if (result.direction_source.find("target") != std::string::npos) result.confidence += 0.04f;
		if (result.used_hdr == dynamic_lighting::m_source_distant_prefer_hdr) result.confidence += 0.02f;
		result.confidence = std::clamp(result.confidence, 0.0f, 1.0f);

		dynamic_lighting::m_source_distant_candidates_rejected += static_cast<std::uint32_t>(
			(dynamic_lighting::m_source_distant_entities_found > 0u ? dynamic_lighting::m_source_distant_entities_found - 1u : 0u) +
			(valid_skylight_candidates > 0u ? valid_skylight_candidates - 1u : 0u));
		dynamic_lighting::m_source_distant_selected_confidence = result.confidence;
		dynamic_lighting::m_source_distant_direction_disagreement = result.direction_disagreement;
		dynamic_lighting::m_source_distant_selected_hdr = result.used_hdr;
		dynamic_lighting::m_source_distant_sign_flipped = result.worldlight_flipped;
		dynamic_lighting::m_source_distant_diagnostics = std::format(
			"entity={}#{} | aliases env/dir/cascade/shadow/sun={}/{}/{}/{}/{} | compiled={} | set={} | direction={} | disagreement={:.2f}deg | colorSimilarity={:.3f} | ambient={} | synthetic={} | confidence={:.3f}",
			result.classname, result.entity_source_index,
			dynamic_lighting::m_source_environment_light_environment_found,
			dynamic_lighting::m_source_environment_light_directional_found,
			dynamic_lighting::m_source_environment_cascade_found,
			dynamic_lighting::m_source_environment_shadow_control_found,
			dynamic_lighting::m_source_environment_env_sun_found,
			result.used_worldlight ? "yes" : "no",
			result.used_hdr ? "HDR" : "LDR",
			result.direction_source, result.direction_disagreement, result.color_similarity,
			result.ambient_valid ? result.ambient_source : "missing", result.synthetic_radiance ? "yes" : "no", result.confidence);

		result.valid = normalize_source_direction(result.direction) &&
			result.confidence >= std::clamp(dynamic_lighting::m_source_distant_min_confidence, 0.0f, 1.0f);
		return result;
	}

	static bool append_source_distant_candidate(const source_bsp_lights::world_light_result_s& parsed)
	{
		dynamic_lighting::m_source_distant_detected = false;
		dynamic_lighting::m_source_distant_imported = false;
		const auto resolved = resolve_source_distant_light(parsed);
		if (!resolved.valid)
		{
			dynamic_lighting::m_source_distant_status = std::format(
				"not detected (environment entities {}, skylight {}, skyambient {}, confidence {:.2f}/{:.2f})",
				dynamic_lighting::m_source_distant_entities_found,
				dynamic_lighting::m_source_distant_worldlights_found,
				dynamic_lighting::m_source_distant_skyambient_found,
				dynamic_lighting::m_source_distant_selected_confidence,
				dynamic_lighting::m_source_distant_min_confidence);
			return false;
		}

		dynamic_lighting::bsp_light_candidate_s candidate = {};
		candidate.source_index = k_source_distant_candidate_index;
		candidate.classname = resolved.classname;
		candidate.targetname = resolved.targetname;
		candidate.map_light_classname = resolved.classname;
		candidate.map_targetname = resolved.targetname;
		candidate.map_hammer_id = resolved.hammer_id;
		candidate.map_light_entity_index = resolved.entity_source_index > 0u
			? static_cast<std::int32_t>(resolved.entity_source_index) : -1;
		candidate.origin = resolved.origin;
		candidate.radiance = resolved.color;
		candidate.scalar = std::clamp(resolved.scalar, 0.0f, 250000.0f);
		candidate.radius = 1.0f;
		candidate.shaped = false;
		candidate.direction = resolved.direction;
		candidate.degrees = 180.0f;
		candidate.softness = 0.0f;
		candidate.exponent = 0.0f;
		candidate.distant = true;
		candidate.distant_angular_diameter = resolved.angular_diameter;
		candidate.distant_source = std::format(
			"direction: {}; color: {}; spread: {}; compiled: {}; confidence: {:.3f}; ambient: {}",
			resolved.direction_source, resolved.color_source, resolved.spread_source,
			resolved.worldlight_source.empty() ? "entity fallback" : resolved.worldlight_source,
			resolved.confidence, resolved.ambient_valid ? resolved.ambient_source : "missing");
		candidate.selected = dynamic_lighting::m_source_distant_light_import;
		candidate.from_worldlight = resolved.used_worldlight;
		candidate.hdr_worldlight = resolved.used_hdr;
		candidate.source_type = static_cast<std::int32_t>(source_bsp_lights::emit_type::skylight);
		candidate.near_camera = true;
		candidate.graph_matched = resolved.used_entity;
		candidate.comment = std::format(
			"SOURCE GLOBAL ENVIRONMENT V20.6 singleton entity={}#{} hammer={} worldlight={} set={} source={} dir={} color={} angularDiameter={:.2f} ({}) disagreement={:.2f}deg colorSimilarity={:.3f} confidence={:.3f} ambient={}{} ",
			resolved.classname, resolved.entity_source_index, resolved.hammer_id,
			resolved.used_worldlight ? "yes" : "no", resolved.used_hdr ? "HDR" : "LDR",
			resolved.worldlight_source.empty() ? "entity fallback" : resolved.worldlight_source,
			resolved.direction_source, resolved.color_source, resolved.angular_diameter,
			resolved.spread_source, resolved.direction_disagreement, resolved.color_similarity,
			resolved.confidence, resolved.ambient_valid ? resolved.ambient_source : "missing",
			resolved.worldlight_flipped ? " signCorrected" : "");

		if (dynamic_lighting::m_map_light_overrides_enabled)
		{
			if (const auto* override_data = source_map_light_overrides::find_best(g_map_light_overrides,
				static_cast<std::int32_t>(candidate.source_index), candidate.map_hammer_id,
				candidate.map_targetname, candidate.map_light_classname))
			{
				apply_candidate_override(candidate, *override_data);
				++dynamic_lighting::m_map_light_overrides_applied;
				if (candidate.override_disabled) ++dynamic_lighting::m_map_light_overrides_disabled;
				candidate.comment += std::format(" override={}",
					candidate.override_summary.empty() ? "matched" : candidate.override_summary);
			}
		}

		capture_candidate_editor_baseline(candidate);
		dynamic_lighting::m_source_distant_detected = true;
		dynamic_lighting::m_source_distant_status = std::format(
			"detected one Distant light | {} | {} | {} | diameter {:.2f} deg | confidence {:.2f} | disagreement {:.1f} deg | direction {:.3f} {:.3f} {:.3f}{}",
			resolved.used_hdr ? "HDR" : "LDR", resolved.direction_source, resolved.color_source,
			candidate.distant_angular_diameter, resolved.confidence, resolved.direction_disagreement,
			candidate.direction.x, candidate.direction.y, candidate.direction.z,
			candidate.override_applied ? " | map override applied" : "");
		dynamic_lighting::m_source_bsp_candidates.emplace_back(std::move(candidate));
		return true;
	}

	static float bsp_worldlight_source_radius(const source_bsp_lights::world_light_s& light)
	{
		if (std::isfinite(light.radius) && light.radius > 0.0f) {
			return light.radius;
		}

		// WORLDLIGHTS with radius=0 can still have attenuation coefficients. Estimate a
		// practical cutoff at denominator=50 (roughly 2% remaining contribution).
		constexpr float attenuation_cutoff = 50.0f;
		const float c = std::max(0.0f, light.constant_attn);
		const float l = std::max(0.0f, light.linear_attn);
		const float q = std::max(0.0f, light.quadratic_attn);
		if (q > 0.000001f)
		{
			const float discriminant = l * l - 4.0f * q * (c - attenuation_cutoff);
			if (discriminant >= 0.0f) {
				return std::max(0.0f, (-l + std::sqrt(discriminant)) / (2.0f * q));
			}
		}
		if (l > 0.000001f) {
			return std::max(0.0f, (attenuation_cutoff - c) / l);
		}

		return light.type == source_bsp_lights::emit_type::spotlight ? 320.0f : 512.0f;
	}

	static float bsp_worldlight_outer_degrees(const source_bsp_lights::world_light_s& light)
	{
		if (!std::isfinite(light.stopdot2) || light.stopdot2 < -1.0f || light.stopdot2 > 1.0f) {
			return 70.0f;
		}
		const float angle = RAD2DEGF(std::acos(std::clamp(light.stopdot2, -1.0f, 1.0f)));
		return std::clamp(angle, 1.0f, 179.0f);
	}

	static float bsp_worldlight_softness(const source_bsp_lights::world_light_s& light)
	{
		if (!std::isfinite(light.stopdot) || !std::isfinite(light.stopdot2)) {
			return 0.18f;
		}
		const float inner = RAD2DEGF(std::acos(std::clamp(light.stopdot, -1.0f, 1.0f)));
		const float outer = RAD2DEGF(std::acos(std::clamp(light.stopdot2, -1.0f, 1.0f)));
		if (outer <= 0.001f) {
			return 0.18f;
		}
		return std::clamp((outer - inner) / outer, 0.02f, 1.0f);
	}


	static void clear_candidate_owner_binding(dynamic_lighting::bsp_light_candidate_s& candidate)
	{
		candidate.graph_owner_bound = false;
		candidate.owner_relative = false;
		candidate.map_owner_entity_index = -1;
		candidate.map_owner_classname.clear();
		candidate.map_owner_targetname.clear();
		candidate.map_owner_model.clear();
		candidate.map_owner_attachment.clear();
		candidate.map_owner_hammer_id = -1;
		candidate.map_owner_initial_health = -1;
		candidate.map_owner_breakable = false;
		candidate.binding_reason = "override unbound";
		candidate.owner_local_origin = candidate.origin;
		candidate.owner_local_direction = candidate.direction;
	}

	static void bind_candidate_to_map_owner(dynamic_lighting::bsp_light_candidate_s& candidate,
		const source_map_entities::entity_s& owner, const std::string_view reason)
	{
		candidate.graph_owner_bound = true;
		candidate.map_owner_entity_index = static_cast<std::int32_t>(owner.source_index);
		candidate.map_owner_classname = owner.classname;
		candidate.map_owner_targetname = owner.targetname;
		candidate.map_owner_model = owner.model;
		// A forced owner override is an entity-level binding. Do not carry an attachment
		// name inherited from a previously auto-matched parent.
		candidate.map_owner_attachment.clear();
		candidate.map_owner_hammer_id = owner.hammer_id;
		candidate.map_owner_initial_health = owner.initial_health;
		candidate.map_owner_breakable = owner.is_breakable_owner;
		candidate.map_owner_origin = owner.origin;
		candidate.map_owner_angles = owner.angles;
		candidate.binding_reason = std::string(reason);
		candidate.owner_relative = true;
		candidate.owner_local_origin = inverse_rotate_source_vector(candidate.origin - owner.origin, owner.angles);
		candidate.owner_local_direction = inverse_rotate_source_vector(candidate.direction, owner.angles);
		if (candidate.owner_local_direction.LengthSqr() > 0.0001f) candidate.owner_local_direction.Normalize();
	}

	static void apply_candidate_override(dynamic_lighting::bsp_light_candidate_s& candidate,
		const source_map_light_overrides::light_override_s& override_data)
	{
		candidate.override_applied = true;
		if (override_data.deleted)
		{
			candidate.selected = false;
			candidate.override_disabled = true;
			candidate.override_summary = "deleted in persistent map-light database";
			return;
		}
		std::vector<std::string> changes;
		if (override_data.has_enabled)
		{
			candidate.selected = override_data.enabled;
			candidate.override_disabled = !override_data.enabled;
			changes.emplace_back(override_data.enabled ? "enabled" : "disabled");
		}
		if (override_data.has_position)
		{
			candidate.origin = override_data.position;
			changes.emplace_back("position");
		}
		if (override_data.has_position_offset)
		{
			candidate.origin += override_data.position_offset;
			changes.emplace_back("position offset");
		}
		if (override_data.has_radiance)
		{
			candidate.radiance = Vector(
				std::max(0.0f, override_data.radiance.x),
				std::max(0.0f, override_data.radiance.y),
				std::max(0.0f, override_data.radiance.z));
			changes.emplace_back("color");
		}
		if (override_data.has_intensity)
		{
			candidate.scalar = std::clamp(override_data.intensity, 0.0f, 250000.0f);
			changes.emplace_back("intensity");
		}
		if (override_data.has_radius)
		{
			candidate.radius = std::clamp(override_data.radius, 0.01f, 128.0f);
			changes.emplace_back("radius");
		}
		if (override_data.has_direction)
		{
			candidate.direction = override_data.direction;
			if (candidate.direction.LengthSqr() <= 0.0001f) candidate.direction = Vector(0.0f, 0.0f, -1.0f);
			candidate.direction.Normalize();
			changes.emplace_back("direction");
		}
		if (override_data.has_intensity_scale)
		{
			candidate.scalar = std::clamp(candidate.scalar * std::max(0.0f, override_data.intensity_scale), 0.0f, 250000.0f);
			changes.emplace_back("intensity");
		}
		if (override_data.has_radius_scale)
		{
			candidate.radius = std::clamp(candidate.radius * std::max(0.0f, override_data.radius_scale), 0.01f, 128.0f);
			changes.emplace_back("radius");
		}
		if (override_data.has_surface_offset)
		{
			candidate.origin += candidate.direction * override_data.surface_offset;
			changes.emplace_back("surface offset");
		}
		if (override_data.has_shaping) { candidate.shaped = override_data.shaping; changes.emplace_back("shaping"); }
		if (override_data.has_degrees)
		{
			if (candidate.distant)
			{
				candidate.distant_angular_diameter = std::clamp(override_data.degrees, 0.01f, 180.0f);
				changes.emplace_back("angular diameter");
			}
			else
			{
				candidate.degrees = std::clamp(override_data.degrees, 1.0f, 180.0f);
				changes.emplace_back("degrees");
			}
		}
		if (override_data.has_softness) { candidate.softness = std::clamp(override_data.softness, 0.0f, 1.0f); changes.emplace_back("softness"); }
		if (override_data.has_exponent) { candidate.exponent = std::clamp(override_data.exponent, 0.0f, 64.0f); changes.emplace_back("exponent"); }
		if (override_data.has_surface_cluster) { candidate.surface_cluster = override_data.surface_cluster; changes.emplace_back("surface cluster"); }
		if (override_data.has_surface_samples) candidate.surface_cluster_samples = std::clamp(override_data.surface_samples, 1, 16);
		if (override_data.has_surface_spread) candidate.surface_cluster_spread = std::clamp(override_data.surface_spread, 0.0f, 4.0f);
		if (override_data.has_surface_aspect) candidate.surface_cluster_aspect = std::clamp(override_data.surface_aspect, 0.1f, 8.0f);
		if (override_data.has_surface_intensity) candidate.surface_cluster_intensity = std::clamp(override_data.surface_intensity, 0.0f, 4.0f);
		if (override_data.has_surface_radius_scale) candidate.surface_cluster_radius_scale = std::clamp(override_data.surface_radius_scale, 0.01f, 4.0f);
		if (override_data.has_surface_pattern) candidate.surface_cluster_pattern = override_data.surface_pattern;
		candidate.override_summary.clear();
		for (std::size_t i = 0u; i < changes.size(); ++i)
		{
			if (i) candidate.override_summary += ", ";
			candidate.override_summary += changes[i];
		}
	}

	static void capture_candidate_editor_baseline(dynamic_lighting::bsp_light_candidate_s& candidate)
	{
		candidate.editor_baseline_valid = true;
		candidate.editor_selected = candidate.selected;
		candidate.editor_origin = candidate.origin;
		candidate.editor_radiance = candidate.radiance;
		candidate.editor_scalar = candidate.scalar;
		candidate.editor_radius = candidate.radius;
		candidate.editor_shaped = candidate.shaped;
		candidate.editor_direction = candidate.direction;
		candidate.editor_degrees = candidate.degrees;
		candidate.editor_softness = candidate.softness;
		candidate.editor_exponent = candidate.exponent;
		candidate.editor_distant_angular_diameter = candidate.distant_angular_diameter;
		candidate.editor_surface_cluster = candidate.surface_cluster;
		candidate.editor_surface_cluster_samples = candidate.surface_cluster_samples;
		candidate.editor_surface_cluster_spread = candidate.surface_cluster_spread;
		candidate.editor_surface_cluster_aspect = candidate.surface_cluster_aspect;
		candidate.editor_surface_cluster_intensity = candidate.surface_cluster_intensity;
		candidate.editor_surface_cluster_radius_scale = candidate.surface_cluster_radius_scale;
		candidate.editor_surface_cluster_pattern = candidate.surface_cluster_pattern;
	}

	static void restore_candidate_editor_baseline(dynamic_lighting::bsp_light_candidate_s& candidate)
	{
		if (!candidate.editor_baseline_valid) return;
		candidate.selected = candidate.editor_selected;
		candidate.origin = candidate.editor_origin;
		candidate.radiance = candidate.editor_radiance;
		candidate.scalar = candidate.editor_scalar;
		candidate.radius = candidate.editor_radius;
		candidate.shaped = candidate.editor_shaped;
		candidate.direction = candidate.editor_direction;
		candidate.degrees = candidate.editor_degrees;
		candidate.softness = candidate.editor_softness;
		candidate.exponent = candidate.editor_exponent;
		candidate.distant_angular_diameter = candidate.editor_distant_angular_diameter;
		candidate.surface_cluster = candidate.editor_surface_cluster;
		candidate.surface_cluster_samples = candidate.editor_surface_cluster_samples;
		candidate.surface_cluster_spread = candidate.editor_surface_cluster_spread;
		candidate.surface_cluster_aspect = candidate.editor_surface_cluster_aspect;
		candidate.surface_cluster_intensity = candidate.editor_surface_cluster_intensity;
		candidate.surface_cluster_radius_scale = candidate.editor_surface_cluster_radius_scale;
		candidate.surface_cluster_pattern = candidate.editor_surface_cluster_pattern;
	}

	static void sanitize_candidate_editor_values(dynamic_lighting::bsp_light_candidate_s& candidate)
	{
		if (!std::isfinite(candidate.origin.x)) candidate.origin.x = 0.0f;
		if (!std::isfinite(candidate.origin.y)) candidate.origin.y = 0.0f;
		if (!std::isfinite(candidate.origin.z)) candidate.origin.z = 0.0f;
		candidate.radiance.x = std::clamp(std::isfinite(candidate.radiance.x) ? candidate.radiance.x : 1.0f, 0.0f, 16.0f);
		candidate.radiance.y = std::clamp(std::isfinite(candidate.radiance.y) ? candidate.radiance.y : 1.0f, 0.0f, 16.0f);
		candidate.radiance.z = std::clamp(std::isfinite(candidate.radiance.z) ? candidate.radiance.z : 1.0f, 0.0f, 16.0f);
		candidate.scalar = std::clamp(std::isfinite(candidate.scalar) ? candidate.scalar : 1.0f, 0.0f, 250000.0f);
		candidate.radius = std::clamp(std::isfinite(candidate.radius) ? candidate.radius : 1.0f, 0.01f, 128.0f);
		if (candidate.direction.LengthSqr() <= 0.0001f || !std::isfinite(candidate.direction.x) ||
			!std::isfinite(candidate.direction.y) || !std::isfinite(candidate.direction.z))
		{
			candidate.direction = Vector(0.0f, 0.0f, -1.0f);
		}
		candidate.direction.Normalize();
		candidate.degrees = std::clamp(std::isfinite(candidate.degrees) ? candidate.degrees : 180.0f, 1.0f, 180.0f);
		candidate.softness = std::clamp(std::isfinite(candidate.softness) ? candidate.softness : 0.0f, 0.0f, 1.0f);
		candidate.exponent = std::clamp(std::isfinite(candidate.exponent) ? candidate.exponent : 0.0f, 0.0f, 64.0f);
		candidate.distant_angular_diameter = std::clamp(
			std::isfinite(candidate.distant_angular_diameter) ? candidate.distant_angular_diameter : 0.53f, 0.01f, 180.0f);
		candidate.surface_cluster_samples = std::clamp(candidate.surface_cluster_samples, 1, 16);
		candidate.surface_cluster_spread = std::clamp(candidate.surface_cluster_spread, 0.0f, 4.0f);
		candidate.surface_cluster_aspect = std::clamp(candidate.surface_cluster_aspect, 0.1f, 8.0f);
		candidate.surface_cluster_intensity = std::clamp(candidate.surface_cluster_intensity, 0.0f, 4.0f);
		candidate.surface_cluster_radius_scale = std::clamp(candidate.surface_cluster_radius_scale, 0.01f, 4.0f);
		if (candidate.graph_owner_bound)
		{
			candidate.owner_local_origin = inverse_rotate_source_vector(candidate.origin - candidate.map_owner_origin, candidate.map_owner_angles);
			candidate.owner_local_direction = inverse_rotate_source_vector(candidate.direction, candidate.map_owner_angles);
			if (candidate.owner_local_direction.LengthSqr() > 0.0001f) candidate.owner_local_direction.Normalize();
		}
	}



	static std::string persistent_source_light_id(const std::int32_t source_index,
		const std::int32_t runtime_entity_index, const std::string_view runtime_kind,
		const std::int32_t hammer_id, const std::string_view targetname)
	{
		if (runtime_entity_index >= 0)
		{
			const std::string kind = runtime_kind.empty() ? std::string("light") : std::string(runtime_kind);
			return std::format("runtime_{}_{}", runtime_entity_index, utils::str_to_lower(kind));
		}
		if (source_index >= 0) return std::format("source_{}", source_index);
		if (hammer_id >= 0) return std::format("hammer_{}", hammer_id);
		if (!targetname.empty()) return std::format("target_{:016x}", utils::string_hash64(utils::str_to_lower(std::string(targetname))));
		return {};
	}

	static source_map_light_overrides::light_point_s persistent_point_from_editor(
		const map_settings::remix_light_settings_s::point_s& point)
	{
		source_map_light_overrides::light_point_s out = {};
		out.position = point.position;
		out.radiance = point.radiance;
		out.radiance_scalar = point.radiance_scalar;
		out.radius = point.radius;
		out.timepoint = point.timepoint;
		out.smoothness = point.smoothness;
		out.use_shaping = point.use_shaping;
		out.direction = point.direction;
		out.angle_offset_attached = point.angle_offset_attached;
		out.degrees = point.degrees;
		out.softness = point.softness;
		out.exponent = point.exponent;
		out.volumetric_scale = point.volumetric_scale;
		out.authoring_shape = point.authoring_shape;
		out.authoring_width = point.authoring_width;
		out.authoring_height = point.authoring_height;
		out.authoring_length = point.authoring_length;
		out.authoring_range = point.authoring_range;
		out.light_rig_mode = point.light_rig_mode;
		out.ies_profile = point.ies_profile;
		out.ies_file = point.ies_file;
		out.ies_axis_rotation = point.ies_axis_rotation;
		out.ies_angle_scale = point.ies_angle_scale;
		out.ies_intensity_scale = point.ies_intensity_scale;
		out.ies_normalize = point.ies_normalize;
		out.ies_strength = point.ies_strength;
		out.ies_focus = point.ies_focus;
		out.ies_emulation = point.ies_emulation;
		out.ies_emulation_samples = point.ies_emulation_samples;
		out.ies_emulation_spread = point.ies_emulation_spread;
		out.ies_emulation_radius_scale = point.ies_emulation_radius_scale;
		out.ies_emulation_intensity_scale = point.ies_emulation_intensity_scale;
		out.ies_emulation_forward_offset = point.ies_emulation_forward_offset;
		out.ies_emulation_pattern = point.ies_emulation_pattern;
		out.ies_emulation_aspect = point.ies_emulation_aspect;
		out.ies_emulation_twist = point.ies_emulation_twist;
		return out;
	}

	static map_settings::remix_light_settings_s::point_s editor_point_from_persistent(
		const source_map_light_overrides::light_point_s& point)
	{
		map_settings::remix_light_settings_s::point_s out = {};
		out.position = point.position;
		out.radiance = point.radiance;
		out.radiance_scalar = std::max(0.0f, point.radiance_scalar);
		out.radius = std::max(0.001f, point.radius);
		out.timepoint = std::max(0.0f, point.timepoint);
		out.smoothness = std::clamp(point.smoothness, 0.0f, 1.0f);
		out.use_shaping = point.use_shaping;
		out.direction = point.direction;
		if (out.direction.LengthSqr() <= 0.0001f) out.direction = Vector(0.0f, 0.0f, 1.0f);
		out.direction.Normalize();
		out.angle_offset_attached = point.angle_offset_attached;
		out.degrees = std::clamp(point.degrees, 0.01f, 180.0f);
		out.softness = std::clamp(point.softness, 0.0f, 1.0f);
		out.exponent = std::clamp(point.exponent, 0.0f, 64.0f);
		out.volumetric_scale = std::max(0.0f, point.volumetric_scale);
		out.authoring_shape = std::clamp(point.authoring_shape,
			static_cast<int>(map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_AUTO),
			static_cast<int>(map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT));
		out.authoring_width = std::max(0.001f, point.authoring_width);
		out.authoring_height = std::max(0.001f, point.authoring_height);
		out.authoring_length = std::max(0.001f, point.authoring_length);
		out.authoring_range = std::max(0.001f, point.authoring_range);
		out.light_rig_mode = std::clamp(point.light_rig_mode,
			static_cast<int>(map_settings::remix_light_settings_s::LIGHT_RIG_MODE_LEGACY),
			static_cast<int>(map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES));
		out.ies_profile = point.ies_profile;
		out.ies_file = point.ies_file;
		out.ies_axis_rotation = point.ies_axis_rotation;
		out.ies_angle_scale = std::clamp(point.ies_angle_scale, 0.01f, 8.0f);
		out.ies_intensity_scale = std::clamp(point.ies_intensity_scale, 0.0f, 32.0f);
		out.ies_normalize = point.ies_normalize;
		out.ies_strength = std::clamp(point.ies_strength, 0.0f, 8.0f);
		out.ies_focus = std::clamp(point.ies_focus, 0.05f, 8.0f);
		out.ies_emulation = point.ies_emulation;
		out.ies_emulation_samples = std::clamp(point.ies_emulation_samples, 0, 24);
		out.ies_emulation_spread = std::clamp(point.ies_emulation_spread, 0.0f, 4.0f);
		out.ies_emulation_radius_scale = std::clamp(point.ies_emulation_radius_scale, 0.01f, 4.0f);
		out.ies_emulation_intensity_scale = std::clamp(point.ies_emulation_intensity_scale, 0.0f, 4.0f);
		out.ies_emulation_forward_offset = std::clamp(point.ies_emulation_forward_offset, -4.0f, 4.0f);
		out.ies_emulation_pattern = point.ies_emulation_pattern.empty() ? "spiral" : point.ies_emulation_pattern;
		out.ies_emulation_aspect = std::clamp(point.ies_emulation_aspect, 0.1f, 8.0f);
		out.ies_emulation_twist = std::clamp(point.ies_emulation_twist, -360.0f, 360.0f);
		return out;
	}

	static std::uint64_t persistent_light_fingerprint(const map_settings::remix_light_settings_s& def)
	{
		std::uint64_t hash = utils::string_hash64(def.persistent_map_light_id);
		auto mix = [&](const auto& value)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
			for (std::size_t i = 0u; i < sizeof(value); ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
		};
		auto mix_string = [&](const std::string& value) { hash ^= utils::string_hash64(value); hash *= 1099511628211ull; };
		mix(def.enabled); mix(def.run_once); mix(def.loop); mix(def.loop_smoothing); mix(def.trigger_always);
		mix_string(def.group); mix_string(def.comment); mix_string(def.animation);
		mix(def.animation_duration); mix(def.animation_speed); mix(def.animation_variation);
		mix(def.animation_axis.x); mix(def.animation_axis.y); mix(def.animation_axis.z);
		mix(def.animation_degrees); mix(def.animation_phase);
		mix_string(def.property_animation); mix(def.property_animation_duration); mix(def.property_animation_speed);
		mix(def.property_animation_variation); mix(def.property_animation_intensity);
		mix_string(def.movement_animation); mix(def.movement_animation_duration); mix(def.movement_animation_speed);
		mix(def.movement_animation_axis.x); mix(def.movement_animation_axis.y); mix(def.movement_animation_axis.z);
		mix(def.movement_animation_degrees); mix(def.movement_animation_phase); mix(def.movement_animation_distance);
		mix_string(def.trigger_choreo_name); mix_string(def.trigger_choreo_actor);
		mix_string(def.trigger_choreo_event); mix_string(def.trigger_choreo_param1);
		mix(def.trigger_sound_hash); mix(def.trigger_delay); mix_string(def.kill_choreo_name);
		mix(def.kill_sound_hash); mix(def.kill_delay); mix(def.attach_prop_radius); mix_string(def.attach_prop_name);
		mix(def.attach_prop_mins.x); mix(def.attach_prop_mins.y); mix(def.attach_prop_mins.z);
		mix(def.attach_prop_maxs.x); mix(def.attach_prop_maxs.y); mix(def.attach_prop_maxs.z);
		mix(def.attach_bone_index); mix_string(def.attach_bone_name);
		for (const auto& point : def.points)
		{
			const auto record = persistent_point_from_editor(point);
			mix(record.position.x); mix(record.position.y); mix(record.position.z);
			mix(record.radiance.x); mix(record.radiance.y); mix(record.radiance.z);
			mix(record.radiance_scalar); mix(record.radius); mix(record.timepoint); mix(record.smoothness);
			mix(record.use_shaping); mix(record.direction.x); mix(record.direction.y); mix(record.direction.z);
			mix(record.angle_offset_attached.x); mix(record.angle_offset_attached.y); mix(record.angle_offset_attached.z);
			mix(record.degrees); mix(record.softness); mix(record.exponent); mix(record.volumetric_scale);
			mix(record.authoring_shape); mix(record.authoring_width); mix(record.authoring_height);
			mix(record.authoring_length); mix(record.authoring_range); mix(record.light_rig_mode);
			mix_string(record.ies_profile); mix_string(record.ies_file); mix(record.ies_axis_rotation);
			mix(record.ies_angle_scale); mix(record.ies_intensity_scale); mix(record.ies_normalize);
			mix(record.ies_strength); mix(record.ies_focus); mix(record.ies_emulation);
			mix(record.ies_emulation_samples); mix(record.ies_emulation_spread);
			mix(record.ies_emulation_radius_scale); mix(record.ies_emulation_intensity_scale);
			mix(record.ies_emulation_forward_offset); mix_string(record.ies_emulation_pattern);
			mix(record.ies_emulation_aspect); mix(record.ies_emulation_twist);
		}
		return hash;
	}

	static std::uint64_t persistent_light_editor_fingerprint(
		const std::vector<map_settings::remix_light_settings_s>& lights)
	{
		std::uint64_t hash = 1469598103934665603ull;
		for (const auto& def : lights)
		{
			hash ^= persistent_light_fingerprint(def);
			hash *= 1099511628211ull;
		}
		return hash ^ static_cast<std::uint64_t>(lights.size());
	}


	static source_map_light_overrides::light_override_s make_full_candidate_override(
		const dynamic_lighting::bsp_light_candidate_s& candidate)
	{
		const std::string_view classname = candidate.map_light_classname.empty()
			? std::string_view(candidate.classname) : std::string_view(candidate.map_light_classname);

		source_map_light_overrides::light_override_s entry = {};
		entry.source_index = static_cast<std::int32_t>(candidate.source_index);
		entry.hammer_id = candidate.map_hammer_id;
		entry.targetname = candidate.map_targetname;
		entry.classname = std::string(classname);
		entry.persistent_id = persistent_source_light_id(entry.source_index, -1, {}, entry.hammer_id, entry.targetname);
		entry.full_rig = true;
		entry.group = candidate.distant ? "Persistent environment lights" : "Persistent Source map lights";
		entry.comment = candidate.comment.empty() ? "Persistent Source map light" : candidate.comment;
		entry.source_kind = candidate.distant ? "bsp_distant" : std::format("bsp_{}", source_bsp_lights::emit_type_name(
			static_cast<source_bsp_lights::emit_type>(candidate.source_type)));
		entry.source_style = candidate.style;
		entry.source_owner = candidate.owner;
		entry.source_key = static_cast<std::int32_t>(candidate.source_index);
		entry.source_transient = false;
		entry.source_live_link = candidate.graph_owner_bound || candidate.style > 0 || candidate.map_light_runtime_trackable;
		entry.has_enabled = true;
		entry.enabled = candidate.selected;
		entry.has_position = true;
		entry.position = candidate.origin;
		entry.has_radiance = true;
		entry.radiance = candidate.radiance;
		entry.has_intensity = true;
		entry.intensity = candidate.scalar;
		entry.has_radius = true;
		entry.radius = candidate.radius;
		entry.has_shaping = true;
		entry.shaping = candidate.shaped;
		entry.has_direction = candidate.shaped || candidate.distant;
		entry.direction = candidate.direction;
		entry.has_degrees = candidate.shaped || candidate.distant;
		entry.degrees = candidate.distant ? candidate.distant_angular_diameter : candidate.degrees;
		entry.has_softness = candidate.shaped;
		entry.softness = candidate.softness;
		entry.has_exponent = candidate.shaped;
		entry.exponent = candidate.exponent;
		entry.has_surface_cluster = true;
		entry.surface_cluster = candidate.surface_cluster;
		entry.has_surface_samples = candidate.surface_cluster;
		entry.surface_samples = candidate.surface_cluster_samples;
		entry.has_surface_spread = candidate.surface_cluster;
		entry.surface_spread = candidate.surface_cluster_spread;
		entry.has_surface_aspect = candidate.surface_cluster;
		entry.surface_aspect = candidate.surface_cluster_aspect;
		entry.has_surface_intensity = candidate.surface_cluster;
		entry.surface_intensity = candidate.surface_cluster_intensity;
		entry.has_surface_radius_scale = candidate.surface_cluster;
		entry.surface_radius_scale = candidate.surface_cluster_radius_scale;
		entry.has_surface_pattern = candidate.surface_cluster;
		entry.surface_pattern = candidate.surface_cluster_pattern.empty() ? "cross" : candidate.surface_cluster_pattern;

		map_settings::remix_light_settings_s::point_s point = {};
		point.position = candidate.origin;
		point.radiance = candidate.radiance;
		point.radiance_scalar = candidate.scalar;
		point.radius = candidate.radius;
		point.use_shaping = candidate.shaped || candidate.distant;
		point.direction = candidate.direction;
		point.degrees = candidate.distant ? candidate.distant_angular_diameter : candidate.degrees;
		point.softness = candidate.softness;
		point.exponent = candidate.exponent;
		point.authoring_shape = candidate.distant
			? map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT
			: (candidate.shaped ? map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT
				: map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT);
		point.authoring_width = candidate.distant ? candidate.distant_angular_diameter : std::max(0.001f, candidate.radius * 2.0f);
		point.authoring_range = std::max(1.0f, candidate.radius * 16.0f);
		point.ies_emulation = candidate.surface_cluster;
		point.ies_emulation_samples = candidate.surface_cluster_samples;
		point.ies_emulation_spread = candidate.surface_cluster_spread;
		point.ies_emulation_aspect = candidate.surface_cluster_aspect;
		point.ies_emulation_intensity_scale = candidate.surface_cluster_intensity;
		point.ies_emulation_radius_scale = candidate.surface_cluster_radius_scale;
		point.ies_emulation_pattern = candidate.surface_cluster_pattern.empty() ? "cross" : candidate.surface_cluster_pattern;
		entry.points.emplace_back(persistent_point_from_editor(point));
		return entry;
	}

	static source_map_light_overrides::light_override_s make_full_editor_override(
		const map_settings::remix_light_settings_s& def)
	{
		source_map_light_overrides::light_override_s entry = {};
		entry.source_index = def.generated_source_index;
		entry.hammer_id = def.generated_hammer_id;
		entry.runtime_entity_index = def.generated_runtime_entity_index;
		entry.runtime_kind = def.generated_runtime_kind;
		entry.targetname = def.generated_targetname;
		entry.classname = def.generated_classname;
		entry.persistent_id = def.persistent_map_light_id;
		entry.full_rig = true;
		entry.group = def.group;
		entry.comment = def.comment;
		entry.has_run_once = true;
		entry.run_once = def.run_once;
		entry.has_loop = true;
		entry.loop = def.loop;
		entry.has_loop_smoothing = true;
		entry.loop_smoothing = def.loop_smoothing;
		entry.has_trigger_always = true;
		entry.trigger_always = def.trigger_always;
		entry.trigger_choreo_name = def.trigger_choreo_name;
		entry.trigger_choreo_actor = def.trigger_choreo_actor;
		entry.trigger_choreo_event = def.trigger_choreo_event;
		entry.trigger_choreo_param1 = def.trigger_choreo_param1;
		entry.trigger_sound_hash = def.trigger_sound_hash;
		entry.trigger_delay = def.trigger_delay;
		entry.kill_choreo_name = def.kill_choreo_name;
		entry.kill_sound_hash = def.kill_sound_hash;
		entry.kill_delay = def.kill_delay;
		entry.attach_prop_radius = def.attach_prop_radius;
		entry.attach_prop_name = def.attach_prop_name;
		entry.attach_prop_mins = def.attach_prop_mins;
		entry.attach_prop_maxs = def.attach_prop_maxs;
		entry.attach_bone_index = def.attach_bone_index;
		entry.attach_bone_name = def.attach_bone_name;
		entry.source_kind = def.generated_source_kind;
		entry.source_style = def.generated_source_style;
		entry.source_owner = def.generated_source_owner;
		entry.source_key = def.generated_source_key;
		entry.source_transient = def.generated_source_transient;
		entry.source_live_link = def.generated_source_live_link;
		entry.has_enabled = true;
		entry.enabled = def.enabled;
		entry.has_animation = true;
		entry.animation = def.animation.empty() ? "stable" : def.animation;
		entry.has_animation_duration = true;
		entry.animation_duration = std::max(0.05f, def.animation_duration);
		entry.has_animation_speed = true;
		entry.animation_speed = std::max(0.05f, def.animation_speed);
		entry.has_animation_variation = true;
		entry.animation_variation = std::clamp(def.animation_variation, 0.0f, 1.0f);
		entry.has_animation_axis = true;
		entry.animation_axis = normalize_map_light_animation_vector(def.animation_axis, Vector(0.0f, 0.0f, 1.0f));
		entry.has_animation_degrees = true;
		entry.animation_degrees = std::clamp(def.animation_degrees, 0.0f, 1440.0f);
		entry.has_animation_phase = true;
		entry.animation_phase = def.animation_phase;
		entry.has_property_animation = true;
		entry.property_animation = def.property_animation.empty() ? "stable" : def.property_animation;
		entry.has_property_animation_duration = true;
		entry.property_animation_duration = std::max(0.05f, def.property_animation_duration);
		entry.has_property_animation_speed = true;
		entry.property_animation_speed = std::max(0.05f, def.property_animation_speed);
		entry.has_property_animation_variation = true;
		entry.property_animation_variation = std::clamp(def.property_animation_variation, 0.0f, 1.0f);
		entry.has_property_animation_intensity = true;
		entry.property_animation_intensity = def.property_animation_intensity >= 0.0f ? def.property_animation_intensity : (def.points.empty() ? 1.0f : std::max(0.0f, def.points.front().radiance_scalar));
		entry.has_movement_animation = true;
		entry.movement_animation = def.movement_animation.empty() ? "none" : def.movement_animation;
		entry.has_movement_animation_duration = true;
		entry.movement_animation_duration = std::max(0.05f, def.movement_animation_duration);
		entry.has_movement_animation_speed = true;
		entry.movement_animation_speed = std::max(0.05f, def.movement_animation_speed);
		entry.has_movement_animation_axis = true;
		entry.movement_animation_axis = normalize_map_light_animation_vector(def.movement_animation_axis, Vector(0.0f, 0.0f, 1.0f));
		entry.has_movement_animation_degrees = true;
		entry.movement_animation_degrees = std::clamp(def.movement_animation_degrees, 0.0f, 1440.0f);
		entry.has_movement_animation_phase = true;
		entry.movement_animation_phase = def.movement_animation_phase;
		entry.has_movement_animation_distance = true;
		entry.movement_animation_distance = std::max(0.0f, def.movement_animation_distance);
		entry.points.reserve(def.points.size());
		for (const auto& point : def.points) entry.points.emplace_back(persistent_point_from_editor(point));
		if (!def.points.empty())
		{
			const auto& point = def.points.front();
			entry.has_position = true;
			entry.position = point.position;
			entry.has_radiance = true;
			entry.radiance = point.radiance;
			entry.has_intensity = true;
			entry.intensity = point.radiance_scalar;
			entry.has_radius = true;
			entry.radius = point.radius;
			entry.has_shaping = true;
			entry.shaping = point.use_shaping;
			entry.has_direction = point.use_shaping || point.authoring_shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT;
			entry.direction = point.direction;
			entry.has_degrees = entry.has_direction;
			entry.degrees = point.degrees;
			entry.has_softness = point.use_shaping;
			entry.softness = point.softness;
			entry.has_exponent = point.use_shaping;
			entry.exponent = point.exponent;
			entry.has_surface_cluster = true;
			entry.surface_cluster = point.ies_emulation;
			entry.has_surface_samples = point.ies_emulation;
			entry.surface_samples = point.ies_emulation_samples;
			entry.has_surface_spread = point.ies_emulation;
			entry.surface_spread = point.ies_emulation_spread;
			entry.has_surface_aspect = point.ies_emulation;
			entry.surface_aspect = point.ies_emulation_aspect;
			entry.has_surface_intensity = point.ies_emulation;
			entry.surface_intensity = point.ies_emulation_intensity_scale;
			entry.has_surface_radius_scale = point.ies_emulation;
			entry.surface_radius_scale = point.ies_emulation_radius_scale;
			entry.has_surface_pattern = point.ies_emulation;
			entry.surface_pattern = point.ies_emulation_pattern.empty() ? "cross" : point.ies_emulation_pattern;
		}
		return entry;
	}

	static source_map_light_overrides::light_override_s make_full_runtime_entity_override(
		const map_settings::remix_light_settings_s& tracked_def, const bool enabled,
		const int runtime_entity_index, const std::string_view runtime_kind,
		const std::string_view runtime_classname)
	{
		auto def = tracked_def;
		def.enabled = enabled;
		def.generated_map_light = true;
		def.generated_source_index = -1;
		def.generated_hammer_id = -1;
		def.generated_runtime_entity_index = runtime_entity_index;
		def.generated_runtime_kind = std::string(runtime_kind);
		def.generated_classname = std::string(runtime_classname);
		def.persistent_map_light_id = persistent_source_light_id(-1, runtime_entity_index, runtime_kind, -1, {});
		def.persistent_map_light_managed = true;
		auto entry = make_full_editor_override(def);
		entry.snapshot_only = true;
		return entry;
	}


	static std::string ensure_persistent_map_light_id(map_settings::remix_light_settings_s& def,
		const std::size_t index, const bool base_config_assignment)
	{
		if (!def.persistent_map_light_id.empty())
		{
			def.persistent_map_light_managed = true;
			return def.persistent_map_light_id;
		}
		def.persistent_map_light_id = persistent_source_light_id(def.generated_source_index,
			def.generated_runtime_entity_index, def.generated_runtime_kind,
			def.generated_hammer_id, def.generated_targetname);
		if (def.persistent_map_light_id.empty())
		{
			if (base_config_assignment)
			{
				const auto stable = persistent_light_fingerprint(def);
				def.persistent_map_light_id = std::format("base_{}_{:016x}", index, stable);
			}
			else
			{
				const auto seed = std::format("{}:{}:{}:{}:{}",
					normalize_map_session_name(map_settings::get_map_name()),
					dynamic_lighting::get_bsp_worldlight_session_generation(),
					static_cast<unsigned long long>(GetTickCount64()),
					g_persistent_next_user_light_id++, index);
				def.persistent_map_light_id = std::format("user_{:016x}", utils::string_hash64(seed));
			}
		}
		def.persistent_map_light_managed = true;
		return def.persistent_map_light_id;
	}

	static map_settings::remix_light_settings_s persistent_editor_light_from_entry(
		const source_map_light_overrides::light_override_s& entry)
	{
		map_settings::remix_light_settings_s def = {};
		def.persistent_map_light_id = entry.persistent_id;
		def.persistent_map_light_managed = true;
		def.persistent_map_light_from_file = true;
		def.generated_map_light = entry.source_index >= 0 || entry.runtime_entity_index >= 0 ||
			!entry.source_kind.empty();
		def.generated_source_index = entry.source_index;
		def.generated_hammer_id = entry.hammer_id;
		def.generated_runtime_entity_index = entry.runtime_entity_index;
		def.generated_runtime_kind = entry.runtime_kind;
		def.generated_targetname = entry.targetname;
		def.generated_classname = entry.classname;
		def.generated_source_kind = entry.source_kind;
		def.generated_source_style = entry.source_style;
		def.generated_source_owner = entry.source_owner;
		def.generated_source_key = entry.source_key;
		def.generated_source_transient = entry.source_transient;
		def.generated_source_live_link = entry.source_live_link;
		def.enabled = entry.has_enabled ? entry.enabled : true;
		def.group = entry.group.empty()
			? (def.generated_map_light ? "Persistent Source map lights" : "Persistent authored lights")
			: entry.group;
		def.comment = entry.comment.empty()
			? (def.generated_map_light ? "Persistent Source map light" : "Persistent authored light")
			: entry.comment;
		def.run_once = entry.has_run_once ? entry.run_once : false;
		def.loop = entry.has_loop ? entry.loop : false;
		def.loop_smoothing = entry.has_loop_smoothing ? entry.loop_smoothing : false;
		def.trigger_always = entry.has_trigger_always ? entry.trigger_always : false;
		def.trigger_choreo_name = entry.trigger_choreo_name;
		def.trigger_choreo_actor = entry.trigger_choreo_actor;
		def.trigger_choreo_event = entry.trigger_choreo_event;
		def.trigger_choreo_param1 = entry.trigger_choreo_param1;
		def.trigger_sound_hash = entry.trigger_sound_hash;
		def.trigger_delay = entry.trigger_delay;
		def.kill_choreo_name = entry.kill_choreo_name;
		def.kill_sound_hash = entry.kill_sound_hash;
		def.kill_delay = entry.kill_delay;
		def.attach_prop_radius = entry.attach_prop_radius;
		def.attach_prop_name = entry.attach_prop_name;
		def.attach_prop_mins = entry.attach_prop_mins;
		def.attach_prop_maxs = entry.attach_prop_maxs;
		def.attach_bone_index = entry.attach_bone_index;
		def.attach_bone_name = entry.attach_bone_name;
		def.animation = entry.has_animation && !entry.animation.empty() ? entry.animation : "stable";
		def.animation_duration = entry.has_animation_duration ? std::max(0.05f, entry.animation_duration) : 1.0f;
		def.animation_speed = entry.has_animation_speed ? std::max(0.05f, entry.animation_speed) : 1.0f;
		def.animation_variation = entry.has_animation_variation ? std::clamp(entry.animation_variation, 0.0f, 1.0f) : 0.0f;
		def.animation_axis = entry.has_animation_axis
			? normalize_map_light_animation_vector(entry.animation_axis, Vector(0.0f, 0.0f, 1.0f))
			: Vector(0.0f, 0.0f, 1.0f);
		def.animation_degrees = entry.has_animation_degrees ? std::clamp(entry.animation_degrees, 0.0f, 1440.0f) : 360.0f;
		def.animation_phase = entry.has_animation_phase ? entry.animation_phase : 0.0f;
		def.property_animation = entry.has_property_animation && !entry.property_animation.empty() ? entry.property_animation : "stable";
		def.property_animation_duration = entry.has_property_animation_duration ? std::max(0.05f, entry.property_animation_duration) : def.animation_duration;
		def.property_animation_speed = entry.has_property_animation_speed ? std::max(0.05f, entry.property_animation_speed) : def.animation_speed;
		def.property_animation_variation = entry.has_property_animation_variation ? std::clamp(entry.property_animation_variation, 0.0f, 1.0f) : def.animation_variation;
		def.property_animation_intensity = entry.has_property_animation_intensity ? std::max(0.0f, entry.property_animation_intensity) : -1.0f;
		def.movement_animation = entry.has_movement_animation && !entry.movement_animation.empty() ? entry.movement_animation : "none";
		def.movement_animation_duration = entry.has_movement_animation_duration ? std::max(0.05f, entry.movement_animation_duration) : def.animation_duration;
		def.movement_animation_speed = entry.has_movement_animation_speed ? std::max(0.05f, entry.movement_animation_speed) : def.animation_speed;
		def.movement_animation_axis = entry.has_movement_animation_axis
			? normalize_map_light_animation_vector(entry.movement_animation_axis, Vector(0.0f, 0.0f, 1.0f)) : def.animation_axis;
		def.movement_animation_degrees = entry.has_movement_animation_degrees ? std::clamp(entry.movement_animation_degrees, 0.0f, 1440.0f) : def.animation_degrees;
		def.movement_animation_phase = entry.has_movement_animation_phase ? entry.movement_animation_phase : def.animation_phase;
		def.movement_animation_distance = entry.has_movement_animation_distance ? std::max(0.0f, entry.movement_animation_distance) : 64.0f;
		def.points.reserve(entry.points.size());
		for (const auto& point : entry.points) def.points.emplace_back(editor_point_from_persistent(point));
		if (def.points.empty() && (entry.has_position || entry.has_radiance || entry.has_radius))
		{
			map_settings::remix_light_settings_s::point_s point = {};
			if (entry.has_position) point.position = entry.position;
			if (entry.has_radiance) point.radiance = entry.radiance;
			if (entry.has_intensity) point.radiance_scalar = std::max(0.0f, entry.intensity);
			if (entry.has_radius) point.radius = std::max(0.001f, entry.radius);
			if (entry.has_shaping) point.use_shaping = entry.shaping;
			if (entry.has_direction) point.direction = entry.direction;
			if (point.direction.LengthSqr() <= 0.0001f) point.direction = Vector(0.0f, 0.0f, 1.0f);
			point.direction.Normalize();
			if (entry.has_degrees) point.degrees = std::clamp(entry.degrees, 0.01f, 180.0f);
			if (entry.has_softness) point.softness = std::clamp(entry.softness, 0.0f, 1.0f);
			if (entry.has_exponent) point.exponent = std::clamp(entry.exponent, 0.0f, 64.0f);
			point.authoring_shape = entry.source_kind == "bsp_distant"
				? map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT
				: (point.use_shaping ? map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT
					: map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT);
			def.points.emplace_back(point);
		}
		return def;
	}


	static bool map_light_override_same_identity(const source_map_light_overrides::light_override_s& lhs,
		const source_map_light_overrides::light_override_s& rhs)
	{
		if (!lhs.persistent_id.empty() || !rhs.persistent_id.empty())
		{
			return !lhs.persistent_id.empty() && !rhs.persistent_id.empty() &&
				utils::str_to_lower(lhs.persistent_id) == utils::str_to_lower(rhs.persistent_id);
		}
		if (lhs.runtime_entity_index >= 0 || rhs.runtime_entity_index >= 0)
		{
			return lhs.runtime_entity_index == rhs.runtime_entity_index &&
				utils::str_to_lower(lhs.runtime_kind) == utils::str_to_lower(rhs.runtime_kind);
		}
		if (lhs.source_index >= 0 || rhs.source_index >= 0) return lhs.source_index == rhs.source_index;
		if (lhs.hammer_id >= 0 || rhs.hammer_id >= 0) return lhs.hammer_id == rhs.hammer_id;
		if (!lhs.targetname.empty() || !rhs.targetname.empty())
		{
			return utils::str_to_lower(lhs.targetname) == utils::str_to_lower(rhs.targetname) &&
				utils::str_to_lower(lhs.classname) == utils::str_to_lower(rhs.classname);
		}
		return !lhs.classname.empty() && utils::str_to_lower(lhs.classname) == utils::str_to_lower(rhs.classname);
	}

	static void apply_runtime_entity_override(map_settings::remix_light_settings_s& def, bool& enabled,
		const source_map_light_overrides::light_override_s& entry)
	{
		if (entry.deleted) { enabled = false; return; }
		if (entry.snapshot_only) return;
		if (entry.full_rig && !entry.points.empty())
		{
			const auto runtime_index = def.generated_runtime_entity_index;
			const auto runtime_kind = def.generated_runtime_kind;
			const auto runtime_class = def.generated_classname;
			auto persisted = persistent_editor_light_from_entry(entry);
			persisted.generated_map_light = true;
			persisted.generated_runtime_entity_index = runtime_index;
			persisted.generated_runtime_kind = runtime_kind;
			persisted.generated_classname = runtime_class;
			persisted.generated_source_live_link = true;
			def = std::move(persisted);
			enabled = def.enabled;
			return;
		}
		if (entry.has_enabled) enabled = entry.enabled;
		if (def.points.empty()) return;
		auto& point = def.points.front();
		if (entry.has_position) point.position = entry.position;
		if (entry.has_position_offset) point.position += entry.position_offset;
		if (entry.has_radiance) point.radiance = entry.radiance;
		if (entry.has_intensity) point.radiance_scalar = std::max(0.0f, entry.intensity);
		if (entry.has_intensity_scale) point.radiance_scalar *= std::max(0.0f, entry.intensity_scale);
		if (entry.has_radius) point.radius = std::max(0.001f, entry.radius);
		if (entry.has_radius_scale) point.radius *= std::max(0.0001f, entry.radius_scale);
		if (entry.has_shaping) point.use_shaping = entry.shaping;
		if (entry.has_direction)
		{
			point.direction = entry.direction;
			if (point.direction.LengthSqr() <= 0.0001f) point.direction = Vector(1.0f, 0.0f, 0.0f);
			point.direction.Normalize();
		}
		if (entry.has_degrees) point.degrees = std::clamp(entry.degrees, 1.0f, 180.0f);
		if (entry.has_softness) point.softness = std::clamp(entry.softness, 0.0f, 1.0f);
		if (entry.has_exponent) point.exponent = std::clamp(entry.exponent, 0.0f, 64.0f);
		if (entry.has_surface_cluster) point.ies_emulation = entry.surface_cluster;
		if (entry.has_surface_samples) point.ies_emulation_samples = std::clamp(entry.surface_samples, 1, 16);
		if (entry.has_surface_spread) point.ies_emulation_spread = std::clamp(entry.surface_spread, 0.0f, 4.0f);
		if (entry.has_surface_aspect) point.ies_emulation_aspect = std::clamp(entry.surface_aspect, 0.1f, 8.0f);
		if (entry.has_surface_intensity) point.ies_emulation_intensity_scale = std::clamp(entry.surface_intensity, 0.0f, 4.0f);
		if (entry.has_surface_radius_scale) point.ies_emulation_radius_scale = std::clamp(entry.surface_radius_scale, 0.01f, 4.0f);
		if (entry.has_surface_pattern) point.ies_emulation_pattern = entry.surface_pattern;
		apply_map_light_animation_override(def, entry);
	}

	static Vector rotate_direction_around_world_axis(Vector direction, const int axis, const float degrees)
	{
		if (direction.LengthSqr() <= 0.0001f) direction = Vector(0.0f, 0.0f, -1.0f);
		direction.Normalize();
		const float radians = DEG2RADF(degrees);
		const float c = std::cos(radians);
		const float sn = std::sin(radians);
		Vector rotated = direction;
		switch (axis)
		{
		case 0: rotated = Vector(direction.x, direction.y * c - direction.z * sn, direction.y * sn + direction.z * c); break;
		case 1: rotated = Vector(direction.x * c + direction.z * sn, direction.y, -direction.x * sn + direction.z * c); break;
		case 2: rotated = Vector(direction.x * c - direction.y * sn, direction.x * sn + direction.y * c, direction.z); break;
		default: break;
		}
		if (rotated.LengthSqr() <= 0.0001f) return Vector(0.0f, 0.0f, -1.0f);
		rotated.Normalize();
		return rotated;
	}

	void dynamic_lighting::clear_bsp_worldlight_lights()
	{
		if (remix_lights::get()) {
			remix_lights::get()->destroy_source_distant_light();
			remix_lights::get()->destroy_lights_with_comment_prefix("BSP worldlight:");
			remix_lights::get()->destroy_lights_with_comment_prefix("BSP tracked worldlight:");
		}
		m_owned_worldlight_bindings.clear();
		m_bsp_worldlight_imported = 0u;
		m_bsp_worldlight_untracked_active = 0u;
		m_owned_worldlights_active = 0u;
		m_owned_worldlights_hidden = 0u;
		m_owned_worldlights_unresolved = 0u;
		m_map_light_model_rejects = 0u;
		m_map_light_duplicate_owner_rejects = 0u;
		m_map_light_attachment_updates = 0u;
		m_map_light_attachment_fallbacks = 0u;
		m_map_light_attachment_cache_hits = 0u;
		m_map_light_handle_generation_rejects = 0u;
		m_map_light_predicted_rebinds = 0u;
		m_map_runtime_light_state_active = 0u;
		m_map_runtime_light_state_hidden = 0u;
		m_map_runtime_light_state_unresolved = 0u;
		m_map_runtime_light_state_updates = 0u;
		m_map_runtime_light_state_pending = 0u;
		m_map_runtime_light_state_missing_held = 0u;
		m_map_runtime_light_state_confirmed_changes = 0u;
		m_map_light_destroyed_by_health = 0u;
		m_map_light_destroyed_by_removal = 0u;
		m_map_light_dormant_retained = 0u;
		m_map_light_owner_rebinds = 0u;
		m_map_light_rebind_deferred = 0u;
		m_bsp_worldlight_create_failed = 0u;
		m_bsp_worldlight_have_import_origin = false;
		m_bsp_worldlight_active_sources.clear();
		m_map_light_io_group_states.clear();
		m_map_light_io_pending_actions.clear();
		m_map_light_io_pending_count = 0u;
		m_map_light_io_root_fire_counts.clear();
		m_map_light_io_next_serial = 1u;
		m_map_light_io_group_observations = 0u;
		m_map_light_io_group_fallbacks = 0u;
		m_map_light_io_group_changes = 0u;
		m_map_light_io_group_terminal_kills = 0u;
		m_map_light_io_group_consensus_commits = 0u;
		m_map_light_io_group_conflicts = 0u;
		m_map_light_io_group_candidate_holds = 0u;
		m_map_light_io_group_split_observations = 0u;
		m_map_light_io_actions_scheduled = 0u;
		m_map_light_io_actions_executed = 0u;
		m_map_light_io_actions_coalesced = 0u;
		m_map_light_io_actions_dropped = 0u;
		m_map_light_io_actions_maxfires_blocked = 0u;
		m_map_light_io_actions_toggle = 0u;
		m_map_light_io_actions_kill = 0u;
		m_map_light_io_actions_manual = 0u;
		m_bsp_worldlight_stream_retained = 0u;
		m_bsp_worldlight_stream_evicted = 0u;
		m_source_distant_entities_found = 0u;
		m_source_environment_light_environment_found = 0u;
		m_source_environment_light_directional_found = 0u;
		m_source_environment_cascade_found = 0u;
		m_source_environment_shadow_control_found = 0u;
		m_source_environment_env_sun_found = 0u;
		m_source_environment_selected_alias = "none";
		m_source_distant_worldlights_found = 0u;
		m_source_distant_skyambient_found = 0u;
		m_source_distant_hdr_sets_found = 0u;
		m_source_distant_ldr_sets_found = 0u;
		m_source_distant_candidates_rejected = 0u;
		m_source_distant_selected_confidence = 0.0f;
		m_source_distant_direction_disagreement = 0.0f;
		m_source_distant_selected_hdr = false;
		m_source_distant_sign_flipped = false;
		m_source_distant_detected = false;
		m_source_distant_imported = false;
		m_source_distant_status = "cleared Source environment light";
		m_source_distant_diagnostics.clear();
		m_bsp_worldlight_status = "cleared imported WORLDLIGHTS";
	}

	bool dynamic_lighting::reload_map_light_overrides()
	{
		std::string map = normalize_map_session_name(m_bsp_worldlight_pending_map);
		if (map.empty()) map = normalize_map_session_name(map_settings::get_map_name());
		g_map_light_overrides = {};
		m_map_light_overrides_loaded = 0u;
		m_map_light_overrides_applied = 0u;
		m_map_light_overrides_disabled = 0u;
		m_map_light_override_invalid_lines = 0u;
		m_map_light_override_path.clear();
		if (!m_map_light_overrides_enabled)
		{
			m_map_light_override_status = "per-map overrides disabled";
			return true;
		}
		if (map.empty())
		{
			m_map_light_override_status = "no current map for overrides";
			return false;
		}
		if (!source_map_light_overrides::load(game::root_path, map, g_map_light_overrides))
		{
			m_map_light_override_status = "override parser failed";
			return false;
		}
		m_map_light_override_path = g_map_light_overrides.path;
		m_map_light_override_status = g_map_light_overrides.status;
		m_map_light_overrides_loaded = static_cast<std::uint32_t>(g_map_light_overrides.overrides.size());
		m_map_light_override_invalid_lines = g_map_light_overrides.invalid_lines;
		return true;
	}


	bool dynamic_lighting::load_persistent_map_lights_into_settings()
	{
		auto& editor_lights = map_settings::get_map_settings().remix_lights;
		m_persistent_map_light_loaded = 0u;
		m_persistent_map_light_materialized = 0u;
		m_persistent_map_light_live_linked = 0u;
		m_persistent_map_light_tombstones = 0u;
		m_persistent_map_light_known_ids.clear();
		m_persistent_map_light_materialized_sources.clear();
		m_persistent_map_light_dirty = false;
		m_persistent_map_light_save_at = 0.0f;

		for (std::size_t i = 0u; i < editor_lights.size(); ++i)
		{
			ensure_persistent_map_light_id(editor_lights[i], i, true);
			editor_lights[i].persistent_map_light_from_file = false;
		}

		if (!m_persistent_map_light_database)
		{
			m_persistent_map_light_status = "persistent per-map light database disabled";
			m_persistent_map_light_editor_fingerprint = persistent_light_editor_fingerprint(editor_lights);
			m_persistent_map_light_fingerprint_valid = true;
			return true;
		}

		std::string map = normalize_map_session_name(map_settings::get_map_name());
		if (map.empty()) map = normalize_map_session_name(m_bsp_worldlight_pending_map);
		if (map.empty())
		{
			m_persistent_map_light_status = "no current map for persistent light database";
			return false;
		}

		source_map_light_overrides::load_result_s loaded = {};
		if (!source_map_light_overrides::load(game::root_path, map, loaded))
		{
			m_persistent_map_light_status = "persistent light database parser failed";
			return false;
		}
		g_map_light_overrides = loaded;
		m_map_light_override_path = loaded.path;
		m_map_light_overrides_loaded = static_cast<std::uint32_t>(loaded.overrides.size());
		m_map_light_override_invalid_lines = loaded.invalid_lines;

		auto find_by_id = [&](const std::string_view id)
		{
			return std::find_if(editor_lights.begin(), editor_lights.end(), [&](const auto& def)
			{
				return !def.persistent_map_light_id.empty() &&
					utils::str_to_lower(def.persistent_map_light_id) == utils::str_to_lower(std::string(id));
			});
		};

		for (const auto& entry : loaded.overrides)
		{
			if (entry.persistent_id.empty()) continue;
			m_persistent_map_light_known_ids.insert(entry.persistent_id);
			if (entry.deleted)
			{
				++m_persistent_map_light_tombstones;
				const auto existing = find_by_id(entry.persistent_id);
				if (existing != editor_lights.end()) editor_lights.erase(existing);
				continue;
			}
			if (!entry.full_rig || entry.points.empty()) continue;
			++m_persistent_map_light_loaded;

			// Runtime/entity-linked lights remain visible and editable in Light Rigging,
			// but remix_lights::light_is_runtime_enabled treats this file-backed record
			// as a controller placeholder. The actual light keeps its Source lifecycle.
			auto def = persistent_editor_light_from_entry(entry);
			const auto existing = find_by_id(entry.persistent_id);
			if (existing != editor_lights.end()) *existing = std::move(def);
			else editor_lights.emplace_back(std::move(def));
			if (entry.source_live_link)
			{
				++m_persistent_map_light_live_linked;
			}
			else
			{
				++m_persistent_map_light_materialized;
				if (entry.source_index >= 0) m_persistent_map_light_materialized_sources.insert(entry.source_index);
			}
		}

		for (auto& def : editor_lights)
		{
			def.persistent_map_light_managed = true;
			m_persistent_map_light_known_ids.insert(def.persistent_map_light_id);
		}
		m_persistent_map_light_editor_fingerprint = persistent_light_editor_fingerprint(editor_lights);
		m_persistent_map_light_fingerprint_valid = true;
		m_persistent_map_light_status = std::format(
			"{} | materialized {} | live-linked {} | tombstones {} | file {}",
			loaded.status, m_persistent_map_light_materialized, m_persistent_map_light_live_linked,
			m_persistent_map_light_tombstones, loaded.path);
		m_map_light_override_status = m_persistent_map_light_status;
		return true;
	}

	bool dynamic_lighting::reload_persistent_map_lights_from_file()
	{
		if (!m_persistent_map_light_database)
		{
			m_persistent_map_light_status = "persistent per-map light database disabled";
			return false;
		}
		map_settings::reload();
		return true;
	}

	void dynamic_lighting::notify_light_editor_changed()
	{
		if (!m_persistent_map_light_database) return;
		m_persistent_map_light_dirty = true;
		m_persistent_map_light_save_at = now() + std::clamp(m_persistent_map_light_save_delay, 0.10f, 10.0f);
	}


	bool dynamic_lighting::reload_current_map_lights()
	{
		std::string map = normalize_map_session_name(m_bsp_worldlight_pending_map);
		if (map.empty()) map = normalize_map_session_name(map_settings::get_map_name());
		if (map.empty())
		{
			m_bsp_worldlight_status = "no current map to reload";
			return false;
		}

		m_bsp_worldlight_pending_map = map;
		m_bsp_worldlight_auto_import_pending = false;
		m_bsp_worldlight_auto_import_retries = 0u;
		clear_bsp_worldlight_lights();
		if (m_map_entity_graph_enabled) rebuild_map_entity_graph();
		if (scan_current_bsp_world_lights() && import_scanned_bsp_world_lights())
		{
			m_bsp_worldlight_loaded_map = map;
			return true;
		}

		if (m_bsp_worldlight_auto_import)
		{
			m_bsp_worldlight_auto_import_pending = true;
			m_bsp_worldlight_auto_import_delay = 1.0f;
		}
		return false;
	}

	bool dynamic_lighting::apply_selected_map_light_edits()
	{
		if (m_source_bsp_selected_index < 0 ||
			static_cast<std::size_t>(m_source_bsp_selected_index) >= m_source_bsp_candidates.size())
		{
			m_map_light_override_status = "no selected imported light";
			return false;
		}
		if (!remix_api::is_initialized() || !remix_lights::get())
		{
			m_map_light_override_status = "Remix API is not ready for live light editing";
			return false;
		}

		auto& candidate = m_source_bsp_candidates[static_cast<std::size_t>(m_source_bsp_selected_index)];
		sanitize_candidate_editor_values(candidate);
		const bool any_imported = import_scanned_bsp_world_lights();
		m_map_light_override_status = any_imported || !candidate.selected
			? "applied selected-light edits to the live map"
			: "editor values accepted, but no map light handle was created";
		return any_imported || !candidate.selected;
	}

	bool dynamic_lighting::save_selected_map_light_edits()
	{
		if (m_source_bsp_selected_index < 0 ||
			static_cast<std::size_t>(m_source_bsp_selected_index) >= m_source_bsp_candidates.size())
		{
			m_map_light_override_status = "no selected imported light";
			return false;
		}

		std::string map = normalize_map_session_name(m_bsp_worldlight_pending_map);
		if (map.empty()) map = normalize_map_session_name(map_settings::get_map_name());
		if (map.empty())
		{
			m_map_light_override_status = "no current map for saving light edits";
			return false;
		}

		auto& candidate = m_source_bsp_candidates[static_cast<std::size_t>(m_source_bsp_selected_index)];
		sanitize_candidate_editor_values(candidate);
		const std::string_view classname = candidate.map_light_classname.empty()
			? std::string_view(candidate.classname) : std::string_view(candidate.map_light_classname);

		auto entry = make_full_candidate_override(candidate);
		if (const auto* existing = source_map_light_overrides::find_best(g_map_light_overrides,
			static_cast<std::int32_t>(candidate.source_index), candidate.map_hammer_id,
			candidate.map_targetname, classname))
		{
			// Keep owner-binding directives authored by hand while replacing all editable light values.
			entry.has_owner_targetname = existing->has_owner_targetname;
			entry.owner_targetname = existing->owner_targetname;
			entry.has_unbind_owner = existing->has_unbind_owner;
			entry.unbind_owner = existing->unbind_owner;

			entry.has_animation = existing->has_animation;
			entry.animation = existing->animation;
			entry.has_animation_duration = existing->has_animation_duration;
			entry.animation_duration = existing->animation_duration;
			entry.has_animation_speed = existing->has_animation_speed;
			entry.animation_speed = existing->animation_speed;
			entry.has_animation_variation = existing->has_animation_variation;
			entry.animation_variation = existing->animation_variation;
			entry.has_animation_axis = existing->has_animation_axis;
			entry.animation_axis = existing->animation_axis;
			entry.has_animation_degrees = existing->has_animation_degrees;
			entry.animation_degrees = existing->animation_degrees;
			entry.has_animation_phase = existing->has_animation_phase;
			entry.animation_phase = existing->animation_phase;
			entry.has_property_animation = existing->has_property_animation;
			entry.property_animation = existing->property_animation;
			entry.has_property_animation_duration = existing->has_property_animation_duration;
			entry.property_animation_duration = existing->property_animation_duration;
			entry.has_property_animation_speed = existing->has_property_animation_speed;
			entry.property_animation_speed = existing->property_animation_speed;
			entry.has_property_animation_variation = existing->has_property_animation_variation;
			entry.property_animation_variation = existing->property_animation_variation;
			entry.has_property_animation_intensity = existing->has_property_animation_intensity;
			entry.property_animation_intensity = existing->property_animation_intensity;
			entry.has_movement_animation = existing->has_movement_animation;
			entry.movement_animation = existing->movement_animation;
			entry.has_movement_animation_duration = existing->has_movement_animation_duration;
			entry.movement_animation_duration = existing->movement_animation_duration;
			entry.has_movement_animation_speed = existing->has_movement_animation_speed;
			entry.movement_animation_speed = existing->movement_animation_speed;
			entry.has_movement_animation_axis = existing->has_movement_animation_axis;
			entry.movement_animation_axis = existing->movement_animation_axis;
			entry.has_movement_animation_degrees = existing->has_movement_animation_degrees;
			entry.movement_animation_degrees = existing->movement_animation_degrees;
			entry.has_movement_animation_phase = existing->has_movement_animation_phase;
			entry.movement_animation_phase = existing->movement_animation_phase;
			entry.has_movement_animation_distance = existing->has_movement_animation_distance;
			entry.movement_animation_distance = existing->movement_animation_distance;
		}

		std::string path;
		std::string error;
		if (!source_map_light_overrides::upsert(game::root_path, map, entry, path, error))
		{
			m_map_light_override_path = path;
			m_map_light_override_status = error.empty() ? "failed to save selected light" : error;
			return false;
		}

		m_map_light_override_path = path;
		reload_map_light_overrides();
		capture_candidate_editor_baseline(candidate);
		candidate.override_applied = true;
		candidate.override_disabled = !candidate.selected;
		candidate.override_summary = "saved editor values";
		const bool live_applied = apply_selected_map_light_edits();
		m_map_light_override_status = live_applied
			? "saved selected light and applied it to the live map"
			: "saved selected light; live apply is waiting for Remix API";
		return true;
	}


	bool dynamic_lighting::save_all_map_lights_to_config()
	{
		std::string map = normalize_map_session_name(m_bsp_worldlight_pending_map);
		if (map.empty()) map = normalize_map_session_name(map_settings::get_map_name());
		if (map.empty())
		{
			m_map_light_override_status = "no current map for generated light config";
			return false;
		}
		if (m_source_bsp_candidates.empty() && m_runtime_projected_lights.empty())
		{
			m_map_light_override_status = "no stable captured map lights to save";
			return false;
		}

		std::vector<source_map_light_overrides::light_override_s> entries;
		entries.reserve(m_source_bsp_candidates.size() + m_runtime_projected_lights.size() + g_map_light_overrides.overrides.size());
		std::uint32_t stable_bsp_count = 0u;
		for (auto& candidate : m_source_bsp_candidates)
		{
			if (bsp_candidate_is_registry_only_environment(candidate)) continue;
			sanitize_candidate_editor_values(candidate);
			auto entry = make_full_candidate_override(candidate);
			if (const auto* existing = source_map_light_overrides::find_best(g_map_light_overrides,
				entry.source_index, entry.hammer_id, entry.targetname, entry.classname))
			{
				if (existing->deleted)
				{
					entries.emplace_back(*existing);
					continue;
				}
				// V20.9 file-first contract: once a captured Source light has a full
				// persistent rig, automatic scans must not overwrite Light Rigging edits.
				// A user can explicitly refresh or edit the record, but background capture
				// only creates records which do not exist yet.
				if (existing->full_rig && !existing->points.empty())
				{
					entries.emplace_back(*existing);
					++stable_bsp_count;
					continue;
				}
				entry.has_owner_targetname = existing->has_owner_targetname;
				entry.owner_targetname = existing->owner_targetname;
				entry.has_unbind_owner = existing->has_unbind_owner;
				entry.unbind_owner = existing->unbind_owner;

				entry.has_animation = existing->has_animation;
				entry.animation = existing->animation;
				entry.has_animation_duration = existing->has_animation_duration;
				entry.animation_duration = existing->animation_duration;
				entry.has_animation_speed = existing->has_animation_speed;
				entry.animation_speed = existing->animation_speed;
				entry.has_animation_variation = existing->has_animation_variation;
				entry.animation_variation = existing->animation_variation;
				entry.has_animation_axis = existing->has_animation_axis;
				entry.animation_axis = existing->animation_axis;
				entry.has_animation_degrees = existing->has_animation_degrees;
				entry.animation_degrees = existing->animation_degrees;
				entry.has_animation_phase = existing->has_animation_phase;
				entry.animation_phase = existing->animation_phase;
				entry.has_property_animation = existing->has_property_animation;
				entry.property_animation = existing->property_animation;
				entry.has_property_animation_duration = existing->has_property_animation_duration;
				entry.property_animation_duration = existing->property_animation_duration;
				entry.has_property_animation_speed = existing->has_property_animation_speed;
				entry.property_animation_speed = existing->property_animation_speed;
				entry.has_property_animation_variation = existing->has_property_animation_variation;
				entry.property_animation_variation = existing->property_animation_variation;
				entry.has_property_animation_intensity = existing->has_property_animation_intensity;
				entry.property_animation_intensity = existing->property_animation_intensity;
				entry.has_movement_animation = existing->has_movement_animation;
				entry.movement_animation = existing->movement_animation;
				entry.has_movement_animation_duration = existing->has_movement_animation_duration;
				entry.movement_animation_duration = existing->movement_animation_duration;
				entry.has_movement_animation_speed = existing->has_movement_animation_speed;
				entry.movement_animation_speed = existing->movement_animation_speed;
				entry.has_movement_animation_axis = existing->has_movement_animation_axis;
				entry.movement_animation_axis = existing->movement_animation_axis;
				entry.has_movement_animation_degrees = existing->has_movement_animation_degrees;
				entry.movement_animation_degrees = existing->movement_animation_degrees;
				entry.has_movement_animation_phase = existing->has_movement_animation_phase;
				entry.movement_animation_phase = existing->movement_animation_phase;
				entry.has_movement_animation_distance = existing->has_movement_animation_distance;
				entry.movement_animation_distance = existing->movement_animation_distance;
			}
			entries.emplace_back(std::move(entry));
			++stable_bsp_count;
		}

		std::vector<const runtime_source_light_s*> stable_runtime_lights;
		stable_runtime_lights.reserve(m_runtime_projected_lights.size());
		for (const auto& runtime_pair : m_runtime_projected_lights)
		{
			const auto& tracked = runtime_pair.second;
			if (tracked.runtime_entity_index >= 0 && !tracked.def.points.empty() && !tracked.duplicate_suppressed)
				stable_runtime_lights.push_back(&tracked);
		}
		std::sort(stable_runtime_lights.begin(), stable_runtime_lights.end(), [](const auto* lhs, const auto* rhs)
		{
			if (lhs->runtime_entity_index != rhs->runtime_entity_index) return lhs->runtime_entity_index < rhs->runtime_entity_index;
			return lhs->runtime_kind < rhs->runtime_kind;
		});

		std::uint32_t stable_runtime_count = 0u;
		for (const auto* tracked_ptr : stable_runtime_lights)
		{
			const auto& tracked = *tracked_ptr;
			auto entry = make_full_runtime_entity_override(tracked.def, tracked.last_enabled,
				tracked.runtime_entity_index, tracked.runtime_kind, tracked.runtime_classname);
			entry.source_kind = tracked.runtime_kind;
			entry.source_style = tracked.source_style;
			entry.source_owner = tracked.source_owner;
			entry.source_key = tracked.source_key;
			entry.source_transient = tracked.transient;
			entry.source_live_link = true;
			if (const auto* existing = source_map_light_overrides::find_runtime_entity(g_map_light_overrides,
				entry.runtime_entity_index, entry.runtime_kind, entry.classname))
			{
				if (existing->deleted)
				{
					entries.emplace_back(*existing);
					continue;
				}
				// A snapshot-only entry is refreshed from Source. Once the user saves it
				// through Light Editor, preserve the authored override verbatim.
				if (!existing->snapshot_only) entry = *existing;
			}
			entries.emplace_back(std::move(entry));
			++stable_runtime_count;
		}

		// Automatic refreshes must not erase hand-authored selectors or runtime entities
		// which are temporarily dormant when the snapshot is written.
		for (const auto& existing : g_map_light_overrides.overrides)
		{
			const bool suppressed_runtime_snapshot = existing.snapshot_only && existing.runtime_entity_index >= 0 &&
				std::any_of(m_runtime_projected_lights.begin(), m_runtime_projected_lights.end(), [&](const auto& pair)
				{
					const auto& tracked = pair.second;
					return tracked.duplicate_suppressed && tracked.runtime_entity_index == existing.runtime_entity_index &&
						utils::str_to_lower(tracked.runtime_kind) == utils::str_to_lower(existing.runtime_kind);
				});
			if (suppressed_runtime_snapshot) continue;

			const bool already_present = std::any_of(entries.begin(), entries.end(), [&](const auto& generated) {
				return map_light_override_same_identity(existing, generated);
			});
			if (!already_present) entries.emplace_back(existing);
		}

		std::string path;
		std::string error;
		if (!source_map_light_overrides::replace_all(game::root_path, map, entries, path, error))
		{
			m_map_light_override_path = path;
			m_map_light_override_status = error.empty() ? "failed to save generated map-light config" : error;
			return false;
		}

		const auto applied_before_reload = m_map_light_overrides_applied;
		const auto disabled_before_reload = m_map_light_overrides_disabled;
		m_map_light_override_path = path;
		if (m_map_light_overrides_enabled)
		{
			reload_map_light_overrides();
			m_map_light_overrides_applied = applied_before_reload;
			m_map_light_overrides_disabled = disabled_before_reload;
		}
		for (auto& candidate : m_source_bsp_candidates)
		{
			if (bsp_candidate_is_registry_only_environment(candidate))
			{
				capture_candidate_editor_baseline(candidate);
				continue;
			}
			capture_candidate_editor_baseline(candidate);
			candidate.override_applied = true;
			candidate.override_disabled = !candidate.selected;
			candidate.override_summary = "generated map config";
		}
		m_map_light_config_dirty = false;
		const auto captured_count = static_cast<std::size_t>(stable_bsp_count) + static_cast<std::size_t>(stable_runtime_count);
		m_map_light_override_status = std::format("saved {} captured stable map lights to config ({} records total; {} compiled/entity-linked + {} runtime entities; environment registry-only records omitted)",
			captured_count, entries.size(), stable_bsp_count, stable_runtime_count);
		return true;
	}

	std::uint32_t dynamic_lighting::get_light_editor_imported_count()
	{
		const auto& lights = map_settings::get_map_settings().remix_lights;
		return static_cast<std::uint32_t>(std::count_if(lights.begin(), lights.end(), [](const auto& def) {
			return def.generated_map_light;
		}));
	}

	bool dynamic_lighting::sync_imported_map_lights_to_light_editor()
	{
		if (m_source_bsp_candidates.empty() && m_runtime_projected_lights.empty())
		{
			m_map_light_override_status = "no stable captured map lights to send to Light Editor";
			return false;
		}

		auto& editor_lights = map_settings::get_map_settings().remix_lights;
		const bool preserve_current_generation_edits = m_map_light_editor_synced_generation == m_bsp_worldlight_session_generation;
		std::vector<map_settings::remix_light_settings_s> previous_generated;
		for (const auto& def : editor_lights)
		{
			if (def.generated_map_light && (preserve_current_generation_edits || def.persistent_map_light_from_file))
				previous_generated.emplace_back(def);
		}
		std::erase_if(editor_lights, [](const auto& def)
		{
			return def.generated_map_light && !def.persistent_map_light_from_file;
		});
		editor_lights.reserve(editor_lights.size() + m_source_bsp_candidates.size() + m_runtime_projected_lights.size());
		std::uint32_t preserved_count = 0u;
		std::uint32_t compiled_count = 0u;
		for (const auto& candidate : m_source_bsp_candidates)
		{
			if (bsp_candidate_is_registry_only_environment(candidate)) continue;
			const auto source_index = static_cast<std::int32_t>(candidate.source_index);
			const auto persistent_id = persistent_source_light_id(source_index, -1, {}, candidate.map_hammer_id, candidate.map_targetname);
			const auto file_backed = std::find_if(editor_lights.begin(), editor_lights.end(), [&](const auto& def)
			{
				return def.persistent_map_light_from_file && def.persistent_map_light_id == persistent_id;
			});
			if (file_backed != editor_lights.end())
			{
				++preserved_count;
				++compiled_count;
				continue;
			}
			const auto previous = std::find_if(previous_generated.begin(), previous_generated.end(), [&](const auto& def)
			{
				if (!def.generated_map_light || def.generated_runtime_entity_index >= 0) return false;
				if (def.generated_source_index >= 0 && source_index >= 0) return def.generated_source_index == source_index;
				return def.generated_hammer_id >= 0 && candidate.map_hammer_id >= 0 && def.generated_hammer_id == candidate.map_hammer_id;
			});

			auto def = previous != previous_generated.end()
				? *previous : make_light_def_from_bsp_candidate(candidate, "Imported map light: ");
			if (previous != previous_generated.end()) ++preserved_count;
			else def.enabled = candidate.selected;
			def.group = "Imported map lights";
			def.generated_map_light = true;
			def.generated_source_index = source_index;
			def.generated_hammer_id = candidate.map_hammer_id;
			def.generated_targetname = candidate.map_targetname;
			def.generated_classname = candidate.map_light_classname.empty() ? candidate.classname : candidate.map_light_classname;
			def.generated_source_kind = std::format("bsp_{}", source_bsp_lights::emit_type_name(
				static_cast<source_bsp_lights::emit_type>(candidate.source_type)));
			def.generated_source_style = candidate.style;
			def.generated_source_owner = candidate.owner;
			def.generated_source_key = static_cast<std::int32_t>(candidate.source_index);
			def.generated_source_transient = false;
			def.generated_source_live_link = candidate.graph_owner_bound || candidate.style > 0 || candidate.map_light_runtime_trackable;
			def.persistent_map_light_id = persistent_source_light_id(source_index, -1, {}, candidate.map_hammer_id, candidate.map_targetname);
			def.persistent_map_light_managed = true;
			editor_lights.emplace_back(std::move(def));
			++compiled_count;
		}

		std::vector<const runtime_source_light_s*> stable_runtime_lights;
		stable_runtime_lights.reserve(m_runtime_projected_lights.size());
		for (const auto& runtime_pair : m_runtime_projected_lights)
		{
			const auto& tracked = runtime_pair.second;
			if (tracked.runtime_entity_index >= 0 && !tracked.def.points.empty() && !tracked.duplicate_suppressed)
				stable_runtime_lights.push_back(&tracked);
		}
		std::sort(stable_runtime_lights.begin(), stable_runtime_lights.end(), [](const auto* lhs, const auto* rhs)
		{
			if (lhs->runtime_entity_index != rhs->runtime_entity_index) return lhs->runtime_entity_index < rhs->runtime_entity_index;
			return lhs->runtime_kind < rhs->runtime_kind;
		});

		std::uint32_t runtime_count = 0u;
		for (const auto* tracked_ptr : stable_runtime_lights)
		{
			const auto& tracked = *tracked_ptr;
			const auto persistent_id = persistent_source_light_id(-1, tracked.runtime_entity_index, tracked.runtime_kind, -1, {});
			const auto file_backed = std::find_if(editor_lights.begin(), editor_lights.end(), [&](const auto& def)
			{
				return def.persistent_map_light_from_file && def.persistent_map_light_id == persistent_id;
			});
			if (file_backed != editor_lights.end())
			{
				++preserved_count;
				++runtime_count;
				continue;
			}
			const auto previous = std::find_if(previous_generated.begin(), previous_generated.end(), [&](const auto& def)
			{
				return def.generated_map_light && def.generated_runtime_entity_index == tracked.runtime_entity_index &&
					utils::str_to_lower(def.generated_runtime_kind) == utils::str_to_lower(tracked.runtime_kind);
			});
			auto def = previous != previous_generated.end() ? *previous : tracked.def;
			if (previous != previous_generated.end()) ++preserved_count;
			else def.enabled = tracked.last_enabled;
			def.group = "Imported runtime map lights";
			def.generated_map_light = true;
			def.generated_source_index = -1;
			def.generated_hammer_id = -1;
			def.generated_runtime_entity_index = tracked.runtime_entity_index;
			def.generated_runtime_kind = tracked.runtime_kind;
			def.generated_classname = tracked.runtime_classname;
			def.generated_source_kind = tracked.runtime_kind;
			def.generated_source_style = tracked.source_style;
			def.generated_source_owner = tracked.source_owner;
			def.generated_source_key = tracked.source_key;
			def.generated_source_transient = tracked.transient;
			def.generated_source_live_link = true;
			def.persistent_map_light_id = persistent_source_light_id(-1, tracked.runtime_entity_index, tracked.runtime_kind, -1, {});
			def.persistent_map_light_managed = true;
			editor_lights.emplace_back(std::move(def));
			++runtime_count;
		}
		m_map_light_editor_synced_generation = m_bsp_worldlight_session_generation;
		m_map_light_override_status = std::format("sent {} stable map lights to Light Editor ({} compiled/entity-linked, {} runtime entities; {} current edits preserved; registry-only environment records omitted)",
			compiled_count + runtime_count, compiled_count, runtime_count, preserved_count);
		return true;
	}

	bool dynamic_lighting::save_light_editor_map_lights_to_config()
	{
		// Outside Light Rigging, ordinary map lights may already have been moved out
		// of map_settings into the active runtime list. Saving that partial vector
		// would incorrectly tombstone valid records, so full-database writes are
		// intentionally restricted to edit mode. Source capture uses its own merger.
		if (!imgui::get() || !imgui::get()->m_light_edit_mode)
		{
			m_persistent_map_light_status = "enter Light Rigging edit mode before saving the full map-light database";
			m_map_light_override_status = m_persistent_map_light_status;
			return false;
		}

		std::string map = normalize_map_session_name(m_bsp_worldlight_pending_map);
		if (map.empty()) map = normalize_map_session_name(map_settings::get_map_name());
		if (map.empty())
		{
			m_map_light_override_status = "no current map for persistent Light Rig save";
			return false;
		}
		if (!m_persistent_map_light_database)
		{
			m_map_light_override_status = "persistent per-map light database disabled";
			return false;
		}

		auto& editor_lights = map_settings::get_map_settings().remix_lights;
		std::vector<source_map_light_overrides::light_override_s> entries;
		entries.reserve(editor_lights.size() + g_map_light_overrides.overrides.size());
		std::unordered_set<std::string> current_ids;
		std::uint32_t authored_count = 0u;
		std::uint32_t imported_count = 0u;
		std::uint32_t live_linked_count = 0u;

		for (std::size_t i = 0u; i < editor_lights.size(); ++i)
		{
			auto& def = editor_lights[i];
			if (def.points.empty()) continue;
			ensure_persistent_map_light_id(def, i, false);
			def.persistent_map_light_managed = true;
			current_ids.insert(def.persistent_map_light_id);
			auto entry = make_full_editor_override(def);
			entry.deleted = false;
			entries.emplace_back(std::move(entry));
			if (def.generated_map_light || def.generated_source_index >= 0 || def.generated_runtime_entity_index >= 0) ++imported_count;
			else ++authored_count;
			if (def.generated_source_live_link) ++live_linked_count;
		}

		// Keep legacy selector-only overrides which do not represent a full rig. They
		// can still carry manual owner binding and scale corrections for Source lights.
		for (const auto& existing : g_map_light_overrides.overrides)
		{
			if (!existing.persistent_id.empty()) continue;
			const bool already_present = std::any_of(entries.begin(), entries.end(), [&](const auto& generated)
			{
				return map_light_override_same_identity(existing, generated);
			});
			if (!already_present) entries.emplace_back(existing);
		}

		// Missing persistent ids become tombstones. This is essential for imported
		// Source lights and base-config lights: deleting them in Light Rigging must
		// survive the next BSP scan / map_settings parse instead of recreating them.
		std::uint32_t tombstones_written = 0u;
		for (const auto& known_id : m_persistent_map_light_known_ids)
		{
			if (current_ids.contains(known_id)) continue;
			source_map_light_overrides::light_override_s tombstone = {};
			if (const auto* existing = source_map_light_overrides::find_persistent(g_map_light_overrides, known_id))
				tombstone = *existing;
			tombstone.persistent_id = known_id;
			tombstone.full_rig = true;
			tombstone.deleted = true;
			tombstone.points.clear();
			tombstone.has_enabled = true;
			tombstone.enabled = false;
			entries.emplace_back(std::move(tombstone));
			++tombstones_written;
		}

		std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs)
		{
			if (lhs.persistent_id.empty() != rhs.persistent_id.empty()) return !lhs.persistent_id.empty();
			if (lhs.persistent_id != rhs.persistent_id) return lhs.persistent_id < rhs.persistent_id;
			if (lhs.runtime_entity_index != rhs.runtime_entity_index) return lhs.runtime_entity_index < rhs.runtime_entity_index;
			return lhs.source_index < rhs.source_index;
		});

		std::string path;
		std::string error;
		if (!source_map_light_overrides::replace_all(game::root_path, map, entries, path, error))
		{
			m_map_light_override_path = path;
			m_map_light_override_status = error.empty() ? "failed to save persistent Light Rig database" : error;
			return false;
		}
		m_map_light_override_path = path;
		reload_map_light_overrides();
		m_persistent_map_light_known_ids = std::move(current_ids);
		for (const auto& entry : entries)
		{
			if (!entry.persistent_id.empty()) m_persistent_map_light_known_ids.insert(entry.persistent_id);
		}
		m_persistent_map_light_editor_fingerprint = persistent_light_editor_fingerprint(editor_lights);
		m_persistent_map_light_fingerprint_valid = true;
		m_persistent_map_light_dirty = false;
		m_persistent_map_light_save_at = 0.0f;
		m_map_light_config_dirty = false;
		m_persistent_map_light_tombstones = tombstones_written;
		m_persistent_map_light_loaded = authored_count + imported_count;
		m_persistent_map_light_live_linked = live_linked_count;
		m_persistent_map_light_status = std::format(
			"saved {} persistent rigs to {} ({} authored, {} Source/imported, {} live-linked, {} tombstones)",
			authored_count + imported_count, path, authored_count, imported_count,
			live_linked_count, tombstones_written);
		m_map_light_override_status = m_persistent_map_light_status;
		return true;
	}

	bool dynamic_lighting::rotate_selected_map_light_direction(const int axis, const float degrees, const bool apply_to_all_spots)
	{
		if (m_source_bsp_selected_index < 0 ||
			static_cast<std::size_t>(m_source_bsp_selected_index) >= m_source_bsp_candidates.size())
		{
			m_map_light_override_status = "no selected imported spotlight";
			return false;
		}

		std::uint32_t changed = 0u;
		auto rotate_candidate = [&](bsp_light_candidate_s& candidate)
		{
			const bool is_spot = candidate.source_type == static_cast<std::int32_t>(source_bsp_lights::emit_type::spotlight) ||
				utils::str_to_lower(candidate.classname).find("spot") != std::string::npos ||
				utils::str_to_lower(candidate.map_light_classname).find("projectedtexture") != std::string::npos;
			if (!candidate.shaped || !is_spot) return;
			candidate.direction = rotate_direction_around_world_axis(candidate.direction, axis, degrees);
			if (candidate.graph_owner_bound)
			{
				candidate.owner_local_direction = inverse_rotate_source_vector(candidate.direction, candidate.map_owner_angles);
				if (candidate.owner_local_direction.LengthSqr() > 0.0001f) candidate.owner_local_direction.Normalize();
			}
			++changed;
		};

		if (apply_to_all_spots)
		{
			for (auto& candidate : m_source_bsp_candidates) rotate_candidate(candidate);
		}
		else rotate_candidate(m_source_bsp_candidates[static_cast<std::size_t>(m_source_bsp_selected_index)]);

		if (changed == 0u)
		{
			m_map_light_override_status = "selected light is not a shaped spotlight";
			return false;
		}
		const bool applied = remix_api::is_initialized() && remix_lights::get() ? import_scanned_bsp_world_lights() : false;
		m_map_light_override_status = std::format("rotated {} spotlight direction(s) by {:+.0f} degrees around {}{}",
			changed, degrees, axis == 0 ? "X" : axis == 1 ? "Y" : "Z", applied ? " and applied live" : "");
		return true;
	}

	bool dynamic_lighting::reset_selected_map_light_edits()
	{
		if (m_source_bsp_selected_index < 0 ||
			static_cast<std::size_t>(m_source_bsp_selected_index) >= m_source_bsp_candidates.size())
		{
			m_map_light_override_status = "no selected imported light";
			return false;
		}
		auto& candidate = m_source_bsp_candidates[static_cast<std::size_t>(m_source_bsp_selected_index)];
		if (!candidate.editor_baseline_valid)
		{
			m_map_light_override_status = "selected light has no editor baseline";
			return false;
		}
		restore_candidate_editor_baseline(candidate);
		return apply_selected_map_light_edits();
	}

	bool dynamic_lighting::remove_selected_map_light_override()
	{
		if (m_source_bsp_selected_index < 0 ||
			static_cast<std::size_t>(m_source_bsp_selected_index) >= m_source_bsp_candidates.size())
		{
			m_map_light_override_status = "no selected imported light";
			return false;
		}
		std::string map = normalize_map_session_name(m_bsp_worldlight_pending_map);
		if (map.empty()) map = normalize_map_session_name(map_settings::get_map_name());
		const auto& candidate = m_source_bsp_candidates[static_cast<std::size_t>(m_source_bsp_selected_index)];
		const std::string_view classname = candidate.map_light_classname.empty()
			? std::string_view(candidate.classname) : std::string_view(candidate.map_light_classname);
		std::string path;
		std::string error;
		if (!source_map_light_overrides::erase(game::root_path, map,
			static_cast<std::int32_t>(candidate.source_index), candidate.map_hammer_id,
			candidate.map_targetname, classname, path, error))
		{
			m_map_light_override_path = path;
			m_map_light_override_status = error;
			return false;
		}
		m_map_light_override_path = path;
		const bool reloaded = reload_current_map_lights();
		m_map_light_override_status = reloaded
			? "deleted selected-light override and restored map values"
			: "override deleted; map-light reload is pending";
		return true;
	}

	std::string dynamic_lighting::get_selected_map_light_runtime_details()
	{
		if (m_source_bsp_selected_index < 0 ||
			static_cast<std::size_t>(m_source_bsp_selected_index) >= m_source_bsp_candidates.size())
		{
			return "No imported light is selected.";
		}

		const auto source_index = m_source_bsp_candidates[static_cast<std::size_t>(m_source_bsp_selected_index)].source_index;
		for (const auto& binding : m_owned_worldlight_bindings)
		{
			if (binding.source_index != source_index) continue;
			const std::string last_transition = binding.last_transition_time < -90000.0f
				? "never" : std::format("{:.2f}s", binding.last_transition_time);
			return std::format(
				"Runtime decision: {} | owner: {} (entity {}, handle 0x{:08x}) | authored light: {} (entity {}, handle 0x{:08x}) | I/O group {}{} | scheduled: {}{} | controls: {} | state {:.3f} | active {} | transitions {} | last transition {}",
				binding.final_state_reason,
				binding.owner_state_reason,
				binding.runtime_owner_index,
				binding.owner_handle_raw,
				binding.runtime_light_state_reason,
				binding.runtime_light_entity_index,
				binding.runtime_light_handle_raw,
				binding.io_control_group,
				binding.io_group_terminal ? " terminal" : "",
				binding.io_scheduled_state_valid ? (binding.io_scheduled_enabled ? "enabled" : "disabled") : "unset",
				binding.io_scheduled_terminal ? " terminal" : "",
				binding.io_control_summary.empty() ? "none" : binding.io_control_summary,
				binding.state_scalar,
				binding.active ? "yes" : "no",
				binding.transition_count,
				last_transition);
		}
		return "Runtime decision: untracked static light; no owner, lightstyle, authored runtime entity or map I/O group is attached.";
	}

	std::string dynamic_lighting::get_selected_map_light_io_group_details()
	{
		if (m_source_bsp_selected_index < 0 ||
			static_cast<std::size_t>(m_source_bsp_selected_index) >= m_source_bsp_candidates.size())
		{
			return "No imported light is selected.";
		}

		const auto& candidate = m_source_bsp_candidates[static_cast<std::size_t>(m_source_bsp_selected_index)];
		const std::uint32_t group_id = candidate.map_light_control_group;
		if (group_id == 0u) return "Selected light is not part of a reconstructed Source I/O power group.";

		std::string text = std::format("I/O group {}", group_id);
		if (const auto state_it = m_map_light_io_group_states.find(group_id); state_it != m_map_light_io_group_states.end())
		{
			const auto& state = state_it->second;
			text += std::format(" | stable {} | terminal {} | conflict {} | candidate {}{} | votes on/off/kill {}/{}/{} | observations {}",
				state.initialized ? (state.enabled ? "enabled" : "disabled") : "uninitialized",
				state.terminal ? "yes" : "no",
				state.conflict ? "yes" : "no",
				state.candidate_valid ? (state.candidate_enabled ? "enabled" : "disabled") : "none",
				state.candidate_valid ? std::format(" since {:.2f}", state.candidate_since) : std::string{},
				state.last_enabled_votes, state.last_disabled_votes, state.last_terminal_votes, state.observations);
		}
		else text += " | no runtime observations";

		const auto members_it = g_map_entity_graph.control_group_members.find(group_id);
		if (members_it == g_map_entity_graph.control_group_members.end()) return text;
		text += std::format("\nMembers ({}): ", members_it->second.size());
		std::size_t shown = 0u;
		std::unordered_set<std::size_t> action_indices;
		for (const auto source_index : members_it->second)
		{
			if (source_index == 0u || static_cast<std::size_t>(source_index) > g_map_entity_graph.entities.size()) continue;
			const auto& entity = g_map_entity_graph.entities[static_cast<std::size_t>(source_index - 1u)];
			if (shown++) text += ", ";
			text += !entity.targetname.empty()
				? std::format("{}#{}", entity.targetname, source_index)
				: std::format("{}#{}", entity.classname, source_index);
			if (const auto found = g_map_entity_graph.light_source_to_action_indices.find(source_index);
				found != g_map_entity_graph.light_source_to_action_indices.end())
			{
				for (const auto index : found->second) action_indices.insert(index);
			}
			if (shown >= 16u && members_it->second.size() > shown)
			{
				text += std::format(", ... +{}", members_it->second.size() - shown);
				break;
			}
		}

		if (!action_indices.empty()) text += "\nResolved control paths:";
		std::size_t path_count = 0u;
		for (const auto action_index : action_indices)
		{
			if (action_index >= g_map_entity_graph.resolved_light_actions.size()) continue;
			const auto& action = g_map_entity_graph.resolved_light_actions[action_index];
			text += std::format("\n  {} {} after {:.2f}s{}: {}", action.output_name,
				source_map_entities::io_action_kind_name(action.kind), action.delay,
				action.indirect ? std::format(" depth {}", action.path_depth) : std::string{},
				action.path.empty() ? action.target : action.path);
			if (++path_count >= 12u)
			{
				if (action_indices.size() > path_count) text += std::format("\n  ... +{} paths", action_indices.size() - path_count);
				break;
			}
		}
		return text;
	}


	std::string dynamic_lighting::get_selected_map_light_io_scheduler_details()
	{
		if (m_source_bsp_selected_index < 0 ||
			static_cast<std::size_t>(m_source_bsp_selected_index) >= m_source_bsp_candidates.size())
		{
			return "No imported light is selected.";
		}
		const auto& candidate = m_source_bsp_candidates[static_cast<std::size_t>(m_source_bsp_selected_index)];
		const std::uint32_t map_source_index = candidate.map_light_entity_index > 0
			? static_cast<std::uint32_t>(candidate.map_light_entity_index) : 0u;
		if (map_source_index == 0u) return "Selected WORLDLIGHT has no matched map-light entity.";

		std::string text;
		for (const auto& binding : m_owned_worldlight_bindings)
		{
			if (binding.source_index != candidate.source_index) continue;
			text = std::format("Scheduled state: {}{} | last action {:.2f} | serial {} | reason: {}",
				binding.io_scheduled_state_valid ? (binding.io_scheduled_enabled ? "enabled" : "disabled") : "unset",
				binding.io_scheduled_terminal ? " terminal" : "",
				binding.io_last_action_time,
				binding.io_last_action_serial,
				binding.io_scheduled_reason);
			break;
		}
		if (text.empty()) text = "Selected WORLDLIGHT is not runtime tracked.";

		std::size_t pending_count = 0u;
		for (const auto& pending : m_map_light_io_pending_actions)
		{
			if (pending.source_index != map_source_index &&
				(candidate.map_light_control_group == 0u || pending.control_group != candidate.map_light_control_group)) continue;
			if (pending_count++ == 0u) text += "\nPending actions:";
			text += std::format("\n  #{} {} at {:.3f} (authored {:.2f}s, root {}, {}, path {})",
				pending.serial, source_map_entities::io_action_kind_name(pending.kind),
				pending.execute_time, pending.authored_delay, pending.root_action_id,
				pending.runtime_captured ? "captured AcceptInput" : "reconstructed",
				pending.path.empty() ? "resolved map I/O" : pending.path);
			if (pending_count >= 12u) break;
		}
		if (pending_count == 0u) text += "\nNo pending actions for this light/group.";

		if (candidate.map_light_control_group > 0u)
		{
			if (const auto* actions = source_map_entities::find_control_group_actions(
				g_map_entity_graph, candidate.map_light_control_group))
			{
				std::unordered_set<std::uint32_t> roots;
				for (const auto action_index : *actions)
				{
					if (action_index >= g_map_entity_graph.resolved_light_actions.size()) continue;
					const auto& action = g_map_entity_graph.resolved_light_actions[action_index];
					if (!roots.insert(action.root_action_id).second) continue;
					const auto fired = m_map_light_io_root_fire_counts.contains(action.root_action_id)
						? m_map_light_io_root_fire_counts[action.root_action_id] : 0u;
					text += std::format("\nRoot action {} fired {} times{}",
						action.root_action_id, fired,
						action.max_fires > 0 ? std::format(" / max {}", action.max_fires) : std::string{});
				}
			}
		}
		return text;
	}

	bool dynamic_lighting::simulate_selected_map_light_io(const source_map_entities::io_action_kind kind)
	{
		if (m_source_bsp_selected_index < 0 ||
			static_cast<std::size_t>(m_source_bsp_selected_index) >= m_source_bsp_candidates.size()) return false;
		const auto& candidate = m_source_bsp_candidates[static_cast<std::size_t>(m_source_bsp_selected_index)];
		const float curtime = now();
		bool applied = false;
		for (auto& binding : m_owned_worldlight_bindings)
		{
			if (binding.source_index != candidate.source_index) continue;
			if (binding.io_scheduled_terminal && kind != source_map_entities::io_action_kind::kill) continue;
			const bool old_enabled = binding.io_scheduled_state_valid
				? binding.io_scheduled_enabled : !binding.map_light_starts_disabled;
			bool new_enabled = old_enabled;
			switch (kind)
			{
			case source_map_entities::io_action_kind::enable: new_enabled = true; break;
			case source_map_entities::io_action_kind::disable: new_enabled = false; break;
			case source_map_entities::io_action_kind::toggle:
				new_enabled = !old_enabled;
				++m_map_light_io_actions_toggle;
				break;
			case source_map_entities::io_action_kind::kill:
				new_enabled = false;
				binding.io_scheduled_terminal = true;
				binding.io_group_terminal = true;
				++m_map_light_io_actions_kill;
				break;
			default: return false;
			}
			binding.io_scheduled_state_valid = true;
			binding.io_scheduled_enabled = new_enabled;
			binding.io_last_action_time = curtime;
			binding.io_last_action_serial = m_map_light_io_next_serial++;
			binding.io_scheduled_reason = std::format("manual inspector {}",
				source_map_entities::io_action_kind_name(kind));
			++m_map_light_io_actions_manual;
			++m_map_light_io_actions_executed;
			applied = true;
			if (m_map_light_transition_log.size() >= 96u)
			{
				m_map_light_transition_log.pop_front();
				++m_map_light_transition_log_dropped;
			}
			m_map_light_transition_log.push_back({ curtime, binding.source_index, binding.io_control_group,
				std::format("manual state {}", old_enabled ? "enabled" : "disabled"),
				binding.io_scheduled_reason });
			m_map_light_last_transition = std::format("WORLDLIGHT #{}: {}",
				binding.source_index, binding.io_scheduled_reason);
			++m_map_light_reason_transitions;
		}
		return applied;
	}

	std::string dynamic_lighting::get_map_light_transition_log_text()
	{
		if (m_map_light_transition_log.empty()) return "No runtime map-light transitions have been recorded.";
		std::string text;
		const std::size_t begin = m_map_light_transition_log.size() > 24u
			? m_map_light_transition_log.size() - 24u : 0u;
		for (std::size_t i = begin; i < m_map_light_transition_log.size(); ++i)
		{
			const auto& entry = m_map_light_transition_log[i];
			if (entry.source_index == 0u)
				text += std::format("[{:.2f}] I/O GROUP {}: {} -> {}\n",
					entry.time, entry.control_group, entry.from_reason, entry.to_reason);
			else
				text += std::format("[{:.2f}] WORLDLIGHT #{} group {}: {} -> {}\n",
					entry.time, entry.source_index, entry.control_group, entry.from_reason, entry.to_reason);
		}
		return text;
	}

	void dynamic_lighting::clear_map_light_transition_log()
	{
		m_map_light_transition_log.clear();
		m_map_light_transition_log_dropped = 0u;
		m_map_light_last_transition = "none";
	}

	bool dynamic_lighting::scan_current_bsp_world_lights()
	{
		m_source_bsp_candidates.clear();
		m_source_bsp_selected_index = -1;
		m_source_bsp_scan_entities = 0u;
		m_source_bsp_scan_lights = 0u;
		m_source_bsp_scan_previewed = 0u;
		m_source_bsp_preview_live_count = 0u;
		m_bsp_worldlight_records = 0u;
		m_bsp_worldlight_candidates = 0u;
		m_bsp_worldlight_imported = 0u;
		m_bsp_worldlight_create_failed = 0u;
		m_bsp_worldlight_skipped_type = 0u;
		m_bsp_worldlight_skipped_style = 0u;
		m_bsp_worldlight_skipped_intensity = 0u;
		m_bsp_worldlight_skipped_distance = 0u;
		m_bsp_worldlight_merged_duplicates = 0u;
		m_bsp_worldlight_stream_refreshes = 0u;
		m_bsp_worldlight_stream_retained = 0u;
		m_bsp_worldlight_stream_evicted = 0u;
		m_bsp_worldlight_surface_clustered = 0u;
		m_map_event_controlled_candidates = 0u;
		m_map_event_sprite_candidates = 0u;
		m_map_event_styled_candidates = 0u;
		m_surface_material_linked = 0u;
		m_surface_material_unresolved = 0u;
		m_surface_event_linked = 0u;
		m_surface_material_status = "scanning BSP TEXINFO / TEXDATA";
		m_source_distant_entities_found = 0u;
		m_source_environment_light_environment_found = 0u;
		m_source_environment_light_directional_found = 0u;
		m_source_environment_cascade_found = 0u;
		m_source_environment_shadow_control_found = 0u;
		m_source_environment_env_sun_found = 0u;
		m_source_environment_selected_alias = "none";
		m_source_distant_worldlights_found = 0u;
		m_source_distant_skyambient_found = 0u;
		m_source_distant_hdr_sets_found = 0u;
		m_source_distant_ldr_sets_found = 0u;
		m_source_distant_candidates_rejected = 0u;
		m_source_distant_selected_confidence = 0.0f;
		m_source_distant_direction_disagreement = 0.0f;
		m_source_distant_selected_hdr = false;
		m_source_distant_sign_flipped = false;
		m_source_distant_detected = false;
		m_source_distant_imported = false;
		m_source_distant_status = "scanning map environment";
		m_source_distant_diagnostics.clear();
		m_bsp_worldlight_backend = "none";
		m_bsp_worldlight_diagnostics.clear();
		m_bsp_worldlight_source.clear();

		std::string map = normalize_map_session_name(m_bsp_worldlight_pending_map);
		if (map.empty()) {
			map = normalize_map_session_name(map_settings::get_map_name());
		}
		if (map.empty())
		{
			m_bsp_worldlight_status = "no current map name";
			return false;
		}

		m_map_entity_graph_matched_lights = 0u;
		m_map_entity_graph_bound_owners = 0u;
		m_map_entity_graph_extruded = 0u;
		if (m_map_entity_graph_enabled && (!g_map_entity_graph_loaded || g_map_entity_graph_map != map)) {
			rebuild_map_entity_graph();
		}
		if ((m_source_distant_light_import || m_bsp_worldlight_include_environment_records) &&
			(!g_map_entity_graph_loaded || g_map_entity_graph_map != map))
		{
			ensure_source_environment_graph(map);
		}
		reload_map_light_overrides();

		source_bsp_lights::world_light_result_s parsed = {};
		std::string engine_error;
		bool parsed_ok = false;

		if (m_bsp_worldlight_prefer_engine_memory)
		{
			const auto* worldbrush = game::get_hoststate_worldbrush_data();
			parsed_ok = source_bsp_lights::read_world_lights_from_engine(worldbrush, parsed);
			if (!parsed_ok) {
				engine_error = parsed.error;
			}
		}

		if (!parsed_ok)
		{
			source_bsp_lights::map_file_s map_file = {};
			std::string file_error;
			if (!source_bsp_lights::load_map_file(game::root_path, map, map_file, file_error))
			{
				m_bsp_worldlight_backend = "none";
				m_bsp_worldlight_diagnostics = engine_error;
				m_bsp_worldlight_status = std::format("runtime import failed: {}; BSP fallback failed: {}",
					engine_error.empty() ? "not attempted" : engine_error,
					file_error.empty() ? "BSP not found" : file_error);
				return false;
			}

			parsed_ok = source_bsp_lights::read_world_lights(map_file, m_bsp_worldlight_prefer_hdr, parsed);
			if (!parsed_ok)
			{
				m_bsp_worldlight_source = map_file.source_path;
				m_bsp_worldlight_backend = "BSP entity fallback";
				m_bsp_worldlight_diagnostics = parsed.diagnostics;

				// A damaged or branch-specific WORLDLIGHT lump must not hide the map sun.
				// The entity graph is parsed from a separate BSP lump, so light_environment can
				// still produce the required singleton Distant light on its own.
				source_bsp_lights::world_light_result_s entity_only = {};
				entity_only.source_path = map_file.source_path;
				entity_only.backend = map_file.from_vpk
					? source_bsp_lights::source_backend::vpk_bsp
					: source_bsp_lights::source_backend::loose_bsp;
				entity_only.used_hdr = m_bsp_worldlight_prefer_hdr;
				if (m_source_distant_allow_entity_fallback && append_source_distant_candidate(entity_only))
				{
					m_bsp_worldlight_candidates = static_cast<std::uint32_t>(m_source_bsp_candidates.size());
					m_source_bsp_scan_lights = m_bsp_worldlight_candidates;
					m_source_bsp_selected_index = m_source_bsp_candidates.empty() ? -1 : 0;
					if (m_map_light_auto_save_config && !m_source_bsp_candidates.empty())
					{
						save_all_map_lights_to_config();
						m_map_light_config_dirty = true;
						m_map_light_config_save_at = now() + 0.75f;
					}
					m_bsp_worldlight_status = std::format(
						"WORLDLIGHTS unavailable ({}); recovered one global Source light from entity aliases",
						parsed.error.empty() ? "unknown layout" : parsed.error);
					return true;
				}

				m_bsp_worldlight_status = std::format("runtime import failed: {}; BSP fallback failed: {}",
					engine_error.empty() ? "not attempted" : engine_error,
					parsed.error.empty() ? "WORLDLIGHTS unavailable" : parsed.error);
				return false;
			}
		}

		m_bsp_worldlight_source = parsed.source_path;
		m_bsp_worldlight_backend = source_bsp_lights::source_backend_name(parsed.backend);
		m_bsp_worldlight_diagnostics = parsed.diagnostics;
		if (!engine_error.empty() && parsed.backend != source_bsp_lights::source_backend::engine_memory) {
			m_bsp_worldlight_diagnostics = std::format("engine: {} | fallback: {}", engine_error, parsed.diagnostics);
		}
		// Engine-memory WORLDLIGHTS do not carry the BSP TEXINFO string table. Resolve
		// it from the map file so emit_surface/facing-poly records can retain their
		// actual Source material provenance.
		if (std::any_of(parsed.lights.begin(), parsed.lights.end(), [](const auto& light) { return light.texinfo >= 0 && light.material_name.empty(); }))
		{
			source_bsp_lights::map_file_s material_map = {};
			std::string material_map_error;
			if (source_bsp_lights::load_map_file(game::root_path, map, material_map, material_map_error))
			{
				std::unordered_map<std::int32_t, std::string> material_names;
				std::string material_error;
				if (source_bsp_lights::read_texinfo_materials(material_map, material_names, material_error))
				{
					for (auto& light : parsed.lights)
					{
						if (const auto it = material_names.find(light.texinfo); it != material_names.end()) light.material_name = it->second;
					}
				}
			}
		}
		m_bsp_worldlight_records = static_cast<std::uint32_t>(parsed.lights.size());
		const auto* camera = game::get_current_view_origin();
		std::uint32_t source_index = 0u;
		for (const auto& light : parsed.lights)
		{
			++source_index;
			if (!bsp_worldlight_type_allowed(light.type))
			{
				++m_bsp_worldlight_skipped_type;
				continue;
			}
			if (!m_bsp_worldlight_include_styled && light.style != 0)
			{
				++m_bsp_worldlight_skipped_style;
				continue;
			}

			const float max_intensity = std::max({ light.intensity.x, light.intensity.y, light.intensity.z });
			if (!std::isfinite(max_intensity) || max_intensity < std::max(0.0f, m_bsp_worldlight_min_intensity))
			{
				++m_bsp_worldlight_skipped_intensity;
				continue;
			}

			Vector color(
				std::max(0.0f, light.intensity.x) / max_intensity,
				std::max(0.0f, light.intensity.y) / max_intensity,
				std::max(0.0f, light.intensity.z) / max_intensity);
			const float scalar = std::clamp(max_intensity * std::max(0.0f, m_bsp_worldlight_intensity_scale), 1.0f, 250000.0f);
			const float source_radius = bsp_worldlight_source_radius(light);
			const float radius = std::clamp(source_radius * std::max(0.00001f, m_bsp_worldlight_radius_scale),
				std::max(0.01f, m_bsp_worldlight_min_radius),
				std::max(m_bsp_worldlight_min_radius, m_bsp_worldlight_max_radius));

			const bool environment_record = light.type == source_bsp_lights::emit_type::skylight ||
				light.type == source_bsp_lights::emit_type::skyambient;
			bool shaped = light.type == source_bsp_lights::emit_type::spotlight ||
				light.type == source_bsp_lights::emit_type::surface ||
				light.type == source_bsp_lights::emit_type::skylight;
			Vector direction = light.normal;
			if (direction.LengthSqr() <= 0.0001f) {
				direction = Vector(0.0f, 0.0f, -1.0f);
			}
			direction.Normalize();
			float degrees = 180.0f;
			float softness = 0.0f;
			float exponent = 0.0f;
			if (light.type == source_bsp_lights::emit_type::spotlight)
			{
				degrees = bsp_worldlight_outer_degrees(light);
				softness = bsp_worldlight_softness(light);
				exponent = std::clamp(std::isfinite(light.exponent) ? light.exponent : 0.0f, 0.0f, 16.0f);
			}
			else if (light.type == source_bsp_lights::emit_type::surface)
			{
				degrees = 90.0f;
				softness = 0.35f;
				exponent = 0.0f;
			}
			else if (light.type == source_bsp_lights::emit_type::skylight)
			{
				degrees = 1.0f;
				softness = 0.0f;
				exponent = 0.0f;
			}

			Vector world_origin = light.origin;
			Vector world_direction = direction;
			const bool owner_is_map_reference = g_map_entity_graph_loaded && map_entity_from_owner_reference(light.owner) != nullptr;
			if (light.owner > 0 && m_bsp_worldlight_bind_owners && !owner_is_map_reference)
			{
				Vector owner_origin(0.0f, 0.0f, 0.0f);
				Vector owner_angles(0.0f, 0.0f, 0.0f);
				bool owner_visible = false;
				if (get_runtime_owner_state(light.owner, owner_origin, owner_angles, owner_visible))
				{
					world_origin = owner_origin + rotate_source_local_vector(light.origin, owner_angles);
					world_direction = rotate_source_local_vector(direction, owner_angles);
					if (world_direction.LengthSqr() > 0.0001f) {
						world_direction.Normalize();
					}
				}
			}

			if (m_bsp_worldlight_merge_duplicates)
			{
				const auto duplicate = std::find_if(m_source_bsp_candidates.begin(), m_source_bsp_candidates.end(), [&](const bsp_light_candidate_s& candidate)
				{
					return candidate.from_worldlight && candidate.source_type == static_cast<std::int32_t>(light.type) &&
						candidate.origin.DistToSqr(world_origin) <= 4.0f;
				});
				if (duplicate != m_source_bsp_candidates.end())
				{
					++m_bsp_worldlight_merged_duplicates;
					continue;
				}
			}

			bsp_light_candidate_s candidate = {};
			candidate.source_index = source_index;
			candidate.classname = std::format("worldlight_{}", source_bsp_lights::emit_type_name(light.type));
			const char* light_set_name = parsed.backend == source_bsp_lights::source_backend::engine_memory
				? "runtime" : (parsed.used_hdr ? "HDR" : "LDR");
			candidate.comment = std::format("WORLDLIGHT #{} {} {} style={} owner={} flags=0x{:x} cluster={}", source_index,
				light_set_name, source_bsp_lights::emit_type_name(light.type), light.style, light.owner,
				static_cast<std::uint32_t>(light.flags), light.cluster);
			candidate.origin = world_origin;
			candidate.radiance = color;
			candidate.scalar = scalar;
			candidate.radius = radius;
			candidate.shaped = shaped;
			candidate.direction = world_direction;
			candidate.degrees = degrees;
			candidate.softness = softness;
			candidate.exponent = exponent;
			candidate.selected = !environment_record ||
				(m_bsp_worldlight_enable_environment_helpers && !m_source_distant_light_import);
			candidate.from_worldlight = true;
			candidate.hdr_worldlight = parsed.used_hdr;
			candidate.source_type = static_cast<std::int32_t>(light.type);
			candidate.style = light.style;
			if (light.style > 0) ++m_map_event_styled_candidates;
			candidate.flags = light.flags;
			candidate.owner = light.owner;
			candidate.texinfo = light.texinfo;
			candidate.surface_material_name = light.material_name;
			candidate.surface_texinfo_linked = light.type == source_bsp_lights::emit_type::surface &&
				m_map_event_surface_material_linkage && light.texinfo >= 0 && !light.material_name.empty();
			if (candidate.surface_texinfo_linked && material_exporter::m_link_event_emissive_materials)
			{
				candidate.surface_pbr_emissive_linked = material_exporter::query_event_emissive_material(
					light.material_name, &candidate.surface_pbr_emissive_reason);
			}
			if (light.type == source_bsp_lights::emit_type::surface)
			{
				if (candidate.surface_texinfo_linked)
				{
					candidate.surface_link_reason = std::format("BSP texinfo {} -> materials/{}.vmt", light.texinfo, light.material_name);
					candidate.comment += std::format(" surfaceMaterial=materials/{}.vmt", light.material_name);
					if (candidate.surface_pbr_emissive_linked)
					{
						candidate.surface_link_reason += " + Auto PBR emissive graph";
						candidate.comment += " pbrEmissiveLink=1";
					}
					++m_surface_material_linked;
				}
				else
				{
					candidate.surface_link_reason = std::format("surface WORLDLIGHT texinfo {} unresolved", light.texinfo);
					++m_surface_material_unresolved;
				}
			}
			candidate.owner_local_origin = light.origin;
			candidate.owner_local_direction = direction;
			candidate.owner_relative = light.owner > 0;
			if (environment_record)
			{
				candidate.comment += m_bsp_worldlight_enable_environment_helpers
					? " environment_record experimental_local_helper"
					: " environment_record registry_only";
			}
			if (light.type == source_bsp_lights::emit_type::surface && m_bsp_worldlight_surface_clusters)
			{
				candidate.surface_cluster = true;
				candidate.surface_cluster_samples = std::clamp(m_bsp_worldlight_surface_cluster_samples, 1, 16);
				candidate.surface_cluster_spread = std::clamp(m_bsp_worldlight_surface_cluster_spread, 0.0f, 4.0f);
				candidate.surface_cluster_aspect = std::clamp(m_bsp_worldlight_surface_cluster_aspect, 0.1f, 8.0f);
				candidate.surface_cluster_intensity = std::clamp(m_bsp_worldlight_surface_cluster_intensity, 0.0f, 4.0f);
				candidate.surface_cluster_radius_scale = std::clamp(m_bsp_worldlight_surface_cluster_radius_scale, 0.01f, 4.0f);
				candidate.surface_cluster_pattern = "cross";
			}

			if (g_map_entity_graph_loaded && m_map_light_match_entities)
			{
				float light_match_score = -1.0e9f;
				const auto* map_light = source_map_entities::find_best_light_entity(g_map_entity_graph,
					world_origin, light.style, static_cast<std::int32_t>(light.type), world_direction,
					m_map_light_entity_match_distance, &light_match_score);
				if (map_light)
				{
					candidate.graph_matched = true;
					candidate.map_light_entity_index = static_cast<std::int32_t>(map_light->source_index);
					candidate.map_hammer_id = map_light->hammer_id;
					candidate.map_targetname = map_light->targetname;
					candidate.map_light_classname = map_light->classname;
					candidate.map_light_origin = candidate.origin;
					candidate.map_light_starts_disabled = map_light->starts_disabled;
					candidate.map_light_runtime_trackable = source_map_entities::class_is_runtime_light(map_light->classname);
					candidate.map_light_control_group = source_map_entities::find_light_control_group(g_map_entity_graph, map_light->source_index);
					candidate.map_light_io_killable = source_map_entities::light_has_control_kind(g_map_entity_graph, map_light->source_index, source_map_entities::io_action_kind::kill);
					candidate.map_light_io_group_kill_all = source_map_entities::control_group_all_have_kind(g_map_entity_graph, candidate.map_light_control_group, source_map_entities::io_action_kind::kill);
					candidate.map_light_control_summary = source_map_entities::describe_light_controls(g_map_entity_graph, map_light->source_index);
					const auto map_light_class_lower = utils::str_to_lower(map_light->classname);
					candidate.surface_event_proxy = light.type == source_bsp_lights::emit_type::surface &&
						m_map_event_include_sprite_proxies && (map_light_class_lower == "env_sprite" || map_light_class_lower == "env_lightglow");
					if (candidate.map_light_control_group > 0u) ++m_map_event_controlled_candidates;
					if (candidate.surface_event_proxy)
					{
						++m_map_event_sprite_candidates;
						++m_surface_event_linked;
						candidate.surface_link_reason += candidate.surface_link_reason.empty() ? "event sprite/glow proxy" : " + event sprite/glow proxy";
					}
					candidate.map_parentname = map_light->parentname;
					candidate.map_owner_attachment = map_light->parent_attachment;
					if (!map_light->targetname.empty()) candidate.targetname = map_light->targetname;
					++m_map_entity_graph_matched_lights;
				}

				const source_map_entities::entity_s* map_owner = nullptr;
				std::string binding_reason;
				if (light.owner >= 0)
				{
					map_owner = map_entity_from_owner_reference(light.owner);
					if (map_owner) binding_reason = "WORLDLIGHT owner reference";
				}
				if (!map_owner && map_light && !map_light->parentname.empty())
				{
					map_owner = source_map_entities::find_best_owner_entity(g_map_entity_graph, world_origin,
						map_light->parentname, m_map_light_owner_bind_distance);
					if (map_owner) binding_reason = "light parentname";
				}
				if (!map_owner && m_map_light_auto_bind_nearby_owners)
				{
					map_owner = source_map_entities::find_best_owner_entity(g_map_entity_graph, world_origin, std::string_view{},
						m_map_light_owner_bind_distance);
					if (map_owner) binding_reason = "nearest movable/breakable map owner";
				}

				if (map_owner)
				{
					candidate.graph_owner_bound = true;
					candidate.map_owner_entity_index = static_cast<std::int32_t>(map_owner->source_index);
					candidate.map_owner_classname = map_owner->classname;
					candidate.map_owner_targetname = map_owner->targetname;
					candidate.map_owner_model = map_owner->model;
					candidate.map_owner_hammer_id = map_owner->hammer_id;
					candidate.map_owner_initial_health = map_owner->initial_health;
					candidate.map_owner_breakable = map_owner->is_breakable_owner;
					if (candidate.map_owner_attachment.empty() && map_light) candidate.map_owner_attachment = map_light->parent_attachment;
					candidate.map_owner_origin = map_owner->origin;
					candidate.map_owner_angles = map_owner->angles;
					candidate.binding_reason = binding_reason;
					candidate.owner_relative = true;
					candidate.owner_local_origin = inverse_rotate_source_vector(world_origin - map_owner->origin, map_owner->angles);
					candidate.owner_local_direction = inverse_rotate_source_vector(world_direction, map_owner->angles);
					if (candidate.owner_local_direction.LengthSqr() > 0.0001f) candidate.owner_local_direction.Normalize();
					++m_map_entity_graph_bound_owners;
				}
			}

			if (!candidate.graph_owner_bound && m_map_light_extrude_from_models && candidate.shaped &&
				candidate.direction.LengthSqr() > 0.0001f && m_map_light_model_surface_offset > 0.0f)
			{
				candidate.origin += candidate.direction * m_map_light_model_surface_offset;
				candidate.origin_extruded = true;
				++m_map_entity_graph_extruded;
			}

			if (m_map_light_overrides_enabled)
			{
				if (const auto* override_data = source_map_light_overrides::find_best(g_map_light_overrides,
					static_cast<std::int32_t>(candidate.source_index), candidate.map_hammer_id,
					candidate.map_targetname, candidate.map_light_classname.empty() ? candidate.classname : candidate.map_light_classname))
				{
					apply_candidate_override(candidate, *override_data);
					++m_map_light_overrides_applied;
					if (candidate.override_disabled) ++m_map_light_overrides_disabled;
					if (override_data->has_unbind_owner && override_data->unbind_owner) {
						clear_candidate_owner_binding(candidate);
					}
					else if (override_data->has_owner_targetname && g_map_entity_graph_loaded)
					{
						if (const auto* forced_owner = source_map_entities::find_by_targetname(g_map_entity_graph, override_data->owner_targetname);
							forced_owner && forced_owner->is_bindable_owner)
						{
							bind_candidate_to_map_owner(candidate, *forced_owner, "per-map override owner");
						}
					}
					else if (candidate.graph_owner_bound)
					{
						candidate.owner_local_origin = inverse_rotate_source_vector(candidate.origin - candidate.map_owner_origin, candidate.map_owner_angles);
						candidate.owner_local_direction = inverse_rotate_source_vector(candidate.direction, candidate.map_owner_angles);
						if (candidate.owner_local_direction.LengthSqr() > 0.0001f) candidate.owner_local_direction.Normalize();
					}
				}
			}
			if (candidate.surface_cluster) ++m_bsp_worldlight_surface_clustered;

			if (candidate.graph_matched || candidate.graph_owner_bound || candidate.origin_extruded || candidate.override_applied)
			{
				candidate.comment += std::format(" graphLight={} hammer={} graphOwner={} bind={} offset={}",
					candidate.map_light_entity_index, candidate.map_hammer_id, candidate.map_owner_entity_index,
					candidate.binding_reason.empty() ? "none" : candidate.binding_reason,
					candidate.origin_extruded ? "forward" : "none");
				if (!candidate.map_owner_model.empty()) candidate.comment += std::format(" ownerModel={}", candidate.map_owner_model);
				if (!candidate.map_owner_attachment.empty()) candidate.comment += std::format(" attachment={}", candidate.map_owner_attachment);
				if (candidate.map_light_control_group > 0u) candidate.comment += std::format(" ioGroup={}", candidate.map_light_control_group);
				if (candidate.map_light_io_killable) candidate.comment += " ioKillable";
				if (candidate.override_applied) candidate.comment += std::format(" override={}", candidate.override_summary.empty() ? "matched" : candidate.override_summary);
			}

			if (camera)
			{
				candidate.camera_distance = camera->DistTo(candidate.origin);
				candidate.near_camera = environment_record ||
					candidate.camera_distance <= std::max(0.0f, m_bsp_worldlight_max_distance);
			}
			else if (environment_record)
			{
				candidate.near_camera = true;
			}
			capture_candidate_editor_baseline(candidate);
			m_source_bsp_candidates.emplace_back(std::move(candidate));
		}

		if (m_source_distant_light_import || m_bsp_worldlight_include_environment_records)
		{
			append_source_distant_candidate(parsed);
		}

		m_bsp_worldlight_candidates = static_cast<std::uint32_t>(m_source_bsp_candidates.size());
		m_source_bsp_scan_lights = m_bsp_worldlight_candidates;
		m_surface_material_status = std::format("emit_surface material-linked {} | unresolved {} | event sprite/glow linked {}",
			m_surface_material_linked, m_surface_material_unresolved, m_surface_event_linked);
		if (m_map_light_auto_save_config && !m_source_bsp_candidates.empty())
		{
			save_all_map_lights_to_config();
			// Allow stable runtime entity lights to appear before the one-time automatic
			// Light Editor sync and final config refresh for this map session.
			m_map_light_config_dirty = true;
			m_map_light_config_save_at = now() + 0.75f;
		}
		if (!m_source_bsp_candidates.empty()) {
			m_source_bsp_selected_index = 0;
		}
		const char* loaded_set = parsed.backend == source_bsp_lights::source_backend::engine_memory
			? "runtime" : (parsed.used_hdr ? "HDR" : "LDR");
		m_bsp_worldlight_status = std::format("loaded {} {} WORLDLIGHT records via {} ({} candidates, distant {}, graph matched/bound {}/{}, overrides {}/{}, surface clusters {}, skips {}/{}/{}, merged {})",
			m_bsp_worldlight_records, loaded_set, m_bsp_worldlight_backend,
			m_bsp_worldlight_candidates, m_source_distant_detected ? "detected" : "missing",
			m_map_entity_graph_matched_lights, m_map_entity_graph_bound_owners,
			m_map_light_overrides_applied, m_map_light_overrides_disabled, m_bsp_worldlight_surface_clustered,
			m_bsp_worldlight_skipped_type, m_bsp_worldlight_skipped_style,
			m_bsp_worldlight_skipped_intensity, m_bsp_worldlight_merged_duplicates);
		m_source_bsp_scan_status = m_bsp_worldlight_status;
		return !m_source_bsp_candidates.empty();
	}

	bool dynamic_lighting::import_scanned_bsp_world_lights()
	{
		if (!remix_lights::get())
		{
			m_bsp_worldlight_status = "Light backend is not ready";
			return false;
		}

		// Native analytical lights require Remix API, but the V20.6 global-light
		// translation can still submit D3DLIGHT_DIRECTIONAL through the ASI. Do not
		// block the entire import before the singleton Distant candidate is reached.

		const auto previous_active_sources = m_bsp_worldlight_active_sources;
		const auto preserved_group_states = m_map_light_io_group_states;
		const auto preserved_pending_actions = m_map_light_io_pending_actions;
		const auto preserved_root_fire_counts = m_map_light_io_root_fire_counts;
		const auto preserved_next_serial = m_map_light_io_next_serial;
		clear_bsp_worldlight_lights();
		m_map_light_io_group_states = preserved_group_states;
		m_map_light_io_pending_actions = preserved_pending_actions;
		m_map_light_io_pending_count = static_cast<std::uint32_t>(m_map_light_io_pending_actions.size());
		m_map_light_io_root_fire_counts = preserved_root_fire_counts;
		m_map_light_io_next_serial = preserved_next_serial;
		m_bsp_worldlight_status = "importing WORLDLIGHTS";
		m_source_distant_imported = false;
		m_bsp_worldlight_skipped_distance = 0u;
		const auto* camera = game::get_current_view_origin();
		std::vector<bsp_light_candidate_s*> candidates;
		candidates.reserve(m_source_bsp_candidates.size());
		for (auto& candidate : m_source_bsp_candidates)
		{
			// A Source global light can originate from the entity lump when WORLDLIGHTS is
			// unavailable or compressed. Do not apply the local WORLDLIGHT-only gate to
			// the singleton Distant candidate. V20.5 detected these candidates but silently
			// discarded them here before CreateLight/SetLight could run.
			if ((!candidate.from_worldlight && !candidate.distant) || !candidate.selected) {
				continue;
			}
			// A full-rig record loaded from <map>.toml is authoritative. Do not
			// recreate the same static WORLDLIGHT after the delayed BSP scan.
			if (m_persistent_map_light_materialized_sources.contains(static_cast<std::int32_t>(candidate.source_index)))
			{
				m_bsp_worldlight_active_sources.insert(candidate.source_index);
				continue;
			}
			candidate.previously_streamed = previous_active_sources.contains(candidate.source_index);
			const auto candidate_type = static_cast<source_bsp_lights::emit_type>(candidate.source_type);
			const bool environment_record = candidate_type == source_bsp_lights::emit_type::skylight ||
				candidate_type == source_bsp_lights::emit_type::skyambient;
			if (camera)
			{
				candidate.camera_distance = camera->DistTo(candidate.origin);
				const float hysteresis = candidate.previously_streamed && m_bsp_worldlight_prefer_previous_stream_set
					? std::max(0.0f, m_bsp_worldlight_stream_hysteresis) : 0.0f;
				candidate.near_camera = environment_record ||
					candidate.camera_distance <= std::max(0.0f, m_bsp_worldlight_max_distance) + hysteresis;
			}
			else if (environment_record)
			{
				candidate.near_camera = true;
			}
			if (m_bsp_worldlight_near_camera_only && (((!camera) && !environment_record) || !candidate.near_camera))
			{
				++m_bsp_worldlight_skipped_distance;
				continue;
			}
			candidates.emplace_back(&candidate);
		}

		std::sort(candidates.begin(), candidates.end(), [](const auto* lhs, const auto* rhs)
		{
			if (lhs->distant != rhs->distant) return lhs->distant;
			const float lhs_bias = lhs->previously_streamed && dynamic_lighting::m_bsp_worldlight_prefer_previous_stream_set
				? dynamic_lighting::m_bsp_worldlight_stream_hysteresis * 0.75f : 0.0f;
			const float rhs_bias = rhs->previously_streamed && dynamic_lighting::m_bsp_worldlight_prefer_previous_stream_set
				? dynamic_lighting::m_bsp_worldlight_stream_hysteresis * 0.75f : 0.0f;
			return lhs->camera_distance - lhs_bias < rhs->camera_distance - rhs_bias;
		});

		// Distant is global map lighting, not a streamed local WORLDLIGHT. Keep every
		// selected singleton Distant outside the local-light budget, including when the
		// local import limit is zero.
		const std::size_t local_limit = static_cast<std::size_t>(std::max(0, m_bsp_worldlight_import_limit));
		const std::size_t distant_count = static_cast<std::size_t>(std::count_if(candidates.begin(), candidates.end(),
			[](const auto* candidate) { return candidate && candidate->distant; }));
		const std::size_t total_limit = distant_count + local_limit;
		if (candidates.size() > total_limit) {
			candidates.resize(total_limit);
		}

		m_bsp_worldlight_untracked_active = 0u;
		for (const auto* candidate : candidates)
		{
			const bool runtime_tracked = !candidate->distant && (candidate->style > 0 || candidate->graph_owner_bound ||
				candidate->map_light_runtime_trackable || candidate->map_light_control_group > 0u ||
				(candidate->owner_relative && m_bsp_worldlight_bind_owners && m_map_light_use_raw_owner_indices));
			if (runtime_tracked)
			{
				auto def = make_light_def_from_bsp_candidate(*candidate, "BSP tracked worldlight: ");
				def.group = "bsp_worldlights_tracked";
				owned_worldlight_binding_s binding = {};
				binding.source_index = candidate->source_index;
				binding.owner_index = candidate->graph_owner_bound ? -1 :
					(m_map_light_use_raw_owner_indices ? candidate->owner : -1);
				binding.map_owner_source_index = candidate->map_owner_entity_index;
				binding.style = candidate->style;
				binding.local_origin = candidate->owner_local_origin;
				binding.local_direction = candidate->owner_local_direction;
				binding.authored_world_origin = candidate->origin;
				binding.authored_world_direction = candidate->direction;
				binding.map_owner_origin = candidate->map_owner_origin;
				binding.map_owner_angles = candidate->map_owner_angles;
				binding.owner_class_hint = candidate->map_owner_classname;
				binding.owner_targetname = candidate->map_owner_targetname;
				binding.owner_model_hint = candidate->map_owner_model;
				binding.owner_attachment = candidate->map_owner_attachment;
				binding.map_light_source_index = candidate->map_light_entity_index;
				binding.io_control_group = candidate->map_light_control_group;
				binding.io_killable = candidate->map_light_io_killable;
				binding.io_group_kill_all = candidate->map_light_io_group_kill_all;
				binding.io_control_summary = candidate->map_light_control_summary;
				binding.map_light_class_hint = candidate->map_light_classname;
				binding.map_light_targetname = candidate->map_targetname;
				binding.map_light_origin = candidate->map_light_origin;
				binding.map_light_starts_disabled = candidate->map_light_starts_disabled;
				binding.runtime_light_trackable = candidate->map_light_runtime_trackable;
				binding.runtime_light_last_enabled = !m_map_event_honor_starts_disabled || !candidate->map_light_starts_disabled;
				binding.runtime_light_pending_enabled = binding.runtime_light_last_enabled;
				binding.io_scheduled_state_valid = binding.io_control_group > 0u;
				binding.io_scheduled_enabled = binding.runtime_light_last_enabled;
				binding.io_scheduled_reason = binding.io_control_group > 0u
					? std::format("authored map I/O start {}", binding.io_scheduled_enabled ? "enabled" : "disabled")
					: "no scheduled map I/O action";
				if (binding.map_light_source_index > 0)
				{
					if (const auto exact = m_map_light_io_exact_states.find(
						static_cast<std::uint32_t>(binding.map_light_source_index));
						exact != m_map_light_io_exact_states.end())
					{
						binding.io_scheduled_state_valid = true;
						binding.io_scheduled_enabled = exact->second.enabled;
						binding.io_scheduled_terminal = exact->second.terminal;
						binding.io_group_terminal = exact->second.terminal;
						binding.io_last_action_time = exact->second.changed_at;
						binding.io_last_action_serial = exact->second.serial;
						binding.io_scheduled_reason = exact->second.reason;
					}
				}
				binding.runtime_light_state_reason = binding.runtime_light_trackable
					? (binding.runtime_light_last_enabled ? "authored start enabled" : "authored start disabled")
					: "not runtime-tracked";
				binding.owner_hammer_id = candidate->map_owner_hammer_id;
				binding.owner_initial_health = candidate->map_owner_initial_health;
				binding.owner_breakable = candidate->map_owner_breakable;
				binding.binding_reason = candidate->binding_reason;
				binding.graph_bound = candidate->graph_owner_bound;
				binding.local_transform_valid = candidate->graph_owner_bound ||
					(candidate->owner_relative && m_map_light_use_raw_owner_indices);
				binding.extrude_from_owner = binding.local_transform_valid;
				binding.owner_last_seen_time = now();
				binding.owner_last_rebind_attempt_time = -99999.0f;
				binding.last_state_update_time = now();
				binding.state_scalar = binding.runtime_light_trackable && m_map_event_honor_starts_disabled && binding.map_light_starts_disabled ? 0.0f : 1.0f;
				binding.def = std::move(def);
				m_owned_worldlight_bindings.emplace_back(std::move(binding));
				m_bsp_worldlight_active_sources.insert(candidate->source_index);
				continue;
			}

			if (candidate->distant)
			{
				std::string map = normalize_map_session_name(m_bsp_worldlight_pending_map);
				if (map.empty()) map = normalize_map_session_name(map_settings::get_map_name());
				const std::uint64_t stable_hash = utils::string_hash64(std::format(
					"source-map-distant:{}:{}", map, candidate->source_index));
				const Vector final_radiance = candidate->radiance * candidate->scalar;
				const bool created = remix_lights::get()->upsert_source_distant_light(
					stable_hash, candidate->direction, final_radiance,
					candidate->distant_angular_diameter, 1.0f);
				if (created)
				{
					++m_bsp_worldlight_untracked_active;
					m_bsp_worldlight_active_sources.insert(candidate->source_index);
					m_source_distant_imported = true;
					m_source_distant_status += std::format(" | {}",
						remix_lights::get()->source_distant_runtime_status());
				}
				else
				{
					++m_bsp_worldlight_create_failed;
					m_source_distant_status += std::format(" | {}",
						remix_lights::get()->source_distant_runtime_status());
				}
				continue;
			}

			auto def = make_light_def_from_bsp_candidate(*candidate, "BSP worldlight: ");
			def.group = "bsp_worldlights";
			if (remix_lights::get()->add_single_map_setting_light_report(&def)) {
				++m_bsp_worldlight_untracked_active;
				m_bsp_worldlight_active_sources.insert(candidate->source_index);
			}
			else {
				++m_bsp_worldlight_create_failed;
			}
		}

		m_bsp_worldlight_stream_retained = 0u;
		for (const auto source_index : m_bsp_worldlight_active_sources) {
			if (previous_active_sources.contains(source_index)) ++m_bsp_worldlight_stream_retained;
		}
		m_bsp_worldlight_stream_evicted = 0u;
		for (const auto source_index : previous_active_sources) {
			if (!m_bsp_worldlight_active_sources.contains(source_index)) ++m_bsp_worldlight_stream_evicted;
		}

		update_owned_worldlights();
		m_bsp_worldlight_imported = m_bsp_worldlight_untracked_active + m_owned_worldlights_active;

		if (camera)
		{
			m_bsp_worldlight_last_import_origin = *camera;
			m_bsp_worldlight_have_import_origin = true;
		}
		m_bsp_worldlight_next_stream_check = now() + std::max(0.1f, m_bsp_worldlight_stream_interval);
		m_bsp_worldlight_status = std::format("imported {} WORLDLIGHTS (distant {}, failed {}, distance skipped {}, limit {}, stream refreshes {}, retained/evicted {}/{})",
			m_bsp_worldlight_imported, m_source_distant_imported ? "active" : (m_source_distant_detected ? "detected/not active" : "missing"),
			m_bsp_worldlight_create_failed, m_bsp_worldlight_skipped_distance,
			m_bsp_worldlight_import_limit, m_bsp_worldlight_stream_refreshes,
			m_bsp_worldlight_stream_retained, m_bsp_worldlight_stream_evicted);
		if (m_bsp_worldlight_imported > 0u)
		{
			m_bsp_worldlight_loaded_map = normalize_map_session_name(m_bsp_worldlight_pending_map);
			m_bsp_worldlight_auto_import_retries = 0u;
		}
		return m_bsp_worldlight_imported > 0u;
	}

	void dynamic_lighting::update_budget_window(const float curtime)
	{
		if (m_runtime_budget_window_start < -90000.0f || curtime - m_runtime_budget_window_start >= 1.0f)
		{
			m_runtime_budget_window_start = curtime;
			m_runtime_spawned_this_second = 0u;
			m_runtime_muzzle_this_second = 0u;
			m_runtime_hash_this_second = 0u;
		}

		m_runtime_active_lights_snapshot = remix_lights::get() ? static_cast<std::uint32_t>(remix_lights::get()->get_active_light_count()) : 0u;
	}

	bool dynamic_lighting::can_spawn_runtime_light(const map_settings::remix_light_settings_s& def, const bool delayed)
	{
		if (!m_runtime_budgets_enabled) {
			return true;
		}

		update_budget_window(now());
		const bool priority_muzzle = def.comment.find("auto muzzle flash") != std::string::npos;
		const std::uint32_t active_reserve = priority_muzzle ? 4u : 0u;
		const std::uint32_t spawn_reserve = priority_muzzle ? 8u : 0u;

		if (delayed && m_pending_lights.size() >= m_runtime_max_pending_lights)
		{
			++m_runtime_skipped_pending_limit;
			++m_runtime_skipped_budget;
			if (priority_muzzle) {
				++m_muzzle_skipped_budget;
				m_muzzle_last_reject_reason = "pending light limit";
			}
			return false;
		}

		if (remix_lights::get() && remix_lights::get()->get_active_light_count() >= m_runtime_max_active_lights + active_reserve)
		{
			++m_runtime_skipped_active_limit;
			++m_runtime_skipped_budget;
			if (priority_muzzle) {
				++m_muzzle_skipped_budget;
				m_muzzle_last_reject_reason = "active light limit";
			}
			return false;
		}

		if (m_runtime_spawned_this_second >= m_runtime_max_spawns_per_second + spawn_reserve)
		{
			++m_runtime_skipped_budget;
			if (priority_muzzle) {
				++m_muzzle_skipped_budget;
				m_muzzle_last_reject_reason = "global spawn budget";
			}
			return false;
		}

		++m_runtime_spawned_this_second;
		return true;
	}

	bool dynamic_lighting::can_process_sound_hash_runtime_event()
	{
		if (!m_runtime_budgets_enabled) {
			return true;
		}

		update_budget_window(now());
		if (m_runtime_hash_this_second >= m_runtime_max_sound_hash_per_second)
		{
			++m_runtime_skipped_hash_budget;
			return false;
		}

		++m_runtime_hash_this_second;
		return true;
	}

	bool dynamic_lighting::can_process_muzzle_runtime_event()
	{
		if (!m_runtime_budgets_enabled) {
			return true;
		}

		update_budget_window(now());
		if (m_runtime_muzzle_this_second >= m_runtime_max_muzzle_per_second)
		{
			++m_runtime_skipped_muzzle_budget;
			return false;
		}

		++m_runtime_muzzle_this_second;
		return true;
	}

	void dynamic_lighting::queue_or_spawn(map_settings::remix_light_settings_s&& def, const float delay)
	{
		if (!remix_api::is_initialized() || !remix_lights::get()) {
			if (def.comment.find("auto muzzle flash") != std::string::npos) {
				++m_muzzle_skipped_budget;
				m_muzzle_last_reject_reason = "Remix light backend not ready";
			}
			return;
		}

		if (delay > 0.0f)
		{
			if (!can_spawn_runtime_light(def, true)) {
				return;
			}

			m_pending_lights.emplace_back(pending_light_s{ std::move(def), delay });
			return;
		}

		if (!can_spawn_runtime_light(def, false)) {
			return;
		}

		remix_lights::get()->add_single_map_setting_light(&def);
	}


	float dynamic_lighting::clamp_workflow_radius(const float radius)
	{
		if (!m_small_radius_mode) {
			return radius;
		}

		return std::clamp(radius, 0.0f, std::max(0.01f, m_small_radius_max));
	}

	const map_settings::light_anchor_s* dynamic_lighting::find_light_anchor(const std::string& name)
	{
		if (name.empty()) {
			return nullptr;
		}

		const auto name_lower = utils::str_to_lower(name);
		for (const auto& anchor : map_settings::get_map_settings().light_anchors)
		{
			if (utils::str_to_lower(anchor.name) == name_lower) {
				return &anchor;
			}
		}

		return nullptr;
	}

	map_settings::dynamic_light_event_s dynamic_lighting::build_event_from_anchor(const map_settings::light_anchor_s& anchor, const map_settings::dynamic_light_event_s* trigger_ev)
	{
		map_settings::dynamic_light_event_s ev = trigger_ev ? *trigger_ev : map_settings::dynamic_light_event_s{};
		ev.name = ev.name.empty() ? std::format("anchor_{}", anchor.name) : ev.name;
		ev.preset = anchor.preset;
		ev.animation = anchor.animation;
		ev.position = anchor.position;
		ev.offset = anchor.offset;
		ev.activate_anchor.clear();
		ev.use_source_origin = false;
		ev.use_camera_when_no_source = false;
		ev.radiance = anchor.radiance;
		ev.scalar = anchor.scalar * std::max(0.0f, ev.scalar);
		ev.radius = anchor.radius;
		ev.volumetric_scale = anchor.volumetric_scale;
		ev.duration = trigger_ev ? std::max(0.01f, ev.duration) : anchor.duration;
		ev.speed = anchor.speed;
		ev.variation = anchor.variation;
		ev.loop = trigger_ev ? (ev.loop || anchor.loop) : anchor.loop;
		ev.loop_smoothing = trigger_ev ? (ev.loop_smoothing || anchor.loop_smoothing) : anchor.loop_smoothing;
		ev.use_shaping = anchor.use_shaping;
		ev.direction = anchor.direction;
		ev.degrees = anchor.degrees;
		ev.softness = anchor.softness;
		ev.exponent = anchor.exponent;
		ev.animation_axis = anchor.animation_axis;
		ev.animation_degrees = anchor.animation_degrees;
		ev.animation_phase = anchor.animation_phase;
		ev.comment = anchor.comment.empty() ? std::format("Light anchor: {}", anchor.name) : anchor.comment;
		return ev;
	}

	bool dynamic_lighting::spawn_anchor_preview(const map_settings::light_anchor_s& anchor)
	{
		auto ev = build_event_from_anchor(anchor, nullptr);
		ev.name = std::format("preview_anchor_{}", anchor.name);
		ev.cooldown = 0.0f;
		ev.once = false;
		ev.comment = std::format("Light anchor preview: {}", anchor.name);
		trigger_event(ev, nullptr, nullptr);
		return true;
	}

	static std::string anchor_vec3_toml(const char* key, const Vector& value)
	{
		return std::format("{} = [{:.3f}, {:.3f}, {:.3f}]", key, value.x, value.y, value.z);
	}

	std::string dynamic_lighting::build_light_anchor_toml(const map_settings::light_anchor_s& anchor)
	{
		std::string out;
		out += std::format("{{ name = \"{}\", preset = \"{}\", animation = \"{}\", ",
			toml_escape_inline_string_dyn(anchor.name), toml_escape_inline_string_dyn(anchor.preset), toml_escape_inline_string_dyn(anchor.animation));
		out += anchor_vec3_toml("position", anchor.position);
		out += std::format(", scalar = {:.3g}, radius = {:.3g}, duration = {:.3g}, speed = {:.3g}, variation = {:.3g}", anchor.scalar, anchor.radius, anchor.duration, anchor.speed, anchor.variation);
		if (anchor.offset.LengthSqr() > 0.0001f) {
			out += std::format(", {}", anchor_vec3_toml("offset", anchor.offset));
		}
		if (anchor.radiance.x != 1.0f || anchor.radiance.y != 0.85f || anchor.radiance.z != 0.55f) {
			out += std::format(", {}", anchor_vec3_toml("radiance", anchor.radiance));
		}
		if (anchor.volumetric_scale != 1.0f) {
			out += std::format(", volumetric_scale = {:.3g}", anchor.volumetric_scale);
		}
		if (anchor.loop) {
			out += ", loop = true";
		}
		if (anchor.loop_smoothing) {
			out += ", loop_smoothing = true";
		}
		if (anchor.use_shaping)
		{
			out += std::format(", {}, degrees = {:.3g}, softness = {:.3g}, exponent = {:.3g}",
				anchor_vec3_toml("direction", anchor.direction), anchor.degrees, anchor.softness, anchor.exponent);
		}
		if (anchor.animation_axis.x != 0.0f || anchor.animation_axis.y != 0.0f || anchor.animation_axis.z != 1.0f) {
			out += std::format(", {}", anchor_vec3_toml("animation_axis", anchor.animation_axis));
		}
		if (anchor.animation_degrees != 360.0f) {
			out += std::format(", animation_degrees = {:.3g}", anchor.animation_degrees);
		}
		if (anchor.animation_phase != 0.0f) {
			out += std::format(", animation_phase = {:.3g}", anchor.animation_phase);
		}
		if (!anchor.comment.empty()) {
			out += std::format(", comment = \"{}\"", toml_escape_inline_string_dyn(anchor.comment));
		}
		out += " }";
		return out;
	}

	std::string dynamic_lighting::build_anchor_event_toml(const std::string& anchor_name, const std::string& trigger_body, const float duration, const float cooldown, const bool loop)
	{
		return std::format("{{ name = \"ev_{}\", trigger = {{ {} }}, activate_anchor = \"{}\", duration = {:.3g}, cooldown = {:.3g}, use_source_origin = false, loop = {} }}",
			toml_escape_inline_string_dyn(anchor_name), trigger_body, toml_escape_inline_string_dyn(anchor_name), duration, cooldown, loop ? "true" : "false");
	}

	bool dynamic_lighting::append_anchor_export(const map_settings::light_anchor_s& anchor, const std::string& event_toml)
	{
		if (game::root_path.empty())
		{
			char path[MAX_PATH];
			GetModuleFileNameA(nullptr, path, MAX_PATH);
			game::root_path = path;
			utils::erase_substring(game::root_path, "left4dead2.exe");
		}

		const std::string out_path = game::root_path + COMPMOD_ASSET_DIR "logs\\mapsettings_workbench_export.toml";
		std::filesystem::create_directories(std::filesystem::path(out_path).parent_path());
		std::ofstream out(out_path, std::ios::out | std::ios::app);
		if (!out.is_open()) {
			return false;
		}

		out << "\n\n# -----------------------------------------------------------------------------\n";
		out << "# Light Anchor Workbench export: " << anchor.name << "\n";
		out << "# Map: " << map_settings::get_map_settings().mapname << "\n";
		out << "# Paste the first entry into [LIGHT_ANCHORS] for this map and the second into [LIGHT_EVENTS].\n";
		out << build_light_anchor_toml(anchor) << "\n";
		if (!event_toml.empty()) {
			out << event_toml << "\n";
		}
		return true;
	}

	map_settings::remix_light_settings_s dynamic_lighting::build_light_from_event(const map_settings::dynamic_light_event_s& ev, const Vector& origin, const Vector& forward)
	{
		const auto& preset = find_dyn_light_preset(ev.preset);
		const auto animation = utils::str_to_lower(ev.animation);
		const float vscale = deterministic_variation(ev.name + ev.preset + ev.animation, ev.variation);
		const float duration = std::max(0.01f, ev.duration / std::max(0.01f, ev.speed));

		const bool is_custom = utils::str_to_lower(ev.preset) == "custom";
		const Vector radiance = is_custom ? ev.radiance : preset.radiance;
		const float base_scalar = (is_custom ? 1.0f : preset.scalar) * ev.scalar * vscale;
		const float radius = clamp_workflow_radius((ev.radius != 48.0f ? ev.radius : preset.radius) * vscale);
		const float volumetric = (is_custom ? ev.volumetric_scale : preset.volumetric) * ev.volumetric_scale;
		const bool rotating_animation = animation_is_rotator(animation) || animation_is_sweep(animation);
		const bool shaped = ev.use_shaping || preset.shaped || rotating_animation;
		Vector direction = ev.direction;
		if (direction.LengthSqr() <= 0.0001f) {
			direction = forward;
		}
		direction.Normalize();

		const Vector anim_axis = axis_for_animation(animation, ev.animation_axis, direction);
		const float anim_degrees = std::clamp(ev.animation_degrees, 0.0f, 1440.0f);
		const float anim_phase = ev.animation_phase;

		const float degrees = ev.use_shaping ? ev.degrees : preset.degrees;
		const float softness = ev.use_shaping ? ev.softness : preset.softness;
		const float exponent = ev.use_shaping ? ev.exponent : preset.exponent;

		std::vector<map_settings::remix_light_settings_s::point_s> points;

		if (animation == "muzzle_flash")
		{
			const float fade = std::clamp(m_muzzle_flash_fade, 0.005f, duration);
			const float fade_start = std::max(0.0f, duration - fade);
			points.push_back(make_point(origin, radiance, base_scalar * 1.00f, radius, 0.0f, volumetric, shaped, direction, degrees, softness, exponent));
			if (fade_start > 0.001f) {
				points.push_back(make_point(origin, radiance, base_scalar * 0.92f, radius, fade_start, volumetric, shaped, direction, degrees, softness, exponent));
			}
			points.push_back(make_point(origin, radiance, base_scalar * 0.35f, radius * 0.88f, fade_start + fade * 0.45f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, 0.0f, radius * 0.55f, duration, volumetric, shaped, direction, degrees, softness, exponent));
		}
		else if (animation == "lightning_flash" || animation == "flash")
		{
			points.push_back(make_point(origin, radiance, 0.0f, radius, 0.0f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 1.00f, radius, duration * 0.08f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.12f, radius * 0.85f, duration * 0.45f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.85f, radius, duration * 0.62f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, 0.0f, radius * 0.75f, duration, volumetric, shaped, direction, degrees, softness, exponent));
		}
		else if (animation == "fire_pulse")
		{
			points.push_back(make_point(origin, radiance, base_scalar * 0.70f, radius * 0.92f, 0.0f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 1.10f, radius * 1.04f, duration * 0.28f, volumetric * 1.10f, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.82f, radius * 0.95f, duration * 0.62f, volumetric * 0.92f, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 1.00f, radius * 1.00f, duration, volumetric, shaped, direction, degrees, softness, exponent));
		}
		else if (animation == "alarm_pulse")
		{
			points.push_back(make_point(origin, radiance, 0.0f, radius, 0.0f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar, radius, duration * 0.18f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.16f, radius * 0.85f, duration * 0.52f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar, radius, duration * 0.70f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, 0.0f, radius * 0.80f, duration, volumetric, shaped, direction, degrees, softness, exponent));
		}
		else if (animation == "broken_fluorescent")
		{
			points.push_back(make_point(origin, radiance, base_scalar * 0.10f, radius, 0.0f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 1.00f, radius, duration * 0.08f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.08f, radius, duration * 0.15f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.75f, radius, duration * 0.42f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.18f, radius, duration * 0.58f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.60f, radius, duration, volumetric, shaped, direction, degrees, softness, exponent));
		}
		else if (animation == "tv_noise")
		{
			points.push_back(make_point(origin, radiance, base_scalar * 0.45f, radius, 0.0f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.82f, radius * 0.96f, duration * 0.22f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.25f, radius * 0.92f, duration * 0.48f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 1.00f, radius, duration * 0.72f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.52f, radius, duration, volumetric, shaped, direction, degrees, softness, exponent));
		}
		else if (animation == "generator_stutter")
		{
			points.push_back(make_point(origin, radiance, base_scalar * 0.38f, radius, 0.0f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 1.10f, radius, duration * 0.12f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.44f, radius, duration * 0.32f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.90f, radius, duration * 0.70f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.50f, radius, duration, volumetric, shaped, direction, degrees, softness, exponent));
		}
		else if (animation == "candle_flicker")
		{
			points.push_back(make_point(origin, radiance, base_scalar * 0.62f, radius * 0.88f, 0.0f, volumetric * 0.85f, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, Vector(1.0f, 0.46f, 0.12f), base_scalar * 1.05f, radius * 1.00f, duration * 0.18f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, Vector(1.0f, 0.31f, 0.06f), base_scalar * 0.72f, radius * 0.92f, duration * 0.40f, volumetric * 0.78f, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, Vector(1.0f, 0.58f, 0.18f), base_scalar * 1.18f, radius * 1.04f, duration * 0.68f, volumetric * 1.05f, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.66f, radius * 0.90f, duration, volumetric * 0.88f, shaped, direction, degrees, softness, exponent));
		}
		else if (animation == "unstable_bulb")
		{
			points.push_back(make_point(origin, radiance, base_scalar * 0.92f, radius, 0.0f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.28f, radius * 0.92f, duration * 0.18f, volumetric * 0.70f, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 1.22f, radius * 1.03f, duration * 0.30f, volumetric * 1.05f, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.48f, radius * 0.96f, duration * 0.54f, volumetric * 0.80f, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 1.08f, radius, duration * 0.82f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.88f, radius, duration, volumetric, shaped, direction, degrees, softness, exponent));
		}
		else if (animation == "fluorescent_random")
		{
			points.push_back(make_point(origin, radiance, base_scalar * 0.05f, radius, 0.0f, volumetric * 0.45f, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 1.25f, radius, duration * 0.06f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.02f, radius * 0.95f, duration * 0.11f, volumetric * 0.35f, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.70f, radius, duration * 0.28f, volumetric * 0.75f, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.10f, radius * 0.92f, duration * 0.46f, volumetric * 0.40f, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 1.05f, radius, duration * 0.73f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.36f, radius * 0.96f, duration, volumetric * 0.65f, shaped, direction, degrees, softness, exponent));
		}
		else if (animation == "soft_flicker")
		{
			points.push_back(make_point(origin, radiance, base_scalar * 0.78f, radius, 0.0f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 1.00f, radius * 1.02f, duration * 0.34f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.84f, radius * 0.98f, duration * 0.70f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.92f, radius, duration, volumetric, shaped, direction, degrees, softness, exponent));
		}
		else if (animation == "pulse_slow" || animation == "pulse_fast" || animation == "breathing")
		{
			const float low = animation == "breathing" ? 0.35f : 0.18f;
			const float high = animation == "pulse_fast" ? 1.25f : 1.05f;
			points.push_back(make_point(origin, radiance, base_scalar * low, radius * 0.95f, 0.0f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * high, radius * 1.04f, duration * 0.25f, volumetric * 1.05f, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * 0.55f, radius * 0.98f, duration * 0.55f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar * low, radius * 0.95f, duration, volumetric, shaped, direction, degrees, softness, exponent));
		}
		else if (animation == "strobe_fast" || animation == "strobe_slow")
		{
			const int flashes = animation == "strobe_fast" ? 4 : 2;
			for (int i = 0; i < flashes; ++i)
			{
				const float base_t = duration * (static_cast<float>(i) / static_cast<float>(flashes));
				points.push_back(make_point(origin, radiance, 0.0f, radius, base_t, volumetric, shaped, direction, degrees, softness, exponent));
				points.push_back(make_point(origin, radiance, base_scalar * 1.25f, radius, std::min(duration, base_t + duration * 0.035f), volumetric, shaped, direction, degrees, softness, exponent));
				points.push_back(make_point(origin, radiance, 0.0f, radius * 0.85f, std::min(duration, base_t + duration * 0.085f), volumetric, shaped, direction, degrees, softness, exponent));
			}
			points.push_back(make_point(origin, radiance, 0.0f, radius * 0.85f, duration, volumetric, shaped, direction, degrees, softness, exponent));
		}
		else if (animation_is_rotator(animation))
		{
			const int steps = animation == "police_red_blue" ? 8 : 9;
			const float sweep = animation == "lighthouse_sweep" ? 360.0f : anim_degrees;
			for (int i = 0; i < steps; ++i)
			{
				const float u = static_cast<float>(i) / static_cast<float>(steps - 1);
				const float angle = anim_phase + sweep * u;
				const Vector dir = rotate_vector_axis(direction, anim_axis, angle);
				float amp = 1.0f;
				Vector frame_radiance = radiance;
				if (animation == "warning_beacon" || animation == "rotating_beacon") {
					amp = 0.25f + 0.85f * std::max(0.0f, std::sin(u * static_cast<float>(M_PI) * 2.0f));
				}
				else if (animation == "police_red_blue") {
					frame_radiance = (i % 2 == 0) ? Vector(1.0f, 0.02f, 0.015f) : Vector(0.08f, 0.22f, 1.0f);
					amp = (i % 2 == 0) ? 1.05f : 0.95f;
				}
				else if (animation == "lighthouse_sweep") {
					amp = 0.70f + 0.30f * std::sin(u * static_cast<float>(M_PI));
				}
				else if (animation == "disc_spin" || animation == "spot_axis_spin") {
					amp = 0.88f + 0.12f * std::sin((u + 0.15f) * static_cast<float>(M_PI) * 2.0f);
				}
				points.push_back(make_point(origin, frame_radiance, base_scalar * amp, radius, duration * u, volumetric, true, dir, degrees == 180.0f ? 38.0f : degrees, softness, exponent));
			}
		}
		else if (animation_is_sweep(animation))
		{
			const float sweep = std::clamp(anim_degrees, 1.0f, 180.0f);
			const float key_angles[] = { -0.5f, -0.22f, 0.18f, 0.5f, 0.16f, -0.18f, -0.5f };
			constexpr int key_count = static_cast<int>(sizeof(key_angles) / sizeof(key_angles[0]));
			for (int i = 0; i < key_count; ++i)
			{
				const float u = static_cast<float>(i) / static_cast<float>(key_count - 1);
				const Vector dir = rotate_vector_axis(direction, anim_axis, anim_phase + key_angles[i] * sweep);
				const float amp = animation == "pendulum_sweep" ? (0.65f + 0.35f * std::sin(u * static_cast<float>(M_PI))) : 1.0f;
				points.push_back(make_point(origin, radiance, base_scalar * amp, radius, duration * u, volumetric, true, dir, degrees == 180.0f ? 42.0f : degrees, softness, exponent));
			}
		}
		else
		{
			points.push_back(make_point(origin, radiance, base_scalar, radius, 0.0f, volumetric, shaped, direction, degrees, softness, exponent));
			points.push_back(make_point(origin, radiance, base_scalar, radius, duration, volumetric, shaped, direction, degrees, softness, exponent));
		}

		return map_settings::remix_light_settings_s{
			.points = std::move(points),
			.run_once = !ev.loop,
			.loop = ev.loop,
			.loop_smoothing = ev.loop_smoothing,
			.trigger_always = false,
			.comment = ev.comment.empty() ? ev.name : ev.comment
		};
	}

	void dynamic_lighting::trigger_event(map_settings::dynamic_light_event_s& ev, const Vector* source_origin, const Vector* source_forward)
	{
		const bool muzzle_runtime_event = ev.preset == "muzzle_flash" || ev.name.starts_with("auto_muzzle_flash_");
		if (!ev.enabled || (imgui::get() && imgui::get()->m_light_edit_mode && !muzzle_runtime_event)) {
			return;
		}

		const float curtime = now();
		if (ev.cooldown > 0.0f && curtime - ev.last_trigger_time < ev.cooldown) {
			return;
		}

		if (ev.once && ev.was_used) {
			return;
		}

		// Looping dynamic lights are long-lived. Do not spawn duplicates from repeated sound/leaf checks.
		if (ev.loop && ev.was_used) {
			return;
		}

		if (!ev.activate_anchor.empty())
		{
			const auto* anchor = find_light_anchor(ev.activate_anchor);
			if (!anchor)
			{
				game::console();
				std::cout << "[DynamicLighting] Missing light anchor: " << ev.activate_anchor << std::endl;
				return;
			}

			auto anchor_ev = build_event_from_anchor(*anchor, &ev);
			trigger_event(anchor_ev, nullptr, source_forward);

			ev.last_trigger_time = curtime;
			ev.was_used = true;
			return;
		}

		Vector origin = ev.position;
		if (ev.use_source_origin && source_origin) {
			origin = *source_origin;
		}
		else if (ev.use_camera_when_no_source) {
			origin = *game::get_current_view_origin();
		}

		origin += ev.offset;

		Vector forward = source_forward ? *source_forward : *game::get_current_view_forward();
		if (forward.LengthSqr() <= 0.0001f) {
			forward = Vector(0.0f, 1.0f, 0.0f);
		}
		forward.Normalize();

		auto def = build_light_from_event(ev, origin, forward);
		queue_or_spawn(std::move(def), ev.delay);

		ev.last_trigger_time = curtime;
		ev.was_used = true;
		m_debug_last_trigger_time = curtime;
		m_debug_last_trigger_name = ev.name;
		m_debug_last_trigger_origin = origin;
	}


	bool dynamic_lighting::is_zero_origin(const Vector& origin)
	{
		return std::fabs(origin.x) < 0.01f && std::fabs(origin.y) < 0.01f && std::fabs(origin.z) < 0.01f;
	}

	bool dynamic_lighting::try_spawn_from_sound_hash_library(const std::uint32_t hash, const std::string& sound_name, const Vector& origin)
	{
		if (!m_sound_hash_library_enabled || hash == 0u) {
			return false;
		}

		const sound_hash_trigger_s* match = nullptr;
		for (const auto& entry : SOUND_HASH_TRIGGERS)
		{
			if (entry.hash == hash)
			{
				++m_sound_hash_library_matches;
				if (!sound_hash_category_enabled(entry.category))
				{
					++m_sound_hash_library_skipped;
					return false;
				}

				match = &entry;
				break;
			}
		}

		if (!match) {
			return false;
		}

		const bool zero_origin = is_zero_origin(origin);
		if (zero_origin && match->require_valid_origin)
		{
			++m_sound_hash_library_skipped;
			m_sound_hash_last_category = std::format("{} skipped zero-origin", sound_hash_category_name(match->category));
			m_sound_hash_last_sound = sound_name.empty() ? match->source_name : sound_name;
			m_sound_hash_last_hash = hash;
			m_sound_hash_last_origin = origin;
			return false;
		}

		if (!zero_origin && m_sound_hash_reject_near_player_origin &&
			sound_hash_requires_world_origin(match->category) &&
			sound_origin_is_near_player_view(origin))
		{
			++m_sound_hash_library_skipped;
			++m_sound_hash_library_skipped_player_origin;
			m_sound_hash_last_category = std::format("{} skipped near-player origin", sound_hash_category_name(match->category));
			m_sound_hash_last_sound = sound_name.empty() ? match->source_name : sound_name;
			m_sound_hash_last_hash = hash;
			m_sound_hash_last_origin = origin;
			return false;
		}

		const float curtime = now();
		const auto cooldown_it = m_sound_hash_last_trigger_times.find(hash);
		if (cooldown_it != m_sound_hash_last_trigger_times.end() && curtime - cooldown_it->second < match->cooldown)
		{
			++m_sound_hash_library_skipped;
			return false;
		}

		Vector source_origin = origin;
		bool use_source = !zero_origin;
		bool allow_camera_fallback = false;
		if (zero_origin)
		{
			if (match->use_camera_when_zero_origin && m_sound_hash_use_camera_for_zero_origin)
			{
				source_origin = *game::get_current_view_origin();
				allow_camera_fallback = true;
			}
			else
			{
				++m_sound_hash_library_skipped;
				m_sound_hash_last_category = std::format("{} skipped zero-origin", sound_hash_category_name(match->category));
				m_sound_hash_last_sound = sound_name.empty() ? match->source_name : sound_name;
				m_sound_hash_last_hash = hash;
				m_sound_hash_last_origin = origin;
				return false;
			}
		}

		if (!can_process_sound_hash_runtime_event())
		{
			++m_sound_hash_library_skipped;
			m_sound_hash_last_category = std::format("{} skipped hash budget", sound_hash_category_name(match->category));
			m_sound_hash_last_sound = sound_name.empty() ? match->source_name : sound_name;
			m_sound_hash_last_hash = hash;
			m_sound_hash_last_origin = origin;
			return false;
		}

		map_settings::dynamic_light_event_s ev = {};
		ev.name = std::format("hash_{}_0x{:08x}", sound_hash_category_name(match->category), hash);
		ev.preset = match->preset;
		ev.animation = match->animation;
		ev.duration = match->duration;
		ev.cooldown = match->cooldown;
		ev.scalar = match->scalar;
		ev.radius = safe_sound_hash_radius(match->radius);
		ev.use_source_origin = use_source;
		ev.use_camera_when_no_source = allow_camera_fallback;
		ev.comment = std::format("sound hash library: {} / {}", sound_hash_category_name(match->category), match->source_name);

		trigger_event(ev, use_source ? &source_origin : nullptr, nullptr);
		m_sound_hash_last_trigger_times[hash] = curtime;
		++m_sound_hash_library_spawned;
		m_sound_hash_last_category = sound_hash_category_name(match->category);
		m_sound_hash_last_sound = sound_name.empty() ? match->source_name : sound_name;
		m_sound_hash_last_hash = hash;
		m_sound_hash_last_origin = source_origin;
		return true;
	}

	void dynamic_lighting::on_sound_start(const std::uint32_t hash, const std::string& sound_name, const Vector& origin)
	{
		auto& ms = map_settings::get_map_settings();

		if (ms.using_any_dynamic_light_sound_hash || ms.using_any_dynamic_light_sound_name)
		{
			const auto sound_name_lower = utils::str_to_lower(sound_name);

			for (auto& ev : ms.dynamic_light_events)
			{
				if (!ev.enabled || ev.trigger_type != map_settings::DYN_LIGHT_TRIGGER_SOUND) {
					continue;
				}

				const bool hash_match = ev.sound_hash && ev.sound_hash == hash;
				const bool name_match = !ev.sound_name.empty() && sound_name_lower.find(utils::str_to_lower(ev.sound_name)) != std::string::npos;
				if (hash_match || name_match) {
					trigger_event(ev, &origin, nullptr);
				}
			}
		}

		try_spawn_from_sound_hash_library(hash, sound_name, origin);

		if (m_auto_muzzle_flash && is_likely_muzzle_sound(sound_name))
		{
			const auto& profile = m_muzzle_weapon_profiles_enabled ? get_selected_muzzle_profile(sound_name) : MUZZLE_WEAPON_PROFILES[0];
			const float cooldown_scale = m_muzzle_flash_cooldown / 0.018f;
			const float effective_cooldown = std::max(0.0f, profile.cooldown * cooldown_scale);

			const float curtime = now();
			if (curtime - m_last_auto_muzzle_time < effective_cooldown)
			{
				// Duplicate layers for one shot must not consume the per-second muzzle budget.
				++m_muzzle_skipped_cooldown;
				m_muzzle_last_reject_reason = "cooldown duplicate";
				m_muzzle_last_sound = sound_name.empty() ? "<empty cooldown>" : sound_name;
				return;
			}

			if (!can_process_muzzle_runtime_event())
			{
				++m_muzzle_skipped_budget;
				m_muzzle_last_reject_reason = "runtime muzzle budget";
				m_muzzle_last_sound = sound_name.empty() ? "<empty budget>" : sound_name;
				return;
			}

			{
				const Vector old_offset = m_muzzle_flash_offset;
				if (m_muzzle_weapon_profiles_enabled) {
					m_muzzle_flash_offset = profile.offset;
				}

				Vector muzzle_origin = Vector(0.0f, 0.0f, 0.0f);
				Vector muzzle_forward = Vector(0.0f, 1.0f, 0.0f);
				get_auto_muzzle_source(origin, muzzle_origin, muzzle_forward);

				if (m_muzzle_weapon_profiles_enabled) {
					m_muzzle_flash_offset = old_offset;
				}

				map_settings::dynamic_light_event_s ev = {};
				ev.name = std::format("auto_muzzle_flash_{}", profile.name);
				ev.preset = "muzzle_flash";
				ev.animation = "muzzle_flash";
				ev.duration = std::max(0.025f, profile.duration * (m_muzzle_flash_duration / 0.075f));
				ev.delay = std::max(0.0f, m_muzzle_flash_delay);
				ev.cooldown = effective_cooldown;
				ev.radiance = m_muzzle_flash_color;
				ev.scalar = m_muzzle_flash_scalar * (m_muzzle_weapon_profiles_enabled ? profile.scalar : 1.0f);
				ev.radius = std::max(0.001f, profile.radius * m_muzzle_flash_radius);
				ev.use_source_origin = true;
				ev.use_camera_when_no_source = true;
				if (m_muzzle_flash_shape_mode == 1)
				{
					ev.use_shaping = true;
					ev.degrees = 62.0f;
					ev.softness = 0.22f;
					ev.exponent = 0.65f;
				}
				else
				{
					// Sphere is the default for weapon muzzle flash. Shaped/spot lights can look like
					// a disc that points up when Remix/source coordinate bases disagree.
					ev.use_shaping = false;
					ev.degrees = 180.0f;
					ev.softness = 0.0f;
					ev.exponent = 0.0f;
				}
				ev.comment = m_muzzle_flash_shape_mode == 1 ? "auto muzzle flash cone v2" : "auto muzzle flash sphere v2";
				trigger_event(ev, &muzzle_origin, &muzzle_forward);
				++m_muzzle_spawn_attempts;
				m_last_auto_muzzle_time = curtime;

				m_muzzle_last_profile = profile.name;
				m_muzzle_last_sound = sound_name.empty() ? "<empty>" : sound_name;
				m_muzzle_last_origin = muzzle_origin;
				m_muzzle_last_forward = muzzle_forward;
			}
		}
	}

	void dynamic_lighting::on_choreo_start(const std::string_view& name, const std::string_view& actor, const std::string_view& event, const std::string_view& param1)
	{
		auto& ms = map_settings::get_map_settings();
		if (!ms.using_any_dynamic_light_choreo) {
			return;
		}

		for (auto& ev : ms.dynamic_light_events)
		{
			if (!ev.enabled || ev.trigger_type != map_settings::DYN_LIGHT_TRIGGER_CHOREO) {
				continue;
			}

			if (!name.contains(ev.choreo_name)) {
				continue;
			}

			if (!ev.choreo_actor.empty() && !actor.contains(ev.choreo_actor)) {
				continue;
			}

			if (!ev.choreo_event.empty() && !event.contains(ev.choreo_event)) {
				continue;
			}

			if (!ev.choreo_param1.empty() && !param1.contains(ev.choreo_param1)) {
				continue;
			}

			trigger_event(ev, nullptr, nullptr);
		}
	}

	void dynamic_lighting::on_client_frame()
	{
		static float next_d3d_hook_attempt = 0.0f;
		static float next_source_alloc_hook_attempt = 0.0f;
		static float next_server_accept_input_hook_attempt = 0.0f;
		const float curtime = now();
		if (m_bsp_worldlight_last_client_time >= 0.0f && curtime + 0.50f < m_bsp_worldlight_last_client_time)
		{
			// Source curtime restarts with a new level. Keep this probe armed until the
			// map name becomes available, including reloads of the same map name.
			m_bsp_worldlight_session_probe_pending = true;
		}
		m_bsp_worldlight_last_client_time = curtime;
		update_budget_window(curtime);

		if ((m_d3d_light_capture_enabled || m_d3d_light_spawn_enabled) && !m_d3d_hooks_installed && curtime >= next_d3d_hook_attempt)
		{
			install_d3d_light_hooks();
			next_d3d_hook_attempt = curtime + 1.0f;
		}

		if (m_source_alloc_hooks_enabled && m_source_dlight_import && !m_source_alloc_hooks_installed &&
			curtime >= next_source_alloc_hook_attempt)
		{
			install_source_light_allocation_hooks();
			next_source_alloc_hook_attempt = curtime + 1.0f;
		}

		if (m_server_accept_input_capture_enabled && g_map_entity_graph_loaded &&
			!m_server_accept_input_hook_scan_complete && game::server_tools_available() &&
			curtime >= next_server_accept_input_hook_attempt)
		{
			install_server_accept_input_hooks();
			// Bounded incremental scan: at most four prototype entities per slice.
			next_server_accept_input_hook_attempt = curtime + 0.15f;
		}

		const float dt = frame_time();
		if (dt <= 0.0f) {
			return;
		}

		if (remix_api::is_initialized() && remix_lights::get()) {
			update_source_runtime_lights(curtime);
		}

		// V20.9 watches the complete Light Rig model rather than individual ImGui
		// widgets. Any add, delete or parameter edit changes the fingerprint and is
		// atomically persisted after a short debounce.
		if (m_persistent_map_light_database && imgui::get() && imgui::get()->m_light_edit_mode)
		{
			static float next_light_editor_probe = 0.0f;
			if (curtime >= next_light_editor_probe)
			{
				next_light_editor_probe = curtime + 0.35f;
				auto& editor_lights = map_settings::get_map_settings().remix_lights;
				for (std::size_t i = 0u; i < editor_lights.size(); ++i)
				{
					if (editor_lights[i].persistent_map_light_id.empty())
						ensure_persistent_map_light_id(editor_lights[i], i, false);
				}
				const auto fingerprint = persistent_light_editor_fingerprint(editor_lights);
				if (!m_persistent_map_light_fingerprint_valid)
				{
					m_persistent_map_light_editor_fingerprint = fingerprint;
					m_persistent_map_light_fingerprint_valid = true;
				}
				else if (fingerprint != m_persistent_map_light_editor_fingerprint)
				{
					m_persistent_map_light_editor_fingerprint = fingerprint;
					notify_light_editor_changed();
				}
			}

			if (m_persistent_map_light_auto_save && m_persistent_map_light_dirty &&
				curtime >= m_persistent_map_light_save_at)
			{
				if (save_light_editor_map_lights_to_config()) ++m_persistent_map_light_autosaves;
			}
		}

		if (m_map_light_auto_save_config && m_map_light_config_dirty &&
			curtime >= m_map_light_config_save_at &&
			(!m_source_bsp_candidates.empty() || !m_runtime_projected_lights.empty()))
		{
			save_all_map_lights_to_config();
		}

		// Direct scene-editor import is independent from automatic config writing. This
		// makes detected map lights appear in the Light Editor even when the user keeps
		// auto-save disabled while authoring. One sync is performed per map generation.
		if (m_map_light_auto_sync_editor &&
			m_map_light_editor_synced_generation != m_bsp_worldlight_session_generation &&
			curtime >= m_map_light_config_save_at &&
			(!m_source_bsp_candidates.empty() || !m_runtime_projected_lights.empty()))
		{
			sync_imported_map_lights_to_light_editor();
		}

		if (curtime >= m_bsp_worldlight_watchdog_next_check)
		{
			m_bsp_worldlight_watchdog_next_check = curtime + 1.0f;
			const std::string current_map = normalize_map_session_name(map_settings::get_map_name());
			if (!current_map.empty() && (m_bsp_worldlight_session_probe_pending ||
				current_map != normalize_map_session_name(m_bsp_worldlight_pending_map)))
			{
				// Self-heal missed/duplicated callbacks and same-map reloads detected from
				// Source curtime restarting. Do not depend only on the map name changing.
				m_bsp_worldlight_session_probe_pending = false;
				on_map_load(current_map.c_str());
				m_bsp_worldlight_last_client_time = curtime;
			}
		}

		if (m_bsp_worldlight_auto_import_pending)
		{
			if (!m_bsp_worldlight_auto_import)
			{
				m_bsp_worldlight_auto_import_pending = false;
				m_bsp_worldlight_status = "automatic WORLDLIGHT import disabled";
			}
			else if (remix_api::is_initialized() ||
				(remix_lights::get() && remix_lights::source_directional_ff_enabled()))
			{
				m_bsp_worldlight_auto_import_delay -= dt;
				if (m_bsp_worldlight_auto_import_delay <= 0.0f)
				{
					if (scan_current_bsp_world_lights() && import_scanned_bsp_world_lights())
					{
						m_bsp_worldlight_auto_import_pending = false;
					}
					else
					{
						++m_bsp_worldlight_auto_import_retries;
						const float retry_delay = std::min(5.0f, 0.50f + static_cast<float>(m_bsp_worldlight_auto_import_retries) * 0.20f);
						m_bsp_worldlight_auto_import_delay = retry_delay;
						m_bsp_worldlight_status = std::format("map lights not ready; retry {} in {:.1f}s: {}",
							m_bsp_worldlight_auto_import_retries, retry_delay, m_bsp_worldlight_status);
					}
				}
			}
		}

		if (m_bsp_worldlight_stream_nearby && m_bsp_worldlight_near_camera_only &&
			!m_bsp_worldlight_auto_import_pending && m_bsp_worldlight_imported > 0u &&
			!m_source_bsp_candidates.empty() && curtime >= m_bsp_worldlight_next_stream_check)
		{
			m_bsp_worldlight_next_stream_check = curtime + std::max(0.1f, m_bsp_worldlight_stream_interval);
			const auto* camera = game::get_current_view_origin();
			const float threshold = std::max(1.0f, m_bsp_worldlight_stream_move_threshold);
			if (camera && (!m_bsp_worldlight_have_import_origin ||
				camera->DistToSqr(m_bsp_worldlight_last_import_origin) >= threshold * threshold))
			{
				++m_bsp_worldlight_stream_refreshes;
				import_scanned_bsp_world_lights();
			}
		}

		for (auto it = m_pending_lights.begin(); it != m_pending_lights.end();)
		{
			it->delay -= dt;
			if (it->delay <= 0.0f)
			{
				if (can_spawn_runtime_light(it->def, false)) {
					remix_lights::get()->add_single_map_setting_light(&it->def);
				}
				it = m_pending_lights.erase(it);
			}
			else {
				++it;
			}
		}

		auto& ms = map_settings::get_map_settings();
		const float leaf_hz = std::max(0.0f, m_runtime_leaf_check_hz);
		if (ms.using_any_dynamic_light_leaf && (leaf_hz <= 0.0f || curtime >= m_next_leaf_check_time))
		{
			m_next_leaf_check_time = leaf_hz > 0.0f ? curtime + (1.0f / leaf_hz) : curtime;
			for (auto& ev : ms.dynamic_light_events)
			{
				if (!ev.enabled || ev.trigger_type != map_settings::DYN_LIGHT_TRIGGER_LEAF || ev.leafs.empty()) {
					continue;
				}

				if (g_current_leaf >= 0 && ev.leafs.contains(static_cast<std::uint32_t>(g_current_leaf))) {
					trigger_event(ev, nullptr, nullptr);
				}
			}
		}

		if (m_authoring_debug_tools && m_draw_debug && !m_debug_last_trigger_name.empty())
		{
			game::debug_add_text_overlay(&m_debug_last_trigger_origin.x, utils::va("DynamicLight: %s", m_debug_last_trigger_name.c_str()), -1, 1.0f, 0.75f, 0.2f, 1.0f);
		}

		if (m_authoring_debug_tools && m_muzzle_draw_debug && m_muzzle_last_profile != "none")
		{
			Vector forward_probe = m_muzzle_last_origin + (m_muzzle_last_forward * 32.0f);
			game::debug_add_text_overlay(&m_muzzle_last_origin.x, utils::va("Muzzle: %s", m_muzzle_last_profile.c_str()), -1, 1.0f, 0.52f, 0.1f, 1.0f);
			game::debug_add_text_overlay(&forward_probe.x, "muzzle forward", -1, 1.0f, 0.20f, 0.05f, 1.0f);
		}

		if (m_authoring_debug_tools && m_sound_hash_draw_debug && m_sound_hash_last_category != "none")
		{
			game::debug_add_text_overlay(&m_sound_hash_last_origin.x, utils::va("SoundHash: %s 0x%08x", m_sound_hash_last_category.c_str(), m_sound_hash_last_hash), -1, 0.42f, 0.80f, 1.0f, 1.0f);
		}
	}

	void dynamic_lighting::spawn_test_event(const char* preset, const char* animation)
	{
		map_settings::dynamic_light_event_s ev = {};
		ev.name = utils::va("test_%s_%s", preset ? preset : "soft_flash", animation ? animation : "stable");
		ev.preset = preset ? preset : "soft_flash";
		ev.animation = animation ? animation : "stable";
		ev.duration = 0.35f;
		ev.scalar = 1.0f;
		ev.radius = 48.0f;
		ev.use_source_origin = false;
		ev.use_camera_when_no_source = true;
		ev.offset = *game::get_current_view_forward() * 72.0f;
		ev.comment = "workbench test dynamic light";
		trigger_event(ev, nullptr, game::get_current_view_forward());
	}

	void dynamic_lighting::on_d3d_set_light(const DWORD index, const D3DLIGHT9* light)
	{
		if (!m_d3d_light_capture_enabled || !light) {
			return;
		}

		++m_d3d_setlight_calls;
		m_d3d_hook_status = "capturing SetLight calls";
		m_d3d_last_index = index;

		auto& cached = m_d3d_lights[index];
		cached.light = *light;
		m_d3d_last_origin = Vector(light->Position.x, light->Position.y, light->Position.z);

		if (cached.enabled) {
			try_spawn_from_d3d_light(index, cached.light);
		}
	}

	void dynamic_lighting::on_d3d_light_enable(const DWORD index, const BOOL enable)
	{
		if (!m_d3d_light_capture_enabled) {
			return;
		}

		++m_d3d_lightenable_calls;
		m_d3d_hook_status = "capturing LightEnable calls";
		m_d3d_last_index = index;

		auto& cached = m_d3d_lights[index];
		cached.enabled = enable != FALSE;
		if (cached.enabled) {
			try_spawn_from_d3d_light(index, cached.light);
		}
	}

	void dynamic_lighting::try_spawn_from_d3d_light(const DWORD index, const D3DLIGHT9& light)
	{
		if (!m_d3d_light_spawn_enabled || !remix_api::is_initialized()) {
			return;
		}

		if (light.Type != D3DLIGHT_POINT && light.Type != D3DLIGHT_SPOT) {
			return;
		}

		auto& cached = m_d3d_lights[index];
		const float curtime = now();
		if (curtime - cached.last_spawn_time < std::max(0.0f, m_d3d_light_cooldown)) {
			return;
		}

		Vector origin(light.Position.x, light.Position.y, light.Position.z);
		Vector direction(light.Direction.x, light.Direction.y, light.Direction.z);
		if (direction.LengthSqr() <= 0.0001f) {
			direction = *game::get_current_view_forward();
		}
		if (direction.LengthSqr() <= 0.0001f) {
			direction = Vector(0.0f, 1.0f, 0.0f);
		}
		direction.Normalize();

		map_settings::dynamic_light_event_s ev = {};
		ev.name = std::format("d3d_setlight_{}", index);
		ev.preset = "custom";
		ev.animation = "muzzle_flash";
		ev.duration = std::max(0.01f, m_d3d_light_duration);
		ev.cooldown = m_d3d_light_cooldown;
		ev.scalar = std::max(0.0f, m_d3d_light_scalar);
		ev.radius = std::max(8.0f, light.Range * std::max(0.01f, m_d3d_light_radius_scale));
		ev.radiance = Vector(std::max(0.0f, light.Diffuse.r), std::max(0.0f, light.Diffuse.g), std::max(0.0f, light.Diffuse.b));
		ev.use_source_origin = true;
		ev.use_camera_when_no_source = false;
		ev.use_shaping = light.Type == D3DLIGHT_SPOT;
		ev.direction = direction;
		ev.degrees = light.Type == D3DLIGHT_SPOT ? std::clamp(RAD2DEGF(light.Phi), 1.0f, 179.0f) : 180.0f;
		ev.softness = light.Type == D3DLIGHT_SPOT ? 0.20f : 0.0f;
		ev.exponent = light.Type == D3DLIGHT_SPOT ? std::clamp(light.Falloff, 0.01f, 10.0f) : 0.0f;
		ev.comment = "experimental D3D SetLight mirror";

		trigger_event(ev, &origin, &direction);
		cached.last_spawn_time = curtime;
		m_d3d_last_origin = origin;
		++m_d3d_spawned_lights;
	}

	bool dynamic_lighting::is_likely_muzzle_sound(const std::string& sound_name)
	{
		const auto classification = muzzle_sound_classifier::classify(
			sound_name, m_muzzle_strict_fire_tokens, m_muzzle_reject_weapon_handling,
			m_muzzle_allow_weapon_family_fallback);

		if (!classification.candidate) {
			return false;
		}

		++m_muzzle_candidate_sounds;
		m_muzzle_last_sound = sound_name.empty() ? "<empty weapon sound>" : sound_name;

		if (classification.accepted)
		{
			++m_muzzle_accepted_sounds;
			m_muzzle_last_accept_reason = classification.reason;
			m_muzzle_last_reject_reason = "none";
			return true;
		}

		++m_muzzle_rejected_non_fire;
		if (classification.hard_reject) ++m_muzzle_rejected_hard;
		if (classification.strict_reject) ++m_muzzle_rejected_strict;
		m_muzzle_last_reject_reason = classification.reason;
		return false;
	}

}
