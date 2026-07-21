#pragma once
#include <deque>
#include "map_settings.hpp"

namespace components
{
	namespace cmd
	{
		extern bool debug_pos_time;
		extern bool show_api_lights;
		extern bool show_mesh_bone_info_attached;
		extern bool show_mesh_bone_info;
	}

	class remix_lights : public component
	{
	public:
		remix_lights();

		static inline remix_lights* p_this = nullptr;
		static remix_lights* get() { return p_this; }

		static void on_draw_model_exec(const ModelRenderInfo_t& info);
		static void on_event_start(const std::string_view& name, const std::string_view& actor, const std::string_view& event, const std::string_view& param1);
		static void on_event_finish(const std::string_view& name);
		static void on_sound_start(std::uint32_t hash);
		static void on_client_frame();
		static void on_map_load();

		// -----

		class light
		{
		public:

			class interpolator
			{
			public:
				interpolator() = default;

				bool init(const std::vector<map_settings::remix_light_settings_s::point_s>& points,
					bool looping = false, bool loop_smoothing = false);

				bool is_initialized() const { return m_initialized; }

				bool advance_time(float frametime);
				void restart() { m_elapsed_time = 0.0f; }

				void interpolate(
					light* parent,
					remixapi_Float3D* position = nullptr,
					remixapi_Float3D* radiance = nullptr,
					float* radius = nullptr,
					remixapi_Float3D* direction = nullptr,
					float* degrees = nullptr,
					float* softness = nullptr,
					float* exponent = nullptr,
					float* volumetric_scale = nullptr,
					bool is_attached = false
				);

				map_settings::remix_light_settings_s::point_s* get_points() {
					return m_points.data();
				}

				std::vector<map_settings::remix_light_settings_s::point_s>& get_points_vec() {
					return m_points;
				}

				size_t get_points_count() const {
					return m_points.size();
				}

			private:
				bool m_initialized = false;
				bool m_looping = false;
				bool m_loop_smoothing = false;
				std::vector<map_settings::remix_light_settings_s::point_s> m_points;
				std::vector<float> m_segment_durations;
				float m_elapsed_time = 0.0f;
				float m_total_duration = 0.0f;

			public:
				void calculate_segment_durations()
				{
					// total duration defined by the last point
					m_total_duration = m_points.back().timepoint;

					m_segment_durations.clear();
					float total_original_duration = 0.0f;

					// calculate the sum of all segment durations as defined by the points
					for (size_t i = 0; i < m_points.size() - 1; ++i)
					{
						float segment_duration = (m_points)[i + 1].timepoint - (m_points)[i].timepoint;
						total_original_duration += segment_duration;
						m_segment_durations.push_back(segment_duration);
					}

					if (m_looping && m_loop_smoothing && m_points.size() > 1)
					{
						float last_to_first_duration = m_points.back().timepoint - (m_points)[m_points.size() - 2].timepoint;
						total_original_duration += last_to_first_duration;
						m_segment_durations.push_back(last_to_first_duration);
					}

					if (m_looping && m_loop_smoothing && total_original_duration > 0.0f)
					{
						const float scale_factor = m_total_duration / total_original_duration;
						for (auto& duration : m_segment_durations) {
							duration *= scale_factor;
						}
					}
				}

			private:
				void interpolate_timepoints(const size_t start, const size_t end)
				{
					float prev_time = (start > 0) ? (m_points)[start - 1].timepoint : 0.0f;
					const float next_time = (m_points)[end].timepoint;

					const size_t segment_count = end - start;
					const float segment_duration = (next_time - prev_time) / static_cast<float>(segment_count + 1);

					for (size_t i = start; i < end; ++i)
					{
						if ((m_points)[i].timepoint == 0.0f) {
							(m_points)[i].timepoint = prev_time + segment_duration;
						}
						prev_time = (m_points)[i].timepoint; // update for next iteration
					}
				}

				template<typename T>
				T lerp(const T& start, const T& end, float t) const {
					return start + (end - start) * t;
				}
			};

			// ----

			bool has_spawn_trigger() const { return m_def.trigger_sound_hash || !m_def.trigger_choreo_actor.empty(); }
			bool has_kill_trigger() const { return m_def.kill_sound_hash || !m_def.kill_choreo_name.empty(); }
			bool has_attach_parms() const { return m_def.attach_prop_radius != 0.0f || !m_def.attach_prop_name.empty(); }
			bool is_attached() const { return m_attachframe && m_attachframe == m_attachframe_counter; }

