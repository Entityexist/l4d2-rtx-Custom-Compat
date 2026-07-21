#include "std_include.hpp"

namespace glob
{
	bool spawned_external_console = false;
	bool diagnostic_output_initialized = false;
	std::string diagnostic_log_path;
	HWND main_window = nullptr;
}

namespace game
{
	std::vector<std::string> loaded_modules;
	std::string root_path;
	utils::mem::module_info shaderapidx9_module = {};
	utils::mem::module_info studiorender_module = {};
	utils::mem::module_info materialsystem_module = {};
	utils::mem::module_info engine_module = {};
	utils::mem::module_info client_module = {};
	utils::mem::module_info server_module = {};
	utils::mem::module_info vstdlib_module = {};

	const D3DXMATRIX IDENTITY =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	const D3DXMATRIX TC_TRANSLATE_TO_CENTER =
	{
		 1.0f,  0.0f, 0.0f, 0.0f,	// identity
		 0.0f,  1.0f, 0.0f, 0.0f,	// identity
		 0.0f,  0.0f, 1.0f, 0.0f,	// identity
		-0.5f, -0.5f, 0.0f, 1.0f,	// translate to center
	};

	const D3DXMATRIX TC_TRANSLATE_FROM_CENTER_TO_TOP_LEFT =
	{
		1.0f, 0.0f, 0.0f, 0.0f,	// identity
		0.0f, 1.0f, 0.0f, 0.0f,	// identity
		0.0f, 0.0f, 1.0f, 0.0f,	// identity
		0.5f, 0.5f, 0.0f, 1.0f,	// translate back to the top left corner
	};

	sdk::CCvar* get_icvar()
	{
		return interfaces::get() ? interfaces::get()->m_cvar : nullptr;
	}

	ConVar* find_cvar(const char* name)
	{
		if (const auto icvar = game::get_icvar(); icvar) {
			return icvar->find_var(name);
		}
		return nullptr;
	}

	const ConVar* find_cvar_const(const char* name)
	{
		if (const auto icvar = game::get_icvar(); icvar) {
			return icvar->find_var_const(name);
		}
		return nullptr;
	}

	namespace
	{
		std::string g_view_basis_source = "unavailable";
		std::uint64_t g_view_basis_failures = 0u;

		bool valid_view_vector(const Vector& value, const bool allow_zero = false)
		{
			if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) return false;
			if (std::fabs(value.x) > 1048576.0f || std::fabs(value.y) > 1048576.0f || std::fabs(value.z) > 1048576.0f) return false;
			return allow_zero || value.LengthSqr() > 0.000001f;
		}

		bool repair_view_basis(Vector& forward, Vector& right, Vector& up)
		{
			if (!valid_view_vector(forward)) return false;
			forward.NormalizeChecked();

			if (valid_view_vector(right))
			{
				right -= forward * forward.Dot(right);
			}
			if (!valid_view_vector(right))
			{
				if (valid_view_vector(up)) right = forward.Cross(up);
				if (!valid_view_vector(right))
				{
					const Vector world_up = std::fabs(forward.z) < 0.98f ? Vector(0.0f, 0.0f, 1.0f) : Vector(0.0f, 1.0f, 0.0f);
					right = forward.Cross(world_up);
				}
			}
			if (!valid_view_vector(right)) return false;
			right.NormalizeChecked();
			up = right.Cross(forward);
			if (!valid_view_vector(up)) return false;
			up.NormalizeChecked();
			return true;
		}

