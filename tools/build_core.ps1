param(
  [string]$Preset = "core",
  [Alias("BinaryDir")]
  [string]$BuildDir = "",
  [string]$Config = "Release",
  [string]$Target = "nebula4x_cli",
  [string[]]$NativeBuildArgs = @(),
  [Alias("h", "?")]
  [switch]$Help,
  [switch]$Configure,
  [switch]$NoPresetRemap,
  [switch]$EmitScriptRc,
  [switch]$RetryCleanOnFailure,
  [switch]$UseBuildMutex = $false,
  [switch]$NoBuildMutex,
  [switch]$BuildLockRequired,
  [int]$BuildLockWaitSeconds = 15,
  [switch]$KillRunning,
  [switch]$NoKillRunning,
  [switch]$KillOrphanedBuildTools,
  [switch]$NoKillOrphanedBuildTools,
  [int]$OrphanedToolMinAgeSeconds = 1,
  [int]$ConfigureTimeoutSeconds = 180,
  [int]$BuildTimeoutSeconds = 600,
  [int]$InactivityTimeoutSeconds = 180,
  [int]$InactivityGraceSeconds = 20,
  [int]$ConfigureProgressTimeoutSeconds = 240,
  [int]$BuildProgressTimeoutSeconds = 600,
  [int]$MaxTotalSeconds = 0,
  [switch]$NoAutoMaxTotal,
  [int]$WatchdogSeconds = 5,
  [switch]$RetryOnInactivityTimeout,
  [switch]$NoRetryOnInactivityTimeout,
  [switch]$NoAutoConfigureOnMissingBuildFiles,
  [switch]$NoParallel,
  [int]$ParallelJobs = 0
)

$ErrorActionPreference = "Stop"
$scriptStartTime = Get-Date
$script:LastWatchdogTimeoutReason = ""

function Exit-WithCode {
  param([int]$Code)
  if ($EmitScriptRc) {
    Write-Output ("SCRIPT_RC=" + $Code)
  }
  exit $Code
}

function Get-RemainingTimeoutSeconds {
  param([int]$RequestedTimeoutSeconds)

  $requested = [Math]::Max(0, $RequestedTimeoutSeconds)
  if ($MaxTotalSeconds -le 0) {
    return $requested
  }

  $elapsed = ((Get-Date) - $scriptStartTime).TotalSeconds
  $remaining = [int][Math]::Floor($MaxTotalSeconds - $elapsed)
  if ($remaining -le 0) {
    return 0
  }

  if ($requested -eq 0) {
    return $remaining
  }

  return [Math]::Min($requested, $remaining)
}

function Assert-WithinMaxTotalTime {
  param([string]$Phase = "operation")

  if ($MaxTotalSeconds -le 0) {
    return
  }

  $elapsed = ((Get-Date) - $scriptStartTime).TotalSeconds
  if ($elapsed -ge $MaxTotalSeconds) {
    Write-Warning ($Phase + " exceeded MaxTotalSeconds (" + $MaxTotalSeconds + " seconds).")
    Exit-WithCode 124
  }
}

function Resolve-AutoMaxTotalSeconds {
  param(
    [switch]$WillConfigure,
    [int]$ConfigureTimeoutBudgetSeconds,
    [int]$BuildTimeoutBudgetSeconds,
    [string]$TargetName
  )

  $configureBudget = 0
  if ($WillConfigure) {
    if ($ConfigureTimeoutBudgetSeconds -gt 0) {
      $configureBudget = $ConfigureTimeoutBudgetSeconds
    } else {
      $configureBudget = 300
    }
  }

  $buildBudget = if ($BuildTimeoutBudgetSeconds -gt 0) { $BuildTimeoutBudgetSeconds } else { 900 }
  $suggested = $configureBudget + $buildBudget + 90

  $targetLower = if ($TargetName) { $TargetName.Trim().ToLowerInvariant() } else { "" }
  $hardCap = if ($targetLower -like "*nebula4x_tests*") { 900 } else { 1200 }
  return [Math]::Min([Math]::Max(0, $suggested), $hardCap)
}

function Show-Usage {
  $usage = @'
build_core.ps1 - Configure/build helper with watchdogs and process cleanup.

Usage
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_core.ps1 [options]

Options
  -Preset <name>                  CMake preset (default: core)
  -BuildDir / -BinaryDir <path>   Build directory for "cmake --build <path>" mode
  -Config <name>                  Build config (default: Release)
  -Target <name(s)>               CMake target(s), supports whitespace/comma/semicolon separation
  -NativeBuildArgs <args...>      Native args passed after "--" to the generator
  -Configure                       Run configure before build
  -RetryCleanOnFailure             Retry once with --clean-first on non-timeout failure
  -UseBuildMutex / -NoBuildMutex   Enable/disable global build mutex (default: disabled)
  -BuildLockRequired               Fail if mutex cannot be acquired (default: continue without mutex)
  -KillRunning / -NoKillRunning    Stop running target process before build (default: enabled)
  -KillOrphanedBuildTools / -NoKillOrphanedBuildTools
                                   Stop stale cmake/ninja/msbuild processes (default: enabled)
  -OrphanedToolMinAgeSeconds <n>   Age threshold for stale tool process cleanup (default: 1)
  -BuildLockWaitSeconds <n>        Mutex acquire timeout seconds (default: 15)
  -ConfigureTimeoutSeconds <n>     Configure timeout seconds (default: 180)
  -BuildTimeoutSeconds <n>         Build timeout seconds (default: 600)
  -InactivityTimeoutSeconds <n>    Kill child process if no output for n seconds (default: 180, 0 disables)
  -InactivityGraceSeconds <n>      Minimum runtime before inactivity timeout can trigger (default: 20)
  -ConfigureProgressTimeoutSeconds <n>
                                   Configure no-progress timeout seconds (default: 240, 0 disables)
  -BuildProgressTimeoutSeconds <n> Build no-progress timeout seconds (default: 600, 0 disables)
  -MaxTotalSeconds <n>             Hard timeout for whole script runtime (default: 0 = disabled)
  -NoAutoMaxTotal                  Disable automatic MaxTotalSeconds guard when MaxTotalSeconds is not set
  -WatchdogSeconds <n>             Heartbeat interval seconds (default: 5)
  -RetryOnInactivityTimeout / -NoRetryOnInactivityTimeout
                                   Retry once on inactivity timeout with safer settings (default: enabled)
  -NoAutoConfigureOnMissingBuildFiles
                                   Disable automatic configure when build files are missing/corrupt
  -NoParallel                      Disable CMake parallel build flag (--parallel)
  -ParallelJobs <n>                Explicit job count for --parallel (0 = generator default)
  -EmitScriptRc                    Print SCRIPT_RC=<code> before exit
  -Help / -?                       Show this help and exit

Examples
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_core.ps1 -Configure
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_core.ps1 -Preset core-tests-headless -Target nebula4x_tests -Config Release
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_core.ps1 -BuildDir .\out\build\ui-vs -Target nebula4x -Config Release
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_core.ps1 -Preset core-tests-headless -Target "nebula4x_tests /p:UseMultiToolTask=true"
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_core.ps1 -Preset core-tests-headless -Target nebula4x_tests -EmitScriptRc
'@
  Write-Host $usage
}

function Test-ShowHelp {
  param(
    [switch]$HelpSwitch,
    [string]$PresetValue
  )

  if ($HelpSwitch) {
    return $true
  }

  if (-not $PresetValue) {
    return $false
  }

  $token = $PresetValue.Trim().ToLowerInvariant()
  return $token -eq "-?" -or $token -eq "/?" -or $token -eq "help" -or $token -eq "--help" -or $token -eq "-help"
}

