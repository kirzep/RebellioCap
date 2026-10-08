Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$collector = Join-Path $repositoryRoot 'scripts\collect-performance.ps1'
$displayed = Join-Path $repositoryRoot 'tests\data\presentmon-displayed.csv'
$droppedOnly = Join-Path $repositoryRoot 'tests\data\presentmon-dropped-only.csv'
$ffprobe = Join-Path $repositoryRoot 'tests\data\ffprobe-stream-durations.json'

$json = & $collector -AnalyzeCsv $displayed
if ($LASTEXITCODE -ne 0) { throw 'CSV analysis unexpectedly failed.' }
$result = $json | ConvertFrom-Json
if ([Math]::Abs($result.frameTimeP95Ms - 19.6) -gt 0.0001 -or
    [Math]::Abs($result.frameTimeP99Ms - 19.92) -gt 0.0001) {
  throw 'Unexpected frame time percentiles.'
}
if ($result.displayedFrames -ne 3) { throw "Expected 3 displayed frames, got $($result.displayedFrames)." }
if ([Math]::Abs($result.averageFps - 65.217391) -gt 0.0001) {
  throw "Unexpected average FPS: $($result.averageFps)."
}
if ([Math]::Abs($result.onePercentLowFps - 50.200803) -gt 0.0001) {
  throw "Unexpected 1% low: $($result.onePercentLowFps)."
}

& $collector -AnalyzeCsv $droppedOnly 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'A CSV with no displayed frames must fail.' }

$driftJson = & $collector -AnalyzeFfprobe $ffprobe
if ($LASTEXITCODE -ne 0) { throw 'ffprobe duration analysis unexpectedly failed.' }
$drift = $driftJson | ConvertFrom-Json
if ([Math]::Abs($drift.avDriftMs - 25.0) -gt 0.0001) {
  throw "Unexpected A/V drift: $($drift.avDriftMs)."
}

Write-Output 'collect-performance CSV analysis passed'
