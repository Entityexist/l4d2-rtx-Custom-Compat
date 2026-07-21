# Left 4 Dead 1 submitted-binary audit — Bootstrap R1

## Identification

The submitted executable is `left4dead.exe`, a 32-bit PE GUI Source launcher.
Its SHA-1 is `098d07422acc07d560315fafebd021ea661121cd`, and its PE timestamp is
2024-10-19 01:30:26 UTC. This is an L4D1 target, not an L4D2 executable rename.

Submitted module identities:

| Module | SHA-1 | PE timestamp (UTC) |
|---|---|---|
| `engine.dll` | `5580eb708754a7deb1d04f460b56d2e8e3061cd7` | 2026-06-29 14:51:59 |
| `client.dll` | `259c0dc8c6c4fafaa1cf1a398f30c641c419414c` | 2026-06-23 18:31:24 |
| `shaderapidx9.dll` | `b98089e24f83994c977efb51240d0276749f8817` | 2026-06-26 19:08:56 |
| `stdshader_dx9.dll` | `6bd0858b820afeb299ccac786766438fafd8847f` | 2026-05-04 23:18:16 |

## Source interface compatibility

The submitted binaries contain the interface generations used by the shared core:

- `VClient016`
- `VEngineClient013`
- `VEngineEffects001`
- `EngineTraceClient003`
- `VClientEntityList003`
- `VEngineCvar007`
- `VModelInfoClient004`
- `VGUI_Surface030`
- `ShaderApi029`
- `VMaterialSystem080`

The current L4D2 source requests `VGUI_Surface031`, so Bootstrap R1 negotiates 031
first and then safely falls back to L4D1's 030.

## L4D2 signature compatibility result

A PE-image scan of the existing L4D2 signatures produced:

| Module | Sites scanned | Unique | Missing | Ambiguous |
|---|---:|---:|---:|---:|
| `engine.dll` | 23 | 4 | 18 | 1 |
| `client.dll` | 23 | 4 | 17 | 2 |
| `shaderapidx9.dll` | 4 | 0 | 3 | 1 |

This proves that a full L4D2 runtime cannot be enabled safely in L4D1 merely by
changing the executable name. In particular, the L4D2 D3D-device/shader-API
signatures do not resolve uniquely in the submitted `shaderapidx9.dll`.

Unique, read-only data references retained by the bootstrap:

- engine camera origin signature: instruction RVA `0x1219B6`
- engine camera forward signature: instruction RVA `0x12AD27`
- client model-info global signature: instruction RVA `0x18A7F`

Some hook-like signatures also happen to match, but remain disabled until their
complete function ABI, overwritten instructions, register use and return path are
audited for L4D1:

- map-load candidate: engine RVA `0x2BF02B`
- RenderView candidate: client RVA `0x18FC11`
- water-check candidate: client RVA `0x18A2C1`
- third-person candidate: client RVA `0x1D14A4`

The renderer pre-draw signature is ambiguous (three matches), and the current
post-draw/D3D-device/shader-api signatures do not resolve. No renderer patch is
installed in Bootstrap R1.

## Missing inputs for the next porting stage

For a complete L4D1 runtime audit, collect matching copies of:

- `server.dll`
- `studiorender.dll`
- `materialsystem.dll`
- `vstdlib.dll`
- `vguimatsurface.dll`

The first runtime test should also provide `compat.log`, Remix bridge logs and a
crash dump if startup fails. The next safe implementation order is D3D lifecycle,
RenderView/camera lifecycle, map lifecycle, model render, and only then the full
Light Studio/gameplay feature graph.