function Resolve-EffectivePreset {
  param(
    [string]$RequestedPreset,
    [string]$TargetName,
    [switch]$DisableRemap
  )

  if ($DisableRemap) {
    return $RequestedPreset
  }

  $map = @{
    "core-tests" = "core-tests-headless"
    "ui" = "ui-headless"
    "ui-tests" = "ui-tests-headless"
  }

  # When building the game executable, avoid UI+tests presets by default.
  $target = if ($TargetName) { $TargetName.Trim().ToLowerInvariant() } else { "" }
  if ($target -eq "nebula4x") {
    if ($RequestedPreset -in @("ui", "ui-headless", "ui-tests", "ui-tests-headless")) {
      $remapped = "ui-runtime-headless"
      Write-Warning ("Target '" + $TargetName + "' requested with preset '" + $RequestedPreset +
                     "'. Using '" + $remapped + "' for faster/stabler game builds (pass -NoPresetRemap to disable).")
      return $remapped
    }
  }

  if ($map.ContainsKey($RequestedPreset)) {
    $remapped = $map[$RequestedPreset]
    Write-Warning ("Preset '" + $RequestedPreset + "' may contend with IDE background CMake activity. Using '" +
                   $remapped + "' (pass -NoPresetRemap to disable).")
    return $remapped
  }

  return $RequestedPreset
}

function Get-BuildMutex {
  param([int]$WaitSeconds = 15)

  $wait = [Math]::Max(0, $WaitSeconds)
  $mutex = New-Object System.Threading.Mutex($false, 'Local\Nebula4X_BuildMutex')
  $acquired = $false
  try {
    $acquired = $mutex.WaitOne([TimeSpan]::FromSeconds($wait))
  } catch {
    $mutex.Dispose()
    throw
  }

  if (-not $acquired) {
    $mutex.Dispose()
    return $null
  }

  return $mutex
}

function ConvertTo-QuotedArg {
  param([string]$Arg)
  if ($null -eq $Arg) {
    return '""'
  }
  if ($Arg.Contains('"')) {
    $Arg = $Arg.Replace('"', '\"')
  }
  if ($Arg.IndexOfAny([char[]]@(' ', "`t", "`n", "`r")) -ge 0) {
    return '"' + $Arg + '"'
  }
  return $Arg
}

function Split-TargetTokens {
  param([string]$Raw)

  if (-not $Raw -or $Raw.Trim().Length -eq 0) {
    return @()
  }

  $normalized = $Raw.Replace(",", " ").Replace(";", " ")
  return @($normalized -split "\s+" | Where-Object { $_ -and $_.Trim().Length -gt 0 })
}

function Resolve-RepoRoot {
  $scriptDir = Split-Path -Parent $PSCommandPath
  return (Resolve-Path (Join-Path $scriptDir "..")).Path
}

function Resolve-BuildDirPath {
  param([string]$BuildDirInput)

  if (-not $BuildDirInput -or $BuildDirInput.Trim().Length -eq 0) {
    return ""
  }

  $repoRoot = Resolve-RepoRoot
  $candidate = $BuildDirInput.Trim()
  if (-not [System.IO.Path]::IsPathRooted($candidate)) {
    $candidate = Join-Path $repoRoot $candidate
  }
  return [System.IO.Path]::GetFullPath($candidate)
}

function Get-PresetBinaryDirCandidate {
  param([string]$PresetName)

  if (-not $PresetName -or $PresetName.Trim().Length -eq 0) {
    return ""
  }

  $repoRoot = Resolve-RepoRoot
  $presetsPath = Join-Path $repoRoot "CMakePresets.json"
  if (-not (Test-Path $presetsPath)) {
    return ""
  }

  try {
    $json = Get-Content -Path $presetsPath -Raw | ConvertFrom-Json
    if (-not $json.configurePresets) {
      return ""
    }

    $preset = $json.configurePresets | Where-Object { $_.name -eq $PresetName } | Select-Object -First 1
    if (-not $preset -or -not $preset.binaryDir) {
      return ""
    }

    $binaryDir = [string]$preset.binaryDir
    $binaryDir = $binaryDir.Replace('${sourceDir}', $repoRoot)
    if ([System.IO.Path]::IsPathRooted($binaryDir)) {
      return [System.IO.Path]::GetFullPath($binaryDir)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $binaryDir))
  } catch {
    return ""
  }
}

function Resolve-PresetBinaryDir {
  param([string]$PresetName)

  $candidate = Get-PresetBinaryDirCandidate -PresetName $PresetName
  if (-not $candidate) {
    return ""
  }

  if (Test-Path $candidate) {
    return $candidate
  }

  return ""
}

function Resolve-GeneratorForPreset {
  param([string]$PresetName)

  $binaryDir = Resolve-PresetBinaryDir -PresetName $PresetName
  if (-not $binaryDir) {
    return ""
  }

  $cachePath = Join-Path $binaryDir "CMakeCache.txt"
  if (-not (Test-Path $cachePath)) {
    return ""
  }

  try {
    $line = Get-Content -Path $cachePath | Where-Object {
      $_ -like "CMAKE_GENERATOR:*="
    } | Select-Object -First 1

    if (-not $line) {
      return ""
    }

    $idx = $line.IndexOf("=")
    if ($idx -lt 0 -or $idx -ge ($line.Length - 1)) {
      return ""
    }

    return $line.Substring($idx + 1).Trim()
  } catch {
    return ""
  }
}

function Resolve-BuildPresetJobs {
  param([string]$PresetName)

  if (-not $PresetName -or $PresetName.Trim().Length -eq 0) {
    return 0
  }

  $repoRoot = Resolve-RepoRoot
  $presetsPath = Join-Path $repoRoot "CMakePresets.json"
  if (-not (Test-Path $presetsPath)) {
    return 0
  }

  try {
    $json = Get-Content -Path $presetsPath -Raw | ConvertFrom-Json
    if (-not $json.buildPresets) {
      return 0
    }

    $preset = $json.buildPresets | Where-Object { $_.name -eq $PresetName } | Select-Object -First 1
    if (-not $preset) {
      return 0
    }

    if ($null -eq $preset.jobs) {
      return 0
    }

    $jobs = 0
    if ([int]::TryParse([string]$preset.jobs, [ref]$jobs)) {
      return [Math]::Max(0, $jobs)
    }
    return 0
  } catch {
    return 0
  }
}

function Resolve-GeneratorForBuildDir {
  param([string]$BuildDirPath)

  if (-not $BuildDirPath -or $BuildDirPath.Trim().Length -eq 0) {
    return ""
  }

  $cachePath = Join-Path $BuildDirPath "CMakeCache.txt"
  if (-not (Test-Path $cachePath)) {
    return ""
  }

  try {
    $line = Get-Content -Path $cachePath | Where-Object {
      $_ -like "CMAKE_GENERATOR:*="
    } | Select-Object -First 1

    if (-not $line) {
      return ""
    }

    $idx = $line.IndexOf("=")
    if ($idx -lt 0 -or $idx -ge ($line.Length - 1)) {
      return ""
    }

    return $line.Substring($idx + 1).Trim()
  } catch {
    return ""
  }
}

