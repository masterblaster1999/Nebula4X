<#
.SYNOPSIS
  Robust wrapper for building nebula4x.exe via tools/build_core.ps1.
.DESCRIPTION
  Adds sane defaults, fast-path execution, and optional hard total timeout.
#>
param(
  [Alias("h", "?")]
  [switch]$Help,
  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]]$RawArgs = @()
)

$ErrorActionPreference = "Stop"

function Show-Usage {
  $usage = @'
build_game.ps1 - Robust wrapper for building nebula4x.exe via build_core.ps1.

Usage
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build_game.ps1 [options]

Wrapper-only options
  -ForceBuild                     Ignore fast-path and force a rebuild
  -FullUI                         Build the real SDL2/ImGui UI (uses preset ui-runtime-fetchdeps)
  -MaxTotalSeconds <n>            Hard timeout for this wrapper process (0 = disabled)
  -Help / -?                      Show this help and exit

Forwarded options
  All other options are forwarded to tools\build_core.ps1.

Defaults added when not provided
  -Preset ui-runtime-headless (or ui-runtime-fetchdeps when -FullUI is set)
  -Target nebula4x_main_all
  -Config Release
  -BuildTimeoutSeconds 600
  -WatchdogSeconds 5
  -KillOrphanedBuildTools
  -OrphanedToolMinAgeSeconds 1
  -EmitScriptRc

Notes
  - Fast-path is used only when no forwarded build args are provided.
  - If -BuildTimeoutSeconds is set and -ConfigureTimeoutSeconds is not, this
    wrapper aligns configure timeout to the same value for predictable runs.
  - In -FullUI mode, this wrapper performs a fast dependency/network preflight
    and exits quickly with code 125 when FetchContent cannot be reached.

Examples
  .\tools\build_game.cmd
  .\tools\build_game.cmd -FullUI -ForceBuild
  .\tools\build_game.cmd -ForceBuild -BuildTimeoutSeconds 120 -ConfigureTimeoutSeconds 120
  .\tools\build_game.cmd -ForceBuild -BuildTimeoutSeconds 30 -ConfigureTimeoutSeconds 30 -MaxTotalSeconds 75
'@
  Write-Host $usage
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
    # Best effort.
  }

  try {
    Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue
  } catch {
    # Best effort.
  }
}

function Stop-OrphanedBuildToolProcesses {
  param([int]$MinAgeSeconds = 0)

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
      }

      if (-not $shouldStop) {
        continue
      }

      Write-Warning ("Stopping leftover toolchain process '" + $p.ProcessName + "' (PID " + $p.Id + ").")
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

function Try-ParseInt {
  param(
    [string]$Value,
    [ref]$Parsed
  )

  $tmp = 0
  if ([int]::TryParse($Value, [ref]$tmp)) {
    $Parsed.Value = $tmp
    return $true
  }
  return $false
}

function Resolve-RepoRoot {
  $scriptDir = Split-Path -Parent $PSCommandPath
  return (Resolve-Path (Join-Path $scriptDir "..")).Path
}

function Resolve-PresetBinaryDir {
  param(
    [string]$RepoRoot,
    [string]$PresetName
  )

  if (-not $PresetName -or $PresetName.Trim().Length -eq 0) {
    return ""
  }

  $presetsPath = Join-Path $RepoRoot "CMakePresets.json"
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
    $binaryDir = $binaryDir.Replace('${sourceDir}', $RepoRoot)
    if ([System.IO.Path]::IsPathRooted($binaryDir)) {
      return $binaryDir
    }
    return (Resolve-Path (Join-Path $RepoRoot $binaryDir)).Path
  } catch {
    return ""
  }
}

function Get-ExeCandidates {
  param([string]$BinaryDir)

  if (-not $BinaryDir) {
    return @()
  }

  return @(
    (Join-Path $BinaryDir "nebula4x.exe"),
    (Join-Path $BinaryDir "Release\\nebula4x.exe"),
    (Join-Path $BinaryDir "Debug\\nebula4x.exe"),
    (Join-Path $BinaryDir "RelWithDebInfo\\nebula4x.exe"),
    (Join-Path $BinaryDir "MinSizeRel\\nebula4x.exe")
  )
}

