#pragma once

#define VENGINE_EFFECTS_INTERFACE_VERSION "VEngineEffects001"

namespace components
{
	struct dlight_t;
}

namespace sdk
{
	class engine_effects
	{
	public:
		static constexpr int max_dynamic_lights = 32;

		int get_active_dlights(components::dlight_t* list[max_dynamic_lights]);
	};
}
