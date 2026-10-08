[CmdletBinding(DefaultParameterSetName = 'Collect')]
param(
  [Parameter(Mandatory, ParameterSetName = 'Analyze')]
  [ValidateNotNullOrEmpty()]
  [string] $AnalyzeCsv,
  [Parameter(Mandatory, ParameterSetName = 'AnalyzeFfprobe')]
  [ValidateNotNullOrEmpty()]
  [string] $AnalyzeFfprobe,
  [Parameter(ParameterSetName = 'Collect')]
  [switch] $PreflightOnly,
  [Parameter(ParameterSetName = 'Collect')]
  [switch] $PlanOnly,
  [Parameter(ParameterSetName = 'Collect')]
  [string] $SettingsDeclaration,
  [Parameter(ParameterSetName = 'Collect')]
  [ValidateNotNullOrEmpty()]
  [string] $GameExecutable = 'D:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe',
  [Parameter(ParameterSetName = 'Collect')]
  [ValidateNotNullOrEmpty()]
  [string] $SceneDeclaration = 'built-in benchmark',
  [Parameter(ParameterSetName = 'Collect')]
  [ValidateRange(1, 60)]
  [int] $GameWaitMinutes = 10
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot

. (Join-Path $PSScriptRoot 'performance-build-identity.ps1')

function Convert-ToInvariantDouble([object] $value) {
  return [double]::Parse([string]$value, [Globalization.CultureInfo]::InvariantCulture)
}

function Get-Percentile([double[]] $values, [double] $percentile) {
  if ($values.Count -eq 0) { throw 'Cannot calculate a percentile for an empty set.' }
  $sorted = @($values | Sort-Object)
  $rank = ($sorted.Count - 1) * $percentile
  $lower = [Math]::Floor($rank)
  $upper = [Math]::Ceiling($rank)
  if ($lower -eq $upper) { return [double]$sorted[$lower] }
  $weight = $rank - $lower
  return [double]$sorted[$lower] + ([double]$sorted[$upper] - [double]$sorted[$lower]) * $weight
}

function Measure-PresentMonCsv([string] $path) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "PresentMon CSV was not found: $path"
  }
  $rows = @(Import-Csv -LiteralPath $path)
  $displayed = [System.Collections.Generic.List[double]]::new()
  foreach ($row in $rows) {
    $droppedProperty = $row.PSObject.Properties['Dropped']
    $dropped = $false
    if ($null -ne $droppedProperty) {
      $text = ([string]$droppedProperty.Value).Trim().ToLowerInvariant()
      $dropped = $text -in @('1', 'true', 'yes')
    }
    if ($dropped) { continue }
    $intervalProperty = $row.PSObject.Properties['msBetweenPresents']
    if ($null -eq $intervalProperty) { throw 'PresentMon CSV lacks msBetweenPresents.' }
    $interval = Convert-ToInvariantDouble $intervalProperty.Value
    if ($interval -gt 0) { $displayed.Add($interval) }
  }
  if ($displayed.Count -eq 0) { throw 'PresentMon CSV contains no displayed frames.' }
  $mean = ($displayed | Measure-Object -Average).Average
  $percentile99 = Get-Percentile $displayed.ToArray() 0.99
  [PSCustomObject]@{
    status = 'passed'
    displayedFrames = $displayed.Count
    averageFps = 1000.0 / $mean
    onePercentLowFps = 1000.0 / $percentile99
    frameTimeP95Ms = Get-Percentile $displayed.ToArray() 0.95
    frameTimeP99Ms = $percentile99
  }
}

