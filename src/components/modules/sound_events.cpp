#include "std_include.hpp"
#include "components/common/imgui/imgui_helper.hpp"

namespace components
{
	namespace cmd
	{
		bool sound_debug_printing = false;
	}

	const std::deque<sound_events::history_entry>& sound_events::get_history()
	{
		return m_history;
	}

	void sound_events::clear_history()
	{
		m_history.clear();
	}

	void sound_events::push_history(std::uint32_t hash, const std::string& sound_name, const StartSoundParams_t* parms)
	{
		if (!parms) {
			return;
		}

		history_entry entry = {};
		entry.hash = hash;
		entry.name = sound_name;
		entry.origin = parms->origin;
		entry.delay = parms->delay;
		entry.volume = parms->fvol;
		entry.time = interfaces::get()->m_globals ? interfaces::get()->m_globals->curtime : 0.0f;

		m_history.push_front(entry);
		while (m_history.size() > 64u) {
			m_history.pop_back();
		}
	}

	// each of these stands for something .. that we don't care about
	char* skip_sound_chars(const char* pch)
	{
		auto str = (char*)pch;
		while (true)
		{
			if (*str != '*' && *str != '?' && *str != '!' && *str != '#' && *str != '@' && *str != '(' && 
				*str != '>' && *str != '<' && *str != '^' && *str != ')' && *str != '}' && *str != '$') 
			{
				break;
			} str++;
		}
		return str;
	}
	
	void on_start_sound_hk(const StartSoundParams_t* parms)
	{
		if (!loader::is_runtime_ready()) {
			return;
		}

		++sound_events::m_hook_event_count;
		if (interfaces::get() && interfaces::get()->m_globals) {
			sound_events::m_last_hook_event_time = interfaces::get()->m_globals->realtime;
		}
		if (parms && parms->pSfx) 
		{
			char buff[264];
	
			if (const char* sound_name = skip_sound_chars(parms->pSfx->vftable->getname(parms->pSfx, buff, 260u)); 
				sound_name)
			{
				// check if we need to hash sounds
				const auto& ms = map_settings::get_map_settings();

				const bool lights_use_hash = ms.using_any_light_sound_hash;
				const bool transition_use_hash = ms.using_any_transition_sound_hash;
				const bool transition_use_name = ms.using_any_transition_sound_name;
				const bool markers_use_sound_hash = ms.using_any_marker_sound_hash;
				const bool markers_use_sound_name = ms.using_any_marker_sound_name;

				std::string snd_name_forward = sound_name;
				utils::replace_all(snd_name_forward, "\\", "/");

				uint32_t hash = 0u;
				hash = utils::hash32_combine(hash, sound_name);
				hash = utils::hash32_combine(hash, parms->delay);
				hash = utils::hash32_combine(hash, parms->fvol);
				hash = utils::hash32_combine(hash, parms->origin.x);
				hash = utils::hash32_combine(hash, parms->origin.y);
				hash = utils::hash32_combine(hash, parms->origin.z);

				sound_events::push_history(hash, snd_name_forward, parms);

				if (cmd::sound_debug_printing)
				{
					game::print_ingame("[sound_hk] HASH: ( 0x%x ) -- %s -- delay: %.2f -- vol: %.2f -- origin: [%.5f %.5f %.5f] @ time: %.2f\n",
						hash, !snd_name_forward.empty() ? snd_name_forward.c_str() : "NULL", parms->delay, parms->fvol,
						parms->origin.x, parms->origin.y, parms->origin.z, interfaces::get()->m_globals->curtime);
				}

				if (lights_use_hash) {
					remix_lights::on_sound_start(hash);
				}

				if (ms.using_any_dynamic_light_sound_hash || ms.using_any_dynamic_light_sound_name || dynamic_lighting::auto_muzzle_flash_enabled() || dynamic_lighting::m_sound_hash_library_enabled) {
					dynamic_lighting::on_sound_start(hash, snd_name_forward, parms->origin);
				}

				if (transition_use_hash || transition_use_name) {
					remix_vars::on_sound_start(hash, snd_name_forward);
				}

				if (markers_use_sound_hash || markers_use_sound_name) {
					remix_markers::on_sound_start(hash, snd_name_forward);
				}
			}
		}
	}

	HOOK_RETN_PLACE_DEF(on_start_sound_stub_retn);
	__declspec(naked) void on_start_sound_stub()
	{
		__asm
		{
			pushad;
			push	eax;
			call	on_start_sound_hk;
			add		esp, 4;
			popad;
	
			// og
			push    ebx;
			push    esi;
			xor		ecx, ecx;
			push    edi;
			jmp		on_start_sound_stub_retn;
		}
	}

