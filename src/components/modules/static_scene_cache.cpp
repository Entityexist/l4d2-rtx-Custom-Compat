#include "std_include.hpp"
#include <cstdlib>
#include <stdexcept>

namespace components::static_scene_cache
{
	namespace
	{
		constexpr DWORD k_uninitialized_render_state = 0xfefefefeu;
		constexpr DWORD k_persistent_static_category = 1u << 25u;
		constexpr DWORD k_l4d2_sky3d_category = 1u << 26u;

		constexpr D3DRENDERSTATETYPE k_rs_categories = static_cast<D3DRENDERSTATETYPE>(42);
		constexpr D3DRENDERSTATETYPE k_rs_sky_scale = static_cast<D3DRENDERSTATETYPE>(71);
		constexpr D3DRENDERSTATETYPE k_rs_sky_origin_x = static_cast<D3DRENDERSTATETYPE>(72);
		constexpr D3DRENDERSTATETYPE k_rs_sky_origin_y = static_cast<D3DRENDERSTATETYPE>(73);
		constexpr D3DRENDERSTATETYPE k_rs_sky_origin_z = static_cast<D3DRENDERSTATETYPE>(74);

		constexpr std::uint32_t k_warmup_frames = 4u;
		constexpr std::uint32_t k_min_capture_frames = 16u;
		constexpr std::uint32_t k_settle_frames = 12u;
		constexpr std::uint32_t k_max_capture_frames = 300u;
		constexpr std::uint32_t k_extended_capture_frames = 600u;
		constexpr std::uint32_t k_cached_observations_required = 2u;
		constexpr std::uint32_t k_uncached_observations_required = 3u;
		constexpr std::uint32_t k_record_expiry_frames = 240u;
		constexpr std::uint32_t k_manifest_resave_interval = 300u;
		constexpr float k_manifest_ready_ratio = 0.96f;
		constexpr float k_manifest_degraded_ratio = 0.85f;
		constexpr std::uint32_t k_manifest_format = 2u;

		// A covered DrawWorldLists pass is sampled periodically instead of being
		// suppressed forever. This catches late decals, scripted world materials and
		// map events while still removing most per-frame world submissions.
		constexpr std::uint32_t k_pass_capture_confirmations = 3u;
		constexpr std::uint32_t k_pass_recovery_confirmations = 2u;
		constexpr std::uint32_t k_main_pass_validation_interval = 16u;
		constexpr std::uint32_t k_sky_pass_validation_interval = 90u;
		constexpr std::uint32_t k_draw_world_lists_vtable_index = 8u;

		struct record
		{
			std::uint64_t stable_signature = 0u;
			std::uint32_t first_seen_frame = 0u;
			std::uint32_t last_seen_frame = 0u;
			std::uint32_t submitted_frame = 0u;
			std::uint32_t observations = 0u;
			std::uint32_t consecutive_observations = 0u;
			bool persistent_submitted = false;
			bool cache_expected = false;
			bool sky3d = false;
			bool model = false;
		};

		struct model_scope
		{
			bool active = false;
			bool source_static_candidate = false;
			std::uint64_t model_hash = 0u;
			D3DXMATRIX transform = {};
		};


		struct world_pass_record
		{
			std::uint32_t view = 0u;
			std::uint32_t flags = 0u;
			std::uint32_t ordinal = 0u;
			std::int32_t water_bucket = 0;
			std::uint32_t first_seen_frame = 0u;
			std::uint32_t last_seen_frame = 0u;
			std::uint32_t next_validation_frame = 0u;
			std::uint32_t observations = 0u;
			std::uint32_t safe_confirmations = 0u;
			std::uint32_t validation_successes = 0u;
			std::uint32_t validation_failures = 0u;
			std::uint32_t baseline_draws = 0u;
			std::uint64_t baseline_fingerprint_xor = 0u;
			std::uint64_t baseline_fingerprint_sum = 0u;
			std::uint64_t executions = 0u;
			std::uint64_t skips = 0u;
			std::uint64_t estimated_draws_avoided = 0u;
			bool covered = false;
		};

		struct world_pass_scope
		{
			bool active = false;
			bool bypassed = false;
			bool validation_sample = false;
			std::uint64_t key = 0u;
			std::uint64_t fingerprint_xor = 0u;
			std::uint64_t fingerprint_sum = 0u;
			std::uint32_t draws = 0u;
			std::uint32_t safe_draws = 0u;
			std::uint32_t persistent_draws = 0u;
			std::uint32_t pending_draws = 0u;
			std::uint32_t unsafe_draws = 0u;
		};

		struct state
		{
			// V20.8 experimental policy: the complete static-map baking pipeline is
			// gated behind an explicit, session-only acknowledgement. requested_enabled
			// survives map transitions, while enabled describes the current map.
			bool experimental_acknowledged = false;
			bool requested_enabled = false;
			bool reload_required = false;
			experimental_profile requested_profile = experimental_profile::safe_preview;
			experimental_profile active_profile = experimental_profile::safe_preview;
			bool enabled = false;
			bool sky3d_fusion = false;
			bool world_pass_bypass = false;
			bool sky_pass_bypass = false;
			bool xorxor_water_quarantine = false;
			bool full_visibility_capture = false;
			bool model_info_classifier = false;
			bool force_pass_validation = false;
			bool manifest_loaded = false;
			bool manifest_valid = false;
			bool manifest_stale = false;
			bool manifest_dirty = false;
			bool manifest_auto_save = true;
			bool manifest_warm_start = true;
			bool capture_degraded = false;
			phase current = phase::disabled;
			std::string map_name;
			std::string map_key;
			std::string manifest_status = "No map loaded";
			std::filesystem::path manifest_path;
			std::uint64_t bsp_file_size = 0u;
			std::int64_t bsp_write_stamp = 0;
			std::uint32_t bsp_nodes = 0u;
			std::uint32_t bsp_leafs = 0u;
			std::uint32_t frame = 0u;
			std::uint32_t phase_start_frame = 0u;
			std::uint32_t last_new_record_frame = 0u;
			std::uint64_t candidates = 0u;
			std::uint64_t submitted = 0u;
			std::uint64_t skipped = 0u;
			std::uint64_t rejected_dynamic = 0u;
			std::uint64_t rejected_material = 0u;
			std::uint64_t sky_candidates = 0u;
			std::uint64_t model_candidates = 0u;
			std::uint64_t world_pass_calls = 0u;
			std::uint64_t world_pass_skips = 0u;
			std::uint64_t world_pass_validation_runs = 0u;
			std::uint64_t world_pass_invalidations = 0u;
			std::uint64_t estimated_draws_avoided = 0u;
			std::uint64_t expired_records = 0u;
			std::uint32_t last_manifest_save_frame = 0u;
			std::uint32_t main_pass_ordinal = 0u;
			std::uint32_t sky_pass_ordinal = 0u;
			std::unordered_map<std::uint64_t, record> records;
			std::unordered_map<std::uint64_t, world_pass_record> world_passes;
			std::unordered_set<std::uint64_t> manifest_expected_signatures;
			std::unordered_set<std::uint64_t> observed_stable_signatures;
		};

		state g_state;
		thread_local model_scope g_model_scope;
		thread_local CMeshDX8* g_current_mesh = nullptr;
		thread_local world_pass_scope g_world_pass_scope;
		utils::vtable g_engine_renderer_table;
		bool g_engine_renderer_hook_installed = false;

		struct queued_ui_action
		{
			ui_action action = ui_action::print_status;
			bool value = false;
		};

		constexpr std::size_t k_max_pending_ui_actions = 32u;
		std::mutex g_ui_action_mutex;
		std::deque<queued_ui_action> g_ui_actions;
		ui_action_status g_ui_action_status;

		constexpr bool k_runtime_mutation_quarantine = true;

		bool ui_action_is_read_only_or_config_only(const ui_action action)
		{
			switch (action)
			{
			case ui_action::set_acknowledged:
			case ui_action::set_manifest_auto_save:
			case ui_action::set_manifest_warm_start:
			case ui_action::export_audit:
			case ui_action::print_status:
				return true;
			default:
				return false;
			}
		}

		bool ui_action_is_runtime_available_internal(const ui_action action)
		{
			return !k_runtime_mutation_quarantine || ui_action_is_read_only_or_config_only(action);
		}

		const char* ui_action_block_reason_internal(const ui_action action)
		{
			if (ui_action_is_runtime_available_internal(action)) return "Available";
			switch (action)
			{
			case ui_action::install_pass_hook:
			case ui_action::toggle_world_pass_bypass:
			case ui_action::toggle_sky_pass_bypass:
			case ui_action::validate_world_passes:
				return "Blocked: the experimental DrawWorldLists hook and pass-bypass path is quarantined because it can crash the running game.";
			case ui_action::reload_current_map:
				return "Blocked: issuing a map reload from the render callback is not considered runtime-safe.";
			case ui_action::save_manifest:
			case ui_action::reload_manifest:
			case ui_action::clear_manifest:
				return "Blocked: live manifest mutation is quarantined until it is moved to a verified map lifecycle boundary.";
			default:
				return "Blocked: this operation mutates the experimental baker while the map is running and is quarantined for stability.";
			}
		}

		const char* ui_action_name(const ui_action action)
		{
			switch (action)
			{
			case ui_action::set_acknowledged: return "Set acknowledgement";
			case ui_action::activate_safe_preview: return "Activate Safe Preview";
			case ui_action::activate_full_bsp: return "Activate Full BSP Capture";
			case ui_action::activate_full_scene: return "Activate Full Scene Research";
			case ui_action::set_manifest_auto_save: return "Set manifest auto-save";
			case ui_action::set_manifest_warm_start: return "Set manifest warm start";
			case ui_action::save_manifest: return "Save manifest";
			case ui_action::reload_manifest: return "Reload manifest";
			case ui_action::clear_manifest: return "Clear manifest";
			case ui_action::export_audit: return "Export bake audit";
			case ui_action::rebuild_capture: return "Rebuild capture";
			case ui_action::reload_current_map: return "Reload current map";
			case ui_action::disable_experimental: return "Disable experimental baking";
			case ui_action::reset_defaults: return "Reset experimental defaults";
			case ui_action::install_pass_hook: return "Install pass hook";
			case ui_action::toggle_world_pass_bypass: return "Toggle main pass bypass";
			case ui_action::toggle_sky_pass_bypass: return "Toggle Sky3D pass bypass";
			case ui_action::validate_world_passes: return "Validate covered passes";
			case ui_action::print_status: return "Print bake and cache status";
			default: return "Unknown baker action";
			}
		}

		bool ui_action_replaces_pending(const ui_action queued, const ui_action incoming)
		{
			if (queued == incoming)
			{
				return incoming == ui_action::set_acknowledged ||
					incoming == ui_action::set_manifest_auto_save ||
					incoming == ui_action::set_manifest_warm_start;
			}

			const auto is_profile = [](const ui_action value)
			{
				return value == ui_action::activate_safe_preview ||
					value == ui_action::activate_full_bsp ||
					value == ui_action::activate_full_scene;
			};
			return is_profile(queued) && is_profile(incoming);
		}

		void update_ui_action_result(const ui_action action, const bool success, const std::string& result)
		{
			std::scoped_lock lock(g_ui_action_mutex);
			g_ui_action_status.last_action = ui_action_name(action);
			g_ui_action_status.last_result = result;
			if (success) ++g_ui_action_status.executed;
			else ++g_ui_action_status.failed;
			g_ui_action_status.pending_count = static_cast<std::uint32_t>(g_ui_actions.size());
			g_ui_action_status.pending = !g_ui_actions.empty();
		}

