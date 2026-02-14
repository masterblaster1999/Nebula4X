param(
  [string]$BuildDir = "out/build/ui-vs",
  [string]$Config = "Debug",
  [int]$Jobs = 12,
  [int]$TimeoutSec = 1200,
  [int]$HeartbeatSec = 20,
  [switch]$RetryOnTimeout
)

$ErrorActionPreference = "Stop"

if ($Jobs -lt 1) { $Jobs = 1 }
if ($TimeoutSec -lt 30) { $TimeoutSec = 30 }
if ($HeartbeatSec -lt 5) { $HeartbeatSec = 5 }

$cmakeExe = (Get-Command cmake -ErrorAction Stop).Source
$buildPath = [System.IO.Path]::GetFullPath($BuildDir)
if (-not (Test-Path -LiteralPath $buildPath)) {
  throw "Build directory does not exist: $buildPath"
}

function Invoke-BuildAttempt {
  param(
    [int]$AttemptJobs,
    [int]$AttemptTimeoutSec,
    [int]$AttemptHeartbeatSec
  )

  $args = @(
    "--build", $buildPath,
    "--config", $Config,
    "--target", "imgui_sdl",
    "--parallel", "$AttemptJobs"
  )

  Write-Host "[build] $cmakeExe $($args -join ' ')"
  Write-Host "[build] timeout=${AttemptTimeoutSec}s heartbeat=${AttemptHeartbeatSec}s jobs=${AttemptJobs}"

  # Do not redirect stdout/stderr here. Let the child inherit the console so
  # we avoid pipe-buffer deadlocks under very verbose toolchains.
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $cmakeExe
  $psi.Arguments = ($args -join " ")
  $psi.WorkingDirectory = (Get-Location).Path
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $false
  $psi.RedirectStandardError = $false
  $psi.CreateNoWindow = $true

  $proc = New-Object System.Diagnostics.Process
  $proc.StartInfo = $psi
  $null = $proc.Start()

  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  while (-not $proc.HasExited) {
    $finished = $proc.WaitForExit([Math]::Max(1000, $AttemptHeartbeatSec * 1000))
    if ($finished) { break }

    Write-Host ("[build] still running... elapsed={0:n1}s" -f $sw.Elapsed.TotalSeconds)
    if ($sw.Elapsed.TotalSeconds -ge $AttemptTimeoutSec) {
      try { $proc.Kill($true) } catch {}
      return @{ Success = $false; TimedOut = $true; ExitCode = -1; Elapsed = $sw.Elapsed.TotalSeconds }
    }
  }

  $elapsed = $sw.Elapsed.TotalSeconds
  if ($proc.ExitCode -ne 0) {
    return @{ Success = $false; TimedOut = $false; ExitCode = $proc.ExitCode; Elapsed = $elapsed }
  }
  return @{ Success = $true; TimedOut = $false; ExitCode = 0; Elapsed = $elapsed }
}

$result = Invoke-BuildAttempt -AttemptJobs $Jobs -AttemptTimeoutSec $TimeoutSec -AttemptHeartbeatSec $HeartbeatSec
if (-not $result.Success -and $RetryOnTimeout -and $result.TimedOut) {
  $retryJobs = [Math]::Max(1, [int][Math]::Floor($Jobs / 2))
  if ($retryJobs -eq $Jobs -and $Jobs -gt 1) { $retryJobs = $Jobs - 1 }
  if ($retryJobs -lt $Jobs) {
    Write-Host "[build] timed out; retrying once with reduced parallelism ($retryJobs jobs)."
    $result = Invoke-BuildAttempt -AttemptJobs $retryJobs -AttemptTimeoutSec $TimeoutSec -AttemptHeartbeatSec $HeartbeatSec
  }
}

if (-not $result.Success) {
  if ($result.TimedOut) {
    throw "imgui_sdl build timed out after $([int]$result.Elapsed)s."
  }
  throw "imgui_sdl build failed with exit code $($result.ExitCode)."
}

Write-Host ("[build] imgui_sdl completed in {0:n1}s" -f $result.Elapsed)
