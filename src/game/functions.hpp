#pragma once

#define RENDERER_MOD            game::shaderapidx9_module
#define STUDIORENDER_MOD         game::studiorender_module
#define ENGINE_MOD               game::engine_module
#define CLIENT_MOD               game::client_module
#define SERVER_MOD               game::server_module
#define VSTDLIB_MOD              game::vstdlib_module

#define RENDERER_BASE            game::shaderapidx9_module.handle
#define STUDIORENDER_BASE        game::studiorender_module.handle
#define ENGINE_BASE              game::engine_module.handle
#define CLIENT_BASE              game::client_module.handle
#define SERVER_BASE              game::server_module.handle
#define VSTDLIB_BASE             game::vstdlib_module.handle

using namespace components;

namespace glob
{
	extern bool spawned_external_console;
	extern bool diagnostic_output_initialized;
	extern std::string diagnostic_log_path;
	extern HWND main_window;
}

namespace game
{
	extern std::vector<std::string> loaded_modules;
	extern std::string root_path;
	extern utils::mem::module_info shaderapidx9_module;
	extern utils::mem::module_info studiorender_module;
	extern utils::mem::module_info materialsystem_module;
	extern utils::mem::module_info engine_module;
	extern utils::mem::module_info client_module;
	extern utils::mem::module_info server_module;
	extern utils::mem::module_info vstdlib_module;

	extern const D3DXMATRIX IDENTITY;
	extern const D3DXMATRIX TC_TRANSLATE_TO_CENTER;
	extern const D3DXMATRIX TC_TRANSLATE_FROM_CENTER_TO_TOP_LEFT;

	inline components::CRender* get_engine_renderer() { return l4d2::engine_renderer; }
	inline IDirect3DDevice9* get_d3d_device() { return l4d2::d3d_device_ptr ? reinterpret_cast<IDirect3DDevice9*>(*l4d2::d3d_device_ptr) : nullptr; }
	inline IShaderAPIDX8* get_shaderapi() { return l4d2::shaderapi_ptr ? reinterpret_cast<IShaderAPIDX8*>(*l4d2::shaderapi_ptr) : nullptr; }
	inline IMaterialSystem* get_material_system() { return l4d2::material_system_ptr ? reinterpret_cast<IMaterialSystem*>(*l4d2::material_system_ptr) : nullptr; }
	inline worldbrushdata_t* get_hoststate_worldbrush_data() { return l4d2::hoststate_worldbrush_data_ptr ? reinterpret_cast<worldbrushdata_t*>(*l4d2::hoststate_worldbrush_data_ptr) : nullptr; }
	extern sdk::CCvar* get_icvar();
	inline IVModelInfo* get_modelinfo() { return l4d2::modelinfo_ptr ? reinterpret_cast<IVModelInfo*>(*l4d2::modelinfo_ptr) : nullptr; }

	extern ConVar* find_cvar(const char* name);
	extern const ConVar* find_cvar_const(const char* name);

	inline Vector* get_current_view_origin() { return l4d2::current_view_origin; }
	inline Vector* get_current_view_forward() { return l4d2::current_view_forward; }
	inline Vector* get_current_view_right() { return l4d2::current_view_forward ? &l4d2::current_view_forward[3] : nullptr; }
	inline Vector* get_current_view_up() { return l4d2::current_view_forward ? &l4d2::current_view_forward[6] : nullptr; }

	// Returns a repaired camera basis. Recovered engine globals are primary because
	// they are the same view consumed by world2screen; CRender is only a fallback.
	extern bool get_current_view_basis(Vector& origin, Vector& forward, Vector& right, Vector& up);
	extern bool get_current_view_origin_safe(Vector& origin);
	extern const char* get_current_view_basis_source();
	extern std::uint64_t get_current_view_basis_failures();

	inline Vector* get_camera_forward_vector() { return l4d2::camera_forward_vector; }

	// returns C_BaseAnimating class pointer for a given IClientRenderable
	C_BaseAnimating* get_base_animating_for_client_renderable(IClientRenderable* pRenderable);