function Test-BuildDirManifestHealthy {
  param(
    [string]$BuildDirPath,
    [string]$ConfigName = "Release"
  )

  if (-not $BuildDirPath -or $BuildDirPath.Trim().Length -eq 0) {
    return @{ Healthy = $false; Reason = "Build directory path is empty."; Generator = "" }
  }

  if (-not (Test-Path $BuildDirPath)) {
    return @{ Healthy = $false; Reason = "Build directory does not exist."; Generator = "" }
  }

  $cachePath = Join-Path $BuildDirPath "CMakeCache.txt"
  if (-not (Test-Path $cachePath)) {
    return @{ Healthy = $false; Reason = "CMakeCache.txt is missing."; Generator = "" }
  }

  $generator = Resolve-GeneratorForBuildDir -BuildDirPath $BuildDirPath
  if (-not $generator) {
    return @{ Healthy = $false; Reason = "CMAKE_GENERATOR could not be read from CMakeCache.txt."; Generator = "" }
  }

  if ($generator -match '(?i)Ninja') {
    $manifestPath = Join-Path $BuildDirPath "build.ninja"
    if (-not (Test-Path $manifestPath)) {
      return @{ Healthy = $false; Reason = "build.ninja is missing."; Generator = $generator }
    }

    try {
      $lines = Get-Content -Path $manifestPath -ErrorAction Stop
      foreach ($line in $lines) {
        if ($line -match '^\s*include\s+(.+?)\s*$') {
          $includeRel = $Matches[1].Trim().Trim('"')
          if (-not $includeRel) { continue }
          if ($includeRel -eq "CMakeFiles/impl-.ninja") {
            return @{ Healthy = $false; Reason = "build.ninja references CMakeFiles/impl-.ninja (empty config include)."; Generator = $generator }
          }
          if ($includeRel.StartsWith("CMakeFiles/")) {
            $includePath = Join-Path $BuildDirPath ($includeRel.Replace('/', '\'))
            if (-not (Test-Path $includePath)) {
              return @{ Healthy = $false; Reason = ("build.ninja includes missing file '" + $includeRel + "'."); Generator = $generator }
            }
          }
        }
      }
    } catch {
      return @{ Healthy = $false; Reason = ("Failed to validate build.ninja: " + $_.Exception.Message); Generator = $generator }
    }
  }

  if ($generator -match '(?i)Visual Studio') {
    $solution = Get-ChildItem -Path $BuildDirPath -Filter *.sln -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $solution) {
      return @{ Healthy = $false; Reason = "Visual Studio generator cache exists but no .sln file was found."; Generator = $generator }
    }
  }

  return @{ Healthy = $true; Reason = ""; Generator = $generator }
}

function Test-TcpReachable {
  param(
    [string]$HostName,
    [int]$Port = 443,
    [int]$TimeoutMs = 2000
  )

  try {
    $client = New-Object System.Net.Sockets.TcpClient
    $iar = $client.BeginConnect($HostName, $Port, $null, $null)
    $ok = $iar.AsyncWaitHandle.WaitOne([Math]::Max(100, $TimeoutMs), $false)
    if (-not $ok) {
      try { $client.Close() } catch {}
      return $false
    }
    $client.EndConnect($iar)
    $client.Close()
    return $true
  } catch {
    return $false
  }
}

function Test-LocalSdl2ConfigPresent {
  param([string]$RepoRoot)

  $paths = New-Object System.Collections.Generic.List[string]

  if ($env:VCPKG_ROOT) {
    $paths.Add((Join-Path $env:VCPKG_ROOT "installed\\x64-windows\\share\\sdl2\\SDL2Config.cmake"))
    $paths.Add((Join-Path $env:VCPKG_ROOT "installed\\x64-windows\\share\\sdl2\\sdl2-config.cmake"))
  }

  $paths.Add((Join-Path $RepoRoot "vcpkg_installed\\x64-windows\\share\\sdl2\\SDL2Config.cmake"))
  $paths.Add((Join-Path $RepoRoot "vcpkg_installed\\x64-windows\\share\\sdl2\\sdl2-config.cmake"))
  $paths.Add("C:\\vcpkg\\installed\\x64-windows\\share\\sdl2\\SDL2Config.cmake")
  $paths.Add("C:\\vcpkg\\installed\\x64-windows\\share\\sdl2\\sdl2-config.cmake")
  $paths.Add((Join-Path $env:USERPROFILE "vcpkg\\installed\\x64-windows\\share\\sdl2\\SDL2Config.cmake"))
  $paths.Add((Join-Path $env:USERPROFILE "vcpkg\\installed\\x64-windows\\share\\sdl2\\sdl2-config.cmake"))

  foreach ($candidate in $paths) {
    if (-not $candidate) { continue }
    if (Test-Path $candidate) {
      return @{ Found = $true; Path = $candidate }
    }
  }

  return @{ Found = $false; Path = "" }
}

function Test-PresetRequiresUiFetchPreflight {
  param([string]$PresetName)

  if (-not $PresetName) { return $false }
  $p = $PresetName.Trim().ToLowerInvariant()
  return $p -in @("ui", "ui-headless", "ui-runtime-headless", "ui-runtime-fetchdeps")
}

function Invoke-UiDependencyPreflight {
  param([string]$PresetName)

  if (-not (Test-PresetRequiresUiFetchPreflight -PresetName $PresetName)) {
    return
  }

  $repoRoot = Resolve-RepoRoot
  $localSdl = Test-LocalSdl2ConfigPresent -RepoRoot $repoRoot
  if ($localSdl.Found) {
    Write-Host ("[preflight] Found local SDL2 config: " + $localSdl.Path)
  } else {
    Write-Host "[preflight] No local SDL2 config found in common locations."
  }

  $fetchTargets = @(
    @{ Name = "SDL2"; Url = "https://github.com/libsdl-org/SDL.git" },
    @{ Name = "ImGui"; Url = "https://github.com/ocornut/imgui.git" }
  )

  $gitCmd = Get-Command git -ErrorAction SilentlyContinue
  if ($gitCmd) {
    foreach ($target in $fetchTargets) {
      Write-Host ("[preflight] Checking git access to " + $target.Name + "...")
      $gitCheckRc = Invoke-ExternalWithWatchdog -FilePath "git" `
                                                -Arguments @("-c", "credential.interactive=never", "-c", "core.askPass=", "ls-remote", $target.Url, "HEAD") `
                                                -Label ("preflight-git-" + $target.Name.ToLowerInvariant()) `
                                                -TimeoutSeconds 8 `
                                                -HeartbeatSeconds 0 `
                                                -InactivityTimeoutSeconds 4 `
                                                -ProgressTimeoutSeconds 6
      if ($gitCheckRc -ne 0) {
        Write-Warning ("UI dependency preflight failed for preset '" + $PresetName + "': unable to reach " +
                       $target.Name + " repository (" + $target.Url + "). " +
                       "Ensure git can access GitHub in this environment or use local UI dependencies.")
        Exit-WithCode 125
      }
    }
    return
  }

  $fetchReachable = Test-TcpReachable -HostName "github.com" -Port 443 -TimeoutMs 2000

  if (-not $fetchReachable) {
    Write-Warning ("UI dependency preflight failed for preset '" + $PresetName + "': GitHub fetch path is not reachable " +
                   "and no local SDL2 config was found. Install SDL2 locally (e.g., vcpkg) or use a network-enabled environment.")
    Exit-WithCode 125
  }
}

function Test-MsBuildStyleArg {
  param([string]$Arg)
  if (-not $Arg) { return $false }
  return ($Arg -match '^(?i)(/p:|-p:|/m$|/m:|/maxcpucount$|/maxcpucount:|/v:|/verbosity:|/nr:|/nologo$|/bl($|:)|/flp($|:)|/consoleloggerparameters:)')
}

function Split-TargetAndNativeArgs {
  param(
    [string]$RawTarget,
    [string[]]$InitialNativeArgs
  )

  $targets = New-Object System.Collections.Generic.List[string]
  $nativeArgs = New-Object System.Collections.Generic.List[string]
  $autoMoved = New-Object System.Collections.Generic.List[string]
  $dropped = New-Object System.Collections.Generic.List[string]

  if ($InitialNativeArgs) {
    foreach ($arg in $InitialNativeArgs) {
      if ($null -eq $arg) { continue }
      $trimmed = $arg.Trim()
      if ($trimmed.Length -gt 0) {
        $nativeArgs.Add($trimmed)
      }
    }
  }

  $tokens = Split-TargetTokens -Raw $RawTarget
  foreach ($token in $tokens) {
    $t = $token.Trim()
    if ($t.Length -eq 0) {
      continue
    }

    # Handle common MSBuild-style arguments accidentally appended to -Target.
    if (Test-MsBuildStyleArg -Arg $t) {
      $nativeArgs.Add($t)
      $autoMoved.Add($t)
      continue
    }

    # Any remaining switch-like token is treated as a native build arg.
    if ($t.StartsWith("/") -or $t.StartsWith("-")) {
      $nativeArgs.Add($t)
      $autoMoved.Add($t)
      continue
    }

    # Keep target names conservative to avoid passing malformed tokens.
    if ($t -notmatch '^[A-Za-z0-9_./\\:+-]+$') {
      $dropped.Add($t)
      continue
    }

    $targets.Add($t)
  }

  return @{
    Targets = $targets
    NativeArgs = $nativeArgs
    AutoMoved = $autoMoved
    Dropped = $dropped
  }
}

function Get-NativeArgsForGenerator {
  param(
    [string[]]$NativeArgs,
    [string]$Generator
  )

  $filtered = New-Object System.Collections.Generic.List[string]
  $dropped = New-Object System.Collections.Generic.List[string]
  $generatorName = if ($Generator) { $Generator } else { "" }
  $isVisualStudioGenerator = $generatorName -match '(?i)Visual Studio'

  foreach ($arg in $NativeArgs) {
    if (Test-MsBuildStyleArg -Arg $arg -and -not $isVisualStudioGenerator) {
      $dropped.Add($arg)
      continue
    }
    $filtered.Add($arg)
  }

  return @{
    NativeArgs = $filtered
    Dropped = $dropped
  }
}

function Stop-ProcessTree {
  param([int]$ProcessId)

  if ($ProcessId -le 0) {
    return
  }

  try {
    & cmd /c ("taskkill /T /F /PID " + $ProcessId) *> $null
  } catch {
    # Best effort: fall back to direct Stop-Process below.
  }

  try {
    Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue
  } catch {
    # Best effort.
  }
}

function Stop-OrphanedBuildToolProcesses {
  param([int]$MinAgeSeconds = 90)

  $minAge = [Math]::Max(0, $MinAgeSeconds)
  $cutoff = (Get-Date).AddSeconds(-$minAge)
  $names = @("cmake", "ninja", "msbuild", "cl", "link", "mspdbsrv")

  foreach ($name in $names) {
    $procs = Get-Process -Name $name -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne $PID }
    foreach ($p in $procs) {
      $shouldStop = $true
      try {
        if ($p.StartTime -gt $cutoff) {
          $shouldStop = $false
        }
      } catch {
        # If StartTime is unavailable, treat as stale and stop.
      }

      if (-not $shouldStop) {
        continue
      }

      Write-Warning ("Stopping orphaned toolchain process '" + $p.ProcessName + "' (PID " + $p.Id + ").")
      try {
        Stop-Process -Id $p.Id -Force -ErrorAction Stop
      } catch {
        Write-Warning ("Failed to stop process PID " + $p.Id + ": " + $_.Exception.Message)
      }
    }
  }
}

