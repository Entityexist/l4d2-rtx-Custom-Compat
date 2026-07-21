#pragma once

namespace components
{
	class remix_api : public component
	{
	public:
		remix_api();

		static inline remix_api* p_this = nullptr;
		static remix_api* get() { return p_this; }

		void on_renderview();
		bool try_initialize(bool allow_console_log = false);
		static bool is_initialized() { return p_this && p_this->m_initialized; }

		static constexpr std::uint32_t M_MAX_DEBUG_LINES = 512u;
		enum DEBUG_REMIX_LINE_COLOR
		{
			RED = 0u,
			GREEN = 1u,
			TEAL = 2u,
			WHITE = 3u,
		};

		void init_debug_lines();
		void create_quad(remixapi_HardcodedVertex* v_out, uint32_t* i_out, float scale);
		void create_line_quad(remixapi_HardcodedVertex* v_out, uint32_t* i_out, const Vector& p1, const Vector& p2, float width);
		void add_debug_line(const Vector& p1, const Vector& p2, float width, DEBUG_REMIX_LINE_COLOR color);

		void add_debug_circle_based_on_previous(const Vector& center, const Vector& rot, const Vector& scale);
		void add_debug_circle(const Vector& center, const Vector& normal, float radius, float thickness, const Vector& color, bool drawcall_alpha = true);

		static bool can_add_debug_lines()
		{
			const auto api = get();
			return api && api->m_initialized && api->m_debug_line_amount + 1u < M_MAX_DEBUG_LINES;
		}

		void debug_draw_box(const Vector& mins, const Vector& maxs, float line_width, const DEBUG_REMIX_LINE_COLOR& color);
		void debug_draw_box(const VectorAligned& center, const VectorAligned& half_diagonal, float line_width, const DEBUG_REMIX_LINE_COLOR& color);

		void flashlight_create_or_update(const char* player_name, const Vector& pos, const Vector& fwd, const Vector& rt, const Vector& up, bool is_enabled, bool is_player = false);
		static void flashlight_frame();

		struct flashlight_runtime_stats_s
		{
			std::uint64_t frame_updates = 0u;
			std::uint64_t throttled_updates = 0u;
			std::uint64_t stationary_reuses = 0u;
			std::uint64_t forced_motion_updates = 0u;
			std::uint64_t configuration_updates = 0u;
			std::uint64_t create_attempts = 0u;
			std::uint64_t create_failures = 0u;
			std::uint64_t destroyed_handles = 0u;
			std::uint64_t submitted_main = 0u;
			std::uint64_t submitted_core = 0u;
			std::uint64_t submitted_hotspot = 0u;
			std::uint64_t submitted_spill = 0u;
			std::uint64_t removed_stale_owners = 0u;
			std::uint64_t culled_distant_bots = 0u;
			std::uint64_t budget_degraded_rigs = 0u;
			std::uint64_t budget_dropped_layers = 0u;
			std::uint64_t transactional_commits = 0u;
			std::uint64_t transactional_rollbacks = 0u;
			std::uint64_t stale_rig_fallbacks = 0u;
			std::uint64_t retry_suppressed = 0u;
			std::uint64_t draw_failures = 0u;
			std::uint64_t invalidated_handles = 0u;
			std::uint64_t owner_grace_frames_used = 0u;
			std::uint32_t tracked_owners = 0u;
			std::uint32_t active_handles = 0u;
			std::uint32_t requested_layers = 0u;
			std::uint32_t granted_layers = 0u;
		};

		static flashlight_runtime_stats_s flashlight_runtime_stats();
		static void reset_flashlight_runtime_stats();
		static void clear_flashlights();

		remixapi_Interface m_bridge = {};

		struct flashlight_def_s
		{
			Vector pos;
			Vector fwd = { 0.0f, 1.0f, 0.0f };
			Vector rt;
			Vector up;
		};

		struct flashlight_s
		{
			remixapi_LightHandle handle = nullptr;
			remixapi_LightInfoSphereEXT ext = {};
			remixapi_LightInfo info = {};

			remixapi_LightHandle handle_inner = nullptr;
			remixapi_LightInfoSphereEXT ext_inner = {};
			remixapi_LightInfo info_inner = {};

			remixapi_LightHandle handle_hotspot = nullptr;
			remixapi_LightInfoSphereEXT ext_hotspot = {};
			remixapi_LightInfo info_hotspot = {};

			remixapi_LightHandle handle_spill = nullptr;
			remixapi_LightInfoSphereEXT ext_spill = {};
			remixapi_LightInfo info_spill = {};

			flashlight_def_s def = {};
			Vector last_built_pos = {};
			Vector last_built_fwd = { 0.0f, 1.0f, 0.0f };
			std::chrono::steady_clock::time_point last_update_time = {};
			std::uint64_t last_config_fingerprint = 0u;
			std::uint64_t last_seen_frame = 0u;
			std::chrono::steady_clock::time_point retry_after = {};
			std::uint32_t consecutive_build_failures = 0u;
			std::uint32_t missed_owner_frames = 0u;
			std::uint8_t active_layer_mask = 0u;
			std::uint8_t desired_layer_mask = 0u;
			bool has_built_state = false;
			bool stale_rig_fallback = false;
			bool is_player = false;
			bool is_enabled = false;
			bool is_alive = false; // original compatibility marker reset after each per-frame rebuild
		};
		std::unordered_map<std::string, flashlight_s> m_flashlights;
		flashlight_runtime_stats_s m_flashlight_runtime_stats = {};

	private:
		static void begin_scene_callback();
		static void end_scene_callback();
		static void on_present_callback();

		bool m_initialized = false;
		bool m_reported_init_failure = false;
		remixapi_ErrorCode m_last_init_status = REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		int m_last_source_render_frame = std::numeric_limits<int>::min();
		float m_last_source_realtime = -std::numeric_limits<float>::infinity();
		std::uint64_t m_duplicate_render_views_skipped = 0u;

		bool m_debug_lines_initialized = false;
		remixapi_MaterialHandle m_debug_line_materials[4];
		remixapi_MeshHandle m_debug_line_list[M_MAX_DEBUG_LINES];
		std::uint32_t m_debug_line_amount = 0u;
		std::uint64_t m_debug_last_line_hash = 0u;

		struct dbg_circle
		{
			remixapi_MeshHandle handle = nullptr;
			remixapi_Transform transform = {};
			bool uses_custom_transform = false;
		};
		std::vector<dbg_circle> m_debug_circles;
		std::uint64_t m_debug_circles_last_hash = 0u;
		std::vector<remixapi_MaterialHandle> m_debug_circle_materials;
	};
}