	namespace
	{
		bool validate_start_sound_hook_target(const std::uint32_t address)
		{
			static constexpr std::uint8_t expected_prologue[] = { 0x53, 0x56, 0x33, 0xC9, 0x57 };
			const auto engine_begin = static_cast<std::uintptr_t>(game::engine_module.handle);
			const auto engine_end = engine_begin + game::engine_module.size;
			if (!address || static_cast<std::uintptr_t>(address) < engine_begin ||
				static_cast<std::uintptr_t>(address) + sizeof(expected_prologue) > engine_end ||
				utils::memory::is_bad_code_ptr(reinterpret_cast<const void*>(address)) ||
				utils::memory::is_bad_read_ptr(reinterpret_cast<const void*>(address)))
			{
				return false;
			}
			return std::memcmp(reinterpret_cast<const void*>(address), expected_prologue, sizeof(expected_prologue)) == 0;
		}
	}

	ConCommand xo_debug_sound_print_cmd {};
	void xo_debug_sound_print_fn()
	{
		cmd::sound_debug_printing = !cmd::sound_debug_printing;
	}

	ConCommand xo_debug_sound_history_clear_cmd {};
	void xo_debug_sound_history_clear_fn()
	{
		sound_events::clear_history();
	}

	ConCommand xo_debug_runtime_interactions_cmd {};
	void xo_debug_runtime_interactions_fn()
	{
		game::console();
		Vector origin = {}, forward = {}, right = {}, up = {};
		const bool camera_ok = game::get_current_view_basis(origin, forward, right, up);
		void* mdlcache = nullptr;
		if (l4d2::mdl_cache && !utils::memory::is_bad_read_ptr(l4d2::mdl_cache))
		{
			mdlcache = reinterpret_cast<void*>(*reinterpret_cast<DWORD*>(l4d2::mdl_cache));
		}
		std::cout << "[Runtime Recovery] runtimeReady=" << loader::is_runtime_ready() << std::endl;
		std::cout << "[Runtime Recovery] StartSound target=0x" << std::hex << l4d2::hk_addr__start_sound << std::dec
			<< " valid=" << sound_events::m_hook_target_valid
			<< " installed=" << sound_events::m_hook_installed
			<< " candidates=" << sound_events::m_hook_candidate_count
			<< " reselected=" << sound_events::m_hook_target_reselected
			<< " events=" << sound_events::m_hook_event_count
			<< " lastEvent=" << sound_events::m_last_hook_event_time << std::endl;
		std::cout << "[Runtime Recovery] Muzzle candidates=" << dynamic_lighting::m_muzzle_candidate_sounds
			<< " accepted=" << dynamic_lighting::m_muzzle_accepted_sounds
			<< " rejects=" << dynamic_lighting::m_muzzle_rejected_non_fire
			<< " hard=" << dynamic_lighting::m_muzzle_rejected_hard
			<< " strict=" << dynamic_lighting::m_muzzle_rejected_strict
			<< " cooldown=" << dynamic_lighting::m_muzzle_skipped_cooldown
			<< " budget=" << dynamic_lighting::m_muzzle_skipped_budget
			<< " spawned=" << dynamic_lighting::m_muzzle_spawn_attempts
			<< " acceptReason=\"" << dynamic_lighting::m_muzzle_last_accept_reason << "\""
			<< " rejectReason=\"" << dynamic_lighting::m_muzzle_last_reject_reason << "\""
			<< " sound=\"" << dynamic_lighting::m_muzzle_last_sound << "\"" << std::endl;
		std::cout << "[Runtime Recovery] IServerTools=" << game::server_tools_available()
			<< " mdlcache=" << mdlcache << " camera=" << camera_ok
			<< " source=\"" << game::get_current_view_basis_source() << "\""
			<< " cameraFailures=" << game::get_current_view_basis_failures() << std::endl;
		std::cout << "[Runtime Recovery] markers batches=" << map_settings::marker_spawn_attempts()
			<< " failures=" << map_settings::marker_spawn_failures()
			<< " fallbacks=" << map_settings::marker_fallback_count()
			<< " drawCalls=" << remix_markers::m_draw_calls
			<< " drawn=" << remix_markers::m_drawn_markers
			<< " noDevice=" << remix_markers::m_draw_no_device
			<< " status=\"" << map_settings::marker_spawn_status() << "\"" << std::endl;
		std::cout << "[Runtime Recovery] AcceptInput scanComplete=" << dynamic_lighting::m_server_accept_input_hook_scan_complete
			<< " attempts=" << dynamic_lighting::m_server_accept_input_hook_attempts
			<< " failures=" << dynamic_lighting::m_server_accept_input_hook_failures
			<< " vtables=" << dynamic_lighting::m_server_accept_input_vtables
			<< " status=\"" << dynamic_lighting::m_server_accept_input_hook_status << "\"" << std::endl;
		const auto* im = imgui::get();
		std::cout << "[Runtime Recovery] cursor requests=" << imgui::m_cursor_center_count
			<< " suppressed=" << imgui::m_cursor_center_suppressed_count
			<< " legacyWarp=" << imgui::m_legacy_cursor_warp_enabled
			<< " menu=" << (im && im->m_menu_active)
			<< " inputArmed=" << (im && im->m_menu_input_armed)
			<< " openRequests=" << (im ? im->m_menu_open_requests : 0u)
			<< " renderedFrames=" << (im ? im->m_menu_frames_rendered : 0u)
			<< " armEvents=" << (im ? im->m_menu_input_arm_events : 0u)
			<< " backendInit=" << (im ? im->m_backend_init_attempts : 0u)
			<< " backendInitFailures=" << (im ? im->m_backend_init_failures : 0u)
			<< " visibilityTransitions=" << (im ? im->m_cursor_visibility_transitions : 0u)
			<< " unlockMaintenance=" << (im ? im->m_cursor_unlock_maintenance : 0u)
			<< " visibilityRecoveries=" << (im ? im->m_cursor_visibility_recoveries : 0u)
			<< " loadingPassthroughFrames=" << (im ? im->m_loading_passthrough_frames : 0u)
			<< " lastReason=\"" << imgui::m_cursor_center_last_reason << "\"" << std::endl;
		std::cout << "[Runtime Recovery] projection calls=" << ::common::imgui::world2screen_calls()
			<< " failures=" << ::common::imgui::world2screen_failures()
			<< " cameraFallback=" << ::common::imgui::world2screen_fallback_successes() << std::endl;
		if (camera_ok)
		{
			std::cout << "[Runtime Recovery] view origin=" << origin.x << ' ' << origin.y << ' ' << origin.z
				<< " forward=" << forward.x << ' ' << forward.y << ' ' << forward.z << std::endl;
		}
	}