			Vector calculate_direction_for_point(const map_settings::remix_light_settings_s::point_s* point) const;
			Vector calculate_position_for_point(const map_settings::remix_light_settings_s::point_s* point) const;

			// ----
			
			map_settings::remix_light_settings_s m_def = {};
			std::uint32_t m_light_num = 0u;
			float m_timer = 0.0f;
			bool m_is_marked_for_destruction = false;
			interpolator m_mover;
			remixapi_LightHandle m_handle = nullptr;

			// Canonical sphere representation used by interpolation, attachment, debug drawing,
			// compatibility SetLight mirroring and Fake IES helper generation. Native analytical
			// geometry below is rebuilt from this canonical state before every CreateLight call.
			remixapi_LightInfoSphereEXT m_ext = {};

			// ABI-local analytical geometry records. The compatibility ASI bridge header predates
			// these public Remix structs, so V19.7 mirrors their stable C layouts and chains them
			// through remixapi_LightInfo::pNext without changing the generated bridge package.
			struct native_rect_light_info_ext_s
			{
				remixapi_StructType sType = static_cast<remixapi_StructType>(10);
				void* pNext = nullptr;
				remixapi_Float3D position = {};
				remixapi_Float3D xAxis = {};
				float xSize = 1.0f;
				remixapi_Float3D yAxis = {};
				float ySize = 1.0f;
				remixapi_Float3D direction = {};
				std::uint32_t shaping_hasvalue = 0u;
				remixapi_LightInfoLightShaping shaping_value = {};
				float volumetricRadianceScale = 1.0f;
			};

			struct native_disk_light_info_ext_s
			{
				remixapi_StructType sType = static_cast<remixapi_StructType>(9);
				void* pNext = nullptr;
				remixapi_Float3D position = {};
				remixapi_Float3D xAxis = {};
				float xRadius = 0.5f;
				remixapi_Float3D yAxis = {};
				float yRadius = 0.5f;
				remixapi_Float3D direction = {};
				std::uint32_t shaping_hasvalue = 0u;
				remixapi_LightInfoLightShaping shaping_value = {};
				float volumetricRadianceScale = 1.0f;
			};

			struct native_cylinder_light_info_ext_s
			{
				remixapi_StructType sType = static_cast<remixapi_StructType>(8);
				void* pNext = nullptr;
				remixapi_Float3D position = {};
				float radius = 0.5f;
				remixapi_Float3D axis = {};
				float axisLength = 1.0f;
				float volumetricRadianceScale = 1.0f;
			};

			struct native_distant_light_info_ext_s
			{
				remixapi_StructType sType = static_cast<remixapi_StructType>(7);
				void* pNext = nullptr;
				remixapi_Float3D direction = {};
				float angularDiameterDegrees = 0.53f;
				float volumetricRadianceScale = 1.0f;
			};

			static_assert(sizeof(native_distant_light_info_ext_s) == (sizeof(void*) == 8u ? 40u : 28u),
				"Remix Distant ABI layout changed");

			native_rect_light_info_ext_s m_rect_ext = {};
			native_disk_light_info_ext_s m_disk_ext = {};
			native_cylinder_light_info_ext_s m_cylinder_ext = {};
			native_distant_light_info_ext_s m_distant_ext = {};
			remixapi_LightInfo m_info = {};

			// ABI-compatible with remixapi_LightInfoIESEXT from the paired IES-enabled DXVK Remix build.
			// The compatibility ASI intentionally keeps its bridge-generated header unchanged and appends
			// this extension through pNext using the public sType value 28.
			struct native_ies_light_info_ext_s
			{
				remixapi_StructType sType = static_cast<remixapi_StructType>(28);
				void* pNext = nullptr;
				const wchar_t* profilePath = nullptr;
				remixapi_Float3D direction = {};
				float axisRotationDegrees = 0.0f;
				float angleScale = 1.0f;
				float intensityScale = 1.0f;
				std::uint32_t normalize = 1u;
			};

			native_ies_light_info_ext_s m_native_ies_ext = {};
			std::string m_native_ies_authored_path = {};
			std::wstring m_native_ies_profile_path = {};
			bool m_native_ies_resolution_failed = false;
			std::string m_native_ies_status = "Legacy light rig";

