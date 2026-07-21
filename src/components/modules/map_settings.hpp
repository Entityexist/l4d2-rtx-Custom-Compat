#pragma once
#include "remix_vars.hpp"

namespace components
{
	class map_settings : public component
	{
	public:
		map_settings();
		~map_settings() = default;

		static inline map_settings* p_this = nullptr;
		static map_settings* get() { return p_this; }

		enum TRANSITION_MODE : uint8_t
		{
			ONCE_ON_ENTER = 0,
			ONCE_ON_LEAVE = 1,
			ALWAYS_ON_ENTER = 2,
			ALWAYS_ON_LEAVE = 3,
		};

		enum TRANSITION_TRIGGER_TYPE : uint8_t
		{
			CHOREO = 0,
			SOUND = 1,
			LEAF = 2,
		};

		struct remix_transition_s
		{
			TRANSITION_TRIGGER_TYPE trigger_type;

			// choreo trigger
			std::string choreo_name;
			std::string choreo_actor;
			std::string choreo_event;
			std::string choreo_param1;

			// sound trigger
			std::uint32_t sound_hash;
			std::string sound_name;

			// leaf trigger
			std::unordered_set<std::uint32_t> leafs;

			std::string config_name;
			TRANSITION_MODE mode;
			remix_vars::EASE_TYPE interpolate_type;
			float delay_in = 0.0f;
			float delay_out = 0.0f;
			float duration = 0.0f;
			std::uint64_t hash;
			bool _state_enter = false;
		};

		struct marker_trigger_s
		{
			std::string choreo_name;
			std::string choreo_actor;
			std::string choreo_event;
			std::string choreo_param1;
			std::uint32_t sound_hash;
			std::string sound_name;
			float delay = 0.0f;

			bool was_used = false; // internal use :: was this already for any choreo or sound trigger already?
			bool delay_start = false; // internal use :: set to true to start counting
			float delay_elapsed_time = 0.0f; // internal use :: time since counting started

			bool has_trigger() const
			{
				if (sound_hash || !sound_name.empty() || !choreo_name.empty()) {
					return true;
				}
				return false;
			}
		};

		struct marker_settings_s
		{
			std::uint32_t index = 0;
			Vector origin = {};
			bool no_cull = false;
			Vector rotation = { 0.0f, 0.0f, 0.0f };
			Vector scale = { 1.0f, 1.0f, 1.0f }; // no_cull only
			std::unordered_set<std::uint32_t> areas; // no_cull only
			std::unordered_set<std::uint32_t> when_not_in_leafs; // no_cull only

			marker_trigger_s trigger_show = {};
			marker_trigger_s trigger_hide = {};
			bool trigger_always = false;

			std::string comment;
			std::string name;
			Vector color = { 1.0f, 1.0f, 1.0f };
			bool visible = true;
			float visibility_range = 0.0f; // Source units; 0 = unlimited

			void* handle = nullptr; // internal use
			bool is_hidden = false; // internal use
			// Steam/runtime compatibility fallback. Normal dynamic_prop markers that
			// cannot be created safely are rendered through the existing no-cull path.
			bool runtime_nocull_fallback = false; // internal use; never serialized
			std::uint8_t runtime_spawn_attempts = 0u; // internal use
		};

		struct api_config_var
		{
			std::string variable;
			std::string value;
		};

		enum DYNAMIC_LIGHT_TRIGGER_TYPE : uint8_t
		{
			DYN_LIGHT_TRIGGER_SOUND = 0,
			DYN_LIGHT_TRIGGER_CHOREO = 1,
			DYN_LIGHT_TRIGGER_LEAF = 2,
		};

		struct light_anchor_s
		{
			std::string name;
			std::string preset = "soft_flash";
			std::string animation = "stable";
			Vector position = { 0.0f, 0.0f, 0.0f };
			Vector offset = { 0.0f, 0.0f, 0.0f };
			Vector radiance = { 1.0f, 0.85f, 0.55f };
			float scalar = 1.0f;
			float radius = 1.0f;
			float duration = 0.35f;
			float speed = 1.0f;
			float variation = 0.0f;
			float volumetric_scale = 1.0f;
			bool loop = false;
			bool loop_smoothing = false;
			bool use_shaping = false;
			Vector direction = { 0.0f, 0.0f, 1.0f };
			float degrees = 180.0f;
			float softness = 0.0f;
			float exponent = 0.0f;

