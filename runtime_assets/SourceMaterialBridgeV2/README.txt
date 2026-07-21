Source Material Bridge ABI v2 - runtime integration notes
=========================================================

The compat DLL exports RemixSourceMaterialBridge_GetApi. Request ABI version 2
from the 32-bit D3D9/DXVK Remix client process. The V2 API preserves the complete
V1 prefix and adds QueryDrawPacket/GetLatestDrawPacketId.

Per compatible model draw:
  RS150 + RS153 = stable 64-bit Source VMT/material identity
  RS214         = Source model material draw packet id
  RS215         = low draw flags for fast rejection/debugging

The runtime should read RS214 when processing the draw call, query the packet,
and attach model path, entity, skin, body, material ordinal, the declared VMT
texture names and all 16 Source sampler bindings to the Auto PBR/material-capture
record. RS215 contains the low packet flags and can cheaply reject non-model draws. It should not recreate or
resubmit the animated mesh through Remix API: Source/D3D9 remains the owner of
geometry, bones, flexes and viewmodel transforms.

ABI V1 remains available for existing runtimes. A V1 runtime continues to receive
material scalars and stable hashes, but ignores the new model draw context.