function Measure-FfprobeDrift([string] $path) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "ffprobe JSON was not found: $path"
  }
  $document = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
  $streams = @($document.streams)
  $video = @($streams | Where-Object { $_.codec_type -eq 'video' })
  $audio = @($streams | Where-Object { $_.codec_type -eq 'audio' })
  if ($video.Count -ne 1 -or $audio.Count -ne 3) {
    throw "Expected one video and three audio streams, found $($video.Count) and $($audio.Count)."
  }
  $packets = @($document.packets)
  if ($packets.Count -eq 0) { throw 'ffprobe packet evidence is required.' }
  $starts = [System.Collections.Generic.List[double]]::new()
  $ends = [System.Collections.Generic.List[double]]::new()
  foreach ($stream in $streams) {
    $streamPackets = @($packets | Where-Object { [int]$_.stream_index -eq [int]$stream.index } |
      Sort-Object { Convert-ToInvariantDouble $_.pts_time })
    if ($streamPackets.Count -eq 0) { throw "Stream $($stream.index) contains no packets." }
    $first = Convert-ToInvariantDouble $streamPackets[0].pts_time
    $last = $streamPackets[-1]
    $lastDuration = Convert-ToInvariantDouble $last.duration_time
    $starts.Add($first)
    $ends.Add((Convert-ToInvariantDouble $last.pts_time) + $lastDuration)
  }
  $startSpread = [double](($starts | Measure-Object -Maximum).Maximum) -
    [double](($starts | Measure-Object -Minimum).Minimum)
  $endSpread = [double](($ends | Measure-Object -Maximum).Maximum) -
    [double](($ends | Measure-Object -Minimum).Minimum)
  [PSCustomObject]@{
    status = 'passed'
    avDriftMs = [Math]::Max($startSpread, $endSpread) * 1000.0
    startSpreadMs = $startSpread * 1000.0
    endSpreadMs = $endSpread * 1000.0
  }
}

if ($PSCmdlet.ParameterSetName -eq 'Analyze') {
  try {
    Measure-PresentMonCsv $AnalyzeCsv | ConvertTo-Json -Compress
    exit 0
  } catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
  }
}

if ($PSCmdlet.ParameterSetName -eq 'AnalyzeFfprobe') {
  try {
    Measure-FfprobeDrift $AnalyzeFfprobe | ConvertTo-Json -Compress
    exit 0
  } catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
  }
}

function Get-PerformancePreflight {
  $game = [IO.Path]::GetFullPath($GameExecutable)
  $presentMon = Join-Path $repositoryRoot '.tools\presentmon\PresentMon.exe'
  $engine = Join-Path $repositoryRoot 'build\windows-hardware-release\RebellioCap.Engine.exe'
  $ffprobe = Join-Path $repositoryRoot '.tools\vcpkg_installed\x64-windows\tools\ffmpeg\ffprobe.exe'
  foreach ($required in @($game, $presentMon, $engine, $ffprobe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
      throw "Required performance executable was not found: $required"
    }
  }
  $versionLine = ([string](& $presentMon --help 2>&1 | Select-Object -First 1)).Trim()
  if ($versionLine -notmatch '^PresentMon\s+([0-9]+\.[0-9]+\.[0-9]+)$') {
    throw "Unable to verify PresentMon version: $versionLine"
  }
  $presentMonVersion = $matches[1]
  if ($presentMonVersion -ne '2.5.1') {
    throw "PresentMon 2.5.1 is required, found $presentMonVersion."
  }
  $monitorLines = @(& $engine list-monitors)
  if ($LASTEXITCODE -ne 0) { throw 'RebellioCap.Engine list-monitors failed.' }
  $monitors = @($monitorLines | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
    ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.type -eq 'monitor' })
  $primary = @($monitors | Where-Object { $_.fields.primary -eq $true })
  if ($primary.Count -ne 1) {
    throw "Expected exactly one primary DXGI output, found $($primary.Count)."
  }
  [PSCustomObject]@{
    status = 'passed'
    gameExecutable = $game
    presentMon = $presentMon
    presentMonVersion = $presentMonVersion
    engine = $engine
    ffprobe = $ffprobe
    primaryMonitor = [string]$primary[0].fields.id
  }
}