			// Animation preset controls. Used by rotating/sweeping/pulsing dynamic light presets.
			// animation_axis is Source world-space axis for rotation/sweep, e.g. [0,0,1] = yaw around vertical.
			Vector animation_axis = { 0.0f, 0.0f, 1.0f };
			float animation_degrees = 360.0f;
			float animation_phase = 0.0f;

			std::string comment;
		};

		struct dynamic_light_event_s
		{
			std::string name;
			std::string preset = "soft_flash";
			std::string animation = "stable";
			DYNAMIC_LIGHT_TRIGGER_TYPE trigger_type = DYN_LIGHT_TRIGGER_SOUND;

			// sound trigger
			std::uint32_t sound_hash = 0u;
			std::string sound_name;

			// choreo trigger
			std::string choreo_name;
			std::string choreo_actor;
			std::string choreo_event;
			std::string choreo_param1;

			// leaf trigger
			std::unordered_set<std::uint32_t> leafs;

			// playback
			float duration = 0.12f;
			float delay = 0.0f;
			float cooldown = 0.0f;
			float speed = 1.0f;
			float variation = 0.0f;
			bool once = false;
			bool loop = false;
			bool loop_smoothing = false;
			bool enabled = true;

			// position/source
			std::string activate_anchor; // optional: trigger this fixed authoring anchor instead of using the sound origin
			Vector position = {};
			Vector offset = {};
			bool use_source_origin = true;
			bool use_camera_when_no_source = true;

			// overrides
			Vector radiance = { 1.0f, 0.85f, 0.55f };
			float scalar = 1.0f;
			float radius = 48.0f;
			float volumetric_scale = 1.0f;
			bool use_shaping = false;
			Vector direction = { 0.0f, 0.0f, 1.0f };
			float degrees = 180.0f;
			float softness = 0.0f;
			float exponent = 0.0f;

			// Animation preset controls. Used by rotating/sweeping/pulsing dynamic light presets.
			Vector animation_axis = { 0.0f, 0.0f, 1.0f };
			float animation_degrees = 360.0f;
			float animation_phase = 0.0f;

			// runtime bookkeeping
			float last_trigger_time = -99999.0f;
			bool was_used = false;

			std::string comment;
		};

		struct remix_light_settings_s
		{
			enum light_rig_mode_e : int
			{
				LIGHT_RIG_MODE_LEGACY = 0,
				LIGHT_RIG_MODE_NATIVE_IES = 1,
				LIGHT_RIG_MODE_FAKE_IES = 2,
			};

			enum light_authoring_shape_e : int
			{
				LIGHT_AUTHORING_SHAPE_AUTO = 0,
				LIGHT_AUTHORING_SHAPE_POINT = 1,
				LIGHT_AUTHORING_SHAPE_SPOT = 2,
				LIGHT_AUTHORING_SHAPE_DISK = 3,
				LIGHT_AUTHORING_SHAPE_RECT = 4,
				LIGHT_AUTHORING_SHAPE_TUBE = 5,
				LIGHT_AUTHORING_SHAPE_DISTANT = 6,
			};

			struct point_s
			{
				Vector position;
				Vector radiance;
				float radiance_scalar = 1.0f;
				float radius = 1.0f;
				float timepoint = 0.0f;
				float smoothness = 0.5f;

				// shaping
				bool use_shaping = false;
				Vector direction = { 0.0f, 0.0f, 1.0f };
				Vector angle_offset_attached = { 0.0f, 0.0f, 0.0f }; // offset light direction when attached to an entity or bone (Euler)
				float degrees = 180.0; // cone angle
				float softness = 0.0f; // cone
				float exponent = 0.0f; // focus

				// volumetric
				float volumetric_scale = 1.0f;

				// Light Studio analytical geometry. Point/Spot use the sphere/cone extension, while Disc,
				// Rect, Tube and Distant are emitted through their native Remix API extension layouts.
				// These values also drive editor visualizers, presets and TOML round-tripping.
				int authoring_shape = LIGHT_AUTHORING_SHAPE_AUTO;
				float authoring_width = 2.0f;
				float authoring_height = 2.0f;
				float authoring_length = 4.0f;
				float authoring_range = 128.0f;

				// Explicit light backend. All animation keyframes belonging to one light should use the same mode.
				// legacy: no photometric profile; geometry may still be Point/Spot/Disc/Rect/Tube/Distant.
				// native_ies: appends remixapi_LightInfoIESEXT to a compatible Sphere, Disc or Rect geometry chain.
				// fake_ies: keeps the compatibility helper-light cluster for builds without native IES support.
				int light_rig_mode = LIGHT_RIG_MODE_LEGACY;