	namespace namespaces
	{
		namespace C_BaseAnimating
		{
			// returns bone matrix for given bone index
			/// @param this_ptr			C_BaseAnimating ptr
			/// @param bone				bone index
			/// @param boneToWorld		out bone matrix
			inline void GetBoneTransform(void* this_ptr, const int bone, matrix3x4_t* boneToWorld)
			{
				// 55 8B EC 56 8B F1 83 BE ? ? ? ? ? 57 75 ? 8B 46 ? 8B 50 ? 8D 4E ? FF D2 85 C0 74 ? 8B CE E8 ? ? ? ? 8B 86
				utils::hook::call<void(__fastcall)(void* this_ptr, void* null, int bone, matrix3x4_t* boneToWorld)>(l4d2::fn_addr__get_bone_transform)
					(this_ptr, nullptr, bone, boneToWorld);
			}

			// returns bone index for given bone name
			/// @param this_ptr			C_BaseAnimating ptr
			/// @param bone_name		bone name
			/// @return					bone index
			inline int LookupBone(void* this_ptr, const char* bone_name)
			{
				//xref "doorhandlebone"
				return utils::hook::call<int(__fastcall)(void* this_ptr, void* null, const char* bone_name)>(l4d2::fn_addr__lookup_bone)
					(this_ptr, nullptr, bone_name);
			}

			// returns CStudioHdr pointer for given C_BaseAnimating pointer
			/// @param this_ptr			C_BaseAnimating ptr
			/// @return					CStudioHdr ptr
			inline CStudioHdr* GetModelPtr(void* this_ptr)
			{
				// 56 8B F1 83 BE ? ? ? ? ? 75 ? 8B 46 ? 8B 50 ? 8D 4E ? FF D2 85 C0 74 ? 8B CE E8 ? ? ? ? 8B 86 ? ? ? ? 5E 85 C0 74 ? ? ? ? 75 ? 33 C0 C3
				return utils::hook::call<CStudioHdr * (__fastcall)(void* this_ptr, void* null)>(l4d2::fn_addr__get_model_ptr)
					(this_ptr, nullptr);
			}
		}
	}

	inline int get_visframecount() { return l4d2::visframecount ? *l4d2::visframecount : 0; }
	inline components::view_id get_viewid() { return l4d2::viewid ? *l4d2::viewid : components::VIEW_ILLEGAL; }

	extern void con_add_command(ConCommand* cmd, const char* name, void(__cdecl* callback)(), const char* desc);
	extern void debug_add_text_overlay(const float* pos, float duration, const char* text);
	extern void debug_add_text_overlay(const float* pos, const char* text, int line_offset = 0, float r = 1.0f, float g = 1.0f, float b = 1.0f, float a = 1.0f);
	extern void cbaseentity_remove(void* cbaseentity_ptr);
	extern CBaseEntity* server_tools_create_entity(const char* class_name);
	extern bool server_tools_set_key_value(CBaseEntity* entity, const char* key, const char* value);
	extern bool server_tools_dispatch_spawn(CBaseEntity* entity);
	extern bool server_tools_available();

	extern void cvar_uncheat(const char* name);
	extern void cvar_uncheat_and_set_int(const char* name, int val);
	extern void cvar_uncheat_and_set_float(const char* name, float val);

	extern void print_ingame(const char* msg, ...);

	// CM_PointLeafnum
	inline int get_leaf_from_position(const Vector& pos) { return utils::hook::call<int(__cdecl)(const float*)>(l4d2::fn_addr__point_leafnum)(&pos.x); }

	// ::
	// debug print console redirects

	static void(WINAPI* OriginalOutputDebugStringA)(LPCSTR lpOutputString) = nullptr;
	static void(WINAPI* OriginalOutputDebugStringW)(LPCWSTR lpOutputString) = nullptr;

	inline void WINAPI HookedOutputDebugStringA(LPCSTR lpOutputString)
	{
		if (lpOutputString) 
		{
			printf("[>] %s", lpOutputString);
			fflush(stdout);
		}

		// og func
		if (OriginalOutputDebugStringA) {
			OriginalOutputDebugStringA(lpOutputString);
		}
	}