function Get-CollectionPlan([object] $preflight, [string] $settings) {
  if ([string]::IsNullOrWhiteSpace($settings)) {
    throw 'A settings declaration is required for an auditable benchmark plan.'
  }
  $performanceRoot = Join-Path $repositoryRoot 'artifacts\milestone-1\performance'
  $runs = [System.Collections.Generic.List[object]]::new()
  foreach ($index in 1..3) {
    foreach ($kind in @('baseline', 'capture')) {
      $csv = Join-Path $performanceRoot "$kind-$index.csv"
      $runs.Add([PSCustomObject]@{
        kind = $kind
        index = $index
        csv = $csv
        presentMonArguments = @(
          '--process_name', [IO.Path]::GetFileName($preflight.gameExecutable),
          '--v1_metrics',
          '--output_file', $csv,
          '--no_console_stats',
          '--session_name', 'RebellioCapBenchmark'
        )
      })
    }
  }
  [PSCustomObject]@{
    schemaVersion = 2
    buildIdentity = Get-PerformanceBuildIdentity $repositoryRoot $preflight.engine
    settingsDeclaration = $settings
    sceneDeclaration = $SceneDeclaration
    sessionName = 'RebellioCapBenchmark'
    secondSaveAfterSeconds = 1770
    performanceRoot = $performanceRoot
    gameExecutable = $preflight.gameExecutable
    presentMon = $preflight.presentMon
    engine = $preflight.engine
    ffprobe = $preflight.ffprobe
    primaryMonitor = $preflight.primaryMonitor
    captureArguments = @(
      'capture', '--monitor', $preflight.primaryMonitor,
      '--width', '1920', '--height', '1080', '--fps', '60',
      '--bitrate', '30000000', '--buffer-seconds', '30',
      '--clip-seconds', '30', '--container', 'mp4',
      '--duration-seconds', '1800', '--output',
      (Join-Path $performanceRoot 'clips')
    )
    runs = @($runs)
  }
}

if ($PreflightOnly) {
  try {
    Get-PerformancePreflight | ConvertTo-Json -Compress
    exit 0
  } catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
  }
}

if ($PlanOnly) {
  try {
    $preflight = Get-PerformancePreflight
    Get-CollectionPlan $preflight $SettingsDeclaration | ConvertTo-Json -Depth 6
    exit 0
  } catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
  }
}

function Wait-ForGame([string] $executable, [int] $minutes) {
  if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Game executable disappeared: $executable"
  }
  $deadline = [DateTime]::UtcNow.AddMinutes($minutes)
  while ([DateTime]::UtcNow -lt $deadline) {
    $process = Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($executable)) -ErrorAction SilentlyContinue |
      Where-Object { $_.Path -eq $executable } | Select-Object -First 1
    if ($process) { return $process }
    Start-Sleep -Seconds 2
  }
  throw "$executable did not start within $minutes minutes."
}

function Start-ArgumentProcess([string] $file, [string[]] $arguments) {
  $start = [Diagnostics.ProcessStartInfo]::new()
  $start.FileName = $file
  $start.UseShellExecute = $false
  foreach ($argument in $arguments) { [void]$start.ArgumentList.Add($argument) }
  $process = [Diagnostics.Process]::new()
  $process.StartInfo = $start
  if (-not $process.Start()) { throw "Failed to start $file." }
  return $process
}

function Start-RedirectedArgumentProcess([string] $file, [string[]] $arguments) {
  $start = [Diagnostics.ProcessStartInfo]::new()
  $start.FileName = $file
  $start.UseShellExecute = $false
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  foreach ($argument in $arguments) { [void]$start.ArgumentList.Add($argument) }
  $process = [Diagnostics.Process]::new()
  $process.StartInfo = $start
  if (-not $process.Start()) { throw "Failed to start $file." }
  [PSCustomObject]@{
    process = $process
    outputTask = $process.StandardOutput.ReadToEndAsync()
    errorTask = $process.StandardError.ReadToEndAsync()
  }
}

