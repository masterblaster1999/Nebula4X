@echo off
setlocal EnableExtensions

set "ROOT=%~dp0.."
set "PS1=%ROOT%\tools\build_game.ps1"

if not exist "%PS1%" (
  echo build_game.ps1 not found: "%PS1%"
  exit /b 1
)

if /I "%~1"=="-?" goto :show_help
if /I "%~1"=="/?" goto :show_help
if /I "%~1"=="-h" goto :show_help
if /I "%~1"=="--help" goto :show_help
if /I "%~1"=="-help" goto :show_help

echo [run] powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
set "RC=%ERRORLEVEL%"

if not "%RC%"=="0" (
  if "%RC%"=="125" (
    echo [build_game.cmd] FullUI preflight failed: no local SDL2 config and GitHub fetch is unreachable.
  )
  echo [build_game.cmd] failed with exit code %RC%
)
exit /b %RC%

:show_help
echo [run] powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -Help
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -Help
exit /b %ERRORLEVEL%