function Get-ChildProcessIdsRecursively {
  param([int]$RootPid)

  if ($RootPid -le 0) {
    return @()
  }

  $seen = New-Object "System.Collections.Generic.HashSet[int]"
  $queue = New-Object "System.Collections.Generic.Queue[int]"
  $queue.Enqueue($RootPid)

  while ($queue.Count -gt 0) {
    $currentPid = $queue.Dequeue()
    $children = Get-CimInstance Win32_Process -Filter ("ParentProcessId=" + $currentPid) -ErrorAction SilentlyContinue
    foreach ($child in $children) {
      $cid = [int]$child.ProcessId
      if ($seen.Add($cid)) {
        $queue.Enqueue($cid)
      }
    }
  }

  return @($seen)
}

function Get-ProcessTreeActivitySnapshot {
  param([int]$RootPid)

  if ($RootPid -le 0) {
    return @{
      Signature = ""
      Cpu = 0.0
      ProcessCount = 0
      ActiveNames = ""
    }
  }

  $ids = New-Object "System.Collections.Generic.List[int]"
  $ids.Add($RootPid)
  foreach ($cid in (Get-ChildProcessIdsRecursively -RootPid $RootPid)) {
    $ids.Add([int]$cid)
  }

  $idArray = @($ids | Sort-Object -Unique)
  $cpu = 0.0
  $names = New-Object "System.Collections.Generic.HashSet[string]"
  foreach ($procId in $idArray) {
    try {
      $p = Get-Process -Id $procId -ErrorAction Stop
      if ($null -ne $p -and $null -ne $p.CPU) {
        $cpu += [double]$p.CPU
      }
      if ($null -ne $p -and $p.ProcessName) {
        [void]$names.Add($p.ProcessName.ToLowerInvariant())
      }
    } catch {
      # Process ended between enumeration and sampling.
    }
  }

  return @{
    Signature = ($idArray -join ",")
    Cpu = $cpu
    ProcessCount = $idArray.Count
    ActiveNames = (($names | Sort-Object) -join ",")
  }
}

