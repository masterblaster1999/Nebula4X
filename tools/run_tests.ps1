param(
  [string]$Preset = "core-tests-headless",
  [string]$Config = "Release",
  [string]$TestBinaryPath = "",
  [string]$Filter = "",
  [Alias("h", "?")]
  [switch]$Help,
  [switch]$BuildFirst,
  [switch]$ForceBuild,
  [switch]$RebuildIfStale,
  [switch]$NoRebuildIfStale,
  [bool]$SkipBuildIfPresent = $true,
  [switch]$NoSkipBuildIfPresent,
  [switch]$NoPresetRemap,
  [switch]$UseBuildMutex = $false,
  [switch]$NoBuildMutex,
  [switch]$BuildLockRequired,
  [int]$BuildLockWaitSeconds = 15,
  [switch]$KillOrphanedBuildTools = $true,
  [switch]$NoKillOrphanedBuildTools,
  [int]$OrphanedToolMinAgeSeconds = 90,
  [int]$ConfigureTimeoutSeconds = 180,
  [int]$BuildTimeoutSeconds = 600,
  [int]$BuildWatchdogSeconds = 5,
  [switch]$KillRunning = $true,
  [switch]$NoKillRunning,
  [string]$LogLevel = "error",
  [int]$HeartbeatSeconds = 5,
  [string]$JunitPath = "",
  [bool]$VerboseRunner = $true,
  [switch]$Isolated = $true,
  [int]$PerTestTimeoutSeconds = 180,
  [int]$ContinueOnFailure = 1,
  [int]$StartIndex = 1,
  [int]$MaxTests = 0,
  [int]$MaxRunSeconds = 0,
  [int]$MaxTotalSeconds = 0
)

$ErrorActionPreference = "Stop"
$scriptStartTime = Get-Date
$continueOnFailureEnabled = ($ContinueOnFailure -ne 0)

function Show-Usage {
  $usage = @'
run_tests.ps1 - Isolated Nebula4X test runner with watchdog timeouts.

Usage
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_tests.ps1 [options]

Options
  -Preset <name>                  Configure or build preset/alias (default: core-tests-headless)
  -Config <name>                  Build config (default: Release)
  -TestBinaryPath <path>          Explicit test binary path override
  -BuildFirst                     Configure/build nebula4x_tests before running
  -ForceBuild                     Force configure/build even when test binary already exists
  -RebuildIfStale                 Explicitly enable stale-check rebuilds (default: enabled)
  -NoRebuildIfStale               Disable stale-check rebuilds and always reuse existing binary
  -SkipBuildIfPresent <bool>      Skip configure/build if test binary exists (default: true)
  -NoSkipBuildIfPresent           Do not skip configure/build when binary exists
  -Filter <text>                  Run only test names containing text
  -Isolated                       Run each test in separate process (default: on)
  -PerTestTimeoutSeconds <n>      Per-test timeout in isolated mode (default: 180)
  -HeartbeatSeconds <n>           Watchdog heartbeat interval (default: 5)
  -LogLevel <level>               Test log level (default: error)
  -StartIndex <n>                 1-based test start index
  -MaxTests <n>                   Max number of tests to run
  -MaxRunSeconds <n>              Stop run after this total duration
  -MaxTotalSeconds <n>            Hard timeout for whole script runtime (0 = disabled)
  -ContinueOnFailure <0|1>        Continue after failure (default: 1)
  -UseBuildMutex / -NoBuildMutex  Enable/disable build mutex (default: disabled)
  -BuildLockRequired              Fail if mutex cannot be acquired (default: continue without mutex)
  -KillRunning / -NoKillRunning   Stop running test process before run (default: enabled)
  -KillOrphanedBuildTools / -NoKillOrphanedBuildTools
                                  Stop stale cmake/ninja/msbuild during BuildFirst (default: enabled)
  -Help / -?                      Show this help and exit

Examples
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_tests.ps1 -BuildFirst -Filter random_scenario
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\run_tests.ps1 -TestBinaryPath .\out\build\ui-vs\nebula4x_tests.exe -Filter random_scenario
'@
  Write-Host $usage
}

function Should-ShowHelp {
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
    exit 124
  }
}

function Resolve-EffectivePreset {
  param(
    [string]$RequestedPreset,
    [switch]$DisableRemap
  )

  if ($DisableRemap) {
    return $RequestedPreset
  }

  $map = @{
    "core-tests" = "core-tests-headless"
    "ui" = "ui-tests-headless"
    "ui-headless" = "ui-tests-headless"
  }

  if ($map.ContainsKey($RequestedPreset)) {
    $remapped = $map[$RequestedPreset]
    Write-Warning ("Preset '" + $RequestedPreset + "' may contend with IDE background CMake activity. Using '" +
                   $remapped + "' (pass -NoPresetRemap to disable).")
    return $remapped
  }

  return $RequestedPreset
}

