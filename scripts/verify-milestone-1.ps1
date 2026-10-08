[CmdletBinding()]
param(
  [switch] $UsePrimaryMonitor,
  [switch] $CapturePlanOnly,
  [ValidateRange(1, 1440)]
  [int] $DurationMinutes = 30,
  [string] $EvidenceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$usingDefaultEvidenceRoot = [string]::IsNullOrWhiteSpace($EvidenceRoot)
if ($usingDefaultEvidenceRoot) {
  $EvidenceRoot = Join-Path $repositoryRoot 'artifacts\milestone-1'
}
$evidenceRoot = [IO.Path]::GetFullPath($EvidenceRoot)

function Test-IsGeneratedEvidencePath([string] $path) {
  $normalized = $path.Replace('\', '/')
  return $normalized -match '^artifacts/milestone-1/(manifest\.json|doctor\.ndjson|capture-30m\.ndjson|capture-30m\.stderr\.log|ffprobe-(mp4|mkv)\.json|performance\.json|capture-clips/|performance/)'
}

function Assert-CleanSourceTree {
  $dirtyEntries = @(& git -C $repositoryRoot status --porcelain=v1 --untracked-files=all)
  $dirtySource = @($dirtyEntries | Where-Object {
      $path = if ($_.Length -gt 3) { $_.Substring(3) } else { '' }
      -not (Test-IsGeneratedEvidencePath $path)
    })
  if ($dirtySource.Count -gt 0) {
    throw "Evidence capture requires a clean source tree; found: $($dirtySource -join '; ')"
  }
  return (& git -C $repositoryRoot rev-parse HEAD).Trim()
}

function Get-CapturePlan {
  $engine = Join-Path $repositoryRoot 'build\windows-hardware-release\RebellioCap.Engine.exe'
  $ffprobe = Join-Path $repositoryRoot '.tools\vcpkg_installed\x64-windows\tools\ffmpeg\ffprobe.exe'
  if (-not (Test-Path -LiteralPath $engine -PathType Leaf)) {
    throw "Hardware-release engine was not found: $engine"
  }
  if (-not (Test-Path -LiteralPath $ffprobe -PathType Leaf)) {
    throw "Pinned ffprobe was not found: $ffprobe"
  }
  $monitorLines = @(& $engine list-monitors)
  if ($LASTEXITCODE -ne 0) { throw 'RebellioCap.Engine list-monitors failed.' }
  $monitors = @($monitorLines | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
    ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.type -eq 'monitor' })
  $primary = @($monitors | Where-Object { $_.fields.primary -eq $true })
  if ($primary.Count -ne 1) {
    throw "Expected exactly one primary DXGI output, found $($primary.Count)."
  }
  $outputDirectory = Join-Path $evidenceRoot 'capture-clips'
  [PSCustomObject]@{
    engine = $engine
    ffprobe = $ffprobe
    primaryMonitor = [string]$primary[0].fields.id
    outputDirectory = $outputDirectory
    arguments = @(
      'capture', '--monitor', [string]$primary[0].fields.id,
      '--width', '1920', '--height', '1080', '--fps', '60',
      '--bitrate', '30000000', '--buffer-seconds', '30',
      '--clip-seconds', '30', '--container', 'mp4',
      '--duration-seconds', [string]($DurationMinutes * 60),
      '--output', $outputDirectory
    )
    mkvOutput = (Join-Path $outputDirectory 'stream-proof.mkv')
    mkvSmokeArguments = @(
      'smoke', '--seconds', '30', '--container', 'mkv', '--output',
      (Join-Path $outputDirectory 'stream-proof.mkv')
    )
  }
}

if ($CapturePlanOnly) {
  if (-not $UsePrimaryMonitor) { throw '-CapturePlanOnly requires -UsePrimaryMonitor.' }
  Get-CapturePlan | ConvertTo-Json -Depth 5
  exit 0
}

if ($UsePrimaryMonitor) {
  if ($DurationMinutes -ne 30) { throw 'Milestone evidence requires exactly 30 capture minutes.' }
  [void](Assert-CleanSourceTree)
  $plan = Get-CapturePlan
  New-Item -ItemType Directory -Force -Path $evidenceRoot | Out-Null
  New-Item -ItemType Directory -Force -Path $plan.outputDirectory | Out-Null
  $doctorPath = Join-Path $evidenceRoot 'doctor.ndjson'
  $capturePath = Join-Path $evidenceRoot 'capture-30m.ndjson'
  $captureErrorPath = Join-Path $evidenceRoot 'capture-30m.stderr.log'
  $ffprobeMp4Path = Join-Path $evidenceRoot 'ffprobe-mp4.json'
  $ffprobeMkvPath = Join-Path $evidenceRoot 'ffprobe-mkv.json'
  foreach ($path in @($doctorPath, $capturePath, $captureErrorPath,
      $ffprobeMp4Path, $ffprobeMkvPath, $plan.mkvOutput)) {
    if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite existing evidence: $path" }
  }
  if (@(Get-ChildItem -LiteralPath $plan.outputDirectory -File | Where-Object {
        $_.Extension -in @('.mp4', '.mkv')
      }).Count -ne 0) {
    throw "Refusing to mix existing clips with a new evidence run: $($plan.outputDirectory)"
  }
  $doctorOutput = @(& $plan.engine doctor)
  $doctorExit = $LASTEXITCODE
  $doctorOutput | Set-Content -LiteralPath $doctorPath -Encoding utf8NoBOM
  if ($doctorExit -ne 0) {
    throw "RebellioCap.Engine doctor failed with exit $doctorExit; capture was not started."
  }
  Write-Host 'Starting the 30-minute primary-monitor capture. Press F8 twice during the run.'
  & $plan.engine @($plan.arguments) 2> $captureErrorPath |
    Tee-Object -FilePath $capturePath | Out-Host
  $captureExit = $LASTEXITCODE
  if ($captureExit -ne 0) { throw "RebellioCap capture failed with exit $captureExit." }
  $mp4Clips = @(Get-ChildItem -LiteralPath $plan.outputDirectory -Filter '*.mp4' -File |
    Sort-Object LastWriteTimeUtc)
  if ($mp4Clips.Count -ne 2) {
    throw "Expected exactly two MP4 clips from the 30-minute run, found $($mp4Clips.Count)."
  }
  & $plan.ffprobe -v error -show_streams -show_packets -of json $mp4Clips[-1].FullName |
    Set-Content -LiteralPath $ffprobeMp4Path -Encoding utf8NoBOM
  if ($LASTEXITCODE -ne 0) { throw 'ffprobe failed for the late-session MP4 clip.' }
  Write-Host 'Running a 30-second MKV stream-contract smoke capture...'
  & $plan.engine @($plan.mkvSmokeArguments)
  if ($LASTEXITCODE -ne 0) { throw 'RebellioCap MKV smoke capture failed.' }
  & $plan.ffprobe -v error -show_streams -show_packets -of json $plan.mkvOutput |
    Set-Content -LiteralPath $ffprobeMkvPath -Encoding utf8NoBOM
  if ($LASTEXITCODE -ne 0) { throw 'ffprobe failed for the MKV stream-proof clip.' }
}

$requiredFiles = @(
  'manifest.json',
  'doctor.ndjson',
  'capture-30m.ndjson',
  'ffprobe-mp4.json',
  'ffprobe-mkv.json',
  'ctest-unit.xml',
  'ctest-hardware.xml',
  'performance.json',
  'performance/settings-declaration.json',
  'performance/capture-engine.ndjson',
  'performance/engine-sampler.csv',
  'performance/baseline-1.csv',
  'performance/baseline-2.csv',
  'performance/baseline-3.csv',
  'performance/capture-1.csv',
  'performance/capture-2.csv',
  'performance/capture-3.csv',
  'performance/clip-1-ffprobe.json',
  'performance/clip-2-ffprobe.json'
)
$missingFiles = @($requiredFiles | Where-Object {
  -not (Test-Path -LiteralPath (Join-Path $evidenceRoot $_) -PathType Leaf)
})
$failures = [System.Collections.Generic.List[string]]::new()

if ($DurationMinutes -ne 30) {
  $failures.Add('milestone evidence requires exactly 30 capture minutes')
}
if ($missingFiles.Count -gt 0) {
  foreach ($file in $missingFiles) { $failures.Add("missing evidence file: $file") }
}

function Get-RequiredValue([object] $root, [string] $path) {
  $current = $root
  foreach ($part in $path.Split('.')) {
    if ($null -eq $current) {
      $failures.Add("missing manifest field: $path")
      return $null
    }
    $property = $current.PSObject.Properties[$part]
    if ($null -eq $property) {
      $failures.Add("missing manifest field: $path")
      return $null
    }
    $current = $property.Value
  }
  return $current
}

function Require-Passed([object] $manifest, [string] $path) {
  $status = Get-RequiredValue $manifest $path
  if ($status -ne 'passed') { $failures.Add("$path must be passed, found: $status") }
}

function Convert-ToFiniteDouble([object] $value, [string] $name) {
  $number = [double]::Parse([string]$value, [Globalization.CultureInfo]::InvariantCulture)
  if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) {
    throw "$name is not finite."
  }
  return $number
}

