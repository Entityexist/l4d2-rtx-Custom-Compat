#include "std_include.hpp"
#include "components/modules/material_exporter.hpp"

#if defined(_MSC_VER) && defined(_M_IX86)
#pragma comment(linker, "/EXPORT:RemixSourceMaterialBridge_GetApi=_RemixSourceMaterialBridge_GetApi")
#endif

namespace
{
    std::uint64_t __cdecl bridge_get_revision()
    {
        return components::material_exporter::get_source_bridge_registry_revision();
    }

    std::uint32_t __cdecl bridge_query_material(const std::uint64_t hash, RemixSourceMaterialInfoV1* output)
    {
        return components::material_exporter::query_source_bridge_material(hash, output) ? 1u : 0u;
    }

    std::uint32_t __cdecl bridge_get_status(RemixSourceMaterialBridgeStatusV1* output)
    {
        return components::material_exporter::get_source_bridge_status(output) ? 1u : 0u;
    }

    std::uint32_t __cdecl bridge_query_draw_packet(const std::uint32_t packet_id, RemixSourceModelDrawInfoV2* output)
    {
        return components::material_exporter::query_source_bridge_draw_packet(packet_id, output) ? 1u : 0u;
    }

    std::uint32_t __cdecl bridge_get_latest_draw_packet_id()
    {
        return components::material_exporter::get_latest_source_bridge_draw_packet_id();
    }

    const RemixSourceMaterialBridgeApiV1 g_source_material_bridge_api
    {
        REMIX_SOURCE_MATERIAL_BRIDGE_MAGIC,
        sizeof(RemixSourceMaterialBridgeApiV1),
        REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V1,
        0x00150E00u, // V21.14 provider
        &bridge_get_revision,
        &bridge_query_material,
        &bridge_get_status,
    };

    const RemixSourceMaterialBridgeApiV2 g_source_material_bridge_api_v2
    {
        REMIX_SOURCE_MATERIAL_BRIDGE_MAGIC,
        sizeof(RemixSourceMaterialBridgeApiV2),
        REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V2,
        0x00150E00u,
        &bridge_get_revision,
        &bridge_query_material,
        &bridge_get_status,
        &bridge_query_draw_packet,
        &bridge_get_latest_draw_packet_id,
    };
}

extern "C" __declspec(dllexport)
const RemixSourceMaterialBridgeApiV1* __cdecl RemixSourceMaterialBridge_GetApi(
    const std::uint32_t requestedAbiVersion)
{
    if (requestedAbiVersion == REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V1)
        return &g_source_material_bridge_api;
    if (requestedAbiVersion == REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V2)
        return reinterpret_cast<const RemixSourceMaterialBridgeApiV1*>(&g_source_material_bridge_api_v2);
    return nullptr;
}
