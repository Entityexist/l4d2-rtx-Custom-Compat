#pragma once

#include <Windows.h>
#include <cstdint>
#include "../../src/components/modules/source_material_bridge_api.hpp"

namespace source_material_bridge_v2
{
    struct consumer
    {
        const RemixSourceMaterialBridgeApiV2* api = nullptr;

        bool initialize(HMODULE compat_module)
        {
            if (!compat_module) return false;
            const auto get_api = reinterpret_cast<PFN_RemixSourceMaterialBridge_GetApiV2>(
                GetProcAddress(compat_module, REMIX_SOURCE_MATERIAL_BRIDGE_EXPORT));
            if (!get_api) return false;
            api = get_api(REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V2);
            return api && api->magic == REMIX_SOURCE_MATERIAL_BRIDGE_MAGIC &&
                api->abiVersion == REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V2 &&
                api->structSize >= sizeof(RemixSourceMaterialBridgeApiV2);
        }

        bool query_draw(std::uint32_t packet_id, RemixSourceModelDrawInfoV2& out) const
        {
            if (!api || !packet_id) return false;
            out = {};
            out.structSize = sizeof(out);
            out.abiVersion = REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V2;
            return api->QueryDrawPacket(packet_id, &out) != 0u;
        }
    };
}
