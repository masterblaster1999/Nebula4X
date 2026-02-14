@echo off
setlocal EnableExtensions

set "ROOT=%~dp0.."
set "PS1=%ROOT%\tools\build_core.ps1"

if not exist "%PS1%" (
  echo build_core.ps1 not found: "%PS1%"
  exit /b 1
)

if "%~1"=="" (
  set "ARGS=-Preset core-tests-headless -Target nebula4x_tests -Config Release -Configure -BuildTimeoutSeconds 600 -WatchdogSeconds 5 -KillOrphanedBuildTools -OrphanedToolMinAgeSeconds 1 -EmitScriptRc"
) else (
  set "ARGS=%*"
  rem Normalize brittle switch-false syntax that fails through cmd->powershell forwarding.
  call set "ARGS=%%ARGS:-UseBuildMutex:False=-NoBuildMutex%%"
  call set "ARGS=%%ARGS:-UseBuildMutex:$false=-NoBuildMutex%%"
  call set "ARGS=%%ARGS:-UseBuildMutex 0=-NoBuildMutex%%"
  call set "ARGS=%%ARGS:-KillRunning:False=-NoKillRunning%%"
  call set "ARGS=%%ARGS:-KillRunning:$false=-NoKillRunning%%"
  call set "ARGS=%%ARGS:-KillOrphanedBuildTools:False=-NoKillOrphanedBuildTools%%"
  call set "ARGS=%%ARGS:-KillOrphanedBuildTools:$false=-NoKillOrphanedBuildTools%%"
)

echo [run] powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %ARGS%
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %ARGS%
set "RC=%ERRORLEVEL%"

if not "%RC%"=="0" (
  echo [build_core.cmd] failed with exit code %RC%
)
exit /b %RC%
