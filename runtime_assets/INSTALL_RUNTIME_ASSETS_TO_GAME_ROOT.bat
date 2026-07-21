@echo off
setlocal
if "%~1"=="" (
  echo Usage: %~nx0 "D:\SteamLibrary\steamapps\common\Left 4 Dead 2"
  exit /b 1
)
set "GAME=%~1"
if not exist "%GAME%" (
  echo Game root not found: %GAME%
  exit /b 2
)
if not exist "%GAME%\l4d2-rtx\textures" mkdir "%GAME%\l4d2-rtx\textures"
copy /Y "%~dp0textures\xorxor4d_strawberry.png" "%GAME%\l4d2-rtx\textures\xorxor4d_strawberry.png" >nul
if errorlevel 1 exit /b 3
echo Installed V21.7 runtime UI assets into %GAME%\l4d2-rtx
endlocal
