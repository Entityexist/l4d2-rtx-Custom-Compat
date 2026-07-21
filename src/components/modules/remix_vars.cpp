#include "std_include.hpp"
#include "remix_vars.hpp"

namespace components
{
	// checks if str is made up of numbers only
	// ignores dot, comma, minus and whitespaces
	bool is_single_num_or_vector(const std::string& str)
	{
		return std::ranges::all_of(str.begin(), str.end(), [](const char c) {
			return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == ',' || c == '.' || c == '-' || c == ' ';
		});
	}

	bool remix_vars::option_value::compare(const OPTION_TYPE type, const option_value& o) const
	{
		switch (type)
		{
		case OPTION_TYPE_BOOL: return enabled == o.enabled;
		case OPTION_TYPE_INT: return integer == o.integer;
		case OPTION_TYPE_FLOAT: return utils::float_equal(value, o.value);
		case OPTION_TYPE_VEC2:
			return utils::float_equal(vector[0], o.vector[0]) && utils::float_equal(vector[1], o.vector[1]);
		case OPTION_TYPE_VEC3:
			return utils::float_equal(vector[0], o.vector[0]) && utils::float_equal(vector[1], o.vector[1]) &&
				utils::float_equal(vector[2], o.vector[2]);
		case OPTION_TYPE_NONE: return true;
		}
		return false;
	}