function Resolve-FirstExistingPath {
  param([string[]]$Candidates)

  foreach ($path in $Candidates) {
    if (-not $path) { continue }
    if (Test-Path $path) {
      return $path
    }
  }
  return ""
}

function Get-CMakeCacheVar {
  param(
    [string]$CachePath,
    [string]$VarName
  )

  if (-not (Test-Path $CachePath)) {
    return $null
  }

  try {
    $pattern = "^" + [regex]::Escape($VarName) + ":[^=]*=(.*)$"
    $line = Get-Content -Path $CachePath | Where-Object { $_ -match $pattern } | Select-Object -First 1
    if (-not $line) {
      return $null
    }
    $m = [regex]::Match($line, $pattern)
    if (-not $m.Success) {
      return $null
    }
    return $m.Groups[1].Value.Trim()
  } catch {
    return $null
  }
}

function Normalize-CMakeBoolString {
  param([string]$Value)

  if ($null -eq $Value) { return "" }
  $v = $Value.Trim().ToUpperInvariant()
  if ($v -in @("1", "ON", "TRUE", "YES", "Y")) { return "ON" }
  if ($v -in @("0", "OFF", "FALSE", "NO", "N", "", "NOTFOUND")) { return "OFF" }
  return $v
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

$helpTokens = @("-?", "/?", "-h", "--help", "-help", "help")
$showHelp = $Help
$forceBuild = $false
$fullUI = $false
$maxTotalSeconds = 0
$maxTotalSecondsExplicit = $false

$hasPreset = $false
$hasTarget = $false
$hasConfig = $false
$hasBuildTimeout = $false
$hasConfigureTimeout = $false
$hasWatchdog = $false
$hasConfigure = $false
$hasEmitScriptRc = $false
$hasKillOrphanedSwitch = $false
$hasOrphanedAge = $false

$buildTimeoutValue = $null
$configureTimeoutValue = $null
$watchdogValue = 5
$selectedPreset = "ui-runtime-headless"

$forwardArgs = New-Object System.Collections.Generic.List[string]

$i = 0
while ($i -lt $RawArgs.Count) {
  $token = [string]$RawArgs[$i]
  if ($null -eq $token) {
    $i++
    continue
  }

  $trimmed = $token.Trim()
  if ($trimmed.Length -eq 0) {
    $i++
    continue
  }

  $lower = $trimmed.ToLowerInvariant()

  if ($helpTokens -contains $lower) {
    $showHelp = $true
    $i++
    continue
  }

  if ($lower -eq "-forcebuild" -or $lower -eq "/forcebuild") {
    $forceBuild = $true
    $i++
    continue
  }

  if ($lower -eq "-fullui" -or $lower -eq "/fullui") {
    $fullUI = $true
    $i++
    continue
  }

  if ($lower -match '^[/-]maxtotalseconds[:=](-?\d+)$') {
    $maxTotalSeconds = [int]$Matches[1]
    $maxTotalSecondsExplicit = $true
    $i++
    continue
  }

  if ($lower -eq "-maxtotalseconds" -or $lower -eq "/maxtotalseconds") {
    if (($i + 1) -lt $RawArgs.Count) {
      $parsed = 0
      if (Try-ParseInt -Value ([string]$RawArgs[$i + 1]) -Parsed ([ref]$parsed)) {
        $maxTotalSeconds = $parsed
        $maxTotalSecondsExplicit = $true
        $i += 2
        continue
      }
    }
    Write-Error "Missing or invalid value for -MaxTotalSeconds."
    exit 2
  }

  # Normalize brittle switch-false patterns into explicit opposite switches.
  if ($lower -eq "-usebuildmutex:false" -or $lower -eq "-usebuildmutex:$false" -or $lower -eq "-usebuildmutex:0") {
    $forwardArgs.Add("-NoBuildMutex")
    $i++
    continue
  }
  if ($lower -eq "-killrunning:false" -or $lower -eq "-killrunning:$false" -or $lower -eq "-killrunning:0") {
    $forwardArgs.Add("-NoKillRunning")
    $i++
    continue
  }
  if ($lower -eq "-killorphanedbuildtools:false" -or $lower -eq "-killorphanedbuildtools:$false" -or
      $lower -eq "-killorphanedbuildtools:0") {
    $forwardArgs.Add("-NoKillOrphanedBuildTools")
    $hasKillOrphanedSwitch = $true
    $i++
    continue
  }

  if ($lower -match '^[/-]preset[:=](.+)$') {
    $hasPreset = $true
    $selectedPreset = [string]$Matches[1]
  } elseif ($lower -eq "-preset" -or $lower -eq "/preset") {
    $hasPreset = $true
    if (($i + 1) -lt $RawArgs.Count) {
      $selectedPreset = [string]$RawArgs[$i + 1]
    }
  }

  if ($lower -match '^[/-]target([:=].+)?$') {
    $hasTarget = $true
  }
  if ($lower -match '^[/-]config([:=].+)?$') {
    $hasConfig = $true
  }
  if ($lower -match '^[/-]configure$') {
    $hasConfigure = $true
  }
  if ($lower -match '^[/-]emitscriptrc$') {
    $hasEmitScriptRc = $true
  }
  if ($lower -match '^[/-]killorphanedbuildtools$' -or $lower -match '^[/-]nokillorphanedbuildtools$') {
    $hasKillOrphanedSwitch = $true
  }

  if ($lower -match '^[/-]orphanedtoolminageseconds[:=](-?\d+)$') {
    $hasOrphanedAge = $true
  } elseif ($lower -eq "-orphanedtoolminageseconds" -or $lower -eq "/orphanedtoolminageseconds") {
    $hasOrphanedAge = $true
  }

  if ($lower -match '^[/-]watchdogseconds[:=](-?\d+)$') {
    $hasWatchdog = $true
    $watchdogValue = [int]$Matches[1]
  } elseif ($lower -eq "-watchdogseconds" -or $lower -eq "/watchdogseconds") {
    $hasWatchdog = $true
    if (($i + 1) -lt $RawArgs.Count) {
      $parsedWatchdog = 0
      if (Try-ParseInt -Value ([string]$RawArgs[$i + 1]) -Parsed ([ref]$parsedWatchdog)) {
        $watchdogValue = $parsedWatchdog
      }
    }
  }

  if ($lower -match '^[/-]buildtimeoutseconds[:=](-?\d+)$') {
    $hasBuildTimeout = $true
    $buildTimeoutValue = [int]$Matches[1]
  } elseif ($lower -eq "-buildtimeoutseconds" -or $lower -eq "/buildtimeoutseconds") {
    $hasBuildTimeout = $true
    if (($i + 1) -lt $RawArgs.Count) {
      $parsedBuildTimeout = 0
      if (Try-ParseInt -Value ([string]$RawArgs[$i + 1]) -Parsed ([ref]$parsedBuildTimeout)) {
        $buildTimeoutValue = $parsedBuildTimeout
      }
    }
  }

  if ($lower -match '^[/-]configuretimeoutseconds[:=](-?\d+)$') {
    $hasConfigureTimeout = $true
    $configureTimeoutValue = [int]$Matches[1]
  } elseif ($lower -eq "-configuretimeoutseconds" -or $lower -eq "/configuretimeoutseconds") {
    $hasConfigureTimeout = $true
    if (($i + 1) -lt $RawArgs.Count) {
      $parsedConfigureTimeout = 0
      if (Try-ParseInt -Value ([string]$RawArgs[$i + 1]) -Parsed ([ref]$parsedConfigureTimeout)) {
        $configureTimeoutValue = $parsedConfigureTimeout
      }
    }
  }

  $forwardArgs.Add($trimmed)
  $i++
}

if ($showHelp) {
  Show-Usage
  exit 0
}

$forwardedCountFromCaller = $forwardArgs.Count
$repoRoot = Resolve-RepoRoot
$buildCoreScript = Join-Path $repoRoot "tools\build_core.ps1"

if (-not (Test-Path $buildCoreScript)) {
  Write-Error ("build_core.ps1 not found: '" + $buildCoreScript + "'")
  exit 1
}

if (-not $hasPreset) {
  $forwardArgs.Add("-Preset")
  if ($fullUI) {
    $forwardArgs.Add("ui-runtime-fetchdeps")
    $selectedPreset = "ui-runtime-fetchdeps"
  } else {
    $forwardArgs.Add("ui-runtime-headless")
    $selectedPreset = "ui-runtime-headless"
  }
}

if ($fullUI -and $hasPreset -and $selectedPreset -ne "ui-runtime-fetchdeps") {
  Write-Warning ("-FullUI was requested with explicit preset '" + $selectedPreset +
                 "'. Ensure that preset enables real UI dependencies.")
}
if (-not $hasTarget) {
  $forwardArgs.Add("-Target")
  $forwardArgs.Add("nebula4x_main_all")
}
if (-not $hasConfig) {
  $forwardArgs.Add("-Config")
  $forwardArgs.Add("Release")
}
if (-not $hasBuildTimeout) {
  $forwardArgs.Add("-BuildTimeoutSeconds")
  $forwardArgs.Add("600")
  $buildTimeoutValue = 600
}
if (-not $hasWatchdog) {
  $forwardArgs.Add("-WatchdogSeconds")
  $forwardArgs.Add("5")
  $watchdogValue = 5
}
if (-not $hasKillOrphanedSwitch) {
  $forwardArgs.Add("-KillOrphanedBuildTools")
}
if (-not $hasOrphanedAge) {
  $forwardArgs.Add("-OrphanedToolMinAgeSeconds")
  $forwardArgs.Add("1")
}
if (-not $hasEmitScriptRc) {
  $forwardArgs.Add("-EmitScriptRc")
}

$presetBinaryDir = Resolve-PresetBinaryDir -RepoRoot $repoRoot -PresetName $selectedPreset
if (-not $presetBinaryDir -and $selectedPreset -eq "ui-runtime-headless") {
  $presetBinaryDir = Join-Path $repoRoot "out\build\ui-runtime-headless"
}
if (-not $presetBinaryDir -and $selectedPreset -eq "ui-runtime-fetchdeps") {
  $presetBinaryDir = Join-Path $repoRoot "out\build\ui-runtime-fetch"
}

if (-not $hasConfigure -and $presetBinaryDir) {
  $cachePath = Join-Path $presetBinaryDir "CMakeCache.txt"
  if (-not (Test-Path $cachePath)) {
    $forwardArgs.Add("-Configure")
    $hasConfigure = $true
  } else {
    # Keep cached build dirs aligned with preset dependency guarantees.
    $needsStrictUiDeps = $selectedPreset -in @("ui", "ui-headless", "ui-runtime-headless", "ui-runtime-fetchdeps")
    if ($needsStrictUiDeps) {
      $buildUi = Normalize-CMakeBoolString (Get-CMakeCacheVar -CachePath $cachePath -VarName "NEBULA4X_BUILD_UI")
      $fetchDeps = Normalize-CMakeBoolString (Get-CMakeCacheVar -CachePath $cachePath -VarName "NEBULA4X_FETCH_DEPS")
      $requireUiDeps = Normalize-CMakeBoolString (Get-CMakeCacheVar -CachePath $cachePath -VarName "NEBULA4X_REQUIRE_UI_DEPS")

      if ($buildUi -ne "ON" -or $fetchDeps -ne "ON" -or $requireUiDeps -ne "ON") {
        Write-Warning ("Preset '" + $selectedPreset + "' expects UI deps ON, but cache has " +
                       "NEBULA4X_BUILD_UI=" + $buildUi + ", " +
                       "NEBULA4X_FETCH_DEPS=" + $fetchDeps + ", " +
                       "NEBULA4X_REQUIRE_UI_DEPS=" + $requireUiDeps + ". Reconfiguring.")
        $forwardArgs.Add("-Configure")
        $hasConfigure = $true
      }
    }
  }
}

if ($hasConfigure -and $hasBuildTimeout -and -not $hasConfigureTimeout -and $null -ne $buildTimeoutValue) {
  $forwardArgs.Add("-ConfigureTimeoutSeconds")
  $forwardArgs.Add(([Math]::Max(1, $buildTimeoutValue)).ToString())
  $configureTimeoutValue = [Math]::Max(1, $buildTimeoutValue)
  Write-Warning ("-ConfigureTimeoutSeconds not provided; using " + $configureTimeoutValue +
                 " to match -BuildTimeoutSeconds for predictable short-timeout runs.")
}

$exeCandidates = New-Object System.Collections.Generic.List[string]
if ($presetBinaryDir) {
  foreach ($c in (Get-ExeCandidates -BinaryDir $presetBinaryDir)) {
    $exeCandidates.Add($c)
  }
}
foreach ($c in (Get-ExeCandidates -BinaryDir (Join-Path $repoRoot "out\\build\\ui-runtime-headless"))) {
  $exeCandidates.Add($c)
}
foreach ($c in (Get-ExeCandidates -BinaryDir (Join-Path $repoRoot "out\\build\\ui-runtime"))) {
  $exeCandidates.Add($c)
}

$existingExe = Resolve-FirstExistingPath -Candidates @($exeCandidates)
$preferredExe = if ($exeCandidates.Count -gt 0) { $exeCandidates[0] } else { "" }

if (-not $forceBuild -and -not $hasConfigure -and $forwardedCountFromCaller -eq 0 -and $existingExe) {
  Write-Host ("[fast-path] Using existing '" + $existingExe + "'. Add -ForceBuild to rebuild.")
  & $existingExe --version
  exit [int]$LASTEXITCODE
}

$effectiveBuildTimeout = if ($null -ne $buildTimeoutValue) { $buildTimeoutValue } else { 600 }
$effectiveConfigureTimeout = if ($null -ne $configureTimeoutValue) { $configureTimeoutValue } else { 180 }
if ($effectiveBuildTimeout -lt 0) { $effectiveBuildTimeout = 0 }
if ($effectiveConfigureTimeout -lt 0) { $effectiveConfigureTimeout = 0 }

$computedMax = 0
if ($maxTotalSecondsExplicit) {
  $computedMax = [Math]::Max(0, $maxTotalSeconds)
} else {
  if ($hasConfigure) {
    if ($effectiveBuildTimeout -gt 0 -and $effectiveConfigureTimeout -gt 0) {
      $computedMax = $effectiveBuildTimeout + $effectiveConfigureTimeout + 30
    }
  } elseif ($effectiveBuildTimeout -gt 0) {
    $computedMax = $effectiveBuildTimeout + 30
  }
}

$requiresFetchPreflight = $fullUI -or ($selectedPreset -in @("ui", "ui-headless", "ui-runtime-headless", "ui-runtime-fetchdeps"))
if ($requiresFetchPreflight) {
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
                                                -HeartbeatSeconds 0
      if ($gitCheckRc -ne 0) {
        Write-Warning ("UI dependency preflight failed: unable to reach " + $target.Name + " repository (" +
                       $target.Url + "). Ensure git can access GitHub in this environment.")
        exit 125
      }
    }
  } else {
    # Fallback probe when git isn't available in PATH.
    $fetchReachable = Test-TcpReachable -HostName "github.com" -Port 443 -TimeoutMs 2000
    if (-not $fetchReachable) {
      Write-Warning ("UI dependency preflight failed: GitHub fetch path is not reachable and no local SDL2 config was found. " +
                     "FetchContent cannot download SDL2/ImGui in this environment. " +
                     "Install local SDL2 (e.g., via vcpkg) or run in a network-enabled environment.")
      exit 125
    }
  }
}

$psArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $buildCoreScript) + @($forwardArgs)
$wrapperTimeoutDisplay = if ($computedMax -gt 0) { $computedMax.ToString() + "s" } else { "disabled" }
Write-Host ("[build-game] wrapper hard-timeout: " + $wrapperTimeoutDisplay)

$rc = Invoke-ExternalWithWatchdog -FilePath "powershell" `
                                  -Arguments $psArgs `
                                  -Label "build_core-wrapper" `
                                  -TimeoutSeconds $computedMax `
                                  -HeartbeatSeconds ([Math]::Max(1, $watchdogValue))

if ($rc -ne 0) {
  Write-Host ("[build_game.ps1] failed with exit code " + $rc)
  exit [int]$rc
}

$builtExe = Resolve-FirstExistingPath -Candidates @($exeCandidates)
if ($builtExe) {
  Write-Host ("[ok] Built '" + $builtExe + "'.")
  & $builtExe --version
  exit [int]$LASTEXITCODE
}

if (-not $preferredExe) {
  $preferredExe = Join-Path $repoRoot "out\\build\\ui-runtime-headless\\nebula4x.exe"
}
Write-Warning ("Build completed but executable not found in expected locations; primary expected path: '" + $preferredExe + "'.")
exit 0