function Complete-RedirectedProcess([object] $running, [string] $outputPath,
                                    [string] $errorPath) {
  $running.outputTask.GetAwaiter().GetResult() |
    Set-Content -LiteralPath $outputPath -Encoding utf8NoBOM
  $running.errorTask.GetAwaiter().GetResult() |
    Set-Content -LiteralPath $errorPath -Encoding utf8NoBOM
}

function Invoke-PresentMonRun([object] $plan, [object] $run) {
  if (Test-Path -LiteralPath $run.csv) {
    throw "Refusing to overwrite existing benchmark evidence: $($run.csv)"
  }
  [void](Wait-ForGame $plan.gameExecutable $GameWaitMinutes)
  [void](Read-Host "Prepare $($run.kind) run $($run.index) with the declared settings and scene: $($plan.sceneDeclaration). Press Enter immediately before starting")
  $presentMonProcess = Start-ArgumentProcess $plan.presentMon @($run.presentMonArguments)
  try {
    [void](Read-Host 'Press Enter when the benchmark result screen is visible')
  } finally {
    $terminationFailure = $null
    try {
      $terminator = Start-ArgumentProcess $plan.presentMon @(
        '--terminate_existing_session', '--session_name', $plan.sessionName)
      if (-not $terminator.WaitForExit(15000) -or $terminator.ExitCode -ne 0) {
        if (-not $terminator.HasExited) { Stop-Process -Id $terminator.Id -Force }
        $terminationFailure = "PresentMon session terminator failed for $($run.kind) run $($run.index)."
      }
    } catch {
      $terminationFailure = "PresentMon session terminator failed: $($_.Exception.Message)"
    }
    if (-not $presentMonProcess.WaitForExit(15000)) {
      Stop-Process -Id $presentMonProcess.Id -Force
      $presentMonProcess.WaitForExit()
      $terminationFailure = "PresentMon did not exit for $($run.kind) run $($run.index)."
    }
    if ($null -ne $terminationFailure) { throw $terminationFailure }
  }
  if ($presentMonProcess.ExitCode -ne 0) {
    throw "PresentMon failed for $($run.kind) run $($run.index) with exit $($presentMonProcess.ExitCode)."
  }
  $measurement = Measure-PresentMonCsv $run.csv
  [PSCustomObject]@{
    kind = $run.kind
    index = $run.index
    csv = $run.csv
    displayedFrames = $measurement.displayedFrames
    averageFps = $measurement.averageFps
    onePercentLowFps = $measurement.onePercentLowFps
    frameTimeP95Ms = $measurement.frameTimeP95Ms
    frameTimeP99Ms = $measurement.frameTimeP99Ms
  }
}

function Get-Median([double[]] $values) {
  if ($values.Count -eq 0) { throw 'Cannot calculate the median of an empty set.' }
  $sorted = @($values | Sort-Object)
  $middle = [Math]::Floor($sorted.Count / 2)
  if ($sorted.Count % 2 -eq 1) { return [double]$sorted[$middle] }
  return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Start-EngineSampler([int] $processId, [string] $nvidiaSmi) {
  Start-Job -ScriptBlock {
    param($TargetProcessId, $NvidiaSmiPath)
    while ($true) {
      $process = Get-Process -Id $TargetProcessId -ErrorAction SilentlyContinue
      if (-not $process) { break }
      $encoder = $null
      try {
        $encoder = [double]((& $NvidiaSmiPath '--query-gpu=utilization.encoder' '--format=csv,noheader,nounits' 2>$null |
          Select-Object -First 1).Trim())
      } catch {}
      [PSCustomObject]@{
        timestampUtc = [DateTime]::UtcNow.ToString('O')
        qpc = [Diagnostics.Stopwatch]::GetTimestamp()
        qpcFrequency = [Diagnostics.Stopwatch]::Frequency
        cpuTotalMs = $process.TotalProcessorTime.TotalMilliseconds
        privateBytes = $process.PrivateMemorySize64
        handles = $process.HandleCount
        encoderUtilizationPercent = $encoder
      }
      Start-Sleep -Seconds 1
    }
  } -ArgumentList $processId, $nvidiaSmi
}

function Wait-ForCaptureMoment([Diagnostics.Process] $process,
                               [Diagnostics.Stopwatch] $timer,
                               [int] $elapsedSeconds) {
  while ($timer.Elapsed.TotalSeconds -lt $elapsedSeconds) {
    if ($process.HasExited) {
      throw "RebellioCap exited before the scheduled late-session save with exit $($process.ExitCode)."
    }
    $remaining = $elapsedSeconds - [int][Math]::Floor($timer.Elapsed.TotalSeconds)
    Start-Sleep -Seconds ([Math]::Min(30, [Math]::Max(1, $remaining)))
  }
}

function Read-Ndjson([string] $path) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return @() }
  return @(Get-Content -LiteralPath $path | Where-Object {
      -not [string]::IsNullOrWhiteSpace($_)
    } | ForEach-Object { $_ | ConvertFrom-Json })
}

