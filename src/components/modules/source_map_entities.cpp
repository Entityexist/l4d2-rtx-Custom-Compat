#include "std_include.hpp"
#include "source_map_entities.hpp"
#include "source_bsp_lights.hpp"
#include <cctype>
#include <charconv>
#include <string_view>
#include <system_error>

namespace components::source_map_entities
{
	namespace
	{
		std::string lower_copy(std::string_view value)
		{
			std::string out(value);
			return utils::str_to_lower(out);
		}

		std::string trim_copy(std::string value)
		{
			const auto first = value.find_first_not_of(" \t\r\n");
			if (first == std::string::npos) return {};
			const auto last = value.find_last_not_of(" \t\r\n");
			return value.substr(first, last - first + 1u);
		}

		void parse_parent_reference(const std::string& raw, std::string& parent, std::string& attachment)
		{
			parent = lower_copy(trim_copy(raw));
			attachment.clear();
			const auto comma = parent.find(',');
			if (comma == std::string::npos) return;
			attachment = trim_copy(parent.substr(comma + 1u));
			parent = trim_copy(parent.substr(0u, comma));
		}

		std::string normalized_class(std::string_view value)
		{
			std::string out;
			out.reserve(value.size());
			for (const char c : value)
			{
				if (std::isalnum(static_cast<unsigned char>(c))) {
					out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
				}
			}
			if (out.size() > 1u && out.front() == 'c') {
				out.erase(out.begin());
			}
			return out;
		}

		bool parse_quoted(std::string_view text, std::size_t& cursor, std::string& out)
		{
			out.clear();
			while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
				++cursor;
			}
			if (cursor >= text.size() || text[cursor] != '"') {
				return false;
			}
			++cursor;
			while (cursor < text.size())
			{
				const char c = text[cursor++];
				if (c == '"') {
					return true;
				}
				if (c == '\\' && cursor < text.size())
				{
					const char escaped = text[cursor++];
					switch (escaped)
					{
					case 'n': out.push_back('\n'); break;
					case 'r': out.push_back('\r'); break;
					case 't': out.push_back('\t'); break;
					case '\\': out.push_back('\\'); break;
					case '"': out.push_back('"'); break;
					default: out.push_back(escaped); break;
					}
					continue;
				}
				out.push_back(c);
			}
			return false;
		}

		bool parse_int(std::string_view text, std::int32_t& out)
		{
			if (text.empty()) return false;
			std::int32_t value = 0;
			const auto* begin = text.data();
			const auto* end = text.data() + text.size();
			const auto result = std::from_chars(begin, end, value);
			if (result.ec != std::errc{} || result.ptr != end) return false;
			out = value;
			return true;
		}

		bool parse_float(std::string_view text, float& out)
		{
			if (text.empty()) return false;
			std::string value(text);
			char* end = nullptr;
			const float parsed = std::strtof(value.c_str(), &end);
			if (end != value.c_str() + value.size() || !std::isfinite(parsed)) return false;
			out = parsed;
			return true;
		}