				// Native IES profile controls. ies_file may be absolute or relative to <game>/rtx-remix/ies/.
				std::string ies_profile;
				std::string ies_file;
				float ies_axis_rotation = 0.0f;
				float ies_angle_scale = 1.0f;
				float ies_intensity_scale = 1.0f;
				bool ies_normalize = true;

				// Compatibility profile metadata used by the fake IES rig.
				float ies_strength = 1.0f;
				float ies_focus = 1.0f;

				// Optional pseudo-IES cluster emulation. The authored point remains the controller, while runtime
				// spawns a small helper-light cluster around it to fake asymmetric photometric lobes.
				// samples = number of extra helper lights in addition to the original control light.
				bool ies_emulation = false;
				int ies_emulation_samples = 0;
				float ies_emulation_spread = 0.35f;
				float ies_emulation_radius_scale = 0.45f;
				float ies_emulation_intensity_scale = 0.35f;
				float ies_emulation_forward_offset = 0.08f;
				std::string ies_emulation_pattern = "spiral"; // spiral, ring, line, cross, beam
				float ies_emulation_aspect = 1.0f; // stretches helper layout along the local up axis
				float ies_emulation_twist = 0.0f; // rotates helper layout around light direction, degrees
			};

			std::vector<point_s> points;
			bool run_once = false;
			bool loop = false;
			bool loop_smoothing = false;

			// Procedural Add Light animation metadata. At map load this is expanded into ordinary
			// timepoint points, so the existing remix_lights interpolator remains the runtime path.
			// Keeping the metadata makes the editor/export understandable instead of only showing
			// a raw pile of generated points.
			std::string animation = "stable";
			float animation_duration = 1.0f;
			float animation_speed = 1.0f;
			float animation_variation = 0.0f;
			Vector animation_axis = { 0.0f, 0.0f, 1.0f };
			float animation_degrees = 360.0f;
			float animation_phase = 0.0f;

			// V21.14.5 separates fixture movement from light-output animation. The generated
			// points remain the runtime source of truth; these fields preserve editable intent.
			std::string property_animation = "stable";
			float property_animation_duration = 2.0f;
			float property_animation_speed = 1.0f;
			float property_animation_variation = 0.0f;
			float property_animation_intensity = -1.0f;
			std::string movement_animation = "none";
			float movement_animation_duration = 2.0f;
			float movement_animation_speed = 1.0f;
			Vector movement_animation_axis = { 0.0f, 0.0f, 1.0f };
			float movement_animation_degrees = 110.0f;
			float movement_animation_phase = 0.0f;
			float movement_animation_distance = 64.0f;

			bool trigger_always = false;

			std::string trigger_choreo_name;
			std::string trigger_choreo_actor;
			std::string trigger_choreo_event;
			std::string trigger_choreo_param1;
			std::uint32_t trigger_sound_hash;
			float trigger_delay = 0.0f;

			std::string kill_choreo_name;
			std::uint32_t kill_sound_hash;
			float kill_delay = 0.0f;

			float attach_prop_radius = 0.0f;
			std::string attach_prop_name;
			Vector attach_prop_mins; // min bounds
			Vector attach_prop_maxs; // max bounds
			int attach_bone_index = -1;
			std::string attach_bone_name;

			std::string comment;

			// Authoring / runtime organization. Disabled lights stay in TOML/editor,
			// but the runtime spawn paths ignore them outside light edit mode.
			bool enabled = true;
			std::string group;

			// Runtime-only identity for lights materialized from Source map imports.
			// The generic Light Editor keeps these fields while editing, allowing the
			// result to be written back to map_lights/<map>.toml without parsing comments.
			bool generated_map_light = false;
			std::int32_t generated_source_index = -1;
			std::int32_t generated_hammer_id = -1;
			std::int32_t generated_runtime_entity_index = -1;
			std::string generated_runtime_kind;
			std::string generated_targetname;
			std::string generated_classname;

			// Source Direct Light provenance. These fields are editor/runtime metadata and
			// make imported lights inspectable without decoding their comment strings.
			std::string generated_source_kind;
			std::int32_t generated_source_style = 0;
			std::int32_t generated_source_owner = -1;
			std::int32_t generated_source_key = 0;
			std::uint64_t generated_source_capture_id = 0u;
			bool generated_source_transient = false;
			bool generated_source_live_link = false;

