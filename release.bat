@echo off
set "GAME_EXE=007FRWL"
cd /d "%~dp0"
set "OUT=out\build\release"
set "D3DCOMPILER=%ProgramFiles(x86)%\Windows Kits\10\Redist\D3D\x64\d3dcompiler_47.dll"
if not exist "%D3DCOMPILER%" (echo d3dcompiler_47.dll not found: install the Windows SDK from the Visual Studio Installer & exit /b 1)
if not exist "%OUT%\%GAME_EXE%.exe" call build.bat || exit /b 1
rem the configure step writes where the SDK is
if not exist "%OUT%\sdk_dir.txt" cmake --preset release >nul || exit /b 1
if not exist "%OUT%\sdk_dir.txt" echo %OUT%\sdk_dir.txt was not written, update the SDK. && exit /b 1
set /p SDK=<out\build\release\sdk_dir.txt
rem One name for every release, so releases/latest/download/007FRWL-XBERecomp.zip stays a working link
set "ZIP=007FRWL-XBERecomp.zip"
del /q "%ZIP%" 2>nul
rem an empty assets folder beside the exe, where the game (files or .iso) goes
rd /s /q "%OUT%\release" 2>nul
mkdir "%OUT%\release\assets"
7z a "%ZIP%" ".\%OUT%\%GAME_EXE%.exe" ".\%OUT%\release\assets" .\README.md "%SDK%\THIRD_PARTY.txt" "%D3DCOMPILER%"