function Test-CaptureArtifact([string] $path) {
  try {
    $records = @(Get-Content -LiteralPath $path | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_)
      } | ForEach-Object { $_ | ConvertFrom-Json })
    $started = @($records | Where-Object { $_.type -eq 'recording_started' } | Select-Object -First 1)
    $metrics = @($records | Where-Object {
        if ($_.type -ne 'metrics') { return $false }
        $fieldsProperty = $_.PSObject.Properties['fields']
        return $null -ne $fieldsProperty -and $null -ne $fieldsProperty.Value -and
          $null -ne $fieldsProperty.Value.PSObject.Properties['qpc_frequency']
      })
    if ($started.Count -ne 1 -or $metrics.Count -eq 0) {
      $failures.Add('capture-30m.ndjson has no usable metrics records')
      return
    }
    $last = $metrics[-1]
    $frequency = Convert-ToFiniteDouble $last.fields.qpc_frequency 'capture qpc_frequency'
    if ($frequency -le 0) { throw 'capture qpc_frequency must be positive.' }
    $elapsed = ((Convert-ToFiniteDouble $last.qpc 'capture last qpc') -
      (Convert-ToFiniteDouble $started[0].qpc 'capture start qpc')) / $frequency
    if ($elapsed -lt 1790) { $failures.Add("capture-30m.ndjson covers only $elapsed seconds") }
    foreach ($field in @('dropped_frames_percent', 'hotkey_recognition_ms',
        'completed_saves', 'failed_saves', 'rejected_saves', 'pipeline_errors')) {
      if ($null -eq $last.fields.PSObject.Properties[$field]) {
        throw "capture metrics lack $field."
      }
    }
    if ((Convert-ToFiniteDouble $last.fields.dropped_frames_percent 'capture dropped frames') -gt 0.1) {
      $failures.Add('capture-30m.ndjson dropped frames exceed 0.1 percent')
    }
    if ((Convert-ToFiniteDouble $last.fields.hotkey_recognition_ms 'capture hotkey recognition') -gt 100) {
      $failures.Add('capture-30m.ndjson hotkey recognition exceeds 100 ms')
    }
    if ([int64]$last.fields.completed_saves -lt 2 -or
        [int64]$last.fields.failed_saves -ne 0 -or
        [int64]$last.fields.rejected_saves -ne 0) {
      $failures.Add('capture-30m.ndjson does not prove two successful saves')
    }
    if ([int64]$last.fields.pipeline_errors -ne 0) {
      $failures.Add('capture-30m.ndjson reports pipeline errors')
    }
  } catch {
    $failures.Add("capture-30m.ndjson: $($_.Exception.Message)")
  }
}

