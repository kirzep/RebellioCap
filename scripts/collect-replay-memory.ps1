[CmdletBinding()]
param(
  [Parameter(Mandatory)][ValidatePattern('^\d+:\d+$')][string] $Monitor,
  [ValidateRange(10, 28800)][int] $DurationSeconds = 300,
  [ValidateRange(100000, 500000000)][int] $Bitrate = 200000000,
  [ValidateRange(1, 28800)][int] $BufferSeconds = 30,
  [string] $EnginePath,
  [string] $EvidenceDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (!$EnginePath) { $EnginePath = Join-Path $repositoryRoot 'build/windows-release/RebellioCap.Engine.exe' }
$EnginePath = (Resolve-Path -LiteralPath $EnginePath).Path
if (!$EvidenceDirectory) {
  $EvidenceDirectory = Join-Path $repositoryRoot ('build/replay-memory-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
if (Test-Path -LiteralPath $EvidenceDirectory) { throw 'Evidence directory already exists; choose a fresh path.' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$EvidenceDirectory = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$clipsDirectory = Join-Path $EvidenceDirectory 'clips'
$stdoutPath = Join-Path $EvidenceDirectory 'capture.ndjson'
$stderrPath = Join-Path $EvidenceDirectory 'capture.stderr.log'
$hash = (Get-FileHash -LiteralPath $EnginePath -Algorithm SHA256).Hash
$sourceStatus = @(& git -C $repositoryRoot status --porcelain=v1 --untracked-files=all)
$sourceHead = & git -C $repositoryRoot rev-parse HEAD
$startedUtc = [DateTime]::UtcNow
$arguments = @('capture', '--monitor', $Monitor, '--width', '1920', '--height', '1080',
  '--fps', '60', '--bitrate', "$Bitrate", '--buffer-seconds', "$BufferSeconds",
  '--clip-seconds', "$BufferSeconds", '--duration-seconds', "$DurationSeconds",
  '--output', ('"' + $clipsDirectory + '"'))
[ordered]@{
  startedUtc = $startedUtc.ToString('o'); enginePath = $EnginePath; engineSha256 = $hash
  sourceHead = $sourceHead; sourceStatus = $sourceStatus; arguments = $arguments
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'manifest.json') -Encoding UTF8
$samples = [System.Collections.Generic.List[object]]::new()
$process = Start-Process -FilePath $EnginePath -ArgumentList $arguments -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
# Keep the process handle open so ExitCode remains available after Refresh/HasExited.
$null = $process.Handle
try {
  while (!$process.HasExited) {
    $process.Refresh()
    if (!$process.HasExited) {
      $samples.Add([pscustomobject]@{
        elapsedSeconds = ([DateTime]::UtcNow - $startedUtc).TotalSeconds
        workingSetBytes = $process.WorkingSet64
        privateBytes = $process.PrivateMemorySize64
        cpuSeconds = $process.TotalProcessorTime.TotalSeconds
      })
    }
    if (([DateTime]::UtcNow - $startedUtc).TotalSeconds -gt ($DurationSeconds + 60)) {
      throw 'Capture exceeded duration plus 60-second shutdown allowance.'
    }
    Start-Sleep -Milliseconds 1000
  }
  $process.WaitForExit()
  $exitCode = $process.ExitCode
} finally {
  $process.Refresh()
  if (!$process.HasExited) { Stop-Process -Id $process.Id -Force }
  $samples | Export-Csv -LiteralPath (Join-Path $EvidenceDirectory 'process-memory.csv') -NoTypeInformation
}
$events = @(Get-Content -LiteralPath $stdoutPath | ForEach-Object { $_ | ConvertFrom-Json })
$metricEvents = @($events | Where-Object { $_.type -eq 'metrics' })
$metrics = @($metricEvents | ForEach-Object { $_.fields })
$startEvents = @($events | Where-Object { $_.type -eq 'recording_started' })
$captureSeconds = 0.0
if ($startEvents.Count -eq 1 -and $metricEvents.Count -gt 0) {
  $captureSeconds = ($metricEvents[-1].qpc - $startEvents[0].qpc) / $metrics[-1].qpc_frequency
}
$overBudget = @($metrics | Where-Object { $_.buffered_bytes -gt $_.budget_bytes })
$pipelineErrors = @($metrics | Where-Object { $_.pipeline_errors -ne 0 })
$endingHash = (Get-FileHash -LiteralPath $EnginePath -Algorithm SHA256).Hash
$summary = [ordered]@{
  startedUtc = $startedUtc.ToString('o'); enginePath = $EnginePath; engineSha256 = $hash
  sourceHead = $sourceHead; sourceStatus = $sourceStatus; arguments = $arguments
  exitCode = $exitCode; sampleCount = $samples.Count; metricCount = $metrics.Count
  captureSeconds = $captureSeconds
  peakWorkingSetBytes = ($samples | Measure-Object workingSetBytes -Maximum).Maximum
  peakPrivateBytes = ($samples | Measure-Object privateBytes -Maximum).Maximum
  overBudgetSamples = $overBudget.Count; pipelineErrorSamples = $pipelineErrors.Count
  engineUnchangedDuringRun = ($hash -eq $endingHash)
  limitations = @('Desktop content is uncontrolled; configured bitrate does not establish actual bitrate.',
    'No automated save burst, slow disk, native UI, or gaming performance acceptance.',
    'Packet budget excludes GPU/codec/PCM allocations and is not a process RSS limit.')
}
if ($metrics.Count -gt 0) { $summary.lastMetrics = $metrics[-1] }
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'summary.json') -Encoding UTF8
[pscustomobject]@{
  evidenceDirectory = $EvidenceDirectory; exitCode = $exitCode
  sampleCount = $samples.Count; metricCount = $metrics.Count
  captureSeconds = $captureSeconds
  peakWorkingSetBytes = $summary.peakWorkingSetBytes; peakPrivateBytes = $summary.peakPrivateBytes
  overBudgetSamples = $overBudget.Count; pipelineErrorSamples = $pipelineErrors.Count
} | ConvertTo-Json
if ($null -eq $exitCode -or $exitCode -ne 0 -or $metrics.Count -lt 2 -or $captureSeconds -lt $DurationSeconds - 0.5 -or $overBudget.Count -gt 0 -or $pipelineErrors.Count -gt 0 -or $hash -ne $endingHash) {
  throw "Replay memory run failed; inspect $EvidenceDirectory"
}
