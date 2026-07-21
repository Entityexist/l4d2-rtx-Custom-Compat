param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [int]$MaxCpuCount = 16,
    [switch]$Clean,
    [switch]$SkipValidation,
    [switch]$BuildInstaller
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

& (Join-Path $root "Setup-Dependencies.ps1")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $SkipValidation) {
    & (Join-Path $root "Validate-TraceRayAbiRemixCategory-V21.14.10.ps1")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Clean) {
    Remove-Item (Join-Path $root "build") -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $root "bin") -Recurse -Force -ErrorAction SilentlyContinue
}

$premake = Join-Path $root "tools\premake5.exe"
if (-not (Test-Path -LiteralPath $premake)) { throw "premake5.exe not found: $premake" }

& $premake generate-buildinfo
if ($LASTEXITCODE -ne 0) { throw "premake generate-buildinfo failed." }
& $premake vs2022
if ($LASTEXITCODE -ne 0) { throw "premake vs2022 failed." }

$msbuild = $null
$vswhereCandidates = @(
    (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
    (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
)
foreach ($vswhere in $vswhereCandidates) {
    if (Test-Path $vswhere) {
        $candidate = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" 2>$null | Select-Object -First 1
        if ($candidate) { $msbuild = $candidate; break }
    }
}
if (-not $msbuild) {
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) { $msbuild = $command.Source }
}
if (-not $msbuild) { throw "MSBuild.exe not found. Install Visual Studio C++ with the Win32 toolchain and Windows SDK." }

$project = Join-Path $root "build\l4d2-rtx.vcxproj"
if (-not (Test-Path $project)) { throw "Premake did not generate: $project" }
$arguments = @($project, "/t:Build", "/p:Configuration=$Configuration", "/p:Platform=Win32", "/verbosity:minimal")
if ($MaxCpuCount -gt 0) { $arguments += "/m:$MaxCpuCount" } else { $arguments += "/m" }

Write-Host "Building Compat V21.14.10 ($Configuration|Win32)" -ForegroundColor Cyan
& $msbuild @arguments
if ($LASTEXITCODE -ne 0) { throw "l4d2-rtx MSBuild failed with exit code $LASTEXITCODE" }

if ($BuildInstaller) {
    $installer = Join-Path $root "build\installer.vcxproj"
    if (-not (Test-Path $installer)) { throw "Premake did not generate: $installer" }
    & $msbuild $installer "/t:Build" "/p:Configuration=$Configuration" "/p:Platform=Win32" "/verbosity:minimal" "/m:$MaxCpuCount"
    if ($LASTEXITCODE -ne 0) { throw "installer MSBuild failed with exit code $LASTEXITCODE" }
}

Write-Host "V21.14.10 build completed. DLL output: build\bin\$Configuration" -ForegroundColor Green
if ($BuildInstaller) { Write-Host "Installer output: bin" -ForegroundColor Green }