function Test-FfprobeArtifact([string] $path, [string] $name) {
  try {
    $document = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    $streams = @($document.streams)
    $video = @($streams | Where-Object { $_.codec_type -eq 'video' -and $_.codec_name -eq 'h264' })
    $audio = @($streams | Where-Object { $_.codec_type -eq 'audio' -and $_.codec_name -eq 'aac' })
    if ($video.Count -ne 1 -or $audio.Count -ne 3) {
      throw "expected one H.264 and three AAC streams, found $($video.Count) and $($audio.Count)."
    }
    $names = @($audio | ForEach-Object {
        if ($null -ne $_.tags.PSObject.Properties['handler_name']) { [string]$_.tags.handler_name }
        elseif ($null -ne $_.tags.PSObject.Properties['title']) { [string]$_.tags.title }
        else { '' }
      } | Sort-Object)
    if (($names -join ',') -ne 'Microphone,Mixed,System') {
      throw 'audio stream names must be Mixed, System, and Microphone.'
    }
    foreach ($stream in $streams) {
      if ((Convert-ToFiniteDouble $stream.duration "$name stream duration") -le 0) {
        throw 'all stream durations must be positive.'
      }
    }
    $packets = @($document.packets)
    if ($packets.Count -eq 0) { throw 'packet evidence is missing; run ffprobe with -show_packets.' }
    $starts = [System.Collections.Generic.List[double]]::new()
    $ends = [System.Collections.Generic.List[double]]::new()
    foreach ($stream in $streams) {
      $streamPackets = @($packets | Where-Object { [int]$_.stream_index -eq [int]$stream.index })
      if ($streamPackets.Count -eq 0) { throw "stream $($stream.index) contains no packets." }
      $lastDts = $null
      $packetStarts = [System.Collections.Generic.List[double]]::new()
      $packetEnds = [System.Collections.Generic.List[double]]::new()
      foreach ($packet in $streamPackets) {
        $dts = Convert-ToFiniteDouble $packet.dts_time "$name packet dts_time"
        if ($null -ne $lastDts -and $dts -le $lastDts) {
          throw "stream $($stream.index) has non-monotonic DTS."
        }
        $lastDts = $dts
        $pts = Convert-ToFiniteDouble $packet.pts_time "$name packet pts_time"
        $duration = Convert-ToFiniteDouble $packet.duration_time "$name packet duration_time"
        if ($duration -le 0) { throw "stream $($stream.index) has a non-positive packet duration." }
        $packetStarts.Add($pts)
        $packetEnds.Add($pts + $duration)
      }
      $starts.Add([double](($packetStarts | Measure-Object -Minimum).Minimum))
      $ends.Add([double](($packetEnds | Measure-Object -Maximum).Maximum))
    }
    $startSpreadMs = ([double](($starts | Measure-Object -Maximum).Maximum) -
      [double](($starts | Measure-Object -Minimum).Minimum)) * 1000.0
    $endSpreadMs = ([double](($ends | Measure-Object -Maximum).Maximum) -
      [double](($ends | Measure-Object -Minimum).Minimum)) * 1000.0
    if ([Math]::Max($startSpreadMs, $endSpreadMs) -gt 50.0) {
      throw "A/V packet-boundary drift exceeds 50 ms (start=$startSpreadMs, end=$endSpreadMs)."
    }
  } catch {
    $failures.Add("${name}: $($_.Exception.Message)")
  }
}