		void execute_ui_action(const queued_ui_action& request)
		{
			if (!ui_action_is_runtime_available_internal(request.action))
			{
				throw std::runtime_error(ui_action_block_reason_internal(request.action));
			}

			switch (request.action)
			{
			case ui_action::set_acknowledged:
				set_experimental_acknowledged(request.value);
				break;
			case ui_action::activate_safe_preview:
				activate_experimental_profile(experimental_profile::safe_preview);
				break;
			case ui_action::activate_full_bsp:
				activate_experimental_profile(experimental_profile::full_bsp);
				break;
			case ui_action::activate_full_scene:
				activate_experimental_profile(experimental_profile::full_scene);
				break;
			case ui_action::set_manifest_auto_save:
				set_manifest_auto_save(request.value);
				break;
			case ui_action::set_manifest_warm_start:
				set_manifest_warm_start(request.value);
				break;
			case ui_action::save_manifest:
				save_manifest();
				break;
			case ui_action::reload_manifest:
				reload_manifest();
				break;
			case ui_action::clear_manifest:
				clear_manifest();
				break;
			case ui_action::export_audit:
				export_audit_report();
				break;
			case ui_action::rebuild_capture:
				rebuild();
				break;
			case ui_action::reload_current_map:
			{
				const auto* intf = interfaces::get();
				if (!intf || !intf->m_engine) {
					throw std::runtime_error("Source engine interface is not ready");
				}
				intf->m_engine->execute_client_cmd_unrestricted("retry");
				break;
			}
			case ui_action::disable_experimental:
				disable_experimental();
				break;
			case ui_action::reset_defaults:
				reset_experimental_defaults();
				break;
			case ui_action::install_pass_hook:
				install_pass_hook_command();
				break;
			case ui_action::toggle_world_pass_bypass:
				toggle_world_pass_bypass();
				break;
			case ui_action::toggle_sky_pass_bypass:
				toggle_sky_pass_bypass();
				break;
			case ui_action::validate_world_passes:
				validate_world_passes();
				break;
			case ui_action::print_status:
				status();
				manifest_status();
				world_pass_status();
				break;
			default:
				throw std::runtime_error("Unknown deferred baker action");
			}
		}

		void process_pending_ui_actions()
		{
			// RenderView is the safe boundary: no ImGui callback is on the stack and
			// the static-scene containers have not started processing this frame yet.
			for (std::size_t processed = 0u; processed < 8u; ++processed)
			{
				queued_ui_action request;
				{
					std::scoped_lock lock(g_ui_action_mutex);
					if (g_ui_actions.empty())
					{
						g_ui_action_status.pending = false;
						g_ui_action_status.pending_count = 0u;
						return;
					}
					request = g_ui_actions.front();
					g_ui_actions.pop_front();
					g_ui_action_status.pending_count = static_cast<std::uint32_t>(g_ui_actions.size());
					g_ui_action_status.pending = !g_ui_actions.empty();
				}

				try
				{
					execute_ui_action(request);
					update_ui_action_result(request.action, true, "Executed safely at RenderView boundary");
				}
				catch (const std::exception& error)
				{
					update_ui_action_result(request.action, false, error.what());
					game::console();
					printf("[STATIC SCENE UI] Deferred action '%s' failed: %s\n",
						ui_action_name(request.action), error.what());
				}
				catch (...)
				{
					update_ui_action_result(request.action, false, "Unknown exception");
					game::console();
					printf("[STATIC SCENE UI] Deferred action '%s' failed with an unknown exception.\n",
						ui_action_name(request.action));
				}

				// A map reload invalidates the remainder of the current queue and must be
				// the final operation executed from this RenderView.
				if (request.action == ui_action::reload_current_map) {
					return;
				}
			}
		}

		const char* profile_name(const experimental_profile value)
		{
			switch (value)
			{
			case experimental_profile::safe_preview: return "Safe Preview";
			case experimental_profile::full_bsp: return "Full BSP Capture";
			case experimental_profile::full_scene: return "Full Scene Research";
			default: return "Unknown";
			}
		}

		const char* profile_cache_tag(const experimental_profile value)
		{
			switch (value)
			{
			case experimental_profile::safe_preview: return "safe";
			case experimental_profile::full_bsp: return "full_bsp";
			case experimental_profile::full_scene: return "full_scene";
			default: return "unknown";
			}
		}

		const char* quality_name(const cache_quality value)
		{
			switch (value)
			{
			case cache_quality::none: return "None";
			case cache_quality::learning: return "Learning";
			case cache_quality::good: return "Good";
			case cache_quality::complete: return "Complete";
			case cache_quality::degraded: return "Degraded";
			case cache_quality::stale: return "Stale";
			default: return "Unknown";
			}
		}

		void apply_profile_flags(state& target, const experimental_profile value)
		{
			target.sky3d_fusion = value != experimental_profile::safe_preview;
			target.full_visibility_capture = value != experimental_profile::safe_preview;
			target.model_info_classifier = value == experimental_profile::full_scene;

			// DrawWorldLists bypass is an independent resident-only optimization. A
			// profile never enables the invasive hook automatically.
			target.world_pass_bypass = false;
			target.sky_pass_bypass = false;
			target.force_pass_validation = false;
		}

		const char* phase_name(const phase value)
		{
			switch (value)
			{
			case phase::disabled: return "disabled";
			case phase::warmup: return "warmup";
			case phase::capturing: return "capturing";
			case phase::resident: return "resident";
			default: return "unknown";
			}
		}

		std::uint32_t current_source_frame()
		{
			if (const auto intf = interfaces::get(); intf && intf->m_globals) {
				return static_cast<std::uint32_t>(std::max(intf->m_globals->framecount, 0));
			}
			return g_state.frame + 1u;
		}

		std::uint64_t fnv1a_append(std::uint64_t hash, const void* data, const std::size_t size)
		{
			const auto* bytes = static_cast<const std::uint8_t*>(data);
			for (std::size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ull;
			}
			return hash;
		}

		template <typename value_type>
		std::uint64_t hash_value(std::uint64_t hash, const value_type& value)
		{
			return fnv1a_append(hash, &value, sizeof(value));
		}

		std::uint64_t hash_string(std::uint64_t hash, const std::string_view value)
		{
			return fnv1a_append(hash, value.data(), value.size());
		}

		std::int32_t quantize_transform_value(const float value)
		{
			if (!std::isfinite(value)) {
				return 0;
			}
			return static_cast<std::int32_t>(std::round(value * 1024.0f));
		}

		std::uint64_t hash_transform(std::uint64_t hash, const D3DXMATRIX& matrix)
		{
			for (std::uint32_t row = 0u; row < 4u; ++row)
			{
				for (std::uint32_t column = 0u; column < 4u; ++column)
				{
					const auto quantized = quantize_transform_value(matrix.m[row][column]);
					hash = hash_value(hash, quantized);
				}
			}
			return hash;
		}

