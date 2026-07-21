// V20.0.2: compiled from main.cpp so old generated .vcxproj files do not miss the implementation.
// Keep this file implementation-only; declarations remain in source_compat.hpp.

namespace source_compat
{
    namespace
    {
        runtime_profile g_profile {};
        bool g_detected = false;

        constexpr const char* k_overlay_class_name = "L4D2RTX_SourceCompatOverlay_V2002";
        HANDLE g_overlay_thread = nullptr;
        HANDLE g_overlay_stop_event = nullptr;
        HWND g_overlay_window = nullptr;
        DWORD g_overlay_thread_id = 0u;

        LRESULT CALLBACK generic_overlay_wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
        {
            switch (message)
            {
            case WM_CLOSE:
                ShowWindow(hwnd, SW_HIDE);
                return 0;

            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT:
            {
                PAINTSTRUCT ps {};
                HDC dc = BeginPaint(hwnd, &ps);
                RECT client {};
                GetClientRect(hwnd, &client);

                const HBRUSH background = CreateSolidBrush(RGB(20, 24, 30));
                FillRect(dc, &client, background);
                DeleteObject(background);

                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(225, 232, 240));

                HFONT title_font = CreateFontA(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
                HFONT body_font = CreateFontA(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

                const auto old_font = SelectObject(dc, title_font);
                RECT title_rect { 20, 16, client.right - 20, 52 };
                DrawTextA(dc, "Source Compatibility Control", -1, &title_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

                SelectObject(dc, body_font);
                int y = 62;
                const int line_height = 23;
                auto draw_line = [&](const std::string& label, const COLORREF color = RGB(225, 232, 240))
                {
                    SetTextColor(dc, color);
                    RECT line_rect { 22, y, client.right - 22, y + line_height + 4 };
                    DrawTextA(dc, label.c_str(), -1, &line_rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                    y += line_height;
                };

                draw_line(std::format("Game: {}", game_name()));
                draw_line(std::format("Mode: {}", mode_name()));
                draw_line(std::format("Remix API: {}", components::remix_api::is_initialized() ? "initialized" : "waiting / unavailable"),
                    components::remix_api::is_initialized() ? RGB(120, 225, 150) : RGB(245, 190, 95));

                if (uses_l4d2_runtime())
                {
                    draw_line("L4D2 binary hooks: ENABLED (unsafe outside native L4D2)", RGB(255, 105, 105));
                }
                else
                {
                    draw_line("L4D2 binary hooks and fixed offsets: disabled", RGB(120, 225, 150));
                    draw_line(uses_l4d1_bootstrap()
                        ? "L4D1 native rendering/gameplay code: untouched by bootstrap"
                        : "Native game flashlight: untouched by the ASI compatibility core", RGB(120, 225, 150));
                }

                if (uses_l4d1_bootstrap())
                {
                    const auto& report = l4d1::get_bootstrap_report();
                    draw_line("L4D1 Source interfaces + Remix bridge: enabled", RGB(145, 195, 255));
                    draw_line(std::format("Read-only L4D1 data sites: {}/{}", report.resolved_data_sites, report.total_data_sites),
                        report.resolved_data_sites == report.total_data_sites ? RGB(120, 225, 150) : RGB(245, 190, 95));
                    draw_line(std::format("Observed hook candidates kept disabled: {}", report.disabled_hook_candidates), RGB(245, 190, 95));
                }
                else if (is_hl2_hybrid_experiment())
                {
                    draw_line("Hybrid experiment: Remix bridge + compatibility diagnostics only.", RGB(145, 195, 255));
                    draw_line("Full L4D2 runtime now requires TWO explicit unsafe launch switches.", RGB(245, 190, 95));
                }
                else if (is_generic_source())
                {
                    draw_line("Safe Generic Source core is active.", RGB(145, 195, 255));
                }

                draw_line("This control window is not yet the full L4D2 Light Studio.", RGB(205, 205, 215));
                draw_line("F5: hide/show this window", RGB(205, 205, 215));

                SelectObject(dc, old_font);
                DeleteObject(title_font);
                DeleteObject(body_font);
                EndPaint(hwnd, &ps);
                return 0;
            }

            default:
                return DefWindowProcA(hwnd, message, wparam, lparam);
            }
        }

        DWORD WINAPI generic_overlay_thread_proc(LPVOID)
        {
            g_overlay_thread_id = GetCurrentThreadId();

            WNDCLASSEXA wc {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = generic_overlay_wndproc;
            wc.hInstance = GetModuleHandleA(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
            wc.hbrBackground = nullptr;
            wc.lpszClassName = k_overlay_class_name;
            RegisterClassExA(&wc);

            RECT game_rect { 80, 80, 1280, 720 };
            if (glob::main_window) {
                GetWindowRect(glob::main_window, &game_rect);
            }

            const int width = 660;
            const int height = 330;
            const int x = game_rect.left + 28;
            const int y = game_rect.top + 48;

            g_overlay_window = CreateWindowExA(
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                k_overlay_class_name,
                "RTX Source Compatibility",
                WS_POPUP | WS_CAPTION | WS_SYSMENU,
                x, y, width, height,
                glob::main_window,
                nullptr,
                GetModuleHandleA(nullptr),
                nullptr);

            if (!g_overlay_window)
            {
                UnregisterClassA(k_overlay_class_name, GetModuleHandleA(nullptr));
                return 0;
            }

            ShowWindow(g_overlay_window, SW_SHOWNOACTIVATE);
            UpdateWindow(g_overlay_window);

            bool f5_was_down = false;
            ULONGLONG last_repaint = 0u;

            while (WaitForSingleObject(g_overlay_stop_event, 16u) == WAIT_TIMEOUT)
            {
                MSG message {};
                while (PeekMessageA(&message, nullptr, 0u, 0u, PM_REMOVE))
                {
                    if (message.message == WM_QUIT) {
                        goto overlay_exit;
                    }
                    TranslateMessage(&message);
                    DispatchMessageA(&message);
                }

                const bool f5_down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
                if (f5_down && !f5_was_down && g_overlay_window)
                {
                    const bool visible = IsWindowVisible(g_overlay_window) != FALSE;
                    ShowWindow(g_overlay_window, visible ? SW_HIDE : SW_SHOWNOACTIVATE);
                    if (!visible) {
                        InvalidateRect(g_overlay_window, nullptr, FALSE);
                    }
                }
                f5_was_down = f5_down;

                const ULONGLONG now = GetTickCount64();
                if (g_overlay_window && now - last_repaint >= 500u)
                {
                    // Generic Source has no game-specific RenderView hook that would
                    // retry Remix API initialization. Retry here without touching any
                    // L4D2 address, so a bridge loaded after the ASI can still connect.
                    if (auto* api = components::remix_api::get(); api && !components::remix_api::is_initialized()) {
                        api->try_initialize(false);
                    }

                    InvalidateRect(g_overlay_window, nullptr, FALSE);
                    last_repaint = now;
                }
            }

        overlay_exit:
            if (g_overlay_window)
            {
                DestroyWindow(g_overlay_window);
                g_overlay_window = nullptr;
            }
            UnregisterClassA(k_overlay_class_name, GetModuleHandleA(nullptr));
            g_overlay_thread_id = 0u;
            return 0;
        }

        std::string lower_ascii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::wstring lower_ascii(std::wstring value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t c)
            {
                return static_cast<wchar_t>(std::towlower(c));
            });
            return value;
        }

        std::vector<std::wstring> command_line_arguments()
        {
            std::vector<std::wstring> result;
            int count = 0;
            auto* argv = CommandLineToArgvW(GetCommandLineW(), &count);
            if (!argv) {
                return result;
            }

            result.reserve(static_cast<std::size_t>(std::max(count, 0)));
            for (int i = 0; i < count; ++i) {
                result.emplace_back(argv[i]);
            }
            LocalFree(argv);
            return result;
        }

        bool has_argument(const std::vector<std::wstring>& args, const wchar_t* argument)
        {
            const auto wanted = lower_ascii(std::wstring(argument));
            return std::ranges::any_of(args, [&](const std::wstring& value)
            {
                return lower_ascii(value) == wanted;
            });
        }

        std::string find_game_argument(const std::vector<std::wstring>& args)
        {
            for (std::size_t i = 0; i + 1u < args.size(); ++i)
            {
                if (lower_ascii(args[i]) == L"-game") {
                    return lower_ascii(utils::convert_wstring(args[i + 1u]));
                }
            }
            return {};
        }

        struct executable_probe
        {
            bool pe32_i386_gui = false;
            bool launcher_main_reference = false;
            bool launcher_dll_reference = false;

            bool source_launcher_family() const
            {
                return pe32_i386_gui && launcher_main_reference && launcher_dll_reference;
            }
        };

        executable_probe probe_executable(const std::string& executable_path)
        {
            executable_probe result {};
            std::ifstream file(executable_path, std::ios::binary | std::ios::ate);
            if (!file) return result;

            const auto length = file.tellg();
            if (length <= 0 || length > static_cast<std::streamoff>(16u * 1024u * 1024u)) return result;
            std::vector<char> bytes(static_cast<std::size_t>(length));
            file.seekg(0, std::ios::beg);
            if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return result;

            if (bytes.size() >= sizeof(IMAGE_DOS_HEADER))
            {
                IMAGE_DOS_HEADER dos {};
                std::memcpy(&dos, bytes.data(), sizeof(dos));
                const std::size_t nt_offset = dos.e_lfanew > 0 ? static_cast<std::size_t>(dos.e_lfanew) : bytes.size();
                if (dos.e_magic == IMAGE_DOS_SIGNATURE && nt_offset + sizeof(IMAGE_NT_HEADERS32) <= bytes.size())
                {
                    IMAGE_NT_HEADERS32 nt {};
                    std::memcpy(&nt, bytes.data() + nt_offset, sizeof(nt));
                    result.pe32_i386_gui = nt.Signature == IMAGE_NT_SIGNATURE &&
                        nt.FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
                        nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
                        nt.OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI;
                }
            }

            const std::string_view image(bytes.data(), bytes.size());
            result.launcher_main_reference = image.find("LauncherMain") != std::string_view::npos;
            result.launcher_dll_reference =
                image.find("bin\\launcher.dll") != std::string_view::npos ||
                image.find("bin/launcher.dll") != std::string_view::npos;
            return result;
        }

        game_id identify_game(const std::string& executable_path, const std::string& executable_name,
            const std::string& game_argument)
        {
            const auto path = lower_ascii(executable_path);
            const auto exe = lower_ascii(executable_name);
            const auto game = lower_ascii(game_argument);

            if (exe == "left4dead.exe") {
                return game_id::left_4_dead;
            }
            if (exe == "left4dead2.exe") {
                return game_id::left_4_dead_2;
            }

            if (exe == "hl2.exe")
            {
                if (game.find("portal") != std::string::npos || path.find("\\portal\\") != std::string::npos ||
                    path.find("/portal/") != std::string::npos) {
                    return game_id::portal;
                }
                if (game.find("ep2") != std::string::npos || path.find("\\ep2\\") != std::string::npos ||
                    path.find("/ep2/") != std::string::npos) {
                    return game_id::half_life_2_episode_two;
                }
                if (game.find("episodic") != std::string::npos || path.find("\\episodic\\") != std::string::npos ||
                    path.find("/episodic/") != std::string::npos) {
                    return game_id::half_life_2_episode_one;
                }
                return game_id::half_life_2;
            }

            return game_id::unknown;
        }

        struct window_candidate
        {
            DWORD process_id = 0;
            HWND window = nullptr;
            std::uint64_t area = 0u;
        };

        BOOL CALLBACK enum_windows_callback(const HWND hwnd, const LPARAM parameter)
        {
            auto* candidate = reinterpret_cast<window_candidate*>(parameter);
            if (!candidate || hwnd == GetConsoleWindow() || !IsWindowVisible(hwnd) ||
                GetWindow(hwnd, GW_OWNER) != nullptr) {
                return TRUE;
            }

            DWORD process_id = 0;
            GetWindowThreadProcessId(hwnd, &process_id);
            if (process_id != candidate->process_id) {
                return TRUE;
            }

            RECT rect {};
            if (!GetClientRect(hwnd, &rect)) {
                return TRUE;
            }

            const auto width = static_cast<std::uint64_t>(std::max<LONG>(0, rect.right - rect.left));
            const auto height = static_cast<std::uint64_t>(std::max<LONG>(0, rect.bottom - rect.top));
            const auto area = width * height;
            if (area > candidate->area)
            {
                candidate->area = area;
                candidate->window = hwnd;
            }
            return TRUE;
        }
    }

    void detect(const char* executable_path, const std::string& executable_sha1)
    {
        g_profile = {};
        g_profile.executable_path = executable_path ? executable_path : "";
        g_profile.executable_name = std::filesystem::path(g_profile.executable_path).filename().string();
        g_profile.executable_sha1 = lower_ascii(executable_sha1);

        const auto args = command_line_arguments();
        g_profile.game_argument = find_game_argument(args);
        g_profile.game = identify_game(g_profile.executable_path, g_profile.executable_name, g_profile.game_argument);

        const auto executable_identity = probe_executable(g_profile.executable_path);
        g_profile.pe32_i386_gui = executable_identity.pe32_i386_gui;
        g_profile.launcher_main_export_reference = executable_identity.launcher_main_reference;
        g_profile.launcher_dll_reference = executable_identity.launcher_dll_reference;
        g_profile.launcher_family_matches = executable_identity.source_launcher_family();

        if (g_profile.game == game_id::left_4_dead) {
            // Submitted Steam L4D1 launcher build (2024-10-19 PE timestamp).
            g_profile.expected_sha1 = "098d07422acc07d560315fafebd021ea661121cd";
        }
        else if (g_profile.game == game_id::left_4_dead_2) {
            g_profile.expected_sha1 = "007f496dbac6c45a450e8966958cb741acee6702";
        }
        g_profile.hash_matches = !g_profile.expected_sha1.empty() &&
            g_profile.executable_sha1 == g_profile.expected_sha1;

        const bool force_generic = has_argument(args, L"-xo_source_generic");
        const bool force_l4d2 = has_argument(args, L"-xo_force_l4d2_profile");
        const bool really_force_l4d2 = has_argument(args, L"-xo_really_force_l4d2_runtime");
        const bool allow_unknown = has_argument(args, L"-xo_allow_unsupported_source");
        const bool disable_overlay = has_argument(args, L"-xo_disable_source_overlay");

        g_profile.unsafe_full_runtime_requested = force_l4d2 && really_force_l4d2;

        if (force_generic)
        {
            g_profile.mode = runtime_mode::generic_source;
        }
        else if (g_profile.game == game_id::left_4_dead &&
            (g_profile.hash_matches || g_profile.launcher_family_matches))
        {
            // L4D1 receives only the audited Source-interface/Remix bootstrap. It is
            // intentionally isolated from every L4D2 binary hook and fixed offset.
            g_profile.mode = runtime_mode::native_l4d1_bootstrap;
        }
        else if (g_profile.game == game_id::left_4_dead)
        {
            // A renamed or structurally unexpected executable is kept in the generic
            // no-interface core instead of receiving even the L4D1 bootstrap.
            g_profile.mode = runtime_mode::generic_source;
        }
        else if (g_profile.game == game_id::left_4_dead_2 &&
            (g_profile.hash_matches || g_profile.launcher_family_matches))
        {
            // Accept the exact known build or a future Steam launcher from the same
            // PE32 GUI family. Renderer addresses are still independently validated
            // in shaderapidx9/engine/client/studiorender before hooks are installed.
            g_profile.mode = runtime_mode::native_l4d2;
        }
        else if (g_profile.game == game_id::left_4_dead_2)
        {
            // A file named left4dead2.exe without the expected launcher identity is
            // not allowed to receive binary L4D2 hooks. Keep only the safe core.
            g_profile.mode = runtime_mode::generic_source;
        }
        else if (force_l4d2 && really_force_l4d2)
        {
            // The old V20.0 behavior is still available, but now requires a second
            // deliberate switch so a single copied launch option cannot apply L4D2
            // addresses to HL2 by accident.
            g_profile.mode = runtime_mode::forced_l4d2_experimental;
        }
        else if (force_l4d2)
        {
            // Safe hybrid experiment: keep the Remix bridge and diagnostics, but do
            // not instantiate any component that dereferences L4D2 offsets.
            g_profile.mode = runtime_mode::hl2_hybrid_experimental;
        }
        else if (g_profile.game != game_id::unknown || allow_unknown)
        {
            g_profile.mode = runtime_mode::generic_source;
        }
        else
        {
            g_profile.mode = runtime_mode::blocked;
        }

        g_profile.generic_overlay_enabled = !disable_overlay &&
            (g_profile.mode == runtime_mode::native_l4d1_bootstrap ||
             g_profile.mode == runtime_mode::generic_source ||
             g_profile.mode == runtime_mode::hl2_hybrid_experimental);

        g_detected = true;
    }

    const runtime_profile& get()
    {
        return g_profile;
    }

    bool should_load()
    {
        return g_detected && g_profile.mode != runtime_mode::blocked;
    }

    bool uses_l4d2_runtime()
    {
        return g_profile.mode == runtime_mode::native_l4d2 ||
            g_profile.mode == runtime_mode::forced_l4d2_experimental;
    }

    bool uses_l4d1_bootstrap()
    {
        return g_profile.mode == runtime_mode::native_l4d1_bootstrap;
    }

    bool is_native_l4d1()
    {
        return g_profile.game == game_id::left_4_dead && uses_l4d1_bootstrap();
    }

    bool is_native_l4d2()
    {
        return g_profile.mode == runtime_mode::native_l4d2;
    }

    bool is_generic_source()
    {
        return g_profile.mode == runtime_mode::generic_source;
    }

    bool is_hl2_hybrid_experiment()
    {
        return g_profile.mode == runtime_mode::hl2_hybrid_experimental;
    }

    bool is_forced_l4d2_experiment()
    {
        return g_profile.mode == runtime_mode::forced_l4d2_experimental;
    }

    bool uses_generic_overlay()
    {
        return g_profile.generic_overlay_enabled;
    }

    const char* game_name()
    {
        switch (g_profile.game)
        {
        case game_id::left_4_dead: return "Left 4 Dead";
        case game_id::left_4_dead_2: return "Left 4 Dead 2";
        case game_id::half_life_2: return "Half-Life 2";
        case game_id::half_life_2_episode_one: return "Half-Life 2: Episode One";
        case game_id::half_life_2_episode_two: return "Half-Life 2: Episode Two";
        case game_id::portal: return "Portal";
        default: return "Unknown Source game";
        }
    }

    const char* mode_name()
    {
        switch (g_profile.mode)
        {
        case runtime_mode::native_l4d1_bootstrap: return "Native L4D1 bootstrap (safe/no binary hooks)";
        case runtime_mode::native_l4d2: return "Native L4D2";
        case runtime_mode::generic_source: return "Generic Source (safe core)";
        case runtime_mode::hl2_hybrid_experimental: return "HL2 hybrid experiment (safe hooks only)";
        case runtime_mode::forced_l4d2_experimental: return "Forced L4D2 runtime (DOUBLE-UNSAFE)";
        default: return "Blocked";
        }
    }

    std::string make_window_title()
    {
        if (is_native_l4d2())
        {
#ifdef GIT_DESCRIBE
            return std::format("Left 4 Dead 2 - RTX - {}", GIT_DESCRIBE);
#else
            return "Left 4 Dead 2 - RTX";
#endif
        }

        if (is_native_l4d1()) {
            return "Left 4 Dead - RTX Source Compatibility (Bootstrap)";
        }
        if (is_forced_l4d2_experiment()) {
            return std::format("{} - RTX - L4D2 runtime DOUBLE-UNSAFE", game_name());
        }
        if (is_hl2_hybrid_experiment()) {
            return std::format("{} - RTX Source Compatibility (Hybrid Safe)", game_name());
        }
        if (is_generic_source()) {
            return std::format("{} - RTX Source Compatibility (Generic)", game_name());
        }
        return std::format("{} - RTX Compatibility", game_name());
    }

    HWND find_main_window_for_current_process()
    {
        // Preserve the exact legacy lookup first for native L4D2 installations.
        if (is_native_l4d2())
        {
            if (const auto legacy_window = FindWindowA(nullptr, WINDOW_TITLE_STR); legacy_window) {
                return legacy_window;
            }
        }

        window_candidate candidate {};
        candidate.process_id = GetCurrentProcessId();
        EnumWindows(enum_windows_callback, reinterpret_cast<LPARAM>(&candidate));
        return candidate.window;
    }

    void print_startup_report()
    {
        game::console();
        std::cout << "[SourceCompat] Detected game: " << game_name() << std::endl;
        std::cout << "[SourceCompat] Executable: " << g_profile.executable_path << std::endl;
        std::cout << "[SourceCompat] SHA-1: " <<
            (g_profile.executable_sha1.empty() ? "<unavailable>" : g_profile.executable_sha1) << std::endl;
        std::cout << "[SourceCompat] Runtime mode: " << mode_name() << std::endl;
        std::cout << "[SourceCompat] Launcher identity: PE32/i386/GUI=" << g_profile.pe32_i386_gui
            << " LauncherMain=" << g_profile.launcher_main_export_reference
            << " launcher.dll=" << g_profile.launcher_dll_reference << std::endl;

        if (is_native_l4d1())
        {
            std::cout << "[SourceCompat] L4D1 bootstrap enabled: Source interfaces, Remix API callbacks and read-only diagnostics." << std::endl;
            std::cout << "[SourceCompat] L4D2 fixed offsets, naked-assembly stubs, renderer patches and gameplay hooks remain disabled." << std::endl;
            std::cout << "[SourceCompat] Press F5 for the L4D1 compatibility status window." << std::endl;
        }
        if (is_native_l4d2() && !g_profile.hash_matches)
        {
            std::cout << "[SourceCompat][WARN] L4D2 executable hash differs from the reference build. "
                "Loading is still allowed; offsets remain version-sensitive." << std::endl;
        }
        if (is_generic_source())
        {
            std::cout << "[SourceCompat] L4D2 offsets, interfaces, flashlight replacement and binary hooks are disabled." << std::endl;
            std::cout << "[SourceCompat] Safe core enabled: Remix API initialization and compatibility diagnostics." << std::endl;
            std::cout << "[SourceCompat] Press F5 for the Source Compatibility control window." << std::endl;
            std::cout << "[SourceCompat] Add -xo_force_l4d2_profile for the safe hybrid experiment." << std::endl;
        }
        if (is_hl2_hybrid_experiment())
        {
            std::cout << "[SourceCompat] Safe hybrid selected: no L4D2 fixed addresses will be executed." << std::endl;
            std::cout << "[SourceCompat] The ASI does not replace or update the native HL2 flashlight in this mode." << std::endl;
            std::cout << "[SourceCompat] Press F5 for the Source Compatibility control window." << std::endl;
            std::cout << "[SourceCompat][WARN] The complete legacy path now requires BOTH:" << std::endl;
            std::cout << "[SourceCompat][WARN] -xo_force_l4d2_profile -xo_really_force_l4d2_runtime" << std::endl;
        }
        if (is_forced_l4d2_experiment())
        {
            std::cout << "[SourceCompat][DANGER] Two-switch override accepted. L4D2 addresses and hook layout are being applied to a different executable." << std::endl;
            std::cout << "[SourceCompat][DANGER] A crash or corrupted rendering is expected until an actual game profile is ported." << std::endl;
            std::cout << "[SourceCompat][DANGER] ASI flashlight replacement remains disabled outside native L4D2." << std::endl;
        }
    }

    void start_generic_overlay()
    {
        if (!uses_generic_overlay() || g_overlay_thread) {
            return;
        }

        g_overlay_stop_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!g_overlay_stop_event) {
            return;
        }

        g_overlay_thread = CreateThread(nullptr, 0u, generic_overlay_thread_proc, nullptr, 0u, nullptr);
        if (!g_overlay_thread)
        {
            CloseHandle(g_overlay_stop_event);
            g_overlay_stop_event = nullptr;
        }
    }

    void stop_generic_overlay()
    {
        if (g_overlay_stop_event) {
            SetEvent(g_overlay_stop_event);
        }
        if (g_overlay_thread_id) {
            PostThreadMessageA(g_overlay_thread_id, WM_QUIT, 0u, 0u);
        }

        if (g_overlay_thread)
        {
            // The code lives in this DLL, so do not let the overlay thread continue
            // after unload. It normally leaves within one 16 ms polling interval.
            WaitForSingleObject(g_overlay_thread, 1500u);
            CloseHandle(g_overlay_thread);
            g_overlay_thread = nullptr;
        }
        if (g_overlay_stop_event)
        {
            CloseHandle(g_overlay_stop_event);
            g_overlay_stop_event = nullptr;
        }
    }
}