function Test-PerformanceArtifact([string] $path, [object] $manifest) {
  try {
    $performance = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    $required = @('status', 'sourceCommit', 'baselineRuns', 'captureRuns',
      'cpuAveragePercent', 'workingMemoryExcludingReplayMiB',
      'averageFpsRegressionPercent', 'onePercentLowRegressionPercent',
      'droppedFramesPercent', 'avDriftMs', 'hotkeyRecognitionMs', 'saveContinuity',
      'settingsDeclaration', 'runs')
    $missing = @($required | Where-Object { $null -eq $performance.PSObject.Properties[$_] })
    if ($missing.Count -gt 0) {
      $failures.Add('performance.json lacks auditable benchmark fields')
      return
    }
    if ($performance.status -ne 'passed') { $failures.Add('performance.json status is not passed') }
    if ([string]$performance.sourceCommit -ne [string]$manifest.commit.sha) {
      $failures.Add('performance.json source commit does not match manifest')
    }
    if ([int]$performance.baselineRuns -lt 3 -or [int]$performance.captureRuns -lt 3) {
      $failures.Add('performance.json requires three baseline and three capture runs')
    }
    $thresholds = @(
      @('cpuAveragePercent', 2.0),
      @('workingMemoryExcludingReplayMiB', 250.0),
      @('averageFpsRegressionPercent', 3.0),
      @('onePercentLowRegressionPercent', 5.0),
      @('droppedFramesPercent', 0.1),
      @('avDriftMs', 50.0),
      @('hotkeyRecognitionMs', 100.0)
    )
    foreach ($gate in $thresholds) {
      if ((Convert-ToFiniteDouble $performance.($gate[0]) "performance $($gate[0])") -gt [double]$gate[1]) {
        $failures.Add("performance.json $($gate[0]) exceeds $($gate[1])")
      }
    }
    if ($performance.saveContinuity.status -ne 'passed' -or
        (Convert-ToFiniteDouble $performance.saveContinuity.finalizeSeconds 'performance finalizeSeconds') -gt 2 -or
        $performance.saveContinuity.packetsContinuous -ne $true) {
      $failures.Add('performance.json save continuity gate is not passed')
    }
    foreach ($field in @('baselineRuns', 'captureRuns', 'cpuAveragePercent',
        'workingMemoryExcludingReplayMiB', 'averageFpsRegressionPercent',
        'onePercentLowRegressionPercent')) {
      if ([double]$performance.$field -ne [double]$manifest.performance.$field) {
        $failures.Add("performance.json $field does not match manifest")
      }
    }
    Test-PerformanceRawArtifacts (Split-Path -Parent $path) $performance
  } catch {
    $failures.Add("performance.json: $($_.Exception.Message)")
  }
}