function Get-SaveEvidence([object[]] $metrics) {
  $maximumFinalize = 0.0
  $continuous = $true
  foreach ($save in 1..2) {
    $requestIndex = -1
    $completeIndex = -1
    for ($index = 0; $index -lt $metrics.Count; ++$index) {
      if ($requestIndex -lt 0 -and $metrics[$index].fields.save_requests -ge $save) {
        $requestIndex = $index
      }
      if ($completeIndex -lt 0 -and $metrics[$index].fields.completed_saves -ge $save) {
        $completeIndex = $index
      }
    }
    if ($requestIndex -lt 1 -or $completeIndex -lt $requestIndex -or
        $completeIndex + 1 -ge $metrics.Count) {
      return [PSCustomObject]@{ status = 'failed'; finalizeSeconds = $null; packetsContinuous = $false }
    }
    $frequency = [double]$metrics[$completeIndex].fields.qpc_frequency
    $seconds = ([double]$metrics[$completeIndex].qpc - [double]$metrics[$requestIndex].qpc) / $frequency + 1.0
    $maximumFinalize = [Math]::Max($maximumFinalize, $seconds)
    $before = [double]$metrics[$requestIndex - 1].fields.video_packets + [double]$metrics[$requestIndex - 1].fields.audio_packets
    $during = [double]$metrics[$completeIndex].fields.video_packets + [double]$metrics[$completeIndex].fields.audio_packets
    $after = [double]$metrics[$completeIndex + 1].fields.video_packets + [double]$metrics[$completeIndex + 1].fields.audio_packets
    $continuous = $continuous -and $during -gt $before -and $after -gt $during
  }
  [PSCustomObject]@{
    status = if ($maximumFinalize -le 2.0 -and $continuous) { 'passed' } else { 'failed' }
    finalizeSeconds = $maximumFinalize
    packetsContinuous = $continuous
  }
}

function Get-MaxWorkingMemoryExcludingReplay([object[]] $samples,
                                             [object[]] $metrics) {
  if ($samples.Count -eq 0 -or $metrics.Count -eq 0) {
    throw 'Memory evidence requires both process samples and engine metrics.'
  }
  $frequency = Convert-ToInvariantDouble $metrics[0].fields.qpc_frequency
  if ($frequency -le 0) { throw 'Engine metrics contain an invalid QPC frequency.' }
  $metricIndex = 0
  $maximum = 0.0
  foreach ($sample in $samples) {
    $sampleFrequency = Convert-ToInvariantDouble $sample.qpcFrequency
    if ($sampleFrequency -ne $frequency) {
      throw "Sampler QPC frequency $sampleFrequency does not match engine frequency $frequency."
    }
    $targetQpc = Convert-ToInvariantDouble $sample.qpc
    while ($metricIndex + 1 -lt $metrics.Count -and
           [Math]::Abs((Convert-ToInvariantDouble $metrics[$metricIndex + 1].qpc) - $targetQpc) -le
           [Math]::Abs((Convert-ToInvariantDouble $metrics[$metricIndex].qpc) - $targetQpc)) {
      ++$metricIndex
    }
    $privateBytes = Convert-ToInvariantDouble $sample.privateBytes
    $replayBytes = Convert-ToInvariantDouble $metrics[$metricIndex].fields.replay_bytes
    $maximum = [Math]::Max($maximum, [Math]::Max(0.0, $privateBytes - $replayBytes))
  }
  return $maximum / 1MB
}

