# V21.14.10 — supplied L4D2 binary audit

The four supplied modules form a coherent 32-bit Valve build from June 30, 2026.

| Module | Size | PE timestamp UTC | SHA-256 | Relevant interfaces |
|---|---:|---|---|---|
| `shaderapidx9.dll` | 897,328 | 2026-06-30 18:42:18 | `0db402cc58f86aa4318d5cdb59c8f081a77374448e5d0214a3618910568c3aa6` | `ShaderApi029`, `VMaterialSystem080` |
| `engine.dll` | 4,817,712 | 2026-06-30 18:45:42 | `8a6d468059394bf4cfce32f11dcba12ef45de88da1373084d5a2916d0bf7ba86` | `VEngineClient013`, `EngineTraceClient003`, `VModelInfoClient004` |
| `studiorender.dll` | 634,672 | 2026-06-30 18:41:26 | `3f5f5b0f539e8ad22bcfc4381be41571257c0c29e8061057682f9b8525ca7b85` | `VStudioRender026` |
| `client.dll` | 8,305,664 | 2026-06-30 18:45:35 | `c0a8e1e88f7312db11bcfd4b0ec574d224a34b2d9a615d6088b28335eeb4af1f` | `VClient016`, `EngineTraceClient003` |

## Pattern validation

All 51 compatibility patterns checked against `engine.dll`, `client.dll`, `shaderapidx9.dll` and `studiorender.dll` produced exactly one match. Their resolved RVAs agree with the static addresses currently recorded by the compatibility project.

This rules out a broad pattern drift or a mismatched Valve update as the cause of the current MMB crash. The failure was an interface ABI/index error in the new Light Studio trace wrapper.

## EngineTraceClient003 vtable audit

For the supplied `engine.dll`, the audited client trace vtable places:

- slot 4 at RVA `0x179FF0` — `ClipRayToCollideable` path;
- slot 5 at RVA `0x179760` — `TraceRay` path.

V21.14.10 encodes this distinction explicitly and retains a separate slot configuration for a possible `EngineTraceClient004` fallback.

## Scope of conclusions

The hashes above identify only the supplied modules. Future Valve updates must be re-audited before assuming the same private implementation addresses, although the public interface names and runtime target validation provide a safe first boundary.
