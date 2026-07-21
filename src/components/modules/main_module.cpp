#include "std_include.hpp"
#define USE_BUILD_WORLD_LIST_NOCULL 0

namespace components
{
	namespace cmd
	{
		bool debug_node_vis = false;
	}

	int g_current_leaf = -1;
	int g_current_area = -1;

	// Populated immediately before VIEW_3DSKY BSP recursion. It contains only
	// leaves from sky_camera.area and their parent chain, avoiding accidental
	// duplication of the main map when capture temporarily disables culling.
	std::unordered_set<const mnode_t*> g_full_resident_sky_capture_nodes;

	bool full_resident_world_data_sane(const worldbrushdata_t* world)
	{
		// Do not touch BSP arrays while hoststate is transitioning between maps.
		// The broad upper limit protects against a stale/mismatched structure layout
		// turning a count into an unbounded memory write.
		constexpr int k_max_bsp_elements = 1 << 20;
		return world &&
			world->nodes && world->leafs &&
			world->numnodes > 0 && world->numnodes <= k_max_bsp_elements &&
			world->numleafs > 0 && world->numleafs <= k_max_bsp_elements;
	}

	bool g_player_leaf_update = false;
	map_settings::area_overrides_s* g_player_current_area_override = nullptr; // contains overrides for the current area, nullptr if no overrides exist

	// true if playershadow flag is used during startup (commandline)
	bool g_use_playershadow = false;

	// true if the game is currently rendering our own thirdperson mesh (playershadow related)
	int  g_is_rendering_our_thirdperson_mesh = false;

	void on_renderview()
	{
		if (!loader::is_runtime_ready()) {
			return;
		}
		const auto* runtime_interfaces = interfaces::get();
		if (!runtime_interfaces || !runtime_interfaces->m_engine || !runtime_interfaces->m_engine->is_playing())
		{
			// Loading screens, disconnect transitions and the main menu must remain
			// completely owned by Source. Do not inject fog, markers, lights or FFP
			// defaults until the engine reports a playable map.
			return;
		}

		// Use Source's frame counter when available. The +1 keeps zero reserved as
		// the explicit "no payload captured" sentinel across map transitions.
		if (const auto* intf = interfaces::get(); intf && intf->m_globals)
			main_module::framecount = static_cast<std::uint64_t>(std::max(intf->m_globals->framecount, 0)) + 1u;
		else
			++main_module::framecount;

		static_scene_cache::on_frame();

		const auto dev = game::get_d3d_device();

		// helper for nocull markers
		model_render::get()->m_drew_model = false;

		// set a default material with diffuse set to a warm white
		// so that add light to texture works and does not require rtx.effectLightPlasmaBall (animated)
		D3DMATERIAL9 dmat = {};
		dmat.Diffuse.r = 1.0f;
		dmat.Diffuse.g = 0.8f;
		dmat.Diffuse.b = 0.8f;
		dev->SetMaterial(&dmat);

		// ----
		// ----

		//choreo_events::on_client_frame();
		remix_vars::on_client_frame();
		remix_lights::on_client_frame();
		dynamic_lighting::on_client_frame();
		remix_markers::on_client_frame();
		map_settings::on_frame_autosave();
		game_settings::on_frame();

		main_module::force_cvars();

		// TODO - find better spot to call this
		map_settings::spawn_markers_once();
		// nocull markers handled in 'model_renderer::DrawModelExecute::Detour'

		// CM_PointLeafnum :: get current leaf. Do not dereference a transient camera
		// pointer while the engine is rebuilding its view during map/input transitions.
		Vector safe_view_origin = {};
		if (game::get_current_view_origin_safe(safe_view_origin))
		{
			const auto current_leaf = game::get_leaf_from_position(safe_view_origin);
			g_player_leaf_update = g_current_leaf != current_leaf;
			g_current_leaf = current_leaf;
		}

		// CM_LeafArea :: get current area the camera is in
		g_current_area = l4d2::CM_LeafArea ? l4d2::CM_LeafArea(g_current_leaf) : 0;

		if (const auto api = remix_api::get(); api) {
			api->on_renderview();
		}

		// fog
		if (static bool allow_fog = !flags::has_flag("no_fog"); allow_fog)
		{
			const auto& s = map_settings::get_map_settings();
			const bool has_dist = s.fog_dist > 0.0f;
			const bool has_density = s.fog_density > 0.0f;

			if (has_dist || has_density)
			{
				const float fog_start = 1.0f; // not useful
				dev->SetRenderState(D3DRS_FOGENABLE, TRUE);
				dev->SetRenderState(D3DRS_FOGTABLEMODE, has_dist ? D3DFOG_LINEAR : has_density ? D3DFOG_EXP : D3DFOG_NONE);
				dev->SetRenderState(D3DRS_FOGDENSITY, *(DWORD*)&s.fog_density); // 0-1
				dev->SetRenderState(D3DRS_FOGSTART, *(DWORD*)&fog_start);
				dev->SetRenderState(D3DRS_FOGEND, *(DWORD*)&s.fog_dist);
				dev->SetRenderState(D3DRS_FOGCOLOR, s.fog_color);
			}
			else
			{
				dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
			}
		}
	}

	HOOK_RETN_PLACE_DEF(cviewrenderer_renderview_retn);
	__declspec(naked) void cviewrenderer_renderview_stub()
	{
		__asm
		{
			pushad;
			call	on_renderview;
			popad;

			// og
			mov     eax, [ecx];
			mov     edx, [eax + 0x20];
			jmp		cviewrenderer_renderview_retn;
		}
	}

	// ##
	// ##

	main_module::sky3d_diagnostics_s& main_module::sky3d_diagnostics()
	{
		static sky3d_diagnostics_s unavailable = {};
		return get() ? get()->m_sky3d_diag : unavailable;
	}

	void main_module::reset_sky3d_diagnostics()
	{
		auto& diag = sky3d_diagnostics();
		const bool installed = diag.hook_installed;
		const bool logging = diag.rate_limited_logging;
		const auto generation = get() ? get()->m_sky3d_payload_generation : 0u;
		const auto payload_frame = get() ? get()->m_sky3d_payload_frame : 0u;
		const auto hook_payload_frame = get() ? get()->m_sky3d_hook_payload_frame : 0u;
		const auto payload_signature = get() ? get()->m_sky3d_payload_signature : 0u;
		const bool hook_confirmed = get() ? get()->m_sky3d_payload_hook_confirmed : false;
		const Vector origin = get() ? get()->m_sky3d_origin : Vector{};
		const Vector camera = get() ? get()->m_sky3d_camera_origin : Vector{};
		const int scale = get() ? get()->m_sky3d_scale : 0;
		const int area = get() ? get()->m_sky3d_area : -1;
		diag = {};
		diag.hook_installed = installed;
		diag.rate_limited_logging = logging;
		diag.payload_generation = generation;
		diag.last_payload_frame = payload_frame;
		diag.last_valid_payload_frame = payload_frame;
		diag.last_hook_payload_frame = hook_payload_frame;
		diag.last_payload_signature = payload_signature;
		diag.last_payload_hook_confirmed = hook_confirmed;
		diag.last_origin = origin;
		diag.last_sky_camera_position = camera;
		diag.last_scale = scale;
		diag.last_area = area;
		diag.last_payload_source = payload_frame ? "current runtime payload" : "none";
		diag.last_stage = "counters reset";
	}

	bool main_module::capture_sky3d_payload(const CSkyCamera* sky, const Vector* camera_origin, const char* source)
	{
		auto* main = get();
		if (!main || !sky) return false;

		auto& diag = main->m_sky3d_diag;
		++diag.sky_camera_found;
		const Vector origin = sky->m_skyboxData.origin;
		const int scale = sky->m_skyboxData.scale;
		const int area = sky->m_skyboxData.area;
		const std::string_view source_name = source && *source ? source : "unknown";
		const bool hook_confirmed = source_name.find("SkyboxView::Draw") != std::string_view::npos;
		const int max_scale = game_settings::get()
			? std::clamp(game_settings::get()->sky3d_max_scale.get_as<int>(), 1, 65536)
			: 1024;
		const bool valid_origin = std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z);
		const bool valid_camera_origin = !camera_origin ||
			(std::isfinite(camera_origin->x) && std::isfinite(camera_origin->y) && std::isfinite(camera_origin->z));
		const bool valid_scale = scale > 0 && scale <= max_scale;
		const bool valid_area = area >= -1 && area <= 65535;

		diag.last_origin = origin;
		diag.last_scale = scale;
		diag.last_area = area;
		diag.last_payload_source = std::string(source_name);
		diag.last_payload_hook_confirmed = hook_confirmed;
		if (camera_origin && valid_camera_origin)
		{
			main->m_sky3d_camera_origin = *camera_origin;
			diag.last_sky_camera_position = *camera_origin;
		}

		if (!valid_scale || !valid_origin || !valid_camera_origin || !valid_area)
		{
			if (!valid_scale) ++diag.invalid_scale;
			if (!valid_origin) ++diag.invalid_origin;
			if (!valid_camera_origin) ++diag.invalid_camera_origin;
			if (!valid_area) ++diag.invalid_area;
			if (!valid_scale) diag.last_stage = "sky_camera rejected: invalid or excessive scale";
			else if (!valid_origin) diag.last_stage = "sky_camera rejected: non-finite origin";
			else if (!valid_camera_origin) diag.last_stage = "sky_camera rejected: non-finite camera origin";
			else diag.last_stage = "sky_camera rejected: invalid area";
			return false;
		}

		std::uint64_t signature = 14695981039346656037ull;
		auto mix = [&signature](const void* data, const std::size_t size)
		{
			const auto* bytes = static_cast<const std::uint8_t*>(data);
			for (std::size_t i = 0u; i < size; ++i) signature = (signature ^ bytes[i]) * 1099511628211ull;
		};
		mix(&origin.x, sizeof(origin.x));
		mix(&origin.y, sizeof(origin.y));
		mix(&origin.z, sizeof(origin.z));
		mix(&scale, sizeof(scale));
		mix(&area, sizeof(area));
		const bool transform_changed = signature != main->m_sky3d_payload_signature;

		main->m_sky3d_origin = origin;
		main->m_sky3d_scale = scale;
		main->m_sky3d_area = area;
		main->m_sky3d_payload_frame = framecount;
		main->m_sky3d_payload_signature = signature;
		main->m_sky3d_payload_hook_confirmed = hook_confirmed;
		if (hook_confirmed)
		{
			main->m_sky3d_hook_payload_frame = framecount;
			main->m_sky3d_hook_payload_signature = signature;
		}
		else
		{
			++diag.recovery_payloads;
		}