			// V20.9 persistent per-map light database identity. This is runtime/editor
			// metadata and is intentionally not parsed from the legacy map_settings TOML.
			// Base-config, imported and user-authored rigs receive a stable id when the
			// per-map database is merged after parse_toml().
			std::string persistent_map_light_id;
			bool persistent_map_light_managed = false;
			bool persistent_map_light_from_file = false;
		};

		// ---

		static constexpr float DEFAULT_NOCULL_DIST = 600.0f;

		static constexpr const char* AREA_CULL_MODE_STR[] =
		{
			"NoFrustum",
			"NoFrstmInAr",
			"Stock",
			"ForceAr",
			"AreaDist",
			"Distance"
		};

		enum AREA_CULL_MODE : uint8_t
		{
			AREA_CULL_MODE_NO_FRUSTUM = 0,					// no frustum culling (everywhere)
			AREA_CULL_MODE_NO_FRUSTUM_IN_CURRENT_AREA = 1,	// no frustum culling in current area
			AREA_CULL_MODE_STOCK = 2,						// OG: frustum culling
			AREA_CULL_MODE_FORCE_AREA = 3,					// frustum culling (outside current area) + force all leafs/nodes in current area
			AREA_CULL_MODE_FORCE_AREA_DISTANCE = 4,			// frustum culling (outside current area) + force all leafs/nodes in current area and outside of current area within certain dist to player
			AREA_CULL_MODE_DISTANCE = 5,					// force all leafs/nodes within certain dist to player
			// -------------------
			AREA_CULL_INFO_COUNT = 6,
			AREA_CULL_INFO_DEFAULT = AREA_CULL_MODE_DISTANCE,
			AREA_CULL_INFO_NOCULLDIST_START = AREA_CULL_MODE_FORCE_AREA_DISTANCE,
			AREA_CULL_INFO_NOCULLDIST_END = AREA_CULL_MODE_DISTANCE,
		};

		struct leaf_tweak_s
		{
			std::unordered_set<std::uint32_t> in_leafs;
			std::unordered_set<std::uint32_t> areas;
			std::unordered_set<std::uint32_t> leafs;
			float nocull_dist = 0.0f;
		};

		struct hide_area_s
		{
			std::unordered_set<std::uint32_t> areas;
			std::unordered_set<std::uint32_t> when_not_in_leafs;
		};

		struct area_overrides_s
		{
			std::unordered_set<std::uint32_t> leafs;
			std::unordered_set<std::uint32_t> areas;
			std::unordered_set<std::uint32_t> hide_leafs;
			std::vector<hide_area_s> hide_areas;
			std::vector<leaf_tweak_s> leaf_tweaks;
			AREA_CULL_MODE cull_mode = AREA_CULL_INFO_DEFAULT;
			float nocull_distance = DEFAULT_NOCULL_DIST;
			bool nocull_distance_overrides_in_leaf_twk = false;
			std::uint32_t area_index = 0u;
		};

		struct hide_models_s
		{
			std::unordered_set<std::string> substrings;
			std::unordered_set<float> radii;
		};

		struct unbake_models_s
		{
			std::unordered_set<int> checksums;
		};

		struct map_settings_s
		{
			std::string	mapname;
			float fog_dist = 0.0f;
			float fog_density = 0.0f;
			DWORD fog_color = 0xFFFFFFFF;
			float water_uv_scale = 1.0f;
			float water_offset_top = 0.5f; // top layer
			float water_offset_bottom = 0.0f; // bottom layer
			std::unordered_map<std::uint32_t, area_overrides_s> area_settings;
			float default_nocull_dist = DEFAULT_NOCULL_DIST;
			hide_models_s hide_models;
			unbake_models_s unbake_models;
			std::vector<remix_transition_s> remix_transitions;
			std::vector<marker_settings_s> map_markers;
			std::vector<std::string> api_var_configs;
			std::vector<remix_light_settings_s> remix_lights;
			std::vector<light_anchor_s> light_anchors;
			std::vector<dynamic_light_event_s> dynamic_light_events;
			bool using_any_light_sound_hash = false;
			bool using_any_light_attached_to_prop = false;
			bool using_any_transition_sound_hash = false;
			bool using_any_transition_sound_name = false;

			bool using_any_marker_choreo = false;
			bool using_any_marker_sound_hash = false;
			bool using_any_marker_sound_name = false;

			bool using_any_dynamic_light_sound_hash = false;
			bool using_any_dynamic_light_sound_name = false;
			bool using_any_dynamic_light_choreo = false;
			bool using_any_dynamic_light_leaf = false;
		};

