#include "std_include.hpp"

#include <limits>
#include "source_map_light_overrides.hpp"
#include <cctype>
#include <sstream>
#include <iomanip>

namespace components::source_map_light_overrides
{
	namespace
	{
		std::string trim_copy(std::string value)
		{
			utils::trim(value);
			return value;
		}

		std::string lower_copy(std::string value)
		{
			return utils::str_to_lower(trim_copy(std::move(value)));
		}

		std::string normalize_map_name(std::string map)
		{
			std::replace(map.begin(), map.end(), '/', '\\');
			const auto slash = map.find_last_of('\\');
			if (slash != std::string::npos) map.erase(0, slash + 1u);
			const auto dot = map.rfind(".bsp");
			if (dot != std::string::npos) map.erase(dot);
			for (auto& c : map)
			{
				if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) c = '_';
			}
			return map;
		}

		std::filesystem::path override_path(const std::string& game_root, const std::string& map_name)
		{
			return std::filesystem::path(game_root + COMPMOD_ASSET_DIR) / "map_lights" /
				(normalize_map_name(map_name) + ".toml");
		}

		bool parse_bool(const std::string& value, bool& out)
		{
			const auto v = lower_copy(value);
			if (v == "true" || v == "1" || v == "yes" || v == "on") { out = true; return true; }
			if (v == "false" || v == "0" || v == "no" || v == "off") { out = false; return true; }
			return false;
		}

		bool parse_int(const std::string& value, int& out)
		{
			try
			{
				const auto normalized = trim_copy(value);
				std::size_t consumed = 0u;
				const int parsed = std::stoi(normalized, &consumed, 0);
				if (consumed != normalized.size()) return false;
				out = parsed;
				return true;
			}
			catch (...) { return false; }
		}

		bool parse_u32(const std::string& value, std::uint32_t& out)
		{
			try
			{
				const auto normalized = trim_copy(value);
				std::size_t consumed = 0u;
				const auto parsed = std::stoul(normalized, &consumed, 0);
				if (consumed != normalized.size() || parsed > std::numeric_limits<std::uint32_t>::max()) return false;
				out = static_cast<std::uint32_t>(parsed);
				return true;
			}
			catch (...) { return false; }
		}

		bool parse_float(const std::string& value, float& out)
		{
			try
			{
				const auto normalized = trim_copy(value);
				std::size_t consumed = 0u;
				const float parsed = std::stof(normalized, &consumed);
				if (consumed != normalized.size() || !std::isfinite(parsed)) return false;
				out = parsed;
				return true;
			}
			catch (...) { return false; }
		}

		std::string parse_string(std::string value)
		{
			value = trim_copy(std::move(value));
			if (value.size() >= 2u && ((value.front() == '"' && value.back() == '"') ||
				(value.front() == '\'' && value.back() == '\'')))
			{
				value = value.substr(1u, value.size() - 2u);
			}
			std::string out;
			out.reserve(value.size());
			bool escaped = false;
			for (const char c : value)
			{
				if (escaped) { out.push_back(c); escaped = false; continue; }
				if (c == '\\') { escaped = true; continue; }
				out.push_back(c);
			}
			if (escaped) out.push_back('\\');
			return out;
		}

