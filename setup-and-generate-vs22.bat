@echo off
setlocal
cd /d "%~dp0"

call fetch-deps-toml11.bat
if errorlevel 1 exit /b 1

tools\premake5.exe generate-buildinfo
if errorlevel 1 exit /b 1

tools\premake5.exe vs2022
if errorlevel 1 exit /b 1

echo Generated Visual Studio 2022 solution in build\
exit /b 0
