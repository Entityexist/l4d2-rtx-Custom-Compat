#include "std_include.hpp"

namespace components
{
	utils::memory::allocator loader::mem_allocator_;
	std::vector<std::unique_ptr<component>> loader::components_;

	void loader::initialize()
	{
		runtime_ready_.store(false, std::memory_order_release);
		mem_allocator_.clear();

		if (source_compat::uses_l4d2_runtime())
		{
			// Establish the game root before settings/material modules can touch files.
			// main_module keeps its legacy assignment as an idempotent fallback.
			if (game::root_path.empty())
			{
				char path[MAX_PATH] = {};
				GetModuleFileNameA(nullptr, path, MAX_PATH);
				game::root_path = path;
				utils::erase_substring(game::root_path, "left4dead2.exe");
			}

			// Build providers before event/render hooks. Several legacy hooks are direct
			// JMP patches and become executable inside their constructors, unlike MinHook
			// hooks that are enabled at the end of this function.
			register_component<interfaces>();
			register_component<flags>();
			register_component<game_settings>();
			register_component<remix_api>();
			register_component<remix_vars>();
			register_component<remix_markers>();
			register_component<map_settings>();
			register_component<remix_lights>();
			register_component<dynamic_lighting>();
			register_component<material_exporter>();

			// Event/input/frame hooks are registered only after all consumers exist.
			register_component<choreo_events>();
			register_component<sound_events>();
			register_component<imgui>();
			register_component<main_module>();
			register_component<model_render>();

			// Constructors above may install direct JMP detours immediately. Publish the
			// graph only after every consumer exists, then enable MinHook detours.
			runtime_ready_.store(true, std::memory_order_release);
			XASSERT(MH_EnableHook(MH_ALL_HOOKS) != MH_STATUS::MH_OK);
			return;
		}

		if (source_compat::uses_l4d1_bootstrap())
		{
			// L4D1 exposes the same audited Source interface generations for the core
			// client/engine/trace/cvar APIs. No game_settings/remix_vars component is
			// registered here because those paths still call an L4D2-resolved console
			// function. No MinHook or direct JMP patch is enabled in this branch.
			register_component<interfaces>();
			register_component<flags>();
			register_component<remix_api>();
			runtime_ready_.store(true, std::memory_order_release);

			game::console();
			std::cout << "[L4D1][Bootstrap] Source interfaces and Remix bridge initialized. "
				"All L4D2 binary hooks remain disabled." << std::endl;
			return;
		}

		// Safe Generic Source core
		// Generic Source and HL2 hybrid modes: no L4D2 interfaces, offsets, vtables,
		// flashlight entity reads or binary patches. Native L4D2 still uses the exact
		// full component order above.
		register_component<flags>();
		register_component<remix_api>();
		runtime_ready_.store(true, std::memory_order_release);

		game::console();
		std::cout << "[SourceCompat] Safe Source core initialized. Press F5 for compatibility diagnostics. "
			"Full Light Studio remains disabled until a native game profile supplies a safe D3D device, camera and interfaces." << std::endl;
	}

	void loader::uninitialize()
	{
		runtime_ready_.store(false, std::memory_order_release);

		components_.clear();
		mem_allocator_.clear();
		fflush(stdout);
		fflush(stderr);
	}


	utils::memory::allocator* loader::get_allocator() {
		return &loader::mem_allocator_;
	}
}
