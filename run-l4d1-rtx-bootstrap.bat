@echo off
setlocal
cd /d "%~dp0"

rem Safe L4D1 bootstrap: no L4D2 fixed offsets or binary hooks are enabled.
rem Keep -dxlevel 95 so the Source DX9 renderer and RTX Remix bridge can initialize.
start "" left4dead.exe -dx9 -dxlevel 95 -windowed -noborder %*
