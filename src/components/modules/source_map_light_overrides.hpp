#pragma once

namespace components::source_map_light_overrides
{
	struct light_point_s
	{
		Vector position = Vector(0.0f, 0.0f, 0.0f);
		Vector radiance = Vector(1.0f, 1.0f, 1.0f);
		float radiance_scalar = 1.0f;
		float radius = 1.0f;
		float timepoint = 0.0f;
		float smoothness = 0.5f;
		bool use_shaping = false;
		Vector direction = Vector(0.0f, 0.0f, 1.0f);
		Vector angle_offset_attached = Vector(0.0f, 0.0f, 0.0f);
		float degrees = 180.0f;
		float softness = 0.0f;
		float exponent = 0.0f;
		float volumetric_scale = 1.0f;
		int authoring_shape = 0;
		float authoring_width = 2.0f;
		float authoring_height = 2.0f;
		float authoring_length = 4.0f;
		float authoring_range = 128.0f;
		int light_rig_mode = 0;
		std::string ies_profile;
		std::string ies_file;
		float ies_axis_rotation = 0.0f;
		float ies_angle_scale = 1.0f;
		float ies_intensity_scale = 1.0f;
		bool ies_normalize = true;
		float ies_strength = 1.0f;
		float ies_focus = 1.0f;
		bool ies_emulation = false;
		int ies_emulation_samples = 0;
		float ies_emulation_spread = 0.35f;
		float ies_emulation_radius_scale = 0.45f;
		float ies_emulation_intensity_scale = 0.35f;
		float ies_emulation_forward_offset = 0.08f;
		std::string ies_emulation_pattern = "spiral";
		float ies_emulation_aspect = 1.0f;
		float ies_emulation_twist = 0.0f;
	};

	struct light_override_s
	{
		std::int32_t source_index = -1;
		std::int32_t hammer_id = -1;
		std::int32_t runtime_entity_index = -1;
		std::string runtime_kind;
		std::string targetname;
		std::string classname;
		bool snapshot_only = false;

		// V20.9 full-rig persistent database metadata. Entries without persistent_id
		// remain compatible V20.8 selector overrides. A deleted record is a tombstone
		// which prevents a base-config or Source light from being recreated.
		std::string persistent_id;
		bool full_rig = false;
		bool deleted = false;
		std::string group;
		std::string comment;
		bool has_run_once = false;
		bool run_once = false;
		bool has_loop = false;
		bool loop = false;
		bool has_loop_smoothing = false;
		bool loop_smoothing = false;
		bool has_trigger_always = false;
		bool trigger_always = false;
		std::string trigger_choreo_name;
		std::string trigger_choreo_actor;
		std::string trigger_choreo_event;
		std::string trigger_choreo_param1;
		std::uint32_t trigger_sound_hash = 0u;
		float trigger_delay = 0.0f;
		std::string kill_choreo_name;
		std::uint32_t kill_sound_hash = 0u;
		float kill_delay = 0.0f;
		float attach_prop_radius = 0.0f;
		std::string attach_prop_name;
		Vector attach_prop_mins = Vector(0.0f, 0.0f, 0.0f);
		Vector attach_prop_maxs = Vector(0.0f, 0.0f, 0.0f);
		int attach_bone_index = -1;
		std::string attach_bone_name;
		std::vector<light_point_s> points;

		// Optional Source provenance. It is informational and never participates in
		// selector matching, so older configs and hand-authored selectors remain valid.
		std::string source_kind;
		std::int32_t source_style = 0;
		std::int32_t source_owner = -1;
		std::int32_t source_key = 0;
		bool source_transient = false;
		bool source_live_link = false;

		bool has_enabled = false;
		bool enabled = true;
		bool has_position = false;
		Vector position = Vector(0.0f, 0.0f, 0.0f);
		bool has_position_offset = false;
		Vector position_offset = Vector(0.0f, 0.0f, 0.0f);
		bool has_radiance = false;
		Vector radiance = Vector(1.0f, 1.0f, 1.0f);
		bool has_intensity = false;
		float intensity = 1.0f;
		bool has_radius = false;
		float radius = 1.0f;
		bool has_direction = false;
		Vector direction = Vector(0.0f, 0.0f, -1.0f);
		bool has_intensity_scale = false;
		float intensity_scale = 1.0f;
		bool has_radius_scale = false;
		float radius_scale = 1.0f;
		bool has_surface_offset = false;
		float surface_offset = 0.0f;
		bool has_shaping = false;
		bool shaping = false;
		bool has_degrees = false;
		float degrees = 180.0f;
		bool has_softness = false;
		float softness = 0.0f;
		bool has_exponent = false;
		float exponent = 0.0f;