function Invoke-CaptureRun([object] $plan, [object] $run) {
  # Each capture owns its process until it has exited. The next baseline
  # therefore measures a system without this collector's capture process.
  $continuityRun = $run.index -eq 3
  $prefix = if ($continuityRun) { 'capture' } else { "capture-$($run.index)" }
  $captureLog = Join-Path $plan.performanceRoot "$prefix-engine.ndjson"
  $captureError = Join-Path $plan.performanceRoot "$prefix-engine.stderr.log"
  $samplerCsv = Join-Path $plan.performanceRoot $(if ($continuityRun) { 'engine-sampler.csv' } else { "$prefix-sampler.csv" })
  $runningEngine = Start-RedirectedArgumentProcess $plan.engine @($plan.captureArguments)
  $engineProcess = $runningEngine.process
  $captureTimer = [Diagnostics.Stopwatch]::StartNew()
  $sampler = $null
  try {
    $nvidiaSmi = (Get-Command nvidia-smi.exe -ErrorAction Stop).Source
    $sampler = Start-EngineSampler $engineProcess.Id $nvidiaSmi
    Start-Sleep -Seconds 3
    if ($engineProcess.HasExited) {
      throw "RebellioCap exited before capture run $($run.index): $($runningEngine.errorTask.GetAwaiter().GetResult())"
    }
    if ($continuityRun) {
      [void](Read-Host 'Press F8 once to save the first continuity clip, then press Enter')
    }
    $measurement = Invoke-PresentMonRun $plan $run
    if ($continuityRun) {
      Write-Host 'Waiting until 29:30 in the final capture session for the late A/V-drift clip...'
      Wait-ForCaptureMoment $engineProcess $captureTimer $plan.secondSaveAfterSeconds
      [void](Read-Host 'Press F8 once now to save the late-session continuity clip, then press Enter immediately')
    }
    Write-Host "Waiting for capture run $($run.index) to auto-stop at 30 minutes..."
    $remainingMs = [Math]::Max(0, 1800000 - $captureTimer.Elapsed.TotalMilliseconds) + 120000
    if (-not $engineProcess.WaitForExit([int]$remainingMs)) {
      throw "RebellioCap did not auto-stop for capture run $($run.index)."
    }
    if ($engineProcess.ExitCode -ne 0) {
      throw "RebellioCap capture run $($run.index) failed with exit $($engineProcess.ExitCode)."
    }
    return $measurement
  } finally {
    if (-not $engineProcess.HasExited) { Stop-Process -Id $engineProcess.Id -Force }
    $engineProcess.WaitForExit()
    if ($null -ne $sampler) {
      Stop-Job $sampler -ErrorAction SilentlyContinue
      @(Receive-Job $sampler -ErrorAction SilentlyContinue) | Export-Csv -LiteralPath $samplerCsv -NoTypeInformation
      Remove-Job $sampler -Force -ErrorAction SilentlyContinue
    }
    Complete-RedirectedProcess $runningEngine $captureLog $captureError
  }
}

function Invoke-BenchmarkCollection([object] $plan) {
  foreach ($run in $plan.runs) {
    if ($run.kind -eq 'baseline') {
      Invoke-PresentMonRun $plan $run
    } else {
      Invoke-CaptureRun $plan $run
    }
  }
}

