#pragma once

namespace components
{
	namespace cmd
	{
		extern bool sound_debug_printing;
	}

	class sound_events : public component
	{
	public:
		struct history_entry
		{
			std::uint32_t hash = 0u;
			std::string name;
			Vector origin = {};
			float delay = 0.0f;
			float volume = 0.0f;
			float time = 0.0f;
		};

		sound_events();
		~sound_events() = default;

		static inline sound_events* p_this = nullptr;
		static auto get() { return p_this; }

		static const std::deque<history_entry>& get_history();
		static void clear_history();

		static void push_history(std::uint32_t hash, const std::string& sound_name, const StartSoundParams_t* parms);
		static inline std::deque<history_entry> m_history = {};
		static inline std::uint64_t m_hook_event_count = 0u;
		static inline bool m_hook_target_valid = false;
		static inline bool m_hook_installed = false;
		static inline std::uint32_t m_hook_candidate_count = 0u;
		static inline bool m_hook_target_reselected = false;
		static inline float m_last_hook_event_time = 0.0f;
	};
}