		bool parse_vec3(std::string_view text, Vector& out)
		{
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
			std::string value(text);
			if (std::sscanf(value.c_str(), "%f %f %f", &x, &y, &z) != 3) {
				return false;
			}
			if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
				return false;
			}
			out = Vector(x, y, z);
			return true;
		}

		const std::string* find_last_value(const entity_s& entity, std::string_view key)
		{
			for (auto it = entity.keyvalues.rbegin(); it != entity.keyvalues.rend(); ++it)
			{
				if (it->first == key) return &it->second;
			}
			return nullptr;
		}

		io_action_kind classify_input(std::string_view input)
		{
			const auto normalized = lower_copy(trim_copy(std::string(input)));
			if (normalized == "turnon" || normalized == "enable" || normalized == "show" ||
				normalized == "start" || normalized == "lighton") return io_action_kind::enable;
			if (normalized == "turnoff" || normalized == "disable" || normalized == "hide" ||
				normalized == "stop" || normalized == "lightoff") return io_action_kind::disable;
			if (normalized == "toggle" || normalized == "togglestate") return io_action_kind::toggle;
			if (normalized == "kill" || normalized == "killhierarchy" || normalized == "break") return io_action_kind::kill;
			return io_action_kind::unknown;
		}

		bool wildcard_match(std::string_view pattern, std::string_view value)
		{
			std::size_t p = 0u;
			std::size_t v = 0u;
			std::size_t star = std::string_view::npos;
			std::size_t retry = 0u;
			while (v < value.size())
			{
				if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) { ++p; ++v; continue; }
				if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = v; continue; }
				if (star != std::string_view::npos) { p = star + 1u; v = ++retry; continue; }
				return false;
			}
			while (p < pattern.size() && pattern[p] == '*') ++p;
			return p == pattern.size();
		}

		std::vector<std::string> split_output_fields(const std::string& raw)
		{
			const char separator = raw.find(static_cast<char>(0x1b)) != std::string::npos
				? static_cast<char>(0x1b) : ',';
			std::vector<std::string> fields;
			std::size_t begin = 0u;
			while (begin <= raw.size())
			{
				const auto end = raw.find(separator, begin);
				fields.emplace_back(trim_copy(raw.substr(begin, end == std::string::npos ? std::string::npos : end - begin)));
				if (end == std::string::npos) break;
				begin = end + 1u;
			}
			return fields;
		}

		bool parse_output_action(const std::uint32_t source_index, const std::string& key,
			const std::string& value, io_action_s& out)
		{
			if (key.size() < 3u || !key.starts_with("on")) return false;
			const auto fields = split_output_fields(value);
			if (fields.size() < 2u || fields[0].empty() || fields[1].empty()) return false;
			out = {};
			out.source_entity_index = source_index;
			out.root_source_entity_index = source_index;
			out.output_name = lower_copy(key);
			if (const auto suffix = out.output_name.find('#'); suffix != std::string::npos) {
				out.output_name.erase(suffix);
			}
			out.target = lower_copy(fields[0]);
			out.input = lower_copy(fields[1]);
			if (fields.size() >= 3u) out.parameter = fields[2];
			if (fields.size() >= 4u) parse_float(fields[3], out.delay);
			if (fields.size() >= 5u) parse_int(fields[4], out.max_fires);
			out.kind = classify_input(out.input);
			out.root_output_name = out.output_name;
			out.path = out.output_name;
			return true;
		}

		bool is_multi_manager_class(const std::string_view classname)
		{
			const auto cls = lower_copy(classname);
			return cls == "multi_manager" || cls == "multi_manager_ext";
		}

		bool is_router_class(const std::string_view classname)
		{
			const auto cls = lower_copy(classname);
			return cls == "logic_relay" || cls == "logic_auto" || cls == "logic_timer" ||
				cls == "logic_case" || cls == "logic_branch" || cls == "logic_branch_listener" ||
				cls == "math_counter" || cls == "math_remap" || cls == "logic_compare" ||
				is_multi_manager_class(cls);
		}

		bool is_reserved_multi_manager_key(const std::string_view key)
		{
			return key == "classname" || key == "origin" || key == "angles" || key == "targetname" ||
				key == "spawnflags" || key == "hammerid" || key == "parentname" || key == "model" ||
				key == "wait" || key == "startdisabled" || key == "start_disabled" || key == "state" ||
				key.starts_with("on");
		}

		std::string strip_duplicate_key_suffix(std::string value)
		{
			const auto hash = value.find('#');
			if (hash != std::string::npos) value.erase(hash);
			return trim_copy(value);
		}

		void append_multi_manager_outputs(entity_s& entity)
		{
			if (!is_multi_manager_class(entity.classname)) return;
			for (const auto& [key, value] : entity.keyvalues)
			{
				if (is_reserved_multi_manager_key(key)) continue;
				float delay = 0.0f;
				if (!parse_float(value, delay)) continue;
				const auto target = lower_copy(strip_duplicate_key_suffix(key));
				if (target.empty()) continue;
				io_action_s action = {};
				action.source_entity_index = entity.source_index;
				action.root_source_entity_index = entity.source_index;
				action.output_name = "ontrigger";
				action.root_output_name = action.output_name;
				action.target = target;
				action.input = "toggle";
				action.delay = std::max(0.0f, delay);
				action.kind = io_action_kind::toggle;
				action.path = "ontrigger[multi_manager]";
				entity.outputs.emplace_back(std::move(action));
			}
		}

		void finalize_entity(entity_s& entity)
		{
			if (const auto* value = find_last_value(entity, "classname")) entity.classname = lower_copy(*value);
			if (const auto* value = find_last_value(entity, "targetname")) entity.targetname = lower_copy(*value);
			if (const auto* value = find_last_value(entity, "parentname")) parse_parent_reference(*value, entity.parentname, entity.parent_attachment);
			if (const auto* value = find_last_value(entity, "target")) entity.target = lower_copy(*value);
			if (const auto* value = find_last_value(entity, "model")) entity.model = lower_copy(*value);
			if (const auto* value = find_last_value(entity, "origin")) entity.has_origin = parse_vec3(*value, entity.origin);
			if (const auto* value = find_last_value(entity, "angles")) entity.has_angles = parse_vec3(*value, entity.angles);
			if (const auto* value = find_last_value(entity, "hammerid")) parse_int(*value, entity.hammer_id);
			if (const auto* value = find_last_value(entity, "style")) parse_int(*value, entity.style);
			if (const auto* value = find_last_value(entity, "spawnflags")) parse_int(*value, entity.spawnflags);
			if (const auto* value = find_last_value(entity, "health")) parse_int(*value, entity.initial_health);

			std::int32_t disabled = 0;
			if (const auto* value = find_last_value(entity, "startdisabled")) parse_int(*value, disabled);
			if (const auto* value = find_last_value(entity, "start_disabled")) parse_int(*value, disabled);
			if (const auto* value = find_last_value(entity, "state"))
			{
				std::int32_t state = 1;
				if (parse_int(*value, state) && state == 0) disabled = 1;
			}
			entity.starts_disabled = disabled != 0;
			entity.is_light = class_is_light(entity.classname);
			entity.is_bindable_owner = class_is_bindable_owner(entity.classname);
			entity.is_breakable_owner = class_is_breakable_owner(entity.classname);

			for (const auto& [key, value] : entity.keyvalues)
			{
				io_action_s action = {};
				if (parse_output_action(entity.source_index, key, value, action)) {
					entity.outputs.emplace_back(std::move(action));
				}
			}
			append_multi_manager_outputs(entity);
		}

		float direction_score(const entity_s& entity, const Vector& direction)
		{
			if (!entity.has_angles || direction.LengthSqr() <= 0.0001f) return 0.0f;
			Vector entity_direction(1.0f, 0.0f, 0.0f);
			utils::vector::AngleVectors(entity.angles, &entity_direction);
			if (entity_direction.LengthSqr() <= 0.0001f) return 0.0f;
			entity_direction.Normalize();
			Vector normalized = direction;
			normalized.Normalize();
			return std::clamp(entity_direction.Dot(normalized), -1.0f, 1.0f) * 12.0f;
		}

		bool light_class_compatible(std::string_view classname, const std::int32_t worldlight_type)
		{
			const auto cls = lower_copy(classname);
			const auto type = static_cast<source_bsp_lights::emit_type>(worldlight_type);
			switch (type)
			{
			case source_bsp_lights::emit_type::spotlight:
				return cls == "light_spot" || cls == "point_spotlight" || cls == "env_projectedtexture" || cls == "light_dynamic";
			case source_bsp_lights::emit_type::surface:
				return cls == "light" || cls == "light_spot" || cls == "env_sprite" || cls == "env_lightglow";
			case source_bsp_lights::emit_type::point:
			case source_bsp_lights::emit_type::quakelight:
				return cls == "light" || cls == "light_dynamic";
			default:
				return false;
			}
		}

		struct disjoint_set_s
		{
			explicit disjoint_set_s(const std::size_t count) : parent(count), rank(count, 0u)
			{
				for (std::size_t i = 0u; i < count; ++i) parent[i] = i;
			}

			std::size_t find(const std::size_t value)
			{
				if (parent[value] != value) parent[value] = find(parent[value]);
				return parent[value];
			}

			void unite(const std::size_t lhs, const std::size_t rhs)
			{
				auto a = find(lhs);
				auto b = find(rhs);
				if (a == b) return;
				if (rank[a] < rank[b]) std::swap(a, b);
				parent[b] = a;
				if (rank[a] == rank[b]) ++rank[a];
			}

			std::vector<std::size_t> parent;
			std::vector<std::uint8_t> rank;
		};

		std::vector<std::size_t> resolve_target_indices(const graph_s& graph, const std::string& target)
		{
			std::vector<std::size_t> resolved;
			if (target.empty() || target.front() == '!') return resolved;
			if (target.find_first_of("*?") != std::string::npos)
			{
				for (std::size_t index = 0u; index < graph.entities.size(); ++index)
				{
					const auto& entity = graph.entities[index];
					if (!entity.targetname.empty() && wildcard_match(target, entity.targetname)) resolved.push_back(index);
				}
				return resolved;
			}
			const auto range = graph.targetname_to_index.equal_range(target);
			for (auto it = range.first; it != range.second; ++it) resolved.push_back(it->second);
			return resolved;
		}

		std::vector<std::string> routed_output_names(const entity_s& entity, const std::string_view input)
		{
			const auto in = lower_copy(trim_copy(std::string(input)));
			if (in == "trigger" || (is_multi_manager_class(entity.classname) && in == "toggle")) return { "ontrigger" };
			if (in == "fireuser1") return { "onuser1" };
			if (in == "fireuser2") return { "onuser2" };
			if (in == "fireuser3") return { "onuser3" };
			if (in == "fireuser4") return { "onuser4" };
			if (in == "press") return { "onpressed" };
			if (in == "use") return { "onuse", "onpressed" };
			if (in == "break") return { "onbreak" };
			if (in == "kill" || in == "killhierarchy") return { "onkilled" };
			return {};
		}

		std::string entity_path_name(const entity_s& entity)
		{
			if (!entity.targetname.empty()) return entity.targetname;
			if (!entity.classname.empty()) return std::format("{}#{}", entity.classname, entity.source_index);
			return std::format("entity#{}", entity.source_index);
		}

		std::string action_visit_key(const io_action_s& action)
		{
			return std::format("{}|{}|{}|{}|{}", action.root_action_id, action.source_entity_index,
				action.output_name, action.target, action.input);
		}

		void build_io_graph(graph_s& graph)
		{
			if (graph.entities.empty()) return;
			disjoint_set_s groups(graph.entities.size());
			std::unordered_set<std::size_t> controlled_light_indices;
			std::unordered_map<std::string, std::vector<std::size_t>> semantic_targets;
			std::unordered_set<std::string> emitted_leaf_keys;

			std::unordered_map<std::string, std::vector<std::size_t>> named_lights;
			for (std::size_t index = 0u; index < graph.entities.size(); ++index)
			{
				const auto& entity = graph.entities[index];
				if (entity.is_light && !entity.targetname.empty()) named_lights[entity.targetname].push_back(index);
				if (is_router_class(entity.classname)) ++graph.io_relay_entities;
				if (is_multi_manager_class(entity.classname)) graph.io_multi_manager_links += static_cast<std::uint32_t>(entity.outputs.size());
			}
			for (const auto& [name, indices] : named_lights)
			{
				(void)name;
				for (std::size_t i = 1u; i < indices.size(); ++i) groups.unite(indices.front(), indices[i]);
			}

			for (const auto& entity : graph.entities)
			{
				for (auto action : entity.outputs)
				{
					action.root_action_id = static_cast<std::uint32_t>(graph.io_actions.size() + 1u);
					++graph.io_links;
					const auto targets = resolve_target_indices(graph, action.target);
					for (const auto target_index : targets)
					{
						if (target_index >= graph.entities.size()) continue;
						action.resolved_target_source_indices.push_back(graph.entities[target_index].source_index);
						++graph.io_resolved_links;
					}
					graph.io_actions.emplace_back(std::move(action));
				}
			}

			constexpr std::uint8_t max_path_depth = 8u;
			for (const auto& root_action : graph.io_actions)
			{
				std::unordered_set<std::string> active_edges;
				std::function<void(const io_action_s&, float, std::uint8_t, const std::string&)> walk;
				walk = [&](const io_action_s& current, const float accumulated_delay, const std::uint8_t depth, const std::string& path)
				{
					if (depth > max_path_depth)
					{
						++graph.io_depth_limited;
						return;
					}

					const auto edge_key = action_visit_key(current);
					if (!active_edges.insert(edge_key).second)
					{
						++graph.io_cycles_skipped;
						return;
					}

					const auto resolved_targets = resolve_target_indices(graph, current.target);
					for (const auto target_index : resolved_targets)
					{
						if (target_index >= graph.entities.size()) continue;
						const auto& target_entity = graph.entities[target_index];
						const float total_delay = accumulated_delay + std::max(0.0f, current.delay);
						const auto target_path = path.empty()
							? entity_path_name(target_entity)
							: std::format("{} -> {}", path, entity_path_name(target_entity));

						if (target_entity.is_light && current.kind != io_action_kind::unknown)
						{
							io_action_s leaf = current;
							leaf.root_source_entity_index = root_action.source_entity_index;
							leaf.root_action_id = root_action.root_action_id;
							leaf.root_output_name = root_action.output_name;
							leaf.delay = total_delay;
							leaf.path_depth = depth;
							leaf.indirect = depth > 0u;
							leaf.path = target_path;
							leaf.resolved_target_source_indices = { target_entity.source_index };
							const auto leaf_key = std::format("{}|{}|{}|{}|{}|{}", leaf.root_action_id,
								leaf.root_source_entity_index, leaf.output_name, static_cast<int>(leaf.kind),
								target_entity.source_index, leaf.path);
							if (!emitted_leaf_keys.insert(leaf_key).second) continue;
							graph.resolved_light_actions.emplace_back(std::move(leaf));
							controlled_light_indices.insert(target_index);
							++graph.io_light_links;
							if (depth > 0u) ++graph.io_transitive_light_links;
							if (current.kind == io_action_kind::kill) ++graph.io_kill_links;
							continue;
						}

						if (depth == max_path_depth || !is_router_class(target_entity.classname))
						{
							if (depth == max_path_depth && is_router_class(target_entity.classname)) ++graph.io_depth_limited;
							continue;
						}

						const auto routed_names = routed_output_names(target_entity, current.input);
						if (routed_names.empty()) continue;
						for (const auto& downstream : target_entity.outputs)
						{
							if (std::find(routed_names.begin(), routed_names.end(), downstream.output_name) == routed_names.end()) continue;
							auto routed = downstream;
							routed.root_source_entity_index = root_action.source_entity_index;
							routed.root_action_id = root_action.root_action_id;
							routed.root_output_name = root_action.output_name;
							++graph.io_relay_hops;
							walk(routed, total_delay, static_cast<std::uint8_t>(depth + 1u), target_path);
						}
					}
					active_edges.erase(edge_key);
				};

				const auto root_path = std::format("{}:{}", root_action.source_entity_index, root_action.output_name);
				walk(root_action, 0.0f, 0u, root_path);
			}

			for (std::size_t action_index = 0u; action_index < graph.resolved_light_actions.size(); ++action_index)
			{
				const auto& action = graph.resolved_light_actions[action_index];
				for (const auto source_index : action.resolved_target_source_indices)
				{
					if (source_index == 0u || static_cast<std::size_t>(source_index) > graph.entities.size()) continue;
					const auto target_index = static_cast<std::size_t>(source_index - 1u);
					const auto& target = graph.entities[target_index];
					if (!target.is_light) continue;
					graph.light_source_to_action_indices[source_index].push_back(action_index);
					const auto semantic_key = std::format("{}|{}|{}", action.root_source_entity_index,
						action.root_output_name, static_cast<int>(action.kind));
					auto& accumulated = semantic_targets[semantic_key];
					if (!accumulated.empty()) groups.unite(accumulated.front(), target_index);
					accumulated.push_back(target_index);
				}
			}

			std::unordered_map<std::size_t, std::uint32_t> root_to_group;
			std::uint32_t next_group = 1u;
			for (std::size_t index = 0u; index < graph.entities.size(); ++index)
			{
				if (!controlled_light_indices.contains(index)) continue;
				const auto root = groups.find(index);
				auto [it, inserted] = root_to_group.emplace(root, next_group);
				if (inserted) ++next_group;
				const auto group_id = it->second;
				const auto source_index = graph.entities[index].source_index;
				graph.light_source_to_control_group[source_index] = group_id;
				graph.control_group_members[group_id].push_back(source_index);
			}
			for (const auto& [group_id, members] : graph.control_group_members)
			{
				std::unordered_set<std::size_t> unique_actions;
				for (const auto source_index : members)
				{
					const auto found = graph.light_source_to_action_indices.find(source_index);
					if (found == graph.light_source_to_action_indices.end()) continue;
					for (const auto action_index : found->second)
					{
						if (action_index < graph.resolved_light_actions.size()) unique_actions.insert(action_index);
					}
				}
				auto& actions = graph.control_group_to_action_indices[group_id];
				actions.assign(unique_actions.begin(), unique_actions.end());
				std::sort(actions.begin(), actions.end(), [&](const std::size_t lhs, const std::size_t rhs)
				{
					const auto& a = graph.resolved_light_actions[lhs];
					const auto& b = graph.resolved_light_actions[rhs];
					if (a.delay != b.delay) return a.delay < b.delay;
					if (a.root_action_id != b.root_action_id) return a.root_action_id < b.root_action_id;
					return lhs < rhs;
				});
			}

			graph.controlled_light_entities = static_cast<std::uint32_t>(controlled_light_indices.size());
			graph.light_control_groups = static_cast<std::uint32_t>(graph.control_group_members.size());
		}

	}

	const char* io_action_kind_name(const io_action_kind kind)
	{
		switch (kind)
		{
		case io_action_kind::enable: return "enable";
		case io_action_kind::disable: return "disable";
		case io_action_kind::toggle: return "toggle";
		case io_action_kind::kill: return "kill";
		default: return "other";
		}
	}

	bool class_is_light(std::string_view classname)
	{
		const auto cls = lower_copy(classname);
		return cls == "light" || cls == "light_spot" || cls == "light_dynamic" ||
			cls == "point_spotlight" || cls == "env_projectedtexture" ||
			cls == "light_environment" || cls == "light_directional" ||
			cls == "env_sprite" || cls == "env_lightglow";
	}

	bool class_is_environment_light(std::string_view classname)
	{
		const auto cls = lower_copy(classname);
		return cls == "light_environment" || cls == "light_directional" ||
			cls == "env_cascade_light" || cls == "shadow_control" || cls == "env_sun";
	}

	bool class_is_runtime_light(std::string_view classname)
	{
		const auto cls = normalized_class(classname);
		return cls.find("dynamiclight") != std::string::npos ||
			cls.find("lightdynamic") != std::string::npos ||
			cls.find("projectedtexture") != std::string::npos ||
			cls.find("pointspotlight") != std::string::npos ||
			cls.find("spotlightend") != std::string::npos ||
			cls.find("lightglow") != std::string::npos;
	}

	bool class_is_breakable_owner(std::string_view classname)
	{
		const auto cls = lower_copy(classname);
		return cls.find("breakable") != std::string::npos || cls.find("physbox") != std::string::npos ||
			cls == "prop_physics" || cls == "prop_physics_multiplayer" || cls == "prop_physics_override" ||
			cls == "prop_fuel_barrel" || cls == "prop_car_alarm" || cls == "prop_car_glass";
	}

	bool class_is_bindable_owner(std::string_view classname)
	{
		const auto cls = lower_copy(classname);
		if (class_is_breakable_owner(cls)) return true;
		return cls == "prop_dynamic" || cls == "prop_dynamic_override" || cls == "prop_dynamic_ornament" || cls == "prop_door_rotating" ||
			cls == "func_door" || cls == "func_door_rotating" || cls == "func_movelinear" ||
			cls == "func_rotating" || cls == "func_tracktrain" || cls == "func_brush" ||
			cls == "momentary_rot_button" || cls == "func_button";
	}

	bool runtime_class_matches_map_class(std::string_view runtime_classname, std::string_view map_classname)
	{
		const auto runtime = normalized_class(runtime_classname);
		const auto map = normalized_class(map_classname);
		if (runtime.empty() || map.empty()) return false;
		if (runtime == map || runtime.find(map) != std::string::npos || map.find(runtime) != std::string::npos) return true;
		if (map.find("propphysics") != std::string::npos && runtime.find("physicsprop") != std::string::npos) return true;
		if (map.find("propdynamic") != std::string::npos && runtime.find("propdynamic") != std::string::npos) return true;
		if (map.find("propdoorrotating") != std::string::npos && runtime.find("propdoorrotating") != std::string::npos) return true;
		if (map.find("funcbreakable") != std::string::npos && runtime.find("breakable") != std::string::npos) return true;
		if (map.find("funcdoor") != std::string::npos && runtime.find("door") != std::string::npos) return true;
		if (map.find("funcrotating") != std::string::npos && runtime.find("rotating") != std::string::npos) return true;
		if (map.find("funcmovelinear") != std::string::npos && runtime.find("movelinear") != std::string::npos) return true;
		if (map.find("functracktrain") != std::string::npos && runtime.find("tracktrain") != std::string::npos) return true;
		if ((map.find("lightdynamic") != std::string::npos || map.find("dynamiclight") != std::string::npos) &&
			runtime.find("dynamiclight") != std::string::npos) return true;
		if (map.find("envprojectedtexture") != std::string::npos && runtime.find("projectedtexture") != std::string::npos) return true;
		if (map.find("pointspotlight") != std::string::npos && runtime.find("pointspotlight") != std::string::npos) return true;
		if (map.find("envlightglow") != std::string::npos && runtime.find("lightglow") != std::string::npos) return true;
		return false;
	}

	bool parse_entity_lump(const std::string_view text, graph_s& out, std::string& error)
	{
		out = {};
		error.clear();
		std::size_t cursor = 0u;
		std::uint32_t source_index = 0u;
		while (cursor < text.size())
		{
			while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
			if (cursor >= text.size()) break;
			if (text[cursor] != '{') {
				++cursor;
				continue;
			}
			++cursor;
			entity_s entity = {};
			entity.source_index = ++source_index;
			bool closed = false;
			while (cursor < text.size())
			{
				while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
				if (cursor >= text.size()) break;
				if (text[cursor] == '}') {
					++cursor;
					closed = true;
					break;
				}
				std::string key;
				std::string value;
				if (!parse_quoted(text, cursor, key) || !parse_quoted(text, cursor, value))
				{
					error = std::format("malformed entity key/value near byte {}", cursor);
					return false;
				}
				entity.keyvalues.emplace_back(lower_copy(key), value);
			}
			if (!closed) {
				error = std::format("unterminated entity block {}", source_index);
				return false;
			}
			finalize_entity(entity);
			const std::size_t index = out.entities.size();
			if (!entity.targetname.empty())
			{
				out.targetname_to_index.emplace(entity.targetname, index);
				++out.named_entities;
			}
			if (entity.is_light) ++out.light_entities;
			if (entity.is_bindable_owner) ++out.bindable_owners;
			if (!entity.parent_attachment.empty()) ++out.attachment_links;
			out.entities.emplace_back(std::move(entity));
		}
		if (out.entities.empty()) {
			error = "entity lump contained no entity blocks";
			return false;
		}
		build_io_graph(out);
		return true;
	}

	const entity_s* find_by_targetname(const graph_s& graph, const std::string_view targetname)
	{
		if (targetname.empty()) return nullptr;
		const auto range = graph.targetname_to_index.equal_range(lower_copy(targetname));
		if (range.first == range.second) return nullptr;
		const auto index = range.first->second;
		return index < graph.entities.size() ? &graph.entities[index] : nullptr;
	}

	const entity_s* find_by_hammer_id(const graph_s& graph, const std::int32_t hammer_id)
	{
		if (hammer_id < 0) return nullptr;
		for (const auto& entity : graph.entities)
		{
			if (entity.hammer_id == hammer_id) return &entity;
		}
		return nullptr;
	}

	io_action_kind classify_io_input(const std::string_view input)
	{
		return classify_input(input);
	}

	std::vector<std::string> routed_outputs_for_input(const std::string_view classname, const std::string_view input)
	{
		entity_s temporary = {};
		temporary.classname = lower_copy(classname);
		return routed_output_names(temporary, input);
	}

	std::uint32_t find_light_control_group(const graph_s& graph, const std::uint32_t light_source_index)
	{
		const auto it = graph.light_source_to_control_group.find(light_source_index);
		return it == graph.light_source_to_control_group.end() ? 0u : it->second;
	}

	const std::vector<std::size_t>* find_control_group_actions(const graph_s& graph, const std::uint32_t group_id)
	{
		const auto it = graph.control_group_to_action_indices.find(group_id);
		return it == graph.control_group_to_action_indices.end() ? nullptr : &it->second;
	}

	bool light_has_control_kind(const graph_s& graph, const std::uint32_t light_source_index, const io_action_kind kind)
	{
		const auto found = graph.light_source_to_action_indices.find(light_source_index);
		if (found == graph.light_source_to_action_indices.end()) return false;
		for (const auto action_index : found->second)
		{
			if (action_index < graph.resolved_light_actions.size() && graph.resolved_light_actions[action_index].kind == kind) return true;
		}
		return false;
	}

	bool control_group_all_have_kind(const graph_s& graph, const std::uint32_t group_id, const io_action_kind kind)
	{
		const auto found = graph.control_group_members.find(group_id);
		if (found == graph.control_group_members.end() || found->second.empty()) return false;
		for (const auto source_index : found->second)
		{
			if (!light_has_control_kind(graph, source_index, kind)) return false;
		}
		return true;
	}

	std::string describe_light_controls(const graph_s& graph, const std::uint32_t light_source_index)
	{
		const auto found = graph.light_source_to_action_indices.find(light_source_index);
		if (found == graph.light_source_to_action_indices.end()) return "not controlled by map I/O";
		std::string result;
		std::unordered_set<std::string> unique;
		for (const auto action_index : found->second)
		{
			if (action_index >= graph.resolved_light_actions.size()) continue;
			const auto& action = graph.resolved_light_actions[action_index];
			const auto text = action.indirect
				? std::format("{} via {} -> {} ({:.2f}s, depth {})", action.output_name,
					action.path, io_action_kind_name(action.kind), action.delay, action.path_depth)
				: std::format("{} -> {} {} ({:.2f}s)", action.output_name,
					action.target, io_action_kind_name(action.kind), action.delay);
			if (!unique.insert(text).second) continue;
			if (!result.empty()) result += "; ";
			result += text;
			if (result.size() > 420u) {
				result += " ...";
				break;
			}
		}
		return result.empty() ? "not controlled by map I/O" : result;
	}

	const entity_s* find_best_light_entity(const graph_s& graph, const Vector& origin,
		const std::int32_t style, const std::int32_t worldlight_type, const Vector& direction,
		const float max_distance, float* out_score)
	{
		const float max_distance_sq = std::max(1.0f, max_distance) * std::max(1.0f, max_distance);
		const entity_s* best = nullptr;
		float best_score = -1.0e9f;
		for (const auto& entity : graph.entities)
		{
			if (!entity.is_light || !entity.has_origin || !light_class_compatible(entity.classname, worldlight_type)) continue;
			const float distance_sq = entity.origin.DistToSqr(origin);
			if (distance_sq > max_distance_sq) continue;
			const float distance = std::sqrt(std::max(0.0f, distance_sq));
			float score = 100.0f - distance * 1.5f;
			if (style > 0 && entity.style == style) score += 40.0f;
			else if (style != entity.style) score -= 12.0f;
			if (!entity.targetname.empty()) score += 4.0f;
			if (!entity.parentname.empty()) score += 15.0f;
			score += direction_score(entity, direction);
			if (score > best_score)
			{
				best_score = score;
				best = &entity;
			}
		}
		if (out_score) *out_score = best_score;
		return best;
	}

	const entity_s* find_best_owner_entity(const graph_s& graph, const Vector& origin,
		const std::string_view preferred_parent, const float max_distance, float* out_score)
	{
		if (!preferred_parent.empty())
		{
			if (const auto* parent = find_by_targetname(graph, preferred_parent); parent && parent->is_bindable_owner)
			{
				if (out_score) *out_score = 1000.0f;
				return parent;
			}
		}

		const float max_distance_sq = std::max(1.0f, max_distance) * std::max(1.0f, max_distance);
		const entity_s* best = nullptr;
		float best_score = -1.0e9f;
		for (const auto& entity : graph.entities)
		{
			if (!entity.is_bindable_owner || !entity.has_origin) continue;
			const float distance_sq = entity.origin.DistToSqr(origin);
			if (distance_sq > max_distance_sq) continue;
			const float distance = std::sqrt(std::max(0.0f, distance_sq));
			float score = 100.0f - distance;
			if (entity.is_breakable_owner) score += 25.0f;
			if (!entity.targetname.empty()) score += 10.0f;
			if (!entity.model.empty()) score += 5.0f;
			if (score > best_score)
			{
				best_score = score;
				best = &entity;
			}
		}
		if (out_score) *out_score = best_score;
		return best;
	}
}
