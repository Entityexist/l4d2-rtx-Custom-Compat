#pragma once

#include <cstdint>

// Stable C ABI shared by optional Source Engine compatibility providers and DXVK Remix.
// Keep this file POD-only: no STL, exceptions, virtual functions or cross-DLL allocation.
#define REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V1 1u
#define REMIX_SOURCE_MATERIAL_BRIDGE_ABI_V2 2u
#define REMIX_SOURCE_MATERIAL_BRIDGE_MAGIC 0x534D4252u /* 'SMBR' */
#define REMIX_SOURCE_MATERIAL_BRIDGE_EXPORT "RemixSourceMaterialBridge_GetApi"
#define REMIX_SOURCE_RS_MODEL_DRAW_PACKET_ID 214u
#define REMIX_SOURCE_RS_MODEL_DRAW_PACKET_FLAGS 215u

#if defined(_MSC_VER)
#define REMIX_SOURCE_MATERIAL_BRIDGE_CALL __cdecl
#else
#define REMIX_SOURCE_MATERIAL_BRIDGE_CALL
#endif

enum RemixSourceMaterialFlagsV1 : std::uint64_t
{
    REMIX_SOURCE_MATERIAL_HAS_ALBEDO                 = 1ull << 0,
    REMIX_SOURCE_MATERIAL_HAS_NORMAL                 = 1ull << 1,
    REMIX_SOURCE_MATERIAL_HAS_ROUGHNESS              = 1ull << 2,
    REMIX_SOURCE_MATERIAL_HAS_METALLIC               = 1ull << 3,
    REMIX_SOURCE_MATERIAL_HAS_SPECULAR_MASK          = 1ull << 4,
    REMIX_SOURCE_MATERIAL_HAS_PHONG_EXPONENT_TEXTURE = 1ull << 5,
    REMIX_SOURCE_MATERIAL_HAS_HEIGHT                 = 1ull << 6,
    REMIX_SOURCE_MATERIAL_SSBUMP                     = 1ull << 7,
    REMIX_SOURCE_MATERIAL_SELF_ILLUM                 = 1ull << 8,
    REMIX_SOURCE_MATERIAL_ALPHA_TEST                 = 1ull << 9,
    REMIX_SOURCE_MATERIAL_TRANSLUCENT                = 1ull << 10,
    REMIX_SOURCE_MATERIAL_ADDITIVE                   = 1ull << 11,
    REMIX_SOURCE_MATERIAL_TWO_SIDED                  = 1ull << 12,
    REMIX_SOURCE_MATERIAL_WATER                      = 1ull << 13,
    REMIX_SOURCE_MATERIAL_GLASS                      = 1ull << 14,
    REMIX_SOURCE_MATERIAL_UNLIT                      = 1ull << 15,
    REMIX_SOURCE_MATERIAL_DYNAMIC                    = 1ull << 16,
    REMIX_SOURCE_MATERIAL_DETAIL                     = 1ull << 17,
    REMIX_SOURCE_MATERIAL_DUAL_LAYER                 = 1ull << 18,
    REMIX_SOURCE_MATERIAL_ENV_MAP                    = 1ull << 19,
    REMIX_SOURCE_MATERIAL_PARALLAX                   = 1ull << 20,
    REMIX_SOURCE_MATERIAL_EVENT_EMISSIVE             = 1ull << 21,
    REMIX_SOURCE_MATERIAL_ASSET_BACKED               = 1ull << 22,
    REMIX_SOURCE_MATERIAL_PATCH_RESOLVED              = 1ull << 23,
};