$preflight = Get-PerformancePreflight
if ([string]::IsNullOrWhiteSpace($SettingsDeclaration)) {
  $SettingsDeclaration = Read-Host 'Declare the exact game preset, resolution, and display mode'
}
$plan = Get-CollectionPlan $preflight $SettingsDeclaration
$sourceCommit = $plan.buildIdentity.sourceCommit
New-Item -ItemType Directory -Force -Path $plan.performanceRoot | Out-Null
$clips = $plan.captureArguments[-1]
New-Item -ItemType Directory -Force -Path $clips | Out-Null
$settingsPath = Join-Path $plan.performanceRoot 'settings-declaration.json'
$captureLog = Join-Path $plan.performanceRoot 'capture-engine.ndjson'
$captureError = Join-Path $plan.performanceRoot 'engine.stderr.log'
$samplerCsv = Join-Path $plan.performanceRoot 'engine-sampler.csv'
$performanceSummaryPath = Join-Path $repositoryRoot 'artifacts\milestone-1\performance.json'
foreach ($path in @($settingsPath, $captureLog, $captureError, $samplerCsv,
    $performanceSummaryPath,
    (Join-Path $plan.performanceRoot 'clip-1-ffprobe.json'),
    (Join-Path $plan.performanceRoot 'clip-2-ffprobe.json'))) {
  if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite existing evidence: $path" }
}
foreach ($run in $plan.runs) {
  if (Test-Path -LiteralPath $run.csv) { throw "Refusing to overwrite existing evidence: $($run.csv)" }
}
foreach ($index in 1..2) {
  foreach ($suffix in @('engine.ndjson', 'engine.stderr.log', 'sampler.csv')) {
    $path = Join-Path $plan.performanceRoot "capture-$index-$suffix"
    if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite existing evidence: $path" }
  }
}
if (@(Get-ChildItem -LiteralPath $clips -Filter '*.mp4' -File).Count -ne 0) {
  throw "Refusing to mix existing clips with a new evidence run: $clips"
}
$sourceArchive = New-PerformanceSourceArchive $repositoryRoot (Join-Path $plan.performanceRoot 'source.zip')
$plan.buildIdentity | Add-Member -NotePropertyName sourceArchive -NotePropertyValue $sourceArchive
[PSCustomObject]@{
  recordedAtUtc = [DateTime]::UtcNow.ToString('O')
  declaration = $SettingsDeclaration
  sceneDeclaration = $SceneDeclaration
  buildIdentity = $plan.buildIdentity
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $settingsPath -Encoding utf8NoBOM

$results = @(Invoke-BenchmarkCollection $plan)

$baseline = @($results | Where-Object { $_.kind -eq 'baseline' })
$capture = @($results | Where-Object { $_.kind -eq 'capture' })
$baselineAverage = Get-Median @($baseline.averageFps)
$captureAverage = Get-Median @($capture.averageFps)
$baselineLow = Get-Median @($baseline.onePercentLowFps)
$captureLow = Get-Median @($capture.onePercentLowFps)
$averageRegression = ($baselineAverage - $captureAverage) * 100.0 / $baselineAverage
$lowRegression = ($baselineLow - $captureLow) * 100.0 / $baselineLow

$samples = @(Import-Csv -LiteralPath $samplerCsv)
$cpuAverage = $null
if ($samples.Count -ge 2) {
  $cpuDelta = (Convert-ToInvariantDouble $samples[-1].cpuTotalMs) - (Convert-ToInvariantDouble $samples[0].cpuTotalMs)
  $wallSeconds = ([DateTime]::Parse($samples[-1].timestampUtc) - [DateTime]::Parse($samples[0].timestampUtc)).TotalSeconds
  if ($wallSeconds -gt 0) {
    $cpuAverage = $cpuDelta / ($wallSeconds * 1000.0 * [Environment]::ProcessorCount) * 100.0
  }
}
$records = Read-Ndjson $captureLog
$metrics = @($records | Where-Object { $_.type -eq 'metrics' })
if ($metrics.Count -eq 0) { throw 'Capture NDJSON contains no metrics records.' }
$lastMetrics = $metrics[-1].fields
$workingMemory = Get-MaxWorkingMemoryExcludingReplay $samples $metrics
$saveEvidence = Get-SaveEvidence $metrics
$clipFiles = @(Get-ChildItem -LiteralPath $clips -Filter '*.mp4' -File | Sort-Object Name)
if ($clipFiles.Count -ne 2) {
  throw "Expected exactly two saved continuity clips, found $($clipFiles.Count)."
}
$clipDrifts = [System.Collections.Generic.List[double]]::new()
for ($index = 0; $index -lt $clipFiles.Count; ++$index) {
  $ffprobePath = Join-Path $plan.performanceRoot "clip-$($index + 1)-ffprobe.json"
  $ffprobeJson = & $plan.ffprobe -v error -show_streams -show_packets -of json $clipFiles[$index].FullName
  if ($LASTEXITCODE -ne 0) { throw "ffprobe failed for $($clipFiles[$index].FullName)." }
  $ffprobeJson | Set-Content -LiteralPath $ffprobePath -Encoding utf8NoBOM
  $clipDrifts.Add((Measure-FfprobeDrift $ffprobePath).avDriftMs)
}
$avDrift = [double](($clipDrifts | Measure-Object -Maximum).Maximum)
$encoderValues = @($samples.encoderUtilizationPercent | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_)
  } | ForEach-Object { Convert-ToInvariantDouble $_ })
