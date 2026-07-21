#pragma once

namespace components
{
	class imgui : public component
	{
	public:
		imgui();
		~imgui();

		static inline imgui* p_this = nullptr;
		static imgui* get() { return p_this; }

		static void endscene_stub(bool isolated_present_scene);
		static void on_map_load();
		bool overlay_present_available() const;
		bool gameplay_present_active() const;
		void suspend_for_loading_screen();
		void acquire_menu_cursor();
		void release_menu_cursor();
		void apply_cursor_visibility(bool visible, const char* reason);
		void maintain_menu_cursor_ownership();
		void schedule_gameplay_input_recovery(std::uint32_t frames, const char* reason);
		void recover_gameplay_input_frame();
		void service_gameplay_input_state(bool apply_recovery_frame);
		bool menu_owns_input() const { return m_menu_active && m_menu_input_armed && !m_im_allow_game_input; }
		bool game_input_passthrough() const { return !menu_owns_input(); }

		void devgui();
		bool input_message(UINT message_type, WPARAM wparam, LPARAM lparam);

		bool m_menu_active = false;
		bool m_menu_input_armed = false;
		bool m_menu_just_opened = false;
		bool m_initialized_device = false;
		bool m_cursor_visibility_known = false;
		bool m_cursor_visible_applied = false;
		bool m_menu_cursor_owned = false;
		bool m_cursor_visible_before_menu = false;
		bool m_loading_screen_suspended = false;
		bool m_gameplay_present_was_active = false;
		std::uint32_t m_gameplay_input_recovery_frames = 0u;
		std::uint64_t m_cursor_visibility_transitions = 0u;
		std::uint64_t m_cursor_unlock_maintenance = 0u;
		std::uint64_t m_cursor_visibility_recoveries = 0u;
		std::uint64_t m_loading_passthrough_frames = 0u;
		std::uint64_t m_menu_open_requests = 0u;
		std::uint64_t m_menu_close_requests = 0u;
		std::uint64_t m_menu_frames_rendered = 0u;
		std::uint64_t m_menu_input_arm_events = 0u;
		std::uint64_t m_backend_init_attempts = 0u;
		std::uint64_t m_backend_init_failures = 0u;
		std::uint64_t m_overlay_safe_backbuffer_frames = 0u;
		std::uint64_t m_overlay_frontend_present_frames = 0u;
		std::uint64_t m_overlay_gameplay_bridge_frames = 0u;
		std::uint64_t m_overlay_backbuffer_failures = 0u;
		std::uint64_t m_overlay_begin_scene_failures = 0u;
		std::uint64_t m_overlay_end_scene_failures = 0u;
		std::uint64_t m_gameplay_input_recovery_events = 0u;
		std::uint64_t m_gameplay_input_recovery_frames_applied = 0u;
		static inline std::uint64_t m_cursor_center_count = 0u;
		static inline std::uint64_t m_cursor_center_suppressed_count = 0u;
		static inline bool m_legacy_cursor_warp_enabled = false;
		static inline std::string m_cursor_center_last_reason = "never";

		void style_xo();

		static bool cvar_toggle_button_bool(const char* cvar_str, const char* btn_text, ImVec2 btn_size = ImVec2(0, 0), const char* tt_text = nullptr, bool invert = false);
		static bool toggle_button_bool(bool* bool_ptr, const char* btn_text, ImVec2 btn_size = ImVec2(0, 0), const char* tt_text = nullptr, bool invert = false);
		static bool cvar_toggle_button_int(const char* cvar_str, const char* btn_text, ImVec2 btn_size = ImVec2(0, 0), const char* tt_text = nullptr, int off_override = 0, int on_override = 0);

		ImVec4 ImGuiCol_ButtonGreen = ImVec4(0.3f, 0.4f, 0.05f, 0.7f);
		ImVec4 ImGuiCol_ButtonYellow = ImVec4(0.4f, 0.3f, 0.1f, 0.8f);
		ImVec4 ImGuiCol_ButtonRed = ImVec4(0.48f, 0.15f, 0.15f, 1.00f);
		ImVec4 ImGuiCol_ContainerBackground = ImVec4(0.220f, 0.220f, 0.220f, 0.863f);
		ImVec4 ImGuiCol_ContainerBorder = ImVec4(0.099f, 0.099f, 0.099f, 0.901f);