		if (transform_changed || main->m_sky3d_payload_generation == 0u)
		{
			++main->m_sky3d_payload_generation;
			++diag.payload_transform_changes;
		}
		else
		{
			++diag.payload_refreshes;
		}

		++diag.valid_payloads;
		diag.payload_generation = main->m_sky3d_payload_generation;
		diag.last_payload_signature = signature;
		diag.last_payload_frame = framecount;
		diag.last_valid_payload_frame = framecount;
		diag.last_hook_payload_frame = main->m_sky3d_hook_payload_frame;
		diag.last_stage = std::format("valid sky_camera payload captured ({}, {})", diag.last_payload_source,
			hook_confirmed ? "hook-confirmed" : "recovery-only");
		return true;
	}

	void main_module::reset_sky3d_payload(const char* reason)
	{
		auto* main = get();
		if (!main) return;
		main->m_sky3d_origin.Init();
		main->m_sky3d_camera_origin.Init();
		main->m_sky3d_scale = 0;
		main->m_sky3d_area = -1;
		main->m_sky3d_payload_frame = 0u;
		main->m_sky3d_hook_payload_frame = 0u;
		main->m_sky3d_payload_signature = 0u;
		main->m_sky3d_hook_payload_signature = 0u;
		main->m_sky3d_payload_hook_confirmed = false;
		++main->m_sky3d_payload_generation;

		auto& diag = main->m_sky3d_diag;
		++diag.payload_resets;
		diag.payload_generation = main->m_sky3d_payload_generation;
		diag.last_payload_frame = 0u;
		diag.last_valid_payload_frame = 0u;
		diag.last_hook_payload_frame = 0u;
		diag.last_payload_signature = 0u;
		diag.last_payload_hook_confirmed = false;
		diag.last_origin.Init();
		diag.last_sky_camera_position.Init();
		diag.last_scale = 0;
		diag.last_area = -1;
		diag.last_payload_source = "none";
		diag.last_stage = std::format("payload reset: {}", reason && *reason ? reason : "unspecified");
	}

	std::uint64_t main_module::sky3d_payload_age_frames()
	{
		const auto* main = get();
		if (!main || main->m_sky3d_scale <= 0 || main->m_sky3d_payload_frame == 0u) {
			return std::numeric_limits<std::uint64_t>::max();
		}
		return framecount >= main->m_sky3d_payload_frame ? framecount - main->m_sky3d_payload_frame : 0u;
	}

	bool main_module::sky3d_payload_is_fresh(std::uint64_t max_age_frames)
	{
		auto* main = get();
		if (!main || main->m_sky3d_scale <= 0 || main->m_sky3d_payload_frame == 0u) return false;
		if (max_age_frames == 0u)
		{
			auto* settings = game_settings::get();
			max_age_frames = settings
				? static_cast<std::uint64_t>(std::clamp(settings->sky3d_payload_max_age_frames.get_as<int>(), 1, 120))
				: 8u;
		}
		const auto age = sky3d_payload_age_frames();
		const bool fresh = age <= max_age_frames;
		if (fresh && age > 0u)
		{
			auto& diag = main->m_sky3d_diag;
			if (diag.last_reuse_note_frame != framecount)
			{
				diag.last_reuse_note_frame = framecount;
				++diag.payload_reused;
			}
		}
		return fresh;
	}


	std::uint64_t main_module::sky3d_hook_payload_age_frames()
	{
		const auto* main = get();
		if (!main || main->m_sky3d_hook_payload_frame == 0u || main->m_sky3d_hook_payload_signature == 0u)
			return std::numeric_limits<std::uint64_t>::max();
		return framecount >= main->m_sky3d_hook_payload_frame ? framecount - main->m_sky3d_hook_payload_frame : 0u;
	}

	bool main_module::sky3d_payload_is_hook_confirmed(std::uint64_t max_age_frames)
	{
		const auto* main = get();
		if (!main || main->m_sky3d_payload_signature == 0u ||
			main->m_sky3d_payload_signature != main->m_sky3d_hook_payload_signature) return false;
		if (max_age_frames == 0u)
		{
			auto* settings = game_settings::get();
			max_age_frames = settings
				? static_cast<std::uint64_t>(std::clamp(settings->sky3d_payload_max_age_frames.get_as<int>(), 1, 120))
				: 8u;
		}
		return sky3d_hook_payload_age_frames() <= max_age_frames;
	}

	bool main_module::sky3d_payload_is_capture_eligible(std::uint64_t max_age_frames)
	{
		if (!sky3d_payload_is_fresh(max_age_frames)) return false;
		auto* settings = game_settings::get();
		if (!settings || !settings->sky3d_require_hook_confirmation.get_as<bool>()) return true;
		return sky3d_payload_is_hook_confirmed(max_age_frames);
	}

	std::uint64_t main_module::sky3d_payload_signature()
	{
		const auto* main = get();
		return main ? main->m_sky3d_payload_signature : 0u;
	}

	const char* main_module::sky3d_health_summary()
	{
		const auto* main = get();
		if (!main) return "main module unavailable";
		const auto& diag = main->m_sky3d_diag;
		auto* settings = game_settings::get();
		if (settings && !settings->enable_3d_sky.get_as<bool>()) return "disabled by settings";
		if (!diag.hook_installed) return "SkyboxView hook not installed";
		if (diag.view_3dsky_detections == 0u) return "hook active; VIEW_3DSKY not observed";
		if (diag.sky_camera_found == 0u) return "VIEW_3DSKY observed; sky_camera not found";
		if (!sky3d_payload_is_fresh()) return "sky_camera payload missing or stale; Source fallback active";
		if (!sky3d_payload_is_capture_eligible()) return "recovery-only payload; waiting for VIEW_3DSKY hook confirmation";
		if (diag.static_candidates == 0u) return "eligible payload; no static-scene sky candidates";
		if (diag.submitted_objects == 0u) return "sky candidates observed; none submitted yet";
		if (diag.source_draw_suppressed == 0u) return "persistent sky submitted; waiting for resident Source-draw suppression";
		return "3D skybox payload, persistent submission and Source suppression confirmed";
	}

	void main_module::note_sky3d_static_candidate()
	{
		auto& diag = sky3d_diagnostics();
		++diag.static_candidates;
		diag.last_candidate_frame = framecount;
		diag.last_stage = "3D sky geometry reached static-scene candidate stage";
	}

	void main_module::note_sky3d_submitted_object()
	{
		auto& diag = sky3d_diagnostics();
		++diag.submitted_objects;
		diag.last_submission_frame = framecount;
		diag.last_stage = "3D sky geometry submitted with persistent category";
	}

	void main_module::note_sky3d_missing_payload()
	{
		auto& diag = sky3d_diagnostics();
		++diag.rejected_missing_payload;
		diag.last_stage = "static-scene capture rejected: missing valid sky_camera payload";
	}

	void main_module::note_sky3d_stale_payload()
	{
		auto& diag = sky3d_diagnostics();
		if (diag.last_stale_note_frame == framecount) return;
		diag.last_stale_note_frame = framecount;
		++diag.payload_stale;
		diag.last_stage = "sky_camera payload exceeded freshness window";
	}

	void main_module::note_sky3d_recovery_rejected()
	{
		auto& diag = sky3d_diagnostics();
		if (diag.last_recovery_reject_note_frame == framecount) return;
		diag.last_recovery_reject_note_frame = framecount;
		++diag.recovery_capture_rejected;
		diag.last_stage = "static-scene capture rejected: payload lacks recent VIEW_3DSKY hook confirmation";
	}

	void main_module::note_sky3d_safe_fallback()
	{
		auto& diag = sky3d_diagnostics();
		if (diag.last_fallback_note_frame == framecount) return;
		diag.last_fallback_note_frame = framecount;
		++diag.safe_fallbacks;
		diag.last_stage = "safe Source rendering fallback kept for 3D skybox";
	}

	void main_module::note_sky3d_source_suppressed()
	{
		auto& diag = sky3d_diagnostics();
		++diag.source_draw_suppressed;
		diag.last_suppressed_frame = framecount;
		diag.last_stage = "resident 3D sky record suppressed the matching Source draw";
	}

	std::string main_module::export_runtime_diagnostics()
	{
		const std::filesystem::path final_path = std::filesystem::path(game::root_path) / "l4d2-rtx" / "logs" / "runtime_diagnostics_v21_5.txt";
		const auto temp_path = final_path.string() + ".tmp";
		std::error_code ec;
		std::filesystem::create_directories(final_path.parent_path(), ec);
		std::ofstream out(temp_path, std::ios::out | std::ios::trunc);
		if (!out.is_open()) return std::format("failed to open {}", temp_path);
		const auto& sky = sky3d_diagnostics();
		const auto fl = remix_api::flashlight_runtime_stats();
		// V21.8.1 runtime diagnostics baseline retained for regression validation.
		out << "L4D2 RTX Compatibility Mod V21.11 runtime diagnostics\n\n";
		out << "[sky3d]\n";
		out << "health = " << sky3d_health_summary() << '\n';
		out << "stage = " << sky.last_stage << '\n';
		out << "payload_source = " << sky.last_payload_source << '\n';
		out << "payload_hook_confirmed = " << (sky.last_payload_hook_confirmed ? "true" : "false") << '\n';
		out << "payload_signature = " << sky.last_payload_signature << '\n';
		out << "payload_generation = " << sky.payload_generation << '\n';
		out << "payload_age_frames = " << sky3d_payload_age_frames() << '\n';
		out << "hook_payload_age_frames = " << sky3d_hook_payload_age_frames() << '\n';
		out << "hook_calls = " << sky.hook_calls << '\n';
		out << "view_3dsky = " << sky.view_3dsky_detections << '\n';
		out << "valid_payloads = " << sky.valid_payloads << '\n';
		out << "recovery_payloads = " << sky.recovery_payloads << '\n';
		out << "recovery_capture_rejected = " << sky.recovery_capture_rejected << '\n';
		out << "static_candidates = " << sky.static_candidates << '\n';
		out << "submitted_objects = " << sky.submitted_objects << '\n';
		out << "source_draw_suppressed = " << sky.source_draw_suppressed << '\n';
		out << "origin = " << sky.last_origin.x << ' ' << sky.last_origin.y << ' ' << sky.last_origin.z << '\n';
		out << "scale = " << sky.last_scale << "\narea = " << sky.last_area << "\n\n";
		out << "[flashlight]\n";
		out << "tracked_owners = " << fl.tracked_owners << '\n';
		out << "active_handles = " << fl.active_handles << '\n';
		out << "requested_layers = " << fl.requested_layers << '\n';
		out << "granted_layers = " << fl.granted_layers << '\n';
		out << "transactional_commits = " << fl.transactional_commits << '\n';
		out << "transactional_rollbacks = " << fl.transactional_rollbacks << '\n';
		out << "stale_rig_fallbacks = " << fl.stale_rig_fallbacks << '\n';
		out << "retry_suppressed = " << fl.retry_suppressed << '\n';
		out << "create_failures = " << fl.create_failures << '\n';
		out << "draw_failures = " << fl.draw_failures << '\n';
		out << "invalidated_handles = " << fl.invalidated_handles << '\n';
		out << "owner_grace_frames_used = " << fl.owner_grace_frames_used << '\n';
		out.close();
		if (!out) return "failed while writing runtime diagnostics";
		const auto temp_w = std::filesystem::path(temp_path).wstring();
		const auto final_w = final_path.wstring();
		if (!MoveFileExW(temp_w.c_str(), final_w.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			std::filesystem::remove(temp_path, ec);
			return "failed to atomically replace runtime diagnostic report";
		}
		return final_path.string();
	}

	void on_skyboxdraw()
	{
		if (!loader::is_runtime_ready()) {
			return;
		}

		auto* main = main_module::get();
		if (!main) return;
		auto& diag = main->m_sky3d_diag;
		auto* settings = game_settings::get();
		diag.rate_limited_logging = settings && settings->sky3d_diagnostic_logging.get_as<bool>();
		++diag.hook_calls;
		diag.last_view_id = static_cast<std::uint32_t>(game::get_viewid());
		diag.last_stage = "SkyboxView::Draw hook entered";

		if (game::get_viewid() != VIEW_3DSKY)
		{
			++diag.hook_non_3d_calls;
			diag.last_stage = "SkyboxView hook entered outside VIEW_3DSKY";
			return;
		}

		++diag.view_3dsky_detections;
		const Vector camera_origin = l4d2::g_vecCurrentRenderOrigin ? *l4d2::g_vecCurrentRenderOrigin : Vector{};
		diag.last_stage = "VIEW_3DSKY detected";

		if (!settings || !settings->enable_3d_sky.get_as<bool>())
		{
			diag.last_stage = "hook active, feature disabled in settings";
			return;
		}

		if (const auto* sky = l4d2::GetCurrentSkyCamera ? l4d2::GetCurrentSkyCamera() : nullptr; sky)
		{
			main_module::capture_sky3d_payload(sky, &camera_origin, "SkyboxView::Draw");
		}
		else
		{
			diag.last_stage = "VIEW_3DSKY observed, sky_camera not found";
		}

		if (diag.rate_limited_logging)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now - diag.last_log_time >= std::chrono::seconds(2))
			{
				diag.last_log_time = now;
				const auto age = main_module::sky3d_payload_age_frames();
				game::console();
				printf("[Sky3D V21.8.1] hook=%llu view=%llu camera=%llu payload=%llu stale=%llu fallback=%llu candidates=%llu submitted=%llu age=%s stage=%s scale=%d origin=(%.1f %.1f %.1f)\n",
					static_cast<unsigned long long>(diag.hook_calls), static_cast<unsigned long long>(diag.view_3dsky_detections),
					static_cast<unsigned long long>(diag.sky_camera_found), static_cast<unsigned long long>(diag.valid_payloads),
					static_cast<unsigned long long>(diag.payload_stale), static_cast<unsigned long long>(diag.safe_fallbacks),
					static_cast<unsigned long long>(diag.static_candidates), static_cast<unsigned long long>(diag.submitted_objects),
					age == std::numeric_limits<std::uint64_t>::max() ? "n/a" : utils::va("%llu", static_cast<unsigned long long>(age)),
					diag.last_stage.c_str(), diag.last_scale, diag.last_origin.x, diag.last_origin.y, diag.last_origin.z);
			}
		}
	}

	HOOK_RETN_PLACE_DEF(skyboxview_draw_internal_retn);
	__declspec(naked) void skyboxview_draw_internal_stub()
	{
		__asm
		{
			add     esp, 0x14;

			pushad;
			call	on_skyboxdraw;
			popad;

			// og
			cmp     byte ptr[ebp + 0xC], 0;
			jmp		skyboxview_draw_internal_retn;
		}
	}

	// called on EndScene - remix_api::end_scene_callback()
	void main_module::iterate_entities()
	{
		const auto intf = interfaces::get();
		const auto max_ent = intf->m_entity_list->get_max_entity();

		for (auto i = 0; i < max_ent; i++)
		{
			if (const auto	entity = reinterpret_cast<sdk::c_base_player*>(intf->m_entity_list->get_client_entity(i));
				entity)
			{
				if (const auto* m_classes = entity->client_class();
					m_classes)
				{
					switch (m_classes->class_id)
					{
					default:
						continue;

					case sdk::ET_CTERRORPLAYER:
					case sdk::ET_SURVIVORBOT:
					{
						sdk::player_info_t info;
						if (!intf->m_engine->get_player_info(i, &info)) {
							continue;
						}

						/*if (entity->is_local_player()) {
							int break_me = 1;
						}*/

						if (const auto is_player = i == intf->m_engine->get_local_player();
							is_player)
						{
							if (g_use_playershadow)
							{
								if (auto& playermodel_substr = main_module::get()->m_playermodel_substr;
									playermodel_substr.empty())
								{
#if 0								// old hardcoded system
									const std::string_view name = entity->get_player_model_name();

									if (name == "Coach") {
										playermodel_substr = "models/survivors/coach";
									}
									else if (name == "Mechanic" || name == "Ellis") {
										playermodel_substr = "models/survivors/mechanic";
									}
									else if (name == "Gambler" || name == "Nick") {
										playermodel_substr = "models/survivors/gambler";
									}
									else if (name == "Producer" || name == "Rochelle") {
										playermodel_substr = "models/survivors/producer";
									}
									else if (name == "NamVet" || name == "Bill") {
										playermodel_substr = "models/survivors/namvet";
									}
									else if (name == "TeenGirl" || name == "Zoey" || name == "TeenAngst") {
										playermodel_substr = "models/survivors/teenangst";
									}
									else if (name == "Biker" || name == "Francis") {
										playermodel_substr = "models/survivors/biker";
									}
									else if (name == "Manager" || name == "Louis") {
										playermodel_substr = "models/survivors/manager";
									}
									else {
										playermodel_substr = "INVALID";
									}
#endif

									// new dynamic system - takes path of first material referenced within the model
									// found a better way to do this so this is not really needed rn
#if 0
									bool found_valid_material = false;
									if (const auto mdl = entity->get_model(); mdl)
									{
										IMaterial* pmat = nullptr;
										const auto modelinfo = game::get_modelinfo();
										modelinfo->vftable->GetModelMaterials(modelinfo, mdl, 1, &pmat);

										if (pmat)
										{
											std::string str = pmat->vftable->GetName(pmat);
											const size_t last_slash = str.find_last_of("/\\");

											if (last_slash != std::string::npos)
											{
												playermodel_substr = str.substr(0, last_slash + 1);
												found_valid_material = true;
											}
										}
									}

									if (!found_valid_material) {
										playermodel_substr = "INVALID";
									}
#endif
								}
							}

							const auto& flashlight_enabled = entity->read<bool>(0x14D8);

							auto& eyepos = main_module::get()->m_player_eye_pos;
							eyepos = entity->read<Vector>(0x1110);
							//const auto& eyepos = entity->read<Vector>(0x1110);

							const auto& fwd = entity->read<Vector>(0x111C);
							const auto& rt = entity->read<Vector>(0x1134);
							const auto& up = entity->read<Vector>(0x1128);
							remix_api::get()->flashlight_create_or_update(info.name, eyepos, fwd, rt, up, flashlight_enabled, true);

							// Safe recovery path: when the SkyboxView hook has not produced a fresh
							// payload yet, entity iteration may refresh sky_camera metadata. It does
							// not force static capture and therefore cannot hide Source geometry.
							if (game_settings::get()->enable_3d_sky.get_as<bool>() &&
								game_settings::get()->sky3d_safe_source_fallback.get_as<bool>() &&
								!main_module::sky3d_payload_is_fresh())
							{
								if (const auto* sky = l4d2::GetCurrentSkyCamera ? l4d2::GetCurrentSkyCamera() : nullptr; sky)
								{
									main_module::capture_sky3d_payload(sky, nullptr, "entity iteration fallback");
								}
							}
						}

						else // SurvivorBot
						{
							const auto& m_fEffects = entity->read<int>(0xE0);
							const bool flashlight_enabled = m_fEffects & 4;

							const auto& eyepos = entity->get_eye_pos();
							const auto& angles = entity->read<Vector>(0x196C); // m_angEyeAngles[0] - DT_CSPlayer

							Vector fwd, rt, up;
							utils::vector::AngleVectors(angles, &fwd, &rt, &up);

							remix_api::get()->flashlight_create_or_update(info.name, eyepos, fwd, rt, up, flashlight_enabled);
						}
						break;
					}
					}
				}
			}
		}

		// Compatibility lifecycle restored from V20.9: flashlight_frame() marks every
		// owner unseen after rebuilding the current frame, while entity iteration marks
		// owners that still exist. A missing/dead/disconnected owner is disabled before
		// the next frame rebuild, matching the original working behaviour.
		if (remix_api::is_initialized())
		{
			for (auto& [name, flashlight] : remix_api::get()->m_flashlights)
			{
				if (!flashlight.is_alive) {
					flashlight.is_enabled = false;
				}
			}
		}
	}


	/**
	 * Force visibility of a specific node
	 * @param node_index	The node to force vis for
	 * @param player_node	The node the player is currently in
	 */
	void force_node_vis(int node_index, bool hide = false)
	{
		const auto world = game::get_hoststate_worldbrush_data();
		const auto root_node = &world->nodes[0];

		int next_node_index = node_index;
		while (next_node_index >= 0)
		{
			if (node_index == g_current_leaf) {
				break;
			}

			const auto node = &world->nodes[next_node_index];

			if (!hide)
			{
				// node was already set to current visframe, do not continue
				if (node->visframe == game::get_visframecount()) {
					break;
				}

				// force node vis
				node->visframe = game::get_visframecount();
			}
			else
			{
				// nodes already hidden
				if (node->visframe == 0) {
					break;
				}

				node->visframe = 0;
			}

			// we only need to traverse to the root node
			if (node == root_node) {
				break;
			}

			next_node_index = &node->parent[0] - root_node;
		}
	}

	/**
	 * Force visibility of a specific leaf
	 * @param leaf_index   The leaf to force vis for
	 * @param player_node  The node the player is currently in
	 */
	void force_leaf_vis(int leaf_index, bool hide = false)
	{
		const auto world = game::get_hoststate_worldbrush_data();
		auto leaf_node = &world->leafs[leaf_index];
		auto parent_node_index = &leaf_node->parent[0] - &world->nodes[0];

		if (!hide)
		{
			// force leaf vis
			leaf_node->visframe = game::get_visframecount();
		}
		else
		{
			leaf_node->visframe = 0;
		}

		// force nodes
		force_node_vis(parent_node_index, hide);
	}

	// check if a boundingbox is within a specified radius around the player
	bool is_aabb_within_distance(const VectorAligned& center, const VectorAligned& half_diagonal, const Vector& player_origin, const float radius)
	{
		const Vector min_bounds = center - half_diagonal;
		const Vector max_bounds = center + half_diagonal;

		auto sq_dist = 0.0f;
		for (auto i = 0; i < 3; ++i)
		{
			if (player_origin[i] < min_bounds[i])
			{
				const auto d = min_bounds[i] - player_origin[i];
				sq_dist += d * d;
			}
			else if (player_origin[i] > max_bounds[i])
			{
				const auto d = player_origin[i] - max_bounds[i];
				sq_dist += d * d;
			}

			// return false if distance exceeds radius sqr
			if (sq_dist > radius * radius) {
				return false;
			}
		}

		return true;
	}

	// Stub before calling 'R_CullNode' in 'R_RecursiveWorldNode'
	// Return 0 to NOT cull the node
	int r_cullnode_wrapper(mnode_t* node)
	{
		const auto view = game::get_viewid();
		if (view == VIEW_MONITOR) {
			return l4d2::R_CullNode ? l4d2::R_CullNode(node) : false;
		}

		// Full Resident capture deliberately exposes the complete BSP to the fixed-function
		// bridge for a short capture window. This is not a per-frame resident culling mode.
		if (static_scene_cache::requires_full_visibility_capture() &&
			full_resident_world_data_sane(game::get_hoststate_worldbrush_data()))
		{
			if (view == VIEW_MAIN) {
				return 0;
			}
			if (view == VIEW_3DSKY && static_scene_cache::sky3d_fusion_enabled() &&
				g_full_resident_sky_capture_nodes.contains(node)) {
				return 0;
			}
		}

		if (view == VIEW_3DSKY) {
			return l4d2::R_CullNode ? l4d2::R_CullNode(node) : false;
		}

		// default culling mode or no culling if cmd was used
		map_settings::AREA_CULL_MODE cmode = imgui::get()->m_disable_cullnode ? map_settings::AREA_CULL_MODE_NO_FRUSTUM : map_settings::AREA_CULL_INFO_DEFAULT;
		int node_index = 0;

		/*if (imgui::get()->m_disable_cullnode) {
			return 0;
		}*/

		// check if we have area overrides
		if (g_player_current_area_override)
		{
			// set area cull mode
			cmode = g_player_current_area_override->cull_mode;

			// calculate index of leaf/node
			if (node->contents >= 0) { // this is a leaf
				node_index = (mleaf_t*)node - &game::get_hoststate_worldbrush_data()->leafs[0];
			}
			else { // this is a node
				node_index = node - &game::get_hoststate_worldbrush_data()->nodes[0];
			}

			// HIDE
			// check if this node was forced visible
			if (!g_player_current_area_override->leafs.contains(node_index))
			{
				// if node not forced, iterate all hidden area entries
				for (const auto& hidden_area : g_player_current_area_override->hide_areas)
				{
					// check if node is part of a hidden area but only cull if the player is not in a specified leaf
					if (hidden_area.areas.contains((std::uint32_t)node->area)
						&& !hidden_area.when_not_in_leafs.contains(g_current_leaf))
					{
						return 1;
					}
				}

				// check if this leaf is set to be hidden
				if (g_player_current_area_override->hide_leafs.contains(node_index)) {
					return 1;
				}
			}
		}

		// draw node if culling mode is set to 'no culling'
		if (cmode == map_settings::AREA_CULL_MODE_NO_FRUSTUM) {
			return 0;
		}


		/*bool is_leaf = node->contents >= 0;
		const auto world = game::get_hoststate_worldbrush_data();
		int idx = is_leaf ? ((mleaf_t*)node - &world->leafs[0]) : (node - &world->nodes[0]);
		if (idx == 3152 || idx == 3136)
		{
			int x = 1;
			return 0;
		}*/

		// "global" nocull distance if area has no overrides
		float nocull_dist = map_settings::get_map_settings().default_nocull_dist;
		const bool using_distance_based_mode = cmode >= map_settings::AREA_CULL_INFO_NOCULLDIST_START && cmode <= map_settings::AREA_CULL_INFO_NOCULLDIST_END;

		if (using_distance_based_mode && g_player_current_area_override)
		{
			// nocull distance if area has override
			nocull_dist = g_player_current_area_override->nocull_distance;

			// if any leaf tweak has a nocull override
			if (g_player_current_area_override->nocull_distance_overrides_in_leaf_twk)
			{
				for (const auto& lt : g_player_current_area_override->leaf_tweaks)
				{
					// check if node the player is currently in has any overrides
					if (lt.in_leafs.contains(g_current_leaf))
					{
						nocull_dist = lt.nocull_dist;
						break;
					}
				}
			}
		}

		// if no area override or if cull mode is distance based
		if (   !g_player_current_area_override
			|| using_distance_based_mode)
		{
			if (is_aabb_within_distance(node->m_vecCenter, node->m_vecHalfDiagonal, *game::get_current_view_origin(), nocull_dist)) {
				return 0;
			}

			// if forcing current area + distance
			if (cmode == map_settings::AREA_CULL_MODE_FORCE_AREA_DISTANCE)
			{
				if ((int)node->area == g_current_area) {
					return 0;
				}
			}
		}

		// MODE: force all leafs/nodes in CURRENT area
		else if (cmode == map_settings::AREA_CULL_MODE_NO_FRUSTUM_IN_CURRENT_AREA
			  || cmode == map_settings::AREA_CULL_MODE_FORCE_AREA)
		{
			// force draw this node/leaf if it's within the forced area
			if ((int)node->area == g_current_area) {
				return 0;
			}
		}

		// R_CullNode - uses area frustums if avail. and not in a solid - uses player frustum otherwise
		if (!l4d2::R_CullNode || !l4d2::R_CullNode(node)) {
			return 0;
		}

		// check if we have area overrides
		if (g_player_current_area_override)
		{
			// check if this leaf/node is part of a forced area
			if (g_player_current_area_override->areas.contains((std::uint32_t)node->area)) {
				return 0;
			}

			// check if this leaf/node is force enabled
			if (g_player_current_area_override->leafs.contains(node_index)) {
				return 0;
			}

			// check if there are leaf specific tweaks
			if (!g_player_current_area_override->leaf_tweaks.empty())
			{
				for (const auto& lt : g_player_current_area_override->leaf_tweaks)
				{
					// check if node the player is currently in has any overrides
					if (lt.in_leafs.contains(g_current_leaf))
					{
						// if so, check if the current node to be culled is part of a forced area
						// note: areas are not vis forced - this only disables frustum culling and relies on PVS
						if (lt.areas.contains((std::uint32_t)node->area)) {
							return 0;
						}

						// force individual leafs
						if (lt.leafs.contains(node_index)) {
							return 0;
						}
					}
				}
			}
		}

		// cull node
		return 1;
	}

	HOOK_RETN_PLACE_DEF(r_cullnode_cull_retn);
	HOOK_RETN_PLACE_DEF(r_cullnode_skip_retn);
	__declspec(naked) void r_cullnode_stub()
	{
		__asm
		{
			pushad;
#if USE_BUILD_WORLD_LIST_NOCULL
			push	esi;
#else
			push	ebx;
#endif
			call	r_cullnode_wrapper; // return 0 to not jump
			add		esp, 4;
			test	eax, eax;
			jz		SKIP; // jump if eax = 0
			popad;

#if USE_BUILD_WORLD_LIST_NOCULL
			mov     ecx, [ebp - 4];
#endif
			add     esp, 4; // og
			jmp		r_cullnode_cull_retn;

		SKIP:
			popad;

#if USE_BUILD_WORLD_LIST_NOCULL
			mov     ecx, [ebp - 4];
#endif

			add     esp, 4; // og
			jmp		r_cullnode_skip_retn;
		}
	}


	// Trigger leaf/node forcing logic and updates 'g_player_current_area_override' when 'pre_recursive_world_node()' gets called
	void main_module::trigger_vis_logic()
	{
		g_player_leaf_update = true;
		g_player_current_area_override = nullptr;
	}

	// called from remix_api::on_present_callback()
	void main_module::hud_draw_area_info()
	{
		// Draw current node/leaf as HUD
		if (cmd::debug_node_vis && d3d_font)
		{
			RECT rect;
			if (g_current_area != -1)
			{
				SetRect(&rect, get()->m_hud_debug_node_vis_pos[0], get()->m_hud_debug_node_vis_pos[1], 512, 512);
				d3d_font->DrawTextA(nullptr, utils::va("Area: %d", g_current_area), -1, &rect, DT_NOCLIP, D3DCOLOR_XRGB(255, 255, 255)); // text length (-1 = null-terminated)
			}

			if (g_current_leaf != -1)
			{
				SetRect(&rect, get()->m_hud_debug_node_vis_pos[0], get()->m_hud_debug_node_vis_pos[1] + 15, 512, 512);
				d3d_font->DrawTextA(nullptr, utils::va("Leaf: %d", g_current_leaf), -1, &rect, DT_NOCLIP, D3DCOLOR_XRGB(50, 255, 20));
			}

			if (get()->m_hud_debug_node_vis_has_forced_leafs)
			{
				SetRect(&rect, get()->m_hud_debug_node_vis_pos[0], get()->m_hud_debug_node_vis_pos[1] + 40, 512, 512);
				d3d_font->DrawTextA(nullptr, "Individual forced leafs", -1, &rect, DT_NOCLIP, D3DCOLOR_XRGB(0, 255, 255));
			}

			if (get()->m_hud_debug_node_vis_has_forced_arealeafs)
			{
				SetRect(&rect, get()->m_hud_debug_node_vis_pos[0], get()->m_hud_debug_node_vis_pos[1] + 55, 512, 512);
				d3d_font->DrawTextA(nullptr, "Leafs of a forced area", -1, &rect, DT_NOCLIP, D3DCOLOR_XRGB(255, 0, 0));
			}
		}
	}

	void pre_recursive_world_node()
	{
		const auto view = game::get_viewid();
		if (view == VIEW_MONITOR) {
			return;
		}

		const auto world = game::get_hoststate_worldbrush_data();
		if (!world) {
			return;
		}

		g_full_resident_sky_capture_nodes.clear();
		if (static_scene_cache::requires_full_visibility_capture())
		{
			if (!full_resident_world_data_sane(world))
			{
				static bool warned_invalid_world = false;
				if (!warned_invalid_world)
				{
					warned_invalid_world = true;
					game::console();
					printf("[STATIC SCENE] Full-BSP capture deferred: invalid/incomplete worldbrushdata.\n");
				}
				return;
			}
			const auto visframe = game::get_visframecount();
			if (view == VIEW_MAIN)
			{
				for (auto i = 0; i < world->numnodes; ++i) {
					world->nodes[i].visframe = visframe;
				}
				for (auto i = 0; i < world->numleafs; ++i) {
					world->leafs[i].visframe = visframe;
				}
			}
			else if (view == VIEW_3DSKY && static_scene_cache::sky3d_fusion_enabled())
			{
				const auto sky_area = main_module::get()->m_sky3d_area;
				if (sky_area >= 0)
				{
					for (auto i = 0; i < world->numleafs; ++i)
					{
						auto* leaf = &world->leafs[i];
						if (leaf->area != sky_area) {
							continue;
						}

						leaf->visframe = visframe;
						g_full_resident_sky_capture_nodes.insert(reinterpret_cast<mnode_t*>(leaf));
						std::uint32_t parent_steps = 0u;
						const auto parent_limit = static_cast<std::uint32_t>(world->numnodes + world->numleafs);
						for (auto* parent = leaf->parent; parent && parent_steps < parent_limit;
							parent = parent->parent, ++parent_steps)
						{
							parent->visframe = visframe;
							g_full_resident_sky_capture_nodes.insert(parent);
						}
					}
				}
			}
		}

		if (view == VIEW_3DSKY) {
			return;
		}

		// reset
		main_module::get()->m_hud_debug_node_vis_has_forced_leafs = false;
		main_module::get()->m_hud_debug_node_vis_has_forced_arealeafs = false;

		auto& map_settings = map_settings::get_map_settings();

		// visualize current leaf + forced leafs (map_settings)
		if (g_current_leaf < world->numleafs)
		{
			if (cmd::debug_node_vis)
			{
				const auto curr_leaf = &world->leafs[g_current_leaf];
				remix_api::get()->debug_draw_box(curr_leaf->m_vecCenter, curr_leaf->m_vecHalfDiagonal, 2.0f, remix_api::DEBUG_REMIX_LINE_COLOR::GREEN); // current leaf

				// does the area the player is currently in have any overrides?
				if (g_player_current_area_override)
				{
					// visualize forced leafs
					for (const auto& l : g_player_current_area_override->leafs)
					{
						if (!remix_api::can_add_debug_lines()) {
							break;
						}

						if (const auto	forced_leaf = &world->leafs[l];
										forced_leaf != curr_leaf)
						{
							// visualize near-by leaf overrides (TEAL)
							if (game::get_current_view_origin()->DistToSqr(forced_leaf->m_vecCenter) < 2000.0f * 2000.0f)
							{
								remix_api::get()->debug_draw_box(forced_leaf->m_vecCenter, forced_leaf->m_vecHalfDiagonal, 3.5f, remix_api::DEBUG_REMIX_LINE_COLOR::TEAL);
								main_module::get()->m_hud_debug_node_vis_has_forced_leafs = true;
							}
						}
					}

					// visualize leafs of forced areas
					for (const auto& a : g_player_current_area_override->areas)
					{
						for (auto i = 0u; i < (std::uint32_t)world->numleafs; i++)
						{
							if (!remix_api::can_add_debug_lines()) {
								break;
							}

							// visualize near-by leafs that are part of area overrides (RED)
							if (const auto	forced_leaf = &world->leafs[i];
											forced_leaf != curr_leaf && a == (std::uint32_t)forced_leaf->area)
							{
								if (game::get_current_view_origin()->DistToSqr(forced_leaf->m_vecCenter) < 350.0f * 350.0f)
								{
									remix_api::get()->debug_draw_box(forced_leaf->m_vecCenter, forced_leaf->m_vecHalfDiagonal, 3.5f, remix_api::DEBUG_REMIX_LINE_COLOR::RED);
									main_module::get()->m_hud_debug_node_vis_has_forced_arealeafs = true;
								}
							}
						}
					}

					// visualize leafs of forced areas defined in leaf_tweaks
					for (const auto& lt : g_player_current_area_override->leaf_tweaks)
					{
						if (lt.in_leafs.contains(g_current_leaf))
						{
							for (auto i = 0u; i < (std::uint32_t)world->numleafs; i++)
							{
								if (!remix_api::can_add_debug_lines()) {
									break;
								}

								// visualize near-by leafs that are part of area overrides (RED)
								if (const auto	forced_leaf = &world->leafs[i];
									forced_leaf != curr_leaf && lt.areas.contains((std::uint32_t)forced_leaf->area))
								{
									if (game::get_current_view_origin()->DistToSqr(forced_leaf->m_vecCenter) < 350.0f * 350.0f)
									{
										remix_api::get()->debug_draw_box(forced_leaf->m_vecCenter, forced_leaf->m_vecHalfDiagonal, 3.5f, remix_api::DEBUG_REMIX_LINE_COLOR::RED);
										main_module::get()->m_hud_debug_node_vis_has_forced_arealeafs = true;
									}
								}
							}
						}
					}
				}
			}
		}

		// #
		// leaf/node forcing

		// We have to set all nodes from the target leaf to the root node or the node the the player is in to the current visframe
		// Otherwise, 'R_RecursiveWorldNode' will never reach the target leaf

		if (!map_settings.area_settings.empty())
		{
			//if (leaf_update)
			//if (g_player_leaf_update)
			{
				g_player_current_area_override = nullptr;

				if (const auto& t = map_settings.area_settings.find(g_current_area);
					t != map_settings.area_settings.end())
				{
					g_player_current_area_override = &t->second; // cache

					// force all specified leafs/nodes
					for (const auto& l : g_player_current_area_override->leafs)
					{
						if (l < static_cast<std::uint32_t>(world->numleafs)) {
							force_leaf_vis(l); // force leaf to be visible
						}
					}

					// force all leafs/nodes in specified areas
					if (!g_player_current_area_override->areas.empty())
					{
						for (auto i = 0; i < world->numleafs; i++)
						{
							if (const auto& l = world->leafs[i];
								g_player_current_area_override->areas.contains((std::uint32_t)l.area))
							{
								force_leaf_vis(i);
							}
						}
					}
				}
			}
		}
		else {
			g_player_current_area_override = nullptr;
		}

		const map_settings::AREA_CULL_MODE cmode = !g_player_current_area_override ? map_settings::AREA_CULL_INFO_DEFAULT : g_player_current_area_override->cull_mode;
		const float nocull_dist = !g_player_current_area_override ? map_settings.default_nocull_dist : g_player_current_area_override->nocull_distance;

		const bool check_area = cmode == map_settings::AREA_CULL_MODE_FORCE_AREA_DISTANCE;

		// MODE: force all leafs/nodes within a certain dist to the player (+ only in current area modifier)
		if ((  check_area
			|| cmode == map_settings::AREA_CULL_MODE_DISTANCE)
			&& nocull_dist > 0.0f)
		{
			for (auto i = 0; i < world->numleafs; i++)
			{
				if (auto& l = world->leafs[i];
					!check_area || (int)l.area == g_current_area) // ignore area check if distance mode
				{
					if (is_aabb_within_distance(l.m_vecCenter, l.m_vecHalfDiagonal, *game::get_current_view_origin(), nocull_dist)) {
						force_leaf_vis(i);
					}
				}
			}
		}

		// MODE: force all leafs/nodes in current area
		else if (cmode == map_settings::AREA_CULL_MODE_FORCE_AREA)
		{
			for (auto i = 0; i < world->numleafs; i++)
			{
				if (auto& l = world->leafs[i];
					(int)l.area == g_current_area)
				{
					force_leaf_vis(i);
				}
			}
		}

		if (g_player_current_area_override)
		{
			// leaf tweaks: this forces all leafs of an area that is forced per leaf
			if (!g_player_current_area_override->leaf_tweaks.empty())
			{
				for (const auto& lt : g_player_current_area_override->leaf_tweaks)
				{
					if (lt.in_leafs.contains(g_current_leaf))
					{
						for (auto i = 0u; i < (std::uint32_t)world->numleafs; i++)
						{
							// visualize near-by leafs that are part of area overrides (RED)
							if (const auto	forced_leaf = &world->leafs[i];
								lt.areas.contains((std::uint32_t)forced_leaf->area)
								|| lt.leafs.contains(i))
							{
								force_leaf_vis(i);
							}
						}
					}
				}
			}
		}

		// update visibility of nocull markers
		if (g_player_leaf_update)
		{
			for (auto& m : map_settings.map_markers)
			{
				// ignore normal markers
				if (!m.no_cull || m.areas.empty()) {
					continue;
				}

				// hide marker
				m.is_hidden = true;

				// check if player is in specified area & not in specified leaf
				if (m.areas.contains(g_current_area) && !m.when_not_in_leafs.contains(g_current_leaf)) {
					m.is_hidden = false; // show marker
				}
			}
		}

		// leaf transitions
		if (g_player_leaf_update && !map_settings.remix_transitions.empty())
		{
			for (auto t = map_settings.remix_transitions.begin(); t != map_settings.remix_transitions.end();)
			{
				// only handle leaf transitions
				if (t->trigger_type != map_settings::TRANSITION_TRIGGER_TYPE::LEAF) {
					++t; continue;
				}

				bool iterpp = false;
				bool trigger_transition = false;

				const bool keep_transition = t->mode >= map_settings::ALWAYS_ON_ENTER;
				const bool trigger_on_enter = t->mode == map_settings::ONCE_ON_ENTER || t->mode == map_settings::ALWAYS_ON_ENTER;
				const bool trigger_on_leave = t->mode == map_settings::ONCE_ON_LEAVE || t->mode == map_settings::ALWAYS_ON_LEAVE;

				if (t->leafs.contains(g_current_leaf))
				{
					if (!t->_state_enter) // first time we enter the leafset
					{
						if (trigger_on_enter) {
							trigger_transition = true;
						}

						t->_state_enter = true;
					}
				}

				// no longer touching any leaf in this set
				else
				{
					if (t->_state_enter) // player just moved out of the leafset
					{
						if (trigger_on_leave) {
							trigger_transition = true;
						}
					}

					t->_state_enter = false;
				}

				if (trigger_transition)
				{
					bool can_add_transition = true;

					// do not allow the same transition twice
					/*for (const auto& ip : remix_vars::interpolate_stack)
					{
						if (ip.identifier == t->hash)
						{
							can_add_transition = false;
							break;
						}
					}*/

					if (can_add_transition)
					{
						remix_vars::parse_and_apply_conf_with_lerp(
							t->config_name,
							t->hash,
							t->interpolate_type,
							t->duration,
							t->delay_in,
							t->delay_out);

						if (!keep_transition)
						{
							t = map_settings.remix_transitions.erase(t);
							iterpp = true; // erase returns the next iterator
						}
					}
				}

				if (!iterpp) {
					++t;
				}
			}
		}
#if 0
		for (auto i = 0; i < world->numleafs; i++)
		{
			// leaf forcing test of disp
			if (i == 3152 || i == 3136)
			{
				force_leaf_vis(i);
			}

			if (i == 3174)
			{
				int x = 1;
			}

			if (imgui::get()->m_enable_area_forcing)
			{
				if (auto& l = world->leafs[i];
					(int)l.area == g_current_area)
				{
					force_leaf_vis(i);
				}
			}
		}
#endif
	}