		// Optional authored animation for imported map lights. The editor expands this
		// metadata into ordinary timepoint points at runtime, so map overrides remain
		// compact and editable instead of serializing a large generated point list.
		bool has_animation = false;
		std::string animation = "stable";
		bool has_animation_duration = false;
		float animation_duration = 1.0f;
		bool has_animation_speed = false;
		float animation_speed = 1.0f;
		bool has_animation_variation = false;
		float animation_variation = 0.0f;
		bool has_animation_axis = false;
		Vector animation_axis = Vector(0.0f, 0.0f, 1.0f);
		bool has_animation_degrees = false;
		float animation_degrees = 360.0f;
		bool has_animation_phase = false;
		float animation_phase = 0.0f;

		bool has_property_animation = false;
		std::string property_animation = "stable";
		bool has_property_animation_duration = false;
		float property_animation_duration = 2.0f;
		bool has_property_animation_speed = false;
		float property_animation_speed = 1.0f;
		bool has_property_animation_variation = false;
		float property_animation_variation = 0.0f;
		bool has_property_animation_intensity = false;
		float property_animation_intensity = -1.0f;
		bool has_movement_animation = false;
		std::string movement_animation = "none";
		bool has_movement_animation_duration = false;
		float movement_animation_duration = 2.0f;
		bool has_movement_animation_speed = false;
		float movement_animation_speed = 1.0f;
		bool has_movement_animation_axis = false;
		Vector movement_animation_axis = Vector(0.0f, 0.0f, 1.0f);
		bool has_movement_animation_degrees = false;
		float movement_animation_degrees = 110.0f;
		bool has_movement_animation_phase = false;
		float movement_animation_phase = 0.0f;
		bool has_movement_animation_distance = false;
		float movement_animation_distance = 64.0f;

		bool has_owner_targetname = false;
		std::string owner_targetname;
		bool has_unbind_owner = false;
		bool unbind_owner = false;

		bool has_surface_cluster = false;
		bool surface_cluster = false;
		bool has_surface_samples = false;
		int surface_samples = 4;
		bool has_surface_spread = false;
		float surface_spread = 0.65f;
		bool has_surface_aspect = false;
		float surface_aspect = 1.0f;
		bool has_surface_intensity = false;
		float surface_intensity = 0.30f;
		bool has_surface_radius_scale = false;
		float surface_radius_scale = 0.45f;
		bool has_surface_pattern = false;
		std::string surface_pattern = "cross";
	};

	struct load_result_s
	{
		std::vector<light_override_s> overrides;
		std::string path;
		std::string status;
		std::uint32_t invalid_lines = 0u;
		std::uint32_t persistent_rigs = 0u;
		std::uint32_t tombstones = 0u;
		bool file_found = false;
	};

	bool load(const std::string& game_root, const std::string& map_name, load_result_s& out);
	const light_override_s* find_best(const load_result_s& data, std::int32_t source_index,
		std::int32_t hammer_id, std::string_view targetname, std::string_view classname);
	const light_override_s* find_runtime_entity(const load_result_s& data, std::int32_t runtime_entity_index,
		std::string_view runtime_kind, std::string_view classname);
	const light_override_s* find_persistent(const load_result_s& data, std::string_view persistent_id);
	bool upsert(const std::string& game_root, const std::string& map_name,
		const light_override_s& entry, std::string& out_path, std::string& error);
	bool replace_all(const std::string& game_root, const std::string& map_name,
		const std::vector<light_override_s>& entries, std::string& out_path, std::string& error);
	bool erase(const std::string& game_root, const std::string& map_name,
		std::int32_t source_index, std::int32_t hammer_id, std::string_view targetname,
		std::string_view classname, std::string& out_path, std::string& error);
}
