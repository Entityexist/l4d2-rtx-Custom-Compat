param(
    [ValidateSet("L4D2", "L4D1")]
    [string]$Target = "L4D2",
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [int]$MaxCpuCount = 16,
    [switch]$Clean,
    [switch]$SkipValidation,
    [switch]$BuildInstaller
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$params = @{
    Configuration = $Configuration
    MaxCpuCount = $MaxCpuCount
    Clean = $Clean
    SkipValidation = $SkipValidation
}

if ($Target -eq "L4D1") {
    if ($BuildInstaller) { throw "BuildInstaller is available only for the L4D2 target." }
    & (Join-Path $root "Build-L4D1-Bootstrap-R1.ps1") @params
} else {
    $params.BuildInstaller = $BuildInstaller
    & (Join-Path $root "Build-V21.14.10.ps1") @params
}
exit $LASTEXITCODE
