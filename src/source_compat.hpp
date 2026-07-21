#pragma once

namespace source_compat
{
    enum class game_id
    {
        unknown,
        left_4_dead,
        left_4_dead_2,
        half_life_2,
        half_life_2_episode_one,
        half_life_2_episode_two,
        portal,
    };

    enum class runtime_mode
    {
        blocked,
        native_l4d1_bootstrap,
        native_l4d2,
        generic_source,
        hl2_hybrid_experimental,
        forced_l4d2_experimental,
    };

    struct runtime_profile
    {
        game_id game = game_id::unknown;
        runtime_mode mode = runtime_mode::blocked;
        std::string executable_path;
        std::string executable_name;
        std::string executable_sha1;
        std::string expected_sha1;
        std::string game_argument;
        bool hash_matches = false;
        bool pe32_i386_gui = false;
        bool launcher_main_export_reference = false;
        bool launcher_dll_reference = false;
        bool launcher_family_matches = false;
        bool generic_overlay_enabled = false;
        bool unsafe_full_runtime_requested = false;
    };

    void detect(const char* executable_path, const std::string& executable_sha1);
    const runtime_profile& get();

    bool should_load();
    bool uses_l4d2_runtime();
    bool uses_l4d1_bootstrap();
    bool is_native_l4d1();
    bool is_native_l4d2();
    bool is_generic_source();
    bool is_hl2_hybrid_experiment();
    bool is_forced_l4d2_experiment();
    bool uses_generic_overlay();

    const char* game_name();
    const char* mode_name();
    std::string make_window_title();

    HWND find_main_window_for_current_process();
    void print_startup_report();

    void start_generic_overlay();
    void stop_generic_overlay();
}
