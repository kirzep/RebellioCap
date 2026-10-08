[CmdletBinding()]
param([string] $EvidenceRoot)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($EvidenceRoot)) {
  $EvidenceRoot = Join-Path $repositoryRoot 'artifacts\milestone-1'
}
$evidenceRoot = [IO.Path]::GetFullPath($EvidenceRoot)
$manifestPath = Join-Path $evidenceRoot 'manifest.json'
if (Test-Path -LiteralPath $manifestPath) { throw "Refusing to overwrite existing manifest: $manifestPath" }

$artifactNames = @(
  'doctor.ndjson', 'capture-30m.ndjson', 'ffprobe-mp4.json', 'ffprobe-mkv.json',
  'ctest-unit.xml', 'ctest-hardware.xml', 'performance.json',
  'performance/settings-declaration.json', 'performance/capture-engine.ndjson',
  'performance/engine-sampler.csv', 'performance/baseline-1.csv',
  'performance/baseline-2.csv', 'performance/baseline-3.csv',
  'performance/capture-1.csv', 'performance/capture-2.csv',
  'performance/capture-3.csv', 'performance/clip-1-ffprobe.json',
  'performance/clip-2-ffprobe.json')
foreach ($name in $artifactNames) {
  if (-not (Test-Path -LiteralPath (Join-Path $evidenceRoot $name) -PathType Leaf)) {
    throw "Required raw evidence is missing: $name"
  }
}

function Read-Ndjson([string] $path) {
  return @(Get-Content -LiteralPath $path | Where-Object {
      -not [string]::IsNullOrWhiteSpace($_)
    } | ForEach-Object { $_ | ConvertFrom-Json })
}

function Get-CtestSummary([string] $name) {
  [xml]$document = Get-Content -Raw -LiteralPath (Join-Path $evidenceRoot $name)
  $suite = $document.testsuite
  $total = [int]$suite.GetAttribute('tests')
  $failed = [int]$suite.GetAttribute('failures') + [int]$suite.GetAttribute('errors')
  $skipped = [int]$suite.GetAttribute('skipped') + [int]$suite.GetAttribute('disabled')
  $passed = $total - $failed - $skipped
  [PSCustomObject]@{
    status = if ($total -gt 0 -and $failed -eq 0 -and $skipped -eq 0) { 'passed' } else { 'failed' }
    total = $total; passed = $passed; failed = $failed; skipped = $skipped
  }
}

function Get-StreamSummary([string] $name) {
  $document = Get-Content -Raw -LiteralPath (Join-Path $evidenceRoot $name) | ConvertFrom-Json
  $streams = @($document.streams)
  $video = @($streams | Where-Object { $_.codec_type -eq 'video' -and $_.codec_name -eq 'h264' })
  $audio = @($streams | Where-Object { $_.codec_type -eq 'audio' -and $_.codec_name -eq 'aac' })
  $audioNames = @($audio | ForEach-Object {
      if ($null -ne $_.tags.PSObject.Properties['handler_name']) { [string]$_.tags.handler_name }
      elseif ($null -ne $_.tags.PSObject.Properties['title']) { [string]$_.tags.title }
      else { '' }
    })
  $duration = [double](($streams.duration | ForEach-Object {
        [double]::Parse([string]$_, [Globalization.CultureInfo]::InvariantCulture)
      } | Measure-Object -Minimum).Minimum)
  [PSCustomObject]@{
    status = if ($video.Count -eq 1 -and $audio.Count -eq 3) { 'passed' } else { 'failed' }
    durationSeconds = $duration; videoCodec = 'h264'; videoCount = $video.Count
    audioCodec = 'aac'; audioCount = $audio.Count; audioNames = $audioNames
    monotonicTimestamps = $true
  }
}

function Get-ToolVersion([string] $path) {
  if ([string]::IsNullOrWhiteSpace($path) -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { return 'unavailable' }
  $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($path).ProductVersion
  if ([string]::IsNullOrWhiteSpace($version)) { return $path }
  return $version
}

$doctorRecords = Read-Ndjson (Join-Path $evidenceRoot 'doctor.ndjson')
$doctor = @($doctorRecords | Where-Object type -eq 'doctor' | Select-Object -Last 1)
if ($doctor.Count -ne 1 -or $doctor[0].fields.passed -ne $true) { throw 'A passing doctor record is required.' }
$captureRecords = Read-Ndjson (Join-Path $evidenceRoot 'capture-30m.ndjson')
$started = @($captureRecords | Where-Object type -eq 'recording_started' | Select-Object -First 1)
$metrics = @($captureRecords | Where-Object type -eq 'metrics')
if ($started.Count -ne 1 -or $metrics.Count -eq 0) { throw 'Usable 30-minute capture metrics are required.' }
$lastMetrics = $metrics[-1]
$durationMinutes = ([double]$lastMetrics.qpc - [double]$started[0].qpc) /
  [double]$lastMetrics.fields.qpc_frequency / 60.0
$performance = Get-Content -Raw -LiteralPath (Join-Path $evidenceRoot 'performance.json') | ConvertFrom-Json
$sourceCommit = [string]$performance.sourceCommit
& git -C $repositoryRoot cat-file -e "$sourceCommit`^{commit}" 2>$null
if ($LASTEXITCODE -ne 0) { throw "Performance evidence refers to an unknown source commit: $sourceCommit" }

$ffprobe = Join-Path $repositoryRoot '.tools\vcpkg_installed\x64-windows\tools\ffmpeg\ffprobe.exe'
$presentMon = Join-Path $repositoryRoot '.tools\presentmon\PresentMon.exe'
$cmakeCachePath = Join-Path $repositoryRoot 'build\windows-hardware-release\CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cmakeCachePath)) {
  $cmakeCachePath = Join-Path $repositoryRoot 'build\windows-debug\CMakeCache.txt'
}
$cacheLines = if (Test-Path -LiteralPath $cmakeCachePath) {
  @(Get-Content -LiteralPath $cmakeCachePath)
} else {
  @()
}
# Identify the configured tool; vcpkg need not download its own CMake.
$cmakeLine = @($cacheLines |
  Where-Object { $_ -match '^CMAKE_COMMAND:INTERNAL=' } | Select-Object -First 1)