function Invoke-ExternalWithWatchdog {
  param(
    [string]$FilePath,
    [string[]]$Arguments,
    [string]$Label,
    [int]$TimeoutSeconds,
    [int]$HeartbeatSeconds,
    [int]$InactivityTimeoutSeconds = 0,
    [int]$ProgressTimeoutSeconds = 0
  )

  $script:LastWatchdogTimeoutReason = ""
  $timeout = [Math]::Max(0, $TimeoutSeconds)
  $heartbeat = [Math]::Max(0, $HeartbeatSeconds)
  $idleTimeout = [Math]::Max(0, $InactivityTimeoutSeconds)
  $progressTimeout = [Math]::Max(0, $ProgressTimeoutSeconds)
  $quotedArgs = @($Arguments | ForEach-Object { ConvertTo-QuotedArg $_ })
  Write-Host ("[" + $Label + "] " + $FilePath + " " + ($quotedArgs -join " "))

  $argString = ($quotedArgs -join " ")
  $stdoutPath = Join-Path $env:TEMP ("n4x_build_" + [guid]::NewGuid().ToString("N") + ".stdout.log")
  $stderrPath = Join-Path $env:TEMP ("n4x_build_" + [guid]::NewGuid().ToString("N") + ".stderr.log")
  $exitCodePath = Join-Path $env:TEMP ("n4x_build_" + [guid]::NewGuid().ToString("N") + ".exitcode.log")

  $proc = $null
  $state = [hashtable]::Synchronized(@{
    LastOutput = Get-Date
    LastProgress = Get-Date
    LastProgressHint = ""
  })
  [long]$stdoutOffset = 0
  [long]$stderrOffset = 0
  $stdoutCarry = ""
  $stderrCarry = ""

  $isProgressLine = {
    param([string]$Line)
    if (-not $Line) { return $false }
    $t = $Line.Trim()
    if ($t.Length -eq 0) { return $false }
    if ($t -match '^\[[0-9]+\s*/\s*[0-9]+\]') { return $true }
    if ($t -match '(^|\s)[0-9]{1,3}%($|\s)') { return $true }
    if ($t -match '(?i)\b(building|linking|built target|configuring done|generating done|build files have been written|creating|copying|compiling|scanning dependencies|finished)\b') { return $true }
    return $false
  }

  $readChunk = {
    param([string]$Path, [long]$Offset)
    if (-not (Test-Path $Path)) {
      return @{ Text = ""; Offset = $Offset }
    }

    $text = ""
    $newOffset = $Offset
    try {
      $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
      try {
        $null = $fs.Seek($Offset, [System.IO.SeekOrigin]::Begin)
        $sr = New-Object System.IO.StreamReader($fs)
        try {
          $text = $sr.ReadToEnd()
        } finally {
          $sr.Dispose()
        }
        $newOffset = $fs.Position
      } finally {
        $fs.Dispose()
      }
    } catch {
      # Best effort; treat as no new data this tick.
    }

    return @{ Text = $text; Offset = $newOffset }
  }

  $flushChunk = {
    param(
      [string]$Chunk,
      [ref]$Carry,
      [hashtable]$State
    )

    if ($null -eq $Chunk -or $Chunk.Length -eq 0) {
      return
    }
    # Any byte-level output counts as activity, even if no newline has arrived yet.
    $State["LastOutput"] = Get-Date

    $combined = $Carry.Value + $Chunk
    $combined = $combined -replace "`r`n", "`n"
    $parts = $combined -split "`n", -1
    if ($parts.Count -le 0) {
      return
    }

    for ($i = 0; $i -lt ($parts.Count - 1); ++$i) {
      $line = $parts[$i]
      if ($null -eq $line -or $line.Length -eq 0) { continue }
      Write-Host $line
      $State["LastOutput"] = Get-Date
      if (& $isProgressLine $line) {
        $State["LastProgress"] = Get-Date
        $State["LastProgressHint"] = $line.Trim()
      }
    }
    $Carry.Value = $parts[$parts.Count - 1]
  }

  try {
    $quotedFilePath = ConvertTo-QuotedArg $FilePath
    $redirectCmd = $quotedFilePath
    if ($argString -and $argString.Trim().Length -gt 0) {
      $redirectCmd += " " + $argString
    }
    $redirectCmd += " > " + (ConvertTo-QuotedArg $stdoutPath)
    $redirectCmd += " 2> " + (ConvertTo-QuotedArg $stderrPath)
    $redirectCmd += " & echo !ERRORLEVEL! > " + (ConvertTo-QuotedArg $exitCodePath)

    $proc = Start-Process -FilePath "cmd.exe" `
                          -ArgumentList @("/d", "/v:on", "/s", "/c", $redirectCmd) `
                          -NoNewWindow `
                          -PassThru
  } catch {
    Write-Error ("Failed to start process '" + $FilePath + "': " + $_.Exception.Message)
    return 1
  }

  $startedAt = Get-Date
  $lastWatchdog = $startedAt
  $didTimeout = $false
  $timeoutReason = ""
  $childPid = if ($proc) { $proc.Id } else { -1 }
  $activityPollSeconds = 3
  $lastActivityCheck = $startedAt
  $activity = Get-ProcessTreeActivitySnapshot -RootPid $childPid
  $lastTreeSignature = [string]$activity.Signature
  $lastTreeCpu = [double]$activity.Cpu
  $lastTreeCount = [int]$activity.ProcessCount
  $lastActiveNames = [string]$activity.ActiveNames
  $lastWatchdogCpu = $lastTreeCpu

  try {
    while ($proc -and -not $proc.WaitForExit(250)) {
      $stdoutDelta = & $readChunk $stdoutPath $stdoutOffset
      $stdoutOffset = [long]$stdoutDelta.Offset
      & $flushChunk ([string]$stdoutDelta.Text) ([ref]$stdoutCarry) $state

      $stderrDelta = & $readChunk $stderrPath $stderrOffset
      $stderrOffset = [long]$stderrDelta.Offset
      & $flushChunk ([string]$stderrDelta.Text) ([ref]$stderrCarry) $state

      $now = Get-Date
      if (($now - $lastActivityCheck).TotalSeconds -ge $activityPollSeconds) {
        $snap = Get-ProcessTreeActivitySnapshot -RootPid $childPid
        $sig = [string]$snap.Signature
        $cpu = [double]$snap.Cpu
        $procCount = [int]$snap.ProcessCount
        $activeNames = [string]$snap.ActiveNames
        $treeChanged = ($sig -ne $lastTreeSignature -or
                        $procCount -ne $lastTreeCount -or
                        $activeNames -ne $lastActiveNames)
        $cpuDeltaSinceLast = $cpu - $lastTreeCpu
        if ($treeChanged -or $cpuDeltaSinceLast -gt 0.01) {
          $state["LastOutput"] = $now
        }
        if ($treeChanged -or $cpuDeltaSinceLast -gt 0.20) {
          $state["LastProgress"] = $now
          if ($treeChanged) {
            $state["LastProgressHint"] = "process-tree-change"
          } else {
            $state["LastProgressHint"] = ("cpu-delta+" + ([Math]::Round($cpuDeltaSinceLast, 2)).ToString("0.00"))
          }
        }
        $lastTreeSignature = $sig
        $lastTreeCpu = $cpu
        $lastTreeCount = $procCount
        $lastActiveNames = $activeNames
        $lastActivityCheck = $now
      }

      $elapsedSeconds = ($now - $startedAt).TotalSeconds
      $idleSeconds = ($now - [datetime]$state["LastOutput"]).TotalSeconds
      $progressSeconds = ($now - [datetime]$state["LastProgress"]).TotalSeconds
      if ($timeout -gt 0 -and $elapsedSeconds -ge $timeout) {
        $didTimeout = $true
        $timeoutReason = "overall timeout"
        break
      }

      if ($idleTimeout -gt 0 -and
          $elapsedSeconds -ge [Math]::Max(0, $InactivityGraceSeconds) -and
          $idleSeconds -ge $idleTimeout) {
        $didTimeout = $true
        $timeoutReason = "output inactivity timeout"
        break
      }
      if ($progressTimeout -gt 0 -and
          $elapsedSeconds -ge [Math]::Max(0, $InactivityGraceSeconds) -and
          $progressSeconds -ge $progressTimeout) {
        $didTimeout = $true
        $timeoutReason = "progress timeout"
        break
      }

      if ($heartbeat -gt 0 -and (($now - $lastWatchdog).TotalSeconds -ge $heartbeat)) {
        $cpuDelta = [Math]::Max(0.0, $lastTreeCpu - $lastWatchdogCpu)
        Write-Host ("[watchdog] " + $Label + " running (" + [int][Math]::Floor($elapsedSeconds) +
                    " s, idle " + [int][Math]::Floor($idleSeconds) + " s, tree " + $lastTreeCount +
                    " proc, cpu +" + ([Math]::Round($cpuDelta, 2)).ToString("0.00") +
                    ", progress " + [int][Math]::Floor($progressSeconds) + " s, active [" + $lastActiveNames + "])")
        $lastWatchdogCpu = $lastTreeCpu
        $lastWatchdog = $now
      }
    }

    if ($proc) {
      $stdoutDelta = & $readChunk $stdoutPath $stdoutOffset
      $stdoutOffset = [long]$stdoutDelta.Offset
      & $flushChunk ([string]$stdoutDelta.Text) ([ref]$stdoutCarry) $state

      $stderrDelta = & $readChunk $stderrPath $stderrOffset
      $stderrOffset = [long]$stderrDelta.Offset
      & $flushChunk ([string]$stderrDelta.Text) ([ref]$stderrCarry) $state
    }

    if ($didTimeout) {
      $reason = if ($timeoutReason) { $timeoutReason } else { "timeout" }
      $script:LastWatchdogTimeoutReason = $reason
      $progressAge = [int][Math]::Floor(((Get-Date) - [datetime]$state["LastProgress"]).TotalSeconds)
      $progressHint = if ($state.ContainsKey("LastProgressHint")) { [string]$state["LastProgressHint"] } else { "" }
      if (-not $progressHint) { $progressHint = "n/a" }
      Write-Warning ($Label + " hit " + $reason + ". Last progress " + $progressAge +
                     " s ago (" + $progressHint + "). Terminating process tree (PID " + $childPid + ").")
      Stop-ProcessTree -ProcessId $childPid
      Stop-OrphanedBuildToolProcesses -MinAgeSeconds 0
      try {
        Wait-Process -Id $childPid -Timeout 5 -ErrorAction SilentlyContinue
      } catch {
      }
      return 124
    }

    if ($stdoutCarry.Length -gt 0) {
      Write-Host $stdoutCarry
      $stdoutCarry = ""
    }
    if ($stderrCarry.Length -gt 0) {
      Write-Host $stderrCarry
      $stderrCarry = ""
    }

    try {
      if ($proc) {
        $proc.WaitForExit()
      }
    } catch {
    }

    $parsedExitCode = $null
    if (Test-Path $exitCodePath) {
      try {
        $rawExit = (Get-Content -Path $exitCodePath -ErrorAction Stop | Select-Object -First 1).Trim()
        $tmpExit = 0
        if ([int]::TryParse($rawExit, [ref]$tmpExit)) {
          $parsedExitCode = $tmpExit
        }
      } catch {
      }
    }

    if ($null -ne $parsedExitCode) {
      return [int]$parsedExitCode
    }

    if ($proc -and $proc.HasExited) {
      # Fallback if exit-code sidecar could not be read.
      try {
        $proc.Refresh()
        if ($null -ne $proc.ExitCode) {
          return [int]$proc.ExitCode
        }
      } catch {
      }
    }

    return 1
  } finally {
    try {
      if ($proc) {
        $proc.Dispose()
      }
    } catch {
    }
    try {
      if (Test-Path $stdoutPath) {
        Remove-Item -Path $stdoutPath -Force -ErrorAction SilentlyContinue
      }
    } catch {
    }
    try {
      if (Test-Path $stderrPath) {
        Remove-Item -Path $stderrPath -Force -ErrorAction SilentlyContinue
      }
    } catch {
    }
    try {
      if (Test-Path $exitCodePath) {
        Remove-Item -Path $exitCodePath -Force -ErrorAction SilentlyContinue
      }
    } catch {
    }
  }
}

function Invoke-CMakeBuild {
  param(
    [string]$PresetName,
    [string]$BuildDirPath,
    [string]$ConfigName,
    [string]$TargetName,
    [string[]]$NativeArgs,
    [switch]$CleanFirst,
    [int]$TimeoutSeconds = 0,
    [string]$BuildLabel = "build",
    [int]$InactivityTimeoutOverride = -1,
    [int]$ProgressTimeoutOverride = -1,
    [switch]$DisableParallelForThisBuild
  )

  $parsed = Split-TargetAndNativeArgs -RawTarget $TargetName -InitialNativeArgs $NativeArgs
  $targets = @($parsed.Targets)
  $nativeBuildArgs = @($parsed.NativeArgs)
  $autoMoved = @($parsed.AutoMoved)
  $dropped = @($parsed.Dropped)

  if ($autoMoved.Count -gt 0) {
    Write-Warning ("Moved non-target token(s) from -Target to native build args: " + ($autoMoved -join ", "))
  }

  if ($dropped.Count -gt 0) {
    Write-Warning ("Dropped malformed target token(s): " + ($dropped -join ", "))
  }

  $generator = ""
  if ($BuildDirPath -and $BuildDirPath.Trim().Length -gt 0) {
    $generator = Resolve-GeneratorForBuildDir -BuildDirPath $BuildDirPath
  } else {
    $generator = Resolve-GeneratorForPreset -PresetName $PresetName
  }
  $filteredForGenerator = Get-NativeArgsForGenerator -NativeArgs $nativeBuildArgs -Generator $generator
  $nativeBuildArgs = @($filteredForGenerator.NativeArgs)
  $droppedForGenerator = @($filteredForGenerator.Dropped)
  if ($droppedForGenerator.Count -gt 0) {
    $genLabel = if ($generator) { $generator } else { "unknown/non-MSBuild" }
    Write-Warning ("Dropped MSBuild-specific native arg(s) for generator '" + $genLabel + "': " +
                   ($droppedForGenerator -join ", "))
  }

  $buildArgs = @("--build")
  $usingPresetBuild = $false
  if ($BuildDirPath -and $BuildDirPath.Trim().Length -gt 0) {
    $buildArgs += $BuildDirPath
  } else {
    $usingPresetBuild = $true
    $buildArgs += "--preset"
    $buildArgs += $PresetName
  }
  $buildArgs += "--config"
  $buildArgs += $ConfigName
  # Default to parallel builds, with optional explicit job limit.
  # Important: when building via a CMake build preset that already declares
  # "jobs", avoid adding a bare "--parallel" override because that can bypass
  # preset throttling and cause heavy contention/stalls.
  $presetJobs = 0
  if ($usingPresetBuild) {
    $presetJobs = Resolve-BuildPresetJobs -PresetName $PresetName
  }
  if ((-not $NoParallel) -and (-not $DisableParallelForThisBuild)) {
    if ($ParallelJobs -gt 0) {
      $buildArgs += "--parallel"
      $buildArgs += $ParallelJobs.ToString()
    } elseif ($usingPresetBuild -and $presetJobs -gt 0) {
      Write-Host ("[build] Preset '" + $PresetName + "' declares jobs=" + $presetJobs +
                  "; keeping preset parallelism (no --parallel override).")
    } else {
      $buildArgs += "--parallel"
    }
  }
  if ($CleanFirst) {
    $buildArgs += "--clean-first"
  }
  if ($targets.Count -gt 0) {
    $buildArgs += "--target"
    $buildArgs += $targets
  } elseif ($TargetName -and $TargetName.Trim().Length -gt 0) {
    Write-Warning "No valid CMake targets remained after sanitization; building the default target set."
  }

  if ($nativeBuildArgs.Count -gt 0) {
    $buildArgs += "--"
    $buildArgs += $nativeBuildArgs
  }

  $effectiveIdleTimeout = if ($InactivityTimeoutOverride -ge 0) { $InactivityTimeoutOverride } else { $InactivityTimeoutSeconds }
  $effectiveProgressTimeout = if ($ProgressTimeoutOverride -ge 0) { $ProgressTimeoutOverride } else { $BuildProgressTimeoutSeconds }

  return Invoke-ExternalWithWatchdog -FilePath "cmake" `
                                     -Arguments $buildArgs `
                                     -Label $BuildLabel `
                                     -TimeoutSeconds $TimeoutSeconds `
                                     -HeartbeatSeconds $WatchdogSeconds `
                                     -InactivityTimeoutSeconds $effectiveIdleTimeout `
                                     -ProgressTimeoutSeconds $effectiveProgressTimeout
}

function Stop-TargetProcessIfRunning {
  param(
    [string]$TargetName
  )

  if (-not $TargetName -or $TargetName.Trim().Length -eq 0) {
    return
  }

  $procName = $TargetName.Trim()
  try {
    $procs = Get-Process -Name $procName -ErrorAction SilentlyContinue
    if ($procs) {
      Write-Warning ("Stopping running process '" + $procName + "' to prevent linker file-lock errors.")
      $procs | Stop-Process -Force
      Start-Sleep -Milliseconds 250
    }
  } catch {
    Write-Warning ("Failed to stop process '" + $procName + "': " + $_.Exception.Message)
  }
}

try {
  if ($MaxTotalSeconds -lt 0) {
    Write-Warning "MaxTotalSeconds cannot be negative; treating as 0 (disabled)."
    $MaxTotalSeconds = 0
  }
  if ($ConfigureProgressTimeoutSeconds -lt 0) {
    Write-Warning "ConfigureProgressTimeoutSeconds cannot be negative; treating as 0 (disabled)."
    $ConfigureProgressTimeoutSeconds = 0
  }
  if ($BuildProgressTimeoutSeconds -lt 0) {
    Write-Warning "BuildProgressTimeoutSeconds cannot be negative; treating as 0 (disabled)."
    $BuildProgressTimeoutSeconds = 0
  }
  if ($InactivityTimeoutSeconds -lt 0) {
    Write-Warning "InactivityTimeoutSeconds cannot be negative; treating as 0 (disabled)."
    $InactivityTimeoutSeconds = 0
  }
  if ($InactivityGraceSeconds -lt 0) {
    Write-Warning "InactivityGraceSeconds cannot be negative; treating as 0."
    $InactivityGraceSeconds = 0
  }
  if ($ParallelJobs -lt 0) {
    Write-Warning "ParallelJobs cannot be negative; treating as 0 (generator default)."
    $ParallelJobs = 0
  }
  if ($NoParallel -and $ParallelJobs -gt 0) {
    Write-Warning ("Ignoring -ParallelJobs " + $ParallelJobs + " because -NoParallel was set.")
  }

  $showHelp = Test-ShowHelp -HelpSwitch:$Help -PresetValue $Preset
  if ($showHelp) {
    Show-Usage
    Exit-WithCode 0
  }

  if ($MaxTotalSeconds -le 0 -and -not $NoAutoMaxTotal) {
    $autoMax = Resolve-AutoMaxTotalSeconds -WillConfigure:$Configure `
                                           -ConfigureTimeoutBudgetSeconds $ConfigureTimeoutSeconds `
                                           -BuildTimeoutBudgetSeconds $BuildTimeoutSeconds `
                                           -TargetName $Target
    if ($autoMax -gt 0) {
      $MaxTotalSeconds = $autoMax
      Write-Warning ("-MaxTotalSeconds not provided; enabling auto watchdog limit of " + $MaxTotalSeconds +
                     "s to prevent long hangs. Use -MaxTotalSeconds to override or -NoAutoMaxTotal to disable.")
    }
  }

  if ($MaxTotalSeconds -le 0 -and $NoAutoMaxTotal -and
      $ConfigureTimeoutSeconds -le 0 -and $BuildTimeoutSeconds -le 0 -and
      $InactivityTimeoutSeconds -le 0 -and $BuildProgressTimeoutSeconds -le 0) {
    Write-Warning "All watchdog timeouts are disabled; stalled tools may run indefinitely."
  }

  if ($MaxTotalSeconds -gt 0) {
    Write-Host ("[watchdog] MaxTotalSeconds=" + $MaxTotalSeconds + "s")
  }

  $useBuildMutexEffective = $UseBuildMutex -and -not $NoBuildMutex
  $killRunningEffective = if ($NoKillRunning) { $false } elseif ($KillRunning) { $true } else { $true }
  $killOrphanedEffective = if ($NoKillOrphanedBuildTools) { $false } elseif ($KillOrphanedBuildTools) { $true } else { $true }
  $retryOnInactivityEffective = if ($NoRetryOnInactivityTimeout) { $false } elseif ($RetryOnInactivityTimeout) { $true } else { $true }
  $autoConfigureOnMissingBuildFiles = -not $NoAutoConfigureOnMissingBuildFiles
  $resolvedBuildDir = Resolve-BuildDirPath -BuildDirInput $BuildDir

  if ($resolvedBuildDir -and $resolvedBuildDir.Trim().Length -gt 0) {
    if (-not (Test-Path $resolvedBuildDir)) {
      if ($Configure) {
        Write-Host ("[build-dir] Build directory '" + $resolvedBuildDir + "' does not exist; configure will create it.")
      } elseif ($autoConfigureOnMissingBuildFiles) {
        Write-Warning ("Build directory '" + $resolvedBuildDir +
                       "' does not exist. Automatically enabling -Configure to (re)generate build files. " +
                       "Use -NoAutoConfigureOnMissingBuildFiles to disable.")
        $Configure = $true
      } else {
        Write-Error ("Build directory '" + $resolvedBuildDir + "' does not exist. Use -Configure or provide an existing CMake binary dir.")
        Exit-WithCode 2
      }
    } else {
      $cachePath = Join-Path $resolvedBuildDir "CMakeCache.txt"
      if (-not $Configure -and -not (Test-Path $cachePath)) {
        if ($autoConfigureOnMissingBuildFiles) {
          Write-Warning ("Build directory '" + $resolvedBuildDir +
                         "' has no CMakeCache.txt. Automatically enabling -Configure to recover. " +
                         "Use -NoAutoConfigureOnMissingBuildFiles to disable.")
          $Configure = $true
        } else {
          Write-Error ("Build directory '" + $resolvedBuildDir + "' has no CMakeCache.txt. Use -Configure first.")
          Exit-WithCode 2
        }
      }
    }
  }

  $buildMutex = $null
  if ($useBuildMutexEffective) {
    $buildMutex = Get-BuildMutex -WaitSeconds $BuildLockWaitSeconds
    if ($null -eq $buildMutex -and $killOrphanedEffective) {
      Write-Warning ("Timed out waiting for build mutex after " + $BuildLockWaitSeconds +
                     " seconds. Attempting orphaned-tool cleanup, then retrying lock once.")
      Stop-OrphanedBuildToolProcesses -MinAgeSeconds $OrphanedToolMinAgeSeconds
      $buildMutex = Get-BuildMutex -WaitSeconds $BuildLockWaitSeconds
    }
    if ($null -eq $buildMutex) {
      if ($BuildLockRequired) {
        Write-Error ("Timed out waiting for build mutex after " + $BuildLockWaitSeconds +
                     " seconds. Another build process is likely running.")
        Exit-WithCode 126
      }
      Write-Warning ("Timed out waiting for build mutex after " + $BuildLockWaitSeconds +
                     " seconds. Continuing without mutex (pass -BuildLockRequired to fail instead).")
    } else {
      Write-Host "[build-lock] Acquired global build mutex."
    }
  }

  $effectivePreset = ""
  if ($resolvedBuildDir -and $resolvedBuildDir.Trim().Length -gt 0) {
    if ($Preset -and $Preset.Trim().Length -gt 0 -and $Preset.Trim().ToLowerInvariant() -ne "core") {
      Write-Warning ("-BuildDir was provided; ignoring -Preset '" + $Preset + "'.")
    }
    Write-Host ("[build-dir] Using explicit build directory '" + $resolvedBuildDir + "'.")
  } else {
    $effectivePreset = Resolve-EffectivePreset -RequestedPreset $Preset `
                                              -TargetName $Target `
                                              -DisableRemap:$NoPresetRemap
  }

  $effectiveBuildDir = $resolvedBuildDir
  if ((-not $effectiveBuildDir -or $effectiveBuildDir.Trim().Length -eq 0) -and
      $effectivePreset -and $effectivePreset.Trim().Length -gt 0) {
    $effectiveBuildDir = Get-PresetBinaryDirCandidate -PresetName $effectivePreset
  }

  if (-not $Configure -and $effectiveBuildDir -and $effectiveBuildDir.Trim().Length -gt 0) {
    if (-not (Test-Path $effectiveBuildDir)) {
      if ($autoConfigureOnMissingBuildFiles) {
        Write-Warning ("Build directory '" + $effectiveBuildDir +
                       "' is missing for this preset. Automatically enabling -Configure to regenerate it. " +
                       "Use -NoAutoConfigureOnMissingBuildFiles to disable.")
        $Configure = $true
      }
    } else {
      $manifestCheck = Test-BuildDirManifestHealthy -BuildDirPath $effectiveBuildDir -ConfigName $Config
      if (-not $manifestCheck.Healthy) {
        if ($autoConfigureOnMissingBuildFiles) {
          $genLabel = if ($manifestCheck.Generator) { $manifestCheck.Generator } else { "unknown generator" }
          Write-Warning ("Detected broken/incomplete build files in '" + $effectiveBuildDir + "' (" +
                         $genLabel + "): " + $manifestCheck.Reason + " " +
                         "Automatically enabling -Configure to self-heal this build directory. " +
                         "Use -NoAutoConfigureOnMissingBuildFiles to disable.")
          $Configure = $true
        } else {
          Write-Error ("Build directory '" + $effectiveBuildDir + "' is not healthy: " + $manifestCheck.Reason +
                       " Run with -Configure to regenerate build files.")
          Exit-WithCode 2
        }
      }
    }
  }

  try {
    if ($Configure) {
      $configureArgs = @()
      if ($resolvedBuildDir -and $resolvedBuildDir.Trim().Length -gt 0) {
        $repoRoot = Resolve-RepoRoot
        $configureArgs = @("-S", $repoRoot, "-B", $resolvedBuildDir)
      } else {
        Invoke-UiDependencyPreflight -PresetName $effectivePreset
        $configureArgs = @("--preset", $effectivePreset)
      }

      Assert-WithinMaxTotalTime -Phase "configure"
      if ($killOrphanedEffective) {
        Stop-OrphanedBuildToolProcesses -MinAgeSeconds $OrphanedToolMinAgeSeconds
      }

      $configureTimeoutEffective = Get-RemainingTimeoutSeconds -RequestedTimeoutSeconds $ConfigureTimeoutSeconds
      if ($ConfigureTimeoutSeconds -gt 0 -and $configureTimeoutEffective -le 0) {
        Write-Warning ("No remaining timeout budget before configure step (MaxTotalSeconds=" + $MaxTotalSeconds + ").")
        Exit-WithCode 124
      }

      $configureExitCode = Invoke-ExternalWithWatchdog -FilePath "cmake" `
                                                       -Arguments $configureArgs `
                                                       -Label "configure" `
                                                       -TimeoutSeconds $configureTimeoutEffective `
                                                       -HeartbeatSeconds $WatchdogSeconds `
                                                       -InactivityTimeoutSeconds $InactivityTimeoutSeconds `
                                                       -ProgressTimeoutSeconds $ConfigureProgressTimeoutSeconds
      if ($configureExitCode -ne 0) {
        Exit-WithCode $configureExitCode
      }
    }

    if ($killRunningEffective) {
      Stop-TargetProcessIfRunning -TargetName $Target
    }

    if ($killOrphanedEffective) {
      Stop-OrphanedBuildToolProcesses -MinAgeSeconds $OrphanedToolMinAgeSeconds
    }

    Assert-WithinMaxTotalTime -Phase "build"
    $buildTimeoutEffective = Get-RemainingTimeoutSeconds -RequestedTimeoutSeconds $BuildTimeoutSeconds
    if ($BuildTimeoutSeconds -gt 0 -and $buildTimeoutEffective -le 0) {
      Write-Warning ("No remaining timeout budget before build step (MaxTotalSeconds=" + $MaxTotalSeconds + ").")
      Exit-WithCode 124
    }

    $exitCode = Invoke-CMakeBuild -PresetName $effectivePreset `
                                  -BuildDirPath $resolvedBuildDir `
                                  -ConfigName $Config `
                                  -TargetName $Target `
                                  -NativeArgs $NativeBuildArgs `
                                  -TimeoutSeconds $buildTimeoutEffective `
                                  -BuildLabel "build"
    if ($exitCode -eq 0) {
      Exit-WithCode 0
    }

    if ($exitCode -eq 124 -and
        $retryOnInactivityEffective -and
        ($script:LastWatchdogTimeoutReason -eq "output inactivity timeout" -or
         $script:LastWatchdogTimeoutReason -eq "progress timeout")) {
      $inactivityRetryTimeout = if ($InactivityTimeoutSeconds -gt 0) {
        [Math]::Min(1800, [Math]::Max(300, $InactivityTimeoutSeconds * 2))
      } else {
        300
      }
      $progressRetryTimeout = if ($BuildProgressTimeoutSeconds -gt 0) {
        [Math]::Min(1800, [Math]::Max(600, $BuildProgressTimeoutSeconds * 2))
      } else {
        600
      }

      Write-Warning ("Build hit " + $script:LastWatchdogTimeoutReason + ". Retrying once in stabilization mode " +
                     "(serial build, inactivity timeout " + $inactivityRetryTimeout +
                     "s, progress timeout " + $progressRetryTimeout + "s).")

      Assert-WithinMaxTotalTime -Phase "inactivity-retry build"
      $retryBuildTimeoutEffective = Get-RemainingTimeoutSeconds -RequestedTimeoutSeconds $BuildTimeoutSeconds
      if ($BuildTimeoutSeconds -gt 0 -and $retryBuildTimeoutEffective -le 0) {
        Write-Warning ("No remaining timeout budget before inactivity-retry build (MaxTotalSeconds=" + $MaxTotalSeconds + ").")
        Exit-WithCode 124
      }

      $retryInactivityExit = Invoke-CMakeBuild -PresetName $effectivePreset `
                                               -BuildDirPath $resolvedBuildDir `
                                               -ConfigName $Config `
                                               -TargetName $Target `
                                               -NativeArgs $NativeBuildArgs `
                                               -TimeoutSeconds $retryBuildTimeoutEffective `
                                               -BuildLabel "build-retry-inactivity" `
                                               -InactivityTimeoutOverride $inactivityRetryTimeout `
                                               -ProgressTimeoutOverride $progressRetryTimeout `
                                               -DisableParallelForThisBuild
      if ($retryInactivityExit -eq 0) {
        Exit-WithCode 0
      }

      $exitCode = $retryInactivityExit
    }

    if ($exitCode -eq 124) {
      Exit-WithCode 124
    }

    if (-not $RetryCleanOnFailure) {
      Exit-WithCode $exitCode
    }

    Write-Warning ("Build failed with exit code " + $exitCode + ". Retrying once with --clean-first.")
    Assert-WithinMaxTotalTime -Phase "clean-retry build"
    $retryTimeoutEffective = Get-RemainingTimeoutSeconds -RequestedTimeoutSeconds $BuildTimeoutSeconds
    if ($BuildTimeoutSeconds -gt 0 -and $retryTimeoutEffective -le 0) {
      Write-Warning ("No remaining timeout budget before clean-retry build (MaxTotalSeconds=" + $MaxTotalSeconds + ").")
      Exit-WithCode 124
    }

    $retryExitCode = Invoke-CMakeBuild -PresetName $effectivePreset `
                                       -BuildDirPath $resolvedBuildDir `
                                       -ConfigName $Config `
                                       -TargetName $Target `
                                       -NativeArgs $NativeBuildArgs `
                                       -CleanFirst `
                                       -TimeoutSeconds $retryTimeoutEffective `
                                       -BuildLabel "build-retry-clean"
    Exit-WithCode $retryExitCode
  } finally {
    if ($buildMutex) {
      try { $buildMutex.ReleaseMutex() | Out-Null } catch {}
      try { $buildMutex.Dispose() } catch {}
      Write-Host "[build-lock] Released global build mutex."
    }
  }
} catch {
  Write-Error ("Unhandled build_core failure: " + $_.Exception.Message)
  Exit-WithCode 1
}
