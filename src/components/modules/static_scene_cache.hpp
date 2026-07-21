#pragma once

namespace components { class prim_fvf_context; }

namespace components::static_scene_cache
{
	enum class phase : std::uint8_t
	{
		disabled,
		warmup,
		capturing,
		resident,
	};

	enum class experimental_profile : std::uint8_t
	{
		safe_preview,
		full_bsp,
		full_scene,
	};

	enum class cache_quality : std::uint8_t
	{
		none,
		learning,
		good,
		complete,
		degraded,
		stale,
	};

	// UI commands are queued from the ImGui/Present path and executed at the
	// beginning of the next RenderView. This prevents the experimental baker
	// from clearing capture containers, touching manifests or installing hooks
	// while the current ImGui/render pass is still being assembled.
	enum class ui_action : std::uint8_t
	{
		set_acknowledged,
		activate_safe_preview,
		activate_full_bsp,
		activate_full_scene,
		set_manifest_auto_save,
		set_manifest_warm_start,
		save_manifest,
		reload_manifest,
		clear_manifest,
		export_audit,
		rebuild_capture,
		reload_current_map,
		disable_experimental,
		reset_defaults,
		install_pass_hook,
		toggle_world_pass_bypass,
		toggle_sky_pass_bypass,
		validate_world_passes,
		print_status,
	};

	struct ui_action_status
	{
		bool pending = false;
		std::uint32_t pending_count = 0u;
		std::uint64_t queued = 0u;
		std::uint64_t executed = 0u;
		std::uint64_t failed = 0u;
		std::uint64_t blocked = 0u;
		bool runtime_quarantine = true;
		std::string last_action = "None";
		std::string last_result = "No deferred baker action has run yet";
	};

	struct status_snapshot
	{
		bool acknowledged = false;
		bool enabled = false;
		bool requested_enabled = false;
		bool reload_required = false;
		bool persistent_submission = false;
		bool sky3d_fusion = false;
		bool full_visibility_capture = false;
		bool model_info_classifier = false;
		bool renderer_hook_installed = false;
		bool world_pass_bypass = false;
		bool sky_pass_bypass = false;
		bool xorxor_water_quarantine = false;
		bool manifest_loaded = false;
		bool manifest_valid = false;
		bool manifest_stale = false;
		bool manifest_dirty = false;
		bool manifest_auto_save = true;
		bool manifest_warm_start = true;
		bool capture_degraded = false;
		phase current = phase::disabled;
		experimental_profile active_profile = experimental_profile::safe_preview;
		experimental_profile requested_profile = experimental_profile::safe_preview;
		cache_quality quality = cache_quality::none;
		std::string map_name;
		std::string manifest_path;
		std::string manifest_status;
		std::uint32_t frame = 0u;
		std::uint32_t phase_age = 0u;
		std::uint32_t quiet_frames = 0u;
		std::uint32_t bsp_nodes = 0u;
		std::uint32_t bsp_leafs = 0u;
		float progress = 0.0f;
		float completeness = 0.0f;
		float manifest_coverage = 0.0f;
		float world_pass_coverage = 0.0f;
		std::uint64_t records = 0u;
		std::uint64_t stable_records = 0u;
		std::uint64_t expired_records = 0u;
		std::uint64_t candidates = 0u;
		std::uint64_t submitted = 0u;
		std::uint64_t skipped = 0u;
		std::uint64_t rejected_dynamic = 0u;
		std::uint64_t rejected_material = 0u;
		std::uint64_t sky_candidates = 0u;
		std::uint64_t model_candidates = 0u;
		std::uint64_t world_passes = 0u;
		std::uint64_t covered_world_passes = 0u;
		std::uint64_t world_pass_skips = 0u;
		std::uint64_t estimated_draws_avoided = 0u;
		std::uint64_t manifest_expected = 0u;
		std::uint64_t manifest_matched = 0u;
		std::uint64_t manifest_missing = 0u;
		std::uint64_t manifest_new = 0u;
	};

	struct draw_arguments
	{
		D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
		std::int32_t base_vertex_index = 0;
		std::uint32_t min_vertex_index = 0;
		std::uint32_t num_vertices = 0;
		std::uint32_t start_index = 0;
		std::uint32_t primitive_count = 0;
	};

	// Map/session lifecycle.
	void on_map_load(const char* map_name);
	void on_map_unload();
	void on_frame();

	// Model scope lets the primitive hook distinguish prop_static-style model draws
	// from animated/dynamic models without changing the renderer ABI.
	void begin_model_draw(const ModelRenderInfo_t& info, bool source_static_candidate);
	void end_model_draw();

	// Called by the CMeshDX8 interception path.
	void on_pre_draw(CMeshDX8* mesh);
	void process_draw(IDirect3DDevice9* device, prim_fvf_context& context, const draw_arguments& args);
	void quarantine_xorxor_water();

	// Install the IRender::DrawWorldLists detour used by the adaptive resident-pass bypass.
	bool install_renderer_hooks();

	// Experimental feature gate and presets. These settings are session-only and
	// intentionally default to disabled on every process start.
	void set_experimental_acknowledged(bool acknowledged);
	bool experimental_acknowledged();
	bool activate_experimental_profile(experimental_profile profile);
	void disable_experimental();
	void reset_experimental_defaults();
	const char* experimental_profile_name(experimental_profile profile);
	const char* cache_quality_name(cache_quality quality);
	status_snapshot snapshot();

	// Thread-safe UI action queue. The optional value is used by boolean
	// actions such as acknowledgement and manifest policy toggles.
	void enqueue_ui_action(ui_action action, bool value = false);
	ui_action_status get_ui_action_status();
	bool ui_action_runtime_available(ui_action action);
	const char* ui_action_runtime_block_reason(ui_action action);

	// V20.8 persistent manifest controls. The manifest stores stable draw
	// signatures and map identity only; geometry is still resubmitted through D3D9.
	void set_manifest_auto_save(bool enabled);
	void set_manifest_warm_start(bool enabled);
	void save_manifest();
	void reload_manifest();
	void clear_manifest();
	void export_audit_report();
	void manifest_status();

	// Console controls.
	void toggle();
	void rebuild();
	void force_resident();
	void toggle_sky3d_fusion();
	void toggle_full_visibility_capture();
	void toggle_model_info_classifier();
	void install_pass_hook_command();
	void toggle_world_pass_bypass();
	void toggle_sky_pass_bypass();
	void validate_world_passes();
	void world_pass_status();
	void status();

	bool is_enabled();
	bool is_resident();
	bool sky3d_fusion_enabled();
	bool full_visibility_capture_enabled();
	bool model_info_classifier_enabled();
	bool world_pass_bypass_enabled();
	bool sky_pass_bypass_enabled();
	bool xorxor_water_quarantined();
	bool requires_full_visibility_capture();
	phase current_phase();
}