		std::string sanitize_map_key(std::string value)
		{
			std::replace(value.begin(), value.end(), '\\', '/');
			const auto slash = value.find_last_of('/');
			if (slash != std::string::npos) {
				value.erase(0u, slash + 1u);
			}
			const auto dot = value.rfind(".bsp");
			if (dot != std::string::npos && dot + 4u == value.size()) {
				value.erase(dot);
			}
			for (auto& character : value)
			{
				const bool safe = (character >= 'a' && character <= 'z') ||
					(character >= 'A' && character <= 'Z') ||
					(character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.';
				if (!safe) character = '_';
			}
			return value.empty() ? "unknown" : value;
		}

		std::filesystem::path manifest_root_path()
		{
			return std::filesystem::path(game::root_path) / "l4d2-rtx" / "cache" / "static_scene";
		}

		std::filesystem::path audit_root_path()
		{
			return std::filesystem::path(game::root_path) / "l4d2-rtx" / "logs" / "static_scene";
		}

		std::uint64_t parse_u64(const std::string& text, const int base = 10)
		{
			char* end = nullptr;
			const auto value = std::strtoull(text.c_str(), &end, base);
			return end && end != text.c_str() ? static_cast<std::uint64_t>(value) : 0u;
		}

		std::int64_t parse_i64(const std::string& text)
		{
			char* end = nullptr;
			const auto value = std::strtoll(text.c_str(), &end, 10);
			return end && end != text.c_str() ? static_cast<std::int64_t>(value) : 0;
		}

		void refresh_map_identity()
		{
			g_state.map_key = sanitize_map_key(g_state.map_name);
			g_state.manifest_path = manifest_root_path() /
				(g_state.map_key + "." + profile_cache_tag(g_state.active_profile) + ".v208.bake");
			g_state.bsp_file_size = 0u;
			g_state.bsp_write_stamp = 0;
			g_state.bsp_nodes = 0u;
			g_state.bsp_leafs = 0u;

			if (const auto* world = game::get_hoststate_worldbrush_data(); world)
			{
				constexpr int k_max_reasonable_bsp_elements = 4'000'000;
				if (world->numnodes > 0 && world->numnodes <= k_max_reasonable_bsp_elements)
					g_state.bsp_nodes = static_cast<std::uint32_t>(world->numnodes);
				if (world->numleafs > 0 && world->numleafs <= k_max_reasonable_bsp_elements)
					g_state.bsp_leafs = static_cast<std::uint32_t>(world->numleafs);
			}

			const std::array<std::filesystem::path, 3> candidates{
				std::filesystem::path(game::root_path) / "left4dead2" / "maps" / (g_state.map_key + ".bsp"),
				std::filesystem::path(game::root_path) / "maps" / (g_state.map_key + ".bsp"),
				std::filesystem::current_path() / "left4dead2" / "maps" / (g_state.map_key + ".bsp"),
			};
			for (const auto& candidate : candidates)
			{
				std::error_code ec;
				if (!std::filesystem::is_regular_file(candidate, ec)) continue;
				g_state.bsp_file_size = std::filesystem::file_size(candidate, ec);
				if (ec) g_state.bsp_file_size = 0u;
				ec.clear();
				const auto stamp = std::filesystem::last_write_time(candidate, ec);
				if (!ec) {
					g_state.bsp_write_stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(stamp.time_since_epoch()).count();
				}
				break;
			}
		}

		std::uint64_t hash_texture_descriptor(std::uint64_t hash, IDirect3DBaseTexture9* texture)
		{
			const auto type = texture ? texture->GetType() : D3DRTYPE_FORCE_DWORD;
			hash = hash_value(hash, type);
			if (!texture) return hash;

			if (type == D3DRTYPE_TEXTURE)
			{
				auto* typed = static_cast<IDirect3DTexture9*>(texture);
				D3DSURFACE_DESC desc = {};
				const auto levels = typed->GetLevelCount();
				hash = hash_value(hash, levels);
				if (SUCCEEDED(typed->GetLevelDesc(0u, &desc)))
				{
					hash = hash_value(hash, desc.Width);
					hash = hash_value(hash, desc.Height);
					hash = hash_value(hash, desc.Format);
					hash = hash_value(hash, desc.Usage);
					hash = hash_value(hash, desc.Pool);
				}
			}
			else if (type == D3DRTYPE_CUBETEXTURE)
			{
				auto* typed = static_cast<IDirect3DCubeTexture9*>(texture);
				D3DSURFACE_DESC desc = {};
				const auto levels = typed->GetLevelCount();
				hash = hash_value(hash, levels);
				if (SUCCEEDED(typed->GetLevelDesc(0u, &desc)))
				{
					hash = hash_value(hash, desc.Width);
					hash = hash_value(hash, desc.Height);
					hash = hash_value(hash, desc.Format);
				}
			}
			else if (type == D3DRTYPE_VOLUMETEXTURE)
			{
				auto* typed = static_cast<IDirect3DVolumeTexture9*>(texture);
				D3DVOLUME_DESC desc = {};
				const auto levels = typed->GetLevelCount();
				hash = hash_value(hash, levels);
				if (SUCCEEDED(typed->GetLevelDesc(0u, &desc)))
				{
					hash = hash_value(hash, desc.Width);
					hash = hash_value(hash, desc.Height);
					hash = hash_value(hash, desc.Depth);
					hash = hash_value(hash, desc.Format);
				}
			}
			return hash;
		}

		std::uint64_t make_stable_draw_signature(IDirect3DDevice9* device, const prim_fvf_context& context,
			const draw_arguments& args, const bool sky3d)
		{
			std::uint64_t hash = 14695981039346656037ull;
			hash = hash_value(hash, args.type);
			hash = hash_value(hash, args.base_vertex_index);
			hash = hash_value(hash, args.min_vertex_index);
			hash = hash_value(hash, args.num_vertices);
			hash = hash_value(hash, args.start_index);
			hash = hash_value(hash, args.primitive_count);
			hash = hash_value(hash, sky3d);
			if (sky3d) hash = hash_value(hash, main_module::sky3d_payload_signature());
			hash = hash_string(hash, context.info.material_name);
			hash = hash_string(hash, context.info.shader_name);
			hash = hash_transform(hash, context.info.buffer_state.m_Transform[0]);
			if (g_current_mesh)
			{
				hash = hash_value(hash, g_current_mesh->m_VertexFormat);
				hash = hash_value(hash, g_current_mesh->m_FirstIndex);
				hash = hash_value(hash, g_current_mesh->m_NumIndices);
				hash = hash_value(hash, g_current_mesh->m_NumVertices);
				if (g_current_mesh->m_pVertexBuffer)
				{
					auto* vertex_buffer = reinterpret_cast<IVertexBuffer*>(g_current_mesh->m_pVertexBuffer);
					if (vertex_buffer->vftable)
					{
						if (vertex_buffer->vftable->VertexCount) hash = hash_value(hash, vertex_buffer->vftable->VertexCount(vertex_buffer));
						if (vertex_buffer->vftable->GetVertexFormat) hash = hash_value(hash, vertex_buffer->vftable->GetVertexFormat(vertex_buffer));
						if (vertex_buffer->vftable->IsDynamic) hash = hash_value(hash, vertex_buffer->vftable->IsDynamic(vertex_buffer));
					}
				}
				if (g_current_mesh->m_pIndexBuffer)
				{
					auto* index_buffer = reinterpret_cast<IIndexBuffer*>(g_current_mesh->m_pIndexBuffer);
					if (index_buffer->vftable)
					{
						if (index_buffer->vftable->IndexCount) hash = hash_value(hash, index_buffer->vftable->IndexCount(index_buffer));
						if (index_buffer->vftable->IndexFormat) hash = hash_value(hash, index_buffer->vftable->IndexFormat(index_buffer));
						if (index_buffer->vftable->IsDynamic) hash = hash_value(hash, index_buffer->vftable->IsDynamic(index_buffer));
					}
				}
			}
			if (g_model_scope.active)
			{
				hash = hash_value(hash, g_model_scope.model_hash);
				hash = hash_transform(hash, g_model_scope.transform);
			}
			for (std::uint32_t stage = 0u; stage < 4u; ++stage)
			{
				IDirect3DBaseTexture9* texture = nullptr;
				if (SUCCEEDED(device->GetTexture(stage, &texture)))
				{
					hash = hash_texture_descriptor(hash, texture);
					if (texture) texture->Release();
				}
			}
			D3DXMATRIX texture_transform = {};
			if (SUCCEEDED(device->GetTransform(D3DTS_TEXTURE0, &texture_transform))) {
				hash = hash_transform(hash, texture_transform);
			}
			DWORD texture_factor = 0u;
			if (SUCCEEDED(device->GetRenderState(D3DRS_TEXTUREFACTOR, &texture_factor))) {
				hash = hash_value(hash, texture_factor);
			}
			return hash;
		}

		std::uint64_t matched_manifest_signatures()
		{
			std::uint64_t matched = 0u;
			for (const auto signature : g_state.manifest_expected_signatures) {
				if (g_state.observed_stable_signatures.contains(signature)) ++matched;
			}
			return matched;
		}

		float manifest_coverage_ratio()
		{
			if (!g_state.manifest_valid || g_state.manifest_expected_signatures.empty()) return 0.0f;
			return static_cast<float>(matched_manifest_signatures()) /
				static_cast<float>(g_state.manifest_expected_signatures.size());
		}

		std::uint64_t stable_record_count()
		{
			std::uint64_t count = 0u;
			for (const auto& [key, entry] : g_state.records)
			{
				(void)key;
				const std::uint32_t required = entry.cache_expected && g_state.manifest_warm_start
					? k_cached_observations_required : k_uncached_observations_required;
				if (entry.persistent_submitted || entry.consecutive_observations >= required) ++count;
			}
			return count;
		}

		float world_pass_coverage_ratio()
		{
			if (g_state.world_passes.empty()) return 1.0f;
			std::uint64_t covered = 0u;
			for (const auto& [key, pass] : g_state.world_passes) {
				(void)key;
				if (pass.covered) ++covered;
			}
			return static_cast<float>(covered) / static_cast<float>(g_state.world_passes.size());
		}

		float completeness_score()
		{
			if (!g_state.enabled || g_state.records.empty()) return 0.0f;
			const float stability = static_cast<float>(stable_record_count()) /
				static_cast<float>(g_state.records.size());
			const auto quiet_frames = g_state.frame >= g_state.last_new_record_frame
				? g_state.frame - g_state.last_new_record_frame : 0u;
			const float quiet = std::min(1.0f,
				static_cast<float>(quiet_frames) / static_cast<float>(k_settle_frames));
			const float cache = g_state.manifest_valid && !g_state.manifest_expected_signatures.empty()
				? manifest_coverage_ratio() : 1.0f;
			const float passes = world_pass_coverage_ratio();
			float score = stability * 0.45f + quiet * 0.25f + cache * 0.20f + passes * 0.10f;
			if (g_state.current == phase::resident && !g_state.capture_degraded) score = std::max(score, 0.95f);
			return std::clamp(score, 0.0f, 1.0f);
		}

		cache_quality current_quality()
		{
			if (g_state.manifest_stale) return cache_quality::stale;
			if (!g_state.enabled) return cache_quality::none;
			if (g_state.capture_degraded) return cache_quality::degraded;
			if (g_state.current != phase::resident) return cache_quality::learning;
			return completeness_score() >= 0.985f ? cache_quality::complete : cache_quality::good;
		}

		void rebuild_observed_signatures()
		{
			g_state.observed_stable_signatures.clear();
			for (const auto& [key, entry] : g_state.records)
			{
				(void)key;
				const auto required = entry.cache_expected && g_state.manifest_warm_start
					? k_cached_observations_required
					: k_uncached_observations_required;
				if (entry.stable_signature != 0u &&
					(entry.persistent_submitted || entry.consecutive_observations >= required))
				{
					g_state.observed_stable_signatures.insert(entry.stable_signature);
				}
			}
		}

		void prune_stale_records()
		{
			if (g_state.current != phase::capturing || (g_state.frame % 60u) != 0u) return;
			bool removed = false;
			for (auto it = g_state.records.begin(); it != g_state.records.end();)
			{
				const auto& entry = it->second;
				if (!entry.persistent_submitted && g_state.frame > entry.last_seen_frame + k_record_expiry_frames)
				{
					it = g_state.records.erase(it);
					++g_state.expired_records;
					removed = true;
				}
				else ++it;
			}
			if (removed) rebuild_observed_signatures();
		}

		void load_manifest_internal()
		{
			g_state.manifest_loaded = false;
			g_state.manifest_valid = false;
			g_state.manifest_stale = false;
			g_state.manifest_expected_signatures.clear();
			g_state.manifest_status = "No manifest for this map/profile";
			std::ifstream file(g_state.manifest_path);
			if (!file.is_open()) return;

			std::uint32_t format = 0u;
			std::uint32_t profile = 0u;
			std::uint32_t nodes = 0u;
			std::uint32_t leafs = 0u;
			std::uint64_t bsp_size = 0u;
			std::int64_t bsp_stamp = 0;
			std::string map;
			std::unordered_set<std::uint64_t> signatures;
			std::string line;
			while (std::getline(file, line))
			{
				if (line.empty() || line[0] == '#') continue;
				const auto separator = line.find('=');
				if (separator == std::string::npos) continue;
				const auto key = line.substr(0u, separator);
				const auto value = line.substr(separator + 1u);
				if (key == "format") format = static_cast<std::uint32_t>(parse_u64(value));
				else if (key == "map") map = value;
				else if (key == "profile") profile = static_cast<std::uint32_t>(parse_u64(value));
				else if (key == "bsp_size") bsp_size = parse_u64(value);
				else if (key == "bsp_stamp") bsp_stamp = parse_i64(value);
				else if (key == "bsp_nodes") nodes = static_cast<std::uint32_t>(parse_u64(value));
				else if (key == "bsp_leafs") leafs = static_cast<std::uint32_t>(parse_u64(value));
				else if (key == "signature")
				{
					const auto signature = parse_u64(value, 16);
					if (signature != 0u) signatures.insert(signature);
				}
			}
			g_state.manifest_loaded = true;
			const bool identity_mismatch = format != k_manifest_format || map != g_state.map_key ||
				profile != static_cast<std::uint32_t>(g_state.active_profile) ||
				(g_state.bsp_file_size != 0u && bsp_size != 0u && g_state.bsp_file_size != bsp_size) ||
				(g_state.bsp_write_stamp != 0 && bsp_stamp != 0 && g_state.bsp_write_stamp != bsp_stamp) ||
				(g_state.bsp_nodes != 0u && nodes != 0u && g_state.bsp_nodes != nodes) ||
				(g_state.bsp_leafs != 0u && leafs != 0u && g_state.bsp_leafs != leafs);
			if (identity_mismatch || signatures.empty())
			{
				g_state.manifest_stale = true;
				g_state.manifest_status = identity_mismatch
					? "Manifest is stale for this map/profile"
					: "Manifest contains no usable signatures";
				return;
			}
			g_state.manifest_expected_signatures = std::move(signatures);
			g_state.manifest_valid = true;
			g_state.manifest_status = "Loaded " +
				std::to_string(g_state.manifest_expected_signatures.size()) + " stable signatures";
		}

		bool write_manifest_internal(const bool force)
		{
			std::set<std::uint64_t> signatures;
			for (const auto& [key, entry] : g_state.records)
			{
				(void)key;
				if (entry.persistent_submitted && entry.stable_signature != 0u) {
					signatures.insert(entry.stable_signature);
				}
			}
			if (signatures.empty())
			{
				g_state.manifest_status = "Manifest not saved: no resident signatures";
				return false;
			}
			if (!force && g_state.manifest_valid && !g_state.manifest_expected_signatures.empty())
			{
				const float retained = manifest_coverage_ratio();
				if (g_state.capture_degraded && retained < k_manifest_degraded_ratio)
				{
					g_state.manifest_status = "Previous manifest preserved: current capture is degraded";
					return false;
				}
			}
			std::error_code ec;
			std::filesystem::create_directories(g_state.manifest_path.parent_path(), ec);
			const auto temporary = g_state.manifest_path.string() + ".tmp";
			std::ofstream file(temporary, std::ios::trunc);
			if (!file.is_open())
			{
				g_state.manifest_status = "Manifest save failed: cannot open output";
				return false;
			}
			file << "# L4D2 RTX experimental static-map bake manifest V20.8\n";
			file << "format=" << k_manifest_format << "\nmap=" << g_state.map_key
				<< "\nprofile=" << static_cast<unsigned>(g_state.active_profile) << '\n';
			file << "bsp_size=" << g_state.bsp_file_size << "\nbsp_stamp="
				<< g_state.bsp_write_stamp << '\n';
			file << "bsp_nodes=" << g_state.bsp_nodes << "\nbsp_leafs="
				<< g_state.bsp_leafs << '\n';
			file << "resident_records=" << signatures.size() << "\nsubmitted="
				<< g_state.submitted << '\n';
			for (const auto signature : signatures)
			{
				file << "signature=" << std::hex << std::setw(16) << std::setfill('0')
					<< signature << std::dec << "\n";
			}
			file.close();
			if (!file)
			{
				g_state.manifest_status = "Manifest save failed while writing";
				return false;
			}
			std::filesystem::remove(g_state.manifest_path, ec);
			ec.clear();
			std::filesystem::rename(temporary, g_state.manifest_path, ec);
			if (ec)
			{
				ec.clear();
				std::filesystem::copy_file(temporary, g_state.manifest_path,
					std::filesystem::copy_options::overwrite_existing, ec);
				if (!ec) std::filesystem::remove(temporary, ec);
			}
			if (ec)
			{
				g_state.manifest_status = "Manifest save failed during atomic replace";
				return false;
			}
			g_state.manifest_dirty = false;
			g_state.last_manifest_save_frame = g_state.frame;
			g_state.manifest_status = "Saved " + std::to_string(signatures.size()) +
				" resident signatures";
			return true;
		}

		bool transform_is_identity(const D3DXMATRIX& matrix, const float epsilon = 0.0005f)
		{
			for (std::uint32_t row = 0u; row < 4u; ++row)
			{
				for (std::uint32_t column = 0u; column < 4u; ++column)
				{
					const float expected = row == column ? 1.0f : 0.0f;
					if (!std::isfinite(matrix.m[row][column]) ||
						std::abs(matrix.m[row][column] - expected) > epsilon) {
						return false;
					}
				}
			}
			return true;
		}

		DWORD float_bits(const float value)
		{
			DWORD result = 0u;
			static_assert(sizeof(result) == sizeof(value));
			std::memcpy(&result, &value, sizeof(result));
			return result;
		}

		void set_float_state(IDirect3DDevice9* device, prim_fvf_context& context,
			const D3DRENDERSTATETYPE state_id, const float value)
		{
			context.save_rs(device, state_id);
			device->SetRenderState(state_id, float_bits(value));
		}

		bool contains_any(const std::string_view value, const std::initializer_list<std::string_view> tokens)
		{
			for (const auto token : tokens)
			{
				if (value.find(token) != std::string_view::npos) {
					return true;
				}
			}
			return false;
		}

		bool primitive_supported(const D3DPRIMITIVETYPE type)
		{
			return type == D3DPT_TRIANGLELIST || type == D3DPT_TRIANGLESTRIP || type == D3DPT_TRIANGLEFAN;
		}

		bool material_is_water(const prim_fvf_context& context)
		{
			return context.modifiers.as_water ||
				context.info.shader_name.contains("Water") ||
				context.info.shader_name.contains("water");
		}

		bool material_is_safe(IDirect3DDevice9* device, const prim_fvf_context& context, const bool sky3d)
		{
			if (context.info.material_name.empty() || context.info.shader_name.empty()) {
				return false;
			}

			const auto material = context.info.material_name;
			const auto shader = context.info.shader_name;
			const bool water = material_is_water(context);

			// Xorxor water is a dynamic two-draw compatibility conversion. It must
			// never enter persistent static capture or DrawWorldLists coverage,
			// otherwise the original and secondary layers can be suppressed after
			// warm-up. Treat every water pass as unsafe for static ownership.
			if (water)
			{
				xorxor_water::note_static_cache_bypass();
				return false;
			}

			if (contains_any(material, {
				"skybox/", "tools/toolssky", "particle/", "particles/", "sprites/", "vgui/", "hud/", "decals/",
				"effects/", "cable/", "rope/", "models/weapons/", "models/survivors/",
				"models/infected/", "models/v_", "models/w_" })) {
				return false;
			}

			if (contains_any(shader, {
				"Sprite", "sprite", "Particle", "particle", "Cable", "Rope", "Shadow",
				"Refract", "EyeRefract", "Teeth", "Skin", "Infected", "Bik", "Sky", "sky" })) {
				return false;
			}

			DWORD alpha_blend = FALSE;
			DWORD z_write = TRUE;
			device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alpha_blend);
			device->GetRenderState(D3DRS_ZWRITEENABLE, &z_write);

			if (alpha_blend != FALSE) {
				return false;
			}
			if (z_write == FALSE) {
				return false;
			}

			// VIEW_3DSKY commonly contains Black/Unlit vista geometry. Keep it only
			// when it is depth-writing opaque geometry; the checks above already
			// exclude overlays, particles and screen-space effects.
			(void)sky3d;
			return true;
		}

