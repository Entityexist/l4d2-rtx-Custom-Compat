# Building

## Requirements

- Windows 10 or 11
- Visual Studio 2022 with **Desktop development with C++**
- Windows SDK
- Python 3 available as `python`
- Git is recommended, but not required

The build scripts automatically download the pinned `toml11` dependency into the ignored `deps/toml11` directory.

## L4D2

```powershell
.\BUILD.ps1 -Target L4D2 -Configuration Release -Clean
```

To build the installer as well:

```powershell
.\BUILD.ps1 -Target L4D2 -Configuration Release -Clean -BuildInstaller
```

## L4D1 compatibility bootstrap

```powershell
.\BUILD.ps1 -Target L4D1 -Configuration Release -Clean
```

Set `L4D2_ROOT` or `L4D1_ROOT` before generation when you want Premake to route the output and debugger directly to a game directory.

## Manual project generation

```powershell
.\Setup-Dependencies.ps1
.\tools\premake5.exe generate-buildinfo
.\tools\premake5.exe vs2022
```

The generated Visual Studio solution is placed in `build/` and is intentionally not tracked by Git.
