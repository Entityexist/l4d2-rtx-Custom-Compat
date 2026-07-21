$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$python = Get-Command python.exe -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command py.exe -ErrorAction SilentlyContinue }
if (-not $python) { throw "Python was not found. Install Python or run tools\validate_l4d1_bootstrap.py manually." }

if ($python.Name -ieq "py.exe") {
    & $python.Source -3 (Join-Path $root "tools\validate_l4d1_bootstrap.py")
} else {
    & $python.Source (Join-Path $root "tools\validate_l4d1_bootstrap.py")
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($python.Name -ieq "py.exe") {
    & $python.Source -3 (Join-Path $root "validation\validate_trace_ray_abi_remix_category_v21_14_10.py")
} else {
    & $python.Source (Join-Path $root "validation\validate_trace_ray_abi_remix_category_v21_14_10.py")
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "L4D1 Bootstrap R1 validation passed." -ForegroundColor Green
