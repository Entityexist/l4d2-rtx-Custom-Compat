#include <std_include.hpp>

namespace sdk
{
	int engine_effects::get_active_dlights(components::dlight_t* list[max_dynamic_lights])
	{
		if (!this || !list) {
			return 0;
		}

		using original_fn = int(__thiscall*)(engine_effects*, components::dlight_t**);
		return (*reinterpret_cast<original_fn**>(this))[6](this, list);
	}
}
