#include "std_include.hpp"

namespace l4d1
{
    namespace
    {
        bootstrap_report g_report {};

        bool address_inside_module(const DWORD address, const utils::mem::module_info& module,
            const std::size_t bytes = sizeof(DWORD))
        {
            if (!module.handle || !module.size || address < module.handle) return false;
            const auto begin = static_cast<std::uint64_t>(module.handle);
            const auto end = begin + static_cast<std::uint64_t>(module.size);
            const auto value = static_cast<std::uint64_t>(address);
            return value <= end && bytes <= end - value;
        }

        template <typename T>
        bool resolve_embedded_global(utils::mem::module_info& module, const std::string_view signature,
            const DWORD immediate_offset, T*& destination, const char* description)
        {
            destination = nullptr;
            const auto matches = utils::mem::find_pattern_matches(module, signature, immediate_offset);
            if (matches.size() != 1u)
            {
                game::console();
                std::cout << "[L4D1][Bootstrap] " << description << ": expected one signature match, found "
                    << matches.size() << ". Site remains disabled." << std::endl;
                return false;
            }

            const auto operand = matches.front();
            if (!address_inside_module(operand, module, sizeof(DWORD)))
            {
                game::console();
                std::cout << "[L4D1][Bootstrap] " << description
                    << ": immediate operand is outside the scanned module." << std::endl;
                return false;
            }

            const auto global_address = *reinterpret_cast<const DWORD*>(operand);
            if (!address_inside_module(global_address, module, sizeof(T)))
            {
                game::console();
                std::cout << "[L4D1][Bootstrap] " << description
                    << ": resolved global is outside the owning module." << std::endl;
                return false;
            }

            destination = reinterpret_cast<T*>(global_address);
            std::cout << "[L4D1][Bootstrap] " << description << " resolved at RVA 0x"
                << std::hex << (global_address - module.handle) << std::dec << '.' << std::endl;
            return true;
        }

        std::size_t count_disabled_hook_candidates()
        {
            std::size_t count = 0u;
            const auto add_unique = [&](utils::mem::module_info& module, const std::string_view signature,
                const DWORD offset = 0u)
            {
                if (utils::mem::find_pattern_matches(module, signature, offset).size() == 1u) ++count;
            };

            // These signatures were observed in the submitted L4D1 build, but are
            // deliberately NOT installed because their surrounding ABI and naked
            // stub stack layouts have not yet been audited against L4D1.
            add_unique(game::engine_module, "8D 4F ? 33 D2");
            add_unique(game::client_module, "? ? 8B 50 ? FF D2 8B 0D ? ? ? ? E8 ? ? ? ? E8");
            add_unique(game::client_module, "74 ? 83 8E ? ? ? ? ? EB ? 80 7F");
            add_unique(game::client_module, "6A ? FF D0 85 C0 0F 85 ? ? ? ? 8B 56");
            return count;
        }
    }

    bool init_bootstrap_addresses()
    {
        g_report = {};
        g_report.total_data_sites = 3u;

        game::console();
        std::cout << "[L4D1][Bootstrap] Starting read-only address audit. No binary hooks will be installed." << std::endl;

        g_report.current_view_origin_resolved = resolve_embedded_global(
            game::engine_module,
            "F3 0F 10 05 ? ? ? ? F3 0F 10 0D ? ? ? ? F3 0F 10 15 ? ? ? ? 53",
            4u, l4d2::current_view_origin, "current view origin");

        g_report.current_view_forward_resolved = resolve_embedded_global(
            game::engine_module,
            "68 ? ? ? ? 68 ? ? ? ? E8 ? ? ? ? 83 C4 ? 56",
            1u, l4d2::current_view_forward, "current view forward");

        g_report.modelinfo_global_resolved = resolve_embedded_global(
            game::client_module,
            "8B 0D ? ? ? ? ? ? 50 8B 42 ? FF D0 85 C0 74 ? 8B 0D ? ? ? ? ? ? 50 8B 82",
            2u, l4d2::modelinfo_ptr, "model info global");

        g_report.resolved_data_sites =
            static_cast<std::size_t>(g_report.current_view_origin_resolved) +
            static_cast<std::size_t>(g_report.current_view_forward_resolved) +
            static_cast<std::size_t>(g_report.modelinfo_global_resolved);
        g_report.disabled_hook_candidates = count_disabled_hook_candidates();
        g_report.initialized = true;

        std::cout << "[L4D1][Bootstrap] Read-only data recovery: " << g_report.resolved_data_sites
            << '/' << g_report.total_data_sites << ". Audited hook candidates kept disabled: "
            << g_report.disabled_hook_candidates << '.' << std::endl;
        return true;
    }

    const bootstrap_report& get_bootstrap_report()
    {
        return g_report;
    }
}
