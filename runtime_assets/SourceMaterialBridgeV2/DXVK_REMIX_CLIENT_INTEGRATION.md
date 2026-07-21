# DXVK Remix client integration

At D3D9 draw interception, read custom render state RS214 (numeric render state 214). When non-zero, query
`RemixSourceMaterialBridgeApiV2::QueryDrawPacket` before the draw record is sent
to the Remix server. Merge the returned metadata with the material identified by
the RS150/RS153 64-bit hash.

Recommended runtime policy:

1. Keep the native captured stage-0 texture as the albedo resource. When
   `REMIX_SOURCE_DRAW_EXPLICIT_ALBEDO` is set, the compat has already rebound the
   resolved Source `$basetexture` through the fixed-function model pass.
2. Use `albedoTexture`, `normalTexture`, `roughnessTexture` and `emissiveTexture`
   as Source asset names/provenance, not as GPU pointers.
3. Include `modelHash`, `skin`, `body` and `materialOrdinal` in diagnostics and
   optional variant keys. Do not replace the primary material key with entity id.
4. Use `instanceHash` only for per-instance overrides.
5. Ignore stale packets when `QueryDrawPacket` fails; never block the draw.
6. Query ABI V1 as a fallback when ABI V2 is unavailable.

This directory is a reference consumer. It is intentionally not linked into the
L4D2 compat DLL and must be integrated into the matching 32-bit DXVK/Remix D3D9
client build.
