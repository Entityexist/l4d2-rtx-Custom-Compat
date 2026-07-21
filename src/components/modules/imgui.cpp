#include "std_include.hpp"
#include <cctype>
#include <numeric>
#include <limits>
#include "components/common/imgui/imgui_helper.hpp"
#include "components/common/toml.hpp"
#include "components/common/imgui/font_awesome_solid_900.hpp"
#include "components/common/imgui/font_defines.hpp"
#include "components/common/imgui/font_opensans.hpp"
#include "source_bsp_lights.hpp"

#include "imgui_internal.h"

// Allow us to directly call the ImGui WndProc function.
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#define SPACING_INDENT_BEGIN ImGui::Spacing(); ImGui::Indent()
#define SPACING_INDENT_END ImGui::Spacing(); ImGui::Unindent()
#define TT(TXT) ImGui::SetItemTooltipBlur((TXT));

#define SET_CHILD_WIDGET_WIDTH			ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(80.0f));
#define SET_CHILD_WIDGET_WIDTH_MAN(V)	ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild((V)));

namespace components
{
	namespace
	{
		IDirect3DTexture9* g_about_strawberry_texture = nullptr;
		bool g_about_strawberry_load_attempted = false;
		IDirect3DDevice9* g_overlay_present_device = nullptr;


		class remix_instance_category_guard
		{
		public:
			explicit remix_instance_category_guard(IDirect3DDevice9* device)
				: m_device(device)
			{
			}

			~remix_instance_category_guard()
			{
				restore_neutral();
			}

			void set(const DWORD categories)
			{
				if (!m_device) return;
				m_touched = SUCCEEDED(m_device->SetRenderState(
					static_cast<D3DRENDERSTATETYPE>(42), categories));
			}

			void restore_neutral()
			{
				if (!m_device || !m_touched) return;
				// RS42 is a one-draw Remix bridge payload, not persistent D3D9 state.
				// Restoring a sampled value can re-emit a stale IGNORE_LIGHTS/WORLD_UI
				// category on the next Source draw. Always end an ImGui scope at neutral 0.
				m_device->SetRenderState(static_cast<D3DRENDERSTATETYPE>(42), 0u);
				m_touched = false;
			}

			remix_instance_category_guard(const remix_instance_category_guard&) = delete;
			remix_instance_category_guard& operator=(const remix_instance_category_guard&) = delete;

		private:
			IDirect3DDevice9* m_device = nullptr;
			bool m_touched = false;
		};

		class d3d9_state_guard
		{
		public:
			explicit d3d9_state_guard(IDirect3DDevice9* device)
				: m_device(device)
			{
				if (!m_device) return;

				if (SUCCEEDED(m_device->CreateStateBlock(D3DSBT_ALL, &m_state)) && m_state)
				{
					if (FAILED(m_state->Capture()))
					{
						m_state->Release();
						m_state = nullptr;
					}
				}

				// D3D9 state blocks do not reliably preserve render targets/depth surfaces
				// across native D3D9, DXVK and RTX Remix. Capture them explicitly.
				for (DWORD slot = 0; slot < m_render_targets.size(); ++slot)
					m_have_render_target[slot] = SUCCEEDED(m_device->GetRenderTarget(slot, &m_render_targets[slot]));
				m_have_depth_stencil = SUCCEEDED(m_device->GetDepthStencilSurface(&m_depth_stencil));

				m_have_viewport = SUCCEEDED(m_device->GetViewport(&m_viewport));
				m_have_scissor = SUCCEEDED(m_device->GetScissorRect(&m_scissor));
				m_have_fvf = SUCCEEDED(m_device->GetFVF(&m_fvf));
				m_have_vertex_declaration = SUCCEEDED(m_device->GetVertexDeclaration(&m_vertex_declaration));
				m_have_vertex_shader = SUCCEEDED(m_device->GetVertexShader(&m_vertex_shader));
				m_have_pixel_shader = SUCCEEDED(m_device->GetPixelShader(&m_pixel_shader));
				m_have_indices = SUCCEEDED(m_device->GetIndices(&m_index_buffer));
				m_have_stream0 = SUCCEEDED(m_device->GetStreamSource(
					0u, &m_stream0, &m_stream0_offset, &m_stream0_stride));
				m_have_texture0 = SUCCEEDED(m_device->GetTexture(0u, &m_texture0));
			}

			~d3d9_state_guard()
			{
				if (!m_device) return;

				if (m_state) m_state->Apply();

				for (DWORD slot = 0; slot < m_render_targets.size(); ++slot)
				{
					if (m_have_render_target[slot] && m_render_targets[slot])
						m_device->SetRenderTarget(slot, m_render_targets[slot]);
					else if (slot > 0)
						m_device->SetRenderTarget(slot, nullptr);
				}
				if (m_have_depth_stencil) m_device->SetDepthStencilSurface(m_depth_stencil);

				if (m_have_vertex_shader) m_device->SetVertexShader(m_vertex_shader);
				if (m_have_pixel_shader) m_device->SetPixelShader(m_pixel_shader);
				if (m_have_stream0) m_device->SetStreamSource(
					0u, m_stream0, m_stream0_offset, m_stream0_stride);
				if (m_have_indices) m_device->SetIndices(m_index_buffer);
				if (m_have_texture0) m_device->SetTexture(0u, m_texture0);
				if (m_have_viewport) m_device->SetViewport(&m_viewport);
				if (m_have_scissor) m_device->SetScissorRect(&m_scissor);
				if (m_have_vertex_declaration && m_vertex_declaration)
					m_device->SetVertexDeclaration(m_vertex_declaration);
				else if (m_have_fvf)
					m_device->SetFVF(m_fvf);

				// RS42 is a transient Remix/Xorxor category payload. Never replay a
				// sampled category after ImGui; neutral zero prevents WORLD_UI or
				// IGNORE_LIGHTS from leaking into the next Source draw.
				m_device->SetRenderState(static_cast<D3DRENDERSTATETYPE>(42), 0u);

				if (m_texture0) m_texture0->Release();
				if (m_stream0) m_stream0->Release();
				if (m_index_buffer) m_index_buffer->Release();
				if (m_pixel_shader) m_pixel_shader->Release();
				if (m_vertex_shader) m_vertex_shader->Release();
				if (m_vertex_declaration) m_vertex_declaration->Release();
				if (m_depth_stencil) m_depth_stencil->Release();
				for (auto*& target : m_render_targets)
				{
					if (target) target->Release();
					target = nullptr;
				}
				if (m_state) m_state->Release();
			}

			d3d9_state_guard(const d3d9_state_guard&) = delete;
			d3d9_state_guard& operator=(const d3d9_state_guard&) = delete;

		private:
			IDirect3DDevice9* m_device = nullptr;
			IDirect3DStateBlock9* m_state = nullptr;
			std::array<IDirect3DSurface9*, 4> m_render_targets{};
			std::array<bool, 4> m_have_render_target{};
			IDirect3DSurface9* m_depth_stencil = nullptr;
			IDirect3DVertexDeclaration9* m_vertex_declaration = nullptr;
			IDirect3DVertexShader9* m_vertex_shader = nullptr;
			IDirect3DPixelShader9* m_pixel_shader = nullptr;
			IDirect3DIndexBuffer9* m_index_buffer = nullptr;
			IDirect3DVertexBuffer9* m_stream0 = nullptr;
			IDirect3DBaseTexture9* m_texture0 = nullptr;
			D3DVIEWPORT9 m_viewport{};
			RECT m_scissor{};
			DWORD m_fvf = 0u;
			UINT m_stream0_offset = 0u;
			UINT m_stream0_stride = 0u;
			bool m_have_depth_stencil = false;
			bool m_have_viewport = false;
			bool m_have_scissor = false;
			bool m_have_fvf = false;
			bool m_have_vertex_declaration = false;
			bool m_have_vertex_shader = false;
			bool m_have_pixel_shader = false;
			bool m_have_indices = false;
			bool m_have_stream0 = false;
			bool m_have_texture0 = false;
		};

		IDirect3DTexture9* about_strawberry_texture()
		{
			if (!g_about_strawberry_texture && !g_about_strawberry_load_attempted)
			{
				g_about_strawberry_load_attempted = true;
				if (auto* device = game::get_d3d_device(); device)
				{
					D3DXCreateTextureFromFileA(device,
						COMPMOD_ASSET_DIR "textures\\xorxor4d_strawberry.png", &g_about_strawberry_texture);
				}
			}
			return g_about_strawberry_texture;
		}
	}

	WNDPROC g_game_wndproc = nullptr;

	LRESULT __stdcall wnd_proc_hk(HWND window, UINT message_type, WPARAM wparam, LPARAM lparam)
	{
		auto* im = imgui::get();
		bool pass_msg_to_game = false;
		bool input_dispatched = false;

		switch (message_type)
		{
		case WM_KEYUP: // always pass button up events to prevent "stuck" game keys

		// allows user to move the game window via titlebar :>
		case WM_NCLBUTTONDOWN: case WM_NCLBUTTONUP: case WM_NCMOUSEMOVE: case WM_NCMOUSELEAVE:
		case WM_WINDOWPOSCHANGED:
		case WM_NCACTIVATE:
		case WM_SETFOCUS: case WM_KILLFOCUS:
		case WM_SYSCOMMAND:
		case WM_GETMINMAXINFO: case WM_ENTERSIZEMOVE: case WM_EXITSIZEMOVE:
		case WM_SIZING: case WM_MOVING: case WM_MOVE:
			pass_msg_to_game = true;
			break;

		case WM_SETCURSOR:
			if (im && im->menu_owns_input())
			{
				// ImGui draws the only visible pointer while the F5 menu owns input.
				SetCursor(nullptr);
				return TRUE;
			}

			// During the bounded front-end -> gameplay recovery window Source can
			// issue one or more late WM_SETCURSOR messages with the arrow cursor.
			// Eat only those transition messages; normal VGUI cursor handling is
			// untouched after the recovery counter reaches zero.
			if (im && !im->m_menu_active && im->gameplay_present_active() &&
				im->m_gameplay_input_recovery_frames > 0u)
			{
				SetCursor(nullptr);
				return TRUE;
			}

			pass_msg_to_game = true;
			break;

		case WM_INPUT:
		case WM_MOUSEMOVE:
		case WM_LBUTTONDOWN: case WM_LBUTTONUP:
		case WM_RBUTTONDOWN: case WM_RBUTTONUP:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP:
		case WM_XBUTTONDOWN: case WM_XBUTTONUP:
		case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
		case WM_CAPTURECHANGED:
			if (im && im->m_menu_active)
			{
				const bool consumed = im->input_message(message_type, wparam, lparam);
				input_dispatched = true;
				if (consumed) return 1;
				pass_msg_to_game = true;
			}
			break;

		case WM_MOUSEACTIVATE:
			// Never let Source enter relative-mouse mode while the visible F5 menu owns input.
			if (!im || !im->m_menu_active || im->game_input_passthrough())
				pass_msg_to_game = true;
			else
			{
				ImGui_ImplWin32_WndProcHandler(window, message_type, wparam, lparam);
				return MA_ACTIVATEANDEAT;
			}
			break;

		default: break;
		}

		if (!input_dispatched && im && im->input_message(message_type, wparam, lparam) && !pass_msg_to_game)
			return true;

		return CallWindowProc(g_game_wndproc, window, message_type, wparam, lparam);
	}

	void center_cursor(const char* reason)
	{
		++imgui::m_cursor_center_count;
		++imgui::m_cursor_center_suppressed_count;
		imgui::m_cursor_center_last_reason = reason ? reason : "unknown";
		// V21.14 never calls the Win32 SetCursorPos path. Source receives cursor
		// ownership through ISurface only, so a slow frame cannot snap the desktop
		// pointer to the centre even if a legacy caller requests recentering.
	}

	bool imgui::overlay_present_available() const
	{
		return glob::main_window != nullptr &&
			IsWindow(glob::main_window) != FALSE &&
			(g_overlay_present_device != nullptr || game::get_d3d_device() != nullptr);
	}

	bool imgui::gameplay_present_active() const
	{
		const auto* intf = interfaces::get();
		return loader::is_runtime_ready() && intf && intf->m_engine && intf->m_engine->is_playing();
	}

	void imgui::acquire_menu_cursor()
	{
		if (m_menu_cursor_owned) return;

		ClipCursor(nullptr);
		ReleaseCapture();

		if (const auto* intf = interfaces::get(); intf && intf->m_surface)
		{
			m_cursor_visible_before_menu = intf->m_surface->is_cursor_visible();
			intf->m_surface->unlock_cursor();
			intf->m_surface->set_cursor_always_visible(true);
		}
		else
		{
			m_cursor_visible_before_menu = false;
		}

		m_menu_cursor_owned = true;
		m_cursor_visibility_known = true;
		m_cursor_visible_applied = true;
		++m_cursor_visibility_transitions;
	}

	void imgui::release_menu_cursor()
	{
		ImGui::GetIO().MouseDrawCursor = false;
		const bool gameplay_active = gameplay_present_active();
		if (!m_menu_cursor_owned && !gameplay_active) return;

		// A menu can be opened in the Source front-end and closed after a map has
		// loaded. In that case the pre-menu cursor state is no longer authoritative:
		// gameplay must always regain locked relative input.
		if (const auto* intf = interfaces::get(); intf && intf->m_surface)
		{
			if (gameplay_active)
			{
				intf->m_surface->set_cursor_always_visible(false);
				intf->m_surface->lock_cursor();
			}
			else
			{
				intf->m_surface->set_cursor_always_visible(m_cursor_visible_before_menu);
				if (m_cursor_visible_before_menu) intf->m_surface->unlock_cursor();
			}
		}

		if (gameplay_active && glob::main_window && IsWindow(glob::main_window))
		{
			SetFocus(glob::main_window);
			SetCursor(nullptr);
		}

		m_menu_cursor_owned = false;
		m_cursor_visibility_known = true;
		m_cursor_visible_applied = gameplay_active ? false : m_cursor_visible_before_menu;
		++m_cursor_visibility_transitions;

		if (gameplay_active)
			schedule_gameplay_input_recovery(8u, "F5 menu release");
	}

	void imgui::apply_cursor_visibility(const bool visible, const char* reason)
	{
		m_cursor_center_last_reason = reason ? reason : "cursor visibility transition";
		if (visible) acquire_menu_cursor();
		else release_menu_cursor();
	}

	void imgui::maintain_menu_cursor_ownership()
	{
		auto& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		io.MouseDrawCursor = menu_owns_input();
	}

	void imgui::schedule_gameplay_input_recovery(const std::uint32_t frames, const char* reason)
	{
		if (frames == 0u) return;
		m_gameplay_input_recovery_frames = std::max(m_gameplay_input_recovery_frames, frames);
		m_cursor_center_last_reason = reason ? reason : "gameplay input recovery";
		++m_gameplay_input_recovery_events;
	}

	void imgui::recover_gameplay_input_frame()
	{
		if (m_gameplay_input_recovery_frames == 0u || !gameplay_present_active()) return;
		if (m_menu_active || m_menu_input_armed)
		{
			// A visible F5 menu intentionally owns the pointer. Keep the request
			// pending and apply it immediately after the menu closes.
			return;
		}

		m_im_allow_game_input = false;
		m_menu_just_opened = false;
		m_menu_cursor_owned = false;

		auto& io = ImGui::GetIO();
		io.MouseDrawCursor = false;
		for (bool& button : io.MouseDown) button = false;

		if (const auto* intf = interfaces::get(); intf && intf->m_surface)
		{
			intf->m_surface->set_cursor_always_visible(false);
			intf->m_surface->lock_cursor();
		}

		// Never steal focus from another application. When the game is already
		// foreground, hide any late Win32 arrow cursor left by the front-end.
		if (glob::main_window && IsWindow(glob::main_window) &&
			GetForegroundWindow() == glob::main_window)
		{
			SetFocus(glob::main_window);
			SetCursor(nullptr);
		}

		m_cursor_visibility_known = true;
		m_cursor_visible_applied = false;
		++m_cursor_visibility_transitions;
		++m_gameplay_input_recovery_frames_applied;
		--m_gameplay_input_recovery_frames;
	}

	void imgui::service_gameplay_input_state(const bool apply_recovery_frame)
	{
		const bool gameplay_active = gameplay_present_active();
		m_loading_screen_suspended = !gameplay_active;

		if (gameplay_active && !m_gameplay_present_was_active)
		{
			m_gameplay_present_was_active = true;
			if (!m_menu_active)
			{
				m_menu_input_armed = false;
				release_menu_cursor();
				schedule_gameplay_input_recovery(12u, "front-end to gameplay transition");
			}
		}
		else if (!gameplay_active)
		{
			m_gameplay_present_was_active = false;
		}

		if (!m_menu_active)
		{
			m_menu_input_armed = false;
			maintain_menu_cursor_ownership();
			if (apply_recovery_frame) recover_gameplay_input_frame();
		}
	}


	void imgui::suspend_for_loading_screen()
	{
		++m_loading_passthrough_frames;
		m_loading_screen_suspended = true;
		// Loading/menu transitions suspend only map-dependent systems. The F5 overlay,
		// its visible state and its input ownership survive until the user closes it.
	}

	bool imgui::input_message(const UINT message_type, const WPARAM wparam, const LPARAM lparam)
	{
		if (message_type == WM_KEYUP && wparam == VK_F5)
		{
			// Use physical RMB state. ImGui's cached MouseDown can remain stale after a
			// device reset and must not permanently block the F5 toggle.
			const bool physical_rmb_down = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
			if (!physical_rmb_down)
			{
				const bool opening = !m_menu_active;
				m_menu_active = opening;
				m_im_allow_game_input = false;
				if (opening)
				{
					++m_menu_open_requests;
					m_menu_input_armed = false;
					m_menu_just_opened = true;
					m_loading_screen_suspended = false;
					// Do not steal input yet. The Present hook arms ownership only after
					// one successful ImGui frame has actually been submitted.
				}
				else
				{
					++m_menu_close_requests;
					m_menu_input_armed = false;
					m_menu_just_opened = false;
					if (const auto* intf = interfaces::get(); intf && intf->m_surface && intf->m_surface->is_cursor_visible())
						center_cursor("menu close (suppressed)");
					apply_cursor_visibility(false, "menu close");
				}
			}
			else
			{
				ImGui_ImplWin32_WndProcHandler(glob::main_window, message_type, wparam, lparam);
			}
		}

		if (m_menu_active)
		{
			ImGui_ImplWin32_WndProcHandler(glob::main_window, message_type, wparam, lparam);
			const bool physical_rmb_down = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
			const bool camera_modifier_down = (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;

			// Camera passthrough is intentionally explicit: Left Alt + RMB outside the
			// window. It is unavailable until the visible menu frame has armed input.
			if (m_menu_input_armed && !m_im_window_hovered && physical_rmb_down && camera_modifier_down)
			{
				// center cursor and only call set_cursor_always_visible once
				if (!m_im_allow_game_input)
				{
					center_cursor("RMB camera capture (suppressed)");
					apply_cursor_visibility(false, "RMB game-input capture");
				}

				ImGui::SetWindowFocus(); // unfocus input text
				m_im_allow_game_input = true;
				return false;
			}

			// ^ wait until mouse is up and call set_cursor_always_visible once
			if (m_im_allow_game_input && (!physical_rmb_down || !camera_modifier_down))
			{
				m_im_allow_game_input = false;
				apply_cursor_visibility(true, "RMB game-input release");
				return false;
			}
		}
		else {
			m_im_allow_game_input = false; // always reset if there is no imgui window open
		}

		return menu_owns_input();
	}

	// ------

	bool imgui::cvar_toggle_button_bool(const char* cvar_str, const char* btn_text, ImVec2 btn_size, const char* tt_text, bool invert)
	{
		bool return_val = false;

		if (const auto& var = game::find_cvar_const(cvar_str); var)
		{
			const bool cvar_enabled = var->m_Value.m_nValue != 0;
			const bool color_active = invert ? !cvar_enabled : cvar_enabled;
			if (color_active)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
			}

			if (ImGui::Button(btn_text, btn_size))
			{
				interfaces::get()->m_engine->execute_client_cmd_unrestricted(utils::va("sv_cheats 1; %s %s", cvar_str, var->m_Value.m_nValue ? "0" : "1"));
				return_val = true;
			}

			if (tt_text) {
				TT(tt_text);
			}

			if (color_active) {
				ImGui::SafePopStyleColor(3, __LINE__);
			}
		}
		else {
			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "BAD CVAR");
			ImGui::PopFont();
		}

		return return_val;
	}

	bool imgui::toggle_button_bool(bool* bool_ptr, const char* btn_text, ImVec2 btn_size, const char* tt_text, bool invert)
	{
		bool return_val = false;

		if (bool_ptr)
		{
			const bool color_active = invert ? !*bool_ptr : *bool_ptr;
			if (color_active)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
			}

			if (ImGui::Button(btn_text, btn_size))
			{
				*bool_ptr = !*bool_ptr;
				return_val = true;
			}

			if (tt_text) {
				TT(tt_text);
			}

			if (color_active) {
				ImGui::SafePopStyleColor(3, __LINE__);
			}
		}
		else {
			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "BAD CVAR");
			ImGui::PopFont();
		}

		return return_val;
	}

	bool imgui::cvar_toggle_button_int(const char* cvar_str, const char* btn_text, ImVec2 btn_size, const char* tt_text, int off_override, int on_override)
	{
		bool return_val = false;

		if (const auto& var = game::find_cvar_const(cvar_str); var)
		{
			bool styled = false;
			if (var->m_Value.m_nValue)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
				styled = true;
			}

			if (ImGui::Button(btn_text, btn_size))
			{
				int toggle_val = var->m_Value.m_nValue
								 ? off_override ? off_override : 0
								 : on_override  ? on_override  : 1;

				interfaces::get()->m_engine->execute_client_cmd_unrestricted(utils::va("sv_cheats 1; %s %d", cvar_str, toggle_val));
				return_val = true;
			}

			if (tt_text) {
				TT(tt_text);
			}

			if (styled) {
				ImGui::SafePopStyleColor(3, __LINE__);
			}
		}
		else {
			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "BAD CVAR");
			ImGui::PopFont();
		}

		return return_val;
	}

	// ------

	bool reload_mapsettings_popup()
	{
		bool result = false;
		if (ImGui::BeginPopupModal("Reload MapSettings?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			common::imgui::draw_background_blur();
			const auto half_width = ImGui::GetContentRegionMax().x * 0.5f;
			auto line1_str = "You'll loose all unsaved changes if you continue!";
			auto line2_str = "Use the copy to clipboard buttons and manually update  ";
			auto line3_str = "the map_settings.toml file if you've made changes.";

			ImGui::Spacing();
			ImGui::SetCursorPosX(5.0f + half_width - (ImGui::CalcTextSize(line1_str).x * 0.5f));
			ImGui::TextUnformatted(line1_str);

			ImGui::Spacing();
			ImGui::SetCursorPosX(5.0f + half_width - (ImGui::CalcTextSize(line2_str).x * 0.5f));
			ImGui::TextUnformatted(line2_str);
			ImGui::SetCursorPosX(5.0f + half_width - (ImGui::CalcTextSize(line3_str).x * 0.5f));
			ImGui::TextUnformatted(line3_str);

			ImGui::Spacing(0, 8);
			ImGui::Spacing(0, 0); ImGui::SameLine();

			ImVec2 button_size(half_width - 6.0f - ImGui::GetStyle().WindowPadding.x, 0.0f);
			if (ImGui::Button("Reload", button_size))
			{
				result = true;
				imgui::get()->m_light_edit_mode = false;
				map_settings::reload();
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine(0, 6);
			if (ImGui::Button("Cancel", button_size)) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		return result;
	}

	bool reload_mapsettings_button_with_popup(const char* ID)
	{
		ImGui::PushFont(common::imgui::font::BOLD);
		if (ImGui::Button(utils::va("Reload MapSettings  %s##%s", ICON_FA_REDO, ID), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
		{
			if (!ImGui::IsPopupOpen("Reload MapSettings?")) {
				ImGui::OpenPopup("Reload MapSettings?");
			}
		}
		ImGui::PopFont();

		return reload_mapsettings_popup();
	}


	namespace
	{
		struct light_preset_s
		{
			const char* name;
			const char* tooltip;
			Vector radiance;
			float scalar;
			float radius;
			float volumetric_scale;
			bool use_shaping;
			float degrees;
			float softness;
			float exponent;
			const char* ies_profile;
			float ies_strength;
			float ies_focus;
		};

		static const light_preset_s LIGHT_WORKBENCH_PRESETS[] =
		{
			{ "Fluorescent", "Cold interior tube light with pseudo-IES wide downwash.", { 0.76f, 0.88f, 1.00f }, 1.00f, 2.60f, 0.35f, true, 128.0f, 0.52f, 0.08f, "fluorescent_tube", 1.00f, 1.00f },
			{ "Warm Bulb", "Small warm omni bulb / room light.", { 1.00f, 0.72f, 0.42f }, 1.10f, 2.10f, 0.25f, false, 180.0f, 0.00f, 0.00f, "bulb_a19", 1.00f, 1.00f },
			{ "Downlight", "Ceiling recessed light: tight downward cone.", { 1.00f, 0.82f, 0.62f }, 1.05f, 2.80f, 0.22f, true, 64.0f, 0.22f, 0.45f, "recessed_downlight", 1.00f, 1.00f },
			{ "Exit Sign", "Green exit sign / wall-wash helper.", { 0.42f, 1.00f, 0.38f }, 0.55f, 1.35f, 0.08f, true, 116.0f, 0.62f, 0.05f, "exit_sign_wallwash", 1.00f, 1.00f },
			{ "TV Panel", "Soft blue panel glow for TV/monitor screens.", { 0.48f, 0.68f, 1.00f }, 0.70f, 2.20f, 0.10f, true, 135.0f, 0.70f, 0.04f, "tv_panel", 1.00f, 1.00f },
			{ "Fire Barrel", "Orange fire source with pulse-friendly radius.", { 1.00f, 0.34f, 0.08f }, 1.20f, 3.20f, 1.10f, false, 180.0f, 0.00f, 0.00f, "fire_emitter", 1.00f, 1.00f },
			{ "Alarm Red", "Small red emergency/alarm beacon base.", { 1.00f, 0.08f, 0.03f }, 1.15f, 2.00f, 0.45f, true, 42.0f, 0.25f, 0.50f, "spot_25", 0.85f, 0.70f },
			{ "Street Cutoff", "Warm street lamp with cutoff-style pseudo-IES cone.", { 1.00f, 0.78f, 0.48f }, 1.10f, 4.50f, 0.35f, true, 92.0f, 0.18f, 0.72f, "street_lamp_cutoff", 1.00f, 1.00f },
			{ "View Spot", "Cone light aimed along the current camera direction.", { 1.00f, 0.95f, 0.85f }, 1.00f, 3.00f, 0.25f, true, 58.0f, 0.35f, 0.65f, "spot_25", 0.80f, 0.90f },
			{ "Headlight", "Narrow vehicle/searchlight cone aimed from view.", { 1.00f, 0.94f, 0.76f }, 1.25f, 5.00f, 0.25f, true, 36.0f, 0.20f, 0.80f, "spot_25", 1.10f, 1.20f },
		};

		struct numeric_light_preset_s
		{
			const char* name;
			float value;
			const char* tooltip;
		};

		static const numeric_light_preset_s LIGHT_BRIGHTNESS_PRESETS[] =
		{
			{ "Very Dim", 0.20f, "Accent/indicator light." },
			{ "Dim", 0.50f, "Subtle practical light." },
			{ "Normal", 1.00f, "Neutral authored intensity." },
			{ "Strong", 2.00f, "Strong room fixture." },
			{ "Hero", 4.00f, "Dominant authored fixture." },
			{ "Extreme", 8.00f, "Large exterior/search light." },
		};

		static const numeric_light_preset_s LIGHT_RADIUS_PRESETS[] =
		{
			{ "Tiny", 0.35f, "Indicator/candle-sized emitter." },
			{ "Small", 1.00f, "Small bulb or prop fixture." },
			{ "Room", 2.50f, "Typical interior fixture." },
			{ "Large", 5.00f, "Large room/street fixture." },
			{ "Street", 8.00f, "Streetlight-scale source." },
			{ "Exterior", 16.00f, "Large exterior source." },
		};

		const char* light_rig_mode_ui_name(const int mode)
		{
			switch (mode)
			{
			case map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES: return "Native IES";
			case map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES: return "Fake IES";
			case map_settings::remix_light_settings_s::LIGHT_RIG_MODE_LEGACY:
			default: return "Analytical";
			}
		}

		int authored_light_shape(const map_settings::remix_light_settings_s& light)
		{
			if (!light.points.empty())
			{
				const auto& point = light.points.front();
				if (point.authoring_shape != map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_AUTO)
					return point.authoring_shape;
				if (point.use_shaping)
					return map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT;
			}

			const std::string source = utils::str_to_lower(light.generated_source_kind + " " +
				light.generated_runtime_kind + " " + light.generated_classname + " " + light.comment);
			if (source.contains("distant") || source.contains("sun") ||
				source.contains("skylight") || source.contains("skyambient") ||
				source.contains("light_environment"))
				return map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT;
			return map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT;
		}

		bool is_direct_sun_light(const map_settings::remix_light_settings_s& light)
		{
			return authored_light_shape(light) ==
				map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT;
		}

		const char* light_type_icon(const map_settings::remix_light_settings_s& light)
		{
			switch (authored_light_shape(light))
			{
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT: return ICON_FA_SUN;
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT: return ICON_FA_FLASHLIGHT;
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK: return ICON_FA_CIRCLE;
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT: return ICON_FA_SQUARE;
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE: return ICON_FA_GRIP_LINES;
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT:
			default: return ICON_FA_LIGHTBULB;
			}
		}

		const char* light_type_name(const map_settings::remix_light_settings_s& light)
		{
			switch (authored_light_shape(light))
			{
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT: return "Direct / Sun";
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT: return "Spot";
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK: return "Disk";
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT: return "Rect";
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE: return "Tube";
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT:
			default: return "Point";
			}
		}

		const char* light_origin_badge(const map_settings::remix_light_settings_s& light)
		{
			if (is_direct_sun_light(light)) return "SUN";
			if (light.generated_map_light) return "IMPORTED";
			if (light.generated_source_transient) return "SOURCE";
			if (light.persistent_map_light_from_file) return "MAP DB";
			return "AUTHORED";
		}

		Vector direction_to_light_rotation(const Vector& authored_direction)
		{
			Vector direction = authored_direction;
			if (direction.LengthSqr() <= 0.000001f) direction = Vector(0.0f, 0.0f, -1.0f);
			direction.NormalizeChecked();
			const float horizontal = std::sqrt(direction.x * direction.x + direction.y * direction.y);
			return Vector(
				RAD2DEG(std::atan2(-direction.z, horizontal)),
				RAD2DEG(std::atan2(direction.y, direction.x)),
				0.0f);
		}

		void ensure_game_root_path();

		std::filesystem::path native_ies_folder_path()
		{
			ensure_game_root_path();
			return std::filesystem::path(game::root_path) / "rtx-remix" / "ies";
		}

		std::vector<std::string> scan_native_ies_profiles()
		{
			std::vector<std::string> files;
			const auto folder = native_ies_folder_path();
			std::error_code ec;
			std::filesystem::create_directories(folder, ec);
			ec.clear();
			for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
			{
				if (ec) break;
				if (!entry.is_regular_file(ec)) continue;
				auto ext = utils::str_to_lower(entry.path().extension().string());
				if (ext == ".ies") files.push_back(entry.path().filename().string());
			}
			std::sort(files.begin(), files.end());
			return files;
		}


		void ensure_game_root_path()
		{
			if (game::root_path.empty())
			{
				char path[MAX_PATH];
				GetModuleFileNameA(nullptr, path, MAX_PATH);
				game::root_path = path;
				utils::erase_substring(game::root_path, "left4dead2.exe");
			}
		}

		std::string mapsettings_workbench_export_path()
		{
			ensure_game_root_path();
			return game::root_path + COMPMOD_ASSET_DIR "logs\\mapsettings_workbench_export.toml";
		}

		bool append_to_mapsettings_workbench_export(const std::string& category, const std::string& label, const std::string& hint, const std::string& toml_text)
		{
			const auto path = mapsettings_workbench_export_path();
			std::filesystem::create_directories(std::filesystem::path(path).parent_path());

			std::ofstream file(path, std::ios::out | std::ios::app);
			if (!file.is_open())
			{
				game::console();
				std::cout << "[MapSettingsWorkbench] Failed to open export file: " << path << std::endl;
				return false;
			}

			file << "\n\n# -----------------------------------------------------------------------------\n";
			file << "# " << category << " Workbench export: " << label << "\n";
			file << "# Map: " << map_settings::get_map_settings().mapname << "\n";
			if (!hint.empty()) {
				file << "# " << hint << "\n";
			}
			file << toml_text << "\n";
			return true;
		}

		bool append_to_light_workbench_export(const std::string& label, const std::string& toml_text)
		{
			return append_to_mapsettings_workbench_export("Light", label, "Paste into [remix_lights] for this map in map_settings.toml.", toml_text);
		}

		map_settings::remix_light_settings_s build_current_edit_light_def(remix_lights::light* edit_light)
		{
			// The editor source of truth is m_def.points. m_mover is only a runtime copy.
			return edit_light ? edit_light->m_def : map_settings::remix_light_settings_s{};
		}

		void copy_text_to_clipboard(const std::string& text)
		{
			ImGui::LogToClipboard();
			ImGui::LogText("%s", text.c_str());
			ImGui::LogFinish();
		}


		std::string ui_to_lower(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
		}

		bool ui_filter_contains(const std::string& haystack, const std::string& needle)
		{
			if (needle.empty()) {
				return true;
			}

			return ui_to_lower(haystack).find(ui_to_lower(needle)) != std::string::npos;
		}

		void ui_status_pill(const char* text, const bool active, const char* tooltip = nullptr)
		{
			const ImVec4 active_col = ImVec4(0.42f, 0.86f, 0.48f, 1.00f);
			const ImVec4 inactive_col = ImVec4(0.72f, 0.72f, 0.72f, 0.72f);
			ImGui::PushStyleColor(ImGuiCol_Text, active ? active_col : inactive_col);
			ImGui::TextUnformatted(text);
			ImGui::SafePopStyleColor(1, __LINE__);
			if (tooltip) {
				TT(tooltip);
			}
		}

		void ui_subsection(const char* title, const char* hint = nullptr, const char* icon = nullptr)
		{
			ImGui::Spacing(0, 8);
			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			if (icon && *icon) {
				ImGui::Text("%s  %s", icon, title);
			}
			else {
				ImGui::TextUnformatted(title);
			}
			ImGui::PopFont();
			if (hint && *hint) {
				ImGui::TextWrapped("%s", hint);
			}
			ImGui::Separator();
			ImGui::Spacing(0, 3);
		}

		void ui_metric_card(const char* label, const std::string& value, const char* hint = nullptr)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 7.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.11f, 0.12f, 0.68f));
			ImGui::BeginChild(label, ImVec2(0.0f, 54.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::TextDisabled("%s", label);
			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			ImGui::TextUnformatted(value.c_str());
			ImGui::PopFont();
			ImGui::EndChild();
			ImGui::SafePopStyleColor(1, __LINE__);
			ImGui::PopStyleVar(2);
			if (hint) {
				TT(hint);
			}
		}

		void ui_toggle_row(const char* left_label, bool* left_value, const char* left_tt, const char* right_label, bool* right_value, const char* right_tt)
		{
			const auto button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f, 0.0f);
			imgui::toggle_button_bool(left_value, left_label, button_size, left_tt);
			ImGui::SameLine();
			imgui::toggle_button_bool(right_value, right_label, button_size, right_tt);
		}


		bool ui_advanced_mode()
		{
			return game_settings::get()->ui_advanced_mode.get_as<bool>();
		}

		bool ui_compact_descriptions()
		{
			return game_settings::get()->ui_compact_descriptions.get_as<bool>();
		}

		int ui_responsive_column_count(const float minimum_column_width, const int maximum_columns)
		{
			const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			const int fit = static_cast<int>((available + spacing) / std::max(1.0f, minimum_column_width + spacing));
			return std::clamp(fit, 1, std::max(1, maximum_columns));
		}

		void ui_basic_mode_note(const char* advanced_summary)
		{
			if (ui_advanced_mode()) return;
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("Basic interface: %s Advanced controls are hidden; enable Advanced UI in the footer when needed.", advanced_summary);
			ImGui::SafePopStyleColor(1, __LINE__);
		}


		void ui_warning_banner(const char* title, const char* body, const ImVec4& accent = ImVec4(1.0f, 0.64f, 0.20f, 1.0f))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 9.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(11.0f, 9.0f));
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.11f, 0.07f, 0.78f));
			ImGui::BeginChild(title, ImVec2(0.0f, 78.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			ImGui::PushStyleColor(ImGuiCol_Text, accent);
			ImGui::PushFont(common::imgui::font::BOLD);
			ImGui::TextUnformatted(title);
			ImGui::PopFont();
			ImGui::SafePopStyleColor(1, __LINE__);
			ImGui::TextWrapped("%s", body);
			ImGui::EndChild();
			ImGui::SafePopStyleColor(1, __LINE__);
			ImGui::PopStyleVar(2);
		}

		const char* source_threading_label(const int mode)
		{
			switch (mode)
			{
			case 1: return "Stable Hybrid";
			case 2: return "Queue Guard";
			case 3: return "Queue Performance";
			case 4: return "Stress / Unsafe";
			default: return "Safe Single";
			}
		}

		const char* source_threading_risk_label(const int mode, const int mitigation)
		{
			if (mode == 0) { return "very low"; }
			if (mode == 1) { return "low"; }
			if (mode == 2 && mitigation >= 2) { return "medium-low"; }
			if (mode == 2) { return "medium"; }
			if (mode == 3 && mitigation >= 1) { return "medium-high"; }
			return "high";
		}

		int source_threading_mat_queue_value(const int mode)
		{
			return mode >= 3 ? 2 : mode == 2 ? -1 : 0;
		}

		void apply_source_threading_ui_preset(const int mode, const bool particles, const bool details, const bool ropes, const bool frame_sync, const int mitigation, const bool quarantine)
		{
			auto gs = game_settings::get();
			gs->source_threading_mode.set_var(mode, true);
			gs->source_threaded_particles.set_var(particles, true);
			gs->source_threaded_detailprops.set_var(details, true);
			gs->source_queued_ropes.set_var(ropes, true);
			gs->source_queue_frame_sync_guard.set_var(frame_sync, true);
			gs->source_queue_flicker_mitigation.set_var(mitigation, true);
			gs->source_queue_geometry_quarantine.set_var(quarantine, true);
		}

		void draw_source_multicore_panel()
		{
			auto gs = game_settings::get();
			auto mode = gs->source_threading_mode.get_as<int*>();
			auto mitigation = gs->source_queue_flicker_mitigation.get_as<int*>();
			*mode = std::clamp(*mode, 0, 4);
			*mitigation = std::clamp(*mitigation, 0, 2);

			ImGui::Spacing(0.0f, 8.0f);
			ui_subsection("Experimental Source Multicore", "Profiles for testing the queued Source renderer. Safe/Hybrid should be stable; queued profiles can give a large CPU-side gain but may cause Remix capture flicker.", ICON_FA_MICROCHIP);

			if (ImGui::BeginTable("##source_threading_cards", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); ui_metric_card("Profile", source_threading_label(*mode), "Current threading profile.");
				ImGui::TableNextColumn(); ui_metric_card("mat_queue", std::format("{}", source_threading_mat_queue_value(*mode)), "Resolved mat_queue_mode value.");
				ImGui::TableNextColumn(); ui_metric_card("Flicker risk", source_threading_risk_label(*mode, *mitigation), "Approximate risk for one-frame missing geometry in Remix capture.");
				ImGui::TableNextColumn(); ui_metric_card("Goal", *mode >= 2 ? "+FPS test" : "stable", "Queued modes are where the extra performance usually comes from.");
				ImGui::EndTable();
			}

			const auto preset_button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 4.0f) / 5.0f, 0.0f);
			if (ImGui::Button("Safe", preset_button_size)) { apply_source_threading_ui_preset(0, false, false, false, true, 1, true); }
			TT("Compatibility baseline. mat_queue_mode 0, no extra threaded render paths.");
			ImGui::SameLine();
			if (ImGui::Button("Hybrid", preset_button_size)) { apply_source_threading_ui_preset(1, true, false, false, true, 1, true); }
			TT("Keeps mat_queue_mode 0, but allows low-risk worker cvars. Use this before queued renderer tests.");
			ImGui::SameLine();
			if (ImGui::Button("Queue Guard", preset_button_size)) { apply_source_threading_ui_preset(2, false, false, false, true, 2, true); }
			TT("mat_queue_mode -1 with strict flicker mitigation. First queued profile to test on a map.");
			ImGui::SameLine();
			if (ImGui::Button("Queue Perf", preset_button_size)) { apply_source_threading_ui_preset(3, true, false, false, true, 1, true); }
			TT("Forced queued renderer with balanced guards. Candidate for the +FPS path if it does not flicker on this map.");
			ImGui::SameLine();
			if (ImGui::Button("Stress", preset_button_size)) { apply_source_threading_ui_preset(4, true, true, true, false, 0, false); }
			TT("Unsafe A/B test only. Maximum queueing, few guards, highest flicker risk.");

			if (ImGui::Button("Apply threading cvars now", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
			{
				main_module::cross_handle_map_and_game_settings();
			}
			TT("Applies the selected Source threading profile immediately without waiting for a map reload.");

			static const char* source_threading_modes[] =
			{
				"0 - Safe forced single-thread",
				"1 - Stable hybrid, no queued renderer",
				"2 - Queue Guard, Source auto queue",
				"3 - Queue Performance, forced queued renderer",
				"4 - Stress / unsafe queued renderer",
			};
			static const char* flicker_mitigation_modes[] =
			{
				"0 - Off / fastest",
				"1 - Balanced frame sync",
				"2 - Strict sync + quarantine",
			};

			ImGui::Spacing(0, 6.0f);
			if (ImGui::BeginTable("##source_threading_controls", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn();
				SET_CHILD_WIDGET_WIDTH_MAN(220.0f);
				ImGui::Combo("Threading Profile", mode, source_threading_modes, IM_ARRAYSIZE(source_threading_modes));
				TT(gs->source_threading_mode.get_tooltip_string().c_str());
				SET_CHILD_WIDGET_WIDTH_MAN(220.0f);
				ImGui::Combo("Flicker Mitigation", mitigation, flicker_mitigation_modes, IM_ARRAYSIZE(flicker_mitigation_modes));
				TT(gs->source_queue_flicker_mitigation.get_tooltip_string().c_str());

				ImGui::TableNextColumn();
				ImGui::Checkbox("Frame Sync Guard", gs->source_queue_frame_sync_guard.get_as<bool*>());
				TT(gs->source_queue_frame_sync_guard.get_tooltip_string().c_str());
				ImGui::Checkbox("Geometry Quarantine", gs->source_queue_geometry_quarantine.get_as<bool*>());
				TT(gs->source_queue_geometry_quarantine.get_tooltip_string().c_str());
				ImGui::Checkbox("Debug Plan", gs->source_queue_debug_overlay.get_as<bool*>());
				TT(gs->source_queue_debug_overlay.get_tooltip_string().c_str());
				ImGui::EndTable();
			}

			const bool experimental_source_threading = *mode != 0;
			ImGui::BeginDisabled(!experimental_source_threading);
			ui_toggle_row("Threaded Particles", gs->source_threaded_particles.get_as<bool*>(), gs->source_threaded_particles.get_tooltip_string().c_str(),
				"Threaded Detail Props", gs->source_threaded_detailprops.get_as<bool*>(), gs->source_threaded_detailprops.get_tooltip_string().c_str());
			ImGui::Checkbox("Queued Ropes", gs->source_queued_ropes.get_as<bool*>());
			TT(gs->source_queued_ropes.get_tooltip_string().c_str());
			ImGui::EndDisabled();

			if (*mode >= 2)
			{
				ui_warning_banner("Queued renderer warning", "Queued modes are the only realistic place to chase the ~30% gain, but Remix can capture a partially updated frame. Test per-map: Safe -> Queue Guard -> Queue Perf. If objects disappear for one frame, raise mitigation or go back one profile.");
			}
			else if (*mode == 1)
			{
				ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.65f, 1.0f), "Hybrid mode: mat_queue_mode stays 0. This should avoid queued-renderer geometry dropouts.");
			}

			if (gs->source_queue_debug_overlay.get_as<bool>())
			{
				const int mat_queue = source_threading_mat_queue_value(*mode);
				const bool queued = *mode >= 2;
				const bool strict = queued && *mitigation >= 2;
				const bool particles = experimental_source_threading && gs->source_threaded_particles.get_as<bool>() && !strict;
				const bool details = experimental_source_threading && gs->source_threaded_detailprops.get_as<bool>() && !(queued && gs->source_queue_geometry_quarantine.get_as<bool>());
				const bool ropes = experimental_source_threading && gs->source_queued_ropes.get_as<bool>() && !(queued && gs->source_queue_geometry_quarantine.get_as<bool>());
				const bool frame_sync = queued && (gs->source_queue_frame_sync_guard.get_as<bool>() || *mitigation >= 1);

				ImGui::SeparatorText("Resolved cvar plan");
				ImGui::Text("mat_queue_mode = %d", mat_queue);
				ImGui::Text("mat_frame_sync_enable = %d", frame_sync ? 1 : 0);
				ImGui::Text("mat_forcehardwaresync = %d", strict ? 1 : 0);
				ImGui::Text("r_threaded_particles = %d", particles ? 1 : 0);
				ImGui::Text("r_threadeddetailprops = %d", details ? 1 : 0);
				ImGui::Text("r_queued_ropes = %d", ropes ? 1 : 0);
				if (ImGui::Button("Copy cvar test block"))
				{
					copy_text_to_clipboard(std::format("mat_queue_mode {}\nmat_frame_sync_enable {}\nmat_forcehardwaresync {}\nr_threaded_particles {}\nr_threadeddetailprops {}\nr_queued_ropes {}",
						mat_queue, frame_sync ? 1 : 0, strict ? 1 : 0, particles ? 1 : 0, details ? 1 : 0, ropes ? 1 : 0));
				}
			}
		}

		void draw_mapsettings_workspace_header()
		{
			const auto& ms = map_settings::get_map_settings();
			const auto lights = remix_lights::get();
			unsigned enabled_lights = 0u;
			unsigned grouped_lights = 0u;
			unsigned legacy_rigs = 0u;
			unsigned native_ies_rigs = 0u;
			unsigned fake_ies_rigs = 0u;
			unsigned pseudo_ies_authored_helpers = 0u;

			for (const auto& light : ms.remix_lights)
			{
				if (light.enabled) {
					++enabled_lights;
				}
				if (!light.group.empty()) {
					++grouped_lights;
				}

				if (!light.points.empty())
				{
					const auto mode = light.points.front().light_rig_mode;
					if (mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES) ++native_ies_rigs;
					else if (mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES) ++fake_ies_rigs;
					else ++legacy_rigs;
				}

				for (const auto& point : light.points)
				{
					if (point.light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES && point.ies_emulation && point.ies_emulation_samples > 0)
					{
						pseudo_ies_authored_helpers += static_cast<unsigned>(std::clamp(point.ies_emulation_samples, 0, 24));
					}
				}
			}

			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 9.0f));
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.10f, 0.11f, 0.74f));
			ImGui::BeginChild("##mapsettings_workspace_header", ImVec2(0.0f, 132.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			ImGui::TextUnformatted("Map Settings Workspace");
			ImGui::PopFont();
			ImGui::SameLine();
			ui_status_pill(imgui::get()->m_light_edit_mode ? "EDIT MODE ON" : "EDIT MODE OFF", imgui::get()->m_light_edit_mode,
				"Entering edit mode now performs the required save/reload automatically without a confirmation dialog.");
			ImGui::TextWrapped("Map-level overview. Full light creation and editing now lives in the main Light Studio tab. Export remains manual and safe.");

			if (ImGui::BeginTable("##mapsettings_workspace_cards", 5, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); ui_metric_card("Lights", std::format("{} / {} on", enabled_lights, static_cast<unsigned>(ms.remix_lights.size())), "Enabled authored remix lights in this map.");
				ImGui::TableNextColumn(); ui_metric_card("Groups", std::format("{} grouped", grouped_lights), "Lights with a group name, useful for filtering/debugging large maps.");
				ImGui::TableNextColumn(); ui_metric_card("Rig Modes", std::format("L{} / I{} / F{}", legacy_rigs, native_ies_rigs, fake_ies_rigs), "Legacy / native IES / fake IES authored controller lights.");
				ImGui::TableNextColumn(); ui_metric_card("Fake Helpers", std::format("{} authored / {} live", pseudo_ies_authored_helpers, lights ? lights->get_ies_cluster_child_count() : 0u), "Helper-light demand and current runtime count for Fake IES rigs only.");
				ImGui::TableNextColumn(); ui_metric_card("Muzzle", dynamic_lighting::m_auto_muzzle_flash ? "auto on" : "manual/off", "Separated strict muzzle flash subsystem.");
				ImGui::EndTable();
			}

			ImGui::EndChild();
			ImGui::SafePopStyleColor(1, __LINE__);
			ImGui::PopStyleVar(2);
		}


		struct light_audit_issue_s
		{
			std::string severity;
			std::string target;
			std::string message;
		};

		std::vector<light_audit_issue_s> run_light_authoring_audit()
		{
			std::vector<light_audit_issue_s> issues;
			const auto& ms = map_settings::get_map_settings();
			std::unordered_map<std::string, int> comments;

			auto add_issue = [&](std::string severity, std::string target, std::string message)
			{
				issues.push_back({ std::move(severity), std::move(target), std::move(message) });
			};

			const int helper_budget = std::clamp(remix_lights::ies_cluster_global_budget(), 0, 512);
			int authored_helpers = 0;

			for (size_t i = 0u; i < ms.remix_lights.size(); ++i)
			{
				const auto& light = ms.remix_lights[i];
				const std::string target = std::format("light #{}{}{}",
					i,
					light.group.empty() ? "" : std::format(" [{}]", light.group),
					light.comment.empty() ? "" : std::format(" - {}", light.comment));

				if (!light.enabled) {
					add_issue("info", target, "Light is disabled. It will stay in TOML/editor but will not spawn at runtime.");
				}
				if (light.group.empty()) {
					add_issue("info", target, "No group assigned. Grouping helps filter/debug large maps.");
				}
				if (!light.comment.empty()) {
					++comments[light.comment];
				}
				if (light.points.empty()) {
					add_issue("error", target, "Light has no points, so it cannot create a Remix light.");
					continue;
				}
				if ((!light.animation.empty() && light.animation != "stable" && light.animation != "none" && light.animation != "off") && light.points.size() <= 1u) {
					add_issue("warning", target, "Animation metadata is set, but this light only has one point. Use Apply Live or re-expand the animation.");
				}
				if (light.loop && light.points.size() <= 1u) {
					add_issue("warning", target, "Loop is enabled on a single-point light. It has nothing to interpolate.");
				}
				if (light.trigger_sound_hash && light.trigger_choreo_name.size()) {
					add_issue("warning", target, "Both sound and choreo spawn triggers are set. Verify that this is intentional.");
				}

				for (size_t p = 0u; p < light.points.size(); ++p)
				{
					const auto& point = light.points[p];
					const auto point_target = std::format("{} / point #{}", target, p);
					if (point.radius <= 0.001f) {
						add_issue("error", point_target, "Radius is zero or negative. The light will be invisible.");
					}
					if (point.radiance_scalar <= 0.0f) {
						add_issue("warning", point_target, "Radiance scalar is zero or negative. The light can be effectively black.");
					}
					if (point.radiance.LengthSqr() <= 0.0001f) {
						add_issue("warning", point_target, "Radiance color is almost black.");
					}
					if (point.use_shaping && point.direction.LengthSqr() <= 0.0001f) {
						add_issue("warning", point_target, "Spot/disc shaping is enabled, but direction vector is invalid.");
					}
					if (point.light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES)
					{
						if (point.ies_file.empty()) add_issue("error", point_target, "Native IES mode requires ies_file. This light is blocked instead of silently falling back.");
						if (point.ies_angle_scale <= 0.0f) add_issue("error", point_target, "Native IES angle scale must be greater than zero.");
						if (point.ies_intensity_scale < 0.0f) add_issue("error", point_target, "Native IES intensity scale cannot be negative.");
					}
					else if (!point.ies_file.empty())
					{
						add_issue("info", point_target, "ies_file is stored but ignored because this light is not in Native IES mode.");
					}
					if (point.light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES && point.ies_emulation)
					{
						if (point.ies_emulation_samples <= 0) {
							add_issue("warning", point_target, "Fake IES rig is enabled, but helper count is 0.");
						}
						if (point.ies_emulation_intensity_scale <= 0.0f) {
							add_issue("warning", point_target, "Fake IES helper power is 0. Helpers will spawn but contribute no light.");
						}
						authored_helpers += std::clamp(point.ies_emulation_samples, 0, 24);
					}
				}
			}

			for (const auto& [comment, count] : comments)
			{
				if (count > 1) {
					add_issue("info", "comments", std::format("Comment '{}' is used by {} lights. This is allowed, but unique comments make debugging easier.", comment, count));
				}
			}

			if (authored_helpers > helper_budget) {
				add_issue("warning", "Fake IES budget", std::format("Authored Fake IES helpers request {} lights, but runtime budget is {}. Some helpers will be skipped.", authored_helpers, helper_budget));
			}

			return issues;
		}

		std::string build_light_audit_report(const std::vector<light_audit_issue_s>& issues)
		{
			std::string report = std::format("Light Audit for map '{}': {} issue(s)\n", map_settings::get_map_settings().mapname, issues.size());
			for (const auto& issue : issues)
			{
				report += std::format("[{}] {} - {}\n", issue.severity, issue.target, issue.message);
			}
			return report;
		}

		void draw_light_audit_and_runtime_debug_panel()
		{
			ui_subsection("Light Audit / Runtime Debug", "Find bad authored lights, watch runtime handles/helper budgets, and diagnose flicker or missing-light issues.", ICON_FA_STETHOSCOPE);

			auto* lights = remix_lights::get();
			const auto stats = lights ? lights->build_runtime_debug_stats() : remix_lights::runtime_debug_stats_s{};
			if (ImGui::BeginTable("##runtime_light_debug_cards", 5, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); ui_metric_card("Active", std::format("{}", stats.active_lights), "Active remix_lights objects in runtime memory.");
				ImGui::TableNextColumn(); ui_metric_card("Handles", std::format("{}", stats.spawned_handles), "Lights that currently have a Remix API handle.");
				ImGui::TableNextColumn(); ui_metric_card("Animated", std::format("{}", stats.animated_lights), "Runtime lights with timepoints/mover/animation metadata.");
				ImGui::TableNextColumn(); ui_metric_card("IES children", std::format("{}", stats.ies_child_handles), "Currently spawned Fake IES helper handles.");
				ImGui::TableNextColumn(); ui_metric_card("Failures", std::format("{}", stats.create_light_failures), "CreateLight failures seen since map load.");
				ImGui::EndTable();
			}

			if (ImGui::BeginTable("##runtime_light_debug_cards_2", 5, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); ui_metric_card("Pending", std::format("{}", stats.pending_trigger_lights), "Triggered/delayed lights that exist but are not spawned yet.");
				ImGui::TableNextColumn(); ui_metric_card("Attached", std::format("{}", stats.attached_lights), "Lights currently attached to a prop/entity/bone.");
				ImGui::TableNextColumn(); ui_metric_card("Destroying", std::format("{}", stats.marked_for_destroy), "Lights waiting for kill-delay destruction.");
				ImGui::TableNextColumn(); ui_metric_card("Filtered", std::format("{}", stats.runtime_group_filtered), "Runtime lights skipped by group filter.");
				ImGui::TableNextColumn(); ui_metric_card("Budget skip", std::format("{}", stats.ies_budget_skipped), "Fake IES helper requests skipped because the global budget was reached.");
				ImGui::EndTable();
			}

			static std::vector<light_audit_issue_s> audit_issues;
			static bool audit_ran = false;
			const auto button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f, 0.0f);
			if (ImGui::Button("Run Light Audit", button_size))
			{
				audit_issues = run_light_authoring_audit();
				audit_ran = true;
			}
			TT("Scans authored map lights for missing points, invalid radius, Fake IES budget problems, animation metadata without points, and Native IES configuration errors.");
			ImGui::SameLine();
			ImGui::BeginDisabled(!audit_ran);
			if (ImGui::Button("Copy Audit Report", button_size)) {
				copy_text_to_clipboard(build_light_audit_report(audit_issues));
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Clear Runtime Log", button_size)) {
				remix_lights::clear_debug_lifecycle_log();
			}

			ImGui::Checkbox("Lifecycle Log", &remix_lights::debug_lifecycle_log_enabled());
			TT("When enabled, records spawn/update/destroy/CreateLight failure events. Leave OFF during normal play because animated lights can update every frame.");

			if (audit_ran)
			{
				ImGui::SeparatorText("Audit Results");
				if (audit_issues.empty())
				{
					ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.65f, 1.0f), "No obvious authored-light issues found.");
				}
				else if (ImGui::BeginTable("##light_audit_results", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 220.0f)))
				{
					ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed, 82.0f);
					ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch, 0.35f);
					ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 0.65f);
					ImGui::TableHeadersRow();
					for (const auto& issue : audit_issues)
					{
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(issue.severity.c_str());
						ImGui::TableNextColumn(); ImGui::TextWrapped("%s", issue.target.c_str());
						ImGui::TableNextColumn(); ImGui::TextWrapped("%s", issue.message.c_str());
					}
					ImGui::EndTable();
				}
			}

			const auto& log = remix_lights::debug_lifecycle_log();
			ImGui::SeparatorText("Runtime Lifecycle Log");
			if (log.empty())
			{
				ImGui::TextDisabled("No runtime lifecycle events recorded. Enable Lifecycle Log before reproducing the problem.");
			}
			else
			{
				ImGui::BeginChild("##light_lifecycle_log", ImVec2(0.0f, 180.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
				for (const auto& line : log) {
					ImGui::TextUnformatted(line.c_str());
				}
				ImGui::EndChild();
			}
		}


		std::string toml_escape_inline_string(std::string value)
		{
			utils::replace_all(value, "\\", "\\\\");
			utils::replace_all(value, "\"", "\\\"");
			return value;
		}

		std::string build_choreo_trigger_snippet(const choreo_events::history_entry& ev)
		{
			std::string snippet = "trigger = { choreo = \"" + toml_escape_inline_string(ev.name) + "\"";
			if (!ev.actor.empty()) {
				snippet += ", actor = \"" + toml_escape_inline_string(ev.actor) + "\"";
			}
			if (!ev.event.empty()) {
				snippet += ", event = \"" + toml_escape_inline_string(ev.event) + "\"";
			}
			if (!ev.param1.empty()) {
				snippet += ", param1 = \"" + toml_escape_inline_string(ev.param1) + "\"";
			}
			snippet += " }";
			return snippet;
		}

		void apply_light_workbench_preset(map_settings::remix_light_settings_s::point_s& point, const light_preset_s& preset)
		{
			// Style presets are deliberately non-destructive for physical scale and output.
			// Brightness and radius have their own context menus in Light Studio.
			const float preserved_intensity = point.radiance_scalar;
			const float preserved_radius = point.radius;
			const int preserved_mode = point.light_rig_mode;

			point.radiance = preset.radiance;
			point.volumetric_scale = preset.volumetric_scale;
			point.use_shaping = preset.use_shaping;
			point.degrees = preset.degrees;
			point.softness = preset.softness;
			point.exponent = preset.exponent;
			point.ies_profile = preset.ies_profile ? preset.ies_profile : "";
			point.ies_strength = preset.ies_strength;
			point.ies_focus = preset.ies_focus;
			point.radiance_scalar = preserved_intensity;
			point.radius = preserved_radius;
			point.light_rig_mode = preserved_mode;

			if (preset.use_shaping)
			{
				point.direction = *game::get_current_view_forward();
				point.direction.NormalizeChecked();
			}
			else
			{
				point.direction = { 0.0f, 0.0f, 1.0f };
				point.angle_offset_attached = { 0.0f, 0.0f, 0.0f };
			}
		}


		static const char* IES_WORKBENCH_PROFILES[] =
		{
			"none",
			"omni_soft",
			"bulb_a19",
			"fluorescent_tube",
			"recessed_downlight",
			"spot_25",
			"street_lamp_cutoff",
			"exit_sign_wallwash",
			"tv_panel",
			"fire_emitter",
		};

		int ies_profile_index_for(const std::string& profile)
		{
			for (int i = 0; i < static_cast<int>(sizeof(IES_WORKBENCH_PROFILES) / sizeof(IES_WORKBENCH_PROFILES[0])); ++i)
			{
				if (profile == IES_WORKBENCH_PROFILES[i]) {
					return i;
				}
			}
			return 0;
		}

		static const char* IES_CLUSTER_PATTERNS[] =
		{
			"spiral",
			"ring",
			"line",
			"cross",
			"beam",
		};

		int ies_cluster_pattern_index_for(const std::string& pattern)
		{
			auto normalized = utils::str_to_lower(pattern.empty() ? "spiral" : pattern);
			utils::replace_all(normalized, "-", "_");
			for (int i = 0; i < static_cast<int>(sizeof(IES_CLUSTER_PATTERNS) / sizeof(IES_CLUSTER_PATTERNS[0])); ++i)
			{
				if (normalized == IES_CLUSTER_PATTERNS[i]) {
					return i;
				}
			}
			return 0;
		}

		static const char* ADD_LIGHT_ANIMATION_PRESETS[] =
		{
			"stable",
			"pulse_slow",
			"pulse_fast",
			"breathing",
			"fire_pulse",
			"candle_flicker",
			"soft_flicker",
			"unstable_bulb",
			"broken_fluorescent",
			"fluorescent_random",
			"generator_stutter",
			"tv_noise",
			"strobe_fast",
			"strobe_slow",
			"rotating_yaw",
			"disc_spin",
			"spot_axis_spin",
			"spot_axis_sweep",
			"searchlight_sweep",
			"pendulum_sweep",
		};

		// V21.14.5: output and movement are authored independently. Movement never
		// changes radiance; output animation never moves the fixture.
		static const char* LIGHT_OUTPUT_ANIMATION_PRESETS[] =
		{
			"stable", "pulse_slow", "pulse_fast", "breathing", "fire_pulse",
			"candle_flicker", "soft_flicker", "unstable_bulb", "broken_fluorescent",
			"fluorescent_random", "generator_stutter", "tv_noise", "strobe_fast", "strobe_slow",
		};

		static const char* LIGHT_MOVEMENT_ANIMATION_PRESETS[] =
		{
			"none", "rotate_yaw", "axis_spin", "sweep", "pendulum", "vertical_bob", "axis_patrol", "orbit",
		};

		void apply_workbench_ies_cluster_defaults(map_settings::remix_light_settings_s::point_s& point)
		{
			const auto profile = utils::str_to_lower(point.ies_profile);
			if (profile == "fluorescent_tube")
			{
				point.ies_emulation = true; point.ies_emulation_samples = 5; point.ies_emulation_pattern = "line";
				point.ies_emulation_spread = 0.38f; point.ies_emulation_radius_scale = 0.34f; point.ies_emulation_intensity_scale = 0.30f; point.ies_emulation_forward_offset = 0.02f; point.ies_emulation_aspect = 1.0f; point.ies_emulation_twist = 0.0f;
			}
			else if (profile == "street_lamp_cutoff" || profile == "recessed_downlight")
			{
				point.ies_emulation = true; point.ies_emulation_samples = profile == "street_lamp_cutoff" ? 7 : 5; point.ies_emulation_pattern = "ring";
				point.ies_emulation_spread = profile == "street_lamp_cutoff" ? 0.42f : 0.28f; point.ies_emulation_radius_scale = 0.38f; point.ies_emulation_intensity_scale = 0.36f; point.ies_emulation_forward_offset = 0.06f; point.ies_emulation_aspect = 0.70f; point.ies_emulation_twist = 0.0f;
			}
			else if (profile == "spot_25")
			{
				point.ies_emulation = true; point.ies_emulation_samples = 4; point.ies_emulation_pattern = "beam";
				point.ies_emulation_spread = 0.22f; point.ies_emulation_radius_scale = 0.28f; point.ies_emulation_intensity_scale = 0.28f; point.ies_emulation_forward_offset = 0.12f; point.ies_emulation_aspect = 1.25f; point.ies_emulation_twist = 0.0f;
			}
			else if (profile == "exit_sign_wallwash" || profile == "tv_panel")
			{
				point.ies_emulation = true; point.ies_emulation_samples = 4; point.ies_emulation_pattern = "line";
				point.ies_emulation_spread = 0.30f; point.ies_emulation_radius_scale = 0.42f; point.ies_emulation_intensity_scale = 0.22f; point.ies_emulation_forward_offset = 0.02f; point.ies_emulation_aspect = 0.55f; point.ies_emulation_twist = 90.0f;
			}
		}

		void apply_workbench_ies_profile(map_settings::remix_light_settings_s::point_s& point)
		{
			const auto profile = utils::str_to_lower(point.ies_profile);
			if (profile.empty() || profile == "none") {
				return;
			}

			auto aim_from_view = [&point](const float cone, const float soft, const float exp)
				{
					point.use_shaping = true;
					point.degrees = cone;
					point.softness = soft;
					point.exponent = exp * std::max(0.05f, point.ies_focus);
					point.direction = *game::get_current_view_forward();
					point.direction.NormalizeChecked();
				};

			const float s = std::max(0.0f, point.ies_strength);
			if (profile == "omni_soft") { point.use_shaping = false; point.degrees = 180.0f; point.radiance_scalar *= 0.85f * s; }
			else if (profile == "bulb_a19") { point.use_shaping = false; point.degrees = 180.0f; point.radiance = Vector(1.0f, 0.72f, 0.42f); point.radiance_scalar *= 1.10f * s; }
			else if (profile == "fluorescent_tube") { point.radiance = Vector(0.76f, 0.88f, 1.0f); aim_from_view(128.0f, 0.52f, 0.08f); point.radiance_scalar *= 0.85f * s; }
			else if (profile == "recessed_downlight") { aim_from_view(64.0f, 0.22f, 0.45f); point.radiance_scalar *= 1.15f * s; }
			else if (profile == "spot_25") { aim_from_view(25.0f, 0.08f, 1.20f); point.radiance_scalar *= 1.35f * s; }
			else if (profile == "street_lamp_cutoff") { point.radiance = Vector(1.0f, 0.78f, 0.48f); aim_from_view(92.0f, 0.18f, 0.72f); point.radiance_scalar *= 1.05f * s; }
			else if (profile == "exit_sign_wallwash") { point.radiance = Vector(0.42f, 1.0f, 0.38f); aim_from_view(116.0f, 0.62f, 0.05f); point.radiance_scalar *= 0.55f * s; }
			else if (profile == "tv_panel") { point.radiance = Vector(0.48f, 0.68f, 1.0f); aim_from_view(135.0f, 0.70f, 0.04f); point.radiance_scalar *= 0.70f * s; }
			else if (profile == "fire_emitter") { point.use_shaping = false; point.degrees = 180.0f; point.radiance = Vector(1.0f, 0.34f, 0.08f); point.radius = std::max(point.radius, 2.0f); point.radiance_scalar *= 1.10f * s; }
		}

		struct add_light_animation_defaults_s
		{
			float duration;
			float speed;
			float variation;
			float degrees;
			float phase;
			Vector axis;
			bool shaped;
			float cone_degrees;
			const char* group;
			const char* hint;
		};

		add_light_animation_defaults_s add_light_animation_defaults_for(const std::string& raw_name)
		{
			auto name = utils::str_to_lower(raw_name);
			utils::replace_all(name, "-", "_");

			if (name == "fire_pulse" || name == "flame_small" || name == "flame_large") {
				return { 1.20f, 1.00f, 0.22f, 360.0f, 0.0f, Vector(0.0f, 0.0f, 1.0f), false, 180.0f, "fire", "Fire/molotov/barrel: warm intensity and color fluctuation." };
			}
			if (name == "candle_flicker") {
				return { 1.35f, 1.00f, 0.18f, 360.0f, 0.0f, Vector(0.0f, 0.0f, 1.0f), false, 180.0f, "fire", "Small warm flicker for candles/lanterns." };
			}
			if (name == "unstable_bulb" || name == "generator_stutter") {
				return { 1.05f, 1.00f, 0.35f, 360.0f, 0.0f, Vector(0.0f, 0.0f, 1.0f), false, 180.0f, "power", "Bad electrical supply: dips, spikes, recovery." };
			}
			if (name == "broken_fluorescent" || name == "fluorescent_random") {
				return { 0.70f, 1.00f, 0.45f, 360.0f, 0.0f, Vector(0.0f, 0.0f, 1.0f), false, 180.0f, "fluorescent", "Random tube flicker: hard off/on bursts." };
			}
			if (name == "soft_flicker" || name == "tv_noise") {
				return { 0.85f, 1.00f, 0.25f, 360.0f, 0.0f, Vector(0.0f, 0.0f, 1.0f), false, 180.0f, "screen", "Soft screen/TV variation." };
			}
			if (name == "strobe_fast") {
				return { 0.25f, 1.00f, 0.10f, 360.0f, 0.0f, Vector(0.0f, 0.0f, 1.0f), false, 180.0f, "alarm", "Fast emergency strobe." };
			}
			if (name == "strobe_slow") {
				return { 0.80f, 1.00f, 0.10f, 360.0f, 0.0f, Vector(0.0f, 0.0f, 1.0f), false, 180.0f, "alarm", "Slow emergency strobe." };
			}
			if (name == "disc_spin" || name == "spot_axis_spin" || name == "rotating_yaw" || name == "rotating_beacon" || name == "warning_beacon" || name == "axis_spin" || name == "rotate_yaw" || name == "orbit") {
				return { 1.20f, 1.00f, 0.08f, 360.0f, 0.0f, Vector(0.0f, 0.0f, 1.0f), true, 38.0f, "rotating", "Rotating spotlight/disc around an axis." };
			}
			if (name == "searchlight_sweep" || name == "pendulum_sweep" || name == "spot_axis_sweep" || name == "sweep" || name == "pendulum" || name == "vertical_bob" || name == "axis_patrol") {
				return { 2.20f, 1.00f, 0.10f, 110.0f, 0.0f, Vector(0.0f, 0.0f, 1.0f), true, 35.0f, "searchlight", "Back-and-forth spotlight sweep." };
			}
			if (name == "pulse_fast") {
				return { 0.45f, 1.00f, 0.18f, 360.0f, 0.0f, Vector(0.0f, 0.0f, 1.0f), false, 180.0f, "pulse", "Fast pulsing intensity." };
			}
			if (name == "breathing" || name == "pulse_slow") {
				return { 2.00f, 1.00f, 0.15f, 360.0f, 0.0f, Vector(0.0f, 0.0f, 1.0f), false, 180.0f, "pulse", "Slow soft pulse/breathing." };
			}

			return { 1.00f, 1.00f, 0.0f, 360.0f, 0.0f, Vector(0.0f, 0.0f, 1.0f), false, 180.0f, "", "Stable light; no procedural animation." };
		}

		Vector normalize_or_ui(Vector value, const Vector& fallback)
		{
			if (value.LengthSqr() <= 0.0001f) { value = fallback; }
			if (value.LengthSqr() <= 0.0001f) { value = Vector(0.0f, 0.0f, 1.0f); }
			value.Normalize();
			return value;
		}

		Vector rotate_axis_ui(Vector value, Vector axis, const float degrees)
		{
			value = normalize_or_ui(value, Vector(0.0f, 1.0f, 0.0f));
			axis = normalize_or_ui(axis, Vector(0.0f, 0.0f, 1.0f));
			const float radians = degrees * static_cast<float>(M_PI / 180.0);
			const float c = std::cos(radians);
			const float sinv = std::sin(radians);
			const float d = value.Dot(axis);
			return normalize_or_ui(value * c + axis.Cross(value) * sinv + axis * (d * (1.0f - c)), value);
		}

		std::vector<map_settings::remix_light_settings_s::point_s> build_add_light_animation_points(
			const map_settings::remix_light_settings_s::point_s& source_point,
			std::string animation,
			float cycle_time,
			float variation,
			Vector animation_axis,
			float animation_degrees,
			float animation_phase)
		{
			auto name = utils::str_to_lower(animation);
			utils::replace_all(name, "-", "_");

			auto base = source_point;
			base.timepoint = 0.0f;
			cycle_time = std::max(0.05f, cycle_time);
			variation = std::clamp(variation, 0.0f, 1.0f);

			const float base_scalar = base.radiance_scalar;
			const Vector base_radiance = base.radiance;
			const Vector base_dir = normalize_or_ui(base.direction, Vector(0.0f, 1.0f, 0.0f));
			animation_axis = normalize_or_ui(animation_axis, Vector(0.0f, 0.0f, 1.0f));

			std::vector<map_settings::remix_light_settings_s::point_s> points;
			auto add_point = [&](float t, float scalar_mul, Vector dir, Vector radiance_override = Vector(-1.0f, -1.0f, -1.0f))
			{
				auto pt = base;
				pt.timepoint = t;
				pt.radiance_scalar = base_scalar * std::max(0.0f, scalar_mul);
				pt.direction = normalize_or_ui(dir, base_dir);
				if (radiance_override.x >= 0.0f) { pt.radiance = radiance_override; }
				points.push_back(pt);
			};

			if (name.empty() || name == "stable" || name == "none" || name == "off") {
				points.push_back(base);
				return points;
			}

			const float vlow = std::max(0.0f, 1.0f - variation);
			const float vhigh = 1.0f + variation;

			if (name == "pulse_slow" || name == "pulse_fast" || name == "breathing")
			{
				const float low = name == "breathing" ? 0.30f : 0.18f;
				const float high = name == "pulse_fast" ? 1.35f : 1.08f;
				add_point(0.0f, low * vlow, base_dir);
				add_point(cycle_time * 0.5f, high * vhigh, base_dir);
				add_point(cycle_time, low * vlow, base_dir);
			}
			else if (name == "soft_flicker" || name == "broken_fluorescent" || name == "tv_noise" || name == "generator_stutter")
			{
				const float vals[] = { 0.20f, 1.10f, 0.55f, 1.30f, 0.08f, 0.95f, 0.35f, 1.15f, 0.20f };
				const int count = static_cast<int>(sizeof(vals) / sizeof(vals[0]));
				for (int i = 0; i < count; ++i) {
					add_point(cycle_time * (static_cast<float>(i) / static_cast<float>(count - 1)), vals[i] * vhigh, base_dir);
				}
			}
			else if (name == "fire_pulse" || name == "flame_small" || name == "flame_large")
			{
				const float vals[] = { 0.68f, 1.18f, 0.84f, 1.35f, 0.74f, 1.08f, 0.62f };
				const int count = static_cast<int>(sizeof(vals) / sizeof(vals[0]));
				for (int i = 0; i < count; ++i)
				{
					const float u = static_cast<float>(i) / static_cast<float>(count - 1);
					const Vector fire_color = Vector(1.0f, 0.30f + 0.20f * ((i % 3) / 2.0f), 0.05f + 0.08f * u);
					add_point(cycle_time * u, vals[i] * vhigh, base_dir, fire_color);
				}
			}
			else if (name == "candle_flicker" || name == "unstable_bulb" || name == "fluorescent_random")
			{
				const float* vals = nullptr;
				int count = 0;
				const float candle_vals[] = { 0.62f, 1.05f, 0.72f, 1.18f, 0.66f };
				const float bulb_vals[] = { 0.92f, 0.28f, 1.22f, 0.48f, 1.08f, 0.88f };
				const float fl_vals[] = { 0.05f, 1.25f, 0.02f, 0.70f, 0.10f, 1.05f, 0.36f };
				if (name == "candle_flicker") { vals = candle_vals; count = static_cast<int>(sizeof(candle_vals) / sizeof(candle_vals[0])); }
				else if (name == "unstable_bulb") { vals = bulb_vals; count = static_cast<int>(sizeof(bulb_vals) / sizeof(bulb_vals[0])); }
				else { vals = fl_vals; count = static_cast<int>(sizeof(fl_vals) / sizeof(fl_vals[0])); }
				for (int i = 0; i < count; ++i)
				{
					Vector color = base_radiance;
					if (name == "candle_flicker" && (i % 2)) { color = Vector(1.0f, 0.46f + 0.04f * static_cast<float>(i), 0.10f); }
					add_point(cycle_time * (static_cast<float>(i) / static_cast<float>(std::max(1, count - 1))), vals[i] * vhigh, base_dir, color);
				}
			}
			else if (name == "strobe_fast" || name == "strobe_slow")
			{
				const int flashes = name == "strobe_fast" ? 5 : 2;
				for (int i = 0; i <= flashes * 2; ++i) {
					add_point(cycle_time * (static_cast<float>(i) / static_cast<float>(flashes * 2)), (i % 2) ? 1.8f * vhigh : 0.02f, base_dir);
				}
			}
			else if (name == "rotating_yaw" || name == "rotating_pitch" || name == "rotating_roll" || name == "rotating_beacon" || name == "warning_beacon" || name == "police_red_blue" || name == "lighthouse_sweep" || name == "disc_spin" || name == "spot_axis_spin")
			{
				if (name == "rotating_pitch") { animation_axis = Vector(1.0f, 0.0f, 0.0f); }
				else if (name == "rotating_roll") { animation_axis = Vector(0.0f, 1.0f, 0.0f); }
				base.use_shaping = true;
				if (base.degrees >= 179.9f) { base.degrees = 38.0f; }
				const int steps = name == "police_red_blue" ? 8 : 9;
				for (int i = 0; i < steps; ++i)
				{
					const float u = static_cast<float>(i) / static_cast<float>(steps - 1);
					const float angle = animation_phase + animation_degrees * u;
					float scalar = (name == "warning_beacon" || name == "rotating_beacon") ? (0.25f + 1.10f * (0.5f + 0.5f * std::sin(u * static_cast<float>(M_PI) * 2.0f))) : 1.0f;
					Vector color = base_radiance;
					if (name == "police_red_blue") { color = (i % 2) ? Vector(0.25f, 0.35f, 1.0f) : Vector(1.0f, 0.08f, 0.03f); scalar = 1.25f; }
					add_point(cycle_time * u, scalar * vhigh, rotate_axis_ui(base_dir, animation_axis, angle), color);
				}
			}
			else if (name == "searchlight_sweep" || name == "pendulum_sweep" || name == "spot_axis_sweep")
			{
				base.use_shaping = true;
				if (base.degrees >= 179.9f) { base.degrees = 35.0f; }
				const float half = std::max(0.0f, animation_degrees) * 0.5f;
				add_point(0.0f, 0.85f * vlow, rotate_axis_ui(base_dir, animation_axis, animation_phase - half));
				add_point(cycle_time * 0.5f, 1.15f * vhigh, rotate_axis_ui(base_dir, animation_axis, animation_phase + half));
				add_point(cycle_time, 0.85f * vlow, rotate_axis_ui(base_dir, animation_axis, animation_phase - half));
			}
			else {
				points.push_back(base);
			}

			return points;
		}

		bool add_light_animation_is_static_name(std::string name)
		{
			name = utils::str_to_lower(name);
			utils::replace_all(name, "-", "_");
			return name.empty() || name == "stable" || name == "none" || name == "off";
		}


		std::string normalize_light_animation_name(std::string name)
		{
			name = utils::str_to_lower(name);
			utils::replace_all(name, "-", "_");
			return name;
		}

		const char* light_animation_compact_label(const std::string& raw_name)
		{
			const auto name = normalize_light_animation_name(raw_name);
			if (name == "stable") return "Stable";
			if (name == "pulse_slow") return "Slow Pulse";
			if (name == "pulse_fast") return "Fast Pulse";
			if (name == "breathing") return "Breathe";
			if (name == "fire_pulse") return "Fire";
			if (name == "candle_flicker") return "Candle";
			if (name == "soft_flicker") return "Soft Flicker";
			if (name == "unstable_bulb") return "Bad Bulb";
			if (name == "broken_fluorescent") return "Broken Tube";
			if (name == "fluorescent_random") return "Tube Random";
			if (name == "generator_stutter") return "Generator";
			if (name == "tv_noise") return "TV Noise";
			if (name == "strobe_fast") return "Fast Strobe";
			if (name == "strobe_slow") return "Slow Strobe";
			if (name == "none") return "None";
			if (name == "rotate_yaw") return "Yaw";
			if (name == "axis_spin") return "Axis Spin";
			if (name == "sweep") return "Sweep";
			if (name == "pendulum") return "Pendulum";
			if (name == "vertical_bob") return "Vertical";
			if (name == "axis_patrol") return "Patrol";
			if (name == "orbit") return "Orbit";
			return "Custom";
		}

		std::string compact_light_menu_text(std::string text, const size_t max_length = 28u)
		{
			if (text.size() <= max_length) return text;
			text.resize(max_length > 3u ? max_length - 3u : max_length);
			if (max_length > 3u) text += "...";
			return text;
		}

		bool is_light_movement_animation_name(const std::string& raw_name)
		{
			const auto name = normalize_light_animation_name(raw_name);
			return name == "rotate_yaw" || name == "axis_spin" || name == "sweep" || name == "pendulum" ||
				name == "vertical_bob" || name == "axis_patrol" || name == "orbit" ||
				name == "rotating_yaw" || name == "disc_spin" || name == "spot_axis_spin" ||
				name == "spot_axis_sweep" || name == "searchlight_sweep" || name == "pendulum_sweep";
		}

		std::string canonical_light_movement_name(const std::string& raw_name)
		{
			const auto name = normalize_light_animation_name(raw_name);
			if (name == "rotating_yaw") return "rotate_yaw";
			if (name == "disc_spin" || name == "spot_axis_spin") return "axis_spin";
			if (name == "spot_axis_sweep" || name == "searchlight_sweep") return "sweep";
			if (name == "pendulum_sweep") return "pendulum";
			return is_light_movement_animation_name(name) ? name : "none";
		}

		int light_animation_index(const char* const* names, const int count, const std::string& raw_name, const int fallback = 0)
		{
			const auto name = normalize_light_animation_name(raw_name);
			for (int i = 0; i < count; ++i) if (name == names[i]) return i;
			return std::clamp(fallback, 0, std::max(0, count - 1));
		}

		float light_output_peak_multiplier(const std::string& raw_name, const float variation)
		{
			const auto name = normalize_light_animation_name(raw_name);
			const float vhigh = 1.0f + std::clamp(variation, 0.0f, 1.0f);
			if (name == "pulse_fast") return 1.35f * vhigh;
			if (name == "pulse_slow" || name == "breathing") return 1.08f * vhigh;
			if (name == "soft_flicker" || name == "broken_fluorescent" || name == "tv_noise" || name == "generator_stutter") return 1.30f * vhigh;
			if (name == "fire_pulse" || name == "flame_small" || name == "flame_large") return 1.35f * vhigh;
			if (name == "candle_flicker") return 1.18f * vhigh;
			if (name == "unstable_bulb") return 1.22f * vhigh;
			if (name == "fluorescent_random") return 1.25f * vhigh;
			if (name == "strobe_fast" || name == "strobe_slow") return 1.80f * vhigh;
			return 1.0f;
		}

		float infer_light_animation_base_power(const map_settings::remix_light_settings_s& def)
		{
			if (std::isfinite(def.property_animation_intensity) && def.property_animation_intensity >= 0.0f)
				return def.property_animation_intensity;
			if (def.points.empty()) return 1.0f;
			float peak = 0.0f;
			for (const auto& point : def.points) peak = std::max(peak, std::max(0.0f, point.radiance_scalar));
			const bool split_output_active = !add_light_animation_is_static_name(def.property_animation);
			const auto output_name = split_output_active ? def.property_animation : def.animation;
			const float variation = split_output_active ? def.property_animation_variation : def.animation_variation;
			return peak / std::max(0.001f, light_output_peak_multiplier(output_name, variation));
		}

		map_settings::remix_light_settings_s::point_s sample_light_animation_points(
			const std::vector<map_settings::remix_light_settings_s::point_s>& points, float time)
		{
			if (points.empty()) return {};
			if (points.size() == 1u) return points.front();
			const float duration = std::max(0.0001f, points.back().timepoint);
			time = std::fmod(std::max(0.0f, time), duration);
			if (time < 0.0f) time += duration;
			size_t upper = 1u;
			while (upper < points.size() && points[upper].timepoint < time) ++upper;
			upper = std::min(upper, points.size() - 1u);
			const size_t lower = upper > 0u ? upper - 1u : 0u;
			const auto& a = points[lower];
			const auto& b = points[upper];
			const float span = std::max(0.0001f, b.timepoint - a.timepoint);
			const float alpha = std::clamp((time - a.timepoint) / span, 0.0f, 1.0f);
			auto out = a;
			out.position = a.position + (b.position - a.position) * alpha;
			out.direction = normalize_or_ui(a.direction + (b.direction - a.direction) * alpha, a.direction);
			out.radiance = a.radiance + (b.radiance - a.radiance) * alpha;
			out.radiance_scalar = a.radiance_scalar + (b.radiance_scalar - a.radiance_scalar) * alpha;
			out.timepoint = time;
			return out;
		}

		void migrate_legacy_light_animation_metadata(map_settings::remix_light_settings_s& def)
		{
			if (def.property_animation_intensity < 0.0f) def.property_animation_intensity = infer_light_animation_base_power(def);
			if (add_light_animation_is_static_name(def.property_animation) && canonical_light_movement_name(def.movement_animation) == "none")
			{
				const auto legacy = normalize_light_animation_name(def.animation);
				if (is_light_movement_animation_name(legacy))
				{
					def.movement_animation = canonical_light_movement_name(legacy);
					def.movement_animation_duration = std::max(0.05f, def.animation_duration);
					def.movement_animation_speed = std::max(0.05f, def.animation_speed);
					def.movement_animation_axis = normalize_or_ui(def.animation_axis, Vector(0.0f, 0.0f, 1.0f));
					def.movement_animation_degrees = def.animation_degrees;
					def.movement_animation_phase = def.animation_phase;
				}
				else if (!add_light_animation_is_static_name(legacy))
				{
					def.property_animation = legacy;
					def.property_animation_duration = std::max(0.05f, def.animation_duration);
					def.property_animation_speed = std::max(0.05f, def.animation_speed);
					def.property_animation_variation = std::clamp(def.animation_variation, 0.0f, 1.0f);
				}
			}
		}

		std::vector<map_settings::remix_light_settings_s::point_s> build_split_light_animation_points(
			const map_settings::remix_light_settings_s::point_s& source_point,
			const map_settings::remix_light_settings_s& def)
		{
			auto base = source_point;
			base.timepoint = 0.0f;
			base.radiance_scalar = std::max(0.0f, def.property_animation_intensity);
			const auto output_name = normalize_light_animation_name(def.property_animation);
			const auto movement_name = canonical_light_movement_name(def.movement_animation);
			const bool has_output = !add_light_animation_is_static_name(output_name);
			const bool has_movement = movement_name != "none";
			if (!has_output && !has_movement) return { base };

			const float output_cycle = std::max(0.05f, def.property_animation_duration) / std::max(0.05f, def.property_animation_speed);
			const float movement_cycle = std::max(0.05f, def.movement_animation_duration) / std::max(0.05f, def.movement_animation_speed);
			const float duration = std::max(has_output ? output_cycle : 0.0f, has_movement ? movement_cycle : 0.0f);
			auto output_points = has_output
				? build_add_light_animation_points(base, output_name, output_cycle, def.property_animation_variation, Vector(0.0f, 0.0f, 1.0f), 0.0f, 0.0f)
				: std::vector<map_settings::remix_light_settings_s::point_s>{ base };

			std::vector<map_settings::remix_light_settings_s::point_s> movement_points{ base };
			if (has_movement && movement_name != "vertical_bob" && movement_name != "axis_patrol" && movement_name != "orbit")
			{
				std::string legacy_name = movement_name == "rotate_yaw" ? "rotating_yaw" :
					movement_name == "axis_spin" ? "spot_axis_spin" : movement_name == "pendulum" ? "pendulum_sweep" : "searchlight_sweep";
				movement_points = build_add_light_animation_points(base, legacy_name, movement_cycle, 0.0f,
					def.movement_animation_axis, def.movement_animation_degrees, def.movement_animation_phase);
				for (auto& point : movement_points)
				{
					point.radiance = base.radiance;
					point.radiance_scalar = base.radiance_scalar;
				}
			}

			const Vector move_axis = normalize_or_ui(def.movement_animation_axis, Vector(0.0f, 0.0f, 1.0f));
			Vector orbit_u = normalize_or_ui(move_axis.Cross(Vector(0.0f, 1.0f, 0.0f)), Vector(1.0f, 0.0f, 0.0f));
			Vector orbit_v = normalize_or_ui(move_axis.Cross(orbit_u), Vector(0.0f, 1.0f, 0.0f));
			constexpr int sample_count = 24;
			std::vector<map_settings::remix_light_settings_s::point_s> result;
			result.reserve(sample_count + 1);
			for (int i = 0; i <= sample_count; ++i)
			{
				const float u = static_cast<float>(i) / static_cast<float>(sample_count);
				const float t = duration * u;
				auto point = has_output ? sample_light_animation_points(output_points, t) : base;
				if (has_movement)
				{
					if (movement_name == "vertical_bob" || movement_name == "axis_patrol")
					{
						const float wave = movement_name == "axis_patrol"
							? (1.0f - 4.0f * std::abs(std::fmod(t / movement_cycle + 0.25f, 1.0f) - 0.5f))
							: std::sin((t / movement_cycle) * static_cast<float>(M_PI) * 2.0f + def.movement_animation_phase * static_cast<float>(M_PI / 180.0));
						point.position = base.position + move_axis * (wave * std::max(0.0f, def.movement_animation_distance));
						point.direction = base.direction;
					}
					else if (movement_name == "orbit")
					{
						const float angle = ((t / movement_cycle) * 360.0f + def.movement_animation_phase) * static_cast<float>(M_PI / 180.0);
						point.position = base.position + (orbit_u * std::cos(angle) + orbit_v * std::sin(angle)) * std::max(0.0f, def.movement_animation_distance);
						point.direction = base.direction;
					}
					else
					{
						const auto movement_point = sample_light_animation_points(movement_points, t);
						point.position = movement_point.position;
						point.direction = movement_point.direction;
						point.use_shaping = movement_point.use_shaping;
						point.degrees = movement_point.degrees;
					}
				}
				point.timepoint = t;
				result.push_back(point);
			}
			return result;
		}

		void rebuild_split_light_animation(map_settings::remix_light_settings_s& def,
			const map_settings::remix_light_settings_s::point_s* preferred_base = nullptr)
		{
			migrate_legacy_light_animation_metadata(def);
			if (def.points.empty() && !preferred_base) return;
			auto base = preferred_base ? *preferred_base : def.points.front();
			base.timepoint = 0.0f;
			base.radiance_scalar = std::max(0.0f, def.property_animation_intensity);
			def.property_animation = normalize_light_animation_name(def.property_animation);
			if (def.property_animation.empty() || def.property_animation == "none" || def.property_animation == "off") def.property_animation = "stable";
			def.movement_animation = canonical_light_movement_name(def.movement_animation);
			def.property_animation_duration = std::max(0.05f, def.property_animation_duration);
			def.property_animation_speed = std::max(0.05f, def.property_animation_speed);
			def.property_animation_variation = std::clamp(def.property_animation_variation, 0.0f, 1.0f);
			def.movement_animation_duration = std::max(0.05f, def.movement_animation_duration);
			def.movement_animation_speed = std::max(0.05f, def.movement_animation_speed);
			def.movement_animation_axis = normalize_or_ui(def.movement_animation_axis, Vector(0.0f, 0.0f, 1.0f));
			def.movement_animation_degrees = std::clamp(def.movement_animation_degrees, 0.0f, 1440.0f);
			def.movement_animation_distance = std::max(0.0f, def.movement_animation_distance);
			def.points = build_split_light_animation_points(base, def);
			def.animation = !add_light_animation_is_static_name(def.property_animation) ? def.property_animation : def.movement_animation;
			def.animation_duration = !add_light_animation_is_static_name(def.property_animation) ? def.property_animation_duration : def.movement_animation_duration;
			def.animation_speed = !add_light_animation_is_static_name(def.property_animation) ? def.property_animation_speed : def.movement_animation_speed;
			def.animation_variation = def.property_animation_variation;
			def.animation_axis = def.movement_animation_axis;
			def.animation_degrees = def.movement_animation_degrees;
			def.animation_phase = def.movement_animation_phase;
			def.loop = !add_light_animation_is_static_name(def.property_animation) || def.movement_animation != "none";
			def.loop_smoothing = false;
		}

		size_t ui_light_point_index_from_ptr(const remix_lights::light* edit_light, const map_settings::remix_light_settings_s::point_s* point)
		{
			if (!edit_light || !point || edit_light->m_def.points.empty()) {
				return 0u;
			}

			const auto* begin = edit_light->m_def.points.data();
			const auto* end = begin + edit_light->m_def.points.size();
			if (point >= begin && point < end) {
				return static_cast<size_t>(point - begin);
			}

			return 0u;
		}

		map_settings::remix_light_settings_s::point_s* ui_light_point_by_index(remix_lights::light* edit_light, size_t index)
		{
			if (!edit_light || edit_light->m_def.points.empty()) {
				return nullptr;
			}

			index = std::min(index, edit_light->m_def.points.size() - 1u);
			return &edit_light->m_def.points[index];
		}

		void rebuild_edit_light_runtime_from_def(remix_lights::light* edit_light, size_t preview_point_index = 0u)
		{
			if (!edit_light || edit_light->m_def.points.empty()) {
				return;
			}

			if (edit_light->m_def.points.size() > 1u)
			{
				edit_light->m_mover.init(edit_light->m_def.points, edit_light->m_def.loop, edit_light->m_def.loop_smoothing);
				remix_lights::get()->update_remix_light(edit_light);
			}
			else
			{
				edit_light->m_mover.init(edit_light->m_def.points, false, false);
				preview_point_index = std::min(preview_point_index, edit_light->m_def.points.size() - 1u);
				remix_lights::get()->update_static_remix_light(edit_light, &edit_light->m_def.points[preview_point_index]);
			}
		}

		void copy_light_backend_to_all_points(remix_lights::light* edit_light, const map_settings::remix_light_settings_s::point_s& source)
		{
			if (!edit_light) {
				return;
			}

			for (auto& point : edit_light->m_def.points)
			{
				point.use_shaping = source.use_shaping;
				point.authoring_shape = source.authoring_shape;
				point.authoring_width = source.authoring_width;
				point.authoring_height = source.authoring_height;
				point.authoring_length = source.authoring_length;
				point.authoring_range = source.authoring_range;
				point.light_rig_mode = source.light_rig_mode;
				point.ies_profile = source.ies_profile;
				point.ies_file = source.ies_file;
				point.ies_axis_rotation = source.ies_axis_rotation;
				point.ies_angle_scale = source.ies_angle_scale;
				point.ies_intensity_scale = source.ies_intensity_scale;
				point.ies_normalize = source.ies_normalize;
				point.ies_strength = source.ies_strength;
				point.ies_focus = source.ies_focus;
				point.ies_emulation = source.light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES && source.ies_emulation;
				point.ies_emulation_samples = source.ies_emulation_samples;
				point.ies_emulation_spread = source.ies_emulation_spread;
				point.ies_emulation_radius_scale = source.ies_emulation_radius_scale;
				point.ies_emulation_intensity_scale = source.ies_emulation_intensity_scale;
				point.ies_emulation_forward_offset = source.ies_emulation_forward_offset;
				point.ies_emulation_pattern = source.ies_emulation_pattern;
				point.ies_emulation_aspect = source.ies_emulation_aspect;
				point.ies_emulation_twist = source.ies_emulation_twist;
			}
		}

		Vector calculate_edit_light_center_from_def(const remix_lights::light* edit_light)
		{
			Vector center = {};
			if (!edit_light || edit_light->m_def.points.empty()) {
				return center;
			}

			for (const auto& point : edit_light->m_def.points) {
				center += point.position;
			}

			center /= static_cast<float>(edit_light->m_def.points.size());
			return center;
		}

		void translate_edit_light_points(remix_lights::light* edit_light, const Vector& offset)
		{
			if (!edit_light) {
				return;
			}

			for (auto& point : edit_light->m_def.points) {
				point.position += offset;
			}

			rebuild_edit_light_runtime_from_def(edit_light);
		}

		void move_edit_light_center_to_position(remix_lights::light* edit_light, const Vector& target_position)
		{
			if (!edit_light || edit_light->m_def.points.empty()) {
				return;
			}

			translate_edit_light_points(edit_light, target_position - calculate_edit_light_center_from_def(edit_light));
		}


		bool light_def_pointer_in_vector(const std::vector<map_settings::remix_light_settings_s>& lights,
			const map_settings::remix_light_settings_s* ptr)
		{
			if (!ptr) {
				return false;
			}

			for (const auto& light : lights)
			{
				if (&light == ptr) {
					return true;
				}
			}

			return false;
		}

		void translate_map_light_def(map_settings::remix_light_settings_s& def, const Vector& offset)
		{
			for (auto& point : def.points) {
				point.position += offset;
			}
		}

		Vector map_light_def_center(const map_settings::remix_light_settings_s& def)
		{
			Vector center = {};
			if (def.points.empty()) {
				return center;
			}

			for (const auto& point : def.points) {
				center += point.position;
			}

			center /= static_cast<float>(def.points.size());
			return center;
		}


		void rebuild_editor_light_preview(remix_lights* lights,
			std::vector<map_settings::remix_light_settings_s>& map_lights,
			map_settings::remix_light_settings_s* selection,
			bool preview_all_lights);

		float light_editor_resolve_horizontal_fov()
		{
			const auto im = imgui::get();
			float fov = im ? std::clamp(im->m_light_editor_manual_fov, 20.0f, 160.0f) : 75.0f;
			if (!im || !im->m_light_editor_dynamic_fov) {
				return fov;
			}

			const char* fov_cvars[] = { "fov_desired", "default_fov", "fov", "cl_fov" };
			for (const auto* name : fov_cvars)
			{
				if (const auto* var = game::find_cvar_const(name); var)
				{
					const float candidate = var->m_Value.m_fValue;
					if (candidate >= 20.0f && candidate <= 160.0f) {
						return candidate;
					}
				}
			}
			return fov;
		}

		bool light_editor_mouse_world_ray(Vector& out_origin, Vector& out_dir)
		{
			Vector view_origin = {};
			Vector view_forward = {};
			Vector view_right = {};
			Vector view_up = {};
			if (!game::get_current_view_basis(view_origin, view_forward, view_right, view_up)) {
				return false;
			}

			const auto& io = ImGui::GetIO();
			float screen_w = io.DisplaySize.x;
			float screen_h = io.DisplaySize.y;
			if (screen_w <= 1.0f || screen_h <= 1.0f)
			{
				int engine_w = 0, engine_h = 0;
				interfaces::get()->m_engine->get_screen_size(engine_w, engine_h);
				screen_w = static_cast<float>(engine_w);
				screen_h = static_cast<float>(engine_h);
			}
			if (screen_w <= 1.0f || screen_h <= 1.0f) {
				return false;
			}

			const float nx = std::clamp(((io.MousePos.x / screen_w) - 0.5f) * 2.0f, -2.0f, 2.0f);
			const float ny = std::clamp((0.5f - (io.MousePos.y / screen_h)) * 2.0f, -2.0f, 2.0f);
			const float aspect = screen_w / screen_h;
			const float horizontal_fov = light_editor_resolve_horizontal_fov();
			const float tan_half_horizontal = std::tan(DEG2RAD(horizontal_fov) * 0.5f);
			const float tan_half_vertical = tan_half_horizontal / std::max(0.01f, aspect);

			out_origin = view_origin;
			out_dir = view_forward + (view_right * (nx * tan_half_horizontal)) + (view_up * (ny * tan_half_vertical));
			if (out_dir.LengthSqr() <= 0.0001f) {
				out_dir = view_forward;
			}
			out_dir.NormalizeChecked();
			return true;
		}

		bool light_editor_ray_plane_intersection(const Vector& ray_origin, const Vector& ray_dir,
			const Vector& plane_origin, Vector plane_normal, Vector& out_hit)
		{
			if (plane_normal.LengthSqr() <= 0.000001f) return false;
			plane_normal.NormalizeChecked();
			const float denom = plane_normal.Dot(ray_dir);
			if (std::fabs(denom) <= 0.00001f) return false;
			const float distance = plane_normal.Dot(plane_origin - ray_origin) / denom;
			if (!std::isfinite(distance)) return false;
			out_hit = ray_origin + ray_dir * distance;
			return true;
		}

		bool light_editor_axis_parameter_from_ray(const Vector& ray_origin, Vector ray_dir,
			const Vector& axis_origin, Vector axis_dir, float& out_axis_parameter)
		{
			if (ray_dir.LengthSqr() <= 0.000001f || axis_dir.LengthSqr() <= 0.000001f) return false;
			ray_dir.NormalizeChecked();
			axis_dir.NormalizeChecked();
			const Vector w0 = ray_origin - axis_origin;
			const float a = ray_dir.Dot(ray_dir);
			const float b = ray_dir.Dot(axis_dir);
			const float c = axis_dir.Dot(axis_dir);
			const float d = ray_dir.Dot(w0);
			const float e = axis_dir.Dot(w0);
			const float denominator = a * c - b * b;
			if (std::fabs(denominator) <= 0.00001f) return false;
			out_axis_parameter = (a * e - b * d) / denominator;
			return std::isfinite(out_axis_parameter);
		}

		void light_editor_direction_basis(Vector direction, Vector& out_right, Vector& out_up)
		{
			if (direction.LengthSqr() <= 0.000001f) direction = Vector(0.0f, 0.0f, -1.0f);
			direction.NormalizeChecked();
			const Vector reference = std::fabs(direction.z) < 0.92f ? Vector(0.0f, 0.0f, 1.0f) : Vector(0.0f, 1.0f, 0.0f);
			out_right = reference.Cross(direction);
			if (out_right.LengthSqr() <= 0.000001f) out_right = Vector(1.0f, 0.0f, 0.0f);
			out_right.NormalizeChecked();
			out_up = direction.Cross(out_right);
			if (out_up.LengthSqr() <= 0.000001f) out_up = Vector(0.0f, 1.0f, 0.0f);
			out_up.NormalizeChecked();
		}

		Vector light_editor_mouse_world_position(const float distance)
		{
			Vector origin, dir;
			if (!light_editor_mouse_world_ray(origin, dir)) {
				const auto* view_origin = game::get_current_view_origin();
				const auto* view_forward = game::get_current_view_forward();
				return (view_origin && view_forward) ? (*view_origin + (*view_forward * distance)) : Vector();
			}

			return origin + (dir * distance);
		}

		bool light_editor_mouse_surface_position(const float max_distance, Vector& out_position, Vector* out_normal = nullptr)
		{
			Vector origin, direction;
			if (!light_editor_mouse_world_ray(origin, direction)) return false;

			const auto* intf = interfaces::get();
			if (!intf || !intf->m_engine_trace) return false;

			const float distance = std::clamp(max_distance, 16.0f, 8192.0f);
			const Vector end = origin + direction * distance;
			const auto ray = sdk::make_ray(origin, end);

			const void* local_player = nullptr;
			if (intf->m_entity_list && intf->m_engine)
			{
				local_player = intf->m_entity_list->get_client_entity(intf->m_engine->get_local_player());
			}

			sdk::trace_filter_skip_entity filter(local_player);
			sdk::game_trace trace = {};
			constexpr unsigned int mask_light_placement = 0x4600400Bu; // MASK_SHOT: world, props and physical geometry.
			intf->m_engine_trace->trace_ray(ray, mask_light_placement, &filter, &trace);
			if (!trace.did_hit() || trace.fraction >= 1.0f || trace.start_solid) return false;
			if (!std::isfinite(trace.endpos.x) || !std::isfinite(trace.endpos.y) || !std::isfinite(trace.endpos.z)) return false;

			Vector normal = trace.plane.normal;
			if (normal.LengthSqr() <= 0.000001f) normal = direction * -1.0f;
			normal.NormalizeChecked();
			const auto* im = imgui::get();
			const float offset = im ? std::clamp(im->m_light_editor_surface_offset, 0.0f, 64.0f) : 2.0f;
			out_position = trace.endpos + normal * offset;
			if (out_normal) *out_normal = normal;
			return true;
		}

		Vector light_editor_resolve_spawn_position(const float max_distance, bool* surface_hit = nullptr)
		{
			Vector position = {};
			const auto* im = imgui::get();
			const bool use_surface = !im || im->m_light_editor_surface_placement;
			const bool hit = use_surface && light_editor_mouse_surface_position(max_distance, position);
			if (!hit) position = light_editor_mouse_world_position(max_distance);
			if (surface_hit) *surface_hit = hit;
			return position;
		}

		void apply_radius_delta_to_light_def(map_settings::remix_light_settings_s& def, const float delta, const bool all_points)
		{
			if (def.points.empty()) return;

			auto apply = [delta](map_settings::remix_light_settings_s::point_s& point)
			{
				const int shape = point.authoring_shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_AUTO
					? (point.use_shaping ? map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT : map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT)
					: point.authoring_shape;
				if (shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK ||
					shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT)
				{
					point.authoring_width = std::max(0.001f, point.authoring_width + delta * 2.0f);
					point.authoring_height = std::max(0.001f, point.authoring_height + delta * 2.0f);
				}
				else if (shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE)
				{
					point.authoring_width = std::max(0.001f, point.authoring_width + delta);
					point.authoring_length = std::max(0.001f, point.authoring_length + delta * 2.0f);
				}
				else if (shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT)
				{
					point.authoring_width = std::max(0.001f, point.authoring_width + delta);
				}
				else
				{
					point.radius = std::max(0.001f, point.radius + delta);
				}
			};

			if (all_points) for (auto& point : def.points) apply(point);
			else apply(def.points.front());
		}

		void apply_radiance_delta_to_light_def(map_settings::remix_light_settings_s& def, const float delta, const bool all_points)
		{
			if (def.points.empty()) {
				return;
			}

			auto apply = [delta](map_settings::remix_light_settings_s::point_s& point)
			{
				point.radiance_scalar = std::max(0.0f, point.radiance_scalar + delta);
			};

			if (all_points) {
				for (auto& point : def.points) { apply(point); }
			} else {
				apply(def.points.front());
			}
		}

		void apply_rotation_delta_to_light_def(map_settings::remix_light_settings_s& def, const float yaw_delta, const float pitch_delta, const bool all_points)
		{
			const auto* view_right = game::get_current_view_right();
			const auto* view_up = game::get_current_view_up();
			if (!view_right || !view_up || def.points.empty()) {
				return;
			}

			auto apply = [&](map_settings::remix_light_settings_s::point_s& point)
			{
				Vector dir = point.direction;
				if (dir.LengthSqr() <= 0.0001f) {
					dir = game::get_current_view_forward() ? *game::get_current_view_forward() : Vector(0.0f, 1.0f, 0.0f);
				}

				dir += (*view_right * yaw_delta) + (*view_up * pitch_delta);
				if (dir.LengthSqr() <= 0.0001f) {
					dir = Vector(0.0f, 1.0f, 0.0f);
				}
				dir.NormalizeChecked();
				point.direction = dir;
				point.use_shaping = true;
				point.degrees = std::clamp(point.degrees <= 0.0f ? 35.0f : point.degrees, 1.0f, 179.0f);
			};

			if (all_points) {
				for (auto& point : def.points) { apply(point); }
			} else {
				apply(def.points.front());
			}
		}

		void sync_selected_map_light_to_runtime(remix_lights* lights,
			map_settings::remix_light_settings_s* selection,
			map_settings::remix_light_settings_s* changed_light,
			std::vector<map_settings::remix_light_settings_s>& map_lights,
			const bool preview_all_lights)
		{
			if (!lights || !changed_light) {
				return;
			}

			if (changed_light == selection)
			{
				if (auto edit_light = lights->get_first_active_light(); edit_light)
				{
					edit_light->m_def = *changed_light;
					rebuild_edit_light_runtime_from_def(edit_light);
					return;
				}
			}

			rebuild_editor_light_preview(lights, map_lights, selection, preview_all_lights);
		}

		const char* light_authoring_shape_name(const int shape);

		map_settings::remix_light_settings_s make_mouse_spawn_light_def(const Vector& position, const int authoring_shape)
		{
			const auto im = imgui::get();
			const auto radius = im ? std::max(0.001f, im->m_light_editor_spawn_radius) : 1.0f;
			const auto intensity = im ? std::max(0.0f, im->m_light_editor_spawn_intensity) : 10.0f;
			const int shape = authoring_shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_AUTO
				? map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT : authoring_shape;

			map_settings::remix_light_settings_s::point_s pt = {};
			pt.position = position;
			pt.radiance = Vector(1.0f, 1.0f, 1.0f);
			pt.radiance_scalar = intensity;
			pt.radius = radius;
			pt.timepoint = 0.0f;
			pt.smoothness = 0.5f;
			pt.authoring_shape = shape;
			pt.authoring_width = std::max(0.25f, radius * 2.0f);
			pt.authoring_height = std::max(0.25f, radius * 2.0f);
			pt.authoring_length = std::max(1.0f, radius * 6.0f);
			pt.authoring_range = std::max(32.0f, im ? im->m_debugvis_cone_height : 60.0f);
			pt.use_shaping = shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT ||
				shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK ||
				shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT ||
				shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT;
			pt.degrees = pt.use_shaping ? 35.0f : 180.0f;
			pt.softness = pt.use_shaping ? 0.12f : 0.0f;
			pt.exponent = pt.use_shaping ? 0.65f : 0.0f;
			if (const auto* fwd = game::get_current_view_forward(); fwd) {
				pt.direction = *fwd;
				pt.direction.NormalizeChecked();
			}

			map_settings::remix_light_settings_s def = {};
			def.points = { pt };
			def.run_once = false;
			def.loop = true;
			def.loop_smoothing = false;
			def.trigger_always = false;
			def.enabled = true;
			def.group = "mouse_authoring";
			def.comment = std::string("middle-click ") + utils::str_to_lower(std::string(light_authoring_shape_name(shape))) + " light";
			return def;
		}

		Vector normalized_light_debug_color(const map_settings::remix_light_settings_s::point_s& point)
		{
			Vector color = point.radiance;
			if (color.LengthSqr() <= 0.0001f) {
				color = Vector(1.0f, 1.0f, 1.0f);
			}

			color.NormalizeChecked();
			color.x = std::clamp(color.x, 0.12f, 1.0f);
			color.y = std::clamp(color.y, 0.12f, 1.0f);
			color.z = std::clamp(color.z, 0.12f, 1.0f);
			return color;
		}

		ImU32 light_debug_color_u32(const map_settings::remix_light_settings_s::point_s& point,
			const bool selected, const bool enabled, const float alpha_scale = 1.0f)
		{
			const Vector color = normalized_light_debug_color(point);
			const float alpha = std::clamp((enabled ? (selected ? 1.0f : 0.72f) : 0.34f) * alpha_scale, 0.0f, 1.0f);
			return ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, alpha));
		}

		void draw_projected_line(ImDrawList* draw_list, const Vector& start, const Vector& end, const ImU32 color, const float thickness)
		{
			Vector start_screen, end_screen;
			if (common::imgui::world2screen(start, start_screen) && common::imgui::world2screen(end, end_screen)) {
				draw_list->AddLine(ImVec2(start_screen.x, start_screen.y), ImVec2(end_screen.x, end_screen.y), color, thickness);
			}
		}

		void draw_projected_arrow(ImDrawList* draw_list, const Vector& start, const Vector& end,
			const ImU32 color, const float thickness, const float head_size = 7.0f)
		{
			Vector start_screen, end_screen;
			if (!common::imgui::world2screen(start, start_screen) || !common::imgui::world2screen(end, end_screen)) return;
			const ImVec2 a(start_screen.x, start_screen.y);
			const ImVec2 b(end_screen.x, end_screen.y);
			draw_list->AddLine(a, b, color, thickness);
			ImVec2 d = b - a;
			const float len = std::sqrt(d.x * d.x + d.y * d.y);
			if (len <= 0.001f) return;
			d.x /= len; d.y /= len;
			const ImVec2 n(-d.y, d.x);
			const ImVec2 base = b - d * head_size;
			draw_list->AddTriangleFilled(b, base + n * (head_size * 0.55f), base - n * (head_size * 0.55f), color);
		}

		void draw_projected_ellipse(ImDrawList* draw_list, const Vector& center,
			const Vector& axis_x, const Vector& axis_y, const ImU32 color, const float thickness, const int segments = 48)
		{
			ImVec2 first = {};
			ImVec2 previous = {};
			bool have_first = false;
			bool have_previous = false;
			for (int i = 0; i <= segments; ++i)
			{
				const float angle = static_cast<float>(i) / static_cast<float>(segments) * M_PI * 2.0f;
				const Vector world = center + axis_x * std::cos(angle) + axis_y * std::sin(angle);
				Vector screen;
				if (!common::imgui::world2screen(world, screen)) {
					have_previous = false;
					continue;
				}
				const ImVec2 current(screen.x, screen.y);
				if (!have_first) { first = current; have_first = true; }
				if (have_previous) draw_list->AddLine(previous, current, color, thickness);
				previous = current;
				have_previous = true;
			}
			if (have_first && have_previous) draw_list->AddLine(previous, first, color, thickness);
		}

		int resolved_light_authoring_shape(const map_settings::remix_light_settings_s::point_s& point)
		{
			if (point.authoring_shape != map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_AUTO) {
				return point.authoring_shape;
			}
			return point.use_shaping
				? map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT
				: map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT;
		}

		const char* light_authoring_shape_name(const int shape)
		{
			switch (shape)
			{
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT: return "Point";
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT: return "Spot";
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK: return "Disc";
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT: return "Rect";
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE: return "Tube";
			case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT: return "Directional / Distant";
			default: return "Auto";
			}
		}

		float light_editor_visibility_alpha(const float distance, const imgui* im)
		{
			if (!im) return 1.0f;
			const float max_distance = std::max(1.0f, im->m_light_editor_gizmo_visibility_distance);
			const float fade_start = std::clamp(im->m_light_editor_gizmo_fade_start, 0.0f, max_distance);
			if (distance <= fade_start || fade_start >= max_distance - 0.001f) return 1.0f;
			return std::clamp(1.0f - (distance - fade_start) / (max_distance - fade_start), 0.06f, 1.0f);
		}

		void draw_authored_light_shape_gizmo(const map_settings::remix_light_settings_s& def,
			const bool selected, const float distance, const float alpha_scale)
		{
			const auto im = imgui::get();
			if (!im || def.points.empty()) return;
			if (!selected && distance > std::max(1.0f, im->m_light_editor_shape_visibility_distance)) return;

			const auto& first_point = def.points.front();
			const Vector color_vec = normalized_light_debug_color(first_point);
			const ImU32 color = light_debug_color_u32(first_point, selected, def.enabled, alpha_scale);
			const ImU32 muted_color = ImGui::ColorConvertFloat4ToU32(ImVec4(color_vec.x, color_vec.y, color_vec.z,
				std::clamp((selected ? 0.48f : 0.23f) * alpha_scale, 0.0f, 1.0f)));
			const ImU32 path_color = ImGui::ColorConvertFloat4ToU32(ImVec4(color_vec.x, color_vec.y, color_vec.z,
				std::clamp((selected ? 0.82f : 0.40f) * alpha_scale, 0.0f, 1.0f)));
			auto* draw_list = ImGui::GetForegroundDrawList();

			for (size_t i = 1u; i < def.points.size(); ++i) {
				draw_projected_line(draw_list, def.points[i - 1u].position, def.points[i].position, path_color, selected ? 2.0f : 1.0f);
			}

			for (const auto& point : def.points)
			{
				Vector dir = point.direction;
				if (dir.LengthSqr() <= 0.0001f) dir = Vector(0.0f, 0.0f, -1.0f);
				dir.NormalizeChecked();
				Vector local_right, local_up;
				light_editor_direction_basis(dir, local_right, local_up);

				const int shape = resolved_light_authoring_shape(point);
				const float source_radius = std::max(0.01f, point.radius);
				const float width = std::max(0.01f, point.authoring_width);
				const float height = std::max(0.01f, point.authoring_height);
				const float length = std::max(0.01f, point.authoring_length);
				const float range = std::max(4.0f, point.authoring_range > 0.0f ? point.authoring_range : im->m_debugvis_cone_height);

				if (shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT)
				{
					if (im->m_debugvis_radius)
					{
						ImGui::Draw3DCircle(draw_list, point.position, Vector(0.0f, 0.0f, 1.0f), source_radius, false, muted_color, selected ? 1.8f : 1.0f, 48);
						if (selected)
						{
							ImGui::Draw3DCircle(draw_list, point.position, Vector(0.0f, 1.0f, 0.0f), source_radius, false, muted_color, 1.0f, 36);
							ImGui::Draw3DCircle(draw_list, point.position, Vector(1.0f, 0.0f, 0.0f), source_radius, false, muted_color, 1.0f, 36);
						}
					}
				}
				else if (shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT)
				{
					if (!im->m_debugvis_shaping) continue;
					const float cone_deg = std::clamp(point.degrees <= 0.0f ? 35.0f : point.degrees, 1.0f, 89.0f);
					const float end_radius = range * std::tan(DEG2RAD(cone_deg));
					const Vector end_center = point.position + dir * range;
					draw_projected_line(draw_list, point.position, end_center, color, selected ? 2.2f : 1.2f);
					draw_projected_ellipse(draw_list, end_center, local_right * end_radius, local_up * end_radius, muted_color, selected ? 1.7f : 1.0f, 48);
					const Vector edges[] = {
						end_center + local_right * end_radius,
						end_center - local_right * end_radius,
						end_center + local_up * end_radius,
						end_center - local_up * end_radius,
					};
					for (const auto& edge : edges) draw_projected_line(draw_list, point.position, edge, muted_color, selected ? 1.6f : 0.9f);
					if (point.softness > 0.001f)
					{
						const float inner_deg = std::max(0.5f, cone_deg * (1.0f - std::clamp(point.softness, 0.0f, 0.95f)));
						const float inner_radius = range * std::tan(DEG2RAD(inner_deg));
						draw_projected_ellipse(draw_list, end_center, local_right * inner_radius, local_up * inner_radius, color, 1.0f, 40);
					}
				}
				else if (shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK)
				{
					draw_projected_ellipse(draw_list, point.position, local_right * (width * 0.5f), local_up * (height * 0.5f), color, selected ? 2.2f : 1.2f, 56);
					draw_projected_arrow(draw_list, point.position, point.position + dir * std::max(8.0f, range * 0.22f), color, selected ? 2.0f : 1.2f);
					if (selected) draw_projected_line(draw_list, point.position - local_right * width * 0.5f, point.position + local_right * width * 0.5f, muted_color, 1.0f);
				}
				else if (shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT)
				{
					const Vector rx = local_right * (width * 0.5f);
					const Vector uy = local_up * (height * 0.5f);
					const Vector corners[] = { point.position - rx - uy, point.position + rx - uy, point.position + rx + uy, point.position - rx + uy };
					for (int i = 0; i < 4; ++i) draw_projected_line(draw_list, corners[i], corners[(i + 1) % 4], color, selected ? 2.2f : 1.2f);
					draw_projected_arrow(draw_list, point.position, point.position + dir * std::max(8.0f, range * 0.22f), color, selected ? 2.0f : 1.2f);
					if (selected) { draw_projected_line(draw_list, corners[0], corners[2], muted_color, 0.9f); draw_projected_line(draw_list, corners[1], corners[3], muted_color, 0.9f); }
				}
				else if (shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE)
				{
					// The native Remix cylinder uses the authored direction as its longitudinal axis.
					// Keep the editor cage aligned with that exact runtime representation.
					const Vector a = point.position - dir * (length * 0.5f);
					const Vector b = point.position + dir * (length * 0.5f);
					draw_projected_line(draw_list, a, b, color, selected ? 4.0f : 2.5f);
					ImGui::Draw3DCircle(draw_list, a, dir, width * 0.5f, false, muted_color, 1.1f, 28);
					ImGui::Draw3DCircle(draw_list, b, dir, width * 0.5f, false, muted_color, 1.1f, 28);
				}
				else if (shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT)
				{
					const float icon_radius = std::max(2.0f, width * 0.5f);
					draw_projected_ellipse(draw_list, point.position, local_right * icon_radius, local_up * icon_radius, color, selected ? 2.2f : 1.2f, 44);
					for (int i = -2; i <= 2; ++i)
					{
						const Vector offset = local_right * (static_cast<float>(i) * icon_radius * 0.45f);
						draw_projected_arrow(draw_list, point.position + offset - dir * range * 0.18f,
							point.position + offset + dir * range * 0.35f, color, selected ? 1.8f : 1.0f, 6.0f);
					}
				}

				if (selected && point.light_rig_mode != map_settings::remix_light_settings_s::LIGHT_RIG_MODE_LEGACY)
				{
					const float lobe_range = std::max(12.0f, range * 0.55f);
					const Vector lobe_center = point.position + dir * lobe_range;
					const float lobe_radius = std::max(1.0f, lobe_range * std::tan(DEG2RAD(std::clamp(point.degrees, 2.0f, 78.0f))) * 0.55f);
					draw_projected_ellipse(draw_list, lobe_center, local_right * lobe_radius,
						local_up * lobe_radius * std::max(0.2f, point.ies_emulation_aspect), muted_color, 1.0f, 36);
				}
			}
		}

		void commit_active_editor_light_to_selection(remix_lights* lights, map_settings::remix_light_settings_s* selection)
		{
			if (!lights || !selection) {
				return;
			}

			if (const auto edit_light = lights->get_first_active_light(); edit_light) {
				*selection = build_current_edit_light_def(edit_light);
			}
		}

		void rebuild_editor_light_preview(remix_lights* lights,
			std::vector<map_settings::remix_light_settings_s>& map_lights,
			map_settings::remix_light_settings_s* selection,
			const bool preview_all_lights)
		{
			if (!lights) {
				return;
			}

			lights->destroy_and_clear_all_active_lights();

			if (selection && !selection->points.empty()) {
				lights->add_single_map_setting_light_for_editing(selection);
			}

			if (!preview_all_lights) {
				return;
			}

			for (auto& def : map_lights)
			{
				if (&def == selection || !def.enabled || def.points.empty()) {
					continue;
				}

				lights->add_single_map_setting_light_for_editing(&def);
			}
		}


		void snap_map_light_positions(map_settings::remix_light_settings_s& def, const float raw_step, const bool all_points)
		{
			if (def.points.empty()) return;
			const float step = std::max(0.001f, raw_step);
			auto snapped_position = [step](const Vector& position)
			{
				return Vector(
					std::round(position.x / step) * step,
					std::round(position.y / step) * step,
					std::round(position.z / step) * step);
			};
			if (all_points)
			{
				const Vector delta = snapped_position(def.points.front().position) - def.points.front().position;
				for (auto& point : def.points) point.position += delta;
			}
			else
			{
				def.points.front().position = snapped_position(def.points.front().position);
			}
		}

		Vector light_editor_world_axis(const int axis)
		{
			switch (axis)
			{
			case 1: return Vector(1.0f, 0.0f, 0.0f);
			case 2: return Vector(0.0f, 1.0f, 0.0f);
			case 3: return Vector(0.0f, 0.0f, 1.0f);
			default: return Vector();
			}
		}

		float light_editor_point_segment_distance_sqr(const ImVec2& point, const ImVec2& start, const ImVec2& end)
		{
			const float vx = end.x - start.x;
			const float vy = end.y - start.y;
			const float length_sqr = vx * vx + vy * vy;
			if (length_sqr <= 0.0001f)
			{
				const float dx = point.x - start.x;
				const float dy = point.y - start.y;
				return dx * dx + dy * dy;
			}
			const float t = std::clamp(((point.x - start.x) * vx + (point.y - start.y) * vy) / length_sqr, 0.0f, 1.0f);
			const float px = start.x + vx * t;
			const float py = start.y + vy * t;
			const float dx = point.x - px;
			const float dy = point.y - py;
			return dx * dx + dy * dy;
		}

		bool light_editor_key_pressed_once(const int vk)
		{
			static bool previous[256] = {};
			if (vk < 0 || vk >= 256) return false;
			const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
			const bool pressed = down && !previous[vk];
			previous[vk] = down;
			return pressed;
		}

		bool light_editor_key_repeat(const int vk)
		{
			static bool previous[256] = {};
			static double next_repeat[256] = {};
			if (vk < 0 || vk >= 256) return false;
			const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
			const double now = ImGui::GetTime();
			bool fire = false;
			if (down && !previous[vk])
			{
				fire = true;
				next_repeat[vk] = now + 0.28;
			}
			else if (down && now >= next_repeat[vk])
			{
				fire = true;
				next_repeat[vk] = now + 0.045;
			}
			if (!down) next_repeat[vk] = 0.0;
			previous[vk] = down;
			return fire;
		}

		void clear_generated_map_light_identity(map_settings::remix_light_settings_s& def)
		{
			def.generated_map_light = false;
			def.generated_source_index = -1;
			def.generated_hammer_id = -1;
			def.generated_runtime_entity_index = -1;
			def.generated_runtime_kind.clear();
			def.generated_targetname.clear();
			def.generated_classname.clear();
			def.generated_source_kind.clear();
			def.generated_source_style = 0;
			def.generated_source_owner = -1;
			def.generated_source_key = 0;
			def.generated_source_capture_id = 0u;
			def.generated_source_transient = false;
			def.generated_source_live_link = false;
			def.persistent_map_light_id.clear();
			def.persistent_map_light_managed = false;
			def.persistent_map_light_from_file = false;
		}

		void apply_animation_to_map_light_def(map_settings::remix_light_settings_s& def,
			const std::string& animation, float duration, float speed, float variation,
			Vector axis, float degrees, float phase)
		{
			if (def.points.empty()) return;
			migrate_legacy_light_animation_metadata(def);
			auto base = def.points.front();
			base.timepoint = 0.0f;
			const auto name = normalize_light_animation_name(animation);
			if (is_light_movement_animation_name(name))
			{
				def.movement_animation = canonical_light_movement_name(name);
				def.movement_animation_duration = std::max(0.05f, duration);
				def.movement_animation_speed = std::max(0.05f, speed);
				def.movement_animation_axis = normalize_or_ui(axis, Vector(0.0f, 0.0f, 1.0f));
				def.movement_animation_degrees = std::clamp(degrees, 0.0f, 1440.0f);
				def.movement_animation_phase = phase;
			}
			else
			{
				def.property_animation = add_light_animation_is_static_name(name) ? "stable" : name;
				def.property_animation_duration = std::max(0.05f, duration);
				def.property_animation_speed = std::max(0.05f, speed);
				def.property_animation_variation = std::clamp(variation, 0.0f, 1.0f);
			}
			rebuild_split_light_animation(def, &base);
		}

		bool handle_light_editor_keyboard(remix_lights* lights,
			std::vector<map_settings::remix_light_settings_s>& map_lights,
			map_settings::remix_light_settings_s*& selection,
			bool& reset_point_selection)
		{
			const auto im = imgui::get();
			if (!im || !im->m_light_editor_keyboard_controls || !selection || selection->points.empty()) return false;
			const auto& io = ImGui::GetIO();
			if (io.WantTextInput || ImGui::IsAnyItemActive()) return false;

			bool changed = false;
			if (light_editor_key_pressed_once('W')) im->m_light_editor_transform_mode = 0;
			if (light_editor_key_pressed_once('E')) im->m_light_editor_transform_mode = 1;
			if (light_editor_key_pressed_once('R')) im->m_light_editor_transform_mode = 2;
			if (light_editor_key_pressed_once('T')) im->m_light_editor_transform_mode = 3;
			if (light_editor_key_pressed_once('X')) im->m_light_editor_axis_constraint = im->m_light_editor_axis_constraint == 1 ? 0 : 1;
			if (light_editor_key_pressed_once('Y')) im->m_light_editor_axis_constraint = im->m_light_editor_axis_constraint == 2 ? 0 : 2;
			if (light_editor_key_pressed_once('Z')) im->m_light_editor_axis_constraint = im->m_light_editor_axis_constraint == 3 ? 0 : 3;
			if (light_editor_key_pressed_once('C')) im->m_light_editor_axis_constraint = 0;

			const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
			const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			if (ctrl && light_editor_key_pressed_once('S'))
			{
				commit_active_editor_light_to_selection(lights, selection);
				if (dynamic_lighting::get_light_editor_imported_count() > 0u) {
					dynamic_lighting::save_light_editor_map_lights_to_config();
				}
			}
			if (light_editor_key_pressed_once(VK_SPACE))
			{
				selection->enabled = !selection->enabled;
				rebuild_editor_light_preview(lights, map_lights, selection, im->m_light_editor_preview_all);
				changed = true;
			}

			const int left = light_editor_key_repeat(VK_LEFT) ? 1 : 0;
			const int right = light_editor_key_repeat(VK_RIGHT) ? 1 : 0;
			const int up = light_editor_key_repeat(VK_UP) ? 1 : 0;
			const int down = light_editor_key_repeat(VK_DOWN) ? 1 : 0;
			const int page_up = light_editor_key_repeat(VK_PRIOR) ? 1 : 0;
			const int page_down = light_editor_key_repeat(VK_NEXT) ? 1 : 0;
			if (!(left || right || up || down || page_up || page_down)) return changed;

			float multiplier = shift ? 8.0f : 1.0f;
			if (ctrl) multiplier *= 0.25f;
			const bool all_points = im->m_light_editor_drag_whole_light;
			const Vector constrained_axis = light_editor_world_axis(im->m_light_editor_axis_constraint);
			if (im->m_light_editor_transform_mode == 0)
			{
				Vector delta = {};
				const float step = std::max(0.001f, im->m_light_editor_keyboard_step) * multiplier;
				if (im->m_light_editor_axis_constraint > 0)
				{
					const float sign = static_cast<float>((right + up + page_up) - (left + down + page_down));
					delta = constrained_axis * (step * sign);
				}
				else
				{
					Vector view_origin = {}, view_forward = {}, view_right = {}, view_up = {};
					if (game::get_current_view_basis(view_origin, view_forward, view_right, view_up))
					{
						delta += view_right * (step * static_cast<float>(right - left));
						delta += view_up * (step * static_cast<float>(up - down));
						delta += view_forward * (step * static_cast<float>(page_up - page_down));
					}
				}
				if (delta.LengthSqr() > 0.0f)
				{
					if (all_points) translate_map_light_def(*selection, delta);
					else selection->points.front().position += delta;
					if (im->m_light_editor_snap) snap_map_light_positions(*selection, im->m_light_editor_snap_step, all_points);
					changed = true;
				}
			}
			else if (im->m_light_editor_transform_mode == 1)
			{
				const float angle = std::max(0.1f, im->m_light_editor_angle_step) * multiplier;
				auto rotate_points = [&](const Vector& axis, const float signed_angle)
				{
					auto apply = [&](map_settings::remix_light_settings_s::point_s& point)
					{
						point.direction = rotate_axis_ui(point.direction, axis, signed_angle);
						point.use_shaping = true;
						point.degrees = std::clamp(point.degrees <= 0.0f ? 35.0f : point.degrees, 1.0f, 179.0f);
					};
					if (all_points) for (auto& point : selection->points) apply(point); else apply(selection->points.front());
				};
				if (im->m_light_editor_axis_constraint > 0)
				{
					const float sign = static_cast<float>((right + up + page_up) - (left + down + page_down));
					if (sign != 0.0f) { rotate_points(constrained_axis, angle * sign); changed = true; }
				}
				else
				{
					Vector view_origin = {}, view_forward = {}, view_right_axis = {}, view_up_axis = {};
					if (game::get_current_view_basis(view_origin, view_forward, view_right_axis, view_up_axis))
					{
						if (left || right) { rotate_points(view_up_axis, angle * static_cast<float>(right - left)); changed = true; }
						if (up || down) { rotate_points(view_right_axis, angle * static_cast<float>(down - up)); changed = true; }
						if (page_up || page_down) { rotate_points(view_forward, angle * static_cast<float>(page_up - page_down)); changed = true; }
					}
				}
			}
			else if (im->m_light_editor_transform_mode == 2)
			{
				const float radius_delta = std::max(0.01f, im->m_light_editor_keyboard_step * 0.25f) * multiplier *
					static_cast<float>((right + up + page_up) - (left + down + page_down));
				if (radius_delta != 0.0f)
				{
					apply_radius_delta_to_light_def(*selection, radius_delta, all_points);
					changed = true;
				}
			}
			else
			{
				const float intensity_delta = std::max(0.01f, im->m_light_editor_keyboard_step * 0.25f) * multiplier *
					static_cast<float>((right + up + page_up) - (left + down + page_down));
				if (intensity_delta != 0.0f)
				{
					apply_radiance_delta_to_light_def(*selection, intensity_delta, all_points);
					changed = true;
				}
			}

			if (changed)
			{
				sync_selected_map_light_to_runtime(lights, selection, selection, map_lights, im->m_light_editor_preview_all);
				reset_point_selection = false;
			}
			return changed;
		}

		bool draw_all_authored_light_gizmos(remix_lights* lights,
			std::vector<map_settings::remix_light_settings_s>& map_lights,
			map_settings::remix_light_settings_s*& selection,
			bool& reset_point_selection)
		{
			const auto im = imgui::get();
			if (!im) return false;

			if (!light_def_pointer_in_vector(map_lights, selection))
			{
				selection = map_lights.empty() ? nullptr : &map_lights.front();
				reset_point_selection = true;
			}

			static map_settings::remix_light_settings_s* dragging_light = nullptr;
			static map_settings::remix_light_settings_s* context_light = nullptr;
			static map_settings::remix_light_settings_s drag_start_def = {};
			static int dragging_handle = 0;
			static Vector drag_start_center = {};
			static Vector drag_start_hit = {};
			static Vector drag_plane_normal = {};
			static Vector drag_start_plane_vector = {};
			static float drag_start_axis_parameter = 0.0f;
			static float drag_start_mouse_radius = 1.0f;
			static ImVec2 drag_start_mouse = {};
			static ImVec2 drag_start_screen_center = {};
			static Vector pending_spawn_position = {};
			static bool pending_spawn_valid = false;
			static bool pending_spawn_surface_hit = false;
			static ImVec2 context_popup_anchor = {};
			static ImVec2 create_popup_anchor = {};

			if (!light_def_pointer_in_vector(map_lights, dragging_light))
			{
				dragging_light = nullptr;
				dragging_handle = 0;
			}
			if (!light_def_pointer_in_vector(map_lights, context_light)) context_light = nullptr;

			bool changed = false;
			const auto& io = ImGui::GetIO();
			const ImVec2 mouse_pos = io.MousePos;
			// The main Compat window can cover most of the viewport. Blocking interaction
			// on AnyWindow made the gizmo impossible to grab even when the pointer was not
			// over a control. Only active/hovered widgets and text input own the mouse.
			const bool hovering_ui_control = ImGui::GetHoveredID() != 0 || ImGui::IsAnyItemActive() || io.WantTextInput;
			const bool can_mouse_edit = im->m_light_editor_mouse_drag && !hovering_ui_control;
			Vector stable_view_origin = {};
			Vector stable_view_forward = {};
			Vector stable_view_right = {};
			Vector stable_view_up = {};
			const bool have_stable_view = game::get_current_view_basis(
				stable_view_origin, stable_view_forward, stable_view_right, stable_view_up);
			const bool have_view_origin = have_stable_view || game::get_current_view_origin_safe(stable_view_origin);
			const Vector* view_origin = have_view_origin ? &stable_view_origin : nullptr;
			const Vector* view_forward = have_stable_view ? &stable_view_forward : nullptr;

			struct candidate_s
			{
				map_settings::remix_light_settings_s* def = nullptr;
				Vector center = {};
				Vector screen = {};
				float distance = 0.0f;
				float alpha = 1.0f;
				size_t index = 0u;
				bool selected = false;
			};

			std::vector<candidate_s> candidates;
			bool selected_projection_failed = false;
			candidates.reserve(std::min<size_t>(map_lights.size(), static_cast<size_t>(std::max(1, im->m_light_editor_max_visible_gizmos))));
			const float max_visibility_distance = std::max(1.0f, im->m_light_editor_gizmo_visibility_distance);
			for (size_t i = 0u; i < map_lights.size(); ++i)
			{
				auto& def = map_lights[i];
				if (def.points.empty()) continue;
				const bool selected = selection == &def;
				if (!selected && !im->m_light_editor_gizmos_all) continue;
				const Vector center = map_light_def_center(def);
				const float distance = view_origin ? view_origin->DistTo(center) : 0.0f;
				if ((!selected || !im->m_light_editor_selected_always_visible) && distance > max_visibility_distance) continue;
				Vector screen;
				if (!common::imgui::world2screen(center, screen))
				{
					selected_projection_failed |= selected;
					continue;
				}
				candidates.push_back({ &def, center, screen, distance, selected ? 1.0f : light_editor_visibility_alpha(distance, im), i, selected });
			}
			std::stable_sort(candidates.begin(), candidates.end(), [](const candidate_s& a, const candidate_s& b)
			{
				if (a.selected != b.selected) return a.selected;
				return a.distance < b.distance;
			});
			const size_t visible_limit = static_cast<size_t>(std::clamp(im->m_light_editor_max_visible_gizmos, 1, 4096));
			if (candidates.size() > visible_limit) candidates.resize(visible_limit);

			map_settings::remix_light_settings_s* hovered_light = nullptr;
			float hovered_dist_sqr = FLT_MAX;
			const float pick_radius = std::clamp(im->m_light_editor_gizmo_pick_radius, 4.0f, 80.0f);
			const float pick_radius_sqr = pick_radius * pick_radius;
			for (const auto& candidate : candidates)
			{
				auto& def = *candidate.def;
				draw_authored_light_shape_gizmo(def, candidate.selected, candidate.distance, candidate.alpha);
				const auto& first_point = def.points.front();
				const ImU32 color = light_debug_color_u32(first_point, candidate.selected, def.enabled, candidate.alpha);
				const ImVec2 p(candidate.screen.x, candidate.screen.y);
				const float marker_radius = candidate.selected ? 8.0f : 5.5f;
				const int shape = resolved_light_authoring_shape(first_point);
				auto* background = ImGui::GetForegroundDrawList();
				switch (shape)
				{
				case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT:
					background->AddTriangleFilled(p + ImVec2(0.0f, -marker_radius), p + ImVec2(marker_radius, marker_radius), p + ImVec2(-marker_radius, marker_radius), color); break;
				case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK:
					background->AddCircle(p, marker_radius, color, 24, candidate.selected ? 3.0f : 2.0f); break;
				case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT:
					background->AddRectFilled(p - ImVec2(marker_radius, marker_radius * 0.72f), p + ImVec2(marker_radius, marker_radius * 0.72f), color, 1.5f); break;
				case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE:
					background->AddLine(p - ImVec2(marker_radius, 0.0f), p + ImVec2(marker_radius, 0.0f), color, candidate.selected ? 5.0f : 3.0f); break;
				case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT:
					background->AddQuadFilled(p + ImVec2(0.0f, -marker_radius), p + ImVec2(marker_radius, 0.0f), p + ImVec2(0.0f, marker_radius), p + ImVec2(-marker_radius, 0.0f), color); break;
				default:
					background->AddCircleFilled(p, marker_radius, color); break;
				}
				if (candidate.selected) background->AddCircle(p, pick_radius, ImGui::GetColorU32(ImGuiCol_Text), 28, 2.0f);

				if (im->m_debugvis_light_labels && (candidate.selected || candidate.distance <= std::max(1.0f, im->m_light_editor_label_visibility_distance)))
				{
					const char* group = def.group.empty() ? "nogroup" : def.group.c_str();
					const char* comment = def.comment.empty() ? "light" : def.comment.c_str();
					const auto label = utils::va("#%zu %s | %s | %s | pts %zu | R %.2f | I %.1f | %.0fu",
						candidate.index, group, comment, light_authoring_shape_name(shape), def.points.size(), first_point.radius,
						first_point.radiance_scalar, candidate.distance);
					background->AddText(p + ImVec2(12.0f, -10.0f), color, label);
				}

				const float dx = p.x - mouse_pos.x;
				const float dy = p.y - mouse_pos.y;
				const float dist_sqr = dx * dx + dy * dy;
				if (dist_sqr <= pick_radius_sqr && dist_sqr < hovered_dist_sqr)
				{
					hovered_light = &def;
					hovered_dist_sqr = dist_sqr;
				}
			}

			if (selected_projection_failed)
			{
				auto* foreground = ImGui::GetForegroundDrawList();
				foreground->AddText(ImVec2(18.0f, 72.0f), ImGui::GetColorU32(ImVec4(1.0f, 0.45f, 0.15f, 1.0f)),
					"Light gizmo projection unavailable - camera fallback active / light may be behind view");
			}

			const Vector axis_world[4] = { Vector(), Vector(1.0f, 0.0f, 0.0f), Vector(0.0f, 1.0f, 0.0f), Vector(0.0f, 0.0f, 1.0f) };
			const ImU32 axis_colors[4] = {
				0u,
				ImGui::GetColorU32(ImVec4(1.00f, 0.22f, 0.18f, 1.0f)),
				ImGui::GetColorU32(ImVec4(0.18f, 1.00f, 0.30f, 1.0f)),
				ImGui::GetColorU32(ImVec4(0.20f, 0.52f, 1.00f, 1.0f))
			};
			const char* axis_names[4] = { "", "X", "Y", "Z" };
			int hovered_handle = 0;
			float hovered_handle_dist_sqr = FLT_MAX;
			auto consider_handle = [&](const int id, const float distance_sqr)
			{
				if (distance_sqr < hovered_handle_dist_sqr)
				{
					hovered_handle = id;
					hovered_handle_dist_sqr = distance_sqr;
				}
			};

			ImVec2 axis_screen_start[4] = {};
			ImVec2 axis_screen_end[4] = {};
			bool axis_visible[4] = {};
			Vector selected_center = {};
			Vector selected_center_screen = {};
			float selected_world_length = 0.0f;
			if (selection && !selection->points.empty() && view_origin && common::imgui::world2screen(map_light_def_center(*selection), selected_center_screen))
			{
				selected_center = map_light_def_center(*selection);
				const float distance = std::max(32.0f, view_origin->DistTo(selected_center));
				const float pixel_length = std::clamp(im->m_light_editor_gizmo_axis_length, 20.0f, 160.0f);
				selected_world_length = distance * pixel_length / 700.0f;
				const ImVec2 start(selected_center_screen.x, selected_center_screen.y);
				auto* foreground = ImGui::GetForegroundDrawList();

				if (im->m_light_editor_transform_mode == 0)
				{
					if (im->m_light_editor_axis_gizmo)
					{
						for (int axis = 1; axis <= 3; ++axis)
						{
							Vector end_screen;
							if (!common::imgui::world2screen(selected_center + axis_world[axis] * selected_world_length, end_screen)) continue;
							axis_visible[axis] = true;
							axis_screen_start[axis] = start;
							axis_screen_end[axis] = ImVec2(end_screen.x, end_screen.y);
							const bool active = dragging_handle == axis || im->m_light_editor_axis_constraint == axis;
							foreground->AddLine(start, axis_screen_end[axis], axis_colors[axis], active ? 5.0f : 3.0f);
							ImVec2 d = axis_screen_end[axis] - start;
							const float len = std::sqrt(d.x * d.x + d.y * d.y);
							if (len > 0.001f)
							{
								d.x /= len; d.y /= len;
								const ImVec2 n(-d.y, d.x);
								const ImVec2 base = axis_screen_end[axis] - d * 12.0f;
								foreground->AddTriangleFilled(axis_screen_end[axis], base + n * 6.0f, base - n * 6.0f, axis_colors[axis]);
							}
							foreground->AddText(axis_screen_end[axis] + ImVec2(7.0f, -7.0f), axis_colors[axis], axis_names[axis]);
							const float hit = light_editor_point_segment_distance_sqr(mouse_pos, start, axis_screen_end[axis]);
							if (hit <= 100.0f) consider_handle(axis, hit);
						}
					}

					if (im->m_light_editor_plane_gizmo)
					{
						struct plane_handle_s { int id; int a; int b; ImU32 color; };
						const plane_handle_s planes[] = {
							{ 4, 1, 2, ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.15f, 0.65f)) },
							{ 5, 1, 3, ImGui::GetColorU32(ImVec4(1.0f, 0.25f, 0.85f, 0.65f)) },
							{ 6, 2, 3, ImGui::GetColorU32(ImVec4(0.20f, 0.95f, 0.95f, 0.65f)) },
						};
						for (const auto& plane : planes)
						{
							Vector plane_screen;
							const Vector world = selected_center + (axis_world[plane.a] + axis_world[plane.b]) * (selected_world_length * 0.24f);
							if (!common::imgui::world2screen(world, plane_screen)) continue;
							const ImVec2 hp(plane_screen.x, plane_screen.y);
							foreground->AddRectFilled(hp - ImVec2(5.0f, 5.0f), hp + ImVec2(5.0f, 5.0f), plane.color, 1.5f);
							const float dx = mouse_pos.x - hp.x, dy = mouse_pos.y - hp.y;
							const float hit = dx * dx + dy * dy;
							if (hit <= 121.0f) consider_handle(plane.id, hit);
						}
					}
					if (im->m_light_editor_center_handle)
					{
						const ImU32 center_color = ImGui::GetColorU32(ImVec4(0.92f, 0.92f, 0.92f, 0.9f));
						foreground->AddRectFilled(start - ImVec2(5.0f, 5.0f), start + ImVec2(5.0f, 5.0f), center_color, 1.0f);
						const float dx = mouse_pos.x - start.x, dy = mouse_pos.y - start.y;
						const float hit = dx * dx + dy * dy;
						if (hit <= 100.0f) consider_handle(7, hit);
					}
				}
				else if (im->m_light_editor_transform_mode == 1)
				{
					auto draw_rotation_ring = [&](const int axis, const float radius)
					{
						ImVec2 first = {};
						ImVec2 previous = {};
						bool have_previous = false;
						for (int i = 0; i <= 64; ++i)
						{
							const float angle = static_cast<float>(i) / 64.0f * M_PI * 2.0f;
							Vector a;
							if (axis == 1) { a = Vector(0.0f, std::cos(angle), std::sin(angle)); }
							else if (axis == 2) { a = Vector(std::cos(angle), 0.0f, std::sin(angle)); }
							else { a = Vector(std::cos(angle), std::sin(angle), 0.0f); }
							Vector screen;
							if (!common::imgui::world2screen(selected_center + a * radius, screen)) { have_previous = false; continue; }
							const ImVec2 current(screen.x, screen.y);
							if (!have_previous) first = current;
							else
							{
								foreground->AddLine(previous, current, axis_colors[axis], dragging_handle == axis + 7 ? 4.5f : 2.5f);
								const float hit = light_editor_point_segment_distance_sqr(mouse_pos, previous, current);
								if (hit <= 81.0f) consider_handle(axis + 7, hit);
							}
							previous = current;
							have_previous = true;
						}
					};
					draw_rotation_ring(1, selected_world_length * 0.68f);
					draw_rotation_ring(2, selected_world_length * 0.76f);
					draw_rotation_ring(3, selected_world_length * 0.84f);
					foreground->AddText(start + ImVec2(12.0f, 12.0f), ImGui::GetColorU32(ImGuiCol_Text), "Rotate rings");
				}
				else if (im->m_light_editor_transform_mode == 2)
				{
					const float ring_radius = std::clamp(im->m_light_editor_gizmo_axis_length * 0.72f, 24.0f, 100.0f);
					const ImU32 shape_color = ImGui::GetColorU32(ImVec4(1.0f, 0.72f, 0.18f, 1.0f));
					foreground->AddCircle(start, ring_radius, shape_color, 48, dragging_handle == 11 ? 4.0f : 2.5f);
					const ImVec2 hp = start + ImVec2(ring_radius, 0.0f);
					foreground->AddCircleFilled(hp, 7.0f, shape_color);
					const float dx = mouse_pos.x - hp.x, dy = mouse_pos.y - hp.y;
					const float hit = dx * dx + dy * dy;
					if (hit <= 144.0f) consider_handle(11, hit);
					foreground->AddText(start + ImVec2(12.0f, 12.0f), shape_color, "Radius / shape");
				}
				else
				{
					const ImU32 intensity_color = ImGui::GetColorU32(ImVec4(1.0f, 0.92f, 0.30f, 1.0f));
					const ImVec2 hp = start + ImVec2(0.0f, -std::clamp(im->m_light_editor_gizmo_axis_length, 36.0f, 110.0f));
					foreground->AddLine(start, hp, intensity_color, dragging_handle == 12 ? 5.0f : 3.0f);
					foreground->AddQuadFilled(hp + ImVec2(0.0f, -8.0f), hp + ImVec2(8.0f, 0.0f), hp + ImVec2(0.0f, 8.0f), hp + ImVec2(-8.0f, 0.0f), intensity_color);
					const float dx = mouse_pos.x - hp.x, dy = mouse_pos.y - hp.y;
					const float hit = dx * dx + dy * dy;
					if (hit <= 196.0f) consider_handle(12, hit);
					foreground->AddText(hp + ImVec2(12.0f, -8.0f), intensity_color, utils::va("Intensity %.2f", selection->points.front().radiance_scalar));
				}
			}

			const bool world_context_click = can_mouse_edit &&
				(ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle));
			if (world_context_click)
			{
				if (hovered_light)
				{
					commit_active_editor_light_to_selection(lights, selection);
					selection = hovered_light;
					context_light = hovered_light;
					reset_point_selection = true;
					rebuild_editor_light_preview(lights, map_lights, selection, im->m_light_editor_preview_all);
					changed = true;
					context_popup_anchor = mouse_pos + ImVec2(8.0f, 8.0f);
					ImGui::OpenPopup("##LightEditorWorldContextPopup");
				}
				else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
				{
					// Empty-space MMB remains the explicit create gesture; RMB is kept free for camera control.
					pending_spawn_position = light_editor_resolve_spawn_position(
						std::clamp(im->m_light_editor_spawn_distance, 16.0f, 8192.0f), &pending_spawn_surface_hit);
					pending_spawn_valid = true;
					create_popup_anchor = mouse_pos + ImVec2(8.0f, 8.0f);
					ImGui::OpenPopup("##LightEditorWorldCreatePopup");
				}
			}

			auto pin_popup_to_anchor = [](const ImVec2& requested_anchor)
			{
				const ImGuiViewport* viewport = ImGui::GetMainViewport();
				const ImVec2 popup_size = ImGui::GetWindowSize();
				const ImVec2 work_min = viewport ? viewport->WorkPos + ImVec2(4.0f, 4.0f) : ImVec2(4.0f, 4.0f);
				const ImVec2 work_max = viewport
					? viewport->WorkPos + viewport->WorkSize - popup_size - ImVec2(4.0f, 4.0f)
					: ImGui::GetIO().DisplaySize - popup_size - ImVec2(4.0f, 4.0f);
				const ImVec2 pinned(
					std::clamp(requested_anchor.x, work_min.x, std::max(work_min.x, work_max.x)),
					std::clamp(requested_anchor.y, work_min.y, std::max(work_min.y, work_max.y)));
				ImGui::SetWindowPos(pinned, ImGuiCond_Always);
			};

			// Capture the click position once. The old fixed 360 px clamp moved the menu
			// upward even when there was enough space below the cursor.
			ImGui::SetNextWindowPos(context_popup_anchor, ImGuiCond_Appearing);
			if (ImGui::BeginPopup("##LightEditorWorldContextPopup", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
			{
				pin_popup_to_anchor(context_popup_anchor);
				if (!context_light) ImGui::TextDisabled("Light unavailable");
				else
				{
					const auto compact_title = compact_light_menu_text(context_light->comment.empty() ? "Selected light" : context_light->comment);
					const auto compact_group = compact_light_menu_text(context_light->group.empty() ? "none" : context_light->group, 20u);
					ImGui::TextUnformatted(compact_title.c_str());
					ImGui::TextDisabled("Group: %s", compact_group.c_str());
					if (ImGui::Checkbox("Enabled", &context_light->enabled)) { rebuild_editor_light_preview(lights, map_lights, selection, im->m_light_editor_preview_all); changed = true; }

					if (ImGui::BeginMenu("Rig"))
					{
						const int rig_modes[] = {
							map_settings::remix_light_settings_s::LIGHT_RIG_MODE_LEGACY,
							map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES,
							map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES,
						};
						const int current_shape = context_light->points.empty()
							? map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT
							: resolved_light_authoring_shape(context_light->points.front());
						const bool native_ies_geometry_supported =
							current_shape != map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE &&
							current_shape != map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT;
						for (const int mode : rig_modes)
						{
							const bool selected_mode = !context_light->points.empty() && context_light->points.front().light_rig_mode == mode;
							const bool enabled_mode = mode != map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES || native_ies_geometry_supported;
							if (ImGui::MenuItem(light_rig_mode_ui_name(mode), nullptr, selected_mode, enabled_mode))
							{
								for (auto& point : context_light->points)
								{
									point.light_rig_mode = mode;
									point.ies_emulation = mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES;
									if (point.ies_emulation && point.ies_emulation_samples <= 0) point.ies_emulation_samples = 4;
								}
								sync_selected_map_light_to_runtime(lights, selection, context_light, map_lights, im->m_light_editor_preview_all);
								changed = true;
							}
						}
						if (!native_ies_geometry_supported) ImGui::TextDisabled("Point/Spot, Disc, Rect only.");
						ImGui::EndMenu();
					}

					if (ImGui::BeginMenu("Style"))
					{
						ImGui::TextDisabled("Keeps power/radius");
						ImGui::Separator();
						for (const auto& preset : LIGHT_WORKBENCH_PRESETS)
						{
							if (ImGui::MenuItem(preset.name))
							{
								for (auto& point : context_light->points) apply_light_workbench_preset(point, preset);
								sync_selected_map_light_to_runtime(lights, selection, context_light, map_lights, im->m_light_editor_preview_all);
								changed = true;
							}
						}
						ImGui::EndMenu();
					}

					if (ImGui::BeginMenu("Type"))
					{
						const int shapes[] = {
							map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT,
							map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT,
							map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK,
							map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT,
							map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE,
							map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT,
						};
						for (const int shape : shapes)
						{
							const bool selected_shape = !context_light->points.empty() && resolved_light_authoring_shape(context_light->points.front()) == shape;
							if (ImGui::MenuItem(light_authoring_shape_name(shape), nullptr, selected_shape))
							{
								for (auto& point : context_light->points)
								{
									point.authoring_shape = shape;
									if ((shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE ||
										shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT) &&
										point.light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES)
									{
										point.light_rig_mode = map_settings::remix_light_settings_s::LIGHT_RIG_MODE_LEGACY;
									}
									point.use_shaping = shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT ||
										shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK ||
										shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT ||
										shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT;
									if (point.use_shaping && point.degrees >= 179.0f) point.degrees = 58.0f;
								}
								sync_selected_map_light_to_runtime(lights, selection, context_light, map_lights, im->m_light_editor_preview_all);
								changed = true;
							}
						}
						ImGui::EndMenu();
					}
					if (ImGui::BeginMenu("Animate"))
					{
						if (ImGui::BeginMenu("Output"))
						{
							for (const auto* preset : LIGHT_OUTPUT_ANIMATION_PRESETS)
							{
								if (ImGui::MenuItem(light_animation_compact_label(preset)))
								{
									const auto defaults = add_light_animation_defaults_for(preset);
									apply_animation_to_map_light_def(*context_light, preset, defaults.duration, defaults.speed,
										defaults.variation, defaults.axis, defaults.degrees, defaults.phase);
									sync_selected_map_light_to_runtime(lights, selection, context_light, map_lights, im->m_light_editor_preview_all);
									changed = true;
								}
							}
							ImGui::EndMenu();
						}
						if (ImGui::BeginMenu("Movement"))
						{
							for (const auto* preset : LIGHT_MOVEMENT_ANIMATION_PRESETS)
							{
								if (ImGui::MenuItem(light_animation_compact_label(preset)))
								{
									const auto defaults = add_light_animation_defaults_for(preset);
									migrate_legacy_light_animation_metadata(*context_light);
									auto base = context_light->points.front();
									context_light->movement_animation = canonical_light_movement_name(preset);
									context_light->movement_animation_duration = defaults.duration;
									context_light->movement_animation_speed = defaults.speed;
									context_light->movement_animation_axis = defaults.axis;
									context_light->movement_animation_degrees = defaults.degrees;
									context_light->movement_animation_phase = defaults.phase;
									rebuild_split_light_animation(*context_light, &base);
									sync_selected_map_light_to_runtime(lights, selection, context_light, map_lights, im->m_light_editor_preview_all);
									changed = true;
								}
							}
							ImGui::EndMenu();
						}
						ImGui::EndMenu();
					}
					if (ImGui::MenuItem("Edit")) { selection = context_light; reset_point_selection = true; ImGui::CloseCurrentPopup(); }
					if (ImGui::MenuItem("Duplicate"))
					{
						auto duplicate = *context_light;
						duplicate.comment += duplicate.comment.empty() ? "copy" : " copy";
						clear_generated_map_light_identity(duplicate);
						map_lights.emplace_back(std::move(duplicate));
						selection = &map_lights.back(); context_light = nullptr; reset_point_selection = true;
						rebuild_editor_light_preview(lights, map_lights, selection, im->m_light_editor_preview_all);
						changed = true; ImGui::CloseCurrentPopup();
					}
					if (context_light && ImGui::MenuItem("Save"))
					{
						commit_active_editor_light_to_selection(lights, selection);
						dynamic_lighting::save_light_editor_map_lights_to_config(); ImGui::CloseCurrentPopup();
					}
					if (context_light && ImGui::MenuItem("Delete"))
					{
						const auto erase_it = std::find_if(map_lights.begin(), map_lights.end(), [&](const auto& def) { return &def == context_light; });
						if (erase_it != map_lights.end()) map_lights.erase(erase_it);
						selection = map_lights.empty() ? nullptr : &map_lights.front(); context_light = nullptr; reset_point_selection = true;
						rebuild_editor_light_preview(lights, map_lights, selection, im->m_light_editor_preview_all);
						changed = true; ImGui::CloseCurrentPopup();
					}
				}
				ImGui::EndPopup();
			}

			ImGui::SetNextWindowPos(create_popup_anchor, ImGuiCond_Appearing);
			if (ImGui::BeginPopup("##LightEditorWorldCreatePopup", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
			{
				pin_popup_to_anchor(create_popup_anchor);
				ImGui::TextDisabled(pending_spawn_surface_hit ? "Create on surface" : "Create at ray limit");
				ImGui::SetNextItemWidth(160.0f);
				if (ImGui::DragFloat("Ray Max", &im->m_light_editor_spawn_distance, 2.0f, 16.0f, 8192.0f, "%.0f"))
				{
					im->m_light_editor_spawn_distance = std::clamp(im->m_light_editor_spawn_distance, 16.0f, 8192.0f);
					pending_spawn_position = light_editor_resolve_spawn_position(im->m_light_editor_spawn_distance, &pending_spawn_surface_hit);
				}
				ImGui::SetNextItemWidth(160.0f);
				if (ImGui::DragFloat("Radius", &im->m_light_editor_spawn_radius, 0.025f, 0.001f, 128.0f, "%.3f")) im->m_light_editor_spawn_radius = std::clamp(im->m_light_editor_spawn_radius, 0.001f, 128.0f);
				ImGui::SetNextItemWidth(160.0f);
				if (ImGui::DragFloat("Power", &im->m_light_editor_spawn_intensity, 0.25f, 0.0f, 100000.0f, "%.1f")) im->m_light_editor_spawn_intensity = std::max(0.0f, im->m_light_editor_spawn_intensity);
				if (pending_spawn_valid) ImGui::TextDisabled("XYZ: %.1f %.1f %.1f", pending_spawn_position.x, pending_spawn_position.y, pending_spawn_position.z);

				const int spawn_shapes[] = {
					map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT,
					map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT,
					map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK,
					map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT,
					map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE,
					map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT,
				};
				for (const int shape : spawn_shapes)
				{
					if (ImGui::Button(light_authoring_shape_name(shape), ImVec2(100.0f, 0.0f)) && pending_spawn_valid)
					{
						commit_active_editor_light_to_selection(lights, selection);
						map_lights.push_back(make_mouse_spawn_light_def(pending_spawn_position, shape));
						selection = &map_lights.back(); reset_point_selection = true;
						rebuild_editor_light_preview(lights, map_lights, selection, im->m_light_editor_preview_all);
						changed = true; ImGui::CloseCurrentPopup(); break;
					}
					if ((shape % 2) == 1 && shape != spawn_shapes[IM_ARRAYSIZE(spawn_shapes) - 1]) ImGui::SameLine();
				}
				ImGui::TextDisabled(pending_spawn_surface_hit ? "Surface trace hit" : "No hit - distance fallback");
				ImGui::EndPopup();
			}

			auto begin_drag = [&](map_settings::remix_light_settings_s* light, const int handle)
			{
				if (!light || light->points.empty()) return;
				if (selection != light)
				{
					commit_active_editor_light_to_selection(lights, selection);
					selection = light; reset_point_selection = true;
					rebuild_editor_light_preview(lights, map_lights, selection, im->m_light_editor_preview_all);
				}
				dragging_light = light;
				dragging_handle = handle;
				drag_start_def = *light;
				drag_start_center = map_light_def_center(*light);
				drag_start_mouse = mouse_pos;
				Vector screen;
				if (common::imgui::world2screen(drag_start_center, screen)) drag_start_screen_center = ImVec2(screen.x, screen.y);
				drag_start_mouse_radius = std::max(1.0f, std::sqrt((mouse_pos.x - drag_start_screen_center.x) * (mouse_pos.x - drag_start_screen_center.x) + (mouse_pos.y - drag_start_screen_center.y) * (mouse_pos.y - drag_start_screen_center.y)));

				Vector ray_origin, ray_dir;
				if (!light_editor_mouse_world_ray(ray_origin, ray_dir)) return;
				if (handle >= 1 && handle <= 3) light_editor_axis_parameter_from_ray(ray_origin, ray_dir, drag_start_center, axis_world[handle], drag_start_axis_parameter);
				else
				{
					if (handle == 4) drag_plane_normal = axis_world[3];
					else if (handle == 5) drag_plane_normal = axis_world[2];
					else if (handle == 6) drag_plane_normal = axis_world[1];
					else if (handle == 7 || handle == 13) drag_plane_normal = view_forward ? *view_forward : Vector(0.0f, 1.0f, 0.0f);
					else if (handle >= 8 && handle <= 10) drag_plane_normal = axis_world[handle - 7];
					if (light_editor_ray_plane_intersection(ray_origin, ray_dir, drag_start_center, drag_plane_normal, drag_start_hit))
					{
						drag_start_plane_vector = drag_start_hit - drag_start_center;
						if (drag_start_plane_vector.LengthSqr() > 0.000001f) drag_start_plane_vector.NormalizeChecked();
					}
				}
			};

			if (can_mouse_edit && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				if (hovered_handle > 0 && selection) begin_drag(selection, hovered_handle);
				else if (hovered_light)
				{
					int body_handle = 7;
					if (io.KeyShift || im->m_light_editor_transform_mode == 2) body_handle = 11;
					else if (io.KeyCtrl || im->m_light_editor_transform_mode == 3) body_handle = 12;
					else if (io.KeyAlt || im->m_light_editor_transform_mode == 1) body_handle = 13;
					begin_drag(hovered_light, body_handle);
				}
			}

			if (can_mouse_edit && (hovered_handle > 0 || hovered_light))
			{
				const char* hint = im->m_light_editor_transform_mode == 0 ? "Move: axes, plane squares or center handle" :
					im->m_light_editor_transform_mode == 1 ? "Rotate: drag colored rings" :
					im->m_light_editor_transform_mode == 2 ? "Shape: drag orange radius handle" : "Intensity: drag yellow handle";
				ImGui::GetForegroundDrawList()->AddText(mouse_pos + ImVec2(14.0f, 14.0f), ImGui::GetColorU32(ImGuiCol_Text), hint);
			}

			auto* wheel_light = hovered_light ? hovered_light : (hovered_handle > 0 ? selection : nullptr);
			if (can_mouse_edit && wheel_light && io.MouseWheel != 0.0f && view_forward && view_origin)
			{
				if (selection != wheel_light) { commit_active_editor_light_to_selection(lights, selection); selection = wheel_light; reset_point_selection = true; }
				const float distance = std::max(32.0f, view_origin->DistTo(map_light_def_center(*wheel_light)));
				const float scale = distance * std::clamp(im->m_light_editor_gizmo_depth_scale, 0.0001f, 0.5f);
				const Vector delta = *view_forward * (io.MouseWheel * scale);
				if (im->m_light_editor_drag_whole_light) translate_map_light_def(*wheel_light, delta); else wheel_light->points.front().position += delta;
				if (im->m_light_editor_snap) snap_map_light_positions(*wheel_light, im->m_light_editor_snap_step, im->m_light_editor_drag_whole_light);
				sync_selected_map_light_to_runtime(lights, selection, wheel_light, map_lights, im->m_light_editor_preview_all);
				changed = true;
			}

			if (!io.MouseDown[0]) { dragging_light = nullptr; dragging_handle = 0; }
			if (im->m_light_editor_mouse_drag && dragging_light && io.MouseDown[0])
			{
				const ImVec2 mouse_delta = mouse_pos - drag_start_mouse;
				const float mouse_distance = std::sqrt(mouse_delta.x * mouse_delta.x + mouse_delta.y * mouse_delta.y);
				if (mouse_distance >= std::max(0.0f, im->m_light_editor_drag_threshold))
				{
					*dragging_light = drag_start_def;
					Vector ray_origin, ray_dir;
					const bool have_ray = light_editor_mouse_world_ray(ray_origin, ray_dir);
					bool transform_applied = false;

					if (dragging_handle >= 1 && dragging_handle <= 3 && have_ray)
					{
						float current_parameter = drag_start_axis_parameter;
						if (light_editor_axis_parameter_from_ray(ray_origin, ray_dir, drag_start_center, axis_world[dragging_handle], current_parameter))
						{
							const Vector delta = axis_world[dragging_handle] * (current_parameter - drag_start_axis_parameter);
							if (im->m_light_editor_drag_whole_light) translate_map_light_def(*dragging_light, delta); else dragging_light->points.front().position += delta;
							transform_applied = true;
						}
					}
					else if (dragging_handle >= 4 && dragging_handle <= 7 && have_ray)
					{
						Vector current_hit;
						if (light_editor_ray_plane_intersection(ray_origin, ray_dir, drag_start_center, drag_plane_normal, current_hit))
						{
							const Vector delta = current_hit - drag_start_hit;
							if (im->m_light_editor_drag_whole_light) translate_map_light_def(*dragging_light, delta); else dragging_light->points.front().position += delta;
							transform_applied = true;
						}
					}
					else if (dragging_handle >= 8 && dragging_handle <= 10 && have_ray)
					{
						Vector current_hit;
						if (light_editor_ray_plane_intersection(ray_origin, ray_dir, drag_start_center, drag_plane_normal, current_hit))
						{
							Vector current_vector = current_hit - drag_start_center;
							if (current_vector.LengthSqr() > 0.000001f && drag_start_plane_vector.LengthSqr() > 0.000001f)
							{
								current_vector.NormalizeChecked();
								Vector axis = axis_world[dragging_handle - 7];
								const float radians = std::atan2(axis.Dot(drag_start_plane_vector.Cross(current_vector)), drag_start_plane_vector.Dot(current_vector));
								float degrees = RAD2DEG(radians);
								if (im->m_light_editor_snap) degrees = std::round(degrees / std::max(0.1f, im->m_light_editor_angle_step)) * std::max(0.1f, im->m_light_editor_angle_step);
								auto apply = [&](map_settings::remix_light_settings_s::point_s& point) { point.direction = rotate_axis_ui(point.direction, axis, degrees); point.use_shaping = true; if (point.degrees >= 179.0f) point.degrees = 58.0f; };
								if (im->m_light_editor_drag_whole_light) for (auto& point : dragging_light->points) apply(point); else apply(dragging_light->points.front());
								transform_applied = true;
							}
						}
					}
					else if (dragging_handle == 11)
					{
						const float current_radius = std::max(1.0f, std::sqrt((mouse_pos.x - drag_start_screen_center.x) * (mouse_pos.x - drag_start_screen_center.x) + (mouse_pos.y - drag_start_screen_center.y) * (mouse_pos.y - drag_start_screen_center.y)));
						const float ratio = std::clamp(current_radius / drag_start_mouse_radius, 0.01f, 100.0f);
						auto apply = [&](map_settings::remix_light_settings_s::point_s& point, const map_settings::remix_light_settings_s::point_s& original)
						{
							const int shape = resolved_light_authoring_shape(original);
							if (shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT || shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT) point.radius = std::max(0.001f, original.radius * ratio);
							else if (shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK || shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT) { point.authoring_width = std::max(0.001f, original.authoring_width * ratio); point.authoring_height = std::max(0.001f, original.authoring_height * ratio); }
							else if (shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE) { point.authoring_width = std::max(0.001f, original.authoring_width * ratio); point.authoring_length = std::max(0.001f, original.authoring_length * ratio); }
							else point.authoring_width = std::max(0.001f, original.authoring_width * ratio);
						};
						if (im->m_light_editor_drag_whole_light) for (size_t i = 0; i < dragging_light->points.size(); ++i) apply(dragging_light->points[i], drag_start_def.points[i]);
						else apply(dragging_light->points.front(), drag_start_def.points.front());
						transform_applied = true;
					}
					else if (dragging_handle == 12)
					{
						const float sensitivity = std::clamp(im->m_light_editor_gizmo_intensity_scale, 0.001f, 100.0f) * 0.02f;
						const float factor = std::exp(-mouse_delta.y * sensitivity);
						auto apply = [&](map_settings::remix_light_settings_s::point_s& point, const map_settings::remix_light_settings_s::point_s& original) { point.radiance_scalar = std::max(0.0f, original.radiance_scalar * factor); };
						if (im->m_light_editor_drag_whole_light) for (size_t i = 0; i < dragging_light->points.size(); ++i) apply(dragging_light->points[i], drag_start_def.points[i]);
						else apply(dragging_light->points.front(), drag_start_def.points.front());
						transform_applied = true;
					}
					else if (dragging_handle == 13)
					{
						const float scale = std::clamp(im->m_light_editor_gizmo_rotate_scale, 0.0001f, 0.2f);
						apply_rotation_delta_to_light_def(*dragging_light, mouse_delta.x * scale, -mouse_delta.y * scale, im->m_light_editor_drag_whole_light);
						transform_applied = true;
					}

					if (transform_applied)
					{
						if (im->m_light_editor_snap && dragging_handle >= 1 && dragging_handle <= 7) snap_map_light_positions(*dragging_light, im->m_light_editor_snap_step, im->m_light_editor_drag_whole_light);
						sync_selected_map_light_to_runtime(lights, selection, dragging_light, map_lights, im->m_light_editor_preview_all);
						changed = true;
					}
				}
			}

			changed |= handle_light_editor_keyboard(lights, map_lights, selection, reset_point_selection);
			return changed;
		}

		void apply_add_light_animation_to_edit_light(
			remix_lights::light* edit_light,
			map_settings::remix_light_settings_s::point_s* base_point,
			const std::string& animation,
			float duration,
			float speed,
			float variation,
			Vector axis,
			float degrees,
			float phase)
		{
			if (!edit_light || !base_point) return;
			migrate_legacy_light_animation_metadata(edit_light->m_def);
			auto base = *base_point;
			base.timepoint = 0.0f;
			const auto name = normalize_light_animation_name(animation);
			if (is_light_movement_animation_name(name))
			{
				edit_light->m_def.movement_animation = canonical_light_movement_name(name);
				edit_light->m_def.movement_animation_duration = std::max(0.05f, duration);
				edit_light->m_def.movement_animation_speed = std::max(0.05f, speed);
				edit_light->m_def.movement_animation_axis = normalize_or_ui(axis, Vector(0.0f, 0.0f, 1.0f));
				edit_light->m_def.movement_animation_degrees = std::clamp(degrees, 0.0f, 1440.0f);
				edit_light->m_def.movement_animation_phase = phase;
			}
			else
			{
				edit_light->m_def.property_animation = add_light_animation_is_static_name(name) ? "stable" : name;
				edit_light->m_def.property_animation_duration = std::max(0.05f, duration);
				edit_light->m_def.property_animation_speed = std::max(0.05f, speed);
				edit_light->m_def.property_animation_variation = std::clamp(variation, 0.0f, 1.0f);
			}
			rebuild_split_light_animation(edit_light->m_def, &base);
			rebuild_edit_light_runtime_from_def(edit_light);
		}

		void draw_light_workbench(remix_lights::light* edit_active_light,
			map_settings::remix_light_settings_s::point_s*& active_point_selection,
			const bool is_static_light_with_single_point)
		{
			(void)is_static_light_with_single_point;

			if (!edit_active_light || !active_point_selection) {
				return;
			}

			auto refresh_light_runtime = [&]()
			{
				const size_t selected_index = ui_light_point_index_from_ptr(edit_active_light, active_point_selection);
				rebuild_edit_light_runtime_from_def(edit_active_light, selected_index);
				active_point_selection = ui_light_point_by_index(edit_active_light, selected_index);
			};

			auto propagate_backend = [&]()
			{
				copy_light_backend_to_all_points(edit_active_light, *active_point_selection);
				refresh_light_runtime();
			};

			ImGui::Spacing(0, 8);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
			ImGui::TableHeaderDropshadow();
			const bool workbench_state = ImGui::CollapsingHeader("Selected Light", ImGuiTreeNodeFlags_DefaultOpen);
			ImGui::PopStyleVar();
			if (!workbench_state) return;

			migrate_legacy_light_animation_metadata(edit_active_light->m_def);
			const bool has_output_animation = !add_light_animation_is_static_name(edit_active_light->m_def.property_animation);
			const bool has_movement_animation = canonical_light_movement_name(edit_active_light->m_def.movement_animation) != "none";
			const bool has_animation = has_output_animation || has_movement_animation || edit_active_light->m_def.points.size() > 1u;
			const std::string animation_summary = has_output_animation && has_movement_animation ? "output + move" :
				(has_output_animation ? "output" : (has_movement_animation ? "movement" : "static"));

			if (ImGui::BeginTable("##light_studio_summary", 5, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); ui_metric_card("Geometry", light_authoring_shape_name(resolved_light_authoring_shape(*active_point_selection)), "Native analytical geometry submitted through the Remix API pNext chain.");
				ImGui::TableNextColumn(); ui_metric_card("Rig", light_rig_mode_ui_name(active_point_selection->light_rig_mode), "Exactly one light rig backend is active for the complete authored light.");
				ImGui::TableNextColumn(); ui_metric_card("Keyframes", std::format("{}", edit_active_light->m_def.points.size()), "Authored points used by the animation interpolator.");
				ImGui::TableNextColumn(); ui_metric_card("Animation", animation_summary, "Output and fixture movement are independent modules.");
				ImGui::TableNextColumn(); ui_metric_card("Runtime", edit_active_light->m_native_ies_status.empty() ? "not evaluated" : edit_active_light->m_native_ies_status, "Result of the most recent light rebuild.");
				ImGui::EndTable();
			}

			ImGui::Spacing(0, 8);
			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			ImGui::SeparatorText(" 1. Light Rigging ");
			ImGui::PopFont();
			ImGui::TextDisabled("Choose one backend for the complete authored light. The selection is propagated to every animation keyframe.");

			const float mode_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
			const int modes[] = {
				map_settings::remix_light_settings_s::LIGHT_RIG_MODE_LEGACY,
				map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES,
				map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES,
			};
			const char* mode_labels[] = { "Analytical", "Native IES", "Fake IES" };
			for (int i = 0; i < 3; ++i)
			{
				if (i) ImGui::SameLine();
				const bool selected = active_point_selection->light_rig_mode == modes[i];
				if (selected) ImGui::PushStyleColor(ImGuiCol_Button, imgui::get()->ImGuiCol_ButtonGreen);
				if (ImGui::Button(utils::va("%s##rig_mode_%d", mode_labels[i], i), ImVec2(mode_width, 0.0f)))
				{
					active_point_selection->light_rig_mode = modes[i];
					if (modes[i] == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES)
					{
						if (active_point_selection->ies_emulation_samples <= 0) active_point_selection->ies_emulation_samples = 4;
						active_point_selection->ies_emulation = true;
					}
					else
					{
						active_point_selection->ies_emulation = false;
					}
					propagate_backend();
				}
				if (selected) ImGui::SafePopStyleColor(1, __LINE__);
			}

			const char* rig_help = active_point_selection->light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES
				? "Native IES: one analytical light with a real photometric profile. Best quality when the paired DXVK build supports IES."
				: active_point_selection->light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES
				? "Fake IES: compatibility controller plus helper-light cluster. Use only when native IES is unavailable."
				: "Analytical: one native Point/Spot, Disc, Rect, Tube or Distant light without a photometric profile.";
			ImGui::TextWrapped("%s", rig_help);

			ImGui::Spacing(0, 10);
			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			ImGui::SeparatorText(" 2. Geometry & Visualizer ");
			ImGui::PopFont();
			ImGui::TextDisabled("The selected geometry drives both the scene gizmo and the native Remix analytical-light extension. Native IES supports Point/Spot, Disc and Rect; choosing Tube or Distant switches the rig back to Legacy.");

			bool geometry_changed = false;
			const char* geometry_names[] = { "Auto", ICON_FA_LIGHTBULB "  Point", ICON_FA_FLASHLIGHT "  Spot", ICON_FA_CIRCLE "  Disc", ICON_FA_SQUARE "  Rect", ICON_FA_GRIP_LINES "  Tube", ICON_FA_SUN "  Directional / Distant" };
			int geometry_shape = std::clamp(
				active_point_selection->authoring_shape,
				static_cast<int>(map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_AUTO),
				static_cast<int>(map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT));
			SET_CHILD_WIDGET_WIDTH_MAN(235.0f);
			if (ImGui::Combo("Authoring Shape", &geometry_shape, geometry_names, IM_ARRAYSIZE(geometry_names)))
			{
				active_point_selection->authoring_shape = geometry_shape;
				switch (geometry_shape)
				{
				case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT:
				case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE:
					active_point_selection->use_shaping = false;
					break;
				case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT:
				case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK:
				case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT:
				case map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT:
					active_point_selection->use_shaping = true;
					if (active_point_selection->degrees <= 0.1f || active_point_selection->degrees >= 180.0f) active_point_selection->degrees = 35.0f;
					break;
				default:
					break;
				}
				if ((geometry_shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE ||
					geometry_shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT) &&
					active_point_selection->light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES)
				{
					active_point_selection->light_rig_mode = map_settings::remix_light_settings_s::LIGHT_RIG_MODE_LEGACY;
				}
				geometry_changed = true;
			}
			TT("Selects the editable fixture geometry. Auto resolves to Point or Spot from Cone Shaping. The value is propagated to all animation keyframes.");

			if (ImGui::BeginTable("##light_geometry_values", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(105.0f); geometry_changed |= ImGui::DragFloat("Width", &active_point_selection->authoring_width, 0.05f, 0.001f, 8192.0f, "%.2f");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(105.0f); geometry_changed |= ImGui::DragFloat("Height", &active_point_selection->authoring_height, 0.05f, 0.001f, 8192.0f, "%.2f");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(105.0f); geometry_changed |= ImGui::DragFloat("Length", &active_point_selection->authoring_length, 0.05f, 0.001f, 8192.0f, "%.2f");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(105.0f); geometry_changed |= ImGui::DragFloat("Range", &active_point_selection->authoring_range, 0.25f, 0.001f, 32768.0f, "%.1f");
				ImGui::EndTable();
			}
			if (geometry_changed)
			{
				active_point_selection->authoring_width = std::max(0.001f, active_point_selection->authoring_width);
				active_point_selection->authoring_height = std::max(0.001f, active_point_selection->authoring_height);
				active_point_selection->authoring_length = std::max(0.001f, active_point_selection->authoring_length);
				active_point_selection->authoring_range = std::max(0.001f, active_point_selection->authoring_range);
				propagate_backend();
			}

			ImGui::Spacing(0, 10);
			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			ImGui::SeparatorText(" 3. Output & Transform ");
			ImGui::PopFont();
			ImGui::TextDisabled("Position, rotation/direction, color, intensity and radius are shared concepts in every rig mode.");

			bool standard_changed = false;
			standard_changed |= ImGui::Widget_PrettyDragVec3("Position##LightStudio", &active_point_selection->position.x, false, 105.0f, 0.05f, -FLT_MAX, FLT_MAX, "X", "Y", "Z");

			static const map_settings::remix_light_settings_s::point_s* rotation_owner = nullptr;
			static Vector light_rotation = {};
			if (rotation_owner != active_point_selection)
			{
				rotation_owner = active_point_selection;
				light_rotation = direction_to_light_rotation(active_point_selection->direction);
			}

			const bool direction_changed = ImGui::Widget_PrettyDragVec3("Direction Vector##LightStudio", &active_point_selection->direction.x, true, 105.0f, 0.01f, -1.0f, 1.0f, "X", "Y", "Z");
			if (direction_changed)
			{
				active_point_selection->direction.NormalizeChecked();
				if (active_point_selection->direction.LengthSqr() <= 0.000001f) active_point_selection->direction = Vector(0.0f, 0.0f, -1.0f);
				light_rotation = direction_to_light_rotation(active_point_selection->direction);
				standard_changed = true;
			}

			SET_CHILD_WIDGET_WIDTH_MAN(220.0f);
			if (ImGui::DragFloat2("Rotation (Pitch / Yaw)", &light_rotation.x, 0.25f, -360.0f, 360.0f, "%.1f deg"))
			{
				light_rotation.x = std::clamp(light_rotation.x, -360.0f, 360.0f);
				light_rotation.y = std::clamp(light_rotation.y, -360.0f, 360.0f);
				const Vector euler(light_rotation.x, light_rotation.y, 0.0f);
				utils::vector::AngleVectors(euler, &active_point_selection->direction);
				active_point_selection->direction.NormalizeChecked();
				standard_changed = true;
			}
			TT("User-friendly rotation for the light axis. Native IES roll around that axis is controlled separately by Axis Rotation.");

			if (ImGui::BeginTable("##light_standard_values", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn();
				SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
				standard_changed |= ImGui::ColorEdit3("Color", &active_point_selection->radiance.x, ImGuiColorEditFlags_Float);

				ImGui::TableNextColumn();
				SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
				standard_changed |= ImGui::DragFloat("Intensity", &active_point_selection->radiance_scalar, 0.02f, 0.0f, 1000.0f, "%.3f");
				if (ImGui::BeginPopupContextItem("##brightness_presets"))
				{
					ImGui::TextDisabled("Brightness presets only");
					ImGui::Separator();
					for (const auto& preset : LIGHT_BRIGHTNESS_PRESETS)
					{
						if (ImGui::MenuItem(preset.name, nullptr, utils::float_equal(active_point_selection->radiance_scalar, preset.value)))
						{
							active_point_selection->radiance_scalar = preset.value;
							standard_changed = true;
						}
						TT(preset.tooltip);
					}
					ImGui::EndPopup();
				}
				TT("Right-click for brightness-only presets. These never change radius or light style.");

				ImGui::TableNextColumn();
				SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
				standard_changed |= ImGui::DragFloat("Radius", &active_point_selection->radius, 0.02f, 0.001f, 1000.0f, "%.3f");
				if (ImGui::BeginPopupContextItem("##radius_presets"))
				{
					ImGui::TextDisabled("Radius presets only");
					ImGui::Separator();
					for (const auto& preset : LIGHT_RADIUS_PRESETS)
					{
						if (ImGui::MenuItem(preset.name, nullptr, utils::float_equal(active_point_selection->radius, preset.value)))
						{
							active_point_selection->radius = preset.value;
							standard_changed = true;
						}
						TT(preset.tooltip);
					}
					ImGui::EndPopup();
				}
				TT("Right-click for radius-only presets. These never change brightness or light style.");

				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(120.0f); standard_changed |= ImGui::DragFloat("Volumetric", &active_point_selection->volumetric_scale, 0.01f, 0.0f, 16.0f, "%.2f");
				ImGui::TableNextColumn(); standard_changed |= ImGui::Checkbox("Cone Shaping", &active_point_selection->use_shaping);
				ImGui::TableNextColumn();
				if (ImGui::Button("Aim From Camera", ImVec2(-FLT_MIN, 0.0f)))
				{
					active_point_selection->direction = *game::get_current_view_forward();
					active_point_selection->direction.NormalizeChecked();
					active_point_selection->use_shaping = true;
					if (active_point_selection->degrees >= 180.0f) active_point_selection->degrees = 58.0f;
					standard_changed = true;
				}
				ImGui::EndTable();
			}

			if (active_point_selection->use_shaping)
			{
				if (ImGui::BeginTable("##light_shaping_values", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(110.0f); standard_changed |= ImGui::DragFloat("Cone Angle", &active_point_selection->degrees, 0.25f, 0.1f, 180.0f, "%.1f deg");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(110.0f); standard_changed |= ImGui::DragFloat("Softness", &active_point_selection->softness, 0.01f, 0.0f, 1.0f, "%.2f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(110.0f); standard_changed |= ImGui::DragFloat("Focus", &active_point_selection->exponent, 0.02f, 0.0f, 64.0f, "%.2f");
					ImGui::EndTable();
				}
			}

			const float preset_button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
			if (ImGui::Button("Style Presets  (right-click)##style_presets", ImVec2(preset_button_width, 0.0f))) ImGui::OpenPopup("##style_presets_popup");
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##style_presets_popup");
			if (ImGui::BeginPopup("##style_presets_popup"))
			{
				ImGui::TextDisabled("Style only - intensity and radius stay unchanged");
				ImGui::Separator();
				for (const auto& preset : LIGHT_WORKBENCH_PRESETS)
				{
					if (ImGui::MenuItem(preset.name))
					{
						apply_light_workbench_preset(*active_point_selection, preset);
						standard_changed = true;
					}
					TT(preset.tooltip);
				}
				ImGui::EndPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Move To Camera", ImVec2(preset_button_width, 0.0f)))
			{
				active_point_selection->position = *game::get_current_view_origin();
				standard_changed = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Reset Direction", ImVec2(preset_button_width, 0.0f)))
			{
				active_point_selection->direction = Vector(0.0f, 0.0f, -1.0f);
				standard_changed = true;
			}

			if (standard_changed)
			{
				// Animated keyframes are output samples, not a new base value.
				// Base Power is edited only in the animation module to prevent compounded dimming.
				if (!has_animation)
					edit_active_light->m_def.property_animation_intensity = std::max(0.0f, active_point_selection->radiance_scalar);
				refresh_light_runtime();
			}

			ImGui::Spacing(0, 10);
			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			ImGui::SeparatorText(" 4. Rig Settings ");
			ImGui::PopFont();
			if (active_point_selection->light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_LEGACY)
			{
				ImGui::TextWrapped("Analytical backend active. The selected geometry is submitted as one native Remix light; no IES profile or helper cluster is attached.");
			}
			else if (active_point_selection->light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES)
			{
				ImGui::SeparatorText("Native IES Profile Settings");
				ImGui::TextDisabled("Uses remixapi_LightInfoIESEXT in the paired IES-enabled DXVK Remix build. Missing files block creation instead of silently falling back.");

				bool native_changed = false;
				SET_CHILD_WIDGET_WIDTH_MAN(300.0f);
				native_changed |= ImGui::InputText("IES File", &active_point_selection->ies_file);
				TT("Absolute path or filename relative to <game>\\rtx-remix\\ies\\. The .ies extension is optional.");

				static std::vector<std::string> native_profiles;
				static bool native_profiles_scanned = false;
				if (!native_profiles_scanned) { native_profiles = scan_native_ies_profiles(); native_profiles_scanned = true; }
				const float native_button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
				if (ImGui::Button("Select Profile", ImVec2(native_button_width, 0.0f))) ImGui::OpenPopup("##native_ies_profile_popup");
				if (ImGui::BeginPopup("##native_ies_profile_popup"))
				{
					if (native_profiles.empty()) ImGui::TextDisabled("No .ies files found");
					for (const auto& file : native_profiles)
					{
						if (ImGui::MenuItem(file.c_str(), nullptr, active_point_selection->ies_file == file))
						{
							active_point_selection->ies_file = file;
							native_changed = true;
						}
					}
					ImGui::EndPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("Refresh Files", ImVec2(native_button_width, 0.0f)))
				{
					native_profiles = scan_native_ies_profiles();
					native_profiles_scanned = true;
				}
				ImGui::SameLine();
				if (ImGui::Button("Open IES Folder", ImVec2(native_button_width, 0.0f)))
				{
					const auto folder = native_ies_folder_path();
					std::error_code ec;
					std::filesystem::create_directories(folder, ec);
					ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				}

				if (ImGui::BeginTable("##native_ies_controls", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); native_changed |= ImGui::DragFloat("Angle Scale", &active_point_selection->ies_angle_scale, 0.01f, 0.01f, 8.0f, "%.2f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); native_changed |= ImGui::DragFloat("IES Intensity", &active_point_selection->ies_intensity_scale, 0.01f, 0.0f, 32.0f, "%.2f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); native_changed |= ImGui::DragFloat("Axis Rotation", &active_point_selection->ies_axis_rotation, 0.5f, -360.0f, 360.0f, "%.1f deg");
					ImGui::TableNextColumn(); native_changed |= ImGui::Checkbox("Normalize Flux", &active_point_selection->ies_normalize);
					ImGui::EndTable();
				}
				ImGui::TextDisabled("Status: %s", edit_active_light->m_native_ies_status.c_str());
				if (ImGui::Button("Retry / Rebuild Selected Profile", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
				{
					// Clear only the ASI-side resolved-path cache. DXVK owns profile-content hot reload.
					edit_active_light->m_native_ies_authored_path.clear();
					edit_active_light->m_native_ies_profile_path.clear();
					edit_active_light->m_native_ies_resolution_failed = false;
					native_changed = true;
				}
				TT("Retries a profile that was missing when the light was first created. Existing profile content is reloaded by DXVK hot reload.");
				if (native_changed) propagate_backend();
			}
			else
			{
				ImGui::SeparatorText("Fake IES Compatibility Settings");
				ImGui::TextDisabled("Compatibility rig: one controller light plus helper lights. Use only when the paired DXVK build cannot consume native IES.");

				bool fake_changed = false;
				int profile_index = ies_profile_index_for(active_point_selection->ies_profile);
				SET_CHILD_WIDGET_WIDTH_MAN(220.0f);
				if (ImGui::Combo("Profile", &profile_index, IES_WORKBENCH_PROFILES, IM_ARRAYSIZE(IES_WORKBENCH_PROFILES)))
				{
					active_point_selection->ies_profile = profile_index == 0 ? "" : IES_WORKBENCH_PROFILES[profile_index];
					fake_changed = true;
				}

				if (ImGui::BeginTable("##fake_ies_primary", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); fake_changed |= ImGui::DragFloat("Profile Strength", &active_point_selection->ies_strength, 0.01f, 0.0f, 8.0f, "%.2f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); fake_changed |= ImGui::DragFloat("Profile Focus", &active_point_selection->ies_focus, 0.01f, 0.05f, 8.0f, "%.2f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f);
					if (ImGui::InputInt("Helpers", &active_point_selection->ies_emulation_samples, 1, 2))
					{
						active_point_selection->ies_emulation_samples = std::clamp(active_point_selection->ies_emulation_samples, 1, 24);
						active_point_selection->ies_emulation = true;
						fake_changed = true;
					}
					ImGui::TableNextColumn();
					int pattern_index = ies_cluster_pattern_index_for(active_point_selection->ies_emulation_pattern);
					SET_CHILD_WIDGET_WIDTH_MAN(100.0f);
					if (ImGui::Combo("Pattern", &pattern_index, IES_CLUSTER_PATTERNS, IM_ARRAYSIZE(IES_CLUSTER_PATTERNS)))
					{
						active_point_selection->ies_emulation_pattern = IES_CLUSTER_PATTERNS[pattern_index];
						fake_changed = true;
					}
					ImGui::EndTable();
				}

				if (ImGui::BeginTable("##fake_ies_secondary", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); fake_changed |= ImGui::DragFloat("Spread", &active_point_selection->ies_emulation_spread, 0.01f, 0.0f, 4.0f, "%.2f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); fake_changed |= ImGui::DragFloat("Helper Radius", &active_point_selection->ies_emulation_radius_scale, 0.01f, 0.01f, 4.0f, "%.2f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); fake_changed |= ImGui::DragFloat("Helper Power", &active_point_selection->ies_emulation_intensity_scale, 0.01f, 0.0f, 4.0f, "%.2f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); fake_changed |= ImGui::DragFloat("Forward Offset", &active_point_selection->ies_emulation_forward_offset, 0.01f, -4.0f, 4.0f, "%.2f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); fake_changed |= ImGui::DragFloat("Aspect", &active_point_selection->ies_emulation_aspect, 0.01f, 0.1f, 8.0f, "%.2f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); fake_changed |= ImGui::DragFloat("Twist", &active_point_selection->ies_emulation_twist, 0.5f, -360.0f, 360.0f, "%.1f deg");
					ImGui::EndTable();
				}

				const float fake_button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
				if (ImGui::Button("Apply Profile Shape", ImVec2(fake_button_width, 0.0f)))
				{
					const float intensity = active_point_selection->radiance_scalar;
					const float radius = active_point_selection->radius;
					apply_workbench_ies_profile(*active_point_selection);
					active_point_selection->radiance_scalar = intensity;
					active_point_selection->radius = radius;
					fake_changed = true;
				}
				TT("Applies color/cone/profile character but preserves brightness and radius.");
				ImGui::SameLine();
				if (ImGui::Button("Apply Helper Defaults", ImVec2(fake_button_width, 0.0f)))
				{
					apply_workbench_ies_cluster_defaults(*active_point_selection);
					active_point_selection->ies_emulation = true;
					fake_changed = true;
				}
				if (fake_changed) propagate_backend();
			}

			ImGui::Spacing(0, 10);
			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			ImGui::SeparatorText(" Animation Modules ");
			ImGui::PopFont();
			ImGui::TextDisabled("Movement changes only position/direction. Output changes only base power and color.");

			if (ImGui::BeginTabBar("##light_animation_modules", ImGuiTabBarFlags_None))
			{
				if (ImGui::BeginTabItem("Brightness & Color"))
				{
					int output_index = light_animation_index(LIGHT_OUTPUT_ANIMATION_PRESETS,
						IM_ARRAYSIZE(LIGHT_OUTPUT_ANIMATION_PRESETS), edit_active_light->m_def.property_animation);
					bool output_changed = false;
					SET_CHILD_WIDGET_WIDTH_MAN(185.0f);
					if (ImGui::Combo("Output##light_output_module", &output_index,
						LIGHT_OUTPUT_ANIMATION_PRESETS, IM_ARRAYSIZE(LIGHT_OUTPUT_ANIMATION_PRESETS)))
					{
						edit_active_light->m_def.property_animation = LIGHT_OUTPUT_ANIMATION_PRESETS[output_index];
						const auto defaults = add_light_animation_defaults_for(edit_active_light->m_def.property_animation);
						edit_active_light->m_def.property_animation_duration = defaults.duration;
						edit_active_light->m_def.property_animation_speed = defaults.speed;
						edit_active_light->m_def.property_animation_variation = defaults.variation;
						output_changed = true;
					}

					if (ImGui::BeginTable("##light_output_controls", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
					{
						ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f);
						output_changed |= ImGui::DragFloat("Base Power", &edit_active_light->m_def.property_animation_intensity, 0.02f, 0.0f, 100000.0f, "%.3f");
						ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f);
						output_changed |= ImGui::DragFloat("Duration##output", &edit_active_light->m_def.property_animation_duration, 0.01f, 0.05f, 120.0f, "%.2f s");
						ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f);
						output_changed |= ImGui::DragFloat("Speed##output", &edit_active_light->m_def.property_animation_speed, 0.01f, 0.05f, 20.0f, "%.2fx");
						ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f);
						output_changed |= ImGui::DragFloat("Variation##output", &edit_active_light->m_def.property_animation_variation, 0.01f, 0.0f, 1.0f, "%.2f");
						ImGui::EndTable();
					}
					TT("Base Power is stored separately and never re-read from a dim animation keyframe. Reapplying presets cannot compound intensity toward zero.");

					if (output_changed)
					{
						auto base = *ui_light_point_by_index(edit_active_light, 0u);
						base.radiance_scalar = std::max(0.0f, edit_active_light->m_def.property_animation_intensity);
						rebuild_split_light_animation(edit_active_light->m_def, &base);
						rebuild_edit_light_runtime_from_def(edit_active_light);
						active_point_selection = ui_light_point_by_index(edit_active_light, 0u);
					}
					if (ImGui::Button("Clear Output", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
					{
						auto base = *ui_light_point_by_index(edit_active_light, 0u);
						edit_active_light->m_def.property_animation = "stable";
						rebuild_split_light_animation(edit_active_light->m_def, &base);
						rebuild_edit_light_runtime_from_def(edit_active_light);
						active_point_selection = ui_light_point_by_index(edit_active_light, 0u);
					}
					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem("Movement"))
				{
					int movement_index = light_animation_index(LIGHT_MOVEMENT_ANIMATION_PRESETS,
						IM_ARRAYSIZE(LIGHT_MOVEMENT_ANIMATION_PRESETS), canonical_light_movement_name(edit_active_light->m_def.movement_animation));
					bool movement_changed = false;
					SET_CHILD_WIDGET_WIDTH_MAN(185.0f);
					if (ImGui::Combo("Motion##light_movement_module", &movement_index,
						LIGHT_MOVEMENT_ANIMATION_PRESETS, IM_ARRAYSIZE(LIGHT_MOVEMENT_ANIMATION_PRESETS)))
					{
						edit_active_light->m_def.movement_animation = LIGHT_MOVEMENT_ANIMATION_PRESETS[movement_index];
						const auto defaults = add_light_animation_defaults_for(edit_active_light->m_def.movement_animation);
						edit_active_light->m_def.movement_animation_duration = defaults.duration;
						edit_active_light->m_def.movement_animation_speed = defaults.speed;
						edit_active_light->m_def.movement_animation_axis = defaults.axis;
						edit_active_light->m_def.movement_animation_degrees = defaults.degrees;
						edit_active_light->m_def.movement_animation_phase = defaults.phase;
						movement_changed = true;
					}
					if (ImGui::BeginTable("##light_movement_controls", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
					{
						ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f);
						movement_changed |= ImGui::DragFloat("Duration##move", &edit_active_light->m_def.movement_animation_duration, 0.01f, 0.05f, 120.0f, "%.2f s");
						ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f);
						movement_changed |= ImGui::DragFloat("Speed##move", &edit_active_light->m_def.movement_animation_speed, 0.01f, 0.05f, 20.0f, "%.2fx");
						ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f);
						movement_changed |= ImGui::DragFloat("Sweep##move", &edit_active_light->m_def.movement_animation_degrees, 0.5f, 0.0f, 1440.0f, "%.1f deg");
						ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f);
						movement_changed |= ImGui::DragFloat("Distance##move", &edit_active_light->m_def.movement_animation_distance, 0.25f, 0.0f, 4096.0f, "%.1f");
						ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f);
						movement_changed |= ImGui::DragFloat("Phase##move", &edit_active_light->m_def.movement_animation_phase, 0.5f, -1440.0f, 1440.0f, "%.1f deg");
						ImGui::EndTable();
					}
					movement_changed |= ImGui::Widget_PrettyDragVec3("Axis##light_movement_module", &edit_active_light->m_def.movement_animation_axis.x,
						true, 105.0f, 0.01f, -1.0f, 1.0f, "X", "Y", "Z");
					if (movement_changed)
					{
						auto base = *ui_light_point_by_index(edit_active_light, 0u);
						base.radiance_scalar = edit_active_light->m_def.property_animation_intensity;
						rebuild_split_light_animation(edit_active_light->m_def, &base);
						rebuild_edit_light_runtime_from_def(edit_active_light);
						active_point_selection = ui_light_point_by_index(edit_active_light, 0u);
					}
					if (ImGui::Button("Clear Movement", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
					{
						auto base = *ui_light_point_by_index(edit_active_light, 0u);
						edit_active_light->m_def.movement_animation = "none";
						rebuild_split_light_animation(edit_active_light->m_def, &base);
						rebuild_edit_light_runtime_from_def(edit_active_light);
						active_point_selection = ui_light_point_by_index(edit_active_light, 0u);
					}
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}

			ImGui::Spacing(0, 10);
			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			ImGui::SeparatorText(" Export ");
			ImGui::PopFont();
			const float export_button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
			if (ImGui::Button("Copy Selected Light TOML", ImVec2(export_button_width, 0.0f)))
			{
				copy_text_to_clipboard(common::toml::build_light_string_for_single_light(build_current_edit_light_def(edit_active_light)));
			}
			ImGui::SameLine();
			if (ImGui::Button("Append To Workbench Export", ImVec2(export_button_width, 0.0f)))
			{
				const auto temp_def = build_current_edit_light_def(edit_active_light);
				append_to_light_workbench_export(temp_def.comment.empty() ? "selected light" : temp_def.comment,
					common::toml::build_light_string_for_single_light(temp_def));
			}
		}

		struct marker_preset_s
		{
			const char* name;
			const char* tooltip;
			bool no_cull;
			Vector scale;
			Vector rotation;
			bool gate_current_area;
			bool exclude_current_leaf;
			const char* comment;
		};

		static const marker_preset_s MARKER_WORKBENCH_PRESETS[] =
		{
			{ "NoCull Anchor", "Small always-spawned marker. Good for attaching RTX helper lights or keeping a reference mesh alive.", true, { 1.00f, 1.00f, 1.00f }, { 0.00f, 0.00f, 0.00f }, false, false, "workbench: nocull anchor" },
			{ "Area Anchor", "NoCull marker gated to the current area. Useful when one global marker would leak into another room/area.", true, { 1.00f, 1.00f, 1.00f }, { 0.00f, 0.00f, 0.00f }, true, false, "workbench: area-gated anchor" },
			{ "Portal Plane", "Flat wide marker plane for doorway/portal blockers. Gate it to the current area and then tune rotation/scale manually.", true, { 4.00f, 0.05f, 2.50f }, { 0.00f, 0.00f, 0.00f }, true, false, "workbench: portal blocker plane" },
			{ "Leaf Exit Blocker", "Area-gated plane that is hidden when the player is in the current leaf. Good for transitions around leaf/area seams.", true, { 4.00f, 0.05f, 2.50f }, { 0.00f, 0.00f, 0.00f }, true, true, "workbench: leaf exit blocker" },
			{ "Tiny Trigger", "Tiny marker for event/trigger experiments without adding a large visible helper mesh.", true, { 0.25f, 0.25f, 0.25f }, { 0.00f, 0.00f, 0.00f }, true, false, "workbench: tiny trigger marker" },
			{ "Regular Marker", "Regular marker mesh. Not NoCull, so it can be culled by the engine and needs map reload to fully validate.", false, { 1.00f, 1.00f, 1.00f }, { 0.00f, 0.00f, 0.00f }, false, false, "workbench: regular marker" },
		};

		bool has_valid_current_area()
		{
			return g_current_area >= 0;
		}

		bool has_valid_current_leaf()
		{
			return g_current_leaf >= 0;
		}

		std::uint32_t current_area_u32()
		{
			return static_cast<std::uint32_t>(std::max(g_current_area, 0));
		}

		std::uint32_t current_leaf_u32()
		{
			return static_cast<std::uint32_t>(std::max(g_current_leaf, 0));
		}

		std::uint32_t next_free_marker_index(const std::vector<map_settings::marker_settings_s>& markers, const bool no_cull)
		{
			for (std::uint32_t candidate = 0u; candidate < 4096u; ++candidate)
			{
				bool used = false;
				for (const auto& marker : markers)
				{
					if (marker.no_cull == no_cull && marker.index == candidate)
					{
						used = true;
						break;
					}
				}

				if (!used) {
					return candidate;
				}
			}

			return static_cast<std::uint32_t>(markers.size());
		}

		void apply_marker_workbench_preset(map_settings::marker_settings_s& marker, const marker_preset_s& preset, const bool move_to_camera)
		{
			marker.no_cull = preset.no_cull;
			marker.scale = preset.scale;
			marker.rotation = preset.rotation;

			if (move_to_camera)
			{
				marker.origin = *game::get_current_view_origin();
				marker.origin.z -= 1.0f;
			}

			if (preset.gate_current_area && has_valid_current_area())
			{
				marker.areas.clear();
				marker.areas.insert(current_area_u32());
			}

			if (preset.exclude_current_leaf && has_valid_current_leaf())
			{
				marker.when_not_in_leafs.insert(current_leaf_u32());
			}

			if (marker.comment.empty()) {
				marker.comment = preset.comment;
			}
		}

		map_settings::marker_settings_s make_marker_from_preset(const std::vector<map_settings::marker_settings_s>& markers, const marker_preset_s& preset)
		{
			map_settings::marker_settings_s marker = {};
			marker.index = next_free_marker_index(markers, preset.no_cull);
			marker.origin = *game::get_current_view_origin();
			marker.origin.z -= 1.0f;
			marker.comment = preset.comment;
			apply_marker_workbench_preset(marker, preset, false);
			return marker;
		}

		std::string build_single_marker_string(const map_settings::marker_settings_s& marker)
		{
			return common::toml::build_map_marker_string_for_current_map(std::vector<map_settings::marker_settings_s>{ marker });
		}

		std::string build_single_culling_entry_string(const map_settings::area_overrides_s& area)
		{
			std::unordered_map<std::uint32_t, map_settings::area_overrides_s> single;
			single.emplace(area.area_index, area);
			return common::toml::build_culling_overrides_string_for_current_map(single);
		}

		std::string build_current_area_leaf_snapshot()
		{
			return std::format("# map: {}\n# current area: {}\n# current leaf: {}\nposition = [{:.2f}, {:.2f}, {:.2f}]",
				map_settings::get_map_settings().mapname,
				g_current_area,
				g_current_leaf,
				game::get_current_view_origin()->x,
				game::get_current_view_origin()->y,
				game::get_current_view_origin()->z);
		}

		map_settings::area_overrides_s* ensure_current_area_override(std::unordered_map<std::uint32_t, map_settings::area_overrides_s>& areas)
		{
			if (!has_valid_current_area()) {
				return nullptr;
			}

			const auto area_num = current_area_u32();
			auto [it, inserted] = areas.try_emplace(area_num);
			if (inserted)
			{
				it->second.cull_mode = map_settings::AREA_CULL_MODE::AREA_CULL_INFO_DEFAULT;
				it->second.nocull_distance = map_settings::get_map_settings().default_nocull_dist;
				it->second.area_index = area_num;
			}

			return &it->second;
		}


		// #
		// Remix Vars / map_configs workbench

		struct remix_var_preset_s
		{
			const char* name;
			const char* file_name;
			const char* tooltip;
			const char* body;
			float duration;
			float delay_out;
			remix_vars::EASE_TYPE ease;
		};

		static const remix_var_preset_s REMIX_VAR_WORKBENCH_PRESETS[] =
		{
			{
				"Interior Dark",
				"wb_interior_dark.conf",
				"Low sky, controlled bloom and neutral soft fog. Good for apartments/hospital interiors.",
R"(# Workbench preset: Interior Dark
rtx.skyBrightness = 0.55
rtx.bloom.burnIntensity = 1.15
rtx.volumetrics.enable = True
rtx.volumetrics.enableFogRemap = True
rtx.volumetrics.depthOffset = 0.05
rtx.volumetrics.froxelMaxDistanceMeters = 55
rtx.volumetrics.froxelDepthSliceDistributionExponent = 2.0
rtx.volumetrics.transmittanceColor = 0.999, 0.999, 0.999
rtx.volumetrics.singleScatteringAlbedo = 0.82, 0.86, 0.92
rtx.volumetrics.fogRemapMaxDistanceMinMeters = 1
rtx.volumetrics.fogRemapMaxDistanceMaxMeters = 36
rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMinMeters = 18
rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMaxMeters = 90
rtx.volumetrics.fogRemapColorMultiscatteringScale = 0.18
rtx.viewDistance.distanceMode = 2
rtx.viewDistance.distanceFadeMin = 900
rtx.viewDistance.distanceFadeMax = 2600
rtx.viewDistance.noiseScale = 5
)",
				2.5f,
				0.0f,
				remix_vars::EASE_TYPE_SIN_INOUT,
			},
			{
				"Safehouse Warm",
				"wb_safehouse_warm.conf",
				"Warmer fog/bloom for safer indoor rooms, lamps and survivor staging areas.",
R"(# Workbench preset: Safehouse Warm
rtx.skyBrightness = 0.85
rtx.bloom.burnIntensity = 1.8
rtx.volumetrics.enable = True
rtx.volumetrics.enableFogRemap = True
rtx.volumetrics.depthOffset = 0.05
rtx.volumetrics.froxelMaxDistanceMeters = 70
rtx.volumetrics.froxelDepthSliceDistributionExponent = 1.75
rtx.volumetrics.transmittanceColor = 0.999, 0.990, 0.940
rtx.volumetrics.singleScatteringAlbedo = 1.0, 0.88, 0.68
rtx.volumetrics.fogRemapMaxDistanceMinMeters = 1
rtx.volumetrics.fogRemapMaxDistanceMaxMeters = 45
rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMinMeters = 20
rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMaxMeters = 100
rtx.volumetrics.fogRemapColorMultiscatteringScale = 0.55
)",
				2.0f,
				0.0f,
				remix_vars::EASE_TYPE_SIN_INOUT,
			},
			{
				"Fire Smoky",
				"wb_fire_smoky.conf",
				"Strong warm volumetric/bloom response for fire barrels, explosions and burning rooms.",
R"(# Workbench preset: Fire Smoky
rtx.bloom.burnIntensity = 18.0
rtx.volumetrics.enable = True
rtx.volumetrics.enableFogRemap = True
rtx.volumetrics.depthOffset = 0.05
rtx.volumetrics.froxelMaxDistanceMeters = 48
rtx.volumetrics.froxelDepthSliceDistributionExponent = 2.0
rtx.volumetrics.transmittanceColor = 0.999, 0.960, 0.860
rtx.volumetrics.singleScatteringAlbedo = 1.0, 0.62, 0.32
rtx.volumetrics.fogRemapMaxDistanceMinMeters = 1
rtx.volumetrics.fogRemapMaxDistanceMaxMeters = 40
rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMinMeters = 18
rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMaxMeters = 90
rtx.volumetrics.fogRemapColorMultiscatteringScale = 4.5
)",
				0.75f,
				0.35f,
				remix_vars::EASE_TYPE_CUBIC_OUT,
			},
			{
				"Alarm Red",
				"wb_alarm_red.conf",
				"Aggressive red pulse helper. Good for alarms, panic events and emergency lighting.",
R"(# Workbench preset: Alarm Red
rtx.bloom.burnIntensity = 9.0
rtx.postfx.enableVignette = True
rtx.postfx.vignetteIntensity = 1.15
rtx.postfx.vignetteRadius = 0.62
rtx.tonemap.colorGradingEnabled = True
rtx.tonemap.colorBalance = 1.28, 0.72, 0.62
rtx.volumetrics.enable = True
rtx.volumetrics.enableFogRemap = True
rtx.volumetrics.singleScatteringAlbedo = 1.0, 0.38, 0.28
rtx.volumetrics.fogRemapColorMultiscatteringScale = 1.35
)",
				0.35f,
				0.2f,
				remix_vars::EASE_TYPE_EXPO_OUT,
			},
			{
				"Storm Outside",
				"wb_storm_outside.conf",
				"Cooler wet outdoor mood with longer view distance. Good base for rain/storm streets.",
R"(# Workbench preset: Storm Outside
rtx.skyBrightness = 1.65
rtx.bloom.burnIntensity = 1.25
rtx.volumetrics.enable = True
rtx.volumetrics.enableFogRemap = True
rtx.volumetrics.depthOffset = 0.05
rtx.volumetrics.froxelMaxDistanceMeters = 130
rtx.volumetrics.froxelDepthSliceDistributionExponent = 1.15
rtx.volumetrics.transmittanceColor = 0.900, 0.940, 1.000
rtx.volumetrics.singleScatteringAlbedo = 0.72, 0.78, 0.92
rtx.volumetrics.fogRemapMaxDistanceMinMeters = 2
rtx.volumetrics.fogRemapMaxDistanceMaxMeters = 90
rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMinMeters = 40
rtx.volumetrics.fogRemapTransmittanceMeasurementDistanceMaxMeters = 160
rtx.volumetrics.fogRemapColorMultiscatteringScale = 0.22
rtx.viewDistance.distanceMode = 2
rtx.viewDistance.distanceFadeMin = 2500
rtx.viewDistance.distanceFadeMax = 7200
rtx.viewDistance.noiseScale = 9
)",
				3.0f,
				0.0f,
				remix_vars::EASE_TYPE_SIN_INOUT,
			},
			{
				"Flashbang/Explosion",
				"wb_flash_explosion.conf",
				"Short bright transition that can bounce back using delay_out. Useful with sound/choreo triggers.",
R"(# Workbench preset: Flashbang / Explosion
rtx.bloom.burnIntensity = 35.0
rtx.postfx.enableChromaticAberration = True
rtx.postfx.chromaticAberrationAmount = 4.0
rtx.tonemap.saturation = 0.72
rtx.volumetrics.enable = True
rtx.volumetrics.enableFogRemap = True
rtx.volumetrics.singleScatteringAlbedo = 1.0, 0.96, 0.78
rtx.volumetrics.fogRemapColorMultiscatteringScale = 6.0
)",
				0.18f,
				0.18f,
				remix_vars::EASE_TYPE_EXPO_OUT,
			},
			{
				"Cinematic Calm",
				"wb_cinematic_calm.conf",
				"Neutral, less harsh post/volumetric setup for screenshots and slower scenes.",
R"(# Workbench preset: Cinematic Calm
rtx.skyBrightness = 2.20
rtx.bloom.burnIntensity = 0.85
rtx.tonemap.saturation = 0.92
rtx.postfx.enableVignette = True
rtx.postfx.vignetteIntensity = 0.55
rtx.volumetrics.enable = True
rtx.volumetrics.enableFogRemap = True
rtx.volumetrics.froxelMaxDistanceMeters = 100
rtx.volumetrics.singleScatteringAlbedo = 0.94, 0.94, 0.94
rtx.volumetrics.fogRemapColorMultiscatteringScale = 0.08
rtx.viewDistance.distanceMode = 2
rtx.viewDistance.distanceFadeMin = 2000
rtx.viewDistance.distanceFadeMax = 5600
rtx.viewDistance.noiseScale = 6
)",
				2.0f,
				0.0f,
				remix_vars::EASE_TYPE_SIN_INOUT,
			},
		};

		std::string map_config_workbench_path(const std::string& file_name)
		{
			ensure_game_root_path();
			return game::root_path + COMPMOD_ASSET_DIR "map_configs\\" + file_name;
		}

		bool write_workbench_conf_file(const std::string& file_name, const std::string& body)
		{
			const auto path = map_config_workbench_path(file_name);
			std::filesystem::create_directories(std::filesystem::path(path).parent_path());

			std::ofstream file(path, std::ios::out | std::ios::trunc);
			if (!file.is_open())
			{
				game::console();
				std::cout << "[RemixVarsWorkbench] Failed to write config: " << path << std::endl;
				return false;
			}

			file << body;
			if (!body.empty() && body.back() != '\n') {
				file << '\n';
			}

			return true;
		}

		std::string option_value_to_conf_string(const remix_vars::OPTION_TYPE type, const remix_vars::option_value& value)
		{
			switch (type)
			{
			case remix_vars::OPTION_TYPE_BOOL:
				return value.enabled ? "True" : "False";
			case remix_vars::OPTION_TYPE_INT:
				return std::to_string(value.integer);
			case remix_vars::OPTION_TYPE_FLOAT:
				return std::format("{:.6g}", value.value);
			case remix_vars::OPTION_TYPE_VEC2:
				return std::format("{:.6g}, {:.6g}", value.vector[0], value.vector[1]);
			case remix_vars::OPTION_TYPE_VEC3:
				return std::format("{:.6g}, {:.6g}, {:.6g}", value.vector[0], value.vector[1], value.vector[2]);
			default:
				return {};
			}
		}

		std::string build_modified_remix_vars_snapshot()
		{
			auto options = remix_vars::options_snapshot(true);
			std::sort(options.begin(), options.end(), [](const auto& a, const auto& b) { return a.name < b.name; });

			std::string out = std::format("# Remix Vars snapshot\n# Map: {}\n# Only variables modified since RTX config/reset state are included.\n",
				map_settings::get_map_settings().mapname);

			for (const auto& snapshot : options)
			{
				if (snapshot.option.type == remix_vars::OPTION_TYPE_NONE) continue;
				const auto value = option_value_to_conf_string(snapshot.option.type, snapshot.option.current);
				if (!value.empty()) out += snapshot.name + " = " + value + "\n";
			}

			if (options.empty()) out += "# No modified Remix Vars detected.\n";
			return out;
		}

		std::string build_leaf_transition_snippet(const std::string& conf_name, const float duration, const float delay_in, const float delay_out, const remix_vars::EASE_TYPE ease, const int mode)
		{
			return std::format("{{ conf = \"{}\", trigger = {{ leafs = [{}] }}, mode = {}, ease = {}, duration = {:.3g}, delay_in = {:.3g}, delay_out = {:.3g} }}",
				toml_escape_inline_string(conf_name),
				has_valid_current_leaf() ? std::to_string(current_leaf_u32()) : "0",
				mode,
				static_cast<int>(ease),
				duration,
				delay_in,
				delay_out);
		}

		std::string build_sound_transition_snippet(const std::string& conf_name, const std::string& sound, const float duration, const float delay_in, const float delay_out, const remix_vars::EASE_TYPE ease, const int mode)
		{
			return std::format("{{ conf = \"{}\", trigger = {{ sound = {} }}, mode = {}, ease = {}, duration = {:.3g}, delay_in = {:.3g}, delay_out = {:.3g} }}",
				toml_escape_inline_string(conf_name),
				sound,
				mode,
				static_cast<int>(ease),
				duration,
				delay_in,
				delay_out);
		}

		bool draw_remix_vars_preset_workbench(remix_vars* var)
		{
			if (!var) {
				return false;
			}

			bool request_config_refresh = false;
			static int selected_preset = 0;
			static float transition_duration = REMIX_VAR_WORKBENCH_PRESETS[0].duration;
			static float transition_delay_in = 0.0f;
			static float transition_delay_out = REMIX_VAR_WORKBENCH_PRESETS[0].delay_out;
			static int transition_mode = 2;
			static remix_vars::EASE_TYPE transition_ease = REMIX_VAR_WORKBENCH_PRESETS[0].ease;
			static std::string sound_trigger = "0x00000000";

			ImGui::Spacing(0, 8);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
			ImGui::TableHeaderDropshadow();
			const bool workbench_state = ImGui::CollapsingHeader("Remix Vars Preset Workbench", ImGuiTreeNodeFlags_DefaultOpen);
			ImGui::PopStyleVar();

			if (!workbench_state) {
				return false;
			}

			ImGui::TextDisabled("Creates/updates reusable .conf files and copies transition snippets for map_settings.toml.");
			ImGui::Spacing(0, 4);

			if (ImGui::BeginCombo("Preset##RemixVarsPreset", REMIX_VAR_WORKBENCH_PRESETS[selected_preset].name, ImGuiComboFlags_None))
			{
				for (int n = 0; n < (int)IM_ARRAYSIZE(REMIX_VAR_WORKBENCH_PRESETS); n++)
				{
					const bool is_selected = selected_preset == n;
					if (ImGui::Selectable(REMIX_VAR_WORKBENCH_PRESETS[n].name, is_selected))
					{
						selected_preset = n;
						transition_duration = REMIX_VAR_WORKBENCH_PRESETS[n].duration;
						transition_delay_out = REMIX_VAR_WORKBENCH_PRESETS[n].delay_out;
						transition_ease = REMIX_VAR_WORKBENCH_PRESETS[n].ease;
					}

					if (is_selected) {
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
			TT(REMIX_VAR_WORKBENCH_PRESETS[selected_preset].tooltip);

			const auto& preset = REMIX_VAR_WORKBENCH_PRESETS[selected_preset];
			ImGui::TextDisabled("File: %s", preset.file_name);

			SET_CHILD_WIDGET_WIDTH_MAN(140.0f);
			if (ImGui::DragFloat("Transition Duration", &transition_duration, 0.01f, 0.0f)) {
				transition_duration = std::clamp(transition_duration, 0.0f, FLT_MAX);
			}
			SET_CHILD_WIDGET_WIDTH_MAN(140.0f);
			if (ImGui::DragFloat("Delay In", &transition_delay_in, 0.01f, 0.0f)) {
				transition_delay_in = std::clamp(transition_delay_in, 0.0f, FLT_MAX);
			}
			SET_CHILD_WIDGET_WIDTH_MAN(140.0f);
			if (ImGui::DragFloat("Delay Out / Return", &transition_delay_out, 0.01f, 0.0f)) {
				transition_delay_out = std::clamp(transition_delay_out, 0.0f, FLT_MAX);
			}

			SET_CHILD_WIDGET_WIDTH_MAN(140.0f);
			if (ImGui::BeginCombo("Ease", remix_vars::EASE_TYPE_STR[transition_ease], ImGuiComboFlags_None))
			{
				for (std::uint32_t n = 0u; n < (std::uint32_t)IM_ARRAYSIZE(remix_vars::EASE_TYPE_STR); n++)
				{
					const bool is_selected = transition_ease == n;
					if (ImGui::Selectable(remix_vars::EASE_TYPE_STR[n], is_selected)) {
						transition_ease = (remix_vars::EASE_TYPE)n;
					}

					if (is_selected) {
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}

			SET_CHILD_WIDGET_WIDTH_MAN(140.0f);
			ImGui::SliderInt("Mode", &transition_mode, 0, 3);
			TT("0/1 once enter/leave, 2/3 always enter/leave. Sounds normally use enter modes.");

			const auto two_row_button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f, 0.0f);

			if (ImGui::Button("Create / Update Preset .conf", two_row_button_size))
			{
				if (write_workbench_conf_file(preset.file_name, preset.body))
				{
					request_config_refresh = true;
					game::console();
					std::cout << "[RemixVarsWorkbench] Wrote config: " << map_config_workbench_path(preset.file_name) << std::endl;
				}
			}
			TT("Writes the selected preset into l4d2-rtx\\map_configs. Existing workbench preset file is overwritten.");

			ImGui::SameLine();
			if (ImGui::Button("Trigger Preset Now", two_row_button_size))
			{
				if (write_workbench_conf_file(preset.file_name, preset.body))
				{
					request_config_refresh = true;
					var->parse_and_apply_conf_with_lerp(
						preset.file_name,
						utils::string_hash64(std::format("workbench_{}_{}", preset.file_name, map_settings::get_map_settings().mapname)),
						transition_ease,
						transition_duration,
						transition_delay_in,
						transition_delay_out);
				}
			}
			TT("Writes the file, then applies it through the existing RemixVars transition system.");

			if (ImGui::Button("Copy Leaf Transition", two_row_button_size))
			{
				copy_text_to_clipboard(build_leaf_transition_snippet(preset.file_name, transition_duration, transition_delay_in, transition_delay_out, transition_ease, transition_mode));
			}
			TT("Copies a CONFIGVARS transition using the current leaf. Paste it into this map's transitions array.");

			ImGui::SameLine();
			if (ImGui::Button("Append Leaf Transition Export", two_row_button_size))
			{
				append_to_mapsettings_workbench_export("RemixVars", preset.file_name, "Paste into [CONFIGVARS] transitions for this map in map_settings.toml.",
					build_leaf_transition_snippet(preset.file_name, transition_duration, transition_delay_in, transition_delay_out, transition_ease, transition_mode));
			}

			ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(140.0f));
			ImGui::InputText("Sound Trigger", &sound_trigger);
			TT("Use hex hash like 0x12345678 or quoted substring like \"weapons/pistol/...\".");

			if (ImGui::Button("Copy Sound Transition", two_row_button_size))
			{
				copy_text_to_clipboard(build_sound_transition_snippet(preset.file_name, sound_trigger, transition_duration, transition_delay_in, transition_delay_out, transition_ease, transition_mode));
			}
			TT("Copies a CONFIGVARS transition using the Sound Trigger field.");

			ImGui::SameLine();
			if (ImGui::Button("Append Sound Transition Export", two_row_button_size))
			{
				append_to_mapsettings_workbench_export("RemixVars", preset.file_name, "Paste into [CONFIGVARS] transitions for this map in map_settings.toml.",
					build_sound_transition_snippet(preset.file_name, sound_trigger, transition_duration, transition_delay_in, transition_delay_out, transition_ease, transition_mode));
			}

			ImGui::Spacing(0, 6);
			ImGui::Separator();
			ImGui::Spacing(0, 4);

			if (ImGui::Button("Copy Modified Vars Snapshot", two_row_button_size))
			{
				copy_text_to_clipboard(build_modified_remix_vars_snapshot());
			}
			TT("Copies all Remix Vars currently marked as modified into .conf format.");

			ImGui::SameLine();
			if (ImGui::Button("Save Modified Vars Snapshot", two_row_button_size))
			{
				const auto filename = "wb_snapshot_modified.conf";
				if (write_workbench_conf_file(filename, build_modified_remix_vars_snapshot()))
				{
					request_config_refresh = true;
					game::console();
					std::cout << "[RemixVarsWorkbench] Wrote modified-vars snapshot: " << map_config_workbench_path(filename) << std::endl;
				}
			}
			TT("Writes modified Remix Vars to l4d2-rtx\\map_configs\\wb_snapshot_modified.conf.");

			return request_config_refresh;
		}
	// #
	// Dynamic Lighting / cinematic event workbench

	static const char* DYNAMIC_LIGHT_PRESETS[] =
	{
		"soft_flash",
		"alarm_red",
		"fire",
		"explosion",
		"lightning",
		"tv_glow",
		"safehouse_warm",
		"muzzle_flash",
		"spot_flash",
		"custom",
	};

	static const char* DYNAMIC_LIGHT_ANIMATIONS[] =
	{
		"stable",
		"soft_flicker",
		"broken_fluorescent",
		"fire_pulse",
		"alarm_pulse",
		"tv_noise",
		"lightning_flash",
		"generator_stutter",
		"candle_flicker",
		"unstable_bulb",
		"fluorescent_random",
		"muzzle_flash",
		"pulse_slow",
		"pulse_fast",
		"breathing",
		"strobe_fast",
		"strobe_slow",
		"rotating_yaw",
		"rotating_pitch",
		"rotating_roll",
		"rotating_beacon",
		"warning_beacon",
		"police_red_blue",
		"searchlight_sweep",
		"pendulum_sweep",
		"lighthouse_sweep",
		"disc_spin",
		"spot_axis_spin",
		"spot_axis_sweep",
	};

	std::string build_dynamic_light_event_snippet(
		const char* preset,
		const char* animation,
		const std::string& trigger,
		float duration,
		float cooldown,
		float scalar,
		float radius,
		bool use_source_origin,
		bool loop,
		const std::string& extra_fields = {})
	{
		return std::format("{{ name = \"wb_{}_{}\", trigger = {{ {} }}, preset = \"{}\", animation = \"{}\", duration = {:.3g}, cooldown = {:.3g}, scalar = {:.3g}, radius = {:.3g}, use_source_origin = {}, loop = {}{} }}",
			toml_escape_inline_string(preset),
			toml_escape_inline_string(animation),
			trigger,
			toml_escape_inline_string(preset),
			toml_escape_inline_string(animation),
			duration,
			cooldown,
			scalar,
			radius,
			use_source_origin ? "true" : "false",
			loop ? "true" : "false",
			extra_fields.empty() ? "" : std::format(", {}", extra_fields));
	}

	void cont_mapsettings_dynamic_lighting()
	{
		static int preset_index = 1; // alarm_red
		static int animation_index = 4; // alarm_pulse
		static float duration = 0.35f;
		static float cooldown = 0.25f;
		static float scalar = 1.0f;
		static float radius = 1.0f;
		static float anim_speed = 1.0f;
		static float anim_variation = 0.0f;
		static Vector anim_axis = Vector(0.0f, 0.0f, 1.0f);
		static float anim_degrees = 360.0f;
		static float anim_phase = 0.0f;
		static bool loop = false;
		static bool show_advanced = false;
		static std::string sound_trigger = "0x00000000";
		static std::string choreo_trigger = "scenes/";
		static std::string choreo_actor;
		static std::string choreo_event;
		static std::string choreo_param1;

		static std::string anchor_name = "anchor_light_01";
		static Vector anchor_position = Vector(0.0f, 0.0f, 0.0f);
		static Vector anchor_offset = Vector(0.0f, 0.0f, 0.0f);
		static Vector anchor_radiance = Vector(1.0f, 0.85f, 0.55f);
		static bool anchor_loop = false;
		static bool anchor_loop_smoothing = false;
		static bool anchor_use_shaping = false;
		static Vector anchor_direction = Vector(0.0f, 0.0f, 1.0f);
		static float anchor_degrees = 180.0f;
		static float anchor_softness = 0.0f;
		static float anchor_exponent = 0.0f;

		const auto two_row_button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f, 0.0f);
		const auto three_row_button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f, 0.0f);
		const auto four_row_button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3.0f) / 4.0f, 0.0f);

		auto find_preset_index = [](const char* name)
		{
			for (int i = 0; i < IM_ARRAYSIZE(DYNAMIC_LIGHT_PRESETS); ++i)
			{
				if (std::string(DYNAMIC_LIGHT_PRESETS[i]) == name) {
					return i;
				}
			}
			return 0;
		};

		auto find_animation_index = [](const char* name)
		{
			for (int i = 0; i < IM_ARRAYSIZE(DYNAMIC_LIGHT_ANIMATIONS); ++i)
			{
				if (std::string(DYNAMIC_LIGHT_ANIMATIONS[i]) == name) {
					return i;
				}
			}
			return 0;
		};

		auto apply_authoring_preset = [&](const char* preset, const char* animation, float new_radius, float new_scalar, float new_duration, bool new_loop, const Vector& rgb, bool cone, const char* suffix)
		{
			preset_index = find_preset_index(preset);
			animation_index = find_animation_index(animation);
			radius = new_radius;
			scalar = new_scalar;
			duration = new_duration;
			loop = new_loop;
			anchor_loop = new_loop;
			anchor_radiance = rgb;
			anchor_use_shaping = cone;
			if (cone)
			{
				anchor_degrees = 38.0f;
				anchor_softness = 0.12f;
				anchor_exponent = 0.8f;
			}
			else
			{
				anchor_degrees = 180.0f;
				anchor_softness = 0.0f;
				anchor_exponent = 0.0f;
			}
			anchor_name = std::format("{}_{}", map_settings::get_map_settings().mapname, suffix);
		};

		auto normalize_vector_or_default = [](Vector v, const Vector& fallback)
		{
			if (v.LengthSqr() > 0.0001f)
			{
				v.Normalize();
				return v;
			}
			return fallback;
		};

		auto animation_extra_fields = [&]()
		{
			const auto axis = normalize_vector_or_default(anim_axis, Vector(0.0f, 0.0f, 1.0f));
			return std::format("speed = {:.3g}, variation = {:.3g}, animation_axis = [{:.3f}, {:.3f}, {:.3f}], animation_degrees = {:.3g}, animation_phase = {:.3g}",
				anim_speed, anim_variation, axis.x, axis.y, axis.z, anim_degrees, anim_phase);
		};

		auto build_anchor_from_ui = [&]()
		{
			map_settings::light_anchor_s anchor = {};
			anchor.name = anchor_name.empty() ? "anchor_light_01" : anchor_name;
			anchor.preset = DYNAMIC_LIGHT_PRESETS[preset_index];
			anchor.animation = DYNAMIC_LIGHT_ANIMATIONS[animation_index];
			anchor.position = anchor_position;
			anchor.offset = anchor_offset;
			anchor.radiance = anchor_radiance;
			anchor.scalar = scalar;
			anchor.radius = radius;
			anchor.duration = duration;
			anchor.speed = anim_speed;
			anchor.variation = anim_variation;
			anchor.animation_axis = normalize_vector_or_default(anim_axis, Vector(0.0f, 0.0f, 1.0f));
			anchor.animation_degrees = anim_degrees;
			anchor.animation_phase = anim_phase;
			anchor.loop = anchor_loop || loop;
			anchor.loop_smoothing = anchor_loop_smoothing;
			anchor.use_shaping = anchor_use_shaping;
			anchor.direction = normalize_vector_or_default(anchor_direction, Vector(0.0f, 0.0f, 1.0f));
			anchor.degrees = anchor_degrees;
			anchor.softness = anchor_softness;
			anchor.exponent = anchor_exponent;
			anchor.comment = std::format("authoring anchor: {}", anchor.name);
			return anchor;
		};

		auto set_anchor_from_camera = [&]()
		{
			anchor_position = *game::get_current_view_origin();
			anchor_direction = normalize_vector_or_default(*game::get_current_view_forward(), Vector(0.0f, 0.0f, 1.0f));
			anchor_name = std::format("{}_{}_anchor", map_settings::get_map_settings().mapname, DYNAMIC_LIGHT_PRESETS[preset_index]);
		};

		auto build_sound_trigger_body = [&]()
		{
			const auto& sound_history = sound_events::get_history();
			if (!sound_history.empty()) {
				return std::format("sound = 0x{:08x}", sound_history.front().hash);
			}
			return std::string("sound = 0x00000000");
		};

		auto draw_light_combo_row = [&]()
		{
			ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(180.0f));
			if (ImGui::BeginCombo("Preset##DynLightPreset", DYNAMIC_LIGHT_PRESETS[preset_index]))
			{
				for (int n = 0; n < IM_ARRAYSIZE(DYNAMIC_LIGHT_PRESETS); n++)
				{
					const bool selected = preset_index == n;
					if (ImGui::Selectable(DYNAMIC_LIGHT_PRESETS[n], selected)) {
						preset_index = n;
					}
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(180.0f));
			if (ImGui::BeginCombo("Animation##DynLightAnim", DYNAMIC_LIGHT_ANIMATIONS[animation_index]))
			{
				for (int n = 0; n < IM_ARRAYSIZE(DYNAMIC_LIGHT_ANIMATIONS); n++)
				{
					const bool selected = animation_index == n;
					if (ImGui::Selectable(DYNAMIC_LIGHT_ANIMATIONS[n], selected)) {
						animation_index = n;
					}
					if (selected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		};

		auto draw_basic_light_controls = [&]()
		{
			draw_light_combo_row();
			if (ImGui::BeginTable("##dyn_light_basic_controls", 4, ImGuiTableFlags_SizingStretchSame))
			{
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Radius", &radius, 0.01f, 0.0f, 32.0f, "%.2f");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Scalar", &scalar, 0.01f, 0.0f, 100.0f, "%.2f");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Duration", &duration, 0.005f, 0.01f, 60.0f, "%.3f");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Cooldown", &cooldown, 0.005f, 0.0f, 60.0f, "%.3f");
				ImGui::EndTable();
			}
			radius = std::clamp(radius, 0.0f, 32.0f);
			scalar = std::clamp(scalar, 0.0f, 100.0f);
			duration = std::clamp(duration, 0.01f, 60.0f);
			cooldown = std::clamp(cooldown, 0.0f, 60.0f);
		};

		auto draw_animation_controls = [&]()
		{
			if (!show_advanced) {
				ImGui::TextDisabled("Advanced animation: speed %.2f | degrees %.0f | axis %.0f %.0f %.0f", anim_speed, anim_degrees, anim_axis.x, anim_axis.y, anim_axis.z);
				return;
			}
			ImGui::SeparatorText("Animation fine tuning");
			if (ImGui::BeginTable("##dyn_anim_controls", 2, ImGuiTableFlags_SizingStretchSame))
			{
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(110.0f); ImGui::DragFloat("Anim Speed", &anim_speed, 0.01f, 0.01f, 8.0f, "%.2f");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(110.0f); ImGui::DragFloat("Variation", &anim_variation, 0.01f, 0.0f, 1.0f, "%.2f");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(110.0f); ImGui::DragFloat("Anim Degrees", &anim_degrees, 0.5f, 0.0f, 1440.0f, "%.1f");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(110.0f); ImGui::DragFloat("Anim Phase", &anim_phase, 0.5f, -1440.0f, 1440.0f, "%.1f");
				ImGui::EndTable();
			}
			ImGui::Widget_PrettyDragVec3("Anim Axis", &anim_axis.x, true, 90.0f, 0.01f, -1.0f, 1.0f, "X", "Y", "Z");
			TT("Axis [0,0,1] = horizontal yaw. Degrees = rotation/sweep range. Phase = start offset.");
		};

		ImGui::TextWrapped("RTX authoring workflow: use small radii, place an anchor at the real object, and let sound/choreo/leaf only activate that fixed light.");
		const auto& ms = map_settings::get_map_settings();
		ImGui::TextDisabled("Map: %s | area %d | leaf %d | anchors %zu | events %zu", ms.mapname.c_str(), g_current_area, g_current_leaf, ms.light_anchors.size(), ms.dynamic_light_events.size());

		static const char* compat_profiles[] = { "Authoring", "Balanced", "Performance", "Ultra Performance" };
		if (dynamic_lighting::m_compat_profile < 0 || dynamic_lighting::m_compat_profile >= IM_ARRAYSIZE(compat_profiles)) {
			dynamic_lighting::m_compat_profile = 1;
		}
		ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(180.0f));
		if (ImGui::Combo("Compat Profile", &dynamic_lighting::m_compat_profile, compat_profiles, IM_ARRAYSIZE(compat_profiles))) {
			dynamic_lighting::apply_compat_profile(dynamic_lighting::m_compat_profile);
		}
		ImGui::SameLine();
		if (ImGui::Button("Apply Profile", two_row_button_size)) {
			dynamic_lighting::apply_compat_profile(dynamic_lighting::m_compat_profile);
		}
		ImGui::TextDisabled("Budget: active %u/%u | spawned/s %u/%u | muzzle/s %u/%u | hash/s %u/%u | skipped %u",
			dynamic_lighting::m_runtime_active_lights_snapshot, dynamic_lighting::m_runtime_max_active_lights,
			dynamic_lighting::m_runtime_spawned_this_second, dynamic_lighting::m_runtime_max_spawns_per_second,
			dynamic_lighting::m_runtime_muzzle_this_second, dynamic_lighting::m_runtime_max_muzzle_per_second,
			dynamic_lighting::m_runtime_hash_this_second, dynamic_lighting::m_runtime_max_sound_hash_per_second,
			dynamic_lighting::m_runtime_skipped_budget);

		imgui::toggle_button_bool(&dynamic_lighting::m_small_radius_mode, "Small Radius Mode", two_row_button_size, "Clamps helper-light radius to the Remix authoring scale. After testing, muzzle/light helpers usually belong around 0.75-6.0, not 70-700.");
		ImGui::SameLine();
		imgui::toggle_button_bool(&show_advanced, "Advanced UI", two_row_button_size, "Shows old diagnostic sliders and experimental tools. Disabled = cleaner workflow.");
		if (show_advanced)
		{
			SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
			ImGui::DragFloat("Small Radius Max", &dynamic_lighting::m_small_radius_max, 0.01f, 0.1f, 128.0f, "%.2f");
		}

		ImGui::Spacing(0, 6);
		if (ImGui::BeginTabBar("##dynamic_lighting_studio_tabs", ImGuiTabBarFlags_Reorderable))
		{
			if (ImGui::BeginTabItem("Create"))
			{
				ImGui::SeparatorText("Quick presets");
				if (ImGui::Button("Fire Pulse", four_row_button_size)) {
					apply_authoring_preset("fire", "fire_pulse", 2.5f, 1.0f, 1.2f, true, Vector(1.0f, 0.45f, 0.16f), false, "fire_anchor");
				}
				TT("Fire/molotov/barrel. Small sphere light with looped pulse.");
				ImGui::SameLine();
				if (ImGui::Button("Broken Lamp", four_row_button_size)) {
					apply_authoring_preset("soft_flash", "broken_fluorescent", 1.8f, 0.85f, 0.8f, true, Vector(0.75f, 0.88f, 1.0f), false, "broken_lamp");
				}
				ImGui::SameLine();
				if (ImGui::Button("Alarm Beacon", four_row_button_size)) {
					apply_authoring_preset("alarm_red", "warning_beacon", 2.0f, 1.15f, 1.0f, true, Vector(1.0f, 0.05f, 0.03f), true, "alarm_beacon");
				}
				ImGui::SameLine();
				if (ImGui::Button("Searchlight", four_row_button_size)) {
					apply_authoring_preset("spot_flash", "searchlight_sweep", 4.5f, 1.0f, 2.2f, true, Vector(0.9f, 0.96f, 1.0f), true, "searchlight");
				}

				if (ImGui::Button("TV Flicker", four_row_button_size)) {
					apply_authoring_preset("tv_glow", "tv_noise", 1.4f, 0.75f, 0.7f, true, Vector(0.35f, 0.55f, 1.0f), false, "tv_flicker");
				}
				ImGui::SameLine();
				if (ImGui::Button("Police Red/Blue", four_row_button_size)) {
					apply_authoring_preset("alarm_red", "police_red_blue", 2.2f, 1.0f, 0.9f, true, Vector(1.0f, 0.05f, 0.05f), true, "police_beacon");
				}
				ImGui::SameLine();
				if (ImGui::Button("Slow Pulse", four_row_button_size)) {
					apply_authoring_preset("safehouse_warm", "breathing", 3.0f, 0.8f, 2.0f, true, Vector(1.0f, 0.78f, 0.45f), false, "warm_pulse");
				}
				ImGui::SameLine();
				if (ImGui::Button("Strobe", four_row_button_size)) {
					apply_authoring_preset("soft_flash", "strobe_fast", 2.0f, 1.4f, 0.22f, true, Vector(1.0f, 1.0f, 0.95f), false, "strobe");
				}

				if (ImGui::Button("Candle", four_row_button_size)) {
					apply_authoring_preset("fire", "candle_flicker", 0.95f, 0.45f, 1.35f, true, Vector(1.0f, 0.48f, 0.12f), false, "candle_flicker");
				}
				TT("Small warm flicker for candles, lanterns and tiny fire sources.");
				ImGui::SameLine();
				if (ImGui::Button("Bad Bulb", four_row_button_size)) {
					apply_authoring_preset("safehouse_warm", "unstable_bulb", 2.2f, 0.85f, 1.05f, true, Vector(1.0f, 0.76f, 0.44f), false, "unstable_bulb");
				}
				TT("Unstable power delivery: warm bulb dips, surges and recovers.");
				ImGui::SameLine();
				if (ImGui::Button("Fluoro Random", four_row_button_size)) {
					apply_authoring_preset("tv_glow", "fluorescent_random", 2.6f, 0.95f, 0.65f, true, Vector(0.75f, 0.88f, 1.0f), false, "fluoro_random");
				}
				TT("More irregular fluorescent flicker than the older broken_fluorescent pattern.");
				ImGui::SameLine();
				if (ImGui::Button("Rotating Spot", four_row_button_size)) {
					apply_authoring_preset("spot_flash", "spot_axis_spin", 3.2f, 1.0f, 1.8f, true, Vector(0.95f, 0.98f, 1.0f), true, "rotating_spot");
				}
				TT("Spot/disc style light rotating around its authored animation_axis.");

				ImGui::SeparatorText("Current light settings");
				draw_basic_light_controls();
				draw_animation_controls();

				imgui::toggle_button_bool(&loop, "Loop Animation", two_row_button_size, "Loopuje aktualny preset. Do fire/TV/alarm/searchlight zwykle ON.");
				ImGui::SameLine();
				imgui::toggle_button_bool(&dynamic_lighting::m_draw_debug, "Draw Last Trigger", two_row_button_size, "Draws the last dynamic light trigger in-world.");

				if (ImGui::Button("Spawn Test At Camera", two_row_button_size)) {
					dynamic_lighting::spawn_test_event(DYNAMIC_LIGHT_PRESETS[preset_index], DYNAMIC_LIGHT_ANIMATIONS[animation_index]);
				}
				TT("Szybki test bez modyfikowania map_settings.toml.");
				ImGui::SameLine();
				if (ImGui::Button("Use Camera For Anchor", two_row_button_size)) {
					set_anchor_from_camera();
				}
				TT("Places the anchor at the current camera and aims it along the view direction.");

				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Anchors"))
			{
				ImGui::SeparatorText("1. Place light at real object");
				if (ImGui::Button("Anchor At Camera", three_row_button_size)) {
					set_anchor_from_camera();
				}
				TT("Move the camera to a lamp, fire, TV or alarm, then click here.");
				ImGui::SameLine();
				if (ImGui::Button("Aim From View", three_row_button_size)) {
					anchor_direction = normalize_vector_or_default(*game::get_current_view_forward(), Vector(0.0f, 0.0f, 1.0f));
				}
				ImGui::SameLine();
				if (ImGui::Button("Preview Anchor", three_row_button_size)) {
					dynamic_lighting::spawn_anchor_preview(build_anchor_from_ui());
				}

				ImGui::SeparatorText("2. Anchor settings");
				draw_basic_light_controls();
				ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(220.0f));
				ImGui::InputText("Anchor Name", &anchor_name);
				ImGui::Widget_PrettyDragVec3("Position", &anchor_position.x, true, 120.0f, 0.25f, -32768.0f, 32768.0f, "X", "Y", "Z");
				ImGui::Widget_PrettyDragVec3("RGB", &anchor_radiance.x, true, 90.0f, 0.01f, 0.0f, 32.0f, "R", "G", "B");

				imgui::toggle_button_bool(&anchor_loop, "Loop", two_row_button_size, "Loop for anchors such as fire, TV, alarm and searchlight.");
				ImGui::SameLine();
				imgui::toggle_button_bool(&anchor_use_shaping, "Cone / Spot", two_row_button_size, "Enable for searchlights, beacons and rotating lights. Disable for fire, TV and soft glow.");

				if (show_advanced)
				{
					ImGui::SeparatorText("Advanced anchor shaping");
					ImGui::Widget_PrettyDragVec3("Offset", &anchor_offset.x, true, 90.0f, 0.05f, -64.0f, 64.0f, "X", "Y", "Z");
					imgui::toggle_button_bool(&anchor_loop_smoothing, "Loop Smooth", two_row_button_size, "Smooths the loop when the light mover supports it.");
					ImGui::SameLine();
					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					ImGui::DragFloat("Cone Degrees", &anchor_degrees, 0.5f, 0.0f, 180.0f, "%.1f");
					if (anchor_use_shaping)
					{
						ImGui::Widget_PrettyDragVec3("Direction", &anchor_direction.x, true, 90.0f, 0.01f, -1.0f, 1.0f, "X", "Y", "Z");
						SET_CHILD_WIDGET_WIDTH_MAN(120.0f); ImGui::DragFloat("Softness", &anchor_softness, 0.01f, 0.0f, 3.14159f, "%.2f");
						SET_CHILD_WIDGET_WIDTH_MAN(120.0f); ImGui::DragFloat("Exponent", &anchor_exponent, 0.01f, 0.0f, 8.0f, "%.2f");
					}
					draw_animation_controls();
				}

				ImGui::SeparatorText("3. Export / bind");
				const auto anchor_toml = dynamic_lighting::build_light_anchor_toml(build_anchor_from_ui());
				const auto anchor_event_toml = dynamic_lighting::build_anchor_event_toml(anchor_name, build_sound_trigger_body(), duration, cooldown, anchor_loop || loop);
				if (ImGui::Button("Copy LIGHT_ANCHOR", two_row_button_size)) {
					copy_text_to_clipboard(anchor_toml);
				}
				ImGui::SameLine();
				if (ImGui::Button("Copy Anchor + Last Sound", two_row_button_size)) {
					copy_text_to_clipboard(anchor_toml + "\n" + anchor_event_toml);
				}
				if (ImGui::Button("Append Anchor Export", two_row_button_size)) {
					dynamic_lighting::append_anchor_export(build_anchor_from_ui(), "");
				}
				ImGui::SameLine();
				if (ImGui::Button("Append Anchor + Last Sound", two_row_button_size)) {
					dynamic_lighting::append_anchor_export(build_anchor_from_ui(), anchor_event_toml);
				}

				const auto& sound_history = sound_events::get_history();
				if (!sound_history.empty())
				{
					const auto& last = sound_history.front();
					ImGui::TextDisabled("Last sound bind: 0x%08x | %s", last.hash, last.name.c_str());
					ImGui::TextDisabled("origin %.1f %.1f %.1f", last.origin.x, last.origin.y, last.origin.z);
				}
				else {
					ImGui::TextDisabled("Last sound bind: none captured yet");
				}
				ImGui::EndTabItem();
			}

			// Muzzle Flash moved to its own V21.0 main tab.

if (ImGui::BeginTabItem("Events"))
			{
				ImGui::SeparatorText("Manual LIGHT_EVENT snippets");
				draw_basic_light_controls();
				if (show_advanced) draw_animation_controls();

				ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(180.0f));
				ImGui::InputText("Sound", &sound_trigger);
				const auto sound_snippet = build_dynamic_light_event_snippet(DYNAMIC_LIGHT_PRESETS[preset_index], DYNAMIC_LIGHT_ANIMATIONS[animation_index], std::format("sound = {}", sound_trigger), duration, cooldown, scalar, radius, false, loop, animation_extra_fields());
				if (ImGui::Button("Copy Sound Event", two_row_button_size)) copy_text_to_clipboard(sound_snippet);
				ImGui::SameLine();
				if (ImGui::Button("Append Sound Event", two_row_button_size)) append_to_mapsettings_workbench_export("LIGHT_EVENTS", DYNAMIC_LIGHT_PRESETS[preset_index], "Paste into [LIGHT_EVENTS] for this map in map_settings.toml.", sound_snippet);
				TT("Default use_source_origin=false here. Prefer activate_anchor workflow, not raw sound-origin lights.");

				ImGui::SeparatorText("Choreo / Leaf");
				ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(180.0f)); ImGui::InputText("Choreo", &choreo_trigger);
				if (show_advanced)
				{
					ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(140.0f)); ImGui::InputText("Actor", &choreo_actor);
					ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(140.0f)); ImGui::InputText("Event", &choreo_event);
					ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(140.0f)); ImGui::InputText("Param1", &choreo_param1);
				}
				std::string choreo_trigger_body = std::format("choreo = \"{}\"", toml_escape_inline_string(choreo_trigger));
				if (!choreo_actor.empty()) choreo_trigger_body += std::format(", actor = \"{}\"", toml_escape_inline_string(choreo_actor));
				if (!choreo_event.empty()) choreo_trigger_body += std::format(", event = \"{}\"", toml_escape_inline_string(choreo_event));
				if (!choreo_param1.empty()) choreo_trigger_body += std::format(", param1 = \"{}\"", toml_escape_inline_string(choreo_param1));
				const auto choreo_snippet = build_dynamic_light_event_snippet(DYNAMIC_LIGHT_PRESETS[preset_index], DYNAMIC_LIGHT_ANIMATIONS[animation_index], choreo_trigger_body, duration, cooldown, scalar, radius, false, loop, animation_extra_fields());
				if (ImGui::Button("Copy Choreo Event", two_row_button_size)) copy_text_to_clipboard(choreo_snippet);
				ImGui::SameLine();
				if (ImGui::Button("Append Choreo Event", two_row_button_size)) append_to_mapsettings_workbench_export("LIGHT_EVENTS", DYNAMIC_LIGHT_PRESETS[preset_index], "Paste into [LIGHT_EVENTS] for this map in map_settings.toml.", choreo_snippet);

				const auto current_origin = *game::get_current_view_origin();
				const auto leaf_extra = std::format("position = [{:.3f}, {:.3f}, {:.3f}], {}", current_origin.x, current_origin.y, current_origin.z, animation_extra_fields());
				const auto leaf_snippet = build_dynamic_light_event_snippet(DYNAMIC_LIGHT_PRESETS[preset_index], DYNAMIC_LIGHT_ANIMATIONS[animation_index], std::format("leafs = [{}]", has_valid_current_leaf() ? std::to_string(current_leaf_u32()) : "0"), duration, cooldown, scalar, radius, false, loop, leaf_extra);
				ImGui::TextDisabled("Leaf event uses current leaf %d and camera position %.1f %.1f %.1f", g_current_leaf, current_origin.x, current_origin.y, current_origin.z);
				if (ImGui::Button("Copy Leaf Event", two_row_button_size)) copy_text_to_clipboard(leaf_snippet);
				ImGui::SameLine();
				if (ImGui::Button("Append Leaf Event", two_row_button_size)) append_to_mapsettings_workbench_export("LIGHT_EVENTS", DYNAMIC_LIGHT_PRESETS[preset_index], "Paste into [LIGHT_EVENTS] for this map in map_settings.toml.", leaf_snippet);

				if (show_advanced)
				{
					ImGui::SeparatorText("Sound Hash Library - experimental");
					imgui::toggle_button_bool(&dynamic_lighting::m_sound_hash_library_enabled, "Hash Library", two_row_button_size, "OFF by default. Sounds are often emitted from the player, so treat this as a test tool.");
					ImGui::SameLine();
					imgui::toggle_button_bool(&dynamic_lighting::m_sound_hash_draw_debug, "Hash Debug", two_row_button_size, "Debug pozycji ostatniego hash triggera.");
					imgui::toggle_button_bool(&dynamic_lighting::m_sound_hash_reject_near_player_origin, "Reject Player-Origin", two_row_button_size, "Rejects sounds emitted too close to player/camera.");
					ImGui::SameLine(); SET_CHILD_WIDGET_WIDTH_MAN(120.0f); ImGui::DragFloat("Reject Distance", &dynamic_lighting::m_sound_hash_reject_near_player_distance, 1.0f, 0.0f, 512.0f, "%.0f");
					SET_CHILD_WIDGET_WIDTH_MAN(120.0f); ImGui::DragFloat("Hash Radius Scale", &dynamic_lighting::m_sound_hash_radius_scale, 0.001f, 0.0f, 1.0f, "%.3f");
					ImGui::SameLine(); SET_CHILD_WIDGET_WIDTH_MAN(120.0f); ImGui::DragFloat("Hash Max Radius", &dynamic_lighting::m_sound_hash_max_radius, 0.01f, 0.0f, 64.0f, "%.2f");
					ImGui::TextDisabled("Hash lib: matches=%u | spawned=%u | skipped=%u | player-origin skipped=%u", dynamic_lighting::m_sound_hash_library_matches, dynamic_lighting::m_sound_hash_library_spawned, dynamic_lighting::m_sound_hash_library_skipped, dynamic_lighting::m_sound_hash_library_skipped_player_origin);
					ImGui::TextDisabled("Last hash: 0x%08x | %s | %s", dynamic_lighting::m_sound_hash_last_hash, dynamic_lighting::m_sound_hash_last_category.c_str(), dynamic_lighting::m_sound_hash_last_sound.c_str());
				}
				ImGui::EndTabItem();
			}


			if (ImGui::BeginTabItem("Light Runtime"))
			{
				ImGui::TextWrapped("Global light budgets, compatibility quality and CPU skinning controls moved to the main Performance tab. This panel keeps only light-system-specific runtime controls.");

				auto& ies_cluster_budget = remix_lights::ies_cluster_global_budget();
				SET_CHILD_WIDGET_WIDTH_MAN(130.0f);
				if (ImGui::DragInt("IES Helper Budget", &ies_cluster_budget, 1.0f, 0, 512, "%d", ImGuiSliderFlags_AlwaysClamp)) {
					ies_cluster_budget = std::clamp(ies_cluster_budget, 0, 512);
				}
				TT("Global cap for Fake IES child/helper lights. 0 disables cluster helper spawning; the controller light still works.");

				static const char* IES_CLUSTER_QUALITY[] = { "Off", "Low Preview", "Balanced", "Authored", "Stress +2" };
				auto& ies_quality_mode = remix_lights::ies_cluster_quality_mode();
				ies_quality_mode = std::clamp(ies_quality_mode, 0, 4);
				SET_CHILD_WIDGET_WIDTH_MAN(130.0f);
				ImGui::Combo("IES Helper Quality", &ies_quality_mode, IES_CLUSTER_QUALITY, IM_ARRAYSIZE(IES_CLUSTER_QUALITY));
				TT("Runtime quality scaler for Fake IES helpers. Off keeps only controller lights; Low/Balanced help diagnose object flicker or bridge overload; Authored uses TOML sample counts; Stress adds a small over-budget test.");

				auto& group_filter_enabled = remix_lights::runtime_group_filter_enabled();
				auto& group_filter = remix_lights::runtime_group_filter();
				ImGui::Checkbox("Runtime Light Group Filter", &group_filter_enabled);
				TT("When enabled, DrawLightInstance/FF mirror and future runtime spawning keep only lights whose group exactly matches this value.");
				ImGui::SameLine();
				SET_CHILD_WIDGET_WIDTH_MAN(130.0f);
				ImGui::InputText("Group##RuntimeLightGroup", &group_filter);

				ImGui::TextDisabled("Budget skips: total=%u | active=%u | pending=%u | muzzle=%u | hash=%u | IES helpers=%u/%d",
					dynamic_lighting::m_runtime_skipped_budget, dynamic_lighting::m_runtime_skipped_active_limit, dynamic_lighting::m_runtime_skipped_pending_limit,
					dynamic_lighting::m_runtime_skipped_muzzle_budget, dynamic_lighting::m_runtime_skipped_hash_budget, remix_lights::get()->get_ies_cluster_child_count(), ies_cluster_budget);
				if (ImGui::Button("Reset Runtime Counters", two_row_button_size)) dynamic_lighting::reset_runtime_budget_counters();
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Advanced"))
			{
				ImGui::SeparatorText("D3D SetLight / LightEnable Sniffer");
				imgui::toggle_button_bool(&dynamic_lighting::m_d3d_light_capture_enabled, "Capture SetLight", two_row_button_size, "Experimental. L4D2 usually does not expose baked lights through SetLight.");
				ImGui::SameLine();
				imgui::toggle_button_bool(&dynamic_lighting::m_d3d_light_spawn_enabled, "Mirror To Remix", two_row_button_size, "Experimental mirror do Remix.");
				ImGui::TextDisabled("Hook: %s | Installed: %s | Attempts: %u | Failures: %u", dynamic_lighting::m_d3d_hook_status.c_str(), dynamic_lighting::m_d3d_hooks_installed ? "yes" : "no", dynamic_lighting::m_d3d_hook_attempts, dynamic_lighting::m_d3d_hook_failures);
				ImGui::TextDisabled("SetLight: %u | LightEnable: %u | Spawned: %u | Last pos: %.1f %.1f %.1f", dynamic_lighting::m_d3d_setlight_calls, dynamic_lighting::m_d3d_lightenable_calls, dynamic_lighting::m_d3d_spawned_lights, dynamic_lighting::m_d3d_last_origin.x, dynamic_lighting::m_d3d_last_origin.y, dynamic_lighting::m_d3d_last_origin.z);
				ImGui::Spacing(0.0f, 4.0f);
				ImGui::SeparatorText("FF SetLight Mirror Backend");
				auto& ff_mirror_enabled = remix_lights::ff_setlight_mirror_enabled();
				auto& ff_mirror_only_visible = remix_lights::ff_setlight_mirror_only_visible();
				auto& ff_start_index = remix_lights::ff_setlight_start_index();
				auto& ff_max_lights = remix_lights::ff_setlight_max_lights();
				auto& ff_scalar = remix_lights::ff_setlight_scalar();
				auto& ff_radius_scale = remix_lights::ff_setlight_radius_scale();
				ImGui::Checkbox("Mirror active RTX lights to D3D SetLight", &ff_mirror_enabled);
				TT("Experimental. Emits our authored/animated RTX lights as D3D9 fixed-function SetLight state as an extra Remix-compatible backend. This does not recover baked map lightmaps automatically.");
				ImGui::Checkbox("Only mirror visible/spawned lights", &ff_mirror_only_visible);
				ImGui::DragInt("FF Start Index", &ff_start_index, 1.0f, 0, 7);
				ImGui::DragInt("FF Max Lights", &ff_max_lights, 1.0f, 0, 8);
				ImGui::DragFloat("FF Scalar", &ff_scalar, 0.005f, 0.0f, 2.0f, "%.3f");
				ImGui::DragFloat("FF Radius Scale", &ff_radius_scale, 0.01f, 0.01f, 10.0f, "%.2f");
				ImGui::TextDisabled("FF emitted: %u | failed: %u", remix_lights::ff_setlight_emitted(), remix_lights::ff_setlight_failed());
				SET_CHILD_WIDGET_WIDTH_MAN(120.0f); ImGui::DragFloat("D3D Scalar", &dynamic_lighting::m_d3d_light_scalar, 0.01f, 0.0f, 50.0f);
				ImGui::SameLine(); SET_CHILD_WIDGET_WIDTH_MAN(120.0f); ImGui::DragFloat("D3D Radius Scale", &dynamic_lighting::m_d3d_light_radius_scale, 0.01f, 0.01f, 10.0f);
				SET_CHILD_WIDGET_WIDTH_MAN(120.0f); ImGui::DragFloat("D3D Duration", &dynamic_lighting::m_d3d_light_duration, 0.001f, 0.01f, 2.0f);
				ImGui::SameLine(); SET_CHILD_WIDGET_WIDTH_MAN(120.0f); ImGui::DragFloat("D3D Cooldown", &dynamic_lighting::m_d3d_light_cooldown, 0.001f, 0.0f, 1.0f);

				ImGui::SeparatorText("Runtime status");
				ImGui::Text("Parsed dynamic light events: %zu", ms.dynamic_light_events.size());
				ImGui::Text("Parsed light anchors: %zu", ms.light_anchors.size());
				ImGui::Text("Last trigger: %s @ %.2f", dynamic_lighting::m_debug_last_trigger_name.empty() ? "none" : dynamic_lighting::m_debug_last_trigger_name.c_str(), dynamic_lighting::m_debug_last_trigger_time);
				ImGui::TextDisabled("Last trigger pos %.1f %.1f %.1f", dynamic_lighting::m_debug_last_trigger_origin.x, dynamic_lighting::m_debug_last_trigger_origin.y, dynamic_lighting::m_debug_last_trigger_origin.z);
				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}
	}


	void cont_mapsettings_muzzle_flash()
	{
		const auto two_row_button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f, 0.0f);
		const auto four_row_button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3.0f) / 4.0f, 0.0f);

		ui_subsection("Separated Muzzle Flash", "Balanced detection prioritizes real fire samples before handling filters and suppresses duplicate shot layers by cooldown.", ICON_FA_BOLT);

		imgui::toggle_button_bool(&dynamic_lighting::m_auto_muzzle_flash, "Auto Muzzle Flash", two_row_button_size, "ON: detects actual weapon-fire sounds and spawns a short flash near flashlight/camera.");
		ImGui::SameLine();
		if (ImGui::Button("Test Muzzle Flash", two_row_button_size)) {
			dynamic_lighting::test_muzzle_flash();
		}

		ImGui::Spacing(0, 6);
		if (ImGui::BeginTabBar("##muzzle_flash_tabs", ImGuiTabBarFlags_Reorderable))
		{
			if (ImGui::BeginTabItem(ICON_FA_SLIDERS_H "  Shape"))
			{
				static const char* muzzle_origin_modes[] = { "Player Flashlight", "Camera/View", "Sound Origin", "Sound Origin + Eye Height" };
				if (dynamic_lighting::m_muzzle_flash_origin_mode < 0 || dynamic_lighting::m_muzzle_flash_origin_mode >= IM_ARRAYSIZE(muzzle_origin_modes)) dynamic_lighting::m_muzzle_flash_origin_mode = 0;
				ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(180.0f));
				ImGui::Combo("Origin", &dynamic_lighting::m_muzzle_flash_origin_mode, muzzle_origin_modes, IM_ARRAYSIZE(muzzle_origin_modes));
				TT("Player Flashlight/Camera are usually best. Sound Origin often sits at survivor feet or at a weapon script event.");
				ImGui::SameLine();
				if (ImGui::Button("Reset V21 Defaults"))
				{
					dynamic_lighting::m_auto_muzzle_flash = true;
					dynamic_lighting::m_muzzle_flash_origin_mode = 0;
					dynamic_lighting::m_muzzle_flash_shape_mode = 0;
					dynamic_lighting::m_muzzle_weapon_profile_mode = 0;
					dynamic_lighting::m_muzzle_weapon_profiles_enabled = true;
					dynamic_lighting::m_muzzle_flash_radius = 0.02f;
					dynamic_lighting::m_muzzle_flash_scalar = 0.07f;
					dynamic_lighting::m_muzzle_flash_duration = 0.300f;
					dynamic_lighting::m_muzzle_flash_fade = 0.090f;
					dynamic_lighting::m_muzzle_flash_delay = 0.000f;
					dynamic_lighting::m_muzzle_flash_cooldown = 0.018f;
					dynamic_lighting::m_muzzle_flash_color = Vector(1.0f, 0.68f, 0.30f);
					dynamic_lighting::m_muzzle_flash_offset = Vector(18.0f, -2.0f, -3.0f);
					dynamic_lighting::m_muzzle_strict_fire_tokens = false;
					dynamic_lighting::m_muzzle_reject_weapon_handling = true;
					dynamic_lighting::m_muzzle_allow_weapon_family_fallback = true;
					dynamic_lighting::m_muzzle_draw_debug = false;
					game_settings::mark_dirty("muzzle defaults");
				}
				TT("Restores the V21 sphere-light baseline, 300 ms envelope and warm weapon flash color.");

				if (ImGui::BeginTable("##muzzle_standalone_basic", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Radius", &dynamic_lighting::m_muzzle_flash_radius, 0.001f, 0.001f, 4.0f, "%.3f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Intensity", &dynamic_lighting::m_muzzle_flash_scalar, 0.005f, 0.0f, 10.0f, "%.3f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Duration", &dynamic_lighting::m_muzzle_flash_duration, 0.005f, 0.025f, 1.0f, "%.3f s");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Fade", &dynamic_lighting::m_muzzle_flash_fade, 0.005f, 0.005f, 1.0f, "%.3f s");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Delay", &dynamic_lighting::m_muzzle_flash_delay, 0.001f, 0.0f, 0.5f, "%.3f s");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Cooldown", &dynamic_lighting::m_muzzle_flash_cooldown, 0.001f, 0.0f, 1.0f, "%.3f");
					ImGui::EndTable();
				}
				ImGui::ColorEdit3("Flash Color", &dynamic_lighting::m_muzzle_flash_color.x, ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_PickerHueBar);

				ImGui::Widget_PrettyDragVec3("Muzzle Offset", &dynamic_lighting::m_muzzle_flash_offset.x, true, 90.0f, 0.1f, -512.0f, 512.0f, "F", "H", "V");
				TT("F=forward, H=height/up, V=right. Keep radius small; this is a close weapon flash, not a room light.");

				static const char* muzzle_shape_modes[] = { "Sphere Light", "Cone/Spot Light" };
				static const char* muzzle_weapon_profiles[] = { "Auto", "Pistol", "SMG", "Rifle", "Shotgun", "Sniper", "Mounted Gun" };
				if (dynamic_lighting::m_muzzle_flash_shape_mode < 0 || dynamic_lighting::m_muzzle_flash_shape_mode >= IM_ARRAYSIZE(muzzle_shape_modes)) dynamic_lighting::m_muzzle_flash_shape_mode = 0;
				if (dynamic_lighting::m_muzzle_weapon_profile_mode < 0 || dynamic_lighting::m_muzzle_weapon_profile_mode >= IM_ARRAYSIZE(muzzle_weapon_profiles)) dynamic_lighting::m_muzzle_weapon_profile_mode = 0;
				ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(180.0f));
				ImGui::Combo("Shape", &dynamic_lighting::m_muzzle_flash_shape_mode, muzzle_shape_modes, IM_ARRAYSIZE(muzzle_shape_modes));
				ImGui::SameLine();
				ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(180.0f));
				ImGui::Combo("Weapon Profile", &dynamic_lighting::m_muzzle_weapon_profile_mode, muzzle_weapon_profiles, IM_ARRAYSIZE(muzzle_weapon_profiles));

				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem(ICON_FA_CROSSHAIRS "  Detection"))
			{
				ui_toggle_row("Weapon Profiles", &dynamic_lighting::m_muzzle_weapon_profiles_enabled,
					"Auto profile for pistol/SMG/rifle/shotgun/sniper.",
					"Muzzle Debug", &dynamic_lighting::m_muzzle_draw_debug,
					"Draws last flash position and forward vector.");
				ui_toggle_row("Balanced Family Fallback", &dynamic_lighting::m_muzzle_allow_weapon_family_fallback,
					"Accepts known firearm-family samples when no explicit fire token exists and no handling/tail token is present.",
					"Reject Handling", &dynamic_lighting::m_muzzle_reject_weapon_handling,
					"Rejects handling-only sounds, but explicit fire/gunfire evidence now has priority over pump/bolt/slide words.");
				ImGui::Checkbox("Strict explicit-fire tokens only", &dynamic_lighting::m_muzzle_strict_fire_tokens);
				TT("Authoring option. Disables the balanced weapon-family fallback; explicit fire/gunfire samples still bypass ambiguous handling words.");

				ImGui::Spacing(0, 8);
				ImGui::TextDisabled("Recommended gameplay mode:");
				ui_status_pill(dynamic_lighting::m_muzzle_strict_fire_tokens ? "Strict classifier" : "Balanced classifier", !dynamic_lighting::m_muzzle_strict_fire_tokens);
				ImGui::SameLine();
				ui_status_pill(dynamic_lighting::m_muzzle_reject_weapon_handling ? "Handling blacklist ON" : "Handling blacklist OFF", dynamic_lighting::m_muzzle_reject_weapon_handling);

				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem(ICON_FA_TACHOMETER_ALT "  Runtime Status"))
			{
				if (ImGui::BeginTable("##muzzle_status_cards", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableNextColumn(); ui_metric_card("Candidates", std::format("{}", dynamic_lighting::m_muzzle_candidate_sounds));
					ImGui::TableNextColumn(); ui_metric_card("Accepted", std::format("{}", dynamic_lighting::m_muzzle_accepted_sounds));
					ImGui::TableNextColumn(); ui_metric_card("Spawn attempts", std::format("{}", dynamic_lighting::m_muzzle_spawn_attempts));
					ImGui::TableNextColumn(); ui_metric_card("Non-fire rejects", std::format("{}", dynamic_lighting::m_muzzle_rejected_non_fire));
					ImGui::TableNextColumn(); ui_metric_card("Cooldown dupes", std::format("{}", dynamic_lighting::m_muzzle_skipped_cooldown));
					ImGui::TableNextColumn(); ui_metric_card("Budget skips", std::format("{}", dynamic_lighting::m_muzzle_skipped_budget));
					ImGui::EndTable();
				}
				ImGui::TextDisabled("Last accepted: %s | Last reject/skip: %s",
					dynamic_lighting::m_muzzle_last_accept_reason.c_str(), dynamic_lighting::m_muzzle_last_reject_reason.c_str());
				ImGui::TextDisabled("Hard rejects: %u | Strict rejects: %u | Profile: %s",
					dynamic_lighting::m_muzzle_rejected_hard, dynamic_lighting::m_muzzle_rejected_strict,
					dynamic_lighting::m_muzzle_last_profile.empty() ? "none" : dynamic_lighting::m_muzzle_last_profile.c_str());

				ImGui::TextDisabled("Last sound: %s", dynamic_lighting::m_muzzle_last_sound.c_str());
				ImGui::TextDisabled("pos %.1f %.1f %.1f | fwd %.2f %.2f %.2f",
					dynamic_lighting::m_muzzle_last_origin.x, dynamic_lighting::m_muzzle_last_origin.y, dynamic_lighting::m_muzzle_last_origin.z,
					dynamic_lighting::m_muzzle_last_forward.x, dynamic_lighting::m_muzzle_last_forward.y, dynamic_lighting::m_muzzle_last_forward.z);

				if (ImGui::Button("Reset Muzzle Counters", four_row_button_size)) {
					dynamic_lighting::m_muzzle_candidate_sounds = 0u;
					dynamic_lighting::m_muzzle_accepted_sounds = 0u;
					dynamic_lighting::m_muzzle_rejected_non_fire = 0u;
					dynamic_lighting::m_muzzle_rejected_hard = 0u;
					dynamic_lighting::m_muzzle_rejected_strict = 0u;
					dynamic_lighting::m_muzzle_skipped_cooldown = 0u;
					dynamic_lighting::m_muzzle_skipped_budget = 0u;
					dynamic_lighting::m_muzzle_spawn_attempts = 0u;
					dynamic_lighting::m_muzzle_last_accept_reason = "none";
					dynamic_lighting::m_muzzle_last_reject_reason = "none";
				}

				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}
	}


	// #
	// #

	void cont_general_map_quick_access()
	{
		static constexpr const char* QUICK_MAPS[] =
		{
			"c1m1_hotel", "c1m2_streets", "c1m3_mall", "c1m4_atrium",
			"c2m1_highway", "c2m2_fairgrounds", "c2m3_coaster", "c2m4_barns", "c2m5_concert",
			"c3m1_plankcountry", "c3m2_swamp", "c3m3_shantytown", "c3m4_plantation",
			"c4m1_milltown_a", "c4m2_sugarmill_a", "c4m3_sugarmill_b", "c4m4_milltown_b", "c4m5_milltown_escape",
			"c5m1_waterfront", "c5m2_park", "c5m3_cemetery", "c5m4_quarter", "c5m5_bridge",
			"c6m1_riverbank", "c6m2_bedlam", "c6m3_port",
			"c7m1_docks", "c7m2_barge", "c7m3_port",
			"c8m1_apartment", "c8m2_subway", "c8m3_sewers", "c8m4_interior", "c8m5_rooftop",
			"c9m1_alleys", "c9m2_lots",
			"c10m1_caves", "c10m2_drainage", "c10m3_ranchhouse", "c10m4_mainstreet", "c10m5_houseboat",
			"c11m1_greenhouse", "c11m2_offices", "c11m3_garage", "c11m4_terminal", "c11m5_runway",
			"c12m1_hilltop", "c12m2_traintunnel", "c12m3_bridge", "c12m4_barn", "c12m5_cornfield",
			"c13m1_alpinecreek", "c13m2_southpinestream", "c13m3_memorialbridge", "c13m4_cutthroatcreek"
		};

		static std::string map_filter;
		static std::string selected_map = QUICK_MAPS[0];
		const std::string current_map = map_settings::get_map_name();

		ui_subsection("Maps", "Current map, filtered campaign selector and safe one-click reload actions.", ICON_FA_MAP);
		if (ImGui::BeginTable("##dev_map_cards", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ui_metric_card("Current map", current_map.empty() ? "not loaded" : current_map);
			ImGui::TableNextColumn(); ui_metric_card("Selection", selected_map);
			ImGui::TableNextColumn(); ui_metric_card("Map light DB", dynamic_lighting::m_persistent_map_light_status.empty() ? "idle" : dynamic_lighting::m_persistent_map_light_status);
			ImGui::EndTable();
		}

		ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(220.0f));
		ImGui::InputTextWithHint("##quick_map_filter", "Filter map name...", &map_filter);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(260.0f));
		if (ImGui::BeginCombo("##quick_map_combo", selected_map.c_str()))
		{
			for (const char* map_name : QUICK_MAPS)
			{
				if (!map_filter.empty() && !ui_filter_contains(map_name, map_filter)) continue;
				const bool selected = selected_map == map_name;
				if (ImGui::Selectable(map_name, selected)) selected_map = map_name;
				if (selected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const ImVec2 button_size((ImGui::GetContentRegionAvail().x - spacing * 3.0f) / 4.0f, 0.0f);
		if (ImGui::Button(ICON_FA_MAP_MARKED_ALT "  Load Selected", button_size))
			interfaces::get()->m_engine->execute_client_cmd_unrestricted(("map " + selected_map).c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(current_map.empty());
		if (ImGui::Button(ICON_FA_REDO "  Restart Current", button_size))
			interfaces::get()->m_engine->execute_client_cmd_unrestricted(("map " + current_map).c_str());
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_SYNC "  Reload Map Data", button_size))
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("xo_mapsettings_update; xo_gamesettings_update");
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_LIGHTBULB "  Open Map Lights", button_size))
		{
			map_settings::save_current_map_authoring_data(true);
			imgui::get()->m_light_edit_mode = true;
			map_settings::reload();
			imgui::get()->m_light_edit_mode = true;
			if (dynamic_lighting::m_map_light_auto_sync_editor)
				dynamic_lighting::sync_imported_map_lights_to_light_editor();
		}
	}

	void cont_general_gameplay_quick_access()
	{
		auto run = [](const char* command)
		{
			if (interfaces::get() && interfaces::get()->m_engine)
				interfaces::get()->m_engine->execute_client_cmd_unrestricted(command);
		};

		auto gs = game_settings::get();
		ui_subsection("Movement and Director", "Fast authoring controls with persistent, throttled automation. No command is repeated every frame.", ICON_FA_RUNNING);

		if (ImGui::BeginTable("##dev_quick_actions", ui_responsive_column_count(150.0f, 4), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn();
			if (ImGui::Button(ICON_FA_RUNNING "  Toggle No Clip", ImVec2(-1.0f, 0.0f))) run("sv_cheats 1; noclip");
			TT("Toggles Source noclip for the local player.");

			ImGui::TableNextColumn();
			if (ImGui::Button(ICON_FA_STOP "  Director Stop", ImVec2(-1.0f, 0.0f))) run("sv_cheats 1; director_stop");

			ImGui::TableNextColumn();
			if (ImGui::Button(ICON_FA_PLAY "  Director Start", ImVec2(-1.0f, 0.0f)))
			{
				gs->quick_auto_stop_director.set_var(false);
				run("sv_cheats 1; director_start");
			}
			TT("Starting the Director also disables Auto Stop Director so it is not stopped again on the next retry.");

			ImGui::TableNextColumn();
			if (ImGui::Button(ICON_FA_USER_SLASH "  Kick Bots Now", ImVec2(-1.0f, 0.0f)))
				run("sv_cheats 1; kick rochelle; kick coach; kick ellis; kick nick; kick louis; kick zoey; kick francis; kick bill");
			ImGui::EndTable();
		}

		if (ImGui::BeginTable("##dev_persistent_toggles", ui_responsive_column_count(260.0f, 2), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn();
			if (ImGui::Checkbox("Auto Stop Director", gs->quick_auto_stop_director.get_as<bool*>()))
			{
				if (gs->quick_auto_stop_director.get_as<bool>()) run("sv_cheats 1; director_stop");
			}
			TT("Persisted in game_settings.toml. Re-applied after map load and then at a five-second retry interval.");
			ImGui::TextDisabled("%s", gs->quick_auto_stop_director.get_as<bool>() ? "Persistent Director stop enabled" : "Manual Director control");

			ImGui::TableNextColumn();
			if (ImGui::Checkbox("Auto Kick Survivor Bots", gs->quick_auto_kick_bots.get_as<bool*>()))
			{
				if (gs->quick_auto_kick_bots.get_as<bool>())
					run("sv_cheats 1; kick rochelle; kick coach; kick ellis; kick nick; kick louis; kick zoey; kick francis; kick bill");
			}
			TT("Persisted in game_settings.toml. Starts after map initialization and retries every eight seconds without frame spam.");
			ImGui::TextDisabled("%s", gs->quick_auto_kick_bots.get_as<bool>() ? "Bot removal guard enabled" : "Bots are not removed automatically");
			ImGui::EndTable();
		}

		static float noclip_speed = 12.0f;
		if (ImGui::BeginTable("##dev_noclip_speed", ui_responsive_column_count(130.0f, 5), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::DragFloat("No Clip Speed", &noclip_speed, 0.25f, 1.0f, 60.0f, "%.1f"))
				noclip_speed = std::clamp(noclip_speed, 1.0f, 60.0f);
			ImGui::TableNextColumn(); if (ImGui::Button("Apply", ImVec2(-1.0f, 0.0f))) run(utils::va("sv_cheats 1; sv_noclipspeed %.2f", noclip_speed));
			ImGui::TableNextColumn(); if (ImGui::Button("Slow", ImVec2(-1.0f, 0.0f))) { noclip_speed = 5.0f; run("sv_cheats 1; sv_noclipspeed 5"); }
			ImGui::TableNextColumn(); if (ImGui::Button("Normal", ImVec2(-1.0f, 0.0f))) { noclip_speed = 12.0f; run("sv_cheats 1; sv_noclipspeed 12"); }
			ImGui::TableNextColumn(); if (ImGui::Button("Fast", ImVec2(-1.0f, 0.0f))) { noclip_speed = 30.0f; run("sv_cheats 1; sv_noclipspeed 30"); }
			ImGui::EndTable();
		}
	}

	void cont_general_quickcommands()
	{
		const auto four_row_button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4.0f, 0);
		const auto three_row_button_size = ImVec2(ImGui::CalcButtonWidthSameRow(3), 0);

		ImGui::SeparatorTextLarge(" AI / Bot Overrides ", false);
		ImGui::TextDisabled("Director Start/Stop and bot removal live only in Dev -> Movement / Director to avoid duplicate controls.");

		imgui::cvar_toggle_button_bool("director_no_specials", "No Specials", four_row_button_size, "director_no_specials :: This command, if set to 1, will disable the spawning of PZ zombies");
		ImGui::SameLine();
		imgui::cvar_toggle_button_bool("director_no_bosses", "No Bosses", four_row_button_size, "director_no_bosses :: Setting this command to 1 will completely disable the spawning of bosses");
		ImGui::SameLine();
		imgui::cvar_toggle_button_bool("sb_stop", "Freeze Surv Bots", four_row_button_size, "sb_stop :: This command, if set to 1, will stop all survivor bots, but not zombie bots");
		ImGui::SameLine();
		imgui::cvar_toggle_button_bool("nb_stop", "Freeze All Bots", four_row_button_size, "nb_stop :: This command will freeze all bots in the game. Setting this command back to 0 will unfreeze the bots");

		imgui::cvar_toggle_button_bool("nb_vision_ignore_survivors", "Infected Ignore Player", four_row_button_size, "nb_vision_ignore_survivors :: Ignore survivor targets while testing infected navigation");

		ImGui::Spacing(0, 8);
		ImGui::SeparatorTextLarge(" Screenshot / HUD Settings ");

		{
			// button toggling both options
			const auto r_drawviewmodel = game::find_cvar_const("r_drawviewmodel");
			const auto r_drawvgui = game::find_cvar_const("r_drawvgui");
			if (r_drawviewmodel && r_drawvgui)
			{
				const bool is_active = !(r_drawviewmodel->m_Value.m_nValue && r_drawvgui->m_Value.m_nValue);
				if (is_active)
				{
					ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
				}

				if (ImGui::Button("Toggle Screenshot Mode", ImVec2(ImGui::GetContentRegionAvail().x, 38)))
				{
					const char* screenshot_mode_str = is_active ? "1" : "0";
					interfaces::get()->m_engine->execute_client_cmd_unrestricted(utils::va("sv_cheats 1; r_drawviewmodel %s; r_drawvgui %s", screenshot_mode_str, screenshot_mode_str));
				}

				if (is_active) {
					ImGui::SafePopStyleColor(3, __LINE__);
				}
			}

			// single options
			imgui::cvar_toggle_button_bool("r_drawviewmodel", "Hide Viewmodel", three_row_button_size, "r_drawviewmodel :: Toggle viewmodel drawing", true);

			ImGui::SameLine();
			imgui::cvar_toggle_button_int("hidehud", "Hide HUD", three_row_button_size, "hidehud :: Toggle In-Game HUD drawing", 0, 4);

			ImGui::SameLine();
			imgui::cvar_toggle_button_bool("r_drawvgui", "Hide VGui", three_row_button_size, "r_drawvgui :: Toggle UI drawing", true);
		}

		ImGui::Spacing(0, 8);
		ImGui::SeparatorTextLarge(" Cheats ");

		imgui::cvar_toggle_button_bool("god", "God", four_row_button_size, "god :: This cheat enables and disables god mode for your entire team. In god mode, you and your team are invincible and will not take any damage");

		ImGui::SameLine();
		imgui::cvar_toggle_button_bool("buddha", "Buddha", four_row_button_size, "buddha :: Enables or disables buddha mode (you appear to take damage but can't die)");

		ImGui::SameLine();
		imgui::cvar_toggle_button_bool("sv_infinite_ammo", "Infinite Ammo", four_row_button_size, "sv_infinite_ammo :: Enables and disable infinite ammo");






	}

	void cont_general_infected()
	{
		const auto three_row_button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f, 0);

		ImGui::SeparatorTextLarge(" Infected Spawing ");

		if (ImGui::Button("Spawn Infected", three_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; z_spawn zombie");
		}

		ImGui::SameLine();
		if (ImGui::Button("Spawn Spitter", three_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; z_spawn spitter");
		}

		ImGui::SameLine();
		if (ImGui::Button("Spawn Jockey", three_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; z_spawn jockey");
		}


		if (ImGui::Button("Spawn Charger", three_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; z_spawn charger");
		}

		ImGui::SameLine();
		if (ImGui::Button("Spawn Boomer", three_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; z_spawn boomer");
		}

		ImGui::SameLine();
		if (ImGui::Button("Spawn Hunter", three_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; z_spawn hunter");
		}


		if (ImGui::Button("Spawn Witch", three_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; z_spawn witch");
		}

		ImGui::SameLine();
		if (ImGui::Button("Spawn Smoker", three_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; z_spawn smoker");
		}

		ImGui::SameLine();
		if (ImGui::Button("Spawn Tank", three_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; z_spawn tank");
		}
	}

	void cont_general_weapons()
	{
		const auto four_row_button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4.0f, 0);

		if (ImGui::Button("Give Pistol", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give pistol");
		} TT("sv_cheats 1; give pistol");

		ImGui::SameLine();
		if (ImGui::Button("Give Pistol Magnum", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give pistol_magnum");
		} TT("sv_cheats 1; give pistol_magnum");

		ImGui::SameLine();
		if (ImGui::Button("Give Autoshotgun", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give autoshotgun");
		} TT("sv_cheats 1; give autoshotgun");

		ImGui::SameLine();
		if (ImGui::Button("Give Shotgun Chrome", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give shotgun_chrome");
		} TT("sv_cheats 1; give shotgun_chrome");

		//
		if (ImGui::Button("Give Pumpshotgun", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give pumpshotgun");
		} TT("sv_cheats 1; give pumpshotgun");

		ImGui::SameLine();
		if (ImGui::Button("Give Shotgun Spas", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give shotgun_spas");
		} TT("sv_cheats 1; give shotgun_spas");

		ImGui::SameLine();
		if (ImGui::Button("Give SMG", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give smg");
		} TT("sv_cheats 1; give smg");

		ImGui::SameLine();
		if (ImGui::Button("Give Mp5", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give smg_mp5");
		} TT("sv_cheats 1; give smg_mp5");

		//
		if (ImGui::Button("Give SMG Silenced", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give smg_silenced");
		} TT("sv_cheats 1; give smg_silenced");

		ImGui::SameLine();
		if (ImGui::Button("Give Ak47", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give rifle_ak47");
		} TT("sv_cheats 1; give rifle_ak47");

		ImGui::SameLine();
		if (ImGui::Button("Give Sg552", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give rifle_sg552");
		} TT("sv_cheats 1; give rifle_sg552");

		ImGui::SameLine();
		if (ImGui::Button("Give M16", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give rifle");
		} TT("sv_cheats 1; give rifle");


		//
		if (ImGui::Button("Give M60", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give rifle_m60");
		} TT("sv_cheats 1; give rifle_m60");

		ImGui::SameLine();
		if (ImGui::Button("Give Combat Rifle", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give rifle_desert");
		} TT("sv_cheats 1; give rifle_desert");

		ImGui::SameLine();
		if (ImGui::Button("Give Hunting Rifle", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give hunting_rifle");
		} TT("sv_cheats 1; give hunting_rifle");

		ImGui::SameLine();
		if (ImGui::Button("Give Sniper Military", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give sniper_military");
		} TT("sv_cheats 1; give sniper_military");


		//
		if (ImGui::Button("Give AWP", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give sniper_awp");
		} TT("sv_cheats 1; give sniper_awp");

		ImGui::SameLine();
		if (ImGui::Button("Give Sniper Scout", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give sniper_scout");
		} TT("sv_cheats 1; give sniper_scout");

		ImGui::SameLine();
		if (ImGui::Button("Give Grenade Launcher", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give weapon_grenade_launcher");
		} TT("sv_cheats 1; give weapon_grenade_launcher");

		ImGui::SameLine();
		if (ImGui::Button("Give Hunter Claws", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give melee");
		} TT("sv_cheats 1; give melee");

		//
		if (ImGui::Button("Give Boomer Bile", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give vomitjar");
		} TT("sv_cheats 1; give vomitjar");

		ImGui::SameLine();
		if (ImGui::Button("Give Chainsaw", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give chainsaw");
		} TT("sv_cheats 1; give chainsaw");

		ImGui::SameLine();
		if (ImGui::Button("Give Frying Pan", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give frying_pan");
		} TT("sv_cheats 1; give frying_pan");

		ImGui::SameLine();
		if (ImGui::Button("Give Guitar", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give electric_guitar");
		} TT("sv_cheats 1; give electric_guitar");

		//
		if (ImGui::Button("Give Katana", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give katana");
		} TT("sv_cheats 1; give katana");

		ImGui::SameLine();
		if (ImGui::Button("Give Machete", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give machete");
		} TT("sv_cheats 1; give machete");

		ImGui::SameLine();
		if (ImGui::Button("Give Nightstick", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give tonfa");
		} TT("sv_cheats 1; give tonfa");

		ImGui::SameLine();
		if (ImGui::Button("Give Molotov", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give molotov");
		} TT("sv_cheats 1; give molotov");

		//
		if (ImGui::Button("Give Pipe Bomb", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give pipe_bomb");
		} TT("sv_cheats 1; give pipe_bomb");

		ImGui::SameLine();
		if (ImGui::Button("Give Propane Tank", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give propanetank");
		} TT("sv_cheats 1; give propanetank");

		ImGui::SameLine();
		if (ImGui::Button("Give Gas Can", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give gascan");
		} TT("sv_cheats 1; give gascan");

		ImGui::SameLine();
		if (ImGui::Button("Give Oxygen Tank", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give oxygentank");
		} TT("sv_cheats 1; give oxygentank");

		//
		if (ImGui::Button("Give First Aid Kit", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give first_aid_kit");
		} TT("sv_cheats 1; give first_aid_kit");

		ImGui::SameLine();
		if (ImGui::Button("Give Defibrilator", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give defibrilator");
		} TT("sv_cheats 1; give defibrilator");

		ImGui::SameLine();
		if (ImGui::Button("Give Adrenaline", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give adrenaline");
		} TT("sv_cheats 1; give adrenaline");

		ImGui::SameLine();
		if (ImGui::Button("Give Pain Pills", four_row_button_size)) {
			interfaces::get()->m_engine->execute_client_cmd_unrestricted("sv_cheats 1; give pain_pills");
		} TT("sv_cheats 1; give pain_pills");
	}

	void cont_general_maps()
	{
		const auto five_row_button_size = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4.0f;

		ImGui::SeparatorTextLarge(" Dead Center C1 ");

		if (ImGui::Button("c1m1_hotel", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c1m1_hotel"); }

		ImGui::SameLine();
		if (ImGui::Button("c1m2_streets", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c1m2_streets"); }

		ImGui::SameLine();
		if (ImGui::Button("c1m3_mall", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c1m3_mall"); }

		ImGui::SameLine();
		if (ImGui::Button("c1m4_atrium", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c1m4_atrium"); }

		// --

		ImGui::Spacing(0, 4);
		ImGui::SeparatorTextLarge(" Dark Carnival C2 ");

		if (ImGui::Button("c2m1_highway", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c2m1_highway"); }

		ImGui::SameLine();
		if (ImGui::Button("c2m2_fairgrounds", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c2m2_fairgrounds"); }

		ImGui::SameLine();
		if (ImGui::Button("c2m3_coaster", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c2m3_coaster"); }

		ImGui::SameLine();
		if (ImGui::Button("c2m4_barns", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c2m4_barns"); }

		ImGui::SameLine();
		if (ImGui::Button("c2m5_concert", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c2m5_concert"); }

		// --

		ImGui::Spacing(0, 4);
		ImGui::SeparatorTextLarge(" Swamp Fever C3 ");

		if (ImGui::Button("c3m1_plankcountry", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c3m1_plankcountry"); }

		ImGui::SameLine();
		if (ImGui::Button("c3m2_swamp", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c3m2_swamp"); }

		ImGui::SameLine();
		if (ImGui::Button("c3m3_shantytown", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c3m3_shantytown"); }

		ImGui::SameLine();
		if (ImGui::Button("c3m4_plantation", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c3m4_plantation"); }

		// --

		ImGui::Spacing(0, 4);
		ImGui::SeparatorTextLarge(" Hard Rain C4 ");

		if (ImGui::Button("c4m1_milltown_a", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c4m1_milltown_a"); }

		ImGui::SameLine();
		if (ImGui::Button("c4m2_sugarmill_a", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c4m2_sugarmill_a"); }

		ImGui::SameLine();
		if (ImGui::Button("c4m3_sugarmill_b", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c4m3_sugarmill_b"); }

		ImGui::SameLine();
		if (ImGui::Button("c4m4_milltown_b", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c4m4_milltown_b"); }

		ImGui::SameLine();
		if (ImGui::Button("c4m5_milltown_escape", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c4m5_milltown_escape"); }

		// --

		ImGui::Spacing(0, 4);
		ImGui::SeparatorTextLarge(" The Parish C5 ");

		if (ImGui::Button("c5m1_waterfront", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c5m1_waterfront"); }

		ImGui::SameLine();
		if (ImGui::Button("c5m2_park", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c5m2_park"); }

		ImGui::SameLine();
		if (ImGui::Button("c5m3_cemetery", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c5m3_cemetery"); }

		ImGui::SameLine();
		if (ImGui::Button("c5m4_quarter", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c5m4_quarter"); }

		ImGui::SameLine();
		if (ImGui::Button("c5m5_bridge", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c5m5_bridge"); }

		// --

		ImGui::Spacing(0, 4);
		ImGui::SeparatorTextLarge(" The Passing C6 ");

		if (ImGui::Button("c6m1_riverbank", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c6m1_riverbank"); }

		ImGui::SameLine();
		if (ImGui::Button("c6m2_bedlam", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c6m2_bedlam"); }

		ImGui::SameLine();
		if (ImGui::Button("c6m3_port", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c6m3_port"); }

		// --

		ImGui::Spacing(0, 4);
		ImGui::SeparatorTextLarge(" The Scarifice C7 ");

		if (ImGui::Button("c7m1_docks", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c7m1_docks"); }

		ImGui::SameLine();
		if (ImGui::Button("c7m2_barge", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c7m2_barge"); }

		ImGui::SameLine();
		if (ImGui::Button("c7m3_port", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c7m3_port"); }

		// --

		ImGui::Spacing(0, 4);
		ImGui::SeparatorTextLarge(" NO Mercy C8 ");

		if (ImGui::Button("c8m1_apartment", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c8m1_apartment"); }

		ImGui::SameLine();
		if (ImGui::Button("c8m2_subway", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c8m2_subway"); }

		ImGui::SameLine();
		if (ImGui::Button("c8m3_sewers", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c8m3_sewers"); }

		ImGui::SameLine();
		if (ImGui::Button("c8m4_interior", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c8m4_interior"); }

		ImGui::SameLine();
		if (ImGui::Button("c8m5_rooftop", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c8m5_rooftop"); }

		// --

		ImGui::Spacing(0, 4);
		ImGui::SeparatorTextLarge(" Crash Course C9 ");

		if (ImGui::Button("c9m1_alleys", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c9m1_alleys"); }

		ImGui::SameLine();
		if (ImGui::Button("c9m2_lots", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c9m2_lots"); }

		// --

		ImGui::Spacing(0, 4);
		ImGui::SeparatorTextLarge(" Death Toll C10 ");

		if (ImGui::Button("c10m1_caves", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c10m1_caves"); }

		ImGui::SameLine();
		if (ImGui::Button("c10m2_drainage", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c10m2_drainage"); }

		ImGui::SameLine();
		if (ImGui::Button("c10m3_ranchhouse", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c10m3_ranchhouse"); }

		ImGui::SameLine();
		if (ImGui::Button("c10m4_mainstreet", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c10m4_mainstreet"); }

		ImGui::SameLine();
		if (ImGui::Button("c10m5_houseboat", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c10m5_houseboat"); }

		// --

		ImGui::Spacing(0, 4);
		ImGui::SeparatorTextLarge(" Dead Air C11 ");

		if (ImGui::Button("c11m1_greenhouse", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c11m1_greenhouse"); }

		ImGui::SameLine();
		if (ImGui::Button("c11m2_offices", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c11m2_offices"); }

		ImGui::SameLine();
		if (ImGui::Button("c11m3_garage", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c11m3_garage"); }

		ImGui::SameLine();
		if (ImGui::Button("c11m4_terminal", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c11m4_terminal"); }

		ImGui::SameLine();
		if (ImGui::Button("c11m5_runway", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c11m5_runway"); }

		// --

		ImGui::Spacing(0, 4);
		ImGui::SeparatorTextLarge(" Blood Harvest C12 ");

		if (ImGui::Button("c12m1_hilltop", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c12m1_hilltop"); }

		ImGui::SameLine();
		if (ImGui::Button("c12m2_traintunnel", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c12m2_traintunnel"); }

		ImGui::SameLine();
		if (ImGui::Button("c12m3_bridge", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c12m3_bridge"); }

		ImGui::SameLine();
		if (ImGui::Button("c12m4_barn", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c12m4_barn"); }

		ImGui::SameLine();
		if (ImGui::Button("c12m5_cornfield", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c12m5_cornfield"); }

		// --

		ImGui::Spacing(0, 4);
		ImGui::SeparatorTextLarge(" Cold Stream C13 ");

		if (ImGui::Button("c13m1_alpinecreek", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c13m1_alpinecreek"); }

		ImGui::SameLine();
		if (ImGui::Button("c13m2_southpinestream", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c13m2_southpinestream"); }

		ImGui::SameLine();
		if (ImGui::Button("c13m3_memorialbridge", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c13m3_memorialbridge"); }

		ImGui::SameLine();
		if (ImGui::Button("c13m4_cutthroatcreek", ImVec2(five_row_button_size, 0))) { interfaces::get()->m_engine->execute_client_cmd_unrestricted("map c13m4_cutthroatcreek"); }
	}



	void cont_general_import_lights_from_maps()
	{
		const float avail = ImGui::GetContentRegionAvail().x;
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const auto two_button_size = ImVec2((avail - spacing) / 2.0f, 0.0f);
		const auto four_button_size = ImVec2((avail - spacing * 3.0f) / 4.0f, 0.0f);

		ui_subsection("Map Lights", "Automatic Source map-light import with stable pages for status, persistence, inspection and advanced integration.", ICON_FA_LIGHTBULB);

		static int map_light_page = 0;
		if (ImGui::BeginTabBar("##map_light_import_pages", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll))
		{
			if (ImGui::BeginTabItem(ICON_FA_BOLT "  Overview")) { map_light_page = 0; ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem(ICON_FA_DATABASE "  Database")) { map_light_page = 1; ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem(ICON_FA_SEARCH "  Inspector")) { map_light_page = 2; ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem(ICON_FA_BOLT "  Runtime Effects")) { map_light_page = 3; ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Event Lights")) { map_light_page = 4; ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Facing Poly")) { map_light_page = 5; ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem(ICON_FA_COG "  Advanced")) { map_light_page = 6; ImGui::EndTabItem(); }
			ImGui::EndTabBar();
		}

		if (map_light_page == 0)
		{
		if (ImGui::BeginTable("##map_light_quick_status", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ui_metric_card("Map", dynamic_lighting::get_bsp_worldlight_pending_map().empty() ? "waiting" : dynamic_lighting::get_bsp_worldlight_pending_map());
			ImGui::TableNextColumn(); ui_metric_card("Detected", std::format("{}", dynamic_lighting::m_bsp_worldlight_candidates));
			ImGui::TableNextColumn(); ui_metric_card("Active", std::format("{}", dynamic_lighting::m_bsp_worldlight_imported));
			ImGui::TableNextColumn(); ui_metric_card("Backend", dynamic_lighting::m_bsp_worldlight_backend.empty() ? "waiting" : dynamic_lighting::m_bsp_worldlight_backend);
			ImGui::EndTable();
		}

		ImGui::SeparatorText("Automatic workflow");
		imgui::toggle_button_bool(&dynamic_lighting::m_bsp_worldlight_auto_import, "Auto Import", two_button_size,
			"Imports compiled WORLDLIGHTS automatically after map load and retries an early failed scan.");
		ImGui::SameLine();
		imgui::toggle_button_bool(&dynamic_lighting::m_map_light_runtime_tracking, "Live Source Lights", two_button_size,
			"Tracks Source dynamic lights, lightstyles, moving owners and authored on/off state.");

		if (imgui::toggle_button_bool(&dynamic_lighting::m_map_light_auto_sync_editor, "Sync to Light Studio", two_button_size,
			"Mirrors stable imported lights into Light Studio after capture and refreshes them after re-import.") &&
			dynamic_lighting::m_map_light_auto_sync_editor)
		{
			dynamic_lighting::sync_imported_map_lights_to_light_editor();
		}
		ImGui::SameLine();
		imgui::toggle_button_bool(&dynamic_lighting::m_source_distant_light_import, "Global Sun / Moon", two_button_size,
			"Builds one Source global environment light from emit_skylight, light_environment, light_directional, env_cascade_light, shadow_control or env_sun. Native Distant is preferred and D3D9 directional is the compatibility fallback.");

		if (ImGui::Button("Reload Map Lights", four_button_size)) dynamic_lighting::reload_current_map_lights();
		ImGui::SameLine();
		if (ImGui::Button("Retry Sun / Moon", four_button_size))
		{
			dynamic_lighting::m_source_distant_light_import = true;
			dynamic_lighting::reload_current_map_lights();
		}
		ImGui::SameLine();
		if (ImGui::Button("Sync Light Studio", four_button_size)) dynamic_lighting::sync_imported_map_lights_to_light_editor();
		ImGui::SameLine();
		if (ImGui::Button("Select Nearest", four_button_size)) dynamic_lighting::select_nearest_bsp_candidate_to_camera();

		ImGui::TextWrapped("%s", dynamic_lighting::m_bsp_worldlight_status.c_str());
		if (const auto* lights = remix_lights::get())
		{
			ImGui::TextDisabled("Distant runtime: %s", lights->source_distant_runtime_status().c_str());
			ImGui::TextDisabled("native %s code %d draw %llu | FF directional %s draw %llu | selected %s",
				lights->source_distant_light_active() ? "active" : "missing",
				lights->source_distant_last_error_code(),
				static_cast<unsigned long long>(lights->source_distant_draw_calls()),
				lights->source_directional_ff_active() ? "active" : "off",
				static_cast<unsigned long long>(lights->source_directional_ff_draw_calls()),
				dynamic_lighting::m_source_environment_selected_alias.c_str());
		}
		ImGui::TextDisabled("records %u | candidates %u | active %u | generation %u",
			dynamic_lighting::m_bsp_worldlight_records, dynamic_lighting::m_bsp_worldlight_candidates,
			dynamic_lighting::m_bsp_worldlight_imported, dynamic_lighting::get_bsp_worldlight_session_generation());

		}

		if (map_light_page == 1)
		{
			ui_subsection("Persistent Map-Light Database", "Per-map capture, restore and database maintenance.", ICON_FA_DATABASE);
			imgui::toggle_button_bool(&dynamic_lighting::m_persistent_map_light_database, "Per-map Database", two_button_size,
				"Uses l4d2-rtx/map_lights/<map>.toml as the persistent source for captured Source lights and every authored Light Rig on this map.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_persistent_map_light_auto_save, "Auto-save Rig Edits", two_button_size,
				"Automatically updates the map file after Light Rig additions, edits or deletions. Deleted captured lights are stored as tombstones so they do not respawn.");

			imgui::toggle_button_bool(&dynamic_lighting::m_map_light_auto_save_config, "Capture Source Updates", two_button_size,
				"Writes stable Source/BSP captures into the same per-map database. Transient muzzle and explosion lights remain runtime-only.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_map_light_overrides_enabled, "Apply Saved Source Edits", two_button_size,
				"Applies stored settings to live-linked Source entities and suppresses records marked as deleted.");

			SET_CHILD_WIDGET_WIDTH_MAN(130.0f);
			ImGui::DragFloat("Save delay", &dynamic_lighting::m_persistent_map_light_save_delay, 0.05f, 0.10f, 10.0f, "%.2f s");
			TT("Debounce interval after the last Light Rig edit before the map database is atomically replaced.");

			const bool can_save_full_database = imgui::get() && imgui::get()->m_light_edit_mode;
			ImGui::BeginDisabled(!can_save_full_database);
			if (ImGui::Button("Save All Light Rigs", two_button_size))
				dynamic_lighting::save_light_editor_map_lights_to_config();
			ImGui::EndDisabled();
			if (!can_save_full_database) TT("Enter Light Rigging edit mode first. Normal gameplay moves active static lights out of the editable map-settings vector.");
			ImGui::SameLine();
			if (ImGui::Button("Reload Map Light File", two_button_size))
				dynamic_lighting::reload_persistent_map_lights_from_file();

			ImGui::TextWrapped("%s", dynamic_lighting::m_persistent_map_light_status.c_str());
			ImGui::TextDisabled("loaded %u | materialized %u | live-linked %u | deleted %u | autosaves %u",
				dynamic_lighting::m_persistent_map_light_loaded, dynamic_lighting::m_persistent_map_light_materialized,
				dynamic_lighting::m_persistent_map_light_live_linked, dynamic_lighting::m_persistent_map_light_tombstones,
				dynamic_lighting::m_persistent_map_light_autosaves);

			ImGui::SeparatorText("Streaming limits");
			SET_CHILD_WIDGET_WIDTH_MAN(130.0f);
			ImGui::DragInt("Active light limit", &dynamic_lighting::m_bsp_worldlight_import_limit, 1.0f, 0, 1024);
			ImGui::SameLine();
			SET_CHILD_WIDGET_WIDTH_MAN(130.0f);
			ImGui::DragFloat("Import distance", &dynamic_lighting::m_bsp_worldlight_max_distance, 25.0f, 0.0f, 30000.0f, "%.0f");
		}

		ImGui::Spacing(0, 6);
		if (map_light_page == 2)
		{
			ui_subsection("Detected Lights and Inspector", "Select, edit and persist a detected Source light.", ICON_FA_SEARCH);
			const auto registry = dynamic_lighting::get_source_direct_light_registry();
			const auto compiled_count = static_cast<std::uint32_t>(std::count_if(registry.begin(), registry.end(), [](const auto& light) {
				return light.source_index >= 0;
			}));
			const auto stable_runtime_count = static_cast<std::uint32_t>(std::count_if(registry.begin(), registry.end(), [](const auto& light) {
				return light.source_index < 0 && !light.transient;
			}));
			const auto transient_count = static_cast<std::uint32_t>(std::count_if(registry.begin(), registry.end(), [](const auto& light) {
				return light.transient;
			}));
			const auto suppressed_count = static_cast<std::uint32_t>(std::count_if(registry.begin(), registry.end(), [](const auto& light) {
				return light.duplicate_suppressed;
			}));
			const auto environment_count = static_cast<std::uint32_t>(std::count_if(registry.begin(), registry.end(), [](const auto& light) {
				return light.kind == "bsp_distant" || light.kind == "bsp_skylight" || light.kind == "bsp_skyambient";
			}));

			ImGui::TextWrapped("One registry for compiled WORLDLIGHTS, live projected/entity lights and transient dlight/elight allocations. Stable sources stay live-linked; transient lights can be frozen as editable scene lights.");
			ImGui::TextDisabled("compiled %u (environment %u) | stable runtime %u | transient d/e %u | duplicate runtime suppressed %u | frozen snapshots %u",
				compiled_count, environment_count, stable_runtime_count, transient_count, suppressed_count,
				dynamic_lighting::m_source_direct_snapshot_lights);

			if (ImGui::Button("Sync Stable to Editor", four_button_size)) dynamic_lighting::sync_imported_map_lights_to_light_editor();
			ImGui::SameLine();
			if (ImGui::Button("Freeze Active d/e", four_button_size)) dynamic_lighting::snapshot_active_source_direct_lights_to_editor();
			ImGui::SameLine();
			if (ImGui::Button("Clear Frozen", four_button_size)) dynamic_lighting::clear_source_direct_light_snapshots();
			ImGui::SameLine();
			if (ImGui::Button("Copy Registry", four_button_size)) copy_text_to_clipboard(dynamic_lighting::get_source_direct_light_report());

			SET_CHILD_WIDGET_WIDTH_MAN(130.0f);
			ImGui::DragInt("Freeze limit", &dynamic_lighting::m_source_direct_snapshot_limit, 1.0f, 1, 128);
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_source_direct_deduplicate_runtime, "Deduplicate stable runtime", two_button_size,
				"Suppress a live projected/entity light when an equivalent compiled WORLDLIGHT is already active at the same position and direction.");
			SET_CHILD_WIDGET_WIDTH_MAN(130.0f);
			ImGui::DragFloat("Duplicate distance", &dynamic_lighting::m_source_direct_duplicate_distance, 0.5f, 0.0f, 256.0f, "%.1f");
			ImGui::SameLine();
			SET_CHILD_WIDGET_WIDTH_MAN(130.0f);
			ImGui::DragFloat("Direction dot", &dynamic_lighting::m_source_direct_duplicate_direction_dot, 0.01f, -1.0f, 1.0f, "%.2f");

			// V20.3 Source environment light diagnostics retained under the simplified Map Lights UI.
			ImGui::SeparatorText("Sun / Moon environment resolver");
			imgui::toggle_button_bool(&dynamic_lighting::m_source_distant_light_import, "Import one global light", two_button_size,
				"Resolves one map-wide Source environment light and submits it through native Remix Distant plus an optional D3D9 directional fallback.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_source_distant_prefer_hdr, "Prefer HDR set", two_button_size,
				"Prefers LUMP_WORLDLIGHTS_HDR and _lightHDR, while retaining an explicit LDR fallback.");
			imgui::toggle_button_bool(&dynamic_lighting::m_source_distant_compare_bsp_sets, "Compare HDR and LDR", two_button_size,
				"Reads both compiled WORLDLIGHT sets for the environment resolver even when local lights come from engine memory.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_source_distant_follow_vrad_first_environment, "Follow first environment", two_button_size,
				"Mirrors stock VRAD: the first light_environment in entity order is the canonical map sun/moon.");
			imgui::toggle_button_bool(&dynamic_lighting::m_source_distant_use_worldlight_direction, "Prefer compiled direction", two_button_size,
				"Uses the selected emit_skylight normal after entity matching and direction-convention validation.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_source_distant_allow_entity_fallback, "Entity fallback", two_button_size,
				"Uses light_environment target/pitch/angles and linearized _light values if compiled skylight data is unavailable.");
			imgui::toggle_button_bool(&dynamic_lighting::m_source_distant_match_worldlight_to_entity, "Match entity to skylight", two_button_size,
				"Scores compiled skylights by direction, HDR/LDR set, style and chromaticity against the canonical environment entity.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_source_distant_use_target_direction, "Honor target first", two_button_size,
				"Matches VRAD ParseLightGeneric: an authored target overrides pitch and angles when it resolves to a valid entity.");
			imgui::toggle_button_bool(&dynamic_lighting::m_source_distant_linearize_entity_color, "VRAD linear color", two_button_size,
				"Converts authored 0..255 RGB through the VRAD 2.2 gamma path and keeps entity fallback intensity in WORLDLIGHT magnitude.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_source_distant_spread_is_radius, "Spread is angular radius", two_button_size,
				"Converts Source SunSpreadAngle to a full Remix angular diameter by multiplying it by two.");

			imgui::toggle_button_bool(&dynamic_lighting::m_source_environment_use_env_cascade, "Use env_cascade_light", two_button_size,
				"Accepts a cascade-shadow directional entity as a runtime/global-light direction source when the baked environment entity is missing.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_source_environment_use_shadow_control, "Use shadow_control", two_button_size,
				"Uses Source's dynamic shadow direction as a fallback clue; it never replaces a valid compiled emit_skylight.");
			imgui::toggle_button_bool(&dynamic_lighting::m_source_environment_use_env_sun, "Use env_sun direction", two_button_size,
				"Uses the visual sun/moon orientation only as the last direction fallback. env_sun itself does not emit light.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_source_environment_force_direction_only, "Create from direction-only", two_button_size,
				"Creates a conservative neutral global light when only shadow_control or env_sun survived in the BSP entity data.");

			if (remix_lights::get())
			{
				imgui::toggle_button_bool(&remix_lights::source_directional_ff_enabled(), "D3D9 directional fallback", two_button_size,
					"Submits D3DLIGHT_DIRECTIONAL every frame through the ASI when native Remix Distant is rejected or ignored by the paired bridge.");
				ImGui::SameLine();
				SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragInt("FF index", &remix_lights::source_directional_ff_index(), 1.0f, 0, 7);
				ImGui::SameLine();
				SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("FF scalar", &remix_lights::source_directional_ff_scalar(), 0.01f, 0.0f, 4.0f, "%.2f");
			}

			SET_CHILD_WIDGET_WIDTH_MAN(210.0f);
			ImGui::Combo("Direction convention", &dynamic_lighting::m_source_distant_direction_mode,
				"Auto: align VRAD to entity\0Trust Source/VRAD normal\0Reverse normal\0");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Auto keeps Source's propagation direction and only reverses a compiled normal when the canonical entity strongly confirms the opposite sign.");

			if (ImGui::BeginTable("##source_distant_parameters_v203", 7, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Intensity scale", &dynamic_lighting::m_source_distant_intensity_scale, 1.0f, 0.0f, 2000.0f, "%.1f");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Default diameter", &dynamic_lighting::m_source_distant_default_angular_diameter, 0.01f, 0.01f, 180.0f, "%.2f deg");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Minimum diameter", &dynamic_lighting::m_source_distant_min_angular_diameter, 0.01f, 0.001f, 180.0f, "%.3f deg");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Maximum diameter", &dynamic_lighting::m_source_distant_max_angular_diameter, 0.5f, 0.01f, 180.0f, "%.1f deg");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Max pair angle", &dynamic_lighting::m_source_distant_max_pair_angle, 1.0f, 1.0f, 180.0f, "%.0f deg");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Sign improvement", &dynamic_lighting::m_source_distant_sign_flip_min_improvement, 0.5f, 0.0f, 90.0f, "%.1f deg");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Min confidence", &dynamic_lighting::m_source_distant_min_confidence, 0.01f, 0.0f, 1.0f, "%.2f");
				ImGui::EndTable();
			}
			SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
			ImGui::DragFloat("Direction-only strength", &dynamic_lighting::m_source_environment_default_strength, 0.01f, 0.001f, 100.0f, "%.3f");
			if (ImGui::Button("Retry detected Distant", two_button_size))
			{
				dynamic_lighting::m_source_distant_light_import = true;
				dynamic_lighting::reload_current_map_lights();
			}
			ImGui::SameLine();
			if (ImGui::Button("Native Distant API test", two_button_size))
			{
				if (auto* lights = remix_lights::get())
				{
					lights->upsert_source_distant_light(
						utils::string_hash64("source-distant-api-test"),
						Vector(0.35f, -0.25f, -0.90f), Vector(55.0f, 48.0f, 38.0f), 0.53f, 1.0f);
				}
			}
			ImGui::TextWrapped("Distant: %s", dynamic_lighting::m_source_distant_status.c_str());
			if (const auto* lights = remix_lights::get())
			{
				ImGui::TextWrapped("Native submission: %s", lights->source_distant_runtime_status().c_str());
				ImGui::TextDisabled("native handle %s | CreateLight %d | ABI sType 7 | DrawLightInstance %llu",
					lights->source_distant_light_active() ? "active" : "missing", lights->source_distant_last_error_code(),
					static_cast<unsigned long long>(lights->source_distant_draw_calls()));
				ImGui::TextDisabled("D3D9 directional %s | SetLight/LightEnable frames %llu | index %d",
					lights->source_directional_ff_active() ? "active" : "off",
					static_cast<unsigned long long>(lights->source_directional_ff_draw_calls()),
					remix_lights::source_directional_ff_index());
			}
			ImGui::TextDisabled("aliases light_environment/light_directional/cascade/shadow_control/env_sun %u/%u/%u/%u/%u | selected %s",
				dynamic_lighting::m_source_environment_light_environment_found,
				dynamic_lighting::m_source_environment_light_directional_found,
				dynamic_lighting::m_source_environment_cascade_found,
				dynamic_lighting::m_source_environment_shadow_control_found,
				dynamic_lighting::m_source_environment_env_sun_found,
				dynamic_lighting::m_source_environment_selected_alias.c_str());
			ImGui::TextDisabled("entities %u | skylight %u | skyambient %u | HDR/LDR sets %u/%u | rejected %u | confidence %.2f | disagreement %.1f deg | selected %s%s | active %s",
				dynamic_lighting::m_source_distant_entities_found, dynamic_lighting::m_source_distant_worldlights_found,
				dynamic_lighting::m_source_distant_skyambient_found,
				dynamic_lighting::m_source_distant_hdr_sets_found, dynamic_lighting::m_source_distant_ldr_sets_found,
				dynamic_lighting::m_source_distant_candidates_rejected,
				dynamic_lighting::m_source_distant_selected_confidence,
				dynamic_lighting::m_source_distant_direction_disagreement,
				dynamic_lighting::m_source_distant_selected_hdr ? "HDR" : "LDR",
				dynamic_lighting::m_source_distant_sign_flipped ? "/reversed" : "",
				dynamic_lighting::m_source_distant_imported ? "yes" : "no");
			if (!dynamic_lighting::m_source_distant_diagnostics.empty())
				ImGui::TextWrapped("Resolver: %s", dynamic_lighting::m_source_distant_diagnostics.c_str());

			ImGui::SeparatorText("V20.3 local-light inference (V20.2 resolver retained)");
			imgui::toggle_button_bool(&dynamic_lighting::m_source_light_axis_resolver, "Axis resolver", two_button_size,
				"Scores Source forward/right/up axes, target endpoints, temporal history and nearby compiled WORLDLIGHTS instead of trusting one field.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_source_light_basis_recovery, "90-degree basis recovery", two_button_size,
				"Allows local right/up/down to win only when a target, WORLDLIGHT or stable temporal direction strongly validates the correction.");
			imgui::toggle_button_bool(&dynamic_lighting::m_source_light_temporal_axis_prior, "Temporal axis prior", two_button_size,
				"Prevents a light from alternating between perpendicular axes when Source network properties are stale or update at different times.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_source_light_compiled_axis_prior, "WORLDLIGHT axis prior", two_button_size,
				"Uses the nearest compatible compiled VRAD spotlight as a high-confidence direction validator for stable runtime lights.");
			imgui::toggle_button_bool(&dynamic_lighting::m_source_light_shape_hysteresis, "Shape hysteresis", two_button_size,
				"Requires repeated evidence before a dynamic Source light changes between sphere and spotlight.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_source_light_parameter_sanity, "Parameter sanity", two_button_size,
				"Sanitizes invalid FOV/range/brightness values and can derive point_spotlight FOV from beam width and length.");
			if (ImGui::BeginTable("##source_light_inference_thresholds", 5, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Switch margin", &dynamic_lighting::m_source_light_axis_switch_margin, 0.01f, 0.0f, 0.75f, "%.2f");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Max axis jump", &dynamic_lighting::m_source_light_max_axis_jump_degrees, 1.0f, 5.0f, 180.0f, "%.0f deg");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(90.0f); ImGui::DragFloat("Compiled prior range", &dynamic_lighting::m_source_light_compiled_prior_distance, 1.0f, 0.0f, 512.0f, "%.0f");
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(70.0f); ImGui::DragInt("Spot confirm", &dynamic_lighting::m_source_light_spot_confirm_frames, 1.0f, 1, 30);
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(70.0f); ImGui::DragInt("Sphere confirm", &dynamic_lighting::m_source_light_sphere_confirm_frames, 1.0f, 1, 30);
				ImGui::EndTable();
			}
			ImGui::TextDisabled("axis corrections %u | basis recoveries %u | axis switches %u | shape hysteresis holds %u",
				dynamic_lighting::m_source_light_axis_corrections, dynamic_lighting::m_source_light_basis_recoveries,
				dynamic_lighting::m_source_light_axis_switches, dynamic_lighting::m_source_light_shape_holds);

			ImGui::Checkbox("Read light_environment / skyambient records", &dynamic_lighting::m_bsp_worldlight_include_environment_records);
			ImGui::SameLine();
			ImGui::Checkbox("Experimental local sky helpers", &dynamic_lighting::m_bsp_worldlight_enable_environment_helpers);
			if (dynamic_lighting::m_bsp_worldlight_enable_environment_helpers)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f),
					"Sky records are directional/environment data. Local helper lights are diagnostic only and are disabled by default.");
			}
			ImGui::TextWrapped("Registry: %s", dynamic_lighting::m_source_direct_status.c_str());

			static bool show_compiled = true;
			static bool show_stable_runtime = true;
			static bool show_transient = true;
			static bool show_disabled = true;
			static char registry_filter[96] = {};
			ImGui::Checkbox("Compiled", &show_compiled);
			ImGui::SameLine();
			ImGui::Checkbox("Stable runtime", &show_stable_runtime);
			ImGui::SameLine();
			ImGui::Checkbox("Transient", &show_transient);
			ImGui::SameLine();
			ImGui::Checkbox("Disabled", &show_disabled);
			SET_CHILD_WIDGET_WIDTH_MAN(260.0f);
			ImGui::InputTextWithHint("##SourceDirectRegistryFilter", "filter kind/class/comment", registry_filter, IM_ARRAYSIZE(registry_filter));

			if (ImGui::BeginTable("SourceDirectLightRegistryTable", 10,
				ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
				ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 245.0f)))
			{
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 115.0f);
				ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 72.0f);
				ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 72.0f);
				ImGui::TableSetupColumn("Class / style", ImGuiTableColumnFlags_WidthStretch, 150.0f);
				ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed, 155.0f);
				ImGui::TableSetupColumn("Power", ImGuiTableColumnFlags_WidthFixed, 72.0f);
				ImGui::TableSetupColumn("Radius / FOV", ImGuiTableColumnFlags_WidthFixed, 105.0f);
				ImGui::TableSetupColumn("Detection", ImGuiTableColumnFlags_WidthStretch, 165.0f);
				ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthStretch, 120.0f);
				ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 72.0f);
				ImGui::TableHeadersRow();

				const std::string filter = utils::str_to_lower(std::string(registry_filter));
				int source_direct_row_id = 0;
				for (const auto& light : registry)
				{
					const bool compiled = light.source_index >= 0;
					const bool stable_runtime = !compiled && !light.transient;
					if ((compiled && !show_compiled) || (stable_runtime && !show_stable_runtime) ||
						(light.transient && !show_transient) || (!light.enabled && !show_disabled)) continue;
					if (!filter.empty())
					{
						const auto haystack = utils::str_to_lower(light.kind + " " + light.classname + " " + light.comment +
							" " + light.direction_source + " " + light.shape_reason + " " + light.owner_category +
							" " + light.owner_classname + " " + light.owner_model + " " + light.policy_reason + " " + light.surface_material_name);
						if (haystack.find(filter) == std::string::npos) continue;
					}

					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(light.kind.c_str());
					ImGui::TableNextColumn();
					const bool environment_record = light.kind == "bsp_distant" || light.kind == "bsp_skylight" || light.kind == "bsp_skyambient";
					ImGui::TextUnformatted(!light.policy_allowed ? "filtered" : (light.duplicate_suppressed ? "dedup" :
						(environment_record && !light.enabled ? "registry" : (light.enabled ? "on" : "off"))));
					ImGui::TableNextColumn();
					if (light.source_index >= 0) ImGui::Text("S%d", light.source_index);
					else if (light.runtime_entity_index >= 0) ImGui::Text("E%d", light.runtime_entity_index);
					else ImGui::Text("K%d", light.source_key);
					ImGui::TableNextColumn();
					ImGui::Text("%s", light.classname.empty() ? "-" : light.classname.c_str());
					if (light.style != 0)
					{
						ImGui::SameLine();
						ImGui::TextDisabled("style %d x%.2f", light.style, light.style_value);
					}
					ImGui::TableNextColumn();
					ImGui::Text("%.1f %.1f %.1f", light.position.x, light.position.y, light.position.z);
					ImGui::TableNextColumn();
					ImGui::Text("%.1f", light.intensity);
					ImGui::TableNextColumn();
					if (light.kind == "bsp_distant") ImGui::Text("sun / %.2f deg", light.outer_angle);
					else if (light.shaped) ImGui::Text("%.2f / %.0f", light.radius, light.outer_angle);
					else ImGui::Text("%.2f", light.radius);
					ImGui::TableNextColumn();
					ImGui::Text("dir %.0f%% | shape %.0f%%", light.direction_confidence * 100.0f, light.shape_confidence * 100.0f);
					if (light.direction_corrected)
					{
						ImGui::SameLine();
						ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f), light.basis_recovered ? "basis fix" : "corrected");
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("Direction: %s (%.2f)\nAxis disagreement: %.1f deg\nStable frames: %u | switches: %u\nCandidates: %s\nShape: %s (%.2f, evidence %.2f)\nParameters: %s\nVector: %.3f %.3f %.3f",
							light.direction_source.c_str(), light.direction_confidence, light.axis_disagreement_degrees,
							light.axis_stable_frames, light.axis_switches, light.axis_candidates.c_str(),
							light.shape_reason.c_str(), light.shape_confidence, light.shape_evidence,
							light.parameter_sources.c_str(), light.direction.x, light.direction.y, light.direction.z);
					}
					ImGui::TableNextColumn();
					if (light.matched_compiled_source >= 0) ImGui::Text("WORLDLIGHT %d", light.matched_compiled_source);
					else if (!light.surface_material_name.empty()) ImGui::Text("material %s", light.surface_material_name.c_str());
					else if (light.owner >= 0) ImGui::Text("%s #%d", light.owner_category.empty() ? "owner" : light.owner_category.c_str(), light.owner);
					else ImGui::TextUnformatted(light.transient ? "runtime snapshot" : (light.shadows ? "shadows" : "unbound"));
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Policy: %s\nReason: %s\nOwner class: %s\nOwner model: %s\nEvent controlled: %s\n%s",
						light.policy_allowed ? "allowed" : "filtered", light.policy_reason.c_str(),
						light.owner_classname.c_str(), light.owner_model.c_str(), light.event_controlled ? "yes" : "no", light.comment.c_str());
					ImGui::TableNextColumn();
					ImGui::PushID(source_direct_row_id++);
					if (compiled)
					{
						if (ImGui::SmallButton("Inspect"))
						{
							const auto candidate = std::find_if(dynamic_lighting::m_source_bsp_candidates.begin(),
								dynamic_lighting::m_source_bsp_candidates.end(), [&](const auto& entry)
								{ return static_cast<std::int32_t>(entry.source_index) == light.source_index; });
							if (candidate != dynamic_lighting::m_source_bsp_candidates.end())
							{
								dynamic_lighting::m_source_bsp_selected_index = static_cast<int>(
									std::distance(dynamic_lighting::m_source_bsp_candidates.begin(), candidate));
								dynamic_lighting::preview_bsp_candidate(dynamic_lighting::m_source_bsp_selected_index);
							}
						}
					}
					else if (light.transient)
					{
						if (ImGui::SmallButton("Freeze"))
							dynamic_lighting::snapshot_source_direct_light_to_editor(light.capture_id);
					}
					else if (ImGui::SmallButton("Sync"))
					{
						dynamic_lighting::sync_imported_map_lights_to_light_editor();
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}

		ImGui::Spacing(0, 6);
		ImGui::SeparatorText("Imported light editor");
		if (dynamic_lighting::m_source_bsp_candidates.empty())
		{
			ImGui::TextDisabled("No imported lights are available. Load a map or press Reload.");
		}
		else
		{
			dynamic_lighting::m_source_bsp_selected_index = std::clamp(dynamic_lighting::m_source_bsp_selected_index, 0,
				static_cast<int>(dynamic_lighting::m_source_bsp_candidates.size()) - 1);
			SET_CHILD_WIDGET_WIDTH_MAN(115.0f);
			ImGui::DragInt("Light", &dynamic_lighting::m_source_bsp_selected_index, 1.0f, 0,
				static_cast<int>(dynamic_lighting::m_source_bsp_candidates.size()) - 1);
			ImGui::SameLine();
			if (ImGui::Button("Previous")) dynamic_lighting::m_source_bsp_selected_index = std::max(0, dynamic_lighting::m_source_bsp_selected_index - 1);
			ImGui::SameLine();
			if (ImGui::Button("Next")) dynamic_lighting::m_source_bsp_selected_index = std::min(
				static_cast<int>(dynamic_lighting::m_source_bsp_candidates.size()) - 1,
				dynamic_lighting::m_source_bsp_selected_index + 1);
			ImGui::SameLine();
			ImGui::TextDisabled("%d / %zu", dynamic_lighting::m_source_bsp_selected_index + 1,
				dynamic_lighting::m_source_bsp_candidates.size());

			auto& selected = dynamic_lighting::m_source_bsp_candidates[
				static_cast<std::size_t>(dynamic_lighting::m_source_bsp_selected_index)];
			const bool editor_dirty = selected.editor_baseline_valid && (
				selected.selected != selected.editor_selected ||
				selected.origin.DistToSqr(selected.editor_origin) > 0.000001f ||
				selected.radiance.DistToSqr(selected.editor_radiance) > 0.000001f ||
				std::abs(selected.scalar - selected.editor_scalar) > 0.0001f ||
				std::abs(selected.radius - selected.editor_radius) > 0.0001f ||
				selected.shaped != selected.editor_shaped ||
				selected.direction.DistToSqr(selected.editor_direction) > 0.000001f ||
				std::abs(selected.degrees - selected.editor_degrees) > 0.0001f ||
				std::abs(selected.softness - selected.editor_softness) > 0.0001f ||
				std::abs(selected.exponent - selected.editor_exponent) > 0.0001f ||
				std::abs(selected.distant_angular_diameter - selected.editor_distant_angular_diameter) > 0.0001f ||
				selected.surface_cluster != selected.editor_surface_cluster ||
				selected.surface_cluster_samples != selected.editor_surface_cluster_samples ||
				std::abs(selected.surface_cluster_spread - selected.editor_surface_cluster_spread) > 0.0001f ||
				std::abs(selected.surface_cluster_intensity - selected.editor_surface_cluster_intensity) > 0.0001f);
			ImGui::Checkbox("Enabled", &selected.selected);
			ImGui::SameLine();
			ImGui::TextDisabled("source #%u | %s | style %d%s%s", selected.source_index,
				selected.classname.c_str(), selected.style, selected.override_applied ? " | saved override" : "",
				editor_dirty ? " | UNSAVED" : "");

			ImGui::DragFloat3("Position", &selected.origin.x, 0.25f);
			ImGui::ColorEdit3("Color", &selected.radiance.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
			SET_CHILD_WIDGET_WIDTH_MAN(150.0f);
			ImGui::DragFloat("Intensity", &selected.scalar, 25.0f, 0.0f, 250000.0f, "%.1f");
			ImGui::SameLine();
			SET_CHILD_WIDGET_WIDTH_MAN(150.0f);
			ImGui::DragFloat("Radius", &selected.radius, 0.02f, 0.01f, 128.0f, "%.2f");

			if (selected.distant)
			{
				ImGui::TextDisabled("Native Distant light: position and radius are ignored by the Remix analytical backend.");
				ImGui::DragFloat3("Sun direction", &selected.direction.x, 0.01f, -1.0f, 1.0f);
				SET_CHILD_WIDGET_WIDTH_MAN(150.0f);
				ImGui::DragFloat("Angular diameter", &selected.distant_angular_diameter, 0.05f, 0.01f, 180.0f, "%.2f deg");
				ImGui::TextWrapped("Detection: %s", selected.distant_source.empty() ? "Source environment resolver" : selected.distant_source.c_str());
			}
			else
			{
				ImGui::Checkbox("Directional / cone light", &selected.shaped);
				if (selected.shaped)
				{
					ImGui::DragFloat3("Direction", &selected.direction.x, 0.01f, -1.0f, 1.0f);
					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					ImGui::DragFloat("Cone angle", &selected.degrees, 0.5f, 1.0f, 180.0f, "%.1f");
					ImGui::SameLine();
					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					ImGui::DragFloat("Softness", &selected.softness, 0.01f, 0.0f, 1.0f, "%.2f");
					ImGui::SameLine();
					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					ImGui::DragFloat("Exponent", &selected.exponent, 0.1f, 0.0f, 64.0f, "%.1f");

					const bool selected_is_spot = selected.source_type == static_cast<std::int32_t>(source_bsp_lights::emit_type::spotlight) ||
						utils::str_to_lower(selected.classname).find("spot") != std::string::npos ||
						utils::str_to_lower(selected.map_light_classname).find("projectedtexture") != std::string::npos;
					if (selected_is_spot)
					{
						static bool rotate_all_imported_spots = true;
						ImGui::TextDisabled("90-degree calibration (use only if this map still has legacy/atypical spotlight axes)");
						const float calibration_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 5.0f) / 6.0f;
						if (ImGui::Button("X +90", ImVec2(calibration_width, 0.0f))) dynamic_lighting::rotate_selected_map_light_direction(0, 90.0f, rotate_all_imported_spots);
						ImGui::SameLine();
						if (ImGui::Button("X -90", ImVec2(calibration_width, 0.0f))) dynamic_lighting::rotate_selected_map_light_direction(0, -90.0f, rotate_all_imported_spots);
						ImGui::SameLine();
						if (ImGui::Button("Y +90", ImVec2(calibration_width, 0.0f))) dynamic_lighting::rotate_selected_map_light_direction(1, 90.0f, rotate_all_imported_spots);
						ImGui::SameLine();
						if (ImGui::Button("Y -90", ImVec2(calibration_width, 0.0f))) dynamic_lighting::rotate_selected_map_light_direction(1, -90.0f, rotate_all_imported_spots);
						ImGui::SameLine();
						if (ImGui::Button("Z +90", ImVec2(calibration_width, 0.0f))) dynamic_lighting::rotate_selected_map_light_direction(2, 90.0f, rotate_all_imported_spots);
						ImGui::SameLine();
						if (ImGui::Button("Z -90", ImVec2(calibration_width, 0.0f))) dynamic_lighting::rotate_selected_map_light_direction(2, -90.0f, rotate_all_imported_spots);
						ImGui::Checkbox("Apply calibration to all imported spotlights", &rotate_all_imported_spots);
					}
				}
			}

			if (selected.source_type == static_cast<std::int32_t>(source_bsp_lights::emit_type::surface))
			{
				ImGui::Checkbox("Surface helper cluster", &selected.surface_cluster);
				if (selected.surface_cluster)
				{
					SET_CHILD_WIDGET_WIDTH_MAN(110.0f);
					ImGui::DragInt("Samples", &selected.surface_cluster_samples, 1.0f, 1, 16);
					ImGui::SameLine();
					SET_CHILD_WIDGET_WIDTH_MAN(110.0f);
					ImGui::DragFloat("Spread", &selected.surface_cluster_spread, 0.01f, 0.0f, 4.0f, "%.2f");
					ImGui::SameLine();
					SET_CHILD_WIDGET_WIDTH_MAN(110.0f);
					ImGui::DragFloat("Helper power", &selected.surface_cluster_intensity, 0.01f, 0.0f, 4.0f, "%.2f");
				}
			}

			if (ImGui::Button("Apply Live", four_button_size)) dynamic_lighting::apply_selected_map_light_edits();
			ImGui::SameLine();
			if (ImGui::Button("Save Selected", four_button_size)) dynamic_lighting::save_selected_map_light_edits();
			ImGui::SameLine();
			if (ImGui::Button("Revert Unsaved", four_button_size)) dynamic_lighting::reset_selected_map_light_edits();
			ImGui::SameLine();
			if (ImGui::Button("Delete Saved Edit", four_button_size)) dynamic_lighting::remove_selected_map_light_override();

			ImGui::TextWrapped("Editor: %s", dynamic_lighting::m_map_light_override_status.c_str());
			if (!dynamic_lighting::m_map_light_override_path.empty())
				ImGui::TextDisabled("File: %s", dynamic_lighting::m_map_light_override_path.c_str());
		}

		ImGui::Spacing(0, 6);
		if (map_light_page == 3)
		{
			ui_subsection("Runtime Source Light Policy", "Control which live Source effects become Remix lights. The default profile keeps authored map effects and filters character helper lights.", ICON_FA_BOLT);

			const char* runtime_profiles[] = { "Raw Source", "Balanced Map Lighting", "World / Events Only", "Diagnostics" };
			int runtime_profile = std::clamp(dynamic_lighting::m_source_runtime_profile, 0, 3);
			SET_CHILD_WIDGET_WIDTH_MAN(260.0f);
			if (ImGui::Combo("Runtime profile", &runtime_profile, runtime_profiles, IM_ARRAYSIZE(runtime_profiles)))
			{
				dynamic_lighting::apply_source_runtime_light_profile(runtime_profile);
			}
			ImGui::TextWrapped("%s", dynamic_lighting::m_source_runtime_last_policy.c_str());

			bool runtime_policy_changed = false;
			runtime_policy_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_source_runtime_allow_world, "World / Map Effects", two_button_size,
				"Allows live lights that are not attached to survivors or infected.");
			ImGui::SameLine();
			runtime_policy_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_source_runtime_allow_survivors, "Survivor Helper Lights", two_button_size,
				"Allows yellow point lights and other effects owned by survivors. Disabled by default.");
			runtime_policy_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_source_runtime_allow_infected, "Infected Helper Lights", two_button_size,
				"Allows spotlights and other effects attached to special infected. Disabled by default.");
			ImGui::SameLine();
			runtime_policy_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_source_runtime_allow_unknown_characters, "Unknown Character Effects", two_button_size,
				"Allows character-like owners that could not be classified safely.");

			ImGui::SeparatorText("Source effect classes");
			runtime_policy_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_source_runtime_allow_dlights, "Dlights", four_button_size, "Source dynamic point/cone effects.");
			ImGui::SameLine();
			runtime_policy_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_source_runtime_allow_elights, "Elights", four_button_size, "Entity-attached engine effects.");
			ImGui::SameLine();
			runtime_policy_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_source_runtime_allow_entity_dynamic, "Light Dynamic", four_button_size, "Networked light_dynamic entities.");
			ImGui::SameLine();
			runtime_policy_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_source_runtime_allow_projected, "Projected", four_button_size, "env_projectedtexture entities.");
			runtime_policy_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_source_runtime_allow_point_spotlight, "Point Spotlight", two_button_size, "point_spotlight entities and their endpoints.");
			ImGui::SameLine();
			runtime_policy_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_source_runtime_show_rejected_in_registry, "Show Filtered Records", two_button_size, "Keeps rejected effects visible in diagnostics without submitting them to Remix.");

			ImGui::SeparatorText("World and character limits");
			SET_CHILD_WIDGET_WIDTH_MAN(145.0f);
			runtime_policy_changed |= ImGui::DragFloat("World intensity", &dynamic_lighting::m_source_runtime_world_intensity_scale, 0.01f, 0.0f, 8.0f, "%.2fx");
			ImGui::SameLine(); SET_CHILD_WIDGET_WIDTH_MAN(145.0f);
			runtime_policy_changed |= ImGui::DragFloat("World radius", &dynamic_lighting::m_source_runtime_world_radius_scale, 0.01f, 0.0f, 8.0f, "%.2fx");
			ImGui::SameLine(); SET_CHILD_WIDGET_WIDTH_MAN(145.0f);
			runtime_policy_changed |= ImGui::DragFloat("World max radius", &dynamic_lighting::m_source_runtime_max_radius, 0.05f, 0.10f, 64.0f, "%.2f");
			SET_CHILD_WIDGET_WIDTH_MAN(145.0f);
			runtime_policy_changed |= ImGui::DragFloat("Character intensity", &dynamic_lighting::m_source_runtime_character_intensity_scale, 0.01f, 0.0f, 8.0f, "%.2fx");
			ImGui::SameLine(); SET_CHILD_WIDGET_WIDTH_MAN(145.0f);
			runtime_policy_changed |= ImGui::DragFloat("Character radius", &dynamic_lighting::m_source_runtime_character_radius_scale, 0.01f, 0.0f, 8.0f, "%.2fx");
			ImGui::SameLine(); SET_CHILD_WIDGET_WIDTH_MAN(145.0f);
			runtime_policy_changed |= ImGui::DragFloat("Character max radius", &dynamic_lighting::m_source_runtime_character_max_radius, 0.05f, 0.10f, 32.0f, "%.2f");
			SET_CHILD_WIDGET_WIDTH_MAN(180.0f);
			runtime_policy_changed |= ImGui::DragFloat("Character spot max angle", &dynamic_lighting::m_source_runtime_character_spot_max_angle, 0.25f, 1.0f, 179.0f, "%.1f deg");
			if (runtime_policy_changed)
			{
				game_settings::mark_dirty("Source runtime light policy");
			}

			ImGui::TextDisabled("accepted world/character %u/%u | blocked survivor/infected/unknown/class %u/%u/%u/%u",
				dynamic_lighting::m_source_runtime_world_accepted, dynamic_lighting::m_source_runtime_character_accepted,
				dynamic_lighting::m_source_runtime_rejected_survivor, dynamic_lighting::m_source_runtime_rejected_infected,
				dynamic_lighting::m_source_runtime_rejected_unknown_character, dynamic_lighting::m_source_runtime_rejected_class);

			ImGui::SeparatorText("Runtime capture and compiled import");
			imgui::toggle_button_bool(&dynamic_lighting::m_source_dlight_import, "Dynamic Lights", two_button_size,
				"Mirror active Source dlights and elights.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_source_projectedtexture_import, "Projected Lights", two_button_size,
				"Track light_dynamic, env_projectedtexture and point_spotlight entities.");

			imgui::toggle_button_bool(&dynamic_lighting::m_bsp_worldlight_bind_owners, "Follow Moving Owners", two_button_size,
				"Keep compiled lights attached to moving props and brush entities.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_bsp_worldlight_destroy_with_owner, "Remove With Owner", two_button_size,
				"Disable owner-bound lights when their owner is destroyed or removed.");

			imgui::toggle_button_bool(&dynamic_lighting::m_bsp_worldlight_stream_nearby, "Stream Nearby Lights", two_button_size,
				"Refresh the nearest imported set as the camera moves.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_map_light_overrides_enabled, "Load Saved Edits", two_button_size,
				"Apply the per-map light-edit file during import.");

			ImGui::TextDisabled("Compiled light types");
			imgui::toggle_button_bool(&dynamic_lighting::m_bsp_worldlight_include_point, "Point", four_button_size, "Import emit_point.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_bsp_worldlight_include_spot, "Spot", four_button_size, "Import emit_spotlight.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_bsp_worldlight_include_surface, "Surface", four_button_size, "Import emit_surface.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_bsp_worldlight_include_quake, "Quake", four_button_size, "Import emit_quakelight.");

			SET_CHILD_WIDGET_WIDTH_MAN(145.0f);
			ImGui::DragFloat("Global intensity", &dynamic_lighting::m_bsp_worldlight_intensity_scale, 1.0f, 0.0f, 2000.0f, "%.1f");
			ImGui::SameLine();
			SET_CHILD_WIDGET_WIDTH_MAN(145.0f);
			ImGui::DragFloat("Global radius", &dynamic_lighting::m_bsp_worldlight_radius_scale, 0.0001f, 0.00001f, 0.1f, "%.5f");
		}

		if (map_light_page == 4)
		{
			ui_subsection("Event-driven Map Lights", "Recover lights controlled by Source I/O, relays, lightstyles, alarms and map events.", ICON_FA_BOLT);
			bool event_changed = false;
			event_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_map_event_lights_enabled, "Event Automation", two_button_size,
				"When enabled, compiled lights follow authored Source state instead of remaining permanently on.");
			ImGui::SameLine();
			event_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_map_event_accept_input, "AcceptInput Capture", two_button_size,
				"Captures Enable, Disable, Toggle, TurnOn, TurnOff and Kill delivered at runtime.");
			event_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_map_event_follow_lightstyles, "Lightstyles / Blinking", two_button_size,
				"Follows animated lightstyles used by alarm lamps, blinking electronics and scripted lighting.");
			ImGui::SameLine();
			event_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_map_event_honor_starts_disabled, "Honor Start Disabled", two_button_size,
				"Keeps event lights off until Source logic enables them.");
			event_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_map_event_include_sprite_proxies, "Sprite / Glow Proxies", two_button_size,
				"Associates nearby env_sprite and env_lightglow controls with emit_surface lights.");
			ImGui::SameLine();
			event_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_map_light_io_action_scheduler, "Relay Delay Scheduler", two_button_size,
				"Replays delays authored through logic_relay, multi_manager and entity outputs.");
			if (event_changed) game_settings::mark_dirty("event-driven map lights");

			ImGui::TextWrapped("%s", dynamic_lighting::get_event_light_summary().c_str());
			ImGui::TextDisabled("controlled %u | sprite/glow %u | styled/blinking %u | pending %u | executed %u",
				dynamic_lighting::m_map_event_controlled_candidates, dynamic_lighting::m_map_event_sprite_candidates,
				dynamic_lighting::m_map_event_styled_candidates, dynamic_lighting::m_map_light_io_pending_count,
				dynamic_lighting::m_map_light_io_actions_executed);
			ImGui::SeparatorText("Selected light event test");
			if (ImGui::Button("Turn On", four_button_size)) dynamic_lighting::simulate_selected_map_light_io(source_map_entities::io_action_kind::enable);
			ImGui::SameLine();
			if (ImGui::Button("Turn Off", four_button_size)) dynamic_lighting::simulate_selected_map_light_io(source_map_entities::io_action_kind::disable);
			ImGui::SameLine();
			if (ImGui::Button("Toggle", four_button_size)) dynamic_lighting::simulate_selected_map_light_io(source_map_entities::io_action_kind::toggle);
			ImGui::SameLine();
			if (ImGui::Button("Kill", four_button_size)) dynamic_lighting::simulate_selected_map_light_io(source_map_entities::io_action_kind::kill);
			ImGui::TextWrapped("%s", dynamic_lighting::get_selected_map_light_io_group_details().c_str());
			ImGui::SeparatorText("Observed transitions");
			ImGui::TextWrapped("%s", dynamic_lighting::m_map_light_last_transition.empty() ? "No event-light transition captured yet." : dynamic_lighting::m_map_light_last_transition.c_str());
			if (ImGui::Button("Copy Event Summary", two_button_size))
			{
				const auto summary = dynamic_lighting::get_event_light_summary() + "\n" + dynamic_lighting::get_map_light_transition_log_text();
				ImGui::SetClipboardText(summary.c_str());
			}
			ImGui::SameLine();
			if (ImGui::Button("Clear Transition Log", two_button_size)) dynamic_lighting::clear_map_light_transition_log();
		}

		if (map_light_page == 5)
		{
			ui_subsection("Facing-poly / Emit Surface Linkage", "Expose the BSP TEXINFO material behind emit_surface lights and connect nearby sprite/glow event controls.", ICON_FA_LIGHTBULB);
			bool surface_changed = false;
			surface_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_bsp_worldlight_include_surface, "Import Emit Surface", two_button_size,
				"Imports VRAD emit_surface records generated from emissive/facing polygons.");
			ImGui::SameLine();
			surface_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_map_event_surface_material_linkage, "Resolve Surface Materials", two_button_size,
				"Resolves TEXINFO/TEXDATA names so the light can be traced back to its Source material.");
			surface_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_bsp_worldlight_surface_clusters, "Surface Helper Cluster", two_button_size,
				"Distributes a small helper pattern around broad emissive polygons instead of one oversized point.");
			ImGui::SameLine();
			surface_changed |= imgui::toggle_button_bool(&dynamic_lighting::m_map_event_include_sprite_proxies, "Link Sprite / Glow Events", two_button_size,
				"Links env_sprite or env_lightglow event state to nearby emit_surface records when available.");
			if (surface_changed) game_settings::mark_dirty("facing-poly light linkage");

			SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
			ImGui::DragInt("Cluster samples", &dynamic_lighting::m_bsp_worldlight_surface_cluster_samples, 1.0f, 1, 16);
			ImGui::SameLine(); SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
			ImGui::DragFloat("Cluster spread", &dynamic_lighting::m_bsp_worldlight_surface_cluster_spread, 0.01f, 0.0f, 4.0f, "%.2f");
			ImGui::SameLine(); SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
			ImGui::DragFloat("Cluster power", &dynamic_lighting::m_bsp_worldlight_surface_cluster_intensity, 0.01f, 0.0f, 4.0f, "%.2f");
			ImGui::TextWrapped("%s", dynamic_lighting::m_surface_material_status.c_str());
			ImGui::TextDisabled("material linked %u | unresolved %u | event-linked %u | clustered %u",
				dynamic_lighting::m_surface_material_linked, dynamic_lighting::m_surface_material_unresolved,
				dynamic_lighting::m_surface_event_linked, dynamic_lighting::m_bsp_worldlight_surface_clustered);
			if (ImGui::Button("Copy Facing-poly Report", two_button_size))
			{
				const auto report = dynamic_lighting::get_facing_poly_link_report();
				ImGui::SetClipboardText(report.c_str());
			}
			ImGui::SameLine();
			if (ImGui::Button("Rescan Map Materials", two_button_size)) dynamic_lighting::reload_current_map_lights();

			if (ImGui::BeginTable("##facing_poly_links", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX, ImVec2(0.0f, 300.0f)))
			{
				ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 44.0f);
				ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthFixed, 260.0f);
				ImGui::TableSetupColumn("Texinfo", ImGuiTableColumnFlags_WidthFixed, 70.0f);
				ImGui::TableSetupColumn("Event Group", ImGuiTableColumnFlags_WidthFixed, 90.0f);
				ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableSetupColumn("Link", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableHeadersRow();
				for (const auto& candidate : dynamic_lighting::m_source_bsp_candidates)
				{
					if (candidate.source_type != static_cast<std::int32_t>(source_bsp_lights::emit_type::surface)) continue;
					ImGui::TableNextRow();
					ImGui::TableNextColumn(); ImGui::Text("%u", candidate.source_index);
					ImGui::TableNextColumn(); ImGui::TextUnformatted(candidate.surface_material_name.empty() ? "<unresolved>" : candidate.surface_material_name.c_str());
					ImGui::TableNextColumn(); ImGui::Text("%d", candidate.texinfo);
					ImGui::TableNextColumn(); ImGui::Text("%u", candidate.map_light_control_group);
					ImGui::TableNextColumn(); ImGui::TextUnformatted(candidate.selected && !candidate.override_disabled ? "active" : "off");
					ImGui::TableNextColumn(); ImGui::TextUnformatted(candidate.surface_link_reason.c_str());
				}
				ImGui::EndTable();
			}
		}

		if (map_light_page == 6)
		{
			ui_subsection("Advanced Source Integration", "Entity graph reconstruction, allocation capture and diagnostic counters.", ICON_FA_COG);
			imgui::toggle_button_bool(&dynamic_lighting::m_source_alloc_hooks_enabled, "Capture Allocations", two_button_size,
				"Capture very short dlight and elight allocations between polling frames.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_map_entity_graph_enabled, "Source Entity Graph", two_button_size,
				"Reconstruct map light owners and Source I/O control groups.");

			imgui::toggle_button_bool(&dynamic_lighting::m_map_light_match_entities, "Match Authored Lights", two_button_size,
				"Match compiled lights to authored light entities.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_map_light_auto_bind_nearby_owners, "Auto-bind Nearby Props", two_button_size,
				"Bind otherwise unowned lights to nearby movable map entities.");

			imgui::toggle_button_bool(&dynamic_lighting::m_map_light_extrude_from_models, "Push Out Of Models", two_button_size,
				"Move shaped lights outside owner geometry.");
			ImGui::SameLine();
			imgui::toggle_button_bool(&dynamic_lighting::m_bsp_worldlight_prefer_engine_memory, "Prefer Engine Memory", two_button_size,
				"Use Source's decompressed WORLDLIGHT array before BSP/VPK fallback.");

			ImGui::TextWrapped("Runtime: %s", dynamic_lighting::m_map_light_runtime_status.c_str());
			ImGui::TextWrapped("Entity graph: %s", dynamic_lighting::m_map_entity_graph_status.c_str());
			ImGui::TextDisabled("matched/bound/extruded %u/%u/%u | owner active/hidden/unresolved %u/%u/%u",
				dynamic_lighting::m_map_entity_graph_matched_lights, dynamic_lighting::m_map_entity_graph_bound_owners,
				dynamic_lighting::m_map_entity_graph_extruded, dynamic_lighting::m_owned_worldlights_active,
				dynamic_lighting::m_owned_worldlights_hidden, dynamic_lighting::m_owned_worldlights_unresolved);
			ImGui::TextDisabled("dynamic d/e/projected/entity %u/%u/%u/%u | create failures %u | retry %u",
				dynamic_lighting::m_source_dlight_active, dynamic_lighting::m_source_elight_active,
				dynamic_lighting::m_source_projected_active, dynamic_lighting::m_source_entity_dynamic_active,
				dynamic_lighting::m_bsp_worldlight_create_failed, dynamic_lighting::get_bsp_worldlight_auto_import_retries());
			if (!dynamic_lighting::m_bsp_worldlight_diagnostics.empty())
				ImGui::TextWrapped("Layout: %s", dynamic_lighting::m_bsp_worldlight_diagnostics.c_str());
			if (!dynamic_lighting::m_bsp_worldlight_source.empty())
				ImGui::TextWrapped("Source: %s", dynamic_lighting::m_bsp_worldlight_source.c_str());
		}
	}

	void cont_general_event_workbench()
	{
		ImGui::TextDisabled("Recent sounds/choreographies captured by hooks. Use copy buttons to create map_settings triggers faster.");
		ImGui::Spacing(0, 4);

		const auto two_row_button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f, 0);
		imgui::toggle_button_bool(&cmd::sound_debug_printing, "Sound: also print to console", two_row_button_size, "Same as cmd xo_debug_sound_print");
		ImGui::SameLine();
		imgui::toggle_button_bool(&cmd::scene_print, "Choreo: also print to console", two_row_button_size, "Same as cmd xo_debug_scene_print");

		ImGui::Spacing(0, 8);

		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
		ImGui::TableHeaderDropshadow();
		const bool sound_state = ImGui::CollapsingHeader("Recent Sound Events", ImGuiTreeNodeFlags_DefaultOpen);
		ImGui::PopStyleVar();

		if (sound_state)
		{
			ImGui::BeginDisabled(sound_events::get_history().empty());
			if (ImGui::Button("Clear Sound History   " ICON_FA_BROOM, ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
				sound_events::clear_history();
			}
			ImGui::EndDisabled();

			if (ImGui::BeginTable("EventWorkbenchSoundTable", 8,
				ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
				ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_ScrollY,
				ImVec2(0, 220)))
			{
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 52.0f);
				ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 82.0f);
				ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 280.0f);
				ImGui::TableSetupColumn("Vol", ImGuiTableColumnFlags_WidthFixed, 48.0f);
				ImGui::TableSetupColumn("Delay", ImGuiTableColumnFlags_WidthFixed, 48.0f);
				ImGui::TableSetupColumn("Origin", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 170.0f);
				ImGui::TableSetupColumn("Copy", ImGuiTableColumnFlags_WidthFixed, 120.0f);
				ImGui::TableSetupColumn("Copy2", ImGuiTableColumnFlags_WidthFixed, 120.0f);
				ImGui::TableHeadersRow();

				const auto& history = sound_events::get_history();
				for (auto i = 0u; i < history.size(); ++i)
				{
					const auto& ev = history[i];
					ImGui::PushID((int)i);
					ImGui::TableNextRow();

					ImGui::TableNextColumn();
					ImGui::Text("%.2f", ev.time);

					ImGui::TableNextColumn();
					ImGui::Text("0x%X", ev.hash);

					ImGui::TableNextColumn();
					ImGui::TextWrapped("%s", ev.name.c_str());

					ImGui::TableNextColumn();
					ImGui::Text("%.2f", ev.volume);

					ImGui::TableNextColumn();
					ImGui::Text("%.2f", ev.delay);

					ImGui::TableNextColumn();
					ImGui::Text("%.1f %.1f %.1f", ev.origin.x, ev.origin.y, ev.origin.z);

					ImGui::TableNextColumn();
					if (ImGui::Button("Light Trigger")) {
						copy_text_to_clipboard(std::format("trigger = {{ sound = 0x{:X} }}", ev.hash));
					}
					TT("Copies a light/remix_vars style sound-hash trigger snippet.");

					ImGui::TableNextColumn();
					if (ImGui::Button("Marker Name")) {
						copy_text_to_clipboard("trigger = { show = { sound = \"" + toml_escape_inline_string(ev.name) + "\" }, always = true }");
					}
					TT("Copies a marker trigger using sound-name substring. Useful when hashes change due to origin/volume.");

					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}

		ImGui::Spacing(0, 8);

		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
		ImGui::TableHeaderDropshadow();
		const bool choreo_state = ImGui::CollapsingHeader("Recent Choreography Events", ImGuiTreeNodeFlags_DefaultOpen);
		ImGui::PopStyleVar();

		if (choreo_state)
		{
			ImGui::BeginDisabled(choreo_events::get_history().empty());
			if (ImGui::Button("Clear Choreo History   " ICON_FA_BROOM, ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
				choreo_events::clear_history();
			}
			ImGui::EndDisabled();

			if (ImGui::BeginTable("EventWorkbenchChoreoTable", 7,
				ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
				ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_ScrollY,
				ImVec2(0, 220)))
			{
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 52.0f);
				ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 44.0f);
				ImGui::TableSetupColumn("VCD", ImGuiTableColumnFlags_WidthStretch, 280.0f);
				ImGui::TableSetupColumn("Actor", ImGuiTableColumnFlags_WidthFixed, 96.0f);
				ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthFixed, 96.0f);
				ImGui::TableSetupColumn("Param1", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 140.0f);
				ImGui::TableSetupColumn("Copy", ImGuiTableColumnFlags_WidthFixed, 130.0f);
				ImGui::TableHeadersRow();

				const auto& history = choreo_events::get_history();
				for (auto i = 0u; i < history.size(); ++i)
				{
					const auto& ev = history[i];
					ImGui::PushID((int)i);
					ImGui::TableNextRow();

					ImGui::TableNextColumn();
					ImGui::Text("%.2f", ev.time);

					ImGui::TableNextColumn();
					ImGui::TextUnformatted(ev.is_start ? "Start" : "End");

					ImGui::TableNextColumn();
					ImGui::TextWrapped("%s", ev.name.c_str());

					ImGui::TableNextColumn();
					ImGui::TextWrapped("%s", ev.actor.c_str());

					ImGui::TableNextColumn();
					ImGui::TextWrapped("%s", ev.event.c_str());

					ImGui::TableNextColumn();
					ImGui::TextWrapped("%s", ev.param1.c_str());

					ImGui::TableNextColumn();
					ImGui::BeginDisabled(!ev.is_start);
					if (ImGui::Button("Choreo Trigger")) {
						copy_text_to_clipboard(build_choreo_trigger_snippet(ev));
					}
					ImGui::EndDisabled();
					TT("Copies a choreo trigger snippet for lights, markers or remix var transitions.");

					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}
	}


	struct auto_pbr_profile_s
	{
		const char* name;
		const char* summary;
		bool require_assets;
		bool live_fallback;
		bool debug_previews;
		bool quality_gate;
		bool reject_normals;
		bool roughness;
		bool ssbump_height;
		bool raw_vtf;
		bool html_audit;
		bool quarantine;
		bool auto_review;
		float confidence;
	};

	static const auto_pbr_profile_s AUTO_PBR_PROFILES[] =
	{
		{ "Safe Asset-backed", "Best default: only real Source VMT/VTF materials are auto-activated.", true, true, true, true, true, true, false, true, true, true, false, 0.68f },
		{ "Balanced", "Keeps strict quality checks while allowing labelled live-sampler fallbacks for authoring.", true, true, true, true, true, true, false, true, true, true, false, 0.62f },
		{ "Fast Capture", "Reduces previews and reporting overhead while retaining safe activation rules.", true, true, false, true, true, true, false, false, false, true, false, 0.62f },
		{ "Research / Manual", "Exports the most diagnostic data but never auto-activates review-grade materials.", false, true, true, true, false, true, true, true, true, true, false, 0.50f },
	};

	void apply_auto_pbr_profile(const int profile_index)
	{
		if (profile_index < 0 || profile_index >= IM_ARRAYSIZE(AUTO_PBR_PROFILES)) return;
		const auto& profile = AUTO_PBR_PROFILES[profile_index];
		material_exporter::m_resolve_source_assets_during_capture = true;
		material_exporter::m_require_asset_backed_activation = profile.require_assets;
		material_exporter::m_allow_live_sampler_fallback = profile.live_fallback;
		material_exporter::m_export_debug_png = profile.debug_previews;
		material_exporter::m_enable_quality_gate = profile.quality_gate;
		material_exporter::m_reject_suspicious_native_maps = profile.reject_normals;
		material_exporter::m_generate_roughness_from_albedo = profile.roughness;
		material_exporter::m_generate_ssbump_height = profile.ssbump_height;
		material_exporter::m_export_raw_vtf = profile.raw_vtf;
		material_exporter::m_write_html_audit = profile.html_audit;
		material_exporter::m_quarantine_low_confidence = profile.quarantine;
		material_exporter::m_auto_activate_review_materials = profile.auto_review;
		material_exporter::m_minimum_auto_activation_confidence = profile.confidence;
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
		game_settings::mark_dirty("Auto PBR profile");
	}

	void cont_general_import_materials_from_maps()
	{
		const bool advanced_ui = ui_advanced_mode();
		static int auto_pbr_profile = 1;

		ui_subsection("Auto PBR Studio", "A guided Source-material workflow: capture real VMT/VTF assets, resolve PBR channels, then review only the exceptions.", ICON_FA_MAGIC);
		if (!ui_compact_descriptions())
		{
			ImGui::TextWrapped("Real Source VMT/VTF assets from loose files or mounted VPKs are the primary authority. Live runtime samplers remain labelled fallback data and are not presented as complete materials.");
		}
		ui_basic_mode_note("The uncommon exporter, hash-injection and diagnostic generation controls are hidden.");

		static int auto_pbr_page = 0; // 0 workflow, 1 registry, 2 inspector, 3 advanced
		if (ImGui::BeginTabBar("##auto_pbr_pages", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll))
		{
			if (ImGui::BeginTabItem(ICON_FA_MAGIC "  Workflow")) { auto_pbr_page = 0; ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem(ICON_FA_DATABASE "  Registry")) { auto_pbr_page = 1; ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem(ICON_FA_SEARCH "  Inspector")) { auto_pbr_page = 2; ImGui::EndTabItem(); }
			if (advanced_ui && ImGui::BeginTabItem(ICON_FA_COG "  Advanced")) { auto_pbr_page = 3; ImGui::EndTabItem(); }
			ImGui::EndTabBar();
		}

		if (auto_pbr_page == 0)
		{
		const int profile_columns = ui_responsive_column_count(260.0f, 2);
		if (ImGui::BeginTable("##auto_pbr_profile", profile_columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::Combo("Workflow Profile", &auto_pbr_profile, [](void* data, int idx, const char** out_text) {
				const auto* profiles = static_cast<const auto_pbr_profile_s*>(data);
				*out_text = profiles[idx].name; return true;
			}, const_cast<auto_pbr_profile_s*>(AUTO_PBR_PROFILES), IM_ARRAYSIZE(AUTO_PBR_PROFILES));
			TT(AUTO_PBR_PROFILES[std::clamp(auto_pbr_profile, 0, IM_ARRAYSIZE(AUTO_PBR_PROFILES) - 1)].summary);
			ImGui::TableNextColumn();
			if (ImGui::Button("Apply Profile", ImVec2(-1.0f, 0.0f))) apply_auto_pbr_profile(auto_pbr_profile);
			ImGui::EndTable();
		}

		if (ImGui::BeginTable("##auto_pbr_essentials", ui_responsive_column_count(235.0f, 6), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ImGui::Checkbox("Asset-backed activation", &material_exporter::m_require_asset_backed_activation);
			TT("Recommended. Only a real Source VMT plus its resolved base-color VTF may be automatically activated.");
			ImGui::TableNextColumn(); ImGui::Checkbox("Source shader semantics", &material_exporter::m_enable_source_shader_semantics);
			TT("Interprets Source shader rules before mapping envmap, Phong, alpha, detail and emissive inputs into PBR channels.");
			ImGui::TableNextColumn(); ImGui::Checkbox("Resolve model $cdmaterials", &material_exporter::m_resolve_model_cdmaterials);
			TT("Uses the material search paths compiled into MDL files to find mesh-local VertexLitGeneric VMTs.");
			ImGui::TableNextColumn(); ImGui::Checkbox("Generate roughness", &material_exporter::m_generate_roughness_from_albedo);
			ImGui::TableNextColumn(); ImGui::Checkbox("Quality gate", &material_exporter::m_enable_quality_gate);
			ImGui::TableNextColumn(); ImGui::Checkbox("Apply overrides", &material_exporter::m_apply_material_overrides);
			ImGui::EndTable();
		}

		}

		if (advanced_ui && auto_pbr_page == 3)
		{
			ui_subsection("Advanced Auto PBR Options", "Low-level resolver, export and activation policy controls.", ICON_FA_COG);
			if (ImGui::BeginTable("##pbr_resolver_options", ui_responsive_column_count(250.0f, 3), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn();
				ImGui::Checkbox("Inject stable Source hashes", &material_exporter::m_inject_stable_hashes);
                TT("Independent authoring option. The bridge injects its stable hash automatically while publication is enabled; OFF preserves native Remix hashing when the bridge is also OFF.");
                ImGui::Checkbox("Publish Source Material Bridge", &material_exporter::m_source_material_bridge_enabled);
                TT("Publishes Source VMT/PBR semantics and carries the matching 64-bit identity through RS150/RS153.");
                ImGui::Checkbox("Publish Studio model draw packets", &material_exporter::m_model_material_packets_enabled);
                TT("Adds model path, entity, skin, body, material ordinal and Source sampler bindings through bridge ABI v2. RS214/RS215 identify the packet for the current draw.");
                ImGui::TextDisabled("Studio packet stream: %s | latest packet: %u",
                    material_exporter::m_model_material_packets_enabled ? "enabled" : "disabled",
                    material_exporter::get_latest_source_bridge_draw_packet_id());
				ImGui::Checkbox("Debug PNG previews", &material_exporter::m_export_debug_png);
				ImGui::Checkbox("Reject suspicious normals", &material_exporter::m_reject_suspicious_native_maps);
				ImGui::Checkbox("Include UI materials", &material_exporter::m_include_ui_materials);
				ImGui::Checkbox("Include error materials", &material_exporter::m_include_error_materials);

				ImGui::TableNextColumn();
				ImGui::Checkbox("SSBump diagnostic height", &material_exporter::m_generate_ssbump_height);
				TT("Diagnostic only. SSBump directional energy is not true geometric height.");
				ImGui::Checkbox("Generate water albedo", &material_exporter::m_generate_water_albedo);
				ImGui::Checkbox("Export original VTF", &material_exporter::m_export_raw_vtf);
				ImGui::Checkbox("Export opacity masks", &material_exporter::m_export_opacity_masks);
				ImGui::Checkbox("Export detail authoring assets", &material_exporter::m_export_detail_authoring_assets);

				ImGui::TableNextColumn();
				ImGui::Checkbox("Resolve assets during capture", &material_exporter::m_resolve_source_assets_during_capture);
				ImGui::Checkbox("Source shader semantics", &material_exporter::m_enable_source_shader_semantics);
				ImGui::Checkbox("Resolve model $cdmaterials", &material_exporter::m_resolve_model_cdmaterials);
				ImGui::Checkbox("Inspect VTF metadata", &material_exporter::m_inspect_vtf_metadata);
				ImGui::Checkbox("Preserve dynamic materials for review", &material_exporter::m_preserve_dynamic_materials_for_review);
				ImGui::Checkbox("Allow conflicting $envmapmask fallback", &material_exporter::m_allow_envmapmask_conflict_fallback);
				TT("Off by default. Source often rejects $envmapmask when bump/lightwarp/Phong paths own the specular mask.");
				ImGui::Checkbox("Use detail as PBR microdetail", &material_exporter::m_use_detail_as_pbr_microdetail);
				ImGui::Checkbox("Export semantic manifest", &material_exporter::m_export_semantic_manifest);
				ImGui::Checkbox("Resolve Patch/include VMT", &material_exporter::m_enable_patch_vmt_resolution);
				TT("Builds the final effective VMT by applying include, insert and replace blocks in Source order.");
				ImGui::Checkbox("Calibrated Phong/specular PBR", &material_exporter::m_enable_calibrated_specular);
				TT("Separates highlight width, reflection weight, dielectric F0 and metal classification instead of converting every specular mask into metalness.");
				ImGui::Checkbox("Compose supported detail roles", &material_exporter::m_compose_detail_layers);
				ImGui::Checkbox("Preserve WorldVertexTransition layers", &material_exporter::m_preserve_world_vertex_transition);
				ImGui::Checkbox("Link event emissive candidates", &material_exporter::m_link_event_emissive_materials);
				TT("Marks emissive materials controlled by proxies or light-like names for facing-poly/event-light linkage.");
				ImGui::Checkbox("Allow live sampler fallback", &material_exporter::m_allow_live_sampler_fallback);
				ImGui::Checkbox("Rescan unresolved periodically", &material_exporter::m_rescan_unresolved_assets_periodically);
				ImGui::Checkbox("Quarantine low confidence", &material_exporter::m_quarantine_low_confidence);
				ImGui::Checkbox("Auto-activate review grade", &material_exporter::m_auto_activate_review_materials);
				ImGui::SetNextItemWidth(-1.0f);
				ImGui::SliderFloat("Minimum confidence", &material_exporter::m_minimum_auto_activation_confidence, 0.35f, 0.95f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
				ImGui::Checkbox("Write HTML audit", &material_exporter::m_write_html_audit);
				ImGui::Checkbox("Clean output before export", &material_exporter::m_clean_export_before_export);
				ImGui::Checkbox("Overwrite generated DDS", &material_exporter::m_overwrite_existing_dds);
				ImGui::EndTable();
			}
		}

		const auto records = material_exporter::get_records();
		if (auto_pbr_page == 0)
		{
		ImGui::SeparatorText("Guided workflow");
		const int action_columns = ui_responsive_column_count(190.0f, 3);
		if (ImGui::BeginTable("##auto_pbr_actions", action_columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn();
			if (ImGui::Button("1. Capture Materials", ImVec2(-1.0f, 42.0f))) material_exporter::begin_capture();
			TT("Walk the map after enabling capture so the Source renderer can observe real material usage.");

			ImGui::TableNextColumn();
			ImGui::BeginDisabled(records.empty());
			if (ImGui::Button("2. Resolve + Build PBR", ImVec2(-1.0f, 42.0f))) material_exporter::export_all();
			ImGui::EndDisabled();

			ImGui::TableNextColumn();
			ImGui::BeginDisabled(records.empty());
			if (ImGui::Button("3. Review Missing Assets", ImVec2(-1.0f, 42.0f))) material_exporter::write_audit_report();
			ImGui::EndDisabled();

			ImGui::TableNextColumn();
			if (ImGui::Button("Reload Overrides", ImVec2(-1.0f, 0.0f))) material_exporter::reload_overrides();
			ImGui::TableNextColumn();
			if (ImGui::Button("Create Override File", ImVec2(-1.0f, 0.0f))) material_exporter::write_override_template();
			ImGui::TableNextColumn();
			ImGui::BeginDisabled(records.empty());
			if (ImGui::Button("Rescan Source Assets", ImVec2(-1.0f, 0.0f))) material_exporter::rescan_source_assets();
			ImGui::EndDisabled();

			ImGui::TableNextColumn();
			ImGui::BeginDisabled(records.empty());
			if (ImGui::Button("Rebuild Audit", ImVec2(-1.0f, 0.0f))) material_exporter::write_audit_report();
			ImGui::EndDisabled();
			ImGui::TableNextColumn();
			if (ImGui::Button("Open HTML Audit", ImVec2(-1.0f, 0.0f)))
			{
				const auto path = material_exporter::get_audit_report_path();
				ShellExecuteA(nullptr, "open", path.string().c_str(), nullptr, path.parent_path().string().c_str(), SW_SHOWNORMAL);
			}
			ImGui::TableNextColumn();
			if (ImGui::Button("Open Output Folder", ImVec2(-1.0f, 0.0f)))
			{
				const auto path = material_exporter::get_export_root();
				ShellExecuteA(nullptr, "open", path.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
			ImGui::EndTable();
		}

		ImGui::Spacing(0, 6);
		ImGui::TextWrapped("%s", material_exporter::get_status().c_str());
		ImGui::TextDisabled("Output: %s\\Materials", material_exporter::get_export_root().string().c_str());
		ImGui::TextDisabled("Overrides: %s (%zu section(s))",
			material_exporter::get_overrides_path().string().c_str(),
			material_exporter::get_override_count());

		const auto resolved = std::count_if(records.begin(), records.end(), [](const auto& r) {
			return r.exported && r.dds_ready_count >= 3;
		});
		const auto active = std::count_if(records.begin(), records.end(), [](const auto& r) {
			return r.auto_activated;
		});
		const auto quarantined = std::count_if(records.begin(), records.end(), [](const auto& r) {
			return r.quarantined;
		});
		const auto warning_materials = std::count_if(records.begin(), records.end(), [](const auto& r) {
			return r.warning_count > 0;
		});
		const auto overrides = std::count_if(records.begin(), records.end(), [](const auto& r) {
			return r.override_applied;
		});
		const auto asset_backed = std::count_if(records.begin(), records.end(), [](const auto& r) {
			return r.asset_backed_material;
		});
		const auto vmt_resolved = std::count_if(records.begin(), records.end(), [](const auto& r) {
			return r.source_vmt_resolved;
		});
		const auto missing_assets = std::count_if(records.begin(), records.end(), [](const auto& r) {
			return r.missing_texture_asset_count > 0u || !r.source_vmt_resolved;
		});
		ImGui::SeparatorText("Material summary");
		if (ImGui::BeginTable("##material_asset_cards", ui_responsive_column_count(150.0f, 4), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ui_metric_card("Captured", std::format("{}", records.size()));
			ImGui::TableNextColumn(); ui_metric_card("Real Source VMT", std::format("{}", vmt_resolved));
			ImGui::TableNextColumn(); ui_metric_card("Asset-backed", std::format("{}", asset_backed));
			ImGui::TableNextColumn(); ui_metric_card("Missing assets", std::format("{}", missing_assets));
			ImGui::EndTable();
		}
		ImGui::Text("Resolved PBR: %zu | Auto-active: %zu | Quarantined: %zu", resolved, active, quarantined);
		ImGui::TextDisabled("With warnings: %zu | Manual overrides: %zu | Required channels: Albedo + Normal + Roughness",
			warning_materials, overrides);
		}

		static char search_text[192]{};
		static bool filter_warnings = false;
		static bool filter_incomplete = false;
		static bool filter_fallback = false;
		static bool filter_quarantined = false;
		static bool filter_active = false;
		static bool filter_water = false;
		static bool filter_asset_backed = false;
		static bool filter_missing_assets = false;
		static std::uint64_t selected_hash = 0;

		if (auto_pbr_page == 1)
		{
		ui_subsection("Captured Material Registry", "Search and filter captured Source materials without expanding another nested panel.", ICON_FA_DATABASE);
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##material_search", "Search VMT, shader, profile, surfaceprop or hash...",
			search_text, IM_ARRAYSIZE(search_text));
		if (ImGui::BeginTable("##material_filter_grid", ui_responsive_column_count(135.0f, 4), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ImGui::Checkbox("Warnings", &filter_warnings);
			ImGui::TableNextColumn(); ImGui::Checkbox("Incomplete", &filter_incomplete);
			ImGui::TableNextColumn(); ImGui::Checkbox("Fallback", &filter_fallback);
			ImGui::TableNextColumn(); ImGui::Checkbox("Quarantine", &filter_quarantined);
			ImGui::TableNextColumn(); ImGui::Checkbox("Active", &filter_active);
			ImGui::TableNextColumn(); ImGui::Checkbox("Water", &filter_water);
			ImGui::TableNextColumn(); ImGui::Checkbox("Asset-backed", &filter_asset_backed);
			ImGui::TableNextColumn(); ImGui::Checkbox("Missing assets", &filter_missing_assets);
			ImGui::EndTable();
		}

		const auto lower_copy = [](std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return value;
		};
		const std::string search = lower_copy(search_text);

		const auto matches_filters = [&](const material_exporter::record_view_s& record)
		{
			if (filter_warnings && record.warning_count == 0) return false;
			if (filter_incomplete && record.exported) return false;
			if (filter_quarantined && !record.quarantined) return false;
			if (filter_active && !record.auto_activated) return false;
			if (filter_water && !record.water_material) return false;
			if (filter_asset_backed && !record.asset_backed_material) return false;
			if (filter_missing_assets && record.source_vmt_resolved && record.missing_texture_asset_count == 0u) return false;
			if (filter_fallback)
			{
				const bool fallback = std::any_of(record.channels.begin(), record.channels.end(),
					[](const auto& channel) { return channel.provenance == "Constant fallback"; });
				if (!fallback) return false;
			}
			if (!search.empty())
			{
				const std::string probe = lower_copy(record.material_name + " " +
					record.shader_name + " " + record.shader_profile + " " +
					record.surface_prop + " " + record.category + " " +
					record.material_grade + " " + record.activation_reason + " " +
					record.source_asset_grade + " " + record.source_vmt_status + " " +
					record.source_shader_family + " " + record.specular_semantics + " " +
					record.emission_semantics + " " + record.alpha_semantics + " " +
					record.animation_semantics + " " + record.resolved_vmt_path + " " +
					std::format("{:016x}", record.remix_hash));
				if (!probe.contains(search)) return false;
			}
			return true;
		};

		if (selected_hash == 0 && !records.empty())
			selected_hash = records.front().remix_hash;

		if (ImGui::BeginTable("##source_material_registry_v218", 8,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
			ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
			ImVec2(0, 390)))
		{
			ImGui::TableSetupColumn("Source VMT", ImGuiTableColumnFlags_WidthStretch, 310.0f);
			ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthFixed, 145.0f);
			ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 138.0f);
			ImGui::TableSetupColumn("Draws", ImGuiTableColumnFlags_WidthFixed, 50.0f);
			ImGui::TableSetupColumn("Grade / DDS", ImGuiTableColumnFlags_WidthFixed, 125.0f);
			ImGui::TableSetupColumn("Source assets", ImGuiTableColumnFlags_WidthFixed, 155.0f);
			ImGui::TableSetupColumn("Warnings", ImGuiTableColumnFlags_WidthFixed, 72.0f);
			ImGui::TableSetupColumn("Resolver summary", ImGuiTableColumnFlags_WidthStretch, 260.0f);
			ImGui::TableHeadersRow();

			for (const auto& record : records)
			{
				if (!matches_filters(record)) continue;
				ImGui::PushID(static_cast<int>(record.remix_hash ^ (record.remix_hash >> 32u)));
				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				const bool selected = selected_hash == record.remix_hash;
				const std::string label = "materials/" + record.material_name + ".vmt";
				if (ImGui::Selectable(label.c_str(), selected,
					ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
					selected_hash = record.remix_hash;
				if (record.override_applied)
				ImGui::TextDisabled("manual override");
				if (record.dynamic_material)
				ImGui::TextDisabled("dynamic/static approximation");

				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", record.source_shader_family.c_str());
				ImGui::TextDisabled("%s | %s", record.shader_profile.c_str(), record.category.c_str());

				ImGui::TableNextColumn();
				ImGui::Text("%016llX", static_cast<unsigned long long>(record.remix_hash));

				ImGui::TableNextColumn();
				ImGui::Text("%u", record.draw_count);

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(record.material_grade.c_str());
				if (record.exported)
					ImGui::TextDisabled("DDS %u/8", record.dds_ready_count);
				else
					ImGui::TextDisabled("captured");
				if (record.auto_activated)
					ImGui::TextDisabled("auto-active");
				else if (record.quarantined)
					ImGui::TextDisabled("quarantined");

				ImGui::TableNextColumn();
				if (record.asset_backed_material)
					ImGui::TextUnformatted(ICON_FA_DATABASE "  Asset-backed");
				else if (record.source_vmt_resolved)
					ImGui::TextUnformatted(ICON_FA_EXCLAMATION_TRIANGLE "  Partial");
				else
					ImGui::TextDisabled("live-only / unresolved");
				ImGui::TextDisabled("VTF %u/%u | missing %u",
					record.resolved_texture_asset_count, record.declared_texture_count,
					record.missing_texture_asset_count);

				ImGui::TableNextColumn();
				if (record.warning_count > 0)
					ImGui::Text("%u", record.warning_count);
				else
					ImGui::TextDisabled("0");

				ImGui::TableNextColumn();
				ImGui::TextWrapped("A:%s N:%s R:%s M:%s",
					record.channels[0].provenance.c_str(),
					record.channels[1].provenance.c_str(),
					record.channels[2].provenance.c_str(),
					record.channels[3].provenance.c_str());
				ImGui::TextDisabled("%s", record.specular_semantics.c_str());
				if (!record.semantic_conflicts.empty())
					ImGui::TextDisabled("semantic review: %zu", record.semantic_conflicts.size());
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		}

		const material_exporter::record_view_s* selected_record = nullptr;
		for (const auto& record : records)
			if (record.remix_hash == selected_hash) { selected_record = &record; break; }

		if (!selected_record && !records.empty())
		{
			selected_hash = records.front().remix_hash;
			selected_record = &records.front();
		}

		if (auto_pbr_page != 2) return;
		ui_subsection("Selected Material Inspector", "Resolved Source assets, channel provenance and activation policy for the selected record.", ICON_FA_SEARCH);
		if (!selected_record)
		{
			ImGui::TextDisabled("Capture materials to inspect their resolved PBR channels.");
			return;
		}

		ImGui::TextWrapped("%s", selected_record->resolved_vmt_path.empty()
			? ("materials/" + selected_record->material_name + ".vmt").c_str()
			: selected_record->resolved_vmt_path.c_str());
		ImGui::TextDisabled("%s | %s | identity %s",
			selected_record->shader_name.c_str(), selected_record->shader_profile.c_str(),
			selected_record->identity_key.c_str());
		ImGui::Text("Roughness %.3f | Metalness %.3f | Resolver confidence %.0f%%",
			selected_record->roughness, selected_record->metallic,
			selected_record->confidence * 100.0f);
		ImGui::Text("Grade: %s | %s | Opacity: %s | Detail: %s",
			selected_record->material_grade.c_str(),
			selected_record->auto_activated ? "AUTO-ACTIVE" : (selected_record->quarantined ? "QUARANTINED" : "NOT ACTIVE"),
			selected_record->opacity_exported ? "exported" : "none",
			selected_record->detail_exported ? "exported" : "none");
		ImGui::TextWrapped("Activation policy: %s", selected_record->activation_reason.c_str());

		ImGui::SeparatorText("Original Source asset integrity");
		if (ImGui::BeginTable("##source_asset_integrity", ui_responsive_column_count(160.0f, 4), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ui_metric_card("Asset grade", selected_record->source_asset_grade);
			ImGui::TableNextColumn(); ui_metric_card("Original VMT", selected_record->source_vmt_resolved ? (selected_record->source_vmt_from_vpk ? "VPK" : "loose file") : "missing");
			ImGui::TableNextColumn(); ui_metric_card("Declared VTF", std::format("{}/{}", selected_record->resolved_texture_asset_count, selected_record->declared_texture_count));
			ImGui::TableNextColumn(); ui_metric_card("Base albedo", selected_record->source_albedo_asset_resolved ? "resolved" : "missing");
			ImGui::EndTable();
		}
		ImGui::TextDisabled("VMT status: %s | bytes %llu | content hash %016llX | live texture params %u",
			selected_record->source_vmt_status.c_str(),
			static_cast<unsigned long long>(selected_record->source_vmt_byte_size),
			static_cast<unsigned long long>(selected_record->source_vmt_hash),
			selected_record->live_texture_param_count);
		if (!selected_record->asset_backed_material)
			ImGui::TextWrapped(ICON_FA_EXCLAMATION_TRIANGLE "  This entry is not backed by both a real Source VMT and its declared base-color VTF. Runtime sampler pixels are kept only as a labelled fallback.");
		if (!selected_record->missing_source_assets.empty())
		{
			ImGui::TextDisabled("Missing original Source assets:");
			const std::size_t limit = std::min<std::size_t>(selected_record->missing_source_assets.size(), 8u);
			for (std::size_t i = 0u; i < limit; ++i)
				ImGui::BulletText("%s", selected_record->missing_source_assets[i].c_str());
			if (selected_record->missing_source_assets.size() > limit)
				ImGui::TextDisabled("...and %zu more; full list is written to provenance_graph.json.", selected_record->missing_source_assets.size() - limit);
		}

		ImGui::SeparatorText("Source shader interpretation");
		if (ImGui::BeginTable("##source_shader_semantics", ui_responsive_column_count(190.0f, 4), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ui_metric_card("Shader family", selected_record->source_shader_family);
			ImGui::TableNextColumn(); ui_metric_card("Specular source", selected_record->specular_semantics);
			ImGui::TableNextColumn(); ui_metric_card("Emission source", selected_record->emission_semantics);
			ImGui::TableNextColumn(); ui_metric_card("Alpha mode", selected_record->alpha_semantics);
			ImGui::TableNextColumn(); ui_metric_card("Animation", selected_record->animation_semantics);
			ImGui::TableNextColumn(); ui_metric_card("Model VMT resolver", selected_record->resolved_via_model_search_path ? "resolved via $cdmaterials" : "direct / world path");
			ImGui::TableNextColumn(); ui_metric_card("VTF flags", std::format("normal {} | ssbump {} | alpha {}", selected_record->vtf_normal_flag_count, selected_record->vtf_ssbump_flag_count, selected_record->vtf_alpha_count));
			ImGui::TableNextColumn(); ui_metric_card("Dynamic inputs", std::format("frames {} | proxies {}", selected_record->vtf_animated_count, selected_record->detected_proxies.size()));
			ImGui::TableNextColumn(); ui_metric_card("Material graph", selected_record->material_graph_summary);
			ImGui::TableNextColumn(); ui_metric_card("Patch/include", selected_record->patch_semantics);
			ImGui::TableNextColumn(); ui_metric_card("Detail role", selected_record->detail_semantics);
			ImGui::TableNextColumn(); ui_metric_card("Layer policy", selected_record->layer_semantics);
			ImGui::TableNextColumn(); ui_metric_card("Event emissive", selected_record->event_emissive_semantics);
			ImGui::TableNextColumn(); ui_metric_card("Calibrated PBR", std::format("R {:.3f} | refl {:.3f} | F0 {:.3f} | metal {:.3f}", selected_record->calibrated_roughness, selected_record->calibrated_reflection_weight, selected_record->calibrated_dielectric_f0, selected_record->calibrated_metallic_hint));
			ImGui::EndTable();
		}
		if (!selected_record->vmt_include_chain.empty())
		{
			ImGui::TextDisabled("Resolved VMT include chain:");
			for (const auto& include : selected_record->vmt_include_chain)
				ImGui::BulletText("%s", include.c_str());
		}
		if (!selected_record->vmt_patch_warnings.empty())
		{
			ImGui::TextWrapped(ICON_FA_EXCLAMATION_TRIANGLE "  Patch/include diagnostics:");
			for (const auto& warning : selected_record->vmt_patch_warnings)
				ImGui::BulletText("%s", warning.c_str());
		}

		if (!selected_record->model_material_search_paths.empty())
		{
			ImGui::TextDisabled("VMT candidates (%u):", selected_record->model_candidate_count);
			for (std::size_t i = 0; i < std::min<std::size_t>(selected_record->model_material_search_paths.size(), 6u); ++i)
				ImGui::BulletText("%s", selected_record->model_material_search_paths[i].c_str());
		}
		if (!selected_record->semantic_conflicts.empty())
		{
			ImGui::TextWrapped(ICON_FA_EXCLAMATION_TRIANGLE "  Source semantic conflicts require review:");
			for (const auto& conflict : selected_record->semantic_conflicts)
				ImGui::BulletText("%s", conflict.c_str());
		}
		if (!selected_record->detected_proxies.empty())
		{
			ImGui::TextDisabled("Detected Source material proxies:");
			for (const auto& proxy : selected_record->detected_proxies)
				ImGui::BulletText("%s", proxy.c_str());
		}

		if (ImGui::BeginTable("##selected_material_actions", ui_responsive_column_count(180.0f, 4), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn();
			if (ImGui::Button("Rescan Selected Source", ImVec2(-1.0f, 30.0f))) material_exporter::rescan_source_asset(selected_record->remix_hash);
			ImGui::TableNextColumn();
			if (ImGui::Button("Export Selected Material", ImVec2(-1.0f, 30.0f))) material_exporter::export_material(selected_record->remix_hash);
			ImGui::TableNextColumn();
			if (ImGui::Button("Open Material Folder", ImVec2(-1.0f, 30.0f)))
			{
				const auto path = material_exporter::get_export_root() / "Materials" / selected_record->export_directory;
				ShellExecuteA(nullptr, "open", path.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
			ImGui::TableNextColumn();
			if (ImGui::Button("Open Override File", ImVec2(-1.0f, 30.0f)))
			{
				const auto path = material_exporter::get_overrides_path();
				ShellExecuteA(nullptr, "open", path.string().c_str(), nullptr, path.parent_path().string().c_str(), SW_SHOWNORMAL);
			}
			ImGui::EndTable();
		}

		if (ImGui::BeginTable("##pbr_channel_inspector", 8,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
			ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, 78.0f);
			ImGui::TableSetupColumn("Provenance", ImGuiTableColumnFlags_WidthFixed, 112.0f);
			ImGui::TableSetupColumn("Confidence", ImGuiTableColumnFlags_WidthFixed, 82.0f);
			ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 270.0f);
			ImGui::TableSetupColumn("Sampler", ImGuiTableColumnFlags_WidthFixed, 72.0f);
			ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 82.0f);
			ImGui::TableSetupColumn("Quality", ImGuiTableColumnFlags_WidthStretch, 220.0f);
			ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 92.0f);
			ImGui::TableHeadersRow();

			for (const auto& channel : selected_record->channels)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(channel.name.c_str());
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", channel.provenance.c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%.0f%%", channel.confidence * 100.0f);
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", channel.source.empty() ? "(none)" : channel.source.c_str());
				if (!channel.declared_texture.empty())
					ImGui::TextDisabled("%s", channel.declared_texture.c_str());
				ImGui::TableNextColumn();
				if (channel.sampler_slot >= 0)
					ImGui::Text("%d / %d", channel.sampler_slot, channel.match_score);
				else
					ImGui::TextDisabled("-");
				ImGui::TableNextColumn();
				if (channel.width > 0 && channel.height > 0)
					ImGui::Text("%ux%u", channel.width, channel.height);
				else
					ImGui::TextDisabled("-");
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", channel.quality_status.c_str());
				if (channel.quality_status != "not analysed")
					ImGui::TextDisabled("range %.3f-%.3f | sigma %.3f | unique %u",
						channel.minimum, channel.maximum,
						channel.standard_deviation, channel.unique_values);
				ImGui::TableNextColumn();
				if (channel.rejected)
					ImGui::TextUnformatted("REJECTED");
				else if (channel.ready)
					ImGui::TextUnformatted("ready");
				else
					ImGui::TextDisabled("missing");
			}
			ImGui::EndTable();
		}

		if (selected_record->warning_count > 0)
		{
			ImGui::SeparatorText("Resolver warnings");
			if (ImGui::BeginChild("##material_resolver_warnings", ImVec2(0, 125), true))
			{
				for (const auto& warning : selected_record->warnings)
					ImGui::BulletText("%s", warning.c_str());
			}
			ImGui::EndChild();
		}
	}


	void cont_general_source_debug_commands()
	{
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float width = ImGui::GetContentRegionAvail().x;
		const auto two_button_size = ImVec2((width - spacing) / 2.0f, 0.0f);
		const auto three_button_size = ImVec2((width - spacing * 2.0f) / 3.0f, 0.0f);
		const auto four_button_size = ImVec2((width - spacing * 3.0f) / 4.0f, 0.0f);

		auto run = [](const char* command)
		{
			if (interfaces::get() && interfaces::get()->m_engine) {
				interfaces::get()->m_engine->execute_client_cmd_unrestricted(command);
			}
		};

		ui_subsection("Source Debug Toolkit", "One-click Source and RTX Remix diagnostics. Presets only change console variables or invoke existing xo_* diagnostic commands.", ICON_FA_TERMINAL);

		ImGui::SeparatorText("Automatic diagnostic presets");
		if (ImGui::Button("Runtime Snapshot", three_button_size))
		{
			run("sv_cheats 1; developer 1; con_filter_enable 0; cl_showpos 1; xo_static_scene_status; xo_static_scene_manifest_status; xo_static_scene_pass_status; xo_world_ffp_status");
		}
		TT("Enables basic developer output and prints the main Source capture, static-scene, world-FFP, water and refract bridge status reports.");
		ImGui::SameLine();
		if (ImGui::Button("Lighting Analysis", three_button_size))
		{
			run("sv_cheats 1; developer 1; r_dynamic 1; mat_fullbright 2; mat_specular 0; mat_bumpmap 0; cl_showpos 1");
		}
		TT("Shows the baked/lightmap contribution with specular and normal mapping disabled. Use Reset Source Debug when finished.");
		ImGui::SameLine();
		if (ImGui::Button("Geometry / PVS", three_button_size))
		{
			run("sv_cheats 1; developer 1; mat_wireframe 2; r_drawothermodels 2; r_lockpvs 1; mat_leafvis 1; cl_showpos 1");
		}
		TT("Wireframe, model visibility, locked PVS and leaf visualization for tracking capture/culling problems.");

		if (ImGui::Button("Material Channels", three_button_size))
		{
			run("sv_cheats 1; developer 1; mat_fullbright 0; mat_specular 0; mat_bumpmap 0; mat_showmiplevels 1; mat_info");
		}
		TT("Isolates base material color, displays mip levels and prints current material-system information.");
		ImGui::SameLine();
		if (ImGui::Button("Event Capture", three_button_size))
		{
			run("developer 1; con_filter_enable 0; xo_debug_sound_print; xo_debug_scene_print");
		}
		TT("Toggles sound-hash and choreography-event console tracing for map light triggers.");
		ImGui::SameLine();
		if (ImGui::Button("Validate Capture", three_button_size))
		{
			run("developer 1; xo_static_scene_validate_passes; xo_static_scene_pass_status; xo_static_scene_manifest_status; xo_static_scene_status; xo_world_ffp_status");
		}
		TT("Forces one static-scene pass validation and immediately prints the resulting coverage and conversion status.");

		ImGui::Spacing(0, 6);
		ImGui::SeparatorText("Overlays and focused probes");
		if (ImGui::Button("RTX API Lights", four_button_size)) run("xo_debug_toggle_show_api_lights");
		TT("Toggles visualization of lights submitted through remixapi.");
		ImGui::SameLine();
		if (ImGui::Button("BSP Nodes / Leafs", four_button_size)) run("xo_debug_toggle_node_vis");
		TT("Toggles the internal BSP node and leaf visualization.");
		ImGui::SameLine();
		if (ImGui::Button("Model IDs", four_button_size)) run("xo_debug_toggle_model_info");
		TT("Toggles nearby model names and radii.");
		ImGui::SameLine();
		if (ImGui::Button("Mesh / Bone IDs", four_button_size)) run("xo_debug_show_mesh_bone_info");
		TT("Shows entity indices and bone information for nearby meshes.");

		if (ImGui::Button("Player Position / Time", four_button_size)) run("xo_debug_toggle_pos_time");
		ImGui::SameLine();
		if (ImGui::Button("Unbake IDs", four_button_size)) run("xo_debug_toggle_unbake_model_info");
		ImGui::SameLine();
		ImGui::SameLine();

		ImGui::Spacing(0, 6);
		ImGui::SeparatorText("Individual Source views");
		if (ImGui::Button("Wireframe", four_button_size)) run("sv_cheats 1; mat_wireframe 2");
		ImGui::SameLine();
		if (ImGui::Button("Lightmaps", four_button_size)) run("sv_cheats 1; mat_fullbright 2");
		ImGui::SameLine();
		if (ImGui::Button("Surface Normals", four_button_size)) run("sv_cheats 1; mat_normals 1");
		ImGui::SameLine();
		if (ImGui::Button("Luxels", four_button_size)) run("sv_cheats 1; mat_luxels 1");

		if (ImGui::Button("Mip Levels", four_button_size)) run("sv_cheats 1; mat_showmiplevels 1");
		ImGui::SameLine();
		if (ImGui::Button("Lock PVS", four_button_size)) run("sv_cheats 1; r_lockpvs 1; mat_leafvis 1");
		ImGui::SameLine();
		if (ImGui::Button("Fullbright", four_button_size)) run("sv_cheats 1; mat_fullbright 1");
		ImGui::SameLine();
		if (ImGui::Button("Clear Decals", four_button_size)) run("r_cleardecals");

		ImGui::Spacing(0, 6);
		if (ImGui::Button("Reset Source Debug Views", two_button_size))
		{
			run("developer 0; con_filter_enable 0; cl_showpos 0; net_graph 0; mat_wireframe 0; r_drawothermodels 1; r_lockpvs 0; mat_leafvis -1; mat_fullbright 0; mat_specular 1; mat_bumpmap 1; mat_normals 0; mat_luxels 0; mat_showmiplevels 0");
		}
		TT("Restores the Source rendering/debug cvars changed by the presets. Internal xo_* overlays are toggles and can be disabled with their matching buttons.");
		ImGui::SameLine();
		if (ImGui::Button("Reload Source + Map Settings", two_button_size))
		{
			run("xo_gamesettings_update; xo_mapsettings_update; xo_vars_parse_options");
		}
		TT("Reloads game_settings.toml, map_settings.toml/map.conf and rtx.conf runtime options.");

		static char custom_source_command[512] = {};
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110.0f);
		ImGui::InputTextWithHint("##custom_source_debug_command", "custom Source command or semicolon-separated command chain", custom_source_command, IM_ARRAYSIZE(custom_source_command));
		ImGui::SameLine();
		ImGui::BeginDisabled(custom_source_command[0] == '\0');
		if (ImGui::Button("Execute", ImVec2(100.0f, 0.0f))) run(custom_source_command);
		ImGui::EndDisabled();
	}


		void cont_experimental_static_map_baking()
		{
			auto state = static_scene_cache::snapshot();
			const auto action_status = static_scene_cache::get_ui_action_status();
			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			const float width = ImGui::GetContentRegionAvail().x;
			const auto two_button_size = ImVec2((width - spacing) / 2.0f, 32.0f);
			const auto three_button_size = ImVec2((width - spacing * 2.0f) / 3.0f, 38.0f);
			const auto four_button_size = ImVec2((width - spacing * 3.0f) / 4.0f, 30.0f);

			auto phase_text = [](const static_scene_cache::phase value)
			{
				switch (value)
				{
				case static_scene_cache::phase::disabled: return "Disabled";
				case static_scene_cache::phase::warmup: return "Warmup";
				case static_scene_cache::phase::capturing: return "Capturing";
				case static_scene_cache::phase::resident: return "Resident";
				default: return "Unknown";
				}
			};

			ui_warning_banner("Experimental static-map baking V20.8",
				"Captures stable Source draws into persistent RTX Remix instances and now learns a per-map manifest. The manifest contains signatures and completeness data only; it does not serialize GPU geometry or replace a real BSP-to-USD exporter.",
				ImVec4(1.0f, 0.46f, 0.18f, 1.0f));

			if (action_status.runtime_quarantine)
			{
				ui_warning_banner("V21.3 runtime safety quarantine",
					"Operations that rebuild capture state, switch baker profiles, mutate live manifests, reload the map or install DrawWorldLists hooks are disabled. They were confirmed to crash the running game even when deferred. Status printing, audit export and configuration-only toggles remain available.",
					ImVec4(0.90f, 0.28f, 0.20f, 1.0f));
			}

			if (action_status.pending)
			{
				ui_warning_banner("Baker action queued",
					"The requested operation will execute at the beginning of the next RenderView, outside the ImGui callback. Additional baker controls are temporarily locked.",
					ImVec4(0.28f, 0.64f, 1.0f, 1.0f));
			}
			ImGui::TextDisabled("Deferred actions: pending %u | queued %llu | executed %llu | failed %llu | blocked %llu",
				action_status.pending_count,
				static_cast<unsigned long long>(action_status.queued),
				static_cast<unsigned long long>(action_status.executed),
				static_cast<unsigned long long>(action_status.failed),
				static_cast<unsigned long long>(action_status.blocked));
			ImGui::TextDisabled("Last: %s - %s", action_status.last_action.c_str(), action_status.last_result.c_str());

			ImGui::BeginDisabled(action_status.pending);

			bool acknowledged = state.acknowledged;
			if (ImGui::Checkbox("I understand that Static Map Baking is experimental and session-only", &acknowledged))
			{
				static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::set_acknowledged, acknowledged);
			}
			TT("The acknowledgement is never saved. Persistent manifests may remain on disk, but they cannot activate baking by themselves.");

			ImGui::Spacing(0, 6);
			ui_subsection("Runtime state", "Capture phase, quality, completeness and resident-scene counters.", ICON_FA_DATABASE);
			if (ImGui::BeginTable("##static_bake_runtime_v208", 5,
				ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableSetupColumn("Map");
				ImGui::TableSetupColumn("Profile");
				ImGui::TableSetupColumn("Phase");
				ImGui::TableSetupColumn("Quality");
				ImGui::TableSetupColumn("Submission");
				ImGui::TableHeadersRow();
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", state.map_name.empty() ? "(no map)" : state.map_name.c_str());
				ImGui::TextDisabled("%u nodes / %u leafs", state.bsp_nodes, state.bsp_leafs);
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", static_scene_cache::experimental_profile_name(state.active_profile));
				if (state.reload_required)
					ImGui::TextDisabled("queued: %s", static_scene_cache::experimental_profile_name(state.requested_profile));
				else if (state.requested_enabled && !state.enabled)
					ImGui::TextDisabled("armed for next map");
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(phase_text(state.current));
				ImGui::TextDisabled("age %u | quiet %u", state.phase_age, state.quiet_frames);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(static_scene_cache::cache_quality_name(state.quality));
				ImGui::TextDisabled("%.1f%% complete", state.completeness * 100.0f);
				ImGui::TableNextColumn();
				ImGui::Text("%llu resident", static_cast<unsigned long long>(state.submitted));
				ImGui::TextDisabled("%llu skipped", static_cast<unsigned long long>(state.skipped));
				ImGui::EndTable();
			}

			char phase_progress_label[96] = {};
			snprintf(phase_progress_label, sizeof(phase_progress_label), "%s - %.0f%%",
				phase_text(state.current), state.progress * 100.0f);
			ImGui::ProgressBar(state.progress, ImVec2(-1.0f, 21.0f), phase_progress_label);
			char completeness_label[96] = {};
			snprintf(completeness_label, sizeof(completeness_label), "Bake completeness - %.1f%%",
				state.completeness * 100.0f);
			ImGui::ProgressBar(state.completeness, ImVec2(-1.0f, 21.0f), completeness_label);
			ImGui::TextDisabled("Records %llu | stable %llu | expired %llu | candidates %llu | rejected dynamic %llu | rejected material %llu",
				static_cast<unsigned long long>(state.records),
				static_cast<unsigned long long>(state.stable_records),
				static_cast<unsigned long long>(state.expired_records),
				static_cast<unsigned long long>(state.candidates),
				static_cast<unsigned long long>(state.rejected_dynamic),
				static_cast<unsigned long long>(state.rejected_material));

			if (state.capture_degraded)
			{
				ui_warning_banner("Degraded capture",
					"The capture reached its extended limit without enough stable or previously expected draws. Resident mode remains usable, but the old manifest is protected from automatic replacement.");
			}
			if (state.reload_required)
			{
				ui_warning_banner("Map reload required",
					"The queued profile or disable action cannot replace submitted persistent instances in place. Reload or change the map to apply it safely.");
			}

			ui_subsection("Capture profiles", "Profile changes are quarantined while a map is running.", ICON_FA_LAYER_GROUP);
			const bool profile_actions_available = static_scene_cache::ui_action_runtime_available(static_scene_cache::ui_action::activate_safe_preview);
			ImGui::BeginDisabled(!state.acknowledged || !profile_actions_available);
			if (ImGui::Button("Safe Preview", three_button_size))
			{
				static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::activate_safe_preview);
			}
			TT("Normal Source visibility, worldspawn only. Best first test and best profile for learning a conservative manifest.");
			ImGui::SameLine();
			if (ImGui::Button("Full BSP Capture", three_button_size))
			{
				static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::activate_full_bsp);
			}
			TT("All-BSP visibility sweep plus 3D sky fusion. Cached signatures are validated against the exact profile.");
			ImGui::SameLine();
			if (ImGui::Button("Full Scene Research", three_button_size))
			{
				static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::activate_full_scene);
			}
			TT("Adds experimental static-prop classification. Unknown draws require more consecutive observations than cached draws.");
			ImGui::EndDisabled();
			if (!profile_actions_available)
				ImGui::TextDisabled("Profile activation is disabled by the V21.3 runtime quarantine.");

			ui_subsection("Persistent bake manifest", "Learns stable signatures per map/profile and checks whether the next capture is complete.", ICON_FA_SAVE);
			bool auto_save = state.manifest_auto_save;
			if (ImGui::Checkbox("Auto-save validated Resident manifest and clean unload", &auto_save))
			{
				static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::set_manifest_auto_save, auto_save);
			}
			bool warm_start = state.manifest_warm_start;
			if (ImGui::Checkbox("Use validated manifest for warm-start confidence", &warm_start))
			{
				static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::set_manifest_warm_start, warm_start);
			}
			TT("Cached draws still require consecutive observations and are resubmitted through D3D9. Warm start never restores raw geometry from disk.");

			if (ImGui::BeginTable("##static_bake_manifest_v208", 5,
				ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableSetupColumn("State");
				ImGui::TableSetupColumn("Expected");
				ImGui::TableSetupColumn("Matched");
				ImGui::TableSetupColumn("Missing");
				ImGui::TableSetupColumn("New");
				ImGui::TableHeadersRow();
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::TextWrapped("%s", state.manifest_status.c_str());
				ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(state.manifest_expected));
				ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(state.manifest_matched));
				ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(state.manifest_missing));
				ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(state.manifest_new));
				ImGui::EndTable();
			}
			if (state.manifest_valid)
			{
				char cache_label[96] = {};
				snprintf(cache_label, sizeof(cache_label), "Manifest coverage - %.1f%%", state.manifest_coverage * 100.0f);
				ImGui::ProgressBar(state.manifest_coverage, ImVec2(-1.0f, 20.0f), cache_label);
			}
			ImGui::TextDisabled("%s", state.manifest_path.empty() ? "No manifest path" : state.manifest_path.c_str());

			const bool manifest_mutation_available = static_scene_cache::ui_action_runtime_available(static_scene_cache::ui_action::save_manifest);
			ImGui::BeginDisabled(!manifest_mutation_available || !state.acknowledged || state.submitted == 0u || state.current != static_scene_cache::phase::resident);
			if (ImGui::Button("Save manifest", four_button_size)) static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::save_manifest);
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!manifest_mutation_available);
			if (ImGui::Button("Reload manifest", four_button_size)) static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::reload_manifest);
			ImGui::SameLine();
			if (ImGui::Button("Clear manifest", four_button_size)) static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::clear_manifest);
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Export audit", four_button_size)) static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::export_audit);
			TT("The audit lists missing and newly discovered stable signatures under l4d2-rtx/logs/static_scene.");
			if (!manifest_mutation_available)
				ImGui::TextDisabled("Save, reload and clear are disabled during runtime. Export audit remains available.");

			ui_subsection("Session actions", "Runtime-mutating session actions are quarantined for stability.", ICON_FA_SYNC_ALT);
			const bool session_actions_available = static_scene_cache::ui_action_runtime_available(static_scene_cache::ui_action::rebuild_capture);
			ImGui::BeginDisabled(!session_actions_available || !state.acknowledged || state.persistent_submission);
			if (ImGui::Button("Rebuild capture now", two_button_size))
			{
				static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::rebuild_capture);
			}
			ImGui::EndDisabled();
			TT("Clears runtime records but keeps the matching disk manifest. Blocked after persistent submission.");
			ImGui::SameLine();
			ImGui::BeginDisabled(!session_actions_available);
			if (ImGui::Button("Reload current map", two_button_size)) static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::reload_current_map);
			ImGui::EndDisabled();

			ImGui::BeginDisabled(!session_actions_available || !state.acknowledged);
			if (ImGui::Button("Disable experimental baking", two_button_size))
			{
				static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::disable_experimental);
			}
			ImGui::SameLine();
			if (ImGui::Button("Reset experimental defaults", two_button_size))
			{
				static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::reset_defaults);
			}
			ImGui::EndDisabled();
			if (!session_actions_available)
				ImGui::TextDisabled("Rebuild, reload, disable and reset are blocked by runtime safety quarantine.");

			if (ImGui::CollapsingHeader("Advanced resident-pass research", ImGuiTreeNodeFlags_None))
			{
				ImGui::TextWrapped("Pass bypass remains independent. A manifest may prove draw completeness, but a DrawWorldLists pass must still establish its own runtime fingerprint before suppression.");
				const bool resident_ready = state.acknowledged && state.current == static_scene_cache::phase::resident;
				const bool pass_actions_available = static_scene_cache::ui_action_runtime_available(static_scene_cache::ui_action::install_pass_hook);
				ImGui::BeginDisabled(!resident_ready || !pass_actions_available);
				if (ImGui::Button("Install pass hook", three_button_size)) static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::install_pass_hook);
				ImGui::SameLine();
				if (ImGui::Button(state.world_pass_bypass ? "Disable main bypass" : "Enable main bypass", three_button_size))
					static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::toggle_world_pass_bypass);
				ImGui::SameLine();
				ImGui::BeginDisabled(!state.sky3d_fusion);
				if (ImGui::Button(state.sky_pass_bypass ? "Disable Sky3D bypass" : "Enable Sky3D bypass", three_button_size))
					static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::toggle_sky_pass_bypass);
				ImGui::EndDisabled();
				if (ImGui::Button("Validate covered passes", two_button_size)) static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::validate_world_passes);
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("Print bake + cache status", two_button_size))
				{
					static_scene_cache::enqueue_ui_action(static_scene_cache::ui_action::print_status);
				}
				if (!pass_actions_available)
					ImGui::TextDisabled("Hook installation, bypass toggles and pass validation are disabled. Status printing remains available.");
			}

			ui_subsection("Active profile flags", "Live runtime policy and pass coverage.", ICON_FA_CHECK_CIRCLE);
			ImGui::BulletText("All-BSP visibility capture: %s", state.full_visibility_capture ? "ON" : "off");
			ImGui::BulletText("3D sky fusion: %s", state.sky3d_fusion ? "ON" : "off");
			ImGui::BulletText("Static-prop classifier: %s", state.model_info_classifier ? "ON" : "off");
			ImGui::BulletText("DrawWorldLists hook: %s", state.renderer_hook_installed ? "installed" : "not installed");
			ImGui::BulletText("Main/Sky bypass: %s / %s", state.world_pass_bypass ? "ON" : "off", state.sky_pass_bypass ? "ON" : "off");
			if (state.xorxor_water_quarantine)
			{
				ImGui::BulletText("Xorxor water quarantine: ACTIVE");
				ImGui::TextDisabled("Pass-level static bypass is disabled so both dynamic water layers remain visible.");
			}
			ImGui::TextDisabled("World passes %llu | covered %llu | pass coverage %.1f%% | bypassed %llu | estimated DIPs avoided %llu | sky candidates %llu | model candidates %llu",
				static_cast<unsigned long long>(state.world_passes),
				static_cast<unsigned long long>(state.covered_world_passes),
				state.world_pass_coverage * 100.0f,
				static_cast<unsigned long long>(state.world_pass_skips),
				static_cast<unsigned long long>(state.estimated_draws_avoided),
				static_cast<unsigned long long>(state.sky_candidates),
				static_cast<unsigned long long>(state.model_candidates));


			ImGui::EndDisabled();
		}

	} // namespace

	void imgui::tab_general()
	{
		ui_subsection("Dev Quick Tools", "A flat operational workspace. Each function has one owner tab; Dev only keeps map navigation and repeated gameplay actions.", ICON_FA_BOLT);

		if (ImGui::BeginTabBar("##dev_quick_tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll))
		{
			if (ImGui::BeginTabItem(ICON_FA_MAP "  Maps"))
			{
				cont_general_map_quick_access();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(ICON_FA_RUNNING "  Movement / Director"))
			{
				cont_general_gameplay_quick_access();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		ImGui::Spacing(0, 6.0f);
		ImGui::TextDisabled("Spawning and weapon tools: General. Source/event inspection: Diagnostics. Lights, materials, markers and performance remain in their dedicated tabs.");
	}

	void imgui::tab_experimental()
	{
		cont_experimental_static_map_baking();
	}

	void imgui::tab_import_lights_from_maps()
	{
		cont_general_import_lights_from_maps();
	}

	void imgui::tab_import_materials_from_maps()
	{
		cont_general_import_materials_from_maps();
	}


	// #
	// #

	void cont_mapsettings_general()
	{
		auto& ms = map_settings::get_map_settings();

		ImGui::PushFont(common::imgui::font::BOLD);
		if (ImGui::Button("Reload rtx.conf    " ICON_FA_REDO, ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0)))
		{
			if (!ImGui::IsPopupOpen("Reload RtxConf?")) {
				ImGui::OpenPopup("Reload RtxConf?");
			}
		} ImGui::PopFont();

		// popup
		if (ImGui::BeginPopupModal("Reload RtxConf?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			common::imgui::draw_background_blur();
			ImGui::Spacing(0.0f, 0.0f);

			const auto half_width = ImGui::GetContentRegionMax().x * 0.5f;
			auto line1_str = "This will reload the rtx.conf file and re-apply all of it's variables.  ";
			auto line3_str = "(excluding texture hashes)";

			ImGui::Spacing();
			ImGui::SetCursorPosX(5.0f + half_width - (ImGui::CalcTextSize(line1_str).x * 0.5f));
			ImGui::TextUnformatted(line1_str);

			ImGui::PushFont(common::imgui::font::BOLD);
			ImGui::SetCursorPosX(5.0f + half_width - (ImGui::CalcTextSize(line3_str).x * 0.5f));
			ImGui::TextUnformatted(line3_str);
			ImGui::PopFont();

			ImGui::Spacing(0, 8);
			ImGui::Spacing(0, 0); ImGui::SameLine();

			ImVec2 button_size(half_width - 6.0f - ImGui::GetStyle().WindowPadding.x, 0.0f);
			if (ImGui::Button("Reload", button_size))
			{
				remix_vars::xo_vars_parse_options_fn();
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine(0, 6.0f);
			if (ImGui::Button("Cancel", button_size)) {
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}

		ImGui::SameLine();
		reload_mapsettings_button_with_popup("General");




		const auto two_row_button_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 1) / 2.0f, 0);

		ImGui::SeparatorTextLarge(" Debug Views / Info ", true);

		imgui::toggle_button_bool(&cmd::debug_node_vis, "Area / Leaf Info", two_row_button_size, "Toggle bsp node/leaf debug visualization using the remix api\n~~ cmd: xo_debug_toggle_node_vis");
		ImGui::SameLine();
		imgui::toggle_button_bool(&cmd::model_info_vis, "Static Prop Info", two_row_button_size, "Toggle model name and radius visualizations\nUseful for HIDEMODEL (MapSettings)\n~~ cmd: xo_debug_toggle_model_info");


		imgui::toggle_button_bool(&cmd::scene_print, "Choreo: Print Info to Console", two_row_button_size, "This will print info about all choreographies to the console\nUseful for MARKER/LIGHTS (MapSettings)\n~~ cmd: xo_debug_scene_print");
		ImGui::SameLine();
		imgui::toggle_button_bool(&cmd::sound_debug_printing, "Sound: Print Info to Console", two_row_button_size, "This will print info about running sounds to the console\nUseful for MARKER/LIGHTS (MapSettings)\n~~ cmd: xo_debug_sound_print");

		imgui::toggle_button_bool(&cmd::unbake_model_info_vis, "Unbake: Prop Info Visualization", two_row_button_size, "Toggle model unbake info showing checksums, names and bone number visualizations\nUseful for UNBAKE (MapSettings)\n~~ cmd: xo_debug_toggle_unbake_model_info");
		ImGui::SameLine();
		{
			bool temp_unbake = false;
			if (imgui::toggle_button_bool(&temp_unbake, "Unbake: Log Info to File", two_row_button_size, "Log unbake info for all loaded meshes to \"l4d2-rtx\\logs\\mapsettings_unbake_info.log\"\nUseful for UNBAKE (MapSettings)\n~~ cmd: xo_mapsettings_get_unbake_info")) {
				cmd::ms_unbake_info = temp_unbake;
			}
		}

		ImGui::Spacing(0, 4);

		SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
		ImGui::SliderInt2("HUD: Area Debug Pos", &main_module::get()->m_hud_debug_node_vis_pos[0], 0, 512);

		ImGui::Spacing(0, 4);

		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
		ImGui::TableHeaderDropshadow();
		const bool water_header_state = ImGui::CollapsingHeader("Water Settings");
		ImGui::PopStyleVar();

		if (water_header_state)
		{
			SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
			if (ImGui::DragFloat("UV Scale##Water", &ms.water_uv_scale, 0.05f, 0.01f, FLT_MAX, "%.2f")) {
				ms.water_uv_scale = std::clamp(ms.water_uv_scale, 0.0f, FLT_MAX);
			}

			SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
			ImGui::DragFloat("Top Layer Offset", &ms.water_offset_top, 0.05f, -100.0f, 100.0f, "%.2f");
			TT("This can offset the dual rendered water mesh along the Z-Axis (usually the animated surface)");

			SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
			ImGui::DragFloat("Bottom Layer Offset", &ms.water_offset_bottom, 0.05f, -100.0f, 100.0f, "%.2f");
			TT("This can offset the original water mesh along the Z-Axis (usually the surface defining water color)");

			ImGui::Spacing();
			const auto water_status = xorxor_water::snapshot();
			ImGui::SeparatorText("Xorxor Water Ownership");
			ImGui::Text("Detected passes: %llu", static_cast<unsigned long long>(water_status.water_shader_passes));
			ImGui::Text("Dual-layer conversions: %llu", static_cast<unsigned long long>(water_status.converted_dual_draws));
			ImGui::Text("Hidden beneath passes: %llu", static_cast<unsigned long long>(water_status.hidden_beneath_passes));
			ImGui::Text("Unsupported vertex formats: %llu", static_cast<unsigned long long>(water_status.unsupported_vertex_formats));
			ImGui::Text("Stable-hash bypasses: %llu", static_cast<unsigned long long>(water_status.stable_hash_bypasses));
			ImGui::Text("Static-cache bypasses: %llu", static_cast<unsigned long long>(water_status.static_cache_bypasses));
			ImGui::Text("Missing surface textures: %llu", static_cast<unsigned long long>(water_status.missing_surface_textures));
			ImGui::Text("Map cache quarantine: %s", static_scene_cache::xorxor_water_quarantined() ? "ACTIVE" : "not detected");
			if (!water_status.last_material.empty())
			{
				ImGui::TextWrapped("Last material: %s", water_status.last_material.c_str());
				ImGui::Text("Shader: %s | VF: 0x%X", water_status.last_shader.c_str(), water_status.last_vertex_format);
			}
			TT("The original Xorxor dual-draw path owns Source Water. It bypasses Source Material Bridge hashes, Auto PBR synthesis and experimental static-scene capture.");
		}

#if DEBUG
		{
			const auto im = imgui::get();

			if (ImGui::CollapsingHeader("DEBUG Build Section", ImGuiTreeNodeFlags_SpanFullWidth))
			{
				SET_CHILD_WIDGET_WIDTH; ImGui::Checkbox("Disable R_CullNode", &im->m_disable_cullnode);
				SET_CHILD_WIDGET_WIDTH; ImGui::Checkbox("Enable Area Forcing", &im->m_enable_area_forcing);
				SET_CHILD_WIDGET_WIDTH; ImGui::Checkbox("Disable Unbaking", &im->m_debug_disable_unbake);
				SET_CHILD_WIDGET_WIDTH; ImGui::Checkbox("Unbake All Single Bone Meshes", &im->m_debug_unbake_all_single_bones);

				SET_CHILD_WIDGET_WIDTH; ImGui::DragFloat4("Debug Float Vec", im->m_debug_float_vec4, 0.05f);
				SET_CHILD_WIDGET_WIDTH; ImGui::DragInt4("Debug Int Vec", im->m_debug_int_vec4, 0.05f);

				const auto coloredit_flags = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_Float;

				SET_CHILD_WIDGET_WIDTH; ImGui::ColorEdit4("ContainerBg", &im->ImGuiCol_ContainerBackground.x, coloredit_flags);
				SET_CHILD_WIDGET_WIDTH; ImGui::ColorEdit4("ContainerBorder", &im->ImGuiCol_ContainerBorder.x, coloredit_flags);

				SET_CHILD_WIDGET_WIDTH; ImGui::ColorEdit4("ButtonGreen", &im->ImGuiCol_ButtonGreen.x, coloredit_flags);
				SET_CHILD_WIDGET_WIDTH; ImGui::ColorEdit4("ButtonYellow", &im->ImGuiCol_ButtonYellow.x, coloredit_flags);
				SET_CHILD_WIDGET_WIDTH; ImGui::ColorEdit4("ButtonRed", &im->ImGuiCol_ButtonRed.x, coloredit_flags);

				const auto glob = interfaces::get()->m_globals;
				ImGui::Text("Realtime: %.4f", glob->realtime);
				ImGui::Text("Curtime Abs: %.4f", glob->curtime);
				ImGui::Text("MaxClients: %.4f", glob->maxClients);
				ImGui::Text("Frametime Abs: %.4f", glob->absoluteframetime);
				ImGui::Text("Frametime: %.4f", glob->frametime);
			}
		}
#endif
	}

	void cont_mapsettings_fog()
	{
		auto& ms = map_settings::get_map_settings();
		bool fog_enabled = ms.fog_dist != 0.0f || ms.fog_density != 0.0f;

		Vector fog_color = {};
		fog_color.x = static_cast<float>((ms.fog_color >> 16) & 0xFF) / 255.0f * 1.0f;
		fog_color.y = static_cast<float>((ms.fog_color >>  8) & 0xFF) / 255.0f * 1.0f;
		fog_color.z = static_cast<float>((ms.fog_color >>  0) & 0xFF) / 255.0f * 1.0f;

		ImGui::BeginDisabled(!fog_enabled);
		ImGui::PushFont(common::imgui::font::BOLD);
		if (ImGui::Button("Copy Settings to Clipboard   " ICON_FA_SAVE "##Fog", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0)))
		{
			std::string toml_str = ms.mapname + " = { "s;
			if (ms.fog_dist > 0.0f) {
				toml_str += "distance = " + std::to_string(ms.fog_dist) + ", "s;
			}
			else {
				toml_str += "density = " + std::to_string(ms.fog_density) + ", "s;
			}

			toml_str += "color = ["
				+ std::to_string(static_cast<int>(std::round(fog_color.x * 255.0f))) + ", "
				+ std::to_string(static_cast<int>(std::round(fog_color.y * 255.0f))) + ", "
				+ std::to_string(static_cast<int>(std::round(fog_color.z * 255.0f))) + "] }"s;

			ImGui::LogToClipboard();
			ImGui::LogText("%s", toml_str.c_str());
			ImGui::LogFinish();
		} ImGui::PopFont();
		ImGui::EndDisabled();

		ImGui::SameLine();
		reload_mapsettings_button_with_popup("Fog");

		ImGui::Spacing();
		ImGui::Spacing();

		/*static float old_fog_val = 0.0f;
		SET_CHILD_WIDGET_WIDTH;
		if (ImGui::Checkbox("Enable Fog", &fog_enabled))
		{
			if (!fog_enabled)
			{
				old_fog_val = ms.fog_dist;
				ms.fog_dist = 0.0f;
			}
			else
			{
				ms.fog_dist = old_fog_val > 0.0f ? old_fog_val : 15000.0f;
				if (ms.fog_color == 0xFFFFFFFF) {
					ms.fog_color = 0xFF646464;
				}
			}
		}*/

		SET_CHILD_WIDGET_WIDTH;
		if (ImGui::DragFloat("Distance", &ms.fog_dist, 1.0f, 1000.1f))
		{
			ms.fog_dist = ms.fog_dist < 1000.1f ? 1000.1f : ms.fog_dist;
			ms.fog_density = 0.0f;
		}

		SET_CHILD_WIDGET_WIDTH;
		if (ImGui::DragFloat("Density", &ms.fog_density, 0.00001f, 0.0f, 1.0f, "%.5f"))
		{
			ms.fog_density = std::clamp(ms.fog_density, 0.0f, 1.0f);
			ms.fog_dist = 0.0f;
		}

		SET_CHILD_WIDGET_WIDTH;
		if (ImGui::ColorEdit3("Transmission", &fog_color.x, ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_PickerHueBar)) {
			ms.fog_color = D3DCOLOR_COLORVALUE(fog_color.x, fog_color.y, fog_color.z, 1.0f);
		}
	}

	void cont_mapsettings_marker_manipulation()
	{
		auto& markers = map_settings::get_map_settings().map_markers;
		ImGui::PushFont(common::imgui::font::BOLD);
		const auto marker_top_button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
		if (ImGui::Button("Copy All Markers   " ICON_FA_COPY, ImVec2(marker_top_button_width, 0)))
		{
			ImGui::LogToClipboard();
			ImGui::LogText("%s", common::toml::build_map_marker_string_for_current_map(markers).c_str());
			ImGui::LogFinish();
		}
		ImGui::SameLine();
		if (ImGui::Button("Append Markers Export   " ICON_FA_SAVE, ImVec2(marker_top_button_width, 0)))
		{
			const auto toml_str = common::toml::build_map_marker_string_for_current_map(markers);
			if (append_to_mapsettings_workbench_export("Marker", "all current markers", "Paste into [map_markers] for this map in map_settings.toml.", toml_str))
			{
				game::console();
				std::cout << "[MapSettingsWorkbench] Exported markers to: " << mapsettings_workbench_export_path() << std::endl;
			}
		}
		ImGui::PopFont();

		ImGui::SameLine();
		reload_mapsettings_button_with_popup("MapMarker");
		//ImGui::Spacing(0, 4);

		constexpr auto in_buflen = 1024u;
		static char in_area_buf[in_buflen], in_nleaf_buf[in_buflen];
		static map_settings::marker_settings_s* selection = nullptr;

		//
		// MARKER TABLE

		static char marker_filter[128] = {};
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		ImGui::InputTextWithHint("##MarkerFilter", "Filter markers by name, comment or type/index...", marker_filter, IM_ARRAYSIZE(marker_filter));
		const auto marker_filter_lower = utils::str_to_lower(marker_filter);

		ImGui::TableHeaderDropshadow();
		if (ImGui::BeginTable("MarkerTable", 13,
			ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ContextMenuInBody |
			ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_ScrollY, ImVec2(0, 380)))
		{
			ImGui::TableSetupScrollFreeze(0, 1); // make top row always visible
			ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoHide, 12.0f);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_NoResize, 34.0f);
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 100.0f);
			ImGui::TableSetupColumn("Vis", ImGuiTableColumnFlags_NoResize, 28.0f);
			ImGui::TableSetupColumn("NC", ImGuiTableColumnFlags_NoResize, 24.0f);
			ImGui::TableSetupColumn("Trig", ImGuiTableColumnFlags_NoResize, 24.0f);
			ImGui::TableSetupColumn("Areas", ImGuiTableColumnFlags_WidthStretch, 80.0f);
			ImGui::TableSetupColumn("NLeafs", ImGuiTableColumnFlags_WidthStretch, 80.0f);
			ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch, 200.0f);
			ImGui::TableSetupColumn("Pos", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 200.0f);
			ImGui::TableSetupColumn("Rot", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 180.0f);
			ImGui::TableSetupColumn("Scale", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 130.0f);
			ImGui::TableSetupColumn("##Delete", ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoReorder | ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoClip, 16.0f);
			ImGui::TableHeadersRow();

			bool selection_matches_any_entry = false;
			map_settings::marker_settings_s* marked_for_deletion = nullptr;

			for (auto i = 0u; i < markers.size(); i++)
			{
				auto& m = markers[i];
				if (!marker_filter_lower.empty())
				{
					const auto searchable = utils::str_to_lower(std::format("{} {} {}", m.index, m.name, m.comment));
					if (searchable.find(marker_filter_lower) == std::string::npos) continue;
				}

				// default selection
				if (!selection) {
					selection = &m;
				}

				ImGui::TableNextRow();

				// save Y offset
				const auto save_row_min_y_pos = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().FramePadding.y + ImGui::GetStyle().CellPadding.y;

				// handle row background color for selected entry
				const bool is_selected = selection && selection == &m;
				if (is_selected) {
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));
				}

				// -
				ImGui::TableNextColumn();
				if (!is_selected) // only selectable if not selected
				{
					ImGui::Style_InvisibleSelectorPush(); // never show selection - we use tablebg
					if (ImGui::Selectable(utils::va("%d", static_cast<int>(i)), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, 22 + ImGui::GetStyle().CellPadding.y * 1.0f))) {
						selection = &m;
					}
					ImGui::Style_InvisibleSelectorPop();

					if (ImGui::IsItemHovered()) {
						ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.6f)));///*ImGui::GetColorU32(ImGuiCol_TableRowBgAlt)*/);
					}
				}
				else {
					ImGui::Text("%d", static_cast<int>(i)); // if selected
				}

				if (selection && selection == &m) {
					selection_matches_any_entry = true; // check that the selection ptr is up to date
				}

				// - marker num
				ImGui::TableNextColumn();
				ImGui::Text("%d", m.index);

				// - marker name / authored visibility
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", m.name.empty() ? "-" : m.name.c_str());

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(m.visible ? "x" : "");

				// - nocull
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(m.no_cull ? "x" : "");

				// - trig
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(m.trigger_show.has_trigger() || m.trigger_hide.has_trigger() ? "x" : "");

				// - area Input
				ImGui::TableNextColumn();

				if (is_selected) {
					ImGui::Widget_UnorderedSetModifier("MarkerArea", ImGui::Widget_UnorderedSetModifierFlags_Area, selection->areas, in_area_buf, in_buflen);
				}

				ImGui::Spacing();
				ImGui::TextWrapped_IntegersFromUnorderedSet(m.areas);
				ImGui::Spacing();

				// - not in leaf input
				ImGui::TableNextColumn();
				if (is_selected) {
					ImGui::Widget_UnorderedSetModifier("MarkerNLeafs", ImGui::Widget_UnorderedSetModifierFlags_Leaf, selection->when_not_in_leafs, in_nleaf_buf, in_buflen);
				}

				ImGui::Spacing();
				ImGui::TextWrapped_IntegersFromUnorderedSet(m.when_not_in_leafs);
				ImGui::Spacing();

				// - comment
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", m.comment.c_str());

				const auto row_max_y_pos = ImGui::GetItemRectMax().y;

				// - pos
				ImGui::TableNextColumn(); ImGui::Spacing();
				ImGui::Text("%.2f, %.2f, %.2f", m.origin.x, m.origin.y, m.origin.z);

				// - rot
				ImGui::TableNextColumn(); ImGui::Spacing();
				ImGui::Text("%.2f, %.2f, %.2f", m.rotation.x, m.rotation.y, m.rotation.z);

				// - scale
				ImGui::TableNextColumn(); ImGui::Spacing();
				ImGui::Text("%.2f, %.2f, %.2f", m.scale.x, m.scale.y, m.scale.z);

				// delete Button
				ImGui::TableNextColumn();
				{
					ImGui::Style_DeleteButtonPush();
					ImGui::PushID((int)i);

					const auto btn_size = ImVec2(16, is_selected ? (row_max_y_pos - save_row_min_y_pos) : 25.0f);
					if (ImGui::Button("x##Marker", btn_size))
					{
						marked_for_deletion = &m;
						main_module::trigger_vis_logic();
					}

					ImGui::Style_DeleteButtonPop();
					ImGui::PopID();
				}

			} // end for loop

			if (!selection_matches_any_entry)
			{
				for (auto& m : markers)
				{
					if (selection && selection == &m)
					{
						selection_matches_any_entry = true;
						break;
					}
				}

				if (!selection_matches_any_entry) {
					selection = nullptr;
				}
			}
			else if (selection) {
				game::debug_add_text_overlay(&selection->origin.x, "[ImGui] Selected Marker", 0, 0.8f, 1.0f, 0.3f, 0.8f);
			}

			// remove entry
			if (marked_for_deletion)
			{
				for (auto it = markers.begin(); it != markers.end(); ++it)
				{
					if (&*it == marked_for_deletion)
					{
						markers.erase(it);
						selection = nullptr;
						break;
					}
				}
			}
			ImGui::EndTable();
		}

		ImGui::Style_ColorButtonPush(imgui::get()->ImGuiCol_ButtonGreen, true);
		if (ImGui::Button("++ Marker"))
		{
			std::uint32_t free_marker = 0u;
			for (auto i = 0u; i < markers.size(); i++)
			{
				if (markers[i].index == free_marker)
				{
					free_marker++;
					i = 0u; // restart loop
				}
			}

			markers.emplace_back(map_settings::marker_settings_s {
					free_marker, *game::get_current_view_origin() - Vector(0,0,1), true
				});

			selection = &markers.back();
		}
		ImGui::Style_ColorButtonPop();

		if (selection)
		{
			ImGui::SameLine();
			ImGui::Style_ColorButtonPush(imgui::get()->ImGuiCol_ButtonYellow, true);
			if (ImGui::Button("Duplicate Current Marker"))
			{
				markers.emplace_back(map_settings::marker_settings_s{
					.index = selection->index,
					.origin = selection->origin,
					.no_cull = selection->no_cull,
					.rotation = selection->rotation,
					.scale = selection->scale,
					.areas = selection->areas,
					.when_not_in_leafs = selection->when_not_in_leafs,
					.trigger_show = selection->trigger_show,
					.trigger_hide = selection->trigger_hide,
					.trigger_always = selection->trigger_always,
					.comment = selection->comment,
					.name = selection->name,
					.color = selection->color,
					.visible = selection->visible,
					.visibility_range = selection->visibility_range,
					.is_hidden = selection->is_hidden
					});

				selection = &markers.back();
			}
			ImGui::Style_ColorButtonPop();
		}

		ImGui::SameLine();
		ImGui::BeginDisabled(!selection);
		{
			if (ImGui::Button("TP to Marker")) {
				interfaces::get()->m_engine->execute_client_cmd_unrestricted(utils::va("sv_cheats 1; noclip; setpos %.2f %.2f %.2f", selection->origin.x, selection->origin.y, selection->origin.z - 40.0f));
			}

			ImGui::SameLine();
			if (ImGui::Button("TP Marker to Player", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
			{
				selection->origin = *game::get_current_view_origin();
				selection->origin.z -= 1.0f;
			}
			ImGui::EndDisabled();
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Marker Workbench");
		ImGui::TextDisabled("Fast marker presets and safe export helpers for RTX map work.");
		ImGui::Spacing(0, 4);

		static int marker_workbench_preset = 0;
		marker_workbench_preset = std::clamp(marker_workbench_preset, 0, static_cast<int>(IM_ARRAYSIZE(MARKER_WORKBENCH_PRESETS)) - 1);

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
		if (ImGui::BeginCombo("Preset##MarkerWorkbench", MARKER_WORKBENCH_PRESETS[marker_workbench_preset].name))
		{
			for (auto i = 0; i < IM_ARRAYSIZE(MARKER_WORKBENCH_PRESETS); ++i)
			{
				const bool is_selected = marker_workbench_preset == i;
				if (ImGui::Selectable(MARKER_WORKBENCH_PRESETS[i].name, is_selected)) {
					marker_workbench_preset = i;
				}
				if (is_selected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		TT(MARKER_WORKBENCH_PRESETS[marker_workbench_preset].tooltip);

		const auto marker_wb_two_button_width = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f, 0.0f);
		const auto marker_wb_preset = MARKER_WORKBENCH_PRESETS[marker_workbench_preset];

		if (ImGui::Button("++ Preset At Camera", marker_wb_two_button_width))
		{
			markers.emplace_back(make_marker_from_preset(markers, marker_wb_preset));
			selection = &markers.back();
			main_module::trigger_vis_logic();
		}
		TT("Creates a new marker from the selected preset at the current camera/player view origin.");

		ImGui::SameLine();
		ImGui::BeginDisabled(!selection);
		if (ImGui::Button("Apply Preset To Selected", marker_wb_two_button_width) && selection)
		{
			apply_marker_workbench_preset(*selection, marker_wb_preset, false);
			main_module::trigger_vis_logic();
		}
		ImGui::EndDisabled();
		TT("Applies preset scale, NoCull mode and optional area/leaf gating to the selected marker without moving it.");

		ImGui::BeginDisabled(!selection);
		if (ImGui::Button("Selected -> Current Area Gate", marker_wb_two_button_width) && selection)
		{
			selection->no_cull = true;
			selection->areas.clear();
			if (has_valid_current_area()) {
				selection->areas.insert(current_area_u32());
			}
			main_module::trigger_vis_logic();
		}
		TT("Turns the selected marker into a NoCull marker visible only in the current area.");

		ImGui::SameLine();
		if (ImGui::Button("Toggle Current Leaf Exclusion", marker_wb_two_button_width) && selection)
		{
			selection->no_cull = true;
			if (has_valid_current_leaf())
			{
				const auto leaf = current_leaf_u32();
				if (selection->when_not_in_leafs.contains(leaf)) {
					selection->when_not_in_leafs.erase(leaf);
				}
				else {
					selection->when_not_in_leafs.insert(leaf);
				}
			}
			main_module::trigger_vis_logic();
		}
		TT("Toggles the current leaf in N_leafs for the selected marker. Useful for hiding a blocker after crossing a leaf seam.");

		if (ImGui::Button("Copy Selected Marker", marker_wb_two_button_width) && selection)
		{
			copy_text_to_clipboard(build_single_marker_string(*selection));
		}
		TT("Copies only the selected marker as a TOML snippet.");

		ImGui::SameLine();
		if (ImGui::Button("Append Selected Marker Export   " ICON_FA_SAVE, marker_wb_two_button_width) && selection)
		{
			const auto toml_str = build_single_marker_string(*selection);
			const auto label = selection->comment.empty() ? "selected marker" : selection->comment;
			if (append_to_mapsettings_workbench_export("Marker", label, "Paste into [map_markers] for this map in map_settings.toml.", toml_str))
			{
				game::console();
				std::cout << "[MarkerWorkbench] Exported selected marker to: " << mapsettings_workbench_export_path() << std::endl;
			}
		}
		TT("Safe export: appends only the selected marker to l4d2-rtx\\logs\\mapsettings_workbench_export.toml.");
		ImGui::EndDisabled();

		if (ImGui::Button("Copy Current Area/Leaf Snapshot", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
		{
			copy_text_to_clipboard(build_current_area_leaf_snapshot());
		}
		TT("Copies current map, area, leaf and camera origin. Useful when documenting broken culling spots.");

		ImGui::Spacing();
		ImGui::Spacing();

		ImGui::SeparatorText("Modify Marker");

		ImGui::Spacing();
		ImGui::Spacing();

		if (selection)
		{
			int temp_num = (int)selection->index;

			if (!selection->no_cull)
			{
				ImGui::CenterText("- Only 'NoCull' supports live editing - ");
				ImGui::CenterText("- Save and reload MapSettings to see changes - ");
				ImGui::Spacing(0, 6);
			}

			SET_CHILD_WIDGET_WIDTH;
			if (ImGui::DragInt("Marker Type / Index", &temp_num, 0.1f, 0))
			{
				if (temp_num < 0) {
					temp_num = 0;
				}
				else if (!selection->no_cull && temp_num > 99) {
					temp_num = 99;
				}

				selection->index = (std::uint32_t)temp_num;
			}

			//ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.6f, 0.5f));
			ImGui::Widget_PrettyDragVec3("Origin", &selection->origin.x, true, 80.0f, 0.5f,
				-FLT_MAX, FLT_MAX, "X", "Y", "Z");
			//ImGui::PopStyleVar();

			// RAD2DEG -> DEG2RAD
			Vector temp_rot = { RAD2DEG(selection->rotation.x), RAD2DEG(selection->rotation.y), RAD2DEG(selection->rotation.z) };

			ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.6f, 0.5f));
			if (ImGui::Widget_PrettyDragVec3("Rotation", &temp_rot.x, true, 80.0f, 0.1f,
				-360.0f, 360.0f, "Rx", "Ry", "Rz"))
			{
				selection->rotation = { DEG2RAD(temp_rot.x), DEG2RAD(temp_rot.y), DEG2RAD(temp_rot.z) };
			} ImGui::PopStyleVar();

			if (selection->no_cull)
			{
				ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.6f, 0.5f));
				ImGui::Widget_PrettyDragVec3("Scale", &selection->scale.x, true, 80.0f, 0.01f,
					-FLT_MAX, FLT_MAX, "Sx", "Sy", "Sz");
				ImGui::PopStyleVar();
			}

			SET_CHILD_WIDGET_WIDTH;
			ImGui::InputText("Name", &selection->name);
			TT("Human-readable marker name stored per map. The technical marker type remains the numeric index.");

			if (ImGui::Checkbox("Visible", &selection->visible)) main_module::trigger_vis_logic();
			TT("Master authored visibility. Trigger and range state are evaluated independently at runtime.");
			SET_CHILD_WIDGET_WIDTH;
			if (ImGui::DragFloat("Visibility Range", &selection->visibility_range, 10.0f, 0.0f, 20000.0f, "%.0f units", ImGuiSliderFlags_AlwaysClamp)) {
				selection->visibility_range = std::max(0.0f, selection->visibility_range);
			}
			TT("Maximum camera distance in Source units. Set to 0 for unlimited visibility.");

			SET_CHILD_WIDGET_WIDTH;
			ImGui::ColorEdit3("Color", &selection->color.x, ImGuiColorEditFlags_Float);
			TT(selection->no_cull ? "Stored as marker metadata. NoCull marker vertex colors remain index-encoded for RTX identification." : "Applied to the spawned dynamic-prop marker through rendercolor.");

			SET_CHILD_WIDGET_WIDTH;
			ImGui::InputText("Comment", &selection->comment);

			// Trigger section
			{
				auto marker_trigger_settings = [](map_settings::marker_trigger_s& trig, const char* imgui_id)
					{
						ImGui::PushID(imgui_id);

						// --- choreo

						SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
						ImGui::InputText("Choreo Name", &trig.choreo_name);
						TT("Show marker when a specified choreography (vcd) starts playing.\n"
							"Using a show trigger will hide the marker by default until the event triggers logic.\n"
							"This can be a substring. Use cmd 'xo_debug_scene_print' to get info about playing choreo's.");

						// clear sound trigger if choreo is not empty
						if ( !trig.choreo_name.empty() &&
							(!trig.sound_name.empty() || trig.sound_hash))
						{
							trig.sound_name.clear();
							trig.sound_hash = 0u;
						}

						if (!trig.choreo_name.empty())
						{
							SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
							ImGui::InputText("Choreo Actor", &trig.choreo_actor);
							TT("Use this if the choreo name isn't enough to uniquely identify the choreo that should show the marker.\n"
								"This can be a substring. Use cmd 'xo_debug_scene_print' to get info about playing choreo's.");

							SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
							ImGui::InputText("Choreo Event", &trig.choreo_event);
							TT("Use this if the choreo name isn't enough to uniquely identify the choreo that should show the marker.\n"
								"This can be a substring. Use cmd 'xo_debug_scene_print' to get info about playing choreo's.");

							SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
							ImGui::InputText("Choreo Param1", &trig.choreo_param1);
							TT("Use this if the choreo name isn't enough to uniquely identify the choreo that should show the marker.\n"
								"This can be a substring. Use cmd 'xo_debug_scene_print' to get info about playing choreo's.");
						}

						// --- sound

						SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
						std::string temp_sound_hash_str = trig.sound_hash ? std::format("0x{:X}", trig.sound_hash) : "";

						if (ImGui::InputText("Sound Hash", &temp_sound_hash_str, ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue,
							[](ImGuiInputTextCallbackData* data)
							{
								const auto c = static_cast<char>(data->EventChar);
								if (std::isxdigit(c) || c == 'x' || c == 'X') {
									return 0; // allow input
								}
								return 1; // block input
							}))
						{
							trig.sound_hash = static_cast<uint32_t>(std::strtoul(temp_sound_hash_str.c_str(), nullptr, 16));
							temp_sound_hash_str = std::format("0x{:X}", trig.sound_hash);
						}
						TT( "Show marker when a specified sound starts playing.\n"
							"The sound trigger has LOWER precedence than choreo triggering.\n"
							"Use cmd 'xo_debug_toggle_sound_print' to get info about playing sounds.");

						// clear choreo and sound_name if sound_hash is not empty
						if (trig.sound_hash)
						{
							trig.sound_name.clear();
							trig.choreo_name.clear();
						}

						SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
						ImGui::InputText("Sound Name", &trig.sound_name);
						TT("Show marker when a specified sound starts playing.\n"
							"The sound trigger has LOWER precedence than choreo triggering.\n"
							"This can be a substring. Use cmd 'xo_debug_scene_print' to get info about playing choreo's.");

						// clear choreo and sound_has if sound_name is not empty
						if (!trig.sound_name.empty() &&
							(trig.sound_hash || !trig.choreo_name.empty()))
						{
							trig.choreo_name.clear();
							trig.sound_hash = 0u;
						}

						ImGui::BeginDisabled(!trig.has_trigger());
						{
							SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
							if (ImGui::DragFloat("Delay", &trig.delay, 0.05f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
								trig.delay = trig.delay < 0.0f ? 0.0f : trig.delay;
							} TT("Delay show after trigger in seconds.");
						}
						ImGui::EndDisabled();
						ImGui::PopID();
					};

				const auto im = imgui::get();
				const auto cont_bg_color = im->ImGuiCol_ContainerBackground + ImVec4(0.05f, 0.05f, 0.05f, 0.0f);

				ImGui::Spacing(0, 12);
				ImGui::PushFont(common::imgui::font::BOLD_LARGE);
				ImGui::SeparatorText(" Trigger Settings ");
				ImGui::PopFont();
				ImGui::Spacing(0, 4);

				static float cont_height = 0.0f;
				cont_height = ImGui::Widget_ContainerWithDropdownShadow(cont_height, [marker_trigger_settings]
					{
						ImGui::Checkbox("Allow re-triggering when the event reoccurs.", &selection->trigger_always);
						TT("Allow re-triggering when the event reoccurs.");

						ImGuiWindow* window = ImGui::GetCurrentWindow();
						const auto s_workrect_max_x = window->WorkRect.Max.x;
						window->WorkRect.Max.x -= (ImGui::GetStyle().WindowPadding.x * 3.0f);

						// show
						//ImGui::Spacing(0, 12);

						ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
						ImGui::TableHeaderDropshadow(12.0f, 0.6f, 0.0f, window->WorkRect.Max.x - window->DC.CursorPos.x);

						auto spos_header = ImGui::GetCursorScreenPos();
						const auto trigger_show_state = ImGui::CollapsingHeader("Show Trigger");
						if (selection->trigger_show.has_trigger())
						{
							const auto spos_post_header = ImGui::GetCursorScreenPos();
							const auto header_dims = ImGui::GetItemRectSize();
							const auto icon_dims = ImGui::CalcTextSize(ICON_FA_CHECK);
							ImGui::SetCursorScreenPos(spos_header + ImVec2(header_dims.x - icon_dims.x - ImGui::GetStyle().WindowPadding.x - 8.0f, header_dims.y * 0.5f - icon_dims.y * 0.5f));
							ImGui::TextUnformatted(ICON_FA_CHECK);
							ImGui::SetCursorScreenPos(spos_post_header);
						}
						ImGui::PopStyleVar(); // FrameRounding

						if (trigger_show_state) {
							ImGui::Spacing(0, 2);
							marker_trigger_settings(selection->trigger_show, "ShowTrigger");
							ImGui::Spacing(0, 2);
						}

						// hide
						//ImGui::Spacing(0, 12);

						ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
						ImGui::TableHeaderDropshadow(12.0f, 0.6f, 0.0f, window->WorkRect.Max.x - window->DC.CursorPos.x);
						spos_header = ImGui::GetCursorScreenPos();

						const auto trigger_hide_state = ImGui::CollapsingHeader("Hide Trigger");
						if (selection->trigger_show.has_trigger())
						{
							const auto spos_post_header = ImGui::GetCursorScreenPos();
							const auto header_dims = ImGui::GetItemRectSize();
							const auto icon_dims = ImGui::CalcTextSize(ICON_FA_CHECK);
							ImGui::SetCursorScreenPos(spos_header + ImVec2(header_dims.x - icon_dims.x - ImGui::GetStyle().WindowPadding.x - 8.0f, header_dims.y * 0.5f - icon_dims.y * 0.5f));
							ImGui::TextUnformatted(ICON_FA_CHECK);
							ImGui::SetCursorScreenPos(spos_post_header);
						}
						ImGui::PopStyleVar(); // FrameRounding

						if (trigger_hide_state) {
							ImGui::Spacing(0, 2);
							marker_trigger_settings(selection->trigger_hide, "HideTrigger");
							ImGui::Spacing(0, 2);
						}

						// -------

						window->WorkRect.Max.x = s_workrect_max_x;

					}, & cont_bg_color, & im->ImGuiCol_ContainerBorder);

				ImGui::Spacing(0, 4);
			}

		} // selection

		ImGui::Spacing();
		/*ImGui::Spacing();
		if (ImGui::TreeNodeEx("Help##Marker", ImGuiTreeNodeFlags_Selected | ImGuiTreeNodeFlags_SpanAvailWidth))
		{
			ImGui::TextUnformatted(
				"# Spawn unique markers that can be used as anchor meshes (same one can be spawned multiple times)\n"
				"# Parameters ---------------------------------------------------------------------------------------------------------------------------------\n"
				"#\n"
				"# marker    ]     THIS:     number of marker mesh - can get culled BUT that can be controlled via leaf/area forcing (initial spawning can't be forced)[int 0 - 100]\n"
				"# nocull    ]  OR THAT:     number of marker mesh - never getting culled and spawned on map load (eg: useful for distant light) [int 0-inf.]\n"
				"# nocull    |>   areas:     (optional) only show nocull marker when player is in specified area/s [int array]\n"
				"# nocull    |> N_leafs:     (optional) only show nocull marker when player is in ^ and NOT in specified leaf/s [int array]\n"
				"#\n"
				"# position:                 X Y Z position of the marker mesh [3D Vector]\n"
				"# rotation:                 X Y Z rotation of the marker mesh [3D Vector]\n"
				"# scale:                    X Y Z scale of the marker mesh [3D Vector]\n");

			ImGui::TreePop();
		}*/
	}

	void cont_mapsettings_culling_manipulation()
	{
		auto& areas = map_settings::get_map_settings().area_settings;
		ImGui::PushFont(common::imgui::font::BOLD);
		const auto cull_top_button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
		if (ImGui::Button("Copy Culling   " ICON_FA_COPY "##Cull", ImVec2(cull_top_button_width, 0)))
		{
			ImGui::LogToClipboard();
			ImGui::LogText("%s", common::toml::build_culling_overrides_string_for_current_map(areas).c_str());
			ImGui::LogFinish();
		}
		ImGui::SameLine();
		if (ImGui::Button("Append Culling Export   " ICON_FA_SAVE "##Cull", ImVec2(cull_top_button_width, 0)))
		{
			const auto toml_str = common::toml::build_culling_overrides_string_for_current_map(areas);
			if (append_to_mapsettings_workbench_export("Culling", "current area overrides", "Paste into [culling_overrides] for this map in map_settings.toml.", toml_str))
			{
				game::console();
				std::cout << "[MapSettingsWorkbench] Exported culling overrides to: " << mapsettings_workbench_export_path() << std::endl;
			}
		}
		ImGui::PopFont();

		ImGui::SameLine();
		reload_mapsettings_button_with_popup("Cull");
		//ImGui::Spacing(0, 4);

		static map_settings::area_overrides_s* area_selection = nullptr;
		static map_settings::area_overrides_s* area_selection_old = nullptr;
		area_selection_old = area_selection; // we compare at the end of the table

		static map_settings::leaf_tweak_s* tweak_selection = nullptr;
		static map_settings::hide_area_s* hidearea_selection = nullptr;

		constexpr auto in_buflen = 1024u;
		static char in_leafs_buf[in_buflen], in_areas_buf[in_buflen],
			in_twk_in_leafs_buf[in_buflen], in_twk_areas_buf[in_buflen], in_twk_force_leafs_buf[in_buflen],
			in_hide_leafs_buf[in_buflen], in_hide_areas_buf[in_buflen], in_hide_nleafs_buf[in_buflen];

		// # CULL TABLE
		constexpr auto cull_table_num_columns = 6;
		ImGui::TableHeaderDropshadow();

		if (ImGui::BeginTable("CullTable", cull_table_num_columns, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
			ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_ScrollY, ImVec2(0, 480)))
		{
			ImGui::TableSetupScrollFreeze(0, 1); // make top row always visible
			ImGui::TableSetupColumn("Ar", ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoHide, 16.0f);
			ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthStretch, 34.0f);
			ImGui::TableSetupColumn("Leafs", ImGuiTableColumnFlags_WidthStretch, 80.0f);
			ImGui::TableSetupColumn("Areas", ImGuiTableColumnFlags_WidthStretch, 60.0f);
			ImGui::TableSetupColumn("Hide-Leafs", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide, 40.0f);
			ImGui::TableSetupColumn("LeafTweaks & HideAreas", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 140.0f);

			const char* cull_table_tooltips[cull_table_num_columns] =
			{
				"The Area the player has to be in to trigger any override functionality.\n",
				"Different anti/culling modes and additional settings for various use-cases.",
				"The Leafs which will have their visibility forced.",
				"The Areas which will have their visibility forced.",
				"The Leafs with forced invisibility.",
				("LeafTweaks: Additional per leaf overrides that only trigger if the player is in specified Leafs.\n"
				 "~ Can be quite useful at area crossings when used in conjunction with area-specific markers that spawn visibility blockers (eg. a flat plane)\n\n"
				 "HideAreas: This can be used to forcefully cull parts of the map when the game is drawing too much.\n")
			};

			ImGui::TableHeadersRowWithTooltip(cull_table_tooltips);

			bool area_selection_matches_any_entry = false;
			auto row_num = 0u;

			for (auto& [area_num, a] : areas)
			{
				ImGui::TableNextRow();

				// default selection
				if (!area_selection) {
					area_selection = &a;
				}

				// save Y offset
				const auto area_table_first_row_y_pos = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().FramePadding.y + ImGui::GetStyle().CellPadding.y;

				// handle row background color for selected entry
				const bool is_area_selected = area_selection && area_selection == &a;
				const bool player_is_in_area = g_player_current_area_override && g_player_current_area_override == &a;

				// -
				ImGui::TableNextColumn();

				float first_col_width = ImGui::GetCursorScreenPos().x;
				float start_y = ImGui::GetCursorScreenPos().y; // save row start of selector at the end of a row

				if (is_area_selected) {
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));
				}

				// set background for first column - highlight current area
				ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
					player_is_in_area ? ImGui::GetColorU32(ImGuiCol_DragDropTarget) : ImGui::GetColorU32(ImGuiCol_TableHeaderBg));

				// - Area
				const auto ar_num_str = utils::va("%d", (int)area_num);
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x * 0.5f - ImGui::CalcTextSize(ar_num_str).x * 0.5f));
				ImGui::TextUnformatted(ar_num_str);

				if (is_area_selected) {
					area_selection_matches_any_entry = true; // check that the selection ptr is up to date
				}

				// Mode
				ImGui::TableNextColumn();

				// width of first col
				first_col_width = ImGui::GetCursorScreenPos().x - first_col_width;

				// dropdown
				ImGui::PushID((int)area_num);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				if (ImGui::BeginCombo("##ModeSelector", map_settings::AREA_CULL_MODE_STR[a.cull_mode], ImGuiComboFlags_None))
				{
					for (int n = 0; n < IM_ARRAYSIZE(map_settings::AREA_CULL_MODE_STR); n++)
					{
						const bool is_selected = (a.cull_mode == n);
						if (ImGui::Selectable(map_settings::AREA_CULL_MODE_STR[n], is_selected)) {
							a.cull_mode = (map_settings::AREA_CULL_MODE)n;
						}

						if (is_selected) {
							ImGui::SetItemDefaultFocus();
						}

					}
					ImGui::EndCombo();
				}
				TT( "No Frustum:			      Compl. disable frustum culling (everywhere)\n"
					"No Frustum in Area:    Compl. disable frustum culling when in current area\n"
					"Stock:							 Stock frustum culling\n"
					"Force Area:				   ^ + force all nodes/leafs in current area\n"
					"Force Area Dist:		   ^ + all outside of current area within certain dist to player\n"
					"Distance:				       Force all nodes/leafs within certain dist to player");

				if (a.cull_mode >= map_settings::AREA_CULL_INFO_NOCULLDIST_START
					&& a.cull_mode <= map_settings::AREA_CULL_INFO_NOCULLDIST_END)
				{
					ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
					if (ImGui::DragFloat("##NocullDist", &a.nocull_distance, 0.1f, 0.0f, FLT_MAX, "%.0f"))
					{
						a.nocull_distance = a.nocull_distance < 0.0f ? 0.0f : a.nocull_distance;
						main_module::trigger_vis_logic();
					} TT("NoCull Distance - Radius around the player where nothing will get culled.")
				}

				auto row_max_y_pos = ImGui::GetItemRectMax().y;
				ImGui::PopID();

				// - Leafs
				ImGui::TableNextColumn();
				{
					// Leaf Input
					if (is_area_selected) {
						ImGui::Widget_UnorderedSetModifier("CullLeafs", ImGui::Widget_UnorderedSetModifierFlags_Leaf, area_selection->leafs, in_leafs_buf, in_buflen);
					}

					ImGui::Spacing();
					ImGui::TextWrapped_IntegersFromUnorderedSet(a.leafs);
					ImGui::Spacing();
					row_max_y_pos = std::max(row_max_y_pos, ImGui::GetItemRectMax().y);
				}

				// - Areas
				ImGui::TableNextColumn();
				{
					// Area Input
					if (is_area_selected) {
						ImGui::Widget_UnorderedSetModifier("CullAreas", ImGui::Widget_UnorderedSetModifierFlags_Area, area_selection->areas, in_areas_buf, in_buflen);
					}

					ImGui::Spacing();
					ImGui::TextWrapped_IntegersFromUnorderedSet(a.areas);
					ImGui::Spacing();
					row_max_y_pos = std::max(row_max_y_pos, ImGui::GetItemRectMax().y);
				}

				// - Hide Leafs
				ImGui::TableNextColumn();
				{
					// Hide Leafs Input
					if (is_area_selected) {
						ImGui::Widget_UnorderedSetModifier("CullHideLeafs", ImGui::Widget_UnorderedSetModifierFlags_Leaf, area_selection->hide_leafs, in_hide_leafs_buf, in_buflen);
					}

					ImGui::Spacing();
					ImGui::TextWrapped_IntegersFromUnorderedSet(a.hide_leafs);
					ImGui::Spacing();
					row_max_y_pos = std::max(row_max_y_pos, ImGui::GetItemRectMax().y);
				}

				// - tweak leafs + hide_areas
				ImGui::TableNextColumn();

				//
				if (is_area_selected) {
					ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(ImGuiCol_TableRowBg));
				}


				bool any_tweak_with_nocull_override = false;
				if (!a.leaf_tweaks.empty())
				{
					// inline table for leaf tweaks
					constexpr auto twk_leaf_num_columns = 5;
					//ImGui::TableHeaderDropshadow(8.0f, 0.6f, 4.0f);

					if (ImGui::BeginTable("tweak_leafs_nested_table", twk_leaf_num_columns, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
						ImGuiTableFlags_Reorderable | ImGuiTableFlags_ContextMenuInBody))
					{
						ImGui::TableSetupColumn("Tweak in Leafs", ImGuiTableColumnFlags_WidthStretch, 100.0f);
						ImGui::TableSetupColumn("Tweak Areas", ImGuiTableColumnFlags_WidthStretch, 100.0f);
						ImGui::TableSetupColumn("Tweak Leafs", ImGuiTableColumnFlags_WidthStretch, 100.0f);
						ImGui::TableSetupColumn("NoCullDist", ImGuiTableColumnFlags_WidthStretch, 44.0f);
						ImGui::TableSetupColumn("##Delete", ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoReorder | ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoClip, 16.0f);

						const char* twk_leaf_tooltips[twk_leaf_num_columns] =
						{
							"The leaf/s the player has to be in to trigger any leaf tweak functionality.\n",
							"The Areas which will have their visibility forced.",
							"The Leafs which will have their visibility forced.",
							"Can be used to override the 'NoCull Distance' value specified in the area (if > 0).",
							""
						};

						ImGui::TableHeadersRowWithTooltip(twk_leaf_tooltips);

						std::uint32_t cur_row = 0;
						bool selection_matches_any_entry = false;
						map_settings::leaf_tweak_s* marked_for_deletion = nullptr;

						for (auto& lt : a.leaf_tweaks)
						{
							if (is_area_selected && !tweak_selection) {
								tweak_selection = &lt;
							}

							ImGui::TableNextRow();

							// save Y offset because a multiline cell messes with following cells
							const auto tweak_table_first_row_y_pos = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().FramePadding.y + ImGui::GetStyle().CellPadding.y;
							const bool is_tweak_selected = tweak_selection && tweak_selection == &lt;

							if (is_tweak_selected) {
								ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::ColorConvertFloat4ToU32(
									ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(ImGuiCol_TableRowBgAlt)) + ImVec4(0.1f, 0.1f, 0.1f, 0.0f)));
							}
							else {
								ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableRowBg));
							}

							// ---
							// Twk In Leafs
							ImGui::TableNextColumn();

							// save row start of selector at the end of a row
							float twk_start_y = ImGui::GetCursorPosY();
							{
								// Input
								if (is_tweak_selected) {
									ImGui::Widget_UnorderedSetModifier("CullTwkInLeafs", ImGui::Widget_UnorderedSetModifierFlags_Leaf, tweak_selection->in_leafs, in_twk_in_leafs_buf, in_buflen);
								}

								ImGui::Spacing();
								ImGui::TextWrapped_IntegersFromUnorderedSet(lt.in_leafs);
								ImGui::Spacing();
							}

							auto leaftwk_row_max_y_pos = ImGui::GetItemRectMax().y;

							// Twk Areas
							ImGui::TableNextColumn();
							{
								// Input
								if (is_tweak_selected) {
									ImGui::Widget_UnorderedSetModifier("CullTwkAreas", ImGui::Widget_UnorderedSetModifierFlags_Area, tweak_selection->areas, in_twk_areas_buf, in_buflen);
								}

								ImGui::Spacing();
								ImGui::TextWrapped_IntegersFromUnorderedSet(lt.areas);
								ImGui::Spacing();
								leaftwk_row_max_y_pos = std::max(leaftwk_row_max_y_pos, ImGui::GetItemRectMax().y);
							}

							// Twk Forced Leafs
							ImGui::TableNextColumn();
							{
								// Input
								if (is_tweak_selected) {
									ImGui::Widget_UnorderedSetModifier("CullTwkForcedLeafs", ImGui::Widget_UnorderedSetModifierFlags_Leaf, tweak_selection->leafs, in_twk_force_leafs_buf, in_buflen);
								}

								ImGui::Spacing();
								ImGui::TextWrapped_IntegersFromUnorderedSet(lt.leafs);
								ImGui::Spacing();
								leaftwk_row_max_y_pos = std::max(leaftwk_row_max_y_pos, ImGui::GetItemRectMax().y);
							}

							// Twk NoCullDist
							ImGui::TableNextColumn();
							ImGui::PushID((int)cur_row);

							ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
							if (ImGui::DragFloat("##NoCullDist", &lt.nocull_dist, 0.1f, 0.0f, FLT_MAX, "%.0f")) {
								lt.nocull_dist = lt.nocull_dist < 0.0f ? 0.0f : lt.nocull_dist;
							}
							TT("NoCull Distance: Can be used to override the nocull distance for distance based cullmodes.\n"
								"Priority: Leaf NoCull  >  Area NoCull  >  Global (Default) NoCull")

								// this leaf overrides the nocull distance
								any_tweak_with_nocull_override = lt.nocull_dist > 0.0f ? true : any_tweak_with_nocull_override;
							ImGui::PopID();
							leaftwk_row_max_y_pos = std::max(leaftwk_row_max_y_pos, ImGui::GetItemRectMax().y);

							// Delete Button
							ImGui::TableNextColumn();
							{
								if (is_area_selected)
								{
									ImGui::Style_DeleteButtonPush();
									ImGui::PushID((int)cur_row);

									const auto btn_size = ImVec2(16, is_tweak_selected ? (leaftwk_row_max_y_pos - tweak_table_first_row_y_pos) : 25.0f);
									if (ImGui::Button("x##twkleaf", btn_size)) {
										marked_for_deletion = &lt;
									}

									ImGui::Style_DeleteButtonPop();
									ImGui::PopID();
								}

								if (is_area_selected && !is_tweak_selected)
								{
									float content_height = ImGui::GetCursorPosY() - twk_start_y + 2.0f;
									ImGui::SetCursorPosY(twk_start_y - 1.0f);

									ImGui::Style_InvisibleSelectorPush(); // do not show highlight using selector - we use rowbg
									if (ImGui::Selectable(utils::va("##TwkSel%d", cur_row), false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, content_height))) {
										tweak_selection = &lt;
									}
									ImGui::Style_InvisibleSelectorPop();

									if (ImGui::IsItemHovered()) {
										ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.6f)));///*ImGui::GetColorU32(ImGuiCol_TableRowBgAlt)*/);
									}
								}
							}

							cur_row++;
						}

						if (is_area_selected && !selection_matches_any_entry)
						{
							// re-check for new selection (moving towards the start of the table)
							for (auto& lt : a.leaf_tweaks)
							{
								if (tweak_selection && tweak_selection == &lt)
								{
									selection_matches_any_entry = true;
									break;
								}
							}

							if (!selection_matches_any_entry) {
								tweak_selection = nullptr;
							}
						}

						// remove entry
						if (marked_for_deletion)
						{
							for (auto it = area_selection->leaf_tweaks.begin(); it != area_selection->leaf_tweaks.end(); ++it)
							{
								if (&*it == marked_for_deletion)
								{
									area_selection->leaf_tweaks.erase(it);
									tweak_selection = nullptr;
									break;
								}
							}
						}

						// end inline table for leaf tweaks
						ImGui::EndTable();
					}
				}

				row_max_y_pos = std::max(row_max_y_pos, ImGui::GetItemRectMax().y);

				// has this area any nocull overrides?
				a.nocull_distance_overrides_in_leaf_twk = any_tweak_with_nocull_override;

				if (is_area_selected)
				{
					ImGui::Style_ColorButtonPush(imgui::get()->ImGuiCol_ButtonGreen, true);
					if (ImGui::Button("++ Tweak Leaf Entry", ImVec2(ImGui::GetContentRegionAvail().x, 28))) {
						area_selection->leaf_tweaks.emplace_back();
					}
					ImGui::Style_ColorButtonPop();
					ImGui::Spacing(0, 1.0f);
				}

				// ---
				if (!a.hide_areas.empty())
				{
					ImGui::Spacing();
					//ImGui::TableHeaderDropshadow(8.0f, 0.6f, 4.0f);

					// inline table for leaf tweaks
					if (ImGui::BeginTable("hide_areas_nested_table", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_ContextMenuInBody))
					{
						ImGui::TableSetupColumn("Areas", ImGuiTableColumnFlags_WidthStretch, 100.0f);
						ImGui::TableSetupColumn("NLeafs", ImGuiTableColumnFlags_WidthStretch, 73.0f);
						ImGui::TableSetupColumn("##Delete", ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoReorder | ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoClip, 16.0f);
						ImGui::TableHeadersRow();

						std::uint32_t cur_row = 0;
						bool selection_matches_any_entry = false;
						map_settings::hide_area_s* marked_for_deletion = nullptr;

						for (auto& hda : a.hide_areas)
						{
							if (is_area_selected && !hidearea_selection) {
								hidearea_selection = &hda;
							}

							ImGui::TableNextRow();

							// save Y offset because a multiline cell messes with following cells
							const auto hide_table_first_row_y_pos = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().FramePadding.y + ImGui::GetStyle().CellPadding.y;
							const bool is_hda_selected = hidearea_selection && hidearea_selection == &hda;

							if (is_hda_selected) {
								ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::ColorConvertFloat4ToU32(
									ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(ImGuiCol_TableRowBgAlt)) /*- ImVec4(0.1f, 0.1f, 0.1f, 0.0f)*/));
							}
							else {
								ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableRowBg));
							}

							// ---
							// Areas
							ImGui::TableNextColumn();

							// save row start of selector at the end of a row
							float hda_start_y = ImGui::GetCursorPosY();
							{
								// Input
								if (is_hda_selected) {
									ImGui::Widget_UnorderedSetModifier("CullHideAreaArea", ImGui::Widget_UnorderedSetModifierFlags_Area, hidearea_selection->areas, in_hide_areas_buf, in_buflen);
								}

								ImGui::Spacing();
								ImGui::TextWrapped_IntegersFromUnorderedSet(hda.areas);
								ImGui::Spacing();
							}

							// Not in Leafs
							ImGui::TableNextColumn();
							{
								// Input
								if (is_hda_selected) {
									ImGui::Widget_UnorderedSetModifier("CullHideAreaNLeafs", ImGui::Widget_UnorderedSetModifierFlags_Leaf, hidearea_selection->when_not_in_leafs, in_hide_nleafs_buf, in_buflen);
								}

								ImGui::Spacing();
								ImGui::TextWrapped_IntegersFromUnorderedSet(hda.when_not_in_leafs);
								ImGui::Spacing();
							}

							// Delete Button
							ImGui::TableNextColumn();
							{
								if (is_area_selected)
								{

									ImGui::PushID((int)cur_row);
									ImGui::Style_DeleteButtonPush();

									const auto btn_size = ImVec2(16, is_hda_selected ? (ImGui::GetItemRectMax().y - hide_table_first_row_y_pos) : 25.0f);
									if (ImGui::Button("x##twkhidearea", btn_size)) {
										marked_for_deletion = &hda;
									}

									ImGui::Style_DeleteButtonPop();
									ImGui::PopID();
								}

								if (is_area_selected && !is_hda_selected)
								{
									float content_height = ImGui::GetCursorPosY() - hda_start_y + (!cur_row ? 6.0f : 4.0f);
									ImGui::SetCursorPosY(hda_start_y);

									// never show selection
									if (ImGui::Selectable(utils::va("##HdaSel%d", cur_row), false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, content_height))) {
										hidearea_selection = &hda;
									}
								}
							}

							cur_row++;
						}

						if (is_area_selected && !selection_matches_any_entry)
						{
							// re-check for new selection (moving towards the start of the table)
							for (auto& hda : a.hide_areas)
							{
								if (hidearea_selection && hidearea_selection == &hda)
								{
									selection_matches_any_entry = true;
									break;
								}
							}

							if (!selection_matches_any_entry) {
								hidearea_selection = nullptr;
							}
						}

						// remove entry
						if (marked_for_deletion)
						{
							for (auto it = area_selection->hide_areas.begin(); it != area_selection->hide_areas.end(); ++it)
							{
								if (&*it == marked_for_deletion)
								{
									area_selection->hide_areas.erase(it);
									hidearea_selection = nullptr;
									break;
								}
							}
						}

						// end inline table for leaf tweaks
						ImGui::EndTable();
					}
				}

				row_max_y_pos = std::max(row_max_y_pos, ImGui::GetItemRectMax().y);

				if (is_area_selected)
				{
					ImGui::Style_ColorButtonPush(imgui::get()->ImGuiCol_ButtonGreen, true);
					if (ImGui::Button("++ Tweak Hide Area Entry", ImVec2(ImGui::GetContentRegionAvail().x, 28))) {
						area_selection->hide_areas.emplace_back();
					}
					ImGui::Style_ColorButtonPop();
					//ImGui::Spacing(0, 7.0f); // Hack and fails if window or frame padding changes
				}
				else
				{
					ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, start_y - 2.0f));
					const float content_height = row_max_y_pos - area_table_first_row_y_pos;

					ImGuiWindow* window = ImGui::GetCurrentWindow();
					const float min_x = window->ParentWorkRect.Min.x + first_col_width;
					const float max_x = window->ParentWorkRect.Max.x;

					const auto saved_parent_work_rect_min_x = window->ParentWorkRect.Min.x;
					window->ParentWorkRect.Min.x += first_col_width;

					ImGui::Style_InvisibleSelectorPush();
					if (ImGui::Selectable(utils::va("##CullAreaSelector%d", area_num), false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(max_x - min_x, content_height))) {
						area_selection = &a;
					}
					ImGui::Style_InvisibleSelectorPop();

					if (ImGui::IsItemHovered()) {
						ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.6f)));///*ImGui::GetColorU32(ImGuiCol_TableRowBgAlt)*/);
					}

					window->ParentWorkRect.Min.x = saved_parent_work_rect_min_x;
				}

				row_num++;
			} // table end for loop

			if (!area_selection_matches_any_entry)
			{
				// re-check for new selection (moving towards the start of the table)
				for (auto& [a_num, a] : areas)
				{
					if (area_selection && area_selection == &a)
					{
						area_selection_matches_any_entry = true;
						break;
					}
				}

				if (!area_selection_matches_any_entry) {
					area_selection = nullptr;
				}
			}

			ImGui::EndTable();
		} // table end

		bool was_area_removed = false;
		const auto it = has_valid_current_area() ? areas.find(current_area_u32()) : areas.end();
		const auto can_area_be_added = has_valid_current_area() && it == areas.end();
		{
			ImGui::BeginDisabled(!can_area_be_added);
			ImGui::Style_ColorButtonPush(imgui::get()->ImGuiCol_ButtonGreen, true);
			if (ImGui::Button("Add Current Area##Cull", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0)))
			{
				areas.emplace(current_area_u32(), map_settings::area_overrides_s{
						.cull_mode = map_settings::AREA_CULL_MODE::AREA_CULL_INFO_DEFAULT,
						.nocull_distance = map_settings::get_map_settings().default_nocull_dist,
						.area_index = current_area_u32(),
					});
			}
			ImGui::Style_ColorButtonPop();
			ImGui::EndDisabled();
		}

		if (area_selection)
		{
			ImGui::SameLine();
			ImGui::Style_ColorButtonPush(imgui::get()->ImGuiCol_ButtonRed, true);
			if (ImGui::Button("X Remove Selected Area Entry##Cull", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
			{
				// if selection = the area the player is in
				if (has_valid_current_area() && area_selection->area_index == current_area_u32())
				{
					if (it != areas.end()) {
						areas.erase(it);
					}
					g_player_current_area_override = nullptr;
				}
				else {
					areas.erase(area_selection->area_index);
				}

				was_area_removed = true;
			}
			ImGui::Style_ColorButtonPop();
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Culling Workbench");
		ImGui::TextDisabled("Quick anti-culling actions for the current area/leaf. Use export instead of directly editing map_settings.toml.");
		ImGui::Spacing(0, 4);

		const auto cull_wb_two_button_width = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f, 0.0f);
		if (ImGui::Button("Ensure Current Area Entry", cull_wb_two_button_width))
		{
			if (auto area = ensure_current_area_override(areas); area) {
				area_selection = area;
			}
		}
		TT("Creates/selects a culling override for the current area.");

		ImGui::SameLine();
		if (ImGui::Button("Force Current Leaf", cull_wb_two_button_width))
		{
			if (auto area = ensure_current_area_override(areas); area)
			{
				area_selection = area;
				if (has_valid_current_leaf()) {
					area->leafs.insert(current_leaf_u32());
				}
				main_module::trigger_vis_logic();
			}
		}
		TT("Adds the current leaf to the current area's forced leaf list.");

		if (ImGui::Button("Force Current Area", cull_wb_two_button_width))
		{
			if (auto area = ensure_current_area_override(areas); area)
			{
				area_selection = area;
				area->cull_mode = map_settings::AREA_CULL_MODE_FORCE_AREA;
				if (has_valid_current_area()) {
					area->areas.insert(current_area_u32());
				}
				main_module::trigger_vis_logic();
			}
		}
		TT("Switches current area to Force Area mode and also adds current area to the forced areas set.");

		ImGui::SameLine();
		if (ImGui::Button("Distance NoCull 1200", cull_wb_two_button_width))
		{
			if (auto area = ensure_current_area_override(areas); area)
			{
				area_selection = area;
				area->cull_mode = map_settings::AREA_CULL_MODE_DISTANCE;
				area->nocull_distance = 1200.0f;
				main_module::trigger_vis_logic();
			}
		}
		TT("Fast safe default for wider RTX visibility around the player in the current area.");

		ImGui::BeginDisabled(!area_selection);
		if (ImGui::Button("Add Leaf Tweak From Current Leaf", cull_wb_two_button_width) && area_selection)
		{
			map_settings::leaf_tweak_s tweak = {};
			if (has_valid_current_leaf()) {
				tweak.in_leafs.insert(current_leaf_u32());
			}
			if (has_valid_current_area()) {
				tweak.areas.insert(current_area_u32());
			}
			tweak.nocull_dist = area_selection->nocull_distance;
			area_selection->leaf_tweaks.emplace_back(tweak);
			main_module::trigger_vis_logic();
		}
		TT("Creates a leaf tweak gated by the current leaf. Edit target areas/leafs in the table after adding.");

		ImGui::SameLine();
		if (ImGui::Button("Hide Current Leaf", cull_wb_two_button_width) && area_selection)
		{
			if (has_valid_current_leaf()) {
				area_selection->hide_leafs.insert(current_leaf_u32());
			}
			main_module::trigger_vis_logic();
		}
		TT("Adds current leaf to hide_leafs for the selected area override. Useful when the engine draws too much geometry.");

		if (ImGui::Button("Copy Selected Area Culling", cull_wb_two_button_width) && area_selection)
		{
			copy_text_to_clipboard(build_single_culling_entry_string(*area_selection));
		}
		TT("Copies only the selected area culling override as a TOML snippet.");

		ImGui::SameLine();
		if (ImGui::Button("Append Selected Area Export   " ICON_FA_SAVE, cull_wb_two_button_width) && area_selection)
		{
			const auto toml_str = build_single_culling_entry_string(*area_selection);
			if (append_to_mapsettings_workbench_export("Culling", std::format("area {}", area_selection->area_index), "Paste into [culling_overrides] for this map in map_settings.toml.", toml_str))
			{
				game::console();
				std::cout << "[CullingWorkbench] Exported selected culling area to: " << mapsettings_workbench_export_path() << std::endl;
			}
		}
		TT("Safe export: appends only the selected area override to l4d2-rtx\\logs\\mapsettings_workbench_export.toml.");
		ImGui::EndDisabled();

		if (ImGui::Button("Copy Current Area/Leaf Snapshot##Cull", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
		{
			copy_text_to_clipboard(build_current_area_leaf_snapshot());
		}
		TT("Copies current map, area, leaf and camera origin for culling notes.");

		ImGui::Spacing();

		{
			auto& default_nocull_dist = map_settings::get_map_settings().default_nocull_dist;
			SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
			if (ImGui::DragFloat("Def. NoCull Dist", &default_nocull_dist, 0.5f, 0.0f)) {
				default_nocull_dist = default_nocull_dist < 0.0f ? 0.0f : default_nocull_dist;
			}
			TT("Default distance value for the default anti-cull mode (distance) if there is no override for the current area");
		}

		// resets
		if (area_selection_old != area_selection || was_area_removed)
		{
			tweak_selection = nullptr;
			hidearea_selection = nullptr;
		}

		ImGui::Spacing();

		// Outdated and not useful
		/*if (ImGui::TreeNodeEx("Help", ImGuiTreeNodeFlags_Selected | ImGuiTreeNodeFlags_SpanAvailWidth))
		{
			ImGui::TextUnformatted(
				"# Override culling per game area\n"
				"# :: Useful console command: 'xo_debug_toggle_node_vis'\n"
				"# ~~ Parameters:\n"
				"#\n"
				"# in_area:          the area the player has to be in                [int]\n"
				"# areas:            area/s with forced visibility                   [int array]\n"
				"# leafs:            leaf/s with forced visibility                   [int array]\n"
				"#\n"
				"# cull:             [0] disable frustum culling                     [int 0-5]\n"
				"#                   [1] disable frustum culling in current area\n"
				"#                   [2] stock\n"
				"#                   [3] frustum culling (outside current area) + force all nodes/leafs in current area\n"
				"#                   [4] ^ + outside of current area within certain dist to player (param: nocull_dist)\n"
				"#                   [5] force all leafs/nodes within certain dist to player (param: nocull_dist) << default\n"
				"#\n"
				"# nocull_dist:      |> Distance around the player where objects wont get culled - only used on certain cull modes [float] << defaults to 600.0\n"
				"#\n"
				"# -----------       :: This can be used to disable frustum culling for specified areas when the player is in specified leafs\n"
				"#                   :: Useful at area crossings when used in conjunction with nocull - area-specific markers that block visibility\n"
				"# leaf_tweak:																					[array of structure below]\n"
				"#                   |>    in_leafs:     the leaf/s the player has to be in                     [int array]\n"
				"#                   |>       areas:     area/s with forced visibility                          [int array]\n"
				"#                   |>       leafs:     leaf/s with forced visibility                          [int array]\n"
				"#                   |> nocull_dist:     uses per leaf value instead of area value if defined	[float]	<< defaults to 0.0 (off)\n"
				"#\n"
				"# -----------       :: This can be used to forcefully cull parts of the map\n"
				"# hide_areas :																					[array of structure below]\n"
				"#                   |>    areas:   area/s to hide												[int array]\n"
				"#                   |>  N_leafs:   only hide area/s when NOT in leaf/s							[int array]\n"
				"#\n"
				"# hide_leafs :		force hide leaf/s															[int array]\n");

			ImGui::TreePop();
		}*/
	}

	bool check_light_for_modifications(const map_settings::remix_light_settings_s& edit_def, const map_settings::remix_light_settings_s& map_def, std::vector<map_settings::remix_light_settings_s::point_s>* mover_pts)
	{
		const auto& pt = mover_pts && !mover_pts->empty() ? *mover_pts : edit_def.points;

		if (edit_def.enabled != map_def.enabled) { return true; }
		if (edit_def.group != map_def.group) { return true; }
		if (edit_def.run_once != map_def.run_once) { return true; }
		if (edit_def.loop != map_def.loop) { return true; }
		if (edit_def.loop_smoothing != map_def.loop_smoothing) { return true; }
		if (edit_def.animation != map_def.animation) { return true; }
		if (!utils::float_equal(edit_def.animation_duration, map_def.animation_duration)) { return true; }
		if (!utils::float_equal(edit_def.animation_speed, map_def.animation_speed)) { return true; }
		if (!utils::float_equal(edit_def.animation_variation, map_def.animation_variation)) { return true; }
		if (edit_def.animation_axis != map_def.animation_axis) { return true; }
		if (!utils::float_equal(edit_def.animation_degrees, map_def.animation_degrees)) { return true; }
		if (!utils::float_equal(edit_def.animation_phase, map_def.animation_phase)) { return true; }
		if (edit_def.trigger_always != map_def.trigger_always) { return true; }

		if (edit_def.trigger_choreo_name != map_def.trigger_choreo_name) { return true; }
		if (edit_def.trigger_choreo_actor != map_def.trigger_choreo_actor) { return true; }
		if (edit_def.trigger_choreo_event != map_def.trigger_choreo_event) { return true; }
		if (edit_def.trigger_choreo_param1 != map_def.trigger_choreo_param1) { return true; }
		if (edit_def.trigger_sound_hash != map_def.trigger_sound_hash) { return true; }
		if (!utils::float_equal(edit_def.trigger_delay, map_def.trigger_delay)) { return true; }

		if (edit_def.kill_choreo_name != map_def.kill_choreo_name) { return true; }
		if (edit_def.kill_sound_hash != map_def.kill_sound_hash) { return true; }
		if (!utils::float_equal(edit_def.kill_delay, map_def.kill_delay)) { return true; }

		if (!utils::float_equal(edit_def.attach_prop_radius, map_def.attach_prop_radius)) { return true; }
		if (edit_def.attach_prop_mins != map_def.attach_prop_mins) { return true; }
		if (edit_def.attach_prop_maxs != map_def.attach_prop_maxs) { return true; }
		if (edit_def.attach_prop_name != map_def.attach_prop_name) { return true; }

		if (edit_def.comment != map_def.comment) { return true; }

		if (pt.size() != map_def.points.size()) { return true; }

		for (size_t i = 0u; i < pt.size(); i++)
		{
			const auto& edit_p = pt[i];
			const auto& map_p = map_def.points[i];

			if (edit_p.position != map_p.position) { return true; }
			if (edit_p.radiance != map_p.radiance) { return true; }
			if (!utils::float_equal(edit_p.radiance_scalar, map_p.radiance_scalar)) { return true; }
			if (!utils::float_equal(edit_p.radius, map_p.radius)) { return true; }
			if (edit_p.authoring_shape != map_p.authoring_shape) { return true; }
			if (!utils::float_equal(edit_p.authoring_width, map_p.authoring_width)) { return true; }
			if (!utils::float_equal(edit_p.authoring_height, map_p.authoring_height)) { return true; }
			if (!utils::float_equal(edit_p.authoring_length, map_p.authoring_length)) { return true; }
			if (!utils::float_equal(edit_p.authoring_range, map_p.authoring_range)) { return true; }

			// never check the very first timepoint
			// could also re-calculate timepoints to check if there is a mismatch but we are writing all timepoints for now
			if (!i)
			{
				if (!utils::float_equal(edit_p.timepoint, map_p.timepoint)) {
					return true;
				}
			}

			if (!utils::float_equal(edit_p.smoothness, map_p.smoothness)) { return true; }

			if (edit_p.use_shaping != map_p.use_shaping) { return true; }
			if (edit_p.direction != map_p.direction) { return true; }
			if (!utils::float_equal(edit_p.degrees, map_p.degrees)) { return true; }
			if (!utils::float_equal(edit_p.softness, map_p.softness)) { return true; }
			if (!utils::float_equal(edit_p.exponent, map_p.exponent)) { return true; }
			if (!utils::float_equal(edit_p.volumetric_scale, map_p.volumetric_scale)) { return true; }
			if (edit_p.light_rig_mode != map_p.light_rig_mode) { return true; }
			if (edit_p.ies_profile != map_p.ies_profile) { return true; }
			if (edit_p.ies_file != map_p.ies_file) { return true; }
			if (!utils::float_equal(edit_p.ies_axis_rotation, map_p.ies_axis_rotation)) { return true; }
			if (!utils::float_equal(edit_p.ies_angle_scale, map_p.ies_angle_scale)) { return true; }
			if (!utils::float_equal(edit_p.ies_intensity_scale, map_p.ies_intensity_scale)) { return true; }
			if (edit_p.ies_normalize != map_p.ies_normalize) { return true; }
			if (!utils::float_equal(edit_p.ies_strength, map_p.ies_strength)) { return true; }
			if (!utils::float_equal(edit_p.ies_focus, map_p.ies_focus)) { return true; }
			if (edit_p.ies_emulation != map_p.ies_emulation) { return true; }
			if (edit_p.ies_emulation_samples != map_p.ies_emulation_samples) { return true; }
			if (!utils::float_equal(edit_p.ies_emulation_spread, map_p.ies_emulation_spread)) { return true; }
			if (!utils::float_equal(edit_p.ies_emulation_radius_scale, map_p.ies_emulation_radius_scale)) { return true; }
			if (!utils::float_equal(edit_p.ies_emulation_intensity_scale, map_p.ies_emulation_intensity_scale)) { return true; }
			if (!utils::float_equal(edit_p.ies_emulation_forward_offset, map_p.ies_emulation_forward_offset)) { return true; }
			if (edit_p.ies_emulation_pattern != map_p.ies_emulation_pattern) { return true; }
			if (!utils::float_equal(edit_p.ies_emulation_aspect, map_p.ies_emulation_aspect)) { return true; }
			if (!utils::float_equal(edit_p.ies_emulation_twist, map_p.ies_emulation_twist)) { return true; }
		}

		return false;
	}

	void mapsettings_ls_general_light_settings(remix_lights::light* edit_active_light)
	{
		const auto im = imgui::get();
		const auto cont_bg_color = im->ImGuiCol_ContainerBackground + ImVec4(0.05f, 0.05f, 0.05f, 0.0f);

		/*ImGui::Spacing(0, 12);
		ImGui::PushFont(common::imgui::font::BOLD_LARGE);
		ImGui::SeparatorText(" General Light Settings ");
		ImGui::PopFont();*/
		ImGui::Spacing(0, 4);

		static float cont_height = 0.0f;
		cont_height = ImGui::Widget_ContainerWithDropdownShadow(cont_height, [edit_active_light]
			{
				ImGui::BeginDisabled(!edit_active_light->m_mover.is_initialized());
				ImGui::Checkbox("Run Once", &edit_active_light->m_def.run_once);
				TT("Enabled: Destroy light after reaching the last point");

				ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.33f, 0);
				ImGui::Checkbox("Loop", &edit_active_light->m_def.loop);
				TT("Enabled: Looping light that restarts at the first point after reaching the last point.\n"
					"Disabled: Light will stop and stay active when reaching the last point.\n"
					"This does not make a difference when in edit mode.");

				ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.66f, 0);
				if (ImGui::Checkbox("Loop Smoothing", &edit_active_light->m_def.loop_smoothing)) {
					rebuild_edit_light_runtime_from_def(edit_active_light);
				}
				TT("Enabled: Automatically connect and smooth the start and end point.\n"
					"[!] requires 'loop' to be true\n"
					"[!] only position + timepoint is used from the last point");

				ImGui::EndDisabled();
				ImGui::Spacing(0, 6);

				//ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 80.0f);
				//ImGui::TextUnformatted(" Comment ");
				//ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
				ImGui::InputText("Comment", &edit_active_light->m_def.comment);
				TT("Free comment written as the TOML comment above this light.");

				ImGui::Checkbox("Enabled", &edit_active_light->m_def.enabled);
				TT("Disabled lights stay in map_settings.toml and in the editor, but runtime spawn paths ignore them outside light edit mode.");
				ImGui::SameLine();
				SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
				ImGui::InputText("Group", &edit_active_light->m_def.group);
				TT("Authoring group, for example: street, interior, infected, muzzle, signs. Can be used by the runtime group filter.");

				ImGui::Spacing(0, 12);

				ImGuiWindow* window = ImGui::GetCurrentWindow();
				const auto s_workrect_max_x = window->WorkRect.Max.x;
				window->WorkRect.Max.x -= (ImGui::GetStyle().WindowPadding.x * 3.0f);


				ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

				ImGui::TableHeaderDropshadow(12.0f, 0.6f, 0.0f, window->WorkRect.Max.x - window->DC.CursorPos.x);

				const auto spos_pre_trigger_header = ImGui::GetCursorScreenPos();
				const auto triggersettings_state = ImGui::CollapsingHeader("Trigger Settings");

				if (edit_active_light->has_spawn_trigger() || edit_active_light->has_kill_trigger())
				{
					const auto spos_post_header = ImGui::GetCursorScreenPos();
					const auto header_dims = ImGui::GetItemRectSize();
					const auto icon_dims = ImGui::CalcTextSize(ICON_FA_CHECK);
					ImGui::SetCursorScreenPos(spos_pre_trigger_header + ImVec2(header_dims.x - icon_dims.x - ImGui::GetStyle().WindowPadding.x - 8.0f, header_dims.y * 0.5f - icon_dims.y * 0.5f));
					ImGui::TextUnformatted(ICON_FA_CHECK);
					ImGui::SetCursorScreenPos(spos_post_header);
				}

				ImGui::PopStyleVar(); // FrameRounding

				if (triggersettings_state)
				{
					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					ImGui::InputText("Choreo Name##Trigger", &edit_active_light->m_def.trigger_choreo_name);
					TT("Trigger light creation when a specified choreography (vcd) starts playing.\n"
						"The choreo trigger has HIGHER precedence over sound triggering.\n"
						"This can be a substring. Use cmd 'xo_debug_scene_print' to get info about playing choreo's.");

					// clear sound trigger if choreo is not empty
					if (!edit_active_light->m_def.trigger_choreo_name.empty()) {
						edit_active_light->m_def.trigger_sound_hash = 0u;
					}

					if (!edit_active_light->m_def.trigger_choreo_name.empty())
					{
						SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
						ImGui::InputText("Choreo Actor##Trigger", &edit_active_light->m_def.trigger_choreo_actor);
						TT("Use this if the choreo name isn't enough to uniquely identify the choreo that should trigger light creation.\n"
							"This can be a substring. Use cmd 'xo_debug_scene_print' to get info about playing choreo's.");

						SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
						ImGui::InputText("Choreo Event##Trigger", &edit_active_light->m_def.trigger_choreo_event);
						TT("Use this if the choreo name isn't enough to uniquely identify the choreo that should trigger light creation.\n"
							"This can be a substring. Use cmd 'xo_debug_scene_print' to get info about playing choreo's.");

						SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
						ImGui::InputText("Choreo Param1##Trigger", &edit_active_light->m_def.trigger_choreo_param1);
						TT("Use this if the choreo name isn't enough to uniquely identify the choreo that should trigger light creation.\n"
							"This can be a substring. Use cmd 'xo_debug_scene_print' to get info about playing choreo's.");
					}

					// --- sound

					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					std::string temp_sound_hash_str = edit_active_light->m_def.trigger_sound_hash ? std::format("0x{:X}", edit_active_light->m_def.trigger_sound_hash) : "";

					if (ImGui::InputText("Sound Hash##Trigger", &temp_sound_hash_str, ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue,
						[](ImGuiInputTextCallbackData* data)
						{
							const auto c = static_cast<char>(data->EventChar);
							if (std::isxdigit(c) || c == 'x' || c == 'X') {
								return 0; // allow input
							}
							return 1; // block input
						}))
					{
						edit_active_light->m_def.trigger_sound_hash = static_cast<uint32_t>(std::strtoul(temp_sound_hash_str.c_str(), nullptr, 16));
						temp_sound_hash_str = std::format("0x{:X}", edit_active_light->m_def.trigger_sound_hash);
					}
					TT("Trigger light creation when a specified sound starts playing.\n"
						"The sound trigger has LOWER precedence over choreo triggering.\n"
						"Use cmd 'xo_debug_toggle_sound_print' to get info about playing sounds.");

					ImGui::BeginDisabled(!edit_active_light->has_spawn_trigger());
					{
						SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
						if (ImGui::DragFloat("Delay##Trigger", &edit_active_light->m_def.trigger_delay, 0.05f, 0.0f)) {
							edit_active_light->m_def.trigger_delay = edit_active_light->m_def.trigger_delay < 0.0f ? 0.0f : edit_active_light->m_def.trigger_delay;
						} TT("Delay spawn after trigger in seconds.");

						ImGui::Checkbox("Always", &edit_active_light->m_def.trigger_always);
						TT("Retriggering the event again will spawn a new light instance everytime.");
					}
					ImGui::EndDisabled();

					// clear choreo trigger if sound hash is not empty
					if (edit_active_light->m_def.trigger_sound_hash)
					{
						edit_active_light->m_def.trigger_choreo_name.clear();
						edit_active_light->m_def.trigger_choreo_actor.clear();
						edit_active_light->m_def.trigger_choreo_event.clear();
						edit_active_light->m_def.trigger_choreo_param1.clear();
					}

					// --
					// kill choreo


					ImGui::Spacing(0, 12);
					ImGui::TextUnformatted(" Kill Settings ");

					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					ImGui::InputText("Choreo Name##Kill", &edit_active_light->m_def.kill_choreo_name);
					TT("Trigger light deletion when a specified choreography (vcd) starts playing.\n"
						"The choreo trigger has HIGHER precedence over sound triggering.\n"
						"This can be a substring. Use cmd 'xo_debug_scene_print' to get info about playing choreo's.");

					// clear sound trigger if choreo is not empty
					if (!edit_active_light->m_def.kill_choreo_name.empty()) {
						edit_active_light->m_def.kill_sound_hash = 0u;
					}

					// kill sound

					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					std::string temp_kill_sound_hash_str = edit_active_light->m_def.kill_sound_hash ? std::format("0x{:X}", edit_active_light->m_def.kill_sound_hash) : "";

					if (ImGui::InputText("Sound Hash##Kill", &temp_kill_sound_hash_str, ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue,
						[](ImGuiInputTextCallbackData* data)
						{
							const auto c = static_cast<char>(data->EventChar);
							if (std::isxdigit(c) || c == 'x' || c == 'X') {
								return 0; // allow input
							}
							return 1; // block input
						}))
					{
						edit_active_light->m_def.kill_sound_hash = static_cast<uint32_t>(std::strtoul(temp_kill_sound_hash_str.c_str(), nullptr, 16));
						temp_kill_sound_hash_str = std::format("0x{:X}", edit_active_light->m_def.kill_sound_hash);
					}
					TT("Trigger light creation when a specified sound starts playing.\n"
						"The sound trigger has LOWER precedence over choreo triggering.\n"
						"Use cmd 'xo_debug_toggle_sound_print' to get info about playing sounds.");

					ImGui::BeginDisabled(!edit_active_light->has_kill_trigger());
					{
						SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
						if (ImGui::DragFloat("Delay##Kill", &edit_active_light->m_def.kill_delay, 0.05f, 0.0f)) {
							edit_active_light->m_def.kill_delay = edit_active_light->m_def.kill_delay < 0.0f ? 0.0f : edit_active_light->m_def.kill_delay;
						} TT("Delay kill after kill trigger in seconds.");
					}
					ImGui::EndDisabled();

					// clear choreo kill trigger if sound hash is not empty
					if (edit_active_light->m_def.kill_sound_hash) {
						edit_active_light->m_def.kill_choreo_name.clear();
					}
				}

				ImGui::Spacing();
				ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

				ImGui::TableHeaderDropshadow(12.0f, 0.6f, 0.0f, window->WorkRect.Max.x - window->DC.CursorPos.x);

				const auto spos_pre_attach_header = ImGui::GetCursorScreenPos();
				const auto attachprop_settings_state = ImGui::CollapsingHeader("Attach to Prop");

				if (edit_active_light->is_attached())
				{
					const auto spos_post_header = ImGui::GetCursorScreenPos();
					const auto header_dims = ImGui::GetItemRectSize();
					const auto icon_dims = ImGui::CalcTextSize(ICON_FA_CHECK);
					ImGui::SetCursorScreenPos(spos_pre_attach_header + ImVec2(header_dims.x - icon_dims.x - ImGui::GetStyle().WindowPadding.x - 8.0f, header_dims.y * 0.5f - icon_dims.y * 0.5f));
					ImGui::TextUnformatted(ICON_FA_CHECK);
					ImGui::SetCursorScreenPos(spos_post_header);
				}


				ImGui::PopStyleVar(); // FrameRounding

				if (attachprop_settings_state)
				{
					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					if (ImGui::DragFloat("Prop Radius##Attach", &edit_active_light->m_def.attach_prop_radius, 0.001f, 0.0f, 0.0f, "%.6f"))
					{
						edit_active_light->m_def.attach_prop_radius = edit_active_light->m_def.attach_prop_radius < 0.0f ? 0.0f : edit_active_light->m_def.attach_prop_radius;
						if (edit_active_light->m_def.attach_prop_radius > 0.0f) {
							edit_active_light->m_def.attach_prop_name.clear();
						}
					}
					TT("Attach light to a prop with this radius. This + bounds is the recommended way!\n"
						"Use cmd 'xo_debug_toggle_model_info' to get info about nearby props.");

					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					if (ImGui::InputText("Prop Name##Attach", &edit_active_light->m_def.attach_prop_name))
					{
						if (!edit_active_light->m_def.attach_prop_name.empty()) {
							edit_active_light->m_def.attach_prop_radius = 0.0f;
						}
					}
					TT("Attach light to a prop that contains this string within its name.\n"
						"This is slower than using radius + bounds so be aware of that.\n"
						"Use cmd 'xo_debug_toggle_model_info' to get info about nearby props.");

					ImGui::BeginDisabled(!edit_active_light->has_attach_parms());
					{
						if (ImGui::Widget_PrettyDragVec3("Bounds Min", &edit_active_light->m_def.attach_prop_mins.x, true, 120.0f, 0.05f))
						{
							auto& mins = edit_active_light->m_def.attach_prop_mins;
							const auto& maxs = edit_active_light->m_def.attach_prop_maxs;
							mins.x = mins.x > maxs.x ? maxs.x - 1.0f : mins.x;
							mins.y = mins.y > maxs.y ? maxs.y - 1.0f : mins.y;
							mins.z = mins.z > maxs.z ? maxs.z - 1.0f : mins.z;
						} TT("Bounding Box Mins where a light can get attached to a mesh fitting the above parameters.");

						if (ImGui::Widget_PrettyDragVec3("Bounds Max", &edit_active_light->m_def.attach_prop_maxs.x, true, 120.0f, 0.05f))
						{
							const auto& mins = edit_active_light->m_def.attach_prop_mins;
							auto& maxs = edit_active_light->m_def.attach_prop_maxs;
							maxs.x = maxs.x < mins.x ? mins.x + 1.0f : maxs.x;
							maxs.y = maxs.y < mins.y ? mins.y + 1.0f : maxs.y;
							maxs.z = maxs.z < mins.z ? mins.z + 1.0f : maxs.z;
						} TT("Bounding Box Maxs where a light can get attached to a mesh fitting the above parameters.");

						// check if any val of max is smaller than any val of mins and warn the user
						if (edit_active_light->m_def.attach_prop_maxs < edit_active_light->m_def.attach_prop_mins)
						{
							ImGui::PushFont(common::imgui::font::BOLD);
							ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.15f, 0.15f, 1.0f));
							ImGui::TextUnformatted("Invalid Bounds! MAX smaller than MIN (any of X Y Z)");
							ImGui::SafePopStyleColor(1, __LINE__);
							ImGui::PopFont();
						}

						ImGui::EndDisabled();
					}

					ImGui::Spacing(0, 4);
					ImGui::SeparatorText(" Attach to Bone Section ");

					ImGui::BeginDisabled(!edit_active_light->is_attached());
					{
						{
							SET_CHILD_WIDGET_WIDTH_MAN(120.0f);

							const float half_button_width = (ImGui::CalcItemWidth() - ImGui::GetStyle().ItemSpacing.x) / 2.0f; //(window->WorkRect.Max.x - window->DC.CursorPos.x) * 0.5f;
							if (ImGui::Button("Toggle Bone Information", ImVec2(half_button_width, 0))) {
								cmd::show_mesh_bone_info_attached = !cmd::show_mesh_bone_info_attached;
							} TT("Show bone information for the current 'active' mesh that has a light attached.");

							ImGui::SameLine();
							if (ImGui::Button("Reset Tracked Entity", ImVec2(half_button_width, 0))) {
								edit_active_light->m_entity_index = -1;
							} TT("Lights will track the index of the entity that they were first spawned on. You might need to reset it when the original entity was removed or similar.");
						}

						SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
						if (ImGui::DragInt("Prop Bone Index##Attach", &edit_active_light->m_def.attach_bone_index, 0.02f, -1, 256, "%d", ImGuiSliderFlags_AlwaysClamp)) {
							edit_active_light->m_def.attach_bone_name.clear();
						}
						TT("Attach light to a specific bone on the prop.\n"
							"A value of -1 will disable this functionality."
							"Use cmd 'xo_debug_show_mesh_bone_info' to show bone info of the currently 'active' prop.");


						SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
						if (ImGui::InputText("Prop Bone Name##Attach", &edit_active_light->m_def.attach_bone_name))
						{
							if (!edit_active_light->m_def.attach_bone_name.empty()) {
								edit_active_light->m_def.attach_bone_index = -1;
							}
						}
						TT("Attach light to a specific bone on the prop.\n"
							"You need to provide the full bone name. This is slower then using the bone index.\n"
							"Use cmd 'xo_debug_show_mesh_bone_info' to show bone info of the currently 'active' prop.");

						ImGui::EndDisabled();
					}

					ImGui::Spacing(0, 4);

					ImGui::PushFont(common::imgui::font::BOLD);
					ImGui::Indent(4);
					ImGui::Text("Attach Status: %s",
						edit_active_light->has_attach_parms() && edit_active_light->is_attached() ? "Attached." :
						edit_active_light->has_attach_parms() ? "Found no fitting prop to attach to." : "No attach parameters defined.");
					ImGui::Unindent(4);
					ImGui::PopFont();
				}

				window->WorkRect.Max.x = s_workrect_max_x;
				ImGui::Spacing(0, 4);

			}, &cont_bg_color, &im->ImGuiCol_ContainerBorder);

		// debug vis

		if (im->m_debugvis_attach_bounds && edit_active_light->has_attach_parms() && remix_api::is_initialized())
		{
			const auto remixapi = remix_api::get();
			remixapi->debug_draw_box(edit_active_light->m_def.attach_prop_mins, edit_active_light->m_def.attach_prop_maxs, 1.0f,
				edit_active_light->is_attached() ? remix_api::DEBUG_REMIX_LINE_COLOR::WHITE : remix_api::DEBUG_REMIX_LINE_COLOR::RED);
		}
	}

	void mapsettings_ls_playback_visualization_settings(remix_lights::light* edit_active_light, const bool is_static_light_with_single_point)
	{
		const auto im = imgui::get();
		const auto cont_bg_color = im->ImGuiCol_ContainerBackground + ImVec4(0.05f, 0.05f, 0.05f, 0.0f);

		ImGui::Spacing(0, 12);
		ImGui::PushFont(common::imgui::font::BOLD_LARGE);
		ImGui::SeparatorText(" Playback / Visualization Settings ");
		ImGui::PopFont();
		ImGui::Spacing(0, 4);

		static float cont_height = 0.0f;
		cont_height = ImGui::Widget_ContainerWithDropdownShadow(cont_height, [edit_active_light, is_static_light_with_single_point]
			{
				const auto im = imgui::get();
				ImGui::Checkbox("Live Vis.", &im->m_debugvis_live); TT("Enable live visualizations instead of static per point visualizations.");
				ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.49f, 0);
				ImGui::Checkbox("Vis. Light Radius", &im->m_debugvis_radius); TT("Show light radius visualizations.");

				ImGui::Checkbox("Vis. Light Shaping", &im->m_debugvis_shaping); TT("Show light shaping visualizations.");
				ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.49f, 0);
				ImGui::Checkbox("Vis. Light Labels", &im->m_debugvis_light_labels); TT("Show point-light labels with radius, radiance scale and active light backend/profile.");

				ImGui::Checkbox("Vis. IES Cluster", &im->m_debugvis_ies_cluster); TT("Show helper-light positions for the Fake IES Profile Rig.");
				ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.49f, 0);
				ImGui::Checkbox("Vis. Attach Bounds", &im->m_debugvis_attach_bounds); TT("Show bounding box visualization used by light attachment logic.");

				ImGui::SetNextItemWidth(100.0f);
				if (ImGui::InputInt("##Shaping Cone Steps", &im->m_debugvis_cone_steps, 1, 2, ImGuiInputTextFlags_CharsDecimal)) {
					im->m_debugvis_cone_steps = std::clamp(im->m_debugvis_cone_steps, 0, 100);
				} TT("Amount of light shaping cone steps used for debug visualizations.");

				ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.49f, 0);
				ImGui::SetNextItemWidth(100.0f);
				ImGui::DragFloat("Shaping Cone Height", &im->m_debugvis_cone_height); TT("Height of light shaping cone (debug visualizations).");

				ImGui::Spacing(0, 6);

				ImGui::BeginDisabled(!edit_active_light->m_mover.is_initialized());
				ImGui::BeginDisabled(is_static_light_with_single_point);
				if (ImGui::Button("Restart Light Loop", ImVec2(ImGui::GetContentRegionAvail().x * 0.49f - ImGui::GetStyle().ItemSpacing.x, 0))) {
					edit_active_light->m_mover.restart();
				} TT("This resets the current loop to timepoint = 0");
				ImGui::EndDisabled();

				ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.49f);
				if (ImGui::Button("Evenly distribute all timepoints", ImVec2(ImGui::GetContentRegionAvail().x * 0.98f, 0)))
				{
					auto& pts = edit_active_light->m_def.points;
					// clear timepoints for all but the very first & very last points:
					for (size_t i = 1u; i < pts.size() - 1u; i++) {
						pts[i].timepoint = 0.0f;
					}

					rebuild_edit_light_runtime_from_def(edit_active_light);
				} TT("This will clear and recalculate the timepoints of all but the last point to evenly distribute time across all point 2 point segments.");
				ImGui::EndDisabled();

			}, &cont_bg_color, &im->ImGuiCol_ContainerBorder);
	}

	void cont_mapsettings_light_spawning()
	{
		const auto im = imgui::get();
		const auto lights = remix_lights::get();

		static map_settings::remix_light_settings_s* ms_light_selection = nullptr;
		static map_settings::remix_light_settings_s* ms_light_selection_pending = nullptr;

		// Direct edit entry: auto-save current map authoring data, reload once, and enter
		// the existing editor without a blocking confirmation dialog.
		if (!im->m_light_edit_mode)
		{
			ms_light_selection = nullptr;
			ms_light_selection_pending = nullptr;

			ui_warning_banner("Light editor is inactive",
				"Entering edit mode performs one automatic map-settings save and reload. No confirmation popup is required.",
				ImVec4(0.96f, 0.76f, 0.30f, 1.0f));
			if (ImGui::Button(ICON_FA_EDIT "  Enter Light Edit Mode", ImVec2(ImGui::GetContentRegionAvail().x, 36.0f)))
			{
				map_settings::save_current_map_authoring_data(true);
				im->m_light_edit_mode = true;
				map_settings::reload();
				im->m_light_edit_mode = true;
				if (dynamic_lighting::m_map_light_auto_sync_editor)
					dynamic_lighting::sync_imported_map_lights_to_light_editor();
			}
			TT("Saves pending current-map authoring data, reloads map_settings once and opens the existing editor immediately.");
			return;
		}

		auto& ms_lights = map_settings::get_map_settings().remix_lights;
		if (!light_def_pointer_in_vector(ms_lights, ms_light_selection)) ms_light_selection = nullptr;
		if (!light_def_pointer_in_vector(ms_lights, ms_light_selection_pending)) ms_light_selection_pending = nullptr;

		const auto imported_editor_count = dynamic_lighting::get_light_editor_imported_count();
		const auto direct_light_count = static_cast<std::size_t>(std::count_if(ms_lights.begin(), ms_lights.end(),
			[](const auto& light) { return is_direct_sun_light(light); }));
		const auto primary_sun_it = std::find_if(ms_lights.begin(), ms_lights.end(),
			[](const auto& light) { return is_direct_sun_light(light); });
		const std::size_t primary_sun_index = primary_sun_it == ms_lights.end()
			? std::numeric_limits<std::size_t>::max()
			: static_cast<std::size_t>(std::distance(ms_lights.begin(), primary_sun_it));
		if (ImGui::BeginTable("##sun_light_overview", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ui_metric_card(ICON_FA_SUN "  Direct / Sun", std::format("{}", direct_light_count), "Distant lights are pinned and highlighted in the manager.");
			ImGui::TableNextColumn(); ui_metric_card("Current map", map_settings::get_map_name().empty() ? "not loaded" : map_settings::get_map_name());
			ImGui::TableNextColumn(); ui_metric_card("Imported", std::format("{}", imported_editor_count));
			ImGui::TableNextColumn(); ui_metric_card("Selected type", ms_light_selection ? light_type_name(*ms_light_selection) : "none");
			ImGui::EndTable();
		}
		if (primary_sun_it != ms_lights.end())
		{
			const auto& sun = *primary_sun_it;
			ImGui::Text(ICON_FA_SUN "  PRIMARY SUN  |  %s", sun.comment.empty() ? "Direct / Distant light" : sun.comment.c_str());
			if (!sun.points.empty())
			{
				const auto& point = sun.points.front();
				ImGui::TextDisabled("Source %s | direction %.3f %.3f %.3f | angular size %.3f | radiance scalar %.3f",
					light_origin_badge(sun), point.direction.x, point.direction.y, point.direction.z,
					point.authoring_width, point.radiance_scalar);
			}
			else
			{
				ImGui::TextDisabled("Source %s | no authored point payload", light_origin_badge(sun));
			}
		}

		static std::size_t sun_selection_cursor = 0u;
		const float sun_button_spacing = ImGui::GetStyle().ItemSpacing.x;
		const ImVec2 sun_button_size((ImGui::GetContentRegionAvail().x - sun_button_spacing * 2.0f) / 3.0f, 0.0f);
		ImGui::BeginDisabled(direct_light_count == 0u);
		if (ImGui::Button(ICON_FA_SUN "  Select Primary Sun", sun_button_size))
		{
			commit_active_editor_light_to_selection(lights, ms_light_selection);
			const auto found = std::find_if(ms_lights.begin(), ms_lights.end(), [](const auto& light) { return is_direct_sun_light(light); });
			if (found != ms_lights.end())
			{
				ms_light_selection = &*found;
				sun_selection_cursor = 0u;
				rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_SYNC_ALT "  Next Sun", sun_button_size))
		{
			std::vector<map_settings::remix_light_settings_s*> suns;
			for (auto& light : ms_lights) if (is_direct_sun_light(light)) suns.push_back(&light);
			if (!suns.empty())
			{
				commit_active_editor_light_to_selection(lights, ms_light_selection);
				sun_selection_cursor = (sun_selection_cursor + 1u) % suns.size();
				ms_light_selection = suns[sun_selection_cursor];
				rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_MAP_MARKER_ALT "  TP to Sun Origin", sun_button_size) && primary_sun_it != ms_lights.end() && !primary_sun_it->points.empty())
		{
			const auto& position = primary_sun_it->points.front().position;
			interfaces::get()->m_engine->execute_client_cmd_unrestricted(utils::va("sv_cheats 1; noclip; setpos %.2f %.2f %.2f", position.x, position.y, position.z - 40.0f));
		}
		ImGui::EndDisabled();
		if (direct_light_count == 0u)
			ImGui::TextDisabled("No Direct / Distant light is present in the current map database. Run BSP import with Global Sun / Moon enabled.");

		ImGui::TextDisabled("Map Light DB: %s", dynamic_lighting::m_persistent_map_light_status.c_str());
		if (imported_editor_count > 0u)
		{
			if (ImGui::Button(utils::va("Save %u Imported Lights to Map Database", imported_editor_count), ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
			{
				commit_active_editor_light_to_selection(lights, ms_light_selection);
				dynamic_lighting::save_light_editor_map_lights_to_config();
			}
			TT("Atomically writes the full per-map light database. Captured lights, authored rigs, complete point animations, IES settings, attachments, triggers and deletions are preserved.");
			ImGui::TextDisabled("%s", dynamic_lighting::m_map_light_override_status.c_str());
		}

		ImGui::PushFont(common::imgui::font::BOLD);
		if (ImGui::Button("Copy Selected Light to Clipboard   " ICON_FA_SAVE, ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0)))
		{
			if (const auto edit_light = lights->get_first_active_light(); edit_light)
			{
				const auto temp_def = build_current_edit_light_def(edit_light);
				copy_text_to_clipboard(common::toml::build_light_string_for_single_light(temp_def));
			}
		} ImGui::PopFont();

		ImGui::SameLine();
		reload_mapsettings_button_with_popup("RemixLights");

		static std::string light_table_filter;
		static bool light_table_enabled_only = false;
		static bool light_table_grouped_only = false;
		static bool light_table_direct_only = false;
		static bool light_table_imported_only = false;
		static bool light_table_sun_first = true;

		ImGui::Spacing(0, 8);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
		ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(230.0f));
		ImGui::InputText("Filter##LightTableFilter", &light_table_filter);
		TT("Filters the authored light list by group, comment, choreo trigger or kill trigger.");
		ImGui::SameLine();
		imgui::toggle_button_bool(&light_table_enabled_only, "Enabled Only", ImVec2(ImGui::CalcWidgetWidthForChild(110.0f), 0.0f), "Show only enabled lights.");
		ImGui::SameLine();
		imgui::toggle_button_bool(&light_table_direct_only, ICON_FA_SUN " Direct Only", ImVec2(ImGui::CalcWidgetWidthForChild(118.0f), 0.0f), "Show only Direct / Distant map lights.");
		ImGui::SameLine();
		imgui::toggle_button_bool(&light_table_imported_only, "Imported Only", ImVec2(ImGui::CalcWidgetWidthForChild(118.0f), 0.0f), "Show only lights imported or captured from Source.");
		ImGui::SameLine();
		imgui::toggle_button_bool(&light_table_sun_first, ICON_FA_SUN " Sun First", ImVec2(ImGui::CalcWidgetWidthForChild(105.0f), 0.0f), "Pin Direct / Distant lights to the top of the current-map list.");
		ImGui::SameLine();
		if (ImGui::Button("Clear Filter", ImVec2(ImGui::CalcWidgetWidthForChild(105.0f), 0.0f)))
		{
			light_table_filter.clear();
			light_table_enabled_only = false;
			light_table_grouped_only = false;
			light_table_direct_only = false;
			light_table_imported_only = false;
			light_table_sun_first = true;
		}
		ImGui::PopStyleVar();

		ImGui::Spacing(0, 4);
		bool preview_options_changed = false;
		ImGui::Spacing(0, 4);
		if (ImGui::BeginTabBar("##light_editor_settings_tabs", ImGuiTabBarFlags_None))
		{
			if (ImGui::BeginTabItem("Scene"))
			{
				preview_options_changed |= imgui::toggle_button_bool(&im->m_light_editor_preview_all, "Preview All", ImVec2(ImGui::CalcWidgetWidthForChild(115.0f), 0.0f),
					"Keep every enabled authored light active while editing.");
				ImGui::SameLine();
				preview_options_changed |= imgui::toggle_button_bool(&im->m_light_editor_gizmos_all, "All Gizmos", ImVec2(ImGui::CalcWidgetWidthForChild(105.0f), 0.0f),
					"Draw gizmos for all authored lights.");
				ImGui::SameLine();
				preview_options_changed |= imgui::toggle_button_bool(&im->m_light_editor_mouse_drag, "Mouse Drag", ImVec2(ImGui::CalcWidgetWidthForChild(105.0f), 0.0f),
					"Drag light handles directly in the scene.");
				ImGui::SameLine();
				preview_options_changed |= imgui::toggle_button_bool(&im->m_light_editor_drag_whole_light, "Whole Light", ImVec2(ImGui::CalcWidgetWidthForChild(105.0f), 0.0f),
					"Apply transforms to every animation keyframe.");

				imgui::toggle_button_bool(&im->m_light_editor_axis_gizmo, "XYZ Gizmo", ImVec2(ImGui::CalcWidgetWidthForChild(105.0f), 0.0f), "Show XYZ handles.");
				ImGui::SameLine();
				imgui::toggle_button_bool(&im->m_light_editor_plane_gizmo, "Planes", ImVec2(ImGui::CalcWidgetWidthForChild(85.0f), 0.0f), "Show XY/XZ/YZ handles.");
				ImGui::SameLine();
				imgui::toggle_button_bool(&im->m_light_editor_center_handle, "Center", ImVec2(ImGui::CalcWidgetWidthForChild(85.0f), 0.0f), "Show free-move center handle.");
				ImGui::SameLine();
				imgui::toggle_button_bool(&im->m_light_editor_keyboard_controls, "Keyboard", ImVec2(ImGui::CalcWidgetWidthForChild(95.0f), 0.0f), "Enable W/E/R/T and axis shortcuts.");
				ImGui::SameLine();
				imgui::toggle_button_bool(&im->m_light_editor_snap, "Snap", ImVec2(ImGui::CalcWidgetWidthForChild(70.0f), 0.0f), "Snap movement to grid.");
				ImGui::SameLine();
				imgui::toggle_button_bool(&im->m_light_editor_selected_always_visible, "Keep Selected", ImVec2(ImGui::CalcWidgetWidthForChild(110.0f), 0.0f), "Always draw selected light.");

				const auto transform_button_size = ImVec2(ImGui::CalcWidgetWidthForChild(88.0f), 0.0f);
				if (ImGui::Button(im->m_light_editor_transform_mode == 0 ? "[W] Move" : "W Move", transform_button_size)) im->m_light_editor_transform_mode = 0;
				ImGui::SameLine();
				if (ImGui::Button(im->m_light_editor_transform_mode == 1 ? "[E] Rotate" : "E Rotate", transform_button_size)) im->m_light_editor_transform_mode = 1;
				ImGui::SameLine();
				if (ImGui::Button(im->m_light_editor_transform_mode == 2 ? "[R] Shape" : "R Shape", transform_button_size)) im->m_light_editor_transform_mode = 2;
				ImGui::SameLine();
				if (ImGui::Button(im->m_light_editor_transform_mode == 3 ? "[T] Power" : "T Power", transform_button_size)) im->m_light_editor_transform_mode = 3;
				ImGui::SameLine();
				if (ImGui::Button(im->m_light_editor_axis_constraint == 0 ? "[C] Free" : "C Free", ImVec2(78.0f, 0.0f))) im->m_light_editor_axis_constraint = 0;
				ImGui::SameLine();
				if (ImGui::Button(im->m_light_editor_axis_constraint == 1 ? "[X]" : "X", ImVec2(42.0f, 0.0f))) im->m_light_editor_axis_constraint = im->m_light_editor_axis_constraint == 1 ? 0 : 1;
				ImGui::SameLine();
				if (ImGui::Button(im->m_light_editor_axis_constraint == 2 ? "[Y]" : "Y", ImVec2(42.0f, 0.0f))) im->m_light_editor_axis_constraint = im->m_light_editor_axis_constraint == 2 ? 0 : 2;
				ImGui::SameLine();
				if (ImGui::Button(im->m_light_editor_axis_constraint == 3 ? "[Z]" : "Z", ImVec2(42.0f, 0.0f))) im->m_light_editor_axis_constraint = im->m_light_editor_axis_constraint == 3 ? 0 : 3;

				if (ImGui::BeginTable("##light_scene_steps", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(95.0f); ImGui::DragFloat("Move Step", &im->m_light_editor_keyboard_step, 0.05f, 0.001f, 512.0f, "%.3f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(95.0f); ImGui::DragFloat("Angle Step", &im->m_light_editor_angle_step, 0.25f, 0.1f, 90.0f, "%.2f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(95.0f); ImGui::DragFloat("Snap Grid", &im->m_light_editor_snap_step, 0.05f, 0.001f, 1024.0f, "%.3f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(95.0f); ImGui::DragFloat("Axis Size", &im->m_light_editor_gizmo_axis_length, 1.0f, 20.0f, 120.0f, "%.0f px");
					ImGui::EndTable();
				}
				ImGui::TextDisabled("MMB: create/context | W/E/R/T: mode | X/Y/Z: axis | C: free | Ctrl+S: save");
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Display"))
			{
				if (ImGui::Button("Default Details: 270", ImVec2(ImGui::CalcWidgetWidthForChild(150.0f), 0.0f)))
				{
					im->m_light_editor_label_visibility_distance = 270.0f;
					im->m_light_editor_shape_visibility_distance = 270.0f;
				}
				ImGui::SameLine();
				imgui::toggle_button_bool(&im->m_light_editor_dynamic_fov, "Game FOV", ImVec2(ImGui::CalcWidgetWidthForChild(95.0f), 0.0f), "Read current game FOV for cursor ray.");
				if (ImGui::BeginTable("##light_display_values", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Gizmo Range", &im->m_light_editor_gizmo_visibility_distance, 8.0f, 16.0f, 65536.0f, "%.0f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Fade Start", &im->m_light_editor_gizmo_fade_start, 8.0f, 0.0f, 65536.0f, "%.0f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Labels", &im->m_light_editor_label_visibility_distance, 8.0f, 0.0f, 65536.0f, "%.0f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Cones/Details", &im->m_light_editor_shape_visibility_distance, 8.0f, 0.0f, 65536.0f, "%.0f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragInt("Max Gizmos", &im->m_light_editor_max_visible_gizmos, 1.0f, 1, 4096);
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f);
					ImGui::BeginDisabled(im->m_light_editor_dynamic_fov);
					ImGui::DragFloat("Manual FOV", &im->m_light_editor_manual_fov, 0.25f, 20.0f, 160.0f, "%.1f deg");
					ImGui::EndDisabled();
					ImGui::EndTable();
				}
				im->m_light_editor_label_visibility_distance = std::clamp(im->m_light_editor_label_visibility_distance, 0.0f, 65536.0f);
				im->m_light_editor_shape_visibility_distance = std::clamp(im->m_light_editor_shape_visibility_distance, 0.0f, 65536.0f);
				ImGui::TextDisabled("Labels, cones and detailed light geometry default to 270 Source units.");
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Placement"))
			{
				imgui::toggle_button_bool(&im->m_light_editor_surface_placement, "Surface Hit", ImVec2(ImGui::CalcWidgetWidthForChild(105.0f), 0.0f),
					"Trace against world and physical entities; place at the first cursor hit.");
				ImGui::SameLine();
				if (ImGui::Button("Reset Placement", ImVec2(ImGui::CalcWidgetWidthForChild(120.0f), 0.0f)))
				{
					im->m_light_editor_spawn_distance = 2048.0f;
					im->m_light_editor_surface_offset = 2.0f;
				}
				if (ImGui::BeginTable("##light_placement_values", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Ray Max", &im->m_light_editor_spawn_distance, 2.0f, 16.0f, 8192.0f, "%.0f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Surface Offset", &im->m_light_editor_surface_offset, 0.1f, 0.0f, 64.0f, "%.1f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Radius", &im->m_light_editor_spawn_radius, 0.025f, 0.001f, 128.0f, "%.3f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Power", &im->m_light_editor_spawn_intensity, 0.25f, 0.0f, 100000.0f, "%.1f");
					ImGui::EndTable();
				}
				ImGui::TextDisabled("MMB uses the first traced surface. Ray Max is used only as trace limit and no-hit fallback.");
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Advanced"))
			{
				if (ImGui::BeginTable("##light_advanced_values", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Drag Scale", &im->m_light_editor_gizmo_drag_scale, 0.0001f, 0.0001f, 0.05f, "%.4f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Pick Radius", &im->m_light_editor_gizmo_pick_radius, 0.25f, 4.0f, 80.0f, "%.1f px");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Depth Scale", &im->m_light_editor_gizmo_depth_scale, 0.0025f, 0.0001f, 0.5f, "%.4f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Drag Threshold", &im->m_light_editor_drag_threshold, 0.1f, 0.0f, 32.0f, "%.1f px");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Radius Scale", &im->m_light_editor_gizmo_radius_scale, 0.001f, 0.0001f, 1.0f, "%.4f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Power Scale", &im->m_light_editor_gizmo_intensity_scale, 0.01f, 0.001f, 100.0f, "%.3f");
					ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(100.0f); ImGui::DragFloat("Rotate Scale", &im->m_light_editor_gizmo_rotate_scale, 0.001f, 0.0001f, 0.2f, "%.4f");
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		// point table helper - true when the user switched to a different light
		bool reset_point_selection = false;

		// default selection (when table not visible and selection empty)
		if (!ms_light_selection && !ms_lights.empty())
		{
			ms_light_selection = &ms_lights.front();
			reset_point_selection = true;
			rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
		}

		ImGui::Spacing(0, 8);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
		const bool quick_scene_tools_open = ImGui::CollapsingHeader("Selected light: quick tools", ImGuiTreeNodeFlags_DefaultOpen);
		ImGui::PopStyleVar();
		if (quick_scene_tools_open)
		{
			static map_settings::remix_light_settings_s* quick_anim_selection = nullptr;
			if (quick_anim_selection != ms_light_selection) quick_anim_selection = ms_light_selection;

			if (!ms_light_selection || ms_light_selection->points.empty())
			{
				ImGui::TextDisabled("Select a map light in the scene or Light Manager table.");
			}
			else
			{
				// Synchronize any open per-light editor before the quick controls mutate
				// the persistent definition. Never commit after changing module metadata,
				// because that would restore stale animation values over the user's choice.
				commit_active_editor_light_to_selection(lights, ms_light_selection);
				const auto& selected_point = ms_light_selection->points.front();
				ImGui::Text("%s", ms_light_selection->comment.empty() ? "Selected light" : ms_light_selection->comment.c_str());
				ImGui::SameLine();
				ImGui::TextDisabled("%s | %zu points | XYZ %.1f %.1f %.1f",
					ms_light_selection->generated_map_light ? "imported map light" :
						(ms_light_selection->generated_source_transient ? "frozen Source direct light" : "authored light"),
					ms_light_selection->points.size(), selected_point.position.x, selected_point.position.y, selected_point.position.z);
				if (!ms_light_selection->generated_source_kind.empty())
				{
					ImGui::TextDisabled("Source: %s | style %d | owner %d | key %d | %s",
						ms_light_selection->generated_source_kind.c_str(), ms_light_selection->generated_source_style,
						ms_light_selection->generated_source_owner, ms_light_selection->generated_source_key,
						ms_light_selection->generated_source_live_link ? "live-linked" :
						(ms_light_selection->generated_source_transient ? "frozen snapshot" : "static import"));
				}

				migrate_legacy_light_animation_metadata(*ms_light_selection);
				int quick_output_index = light_animation_index(LIGHT_OUTPUT_ANIMATION_PRESETS,
					IM_ARRAYSIZE(LIGHT_OUTPUT_ANIMATION_PRESETS), ms_light_selection->property_animation);
				int quick_move_index = light_animation_index(LIGHT_MOVEMENT_ANIMATION_PRESETS,
					IM_ARRAYSIZE(LIGHT_MOVEMENT_ANIMATION_PRESETS), canonical_light_movement_name(ms_light_selection->movement_animation));
				bool quick_modules_changed = false;
				ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(145.0f));
				if (ImGui::Combo("Output##quick_output", &quick_output_index, LIGHT_OUTPUT_ANIMATION_PRESETS, IM_ARRAYSIZE(LIGHT_OUTPUT_ANIMATION_PRESETS)))
				{
					ms_light_selection->property_animation = LIGHT_OUTPUT_ANIMATION_PRESETS[quick_output_index];
					const auto defaults = add_light_animation_defaults_for(ms_light_selection->property_animation);
					ms_light_selection->property_animation_duration = defaults.duration;
					ms_light_selection->property_animation_speed = defaults.speed;
					ms_light_selection->property_animation_variation = defaults.variation;
					quick_modules_changed = true;
				}
				ImGui::SameLine();
				ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(145.0f));
				if (ImGui::Combo("Move##quick_move", &quick_move_index, LIGHT_MOVEMENT_ANIMATION_PRESETS, IM_ARRAYSIZE(LIGHT_MOVEMENT_ANIMATION_PRESETS)))
				{
					ms_light_selection->movement_animation = LIGHT_MOVEMENT_ANIMATION_PRESETS[quick_move_index];
					const auto defaults = add_light_animation_defaults_for(ms_light_selection->movement_animation);
					ms_light_selection->movement_animation_duration = defaults.duration;
					ms_light_selection->movement_animation_speed = defaults.speed;
					ms_light_selection->movement_animation_axis = defaults.axis;
					ms_light_selection->movement_animation_degrees = defaults.degrees;
					quick_modules_changed = true;
				}
				ImGui::SameLine();
				ImGui::SetNextItemWidth(ImGui::CalcWidgetWidthForChild(105.0f));
				quick_modules_changed |= ImGui::DragFloat("Base Power##quick_output", &ms_light_selection->property_animation_intensity, 0.05f, 0.0f, 100000.0f, "%.2f");
				if (quick_modules_changed)
				{
					auto base = ms_light_selection->points.front();
					base.radiance_scalar = ms_light_selection->property_animation_intensity;
					rebuild_split_light_animation(*ms_light_selection, &base);
					rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
					reset_point_selection = true;
				}

				const float quick_spacing = ImGui::GetStyle().ItemSpacing.x;
				const auto quick_button_size = ImVec2((ImGui::GetContentRegionAvail().x - quick_spacing * 3.0f) / 4.0f, 0.0f);
				if (ImGui::Button("Rebuild Modules", quick_button_size))
				{
					auto base = ms_light_selection->points.front();
					base.radiance_scalar = ms_light_selection->property_animation_intensity;
					rebuild_split_light_animation(*ms_light_selection, &base);
					rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
					reset_point_selection = true;
				}
				TT("Rebuilds independent movement and output modules from the stored Base Power.");
				ImGui::SameLine();
				if (ImGui::Button("Duplicate as Authored", quick_button_size))
				{
					commit_active_editor_light_to_selection(lights, ms_light_selection);
					auto duplicate = *ms_light_selection;
					duplicate.comment += duplicate.comment.empty() ? "copy" : " copy";
					clear_generated_map_light_identity(duplicate);
					ms_lights.emplace_back(std::move(duplicate));
					ms_light_selection = &ms_lights.back();
					quick_anim_selection = nullptr;
					reset_point_selection = true;
					rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
				}
				ImGui::SameLine();
				if (ImGui::Button("Delete from Editor", quick_button_size))
				{
					const auto erase_it = std::find_if(ms_lights.begin(), ms_lights.end(), [&](const auto& def) { return &def == ms_light_selection; });
					if (erase_it != ms_lights.end()) ms_lights.erase(erase_it);
					ms_light_selection = ms_lights.empty() ? nullptr : &ms_lights.front();
					quick_anim_selection = nullptr;
					reset_point_selection = true;
					rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
				}
				ImGui::SameLine();
				if (ImGui::Button("Save Map Light File", quick_button_size))
				{
					commit_active_editor_light_to_selection(lights, ms_light_selection);
					dynamic_lighting::save_light_editor_map_lights_to_config();
				}
			}
		}

		if (preview_options_changed) {
			rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
		}

		commit_active_editor_light_to_selection(lights, ms_light_selection);
		if (draw_all_authored_light_gizmos(lights, ms_lights, ms_light_selection, reset_point_selection)) {
			commit_active_editor_light_to_selection(lights, ms_light_selection);
		}

		//
		// LIGHT TABLE

		ImGui::Spacing(0, 16);
		ImGui::PushFont(common::imgui::font::BOLD_LARGE);
		ImGui::SeparatorText(" Light Manager / Selection ");
		ImGui::PopFont();

		ImGui::TableHeaderDropshadow();

		std::vector<std::size_t> light_display_order(ms_lights.size());
		std::iota(light_display_order.begin(), light_display_order.end(), 0u);
		if (light_table_sun_first)
		{
			std::stable_sort(light_display_order.begin(), light_display_order.end(), [&](const std::size_t lhs, const std::size_t rhs)
			{
				return is_direct_sun_light(ms_lights[lhs]) > is_direct_sun_light(ms_lights[rhs]);
			});
		}

		if (ImGui::BeginTable("LightTable", 16,
			ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ContextMenuInBody |
			ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoHostExtendY | ImGuiTableFlags_ScrollY, ImVec2(0, 134.0f)))
		{
			ImGui::TableSetupScrollFreeze(0, 1); // make top row always visible
			ImGui::TableSetupColumn("##num", ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoHide, 10.0f);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 112.0f);
			ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthStretch, 10.0f);
			ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthStretch, 50.0f);
			ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch, 150.0f);
			ImGui::TableSetupColumn("Anim", ImGuiTableColumnFlags_WidthStretch, 55.0f);
			ImGui::TableSetupColumn("Trig Choreo", ImGuiTableColumnFlags_WidthStretch, 80.0f);
			ImGui::TableSetupColumn("Trig Sound", ImGuiTableColumnFlags_WidthStretch, 10.0f);
			ImGui::TableSetupColumn("Delay Trig", ImGuiTableColumnFlags_WidthStretch, 16.0f);
			ImGui::TableSetupColumn("Always", ImGuiTableColumnFlags_WidthStretch, 10.0f);

			ImGui::TableSetupColumn("Kill Choreo", ImGuiTableColumnFlags_WidthStretch, 80.0f);
			ImGui::TableSetupColumn("Kill Sound", ImGuiTableColumnFlags_WidthStretch, 10.0f);
			ImGui::TableSetupColumn("Delay Kill", ImGuiTableColumnFlags_WidthStretch, 16.0f);

			ImGui::TableSetupColumn("Once", ImGuiTableColumnFlags_WidthStretch, 10.0f);
			ImGui::TableSetupColumn("Loop", ImGuiTableColumnFlags_WidthStretch, 10.0f);
			ImGui::TableSetupColumn("Smooth", ImGuiTableColumnFlags_WidthStretch, 10.0f);
			ImGui::TableHeadersRow();

			bool selection_matches_any_entry = false;
			for (const std::size_t i : light_display_order)
			{
				auto& l = ms_lights[i];

				if (light_table_enabled_only && !l.enabled) {
					continue;
				}
				if (light_table_grouped_only && l.group.empty()) {
					continue;
				}
				if (light_table_direct_only && !is_direct_sun_light(l)) {
					continue;
				}
				if (light_table_imported_only && !l.generated_map_light && !l.generated_source_transient) {
					continue;
				}
				if (!light_table_filter.empty())
				{
					const std::string searchable = std::string(light_type_name(l)) + " " + light_origin_badge(l) + " " +
						l.group + " " + l.comment + " " + l.generated_source_kind + " " + l.generated_classname + " " +
						l.animation + " " + l.trigger_choreo_name + " " + l.kill_choreo_name;
					if (!ui_filter_contains(searchable, light_table_filter)) {
						continue;
					}
				}

				const bool is_selected = ms_light_selection && ms_light_selection == &l;

				ImGui::TableNextRow();

				if (is_direct_sun_light(l))
				{
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
						ImGui::ColorConvertFloat4ToU32(ImVec4(0.28f, 0.19f, 0.03f, 0.82f)));
				}
				if (is_selected) { // handle row background color for selected entry
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));
				}

				// -
				ImGui::TableNextColumn();
				if (!is_selected) // only selectable if not selected
				{
					ImGui::Style_InvisibleSelectorPush(); // never show selection - we use tablebg
					if (ImGui::Selectable(utils::va("%d", static_cast<int>(i)), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
					{
						if (const auto edit_light = lights->get_first_active_light(); edit_light)
						{
							if (check_light_for_modifications(edit_light->m_def, *ms_light_selection, nullptr))
							{
								if (!ImGui::IsPopupOpen("Ignore Changes?", ImGuiPopupFlags_AnyPopup)) {
									ImGui::OpenPopup("Ignore Changes?", ImGuiPopupFlags_AnyPopup);
								}

								ms_light_selection_pending = &l;
							}

							// no modifications on old light -> select new one
							else {
								ms_light_selection = &l;
								ms_light_selection_pending = nullptr;
							}
						}
						else {
							ms_light_selection = &l;
						}
					}
					ImGui::Style_InvisibleSelectorPop();

					if (ImGui::IsItemHovered()) {
						ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.6f)));///*ImGui::GetColorU32(ImGuiCol_TableRowBgAlt)*/);
					}
				}
				else {
					ImGui::Text("%d", i); // if selected
				}

				// check if there is at least one valid selection
				if (ms_light_selection && ms_light_selection == &l)
				{
					// user selected a new light
					if (!is_selected)
					{
						reset_point_selection = true;
						selection_matches_any_entry = false;
						rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
					}
					else
					{
						selection_matches_any_entry = true;
					}
				}

				// type / origin
				ImGui::TableNextColumn();
				if (is_direct_sun_light(l)) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.30f, 1.0f));
				ImGui::Text("%s  %s", light_type_icon(l), light_type_name(l));
				if (is_direct_sun_light(l)) ImGui::SafePopStyleColor(1, __LINE__);
				ImGui::TextDisabled("%s", i == primary_sun_index ? "PRIMARY SUN" : light_origin_badge(l));

				// enabled
				ImGui::TableNextColumn();
				ui_status_pill(l.enabled ? "ON" : "off", l.enabled);

				// group
				ImGui::TableNextColumn();
				ImGui::TextUnformatted_ClippedByColumnTooltip(l.group.c_str());

				// comment
				ImGui::TableNextColumn();
				ImGui::TextUnformatted_ClippedByColumnTooltip(l.comment.c_str());

				// animation
				ImGui::TableNextColumn();
				ImGui::TextUnformatted_ClippedByColumnTooltip((l.animation.empty() || l.animation == "stable") ? "" : l.animation.c_str());

				// trig choreo
				ImGui::TableNextColumn();
				ImGui::TextUnformatted_ClippedByColumnTooltip(l.trigger_choreo_name.c_str());

				// trig sound
				ImGui::TableNextColumn();
				ImGui::Text("%s", l.trigger_sound_hash ? "x" : ""); //ImGui::Text("%#x", l.trigger_sound_hash);

				// trig delay
				ImGui::TableNextColumn();
				ImGui::Text("%.1f", l.trigger_delay);

				// trig always
				ImGui::TableNextColumn();
				ImGui::Text("%s", l.trigger_always ? "x" : "");

				// kill choreo
				ImGui::TableNextColumn();
				ImGui::TextUnformatted_ClippedByColumnTooltip(l.kill_choreo_name.c_str());

				// kill sound
				ImGui::TableNextColumn();
				ImGui::Text("%s", l.kill_sound_hash ? "x" : ""); //ImGui::Text("%#x", l.kill_sound_hash);

				// kill delay
				ImGui::TableNextColumn();
				ImGui::Text("%.1f", l.kill_delay);

				// once
				ImGui::TableNextColumn();
				ImGui::Text("%s", l.run_once ? "x" : "");

				// loop
				ImGui::TableNextColumn();
				ImGui::Text("%s", l.loop ? "x" : "");

				// smooth
				ImGui::TableNextColumn();
				ImGui::Text("%s", l.loop_smoothing ? "x" : "");
			} // end for loop

			// no valid selection found in the last loop
			// re-check because user might have selected something new using the selectable
			if (!selection_matches_any_entry)
			{
				for (auto& l : ms_lights)
				{
					if (ms_light_selection && ms_light_selection == &l)
					{
						selection_matches_any_entry = true;
						break;
					}
				}

				// reset selection ptr if no valid selection was found
				if (!selection_matches_any_entry)
				{
					if (!ms_lights.empty()) {
						ms_light_selection = &ms_lights.front();
					}
					else {
						ms_light_selection = nullptr;
					}
				}
			}

			ImGui::EndTable();
		} // table end



		if (ms_light_selection)
		{
			// create/recreate editor preview lights when needed. Selected light is always inserted first.
			if (!lights->get_active_light_count()) {
				rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
			}

			if (auto edit_light = lights->get_first_active_light();
				edit_light)
			{
				// debug text
				Vector lpos = &edit_light->m_ext.position.x;
				Vector screen_pos = {};
				if (common::imgui::world2screen(lpos, screen_pos)) {
					ImGui::GetForegroundDrawList()->AddCircleFilled(ImVec2(screen_pos.x, screen_pos.y), 4.0f, ImGui::GetColorU32(ImGuiCol_Text));
				}
				game::debug_add_text_overlay(&lpos.x, "  Selected Light", -1, 0.8f, 0.8f, 0.8f, 0.8f);
			}
		}


		ImGui::Spacing(0, 0);
		ImGui::Style_ColorButtonPush(im->ImGuiCol_ButtonGreen, true);
		if (ImGui::Button("Add Light", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0)))
		{
			// write modified params into the current mapsetting light so changes dont get lost
			if (auto edit_light = lights->get_first_active_light(); edit_light && ms_light_selection)
			{
				auto temp_def = edit_light->m_def;
				*ms_light_selection = temp_def;
			}

			map_settings::remix_light_settings_s::point_s pt =
			{
				.position = *game::get_current_view_origin(),
				.radiance = { 10.0f, 10.0f, 10.0f },
				.radiance_scalar = 1.0f,
				.radius = 1.0f,
				.timepoint = 0.0f,
				.smoothness = 0.5f,
				.use_shaping = false,
			};

			ms_lights.push_back(
				map_settings::remix_light_settings_s
				{
					.points = { pt },
					.run_once = false,
					.loop = true,
					.loop_smoothing = false,
					.trigger_always = false,
					.trigger_choreo_name = "",
					.trigger_choreo_actor = "",
					.trigger_choreo_event = "",
					.trigger_choreo_param1 = "",
					.trigger_sound_hash = 0u,
					.trigger_delay = 0.0f,
					.kill_choreo_name = "",
					.kill_sound_hash = 0u,
					.kill_delay = 0.0f,
					.enabled = true,
					.group = "authoring"
				});

			ms_light_selection = &ms_lights.back();
			reset_point_selection = true;

			rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
		}
		ImGui::Style_ColorButtonPop();

		ImGui::BeginDisabled(!ms_light_selection);
		{
			ImGui::SameLine();
			ImGui::Style_ColorButtonPush(im->ImGuiCol_ButtonRed, true);
			if (ImGui::Button("Delete Selected Light", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
			{
				for (auto it = ms_lights.begin(); it != ms_lights.end(); ++it)
				{
					if (&*it == ms_light_selection)
					{
						ms_lights.erase(it);
						reset_point_selection = true;
						ms_light_selection = !ms_lights.empty() ? &ms_lights.front() : nullptr;
						rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
						break;
					}
				}
			}
			ImGui::Style_ColorButtonPop();

			//ImGui::Style_ColorButtonPush(imgui::get()->ImGuiCol_ButtonYellow, true);
			if (ImGui::Button("Duplicate Selected Light", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0)) && ms_light_selection)
			{
				// write modified params into the current mapsetting light so changes dont get lost
				if (auto edit_light = lights->get_first_active_light(); edit_light)
				{
					*ms_light_selection = edit_light->m_def;
				}

				ms_lights.push_back(*ms_light_selection);
				ms_light_selection = &ms_lights.back();
				reset_point_selection = true;
				rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
			} TT("Duplicates the currently selected light.");

			ImGui::SameLine();
			if (ImGui::Button("Stamp Copy To Camera", ImVec2(ImGui::GetContentRegionAvail().x, 0)) && ms_light_selection)
			{
				if (auto edit_light = lights->get_first_active_light(); edit_light)
				{
					*ms_light_selection = edit_light->m_def;
				}

				auto stamped = *ms_light_selection;
				if (!stamped.points.empty())
				{
					const Vector delta = *game::get_current_view_origin() - stamped.points.front().position;
					for (auto& point : stamped.points) {
						point.position = point.position + delta;
					}
				}
				if (!stamped.comment.empty()) { stamped.comment += " [stamp]"; }
				else { stamped.comment = "stamped light"; }

				ms_lights.push_back(std::move(stamped));
				ms_light_selection = &ms_lights.back();
				reset_point_selection = true;
				rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
			} TT("Paint-mode style workflow: duplicates the selected light and moves the copy so its first point lands at the current camera position.");

			//ImGui::Style_ColorButtonPop();

			const bool can_teleport_to_light = ms_light_selection && !ms_light_selection->points.empty();
			if (ImGui::Button("TP to Light", ImVec2(ImGui::GetContentRegionAvail().x, 0)) && can_teleport_to_light)
			{
				const auto& position = ms_light_selection->points.front().position;
				interfaces::get()->m_engine->execute_client_cmd_unrestricted(utils::va("sv_cheats 1; noclip; setpos %.2f %.2f %.2f", position.x, position.y, position.z - 40.0f));
			}

			ImGui::EndDisabled();
		}

		if (auto edit_active_light = lights->get_first_active_light();
			edit_active_light && ms_light_selection)
		{
			//ImGui::Spacing(0, 8);
			mapsettings_ls_general_light_settings(edit_active_light);

			ImGui::Spacing(0, 8);

			const auto is_static_light_with_single_point = !edit_active_light->m_mover.is_initialized();
			// ---
			// holds mover points OR def point if light is static
			static map_settings::remix_light_settings_s::point_s* active_point_selection = nullptr;

			map_settings::remix_light_settings_s::point_s* active_points = nullptr;
			size_t active_points_count = 0u;

			// Editor source of truth: always edit authored m_def.points.
				// The mover owns a runtime/interpolation copy only, so editing mover points directly caused
				// Apply/Rebuild to overwrite or lose settings.
				if (!edit_active_light->m_def.points.empty())
				{
					active_points = edit_active_light->m_def.points.data();
					active_points_count = edit_active_light->m_def.points.size();
				}

				const bool selection_ptr_valid = active_point_selection && active_points &&
					active_point_selection >= active_points && active_point_selection < (active_points + active_points_count);
				if (active_points_count == 0u) {
					active_point_selection = nullptr;
				}
				else if (reset_point_selection || !selection_ptr_valid) {
					active_point_selection = &active_points[0]; // default selection
				}

			draw_light_workbench(edit_active_light, active_point_selection, is_static_light_with_single_point);

			// Light Manager may rebuild procedural animation points and reallocate m_def.points.
			// Refresh the authored point span immediately so the rest of the UI never touches stale mover/runtime pointers.
			if (edit_active_light && !edit_active_light->m_def.points.empty())
			{
				active_points = edit_active_light->m_def.points.data();
				active_points_count = edit_active_light->m_def.points.size();
				const bool workbench_selection_valid = active_point_selection &&
					active_point_selection >= active_points && active_point_selection < (active_points + active_points_count);
				if (!workbench_selection_valid) {
					active_point_selection = &active_points[0];
				}
			}
			else
			{
				active_points = nullptr;
				active_points_count = 0u;
				active_point_selection = nullptr;
			}

			const bool show_advanced_keyframes = edit_active_light->m_def.points.size() > 1u ||
				(!edit_active_light->m_def.animation.empty() && edit_active_light->m_def.animation != "stable" &&
				 edit_active_light->m_def.animation != "none" && edit_active_light->m_def.animation != "off");

			if (show_advanced_keyframes && ImGui::CollapsingHeader("Advanced Animation Keyframes"))
			{
			ImGui::Spacing(0, 16);
			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			ImGui::SeparatorText(" Light Points ");
			ImGui::PopFont();

			ImGui::TableHeaderDropshadow();

			if (ImGui::BeginTable("PointTable", 12,
				ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ContextMenuInBody |
				ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_ScrollY, ImVec2(0, 134.0f)))
			{
				ImGui::TableSetupScrollFreeze(0, 1); // make top row always visible
				ImGui::TableSetupScrollFreeze(0, 1); // make top row always visible
				ImGui::TableSetupColumn("##num", ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoHide, 10.0f);
				ImGui::TableSetupColumn("Timepoint", ImGuiTableColumnFlags_WidthStretch, 30.0f);
				ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthStretch, 100.0f);
				ImGui::TableSetupColumn("Radiance", ImGuiTableColumnFlags_WidthStretch, 60.0f);
				ImGui::TableSetupColumn("Scalar", ImGuiTableColumnFlags_WidthStretch, 30.0f);
				ImGui::TableSetupColumn("Radius", ImGuiTableColumnFlags_WidthStretch, 30.0f);
				ImGui::TableSetupColumn("Smooth", ImGuiTableColumnFlags_WidthStretch, 30.0f);
				ImGui::TableSetupColumn("Shaping", ImGuiTableColumnFlags_WidthStretch, 14.0f);
				ImGui::TableSetupColumn("Direction", ImGuiTableColumnFlags_WidthStretch, 70.0f);
				ImGui::TableSetupColumn("Degrees", ImGuiTableColumnFlags_WidthStretch, 30.0f);
				ImGui::TableSetupColumn("Soft", ImGuiTableColumnFlags_WidthStretch, 30.0f);
				ImGui::TableSetupColumn("Expo", ImGuiTableColumnFlags_WidthStretch, 30.0f);
				ImGui::TableHeadersRow();

				bool selection_matches_any_entry = false;
				for (size_t i = 0u; i < active_points_count; i++)
				{
					auto& p = active_points[i];

					// default selection
					if (!active_point_selection) {
						active_point_selection = &p;
					}

					ImGui::TableNextRow();
					// handle row background color for selected entry
					const bool is_selected = active_point_selection && active_point_selection == &p;
					if (is_selected) {
						ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));
					}

					// -
					ImGui::TableNextColumn();
					if (!is_selected) // only selectable if not selected
					{
						ImGui::Style_InvisibleSelectorPush(); // never show selection - we use tablebg
						if (ImGui::Selectable(utils::va("%d", i), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
							active_point_selection = &p;
						}
						ImGui::Style_InvisibleSelectorPop();

						if (ImGui::IsItemHovered()) {
							ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.6f)));///*ImGui::GetColorU32(ImGuiCol_TableRowBgAlt)*/);
						}
					}
					else {
						ImGui::Text("%d", i); // if selected
					}

					if (active_point_selection && active_point_selection == &p) {
						selection_matches_any_entry = true; // check that the selection ptr is up to date
					}

					// timepoint
					ImGui::TableNextColumn();
					ImGui::Text("%.2f", p.timepoint);

					// pos
					ImGui::TableNextColumn();
					ImGui::TextUnformatted_ClippedByColumnTooltip(utils::va("%.0f, %.0f, %.0f", p.position.x, p.position.y, p.position.z));

					// rad
					ImGui::TableNextColumn();
					ImGui::TextUnformatted_ClippedByColumnTooltip(utils::va("%.0f, %.0f, %.0f", p.radiance.x, p.radiance.y, p.radiance.z));

					// scalar
					ImGui::TableNextColumn();
					ImGui::Text("%.1f", p.radiance_scalar);

					// radius
					ImGui::TableNextColumn();
					ImGui::Text("%.2f", p.radius);

					// smooth
					ImGui::TableNextColumn();
					ImGui::Text("%.2f", p.smoothness);

					// shaping
					ImGui::TableNextColumn();
					ImGui::Text("%s", p.use_shaping ? "x" : "");

					// dir
					ImGui::TableNextColumn();
					ImGui::TextUnformatted_ClippedByColumnTooltip(utils::va("%.2f, %.2f, %.2f", p.direction.x, p.direction.y, p.direction.z));

					// deg
					ImGui::TableNextColumn();
					ImGui::Text("%.1f", p.degrees);

					// soft
					ImGui::TableNextColumn();
					ImGui::Text("%.2f", p.softness);

					// smooth
					ImGui::TableNextColumn();
					ImGui::Text("%.2f", p.smoothness);
				}

				if (!selection_matches_any_entry) {
					active_point_selection = nullptr;
				}

				ImGui::EndTable();
			} // ----------------------------------------

			ImGui::Spacing(0, 0);

			ImGui::Style_ColorButtonPush(im->ImGuiCol_ButtonGreen, true);
			if (ImGui::Button("Add Point", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0)))
				{
					// m_def.points are the editor source of truth. Do not copy from m_mover;
					// the mover is a runtime interpolation copy only.
					auto new_def = edit_active_light->m_def;
					if (!new_def.points.empty())
					{
						const std::uint32_t prev_point_index = static_cast<std::uint32_t>(new_def.points.size() - 1u);
						new_def.points.push_back(new_def.points[prev_point_index]);
						new_def.points.back().timepoint += 2.0f;
					}

					edit_active_light->m_def = std::move(new_def);
					active_points = edit_active_light->m_def.points.data();
					active_points_count = edit_active_light->m_def.points.size();
					active_point_selection = active_points_count ? &active_points[active_points_count - 1u] : nullptr;
					if (active_point_selection) {
						rebuild_edit_light_runtime_from_def(edit_active_light, active_points_count - 1u);
					}
				}
				ImGui::Style_ColorButtonPop();

			ImGui::BeginDisabled(!active_point_selection
					|| active_points_count <= 1u
					|| active_point_selection == active_points); // if first point
				{
					ImGui::SameLine();
					ImGui::Style_ColorButtonPush(im->ImGuiCol_ButtonRed, true);
					if (ImGui::Button("Delete Selected Point", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
					{
						auto new_def = edit_active_light->m_def;
						std::ptrdiff_t pt_index = active_point_selection - active_points;

						if ((size_t)pt_index < new_def.points.size()) {
							new_def.points.erase(new_def.points.begin() + pt_index);
						}

						edit_active_light->m_def = std::move(new_def);
						active_points = edit_active_light->m_def.points.data();
						active_points_count = edit_active_light->m_def.points.size();
						const size_t next_index = active_points_count ? std::min(static_cast<size_t>(std::max<std::ptrdiff_t>(0, pt_index - 1)), active_points_count - 1u) : 0u;
						active_point_selection = active_points_count ? &active_points[next_index] : nullptr;
						if (active_point_selection) {
							rebuild_edit_light_runtime_from_def(edit_active_light, next_index);
						}
					}
					ImGui::Style_ColorButtonPop();
					ImGui::EndDisabled();
			}

			ImGui::Spacing(0, 4);

			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
			ImGui::TableHeaderDropshadow();
			const bool light_transform_state = ImGui::CollapsingHeader("Light Transform (all points)");
			ImGui::PopStyleVar();

			if (light_transform_state)
			{
				//const auto cont_bg_color = im->ImGuiCol_ContainerBackground + ImVec4(0.05f, 0.05f, 0.05f, 0.0f);
				//static float cont_height = 0.0f;
				//cont_height = ImGui::Widget_ContainerWithDropdownShadow(cont_height, [active_points, edit_active_light, is_static_light_with_single_point]
				{
					if (ImGui::Button("Move Whole Light To Camera", ImVec2(ImGui::CalcWidgetWidthForChild(150.0f), 0)))
					{
						move_edit_light_center_to_position(edit_active_light, *game::get_current_view_origin());
						active_points = edit_active_light->m_def.points.data();
						active_points_count = edit_active_light->m_def.points.size();
						active_point_selection = ui_light_point_by_index(edit_active_light, 0u);
					}
					TT("Moves every authored keyframe by the same offset. This preserves the whole animation/IES setup.");

					static float offs_step_offset = 0.5f;

					float offs[3] = {};
					if (ImGui::Widget_PrettyStepVec3("Offset Whole Light", offs, true, 80.0f, offs_step_offset))
					{
						translate_edit_light_points(edit_active_light, Vector(offs[0], offs[1], offs[2]));
						active_points = edit_active_light->m_def.points.data();
						active_points_count = edit_active_light->m_def.points.size();
						active_point_selection = ui_light_point_by_index(edit_active_light, ui_light_point_index_from_ptr(edit_active_light, active_point_selection));
					}

					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					if (ImGui::DragFloat("Step Offset", &offs_step_offset, 0.005f, 0.0f, 100.0f)) {
						offs_step_offset = std::clamp(offs_step_offset, 0.0f, 100.0f);
					}

					ImGui::Spacing(0, 4);
					ImGui::Separator();
					ImGui::Spacing(0, 4);

				}//, &cont_bg_color, &im->ImGuiCol_ContainerBorder);
			}

			ImGui::Spacing(0, 8);

			// -------

			if (active_point_selection && active_points && active_points_count > 0u)
			{
				std::size_t pt_index = 0u;
				if (!is_static_light_with_single_point)
				{
					const auto raw_index = active_point_selection - active_points;
					if (raw_index > 0) {
						pt_index = std::min(static_cast<std::size_t>(raw_index), active_points_count - 1u);
					}
				}

				bool selected_point_changed = false;
				selected_point_changed |= ImGui::Widget_PrettyDragVec3("Position", &active_point_selection->position.x, true, 120.0f, 0.25f);

				Vector normalized_radiance = im->m_debugvis_live ? &edit_active_light->m_info.radiance.x : active_point_selection->radiance;
				normalized_radiance.Normalize();

				//const auto debug_color_bg = ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
				const auto debug_color = ImGui::ColorConvertFloat4ToU32(ImVec4(normalized_radiance.x, normalized_radiance.y, normalized_radiance.z, 1.0f));

				// draw position as circle
				Vector screen_pos = {};
				const bool selected_point_projected = common::imgui::world2screen(
					(im->m_debugvis_live ? &edit_active_light->m_ext.position.x : edit_active_light->calculate_position_for_point(active_point_selection)), screen_pos);
				if (selected_point_projected) {
					ImGui::GetForegroundDrawList()->AddCircleFilled(ImVec2(screen_pos.x, screen_pos.y), 8.0f, debug_color);
				}

				if (im->m_debugvis_light_labels && selected_point_projected)
				{
					const float label_radius = im->m_debugvis_live ? edit_active_light->m_ext.radius : active_point_selection->radius;
					const float label_scale = active_point_selection->radiance_scalar;
					const auto rig_mode = active_point_selection->light_rig_mode;
					const char* label = nullptr;
					if (rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES)
					{
						const char* profile = active_point_selection->ies_file.empty() ? "missing" : active_point_selection->ies_file.c_str();
						label = utils::va("r %.2f | scale %.2f | Native IES %s", label_radius, label_scale, profile);
					}
					else if (rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES)
					{
						const char* profile = active_point_selection->ies_profile.empty() ? "custom" : active_point_selection->ies_profile.c_str();
						const char* pattern = active_point_selection->ies_emulation_pattern.empty() ? "spiral" : active_point_selection->ies_emulation_pattern.c_str();
						const int helpers = active_point_selection->ies_emulation ? active_point_selection->ies_emulation_samples : 0;
						label = utils::va("r %.2f | scale %.2f | Fake IES %s | helpers %d/%s", label_radius, label_scale, profile, helpers, pattern);
					}
					else
					{
						label = utils::va("r %.2f | scale %.2f | Legacy", label_radius, label_scale);
					}
					ImGui::GetForegroundDrawList()->AddText(ImVec2(screen_pos.x + 12.0f, screen_pos.y - 12.0f), debug_color, label);
				}


				selected_point_changed |= ImGui::Widget_PrettyDragVec3("Radiance", &active_point_selection->radiance.x, true, 120.0f, 0.1f, 0.0f, FLT_MAX,
					"R", "G", "B");

				SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
				if (ImGui::DragFloat("Radiance Scale", &active_point_selection->radiance_scalar, 0.1f, 0.0f, FLT_MAX, "%.1f")) {
					active_point_selection->radiance_scalar = active_point_selection->radiance_scalar < 0.0f ? 0.0f : active_point_selection->radiance_scalar;
						selected_point_changed = true;
				} TT("General radiance scalar");


				SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
				if (ImGui::DragFloat("Radius", &active_point_selection->radius, 0.005f, 0.0f, FLT_MAX, "%.2f")) {
					active_point_selection->radius = active_point_selection->radius < 0.0f ? 0.0f : active_point_selection->radius;
						selected_point_changed = true;
				} TT("Radius of light (defaults to 1.0)");

				SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
				if (ImGui::DragFloat("Volumetric Scale", &active_point_selection->volumetric_scale, 0.005f, 0.0f, 20.0f, "%.2f")) {
					active_point_selection->volumetric_scale = active_point_selection->volumetric_scale < 0.0f ? 0.0f : active_point_selection->volumetric_scale;
						selected_point_changed = true;
				} TT("Volumetric Radiance Scale of light (defaults to 1.0)");

				//ImGui::Draw3DCircle(ImGui::GetBackgroundDrawList(), &edit_active_light->ext.position.x, Vector(0.0f, 0.0f, 1.0f), active_point_selection->radius, false, debug_color, 2.0f);
				//ImGui::Draw3DCircle(ImGui::GetBackgroundDrawList(), &edit_active_light->ext.position.x, Vector(0.0f, 1.0f, 0.0f), active_point_selection->radius, false, debug_color, 2.0f);
				//ImGui::Draw3DCircle(ImGui::GetBackgroundDrawList(), &edit_active_light->ext.position.x, Vector(1.0f, 0.0f, 0.0f), active_point_selection->radius, false, debug_color, 2.0f);

				if (im->m_debugvis_radius && remix_api::is_initialized())
				{
					const Vector circle_pos = (im->m_debugvis_live ? &edit_active_light->m_ext.position.x : edit_active_light->calculate_position_for_point(active_point_selection));
					const float radius = im->m_debugvis_live ? edit_active_light->m_ext.radius : active_point_selection->radius;

					const auto remixapi = remix_api::get();
					remixapi->add_debug_circle(circle_pos, Vector(0.0f, 0.0f, 1.0f), radius - 0.02f, radius * 0.1f, normalized_radiance);
					remixapi->add_debug_circle_based_on_previous(circle_pos, Vector(0, 90, 0), Vector(1.0f, 1.0f, 1.0f));
					remixapi->add_debug_circle_based_on_previous(circle_pos, Vector(90, 0, 90), Vector(1.0f, 1.0f, 1.0f));
				}

				// cant edit time of first point

				if (im->m_debugvis_ies_cluster && remix_api::is_initialized() &&
					active_point_selection->light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES &&
					active_point_selection->ies_emulation && active_point_selection->ies_emulation_samples > 0)
				{
					auto normalize_debug = [](Vector value, const Vector& fallback) -> Vector
					{
						if (value.LengthSqr() <= 0.0001f) { value = fallback; }
						if (value.LengthSqr() <= 0.0001f) { value = Vector(0.0f, 0.0f, -1.0f); }
						value.NormalizeChecked();
						return value;
					};

					Vector cluster_dir = im->m_debugvis_live
						? Vector(&edit_active_light->m_ext.shaping_value.direction.x)
						: edit_active_light->calculate_direction_for_point(active_point_selection);
					cluster_dir = normalize_debug(cluster_dir, Vector(0.0f, 0.0f, -1.0f));

					Vector cluster_up = Vector(0.0f, 0.0f, 1.0f);
					if (std::fabs(cluster_dir.Dot(cluster_up)) > 0.96f) { cluster_up = Vector(1.0f, 0.0f, 0.0f); }
					Vector cluster_right = cluster_dir.Cross(cluster_up);
					cluster_right.NormalizeChecked();
					cluster_up = cluster_right.Cross(cluster_dir);
					cluster_up.NormalizeChecked();

					const Vector cluster_base = (im->m_debugvis_live ? &edit_active_light->m_ext.position.x : edit_active_light->calculate_position_for_point(active_point_selection));
					const float cluster_radius = std::max(0.001f, im->m_debugvis_live ? edit_active_light->m_ext.radius : active_point_selection->radius);
					const float cluster_spread = std::max(0.0f, active_point_selection->ies_emulation_spread) * cluster_radius;
					const float child_radius = cluster_radius * std::clamp(active_point_selection->ies_emulation_radius_scale, 0.01f, 4.0f);
					const float forward_offset = active_point_selection->ies_emulation_forward_offset * cluster_radius;
					const int samples = std::clamp(active_point_selection->ies_emulation_samples, 0, 24);
					const float aspect = std::clamp(active_point_selection->ies_emulation_aspect, 0.1f, 8.0f);
					const float twist = active_point_selection->ies_emulation_twist * 0.01745329252f;

					auto cluster_lateral_offset = [](const std::string& raw_pattern, const int index, const int count, const float spread_value, const float aspect_value, const float twist_rad) -> Vector
					{
						const float safe_count = static_cast<float>(std::max(count, 1));
						const float golden_angle = 2.39996323f;
						const float two_pi = 6.28318531f;
						const float t = (static_cast<float>(index) + 0.5f) / safe_count;
						float ring = std::sqrt(std::clamp(t, 0.0f, 1.0f));
						float x = 0.0f, y = 0.0f;
						auto pattern = utils::str_to_lower(raw_pattern.empty() ? "spiral" : raw_pattern);
						utils::replace_all(pattern, "-", "_");

						if (pattern == "ring" || pattern == "donut")
						{
							const float angle = two_pi * static_cast<float>(index) / safe_count;
							x = std::cos(angle) * spread_value;
							y = std::sin(angle) * spread_value;
						}
						else if (pattern == "line" || pattern == "tube")
						{
							const float denom = std::max(1.0f, safe_count - 1.0f);
							const float u = count <= 1 ? 0.0f : (static_cast<float>(index) / denom) * 2.0f - 1.0f;
							x = u * spread_value;
							y = 0.0f;
						}
						else if (pattern == "cross" || pattern == "plus")
						{
							const int arm = index & 3;
							const float arm_step = static_cast<float>(index / 4 + 1) / std::max(1.0f, std::ceil(safe_count / 4.0f));
							const float dist = std::clamp(arm_step, 0.0f, 1.0f) * spread_value;
							x = (arm == 0 ? dist : arm == 1 ? -dist : 0.0f);
							y = (arm == 2 ? dist : arm == 3 ? -dist : 0.0f);
						}
						else
						{
							const float angle = static_cast<float>(index) * golden_angle;
							const float lateral = ring * spread_value * (pattern == "beam" || pattern == "hotspot" ? 0.75f : 1.0f);
							x = std::cos(angle) * lateral * (pattern == "beam" || pattern == "hotspot" ? 0.45f : 1.0f);
							y = std::sin(angle) * lateral;
						}

						y *= aspect_value;
						return Vector(x * std::cos(twist_rad) - y * std::sin(twist_rad), x * std::sin(twist_rad) + y * std::cos(twist_rad), 0.0f);
					};

					for (int i = 0; i < samples; ++i)
					{
						const Vector local = cluster_lateral_offset(active_point_selection->ies_emulation_pattern, i, samples, cluster_spread, aspect, twist);
						const Vector child_pos = cluster_base + cluster_right * local.x + cluster_up * local.y + cluster_dir * forward_offset;
						remix_api::get()->add_debug_circle(child_pos, cluster_dir, std::max(0.03f, child_radius * 0.18f), std::max(0.01f, child_radius * 0.035f), normalized_radiance, false);
					}
				}
				ImGui::BeginDisabled(!pt_index);
				{
					// get min and max timepoint for current point
					float min_timepoint = 0.0f, max_timepoint = FLT_MAX;
					if (pt_index > 0u) {
						min_timepoint = edit_active_light->m_def.points[pt_index - 1u].timepoint + 0.1f; // at least 0.1 diff
					}

					if (pt_index + 1u < edit_active_light->m_def.points.size()) {
						max_timepoint = edit_active_light->m_def.points[pt_index + 1u].timepoint - 0.1f; // at least 0.1 diff
					}

					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					if (ImGui::DragFloat("Timepoint", &active_point_selection->timepoint, 0.005f, min_timepoint, max_timepoint, "%.3f"))
					{
						active_point_selection->timepoint = std::clamp(active_point_selection->timepoint, min_timepoint, max_timepoint);
						selected_point_changed = true;
					}
					TT("Time in seconds at which the light arrives at the point\n"
						"Last point defines the total duration.");

					ImGui::EndDisabled();
				}

				SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
				if (ImGui::DragFloat("Smoothness", &active_point_selection->smoothness, 0.005f, 0.0f, 3.0f, "%.2f")) {
					active_point_selection->smoothness = active_point_selection->smoothness < 0.0f ? 0.0f : active_point_selection->smoothness;
						selected_point_changed = true;
				} TT("Curve smoothness (defaults to 0.5 - values above 1 might produce odd results)");

				ImGui::Spacing(0, 8);

				bool light_shaping_state = active_point_selection->use_shaping;
				if (ImGui::Checkbox("Use Light Shaping", &active_point_selection->use_shaping))
				{
					selected_point_changed = true;
					if (!active_point_selection->use_shaping)
					{
						active_point_selection->direction.x = 0.0f;
						active_point_selection->direction.y = 0.0f;
						active_point_selection->direction.z = 1.0f;
						active_point_selection->degrees = 180.0f;
					}

					// was just toggled on -> set default val
					if (active_point_selection->use_shaping && light_shaping_state != active_point_selection->use_shaping) {
						active_point_selection->degrees = 90.0f;
					}
				}

				if (active_point_selection->use_shaping)
				{
					const bool is_attached = edit_active_light->is_attached();
					if (!is_attached)
					{
						if (ImGui::Widget_PrettyDragVec3("Direction", &active_point_selection->direction.x, true, 120.0f, 0.1f, -1.0f, 1.0f)) {
							active_point_selection->direction.Normalize();
						selected_point_changed = true;
						} TT("Direction of the light");
					}
					else
					{
						if (ImGui::Widget_PrettyDragVec3("Direction (Offset)", &active_point_selection->angle_offset_attached.x, true, 120.0f, 0.1f, -180.0f, 180.0f))
						{
							utils::vector::angle_normalize(active_point_selection->angle_offset_attached);
							utils::vector::AngleVectors(active_point_selection->angle_offset_attached, &active_point_selection->direction);
						selected_point_changed = true;
						} TT("Offset the light direction with reference to the attached bone / entity angles. Offset in Euler Angles.");
					}

					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					if (ImGui::DragFloat("Degrees", &active_point_selection->degrees, 0.25f, 0.0f, 180.0f, "%.1f")) {
						active_point_selection->degrees = std::clamp(active_point_selection->degrees, 0.0f, 180.0f);
						selected_point_changed = true;
					} TT("Cone Angle");

					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					if (ImGui::DragFloat("Softness", &active_point_selection->softness, 0.005f, 0.0f, 1.0f, "%.1f")) {
						active_point_selection->softness = std::clamp(active_point_selection->softness, 0.0f, 1.0f);
						selected_point_changed = true;
					} TT("Cone Softness");

					SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
					if (ImGui::DragFloat("Exponent", &active_point_selection->exponent, 0.005f, 0.0f, 1.0f, "%.1f")) {
						active_point_selection->exponent = std::clamp(active_point_selection->exponent, 0.0f, 1.0f);
						selected_point_changed = true;
					} TT("Cone Focus Exponent");

					if (im->m_debugvis_shaping && remix_api::is_initialized())
					{
						const auto remixapi = remix_api::get();
						const float cone_deg = im->m_debugvis_live ? edit_active_light->m_ext.shaping_value.coneAngleDegrees : active_point_selection->degrees;

						if (cone_deg <= 90.0f)
						{
							const float cone_rad_tan = std::tan(DEG2RAD(cone_deg));
							const float scaled_height = im->m_debugvis_cone_height / (1.0f + cone_rad_tan);

							// draw cone
							for (auto i = 1; i <= im->m_debugvis_cone_steps; ++i)
							{
								const float step_fraction = (float)i / (float)im->m_debugvis_cone_steps;

								float radius = (step_fraction * scaled_height) * cone_rad_tan;

								Vector circle_pos =
									(im->m_debugvis_live ? Vector(&edit_active_light->m_ext.position.x) + Vector(&edit_active_light->m_ext.shaping_value.direction.x) * (step_fraction * scaled_height)
										: edit_active_light->calculate_position_for_point(active_point_selection) + edit_active_light->calculate_direction_for_point(active_point_selection) * (step_fraction * scaled_height)) /*+ edit_active_light->attached_position*/;

								remixapi->add_debug_circle(
									circle_pos,
									im->m_debugvis_live ? &edit_active_light->m_ext.shaping_value.direction.x
									: edit_active_light->calculate_direction_for_point(active_point_selection),
									radius, radius * 0.025f, normalized_radiance, false);
							}
						}

						// draw dir line
						remixapi->add_debug_line(
							im->m_debugvis_live ? &edit_active_light->m_ext.position.x
							: edit_active_light->calculate_position_for_point(active_point_selection),

							im->m_debugvis_live ? Vector(&edit_active_light->m_ext.position.x) + Vector(&edit_active_light->m_ext.shaping_value.direction.x).Scale(im->m_debugvis_cone_height)
							: edit_active_light->calculate_position_for_point(active_point_selection) + edit_active_light->calculate_direction_for_point(active_point_selection).Scale(im->m_debugvis_cone_height), //active_point_selection->direction.Scale(im->m_debugvis_cone_height),
							1.0f, remix_api::WHITE);
					}
				} // end use shaping

				ImGui::Spacing(0, 8);
				mapsettings_ls_playback_visualization_settings(edit_active_light, is_static_light_with_single_point);
				ImGui::Spacing(0, 4);

				if (selected_point_changed)
				{
					rebuild_edit_light_runtime_from_def(edit_active_light, static_cast<size_t>(pt_index));
					active_points = edit_active_light->m_def.points.data();
					active_points_count = edit_active_light->m_def.points.size();
					active_point_selection = ui_light_point_by_index(edit_active_light, static_cast<size_t>(pt_index));
				}
			} // end if active_point_selection
			} // end Advanced Animation Keyframes


			// popup frame
			ImGui::PushID("LightTable");
			if (ImGui::BeginPopupModal("Ignore Changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
			{
				common::imgui::draw_background_blur();
				ImGui::Spacing(0.0f, 0.0f);

				const auto half_width = ImGui::GetContentRegionMax().x * 0.5f;
				auto line1_str = "You'll loose all unsaved changes if you continue!";
				auto line2_str = "Copy to clipboard and manually change map_settings.toml!   ";
				auto line3_str = "Do not forget to save the file ;)";

				ImGui::Spacing();
				ImGui::SetCursorPosX(5.0f + half_width - (ImGui::CalcTextSize(line1_str).x * 0.5f));
				ImGui::TextUnformatted(line1_str);



				ImGui::Spacing(0, 2);
				ImGui::PushFont(common::imgui::font::BOLD);
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x * 0.5f - 120.0f));
				if (ImGui::Button("Copy Selected Light to Clipboard   " ICON_FA_SAVE, ImVec2(240.0f, 0)))
				{
					if (const auto edit_light = lights->get_first_active_light(); edit_light)
					{
						const auto temp_def = build_current_edit_light_def(edit_light);
						copy_text_to_clipboard(common::toml::build_light_string_for_single_light(temp_def));
					}
				} ImGui::PopFont();
				ImGui::Spacing(0, 2);

				ImGui::Spacing();
				ImGui::SetCursorPosX(5.0f + half_width - (ImGui::CalcTextSize(line2_str).x * 0.5f));
				ImGui::TextUnformatted(line2_str);
				ImGui::Spacing(0, 2);

				ImGui::PushFont(common::imgui::font::BOLD);
				ImGui::SetCursorPosX(5.0f + half_width - (ImGui::CalcTextSize(line3_str).x * 0.5f));
				ImGui::TextUnformatted(line3_str);
				ImGui::PopFont();

				ImGui::Spacing(0, 8);
				ImGui::Spacing(0, 0); ImGui::SameLine();

				ImVec2 button_size(half_width - 6.0f - ImGui::GetStyle().WindowPadding.x, 0.0f);
				if (ImGui::Button("Ignore", button_size))
				{
					ms_light_selection = ms_light_selection_pending;
					ms_light_selection_pending = nullptr;
					reset_point_selection = true;
					rebuild_editor_light_preview(lights, ms_lights, ms_light_selection, im->m_light_editor_preview_all);
					ImGui::CloseCurrentPopup();
				}

				ImGui::SameLine(0, 6.0f);
				if (ImGui::Button("Cancel", button_size))
				{
					ms_light_selection_pending = nullptr;
					ImGui::CloseCurrentPopup();
				}

				ImGui::EndPopup();
			}
			ImGui::PopID();
		} // end if edit_active_light && ms_light_selection
	}

	void cont_mapsettings_confvar()
	{
		const auto& var = remix_vars::get();

		ImGui::PushFont(common::imgui::font::BOLD);
		if (ImGui::Button("Reset Vars to Level State   " ICON_FA_REPLY_ALL "##ConfvarReset", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0))) {
			var->reset_all_modified(true);
		} ImGui::PopFont(); TT("This resets all remix vars back to level state (same as when map loads)");

		ImGui::SameLine();
		reload_mapsettings_button_with_popup("Confvar");
		ImGui::Spacing(0, 2);

		static std::string conf_str1, conf_str2;
		static float conf1_transition_time = 0.0f, conf2_transition_time = 0.0f;
		static remix_vars::EASE_TYPE conf1_mode = remix_vars::EASE_TYPE_SIN_IN, conf2_mode = remix_vars::EASE_TYPE_SIN_IN;
		static std::vector<std::string> configs;
		static bool loaded_configs = false;

		ImGui::PushFont(common::imgui::font::BOLD);
		if (ImGui::Button("Refresh Configs   " ICON_FA_REDO, ImVec2(ImGui::GetContentRegionAvail().x, 0)) || !loaded_configs)
		{
			configs.clear();
			if (!game::root_path.empty())
			{
				std::string conf_path = game::root_path + COMPMOD_ASSET_DIR "map_configs\\";
				if (std::filesystem::exists(conf_path))
				{
					for (const auto& d : std::filesystem::directory_iterator(conf_path))
					{
						if (d.path().extension() == ".conf")
						{
							auto file = std::filesystem::path(d.path());
							configs.push_back(file.filename().string());
						}
					}
				}
				loaded_configs = true;
			}
		}
		ImGui::PopFont();
		ImGui::Spacing(0, 6);

		if (draw_remix_vars_preset_workbench(var)) {
			loaded_configs = false;
		}

		ImGui::Spacing(0, 6);
		ImGui::SeparatorTextLarge(" Existing map_configs ");
		ImGui::Spacing(0, 4);

		{

			ImGui::TableHeaderDropshadow();
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
			ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetColorU32(ImGuiCol_FrameBgActive));
			if (ImGui::BeginListBox("##listbox1", ImVec2(ImGui::GetContentRegionAvail().x, 100.0f)))
			{
				for (const auto& str : configs)
				{
					const bool is_selected = conf_str1 == str;
					if (ImGui::Selectable(str.c_str(), is_selected)) {
						conf_str1 = str;
					}

					if (is_selected) {
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndListBox();
			}
			ImGui::SafePopStyleColor(1, __LINE__);
			ImGui::PopStyleVar();
			ImGui::Spacing(0, 4);

			SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
			if (ImGui::DragFloat("Transition Time##1", &conf1_transition_time, 0.005f, 0.0f)) {
				conf1_transition_time = std::clamp(conf1_transition_time, 0.0f, FLT_MAX);
			}

			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
			if (ImGui::BeginCombo("##ModeSelector1", remix_vars::EASE_TYPE_STR[conf1_mode], ImGuiComboFlags_None))
			{
				for (std::uint32_t n = 0u; n < (std::uint32_t)IM_ARRAYSIZE(remix_vars::EASE_TYPE_STR); n++)
				{
					const bool is_selected = conf1_mode == n;
					if (ImGui::Selectable(remix_vars::EASE_TYPE_STR[n], is_selected)) {
						conf1_mode = (remix_vars::EASE_TYPE)n;
					}

					if (is_selected) {
						ImGui::SetItemDefaultFocus();
					}

				}
				ImGui::EndCombo();
			}

			ImGui::SameLine();
			ImGui::BeginDisabled(conf_str1.empty());

			const auto btn_to_label_size = ImGui::CalcWidgetWidthForChild(120.0f);
			if (ImGui::Button("Trigger##1", ImVec2(btn_to_label_size, 0)))
			{
				std::string conf_name = conf_str1;
				if (!conf_name.ends_with(".conf")) {
					conf_name += ".conf";
				}

				var->parse_and_apply_conf_with_lerp(
					conf_name,
					utils::string_hash64(conf_name),
					conf1_mode,
					conf1_transition_time);
			}
			ImGui::EndDisabled();
		}

		ImGui::Spacing(0, 4);
		ImGui::Separator();
		ImGui::Spacing(0, 4);

		{
			ImGui::TableHeaderDropshadow();
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
			ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetColorU32(ImGuiCol_FrameBgActive));
			if (ImGui::BeginListBox("##listbox2", ImVec2(ImGui::GetContentRegionAvail().x, 100.0f)))
			{
				for (const auto& str : configs)
				{
					const bool is_selected = conf_str2 == str;
					if (ImGui::Selectable(str.c_str(), is_selected)) {
						conf_str2 = str;
					}

					if (is_selected) {
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndListBox();
			}
			ImGui::SafePopStyleColor(1, __LINE__);
			ImGui::PopStyleVar();
			ImGui::Spacing(0, 4);

			SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
			if (ImGui::DragFloat("Transition Time##2", &conf2_transition_time, 0.005f, 0.0f)) {
				conf2_transition_time = std::clamp(conf2_transition_time, 0.0f, FLT_MAX);
			}

			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
			if (ImGui::BeginCombo("##ModeSelector2", remix_vars::EASE_TYPE_STR[conf2_mode], ImGuiComboFlags_None))
			{
				for (std::uint32_t n = 0u; n < (std::uint32_t)IM_ARRAYSIZE(remix_vars::EASE_TYPE_STR); n++)
				{
					const bool is_selected = conf2_mode == n;
					if (ImGui::Selectable(remix_vars::EASE_TYPE_STR[n], is_selected)) {
						conf2_mode = (remix_vars::EASE_TYPE)n;
					}

					if (is_selected) {
						ImGui::SetItemDefaultFocus();
					}

				}
				ImGui::EndCombo();
			}

			ImGui::SameLine();
			ImGui::BeginDisabled(conf_str2.empty());

			const auto btn_to_label_size = ImGui::CalcWidgetWidthForChild(120.0f);
			if (ImGui::Button("Trigger##2", ImVec2(btn_to_label_size, 0)))
			{
				std::string conf_name = conf_str2;
				if (!conf_name.ends_with(".conf")) {
					conf_name += ".conf";
				}

				var->parse_and_apply_conf_with_lerp(
					conf_name,
					utils::string_hash64(conf_name),
					conf2_mode,
					conf2_transition_time);
			}
			ImGui::EndDisabled();
		}

		ImGui::Spacing(0, 4);
	}


	void imgui::tab_light_studio()
	{
		ui_subsection("Light Studio", "Create, place and rig map lights. Each light uses one clear backend: Analytical, Native IES or Fake IES.", ICON_FA_LIGHTBULB);

		static bool native_ies_enabled = true;
		static bool native_ies_hot_reload = true;
		static int native_ies_reload_interval_ms = 1000;
		static float native_ies_value_clamp = 0.0f;

		auto apply_ies_runtime_option = [](const char* name, const std::string& value)
		{
			if (!remix_api::is_initialized() || !remix_api::get()) return;
			remix_api::get()->m_bridge.SetConfigVariable(name, value.c_str());
		};

		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
		const bool runtime_open = ImGui::CollapsingHeader("IES Runtime (Advanced)");
		ImGui::PopStyleVar();
		if (runtime_open)
		{
			ImGui::TextDisabled("These controls target the paired IES-enabled DXVK Remix build. They do not alter light radius or authored intensity.");
			bool runtime_changed = false;
			if (ImGui::BeginTable("##native_ies_runtime_options", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); runtime_changed |= ImGui::Checkbox("Enable Profiles", &native_ies_enabled);
				ImGui::TableNextColumn(); runtime_changed |= ImGui::Checkbox("Hot Reload", &native_ies_hot_reload);
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(110.0f); runtime_changed |= ImGui::DragInt("Reload ms", &native_ies_reload_interval_ms, 10.0f, 100, 60000);
				ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(110.0f); runtime_changed |= ImGui::DragFloat("Value Clamp", &native_ies_value_clamp, 0.05f, 0.0f, 100000.0f, "%.2f");
				ImGui::EndTable();
			}
			if (runtime_changed)
			{
				native_ies_reload_interval_ms = std::clamp(native_ies_reload_interval_ms, 100, 60000);
				native_ies_value_clamp = std::max(0.0f, native_ies_value_clamp);
				apply_ies_runtime_option("rtx.lights.ies.enableIesProfiles", native_ies_enabled ? "True" : "False");
				apply_ies_runtime_option("rtx.lights.ies.enableIesProfileHotReload", native_ies_hot_reload ? "True" : "False");
				apply_ies_runtime_option("rtx.lights.ies.iesProfileHotReloadIntervalMs", std::to_string(native_ies_reload_interval_ms));
				apply_ies_runtime_option("rtx.lights.ies.iesProfileValueClamp", std::format("{:.4f}", native_ies_value_clamp));
			}

			const auto* light_runtime = remix_lights::get();
			const auto stats = light_runtime ? light_runtime->build_runtime_debug_stats() : remix_lights::runtime_debug_stats_s{};
			if (ImGui::BeginTable("##light_backend_runtime_cards", 5, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); ui_metric_card("Analytical", std::format("{}", stats.legacy_rig_lights), "Active native analytical controller lights.");
				ImGui::TableNextColumn(); ui_metric_card("Native IES", std::format("{}", stats.native_ies_lights), "Active native IES controller lights.");
				ImGui::TableNextColumn(); ui_metric_card("Fake IES", std::format("{}", stats.fake_ies_rigs), "Active Fake IES controller rigs.");
				ImGui::TableNextColumn(); ui_metric_card("Helpers", std::format("{}", stats.ies_child_handles), "Runtime helper lights spawned only by Fake IES rigs.");
				ImGui::TableNextColumn(); ui_metric_card("IES Errors", std::format("{}", stats.native_ies_failures), "Native profile path/configuration failures since map load.");
				ImGui::EndTable();
			}
		}

		ImGui::Spacing(0, 8);
		ui_subsection("Authoring Workspace", "The light manager, Primary Sun workflow and editor are visible directly instead of hiding behind another expanding panel.", ICON_FA_EDIT);
		cont_mapsettings_light_spawning();
	}

	void imgui::tab_map_settings()
	{
		draw_mapsettings_workspace_header();

		static bool show_advanced_map_tools = false;
		static bool compact_authoring_view = false;

		ImGui::Spacing(0, 6.0f);
		const auto toggle_size = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f, 0.0f);
		imgui::toggle_button_bool(&compact_authoring_view, "Compact Authoring UI", toggle_size,
			"ON: keeps the main workflow cleaner by hiding less-used descriptive text where possible.");
		ImGui::SameLine();
		imgui::toggle_button_bool(&show_advanced_map_tools, "Show Advanced Map Tools", toggle_size,
			"ON: shows legacy/deeper map tools such as marker/culling manipulation and config-var transitions.");

		ImGui::Spacing(0, 6.0f);
		if (!compact_authoring_view)
		{
			ImGui::TextDisabled("V21 auto-save updates only the current map's MARKER and CULL entries; light-database authoring keeps its existing dedicated persistence path.");
			ImGui::TextDisabled("Status: %s", map_settings::map_save_status().c_str());
		}

		ImGui::Spacing(0, 6.0f);
		if (ImGui::BeginTabBar("##mapsettings_workspace_tabs", ImGuiTabBarFlags_Reorderable))
		{
			if (ImGui::BeginTabItem(ICON_FA_LIGHTBULB "  Authoring"))
			{
				if (!compact_authoring_view) {
					ui_subsection("Authoring", "Main remaster workflow: map metadata and selected map-level systems. Light editing is in Light Studio.", ICON_FA_PAINT_BRUSH);
				}

				ui_subsection("General Settings", "Map metadata and configuration maintenance.", ICON_FA_ELLIPSIS_H);
				cont_mapsettings_general();

				ImGui::Spacing(0, 6.0f);
				ImGui::TextDisabled("Light authoring moved to the main Light Studio tab. Map Settings now keeps only map-level systems.");

				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem(ICON_FA_WATER "  World / Visibility"))
			{
				if (!compact_authoring_view) {
					ui_subsection("World / Visibility", "Fog, culling and marker tools. Keep advanced tools hidden unless you are actively fixing map visibility/culling issues.", ICON_FA_EYE);
				}

				ui_subsection("Fog Settings", "Fog and world visibility values for the current map.", ICON_FA_WATER);
				cont_mapsettings_fog();

				if (show_advanced_map_tools)
				{
					ui_subsection("Culling Manipulation", "Advanced current-map visibility overrides.", ICON_FA_EYE_SLASH);
					cont_mapsettings_culling_manipulation();
				}
				else
				{
					ImGui::TextDisabled("Culling manipulation is hidden. Marker authoring has its own main tab.");
				}

				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem(ICON_FA_MAGIC "  Animation / Runtime"))
			{
				if (!compact_authoring_view) {
					ui_subsection("Animation / Runtime", "Dynamic light anchors, animated light presets, performance budgets, Fake IES quality and compatibility switches.", ICON_FA_MAGIC);
				}

				ui_subsection("Animated Lights / Anchors", "Dynamic anchors and map-level animation runtime.", ICON_FA_LIGHTBULB);
				cont_mapsettings_dynamic_lighting();

				if (show_advanced_map_tools)
				{
					ui_subsection("Configvars / Transitions", "Advanced per-map RTX configuration transitions.", ICON_FA_PAINT_BRUSH);
					cont_mapsettings_confvar();
				}
				else
				{
					ImGui::TextDisabled("Config-var transitions are hidden. Enable 'Show Advanced Map Tools' above when needed.");
				}

				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem(ICON_FA_STETHOSCOPE "  Debug / Audit"))
			{
				draw_light_audit_and_runtime_debug_panel();
				ImGui::Spacing(0, 8.0f);
				draw_source_multicore_panel();
				ImGui::EndTabItem();
			}


			ImGui::EndTabBar();
		}

		m_devgui_custom_footer_content = "Area: " + std::to_string(g_current_area) + "\nLeaf: " + std::to_string(g_current_leaf);
	}

	// #
	// #

	struct flashlight_profile_s
	{
		const char* name;
		const char* tooltip;
		bool main_enabled;
		bool core_enabled;
		bool hotspot_enabled;
		bool spill_enabled;
		float main_intensity;
		float main_radius;
		float main_angle;
		float main_softness;
		float main_exponent;
		float core_intensity;
		float core_radius;
		float hotspot_intensity;
		float hotspot_radius;
		float hotspot_angle;
		float hotspot_softness;
		float hotspot_exponent;
		float spill_intensity;
		float spill_radius;
		float spill_angle;
		float spill_softness;
		float spill_exponent;
		Vector color;
	};

	static const flashlight_profile_s FLASHLIGHT_WORKBENCH_PROFILES[] =
	{
		{ "Performance", "Main beam plus a small core fill. Lowest light count.", true, true, false, false, 17000.0f, 0.48f, 27.0f, 0.35f, 0.66f, 7000.0f, 0.08f, 0.0f, 0.2f, 12.0f, 0.1f, 0.9f, 0.0f, 0.8f, 45.0f, 0.7f, 0.3f, { 1.0f, 0.91f, 0.76f } },
		{ "Balanced", "Four-layer rig with conservative intensities and cost.", true, true, true, true, 19000.0f, 0.52f, 28.0f, 0.36f, 0.70f, 9000.0f, 0.10f, 9500.0f, 0.25f, 12.0f, 0.10f, 0.92f, 3500.0f, 0.78f, 47.0f, 0.72f, 0.30f, { 1.0f, 0.92f, 0.78f } },
		{ "Realistic LED", "Neutral LED beam with a focused hotspot and restrained spill.", true, true, true, true, 18000.0f, 0.48f, 25.0f, 0.28f, 0.78f, 7000.0f, 0.08f, 14500.0f, 0.22f, 10.5f, 0.08f, 1.02f, 3200.0f, 0.74f, 43.0f, 0.78f, 0.28f, { 0.92f, 0.97f, 1.0f } },
		{ "Tactical Flashlight", "High-contrast central hotspot with a controlled outer beam.", true, true, true, true, 21000.0f, 0.44f, 22.0f, 0.22f, 0.86f, 8500.0f, 0.08f, 19000.0f, 0.20f, 8.5f, 0.05f, 1.12f, 2500.0f, 0.66f, 38.0f, 0.68f, 0.36f, { 0.95f, 0.98f, 1.0f } },
		{ "Soft Cinematic", "Wide, soft and warm beam for readable interiors and capture.", true, true, false, true, 14500.0f, 0.76f, 36.0f, 0.54f, 0.44f, 10500.0f, 0.12f, 0.0f, 0.2f, 12.0f, 0.1f, 0.9f, 5200.0f, 1.05f, 58.0f, 0.84f, 0.22f, { 1.0f, 0.84f, 0.66f } },
		{ "Wide Beam", "Broad beam for open rooms; hotspot remains subtle.", true, true, true, true, 16000.0f, 0.80f, 42.0f, 0.50f, 0.46f, 9000.0f, 0.12f, 6500.0f, 0.30f, 18.0f, 0.18f, 0.74f, 5200.0f, 1.12f, 64.0f, 0.86f, 0.20f, { 1.0f, 0.90f, 0.74f } },
		{ "Narrow Beam", "Long visual reach and a tight hotspot for corridors.", true, true, true, false, 22500.0f, 0.38f, 18.0f, 0.16f, 0.96f, 6500.0f, 0.07f, 18000.0f, 0.18f, 7.0f, 0.04f, 1.20f, 0.0f, 0.7f, 40.0f, 0.7f, 0.3f, { 0.94f, 0.97f, 1.0f } },
		{ "Source Classic", "Simple warm Source-like cone with local fill.", true, true, false, false, 20000.0f, 0.50f, 25.0f, 0.34f, 0.70f, 9500.0f, 0.10f, 0.0f, 0.2f, 12.0f, 0.1f, 0.9f, 0.0f, 0.8f, 45.0f, 0.7f, 0.3f, { 1.0f, 0.82f, 0.58f } },
		{ "Horror", "Narrow, warm, uneven-looking rig with strong center contrast.", true, true, true, true, 12500.0f, 0.42f, 20.0f, 0.24f, 0.90f, 6000.0f, 0.08f, 15000.0f, 0.18f, 7.5f, 0.04f, 1.18f, 1800.0f, 0.72f, 39.0f, 0.82f, 0.24f, { 1.0f, 0.72f, 0.45f } },
		{ "Custom", "Selected automatically after a manual parameter edit.", true, true, true, true, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, { 1.0f, 1.0f, 1.0f } },
	};

	void apply_flashlight_workbench_profile(const int profile_index)
	{
		if (profile_index < 0 || profile_index >= IM_ARRAYSIZE(FLASHLIGHT_WORKBENCH_PROFILES) - 1) return;
		const auto& profile = FLASHLIGHT_WORKBENCH_PROFILES[profile_index];
		const auto gs = game_settings::get();
		gs->flashlight_enabled.set_var(true, true);
		gs->flashlight_preset.set_var(profile_index, true);
		gs->flashlight_main_enabled.set_var(profile.main_enabled, true);
		gs->flashlight_inner_enabled.set_var(profile.core_enabled, true);
		gs->flashlight_hotspot_enabled.set_var(profile.hotspot_enabled, true);
		gs->flashlight_spill_enabled.set_var(profile.spill_enabled, true);
		gs->flashlight_intensity.set_var(profile.main_intensity, true);
		gs->flashlight_radius.set_var(profile.main_radius, true);
		gs->flashlight_angle.set_var(profile.main_angle, true);
		gs->flashlight_softness.set_var(profile.main_softness, true);
		gs->flashlight_expo.set_var(profile.main_exponent, true);
		gs->flashlight_inner_intensity.set_var(profile.core_intensity, true);
		gs->flashlight_inner_radius.set_var(profile.core_radius, true);
		gs->flashlight_hotspot_intensity.set_var(profile.hotspot_intensity, true);
		gs->flashlight_hotspot_radius.set_var(profile.hotspot_radius, true);
		gs->flashlight_hotspot_angle.set_var(profile.hotspot_angle, true);
		gs->flashlight_hotspot_softness.set_var(profile.hotspot_softness, true);
		gs->flashlight_hotspot_expo.set_var(profile.hotspot_exponent, true);
		gs->flashlight_spill_intensity.set_var(profile.spill_intensity, true);
		gs->flashlight_spill_radius.set_var(profile.spill_radius, true);
		gs->flashlight_spill_angle.set_var(profile.spill_angle, true);
		gs->flashlight_spill_softness.set_var(profile.spill_softness, true);
		gs->flashlight_spill_expo.set_var(profile.spill_exponent, true);
		float color[3] = { profile.color.x, profile.color.y, profile.color.z };
		gs->flashlight_main_color.set_vec(color, true);
		gs->flashlight_inner_color.set_vec(color, true);
		gs->flashlight_hotspot_color.set_vec(color, true);
		gs->flashlight_spill_color.set_vec(color, true);
		const float neutral_direction[2] = { 0.0f, 0.0f };
		gs->flashlight_main_direction_offset.set_vec(neutral_direction, true);
		gs->flashlight_hotspot_direction_offset.set_vec(neutral_direction, true);
		gs->flashlight_spill_direction_offset.set_vec(neutral_direction, true);
		game_settings::mark_dirty("flashlight preset");
	}

	template <typename Variable>
	void draw_flashlight_layer_controls(const char* id, const char* title, Variable& enabled,
		Variable& intensity, Variable& radius, Variable* angle,
		Variable* softness, Variable* exponent, Variable& color, Variable* direction_offset,
		Variable& player_offset, Variable& bot_offset, const bool point_layer = false)
	{
		ImGui::PushID(id);
		ImGui::SeparatorText(title);
		const int primary_columns = ui_responsive_column_count(190.0f, 3);
		if (ImGui::BeginTable("##primary", primary_columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ImGui::Checkbox("Enabled", enabled.get_as<bool*>());
			ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); ImGui::DragFloat("Intensity", intensity.get_as<float*>(), 50.0f, 0.0f, 250000.0f, "%.0f");
			ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f);
			ImGui::DragFloat(point_layer ? "Point Radius" : "Emitter Radius", radius.get_as<float*>(), 0.002f, 0.001f, point_layer ? 0.50f : 4.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			TT(point_layer ? "Physical radius of the small near-field point emitter. It is not the reach of the light; values around 0.06-0.15 are recommended." : "Physical emitter radius used to soften the analytical spotlight. This is not an explicit range control.");
			ImGui::EndTable();
		}

		if (point_layer && radius.get_as<float>() > 0.20f)
		{
			ui_warning_banner("Oversized point emitter", "The Core Sphere radius is above the recommended near-field range and may look like a large glowing volume. Use the repair button or reduce it below 0.20.");
			if (ImGui::Button("Set Recommended Point Radius", ImVec2(-1.0f, 0.0f)))
			{
				radius.set_var(0.10f, true);
				game_settings::mark_dirty("flashlight point radius repair");
			}
		}

		if (angle && softness && exponent)
		{
			if (ImGui::BeginTable("##shape", ui_responsive_column_count(170.0f, 3), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); ImGui::DragFloat("Beam Angle", angle->get_as<float*>(), 0.1f, 1.0f, 179.0f, "%.1f deg");
				ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); ImGui::DragFloat("Edge Softness", softness->get_as<float*>(), 0.005f, 0.0f, 1.0f, "%.3f");
				ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); ImGui::DragFloat("Falloff", exponent->get_as<float*>(), 0.005f, 0.0f, 4.0f, "%.3f");
				ImGui::EndTable();
			}
		}

		ImGui::ColorEdit3("Color / temperature tint", color.get_as<float*>(), ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_PickerHueBar);
		if (direction_offset)
		{
			ImGui::SetNextItemWidth(std::min(240.0f, ImGui::GetContentRegionAvail().x));
			ImGui::DragFloat2("Direction Offset", direction_offset->get_as<float*>(), 0.1f, -45.0f, 45.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
			TT("Local pitch/yaw correction applied to this spotlight only.");
		}
		ImGui::Widget_PrettyDragVec3("Player Offset", player_offset.get_as<float*>(), true, 90.0f, 0.1f, -1000.0f, 1000.0f, "F", "H", "V");
		ImGui::Widget_PrettyDragVec3("Bot Offset", bot_offset.get_as<float*>(), true, 90.0f, 0.1f, -1000.0f, 1000.0f, "F", "H", "V");
		ImGui::TextDisabled(point_layer ? "Core Sphere is a near-field point light. Keep radius small and tune perceived reach with intensity." : "The current Remix analytical-light extension derives visual reach from intensity, emitter radius and shaping.");
		ImGui::PopID();
	}

	void cont_gamesettings_flashlight()
	{
		const auto gs = game_settings::get();
		const int preset_before = std::clamp(gs->flashlight_preset.get_as<int>(), 0, IM_ARRAYSIZE(FLASHLIGHT_WORKBENCH_PROFILES) - 1);
		auto rig_signature = [gs]()
		{
			std::string signature;
			auto append = [&signature](const auto& variable)
			{
				signature += variable.m_name;
				signature += '=';
				signature += variable.get_str_value();
				signature += ';';
			};
			append(gs->flashlight_enabled); append(gs->flashlight_main_enabled); append(gs->flashlight_inner_enabled);
			append(gs->flashlight_hotspot_enabled); append(gs->flashlight_spill_enabled);
			append(gs->flashlight_intensity); append(gs->flashlight_radius); append(gs->flashlight_angle); append(gs->flashlight_softness); append(gs->flashlight_expo);
			append(gs->flashlight_main_color); append(gs->flashlight_main_direction_offset); append(gs->flashlight_offset_player); append(gs->flashlight_offset_bot);
			append(gs->flashlight_inner_intensity); append(gs->flashlight_inner_radius); append(gs->flashlight_inner_color); append(gs->flashlight_inner_offset_player); append(gs->flashlight_inner_offset_bot);
			append(gs->flashlight_hotspot_intensity); append(gs->flashlight_hotspot_radius); append(gs->flashlight_hotspot_angle); append(gs->flashlight_hotspot_softness); append(gs->flashlight_hotspot_expo);
			append(gs->flashlight_hotspot_color); append(gs->flashlight_hotspot_direction_offset); append(gs->flashlight_hotspot_offset_player); append(gs->flashlight_hotspot_offset_bot);
			append(gs->flashlight_spill_intensity); append(gs->flashlight_spill_radius); append(gs->flashlight_spill_angle); append(gs->flashlight_spill_softness); append(gs->flashlight_spill_expo);
			append(gs->flashlight_spill_color); append(gs->flashlight_spill_direction_offset); append(gs->flashlight_spill_offset_player); append(gs->flashlight_spill_offset_bot);
			return signature;
		};
		const auto before = rig_signature();

		ui_subsection("Player Flashlight", "Preset-first controls for the complete rig. Basic mode exposes only the adjustments that materially change the image.", ICON_FA_LIGHTBULB);
		const bool advanced_ui = ui_advanced_mode();
		int preset = preset_before;
		bool preset_applied = false;

		if (ImGui::BeginTable("##flashlight_header", ui_responsive_column_count(240.0f, 3), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ImGui::Checkbox("Enable Flashlight Rig", gs->flashlight_enabled.get_as<bool*>());
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::Combo("Rig Preset", &preset, [](void* data, int idx, const char** out_text) {
				const auto* profiles = static_cast<const flashlight_profile_s*>(data);
				*out_text = profiles[idx].name; return true;
			}, const_cast<flashlight_profile_s*>(FLASHLIGHT_WORKBENCH_PROFILES), IM_ARRAYSIZE(FLASHLIGHT_WORKBENCH_PROFILES)))
			{
				if (preset < IM_ARRAYSIZE(FLASHLIGHT_WORKBENCH_PROFILES) - 1) apply_flashlight_workbench_profile(preset);
				else gs->flashlight_preset.set_var(preset, true);
				preset_applied = true;
			}
			TT(FLASHLIGHT_WORKBENCH_PROFILES[std::clamp(preset, 0, IM_ARRAYSIZE(FLASHLIGHT_WORKBENCH_PROFILES) - 1)].tooltip);
			ImGui::TableNextColumn();
			if (ImGui::Button("Restore Balanced", ImVec2(-1.0f, 0.0f))) { apply_flashlight_workbench_profile(1); preset_applied = true; }
			ImGui::EndTable();
		}

		if (!advanced_ui)
		{
			ui_basic_mode_note("Per-layer colors, offsets, direction corrections and shaping are hidden.");
			if (ImGui::BeginTable("##flashlight_basic_tuning", ui_responsive_column_count(220.0f, 2), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); ImGui::DragFloat("Main Brightness", gs->flashlight_intensity.get_as<float*>(), 100.0f, 0.0f, 250000.0f, "%.0f");
				ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); ImGui::DragFloat("Beam Width", gs->flashlight_angle.get_as<float*>(), 0.2f, 5.0f, 80.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
				ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); ImGui::DragFloat("Near Fill Strength", gs->flashlight_inner_intensity.get_as<float*>(), 50.0f, 0.0f, 50000.0f, "%.0f");
				ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1.0f); ImGui::DragFloat("Point Light Radius", gs->flashlight_inner_radius.get_as<float*>(), 0.002f, 0.01f, 0.50f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
				TT("This is the physical size of the Core Sphere emitter, not its reach. Recommended: 0.06-0.15.");
				ImGui::TableNextColumn(); ImGui::Checkbox("Near Fill Point Light", gs->flashlight_inner_enabled.get_as<bool*>());
				TT("Adds a compact point emitter near the weapon/camera to prevent the beam from appearing to begin several metres away.");
				ImGui::TableNextColumn(); ImGui::Checkbox("Focused Hotspot", gs->flashlight_hotspot_enabled.get_as<bool*>());
				ImGui::TableNextColumn(); ImGui::Checkbox("Soft Outer Spill", gs->flashlight_spill_enabled.get_as<bool*>());
				ImGui::EndTable();
			}
			if (gs->flashlight_inner_radius.get_as<float>() > 0.20f)
			{
				ui_warning_banner("Core point radius is too large", "The point emitter is configured above the recommended near-field range. This can make the flashlight look like a large sphere instead of a compact fill near the weapon.");
				if (ImGui::Button("Repair Core Point Light", ImVec2(-1.0f, 0.0f)))
				{
					gs->flashlight_inner_radius.set_var(0.10f, true);
					gs->flashlight_inner_intensity.set_var(9000.0f, true);
					game_settings::mark_dirty("repair flashlight core point light");
				}
			}
		}
		else if (ImGui::BeginTabBar("##flashlight_rig_layers", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton))
		{
			if (ImGui::BeginTabItem("Main Spotlight"))
			{
				draw_flashlight_layer_controls("main", "Main Spotlight", gs->flashlight_main_enabled, gs->flashlight_intensity, gs->flashlight_radius,
					&gs->flashlight_angle, &gs->flashlight_softness, &gs->flashlight_expo, gs->flashlight_main_color, &gs->flashlight_main_direction_offset, gs->flashlight_offset_player, gs->flashlight_offset_bot);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Core Point"))
			{
				draw_flashlight_layer_controls("core", "Core Point Light", gs->flashlight_inner_enabled, gs->flashlight_inner_intensity, gs->flashlight_inner_radius,
					static_cast<decltype(&gs->flashlight_angle)>(nullptr), static_cast<decltype(&gs->flashlight_softness)>(nullptr),
					static_cast<decltype(&gs->flashlight_expo)>(nullptr), gs->flashlight_inner_color,
					static_cast<decltype(&gs->flashlight_main_direction_offset)>(nullptr), gs->flashlight_inner_offset_player, gs->flashlight_inner_offset_bot, true);
				ImGui::TextWrapped("The compact point emitter fills only the immediate weapon/camera area. Its radius is physical emitter size, not range.");
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Hotspot"))
			{
				draw_flashlight_layer_controls("hotspot", "Hotspot Spotlight", gs->flashlight_hotspot_enabled, gs->flashlight_hotspot_intensity, gs->flashlight_hotspot_radius,
					&gs->flashlight_hotspot_angle, &gs->flashlight_hotspot_softness, &gs->flashlight_hotspot_expo, gs->flashlight_hotspot_color, &gs->flashlight_hotspot_direction_offset, gs->flashlight_hotspot_offset_player, gs->flashlight_hotspot_offset_bot);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Soft Spill"))
			{
				draw_flashlight_layer_controls("spill", "Soft Spill Spotlight", gs->flashlight_spill_enabled, gs->flashlight_spill_intensity, gs->flashlight_spill_radius,
					&gs->flashlight_spill_angle, &gs->flashlight_spill_softness, &gs->flashlight_spill_expo, gs->flashlight_spill_color, &gs->flashlight_spill_direction_offset, gs->flashlight_spill_offset_player, gs->flashlight_spill_offset_bot);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		const auto after = rig_signature();
		if (!preset_applied && before != after && gs->flashlight_preset.get_as<int>() == preset_before)
		{
			gs->flashlight_preset.set_var(IM_ARRAYSIZE(FLASHLIGHT_WORKBENCH_PROFILES) - 1, true);
			game_settings::mark_dirty("custom flashlight rig");
		}
	}

	void cont_gamesettings_quick_cmd()
	{
		if (ImGui::BeginTable("##settings_file_actions", ui_responsive_column_count(210.0f, 2), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn();
			if (ImGui::Button("Flush Auto-Save Now", ImVec2(-1.0f, 0.0f))) game_settings::write_toml();
			ImGui::TableNextColumn();
			if (ImGui::Button("Reload GameSettings", ImVec2(-1.0f, 0.0f)))
			{
				if (!ImGui::IsPopupOpen("Reload GameSettings?")) ImGui::OpenPopup("Reload GameSettings?");
			}
			ImGui::EndTable();
		}

		// popup
		if (ImGui::BeginPopupModal("Reload GameSettings?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			common::imgui::draw_background_blur();
			ImGui::Spacing(0.0f, 0.0f);

			const auto half_width = ImGui::GetContentRegionMax().x * 0.5f;
			auto line1_str = "Reload settings from disk and discard pending in-memory edits?   ";
			auto line2_str = "Normal edits are auto-saved after the debounce interval.";
			auto line3_str = "Use Flush Auto-Save Now only for immediate diagnostics.";

			ImGui::Spacing();
			ImGui::SetCursorPosX(5.0f + half_width - (ImGui::CalcTextSize(line1_str).x * 0.5f));
			ImGui::TextUnformatted(line1_str);

			ImGui::Spacing();
			ImGui::SetCursorPosX(5.0f + half_width - (ImGui::CalcTextSize(line2_str).x * 0.5f));
			ImGui::TextUnformatted(line2_str);

			ImGui::PushFont(common::imgui::font::BOLD);
			ImGui::SetCursorPosX(5.0f + half_width - (ImGui::CalcTextSize(line3_str).x * 0.5f));
			ImGui::TextUnformatted(line3_str);
			ImGui::PopFont();

			ImGui::Spacing(0, 8);
			ImGui::Spacing(0, 0); ImGui::SameLine();

			ImVec2 button_size(half_width - 6.0f - ImGui::GetStyle().WindowPadding.x, 0.0f);
			if (ImGui::Button("Reload", button_size))
			{
				game_settings::xo_gamesettings_update_fn();
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine(0, 6.0f);
			if (ImGui::Button("Cancel", button_size)) {
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
	}

	void cont_gamesettings_renderer_settings()
	{
		const auto gs = game_settings::get();
		ImGui::Checkbox("Enable LOD Forcing", gs->lod_forcing.get_as<bool*>()); TT(gs->lod_forcing.get_tooltip_string().c_str());

		if (ImGui::Checkbox("Enable 3D Skybox (very unstable)", gs->enable_3d_sky.get_as<bool*>())) {
			remix_vars::set_option(remix_vars::get_option("rtx.skyAutoDetect"), remix_vars::string_to_option_value(remix_vars::OPTION_TYPE_FLOAT, gs->enable_3d_sky.get_as<bool>() ? "1" : "0"));
		}
		TT(gs->enable_3d_sky.get_tooltip_string().c_str());

		SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
		auto gs_nocull_dist_ptr = game_settings::get()->default_nocull_distance.get_as<float*>();
		if (ImGui::DragFloat("Def. NoCull Dist", gs_nocull_dist_ptr, 0.5f, 0.0f, FLT_MAX, "%.2f"))
		{
			*gs_nocull_dist_ptr = *gs_nocull_dist_ptr < 0.0f ? 0.0f : *gs_nocull_dist_ptr;
			map_settings::get_map_settings().default_nocull_dist = *gs_nocull_dist_ptr;
		}
		TT(gs->default_nocull_distance.get_tooltip_string().c_str());

		SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
		ImGui::DragFloat("Debug Info Distance", gs->debug_info_distance.get_as<float*>(), 0.1f);
		TT(gs->debug_info_distance.get_tooltip_string().c_str());

		ImGui::TextDisabled("Source multicore profiles moved to Map Settings -> Debug / Audit, next to the light/runtime diagnostics.");

		SET_CHILD_WIDGET_WIDTH_MAN(120.0f);
		ImGui::DragFloat("Player Backwards Offset", gs->player_backwards_offset.get_as<float*>(), 0.01f);
		TT(gs->player_backwards_offset.get_tooltip_string().c_str());
	}

	void imgui::tab_game_settings()
	{
		ui_subsection("General / Gameplay", "Flat sub-tabs replace the former stack of expanding panels. Each command appears in one main location.", ICON_FA_COG);

		if (ImGui::BeginTabBar("##general_workspace_tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll))
		{
			if (ImGui::BeginTabItem(ICON_FA_SAVE "  Configuration"))
			{
				cont_gamesettings_quick_cmd();
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem(ICON_FA_GAMEPAD "  Gameplay"))
			{
				if (ui_advanced_mode())
				{
					if (ImGui::BeginTabBar("##general_gameplay_tabs", ImGuiTabBarFlags_FittingPolicyScroll))
					{
						if (ImGui::BeginTabItem("Commands")) { cont_general_quickcommands(); ImGui::EndTabItem(); }
						if (ImGui::BeginTabItem("Infected")) { cont_general_infected(); ImGui::EndTabItem(); }
						if (ImGui::BeginTabItem("Weapons / Give")) { cont_general_weapons(); ImGui::EndTabItem(); }
						ImGui::EndTabBar();
					}
				}
				else
				{
					ui_basic_mode_note("Advanced gameplay spawning and give commands are hidden. Director and bot controls remain available in Dev.");
				}
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem(ICON_FA_CAMERA "  Renderer"))
			{
				cont_gamesettings_renderer_settings();
				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}

		ImGui::TextDisabled("Player Flashlight, Muzzle Flash, Lights, Materials and Performance use their dedicated main tabs.");
	}

	void imgui::tab_lights()
	{
		ui_subsection("Lights", "BSP light import, persistent map-light database and the existing Light Studio share the original V20.9 runtime paths.", ICON_FA_LIGHTBULB);
		if (ImGui::BeginTabBar("##v21_lights_tabs", ImGuiTabBarFlags_Reorderable))
		{
			if (ImGui::BeginTabItem("Light Studio")) { tab_light_studio(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("BSP / Map Import")) { tab_import_lights_from_maps(); ImGui::EndTabItem(); }
			ImGui::EndTabBar();
		}
	}

	void imgui::tab_player_flashlight()
	{
		ui_subsection("Player Flashlight", "Preset-first setup and runtime evidence are separated into two clear pages.", ICON_FA_LIGHTBULB);
		if (ImGui::BeginTabBar("##player_flashlight_tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll))
		{
			if (ImGui::BeginTabItem(ICON_FA_SLIDERS_H "  Setup"))
			{
				cont_gamesettings_flashlight();
				ImGui::EndTabItem();
			}

			if (ui_advanced_mode() && ImGui::BeginTabItem(ICON_FA_TACHOMETER_ALT "  Runtime"))
			{
				const auto stats = remix_api::flashlight_runtime_stats();
				ui_subsection("Compatibility Runtime", "Per-frame descriptor recreation remains the active, proven Source/Remix compatibility lifecycle.", ICON_FA_SHIELD_ALT);
				if (!ui_compact_descriptions())
					ImGui::TextWrapped("Main, Core, Hotspot and Spill are recreated after Source entity iteration so their descriptors cannot retain stale position or direction data.");

				if (ImGui::BeginTable("##flashlight_runtime_cards", ui_responsive_column_count(170.0f, 4), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableNextColumn(); ui_metric_card("Tracked owners", std::format("{}", stats.tracked_owners));
					ImGui::TableNextColumn(); ui_metric_card("Active handles", std::format("{}", stats.active_handles));
					ImGui::TableNextColumn(); ui_metric_card("Created layers", std::format("{} / {}", stats.granted_layers, stats.requested_layers));
					ImGui::TableNextColumn(); ui_metric_card("Create failures", std::format("{}", stats.create_failures));
					ImGui::TableNextColumn(); ui_metric_card("Frame rebuilds", std::format("{}", stats.frame_updates));
					ImGui::TableNextColumn(); ui_metric_card("Destroyed handles", std::format("{}", stats.destroyed_handles));
					ImGui::TableNextColumn(); ui_metric_card("Draw failures", std::format("{}", stats.draw_failures));
					ImGui::TableNextColumn(); ui_metric_card("Lifecycle", "per-frame");
					ImGui::EndTable();
				}
				ImGui::TextDisabled("Submitted: Main %llu | Core %llu | Hotspot %llu | Spill %llu",
					static_cast<unsigned long long>(stats.submitted_main), static_cast<unsigned long long>(stats.submitted_core),
					static_cast<unsigned long long>(stats.submitted_hotspot), static_cast<unsigned long long>(stats.submitted_spill));
				if (ImGui::BeginTable("##flashlight_diag_actions", ui_responsive_column_count(220.0f, 2), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableNextColumn(); if (ImGui::Button("Reset Flashlight Counters", ImVec2(-1.0f, 0.0f))) remix_api::reset_flashlight_runtime_stats();
					ImGui::TableNextColumn(); if (ImGui::Button("Clear Runtime Owners", ImVec2(-1.0f, 0.0f))) remix_api::clear_flashlights();
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		if (!ui_advanced_mode()) ImGui::TextDisabled("Runtime counters are available after enabling Advanced UI.");
	}


	void imgui::tab_muzzle_flash()
	{
		if (!ImGui::BeginTabBar("##muzzle_flash_tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll)) return;
		if (ImGui::BeginTabItem(ICON_FA_MAGIC "  Presets"))
		{
		ui_subsection("Muzzle Flash Presets", "Presets configure the existing runtime event system; they do not create a second muzzle-flash implementation.", ICON_FA_BOLT);

		auto apply_preset = [](const int preset)
		{
			// Every preset starts from a complete, deterministic runtime baseline so the
			// result never depends on which preset was active previously.
			dynamic_lighting::m_auto_muzzle_flash = true;
			dynamic_lighting::m_muzzle_flash_origin_mode = 0;
			dynamic_lighting::m_muzzle_weapon_profile_mode = 0;
			dynamic_lighting::m_muzzle_weapon_profiles_enabled = true;
			dynamic_lighting::m_muzzle_flash_delay = 0.0f;
			dynamic_lighting::m_muzzle_flash_cooldown = 0.018f;
			dynamic_lighting::m_muzzle_flash_offset = Vector(18.0f, -2.0f, -3.0f);
			dynamic_lighting::m_muzzle_strict_fire_tokens = false;
			dynamic_lighting::m_muzzle_reject_weapon_handling = true;
			dynamic_lighting::m_muzzle_allow_weapon_family_fallback = true;
			dynamic_lighting::m_muzzle_draw_debug = false;

			switch (preset)
			{
			case 0: // Performance
				dynamic_lighting::m_muzzle_flash_shape_mode = 0; dynamic_lighting::m_muzzle_flash_scalar = 0.045f; dynamic_lighting::m_muzzle_flash_radius = 0.015f;
				dynamic_lighting::m_muzzle_flash_duration = 0.120f; dynamic_lighting::m_muzzle_flash_fade = 0.060f; dynamic_lighting::m_muzzle_flash_color = Vector(1.0f, 0.68f, 0.30f);
				dynamic_lighting::m_runtime_max_muzzle_per_second = 12u; break;
			case 1: // Balanced
				dynamic_lighting::m_muzzle_flash_shape_mode = 0; dynamic_lighting::m_muzzle_flash_scalar = 0.070f; dynamic_lighting::m_muzzle_flash_radius = 0.020f;
				dynamic_lighting::m_muzzle_flash_duration = 0.300f; dynamic_lighting::m_muzzle_flash_fade = 0.090f; dynamic_lighting::m_muzzle_flash_color = Vector(1.0f, 0.68f, 0.30f);
				dynamic_lighting::m_runtime_max_muzzle_per_second = 24u; break;
			case 2: // Realistic
				dynamic_lighting::m_muzzle_flash_shape_mode = 0; dynamic_lighting::m_muzzle_flash_scalar = 0.052f; dynamic_lighting::m_muzzle_flash_radius = 0.014f;
				dynamic_lighting::m_muzzle_flash_duration = 0.095f; dynamic_lighting::m_muzzle_flash_fade = 0.070f; dynamic_lighting::m_muzzle_flash_color = Vector(1.0f, 0.72f, 0.34f);
				dynamic_lighting::m_runtime_max_muzzle_per_second = 24u; break;
			case 3: // Tactical cone
				dynamic_lighting::m_muzzle_flash_shape_mode = 1; dynamic_lighting::m_muzzle_flash_scalar = 0.060f; dynamic_lighting::m_muzzle_flash_radius = 0.018f;
				dynamic_lighting::m_muzzle_flash_duration = 0.110f; dynamic_lighting::m_muzzle_flash_fade = 0.075f; dynamic_lighting::m_muzzle_flash_color = Vector(1.0f, 0.82f, 0.52f);
				dynamic_lighting::m_runtime_max_muzzle_per_second = 24u; break;
			default: // Cinematic
				dynamic_lighting::m_muzzle_flash_shape_mode = 0; dynamic_lighting::m_muzzle_flash_scalar = 0.095f; dynamic_lighting::m_muzzle_flash_radius = 0.030f;
				dynamic_lighting::m_muzzle_flash_duration = 0.420f; dynamic_lighting::m_muzzle_flash_fade = 0.180f; dynamic_lighting::m_muzzle_flash_color = Vector(1.0f, 0.58f, 0.22f);
				dynamic_lighting::m_runtime_max_muzzle_per_second = 24u; break;
			}
			game_settings::mark_dirty("muzzle flash preset");
		};
		if (ImGui::BeginTable("##muzzle_preset_grid", ui_responsive_column_count(145.0f, 5), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); if (ImGui::Button("Performance", ImVec2(-1.0f, 0.0f))) apply_preset(0);
			ImGui::TableNextColumn(); if (ImGui::Button("Balanced", ImVec2(-1.0f, 0.0f))) apply_preset(1);
			ImGui::TableNextColumn(); if (ImGui::Button("Realistic", ImVec2(-1.0f, 0.0f))) apply_preset(2);
			ImGui::TableNextColumn(); if (ImGui::Button("Tactical", ImVec2(-1.0f, 0.0f))) apply_preset(3);
			ImGui::TableNextColumn(); if (ImGui::Button("Cinematic", ImVec2(-1.0f, 0.0f))) apply_preset(4);
			ImGui::EndTable();
		}

			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(ICON_FA_BOLT "  Runtime"))
		{
			cont_mapsettings_muzzle_flash();
			ImGui::TextDisabled("Auto-save: %s", game_settings::last_save_status().c_str());
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	void imgui::tab_markers()
	{
		const auto& map_name = map_settings::get_map_name();
		ui_subsection("Markers", "Map status and marker editing are separated into stable pages instead of nested expanding sections.", ICON_FA_DATABASE);
		if (!ImGui::BeginTabBar("##marker_workspace_tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll)) return;

		if (ImGui::BeginTabItem(ICON_FA_INFO_CIRCLE "  Map Status"))
		{
			if (ImGui::BeginTable("##marker_status", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); ui_metric_card("Current map", map_name.empty() ? "not loaded" : map_name);
				ImGui::TableNextColumn(); ui_metric_card("Markers", std::format("{}", map_settings::get_map_settings().map_markers.size()));
				ImGui::TableNextColumn(); ui_metric_card("Persistence", map_settings::map_save_status());
				ImGui::EndTable();
			}
			if (ImGui::BeginTable("##marker_map_actions", ui_responsive_column_count(220.0f, 2), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn(); if (ImGui::Button("Save current map markers now", ImVec2(-1.0f, 0.0f))) map_settings::save_current_map_authoring_data(true);
				ImGui::TableNextColumn(); if (ImGui::Button("Delete all markers on current map", ImVec2(-1.0f, 0.0f)))
				{
					map_settings::destroy_markers();
					map_settings::mark_map_data_dirty("delete all current-map markers");
				}
				ImGui::EndTable();
			}
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(ICON_FA_MAP_MARKER_ALT "  Editor"))
		{
			cont_mapsettings_marker_manipulation();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	void imgui::tab_performance()
	{
		auto drag_u32 = [](const char* label, std::uint32_t& value, const int min_v, const int max_v)
		{
			int temp = static_cast<int>(value);
			if (ImGui::DragInt(label, &temp, 1.0f, min_v, max_v)) value = static_cast<std::uint32_t>(std::clamp(temp, min_v, max_v));
		};
		ui_subsection("Performance", "Hardware-neutral quality levels control runtime budgets. Detailed visual parameters stay in their feature tabs.", ICON_FA_TACHOMETER_ALT);
		const auto width = ImVec2((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3.0f) / 4.0f, 0.0f);
		auto quality_button = [&](const char* label, const int internal_profile)
		{
			if (ImGui::Button(label, width)) { dynamic_lighting::apply_compat_profile(internal_profile); game_settings::mark_dirty("performance quality preset"); }
		};
		quality_button("Low", 3); ImGui::SameLine(); quality_button("Balanced", 2); ImGui::SameLine(); quality_button("High", 1); ImGui::SameLine(); quality_button("Ultra", 0);

		const char* quality = dynamic_lighting::m_compat_profile == 3 ? "Low" : dynamic_lighting::m_compat_profile == 2 ? "Balanced" : dynamic_lighting::m_compat_profile == 1 ? "High" : "Ultra";
		const auto flashlight_stats = remix_api::flashlight_runtime_stats();
		if (ImGui::BeginTable("##performance_cards", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ui_metric_card("Quality", quality);
			ImGui::TableNextColumn(); ui_metric_card("Active lights", std::format("{} / {}", dynamic_lighting::m_runtime_active_lights_snapshot, dynamic_lighting::m_runtime_max_active_lights));
			ImGui::TableNextColumn(); ui_metric_card("Flashlight handles", std::format("{}", flashlight_stats.active_handles));
			ImGui::TableNextColumn(); ui_metric_card("Flashlight update", std::format("{:.0f} Hz", dynamic_lighting::m_flashlight_update_hz));
			ImGui::EndTable();
		}
		ImGui::TextDisabled("Auto-save: %s | flashlight layers granted: %u / %u", game_settings::is_dirty() ? "pending" : "clean", flashlight_stats.granted_layers, flashlight_stats.requested_layers);

		ImGui::Checkbox("Runtime light budgets", &dynamic_lighting::m_runtime_budgets_enabled);
		if (ImGui::BeginTable("##v21_runtime_limits", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); drag_u32("Max Active Lights", dynamic_lighting::m_runtime_max_active_lights, 0, 512);
			ImGui::TableNextColumn(); drag_u32("Max Pending Lights", dynamic_lighting::m_runtime_max_pending_lights, 0, 512);
			ImGui::TableNextColumn(); drag_u32("Spawns / Second", dynamic_lighting::m_runtime_max_spawns_per_second, 0, 256);
			ImGui::TableNextColumn(); drag_u32("Muzzle / Second", dynamic_lighting::m_runtime_max_muzzle_per_second, 0, 128);
			ImGui::TableNextColumn(); drag_u32("Sound Hash / Second", dynamic_lighting::m_runtime_max_sound_hash_per_second, 0, 128);
			ImGui::TableNextColumn(); SET_CHILD_WIDGET_WIDTH_MAN(110.0f); ImGui::DragFloat("Dynamic Update Hz", &dynamic_lighting::m_runtime_leaf_check_hz, 0.5f, 0.0f, 60.0f, "%.1f");
			ImGui::EndTable();
		}
		ui_toggle_row("CPU Skin Throttle", &dynamic_lighting::m_cpu_skin_throttle_enabled, "Optional far animated-mesh mitigation.",
			"Skip Far Ragdolls", &dynamic_lighting::m_cpu_skin_skip_far_ragdolls, "More aggressive; validate visually per map.");
		if (dynamic_lighting::m_compat_profile == 0) ui_warning_banner("Ultra quality cost", "Ultra raises dynamic-light budgets and authoring capacity. It is intended for image quality and editing, not as a hardware-specific mode.");
		draw_source_multicore_panel();
	}

	void imgui::tab_diagnostics()
	{
		auto& diag = main_module::sky3d_diagnostics();
		auto gs = game_settings::get();
		ui_subsection("3D Skybox Diagnostics", "Payload freshness and a passive Source fallback isolate the regression without forcing an experimental render-path replacement.", ICON_FA_TACHOMETER_ALT);
		ImGui::Checkbox("Enable 3D Skybox", gs->enable_3d_sky.get_as<bool*>());
		ImGui::SameLine();
		ImGui::Checkbox("Rate-limited log summaries", gs->sky3d_diagnostic_logging.get_as<bool*>());
		TT("At most one Sky3D summary every two seconds; never logs every frame.");
		ImGui::SameLine();
		ImGui::Checkbox("Allow entity metadata recovery", gs->sky3d_safe_source_fallback.get_as<bool*>());
		ImGui::SameLine();
		ImGui::Checkbox("Require VIEW_3DSKY hook confirmation", gs->sky3d_require_hook_confirmation.get_as<bool*>());
		TT("When enabled, entity recovery can refresh diagnostics but cannot authorize static-scene takeover or hide Source sky geometry.");
		int* max_age = gs->sky3d_payload_max_age_frames.get_as<int*>();
		ImGui::SetNextItemWidth(190.0f);
		ImGui::DragInt("Payload freshness window", max_age, 1.0f, 1, 120, "%d frames");
		TT("Static-scene capture accepts only sky_camera transforms captured within this many rendered frames. Stale state always leaves the original Source draw visible.");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160.0f);
		ImGui::DragInt("Maximum accepted scale", gs->sky3d_max_scale.get_as<int*>(), 1.0f, 1, 65536);

		const auto age = main_module::sky3d_payload_age_frames();
		const auto hook_age = main_module::sky3d_hook_payload_age_frames();
		const std::string age_text = age == std::numeric_limits<std::uint64_t>::max()
			? "unavailable" : std::format("{} frames", age);
		if (ImGui::BeginTable("##sky3d_health_cards", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ui_metric_card("Health", main_module::sky3d_health_summary());
			ImGui::TableNextColumn(); ui_metric_card("Payload age", age_text);
			ImGui::TableNextColumn(); ui_metric_card("Generation", std::format("{}", diag.payload_generation));
			ImGui::TableNextColumn(); ui_metric_card("Payload source", diag.last_payload_source);
			ImGui::TableNextColumn(); ui_metric_card("Hook age", hook_age == std::numeric_limits<std::uint64_t>::max() ? "unavailable" : std::format("{} frames", hook_age));
			ImGui::TableNextColumn(); ui_metric_card("Capture eligible", main_module::sky3d_payload_is_capture_eligible() ? "yes" : "no");
			ImGui::EndTable();
		}

		if (ImGui::BeginTable("##sky3d_diag", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ImGui::Text("Hook installed: %s", diag.hook_installed ? "yes" : "no");
			ImGui::TableNextColumn(); ImGui::Text("Hook calls: %llu", static_cast<unsigned long long>(diag.hook_calls));
			ImGui::TableNextColumn(); ImGui::Text("Non-3D hook calls: %llu", static_cast<unsigned long long>(diag.hook_non_3d_calls));
			ImGui::TableNextColumn(); ImGui::Text("VIEW_3DSKY: %llu", static_cast<unsigned long long>(diag.view_3dsky_detections));
			ImGui::TableNextColumn(); ImGui::Text("sky_camera: %llu", static_cast<unsigned long long>(diag.sky_camera_found));
			ImGui::TableNextColumn(); ImGui::Text("Valid payloads: %llu", static_cast<unsigned long long>(diag.valid_payloads));
			ImGui::TableNextColumn(); ImGui::Text("Invalid scale: %llu", static_cast<unsigned long long>(diag.invalid_scale));
			ImGui::TableNextColumn(); ImGui::Text("Invalid origin: %llu", static_cast<unsigned long long>(diag.invalid_origin));
			ImGui::TableNextColumn(); ImGui::Text("Invalid camera: %llu", static_cast<unsigned long long>(diag.invalid_camera_origin));
			ImGui::TableNextColumn(); ImGui::Text("Invalid area: %llu", static_cast<unsigned long long>(diag.invalid_area));
			ImGui::TableNextColumn(); ImGui::Text("Payload reuse: %llu", static_cast<unsigned long long>(diag.payload_reused));
			ImGui::TableNextColumn(); ImGui::Text("Metadata refreshes: %llu", static_cast<unsigned long long>(diag.payload_refreshes));
			ImGui::TableNextColumn(); ImGui::Text("Transform changes: %llu", static_cast<unsigned long long>(diag.payload_transform_changes));
			ImGui::TableNextColumn(); ImGui::Text("Recovery payloads: %llu", static_cast<unsigned long long>(diag.recovery_payloads));
			ImGui::TableNextColumn(); ImGui::Text("Recovery rejected: %llu", static_cast<unsigned long long>(diag.recovery_capture_rejected));
			ImGui::TableNextColumn(); ImGui::Text("Stale events: %llu", static_cast<unsigned long long>(diag.payload_stale));
			ImGui::TableNextColumn(); ImGui::Text("Safe fallbacks: %llu", static_cast<unsigned long long>(diag.safe_fallbacks));
			ImGui::TableNextColumn(); ImGui::Text("Payload resets: %llu", static_cast<unsigned long long>(diag.payload_resets));
			ImGui::TableNextColumn(); ImGui::Text("Rejected payload: %llu", static_cast<unsigned long long>(diag.rejected_missing_payload));
			ImGui::TableNextColumn(); ImGui::Text("Static candidates: %llu", static_cast<unsigned long long>(diag.static_candidates));
			ImGui::TableNextColumn(); ImGui::Text("Submitted objects: %llu", static_cast<unsigned long long>(diag.submitted_objects));
			ImGui::TableNextColumn(); ImGui::Text("Source draws suppressed: %llu", static_cast<unsigned long long>(diag.source_draw_suppressed));
			ImGui::TableNextColumn(); ImGui::Text("Last view ID: %u", diag.last_view_id);
			ImGui::TableNextColumn(); ImGui::Text("Hook confirmed: %s", diag.last_payload_hook_confirmed ? "yes" : "no");
			ImGui::EndTable();
		}
		ImGui::Text("Last stage: %s", diag.last_stage.c_str());
		ImGui::TextDisabled("sky_camera position: %.2f %.2f %.2f | origin: %.2f %.2f %.2f | scale: %d | area: %d | frame: %llu",
			diag.last_sky_camera_position.x, diag.last_sky_camera_position.y, diag.last_sky_camera_position.z,
			diag.last_origin.x, diag.last_origin.y, diag.last_origin.z, diag.last_scale, diag.last_area,
			static_cast<unsigned long long>(diag.last_payload_frame));
		if (ImGui::Button("Reset 3D skybox counters")) main_module::reset_sky3d_diagnostics();
		ImGui::SameLine();
		if (ImGui::Button("Invalidate current sky payload")) main_module::reset_sky3d_payload("manual diagnostics reset");
		ImGui::SameLine();
		// Baseline validator markers retained: Export V21.8.1 runtime report; Modified Edition - V21.8.1
		if (ImGui::Button("Export V21.11 runtime report"))
		{
			const auto result = main_module::export_runtime_diagnostics();
			game::console();
			printf("[V21.11 Diagnostics] %s\n", result.c_str());
		}

		const auto fl = remix_api::flashlight_runtime_stats();
		ui_subsection("Flashlight Compatibility Lifecycle", "Confirms per-frame handle recreation and submission of each configured layer.", ICON_FA_LIGHTBULB);
		if (ImGui::BeginTable("##flashlight_diag", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ImGui::Text("Tracked owners: %u", fl.tracked_owners);
			ImGui::TableNextColumn(); ImGui::Text("Active handles: %u", fl.active_handles);
			ImGui::TableNextColumn(); ImGui::Text("Requested layers: %u", fl.requested_layers);
			ImGui::TableNextColumn(); ImGui::Text("Created layers: %u", fl.granted_layers);
			ImGui::TableNextColumn(); ImGui::Text("Frame rebuilds: %llu", static_cast<unsigned long long>(fl.frame_updates));
			ImGui::TableNextColumn(); ImGui::Text("Create attempts: %llu", static_cast<unsigned long long>(fl.create_attempts));
			ImGui::TableNextColumn(); ImGui::Text("Create failures: %llu", static_cast<unsigned long long>(fl.create_failures));
			ImGui::TableNextColumn(); ImGui::Text("Destroyed handles: %llu", static_cast<unsigned long long>(fl.destroyed_handles));
			ImGui::TableNextColumn(); ImGui::Text("Submitted Main: %llu", static_cast<unsigned long long>(fl.submitted_main));
			ImGui::TableNextColumn(); ImGui::Text("Submitted Core: %llu", static_cast<unsigned long long>(fl.submitted_core));
			ImGui::TableNextColumn(); ImGui::Text("Submitted Hotspot: %llu", static_cast<unsigned long long>(fl.submitted_hotspot));
			ImGui::TableNextColumn(); ImGui::Text("Submitted Spill: %llu", static_cast<unsigned long long>(fl.submitted_spill));
			ImGui::TableNextColumn(); ImGui::Text("Draw failures: %llu", static_cast<unsigned long long>(fl.draw_failures));
			ImGui::TableNextColumn(); ImGui::TextUnformatted("Governor: compatibility-disabled");
			ImGui::TableNextColumn(); ImGui::TextUnformatted("Transactions: compatibility-disabled");
			ImGui::TableNextColumn(); ImGui::TextUnformatted("Owner cleanup: original alive marker");
			ImGui::EndTable();
		}
		if (ImGui::Button("Reset flashlight diagnostics")) remix_api::reset_flashlight_runtime_stats();

		const auto style_recovery = ImGui::GetStyleColorRecoveryStats();
		ui_subsection("Dear ImGui Style Stack", "Prevents repeated PopStyleColor underflow spam and records the exact source line for the next runtime audit.", ICON_FA_EXCLAMATION_TRIANGLE);
		if (ImGui::BeginTable("##imgui_style_recovery", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableNextColumn(); ui_metric_card("Prevented underflows", std::format("{}", style_recovery.prevented_underflows));
			ImGui::TableNextColumn(); ui_metric_card("Recovered leaks", std::format("{}", style_recovery.recovered_leaks));
			ImGui::TableNextColumn(); ui_metric_card("Last source line", std::format("{}", style_recovery.last_source_line));
			ImGui::TableNextColumn(); ui_metric_card("Requested / available", std::format("{} / {}", style_recovery.last_requested_count, style_recovery.last_available_count));
			ImGui::EndTable();
		}
		if (ImGui::Button("Reset ImGui style diagnostics")) ImGui::ResetStyleColorRecoveryStats();

		ui_subsection("Persistence Diagnostics", "Global and per-map save paths use separate dirty flags and atomic temporary-file replacement.", ICON_FA_SAVE);
		ImGui::Text("Global: %s", game_settings::last_save_status().c_str());
		ImGui::Text("Map: %s", map_settings::map_save_status().c_str());
		ImGui::TextDisabled("Global generation: %llu | Map generation: %llu",
			static_cast<unsigned long long>(game_settings::save_generation()), static_cast<unsigned long long>(map_settings::map_save_generation()));
		if (ImGui::Button("Flush global settings now")) game_settings::write_toml();
		ImGui::SameLine();
		if (ImGui::Button("Flush current map data now")) map_settings::save_current_map_authoring_data(true);
	

		ui_subsection("Advanced Source Diagnostics", "Source commands and event inspection use two stable pages instead of another pair of expanding panels.", ICON_FA_TERMINAL);
		if (ImGui::BeginTabBar("##source_diagnostic_tabs", ImGuiTabBarFlags_FittingPolicyScroll))
		{
			if (ImGui::BeginTabItem(ICON_FA_TERMINAL "  Source Toolkit"))
			{
				cont_general_source_debug_commands();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(ICON_FA_HISTORY "  Events"))
			{
				cont_general_event_workbench();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}


	void imgui::tab_about()
	{
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.09f, 0.11f, 0.82f));
		if (ImGui::BeginChild("##about_project_header", ImVec2(0.0f, 126.0f), true,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			ImGui::PushFont(common::imgui::font::BOLD_LARGE);
			ImGui::TextUnformatted("L4D2 RTX Compatibility Mod");
			ImGui::PopFont();
			ImGui::TextDisabled("Modified Edition - V21.14.10");
			ImGui::Spacing();
			ImGui::TextWrapped("A community compatibility and authoring layer for Left 4 Dead 2 running through NVIDIA RTX Remix. This modified edition expands the original mod while preserving clear attribution to its foundation and dependencies.");
		}
		ImGui::EndChild();
		ImGui::SafePopStyleColor(1, __LINE__);
		ImGui::PopStyleVar(2);

		ui_subsection("Project authorship", "Original project ownership and authorship of this modified update.", ICON_FA_USER_EDIT);
		if (ImGui::BeginTable("##about_authorship", 2,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 245.0f);
			ImGui::TableSetupColumn("Credit", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted("Original creator / base mod");
			ImGui::TableNextColumn();
			if (auto* strawberry = about_strawberry_texture())
			{
				ImGui::Image(reinterpret_cast<ImTextureID>(strawberry), ImVec2(76.0f, 90.0f));
				ImGui::SameLine();
				ImGui::BeginGroup();
				ImGui::PushFont(common::imgui::font::BOLD);
				ImGui::TextUnformatted("xoxor4d");
				ImGui::PopFont();
				ImGui::TextWrapped("Creator of the original L4D2 RTX compatibility mod and project foundation.");
				ImGui::TextDisabled("Strawberry artwork included with the V21.11 runtime assets.");
				ImGui::EndGroup();
			}
			else
			{
				ImGui::PushFont(common::imgui::font::BOLD);
				ImGui::TextUnformatted("xoxor4d");
				ImGui::PopFont();
				ImGui::TextDisabled("Creator of the original L4D2 RTX compatibility mod and project foundation.");
				ImGui::TextDisabled("Strawberry asset not found: l4d2-rtx\\textures\\xorxor4d_strawberry.png");
			}

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted("Modified edition / update author");
			ImGui::TableNextColumn();
			ImGui::PushFont(common::imgui::font::BOLD);
			ImGui::TextUnformatted("Patryk \"Entity\" Harmaci\xC5\x84ski");
			ImGui::PopFont();
			ImGui::TextDisabled("Author and developer of the modified update and its extended feature set.");

			ImGui::EndTable();
		}

		ui_warning_banner("Special thanks",
			"Special thanks to xoxor4d for creating the base compatibility mod that made this modified edition possible.",
			ImVec4(0.96f, 0.76f, 0.30f, 1.0f));

		ui_subsection("Credits and acknowledgements", "Projects and communities used by or supporting this compatibility mod.", ICON_FA_HANDSHAKE);
		if (ImGui::BeginTable("##about_credits", 2,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableSetupColumn("Project / community", ImGuiTableColumnFlags_WidthFixed, 250.0f);
			ImGui::TableSetupColumn("Contribution", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			const struct credit_entry_s
			{
				const char* name;
				const char* contribution;
			} credits[] =
			{
				{ "NVIDIA - RTX Remix", "RTX Remix runtime, rendering platform and development ecosystem." },
				{ "People of the Showcase Discord", "Community knowledge and support - especially the NVIDIA engineers." },
				{ "imgui-blur-effect", "Background blur integration used by the in-game interface." },
				{ "l4d2-internal-base", "Internal modding foundation used by the original project." },
				{ "Dear ImGui", "Immediate-mode graphical user interface framework." },
				{ "MinHook", "Windows API hooking library." },
				{ "toml11", "TOML configuration parsing library." },
			};

			for (const auto& credit : credits)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushFont(common::imgui::font::BOLD);
				ImGui::TextUnformatted(credit.name);
				ImGui::PopFont();
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", credit.contribution);
			}

			ImGui::EndTable();
		}

		ImGui::Spacing(0, 10);
		ImGui::Separator();
		ImGui::TextDisabled("Thank you to everyone contributing knowledge, testing, tools and feedback to the L4D2 RTX community.");
	}

	// #
	// #

	void imgui::devgui()
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		if (m_menu_just_opened && viewport)
		{
			// A resolution change or stale imgui.ini could leave Devgui completely
			// outside the current viewport. Re-centre only on the open edge.
			const float width = std::clamp(900.0f, 420.0f, std::max(420.0f, viewport->WorkSize.x - 32.0f));
			const float height = std::clamp(800.0f, 320.0f, std::max(320.0f, viewport->WorkSize.y - 32.0f));
			ImGui::SetNextWindowPos(ImVec2(
				viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
				viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
				ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
			m_menu_just_opened = false;
		}
		else
		{
			ImGui::SetNextWindowSize(ImVec2(900, 800), ImGuiCond_FirstUseEver);
		}
		const float min_width = viewport ? std::min(620.0f, std::max(320.0f, viewport->WorkSize.x - 16.0f)) : 620.0f;
		const float min_height = viewport ? std::min(460.0f, std::max(240.0f, viewport->WorkSize.y - 16.0f)) : 460.0f;
		ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, min_height), ImVec2(FLT_MAX, FLT_MAX));

		bool old_active_state = m_menu_active;
		if (!ImGui::Begin("Devgui", &m_menu_active, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollWithMouse, &common::imgui::draw_window_blur_callback))
		{
			ImGui::End();
			return;
		}

		// HACK when using the menu close button instead of the hotkey to close the devgui
		// :: use logic in 'imgui::input_message' to toggle the menu by sending a msg
		if (old_active_state != m_menu_active && !m_menu_active)
		{
			m_menu_active = true; // we have to re-set this back to true. We would instantly reopen the gui otherwise
			SendMessage(glob::main_window, WM_KEYUP, VK_F5, 0);
		}

		m_im_window_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow);
		m_im_window_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);

		static bool im_demo_menu = false;
		if (im_demo_menu) {
			ImGui::ShowDemoWindow(&im_demo_menu);
		}

#define ADD_TAB(NAME, FUNC) \
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0))); \
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 8)); \
	if (ImGui::BeginTabItem(NAME)) { \
		ImGui::PopStyleVar(1); \
		const float child_height = std::max(1.0f, ImGui::GetContentRegionAvail().y - 44.0f); \
		if (ImGui::BeginChild("##child_" NAME, ImVec2(0, child_height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_HorizontalScrollbar)) { \
			ImGui::RunWithStyleColorCheckpoint([this]() { FUNC(); }, __LINE__); \
		} \
		ImGui::EndChild(); \
		ImGui::EndTabItem(); \
	} else { ImGui::PopStyleVar(1); } \
	ImGui::SafePopStyleColor(1, __LINE__);

		// ---------------------------------------

		const auto col_top = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.0f));
		const auto col_bottom = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.4f));
		const auto col_border = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.8f));
		const auto pre_tabbar_spos = ImGui::GetCursorScreenPos() - ImGui::GetStyle().WindowPadding;

		ImGui::GetWindowDrawList()->AddRectFilledMultiColor(pre_tabbar_spos, pre_tabbar_spos + ImVec2(ImGui::GetWindowWidth(), 40.0f),
			col_top, col_top, col_bottom, col_bottom);

		ImGui::GetWindowDrawList()->AddLine(pre_tabbar_spos + ImVec2(0, 40.0f), pre_tabbar_spos + ImVec2(ImGui::GetWindowWidth(), 40.0f),
			col_border, 1.0f);

		ImGui::SetCursorScreenPos(pre_tabbar_spos + ImVec2(12,8));

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 8));
		ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		if (ImGui::BeginTabBar("devgui_tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton))
		{
			ImGui::SafePopStyleColor(1, __LINE__);
			ImGui::PopStyleVar(1);

			const bool show_dev = flags::has_flag("dev");
			if (show_dev) ADD_TAB("Dev", tab_general);

			ADD_TAB("Lights", tab_lights);
			ADD_TAB("Player Flashlight", tab_player_flashlight);
			ADD_TAB("Muzzle Flash", tab_muzzle_flash);
			ADD_TAB("Markers", tab_markers);
			ADD_TAB("Map Settings", tab_map_settings);
			ADD_TAB("Materials", tab_import_materials_from_maps);
			ADD_TAB("General", tab_game_settings);
			if (ui_advanced_mode())
			{
				ADD_TAB("Performance", tab_performance);
				ADD_TAB("Diagnostics", tab_diagnostics);
				ADD_TAB("Experimental", tab_experimental);
			}
			ADD_TAB("About", tab_about);
			ImGui::EndTabBar();
		}
		else {
			ImGui::SafePopStyleColor(1, __LINE__);
			ImGui::PopStyleVar(1);
		}
#undef ADD_TAB

		{
			ImGui::Separator();
			auto gs = game_settings::get();
			if (ImGui::BeginTable("##devgui_footer", ui_responsive_column_count(150.0f, 4), ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableNextColumn();
				if (!m_devgui_custom_footer_content.empty()) ImGui::TextUnformatted(m_devgui_custom_footer_content.c_str());
				else ImGui::TextDisabled("Hold Left Alt + RMB outside the UI for temporary game input.");
				m_devgui_custom_footer_content.clear();
				ImGui::TableNextColumn();
				ImGui::Checkbox("Advanced UI", gs->ui_advanced_mode.get_as<bool*>());
				TT("Basic mode hides diagnostics, experimental controls and low-level per-layer authoring. This setting is auto-saved.");
				ImGui::TableNextColumn();
				ImGui::Checkbox("Compact text", gs->ui_compact_descriptions.get_as<bool*>());
				ImGui::TableNextColumn();
				if (ImGui::Button("Demo", ImVec2(-1.0f, 0.0f))) im_demo_menu = !im_demo_menu;
				ImGui::EndTable();
			}
		}

		ImGui::End();
	}

	void imgui::endscene_stub(const bool isolated_present_scene)
	{
		auto* im = imgui::get();
		if (!im || !im->overlay_present_available()) return;

		// EndScene may precede Present. Detect the gameplay edge here, but apply
		// the bounded recovery counter exactly once later from the Present hook.
		im->service_gameplay_input_state(false);
		if (!im->m_menu_active) return;

		auto fail_visible_overlay = [im]()
		{
			// Never leave the game without mouse control when the overlay could not
			// actually submit a complete UI frame. A later frame may re-arm it.
			const bool owned_input = im->m_menu_input_armed || im->m_menu_cursor_owned;
			im->m_menu_input_armed = false;
			if (owned_input) im->release_menu_cursor();
		};

		IDirect3DDevice9* dev = g_overlay_present_device ? g_overlay_present_device : game::get_d3d_device();
		if (!dev)
		{
			fail_visible_overlay();
			return;
		}

		if (!im->m_initialized_device)
		{
			++im->m_backend_init_attempts;
			im->m_initialized_device = ImGui_ImplDX9_Init(dev);
			if (!im->m_initialized_device)
			{
				++im->m_backend_init_failures;
				fail_visible_overlay();
				return;
			}
		}

		d3d9_state_guard state_guard(dev);

		// Xorxor/Remix uses unused render state 42 for instance categories. Keep
		// this private payload scoped to the actual ImGui draw calls only. A D3D9
		// state block is not sufficient because RTX Remix intercepts this state.
		const DWORD ui_categories =
			REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI |
			REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_LIGHTS |
			REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ANTI_CULLING |
			REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_MOTION_BLUR;

		auto render_imgui_frame = [im, dev, ui_categories]()
		{
			ImGui_ImplDX9_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();
			im->maintain_menu_cursor_ownership();
			im->devgui();
			ImGui::Render();

			remix_instance_category_guard category_guard(dev);
			category_guard.set(ui_categories);
			ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
			category_guard.restore_neutral();
		};

		if (isolated_present_scene)
		{
			// The direct Present hook exists only for the Source front-end where the
			// Remix EndScene callback is unavailable. It must own an isolated scene.
			IDirect3DSurface9* backbuffer = nullptr;
			if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) || !backbuffer)
			{
				++im->m_overlay_backbuffer_failures;
				fail_visible_overlay();
				return;
			}

			D3DSURFACE_DESC desc{};
			if (FAILED(backbuffer->GetDesc(&desc)) || FAILED(dev->SetRenderTarget(0, backbuffer)))
			{
				backbuffer->Release();
				++im->m_overlay_backbuffer_failures;
				fail_visible_overlay();
				return;
			}

			for (DWORD slot = 1; slot < 4; ++slot) dev->SetRenderTarget(slot, nullptr);
			dev->SetDepthStencilSurface(nullptr);
			const D3DVIEWPORT9 viewport{ 0u, 0u, desc.Width, desc.Height, 0.0f, 1.0f };
			const RECT scissor{ 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
			dev->SetViewport(&viewport);
			dev->SetScissorRect(&scissor);

			if (FAILED(dev->BeginScene()))
			{
				backbuffer->Release();
				++im->m_overlay_begin_scene_failures;
				fail_visible_overlay();
				return;
			}

			render_imgui_frame();
			const HRESULT end_scene_result = dev->EndScene();
			backbuffer->Release();
			if (FAILED(end_scene_result))
			{
				++im->m_overlay_end_scene_failures;
				fail_visible_overlay();
				return;
			}

			++im->m_overlay_safe_backbuffer_frames;
			++im->m_overlay_frontend_present_frames;
		}
		else
		{
			// Gameplay follows Xorxor's Remix bridge EndScene callback. Do not add a
			// second BeginScene/EndScene pair and do not replace Source render targets.
			render_imgui_frame();
			++im->m_overlay_gameplay_bridge_frames;
		}

		++im->m_menu_frames_rendered;
		if (im->m_menu_active && !im->m_menu_input_armed)
		{
			im->m_menu_input_armed = true;
			++im->m_menu_input_arm_events;
			im->acquire_menu_cursor();
		}
	}

	// called before mapsettings are applied
	void imgui::on_map_load()
	{
		if (auto* im = get(); im)
		{
			im->m_light_edit_mode = false;
			im->m_loading_screen_suspended = false;
			im->m_gameplay_present_was_active = false;
			if (!im->m_menu_active)
			{
				im->m_menu_input_armed = false;
				im->m_im_allow_game_input = false;
				im->schedule_gameplay_input_recovery(12u, "map load");
			}
			// Do not disarm or close an already visible F5 menu while the map changes.
			// If it is closed, the pending bounded recovery will lock relative input
			// on the first frames where engine->is_playing() becomes true.
		}
	}

	void imgui::style_xo()
	{
		ImGuiStyle& style = ImGui::GetStyle();
		style.Alpha = 1.0f;
		style.DisabledAlpha = 0.5f;

		style.WindowPadding = ImVec2(12.0f, 12.0f);
		style.FramePadding = ImVec2(10.0f, 6.0f);
		style.ItemSpacing = ImVec2(8.0f, 7.0f);
		style.ItemInnerSpacing = ImVec2(7.0f, 6.0f);
		style.IndentSpacing = 0.0f;
		style.ColumnsMinSpacing = 10.0f;
		style.ScrollbarSize = 12.0f;
		style.GrabMinSize = 10.0f;

		style.WindowBorderSize = 1.0f;
		style.ChildBorderSize = 1.0f;
		style.PopupBorderSize = 1.0f;
		style.FrameBorderSize = 1.0f;
		style.TabBorderSize = 0.0f;

		style.WindowRounding = 9.0f;
		style.ChildRounding = 10.0f;
		style.FrameRounding = 7.0f;
		style.PopupRounding = 8.0f;
		style.ScrollbarRounding = 8.0f;
		style.GrabRounding = 6.0f;
		style.TabRounding = 7.0f;

		style.CellPadding = ImVec2(7.0f, 5.0f);

		auto& colors = style.Colors;
		colors[ImGuiCol_Text] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
		colors[ImGuiCol_TextDisabled] = ImVec4(0.44f, 0.44f, 0.44f, 1.00f);
		colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.082f, 0.092f, 0.94f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.105f, 0.115f, 0.128f, 0.96f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.085f, 0.095f, 0.105f, 0.97f);
		colors[ImGuiCol_Border] = ImVec4(0.24f, 0.29f, 0.32f, 0.92f);
		colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.23f);
		colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.15f, 0.17f, 1.00f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.26f, 0.30f, 1.00f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.08f, 0.42f, 0.52f, 0.70f);
		colors[ImGuiCol_TitleBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.98f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.15f, 0.98f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15f, 0.15f, 0.15f, 0.98f);
		colors[ImGuiCol_MenuBarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.24f);
		colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.34f, 0.34f, 0.34f, 0.39f);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.54f, 0.54f, 0.54f, 0.47f);
		colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.78f, 0.78f, 0.78f, 0.33f);
		colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
		colors[ImGuiCol_SliderGrab] = ImVec4(1.00f, 1.00f, 1.00f, 0.39f);
		colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.31f);
		colors[ImGuiCol_Button] = ImVec4(0.14f, 0.18f, 0.20f, 1.00f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.34f, 0.38f, 1.00f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.46f, 0.55f, 1.00f);
		colors[ImGuiCol_Header] = ImVec4(0.02f, 0.02f, 0.02f, 0.39f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.17f, 0.25f, 0.27f, 0.78f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.17f, 0.25f, 0.27f, 0.78f);
		colors[ImGuiCol_Separator] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
		colors[ImGuiCol_SeparatorHovered] = ImVec4(0.15f, 0.52f, 0.66f, 0.30f);
		colors[ImGuiCol_SeparatorActive] = ImVec4(0.30f, 0.69f, 0.84f, 0.39f);
		colors[ImGuiCol_ResizeGrip] = ImVec4(0.43f, 0.43f, 0.43f, 0.51f);
		colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.07f, 0.39f, 0.47f, 0.59f);
		colors[ImGuiCol_ResizeGripActive] = ImVec4(0.30f, 0.69f, 0.84f, 0.39f);
		colors[ImGuiCol_TabHovered] = ImVec4(0.20f, 0.54f, 0.66f, 0.52f);
		colors[ImGuiCol_Tab] = ImVec4(0.09f, 0.11f, 0.13f, 0.72f);
		colors[ImGuiCol_TabSelected] = ImVec4(0.12f, 0.43f, 0.55f, 0.84f);
		colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.10f, 0.34f, 0.43f, 0.30f);
		colors[ImGuiCol_TabDimmed] = ImVec4(0.00f, 0.00f, 0.00f, 0.16f);
		colors[ImGuiCol_TabDimmedSelected] = ImVec4(1.00f, 1.00f, 1.00f, 0.24f);
		colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.50f, 0.50f, 0.50f, 0.00f);
		colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 1.00f, 1.00f, 0.35f);
		colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
		colors[ImGuiCol_PlotHistogram] = ImVec4(1.00f, 1.00f, 1.00f, 0.35f);
		colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
		colors[ImGuiCol_TableHeaderBg] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
		colors[ImGuiCol_TableBorderStrong] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
		colors[ImGuiCol_TableBorderLight] = ImVec4(0.00f, 0.00f, 0.00f, 0.54f);
		colors[ImGuiCol_TableRowBg] = ImVec4(0.08f, 0.09f, 0.10f, 0.45f);
		colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.13f, 0.19f, 0.21f, 0.62f);
		colors[ImGuiCol_TextLink] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
		colors[ImGuiCol_TextSelectedBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
		colors[ImGuiCol_DragDropTarget] = ImVec4(0.00f, 0.51f, 0.39f, 0.31f);
		colors[ImGuiCol_NavCursor] = ImVec4(0.86f, 0.86f, 0.86f, 1.00f);
		colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
		colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
		colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.56f);

		// custom colors
		ImGuiCol_ButtonGreen = ImVec4(0.3f, 0.4f, 0.05f, 0.7f);
		ImGuiCol_ButtonYellow = ImVec4(0.4f, 0.3f, 0.1f, 0.8f);
		ImGuiCol_ButtonRed = ImVec4(0.48f, 0.15f, 0.15f, 1.00f);
		ImGuiCol_ContainerBackground = ImVec4(0.115f, 0.125f, 0.135f, 0.940f);
		ImGuiCol_ContainerBorder = ImVec4(0.040f, 0.045f, 0.050f, 0.950f);
	}

	void init_fonts()
	{
		using namespace common::imgui::font;

		auto merge_icons_with_latest_font = [](const float& font_size, const bool font_data_owned_by_atlas = false)
			{
				static const ImWchar icons_ranges[] = {ICON_MIN_FA, ICON_MAX_FA, 0 };

				ImFontConfig icons_config;
				icons_config.MergeMode = true;
				icons_config.PixelSnapH = true;
				icons_config.FontDataOwnedByAtlas = font_data_owned_by_atlas;

				ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void*)fa_solid_900, sizeof(fa_solid_900), font_size, &icons_config, icons_ranges);
			};

		ImGuiIO& io = ImGui::GetIO();

		io.Fonts->AddFontFromMemoryCompressedTTF(opensans_bold_compressed_data, opensans_bold_compressed_size, 18.0f);
		merge_icons_with_latest_font(12.0f, false);

		io.Fonts->AddFontFromMemoryCompressedTTF(opensans_bold_compressed_data, opensans_bold_compressed_size, 17.0f);
		merge_icons_with_latest_font(12.0f, false);

		io.Fonts->AddFontFromMemoryCompressedTTF(opensans_regular_compressed_data, opensans_regular_compressed_size, 18.0f);
		io.Fonts->AddFontFromMemoryCompressedTTF(opensans_regular_compressed_data, opensans_regular_compressed_size, 16.0f);

		ImFontConfig font_cfg;
		font_cfg.FontDataOwnedByAtlas = false;

		io.FontDefault = io.Fonts->AddFontFromMemoryCompressedTTF(opensans_regular_compressed_data, opensans_regular_compressed_size, 17.0f, &font_cfg);
		merge_icons_with_latest_font(17.0f, false);
	}

	namespace
	{
		using present_fn = long(__stdcall*)(IDirect3DDevice9*, RECT*, RECT*, HWND, RGNDATA*); present_fn present_original = {};
		long __stdcall present_hk(IDirect3DDevice9* device, RECT* source_rect, RECT* dest_rect, HWND dest_window_override, RGNDATA* dirty_region)
		{
			g_overlay_present_device = device;
			if (auto* im = imgui::get(); im && im->overlay_present_available())
			{
				// Input recovery remains available even if the Remix callback has not
				// initialized yet. Rendering through Present is front-end-only.
				im->service_gameplay_input_state(true);
				if (!im->gameplay_present_active()) imgui::endscene_stub(true);
			}
			g_overlay_present_device = nullptr;

			return present_original(device, source_rect, dest_rect, dest_window_override, dirty_region);
		}

		using reset_fn = long(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*); reset_fn reset_original = {};
		long __stdcall reset_hk(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* present_parameters)
		{
			auto* im = imgui::get();
			const bool backend_initialized = im && im->m_initialized_device;
			if (backend_initialized) ImGui_ImplDX9_InvalidateDeviceObjects();
			const auto result = reset_original(device, present_parameters);
			if (backend_initialized && SUCCEEDED(result)) ImGui_ImplDX9_CreateDeviceObjects();
			return result;
		}
	}

	imgui::imgui()
	{
		p_this = this;

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		init_fonts();

		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

		style_xo();

		ImGui_ImplWin32_Init(glob::main_window);
		g_game_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(glob::main_window, GWLP_WNDPROC, LONG_PTR(wnd_proc_hk)));


		auto get_virtual = [](void* _class, unsigned int index) {
			return static_cast<unsigned int>((*static_cast<int**>(_class))[index]);
		};

		const auto dev = game::get_d3d_device();
		MH_CreateHook(reinterpret_cast<void*>(get_virtual(dev, 17)), present_hk, reinterpret_cast<void**>(&present_original));
		MH_CreateHook(reinterpret_cast<void*>(get_virtual(dev, 16)), reset_hk, reinterpret_cast<void**>(&reset_original));
	}

	imgui::~imgui()
	{
		release_menu_cursor();
		if (m_initialized_device) ImGui_ImplDX9_Shutdown();
		ImGui_ImplWin32_Shutdown();

		if (g_about_strawberry_texture)
		{
			g_about_strawberry_texture->Release();
			g_about_strawberry_texture = nullptr;
		}
	}
}
