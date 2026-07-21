param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$target = Join-Path $root "deps\toml11"
$header = Join-Path $target "include\toml.hpp"
$tag = "v4.4.0"

if ((Test-Path -LiteralPath $header) -and -not $Force) {
    Write-Host "toml11 $tag is already available." -ForegroundColor Green
    exit 0
}

Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction SilentlyContinue
$git = Get-Command git.exe -ErrorAction SilentlyContinue
if ($git) {
    & $git.Source clone --depth 1 --branch $tag https://github.com/ToruNiina/toml11.git $target
    if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $header)) {
        Write-Host "Downloaded toml11 $tag with Git." -ForegroundColor Green
        exit 0
    }
    Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction SilentlyContinue
}

$archive = Join-Path $env:TEMP "toml11-$tag.zip"
$extract = Join-Path $env:TEMP "toml11-$tag"
Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue
Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/ToruNiina/toml11/archive/refs/tags/$tag.zip" -OutFile $archive
Expand-Archive -LiteralPath $archive -DestinationPath $extract -Force
$source = Get-ChildItem -LiteralPath $extract -Directory | Select-Object -First 1
if (-not $source) { throw "Downloaded toml11 archive does not contain a directory." }
Move-Item -LiteralPath $source.FullName -Destination $target
Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue
if (-not (Test-Path -LiteralPath $header)) { throw "Failed to install toml11 $tag." }
Write-Host "Downloaded toml11 $tag with PowerShell." -ForegroundColor Green