#if USE_BUILD_WORLD_LIST_NOCULL
	HOOK_RETN_PLACE_DEF(p_build_world_list_no_cull_func);
#endif
	HOOK_RETN_PLACE_DEF(pre_recursive_world_node_retn);
	__declspec(naked) void pre_recursive_world_node_stub()
	{
		__asm
		{
			pushad;
			call	pre_recursive_world_node;
			popad;

#if USE_BUILD_WORLD_LIST_NOCULL
			// og
			mov     ecx, [edx + 0x50];
			push	ebx;
			call	p_build_world_list_no_cull_func;
#else
			// og
			mov     edx, [eax + 0x50];
			mov     ecx, ebx;
#endif
			jmp		pre_recursive_world_node_retn;
		}
	}


	// #
	// #

	namespace playershadow
	{
		HOOK_RETN_PLACE_DEF(draw_player_thirdperson_mesh_draw_retn);

		HOOK_RETN_PLACE_DEF(draw_player_thirdperson_mesh_check01_retn);
		__declspec(naked) void draw_player_thirdperson_mesh_check01_stub()
		{
			__asm
			{
				add     esp, 4;
				cmp     eax, ecx;
				jnz		NOT_LOCAL_PLAYER; // jump if not our player mesh
				jmp		draw_player_thirdperson_mesh_check01_retn; // onto check #2

			NOT_LOCAL_PLAYER:
				mov		g_is_rendering_our_thirdperson_mesh, 0;
				jmp		draw_player_thirdperson_mesh_draw_retn; // skip other checks and draw mesh
			}
		}

		HOOK_RETN_PLACE_DEF(draw_player_thirdperson_mesh_check02_retn);
		__declspec(naked) void draw_player_thirdperson_mesh_check02_stub()
		{
			__asm
			{
				push    0xFFFFFFFF;
				call    eax;
				test    eax, eax;
				jnz		IS_THIRDPERSON_CAM; // jump if CAM_IsThirdPerson returns true
				jmp		draw_player_thirdperson_mesh_check02_retn; // onto check #3

			IS_THIRDPERSON_CAM:
				mov		g_is_rendering_our_thirdperson_mesh, 0;
				jmp		draw_player_thirdperson_mesh_draw_retn; // skip other checks and draw mesh
			}
		}


		__declspec(naked) void draw_player_thirdperson_mesh_check03_stub()
		{
			__asm
			{
				mov     ecx, edi;
				call    eax;
				test    al, al;
				jnz		IS_EXT_CAMERA; // jump if in detached camera state
				mov		g_is_rendering_our_thirdperson_mesh, 1;	// we are in first person view - tag as player body
				jmp		draw_player_thirdperson_mesh_draw_retn; // draw mesh

			IS_EXT_CAMERA:
				mov		g_is_rendering_our_thirdperson_mesh, 0; // camera is in detached state - do not tag as player body
				jmp		draw_player_thirdperson_mesh_draw_retn; // still draw mesh tho
			}
		}

		// retn after C_BasePlayer::Draw()
		__declspec(naked) void post_draw_player_thirdperson_mesh_stub()
		{
			__asm
			{
				mov		g_is_rendering_our_thirdperson_mesh, 0;
				retn	8; // og
			}
		}


		// returning 0 skips the impact decal
		int impact_mid_hk(C_BaseEntity* ent)
		{
			if (const auto entity = reinterpret_cast<sdk::c_base_player*>(ent);
				entity)
			{
				if (const auto* m_classes = entity->client_class();
					m_classes)
				{
					switch (m_classes->class_id)
					{
					default:
						break;

					case sdk::ET_CTERRORPLAYER:
					{
						if (entity->is_local_player()) {
							return 0;
						}
						break;
					}
					case sdk::ET_SURVIVORBOT:
						break;
					}
				}
			}

			return 1;
		}

		HOOK_RETN_PLACE_DEF(impact_og_retn);
		HOOK_RETN_PLACE_DEF(impact_skip_retn);
		__declspec(naked) void impact_stub()
		{
			__asm
			{
				pushad;
				push	edi; // C_BaseEntity
				call	impact_mid_hk;
				add		esp, 4;
				test	eax, eax;
				jz		SKIP; // jump if eax = 0
				popad;

				// og
				mov     cl, 1;
				test[ebx + 0x24], cl;
				jmp		impact_og_retn;

			SKIP:
				popad;
				mov     cl, 1;
				jmp		impact_skip_retn;
			}
		}
	}


	// #
	// #

	/**
	 * Called from CModelLoader::Map_LoadModel
	 * @param map_name  Name of loading map
	 */
	void on_map_load_hk(const char* map_name)
	{
		if (!loader::is_runtime_ready()) {
			return;
		}

		static_scene_cache::on_map_load(map_name);

		main_module::reset_sky3d_payload("map load");
		remix_api::clear_flashlights();
		g_full_resident_sky_capture_nodes.clear();
		main_module::get()->m_playermodel_substr.clear();

		// Keep the Event Workbench focused on the current map/session.
		sound_events::clear_history();
		choreo_events::clear_history();

		imgui::on_map_load();
		remix_vars::on_map_load();
		remix_lights::on_map_load();
		dynamic_lighting::on_map_load(map_name);
		material_exporter::on_map_load(map_name);
		map_settings::on_map_load(map_name);
		main_module::force_cvars();

		game::cvar_uncheat("r_propsmaxdist");
		game::cvar_uncheat("cl_detaildist");
		game::cvar_uncheat("cl_detailfade");
		game::cvar_uncheat("cl_footstep_fx");
		game::cvar_uncheat("cl_fov");
		game::cvar_uncheat("cl_impacteffects");
		game::cvar_uncheat("cl_interpolate");
		game::cvar_uncheat("cl_particle_batch_mode");
		game::cvar_uncheat("cl_particle_fallback_base");
		game::cvar_uncheat("cl_particle_fallback_multiplier");
		game::cvar_uncheat("cl_smoke_alpha");
		game::cvar_uncheat("cl_smoke_far");
		game::cvar_uncheat("cl_viewbob");
		game::cvar_uncheat("cpu_level");
		game::cvar_uncheat("gpu_level");
		game::cvar_uncheat("gpu_mem_level");
		game::cvar_uncheat("fx_drawimpactdebris");
		game::cvar_uncheat("fx_drawimpactdust");
		game::cvar_uncheat("fx_drawmetalspark");
		game::cvar_uncheat("r_decals");
		game::cvar_uncheat("r_decalstaticprops");
		game::cvar_uncheat("r_draw_flashlight_3rd_person");
		game::cvar_uncheat("r_draw_lasersight_1st_person");
		game::cvar_uncheat("r_draw_lasersight_3rd_person");
		game::cvar_uncheat("r_drawbatchdecals");
		game::cvar_uncheat("r_drawflecks");
		game::cvar_uncheat("r_drawmodeldecals");
		game::cvar_uncheat("r_drawunderwaterfogblocker");
		game::cvar_uncheat("r_fade360style");
		game::cvar_uncheat("r_flashlight_3rd_person_range");
		game::cvar_uncheat("r_frustumcullworld");
		game::cvar_uncheat("r_impactparticles");
		game::cvar_uncheat("r_maxmodeldecal");
		game::cvar_uncheat("r_occlusion");
		game::cvar_uncheat("r_particle_timescale");
		game::cvar_uncheat("r_queued_decals");
		game::cvar_uncheat("r_queued_ropes");
		game::cvar_uncheat("r_RainParticleDensity");
		game::cvar_uncheat("r_ropetranslucent");
		game::cvar_uncheat("r_ShowViewerArea");
		game::cvar_uncheat("r_snapportal");
		game::cvar_uncheat("r_staticlight_streams");
		game::cvar_uncheat("r_staticpropinfo");
		game::cvar_uncheat("r_teeth");
		game::cvar_uncheat("r_3dsky");
		game::cvar_uncheat("scene_print");
	}

	HOOK_RETN_PLACE_DEF(on_map_load_stub_retn);
	__declspec(naked) void on_map_load_stub()
	{
		__asm
		{
			pushad;
			push    eax;
			call	on_map_load_hk;
			add		esp, 4;
			popad;

			// og
			lea     ecx, [edi + 0x68];
			xor		edx, edx;
			jmp		on_map_load_stub_retn;

		}
	}

	/**
	 * Called from Host_Disconnect
	 * on: disconnect, restart, killserver, stopdemo ...
	 */
	void on_host_disconnect_hk()
	{
		if (!loader::is_runtime_ready()) {
			return;
		}

		static_scene_cache::on_map_unload();
		main_module::reset_sky3d_payload("host disconnect");
		remix_api::clear_flashlights();

		//choreo_events::reset_all();
		main_module::trigger_vis_logic();

		// ----------

		map_settings::on_map_unload();

		// Reset transient per-map variables without synchronous rtx.conf disk parsing.
		remix_vars::on_map_unload();
		main_module::framecount = 0u;
	}

	HOOK_RETN_PLACE_DEF(on_host_disconnect_retn);
	__declspec(naked) void on_host_disconnect_stub()
	{
		__asm
		{
			pushad;
			call	on_host_disconnect_hk;
			popad;

			// og
			mov     ebp, esp;
			sub     esp, 0x10;
			jmp		on_host_disconnect_retn;
		}
	}

	/**
	 * Called from Host_Changelevel
	 * Host_Disconnect is not called when this triggers
	 */
	void on_host_change_level_hk()
	{
		on_host_disconnect_hk();
	}

	HOOK_RETN_PLACE_DEF(on_host_change_level_retn);
	__declspec(naked) void on_host_change_level_stub()
	{
		__asm
		{
			pushad;
			call	on_host_change_level_hk;
			popad;

			// og
			push    0x60;
			lea     ecx, [ebp - 0x64];
			jmp		on_host_change_level_retn;
		}
	}

	// #
	// #

	void main_module::force_cvars()
	{
		// z_wound_client_disabled
		// z_skip_wounds
		// z_randomskins
		// z_randombodygroups
		// z_infected_tinting
		// z_infected_decals
		// z_infected_damage_cutouts
		// z_gibs_per_frame
		// z_forcezombiemodel --- z_forcezombiemodelname

		//game::cvar_uncheat_and_set_int("z_randomskins", 0);
		//game::cvar_uncheat_and_set_int("z_randombodygroups", 0);
		//game::cvar_uncheat_and_set_int("z_infected_tinting", 0);

		//game::cvar_uncheat_and_set_int("z_infected_damage_cutouts", 0);
		//game::cvar_uncheat_and_set_int("z_skip_wounds", 1);
		//game::cvar_uncheat_and_set_int("z_wound_client_disabled", 1);

		if (!game_settings::get()->lod_forcing.get_as<bool>())
		{
			game::cvar_uncheat("r_staticprop_lod");
			game::cvar_uncheat("r_lod");
			game::cvar_uncheat("r_lod_switch_scale"); // hidden cvar
		}
		else
		{
			game::cvar_uncheat_and_set_int("r_staticprop_lod", 0);
			game::cvar_uncheat_and_set_int("r_lod", 0);
			game::cvar_uncheat_and_set_int("r_lod_switch_scale", 1); // hidden cvar
		}

		if (game_settings::get()->force_graphic_settings.get_as<bool>())
		{
			game::cvar_uncheat_and_set_int("cpu_level", 2);
			game::cvar_uncheat_and_set_int("gpu_level", 0);
			game::cvar_uncheat_and_set_int("gpu_mem_level", 2);
		}

		game::cvar_uncheat_and_set_int("r_dopixelvisibility", 0); // fix random crash (dxvk cmdBindPipeline) on map load -> affects sunflare

		game::cvar_uncheat_and_set_int("r_WaterDrawRefraction", 0); // fix weird culling behaviour near water surfaces
		game::cvar_uncheat_and_set_int("r_WaterDrawReflection", 0); // perf?

		// Source renderer threading is intentionally forced to the safe single-threaded path by default.
		// mat_queue_mode -1/2 can improve CPU-side performance, but queued rendering can break Remix capture order
		// and cause one-frame disappearing geometry. Mode 1 is a safer hybrid: no queued renderer, only optional
		// non-render worker cvars. Modes 2-4 are explicit A/B tests with selectable flicker mitigations.
		const int source_threading_mode = std::clamp(game_settings::get()->source_threading_mode.get_as<int>(), 0, 4);
		const int source_flicker_mitigation = std::clamp(game_settings::get()->source_queue_flicker_mitigation.get_as<int>(), 0, 2);
		const bool source_threading_experimental = source_threading_mode != 0;
		const bool source_queued_renderer = source_threading_mode >= 2;
		const bool source_geometry_quarantine = source_queued_renderer && game_settings::get()->source_queue_geometry_quarantine.get_as<bool>();
		const bool source_strict_flicker_guard = source_queued_renderer && source_flicker_mitigation >= 2;
		const int mat_queue_mode = source_threading_mode >= 3 ? 2 : source_threading_mode == 2 ? -1 : 0;
		const bool allow_threaded_particles = source_threading_experimental && game_settings::get()->source_threaded_particles.get_as<bool>() && !source_strict_flicker_guard;

		game::cvar_uncheat_and_set_int("r_threaded_particles", allow_threaded_particles ? 1 : 0);
		game::cvar_uncheat_and_set_int("r_entityclips", 0);
		game::cvar_uncheat_and_set_int("r_PortalTestEnts", 0);

		game::cvar_uncheat_and_set_int("cl_fastdetailsprites", 0);
		game::cvar_uncheat_and_set_int("cl_brushfastpath", 0);
		game::cvar_uncheat_and_set_int("cl_tlucfastpath", 0); //
		game::cvar_uncheat_and_set_int("cl_modelfastpath", 0); // gain 4-5 fps on some act 4 maps but FF rendering not implemented
		game::cvar_uncheat_and_set_int("mat_queue_mode", mat_queue_mode);
		game::cvar_uncheat_and_set_int("r_queued_ropes", source_threading_experimental && game_settings::get()->source_queued_ropes.get_as<bool>() && !source_geometry_quarantine ? 1 : 0);
		game::cvar_uncheat_and_set_int("mat_softwarelighting", 0);
		game::cvar_uncheat_and_set_int("mat_parallaxmap", 0);
		game::cvar_uncheat_and_set_int("mat_frame_sync_enable", source_queued_renderer && (game_settings::get()->source_queue_frame_sync_guard.get_as<bool>() || source_flicker_mitigation >= 1) ? 1 : 0);
		game::cvar_uncheat_and_set_int("mat_forcehardwaresync", source_strict_flicker_guard ? 1 : 0);
		game::cvar_uncheat_and_set_int("mat_forcemanagedtextureintohardware", source_strict_flicker_guard ? 1 : 0);
		game::cvar_uncheat_and_set_int("mat_dof_enabled", 0);
		game::cvar_uncheat_and_set_int("mat_displacementmap", 0);
		game::cvar_uncheat_and_set_int("mat_drawflat", 0);
		game::cvar_uncheat_and_set_int("mat_normalmaps", 0);
		game::cvar_uncheat_and_set_int("r_flashlightrender", 0); // messes up terrain blending otherwise
		game::cvar_uncheat_and_set_int("r_flashlightrendermodels", 0);
		game::cvar_uncheat_and_set_int("r_flashlightrenderworld", 0);
		game::cvar_uncheat_and_set_int("r_FlashlightDetailProps", 0);

		// yes the user could just set it via the console but .. people
		game::cvar_uncheat_and_set_int("r_3dsky", game_settings::get()->enable_3d_sky.get_as<bool>());
		game::cvar_uncheat_and_set_int("mat_fullbright", 1);
		game::cvar_uncheat_and_set_int("mat_softwareskin", 1);
		game::cvar_uncheat_and_set_int("mat_phong", 1);
		game::cvar_uncheat_and_set_int("mat_fastnobump", 1);
		game::cvar_uncheat_and_set_int("mat_disable_bloom", 1);

		game::cvar_uncheat_and_set_int("r_threadeddetailprops", source_threading_experimental && game_settings::get()->source_threaded_detailprops.get_as<bool>() && !source_geometry_quarantine ? 1 : 0);
		game::cvar_uncheat_and_set_int("r_DrawDetailProps", 0); // disables grass (detail) sprites on displacements (unstable and blurry)


		// TODO: HACK
		// remove when displacement-backface culling check is found - currently in use so that
		// displacements are rendered when leaf is forced
		game::cvar_uncheat_and_set_int("r_DispWalkable", 1);


		// graphic settings

		// lvl 0
		game::cvar_uncheat_and_set_int("cl_particle_fallback_base", 0);//3); // 0 = render portalgun viewmodel effects
		game::cvar_uncheat_and_set_int("cl_particle_fallback_multiplier", 1); //2);

		game::cvar_uncheat_and_set_int("cl_impacteffects_limit_general", 10);
		game::cvar_uncheat_and_set_int("cl_impacteffects_limit_exit", 3);
		game::cvar_uncheat_and_set_int("cl_impacteffects_limit_water", 2);
		game::cvar_uncheat_and_set_int("mat_depthfeather_enable", 0);

		game::cvar_uncheat_and_set_int("r_shadowrendertotexture", 0);
		game::cvar_uncheat_and_set_int("r_shadowfromworldlights", 0);

		game::cvar_uncheat_and_set_int("mat_force_vertexfog", 1);
		game::cvar_uncheat_and_set_int("cl_footstep_fx", 1);
		game::cvar_uncheat_and_set_int("cl_ragdoll_self_collision", 1);
		game::cvar_uncheat_and_set_int("cl_ragdoll_maxcount", 24);

		game::cvar_uncheat_and_set_int("cl_detaildist", 1024);
		game::cvar_uncheat_and_set_int("cl_detailfade", 400);
		game::cvar_uncheat_and_set_int("r_drawmodeldecals", 1);
		game::cvar_uncheat_and_set_int("r_decalstaticprops", 1);
		game::cvar_uncheat_and_set_int("cl_player_max_decal_count", 32);

		// lvl 3
		game::cvar_uncheat_and_set_int("r_decals", 2048);
		game::cvar_uncheat_and_set_int("r_decal_overlap_count", 3);

		// disable fog
		game::cvar_uncheat_and_set_int("fog_override", 1);
		game::cvar_uncheat_and_set_int("fog_enable", 0);
	}

	// logic after loading either map or game settings
	void main_module::cross_handle_map_and_game_settings()
	{
		if (remix_api::is_initialized())
		{
			// rtx.skyAutoDetect
			const auto is_3d_sky_enabled = game_settings::get()->enable_3d_sky.get_as<bool>();
			remix_vars::set_option(remix_vars::get_option("rtx.skyAutoDetect"), remix_vars::string_to_option_value(remix_vars::OPTION_TYPE_FLOAT, is_3d_sky_enabled ? "1" : "0"));
		}
	}

	// #
	// #

	ConCommand xo_debug_toggle_node_vis_cmd {};
	void main_module::xo_debug_toggle_node_vis_fn()
	{
		cmd::debug_node_vis = !cmd::debug_node_vis;
	}

	main_module::main_module()
	{
		p_this = this;

		{ // init filepath var
			char path[MAX_PATH]; GetModuleFileNameA(nullptr, path, MAX_PATH);
			game::root_path = path; utils::erase_substring(game::root_path, "left4dead2.exe");
		}

		{ // init d3d font
			D3DXFONT_DESC desc =
			{
				18,                  // Height
				0,                   // Width (0 = default)
				FW_NORMAL,           // Weight (FW_BOLD, FW_LIGHT, etc.)
				1,                   // Mip levels
				FALSE,               // Italic
				DEFAULT_CHARSET,     // Charset
				OUT_DEFAULT_PRECIS,  // Output Precision
				CLIP_DEFAULT_PRECIS, // Clipping Precision
				DEFAULT_PITCH,       // Pitch and Family
				TEXT("Arial")        // Typeface
			};

			D3DXCreateFontIndirect(game::get_d3d_device(), &desc, &d3d_font);
		}


		// #
		// events

		// CModelLoader::Map_LoadModel :: called on map load
		utils::hook(l4d2::hk_addr__on_map_load, on_map_load_stub).install()->quick();
		HOOK_RETN_PLACE(on_map_load_stub_retn, l4d2::hk_addr__on_map_load + 5u);

		// Host_Disconnect :: called on map unload
		utils::hook(l4d2::hk_addr__on_host_disconnect, on_host_disconnect_stub).install()->quick();
		HOOK_RETN_PLACE(on_host_disconnect_retn, l4d2::hk_addr__on_host_disconnect + 5u);

		utils::hook(l4d2::hk_addr__on_host_change_level, on_host_change_level_stub).install()->quick();
		HOOK_RETN_PLACE(on_host_change_level_retn, l4d2::hk_addr__on_host_change_level + 5u);

		// --

		// CViewRender::RenderView :: "start" of current frame (after CViewRender::DrawMonitors)
		utils::hook(l4d2::hk_addr__cviewrenderer_renderview, cviewrenderer_renderview_stub).install()->quick(); // 2501
		HOOK_RETN_PLACE(cviewrenderer_renderview_retn, l4d2::hk_addr__cviewrenderer_renderview + 5u);

		// not really req. rn
		utils::hook::nop(l4d2::hk_addr__skyboxview_draw_internal, 7);
		utils::hook(l4d2::hk_addr__skyboxview_draw_internal, skyboxview_draw_internal_stub).install()->quick();
		HOOK_RETN_PLACE(skyboxview_draw_internal_retn, l4d2::hk_addr__skyboxview_draw_internal + 7u);
		m_sky3d_diag.hook_installed = true;
		m_sky3d_diag.last_stage = "SkyboxView::Draw hook installed";

		// #
		// culling

		// CDispInfo::Render :: disable 'Frustum_t::CullBox' check
		utils::hook::nop(l4d2::nop_addr__cdispinfo_render, 2);

#if USE_BUILD_WORLD_LIST_NOCULL
		// R_RecursiveWorldNodeNoCull:: use 'R_BuildWorldListNoCull' instead of 'R_RecursiveWorldNode'
		utils::hook::nop(ENGINE_BASE + 0xD162D, 2); // THIS will not render bullet holes on bsp?

		// stub before calling 'R_RecursiveWorldNode' to override node/leaf vis
		utils::hook::nop(ENGINE_BASE + 0xD1635, 9);
		utils::hook(ENGINE_BASE + 0xD1635, pre_recursive_world_node_stub, HOOK_JUMP).install()->quick();
		HOOK_RETN_PLACE(p_build_world_list_no_cull_func, ENGINE_BASE + 0xCD630);
		HOOK_RETN_PLACE(pre_recursive_world_node_retn, ENGINE_BASE + 0xD163E);

		// ^ :: while( ... node->contents < -1 .. ) -> jl to jle .. to jmp to cull less (same as returning 0 in cullnode)
		utils::hook::set<BYTE>(ENGINE_BASE + 0xCD665, 0x7E); // needed?

		// ^ :: while( ... !R_CullNode) - wrapper function to impl. additional culling control (force areas/leafs + use frustum culling when needed)
		utils::hook(ENGINE_BASE + 0xCD668, r_cullnode_stub, HOOK_JUMP).install()->quick();
		HOOK_RETN_PLACE(r_cullnode_cull_retn, ENGINE_BASE + 0xCD68F);
		HOOK_RETN_PLACE(r_cullnode_skip_retn, ENGINE_BASE + 0xCD677);

#else
		// stub before calling 'R_RecursiveWorldNode' to override node/leaf vis
		utils::hook(l4d2::hk_addr__pre_recursive_world_node, pre_recursive_world_node_stub, HOOK_JUMP).install()->quick();
		HOOK_RETN_PLACE(pre_recursive_world_node_retn, l4d2::hk_addr__pre_recursive_world_node + 5u);

		// ^ :: while( ... node->contents < -1 .. ) -> jl to jle .. to jmp to cull less
		utils::hook::set<BYTE>(l4d2::jmp_addr__cullnode01, 0x7E);

		// ^ :: while( ... !R_CullNode) - wrapper function to impl. additional culling control (force areas/leafs + use frustum culling when needed)
		utils::hook(l4d2::jmp_addr__cullnode01 + 3u, r_cullnode_stub, HOOK_JUMP).install()->quick();
		HOOK_RETN_PLACE(r_cullnode_cull_retn, l4d2::retn_addr__cullnode_cull);
		HOOK_RETN_PLACE(r_cullnode_skip_retn, l4d2::retn_addr__cullnode_skip);

		// ^ :: backface check -> je to jl
		utils::hook::nop(l4d2::nop_addr__cullnode_backface_check01, 2); // okay - draws a little more but not so heavy on perf.

		// ^ :: backface check -> jnz to je
		utils::hook::set<BYTE>(l4d2::nop_addr__cullnode_backface_check02, 0x74); // ^

		// R_DrawLeaf :: backface check (emissive lamps) plane normal >= -0.00999f
		utils::hook::nop(l4d2::nop_addr__drawleaf_backface_check, 6); // ^
#endif

		// CBrushBatchRender::DrawOpaqueBrushModel :: :: backface check - nop 'if ( bShadowDepth )' to disable culling
		utils::hook::nop(l4d2::nop_addr__draw_opaque_bmodel_backface_check, 2);

		// CClientLeafSystem::ExtractCulledRenderables :: disable 'engine->CullBox' check to disable entity culling in leafs
		// needs r_PortalTestEnts to be 0 -> je to jmp (0xEB)
		utils::hook::set<BYTE>(l4d2::jmp_addr__extract_culled_renderables, 0xEB);

		// ~ always show geometry below water surface
		// CSimpleWorldView::Setup :: nop 'DoesViewPlaneIntersectWater' check
		utils::hook::nop(l4d2::nop_addr__simple_world_view_intersect_water_check, 2);
		// ^ next instruction :: OR m_DrawFlags with 0x60 instead of 0x30
		//utils::hook::set<BYTE>(CLIENT_BASE + 0x1CF471 + 6, 0x60); ...... not needed in l4d2?

		// engine 0xB3383 - no cull overlay decals (no need)

		// ---------------
		// # player shadow

		if (g_use_playershadow = !flags::has_flag("disable_playershadow"); g_use_playershadow)
		{
			// helper var around C_BasePlayer_Draw so we know when we are drawing our player mesh
			// we wrap around each of the three initial checks because we do not want to tag the player body
			// if the game is rendering in third person or when doing intro cinematics

			// GetLocalPlayer check
			utils::hook(l4d2::hk_addr__draw_player_thirdperson_mesh_check01, playershadow::draw_player_thirdperson_mesh_check01_stub, HOOK_JUMP).install()->quick();
			HOOK_RETN_PLACE(playershadow::draw_player_thirdperson_mesh_check01_retn, l4d2::hk_addr__draw_player_thirdperson_mesh_check01 + 11u);

			// CAM_IsThirdPerson check
			utils::hook::nop(l4d2::hk_addr__draw_player_thirdperson_mesh_check02, 6);
			utils::hook(l4d2::hk_addr__draw_player_thirdperson_mesh_check02, playershadow::draw_player_thirdperson_mesh_check02_stub, HOOK_JUMP).install()->quick();
			HOOK_RETN_PLACE(playershadow::draw_player_thirdperson_mesh_check02_retn, l4d2::hk_addr__draw_player_thirdperson_mesh_check02 + 12u);

			// intro cam / "scripted third person" (eg. jockey on player) check
			utils::hook::nop(l4d2::hk_addr__draw_player_thirdperson_mesh_check03, 6);
			utils::hook(l4d2::hk_addr__draw_player_thirdperson_mesh_check03, playershadow::draw_player_thirdperson_mesh_check03_stub, HOOK_JUMP).install()->quick();
			HOOK_RETN_PLACE(playershadow::draw_player_thirdperson_mesh_draw_retn, l4d2::retn_addr__draw_player_thirdperson_mesh); // addr of draw func

			// reset helper var after drawing
			utils::hook(l4d2::retn_addr__draw_player_thirdperson_mesh + 18u, playershadow::post_draw_player_thirdperson_mesh_stub, HOOK_JUMP).install()->quick();

			//
			// F890E disable impact marks on ourselfs
			utils::hook(l4d2::hk_addr__impact_marks_pshadow, playershadow::impact_stub, HOOK_JUMP).install()->quick();
			HOOK_RETN_PLACE(playershadow::impact_og_retn, l4d2::hk_addr__impact_marks_pshadow + 5u);
			HOOK_RETN_PLACE(playershadow::impact_skip_retn, l4d2::retn_addr__impact_marks_pshadow_skip);
		}

		// #
		// commands

		game::con_add_command(&xo_debug_toggle_node_vis_cmd, "xo_debug_toggle_node_vis", xo_debug_toggle_node_vis_fn, "Toggle bsp node/leaf debug visualization using the remix api");
	}

	main_module::~main_module()
	{ }
}