function Test-PerformanceRawArtifacts([string] $root, [object] $summary) {
  $performanceRoot = Join-Path $root 'performance'
  try {
    $settings = Get-Content -Raw -LiteralPath (Join-Path $performanceRoot 'settings-declaration.json') | ConvertFrom-Json
    if ([string]::IsNullOrWhiteSpace([string]$settings.declaration) -or
        [string]$settings.declaration -ne [string]$summary.settingsDeclaration) {
      throw 'settings declaration does not match performance.json.'
    }
    $collector = Join-Path $repositoryRoot 'scripts\collect-performance.ps1'
    $recomputedRuns = [System.Collections.Generic.List[object]]::new()
    foreach ($kind in @('baseline', 'capture')) {
      foreach ($index in 1..3) {
        $csv = Join-Path $performanceRoot "$kind-$index.csv"
        $analysisOutput = & $collector -AnalyzeCsv $csv
        if ($LASTEXITCODE -ne 0) { throw "PresentMon analysis failed for $kind-$index.csv." }
        $analysis = $analysisOutput | ConvertFrom-Json
        $recomputedRuns.Add([PSCustomObject]@{ kind=$kind; index=$index; averageFps=[double]$analysis.averageFps; onePercentLowFps=[double]$analysis.onePercentLowFps })
      }
    }
    if (@($summary.runs).Count -ne 6) { throw 'performance.json must contain six raw run summaries.' }
    foreach ($run in $recomputedRuns) {
      $declared = @($summary.runs | Where-Object { $_.kind -eq $run.kind -and [int]$_.index -eq $run.index })
      if ($declared.Count -ne 1 -or
          [Math]::Abs([double]$declared[0].averageFps - $run.averageFps) -gt 0.000001 -or
          [Math]::Abs([double]$declared[0].onePercentLowFps - $run.onePercentLowFps) -gt 0.000001) {
        throw "PresentMon summary mismatch for $($run.kind)-$($run.index)."
      }
    }
    $baselineAverage = @($recomputedRuns | Where-Object kind -eq 'baseline' | ForEach-Object averageFps | Sort-Object)[1]
    $captureAverage = @($recomputedRuns | Where-Object kind -eq 'capture' | ForEach-Object averageFps | Sort-Object)[1]
    $baselineLow = @($recomputedRuns | Where-Object kind -eq 'baseline' | ForEach-Object onePercentLowFps | Sort-Object)[1]
    $captureLow = @($recomputedRuns | Where-Object kind -eq 'capture' | ForEach-Object onePercentLowFps | Sort-Object)[1]
    $averageRegression = ($baselineAverage - $captureAverage) * 100.0 / $baselineAverage
    $lowRegression = ($baselineLow - $captureLow) * 100.0 / $baselineLow
    if ([Math]::Abs([double]$summary.averageFpsRegressionPercent - $averageRegression) -gt 0.000001 -or
        [Math]::Abs([double]$summary.onePercentLowRegressionPercent - $lowRegression) -gt 0.000001) {
      throw 'PresentMon median regressions do not match performance.json.'
    }

    $samples = @(Import-Csv -LiteralPath (Join-Path $performanceRoot 'engine-sampler.csv'))
    if ($samples.Count -lt 2) { throw 'engine sampler requires at least two samples.' }
    $wallSeconds = ([DateTime]::Parse($samples[-1].timestampUtc) - [DateTime]::Parse($samples[0].timestampUtc)).TotalSeconds
    $cpu = ((Convert-ToFiniteDouble $samples[-1].cpuTotalMs 'sampler cpu') -
      (Convert-ToFiniteDouble $samples[0].cpuTotalMs 'sampler cpu')) /
      ($wallSeconds * 1000.0 * [Environment]::ProcessorCount) * 100.0
    $metrics = @(Get-Content -LiteralPath (Join-Path $performanceRoot 'capture-engine.ndjson') |
      Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | ForEach-Object { $_ | ConvertFrom-Json } |
      Where-Object { $_.type -eq 'metrics' })
    if ($metrics.Count -lt 2) { throw 'performance engine log requires multiple metrics records.' }
    $metricIndex = 0
    $maximumWorkingBytes = 0.0
    foreach ($sample in $samples) {
      $targetQpc = Convert-ToFiniteDouble $sample.qpc 'sampler qpc'
      while ($metricIndex + 1 -lt $metrics.Count -and
             [Math]::Abs((Convert-ToFiniteDouble $metrics[$metricIndex + 1].qpc 'metrics qpc') - $targetQpc) -le
             [Math]::Abs((Convert-ToFiniteDouble $metrics[$metricIndex].qpc 'metrics qpc') - $targetQpc)) { ++$metricIndex }
      $working = (Convert-ToFiniteDouble $sample.privateBytes 'sampler privateBytes') -
        (Convert-ToFiniteDouble $metrics[$metricIndex].fields.replay_bytes 'metrics replay_bytes')
      $maximumWorkingBytes = [Math]::Max($maximumWorkingBytes, [Math]::Max(0.0, $working))
    }
    $workingMiB = $maximumWorkingBytes / 1MB
    if ([Math]::Abs([double]$summary.cpuAveragePercent - $cpu) -gt 0.000001 -or
        [Math]::Abs([double]$summary.workingMemoryExcludingReplayMiB - $workingMiB) -gt 0.000001) {
      throw 'sampler CPU or pointwise memory does not match performance.json.'
    }
    $last = $metrics[-1].fields
    if ([double]$last.dropped_frames_percent -ne [double]$summary.droppedFramesPercent -or
        [double]$last.hotkey_recognition_ms -ne [double]$summary.hotkeyRecognitionMs) {
      throw 'engine dropped-frame or hotkey metrics do not match performance.json.'
    }
    $maximumFinalize = 0.0
    $continuous = $true
    foreach ($save in 1..2) {
      $requestIndex = -1
      $completeIndex = -1
      for ($index = 0; $index -lt $metrics.Count; ++$index) {
        if ($requestIndex -lt 0 -and [int64]$metrics[$index].fields.save_requests -ge $save) { $requestIndex = $index }
        if ($completeIndex -lt 0 -and [int64]$metrics[$index].fields.completed_saves -ge $save) { $completeIndex = $index }
      }
      if ($requestIndex -lt 1 -or $completeIndex -lt $requestIndex -or $completeIndex + 1 -ge $metrics.Count) {
        throw "engine metrics do not bracket save $save."
      }
      $frequency = Convert-ToFiniteDouble $metrics[$completeIndex].fields.qpc_frequency 'metrics qpc_frequency'
      $finalize = ((Convert-ToFiniteDouble $metrics[$completeIndex].qpc 'metrics qpc') -
        (Convert-ToFiniteDouble $metrics[$requestIndex].qpc 'metrics qpc')) / $frequency + 1.0
      $maximumFinalize = [Math]::Max($maximumFinalize, $finalize)
      $packetTotal = { param($record) [double]$record.fields.video_packets + [double]$record.fields.audio_packets }
      $before = & $packetTotal $metrics[$requestIndex - 1]
      $during = & $packetTotal $metrics[$completeIndex]
      $after = & $packetTotal $metrics[$completeIndex + 1]
      $continuous = $continuous -and $during -gt $before -and $after -gt $during
    }
    if ([Math]::Abs([double]$summary.saveContinuity.finalizeSeconds - $maximumFinalize) -gt 0.000001 -or
        [bool]$summary.saveContinuity.packetsContinuous -ne $continuous) {
      throw 'save-continuity metrics do not match performance.json.'
    }
    $drifts = @('clip-1-ffprobe.json', 'clip-2-ffprobe.json' | ForEach-Object {
        $output = & $collector -AnalyzeFfprobe (Join-Path $performanceRoot $_)
        if ($LASTEXITCODE -ne 0) { throw "A/V analysis failed for $_." }
        [double](($output | ConvertFrom-Json).avDriftMs)
      })
    $maximumDrift = [double](($drifts | Measure-Object -Maximum).Maximum)
    if ([Math]::Abs([double]$summary.avDriftMs - $maximumDrift) -gt 0.000001) {
      throw 'packet-boundary A/V drift does not match performance.json.'
    }
  } catch {
    $failures.Add("performance raw artifacts: $($_.Exception.Message)")
  }
}

