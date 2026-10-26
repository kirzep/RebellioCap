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
if ([Math]::Abs($result.medianFps - 62.5) -gt 0.0001 -or
    [Math]::Abs($result.frameTimeP999Ms - 19.992) -gt 0.0001 -or
    [Math]::Abs($result.worstOnePercentMeanFps - 50) -gt 0.0001 -or
    [Math]::Abs($result.worstPointOnePercentMeanFps - 50) -gt 0.0001) {
  throw 'Median, p99.9 and explicitly defined slow-tail FPS must be reported.'
}
if ($result.onePercentLowDefinition -ne '1000 / interpolated p99 frame time (legacy)' -or
    $result.slowTailDefinition -ne '1000 / mean of slowest ceil(N * fraction) frame intervals') {
  throw 'Low-percentile metrics must state their distinct definitions.'
}

$temporaryCsv = Join-Path ([IO.Path]::GetTempPath()) ('presentmon-analysis-' + [guid]::NewGuid() + '.csv')
try {
  @('msBetweenPresents,Dropped', '10,0') + (1..98 | ForEach-Object { '10,0' }) + @('100,0', '200,0') |
    Set-Content -LiteralPath $temporaryCsv
  $tail = (& $collector -AnalyzeCsv $temporaryCsv) | ConvertFrom-Json
  if ($LASTEXITCODE -ne 0 -or $tail.displayedFrames -ne 101 -or
      [Math]::Abs($tail.worstOnePercentMeanFps - (1000.0 / 150)) -gt 0.0001 -or
      [Math]::Abs($tail.worstPointOnePercentMeanFps - 5) -gt 0.0001) {
    throw 'Slow-tail metrics must average the slowest ceil(N * fraction) intervals.'
  }
  foreach ($invalid in @('NaN', 'Infinity', '-1')) {
    @('msBetweenPresents,Dropped', '10,0', "$invalid,0") | Set-Content -LiteralPath $temporaryCsv
    & $collector -AnalyzeCsv $temporaryCsv 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) { throw "Invalid displayed interval $invalid must fail instead of becoming PASS." }
  }
} finally { Remove-Item -LiteralPath $temporaryCsv -ErrorAction SilentlyContinue }

& $collector -AnalyzeCsv $droppedOnly 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) { throw 'A CSV with no displayed frames must fail.' }

$driftJson = & $collector -AnalyzeFfprobe $ffprobe
if ($LASTEXITCODE -ne 0) { throw 'ffprobe duration analysis unexpectedly failed.' }
$drift = $driftJson | ConvertFrom-Json
if ([Math]::Abs($drift.avDriftMs - 25.0) -gt 0.0001) {
  throw "Unexpected A/V drift: $($drift.avDriftMs)."
}

Write-Output 'collect-performance CSV analysis passed'