$encoderAverage = if ($encoderValues.Count -gt 0) {
  [double](($encoderValues | Measure-Object -Average).Average)
} else { $null }
$maximumHandles = [int](($samples.handles | ForEach-Object { [int]$_ } |
  Measure-Object -Maximum).Maximum)
$performancePassed = $baseline.Count -eq 3 -and $capture.Count -eq 3 -and
  $null -ne $cpuAverage -and $cpuAverage -le 2.0 -and $workingMemory -le 250.0 -and
  [double]$lastMetrics.dropped_frames_percent -le 0.1 -and
  $avDrift -le 50.0 -and
  $averageRegression -le 3.0 -and $lowRegression -le 5.0 -and
  [double]$lastMetrics.hotkey_recognition_ms -le 100.0 -and
  $saveEvidence.status -eq 'passed'

$summary = [PSCustomObject]@{
  status = if ($performancePassed) { 'passed' } else { 'failed' }
  sourceCommit = $sourceCommit
  buildIdentity = $plan.buildIdentity
  gameExecutable = $plan.gameExecutable
  settingsDeclaration = $SettingsDeclaration
  sceneDeclaration = $SceneDeclaration
  resourceEvidenceCaptureIndex = 3
  baselineRuns = $baseline.Count
  captureRuns = $capture.Count
  runs = @($results)
  baselineMedianAverageFps = $baselineAverage
  captureMedianAverageFps = $captureAverage
  baselineMedianOnePercentLowFps = $baselineLow
  captureMedianOnePercentLowFps = $captureLow
  averageFpsRegressionPercent = $averageRegression
  onePercentLowRegressionPercent = $lowRegression
  cpuAveragePercent = $cpuAverage
  workingMemoryExcludingReplayMiB = $workingMemory
  maximumHandleCount = $maximumHandles
  encoderUtilizationAveragePercent = $encoderAverage
  droppedFramesPercent = [double]$lastMetrics.dropped_frames_percent
  avDriftMs = $avDrift
  encodeP95Us = [double]$lastMetrics.encode_p95_us
  hotkeyRecognitionMs = [double]$lastMetrics.hotkey_recognition_ms
  saveContinuity = $saveEvidence
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $performanceSummaryPath -Encoding utf8NoBOM
$summary | ConvertTo-Json -Depth 8
if (-not $performancePassed) { exit 1 }