		static map_settings_s& get_map_settings() { return m_map_settings; }
		static const std::string& get_map_name() { return m_map_settings.mapname; }

		void set_settings_for_map(const std::string& map_name);
		static void spawn_markers_once();
		static void destroy_markers();
		static const std::string& marker_spawn_status() { return m_marker_spawn_status; }
		static std::uint32_t marker_spawn_attempts() { return m_marker_spawn_attempts; }
		static std::uint32_t marker_spawn_failures() { return m_marker_spawn_failures; }
		static std::uint32_t marker_fallback_count();
		static void on_map_load(const std::string& map_name);
		static void on_map_unload();
		static void clear_map_settings();
		static void reload();
		static void on_frame_autosave();
		static bool save_current_map_authoring_data(bool force = false);
		static void mark_map_data_dirty(const char* reason = nullptr);
		static bool map_data_dirty() { return m_map_data_dirty; }
		static const std::string& map_save_status() { return m_map_save_status; }
		static std::uint64_t map_save_generation() { return m_map_save_generation; }

		struct level_bool_s
		{
			bool c1m1_hotel = false, c1m2_streets = false, c1m3_mall = false, c1m4_atrium = false,
				 c2m1_highway = false, c2m2_fairgrounds = false, c2m3_coaster = false, c2m4_barns = false, c2m5_concert = false,
				 c3m1_plankcountry = false, c3m2_swamp = false, c3m3_shantytown = false, c3m4_plantation = false,
				 c4m1_milltown_a = false, c4m2_sugarmill_a = false, c4m3_sugarmill_b = false, c4m4_milltown_b = false, c4m5_milltown_escape = false,
				 c5m1_waterfront = false, c5m1_waterfront_sndscape = false, c5m2_park = false, c5m3_cemetery = false, c5m4_quarter = false, c5m5_bridge = false,
				 credits = false, curling_stadium = false, tutorial_standards = false, tutorial_standards_vs = false,
				 c6m1_riverbank = false, c6m2_bedlam = false, c6m3_port = false,
				 c7m1_docks = false, c7m2_barge = false, c7m3_port = false,
				 c8m1_apartment = false, c8m2_subway = false, c8m3_sewers = false, c8m4_interior = false, c8m5_rooftop = false,
				 c9m1_alleys = false, c9m2_lots = false,
				 c10m1_caves = false, c10m2_drainage = false, c10m3_ranchhouse = false, c10m4_mainstreet = false, c10m5_houseboat = false,
				 c11m1_greenhouse = false, c11m2_offices = false, c11m3_garage = false, c11m4_terminal = false, c11m5_runway = false,
				 c12m1_hilltop = false, c12m2_traintunnel = false, c12m3_bridge = false, c12m4_barn = false, c12m5_cornfield = false,
				 c13m1_alpinecreek = false, c13m2_southpinestream = false, c13m3_memorialbridge = false, c13m4_cutthroatcreek = false;

