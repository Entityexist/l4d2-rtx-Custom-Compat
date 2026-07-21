# Left 4 Dead 1 compatibility bootstrap (R1)

This source package extends the L4D2 compatibility mod with a dedicated, isolated
Left 4 Dead 1 runtime profile. It is intentionally a **bootstrap**, not yet the
full L4D2 feature set.

## What is enabled in L4D1

- Detection of `left4dead.exe` and the submitted Steam launcher identity.
- Exported Source interfaces used by the common compatibility core:
  - `VClient016`
  - `VEngineClient013`
  - `VEngineEffects001`
  - `EngineTraceClient003` (TraceRay slot 5)
  - `VClientEntityList003`
  - `VEngineCvar007`
  - `VModelInfoClient004`
  - `VGUI_Surface030` fallback (L4D2 uses 031)
- RTX Remix API initialization and callback registration.
- External F5 compatibility/status window.
- Read-only recovery of three uniquely matching L4D1 global data references:
  camera origin, camera forward and the model-info global.

## What remains disabled

Every L4D2 binary patch, fixed RVA, direct JMP detour, MinHook gameplay hook,
naked-assembly stub, flashlight replacement, Light Studio, marker hook and model
render hook remains disabled in L4D1. A matching byte sequence alone is not enough:
the surrounding function ABI and stack/register contract must be verified first.

## Submitted L4D1 build identity

- `left4dead.exe` SHA-1: `098d07422acc07d560315fafebd021ea661121cd`
- `engine.dll` SHA-1: `5580eb708754a7deb1d04f460b56d2e8e3061cd7`
- `client.dll` SHA-1: `259c0dc8c6c4fafaa1cf1a398f30c641c419414c`
- `shaderapidx9.dll` SHA-1: `b98089e24f83994c977efb51240d0276749f8817`
- `stdshader_dx9.dll` SHA-1: `6bd0858b820afeb299ccac786766438fafd8847f`

`server.dll`, `studiorender.dll`, `materialsystem.dll`, `vstdlib.dll` and
`vguimatsurface.dll` were not supplied for offline signature analysis. They are
loaded/negotiated at runtime where available, but no L4D1 binary addresses are
resolved from them in R1.

## First test

1. Optionally set `L4D1_ROOT` to the Left 4 Dead installation directory, then build
   Win32/x86 Release using `Build-L4D1-Bootstrap-R1.ps1`. Premake will route the
   DLL and debugger to L4D1 while preserving the existing `L4D2_ROOT` path.
2. Install the resulting ASI/DLL exactly as for the L4D2 compatibility mod, but in
   the Left 4 Dead root.
3. Start `run-l4d1-rtx-bootstrap.bat`.
4. Open the console/log and verify:
   - `Native L4D1 bootstrap (safe/no binary hooks)`
   - Source interfaces initialized
   - Remix API initialized (or retrying until the bridge appears)
   - `Read-only data recovery: 3/3` for this submitted build
5. Press F5. The bootstrap status window must appear without blocking normal game input.

## Next porting stage

The next stage should collect L4D1 runtime logs plus `server.dll` and
`studiorender.dll`, then port one hook family at a time: D3D device/present,
RenderView camera lifecycle, map lifecycle, model render, and only then Light
Studio/gameplay features. Each L4D1 hook must receive its own signature and ABI
stub rather than reusing the L4D2 implementation blindly.
