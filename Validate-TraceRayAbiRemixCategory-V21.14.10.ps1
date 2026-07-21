$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ProjectRoot

python (Join-Path $ProjectRoot "validation\validate_trace_ray_abi_remix_category_v21_14_10.py")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

python (Join-Path $ProjectRoot "validation\validate_source_material_bridge_v21_9.py")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Compat V21.14.10 TraceRay ABI and Remix category isolation validation passed." -ForegroundColor Green