		// the following default values will be used on release builds
		bool m_disable_cullnode = false;
		bool m_enable_area_forcing = true;
		bool m_light_edit_mode = false;
		bool m_debug_disable_unbake = false;
		bool m_debug_unbake_all_single_bones = false;

		bool m_debugvis_live = false;
		bool m_debugvis_radius = true;
		bool m_debugvis_shaping = true;
		bool m_debugvis_light_labels = true;
		bool m_debugvis_ies_cluster = true;
		bool m_debugvis_attach_bounds = true;
		bool m_light_editor_preview_all = true;
		bool m_light_editor_gizmos_all = true;
		bool m_light_editor_mouse_drag = true;
		bool m_light_editor_drag_whole_light = true;
		bool m_light_editor_axis_gizmo = true;
		bool m_light_editor_plane_gizmo = true;
		bool m_light_editor_center_handle = true;
		bool m_light_editor_keyboard_controls = true;
		bool m_light_editor_snap = false;
		bool m_light_editor_selected_always_visible = true;
		bool m_light_editor_dynamic_fov = true;
		int m_light_editor_transform_mode = 0; // 0 translate, 1 rotate, 2 radius/shape, 3 intensity
		int m_light_editor_axis_constraint = 0; // 0 camera/free, 1 X, 2 Y, 3 Z
		float m_light_editor_keyboard_step = 1.0f;
		float m_light_editor_angle_step = 5.0f;
		float m_light_editor_snap_step = 1.0f;
		float m_light_editor_gizmo_axis_length = 54.0f;
		float m_light_editor_gizmo_pick_radius = 14.0f;
		float m_light_editor_gizmo_drag_scale = 0.0015f;
		float m_light_editor_gizmo_depth_scale = 0.12f;
		float m_light_editor_gizmo_radius_scale = 0.025f;
		float m_light_editor_gizmo_intensity_scale = 0.15f;
		float m_light_editor_gizmo_rotate_scale = 0.015f;
		float m_light_editor_gizmo_visibility_distance = 2048.0f;
		float m_light_editor_gizmo_fade_start = 1536.0f;
		float m_light_editor_label_visibility_distance = 270.0f;
		float m_light_editor_shape_visibility_distance = 270.0f;
		float m_light_editor_drag_threshold = 3.0f;
		float m_light_editor_manual_fov = 75.0f;
		int m_light_editor_max_visible_gizmos = 128;
		float m_light_editor_spawn_distance = 2048.0f;
		bool m_light_editor_surface_placement = true;
		float m_light_editor_surface_offset = 2.0f;
		float m_light_editor_spawn_radius = 1.0f;
		float m_light_editor_spawn_intensity = 12.0f;
		float m_debugvis_cone_height = 60.0f;
		int m_debugvis_cone_steps = 3u;

		float m_debug_float_vec4[4] = {};
		int m_debug_int_vec4[4] = {};

	private:
		void tab_general();
		void tab_experimental();
		void tab_import_lights_from_maps();
		void tab_import_materials_from_maps();
		void tab_light_studio();
		void tab_lights();
		void tab_player_flashlight();
		void tab_muzzle_flash();
		void tab_markers();
		void tab_performance();
		void tab_map_settings();
		void tab_game_settings();
		void tab_diagnostics();
		void tab_about();
		bool m_im_window_focused = false;
		bool m_im_window_hovered = false;
		bool m_im_allow_game_input = false;
		std::string m_devgui_custom_footer_content;

		static void questionmark(const char* desc)
		{
			ImGui::TextDisabled("(?)");
			if (ImGui::BeginItemTooltip())
			{
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
				ImGui::TextUnformatted(desc);
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
		}
	};
}