function Test-CtestArtifact([string] $path, [string] $suiteName,
                            [object] $manifest) {
  try {
    [xml]$document = Get-Content -Raw -LiteralPath $path
    $suite = $document.testsuite
    if ($null -eq $suite) { throw 'testsuite element is missing.' }
    $total = [int]$suite.GetAttribute('tests')
    $failed = [int]$suite.GetAttribute('failures') + [int]$suite.GetAttribute('errors')
    $skipped = [int]$suite.GetAttribute('skipped') + [int]$suite.GetAttribute('disabled')
    $passed = $total - $failed - $skipped
    $declared = $manifest.ctest.$suiteName
    if ($total -ne [int]$declared.total -or $passed -ne [int]$declared.passed -or
        $failed -ne [int]$declared.failed -or $skipped -ne [int]$declared.skipped) {
      $failures.Add("$suiteName CTest JUnit counts do not match manifest")
    }
    if ($failed -ne 0 -or $skipped -ne 0 -or $total -lt 1) {
      $failures.Add("$suiteName CTest JUnit is not an all-passed run")
    }
    if ($suiteName -eq 'hardware') {
      $hotkey = @($suite.testcase | Where-Object { $_.name -eq 'hotkey_passthrough' })
      if ($hotkey.Count -ne 1 -or
          $null -ne $hotkey[0].PSObject.Properties['failure'] -or
          $null -ne $hotkey[0].PSObject.Properties['skipped'] -or
          [string]$hotkey[0].status -ne 'run') {
        $failures.Add('hardware CTest JUnit has no passing hotkey_passthrough proof')
      }
    }
  } catch {
    $failures.Add("ctest-$suiteName.xml: $($_.Exception.Message)")
  }
}

