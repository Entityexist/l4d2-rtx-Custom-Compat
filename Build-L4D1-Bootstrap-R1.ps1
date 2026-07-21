param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [int]$MaxCpuCount = 16,
    [switch]$Clean,
    [switch]$SkipValidation
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

& (Join-Path $root "Setup-Dependencies.ps1")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $SkipValidation) {
    & (Join-Path $root "Validate-L4D1-Bootstrap-R1.ps1")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Clean) {
    Remove-Item (Join-Path $root "build") -Recurse -Force -ErrorAction SilentlyContinue
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

Write-Host "Building L4D1 Compatibility Bootstrap R1 ($Configuration|Win32)" -ForegroundColor Cyan
& $msbuild @arguments
if ($LASTEXITCODE -ne 0) { throw "l4d2-rtx MSBuild failed with exit code $LASTEXITCODE" }

Write-Host "Build completed. DLL output: build\bin\$Configuration" -ForegroundColor Green
Write-Host "Use run-l4d1-rtx-bootstrap.bat from the Left 4 Dead root for the first test." -ForegroundColor Green
