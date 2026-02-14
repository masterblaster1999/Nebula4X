@echo off
setlocal EnableExtensions

set "ROOT=%~dp0.."
set "EXE=%ROOT%\out\build\core-tests\nebula4x_tests.exe"
set "EXE_FALLBACK=%ROOT%\out\build\core-tests\Release\nebula4x_tests.exe"
set "HEARTBEAT=5"
set "TIMEOUT=180"
set "CONFIG_TIMEOUT=180"
set "BUILD_TIMEOUT=600"
set "ORPHAN_AGE=90"
set "LOG_LEVEL=error"
set "PS1=%ROOT%\tools\run_tests.ps1"
set "RAW_ARGS=%*"
set "SHOW_HELP="
if not "%RAW_ARGS%"=="" (
  echo %RAW_ARGS% | findstr /I /C:"-?" /C:"/?" /C:"-h" /C:"-help" /C:"--help" >nul && set "SHOW_HELP=1"
)
if defined SHOW_HELP (
  if exist "%PS1%" (
    echo [run] powershell -ExecutionPolicy Bypass -File "%PS1%" -?
    powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -?
    exit /b %ERRORLEVEL%
  )
)

if not exist "%EXE%" if exist "%EXE_FALLBACK%" set "EXE=%EXE_FALLBACK%"
if not exist "%EXE%" (
  echo Test executable not found: "%EXE%"
  echo Build first: cmake --build --preset core-tests-headless --config Release --target nebula4x_tests
  exit /b 1
)

tasklist /FI "IMAGENAME eq nebula4x_tests.exe" 2>nul | find /I "nebula4x_tests.exe" >nul
if %ERRORLEVEL%==0 (
  echo Stopping stale nebula4x_tests.exe process...
  taskkill /F /IM nebula4x_tests.exe >nul 2>&1
)

if not exist "%PS1%" goto fallback
set "USER_ARGS=%*"
if not "%USER_ARGS%"=="" (
  rem Normalize brittle switch-false syntax that fails through cmd->powershell forwarding.
  call set "USER_ARGS=%%USER_ARGS:-UseBuildMutex:False=-NoBuildMutex%%"
  call set "USER_ARGS=%%USER_ARGS:-UseBuildMutex:$false=-NoBuildMutex%%"
  call set "USER_ARGS=%%USER_ARGS:-UseBuildMutex 0=-NoBuildMutex%%"
  call set "USER_ARGS=%%USER_ARGS:-KillRunning:False=-NoKillRunning%%"
  call set "USER_ARGS=%%USER_ARGS:-KillRunning:$false=-NoKillRunning%%"
  call set "USER_ARGS=%%USER_ARGS:-KillOrphanedBuildTools:False=-NoKillOrphanedBuildTools%%"
  call set "USER_ARGS=%%USER_ARGS:-KillOrphanedBuildTools:$false=-NoKillOrphanedBuildTools%%"
)
echo [run] powershell -ExecutionPolicy Bypass -File "%PS1%" -Config Release -LogLevel %LOG_LEVEL% -HeartbeatSeconds %HEARTBEAT% -PerTestTimeoutSeconds %TIMEOUT% -ConfigureTimeoutSeconds %CONFIG_TIMEOUT% -BuildTimeoutSeconds %BUILD_TIMEOUT% -BuildWatchdogSeconds %HEARTBEAT% -KillOrphanedBuildTools -OrphanedToolMinAgeSeconds %ORPHAN_AGE% -Isolated -ContinueOnFailure 1 %USER_ARGS%
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -Config Release -LogLevel %LOG_LEVEL% -HeartbeatSeconds %HEARTBEAT% -PerTestTimeoutSeconds %TIMEOUT% -ConfigureTimeoutSeconds %CONFIG_TIMEOUT% -BuildTimeoutSeconds %BUILD_TIMEOUT% -BuildWatchdogSeconds %HEARTBEAT% -KillOrphanedBuildTools -OrphanedToolMinAgeSeconds %ORPHAN_AGE% -Isolated -ContinueOnFailure 1 %USER_ARGS%
set "RC=%ERRORLEVEL%"
if "%RC%"=="9009" goto fallback
exit /b %RC%

:fallback
echo [warn] PowerShell helper unavailable. Falling back to direct test runner.
echo [run] "%EXE%" --verbose --heartbeat-seconds %HEARTBEAT% %*
"%EXE%" --verbose --heartbeat-seconds %HEARTBEAT% %*
set "RC=%ERRORLEVEL%"
exit /b %RC%