			struct ies_child_light_s
			{
				remixapi_LightHandle handle = nullptr;
				remixapi_LightInfoSphereEXT ext = {};
				remixapi_LightInfo info = {};
			};
			std::vector<ies_child_light_s> m_ies_children = {};
			Vector m_attached_position;
			Vector m_attached_angle;
			Vector m_attached_bone_forward = { 1.0f, 0.0f, 0.0f };
			Vector m_attached_bone_right = { 0.0f, 1.0f, 0.0f };
			Vector m_attached_bone_up = { 0.0f, 0.0f, 1.0f };
			std::uint32_t m_attachframe = 0u;
			int m_entity_index = -1;
		};

		bool update_static_remix_light(light* light, const map_settings::remix_light_settings_s::point_s* pt);
		bool update_remix_light(light* light);
		bool spawn_remix_light(light* light);

		void add_all_map_setting_lights_without_creation_trigger();

		void add_single_map_setting_light_for_editing(map_settings::remix_light_settings_s* def);
		void add_single_map_setting_light(map_settings::remix_light_settings_s* def);
		bool add_single_map_setting_light_report(map_settings::remix_light_settings_s* def);
		bool upsert_runtime_light(const map_settings::remix_light_settings_s& def, bool enabled);
		bool has_light_with_exact_comment(std::string_view comment) const;

		// V20.6: Source environment lighting owns one global-light submission with two
		// independent backends. Native Remix Distant is preferred; D3D9 directional
		// SetLight remains available as an ASI-level compatibility fallback.
		bool upsert_source_distant_light(std::uint64_t stable_hash, Vector direction,
			const Vector& radiance, float angular_diameter_degrees, float volumetric_scale = 1.0f);
		void destroy_source_distant_light();
		bool source_distant_light_active() const { return m_source_distant_handle != nullptr; }
		bool source_directional_ff_active() const { return m_source_directional_ff_active && m_source_directional_ff_enabled; }
		int source_distant_last_error_code() const { return static_cast<int>(m_source_distant_last_error); }
		std::uint64_t source_distant_draw_calls() const { return m_source_distant_draw_calls; }
		std::uint64_t source_directional_ff_draw_calls() const { return m_source_directional_ff_draw_calls; }
		const std::string& source_distant_runtime_status() const { return m_source_distant_runtime_status; }
		static bool& source_directional_ff_enabled() { return m_source_directional_ff_enabled; }
		static int& source_directional_ff_index() { return m_source_directional_ff_index; }
		static float& source_directional_ff_scalar() { return m_source_directional_ff_scalar; }

		void destroy_map_light(light* light);
		void destroy_lights_with_comment_prefix(std::string_view prefix);
		void destroy_all_map_lights();
		void destroy_and_clear_all_active_lights();
		void update_all_active_lights();
		void draw_all_active_lights();

		size_t get_active_light_count() { return m_active_lights.size(); }
		light* get_first_active_light() { return !m_active_lights.empty() ? &m_active_lights.front() : nullptr; }
		std::uint32_t get_ies_cluster_child_count() const;

		struct runtime_debug_stats_s
		{
			std::uint32_t active_lights = 0u;
			std::uint32_t spawned_handles = 0u;
			std::uint32_t animated_lights = 0u;
			std::uint32_t attached_lights = 0u;
			std::uint32_t pending_trigger_lights = 0u;
			std::uint32_t marked_for_destroy = 0u;
			std::uint32_t runtime_group_filtered = 0u;
			std::uint32_t legacy_rig_lights = 0u;
			std::uint32_t native_ies_lights = 0u;
			std::uint32_t fake_ies_rigs = 0u;
			std::uint32_t native_ies_failures = 0u;
			std::uint32_t ies_child_handles = 0u;
			std::uint32_t ies_budget_skipped = 0u;
			std::uint32_t create_light_failures = 0u;
		};

		runtime_debug_stats_s build_runtime_debug_stats() const;
		static bool& debug_lifecycle_log_enabled() { return m_debug_lifecycle_log_enabled; }
		static const std::deque<std::string>& debug_lifecycle_log() { return m_debug_lifecycle_log; }
		static void clear_debug_lifecycle_log() { m_debug_lifecycle_log.clear(); }

		// Runtime authoring budget for pseudo-IES helper lights. This prevents one map from
		// accidentally creating hundreds of child lights while iterating in the editor.
		static int& ies_cluster_global_budget() { return m_ies_cluster_global_budget; }
		static int& ies_cluster_quality_mode() { return m_ies_cluster_quality_mode; }
		static bool& runtime_group_filter_enabled() { return m_runtime_group_filter_enabled; }
		static std::string& runtime_group_filter() { return m_runtime_group_filter; }