		std::uint64_t make_draw_key(IDirect3DDevice9* device, const prim_fvf_context& context, const draw_arguments& args, const bool sky3d)
		{
			std::uint64_t hash = 14695981039346656037ull;
			const auto mesh_address = reinterpret_cast<std::uintptr_t>(g_current_mesh);
			hash = hash_value(hash, mesh_address);
			if (g_current_mesh)
			{
				const auto vertex_buffer = reinterpret_cast<std::uintptr_t>(g_current_mesh->m_pVertexBuffer);
				const auto index_buffer = reinterpret_cast<std::uintptr_t>(g_current_mesh->m_pIndexBuffer);
				hash = hash_value(hash, vertex_buffer);
				hash = hash_value(hash, index_buffer);
				hash = hash_value(hash, g_current_mesh->m_VertexFormat);
				hash = hash_value(hash, g_current_mesh->m_FirstIndex);
			}
			hash = hash_value(hash, args.type);
			hash = hash_value(hash, args.base_vertex_index);
			hash = hash_value(hash, args.min_vertex_index);
			hash = hash_value(hash, args.num_vertices);
			hash = hash_value(hash, args.start_index);
			hash = hash_value(hash, args.primitive_count);
			hash = hash_value(hash, sky3d);
			if (sky3d) hash = hash_value(hash, main_module::sky3d_payload_signature());
			hash = hash_string(hash, context.info.material_name);
			hash = hash_string(hash, context.info.shader_name);
			hash = hash_transform(hash, context.info.buffer_state.m_Transform[0]);
			if (g_model_scope.active)
			{
				hash = hash_value(hash, g_model_scope.model_hash);
				hash = hash_transform(hash, g_model_scope.transform);
			}

			// Animated texture proxies and scrolling texture matrices must never be
			// frozen into the resident scene. Their changing bindings become part of
			// the stability key, so they cannot reach the multi-frame threshold.
			for (std::uint32_t stage = 0u; stage < 4u; ++stage)
			{
				IDirect3DBaseTexture9* texture = nullptr;
				if (SUCCEEDED(device->GetTexture(stage, &texture)))
				{
					const auto texture_address = reinterpret_cast<std::uintptr_t>(texture);
					hash = hash_value(hash, texture_address);
					if (texture) {
						texture->Release();
					}
				}
			}

			D3DXMATRIX texture_transform = {};
			if (SUCCEEDED(device->GetTransform(D3DTS_TEXTURE0, &texture_transform))) {
				hash = hash_transform(hash, texture_transform);
			}
			DWORD texture_factor = 0u;
			if (SUCCEEDED(device->GetRenderState(D3DRS_TEXTUREFACTOR, &texture_factor))) {
				hash = hash_value(hash, texture_factor);
			}
			return hash;
		}

		void transition_to(const phase next)
		{
			if (g_state.current == next) {
				return;
			}
			g_state.current = next;
			g_state.phase_start_frame = g_state.frame;
			game::console();
			printf("[STATIC SCENE] phase -> %s (map=%s records=%zu)\n",
				phase_name(next), g_state.map_name.c_str(), g_state.records.size());
		}

		bool is_static_model_name(const std::string_view model_name)
		{
			if (model_name.empty()) {
				return false;
			}
			return !contains_any(model_name, {
				"models/survivors/", "models/infected/", "models/weapons/", "models/v_", "models/w_",
				"models/props_junk/gascan", "ragdoll", "arms", "hands", "viewmodel", "anim_" });
		}

		std::uint64_t avalanche64(std::uint64_t value)
		{
			value ^= value >> 33u;
			value *= 0xff51afd7ed558ccdull;
			value ^= value >> 33u;
			value *= 0xc4ceb9fe1a85ec53ull;
			value ^= value >> 33u;
			return value;
		}

		std::int32_t quantize_water_z(const float value)
		{
			if (!std::isfinite(value)) {
				return std::numeric_limits<std::int32_t>::min();
			}
			const double scaled = static_cast<double>(value) * 8.0;
			if (scaled <= static_cast<double>(std::numeric_limits<std::int32_t>::min())) {
				return std::numeric_limits<std::int32_t>::min();
			}
			if (scaled >= static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
				return std::numeric_limits<std::int32_t>::max();
			}
			return static_cast<std::int32_t>(std::llround(scaled));
		}

		std::uint32_t validation_interval(const world_pass_record& pass)
		{
			return pass.view == static_cast<std::uint32_t>(VIEW_3DSKY)
				? k_sky_pass_validation_interval
				: k_main_pass_validation_interval;
		}

		std::uint64_t make_world_pass_key(const int view, const unsigned int flags,
			const std::uint32_t ordinal, const float water_z)
		{
			std::uint64_t hash = 14695981039346656037ull;
			const auto water_bucket = quantize_water_z(water_z);
			hash = hash_value(hash, view);
			hash = hash_value(hash, flags);
			hash = hash_value(hash, ordinal);
			hash = hash_value(hash, water_bucket);
			return hash;
		}

		void note_world_pass_unsafe()
		{
			if (g_world_pass_scope.active && !g_world_pass_scope.bypassed) {
				++g_world_pass_scope.unsafe_draws;
			}
		}

		void begin_world_pass_primitive()
		{
			if (g_world_pass_scope.active && !g_world_pass_scope.bypassed) {
				++g_world_pass_scope.draws;
			}
		}

		void note_world_pass_safe_draw(const std::uint64_t draw_key, const bool persistent, const bool pending)
		{
			if (!g_world_pass_scope.active || g_world_pass_scope.bypassed) {
				return;
			}

			++g_world_pass_scope.safe_draws;
			if (persistent) {
				++g_world_pass_scope.persistent_draws;
			}
			if (pending) {
				++g_world_pass_scope.pending_draws;
			}
			const auto mixed = avalanche64(draw_key);
			g_world_pass_scope.fingerprint_xor ^= mixed;
			g_world_pass_scope.fingerprint_sum += mixed;
		}

