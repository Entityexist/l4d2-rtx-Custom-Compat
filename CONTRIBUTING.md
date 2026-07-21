# Contributing

1. Create a topic branch from the current main branch.
2. Keep changes limited to one feature or fix where practical.
3. Run the current source validators before opening a pull request:

```powershell
.\Validate-TraceRayAbiRemixCategory-V21.14.10.ps1
.\Validate-L4D1-Bootstrap-R1.ps1
```

4. Build Win32 Release when Visual Studio is available.
5. Do not commit generated `build/`, `bin/`, downloaded `deps/toml11/`, logs, dumps, game installations or packaged archives.
