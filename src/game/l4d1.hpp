#pragma once

namespace l4d1
{
    struct bootstrap_report
    {
        bool initialized = false;
        bool current_view_origin_resolved = false;
        bool current_view_forward_resolved = false;
        bool modelinfo_global_resolved = false;
        std::size_t resolved_data_sites = 0u;
        std::size_t total_data_sites = 3u;
        std::size_t disabled_hook_candidates = 0u;
    };

    // Resolves only uniquely matching global-data references needed by future
    // L4D1 work. It never installs a hook, modifies executable code or enables
    // an L4D2 naked-assembly stub.
    bool init_bootstrap_addresses();
    const bootstrap_report& get_bootstrap_report();
}