		void mark_world_pass_covered(world_pass_record& pass)
		{
			if (pass.covered) {
				return;
			}
			pass.covered = true;
			pass.next_validation_frame = g_state.frame + validation_interval(pass);
			game::console();
			printf("[STATIC SCENE] DrawWorldLists pass covered: view=%u flags=0x%08X ordinal=%u draws=%u validationEvery=%u.\n",
				pass.view, pass.flags, pass.ordinal, pass.baseline_draws, validation_interval(pass));
		}

		void invalidate_world_pass(world_pass_record& pass, const world_pass_scope& scope)
		{
			if (pass.covered) {
				++g_state.world_pass_invalidations;
				++pass.validation_failures;
				game::console();
				printf("[STATIC SCENE] DrawWorldLists pass fallback: view=%u flags=0x%08X ordinal=%u draws=%u unsafe=%u pending=%u.\n",
					pass.view, pass.flags, pass.ordinal, scope.draws, scope.unsafe_draws, scope.pending_draws);
			}
			pass.covered = false;
			pass.safe_confirmations = 0u;
			pass.next_validation_frame = g_state.frame + 1u;
		}

		bool begin_world_list_pass(const unsigned int flags, const float water_z)
		{
			g_world_pass_scope = {};
			if (!g_state.enabled || g_state.current == phase::disabled) {
				return false;
			}

			const int view = game::get_viewid();
			const bool sky3d = view == VIEW_3DSKY;
			if (view != VIEW_MAIN && !sky3d) {
				return false;
			}
			if (sky3d && !g_state.sky3d_fusion) {
				return false;
			}

			const std::uint32_t ordinal = sky3d ? g_state.sky_pass_ordinal++ : g_state.main_pass_ordinal++;
			const auto key = make_world_pass_key(view, flags, ordinal, water_z);
			auto [it, inserted] = g_state.world_passes.try_emplace(key);
			auto& pass = it->second;
			if (inserted)
			{
				pass.view = static_cast<std::uint32_t>(view);
				pass.flags = flags;
				pass.ordinal = ordinal;
				pass.water_bucket = quantize_water_z(water_z);
				pass.first_seen_frame = g_state.frame;
				pass.next_validation_frame = g_state.frame;
			}
			pass.last_seen_frame = g_state.frame;

			g_world_pass_scope.active = true;
			g_world_pass_scope.key = key;
			++g_state.world_pass_calls;

			const bool bypass_enabled = sky3d ? g_state.sky_pass_bypass : g_state.world_pass_bypass;
			if (g_state.current != phase::resident || !bypass_enabled || !pass.covered) {
				++pass.executions;
				return false;
			}

			const bool validation_due = g_state.force_pass_validation || g_state.frame >= pass.next_validation_frame;
			if (validation_due)
			{
				g_state.force_pass_validation = false;
				g_world_pass_scope.validation_sample = true;
				++g_state.world_pass_validation_runs;
				++pass.executions;
				return false;
			}

			g_world_pass_scope.bypassed = true;
			++g_state.world_pass_skips;
			++pass.skips;
			pass.estimated_draws_avoided += pass.baseline_draws;
			g_state.estimated_draws_avoided += pass.baseline_draws;
			return true;
		}

		void end_world_list_pass()
		{
			if (!g_world_pass_scope.active) {
				return;
			}

			const auto scope = g_world_pass_scope;
			g_world_pass_scope = {};
			if (scope.bypassed) {
				return;
			}

			auto it = g_state.world_passes.find(scope.key);
			if (it == g_state.world_passes.end()) {
				return;
			}
			auto& pass = it->second;
			++pass.observations;

			const bool no_unsafe = scope.unsafe_draws == 0u && scope.pending_draws == 0u;
			const bool all_draws_safe = scope.safe_draws == scope.draws;
			const bool all_draws_persistent = scope.persistent_draws == scope.draws;
			const bool safe_nonempty_execution = scope.draws > 0u && no_unsafe && all_draws_safe && all_draws_persistent;
			const bool safe_validation_execution = no_unsafe && all_draws_safe && all_draws_persistent;

			if (g_state.current == phase::capturing)
			{
				if (!safe_nonempty_execution)
				{
					pass.safe_confirmations = 0u;
					pass.covered = false;
					return;
				}

				const bool same_as_baseline =
					pass.baseline_draws == scope.draws &&
					pass.baseline_fingerprint_xor == scope.fingerprint_xor &&
					pass.baseline_fingerprint_sum == scope.fingerprint_sum;
				if (pass.safe_confirmations == 0u || !same_as_baseline)
				{
					pass.safe_confirmations = 1u;
					pass.baseline_draws = scope.draws;
					pass.baseline_fingerprint_xor = scope.fingerprint_xor;
					pass.baseline_fingerprint_sum = scope.fingerprint_sum;
				}
				else {
					++pass.safe_confirmations;
				}

				if (pass.safe_confirmations >= k_pass_capture_confirmations) {
					mark_world_pass_covered(pass);
				}
				return;
			}

			if (g_state.current != phase::resident) {
				return;
			}

			if (scope.validation_sample)
			{
				if (!safe_validation_execution)
				{
					invalidate_world_pass(pass, scope);
					return;
				}
				++pass.validation_successes;
				pass.next_validation_frame = g_state.frame + validation_interval(pass);
				return;
			}

			// A pass that was not fully proven during initial capture remains on the
			// ordinary Source path until several clean resident executions confirm it.
			if (!pass.covered)
			{
				if (!safe_nonempty_execution)
				{
					pass.safe_confirmations = 0u;
					return;
				}
				pass.baseline_draws = std::max(pass.baseline_draws, scope.draws);
				++pass.safe_confirmations;
				if (pass.safe_confirmations >= k_pass_recovery_confirmations) {
					mark_world_pass_covered(pass);
				}
			}
		}

		using draw_world_lists_fn = void(__fastcall*)(IRender*, void*, void*, void*, unsigned int, float);
		void __fastcall draw_world_lists_detour(IRender* renderer, void* edx, void* material_context,
			void* world_list, const unsigned int flags, const float water_z)
		{
			const bool skip = begin_world_list_pass(flags, water_z);
			if (!skip)
			{
				g_engine_renderer_table.original<draw_world_lists_fn>(k_draw_world_lists_vtable_index)(
					renderer, edx, material_context, world_list, flags, water_z);
			}
			end_world_list_pass();
		}

		bool has_persistent_submission(const bool sky_only = false)
		{
			for (const auto& [key, entry] : g_state.records)
			{
				(void)key;
				if (entry.persistent_submitted && (!sky_only || entry.sky3d)) {
					return true;
				}
			}
			return false;
		}
	}

	void on_map_load(const char* map_name)
	{
		const bool acknowledged = g_state.experimental_acknowledged;
		const bool requested_enabled = g_state.requested_enabled;
		const auto requested_profile = g_state.requested_profile;
		const bool manifest_auto_save = g_state.manifest_auto_save;
		const bool manifest_warm_start = g_state.manifest_warm_start;
		g_state = {};
		g_state.experimental_acknowledged = acknowledged;
		g_state.requested_enabled = requested_enabled;
		g_state.requested_profile = requested_profile;
		g_state.active_profile = requested_profile;
		g_state.manifest_auto_save = manifest_auto_save;
		g_state.manifest_warm_start = manifest_warm_start;
		g_state.enabled = acknowledged && requested_enabled;
		if (g_state.enabled) apply_profile_flags(g_state, requested_profile);
		g_state.reload_required = false;
		g_state.map_name = map_name ? map_name : "unknown";
		g_state.frame = current_source_frame();
		g_state.last_new_record_frame = g_state.frame;
		g_state.current = g_state.enabled ? phase::warmup : phase::disabled;
		g_state.phase_start_frame = g_state.frame;
		refresh_map_identity();
		load_manifest_internal();
		g_current_mesh = nullptr; g_model_scope = {}; g_world_pass_scope = {};
		game::console();
		printf("[STATIC SCENE V20.8] map=%s experimental=%d enabled=%d profile=%s cache=%s expected=%zu BSP=%u nodes/%u leafs.\n",
			g_state.map_name.c_str(), g_state.experimental_acknowledged ? 1 : 0, g_state.enabled ? 1 : 0,
			profile_name(g_state.requested_profile), g_state.manifest_status.c_str(),
			g_state.manifest_expected_signatures.size(), g_state.bsp_nodes, g_state.bsp_leafs);
	}

	void on_map_unload()
	{
		if (g_state.current == phase::resident && !g_state.capture_degraded &&
			g_state.manifest_auto_save && g_state.manifest_dirty && !g_state.records.empty()) {
			write_manifest_internal(false);
		}
		g_state.records.clear();
		g_state.world_passes.clear();
		g_state.map_name.clear();
		g_state.current = phase::disabled;
		g_current_mesh = nullptr;
		g_model_scope = {};
		g_world_pass_scope = {};
	}

	void on_frame()
	{
		process_pending_ui_actions();

		const auto source_frame = current_source_frame();
		if (source_frame != g_state.frame)
		{
			g_state.main_pass_ordinal = 0u;
			g_state.sky_pass_ordinal = 0u;
		}
		g_state.frame = source_frame;
		if (!g_state.enabled)
		{
			g_state.current = phase::disabled;
			return;
		}
		if (g_state.current == phase::disabled) {
			transition_to(phase::warmup);
		}

		const auto phase_age = g_state.frame - g_state.phase_start_frame;
		if (g_state.current == phase::warmup && phase_age >= k_warmup_frames)
		{
			transition_to(phase::capturing);
		}
		else if (g_state.current == phase::capturing)
		{
			prune_stale_records();
			const auto quiet_frames = g_state.frame - g_state.last_new_record_frame;
			const float stability = g_state.records.empty() ? 0.0f :
				static_cast<float>(stable_record_count()) / static_cast<float>(g_state.records.size());
			const float coverage = manifest_coverage_ratio();
			const bool cache_ready = !g_state.manifest_valid || g_state.manifest_expected_signatures.empty() || coverage >= k_manifest_ready_ratio;
			const bool settled = phase_age >= k_min_capture_frames && quiet_frames >= k_settle_frames &&
				g_state.submitted > 0u && stability >= 0.90f && cache_ready;
			const bool extend_for_cache = g_state.manifest_valid && !cache_ready && phase_age < k_extended_capture_frames;
			if (settled || (phase_age >= k_max_capture_frames && !extend_for_cache) || phase_age >= k_extended_capture_frames)
			{
				g_state.capture_degraded = !settled || g_state.submitted == 0u || stability < 0.75f ||
					(g_state.manifest_valid && !g_state.manifest_expected_signatures.empty() && coverage < k_manifest_degraded_ratio);
				transition_to(phase::resident);
				if (g_state.manifest_auto_save && !g_state.capture_degraded) {
					write_manifest_internal(false);
				} else if (g_state.capture_degraded) {
					g_state.manifest_status = "Degraded Resident capture was not auto-saved";
				}
			}
		}
		else if (g_state.current == phase::resident && !g_state.capture_degraded &&
			g_state.manifest_auto_save && g_state.manifest_dirty &&
			g_state.frame >= g_state.last_manifest_save_frame + k_manifest_resave_interval)
		{
			write_manifest_internal(false);
		}
	}

	void begin_model_draw(const ModelRenderInfo_t& info, const bool source_static_candidate)
	{
		g_model_scope = {};
		g_model_scope.active = true;
		g_model_scope.source_static_candidate = source_static_candidate && info.pModel && is_static_model_name(info.pModel->szPathName);
		if (info.pModel) {
			g_model_scope.model_hash = utils::string_hash32(info.pModel->szPathName);
		}
		if (info.pModelToWorld) {
			std::memset(&g_model_scope.transform, 0, sizeof(g_model_scope.transform));
			for (std::uint32_t row = 0u; row < 3u; ++row)
			{
				for (std::uint32_t column = 0u; column < 4u; ++column) {
					g_model_scope.transform.m[row][column] = info.pModelToWorld->m_flMatVal[row][column];
				}
			}
			g_model_scope.transform.m[3][3] = 1.0f;
		}
	}