if ($missingFiles.Count -eq 0) {
  Test-CaptureArtifact (Join-Path $evidenceRoot 'capture-30m.ndjson')
  Test-FfprobeArtifact (Join-Path $evidenceRoot 'ffprobe-mp4.json') 'ffprobe-mp4.json'
  Test-FfprobeArtifact (Join-Path $evidenceRoot 'ffprobe-mkv.json') 'ffprobe-mkv.json'
  try {
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $evidenceRoot 'manifest.json') |
      ConvertFrom-Json
  } catch {
    $failures.Add("manifest.json is not valid JSON: $($_.Exception.Message)")
    $manifest = $null
  }

  if ($null -ne $manifest) {
    Test-PerformanceArtifact (Join-Path $evidenceRoot 'performance.json') $manifest
    Test-CtestArtifact (Join-Path $evidenceRoot 'ctest-unit.xml') 'unit' $manifest
    Test-CtestArtifact (Join-Path $evidenceRoot 'ctest-hardware.xml') 'hardware' $manifest
    if ((Get-RequiredValue $manifest 'schemaVersion') -ne 1) {
      $failures.Add('schemaVersion must be 1')
    }
    $sha = Get-RequiredValue $manifest 'commit.sha'
    & git -C $repositoryRoot cat-file -e "$sha`^{commit}" 2>$null
    if ($LASTEXITCODE -ne 0) {
      $failures.Add("manifest source commit does not exist: $sha")
    } else {
      & git -C $repositoryRoot merge-base --is-ancestor $sha HEAD 2>$null
      if ($LASTEXITCODE -ne 0) {
        $failures.Add("manifest source commit $sha is not an ancestor of HEAD")
      } else {
        $changedAfterSource = @(& git -C $repositoryRoot diff --name-only $sha HEAD)
        $sourceChanges = @($changedAfterSource | Where-Object { -not (Test-IsGeneratedEvidencePath $_) })
        if ($sourceChanges.Count -gt 0) {
          $failures.Add("source files changed after evidence commit: $($sourceChanges -join ', ')")
        }
      }
    }
    if ((Get-RequiredValue $manifest 'commit.dirty') -ne $false) {
      $failures.Add('capture-time dirty state must be false')
    }
    if ($usingDefaultEvidenceRoot) {
      $dirtyEntries = @(& git -C $repositoryRoot status --porcelain=v1 --untracked-files=all)
      $dirtySource = @($dirtyEntries | Where-Object {
          $path = if ($_.Length -gt 3) { $_.Substring(3) } else { '' }
          -not (Test-IsGeneratedEvidencePath $path)
        })
      if ($dirtySource.Count -gt 0) {
        $failures.Add('current source tree contains changes outside generated milestone evidence')
      }
    }
    foreach ($path in @('host.osBuild', 'host.gpu', 'host.driver', 'tools.cmake',
        'tools.ninja', 'tools.msvc', 'tools.ffprobe', 'tools.presentMon')) {
      $value = Get-RequiredValue $manifest $path
      if ([string]::IsNullOrWhiteSpace([string]$value)) { $failures.Add("$path must not be empty") }
    }
    foreach ($path in @('ctest.unit.status', 'ctest.hardware.status', 'streams.mp4.status',
        'streams.mkv.status', 'capture30m.status', 'hotkey.status',
        'saveContinuity.status', 'performance.status')) {
      Require-Passed $manifest $path
    }
    foreach ($suite in @('unit', 'hardware')) {
      $failed = Get-RequiredValue $manifest "ctest.$suite.failed"
      $skipped = Get-RequiredValue $manifest "ctest.$suite.skipped"
      $total = Get-RequiredValue $manifest "ctest.$suite.total"
      $passed = Get-RequiredValue $manifest "ctest.$suite.passed"
      if ($failed -ne 0 -or $skipped -ne 0 -or $passed -ne $total -or $total -lt 1) {
        $failures.Add("ctest.$suite must have all tests passed and none skipped")
      }
    }
    foreach ($container in @('mp4', 'mkv')) {
      if ((Get-RequiredValue $manifest "streams.$container.videoCodec") -ne 'h264' -or
          (Get-RequiredValue $manifest "streams.$container.videoCount") -ne 1 -or
          (Get-RequiredValue $manifest "streams.$container.audioCodec") -ne 'aac' -or
          (Get-RequiredValue $manifest "streams.$container.audioCount") -ne 3 -or
          (Get-RequiredValue $manifest "streams.$container.monotonicTimestamps") -ne $true) {
        $failures.Add("streams.$container does not contain one H.264 and three AAC streams with monotonic timestamps")
      }
      $names = @(Get-RequiredValue $manifest "streams.$container.audioNames")
      if (@($names | Sort-Object) -join ',' -ne 'Microphone,Mixed,System') {
        $failures.Add("streams.$container audio names must be Mixed, System, and Microphone")
      }
    }
    if ((Get-RequiredValue $manifest 'capture30m.durationMinutes') -lt 30) { $failures.Add('capture duration is below 30 minutes') }
    if ((Get-RequiredValue $manifest 'capture30m.avDriftMs') -gt 50) { $failures.Add('A/V drift exceeds 50 ms') }
    if ((Get-RequiredValue $manifest 'capture30m.droppedFramesPercent') -gt 0.1) { $failures.Add('dropped frames exceed 0.1 percent') }
    if ((Get-RequiredValue $manifest 'hotkey.recognitionMs') -gt 100) { $failures.Add('hotkey recognition exceeds 100 ms') }
    if ((Get-RequiredValue $manifest 'hotkey.inputPassThrough') -ne $true) { $failures.Add('input pass-through proof is missing') }
    if ((Get-RequiredValue $manifest 'saveContinuity.finalizeSeconds') -gt 2) { $failures.Add('clip finalize exceeds 2 seconds') }
    if ((Get-RequiredValue $manifest 'saveContinuity.packetsIncreasedBeforeDuringAfter') -ne $true) { $failures.Add('save continuity proof is missing') }
    if ((Get-RequiredValue $manifest 'performance.cpuAveragePercent') -gt 2) { $failures.Add('engine CPU exceeds 2 percent') }
    if ((Get-RequiredValue $manifest 'performance.workingMemoryExcludingReplayMiB') -gt 250) { $failures.Add('working memory excluding replay exceeds 250 MiB') }
    if ((Get-RequiredValue $manifest 'performance.averageFpsRegressionPercent') -gt 3) { $failures.Add('average FPS regression exceeds 3 percent') }
    if ((Get-RequiredValue $manifest 'performance.onePercentLowRegressionPercent') -gt 5) { $failures.Add('1 percent low regression exceeds 5 percent') }
    if ((Get-RequiredValue $manifest 'performance.baselineRuns') -lt 3 -or
        (Get-RequiredValue $manifest 'performance.captureRuns') -lt 3) {
      $failures.Add('performance evidence requires three baseline and three capture runs')
    }

    $artifactEntries = @(Get-RequiredValue $manifest 'artifacts')
    foreach ($required in $requiredFiles | Where-Object { $_ -ne 'manifest.json' }) {
      $entry = @($artifactEntries | Where-Object { $_.path -eq "artifacts/milestone-1/$required" })
      if ($entry.Count -ne 1) {
        $failures.Add("manifest must contain one hash entry for $required")
        continue
      }
      $absolute = Join-Path $evidenceRoot $required
      $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $absolute).Hash.ToLowerInvariant()
      if ($actual -ne ([string]$entry[0].sha256).ToLowerInvariant()) {
        $failures.Add("artifact hash mismatch: $($entry[0].path)")
      }
    }

    try {
      $doctorRecords = @(Get-Content -LiteralPath (Join-Path $evidenceRoot 'doctor.ndjson') |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | ForEach-Object { $_ | ConvertFrom-Json })
      $doctor = @($doctorRecords | Where-Object { $_.type -eq 'doctor' } | Select-Object -Last 1)
      if ($doctor.Count -ne 1 -or $doctor[0].fields.passed -ne $true) {
        $failures.Add('doctor.ndjson has no passing doctor record')
      }
    } catch { $failures.Add("doctor.ndjson is invalid: $($_.Exception.Message)") }

  }
}

$passed = $failures.Count -eq 0
[PSCustomObject]@{
  passed = $passed
  missingFiles = $missingFiles
  failures = @($failures)
} | ConvertTo-Json -Depth 5

if (-not $passed) { exit 1 }