function Get-CMakePresetsDoc {
  $path = Join-Path -Path (Get-Location) -ChildPath "CMakePresets.json"
  if (-not (Test-Path $path)) {
    return $null
  }
  try {
    return (Get-Content -Raw -Path $path | ConvertFrom-Json)
  } catch {
    Write-Warning ("Failed to parse CMakePresets.json: " + $_.Exception.Message)
    return $null
  }
}

function Resolve-RunnerPresets {
  param([string]$RequestedPreset)

  $doc = Get-CMakePresetsDoc
  if ($null -eq $doc) {
    return @{
      configure = $RequestedPreset
      build = $RequestedPreset
      binaryDir = ""
      source = "fallback"
    }
  }

  $configureByName = @{}
  $buildToConfigure = @{}
  $buildToTargets = @{}
  $configureToBuilds = @{}

  foreach ($cp in @($doc.configurePresets)) {
    if ($cp -and $cp.name) {
      $configureByName[$cp.name] = $cp
    }
  }

  foreach ($bp in @($doc.buildPresets)) {
    if (-not $bp -or -not $bp.name) { continue }
    $cfg = [string]$bp.configurePreset
    if ($cfg.Length -eq 0) { continue }

    $buildToConfigure[$bp.name] = $cfg
    $buildToTargets[$bp.name] = @($bp.targets)
    if (-not $configureToBuilds.ContainsKey($cfg)) {
      $configureToBuilds[$cfg] = New-Object System.Collections.Generic.List[string]
    }
    $configureToBuilds[$cfg].Add($bp.name) | Out-Null
  }

  # If user passed a build preset, use its mapped configure preset.
  if ($buildToConfigure.ContainsKey($RequestedPreset)) {
    $cfgName = $buildToConfigure[$RequestedPreset]
    $binaryDir = ""
    if ($configureByName.ContainsKey($cfgName)) {
      $binaryDir = [string]$configureByName[$cfgName].binaryDir
    }
    return @{
      configure = $cfgName
      build = $RequestedPreset
      binaryDir = $binaryDir
      source = "build-preset"
    }
  }

  # If user passed a configure preset, pick the best matching build preset.
  if ($configureByName.ContainsKey($RequestedPreset)) {
    $buildName = $RequestedPreset
    if ($buildToConfigure.ContainsKey($buildName) -and $buildToConfigure[$buildName] -eq $RequestedPreset) {
      # same-name build preset exists
    } elseif ($configureToBuilds.ContainsKey($RequestedPreset)) {
      $cands = @($configureToBuilds[$RequestedPreset])
      $preferred = $null
      foreach ($cand in $cands) {
        $targets = @($buildToTargets[$cand])
        if ($targets -contains "nebula4x_tests") {
          $preferred = $cand
          break
        }
      }
      if ($null -eq $preferred -and $cands.Count -gt 0) {
        $preferred = $cands[0]
      }
      if ($preferred) {
        $buildName = $preferred
      }
    }
    return @{
      configure = $RequestedPreset
      build = $buildName
      binaryDir = [string]$configureByName[$RequestedPreset].binaryDir
      source = "configure-preset"
    }
  }

  return @{
    configure = $RequestedPreset
    build = $RequestedPreset
    binaryDir = ""
    source = "fallback"
  }
}