	void end_model_draw()
	{
		g_model_scope = {};
	}

	void on_pre_draw(CMeshDX8* mesh)
	{
		g_current_mesh = mesh;
	}

	void quarantine_xorxor_water()
	{
		const bool first_detection = !g_state.xorxor_water_quarantine;
		g_state.xorxor_water_quarantine = true;

		// Enforce this on every water draw. A profile may be activated after the
		// material was first observed and must not silently re-enable pass bypass.
		g_state.world_pass_bypass = false;
		g_state.sky_pass_bypass = false;
		g_state.force_pass_validation = true;

		if (first_detection)
		{
			g_state.world_passes.clear();
			game::console();
			printf("[STATIC SCENE] Xorxor water detected: DrawWorldLists pass bypass quarantined for this map.\n");
		}
	}

	void process_draw(IDirect3DDevice9* device, prim_fvf_context& context, const draw_arguments& args)
	{
		begin_world_pass_primitive();

		if (!device || !g_state.enabled || g_state.current == phase::disabled || context.modifiers.do_not_render ||
			!g_current_mesh || args.primitive_count == 0u || !primitive_supported(args.type))
		{
			note_world_pass_unsafe();
			return;
		}

		const auto view = game::get_viewid();
		const bool sky3d = view == VIEW_3DSKY;
		if (view != VIEW_MAIN && !sky3d)
		{
			note_world_pass_unsafe();
			return;
		}
		if (sky3d && !g_state.sky3d_fusion)
		{
			note_world_pass_unsafe();
			return;
		}
		if (sky3d)
		{
			auto* settings = game_settings::get();
			const auto max_age = settings
				? static_cast<std::uint64_t>(std::clamp(settings->sky3d_payload_max_age_frames.get_as<int>(), 1, 120))
				: 8u;
			if (!main_module::sky3d_payload_is_capture_eligible(max_age))
			{
				if (main_module::sky3d_payload_age_frames() == std::numeric_limits<std::uint64_t>::max())
					main_module::note_sky3d_missing_payload();
				else if (!main_module::sky3d_payload_is_fresh(max_age))
					main_module::note_sky3d_stale_payload();
				else
					main_module::note_sky3d_recovery_rejected();

				// Never seal or suppress the Source draw with stale sky-camera state.
				// The safe fallback is intentionally passive: Source keeps ownership of
				// this primitive until a fresh payload is observed.
				main_module::note_sky3d_safe_fallback();
				note_world_pass_unsafe();
				return;
			}
		}

		if (g_model_scope.active && !g_model_scope.source_static_candidate)
		{
			++g_state.rejected_dynamic;
			note_world_pass_unsafe();
			return;
		}
		if (const auto vertex_buffer = reinterpret_cast<IVertexBuffer*>(g_current_mesh->m_pVertexBuffer);
			vertex_buffer && vertex_buffer->vftable && vertex_buffer->vftable->IsDynamic(vertex_buffer))
		{
			++g_state.rejected_dynamic;
			note_world_pass_unsafe();
			return;
		}
		// Worldspawn and BSP displacements use identity object space. A transformed
		// non-model draw is normally a brush entity (door, lift, breakable, rotating
		// brush) and must stay dynamic even when it has not moved yet.
		if (!g_model_scope.active && !transform_is_identity(context.info.buffer_state.m_Transform[0]))
		{
			++g_state.rejected_dynamic;
			note_world_pass_unsafe();
			return;
		}
		if (!material_is_safe(device, context, sky3d))
		{
			++g_state.rejected_material;
			note_world_pass_unsafe();
			return;
		}

		++g_state.candidates;
		if (sky3d) {
			++g_state.sky_candidates;
			main_module::note_sky3d_static_candidate();
		}
		if (g_model_scope.active) ++g_state.model_candidates;

		const auto key = make_draw_key(device, context, args, sky3d);
		const auto stable_signature = make_stable_draw_signature(device, context, args, sky3d);
		auto [iterator, inserted] = g_state.records.try_emplace(key);
		auto& entry = iterator->second;
		if (inserted)
		{
			entry.stable_signature = stable_signature;
			entry.cache_expected = g_state.manifest_valid && g_state.manifest_expected_signatures.contains(stable_signature);
			entry.first_seen_frame = g_state.frame;
			entry.sky3d = sky3d;
			entry.model = g_model_scope.active;
			g_state.last_new_record_frame = g_state.frame;
		}
		if (inserted || entry.last_seen_frame != g_state.frame)
		{
			entry.consecutive_observations = !inserted && entry.last_seen_frame + 1u == g_state.frame
				? entry.consecutive_observations + 1u : 1u;
			++entry.observations;
		}
		entry.last_seen_frame = g_state.frame;

		// During warmup Source owns the draw completely. It is a valid static
		// candidate, but not yet a resident draw, so a containing world pass cannot
		// be bypassed at this point.
		if (g_state.current == phase::warmup)
		{
			note_world_pass_safe_draw(key, false, true);
			return;
		}

		const bool was_submitted_on_previous_frame =
			entry.persistent_submitted && entry.submitted_frame < g_state.frame;
		const std::uint32_t required_observations = entry.cache_expected && g_state.manifest_warm_start
			? k_cached_observations_required : k_uncached_observations_required;
		const bool stable = entry.consecutive_observations >= required_observations;
		if (stable && entry.stable_signature != 0u) {
			g_state.observed_stable_signatures.insert(entry.stable_signature);
		}
		if (!stable)
		{
			note_world_pass_safe_draw(key, false, true);
			return;
		}
		if (g_state.current == phase::resident && was_submitted_on_previous_frame)
		{
			context.modifiers.do_not_render = true;
			++g_state.skipped;
			if (sky3d) main_module::note_sky3d_source_suppressed();
			note_world_pass_safe_draw(key, true, false);
			return;
		}

		DWORD categories = 0u;
		device->GetRenderState(k_rs_categories, &categories);
		if (categories == k_uninitialized_render_state) {
			categories = 0u;
		}
		categories |= k_persistent_static_category;
		if (sky3d) {
			categories |= k_l4d2_sky3d_category;
		}
		context.save_rs(device, k_rs_categories);
		device->SetRenderState(k_rs_categories, categories);

		if (sky3d)
		{
			const auto* main = main_module::get();
			// A fresh payload was required above, so no generic scale fallback is
			// allowed here. The exact Source sky_camera transform is propagated.
			const float source_scale = static_cast<float>(main->m_sky3d_scale);
			const Vector source_origin = main->m_sky3d_origin;
			set_float_state(device, context, k_rs_sky_scale, source_scale);
			set_float_state(device, context, k_rs_sky_origin_x, source_origin.x);
			set_float_state(device, context, k_rs_sky_origin_y, source_origin.y);
			set_float_state(device, context, k_rs_sky_origin_z, source_origin.z);
		}

		entry.persistent_submitted = true;
		entry.submitted_frame = g_state.frame;
		g_state.manifest_dirty = true;
		++g_state.submitted;
		if (sky3d) main_module::note_sky3d_submitted_object();
		note_world_pass_safe_draw(key, true, false);
	}

	bool install_renderer_hooks()
	{
		if (g_engine_renderer_hook_installed) {
			return true;
		}

		auto* renderer = game::get_engine_renderer();
		if (!renderer || !renderer->vftable) {
			return false;
		}
		if (!g_engine_renderer_table.init(renderer)) {
			return false;
		}
		if (!g_engine_renderer_table.hook(
			reinterpret_cast<void*>(&draw_world_lists_detour), k_draw_world_lists_vtable_index)) {
			return false;
		}

		g_engine_renderer_hook_installed = true;
		game::console();
		printf("[STATIC SCENE] IRender::DrawWorldLists adaptive bypass hook installed.\n");
		return true;
	}

	void install_pass_hook_command()
	{
		game::console();
		if (!g_state.experimental_acknowledged || !g_state.enabled || g_state.current != phase::resident)
		{
			printf("[STATIC SCENE V20.8] Pass hook blocked: activate a profile and wait for the Resident phase first.\n");
			return;
		}
		if (g_engine_renderer_hook_installed)
		{
			printf("[STATIC SCENE] DrawWorldLists hook is already installed.\n");
			return;
		}
		if (!install_renderer_hooks())
		{
			printf("[STATIC SCENE] DrawWorldLists hook installation FAILED; pass bypass remains disabled.\n");
			g_state.world_pass_bypass = false;
			g_state.sky_pass_bypass = false;
			return;
		}
		printf("[STATIC SCENE] DrawWorldLists hook installed manually. Bypass flags remain unchanged.\n");
	}

	void toggle_world_pass_bypass()
	{
		game::console();
		if (!g_state.experimental_acknowledged || g_state.current != phase::resident)
		{
			printf("[STATIC SCENE V20.8] Main pass bypass requires an acknowledged resident experimental scene.\n");
			return;
		}
		if (g_state.xorxor_water_quarantine)
		{
			printf("[STATIC SCENE] Main pass bypass blocked: Xorxor water owns a dynamic dual-layer pass on this map.\n");
			return;
		}
		const bool requested = !g_state.world_pass_bypass;
		if (requested && !g_engine_renderer_hook_installed && !install_renderer_hooks())
		{
			printf("[STATIC SCENE] Cannot enable main DrawWorldLists bypass: hook installation failed.\n");
			g_state.world_pass_bypass = false;
			return;
		}
		g_state.world_pass_bypass = requested;
		printf("[STATIC SCENE] main DrawWorldLists bypass=%d hook=%d\n",
			g_state.world_pass_bypass ? 1 : 0, g_engine_renderer_hook_installed ? 1 : 0);
	}

	void toggle_sky_pass_bypass()
	{
		game::console();
		if (!g_state.experimental_acknowledged || g_state.current != phase::resident || !g_state.sky3d_fusion)
		{
			printf("[STATIC SCENE V20.8] Sky pass bypass requires a resident profile with Sky3D fusion.\n");
			return;
		}
		if (g_state.xorxor_water_quarantine)
		{
			printf("[STATIC SCENE] Sky pass bypass blocked: Xorxor water quarantine is active for this map.\n");
			return;
		}
		const bool requested = !g_state.sky_pass_bypass;
		if (requested && !g_engine_renderer_hook_installed && !install_renderer_hooks())
		{
			printf("[STATIC SCENE] Cannot enable Sky3D DrawWorldLists bypass: hook installation failed.\n");
			g_state.sky_pass_bypass = false;
			return;
		}
		g_state.sky_pass_bypass = requested;
		printf("[STATIC SCENE] Sky3D DrawWorldLists bypass=%d hook=%d\n",
			g_state.sky_pass_bypass ? 1 : 0, g_engine_renderer_hook_installed ? 1 : 0);
	}

	void validate_world_passes()
	{
		if (!g_state.experimental_acknowledged || g_state.current != phase::resident) {
			return;
		}
		g_state.force_pass_validation = true;
		for (auto& [key, pass] : g_state.world_passes)
		{
			(void)key;
			if (pass.covered) {
				pass.next_validation_frame = g_state.frame;
			}
		}
		game::console();
		printf("[STATIC SCENE] DrawWorldLists validation requested for the next covered pass calls.\n");
	}