enum RemixSourceMaterialClassV1 : std::uint32_t
{
    REMIX_SOURCE_CLASS_UNKNOWN = 0,
    REMIX_SOURCE_CLASS_MASONRY,
    REMIX_SOURCE_CLASS_METAL,
    REMIX_SOURCE_CLASS_WOOD,
    REMIX_SOURCE_CLASS_FABRIC,
    REMIX_SOURCE_CLASS_RUBBER,
    REMIX_SOURCE_CLASS_ORGANIC,
    REMIX_SOURCE_CLASS_FOLIAGE,
    REMIX_SOURCE_CLASS_GLASS,
    REMIX_SOURCE_CLASS_WATER,
    REMIX_SOURCE_CLASS_EMISSIVE,
    REMIX_SOURCE_CLASS_UNLIT,
};



enum RemixSourceModelDrawClassV2 : std::uint32_t
{
    REMIX_SOURCE_DRAW_UNKNOWN = 0,
    REMIX_SOURCE_DRAW_STATIC_PROP,
    REMIX_SOURCE_DRAW_DYNAMIC_PROP,
    REMIX_SOURCE_DRAW_PLAYER,
    REMIX_SOURCE_DRAW_INFECTED,
    REMIX_SOURCE_DRAW_VIEWMODEL,
    REMIX_SOURCE_DRAW_WEAPON_WORLD,
    REMIX_SOURCE_DRAW_RAGDOLL,
};

enum RemixSourceModelDrawFlagsV2 : std::uint64_t
{
    REMIX_SOURCE_DRAW_HAS_MODEL_CONTEXT = 1ull << 0,
    REMIX_SOURCE_DRAW_SKINNED = 1ull << 1,
    REMIX_SOURCE_DRAW_VIEWMODEL_FLAG = 1ull << 2,
    REMIX_SOURCE_DRAW_PLAYER_FLAG = 1ull << 3,
    REMIX_SOURCE_DRAW_INFECTED_FLAG = 1ull << 4,
    REMIX_SOURCE_DRAW_WEAPON_FLAG = 1ull << 5,
    REMIX_SOURCE_DRAW_DYNAMIC_MATERIAL = 1ull << 6,
    REMIX_SOURCE_DRAW_ASSET_BACKED = 1ull << 7,
    REMIX_SOURCE_DRAW_HAS_ALBEDO_BINDING = 1ull << 8,
    REMIX_SOURCE_DRAW_HAS_NORMAL_BINDING = 1ull << 9,
    REMIX_SOURCE_DRAW_EXPLICIT_ALBEDO = 1ull << 10,
};

enum RemixSourceMaskModeV1 : std::uint32_t
{
    REMIX_SOURCE_MASK_NONE = 0,
    REMIX_SOURCE_MASK_DEDICATED_TEXTURE,
    REMIX_SOURCE_MASK_BASE_ALPHA,
    REMIX_SOURCE_MASK_NORMAL_ALPHA,
    REMIX_SOURCE_MASK_BASE_LUMINANCE,
    REMIX_SOURCE_MASK_PHONG_EXPONENT_TEXTURE,
    REMIX_SOURCE_MASK_DETAIL,
    REMIX_SOURCE_MASK_EMISSIVE_BLEND,
};

#pragma pack(push, 8)

struct RemixSourceMaterialInfoV1
{
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    std::uint64_t materialHash;
    std::uint64_t materialRevision;
    std::uint64_t flags;
    std::uint32_t materialClass;
    std::uint32_t specularMaskMode;
    std::uint32_t emissionMaskMode;
    std::uint32_t reserved0;
    float roughness;
    float metallic;
    float dielectricF0;
    float reflectionWeight;
    float phongExponent;
    float phongBoost;
    float emissiveIntensity;
    float alphaTestReference;
    float detailScale;
    float detailBlendFactor;
    float roughnessConfidence;
    float normalConfidence;
    float materialConfidence;
    float reservedFloat0;
    char materialName[260];
    char shaderName[64];
    char surfaceProp[64];
    char providerName[64];
};

struct RemixSourceMaterialBridgeStatusV1
{
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    std::uint32_t enabled;
    std::uint32_t materialCount;
    std::uint64_t registryRevision;
    std::uint64_t queryCount;
    std::uint64_t queryHitCount;
    char gameName[64];
    char status[192];
};