		// Public UI/config accessors for the experimental FF SetLight mirror backend.
		// The actual state stays private so remix_lights remains the only owner of the backend.
		static bool& ff_setlight_mirror_enabled() { return m_ff_setlight_mirror_enabled; }
		static bool& ff_setlight_mirror_only_visible() { return m_ff_setlight_mirror_only_visible; }
		static int& ff_setlight_start_index() { return m_ff_setlight_start_index; }
		static int& ff_setlight_max_lights() { return m_ff_setlight_max_lights; }
		static float& ff_setlight_scalar() { return m_ff_setlight_scalar; }
		static float& ff_setlight_radius_scale() { return m_ff_setlight_radius_scale; }
		static std::uint32_t ff_setlight_emitted() { return m_ff_setlight_emitted; }
		static std::uint32_t ff_setlight_failed() { return m_ff_setlight_failed; }

		void debug_print_player_pos_time();

	private:
		void destroy_ies_emulation_lights(light* light);
		void update_ies_emulation_lights(light* light, const map_settings::remix_light_settings_s::point_s* control_point);
		void emit_ff_setlight_mirror_for_active_lights();
		void emit_source_directional_ff_fallback();
		void push_debug_lifecycle_event(std::string_view event, const light* light = nullptr);

		// Dedicated singleton handle for the Source light_environment / emit_skylight path.
		remixapi_LightHandle m_source_distant_handle = nullptr;
		light::native_distant_light_info_ext_s m_source_distant_ext = {};
		remixapi_LightInfo m_source_distant_info = {};
		remixapi_ErrorCode m_source_distant_last_error = REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		std::uint64_t m_source_distant_hash = 0u;
		std::uint64_t m_source_distant_draw_calls = 0u;
		Vector m_source_directional_ff_direction = Vector(0.0f, 0.0f, -1.0f);
		Vector m_source_directional_ff_radiance = Vector(0.0f, 0.0f, 0.0f);
		bool m_source_directional_ff_active = false;
		std::uint64_t m_source_directional_ff_draw_calls = 0u;
		std::string m_source_distant_runtime_status = "Source environment light not submitted";

		static inline bool m_source_directional_ff_enabled = true;
		static inline int m_source_directional_ff_index = 7;
		static inline float m_source_directional_ff_scalar = 1.0f;

		// Optional compatibility backend: mirror active Remix API lights into the D3D9 fixed-function
		// SetLight/LightEnable state. This is experimental, but useful for testing whether Remix
		// captures lights more consistently through the original FF path than through direct API lights.
		static inline bool m_ff_setlight_mirror_enabled = false;
		static inline bool m_ff_setlight_mirror_only_visible = true;
		static inline int m_ff_setlight_start_index = 0;
		static inline int m_ff_setlight_max_lights = 8;
		static inline float m_ff_setlight_scalar = 0.08f;
		static inline float m_ff_setlight_radius_scale = 1.0f;
		static inline std::uint32_t m_ff_setlight_emitted = 0u;
		static inline std::uint32_t m_ff_setlight_failed = 0u;

		static inline int m_ies_cluster_global_budget = 96;
		static inline int m_ies_cluster_quality_mode = 3; // 0 off, 1 low, 2 balanced, 3 authored, 4 stress
		static inline bool m_runtime_group_filter_enabled = false;
		static inline std::string m_runtime_group_filter;

		static inline bool m_debug_lifecycle_log_enabled = false;
		static inline std::deque<std::string> m_debug_lifecycle_log = {};
		static inline std::uint32_t m_debug_create_light_failures = 0u;
		static inline std::uint32_t m_debug_native_ies_failures = 0u;
		static inline std::uint32_t m_debug_ies_budget_skipped = 0u;

		static inline std::uint32_t m_active_light_spawn_tracker = 0u;
		static inline std::vector<light> m_active_lights = {};
		static inline std::uint32_t m_attachframe_counter = 0u;
		bool m_is_paused = false;

		// -
		float m_dbgpos_print_timer = 0.0f;
		float m_dbgpos_timer_since_movement = 0.0f;
		float m_dbgpos_timepoint_on_movement = 0.0f;
		Vector m_dbgpos_last_pos = {};
		bool m_dbgpos_on_steady_once = false;
		float m_dbgpos_last_curtime = 0.0f;

	};
}
