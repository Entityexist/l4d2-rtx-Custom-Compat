#pragma once
#include "map_settings.hpp"

namespace components
{
	class remix_markers : public component
	{
	public:
		remix_markers();

		static inline remix_markers* p_this = nullptr;
		static remix_markers* get() { return p_this; }

		static void draw_nocull_markers(bool allow_repeat = false);
		static inline std::uint64_t m_last_draw_frame = std::numeric_limits<std::uint64_t>::max();
		static inline std::uint64_t m_draw_calls = 0u;
		static inline std::uint64_t m_draw_no_device = 0u;
		static inline std::uint64_t m_drawn_markers = 0u;
		static void on_sound_start(const std::uint32_t& hash, const std::string& sound_name);
		static void on_event_start(const std::string_view& name, const std::string_view& actor, const std::string_view& event, const std::string_view& param1);
		static void on_client_frame();
	};
}
