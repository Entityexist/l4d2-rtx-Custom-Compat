#pragma once

namespace components
{
	namespace cmd
	{
		extern bool debug_node_vis;
	}

	extern int g_current_leaf;
	extern int g_current_area;
	extern map_settings::area_overrides_s* g_player_current_area_override;

	extern bool g_use_playershadow;
	extern int  g_is_rendering_our_thirdperson_mesh;

	class main_module : public component
	{
	public:
		main_module();
		~main_module();

		static inline main_module* p_this = nullptr;
		static main_module* get() { return p_this; }

		static inline std::uint64_t framecount = 0u;
		static inline LPD3DXFONT d3d_font = nullptr;

		static void iterate_entities();
		static void force_cvars();
		static void cross_handle_map_and_game_settings();

		static void hud_draw_area_info();
		static void trigger_vis_logic();
		static void xo_debug_toggle_node_vis_fn();

		struct sky3d_diagnostics_s
		{
			bool hook_installed = false;
			bool rate_limited_logging = false;
			std::uint64_t hook_calls = 0u;
			std::uint64_t hook_non_3d_calls = 0u;
			std::uint64_t view_3dsky_detections = 0u;
			std::uint64_t sky_camera_found = 0u;
			std::uint64_t valid_payloads = 0u;
			std::uint64_t invalid_scale = 0u;
			std::uint64_t invalid_origin = 0u;
			std::uint64_t invalid_camera_origin = 0u;
			std::uint64_t invalid_area = 0u;
			std::uint64_t payload_reused = 0u;
			std::uint64_t payload_refreshes = 0u;
			std::uint64_t payload_transform_changes = 0u;
			std::uint64_t recovery_payloads = 0u;
			std::uint64_t recovery_capture_rejected = 0u;
			std::uint64_t payload_stale = 0u;
			std::uint64_t payload_resets = 0u;
			std::uint64_t safe_fallbacks = 0u;
			std::uint64_t static_candidates = 0u;
			std::uint64_t submitted_objects = 0u;
			std::uint64_t source_draw_suppressed = 0u;
			std::uint64_t rejected_missing_payload = 0u;
			std::uint64_t payload_generation = 0u;
			std::uint64_t last_payload_signature = 0u;
			std::uint64_t last_payload_frame = 0u;
			std::uint64_t last_valid_payload_frame = 0u;
			std::uint64_t last_hook_payload_frame = 0u;
			std::uint64_t last_candidate_frame = 0u;
			std::uint64_t last_submission_frame = 0u;
			std::uint64_t last_suppressed_frame = 0u;
			std::uint64_t last_reuse_note_frame = std::numeric_limits<std::uint64_t>::max();
			std::uint64_t last_stale_note_frame = std::numeric_limits<std::uint64_t>::max();
			std::uint64_t last_recovery_reject_note_frame = std::numeric_limits<std::uint64_t>::max();
			std::uint64_t last_fallback_note_frame = std::numeric_limits<std::uint64_t>::max();
			std::uint32_t last_view_id = 0u;
			Vector last_sky_camera_position = {};
			Vector last_origin = {};
			int last_scale = 0;
			int last_area = -1;
			bool last_payload_hook_confirmed = false;
			std::string last_payload_source = "none";
			std::string last_stage = "not observed";
			std::chrono::steady_clock::time_point last_log_time = {};
		};

		static sky3d_diagnostics_s& sky3d_diagnostics();
		static void reset_sky3d_diagnostics();
		static bool capture_sky3d_payload(const CSkyCamera* sky, const Vector* camera_origin, const char* source);
		static void reset_sky3d_payload(const char* reason);
		static bool sky3d_payload_is_fresh(std::uint64_t max_age_frames = 0u);
		static bool sky3d_payload_is_hook_confirmed(std::uint64_t max_age_frames = 0u);
		static bool sky3d_payload_is_capture_eligible(std::uint64_t max_age_frames = 0u);
		static std::uint64_t sky3d_payload_age_frames();
		static std::uint64_t sky3d_hook_payload_age_frames();
		static std::uint64_t sky3d_payload_signature();
		static const char* sky3d_health_summary();
		static std::string export_runtime_diagnostics();
		static void note_sky3d_static_candidate();
		static void note_sky3d_submitted_object();
		static void note_sky3d_missing_payload();
		static void note_sky3d_stale_payload();
		static void note_sky3d_recovery_rejected();
		static void note_sky3d_safe_fallback();
		static void note_sky3d_source_suppressed();

		int m_sky3d_scale = 0;
		int m_sky3d_area = -1;
		Vector m_sky3d_origin = {};
		Vector m_sky3d_camera_origin = {};
		std::uint64_t m_sky3d_payload_frame = 0u;
		std::uint64_t m_sky3d_hook_payload_frame = 0u;
		std::uint64_t m_sky3d_payload_generation = 0u;
		std::uint64_t m_sky3d_payload_signature = 0u;
		std::uint64_t m_sky3d_hook_payload_signature = 0u;
		bool m_sky3d_payload_hook_confirmed = false;
		std::string m_playermodel_substr;
		Vector m_player_eye_pos = {};
		sky3d_diagnostics_s m_sky3d_diag = {};

		int  m_hud_debug_node_vis_pos[2] = { 250, 135 };
		bool m_hud_debug_node_vis_has_forced_leafs = false;
		bool m_hud_debug_node_vis_has_forced_arealeafs = false;
	};
}
