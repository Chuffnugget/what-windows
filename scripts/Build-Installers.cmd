@echo off
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Installers.ps1"
set "WHAT_EXIT=%errorlevel%"
pause
exit /b %WHAT_EXIT%
