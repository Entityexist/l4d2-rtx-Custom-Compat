#include "std_include.hpp"

namespace components
{
	template <typename m_interface>
	m_interface* interfaces::get_interface(const std::string& module_name, const std::string& interface_name)
	{
		using create_interface_fn = void* (*)(const char*, int*);
		const auto fn = reinterpret_cast<create_interface_fn>(GetProcAddress(GetModuleHandleA(module_name.c_str()), "CreateInterface"));

		if (!fn) {
			return nullptr;
		}

		return static_cast<m_interface*>(fn(interface_name.c_str(), {}));
	}

#define GET_INTERFACE(DEST, T, MODULE_NAME, VERSION_STR)																									\
		if((DEST) = get_interface<T>((MODULE_NAME), (VERSION_STR)); !(DEST)) {																			\
			Beep(300, 100); Sleep(100); Beep(200, 100);															\
			game::console(); std::cout << "[!][Interfaces] Failed to get interface: '" << (VERSION_STR) << "' in: '" << (MODULE_NAME) << "'" << std::endl;	\
		}

	interfaces::interfaces()
	{
		p_this = this;
		GET_INTERFACE(m_client, sdk::base_client, "client.dll", CLIENT_INTERFACE_VERSION);
		GET_INTERFACE(m_engine, sdk::engine_client, "engine.dll", ENGINE_INTERFACE_VERSION);
		GET_INTERFACE(m_effects, sdk::engine_effects, "engine.dll", VENGINE_EFFECTS_INTERFACE_VERSION);

		// L4D2 exposes EngineTraceClient003 with TraceRay at vtable slot 5.
		// Prefer the audited ABI. A generic Source 004 fallback keeps the standard slot 4.
		const auto trace_target_in_engine = [](sdk::engine_trace* trace, const std::size_t slot)
		{
			if (!trace) return false;
			const auto module = GetModuleHandleA("engine.dll");
			if (!module) return false;

			const auto base = reinterpret_cast<std::uintptr_t>(module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

			const auto target = reinterpret_cast<std::uintptr_t>((*reinterpret_cast<void***>(trace))[slot]);
			const auto end = base + nt->OptionalHeader.SizeOfImage;
			return target >= base && target < end;
		};

		m_engine_trace = get_interface<sdk::engine_trace>("engine.dll", ENGINE_TRACE_CLIENT_VERSION_003);
		if (m_engine_trace && trace_target_in_engine(m_engine_trace, 5u))
		{
			sdk::engine_trace::configure_trace_ray_slot(5u);
		}
		else
		{
			m_engine_trace = get_interface<sdk::engine_trace>("engine.dll", ENGINE_TRACE_CLIENT_VERSION_004);
			if (m_engine_trace && trace_target_in_engine(m_engine_trace, 4u))
				sdk::engine_trace::configure_trace_ray_slot(4u);
			else
				m_engine_trace = nullptr;
		}
		if (!m_engine_trace)
		{
			game::console();
			std::cout << "[!][Interfaces] EngineTraceClient003/004 unavailable; Light Studio surface placement uses distance fallback." << std::endl;
		}
		GET_INTERFACE(m_entity_list, sdk::entity_list, "client.dll", CLIENT_ENTITY_INTERFACE_VERSION);
		GET_INTERFACE(m_model_info, IVModelInfo, "engine.dll", "VModelInfoClient004");
		GET_INTERFACE(m_cvar, sdk::CCvar, "vstdlib.dll", CVAR_INTERFACE_VERSION);

		// L4D2 exposes the legacy server-tools ABI. The entity spawning methods are
		// stable at vtable slots 13/14 across the exposed VSERVERTOOLS generations.
		// Prefer the newest interface and fall back without console/beep noise.
		m_server_tools = get_interface<void>("server.dll", "VSERVERTOOLS003");
		if (!m_server_tools) m_server_tools = get_interface<void>("server.dll", "VSERVERTOOLS002");
		if (!m_server_tools) m_server_tools = get_interface<void>("server.dll", "VSERVERTOOLS001");
		if (!m_server_tools)
		{
			game::console();
			std::cout << "[!][Interfaces] VSERVERTOOLS001-003 unavailable; optional server entity spawning features are disabled." << std::endl;
		}

		m_player_manager = get_interface<sdk::player_info_manager>("server.dll", PLAYER_INFO_MANAGER_INTERFACE_VERSION);
		if (m_player_manager)
			m_globals = m_player_manager->get_global_vars();
		else
		{
			game::console();
			std::cout << "[!][Interfaces] PlayerInfoManager002 unavailable; global timing data is disabled." << std::endl;
		}

		// L4D2 currently exports Surface031; the submitted L4D1 build exports 030.
		m_surface = get_interface<sdk::surface>("vguimatsurface.dll", VGUI_MAT_SURFACE_INTERFACE_VERSION);
		if (!m_surface) m_surface = get_interface<sdk::surface>("vguimatsurface.dll", "VGUI_Surface030");
		if (!m_surface)
		{
			game::console();
			std::cout << "[!][Interfaces] VGUI_Surface030/031 unavailable; VGUI drawing helpers are disabled." << std::endl;
		}
	}
}