		bool parse_vec3(std::string value, Vector& out)
		{
			value = trim_copy(std::move(value));
			if (value.size() >= 2u && value.front() == '[' && value.back() == ']') value = value.substr(1u, value.size() - 2u);
			std::replace(value.begin(), value.end(), ',', ' ');
			std::istringstream stream(value);
			float x = 0.0f, y = 0.0f, z = 0.0f;
			if (!(stream >> x >> y >> z) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
			out = Vector(x, y, z);
			return true;
		}

		std::string strip_comment(std::string line)
		{
			bool quoted = false;
			char quote = 0;
			for (std::size_t i = 0u; i < line.size(); ++i)
			{
				const char c = line[i];
				if ((c == '"' || c == '\'') && (i == 0u || line[i - 1u] != '\\'))
				{
					if (!quoted) { quoted = true; quote = c; }
					else if (quote == c) { quoted = false; quote = 0; }
				}
				if (!quoted && c == '#') { line.resize(i); break; }
			}
			return trim_copy(std::move(line));
		}

		bool apply_point_key(light_point_s& point, const std::string& raw_key, const std::string& raw_value)
		{
			const auto key = lower_copy(raw_key);
			if (key == "position") return parse_vec3(raw_value, point.position);
			if (key == "radiance" || key == "color") return parse_vec3(raw_value, point.radiance);
			if (key == "radiance_scalar" || key == "intensity") return parse_float(raw_value, point.radiance_scalar);
			if (key == "radius") return parse_float(raw_value, point.radius);
			if (key == "timepoint") return parse_float(raw_value, point.timepoint);
			if (key == "smoothness") return parse_float(raw_value, point.smoothness);
			if (key == "use_shaping" || key == "shaping") return parse_bool(raw_value, point.use_shaping);
			if (key == "direction") return parse_vec3(raw_value, point.direction);
			if (key == "angle_offset_attached") return parse_vec3(raw_value, point.angle_offset_attached);
			if (key == "degrees") return parse_float(raw_value, point.degrees);
			if (key == "softness") return parse_float(raw_value, point.softness);
			if (key == "exponent") return parse_float(raw_value, point.exponent);
			if (key == "volumetric_scale") return parse_float(raw_value, point.volumetric_scale);
			if (key == "authoring_shape") return parse_int(raw_value, point.authoring_shape);
			if (key == "authoring_width") return parse_float(raw_value, point.authoring_width);
			if (key == "authoring_height") return parse_float(raw_value, point.authoring_height);
			if (key == "authoring_length") return parse_float(raw_value, point.authoring_length);
			if (key == "authoring_range") return parse_float(raw_value, point.authoring_range);
			if (key == "light_rig_mode") return parse_int(raw_value, point.light_rig_mode);
			if (key == "ies_profile") { point.ies_profile = parse_string(raw_value); return true; }
			if (key == "ies_file") { point.ies_file = parse_string(raw_value); return true; }
			if (key == "ies_axis_rotation") return parse_float(raw_value, point.ies_axis_rotation);
			if (key == "ies_angle_scale") return parse_float(raw_value, point.ies_angle_scale);
			if (key == "ies_intensity_scale") return parse_float(raw_value, point.ies_intensity_scale);
			if (key == "ies_normalize") return parse_bool(raw_value, point.ies_normalize);
			if (key == "ies_strength") return parse_float(raw_value, point.ies_strength);
			if (key == "ies_focus") return parse_float(raw_value, point.ies_focus);
			if (key == "ies_emulation") return parse_bool(raw_value, point.ies_emulation);
			if (key == "ies_emulation_samples") return parse_int(raw_value, point.ies_emulation_samples);
			if (key == "ies_emulation_spread") return parse_float(raw_value, point.ies_emulation_spread);
			if (key == "ies_emulation_radius_scale") return parse_float(raw_value, point.ies_emulation_radius_scale);
			if (key == "ies_emulation_intensity_scale") return parse_float(raw_value, point.ies_emulation_intensity_scale);
			if (key == "ies_emulation_forward_offset") return parse_float(raw_value, point.ies_emulation_forward_offset);
			if (key == "ies_emulation_pattern") { point.ies_emulation_pattern = parse_string(raw_value); return true; }
			if (key == "ies_emulation_aspect") return parse_float(raw_value, point.ies_emulation_aspect);
			if (key == "ies_emulation_twist") return parse_float(raw_value, point.ies_emulation_twist);
			return false;
		}

		bool apply_key(light_override_s& entry, const std::string& raw_key, const std::string& raw_value)
		{
			const auto key = lower_copy(raw_key);
			if (key == "source_index") return parse_int(raw_value, entry.source_index);
			if (key == "hammer_id") return parse_int(raw_value, entry.hammer_id);
			if (key == "runtime_entity_index") return parse_int(raw_value, entry.runtime_entity_index);
			if (key == "runtime_kind") { entry.runtime_kind = parse_string(raw_value); return true; }
			if (key == "targetname") { entry.targetname = parse_string(raw_value); return true; }
			if (key == "classname") { entry.classname = parse_string(raw_value); return true; }
			if (key == "snapshot_only") return parse_bool(raw_value, entry.snapshot_only);
			if (key == "persistent_id") { entry.persistent_id = parse_string(raw_value); return true; }
			if (key == "full_rig") return parse_bool(raw_value, entry.full_rig);
			if (key == "deleted") return parse_bool(raw_value, entry.deleted);
			if (key == "group") { entry.group = parse_string(raw_value); return true; }
			if (key == "comment") { entry.comment = parse_string(raw_value); return true; }
			if (key == "run_once") { entry.has_run_once = parse_bool(raw_value, entry.run_once); return entry.has_run_once; }
			if (key == "loop") { entry.has_loop = parse_bool(raw_value, entry.loop); return entry.has_loop; }
			if (key == "loop_smoothing") { entry.has_loop_smoothing = parse_bool(raw_value, entry.loop_smoothing); return entry.has_loop_smoothing; }
			if (key == "trigger_always") { entry.has_trigger_always = parse_bool(raw_value, entry.trigger_always); return entry.has_trigger_always; }
			if (key == "trigger_choreo_name") { entry.trigger_choreo_name = parse_string(raw_value); return true; }
			if (key == "trigger_choreo_actor") { entry.trigger_choreo_actor = parse_string(raw_value); return true; }
			if (key == "trigger_choreo_event") { entry.trigger_choreo_event = parse_string(raw_value); return true; }
			if (key == "trigger_choreo_param1") { entry.trigger_choreo_param1 = parse_string(raw_value); return true; }
			if (key == "trigger_sound_hash") return parse_u32(raw_value, entry.trigger_sound_hash);
			if (key == "trigger_delay") return parse_float(raw_value, entry.trigger_delay);
			if (key == "kill_choreo_name") { entry.kill_choreo_name = parse_string(raw_value); return true; }
			if (key == "kill_sound_hash") return parse_u32(raw_value, entry.kill_sound_hash);
			if (key == "kill_delay") return parse_float(raw_value, entry.kill_delay);
			if (key == "attach_prop_radius") return parse_float(raw_value, entry.attach_prop_radius);
			if (key == "attach_prop_name") { entry.attach_prop_name = parse_string(raw_value); return true; }
			if (key == "attach_prop_mins") return parse_vec3(raw_value, entry.attach_prop_mins);
			if (key == "attach_prop_maxs") return parse_vec3(raw_value, entry.attach_prop_maxs);
			if (key == "attach_bone_index") return parse_int(raw_value, entry.attach_bone_index);
			if (key == "attach_bone_name") { entry.attach_bone_name = parse_string(raw_value); return true; }
			if (key == "source_kind") { entry.source_kind = parse_string(raw_value); return true; }
			if (key == "source_style") return parse_int(raw_value, entry.source_style);
			if (key == "source_owner") return parse_int(raw_value, entry.source_owner);
			if (key == "source_key") return parse_int(raw_value, entry.source_key);
			if (key == "source_transient") return parse_bool(raw_value, entry.source_transient);
			if (key == "source_live_link") return parse_bool(raw_value, entry.source_live_link);
			if (key == "enabled") { entry.has_enabled = parse_bool(raw_value, entry.enabled); return entry.has_enabled; }
			if (key == "position") { entry.has_position = parse_vec3(raw_value, entry.position); return entry.has_position; }
			if (key == "position_offset" || key == "offset") { entry.has_position_offset = parse_vec3(raw_value, entry.position_offset); return entry.has_position_offset; }
			if (key == "radiance" || key == "color") { entry.has_radiance = parse_vec3(raw_value, entry.radiance); return entry.has_radiance; }
			if (key == "intensity") { entry.has_intensity = parse_float(raw_value, entry.intensity); return entry.has_intensity; }
			if (key == "radius") { entry.has_radius = parse_float(raw_value, entry.radius); return entry.has_radius; }
			if (key == "direction") { entry.has_direction = parse_vec3(raw_value, entry.direction); return entry.has_direction; }
			if (key == "intensity_scale") { entry.has_intensity_scale = parse_float(raw_value, entry.intensity_scale); return entry.has_intensity_scale; }
			if (key == "radius_scale") { entry.has_radius_scale = parse_float(raw_value, entry.radius_scale); return entry.has_radius_scale; }
			if (key == "surface_offset") { entry.has_surface_offset = parse_float(raw_value, entry.surface_offset); return entry.has_surface_offset; }
			if (key == "shaping") { entry.has_shaping = parse_bool(raw_value, entry.shaping); return entry.has_shaping; }
			if (key == "degrees") { entry.has_degrees = parse_float(raw_value, entry.degrees); return entry.has_degrees; }
			if (key == "softness") { entry.has_softness = parse_float(raw_value, entry.softness); return entry.has_softness; }
			if (key == "exponent") { entry.has_exponent = parse_float(raw_value, entry.exponent); return entry.has_exponent; }
			if (key == "animation") { entry.animation = parse_string(raw_value); entry.has_animation = true; return true; }
			if (key == "animation_duration") { entry.has_animation_duration = parse_float(raw_value, entry.animation_duration); return entry.has_animation_duration; }
			if (key == "animation_speed" || key == "speed") { entry.has_animation_speed = parse_float(raw_value, entry.animation_speed); return entry.has_animation_speed; }
			if (key == "animation_variation" || key == "variation") { entry.has_animation_variation = parse_float(raw_value, entry.animation_variation); return entry.has_animation_variation; }
			if (key == "animation_axis") { entry.has_animation_axis = parse_vec3(raw_value, entry.animation_axis); return entry.has_animation_axis; }
			if (key == "animation_degrees") { entry.has_animation_degrees = parse_float(raw_value, entry.animation_degrees); return entry.has_animation_degrees; }
			if (key == "animation_phase") { entry.has_animation_phase = parse_float(raw_value, entry.animation_phase); return entry.has_animation_phase; }
			if (key == "property_animation") { entry.property_animation = parse_string(raw_value); entry.has_property_animation = true; return true; }
			if (key == "property_animation_duration") { entry.has_property_animation_duration = parse_float(raw_value, entry.property_animation_duration); return entry.has_property_animation_duration; }
			if (key == "property_animation_speed") { entry.has_property_animation_speed = parse_float(raw_value, entry.property_animation_speed); return entry.has_property_animation_speed; }
			if (key == "property_animation_variation") { entry.has_property_animation_variation = parse_float(raw_value, entry.property_animation_variation); return entry.has_property_animation_variation; }
			if (key == "property_animation_intensity") { entry.has_property_animation_intensity = parse_float(raw_value, entry.property_animation_intensity); return entry.has_property_animation_intensity; }
			if (key == "movement_animation") { entry.movement_animation = parse_string(raw_value); entry.has_movement_animation = true; return true; }
			if (key == "movement_animation_duration") { entry.has_movement_animation_duration = parse_float(raw_value, entry.movement_animation_duration); return entry.has_movement_animation_duration; }
			if (key == "movement_animation_speed") { entry.has_movement_animation_speed = parse_float(raw_value, entry.movement_animation_speed); return entry.has_movement_animation_speed; }
			if (key == "movement_animation_axis") { entry.has_movement_animation_axis = parse_vec3(raw_value, entry.movement_animation_axis); return entry.has_movement_animation_axis; }
			if (key == "movement_animation_degrees") { entry.has_movement_animation_degrees = parse_float(raw_value, entry.movement_animation_degrees); return entry.has_movement_animation_degrees; }
			if (key == "movement_animation_phase") { entry.has_movement_animation_phase = parse_float(raw_value, entry.movement_animation_phase); return entry.has_movement_animation_phase; }
			if (key == "movement_animation_distance") { entry.has_movement_animation_distance = parse_float(raw_value, entry.movement_animation_distance); return entry.has_movement_animation_distance; }
			if (key == "owner_targetname") { entry.owner_targetname = parse_string(raw_value); entry.has_owner_targetname = true; return true; }
			if (key == "unbind_owner") { entry.has_unbind_owner = parse_bool(raw_value, entry.unbind_owner); return entry.has_unbind_owner; }
			if (key == "surface_cluster") { entry.has_surface_cluster = parse_bool(raw_value, entry.surface_cluster); return entry.has_surface_cluster; }
			if (key == "surface_samples") { entry.has_surface_samples = parse_int(raw_value, entry.surface_samples); return entry.has_surface_samples; }
			if (key == "surface_spread") { entry.has_surface_spread = parse_float(raw_value, entry.surface_spread); return entry.has_surface_spread; }
			if (key == "surface_aspect") { entry.has_surface_aspect = parse_float(raw_value, entry.surface_aspect); return entry.has_surface_aspect; }
			if (key == "surface_intensity") { entry.has_surface_intensity = parse_float(raw_value, entry.surface_intensity); return entry.has_surface_intensity; }
			if (key == "surface_radius_scale") { entry.has_surface_radius_scale = parse_float(raw_value, entry.surface_radius_scale); return entry.has_surface_radius_scale; }
			if (key == "surface_pattern") { entry.surface_pattern = parse_string(raw_value); entry.has_surface_pattern = true; return true; }
			return false;
		}

		bool selectors_match(const light_override_s& entry, const std::int32_t source_index,
			const std::int32_t hammer_id, const std::string_view targetname, const std::string_view classname)
		{
			if (entry.runtime_entity_index >= 0) return false;
			if (source_index >= 0 && entry.source_index >= 0) return entry.source_index == source_index;
			if (hammer_id >= 0 && entry.hammer_id >= 0) return entry.hammer_id == hammer_id;
			if (!targetname.empty() && !entry.targetname.empty())
			{
				if (utils::str_to_lower(entry.targetname) != utils::str_to_lower(std::string(targetname))) return false;
				return classname.empty() || entry.classname.empty() ||
					utils::str_to_lower(entry.classname) == utils::str_to_lower(std::string(classname));
			}
			return false;
		}

		bool same_selector(const light_override_s& lhs, const light_override_s& rhs)
		{
			if (!lhs.persistent_id.empty() || !rhs.persistent_id.empty())
				return !lhs.persistent_id.empty() && !rhs.persistent_id.empty() &&
					utils::str_to_lower(lhs.persistent_id) == utils::str_to_lower(rhs.persistent_id);
			if (lhs.runtime_entity_index >= 0 || rhs.runtime_entity_index >= 0)
			{
				return lhs.runtime_entity_index == rhs.runtime_entity_index &&
					utils::str_to_lower(lhs.runtime_kind) == utils::str_to_lower(rhs.runtime_kind) &&
					(lhs.classname.empty() || rhs.classname.empty() ||
						utils::str_to_lower(lhs.classname) == utils::str_to_lower(rhs.classname));
			}
			return selectors_match(lhs, rhs.source_index, rhs.hammer_id, rhs.targetname, rhs.classname);
		}

		std::string escaped(std::string_view value)
		{
			std::string out;
			out.reserve(value.size());
			for (const char c : value)
			{
				if (c == '\\' || c == '"') out.push_back('\\');
				out.push_back(c);
			}
			return out;
		}

		void write_vec3(std::ostream& file, const char* key, const Vector& value)
		{
			file << key << " = [" << value.x << ", " << value.y << ", " << value.z << "]\n";
		}

		void write_point(std::ostream& file, const light_point_s& p)
		{
			file << "\n[[light.point]]\n";
			write_vec3(file, "position", p.position);
			write_vec3(file, "color", p.radiance);
			file << "radiance_scalar = " << p.radiance_scalar << "\n";
			file << "radius = " << p.radius << "\n";
			file << "timepoint = " << p.timepoint << "\n";
			file << "smoothness = " << p.smoothness << "\n";
			file << "use_shaping = " << (p.use_shaping ? "true" : "false") << "\n";
			write_vec3(file, "direction", p.direction);
			write_vec3(file, "angle_offset_attached", p.angle_offset_attached);
			file << "degrees = " << p.degrees << "\n";
			file << "softness = " << p.softness << "\n";
			file << "exponent = " << p.exponent << "\n";
			file << "volumetric_scale = " << p.volumetric_scale << "\n";
			file << "authoring_shape = " << p.authoring_shape << "\n";
			file << "authoring_width = " << p.authoring_width << "\n";
			file << "authoring_height = " << p.authoring_height << "\n";
			file << "authoring_length = " << p.authoring_length << "\n";
			file << "authoring_range = " << p.authoring_range << "\n";
			file << "light_rig_mode = " << p.light_rig_mode << "\n";
			if (!p.ies_profile.empty()) file << "ies_profile = \"" << escaped(p.ies_profile) << "\"\n";
			if (!p.ies_file.empty()) file << "ies_file = \"" << escaped(p.ies_file) << "\"\n";
			file << "ies_axis_rotation = " << p.ies_axis_rotation << "\n";
			file << "ies_angle_scale = " << p.ies_angle_scale << "\n";
			file << "ies_intensity_scale = " << p.ies_intensity_scale << "\n";
			file << "ies_normalize = " << (p.ies_normalize ? "true" : "false") << "\n";
			file << "ies_strength = " << p.ies_strength << "\n";
			file << "ies_focus = " << p.ies_focus << "\n";
			file << "ies_emulation = " << (p.ies_emulation ? "true" : "false") << "\n";
			file << "ies_emulation_samples = " << p.ies_emulation_samples << "\n";
			file << "ies_emulation_spread = " << p.ies_emulation_spread << "\n";
			file << "ies_emulation_radius_scale = " << p.ies_emulation_radius_scale << "\n";
			file << "ies_emulation_intensity_scale = " << p.ies_emulation_intensity_scale << "\n";
			file << "ies_emulation_forward_offset = " << p.ies_emulation_forward_offset << "\n";
			file << "ies_emulation_pattern = \"" << escaped(p.ies_emulation_pattern) << "\"\n";
			file << "ies_emulation_aspect = " << p.ies_emulation_aspect << "\n";
			file << "ies_emulation_twist = " << p.ies_emulation_twist << "\n";
		}

		void write_entry(std::ostream& file, const light_override_s& entry)
		{
			file << "\n[[light]]\n";
			if (!entry.persistent_id.empty()) file << "persistent_id = \"" << escaped(entry.persistent_id) << "\"\n";
			if (entry.full_rig) file << "full_rig = true\n";
			if (entry.deleted) file << "deleted = true\n";
			if (entry.source_index >= 0) file << "source_index = " << entry.source_index << "\n";
			if (entry.hammer_id >= 0) file << "hammer_id = " << entry.hammer_id << "\n";
			if (entry.runtime_entity_index >= 0) file << "runtime_entity_index = " << entry.runtime_entity_index << "\n";
			if (!entry.runtime_kind.empty()) file << "runtime_kind = \"" << escaped(entry.runtime_kind) << "\"\n";
			if (!entry.targetname.empty()) file << "targetname = \"" << escaped(entry.targetname) << "\"\n";
			if (!entry.classname.empty()) file << "classname = \"" << escaped(entry.classname) << "\"\n";
			if (entry.snapshot_only) file << "snapshot_only = true\n";
			if (!entry.group.empty()) file << "group = \"" << escaped(entry.group) << "\"\n";
			if (!entry.comment.empty()) file << "comment = \"" << escaped(entry.comment) << "\"\n";
			if (entry.has_run_once) file << "run_once = " << (entry.run_once ? "true" : "false") << "\n";
			if (entry.has_loop) file << "loop = " << (entry.loop ? "true" : "false") << "\n";
			if (entry.has_loop_smoothing) file << "loop_smoothing = " << (entry.loop_smoothing ? "true" : "false") << "\n";
			if (entry.has_trigger_always) file << "trigger_always = " << (entry.trigger_always ? "true" : "false") << "\n";
			if (!entry.trigger_choreo_name.empty()) file << "trigger_choreo_name = \"" << escaped(entry.trigger_choreo_name) << "\"\n";
			if (!entry.trigger_choreo_actor.empty()) file << "trigger_choreo_actor = \"" << escaped(entry.trigger_choreo_actor) << "\"\n";
			if (!entry.trigger_choreo_event.empty()) file << "trigger_choreo_event = \"" << escaped(entry.trigger_choreo_event) << "\"\n";
			if (!entry.trigger_choreo_param1.empty()) file << "trigger_choreo_param1 = \"" << escaped(entry.trigger_choreo_param1) << "\"\n";
			if (entry.trigger_sound_hash) file << "trigger_sound_hash = " << entry.trigger_sound_hash << "\n";
			if (entry.trigger_delay != 0.0f) file << "trigger_delay = " << entry.trigger_delay << "\n";
			if (!entry.kill_choreo_name.empty()) file << "kill_choreo_name = \"" << escaped(entry.kill_choreo_name) << "\"\n";
			if (entry.kill_sound_hash) file << "kill_sound_hash = " << entry.kill_sound_hash << "\n";
			if (entry.kill_delay != 0.0f) file << "kill_delay = " << entry.kill_delay << "\n";
			if (entry.attach_prop_radius != 0.0f) file << "attach_prop_radius = " << entry.attach_prop_radius << "\n";
			if (!entry.attach_prop_name.empty()) file << "attach_prop_name = \"" << escaped(entry.attach_prop_name) << "\"\n";
			if (entry.attach_prop_mins.LengthSqr() > 0.0f) write_vec3(file, "attach_prop_mins", entry.attach_prop_mins);
			if (entry.attach_prop_maxs.LengthSqr() > 0.0f) write_vec3(file, "attach_prop_maxs", entry.attach_prop_maxs);
			if (entry.attach_bone_index >= 0) file << "attach_bone_index = " << entry.attach_bone_index << "\n";
			if (!entry.attach_bone_name.empty()) file << "attach_bone_name = \"" << escaped(entry.attach_bone_name) << "\"\n";
			if (!entry.source_kind.empty()) file << "source_kind = \"" << escaped(entry.source_kind) << "\"\n";
			if (entry.source_style != 0) file << "source_style = " << entry.source_style << "\n";
			if (entry.source_owner >= 0) file << "source_owner = " << entry.source_owner << "\n";
			if (entry.source_key != 0) file << "source_key = " << entry.source_key << "\n";
			if (entry.source_transient) file << "source_transient = true\n";
			if (entry.source_live_link) file << "source_live_link = true\n";
			if (entry.has_enabled) file << "enabled = " << (entry.enabled ? "true" : "false") << "\n";
			if (entry.has_position) write_vec3(file, "position", entry.position);
			if (entry.has_position_offset) write_vec3(file, "position_offset", entry.position_offset);
			if (entry.has_radiance) write_vec3(file, "color", entry.radiance);
			if (entry.has_intensity) file << "intensity = " << entry.intensity << "\n";
			if (entry.has_radius) file << "radius = " << entry.radius << "\n";
			if (entry.has_direction) write_vec3(file, "direction", entry.direction);
			if (entry.has_intensity_scale) file << "intensity_scale = " << entry.intensity_scale << "\n";
			if (entry.has_radius_scale) file << "radius_scale = " << entry.radius_scale << "\n";
			if (entry.has_surface_offset) file << "surface_offset = " << entry.surface_offset << "\n";
			if (entry.has_shaping) file << "shaping = " << (entry.shaping ? "true" : "false") << "\n";
			if (entry.has_degrees) file << "degrees = " << entry.degrees << "\n";
			if (entry.has_softness) file << "softness = " << entry.softness << "\n";
			if (entry.has_exponent) file << "exponent = " << entry.exponent << "\n";
			if (entry.has_animation) file << "animation = \"" << escaped(entry.animation) << "\"\n";
			if (entry.has_animation_duration) file << "animation_duration = " << entry.animation_duration << "\n";
			if (entry.has_animation_speed) file << "animation_speed = " << entry.animation_speed << "\n";
			if (entry.has_animation_variation) file << "animation_variation = " << entry.animation_variation << "\n";
			if (entry.has_animation_axis) write_vec3(file, "animation_axis", entry.animation_axis);
			if (entry.has_animation_degrees) file << "animation_degrees = " << entry.animation_degrees << "\n";
			if (entry.has_animation_phase) file << "animation_phase = " << entry.animation_phase << "\n";
			if (entry.has_property_animation) file << "property_animation = \"" << escaped(entry.property_animation) << "\"\n";
			if (entry.has_property_animation_duration) file << "property_animation_duration = " << entry.property_animation_duration << "\n";
			if (entry.has_property_animation_speed) file << "property_animation_speed = " << entry.property_animation_speed << "\n";
			if (entry.has_property_animation_variation) file << "property_animation_variation = " << entry.property_animation_variation << "\n";
			if (entry.has_property_animation_intensity) file << "property_animation_intensity = " << entry.property_animation_intensity << "\n";
			if (entry.has_movement_animation) file << "movement_animation = \"" << escaped(entry.movement_animation) << "\"\n";
			if (entry.has_movement_animation_duration) file << "movement_animation_duration = " << entry.movement_animation_duration << "\n";
			if (entry.has_movement_animation_speed) file << "movement_animation_speed = " << entry.movement_animation_speed << "\n";
			if (entry.has_movement_animation_axis) write_vec3(file, "movement_animation_axis", entry.movement_animation_axis);
			if (entry.has_movement_animation_degrees) file << "movement_animation_degrees = " << entry.movement_animation_degrees << "\n";
			if (entry.has_movement_animation_phase) file << "movement_animation_phase = " << entry.movement_animation_phase << "\n";
			if (entry.has_movement_animation_distance) file << "movement_animation_distance = " << entry.movement_animation_distance << "\n";
			if (entry.has_owner_targetname) file << "owner_targetname = \"" << escaped(entry.owner_targetname) << "\"\n";
			if (entry.has_unbind_owner) file << "unbind_owner = " << (entry.unbind_owner ? "true" : "false") << "\n";
			if (entry.has_surface_cluster) file << "surface_cluster = " << (entry.surface_cluster ? "true" : "false") << "\n";
			if (entry.has_surface_samples) file << "surface_samples = " << entry.surface_samples << "\n";
			if (entry.has_surface_spread) file << "surface_spread = " << entry.surface_spread << "\n";
			if (entry.has_surface_aspect) file << "surface_aspect = " << entry.surface_aspect << "\n";
			if (entry.has_surface_intensity) file << "surface_intensity = " << entry.surface_intensity << "\n";
			if (entry.has_surface_radius_scale) file << "surface_radius_scale = " << entry.surface_radius_scale << "\n";
			if (entry.has_surface_pattern) file << "surface_pattern = \"" << escaped(entry.surface_pattern) << "\"\n";
			for (const auto& point : entry.points) write_point(file, point);
		}

		bool write_all(const std::filesystem::path& path, const std::vector<light_override_s>& entries, std::string& error)
		{
			error.clear();
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			if (ec) { error = std::format("cannot create override directory: {}", ec.message()); return false; }

			const auto temp = path.string() + ".tmp";
			{
				std::ofstream file(temp, std::ios::trunc);
				if (!file.is_open()) { error = "cannot open temporary override file"; return false; }
				file << std::setprecision(9) << "version = 6\n";
				file << "# V20.9 per-map persistent light database. Full rigs, Source captures and tombstones share this file.\n";
				for (const auto& entry : entries) write_entry(file, entry);
				if (!file) { error = "failed while writing override file"; return false; }
			}

			if (!MoveFileExA(temp.c_str(), path.string().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				error = std::format("cannot replace override file (Win32 error {})", GetLastError());
				std::filesystem::remove(temp, ec);
				return false;
			}
			return true;
		}
	}

	bool load(const std::string& game_root, const std::string& map_name, load_result_s& out)
	{
		out = {};
		const auto path = override_path(game_root, map_name);
		out.path = path.string();
		std::ifstream file(path);
		if (!file.is_open())
		{
			out.status = "no per-map light database";
			return true;
		}

		out.file_found = true;
		light_override_s* current = nullptr;
		light_point_s* current_point = nullptr;
		std::string line;
		while (std::getline(file, line))
		{
			line = strip_comment(std::move(line));
			if (line.empty()) continue;
			const auto lowered = lower_copy(line);
			if (lowered == "[[light]]")
			{
				out.overrides.emplace_back();
				current = &out.overrides.back();
				current_point = nullptr;
				continue;
			}
			if (lowered == "[[light.point]]")
			{
				if (!current) { ++out.invalid_lines; continue; }
				current->points.emplace_back();
				current->full_rig = true;
				current_point = &current->points.back();
				continue;
			}
			const auto equals = line.find('=');
			if (equals == std::string::npos) { ++out.invalid_lines; continue; }
			const auto key = trim_copy(line.substr(0u, equals));
			const auto value = trim_copy(line.substr(equals + 1u));
			if (lower_copy(key) == "version") continue;
			const bool accepted = current_point ? apply_point_key(*current_point, key, value) : (current && apply_key(*current, key, value));
			if (!accepted) ++out.invalid_lines;
		}

		for (const auto& entry : out.overrides)
		{
			if (!entry.persistent_id.empty() && entry.full_rig && !entry.deleted) ++out.persistent_rigs;
			if (!entry.persistent_id.empty() && entry.deleted) ++out.tombstones;
		}
		out.status = std::format("loaded {} light records ({} full rigs, {} tombstones){}", out.overrides.size(),
			out.persistent_rigs, out.tombstones,
			out.invalid_lines ? std::format(" ({} invalid lines)", out.invalid_lines) : std::string{});
		return true;
	}

	const light_override_s* find_best(const load_result_s& data, const std::int32_t source_index,
		const std::int32_t hammer_id, const std::string_view targetname, const std::string_view classname)
	{
		const auto target_lower = utils::str_to_lower(std::string(targetname));
		const auto class_lower = utils::str_to_lower(std::string(classname));
		const light_override_s* best = nullptr;
		int best_score = -1;
		for (const auto& entry : data.overrides)
		{
			if (entry.runtime_entity_index >= 0) continue;
			int score = 0;
			bool selector = false;
			if (entry.source_index >= 0)
			{
				selector = true;
				if (entry.source_index != source_index) continue;
				score += 100;
			}
			if (entry.hammer_id >= 0)
			{
				selector = true;
				if (entry.hammer_id != hammer_id) continue;
				score += 80;
			}
			if (!entry.targetname.empty())
			{
				selector = true;
				if (utils::str_to_lower(entry.targetname) != target_lower) continue;
				score += 60;
			}
			if (!entry.classname.empty())
			{
				selector = true;
				if (utils::str_to_lower(entry.classname) != class_lower) continue;
				score += 20;
			}
			if (!selector) continue;
			if (score > best_score) { best = &entry; best_score = score; }
		}
		return best;
	}

	const light_override_s* find_runtime_entity(const load_result_s& data, const std::int32_t runtime_entity_index,
		const std::string_view runtime_kind, const std::string_view classname)
	{
		const auto kind_lower = utils::str_to_lower(std::string(runtime_kind));
		const auto class_lower = utils::str_to_lower(std::string(classname));
		const light_override_s* best = nullptr;
		int best_score = -1;
		for (const auto& entry : data.overrides)
		{
			if (entry.runtime_entity_index < 0 || entry.runtime_entity_index != runtime_entity_index) continue;
			int score = 100;
			if (!entry.runtime_kind.empty())
			{
				if (utils::str_to_lower(entry.runtime_kind) != kind_lower) continue;
				score += 40;
			}
			if (!entry.classname.empty())
			{
				if (utils::str_to_lower(entry.classname) != class_lower) continue;
				score += 20;
			}
			if (score > best_score) { best = &entry; best_score = score; }
		}
		return best;
	}

	const light_override_s* find_persistent(const load_result_s& data, const std::string_view persistent_id)
	{
		if (persistent_id.empty()) return nullptr;
		const auto wanted = utils::str_to_lower(std::string(persistent_id));
		for (const auto& entry : data.overrides)
		{
			if (!entry.persistent_id.empty() && utils::str_to_lower(entry.persistent_id) == wanted) return &entry;
		}
		return nullptr;
	}

	bool upsert(const std::string& game_root, const std::string& map_name,
		const light_override_s& entry, std::string& out_path, std::string& error)
	{
		const auto path = override_path(game_root, map_name);
		out_path = path.string();
		load_result_s loaded = {};
		if (!load(game_root, map_name, loaded)) { error = "failed to read current light database"; return false; }
		auto& entries = loaded.overrides;
		const auto found = std::find_if(entries.begin(), entries.end(), [&](const light_override_s& existing)
		{
			return same_selector(existing, entry);
		});
		if (found != entries.end()) *found = entry;
		else entries.emplace_back(entry);
		return write_all(path, entries, error);
	}

	bool replace_all(const std::string& game_root, const std::string& map_name,
		const std::vector<light_override_s>& entries, std::string& out_path, std::string& error)
	{
		const auto path = override_path(game_root, map_name);
		out_path = path.string();
		return write_all(path, entries, error);
	}

	bool erase(const std::string& game_root, const std::string& map_name,
		const std::int32_t source_index, const std::int32_t hammer_id, const std::string_view targetname,
		const std::string_view classname, std::string& out_path, std::string& error)
	{
		const auto path = override_path(game_root, map_name);
		out_path = path.string();
		load_result_s loaded = {};
		if (!load(game_root, map_name, loaded)) { error = "failed to read current light database"; return false; }
		const auto old_size = loaded.overrides.size();
		std::erase_if(loaded.overrides, [&](const light_override_s& existing)
		{
			return selectors_match(existing, source_index, hammer_id, targetname, classname);
		});
		if (loaded.overrides.size() == old_size) { error = "selected light has no saved override"; return false; }
		return write_all(path, loaded.overrides, error);
	}
}
