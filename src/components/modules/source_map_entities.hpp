#pragma once
#include <string_view>

namespace components::source_map_entities
{
	enum class io_action_kind : std::uint8_t
	{
		unknown = 0,
		enable,
		disable,
		toggle,
		kill,
	};

	struct io_action_s
	{
		std::uint32_t source_entity_index = 0u;
		std::uint32_t root_source_entity_index = 0u;
		std::uint32_t root_action_id = 0u;
		std::string output_name;
		std::string root_output_name;
		std::string target;
		std::string input;
		std::string parameter;
		std::string path;
		float delay = 0.0f;
		std::int32_t max_fires = -1;
		io_action_kind kind = io_action_kind::unknown;
		std::uint8_t path_depth = 0u;
		bool indirect = false;
		std::vector<std::uint32_t> resolved_target_source_indices;
	};

	struct entity_s
	{
		std::uint32_t source_index = 0u;
		std::int32_t hammer_id = -1;
		std::int32_t style = 0;
		std::int32_t spawnflags = 0;
		std::int32_t initial_health = -1;
		std::string classname;
		std::string targetname;
		std::string parentname;
		std::string parent_attachment;
		std::string target;
		std::string model;
		Vector origin = Vector(0.0f, 0.0f, 0.0f);
		Vector angles = Vector(0.0f, 0.0f, 0.0f);
		bool has_origin = false;
		bool has_angles = false;
		bool starts_disabled = false;
		bool is_light = false;
		bool is_bindable_owner = false;
		bool is_breakable_owner = false;
		std::vector<std::pair<std::string, std::string>> keyvalues;
		std::vector<io_action_s> outputs;
	};

	struct graph_s
	{
		std::vector<entity_s> entities;
		std::unordered_multimap<std::string, std::size_t> targetname_to_index;
		std::vector<io_action_s> io_actions;
		std::vector<io_action_s> resolved_light_actions;
		std::unordered_map<std::uint32_t, std::uint32_t> light_source_to_control_group;
		std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> control_group_members;
		std::unordered_map<std::uint32_t, std::vector<std::size_t>> light_source_to_action_indices;
		std::unordered_map<std::uint32_t, std::vector<std::size_t>> control_group_to_action_indices;
		std::uint32_t light_entities = 0u;
		std::uint32_t bindable_owners = 0u;
		std::uint32_t named_entities = 0u;
		std::uint32_t attachment_links = 0u;
		std::uint32_t io_links = 0u;
		std::uint32_t io_resolved_links = 0u;
		std::uint32_t io_light_links = 0u;
		std::uint32_t io_kill_links = 0u;
		std::uint32_t io_transitive_light_links = 0u;
		std::uint32_t io_relay_hops = 0u;
		std::uint32_t io_cycles_skipped = 0u;
		std::uint32_t io_depth_limited = 0u;
		std::uint32_t io_multi_manager_links = 0u;
		std::uint32_t io_relay_entities = 0u;
		std::uint32_t controlled_light_entities = 0u;
		std::uint32_t light_control_groups = 0u;
	};

	bool parse_entity_lump(std::string_view text, graph_s& out, std::string& error);
	const entity_s* find_by_targetname(const graph_s& graph, std::string_view targetname);
	const entity_s* find_by_hammer_id(const graph_s& graph, std::int32_t hammer_id);
	io_action_kind classify_io_input(std::string_view input);
	std::vector<std::string> routed_outputs_for_input(std::string_view classname, std::string_view input);
	const entity_s* find_best_light_entity(const graph_s& graph, const Vector& origin,
		std::int32_t style, std::int32_t worldlight_type, const Vector& direction,
		float max_distance, float* out_score = nullptr);
	const entity_s* find_best_owner_entity(const graph_s& graph, const Vector& origin,
		std::string_view preferred_parent, float max_distance, float* out_score = nullptr);
	std::uint32_t find_light_control_group(const graph_s& graph, std::uint32_t light_source_index);
	std::string describe_light_controls(const graph_s& graph, std::uint32_t light_source_index);
	const std::vector<std::size_t>* find_control_group_actions(const graph_s& graph, std::uint32_t group_id);
	bool light_has_control_kind(const graph_s& graph, std::uint32_t light_source_index, io_action_kind kind);
	bool control_group_all_have_kind(const graph_s& graph, std::uint32_t group_id, io_action_kind kind);
	const char* io_action_kind_name(io_action_kind kind);
	bool class_is_light(std::string_view classname);
	bool class_is_environment_light(std::string_view classname);
	bool class_is_runtime_light(std::string_view classname);
	bool class_is_bindable_owner(std::string_view classname);
	bool class_is_breakable_owner(std::string_view classname);
	bool runtime_class_matches_map_class(std::string_view runtime_classname, std::string_view map_classname);
}