			void update(const std::string& n)
			{
					 if (n == "c1m1_hotel") c1m1_hotel = true;
				else if (n == "c1m2_streets") c1m2_streets = true;
				else if (n == "c1m3_mall") c1m3_mall = true;
				else if (n == "c1m4_atrium") c1m4_atrium = true;
				else if (n == "c2m1_highway") c2m1_highway = true;
				else if (n == "c2m2_fairgrounds") c2m2_fairgrounds = true;
				else if (n == "c2m3_coaster") c2m3_coaster = true;
				else if (n == "c2m4_barns") c2m4_barns = true;
				else if (n == "c2m5_concert") c2m5_concert = true;
				else if (n == "c3m1_plankcountry") c3m1_plankcountry = true;
				else if (n == "c3m2_swamp") c3m2_swamp = true;
				else if (n == "c3m3_shantytown") c3m3_shantytown = true;
				else if (n == "c3m4_plantation") c3m4_plantation = true;
				else if (n == "c4m1_milltown_a") c4m1_milltown_a = true;
				else if (n == "c4m2_sugarmill_a") c4m2_sugarmill_a = true;
				else if (n == "c4m3_sugarmill_b") c4m3_sugarmill_b = true;
				else if (n == "c4m4_milltown_b") c4m4_milltown_b = true;
				else if (n == "c4m5_milltown_escape") c4m5_milltown_escape = true;
				else if (n == "c5m1_waterfront") c5m1_waterfront = true;
				else if (n == "c5m1_waterfront_sndscape") c5m1_waterfront_sndscape = true;
				else if (n == "c5m2_park") c5m2_park = true;
				else if (n == "c5m3_cemetery") c5m3_cemetery = true;
				else if (n == "c5m4_quarter") c5m4_quarter = true;
				else if (n == "c5m5_bridge") c5m5_bridge = true;
				else if (n == "credits") credits = true;
				else if (n == "curling_stadium") curling_stadium = true;
				else if (n == "tutorial_standards") tutorial_standards = true;
				else if (n == "tutorial_standards_vs") tutorial_standards_vs = true;
				else if (n == "c6m1_riverbank") c6m1_riverbank = true;
				else if (n == "c6m2_bedlam") c6m2_bedlam = true;
				else if (n == "c6m3_port") c6m3_port = true;
				else if (n == "c7m1_docks") c7m1_docks = true;
				else if (n == "c7m2_barge") c7m2_barge = true;
				else if (n == "c7m3_port") c7m3_port = true;
				else if (n == "c8m1_apartment") c8m1_apartment = true;
				else if (n == "c8m2_subway") c8m2_subway = true;
				else if (n == "c8m3_sewers") c8m3_sewers = true;
				else if (n == "c8m4_interior") c8m4_interior = true;
				else if (n == "c8m5_rooftop") c8m5_rooftop = true;
				else if (n == "c9m1_alleys") c9m1_alleys = true;
				else if (n == "c9m2_lots") c9m2_lots = true;
				else if (n == "c10m1_caves") c10m1_caves = true;
				else if (n == "c10m2_drainage") c10m2_drainage = true;
				else if (n == "c10m3_ranchhouse") c10m3_ranchhouse = true;
				else if (n == "c10m4_mainstreet") c10m4_mainstreet = true;
				else if (n == "c10m5_houseboat") c10m5_houseboat = true;
				else if (n == "c11m1_greenhouse") c11m1_greenhouse = true;
				else if (n == "c11m2_offices") c11m2_offices = true;
				else if (n == "c11m3_garage") c11m3_garage = true;
				else if (n == "c11m4_terminal") c11m4_terminal = true;
				else if (n == "c11m5_runway") c11m5_runway = true;
				else if (n == "c12m1_hilltop") c12m1_hilltop = true;
				else if (n == "c12m2_traintunnel") c12m2_traintunnel = true;
				else if (n == "c12m3_bridge") c12m3_bridge = true;
				else if (n == "c12m4_barn") c12m4_barn = true;
				else if (n == "c12m5_cornfield") c12m5_cornfield = true;
				else if (n == "c13m1_alpinecreek") c13m1_alpinecreek = true;
				else if (n == "c13m2_southpinestream") c13m2_southpinestream = true;
				else if (n == "c13m3_memorialbridge") c13m3_memorialbridge = true;
				else if (n == "c13m4_cutthroatcreek") c13m4_cutthroatcreek = true;
			}

			void reset()
			{
				memset(this, 0, sizeof(level_bool_s));
			}
		};

		static inline level_bool_s is_level = {};

	private:
		static inline map_settings_s m_map_settings = {};
		static inline std::vector<std::string> m_args;
		static inline bool m_spawned_markers = false;
		static inline std::uint32_t m_marker_spawn_attempts = 0u;
		static inline std::uint32_t m_marker_spawn_failures = 0u;
		static inline std::uint32_t m_marker_dependency_waits = 0u;
		static inline std::string m_marker_spawn_status = "not started";
		static inline std::chrono::steady_clock::time_point m_marker_next_spawn_attempt = {};
		static inline bool m_loaded = false;
		static inline bool m_map_data_dirty = false;
		static inline bool m_map_autosave_ready = false;
		static inline std::string m_map_save_status = "Map data not loaded";
		static inline std::string m_map_last_serialized;
		static inline std::string m_map_dirty_reason = "none";
		static inline std::uint64_t m_map_save_generation = 0u;
		static inline std::chrono::steady_clock::time_point m_map_last_change_time = {};
		static inline constexpr std::chrono::milliseconds m_map_autosave_debounce = std::chrono::milliseconds(850);

		static std::string serialize_current_map_authoring_data();
		static bool replace_map_section_entry(std::string& document, const std::string& section, const std::string& map_name, const std::string& assignment);
		bool parse_toml();
		bool matches_map_name();
		void open_and_set_var_config(const std::string& config, bool no_error = false, bool ignore_hashes = false, const char* custom_path = nullptr);
	};
}
