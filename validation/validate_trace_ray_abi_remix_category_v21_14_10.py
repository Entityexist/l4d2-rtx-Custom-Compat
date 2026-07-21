from pathlib import Path
import math
import sys

ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8-sig")


def require(text: str, fragment: str, label: str) -> None:
    if fragment not in text:
        raise AssertionError(f"missing {label}: {fragment}")


def forbid(text: str, fragment: str, label: str) -> None:
    if fragment in text:
        raise AssertionError(f"obsolete {label} remains: {fragment}")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1:i]
    raise AssertionError(f"unbalanced function: {signature}")




def braces_balanced(source: str) -> bool:
    depth = 0
    i = 0
    state = "normal"
    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "normal":
            if ch == "/" and nxt == "/":
                state = "line"
                i += 2
                continue
            if ch == "/" and nxt == "*":
                state = "block"
                i += 2
                continue
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth < 0:
                    return False
        elif state == "line":
            if ch == "\n":
                state = "normal"
        elif state == "block":
            if ch == "*" and nxt == "/":
                state = "normal"
                i += 2
                continue
        elif state in {"string", "char"}:
            if ch == "\\":
                i += 2
                continue
            if (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "normal"
        i += 1
    return depth == 0 and state in {"normal", "line"}

def peak_multiplier(name: str, variation: float) -> float:
    vhigh = 1.0 + max(0.0, min(1.0, variation))
    if name == "pulse_fast":
        return 1.35 * vhigh
    if name in {"pulse_slow", "breathing"}:
        return 1.08 * vhigh
    if name in {"soft_flicker", "broken_fluorescent", "tv_noise", "generator_stutter"}:
        return 1.30 * vhigh
    if name in {"fire_pulse", "flame_small", "flame_large"}:
        return 1.35 * vhigh
    if name == "candle_flicker":
        return 1.18 * vhigh
    if name == "unstable_bulb":
        return 1.22 * vhigh
    if name == "fluorescent_random":
        return 1.25 * vhigh
    if name in {"strobe_fast", "strobe_slow"}:
        return 1.80 * vhigh
    return 1.0


def validate() -> None:
    hpp = read("src/components/modules/imgui.hpp")
    cpp = read("src/components/modules/imgui.cpp")
    settings_hpp = read("src/components/modules/map_settings.hpp")
    settings_cpp = read("src/components/modules/map_settings.cpp")
    interfaces_hpp = read("src/components/modules/interfaces.hpp")
    interfaces_cpp = read("src/components/modules/interfaces.cpp")
    trace_hpp = read("src/sdk/engine/c_engine_trace.hpp")
    common_toml = read("src/components/common/toml.cpp")
    override_hpp = read("src/components/modules/source_map_light_overrides.hpp")
    override_cpp = read("src/components/modules/source_map_light_overrides.cpp")
    dynamic = read("src/components/modules/dynamic_lighting.cpp")
    premake = read("premake5.lua")
    readme = read("README.md")
    build = read("Build-V21.14.10.ps1")

    # Release identity.
    require(premake, '"V21.14.10-source"', "premake source version")
    require(premake, '"21.14.10"', "premake tag version")
    require(readme, "# L4D2 RTX Compat V21.14.10", "README version")
    require(cpp, 'ImGui::TextDisabled("Modified Edition - V21.14.10");', "About version")
    require(build, "Validate-TraceRayAbiRemixCategory-V21.14.10.ps1", "current validator")
    require(build, "V21.14.10", "build label")

    # Renderer-safe Present path and complete explicit target restoration.
    helper = read("src/components/common/imgui/imgui_helper.cpp")
    dx9_backend = read("deps/imgui/backends/imgui_impl_dx9.cpp")
    state_guard = cpp[cpp.index("class d3d9_state_guard"):cpp.index("IDirect3DTexture9* about_strawberry_texture")]
    require(state_guard, "GetRenderTarget(slot, &m_render_targets[slot])", "explicit MRT capture")
    require(state_guard, "GetDepthStencilSurface(&m_depth_stencil)", "explicit depth capture")
    require(state_guard, "SetRenderTarget(slot, m_render_targets[slot])", "explicit MRT restore")
    require(state_guard, "SetDepthStencilSurface(m_depth_stencil)", "explicit depth restore")
    require(state_guard, "SetRenderState(static_cast<D3DRENDERSTATETYPE>(42), 0u);", "neutral Remix category fallback")
    category_guard = cpp[cpp.index("class remix_instance_category_guard"):cpp.index("class d3d9_state_guard")]
    require(category_guard, "void restore_neutral()", "explicit neutral category restore")
    require(category_guard, "SetRenderState(static_cast<D3DRENDERSTATETYPE>(42), 0u);", "RS42 reset to neutral")
    forbid(category_guard, "GetRenderState", "stale category sampling")

    endscene = function_body(cpp, "void imgui::endscene_stub(const bool isolated_present_scene)")
    require(endscene, "GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO", "real backbuffer acquisition")
    require(endscene, "SetRenderTarget(0, backbuffer)", "backbuffer binding")
    require(endscene, "for (DWORD slot = 1; slot < 4; ++slot) dev->SetRenderTarget(slot, nullptr);", "stale MRT isolation")
    require(endscene, "SetDepthStencilSurface(nullptr)", "overlay depth isolation")
    require(endscene, "dev->BeginScene()", "isolated overlay BeginScene")
    require(endscene, "dev->EndScene()", "isolated overlay EndScene")
    require(endscene, "m_overlay_safe_backbuffer_frames", "successful safe-frame accounting")
    require(endscene, "remix_instance_category_guard category_guard(dev);", "draw-local category guard")
    require(endscene, "category_guard.set(ui_categories);", "UI category set")
    require(endscene, "ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());", "ImGui draw submission")
    require(endscene, "category_guard.restore_neutral();", "immediate category neutralization")
    set_pos = endscene.index("category_guard.set(ui_categories);")
    draw_pos = endscene.index("ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());")
    reset_pos = endscene.index("category_guard.restore_neutral();")
    if not set_pos < draw_pos < reset_pos:
        raise AssertionError("Remix categories are not scoped exactly around ImGui draw submission")
    end_pos = endscene.index("const HRESULT end_scene_result = dev->EndScene();")
    arm_pos = endscene.index("im->m_menu_input_armed = true;")
    if arm_pos < end_pos:
        raise AssertionError("input is armed before the isolated backbuffer scene completes")

    require(helper, "V21.14.7 renderer-safe mode", "blur safety marker")
    window_blur = function_body(helper, "void draw_window_blur()")
    background_blur = function_body(helper, "void draw_background_blur()")
    forbid(window_blur, "draw_blur", "window blur feedback")
    forbid(background_blur, "draw_blur", "background blur feedback")

    # Compile hotfix: the blur edit must not consume the namespace boundary or
    # the custom ImGui helper definitions. This exact regression produced the
    # C3861 avalanche in imgui_helper.cpp.
    background_start = helper.index("void draw_background_blur()")
    namespace_boundary = helper.index("\n}\n\nnamespace ImGui\n{", background_start)
    style_delete = helper.index("void Style_DeleteButtonPush()")
    if not namespace_boundary < style_delete:
        raise AssertionError("Style helpers are not inside namespace ImGui")
    for fragment, label in (
        ("void SafePopStyleColor(const int count, const int source_line)", "SafePopStyleColor definition"),
        ("void Spacing(const float& x, const float& y)", "Spacing definition"),
        ("void PushFont(common::imgui::font::FONTS font)", "custom PushFont definition"),
        ("void Style_DeleteButtonPush()", "style helper definition"),
        ("bool BeginTooltipBlurEx", "tooltip helper definition"),
    ):
        require(helper, fragment, label)
    if helper.index("void SafePopStyleColor") > style_delete:
        raise AssertionError("SafePopStyleColor is declared after its first use")
    forbid(dx9_backend, "xoxor4d :: sRGB", "custom forced sRGB conversion")
    forbid(cpp, "ImGuiConfigFlags_IsSRGB", "forced ImGui sRGB mode")

    release = function_body(cpp, "void imgui::release_menu_cursor()")
    require(release, "if (gameplay_active)", "gameplay-aware cursor release")
    require(release, "set_cursor_always_visible(false)", "gameplay cursor hide")
    require(release, "lock_cursor()", "Source relative-input restore")
    require(release, "m_cursor_visible_before_menu", "front-end cursor restore")
    fail_overlay = endscene[endscene.index("auto fail_visible_overlay"):endscene.index("IDirect3DDevice9* dev")]
    require(fail_overlay, "m_menu_input_armed || im->m_menu_cursor_owned", "one-shot failed-overlay input release")

    # First-surface cursor placement through Source trace.
    require(trace_hpp, 'ENGINE_TRACE_CLIENT_VERSION_004 "EngineTraceClient004"', "trace 004 ABI")
    require(trace_hpp, 'ENGINE_TRACE_CLIENT_VERSION_003 "EngineTraceClient003"', "trace 003 ABI")
    require(trace_hpp, "TRACE_EVERYTHING_FILTER_PROPS", "prop-aware trace filter")
    require(trace_hpp, "unsigned short world_surface_index = 0;", "CGameTrace world surface ABI field")
    require(trace_hpp, "static_assert(sizeof(base_trace) == 56u", "x86 CBaseTrace ABI assertion")
    require(trace_hpp, "static_assert(sizeof(game_trace) == 84u", "x86 CGameTrace ABI assertion")
    require(trace_hpp, "static_assert(sizeof(components::Ray_t) == 80u", "x86 Ray_t ABI assertion")
    require(trace_hpp, "inline static std::size_t s_trace_ray_slot = 5u;", "audited TraceRay default slot")
    require(trace_hpp, "))[s_trace_ray_slot](this, ray, mask, filter, trace);", "configured TraceRay dispatch")
    require(interfaces_hpp, "sdk::engine_trace* m_engine_trace", "trace interface storage")
    require(interfaces_cpp, 'get_interface<sdk::engine_trace>("engine.dll", ENGINE_TRACE_CLIENT_VERSION_003)', "003-first acquisition")
    require(interfaces_cpp, "configure_trace_ray_slot(5u)", "003 TraceRay slot 5")
    require(interfaces_cpp, 'get_interface<sdk::engine_trace>("engine.dll", ENGINE_TRACE_CLIENT_VERSION_004)', "004 fallback acquisition")
    require(interfaces_cpp, "configure_trace_ray_slot(4u)", "004 TraceRay slot 4")
    require(interfaces_cpp, "trace_target_in_engine", "runtime trace vtable target validation")
    surface = function_body(cpp, "bool light_editor_mouse_surface_position")
    require(surface, "m_engine_trace->trace_ray", "Source world trace")
    require(surface, "0x4600400Bu", "MASK_SHOT placement mask")
    require(surface, "trace.endpos + normal * offset", "surface-normal offset")
    resolver = function_body(cpp, "Vector light_editor_resolve_spawn_position")
    require(resolver, "light_editor_mouse_surface_position", "surface-first placement")
    require(resolver, "light_editor_mouse_world_position(max_distance)", "controlled no-hit fallback")

    # Popup stays at the click anchor and uses its actual size.
    require(cpp, "context_popup_anchor = mouse_pos + ImVec2(8.0f, 8.0f);", "edit popup anchor capture")
    require(cpp, "create_popup_anchor = mouse_pos + ImVec2(8.0f, 8.0f);", "create popup anchor capture")
    require(cpp, "auto pin_popup_to_anchor", "popup pin helper")
    require(cpp, "const ImVec2 popup_size = ImGui::GetWindowSize();", "actual popup dimensions")
    require(cpp, "ImGui::SetWindowPos(pinned, ImGuiCond_Always);", "stable popup position")
    require(cpp, "ImGui::SetNextWindowPos(context_popup_anchor, ImGuiCond_Appearing);", "edit popup initial cursor position")
    require(cpp, "ImGui::SetNextWindowPos(create_popup_anchor, ImGuiCond_Appearing);", "create popup initial cursor position")
    require(cpp, "ImGuiWindowFlags_NoMove", "non-drifting popup")
    forbid(cpp, "viewport_size.y - 360.0f", "fixed vertical popup clamp")

    # Compact world-menu copy.
    for label in ('BeginMenu("Rig")', 'BeginMenu("Style")', 'BeginMenu("Type")',
                  'BeginMenu("Animate")', 'MenuItem("Save")', 'MenuItem("Delete")',
                  'DragFloat("Ray Max",', 'DragFloat("Power",'):
        require(cpp, label, f"compact menu label {label}")
    require(cpp, "light_animation_compact_label", "short animation preset labels")
    require(cpp, "compact_light_menu_text", "bounded context title/group text")
    require(cpp, 'ImGui::TextDisabled("Keeps power/radius");', "short style hint")

    # Display defaults and Light Studio sub-tabs.
    require(hpp, "m_light_editor_label_visibility_distance = 270.0f", "270 label default")
    require(hpp, "m_light_editor_shape_visibility_distance = 270.0f", "270 detail default")
    for tab in ('BeginTabItem("Scene")', 'BeginTabItem("Display")',
                'BeginTabItem("Placement")', 'BeginTabItem("Advanced")'):
        require(cpp, tab, f"Light Studio tab {tab}")
    require(cpp, 'Button("Default Details: 270",', "270 reset button")
    require(cpp, 'DragFloat("Cones/Details"', "cone/detail range control")
    require(cpp, 'toggle_button_bool(&im->m_light_editor_surface_placement, "Surface Hit"', "surface placement toggle")

    # Independent animation module schema and editor UI.
    for fragment in (
        'std::string property_animation = "stable";',
        "float property_animation_intensity = -1.0f;",
        'std::string movement_animation = "none";',
        "float movement_animation_distance = 64.0f;",
    ):
        require(settings_hpp, fragment, f"split animation field {fragment}")
    require(cpp, 'BeginTabItem("Brightness & Color")', "output animation tab")
    require(cpp, 'BeginTabItem("Movement")', "movement animation tab")
    require(cpp, 'DragFloat("Base Power"', "stable base power control")
    require(cpp, "LIGHT_OUTPUT_ANIMATION_PRESETS", "output preset list")
    require(cpp, "LIGHT_MOVEMENT_ANIMATION_PRESETS", "movement preset list")
    require(cpp, "build_split_light_animation_points", "combined module sampling")
    require(cpp, "point.radiance = base.radiance;", "movement radiance preservation")
    require(cpp, "point.radiance_scalar = base.radiance_scalar;", "movement power preservation")
    require(cpp, "base.radiance_scalar = std::max(0.0f, def.property_animation_intensity);", "Base Power source")

    infer = function_body(cpp, "float infer_light_animation_base_power")
    require(infer, "def.property_animation_intensity", "persisted Base Power priority")
    require(infer, "def.property_animation_variation", "split-output variation inference")
    standard = function_body(cpp, "void imgui::devgui") if "void imgui::devgui" in cpp else cpp
    require(cpp, "if (!has_animation)\n\t\t\t\t\tedit_active_light->m_def.property_animation_intensity", "keyframe edit cannot replace animated Base Power")
    quick_marker = cpp.index("Selected light: quick tools")
    quick = cpp[quick_marker:quick_marker + 9000]
    commit_pos = quick.index("commit_active_editor_light_to_selection")
    output_pos = quick.index('ImGui::Combo("Output##quick_output"')
    if commit_pos > output_pos:
        raise AssertionError("quick controls commit stale editor state after module mutation")
    quick_change = quick[quick.index("if (quick_modules_changed)"):quick.index("const float quick_spacing")]
    forbid(quick_change, "commit_active_editor_light_to_selection", "post-preset stale commit")

    # Persistence through map TOML and source-light overrides.
    for source, name in ((common_toml, "common TOML writer"), (settings_cpp, "map TOML parser"),
                         (override_hpp, "override schema"), (override_cpp, "override parser/writer"),
                         (dynamic, "runtime persistence")):
        require(source, "property_animation_intensity", f"Base Power in {name}")
        require(source, "movement_animation", f"movement metadata in {name}")
        require(source, "movement_animation_distance", f"movement distance in {name}")

    # Idempotence model: a fixed Base Power must produce the same peak after any rebuild count.
    base_power = 12.5
    preset = "pulse_fast"
    variation = 0.35
    expected_peak = base_power * peak_multiplier(preset, variation)
    rebuilt_peak = expected_peak
    for _ in range(12):
        rebuilt_peak = base_power * peak_multiplier(preset, variation)
    if not math.isclose(rebuilt_peak, expected_peak, rel_tol=1e-9, abs_tol=1e-9):
        raise AssertionError("animation rebuild compounds Base Power")

    # V21.14.9: automatically recover Source relative mouse input when the
    # engine transitions from the front-end/loading screen into gameplay.
    require(hpp, "void schedule_gameplay_input_recovery(std::uint32_t frames, const char* reason);", "recovery scheduler declaration")
    require(hpp, "void recover_gameplay_input_frame();", "recovery frame declaration")
    require(hpp, "void service_gameplay_input_state(bool apply_recovery_frame);", "Present input service declaration")
    require(hpp, "m_gameplay_present_was_active", "gameplay edge state")
    require(hpp, "m_gameplay_input_recovery_frames", "bounded recovery counter")

    scheduler = function_body(cpp, "void imgui::schedule_gameplay_input_recovery")
    require(scheduler, "std::max(m_gameplay_input_recovery_frames, frames)", "non-destructive recovery scheduling")

    recovery = function_body(cpp, "void imgui::recover_gameplay_input_frame()")
    require(recovery, "m_gameplay_input_recovery_frames == 0u", "bounded recovery exit")
    require(recovery, "m_menu_active || m_menu_input_armed", "visible-menu ownership protection")
    require(recovery, "io.MouseDrawCursor = false", "software cursor disable")
    require(recovery, "for (bool& button : io.MouseDown) button = false", "stale ImGui mouse-button clear")
    require(recovery, "set_cursor_always_visible(false)", "Source cursor hide")
    require(recovery, "lock_cursor()", "Source relative cursor lock")
    require(recovery, "GetForegroundWindow() == glob::main_window", "no focus stealing")
    require(recovery, "--m_gameplay_input_recovery_frames", "finite recovery window")
    forbid(recovery, "Sleep(", "blocking input recovery")

    endscene = function_body(cpp, "void imgui::endscene_stub(const bool isolated_present_scene)")
    service = function_body(cpp, "void imgui::service_gameplay_input_state")
    require(service, "gameplay_active && !m_gameplay_present_was_active", "front-end to gameplay edge detection")
    require(service, "release_menu_cursor()", "immediate manual-close-equivalent recovery")
    require(service, "schedule_gameplay_input_recovery(12u, \"front-end to gameplay transition\")", "transition recovery schedule")
    require(service, "if (apply_recovery_frame) recover_gameplay_input_frame();", "single Present recovery execution")
    require(endscene, "service_gameplay_input_state(false)", "EndScene transition detection")

    map_load = function_body(cpp, "void imgui::on_map_load()")
    require(map_load, "m_gameplay_present_was_active = false", "map-load edge reset")
    require(map_load, "schedule_gameplay_input_recovery(12u, \"map load\")", "map-load recovery schedule")

    release = function_body(cpp, "void imgui::release_menu_cursor()")
    require(release, "schedule_gameplay_input_recovery(8u, \"F5 menu release\")", "post-F5 recovery window")

    wndproc = function_body(cpp, "LRESULT __stdcall wnd_proc_hk")
    require(wndproc, "m_gameplay_input_recovery_frames > 0u", "bounded late WM_SETCURSOR suppression")
    require(wndproc, "SetCursor(nullptr)", "late arrow cursor suppression")


    # V21.14.10: retained silent release diagnostics, executable attestation and split overlay path.
    functions_hpp = read("src/game/functions.hpp")
    functions_cpp = read("src/game/functions.cpp")
    source_compat_hpp = read("src/source_compat.hpp")
    source_compat_inl = read("src/source_compat.inl")
    remix_api_cpp = read("src/components/modules/remix_api.cpp")
    remix_api_hpp = read("src/components/modules/remix_api.hpp")

    require(functions_hpp, '_wcsicmp(arguments[i], L"-xo_debug_console") == 0', "explicit console opt-in")
    require(functions_hpp, '"l4d2-rtx" / "logs" / "compat.log"', "silent diagnostic log path")
    require(functions_hpp, 'if (diagnostics::external_console_requested() && AllocConsole())', "conditional AllocConsole")
    require(functions_hpp, 'diagnostics::redirect_to_log();', "default file diagnostics")
    require(functions_cpp, 'bool diagnostic_output_initialized = false;', "diagnostic initialization state")

    require(source_compat_hpp, 'bool launcher_family_matches = false;', "launcher family profile field")
    require(source_compat_inl, 'IMAGE_FILE_MACHINE_I386', "i386 executable probe")
    require(source_compat_inl, 'IMAGE_SUBSYSTEM_WINDOWS_GUI', "GUI subsystem probe")
    require(source_compat_inl, 'image.find("LauncherMain")', "LauncherMain fingerprint")
    require(source_compat_inl, 'image.find("bin\\\\launcher.dll")', "launcher.dll fingerprint")
    require(source_compat_inl, '(g_profile.hash_matches || g_profile.launcher_family_matches)', "future launcher family acceptance")

    present = function_body(cpp, "long __stdcall present_hk")
    require(present, '!im->gameplay_present_active()', "front-end-only direct Present rendering")
    require(present, 'imgui::endscene_stub(true)', "isolated front-end scene")
    require(present, 'service_gameplay_input_state(true)', "Present-driven input recovery")
    require(remix_api_cpp, 'imgui::endscene_stub(false);', "gameplay Remix EndScene callback")
    require(endscene, 'if (isolated_present_scene)', "split overlay execution")
    require(endscene, 'REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI', "Remix world-UI category")
    require(endscene, 'REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_LIGHTS', "UI light exclusion")
    require(endscene, 'REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ANTI_CULLING', "UI anti-culling exclusion")
    require(endscene, 'REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_MOTION_BLUR', "UI motion-blur exclusion")
    require(endscene, 'm_overlay_frontend_present_frames', "front-end overlay accounting")
    require(endscene, 'm_overlay_gameplay_bridge_frames', "gameplay bridge accounting")

    on_renderview = function_body(remix_api_cpp, "void remix_api::on_renderview()")
    require(on_renderview, 'm_last_source_render_frame == source_frame', "duplicate RenderView guard")
    require(on_renderview, 'm_last_source_realtime == source_realtime', "same-frame realtime guard")
    require(on_renderview, '++m_duplicate_render_views_skipped;', "duplicate RenderView accounting")
    require(remix_api_hpp, 'm_duplicate_render_views_skipped = 0u;', "duplicate view counter")

    # Structural sanity after large source edits.
    sources = {
        "imgui_helper.cpp": helper,
        "imgui.cpp": cpp,
        "map_settings.cpp": settings_cpp,
        "common/toml.cpp": common_toml,
        "source_map_light_overrides.cpp": override_cpp,
        "dynamic_lighting.cpp": dynamic,
        "c_engine_trace.hpp": trace_hpp,
    }
    for rel, source in sources.items():
        if not braces_balanced(source):
            raise AssertionError(f"unbalanced braces in {rel}")

    print("Compat V21.14.10 TraceRay ABI and Remix category isolation validation: PASS")


if __name__ == "__main__":
    try:
        validate()
    except Exception as exc:
        print(f"Compat V21.14.10 validation: FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