	void world_pass_status()
	{
		game::console();
		printf("[STATIC SCENE PASS] mainBypass=%d skyBypass=%d waterQuarantine=%d hook=%d passes=%zu calls=%llu skips=%llu validations=%llu invalidations=%llu estimatedDIPsAvoided=%llu\n",
			g_state.world_pass_bypass ? 1 : 0,
			g_state.sky_pass_bypass ? 1 : 0,
			g_state.xorxor_water_quarantine ? 1 : 0,
			g_engine_renderer_hook_installed ? 1 : 0,
			g_state.world_passes.size(),
			static_cast<unsigned long long>(g_state.world_pass_calls),
			static_cast<unsigned long long>(g_state.world_pass_skips),
			static_cast<unsigned long long>(g_state.world_pass_validation_runs),
			static_cast<unsigned long long>(g_state.world_pass_invalidations),
			static_cast<unsigned long long>(g_state.estimated_draws_avoided));

		for (const auto& [key, pass] : g_state.world_passes)
		{
			printf("  key=%016llX view=%u flags=0x%08X ord=%u water=%d covered=%d confirms=%u baseDraws=%u exec=%llu skips=%llu valid=%u/%u next=%u\n",
				static_cast<unsigned long long>(key), pass.view, pass.flags, pass.ordinal,
				pass.water_bucket, pass.covered ? 1 : 0, pass.safe_confirmations,
				pass.baseline_draws,
				static_cast<unsigned long long>(pass.executions),
				static_cast<unsigned long long>(pass.skips),
				pass.validation_successes, pass.validation_failures, pass.next_validation_frame);
		}
	}

	const char* experimental_profile_name(const experimental_profile value)
	{
		return profile_name(value);
	}

	const char* cache_quality_name(const cache_quality value)
	{
		return quality_name(value);
	}

	void set_manifest_auto_save(const bool enabled)
	{
		g_state.manifest_auto_save = enabled;
		if (enabled && g_state.current == phase::resident &&
			!g_state.capture_degraded && g_state.manifest_dirty) {
			write_manifest_internal(false);
		}
	}

	void set_manifest_warm_start(const bool enabled)
	{
		g_state.manifest_warm_start = enabled;
	}

	void save_manifest()
	{
		game::console();
		if (g_state.current != phase::resident)
		{
			g_state.manifest_status = "Manifest not saved: capture has not reached Resident";
			printf("[STATIC SCENE V20.8] %s\n", g_state.manifest_status.c_str());
			return;
		}
		const bool saved = write_manifest_internal(true);
		printf("[STATIC SCENE V20.8] %s\n",
			saved ? "Manifest saved." : g_state.manifest_status.c_str());
	}

	void reload_manifest()
	{
		load_manifest_internal();
		for (auto& [key, entry] : g_state.records)
		{
			(void)key;
			entry.cache_expected = g_state.manifest_valid &&
				g_state.manifest_expected_signatures.contains(entry.stable_signature);
		}
		game::console();
		printf("[STATIC SCENE V20.8] %s\n", g_state.manifest_status.c_str());
	}

	void clear_manifest()
	{
		if (g_state.manifest_path.empty())
		{
			g_state.manifest_status = "Manifest clear skipped: no map/profile path";
			return;
		}
		std::error_code ec;
		std::filesystem::remove(g_state.manifest_path, ec);
		g_state.manifest_loaded = false;
		g_state.manifest_valid = false;
		g_state.manifest_stale = false;
		g_state.manifest_expected_signatures.clear();
		for (auto& [key, entry] : g_state.records)
		{
			(void)key;
			entry.cache_expected = false;
		}
		g_state.manifest_status = ec
			? "Manifest clear failed: " + ec.message()
			: "Manifest cleared";
		game::console();
		printf("[STATIC SCENE V20.8] %s\n", g_state.manifest_status.c_str());
	}

	void export_audit_report()
	{
		std::error_code ec;
		std::filesystem::create_directories(audit_root_path(), ec);
		const auto path = audit_root_path() /
			(g_state.map_key + "." + profile_cache_tag(g_state.active_profile) + "_v208_audit.txt");
		std::ofstream file(path, std::ios::trunc);
		if (!file.is_open())
		{
			game::console();
			printf("[STATIC SCENE V20.8] Audit export failed.\n");
			return;
		}
		const auto matched = matched_manifest_signatures();
		file << "L4D2 RTX Static Map Baking V20.8 audit\n";
		file << "map=" << g_state.map_name << "\nprofile=" << profile_name(g_state.active_profile)
			<< "\nphase=" << phase_name(g_state.current) << '\n';
		file << "quality=" << quality_name(current_quality())
			<< "\ncompleteness=" << completeness_score() << '\n';
		file << "bsp_nodes=" << g_state.bsp_nodes << "\nbsp_leafs=" << g_state.bsp_leafs << '\n';
		file << "records=" << g_state.records.size() << "\nstable_records=" << stable_record_count()
			<< "\nsubmitted=" << g_state.submitted << '\n';
		file << "manifest_status=" << g_state.manifest_status
			<< "\nmanifest_expected=" << g_state.manifest_expected_signatures.size()
			<< "\nmanifest_matched=" << matched << '\n';
		file << "missing_signatures:\n";
		for (const auto signature : g_state.manifest_expected_signatures)
		{
			if (!g_state.observed_stable_signatures.contains(signature)) {
				file << "  " << std::hex << signature << std::dec << '\n';
			}
		}
		file << "new_signatures:\n";
		for (const auto signature : g_state.observed_stable_signatures)
		{
			if (!g_state.manifest_expected_signatures.contains(signature)) {
				file << "  " << std::hex << signature << std::dec << '\n';
			}
		}
		file.close();
		game::console();
		printf("[STATIC SCENE V20.8] Audit exported: %s\n", path.string().c_str());
	}

	void manifest_status()
	{
		game::console();
		const auto matched = matched_manifest_signatures();
		printf("[STATIC SCENE CACHE V20.8] path=%s status=%s loaded=%d valid=%d stale=%d autoSave=%d warmStart=%d expected=%zu matched=%llu missing=%llu new=%llu coverage=%.1f%% dirty=%d\n",
			g_state.manifest_path.string().c_str(), g_state.manifest_status.c_str(),
			g_state.manifest_loaded ? 1 : 0, g_state.manifest_valid ? 1 : 0,
			g_state.manifest_stale ? 1 : 0, g_state.manifest_auto_save ? 1 : 0,
			g_state.manifest_warm_start ? 1 : 0, g_state.manifest_expected_signatures.size(),
			static_cast<unsigned long long>(matched),
			static_cast<unsigned long long>(g_state.manifest_expected_signatures.size() > matched
				? g_state.manifest_expected_signatures.size() - matched : 0u),
			static_cast<unsigned long long>(g_state.observed_stable_signatures.size() > matched
				? g_state.observed_stable_signatures.size() - matched : 0u),
			manifest_coverage_ratio() * 100.0f, g_state.manifest_dirty ? 1 : 0);
	}

	void enqueue_ui_action(const ui_action action, const bool value)
	{
		std::scoped_lock lock(g_ui_action_mutex);

		if (!ui_action_is_runtime_available_internal(action))
		{
			++g_ui_action_status.blocked;
			g_ui_action_status.runtime_quarantine = true;
			g_ui_action_status.last_action = ui_action_name(action);
			g_ui_action_status.last_result = ui_action_block_reason_internal(action);
			g_ui_action_status.pending_count = static_cast<std::uint32_t>(g_ui_actions.size());
			g_ui_action_status.pending = !g_ui_actions.empty();
			game::console();
			printf("[STATIC SCENE UI] Runtime action '%s' blocked by V21.3 safety quarantine.\n", ui_action_name(action));
			return;
		}

		for (auto it = g_ui_actions.begin(); it != g_ui_actions.end();)
		{
			if (ui_action_replaces_pending(it->action, action)) it = g_ui_actions.erase(it);
			else ++it;
		}

		if (g_ui_actions.size() >= k_max_pending_ui_actions)
		{
			g_ui_actions.pop_front();
			++g_ui_action_status.failed;
			g_ui_action_status.last_result = "Oldest deferred action dropped because the queue was full";
		}

		g_ui_actions.push_back({ action, value });
		++g_ui_action_status.queued;
		g_ui_action_status.pending = true;
		g_ui_action_status.pending_count = static_cast<std::uint32_t>(g_ui_actions.size());
		g_ui_action_status.last_action = ui_action_name(action);
		g_ui_action_status.last_result = "Queued for the next RenderView boundary";
	}

	ui_action_status get_ui_action_status()
	{
		std::scoped_lock lock(g_ui_action_mutex);
		auto result = g_ui_action_status;
		result.runtime_quarantine = k_runtime_mutation_quarantine;
		result.pending_count = static_cast<std::uint32_t>(g_ui_actions.size());
		result.pending = !g_ui_actions.empty();
		return result;
	}

	bool ui_action_runtime_available(const ui_action action)
	{
		return ui_action_is_runtime_available_internal(action);
	}

	const char* ui_action_runtime_block_reason(const ui_action action)
	{
		return ui_action_block_reason_internal(action);
	}

	bool experimental_acknowledged()
	{
		return g_state.experimental_acknowledged;
	}

	void set_experimental_acknowledged(const bool acknowledged)
	{
		if (g_state.experimental_acknowledged == acknowledged) {
			return;
		}

		g_state.experimental_acknowledged = acknowledged;
		game::console();
		if (acknowledged)
		{
			printf("[STATIC SCENE V20.8] Experimental static-map baking unlocked for this process only.\n");
			return;
		}

		g_state.requested_enabled = false;
		if (has_persistent_submission())
		{
			g_state.reload_required = true;
			printf("[STATIC SCENE V20.8] Experimental acknowledgement removed. Resident instances remain active until map reload; baking is disabled for the next map.\n");
			return;
		}

		g_state.enabled = false;
		g_state.reload_required = false;
		g_state.world_pass_bypass = false;
		g_state.sky_pass_bypass = false;
		rebuild();
		printf("[STATIC SCENE V20.8] Experimental static-map baking locked and disabled.\n");
	}

	bool activate_experimental_profile(const experimental_profile value)
	{
		game::console();
		if (!g_state.experimental_acknowledged)
		{
			printf("[STATIC SCENE V20.8] Activation blocked. Acknowledge the experimental warning in F5 -> Experimental first.\n");
			return false;
		}

		g_state.requested_enabled = true;
		g_state.requested_profile = value;
		if (has_persistent_submission())
		{
			g_state.reload_required = true;
			printf("[STATIC SCENE V20.8] Profile '%s' scheduled for the next map. Current resident scene cannot be rebuilt in place.\n",
				profile_name(value));
			return false;
		}

		apply_profile_flags(g_state, value);
		g_state.active_profile = value;
		g_state.enabled = true;
		g_state.reload_required = false;
		refresh_map_identity();
		load_manifest_internal();
		rebuild();
		printf("[STATIC SCENE V20.8] Experimental profile '%s' activated. Reloading the map before final validation is recommended.\n",
			profile_name(value));
		return true;
	}

	void disable_experimental()
	{
		game::console();
		g_state.requested_enabled = false;
		if (has_persistent_submission())
		{
			g_state.reload_required = true;
			printf("[STATIC SCENE V20.8] Disable scheduled. Reload/change map to clear resident instances safely.\n");
			return;
		}

		g_state.enabled = false;
		g_state.reload_required = false;
		g_state.sky3d_fusion = false;
		g_state.full_visibility_capture = false;
		g_state.model_info_classifier = false;
		g_state.world_pass_bypass = false;
		g_state.sky_pass_bypass = false;
		rebuild();
		printf("[STATIC SCENE V20.8] Experimental static-map baking disabled.\n");
	}

