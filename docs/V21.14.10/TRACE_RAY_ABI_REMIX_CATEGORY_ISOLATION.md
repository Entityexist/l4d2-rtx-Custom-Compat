# V21.14.10 — TraceRay ABI and RTX Remix Category Isolation

## 1. Light Studio MMB crash

The crash was reproduced by the path used when the middle mouse button requests a surface position for a new or selected light.

The previous wrapper treated `EngineTraceClient003` like the more common Source `EngineTraceClient004` layout and dispatched `TraceRay` through vtable slot 4. Binary inspection of the supplied June 30, 2026 `engine.dll` shows that for this L4D2 client ABI:

- slot 4 resolves to `ClipRayToCollideable`;
- slot 5 resolves to `TraceRay`.

Calling slot 4 with the `TraceRay` argument list shifts the semantic meaning of the arguments. The engine then treats the compatibility filter object as an `ICollideable` and calls an unrelated vtable entry. The supplied dump ends in an execute access violation at `0x7265706F`, which is not inside a loaded module and is consistent with a bogus indirect call.

### Fix

- Prefer `EngineTraceClient003` and configure slot 5.
- Keep `EngineTraceClient004` only as a fallback with slot 4.
- Validate that the selected vtable target belongs to the loaded `engine.dll` image.
- Disable traced placement and use the existing distance fallback if validation fails.
- Restore the missing `worldSurfaceIndex` member in `CGameTrace`.
- Add x86 size assertions for `Ray_t`, `CBaseTrace` and `CGameTrace`.

## 2. Scene becomes unlit/emissive after F5

RTX Remix/Xorxor uses the otherwise unused D3D9 render state 42 as a private instance-category payload. V21.14.9 assigned these categories to ImGui:

- `WORLD_UI`;
- `IGNORE_LIGHTS`;
- `IGNORE_ANTI_CULLING`;
- `IGNORE_MOTION_BLUR`.

A normal D3D9 state block is not a sufficient ownership boundary for a render state intercepted by the Remix bridge. Replaying a sampled RS42 value can also replay a stale category. When `IGNORE_LIGHTS` reaches later Source draw calls, the scene appears emissive or fully unlit.

### Fix

- Set RS42 only immediately before `ImGui_ImplDX9_RenderDrawData`.
- Reset RS42 to neutral `0` immediately after that draw call.
- Reset RS42 to `0` again as the final write of the D3D9 state guard.
- Never sample and replay RS42 for the ImGui scope.
- Preserve ordinary D3D9 render states, targets, shaders, streams and textures normally.

RS42 is treated as a transient one-draw bridge command, not persistent renderer state.

## Expected result

- MMB placement no longer crashes on world geometry or props.
- A failed or unknown trace ABI falls back safely instead of calling an unknown vtable method.
- Opening or closing F5 does not leave `WORLD_UI`/`IGNORE_LIGHTS` active for the world.
- The UI remains excluded from Remix lighting without changing following Source draw calls.