	inline void WINAPI HookedOutputDebugStringW(LPCWSTR lpOutputString)
	{
		if (lpOutputString) 
		{
			// Convert wide string to multibyte string for printf
			char buffer[1024];
			WideCharToMultiByte(CP_UTF8, 0, lpOutputString, -1, buffer, sizeof(buffer), NULL, NULL);

			printf("[>] %s", buffer);
			fflush(stdout); 
		}

		// og func
		if (OriginalOutputDebugStringW) {
			OriginalOutputDebugStringW(lpOutputString);
		}
	}

	inline void SetupDebugOutputHook()
	{
		if (MH_CreateHook(&OutputDebugStringA, &HookedOutputDebugStringA, reinterpret_cast<LPVOID*>(&OriginalOutputDebugStringA)) != MH_OK) 
		{
			std::cout << "[!][ERROR] Failed to create hook for OutputDebugStringA\n";
			return;
		}

		if (MH_CreateHook(&OutputDebugStringW, &HookedOutputDebugStringW, reinterpret_cast<LPVOID*>(&OriginalOutputDebugStringW)) != MH_OK) 
		{
			std::cout << "[!][ERROR] Failed to create hook for OutputDebugStringW\n";
			return;
		}

		if (MH_EnableHook(&OutputDebugStringA) != MH_OK || MH_EnableHook(&OutputDebugStringW) != MH_OK) 
		{
			std::cout << "[!][ERROR] Failed to enable hooks for OutputDebugStringA & OutputDebugStringW\n";
			return;
		}
	}

	namespace diagnostics
	{
		inline bool external_console_requested()
		{
#if defined(_DEBUG)
			return true;
#else
			static const bool requested = []
			{
				int argument_count = 0;
				auto** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
				if (!arguments) return false;
				bool found = false;
				for (int i = 0; i < argument_count; ++i)
				{
					if (_wcsicmp(arguments[i], L"-xo_debug_console") == 0)
					{
						found = true;
						break;
					}
				}
				LocalFree(arguments);
				return found;
			}();
			return requested;
#endif
		}

		inline std::filesystem::path log_path()
		{
			char module_path[MAX_PATH] = {};
			const DWORD length = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
			std::error_code ec;
			std::filesystem::path base = length > 0u
				? std::filesystem::path(module_path).parent_path()
				: std::filesystem::current_path(ec);
			if (base.empty()) base = ".";
			return base / "l4d2-rtx" / "logs" / "compat.log";
		}

		inline bool redirect_to_log()
		{
			const auto path = log_path();
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			glob::diagnostic_log_path = path.string();

			FILE* output = nullptr;
			const bool stdout_ready = freopen_s(&output, glob::diagnostic_log_path.c_str(), "a", stdout) == 0;
			FILE* errors = nullptr;
			const bool stderr_ready = freopen_s(&errors, glob::diagnostic_log_path.c_str(), "a", stderr) == 0;
			if (stdout_ready || stderr_ready)
			{
				setvbuf(stdout, nullptr, _IONBF, 0);
				setvbuf(stderr, nullptr, _IONBF, 0);
				std::cout << "\n[diagnostics] " COMPMOD_NAME " log opened without an external console\n";
			}
			return stdout_ready || stderr_ready;
		}
	}

	/**
	 * Initializes diagnostics without opening a visible CMD window. Release builds
	 * write to l4d2-rtx\logs\compat.log by default. Use -xo_debug_console when
	 * an interactive console is explicitly required.
	 */
	inline void console()
	{
		if (glob::diagnostic_output_initialized) return;
		glob::diagnostic_output_initialized = true;

		if (diagnostics::external_console_requested() && AllocConsole())
		{
			glob::spawned_external_console = true;
			FILE* file = nullptr;
			freopen_s(&file, "CONIN$", "r", stdin);
			freopen_s(&file, "CONOUT$", "w", stdout);
			freopen_s(&file, "CONOUT$", "w", stderr);
			setvbuf(stdout, nullptr, _IONBF, 0);
			setvbuf(stderr, nullptr, _IONBF, 0);
			SetConsoleOutputCP(CP_UTF8);
			SetConsoleTitleA(COMPMOD_NAME " Comp Debug Console");
			return;
		}

		diagnostics::redirect_to_log();
	}
}