	void reset_experimental_defaults()
	{
		g_state.requested_profile = experimental_profile::safe_preview;
		g_state.requested_enabled = false;
		disable_experimental();
	}

	status_snapshot snapshot()
	{
		status_snapshot result;
		result.acknowledged = g_state.experimental_acknowledged;
		result.enabled = g_state.enabled;
		result.requested_enabled = g_state.requested_enabled;
		result.reload_required = g_state.reload_required;
		result.persistent_submission = has_persistent_submission();
		result.sky3d_fusion = g_state.sky3d_fusion;
		result.full_visibility_capture = g_state.full_visibility_capture;
		result.model_info_classifier = g_state.model_info_classifier;
		result.renderer_hook_installed = g_engine_renderer_hook_installed;
		result.world_pass_bypass = g_state.world_pass_bypass;
		result.sky_pass_bypass = g_state.sky_pass_bypass;
		result.xorxor_water_quarantine = g_state.xorxor_water_quarantine;
		result.manifest_loaded = g_state.manifest_loaded;
		result.manifest_valid = g_state.manifest_valid;
		result.manifest_stale = g_state.manifest_stale;
		result.manifest_dirty = g_state.manifest_dirty;
		result.manifest_auto_save = g_state.manifest_auto_save;
		result.manifest_warm_start = g_state.manifest_warm_start;
		result.capture_degraded = g_state.capture_degraded;
		result.current = g_state.current;
		result.active_profile = g_state.active_profile;
		result.requested_profile = g_state.requested_profile;
		result.quality = current_quality();
		result.map_name = g_state.map_name;
		result.manifest_path = g_state.manifest_path.string();
		result.manifest_status = g_state.manifest_status;
		result.frame = g_state.frame;
		result.phase_age = g_state.frame >= g_state.phase_start_frame ? g_state.frame - g_state.phase_start_frame : 0u;
		result.quiet_frames = g_state.frame >= g_state.last_new_record_frame ? g_state.frame - g_state.last_new_record_frame : 0u;
		result.bsp_nodes = g_state.bsp_nodes;
		result.bsp_leafs = g_state.bsp_leafs;
		result.completeness = completeness_score();
		result.manifest_coverage = manifest_coverage_ratio();
		result.world_pass_coverage = world_pass_coverage_ratio();
		result.records = g_state.records.size();
		result.stable_records = stable_record_count();
		result.expired_records = g_state.expired_records;
		result.candidates = g_state.candidates;
		result.submitted = g_state.submitted;
		result.skipped = g_state.skipped;
		result.rejected_dynamic = g_state.rejected_dynamic;
		result.rejected_material = g_state.rejected_material;
		result.sky_candidates = g_state.sky_candidates;
		result.model_candidates = g_state.model_candidates;
		result.world_passes = g_state.world_passes.size();
		for (const auto& [key, pass] : g_state.world_passes) { (void)key; if (pass.covered) ++result.covered_world_passes; }
		result.world_pass_skips = g_state.world_pass_skips;
		result.estimated_draws_avoided = g_state.estimated_draws_avoided;
		result.manifest_expected = g_state.manifest_expected_signatures.size();
		result.manifest_matched = matched_manifest_signatures();
		result.manifest_missing = result.manifest_expected > result.manifest_matched ? result.manifest_expected - result.manifest_matched : 0u;
		result.manifest_new = g_state.observed_stable_signatures.size() > result.manifest_matched ? g_state.observed_stable_signatures.size() - result.manifest_matched : 0u;

		switch (g_state.current)
		{
		case phase::disabled: result.progress = 0.0f; break;
		case phase::warmup:
			result.progress = 0.20f * std::min(1.0f, static_cast<float>(result.phase_age) / static_cast<float>(k_warmup_frames));
			break;
		case phase::capturing:
		{
			const auto capture_limit = g_state.manifest_valid ? k_extended_capture_frames : k_max_capture_frames;
			const float age_progress = std::min(1.0f, static_cast<float>(result.phase_age) / static_cast<float>(capture_limit));
			const float settle_progress = std::min(1.0f, static_cast<float>(result.quiet_frames) / static_cast<float>(k_settle_frames));
			result.progress = 0.20f + 0.75f * std::max(age_progress, settle_progress);
			break;
		}
		case phase::resident: result.progress = 1.0f; break;
		default: result.progress = 0.0f; break;
		}
		return result;
	}

	void toggle()
	{
		if (!g_state.experimental_acknowledged)
		{
			game::console();
			printf("[STATIC SCENE V20.8] Legacy toggle blocked. Unlock the experimental option first.\n");
			return;
		}

		if (g_state.enabled || g_state.requested_enabled) {
			disable_experimental();
		} else {
			activate_experimental_profile(experimental_profile::safe_preview);
		}
	}

	void rebuild()
	{
		if (has_persistent_submission())
		{
			game::console();
			printf("[STATIC SCENE] In-place rebuild blocked: persistent instances require a DXVK scene clear. Reload/change map to rebuild safely.\n");
			return;
		}

		g_state.records.clear();
		g_state.world_passes.clear();
		g_state.candidates = 0u;
		g_state.submitted = 0u;
		g_state.skipped = 0u;
		g_state.rejected_dynamic = 0u;
		g_state.rejected_material = 0u;
		g_state.sky_candidates = 0u;
		g_state.model_candidates = 0u;
		g_state.world_pass_calls = 0u;
		g_state.world_pass_skips = 0u;
		g_state.world_pass_validation_runs = 0u;
		g_state.world_pass_invalidations = 0u;
		g_state.estimated_draws_avoided = 0u;
		g_state.expired_records = 0u;
		g_state.capture_degraded = false;
		g_state.manifest_dirty = false;
		g_state.observed_stable_signatures.clear();
		g_state.main_pass_ordinal = 0u;
		g_state.sky_pass_ordinal = 0u;
		g_state.frame = current_source_frame();
		g_state.last_new_record_frame = g_state.frame;
		g_state.current = phase::disabled;
		transition_to(g_state.enabled ? phase::warmup : phase::disabled);
	}

	void force_resident()
	{
		if (g_state.experimental_acknowledged && g_state.enabled)
		{
			g_state.capture_degraded = true;
			transition_to(phase::resident);
			g_state.manifest_status = "Forced Resident capture was not auto-saved";
		}
	}

	void toggle_sky3d_fusion()
	{
		game::console();
		if (!g_state.experimental_acknowledged || !g_state.requested_enabled)
		{
			printf("[STATIC SCENE V20.8] Sky3D fusion is available only inside an active experimental profile.\n");
			return;
		}
		if (g_state.current == phase::capturing || g_state.current == phase::resident || has_persistent_submission(true))
		{
			printf("[STATIC SCENE] In-place Sky3D mode change blocked: persistent sky instances already exist. Change the setting before map load and reload/change map.\n");
			return;
		}
		g_state.sky3d_fusion = !g_state.sky3d_fusion;
		printf("[STATIC SCENE] Sky3D fusion=%d\n", g_state.sky3d_fusion ? 1 : 0);
	}

	void toggle_full_visibility_capture()
	{
		game::console();
		if (!g_state.experimental_acknowledged || !g_state.requested_enabled)
		{
			printf("[STATIC SCENE V20.8] Full BSP visibility is available only inside an active experimental profile.\n");
			return;
		}
		if (g_state.current == phase::capturing || g_state.current == phase::resident)
		{
			printf("[STATIC SCENE] Change full-visibility mode before capture, then reload/rebuild the map.\n");
			return;
		}
		g_state.full_visibility_capture = !g_state.full_visibility_capture;
		printf("[STATIC SCENE] full BSP visibility capture=%d\n",
			g_state.full_visibility_capture ? 1 : 0);
	}

	void toggle_model_info_classifier()
	{
		game::console();
		if (!g_state.experimental_acknowledged || !g_state.requested_enabled)
		{
			printf("[STATIC SCENE V20.8] Static-prop classification is available only inside an active experimental profile.\n");
			return;
		}
		if (g_state.current == phase::capturing || g_state.current == phase::resident)
		{
			printf("[STATIC SCENE V20.8] Change static-prop classification before capture, then reload/rebuild the map.\n");
			return;
		}
		g_state.model_info_classifier = !g_state.model_info_classifier;
		printf("[STATIC SCENE] VModelInfo static classifier=%d (experimental)\n",
			g_state.model_info_classifier ? 1 : 0);
	}

	void status()
	{
		game::console();
		const auto matched = matched_manifest_signatures();
		printf("[STATIC SCENE V20.8] acknowledged=%d requested=%d enabled=%d reloadRequired=%d profile=%s phase=%s quality=%s map=%s frame=%u records=%zu stable=%llu candidates=%llu submitted=%llu skipped=%llu rejectedDynamic=%llu rejectedMaterial=%llu BSP=%u/%u cache=%s expected=%zu matched=%llu coverage=%.1f%% completeness=%.1f%% degraded=%d passHook=%d mainPassBypass=%d skyPassBypass=%d passSkips=%llu estimatedDIPsAvoided=%llu\n",
			g_state.experimental_acknowledged ? 1 : 0, g_state.requested_enabled ? 1 : 0, g_state.enabled ? 1 : 0,
			g_state.reload_required ? 1 : 0, profile_name(g_state.reload_required ? g_state.requested_profile : g_state.active_profile),
			phase_name(g_state.current), quality_name(current_quality()), g_state.map_name.c_str(), g_state.frame, g_state.records.size(),
			static_cast<unsigned long long>(stable_record_count()), static_cast<unsigned long long>(g_state.candidates),
			static_cast<unsigned long long>(g_state.submitted), static_cast<unsigned long long>(g_state.skipped),
			static_cast<unsigned long long>(g_state.rejected_dynamic), static_cast<unsigned long long>(g_state.rejected_material),
			g_state.bsp_nodes, g_state.bsp_leafs, g_state.manifest_status.c_str(), g_state.manifest_expected_signatures.size(),
			static_cast<unsigned long long>(matched), manifest_coverage_ratio() * 100.0f, completeness_score() * 100.0f,
			g_state.capture_degraded ? 1 : 0, g_engine_renderer_hook_installed ? 1 : 0, g_state.world_pass_bypass ? 1 : 0,
			g_state.sky_pass_bypass ? 1 : 0, static_cast<unsigned long long>(g_state.world_pass_skips),
			static_cast<unsigned long long>(g_state.estimated_draws_avoided));
	}

	bool is_enabled() { return g_state.enabled; }
	bool is_resident() { return g_state.current == phase::resident; }
	bool sky3d_fusion_enabled() { return g_state.sky3d_fusion; }
	bool full_visibility_capture_enabled() { return g_state.full_visibility_capture; }
	bool model_info_classifier_enabled() { return g_state.model_info_classifier; }
	bool world_pass_bypass_enabled() { return g_state.world_pass_bypass; }
	bool sky_pass_bypass_enabled() { return g_state.sky_pass_bypass; }
	bool xorxor_water_quarantined() { return g_state.xorxor_water_quarantine; }
	bool requires_full_visibility_capture()
	{
		return g_state.enabled && g_state.full_visibility_capture &&
			(g_state.current == phase::warmup || g_state.current == phase::capturing);
	}
	phase current_phase() { return g_state.current; }
}