		bool read_raw_view_origin(Vector& origin)
		{
			const auto* raw_origin = get_current_view_origin();
			if (!raw_origin || utils::memory::is_bad_read_ptr(raw_origin) || !valid_view_vector(*raw_origin, true)) return false;
			origin = *raw_origin;
			return true;
		}
	}

	bool get_current_view_origin_safe(Vector& origin)
	{
		if (read_raw_view_origin(origin)) return true;
		if (auto* renderer = get_engine_renderer(); renderer &&
			!utils::memory::is_bad_read_ptr(renderer) && !utils::memory::is_bad_read_ptr(renderer->vftable) &&
			renderer->vftable->ViewOrigin &&
			!utils::memory::is_bad_code_ptr(reinterpret_cast<const void*>(renderer->vftable->ViewOrigin)))
		{
			auto* render = static_cast<IRender*>(renderer);
			const auto* render_origin = renderer->vftable->ViewOrigin(render);
			if (render_origin && !utils::memory::is_bad_read_ptr(render_origin) && valid_view_vector(*render_origin, true))
			{
				origin = *render_origin;
				return true;
			}
		}
		return false;
	}

	bool get_current_view_basis(Vector& origin, Vector& forward, Vector& right, Vector& up)
	{
		// The recovered globals are primary. They match the exact camera used by the
		// existing world2screen path and were the stable behavior before V21.12.2.
		const auto* raw_forward = get_current_view_forward();
		const auto* raw_right = get_current_view_right();
		const auto* raw_up = get_current_view_up();
		if (read_raw_view_origin(origin) && raw_forward && !utils::memory::is_bad_read_ptr(raw_forward))
		{
			forward = *raw_forward;
			right = raw_right && !utils::memory::is_bad_read_ptr(raw_right) ? *raw_right : Vector();
			up = raw_up && !utils::memory::is_bad_read_ptr(raw_up) ? *raw_up : Vector();
			if (repair_view_basis(forward, right, up))
			{
				g_view_basis_source = "engine globals (repaired)";
				return true;
			}
		}

		// CRender is a fallback only. It is useful during early frames but its virtual
		// layout must not hide otherwise valid engine-global camera data.
		if (auto* renderer = get_engine_renderer(); renderer &&
			!utils::memory::is_bad_read_ptr(renderer) &&
			!utils::memory::is_bad_read_ptr(renderer->vftable) &&
			renderer->vftable->ViewOrigin && renderer->vftable->ViewAngles &&
			!utils::memory::is_bad_code_ptr(reinterpret_cast<const void*>(renderer->vftable->ViewOrigin)) &&
			!utils::memory::is_bad_code_ptr(reinterpret_cast<const void*>(renderer->vftable->ViewAngles)))
		{
			auto* render = static_cast<IRender*>(renderer);
			const auto* render_origin = renderer->vftable->ViewOrigin(render);
			const auto* render_angles = renderer->vftable->ViewAngles(render);
			if (render_origin && render_angles &&
				!utils::memory::is_bad_read_ptr(render_origin) && !utils::memory::is_bad_read_ptr(render_angles) &&
				valid_view_vector(*render_origin, true))
			{
				origin = *render_origin;
				utils::vector::AngleVectors(*render_angles, &forward, &right, &up);
				if (repair_view_basis(forward, right, up))
				{
					g_view_basis_source = "CRender fallback";
					return true;
				}
			}
		}

		++g_view_basis_failures;
		g_view_basis_source = "unavailable";
		return false;
	}

	const char* get_current_view_basis_source()
	{
		return g_view_basis_source.c_str();
	}

	std::uint64_t get_current_view_basis_failures()
	{
		return g_view_basis_failures;
	}

	// adds a simple console command
	void con_add_command(ConCommand* cmd, const char* name, void(__cdecl* callback)(), const char* desc)
	{
		// ConCommand *this, const char *pName, void (__cdecl *callback)(), const char *pHelpString, int flags, int (__cdecl *completionFunc)(const char *, char (*)[64]
		utils::hook::call<void(__fastcall)(ConCommand* this_ptr, void* null, const char*, void(__cdecl*)(), const char*, int, int(__cdecl*)(const char*, char(*)[64]))>(l4d2::fn_addr__add_console_cmd)
			(cmd, nullptr, name, callback, desc, 0x20000, nullptr);
	}

	/**
	 * Calls CDebugOverlay::AddTextOverlay
	 * @param pos		Position of text in 3D Space
	 * @param duration	Duration in which text is visible - use 0.0f for per frame stuff
	 * @param text		The text
	 */
	void debug_add_text_overlay(const float* pos, float duration, const char* text)
	{
		utils::hook::call<void(__cdecl)(const float*, float, const char*)>(l4d2::fn_addr__debug_overlay_add_text)
			(pos, duration, text);
	}

	/**
	 * Calls CDebugOverlay::AddTextOverlay
	 * @param pos			Position of text in 3D Space
	 * @param text			The text
	 * @param line_offset	Offset text position
	 * @param r				red (0-1)
	 * @param g				green (0-1)
	 * @param b				blue (0-1)
	 * @param a				alpha (0-1)
	 */
	void debug_add_text_overlay(const float* pos, const char* text, const int line_offset, const float r, const float g, const float b, const float a)
	{
		utils::hook::call<void(__cdecl)(const float*, int, float, float, float, float, float, const char*)>(l4d2::fn_addr__debug_overlay_add_text_colored)
			(pos, line_offset, 0.0f, r, g, b, a, text);
	}

	// remove/destroy a given CBaseEntity
	void cbaseentity_remove(void* cbaseentity_ptr)
	{
		if (cbaseentity_ptr)
		{
			// UTIL_Remove
			utils::hook::call<void(__cdecl)(void* cbaseentity)>(l4d2::fn_addr__util_remove)(cbaseentity_ptr); // #OFFS 2501
		}
	}

	bool server_tools_available()
	{
		return interfaces::get() && interfaces::get()->m_server_tools;
	}

	CBaseEntity* server_tools_create_entity(const char* class_name)
	{
		if (!class_name || !*class_name || !server_tools_available()) return nullptr;
		return utils::hook::call_virtual<13, CBaseEntity*>(interfaces::get()->m_server_tools, class_name);
	}

	bool server_tools_set_key_value(CBaseEntity* entity, const char* key, const char* value)
	{
		if (!entity || !key || !*key || !value || !server_tools_available()) return false;
		return utils::hook::call_virtual<10, bool>(interfaces::get()->m_server_tools, entity, key, value);
	}

	bool server_tools_dispatch_spawn(CBaseEntity* entity)
	{
		if (!entity || !server_tools_available()) return false;
		utils::hook::call_virtual<14, void>(interfaces::get()->m_server_tools, entity);
		return true;
	}

	void cvar_uncheat(const char* name)
	{
		if (const auto ivar = game::get_icvar(); ivar)
		{
			if (auto var = ivar->find_var(name); var)
			{
				var->m_nFlags &= ~(1 << 1); // FCVAR_DEVELOPMENTONLY
				var->m_nFlags &= ~(1 << 4); // FCVAR_HIDDEN
				var->m_nFlags &= ~(1 << 14); // FCVAR_CHEAT
			}
		}
	}

	void cvar_uncheat_and_set_int(const char* name, const int val)
	{
		if (const auto ivar = game::get_icvar(); ivar)
		{
			if (auto var = ivar->find_var(name); var)
			{
				var->vtbl->SetValue_Int(var, val);
				var->m_nFlags &= ~(1 << 1); // FCVAR_DEVELOPMENTONLY
				var->m_nFlags &= ~(1 << 4); // FCVAR_HIDDEN
				var->m_nFlags &= ~(1 << 14); // FCVAR_CHEAT
			}
		}
	}

	void cvar_uncheat_and_set_float(const char* name, const float val)
	{
		if (const auto ivar = game::get_icvar(); ivar)
		{
			if (auto var = ivar->find_var(name); var)
			{
				var->vtbl->SetValue_Float(var, val);
				var->m_nFlags &= ~(1 << 1); // FCVAR_DEVELOPMENTONLY
				var->m_nFlags &= ~(1 << 4); // FCVAR_HIDDEN
				var->m_nFlags &= ~(1 << 14); // FCVAR_CHEAT
			}
		}
	}

	typedef void(__cdecl* msg_fn)(const char* msg, va_list);
	void print_ingame(const char* msg, ...)
	{
		if (msg == nullptr) {
			return;
		}

		static msg_fn fn = (msg_fn)GetProcAddress(GetModuleHandleA("tier0.dll"), "Msg");
		char buffer[989];

		va_list list;
		va_start(list, msg);
		vsprintf(buffer, msg, list);
		perror(buffer);
		va_end(list);
		fn(buffer, list);
	}

	C_BaseAnimating* get_base_animating_for_client_renderable(IClientRenderable* pRenderable)
	{
		if (pRenderable)
		{
			if (const auto unkown = pRenderable->vftable_iclientrenderable->GetIClientUnknown(pRenderable);
				unkown)
			{
				if (const auto base_handle = unkown->vftable_ihandleent->GetRefEHandle(unkown);
					base_handle)
				{
					if (const auto base_entity = interfaces::get()->m_entity_list->get_client_entity_from_handle(*base_handle);
						base_entity) {
						return base_entity->vtbl->GetBaseAnimating(base_entity);
					}
				}
			}
		}

		return nullptr;
	}
}