function Normalize-BinaryDir {
  param([string]$RawBinaryDir)

  if (-not $RawBinaryDir) { return "" }
  $raw = $RawBinaryDir.Trim()
  if ($raw.Length -eq 0) { return "" }

  $sourceDir = (Get-Location).Path
  $out = $raw.Replace('${sourceDir}', $sourceDir).Replace('${sourceDir}/', ($sourceDir + [System.IO.Path]::DirectorySeparatorChar))
  $out = $out.Replace('/', [System.IO.Path]::DirectorySeparatorChar).Replace('\', [System.IO.Path]::DirectorySeparatorChar)
  return $out
}

function Resolve-TestBuildDir {
  param(
    [string]$ConfigurePresetName,
    [string]$ResolvedBinaryDir
  )

  $norm = Normalize-BinaryDir -RawBinaryDir $ResolvedBinaryDir
  if ($norm -and $norm.Length -gt 0) {
    return $norm
  }

  switch ($ConfigurePresetName) {
    "core-tests-headless" { return "out\\build\\core-tests" }
    "core-tests" { return "out\\build\\core-tests-vs" }
    "ui-headless" { return "out\\build\\ui" }
    "ui-tests" { return "out\\build\\ui-tests" }
    "ui-tests-headless" { return "out\\build\\ui-tests" }
    "ui-tests-only-headless" { return "out\\build\\ui-tests" }
    "ui-runtime-headless" { return "out\\build\\ui-runtime-headless" }
    "ui" { return "out\\build\\ui-vs" }
    default { return "out\\build\\core-tests" }
  }
}

function Resolve-TestExecutablePath {
  param(
    [string]$ExplicitPath,
    [string]$BuildDir,
    [string]$ConfigName
  )

  $candidates = New-Object System.Collections.Generic.List[string]
  if ($ExplicitPath -and $ExplicitPath.Trim().Length -gt 0) {
    $candidates.Add($ExplicitPath.Trim())
  }

  if ($BuildDir -and $BuildDir.Trim().Length -gt 0) {
    $baseDir = $BuildDir.Trim()
    if ($ConfigName -and $ConfigName.Trim().Length -gt 0) {
      if ([System.IO.Path]::IsPathRooted($baseDir)) {
        $candidates.Add((Join-Path -Path $baseDir -ChildPath ($ConfigName + "\nebula4x_tests.exe")))
      } else {
        $candidates.Add((Join-Path -Path "." -ChildPath ($baseDir + "\" + $ConfigName + "\nebula4x_tests.exe")))
      }
    }
    # Ninja single-config layout.
    if ([System.IO.Path]::IsPathRooted($baseDir)) {
      $candidates.Add((Join-Path -Path $baseDir -ChildPath "nebula4x_tests.exe"))
    } else {
      $candidates.Add((Join-Path -Path "." -ChildPath ($baseDir + "\nebula4x_tests.exe")))
    }
  }

  foreach ($candidate in $candidates) {
    if (Test-Path $candidate) {
      return @{ Path = $candidate; Candidates = $candidates }
    }
  }

  return @{ Path = ""; Candidates = $candidates }
}

function Get-KnownBuildDirs {
  $dirs = New-Object System.Collections.Generic.List[string]
  $seen = New-Object System.Collections.Generic.HashSet[string]([System.StringComparer]::OrdinalIgnoreCase)

  $addDir = {
    param([string]$DirPath)
    if (-not $DirPath) { return }
    $p = $DirPath.Trim()
    if ($p.Length -eq 0) { return }
    if ($seen.Add($p)) {
      $dirs.Add($p) | Out-Null
    }
  }

  # Current resolved build dir first.
  & $addDir $testBuildDir

  # Common fallback build dirs used by this repo.
  foreach ($d in @(
      "out\build\core-tests",
      "out\build\core-tests-vs",
      "out\build\ui-tests",
      "out\build\ui",
      "out\build\ui-vs"
    )) {
    & $addDir $d
  }

  # Include all configure preset binary dirs from CMakePresets.json.
  $doc = Get-CMakePresetsDoc
  if ($doc -and $doc.configurePresets) {
    foreach ($cp in @($doc.configurePresets)) {
      if (-not $cp) { continue }
      $norm = Normalize-BinaryDir -RawBinaryDir ([string]$cp.binaryDir)
      & $addDir $norm
    }
  }

  return @($dirs)
}

function Resolve-AnyExistingTestExecutable {
  param(
    [string]$ExplicitPath,
    [string]$PrimaryBuildDir,
    [string]$ConfigName
  )

  $primary = Resolve-TestExecutablePath -ExplicitPath $ExplicitPath -BuildDir $PrimaryBuildDir -ConfigName $ConfigName
  if ($primary.Path -and (Test-Path ([string]$primary.Path))) {
    return @{ Path = [string]$primary.Path; FromBuildDir = $PrimaryBuildDir }
  }

  foreach ($dir in @(Get-KnownBuildDirs)) {
    if (-not $dir) { continue }
    $resolved = Resolve-TestExecutablePath -ExplicitPath $ExplicitPath -BuildDir $dir -ConfigName $ConfigName
    if ($resolved.Path -and (Test-Path ([string]$resolved.Path))) {
      return @{ Path = [string]$resolved.Path; FromBuildDir = $dir }
    }
  }

  return @{ Path = ""; FromBuildDir = "" }
}

function Resolve-FullPath {
  param([string]$PathValue)
  if (-not $PathValue -or $PathValue.Trim().Length -eq 0) {
    return ""
  }
  if ([System.IO.Path]::IsPathRooted($PathValue)) {
    return $PathValue
  }
  return (Join-Path -Path (Get-Location).Path -ChildPath $PathValue)
}

function Get-ProjectInputsLatestWriteUtc {
  $latest = [datetime]::MinValue
  $roots = @("src", "include", "tests", "cmake")
  foreach ($r in $roots) {
    $full = Resolve-FullPath -PathValue $r
    if (-not (Test-Path $full)) { continue }
    $files = Get-ChildItem -Path $full -Recurse -File -ErrorAction SilentlyContinue
    foreach ($f in $files) {
      if ($f.LastWriteTimeUtc -gt $latest) {
        $latest = $f.LastWriteTimeUtc
      }
    }
  }

  foreach ($f in @("CMakeLists.txt", "CMakePresets.json", "tools\run_tests.ps1")) {
    $full = Resolve-FullPath -PathValue $f
    if (-not (Test-Path $full)) { continue }
    $item = Get-Item -Path $full -ErrorAction SilentlyContinue
    if ($item -and $item.LastWriteTimeUtc -gt $latest) {
      $latest = $item.LastWriteTimeUtc
    }
  }

  return $latest
}

function Is-TestBinaryStale {
  param([string]$ExePath)

  if (-not $ExePath -or -not (Test-Path $ExePath)) {
    return $true
  }

  $exeItem = Get-Item -Path $ExePath -ErrorAction SilentlyContinue
  if (-not $exeItem) {
    return $true
  }

  $inputsLatest = Get-ProjectInputsLatestWriteUtc
  if ($inputsLatest -eq [datetime]::MinValue) {
    return $false
  }

  return $exeItem.LastWriteTimeUtc -lt $inputsLatest
}

function Acquire-BuildMutex {
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

function Stop-TestsProcessIfRunning {
  try {
    $procs = Get-Process -Name "nebula4x_tests" -ErrorAction SilentlyContinue
    if ($procs) {
      Write-Warning "Stopping running process 'nebula4x_tests' to prevent linker file-lock errors."
      $procs | Stop-Process -Force
      Start-Sleep -Milliseconds 250
    }
  } catch {
    Write-Warning ("Failed to stop running test process: " + $_.Exception.Message)
  }
}

function Quote-Arg {
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

function Stop-ProcessTree {
  param([int]$ProcessId)

  if ($ProcessId -le 0) {
    return
  }

  try {
    & cmd /c ("taskkill /T /F /PID " + $ProcessId) *> $null
  } catch {
    # Best effort: fall back to Stop-Process below.
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

function Invoke-ExternalWithWatchdog {
  param(
    [string]$FilePath,
    [string[]]$Arguments,
    [string]$Label,
    [int]$TimeoutSeconds,
    [int]$HeartbeatSeconds
  )

  $timeout = [Math]::Max(0, $TimeoutSeconds)
  $heartbeat = [Math]::Max(0, $HeartbeatSeconds)
  $quotedArgs = @($Arguments | ForEach-Object { Quote-Arg $_ })
  Write-Host ("[" + $Label + "] " + $FilePath + " " + ($quotedArgs -join " "))

  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $FilePath
  $psi.Arguments = ($quotedArgs -join " ")
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $false

  $proc = New-Object System.Diagnostics.Process
  $proc.StartInfo = $psi
  $null = $proc.Start()

  $startedAt = Get-Date
  $lastWatchdog = $startedAt
  $didTimeout = $false
  $childPid = $proc.Id

  try {
    while (-not $proc.WaitForExit(250)) {
      $now = Get-Date
      $elapsedSeconds = ($now - $startedAt).TotalSeconds
      if ($timeout -gt 0 -and $elapsedSeconds -ge $timeout) {
        $didTimeout = $true
        break
      }

      if ($heartbeat -gt 0 -and (($now - $lastWatchdog).TotalSeconds -ge $heartbeat)) {
        Write-Host ("[watchdog] " + $Label + " running (" + [int][Math]::Floor($elapsedSeconds) + " s)")
        $lastWatchdog = $now
      }
    }

    if ($didTimeout) {
      Write-Warning ($Label + " timed out after " + $timeout + " seconds. Terminating process tree (PID " + $childPid + ").")
      Stop-ProcessTree -ProcessId $childPid
      Stop-OrphanedBuildToolProcesses -MinAgeSeconds 0
      try {
        Wait-Process -Id $childPid -Timeout 5 -ErrorAction SilentlyContinue
      } catch {
      }
      return 124
    }

    $proc.Refresh()
    return [int]$proc.ExitCode
  } finally {
    $proc.Dispose()
  }
}

if (Should-ShowHelp -HelpSwitch:$Help -PresetValue $Preset) {
  Show-Usage
  exit 0
}

if ($MaxTotalSeconds -lt 0) {
  Write-Warning "MaxTotalSeconds cannot be negative; treating as 0 (disabled)."
  $MaxTotalSeconds = 0
}
if ($MaxTotalSeconds -gt 0) {
  Write-Host ("[watchdog] MaxTotalSeconds=" + $MaxTotalSeconds + "s")
}

$effectivePreset = Resolve-EffectivePreset -RequestedPreset $Preset -DisableRemap:$NoPresetRemap
$presetSel = Resolve-RunnerPresets -RequestedPreset $effectivePreset
$configurePreset = [string]$presetSel.configure
$buildPreset = [string]$presetSel.build
$testBuildDir = Resolve-TestBuildDir -ConfigurePresetName $configurePreset -ResolvedBinaryDir ([string]$presetSel.binaryDir)
$skipBuildIfPresentEffective = $SkipBuildIfPresent -and -not $NoSkipBuildIfPresent
$rebuildIfStaleEffective = -not $NoRebuildIfStale

if (($configurePreset -ne $buildPreset) -or ($effectivePreset -ne $configurePreset)) {
  Write-Host ("[preset] requested='" + $Preset + "' effective='" + $effectivePreset +
              "' configure='" + $configurePreset + "' build='" + $buildPreset + "'")
}
$useBuildMutexEffective = $UseBuildMutex -and -not $NoBuildMutex
$killOrphanedEffective = $KillOrphanedBuildTools -and -not $NoKillOrphanedBuildTools
$killRunningEffective = $KillRunning -and -not $NoKillRunning

if ($BuildFirst) {
  $preExistingExe = Resolve-AnyExistingTestExecutable -ExplicitPath $TestBinaryPath -PrimaryBuildDir $testBuildDir -ConfigName $Config
  $haveExistingExe = ($preExistingExe.Path -and (Test-Path ([string]$preExistingExe.Path)))
  $haveFreshExe = $false
  if ($haveExistingExe) {
    if ($rebuildIfStaleEffective) {
      $haveFreshExe = -not (Is-TestBinaryStale -ExePath ([string]$preExistingExe.Path))
    } else {
      $haveFreshExe = $true
    }
  }
  if ($skipBuildIfPresentEffective -and -not $ForceBuild -and $haveFreshExe) {
    Write-Host ("[build] Skipping configure/build because test binary is up-to-date: " + [string]$preExistingExe.Path)
    if ($preExistingExe.FromBuildDir -and ([string]$preExistingExe.FromBuildDir) -ne [string]$testBuildDir) {
      Write-Host ("[build] Reusing existing binary from: " + [string]$preExistingExe.FromBuildDir)
      $testBuildDir = [string]$preExistingExe.FromBuildDir
    }
  } else {
  if ($haveExistingExe -and -not $haveFreshExe -and -not $ForceBuild) {
    Write-Host ("[build] Existing test binary is stale, rebuilding: " + [string]$preExistingExe.Path)
  }
  $buildMutex = $null
  if ($useBuildMutexEffective) {
    $buildMutex = Acquire-BuildMutex -WaitSeconds $BuildLockWaitSeconds
    if ($null -eq $buildMutex -and $killOrphanedEffective) {
      Write-Warning ("Timed out waiting for build mutex after " + $BuildLockWaitSeconds +
                     " seconds. Attempting orphaned-tool cleanup, then retrying lock once.")
      Stop-OrphanedBuildToolProcesses -MinAgeSeconds $OrphanedToolMinAgeSeconds
      $buildMutex = Acquire-BuildMutex -WaitSeconds $BuildLockWaitSeconds
    }
    if ($null -eq $buildMutex) {
      if ($BuildLockRequired) {
        Write-Error ("Timed out waiting for build mutex after " + $BuildLockWaitSeconds +
                     " seconds. Another build process is likely running.")
        exit 126
      }
      Write-Warning ("Timed out waiting for build mutex after " + $BuildLockWaitSeconds +
                     " seconds. Continuing without mutex (pass -BuildLockRequired to fail instead).")
    } else {
      Write-Host "[build-lock] Acquired global build mutex."
    }
  }

  try {
    if ($killRunningEffective) {
      Stop-TestsProcessIfRunning
    }
    if ($killOrphanedEffective) {
      Stop-OrphanedBuildToolProcesses -MinAgeSeconds $OrphanedToolMinAgeSeconds
    }

    $buildDirFull = Resolve-FullPath -PathValue $testBuildDir
    $cachePath = ""
    if ($buildDirFull -and $buildDirFull.Length -gt 0) {
      $cachePath = Join-Path -Path $buildDirFull -ChildPath "CMakeCache.txt"
    }
    $needConfigure = $true
    if ($cachePath -and (Test-Path $cachePath) -and -not $ForceBuild) {
      $needConfigure = $false
      Write-Host ("[configure] Skipping configure (existing cache): " + $cachePath)
    }

    Assert-WithinMaxTotalTime -Phase "build-tests"
    $configureTimeoutEffective = Get-RemainingTimeoutSeconds -RequestedTimeoutSeconds $ConfigureTimeoutSeconds
    $buildTimeoutEffective = Get-RemainingTimeoutSeconds -RequestedTimeoutSeconds $BuildTimeoutSeconds
    if (($ConfigureTimeoutSeconds -gt 0 -and $configureTimeoutEffective -le 0) -or
        ($BuildTimeoutSeconds -gt 0 -and $buildTimeoutEffective -le 0)) {
      Write-Warning ("No remaining timeout budget before build-tests step (MaxTotalSeconds=" + $MaxTotalSeconds + ").")
      exit 124
    }

    $buildScript = Join-Path -Path (Get-Location) -ChildPath "tools\build_core.ps1"
    if (-not (Test-Path $buildScript)) {
      Write-Error ("Missing build helper script: " + $buildScript)
      exit 1
    }

    $psExe = "powershell"
    $buildArgs = @(
      "-NoProfile",
      "-ExecutionPolicy", "Bypass",
      "-File", $buildScript,
      "-Preset", $buildPreset,
      "-Config", $Config,
      "-Target", "nebula4x_tests",
      "-ConfigureTimeoutSeconds", $configureTimeoutEffective.ToString(),
      "-BuildTimeoutSeconds", $buildTimeoutEffective.ToString(),
      "-WatchdogSeconds", [Math]::Max(0, $BuildWatchdogSeconds).ToString(),
      "-NoBuildMutex"
    )
    if ($needConfigure) {
      $buildArgs += "-Configure"
    }

    $helperTimeout = 0
    if ($configureTimeoutEffective -gt 0 -or $buildTimeoutEffective -gt 0) {
      $helperTimeout = [Math]::Max(1, $configureTimeoutEffective + $buildTimeoutEffective + 60)
    }
    if ($MaxTotalSeconds -gt 0) {
      $remainingBudget = Get-RemainingTimeoutSeconds -RequestedTimeoutSeconds 0
      if ($remainingBudget -le 0) {
        Write-Warning ("No remaining timeout budget before build helper (MaxTotalSeconds=" + $MaxTotalSeconds + ").")
        exit 124
      }
      if ($helperTimeout -eq 0) {
        $helperTimeout = [Math]::Max(1, $remainingBudget)
      } else {
        $helperTimeout = [Math]::Min($helperTimeout, [Math]::Max(1, $remainingBudget))
      }
    }

    $buildRc = Invoke-ExternalWithWatchdog -FilePath $psExe `
                                           -Arguments $buildArgs `
                                           -Label "build-tests" `
                                           -TimeoutSeconds $helperTimeout `
                                           -HeartbeatSeconds $BuildWatchdogSeconds
    if ($buildRc -ne 0) {
      exit [int]$buildRc
    }
  } finally {
    if ($buildMutex) {
      try { $buildMutex.ReleaseMutex() | Out-Null } catch {}
      try { $buildMutex.Dispose() } catch {}
      Write-Host "[build-lock] Released global build mutex."
    }
  }
  }
}

$resolvedExe = Resolve-TestExecutablePath -ExplicitPath $TestBinaryPath -BuildDir $testBuildDir -ConfigName $Config
$exe = [string]$resolvedExe.Path
if (-not $exe -or -not (Test-Path $exe)) {
  $fallbackExe = Resolve-AnyExistingTestExecutable -ExplicitPath $TestBinaryPath -PrimaryBuildDir $testBuildDir -ConfigName $Config
  if ($fallbackExe.Path -and (Test-Path ([string]$fallbackExe.Path))) {
    $exe = [string]$fallbackExe.Path
    if ($fallbackExe.FromBuildDir -and ([string]$fallbackExe.FromBuildDir) -ne [string]$testBuildDir) {
      Write-Warning ("Primary preset build dir did not contain tests binary; reusing existing binary from " +
                     [string]$fallbackExe.FromBuildDir + ".")
      $testBuildDir = [string]$fallbackExe.FromBuildDir
    }
  }
}
if (-not $exe -or -not (Test-Path $exe)) {
  $hint = ""
  if ($resolvedExe.Candidates.Count -gt 0) {
    $hint = " Checked: " + (($resolvedExe.Candidates | ForEach-Object { "'" + $_ + "'" }) -join ", ")
  }
  Write-Error ("Test executable not found. Build with -BuildFirst or run cmake --build --preset " +
               $buildPreset + " --config " + $Config + " --target nebula4x_tests." + $hint)
  exit 1
}

if ($killRunningEffective) {
  Stop-TestsProcessIfRunning
}

if ($Isolated -and $JunitPath -and $JunitPath.Trim().Length -gt 0) {
  Write-Warning "JUnit aggregation is not supported in isolated mode; falling back to single-process mode for this run."
  $Isolated = $false
}

if (-not $Isolated) {
  $args = @("--heartbeat-seconds", [Math]::Max(0, $HeartbeatSeconds).ToString())
  if ($LogLevel -and $LogLevel.Trim().Length -gt 0) {
    $args += @("--log-level", $LogLevel.Trim())
  }
  if ($JunitPath -and $JunitPath.Trim().Length -gt 0) {
    $args += @("--junit", $JunitPath)
  }
  if ($VerboseRunner) {
    $args += @("--verbose")
  }
  if ($Filter -and $Filter.Trim().Length -gt 0) {
    $args += @("--filter", $Filter)
  }

  Assert-WithinMaxTotalTime -Phase "test execution"
  $singleProcessTimeout = 0
  if ($MaxRunSeconds -gt 0) {
    $singleProcessTimeout = $MaxRunSeconds
  }
  if ($MaxTotalSeconds -gt 0) {
    $remaining = Get-RemainingTimeoutSeconds -RequestedTimeoutSeconds 0
    if ($remaining -le 0) {
      Write-Warning ("No remaining timeout budget before test execution (MaxTotalSeconds=" + $MaxTotalSeconds + ").")
      exit 124
    }
    if ($singleProcessTimeout -le 0) {
      $singleProcessTimeout = $remaining
    } else {
      $singleProcessTimeout = [Math]::Min($singleProcessTimeout, $remaining)
    }
  }

  $rc = Invoke-ExternalWithWatchdog -FilePath $exe `
                                    -Arguments $args `
                                    -Label "run-tests" `
                                    -TimeoutSeconds $singleProcessTimeout `
                                    -HeartbeatSeconds $HeartbeatSeconds
  if ($JunitPath -and (Test-Path $JunitPath)) {
    Write-Host ("[junit] " + $JunitPath)
  }
  exit $rc
}

# Isolated mode: run each selected test as its own process so a hung test can be timed out and killed.
Assert-WithinMaxTotalTime -Phase "test discovery"
$list = & $exe --list
if ($LASTEXITCODE -ne 0) {
  Write-Error "Failed to list tests from $exe."
  exit [int]$LASTEXITCODE
}

$tests = @()
foreach ($line in $list) {
  $name = [string]$line
  if ($null -eq $name) { continue }
  $name = $name.Trim()
  if ($name.Length -eq 0) { continue }
  if ($Filter -and $Filter.Trim().Length -gt 0) {
    if ($name.IndexOf($Filter, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
      continue
    }
  }
  $tests += $name
}

if ($tests.Count -eq 0) {
  if ($Filter -and $Filter.Trim().Length -gt 0) {
    Write-Error ("No tests matched filter: '" + $Filter + "'")
  } else {
    Write-Error "No tests discovered."
  }
  exit 1
}

$StartIndex = [Math]::Max(1, $StartIndex)
$skipCount = $StartIndex - 1
if ($skipCount -ge $tests.Count) {
  Write-Error ("StartIndex " + $StartIndex + " is beyond discovered test count (" + $tests.Count + ").")
  exit 1
}

if ($skipCount -gt 0) {
  $tests = @($tests | Select-Object -Skip $skipCount)
}

if ($MaxTests -gt 0 -and $MaxTests -lt $tests.Count) {
  $tests = @($tests | Select-Object -First $MaxTests)
}

$timeout = [Math]::Max(1, $PerTestTimeoutSeconds)
$heartbeat = [Math]::Max(0, $HeartbeatSeconds)
$maxRunSeconds = [Math]::Max(0, $MaxRunSeconds)
$runStart = Get-Date
$failed = New-Object System.Collections.Generic.List[string]
$timedOut = New-Object System.Collections.Generic.List[string]
$passed = 0
$total = $tests.Count

Write-Host ("[run] isolated mode: " + $total + " test(s), timeout=" + $timeout + "s, heartbeat=" + $heartbeat + "s")

for ($i = 0; $i -lt $tests.Count; $i++) {
  if ($MaxTotalSeconds -gt 0) {
    $remainingAll = Get-RemainingTimeoutSeconds -RequestedTimeoutSeconds 0
    if ($remainingAll -le 0) {
      Write-Host ("MAX-TOTAL-TIME reached (" + $MaxTotalSeconds + " s). Stopping after " + $i + " test(s).")
      break
    }
  }

  if ($maxRunSeconds -gt 0) {
    $elapsedRunSeconds = ((Get-Date) - $runStart).TotalSeconds
    if ($elapsedRunSeconds -ge $maxRunSeconds) {
      Write-Host ("MAX-RUN-TIME reached (" + $maxRunSeconds + " s). Stopping after " + $i + " test(s).")
      break
    }
  }

  $testName = $tests[$i]
  $index = $i + 1
  Write-Host ("RUN   " + $index + "/" + $total + "  " + $testName)

  # Parent process watchdog prints heartbeat updates while waiting, so disable
  # child heartbeat output in isolated mode to keep logs readable.
  $args = @("--exact", $testName, "--heartbeat-seconds", "0")
  if ($LogLevel -and $LogLevel.Trim().Length -gt 0) {
    $args += @("--log-level", $LogLevel.Trim())
  }
  if ($VerboseRunner) {
    $args += @("--verbose")
  }

  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $exe
  $psi.Arguments = (($args | ForEach-Object { Quote-Arg $_ }) -join " ")
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $false

  $proc = New-Object System.Diagnostics.Process
  $proc.StartInfo = $psi
  $null = $proc.Start()
  $didTimeout = $false
  $effectiveTimeout = $timeout
  if ($MaxTotalSeconds -gt 0) {
    $effectiveTimeout = Get-RemainingTimeoutSeconds -RequestedTimeoutSeconds $timeout
    if ($effectiveTimeout -le 0) {
      try { $proc.Kill() } catch {}
      try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch {}
      $proc.Dispose()
      Write-Host ("MAX-TOTAL-TIME reached (" + $MaxTotalSeconds + " s). Stopping after " + $i + " test(s).")
      break
    }
    $effectiveTimeout = [Math]::Max(1, $effectiveTimeout)
  }
  $startedAt = Get-Date
  $lastWatchdog = $startedAt

  while (-not $proc.HasExited) {
    Start-Sleep -Milliseconds 250

    $now = Get-Date
    $elapsedSeconds = ($now - $startedAt).TotalSeconds
    if ($elapsedSeconds -ge $effectiveTimeout) {
      $didTimeout = $true
      break
    }

    if ($heartbeat -gt 0 -and (($now - $lastWatchdog).TotalSeconds -ge $heartbeat)) {
      Write-Host ("[watchdog] " + $index + "/" + $total + "  " + $testName +
                  " running (" + [int][Math]::Floor($elapsedSeconds) + " s)")
      $lastWatchdog = $now
    }
  }

  if (-not $didTimeout) {
    $proc.WaitForExit() | Out-Null
  }

  if ($didTimeout) {
    Write-Host ("TIMEOUT  " + $testName + "  (" + $effectiveTimeout + " s)")
    try {
      if (-not $proc.HasExited) {
        $proc.Kill()
      }
      Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
      Stop-ProcessTree -ProcessId $proc.Id
      Start-Sleep -Milliseconds 100
    } catch {
      Write-Warning ("Failed to stop timed out process for test '" + $testName + "': " + $_.Exception.Message)
    }
    $proc.Dispose()
    $failed.Add($testName)
    $timedOut.Add($testName)
    if (-not $continueOnFailureEnabled) {
      break
    }
    continue
  }

  $proc.Refresh()
  $rc = [int]$proc.ExitCode
  if ($rc -eq 0) {
    $passed++
  } else {
    $failed.Add($testName)
    Write-Host ("FAIL  " + $testName + "  (rc=" + $rc + ")")
    if (-not $continueOnFailureEnabled) {
      $proc.Dispose()
      break
    }
  }
  $proc.Dispose()
}

$failedCount = $failed.Count
if ($failedCount -eq 0) {
  Write-Host ("All tests passed (" + $passed + "/" + $total + ").")
  exit 0
}

Write-Host (($failedCount).ToString() + " test(s) failed.")
if ($timedOut.Count -gt 0) {
  Write-Host ("Timed out: " + ($timedOut -join ", "))
}
Write-Host ("Failed: " + ($failed -join ", "))
exit 1