	remix_vars::option_handle remix_vars::add_custom_option(const std::string& name, const option_s& o)
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		auto [it, inserted] = custom_options.insert_or_assign(name, o);
		(void)inserted;
		return &*it;
	}

	remix_vars::option_handle remix_vars::get_custom_option(const char* o)
	{
		if (!o) return nullptr;
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (const auto it = custom_options.find(o); it != custom_options.end()) return &*it;
		return nullptr;
	}

	remix_vars::option_handle remix_vars::get_custom_option(const std::string& o)
	{
		return get_custom_option(o.c_str());
	}

	remix_vars::option_handle remix_vars::get_option(const char* o)
	{
		if (!o) return nullptr;
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (const auto it = options.find(o); it != options.end()) return &*it;
		return nullptr;
	}

	remix_vars::option_handle remix_vars::get_option(const std::string& o)
	{
		return get_option(o.c_str());
	}

	namespace
	{
		std::string option_value_to_bridge_string(const remix_vars::OPTION_TYPE type,
			const remix_vars::option_value& value)
		{
			switch (type)
			{
			case remix_vars::OPTION_TYPE_BOOL: return value.enabled ? "True" : "False";
			case remix_vars::OPTION_TYPE_INT: return std::to_string(value.integer);
			case remix_vars::OPTION_TYPE_FLOAT: return std::to_string(value.value);
			case remix_vars::OPTION_TYPE_VEC2:
				return std::to_string(value.vector[0]) + ", " + std::to_string(value.vector[1]);
			case remix_vars::OPTION_TYPE_VEC3:
				return std::to_string(value.vector[0]) + ", " + std::to_string(value.vector[1]) + ", " +
					std::to_string(value.vector[2]);
			case remix_vars::OPTION_TYPE_NONE: return {};
			}
			return {};
		}
	}

	bool remix_vars::set_option(option_handle o, const option_value& v, const bool is_level_setting, const bool always)
	{
		if (!o || !remix_api::is_initialized()) return false;

		std::string name;
		std::string value_string;
		bool bridge_variable = true;
		{
			std::unique_lock<std::recursive_mutex> lock(mutex_);
			if (!always && o->second.current.compare(o->second.type, v))
			{
				if (is_level_setting) o->second.reset_level = v;
				return true;
			}

			o->second.current = v;
			if (is_level_setting) o->second.reset_level = v;
			o->second.modified = !o->second.current.compare(o->second.type, o->second.reset);
			name = o->first;
			value_string = option_value_to_bridge_string(o->second.type, v);
			bridge_variable = !o->second.not_a_remix_var;
		}

		if (!bridge_variable) return true;
		if (value_string.empty()) return false;

		// Never call into Remix Bridge while the option lock is held.  The runtime can
		// invoke callbacks on another thread and a lock here caused periodic hitches or
		// deadlocks in the direct upstream port.
		remix_api::get()->m_bridge.SetConfigVariable(name.c_str(), value_string.c_str());
		return true;
	}

	bool remix_vars::reset_option(option_handle o, const bool reset_to_level_state)
	{
		if (!o || !remix_api::is_initialized()) return false;
		option_value target = {};
		{
			std::lock_guard<std::recursive_mutex> lock(mutex_);
			target = reset_to_level_state ? o->second.reset_level : o->second.reset;
		}
		if (!set_option(o, target, false, true)) return false;
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		return !o->second.modified;
	}

	void remix_vars::reset_all_modified(const bool reset_to_level_state)
	{
		if (!remix_api::is_initialized()) return;
		std::vector<option_handle> modified;
		{
			std::lock_guard<std::recursive_mutex> lock(mutex_);
			modified.reserve(options.size());
			for (auto& o : options) if (o.second.modified) modified.push_back(&o);
		}
		for (auto* option : modified) reset_option(option, reset_to_level_state);
	}

	std::vector<remix_vars::option_snapshot> remix_vars::options_snapshot(const bool modified_only)
	{
		std::vector<option_snapshot> snapshot;
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		snapshot.reserve(options.size());
		for (const auto& [name, option] : options)
		{
			if (!modified_only || option.modified) snapshot.push_back({ name, option });
		}
		return snapshot;
	}

	bool remix_vars::has_interpolation_identifier(const std::uint64_t identifier)
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		return std::ranges::any_of(interpolate_stack, [identifier](const interpolate_entry_s& entry)
		{
			return entry.identifier == identifier;
		});
	}

	void remix_vars::clear_transitions()
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		interpolate_stack.clear();
	}

	/**
	 * Tries to convert a string to <option_value>
	 * @param type	variable type
	 * @param str	string containing the value/s
	 * @return		returns a valid <option_value> even if conversion failed 
	 */
	remix_vars::option_value remix_vars::string_to_option_value(OPTION_TYPE type, const std::string& str)
	{
		option_value out = {};

		switch (type)
		{
		case OPTION_TYPE_NONE:
		case OPTION_TYPE_BOOL:
			out.enabled = str == "True";
			break;
		case OPTION_TYPE_INT:
			out.integer = utils::try_stoi(str);
			break;
		case OPTION_TYPE_FLOAT:
			out.value = utils::try_stof(str);
			break;
		case OPTION_TYPE_VEC2:
			if (const auto v = utils::split(str, ','); v.size() == 2)
			{
				out.vector[0] = utils::try_stof(v[0]);
				out.vector[1] = utils::try_stof(v[1]);
			}
			break;
		case OPTION_TYPE_VEC3:
			if (const auto v = utils::split(str, ','); v.size() == 3)
			{
				out.vector[0] = utils::try_stof(v[0]);
				out.vector[1] = utils::try_stof(v[1]);
				out.vector[2] = utils::try_stof(v[2]);
			}
			break;
		}

		return out;
	}

	/**
	 * Tries to convert a string to <option_s>
	 * @param str	string containing the value/s
	 * @return		option_s - type NONE if conversion failed
	 */
	remix_vars::option_s remix_vars::string_to_option(const std::string& str)
	{
		option_s out = {};

		if (str == "True" || str == "False")
		{
			// is bool
			out.type = OPTION_TYPE_BOOL;
			out.current.enabled = str == "True";
		}
		else if (is_single_num_or_vector(str))
		{
			if (const auto x = utils::split(str, ','); x.size() > 1)
			{
				// is vector
				out.type = OPTION_TYPE_VEC2;
				out.current.vector[0] = utils::try_stof(x[0]);
				out.current.vector[1] = utils::try_stof(x[1]);

				if (x.size() > 2)
				{
					out.type = OPTION_TYPE_VEC3;
					out.current.vector[2] = utils::try_stof(x[2]);
				}
			}
			else
			{
				// is single float
				out.type = OPTION_TYPE_FLOAT; // treat everything as float
				out.current.value = utils::try_stof(str);
			}
		}

		out.reset = out.current;
		out.reset_level = out.current;

		return out;
	}

	/**
	 * Parses the rtx.conf in the root directory and builds an unordered map \n
	 * with pairs made of: <variable name> (std::string) and <variable value/type/...> (option_s) 
	 */
	void remix_vars::parse_rtx_options()
	{
		std::ifstream file;
		if (!utils::open_file_homepath("", "rtx.conf", file)) return;

		option_map parsed;
		std::string input;
		while (std::getline(file, input))
		{
			if (auto pair = utils::split(input, '='); pair.size() == 2u)
			{
				utils::trim(pair[0]);
				utils::trim(pair[1]);
				if (!pair[1].starts_with("0x") && !pair[1].empty())
				{
					if (const auto option = string_to_option(pair[1]); option.type != OPTION_TYPE_NONE) {
						parsed[pair[0]] = option;
					}
				}
			}
		}

		// Swap only after the complete file has been parsed. Readers never observe a
		// half-populated option table. Active transitions are invalid after a reload.
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		interpolate_stack.clear();
		options.swap(parsed);
	}

	/**
	 * Parses a .conf within the map_configs folder lerps to contained values
	 * @param conf_name				config name without extension
	 * @param identifier			unique identifier so one can check if it exists within the interpolate_stack
	 * @param ease					[EASE_TYPE] ease mode
	 * @param duration				duration of the transition (in seconds)
	 * @param delay					delay transition start (in seconds)
	 * @param delay_transition_back	delay between end of transition and transition back to the initial starting value (in seconds) - only active if value > 0
	 */
	void remix_vars::parse_and_apply_conf_with_lerp(const std::string& conf_name, const std::uint64_t& identifier, const EASE_TYPE ease, const float duration, const float delay, const float delay_transition_back)
	{
		std::ifstream file;
		if (utils::open_file_homepath(COMPMOD_ASSET_DIR "map_configs", conf_name, file))
		{
			std::string input;
			while (std::getline(file, input))
			{
				if (utils::starts_with(input, "#") || input.empty()) {
					continue;
				}

				if (auto pair = utils::split(input, '=');
					pair.size() == 2u)
				{
					utils::trim(pair[0]);
					utils::trim(pair[1]);

					if (pair[1].starts_with("0x") || pair[1].empty()) {
						continue;
					}

					if (const auto o = get_option(pair[0].c_str()); o)
					{
						const auto& v = string_to_option_value(o->second.type, pair[1]);

						remix_vars::get()->add_interpolate_entry(identifier, o, v, duration, delay, delay_transition_back, ease);
						//DEBUG_PRINT("[VAR-LERP] Start lerping var: %s to: %s\n", o->first.c_str(), pair[1].c_str());
					}
				}
			}

			file.close();
		}
		else
		{
			game::console();
			printf("[RemixVars] Failed to find config: \"%s\" in \"" COMPMOD_ASSET_DIR "map_configs\"\n", conf_name.c_str());
		}
	}


	// #
	// Interpolation

	 /**
	  * Adds a remix var (option) to the interpolation stack and linearly interpolates it
	  *	@param identifier				unique identifier so one can check if it exists within the interpolate_stack
	  * @param handle					handle of remix var option in the options map (can be nullptr if 'remix_var_name' is used instead)
	  * @param goal						transition goal
	  * @param duration					duration of the transition (in seconds)
	  * @param delay					delay transition start (seconds)
	  *	@param delay_transition_back	delay between end of transition and transition back to the initial starting value (in seconds) - only active if value > 0
	  * @param ease						[EASE_TYPE] ease mode
	  * @param remix_var_name			can be used if handle = nullptr
	  * @return
	  */
	bool remix_vars::add_interpolate_entry(const std::uint64_t& identifier, option_handle handle, const option_value& goal, const float duration, const float delay, const float delay_transition_back, EASE_TYPE ease, const std::string& remix_var_name)
	{
		std::unique_lock<std::recursive_mutex> lock(mutex_);
		option_handle h = handle;
		if (!h)
		{
			if (remix_var_name.empty()) {
				return false;
			}

			h = remix_vars::get()->get_option(remix_var_name);
		}

		if (h)
		{
			// directly apply when no duration and no delay
			if (duration <= 0.0f && delay <= 0.0f)
			{
				lock.unlock();
				return set_option(h, goal, false, true);
			}

			// interpolate over time or set after delay
			else
			{
				// check if we are already interpolating the value
				bool exists = false;
				bool has_entry = false;

				option_value prev_original = {};
				interpolate_entry_s* last_ip = nullptr;

				for (auto it = interpolate_stack.rbegin(); it != interpolate_stack.rend(); ++it)
				{
					auto& ip = *it;
					if (ip.option == h)
					{
						has_entry = true;
						last_ip = &ip;

						// Calculate current_remaining
						float current_remaining = 0.0f;
						if (ip._time_elapsed < 0.0f) {
							current_remaining = -ip._time_elapsed + ip.time_duration;
						}
						else {
							current_remaining = ip.time_duration - ip._time_elapsed;
						}

						// Calculate additional for pending back transition
						float additional = 0.0f;
						if (ip.time_delay_transition_back > 0.0f && !ip._in_backwards_transition) {
							additional = ip.time_delay_transition_back + ip.time_duration;
						}

						if (delay <= current_remaining + additional)
						{
							// update
							ip.identifier = identifier;
							ip._in_backwards_transition = false;
							ip.start = h->second.current;
							ip.goal = goal;
							ip.style = ease;
							ip.time_duration = std::max(0.0f, duration);
							ip.time_delay_transition_back = std::max(0.0f, delay_transition_back);
							ip._time_elapsed = -std::max(0.0f, delay);

							exists = true;
						}

						prev_original = ip.original_start;
						break;  // stop after processing most recent entry
					}
				}

				if (!exists)
				{
					interpolate_entry_s new_entry = {};
					new_entry.identifier = identifier;
					new_entry.option = h;
					new_entry.type = h->second.type;
					new_entry.style = ease;
					new_entry.time_duration = std::max(0.0f, duration);
					new_entry.time_delay_transition_back = std::max(0.0f, delay_transition_back);
					new_entry._time_elapsed = -std::max(0.0f, delay);

					if (has_entry)
					{
						option_value expected_final = {};

						// expected final of previous (last) entry
						if (last_ip->time_delay_transition_back > 0.0f) {
							expected_final = last_ip->original_start;
						}
						else {
							expected_final = last_ip->goal;
						}

						new_entry.start = expected_final;
						new_entry.original_start = prev_original; // propagate from previous
						new_entry.goal = goal;
					}
					else
					{
						new_entry.start = h->second.current;
						new_entry.original_start = h->second.current;  // First ever
						new_entry.goal = goal;
					}

					interpolate_stack.emplace_back(new_entry);
				}
			}

			return true;
		}

		return false;
	}


	void lerp_float(float* current, const float from, const float to, float fraction, remix_vars::EASE_TYPE style)
	{
		if (current)
		{
			const float distance = to - *current;
			if (std::fabs(distance) < 1e-8f)
			{
				*current = to;
				return;
			}

			float e = fraction;

			switch (style)
			{
			default:
			case remix_vars::EASE_TYPE_LINEAR:
				break;

			case remix_vars::EASE_TYPE_SIN_IN:
				e = 1.0f - cosf((fraction * M_PI) * 0.5f);
				break;

			case remix_vars::EASE_TYPE_SIN_OUT:
				e = sinf((fraction * M_PI) * 0.5f);
				break;

			case remix_vars::EASE_TYPE_SIN_INOUT:
				e = -(cosf(M_PI * fraction) - 1.0f) * 0.5f;
				break;

			case remix_vars::EASE_TYPE_CUBIC_IN:
				e = fraction * fraction * fraction;
				break;

			case remix_vars::EASE_TYPE_CUBIC_OUT:
				e = 1.0f - powf(1.0f - fraction, 3.0f);
				break;

			case remix_vars::EASE_TYPE_CUBIC_INOUT:
				e = fraction < 0.5
					? 4.0f * fraction * fraction * fraction
					: 1.0f - powf(-2.0f * fraction + 2.0f, 3.0f) * 0.5f;
				break;

			case remix_vars::EASE_TYPE_EXPO_IN:
				e = fraction == 0.0f
					? 0.0f
					: powf(2.0f, 10.0f * fraction - 10.0f);
				break;

			case remix_vars::EASE_TYPE_EXPO_OUT:
				e = fraction == 1.0f
					? 1.0f
					: 1.0f - powf(2.0f, -10.0f * fraction);
				break;

			case remix_vars::EASE_TYPE_EXPO_INOUT:
				e = fraction == 0.0f ? 0.0f : fraction == 1.0f ? 1.0f
						: fraction < 0.5f
							? powf(2.0f, 20.0f * fraction - 10.0f) * 0.5f
							: (2.0f - powf(2.0f, -20.0f * fraction + 10.0f)) * 0.5f;
				break;
			}

			*current = from + (to - from) * e;
		}
	}


	// main_module::on_map_load_hk
	void remix_vars::on_map_load()
	{
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		custom_options.clear();
		interpolate_stack.clear();
	}

	void remix_vars::on_map_unload()
	{
		{
			std::lock_guard<std::recursive_mutex> lock(mutex_);
			custom_options.clear();
			interpolate_stack.clear();
		}
		// Restore the rtx.conf baseline through forced bridge updates. Re-parsing the
		// entire file from Host_Disconnect performed synchronous I/O during the loading
		// transition and was a visible hitch on some systems.
		reset_all_modified(false);
	}

	// Interpolates Remix variables without holding the option lock while calling
	// into the runtime bridge. This is important because Present/render callbacks
	// may read the same table from another thread.
	void remix_vars::on_client_frame()
	{
		const auto interfaces_ptr = interfaces::get();
		if (!interfaces_ptr || !interfaces_ptr->m_engine || interfaces_ptr->m_engine->is_paused()) return;
		const auto globals = interfaces_ptr->m_globals;
		const float frame_time = globals ? std::max(0.0f, globals->frametime) : 0.0f;

		struct pending_update
		{
			option_handle option = nullptr;
			option_value value = {};
		};
		std::vector<pending_update> pending;

		{
			std::lock_guard<std::recursive_mutex> lock(mutex_);
			interpolate_stack.erase(std::remove_if(interpolate_stack.begin(), interpolate_stack.end(),
				[](const interpolate_entry_s& entry) { return entry._complete; }), interpolate_stack.end());
			pending.reserve(interpolate_stack.size());

			for (auto& ip : interpolate_stack)
			{
				if (!ip.option)
				{
					ip._complete = true;
					continue;
				}

				ip._time_elapsed += frame_time;
				if (ip._time_elapsed < 0.0f) continue;

				const float duration = std::max(0.0f, ip.time_duration);
				const bool transition_time_exceeded = duration <= 0.0f || ip._time_elapsed >= duration;
				const float fraction = transition_time_exceeded ? 1.0f : std::clamp(ip._time_elapsed / duration, 0.0f, 1.0f);
				option_value next = ip.option->second.current;

				switch (ip.type)
				{
				case OPTION_TYPE_INT:
					if (transition_time_exceeded) next.integer = ip.goal.integer;
					else
					{
						float value = static_cast<float>(next.integer);
						lerp_float(&value, static_cast<float>(ip.start.integer), static_cast<float>(ip.goal.integer), fraction, ip.style);
						next.integer = static_cast<int>(std::lround(value));
					}
					ip._complete = transition_time_exceeded || next.integer == ip.goal.integer;
					break;

				case OPTION_TYPE_FLOAT:
					if (transition_time_exceeded) next.value = ip.goal.value;
					else lerp_float(&next.value, ip.start.value, ip.goal.value, fraction, ip.style);
					ip._complete = transition_time_exceeded || utils::float_equal(next.value, ip.goal.value);
					break;

				case OPTION_TYPE_VEC2:
				case OPTION_TYPE_VEC3:
				{
					const int components = ip.type == OPTION_TYPE_VEC2 ? 2 : 3;
					for (int i = 0; i < components; ++i)
					{
						if (transition_time_exceeded) next.vector[i] = ip.goal.vector[i];
						else lerp_float(&next.vector[i], ip.start.vector[i], ip.goal.vector[i], fraction, ip.style);
					}
					ip._complete = transition_time_exceeded;
					if (!ip._complete)
					{
						ip._complete = true;
						for (int i = 0; i < components; ++i) ip._complete &= utils::float_equal(next.vector[i], ip.goal.vector[i]);
					}
					break;
				}

				case OPTION_TYPE_BOOL:
					// Forward transitions apply at their start. Reverse transitions retain the
					// first value until the delayed transition completes.
					if (!ip._in_backwards_transition || transition_time_exceeded) next.enabled = ip.goal.enabled;
					ip._complete = transition_time_exceeded;
					break;

				case OPTION_TYPE_NONE:
					ip._complete = true;
					continue;
				}

				ip.option->second.current = next;
				ip.option->second.modified = !next.compare(ip.option->second.type, ip.option->second.reset);
				if (!ip.option->second.not_a_remix_var) pending.push_back({ ip.option, next });

				if (ip._complete && !ip._in_backwards_transition && ip.time_delay_transition_back > 0.0f)
				{
					ip.start = ip.goal;
					ip.goal = ip.original_start;
					ip._time_elapsed = -std::max(0.0f, ip.time_delay_transition_back);
					ip._in_backwards_transition = true;
					ip._complete = false;
				}
			}
		}

		for (const auto& update : pending) set_option(update.option, update.value, false, true);
	}

	// #
	// #

	void remix_vars::on_sound_start(const std::uint32_t hash, const std::string_view& sound_name)
	{
		// check for spawn trigger
		auto& msettings = map_settings::get_map_settings();
		for (auto it = msettings.remix_transitions.begin(); it != msettings.remix_transitions.end();)
		{
			// only handle sound transitions
			if (it->trigger_type != map_settings::TRANSITION_TRIGGER_TYPE::SOUND) {
				++it; continue;
			}

			bool iterpp = false;
			if ((it->sound_hash && it->sound_hash == hash) || it->sound_name == sound_name)
			{
				const bool can_add_transition = !remix_vars::has_interpolation_identifier(it->hash);

				if (can_add_transition)
				{
					remix_vars::parse_and_apply_conf_with_lerp(
						it->config_name,
						it->hash,
						it->interpolate_type,
						it->duration,
						it->delay_in,
						it->delay_out);

					if (it->mode <= map_settings::TRANSITION_MODE::ONCE_ON_LEAVE)
					{
						it = msettings.remix_transitions.erase(it);
						iterpp = true; // erase returns the next iterator
					}
				}
			}

			if (!iterpp) {
				++it;
			}
		}
	}

	ConCommand xo_vars_parse_options_cmd{};
	void remix_vars::xo_vars_parse_options_fn()
	{
		{
			std::lock_guard<std::recursive_mutex> lock(remix_vars::mutex_);
			remix_vars::custom_options.clear();
			remix_vars::interpolate_stack.clear();
		}
		remix_vars::parse_rtx_options();

		// Reset all settings to the freshly parsed rtx.conf state. Work from stable
		// handles but perform bridge calls outside the map iteration lock.
		if (remix_api::is_initialized())
		{
			struct reset_request { option_handle handle; option_value value; };
			std::vector<reset_request> requests;
			{
				std::lock_guard<std::recursive_mutex> lock(remix_vars::mutex_);
				requests.reserve(remix_vars::options.size());
				for (auto& option : remix_vars::options) requests.push_back({ &option, option.second.reset_level });
			}
			for (const auto& request : requests) remix_vars::set_option(request.handle, request.value, false, true);
		}
	}

	ConCommand xo_vars_reset_all_options_cmd{};
	void xo_vars_reset_all_options_fn()
	{
		remix_vars::reset_all_modified(false);
	}

	ConCommand xo_vars_clear_transitions_cmd{};
	void xo_vars_clear_transitions_fn()
	{
		remix_vars::clear_transitions();
	}

	remix_vars::remix_vars()
	{
		p_this = this;

		// parse rtx.conf once
		parse_rtx_options();

		game::con_add_command(&xo_vars_parse_options_cmd, "xo_vars_parse_options", xo_vars_parse_options_fn, "Re-parse the rtx.conf and resets everything (incl. runtime settings - ignoring tex hashes)");
		game::con_add_command(&xo_vars_reset_all_options_cmd, "xo_vars_reset_all_options", xo_vars_reset_all_options_fn, "Reset all options (modified by .conf files) to the rtx.conf level");
		game::con_add_command(&xo_vars_clear_transitions_cmd, "xo_vars_clear_transitions", xo_vars_clear_transitions_fn, "Clear all ongoing transitions");
	}
}