struct RemixSourceMaterialBridgeApiV1
{
    std::uint32_t magic;
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    std::uint32_t providerVersion;
    std::uint64_t (REMIX_SOURCE_MATERIAL_BRIDGE_CALL *GetRegistryRevision)();
    std::uint32_t (REMIX_SOURCE_MATERIAL_BRIDGE_CALL *QueryMaterial)(std::uint64_t materialHash, RemixSourceMaterialInfoV1* output);
    std::uint32_t (REMIX_SOURCE_MATERIAL_BRIDGE_CALL *GetStatus)(RemixSourceMaterialBridgeStatusV1* output);
};

struct RemixSourceModelDrawInfoV2
{
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    std::uint32_t packetId;
    std::uint32_t drawClass;
    std::uint64_t materialHash;
    std::uint64_t materialRevision;
    std::uint64_t modelHash;
    std::uint64_t instanceHash;
    std::uint64_t flags;
    std::int32_t entityIndex;
    std::int32_t skin;
    std::int32_t body;
    std::int32_t hitboxSet;
    std::uint32_t sourceInstance;
    std::uint32_t materialOrdinal;
    std::uint32_t boundTextureMask;
    std::int32_t albedoSampler;
    std::int32_t normalSampler;
    std::int32_t roughnessSampler;
    std::int32_t emissiveSampler;
    std::uint32_t boundTextureIds[16];
    char modelName[260];
    char materialName[260];
    char shaderName[64];
    char albedoTexture[260];
    char normalTexture[260];
    char roughnessTexture[260];
    char emissiveTexture[260];
};

struct RemixSourceMaterialBridgeApiV2
{
    std::uint32_t magic;
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    std::uint32_t providerVersion;
    std::uint64_t (REMIX_SOURCE_MATERIAL_BRIDGE_CALL *GetRegistryRevision)();
    std::uint32_t (REMIX_SOURCE_MATERIAL_BRIDGE_CALL *QueryMaterial)(std::uint64_t materialHash, RemixSourceMaterialInfoV1* output);
    std::uint32_t (REMIX_SOURCE_MATERIAL_BRIDGE_CALL *GetStatus)(RemixSourceMaterialBridgeStatusV1* output);
    std::uint32_t (REMIX_SOURCE_MATERIAL_BRIDGE_CALL *QueryDrawPacket)(std::uint32_t packetId, RemixSourceModelDrawInfoV2* output);
    std::uint32_t (REMIX_SOURCE_MATERIAL_BRIDGE_CALL *GetLatestDrawPacketId)();
};

#pragma pack(pop)

static_assert(sizeof(RemixSourceMaterialInfoV1) == 560u, "Source material ABI size mismatch");
static_assert(sizeof(RemixSourceMaterialBridgeStatusV1) == 296u, "Source material status ABI size mismatch");
static_assert(sizeof(RemixSourceModelDrawInfoV2) == 1792u, "Source model draw ABI size mismatch");
static_assert(sizeof(RemixSourceMaterialBridgeApiV1) == 16u + 3u * sizeof(void*),
    "Source material V1 API size mismatch");
static_assert(sizeof(RemixSourceMaterialBridgeApiV2) == 16u + 5u * sizeof(void*),
    "Source material V2 API size mismatch");

using PFN_RemixSourceMaterialBridge_GetApi =
    const RemixSourceMaterialBridgeApiV1* (REMIX_SOURCE_MATERIAL_BRIDGE_CALL *)(std::uint32_t requestedAbiVersion);
using PFN_RemixSourceMaterialBridge_GetApiV2 =
    const RemixSourceMaterialBridgeApiV2* (REMIX_SOURCE_MATERIAL_BRIDGE_CALL *)(std::uint32_t requestedAbiVersion);
