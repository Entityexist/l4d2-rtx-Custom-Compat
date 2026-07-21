#pragma once
#include <atomic>
#include <memory>
#include <vector>

namespace components
{
	class component
	{
	public:
		component() {}
		virtual ~component() {}
	};

	class loader
	{
	public:
		static void initialize();
		static void uninitialize();

		static utils::memory::allocator* get_allocator();

		// Direct detours are installed by several component constructors before the
		// complete compatibility graph exists. Keep every top-level runtime callback
		// dormant until all components have been constructed.
		static bool is_runtime_ready() noexcept
		{
			return runtime_ready_.load(std::memory_order_acquire);
		}

	private:
		static std::vector<std::unique_ptr<component>> components_;
		static utils::memory::allocator mem_allocator_;
		static inline std::atomic<bool> runtime_ready_ = false;

		template<class ComponentType>
		static void register_component()
		{
			components_.emplace_back(std::make_unique<ComponentType>());
		}

	};
}

#include "modules/interfaces.hpp"
#include "modules/flags.hpp"
#include "modules/game_settings.hpp"
#include "modules/remix_api.hpp"
#include "modules/choreo_events.hpp"
#include "modules/sound_events.hpp"
#include "modules/remix_lights.hpp"
#include "modules/dynamic_lighting.hpp"
#include "modules/remix_vars.hpp"
#include "modules/remix_markers.hpp"
#include "modules/main_module.hpp"
#include "modules/static_scene_cache.hpp"
#include "modules/model_render.hpp"
#include "modules/material_exporter.hpp"
#include "modules/map_settings.hpp"
#include "modules/imgui.hpp"
