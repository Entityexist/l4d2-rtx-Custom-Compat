@echo off
setlocal EnableExtensions

if not exist "%~dp0tools" mkdir "%~dp0tools"
set "WINGET=winget.exe"
where winget.exe >nul 2>nul || set "WINGET=%LOCALAPPDATA%\Microsoft\WindowsApps\winget.exe"

if exist "%WINGET%" (
    "%WINGET%" install --id Microsoft.DirectXTex.Texconv -e --accept-source-agreements --accept-package-agreements
) else (
    echo WinGet alias was not found. Texconv may already be installed.
)

set "PATH=%PATH%;%LOCALAPPDATA%\Microsoft\WinGet\Links;%LOCALAPPDATA%\Microsoft\WindowsApps"
for /f "delims=" %%I in ('where texconv.exe 2^>nul') do copy /y "%%I" "%~dp0tools\texconv.exe" >nul

if exist "%~dp0tools\texconv.exe" (
    echo Texconv ready: %~dp0tools\texconv.exe
    "%~dp0tools\texconv.exe" -? >nul 2>nul
) else (
    echo Texconv was not found. Copy texconv.exe manually to: %~dp0tools
)
pause
