@echo off
rem Run from an x64 Native Tools Command Prompt for Visual Studio 2022 17.14+ or Visual Studio 2026.
rem Static Microsoft runtime; no extra redistributable installer required.
if not exist "%~dp0..\build" mkdir "%~dp0..\build"
rc /nologo /I "%~dp0..\assets" /fo "%~dp0..\build\app-icon.res" "%~dp0..\src\app\app-icon.rc"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /MT /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /Fo"%~dp0..\build\WindowsBridge.obj" /Fe"%~dp0..\build\WHAT.WindowsHelper.exe" "%~dp0..\src\modules\windows\bridge\WindowsBridge.cpp" "%~dp0..\build\app-icon.res" /link windowsapp.lib user32.lib /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup
exit /b %errorlevel%
