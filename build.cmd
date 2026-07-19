@echo off
REM Windows cmd.exe shim -> build.ps1. Prefers PowerShell 7 (pwsh), falls back
REM to Windows PowerShell (powershell.exe). Forwards all arguments.
where pwsh >nul 2>nul
if %ERRORLEVEL%==0 (
    pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
)
exit /b %ERRORLEVEL%
