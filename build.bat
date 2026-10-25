@echo off
chcp 65001 >nul
rem Сборка Muon. Нужны Visual Studio 2022 (C++), CMake и Git.
cd /d "%~dp0"
if not exist third_party\imgui git clone --depth 1 --branch v1.91.9 https://github.com/ocornut/imgui.git third_party\imgui
if not exist third_party\LibRaw git clone --depth 1 --branch 0.22.2 https://github.com/LibRaw/LibRaw.git third_party\LibRaw
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 || exit /b 1
cmake --build build --config Release --target Muon -- /m || exit /b 1
copy /y bin\Release\Muon.exe Muon.exe >nul
rem Optional depth module for "Optics & light" (about 85 MB, downloaded once).
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_depth.ps1 || echo Depth module was not downloaded, Muon works without it.
echo Готово: %~dp0Muon.exe