$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($collector, [ref]$tokens, [ref]$parseErrors)
foreach ($name in @('Convert-ToInvariantDouble', 'Get-MaxWorkingMemoryExcludingReplay', 'Measure-CaptureResources', 'Read-Ndjson', 'Get-CaptureResourceEvidence')) {
  $definition = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
  }, $true)
  if ($null -eq $definition) { throw "Collector lacks $name." }
  . ([scriptblock]::Create($definition.Extent.Text))
}
$samples = @(
  [pscustomobject]@{ qpc = 1000; qpcFrequency = 1000; cpuTotalMs = 0; privateBytes = 100MB; handles = 90; encoderUtilizationPercent = 5 },
  [pscustomobject]@{ qpc = 2000; qpcFrequency = 1000; cpuTotalMs = 10; privateBytes = 110MB; handles = 100; encoderUtilizationPercent = 15 }
)
$metrics = @(
  [pscustomobject]@{ qpc = 1000; fields = [pscustomobject]@{ qpc_frequency = 1000; replay_bytes = 10MB; dropped_frames_percent = 0 } },
  [pscustomobject]@{ qpc = 2000; fields = [pscustomobject]@{ qpc_frequency = 1000; replay_bytes = 20MB; dropped_frames_percent = 0 } }
)
$resource = Measure-CaptureResources $samples $metrics
if ($resource.status -ne 'passed' -or $resource.workingMemoryExcludingReplayMiB -ne 90 -or
    $resource.maximumHandleCount -ne 100 -or $resource.encoderUtilizationAveragePercent -ne 10 -or
    [Math]::Abs($resource.cpuAveragePercent - (1.0 / [Environment]::ProcessorCount)) -gt 0.00001) {
  throw 'Resource evidence must describe its own run, using QPC for CPU wall time.'
}
$metrics[-1].fields.dropped_frames_percent = 1
if ((Measure-CaptureResources $samples $metrics).status -ne 'failed') { throw 'A run exceeding the drop gate must fail independently.' }
foreach ($case in @('missing-samples', 'single-sample', 'missing-metrics', 'nonfinite', 'backward-cpu', 'backward-clock', 'missing-encoder', 'disjoint', 'truncated')) {
  $badSamples = @($samples | ForEach-Object { $_ | Select-Object * })
  $badMetrics = $metrics
  switch ($case) {
    'missing-samples' { $badSamples = @() }
    'single-sample' { $badSamples = @($samples[0]) }
    'missing-metrics' { $badMetrics = @() }
    'nonfinite' { $badSamples[1].cpuTotalMs = 'NaN' }
    'backward-cpu' { $badSamples[1].cpuTotalMs = -1 }
    'backward-clock' { $badSamples[1].qpc = 500 }
    'missing-encoder' { $badSamples | ForEach-Object { $_.encoderUtilizationPercent = $null } }
    'disjoint' { $badSamples | ForEach-Object { $_.qpc += 1800000 } }
  }
  $rejected = $false
  try { Measure-CaptureResources $badSamples $badMetrics $(if ($case -eq 'truncated') { 1800 } else { 0 }) | Out-Null } catch { $rejected = $true }
  if (-not $rejected) { throw "Missing or invalid resource evidence must fail: $case." }
}
Write-Output 'collect-performance per-run resource analysis passed'

$resourceFixture = Join-Path ([IO.Path]::GetTempPath()) ('capture-resource-' + [guid]::NewGuid())
try {
  New-Item -ItemType Directory -Path $resourceFixture | Out-Null
  foreach ($index in 1..3) {
    $prefix = if ($index -eq 3) { 'capture' } else { "capture-$index" }
    $sampleName = if ($index -eq 3) { 'engine-sampler.csv' } else { "$prefix-sampler.csv" }
    $samples | Export-Csv -LiteralPath (Join-Path $resourceFixture $sampleName) -NoTypeInformation
    $metrics[-1].fields.dropped_frames_percent = if ($index -eq 1) { 1 } else { 0 }
    $metrics | ForEach-Object {
      [pscustomobject]@{ type = 'metrics'; qpc = $_.qpc; fields = $_.fields } | ConvertTo-Json -Depth 4 -Compress
    } | Set-Content -LiteralPath (Join-Path $resourceFixture "$prefix-engine.ndjson")
  }
  $runs = @(1..3 | ForEach-Object { [pscustomobject]@{ index = $_ } })
  $evidence = @(Get-CaptureResourceEvidence $resourceFixture $runs 1)
  if ($evidence.Count -ne 3 -or ($evidence.index -join ',') -ne '1,2,3' -or
      $evidence[0].resources.status -ne 'failed' -or $evidence[2].resources.status -ne 'passed') {
    throw 'Passing capture 3 must not mask failed resource evidence from capture 1.'
  }
  Remove-Item -LiteralPath (Join-Path $resourceFixture 'capture-2-sampler.csv')
  $rejected = $false
  try { Get-CaptureResourceEvidence $resourceFixture $runs 1 | Out-Null } catch { $rejected = $true }
  if (-not $rejected) { throw 'A missing capture-2 sampler must not produce passing evidence.' }
} finally {
  $resolved = [IO.Path]::GetFullPath($resourceFixture)
  if ($resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase) -and
      (Split-Path -Leaf $resolved).StartsWith('capture-resource-')) {
    Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
  }
}
Write-Output 'collect-performance independent capture logs passed'
