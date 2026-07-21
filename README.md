# L4D2 RTX Compat V21.14.10

Compatibility layer and runtime tooling for running **Left 4 Dead 2** with RTX Remix. This cleaned repository also contains the isolated **Left 4 Dead 1 Compatibility Bootstrap R1** profile.

## Current release

V21.14.10 fixes the Light Studio middle-mouse trace crash for the audited June 30, 2026 L4D2 binaries and isolates the private RTX Remix instance-category state used by ImGui.

- `EngineTraceClient003` uses the audited `TraceRay` slot 5 layout.
- The selected trace target is checked against the loaded `engine.dll` image.
- ImGui scopes Remix RS42 categories only around its draw submission and resets them to neutral afterward.
- V21.14.9 silent logging, V21.14.8 mouse recovery, V21.14.7 compile recovery and V21.14.5 Light Studio changes remain included.
- L4D1 Bootstrap R1 enables only audited interfaces, read-only diagnostics and safe Remix initialization; L4D2 binary hooks remain disabled there.

## Repository layout

- `src/` — compatibility DLL source
- `src_installer/` — optional installer source
- `assets/` — packaged L4D2 runtime assets
- `runtime_assets/` — additional material, IES and bridge templates
- `deps/` — vendored build dependencies; `toml11` is downloaded on demand
- `validation/` — current source-contract validators
- `docs/` — current release, audit and installation documentation
- `examples/` — example map-light configuration files

## Build

```powershell
.\BUILD.ps1 -Target L4D2 -Configuration Release -Clean
```

The script downloads pinned `toml11` v4.4.0 when missing, validates the current source and builds the Win32 DLL with Visual Studio 2022/MSBuild. See [BUILDING.md](BUILDING.md) for requirements and the L4D1 bootstrap command.

## Validation

```powershell
.\Validate-TraceRayAbiRemixCategory-V21.14.10.ps1
.\Validate-L4D1-Bootstrap-R1.ps1
```

## Documentation

- [Installation](docs/INSTALLATION.md)
- [V21.14.10 TraceRay and Remix category fix](docs/V21.14.10/TRACE_RAY_ABI_REMIX_CATEGORY_ISOLATION.md)
- [V21.14.10 binary audit](docs/V21.14.10/BINARY_AUDIT_REPORT.md)
- [L4D1 Bootstrap R1](docs/L4D1/BOOTSTRAP_R1.md)
- [L4D1 submitted-binary audit](docs/L4D1/SUBMITTED_BINARY_AUDIT_R1.md)

## Credits and license

This project is based on the L4D2 RTX compatibility work by xoxor4d and contains subsequent modified-edition development. See [LICENSE](LICENSE) and license files inside vendored dependencies before redistribution.