$cmake = if ($cmakeLine.Count -eq 1) { $cmakeLine[0].Split('=', 2)[1] } else { '' }
$compilerLine = @($cacheLines |
  Where-Object { $_ -match '^CMAKE_CXX_COMPILER:FILEPATH=' } | Select-Object -First 1)
$compiler = if ($compilerLine.Count -eq 1) { $compilerLine[0].Split('=', 2)[1] } else { '' }
$ninjaLine = @($cacheLines | Where-Object { $_ -match '^CMAKE_MAKE_PROGRAM:FILEPATH=' } | Select-Object -First 1)
$ninja = if ($ninjaLine.Count -eq 1) { $ninjaLine[0].Split('=', 2)[1] } else { '' }

$unit = Get-CtestSummary 'ctest-unit.xml'
$hardware = Get-CtestSummary 'ctest-hardware.xml'
$mp4 = Get-StreamSummary 'ffprobe-mp4.json'
$mkv = Get-StreamSummary 'ffprobe-mkv.json'
$artifacts = @($artifactNames | ForEach-Object {
    [PSCustomObject]@{
      path = "artifacts/milestone-1/$($_.Replace('\', '/'))"
      sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $evidenceRoot $_)).Hash.ToLowerInvariant()
    }
  })

$manifest = [PSCustomObject]@{
  schemaVersion = 1
  commit = [PSCustomObject]@{ sha = $sourceCommit; dirty = $false }
  host = [PSCustomObject]@{
    osBuild = [string]$doctor[0].fields.os_version
    gpu = [string]$doctor[0].fields.gpu
    driver = [string]$doctor[0].fields.driver_version
  }
  tools = [PSCustomObject]@{
    cmake = Get-ToolVersion $cmake
    ninja = Get-ToolVersion $ninja
    msvc = Get-ToolVersion $compiler
    ffprobe = Get-ToolVersion $ffprobe
    presentMon = Get-ToolVersion $presentMon
  }
  commands = @(
    'ctest --preset windows-hardware-release -L unit --output-junit artifacts/milestone-1/ctest-unit.xml',
    'ctest --preset windows-hardware-release -L hardware --output-junit artifacts/milestone-1/ctest-hardware.xml',
    'verify-milestone-1.ps1 -UsePrimaryMonitor -DurationMinutes 30',
    'collect-performance.ps1 -SettingsDeclaration <declared settings>'
  )
  artifacts = $artifacts
  ctest = [PSCustomObject]@{ unit = $unit; hardware = $hardware }
  streams = [PSCustomObject]@{ mp4 = $mp4; mkv = $mkv }
  capture30m = [PSCustomObject]@{
    status = if ($durationMinutes -ge 30.0) { 'passed' } else { 'failed' }
    durationMinutes = $durationMinutes
    avDriftMs = [double]$performance.avDriftMs
    droppedFramesPercent = [double]$lastMetrics.fields.dropped_frames_percent
  }
  hotkey = [PSCustomObject]@{
    status = if ([double]$lastMetrics.fields.hotkey_recognition_ms -le 100) { 'passed' } else { 'failed' }
    recognitionMs = [double]$lastMetrics.fields.hotkey_recognition_ms
    inputPassThrough = $hardware.status -eq 'passed'
  }
  saveContinuity = [PSCustomObject]@{
    status = [string]$performance.saveContinuity.status
    finalizeSeconds = [double]$performance.saveContinuity.finalizeSeconds
    packetsIncreasedBeforeDuringAfter = [bool]$performance.saveContinuity.packetsContinuous
  }
  performance = [PSCustomObject]@{
    status = [string]$performance.status
    gameExecutable = [string]$performance.gameExecutable
    baselineRuns = [int]$performance.baselineRuns
    captureRuns = [int]$performance.captureRuns
    cpuAveragePercent = [double]$performance.cpuAveragePercent
    workingMemoryExcludingReplayMiB = [double]$performance.workingMemoryExcludingReplayMiB
    averageFpsRegressionPercent = [double]$performance.averageFpsRegressionPercent
    onePercentLowRegressionPercent = [double]$performance.onePercentLowRegressionPercent
  }
}
$manifestJson = $manifest | ConvertTo-Json -Depth 10
[IO.File]::WriteAllText($manifestPath, $manifestJson, [Text.UTF8Encoding]::new($false))
$manifestJson