	sound_events::sound_events()
	{
		// Re-enumerate the exact five-byte prologue at installation time. This catches
		// stale cached addresses and records ambiguity after a Steam engine update.
		const auto candidates = utils::mem::find_pattern_matches(game::engine_module, "53 56 33 C9 57");
		m_hook_candidate_count = static_cast<std::uint32_t>(candidates.size());
		if (!candidates.empty())
		{
			const auto preferred = static_cast<std::uintptr_t>(game::engine_module.handle) + 0x1C0B6u;
			const auto nearest = *std::min_element(candidates.begin(), candidates.end(), [preferred](const DWORD a, const DWORD b)
			{
				const auto da = static_cast<std::uintptr_t>(a) >= preferred ? static_cast<std::uintptr_t>(a) - preferred : preferred - static_cast<std::uintptr_t>(a);
				const auto db = static_cast<std::uintptr_t>(b) >= preferred ? static_cast<std::uintptr_t>(b) - preferred : preferred - static_cast<std::uintptr_t>(b);
				return da < db;
			});
			if (nearest != l4d2::hk_addr__start_sound)
			{
				l4d2::hk_addr__start_sound = nearest;
				m_hook_target_reselected = true;
			}
		}

		m_hook_target_valid = validate_start_sound_hook_target(l4d2::hk_addr__start_sound);
		if (m_hook_target_valid)
		{
			utils::hook(l4d2::hk_addr__start_sound, on_start_sound_stub).install()->quick();
			HOOK_RETN_PLACE(on_start_sound_stub_retn, l4d2::hk_addr__start_sound + 5u);
			m_hook_installed = true;
		}
		else
		{
			game::console();
			std::cerr << "[Sound Events][ERROR] S_StartSound signature resolved to an invalid prologue; "
				"muzzle flash and sound-triggered systems were not hooked." << std::endl;
		}

		game::con_add_command(&xo_debug_sound_print_cmd, "xo_debug_sound_print", xo_debug_sound_print_fn, "Toggle sound debug prints (HASH for map_settings)");
		game::con_add_command(&xo_debug_sound_history_clear_cmd, "xo_debug_sound_history_clear", xo_debug_sound_history_clear_fn, "Clear the ImGui sound event history");
		game::con_add_command(&xo_debug_runtime_interactions_cmd, "xo_debug_runtime_interactions", xo_debug_runtime_interactions_fn,
			"Print muzzle sound hook, marker dependencies and light gizmo camera status");
	}
}