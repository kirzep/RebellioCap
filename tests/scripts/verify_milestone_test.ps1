Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$verifier = Join-Path $repositoryRoot 'scripts\verify-milestone-1.ps1'
$collector = Join-Path $repositoryRoot 'scripts\collect-performance.ps1'
$manifestWriter = Join-Path $repositoryRoot 'scripts\write-milestone-manifest.ps1'
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) "rebelliocap-verifier-$PID"
New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null
try {
  $performanceRoot = Join-Path $temporaryRoot 'performance'
  New-Item -ItemType Directory -Force -Path $performanceRoot | Out-Null
  '{"type":"doctor","fields":{"passed":true,"os_version":"test-os","gpu":"test-gpu","driver_version":"test-driver"}}' | Set-Content -LiteralPath (Join-Path $temporaryRoot 'doctor.ndjson')
  '{"type":"metrics"}' | Set-Content -LiteralPath (Join-Path $temporaryRoot 'capture-30m.ndjson')
  '{}' | Set-Content -LiteralPath (Join-Path $temporaryRoot 'ffprobe-mp4.json')
  '{}' | Set-Content -LiteralPath (Join-Path $temporaryRoot 'ffprobe-mkv.json')
  '<testsuite tests="1" failures="0" errors="0" skipped="0" disabled="0"><testcase name="unit" status="run" /></testsuite>' | Set-Content -LiteralPath (Join-Path $temporaryRoot 'ctest-unit.xml')
  '<testsuite tests="1" failures="0" errors="0" skipped="0" disabled="0"><testcase name="hotkey_passthrough" status="run" /></testsuite>' | Set-Content -LiteralPath (Join-Path $temporaryRoot 'ctest-hardware.xml')
  '{"status":"passed"}' | Set-Content -LiteralPath (Join-Path $temporaryRoot 'performance.json')
  '{}' | Set-Content -LiteralPath (Join-Path $performanceRoot 'settings-declaration.json')
  '{"type":"metrics","qpc":0,"fields":{"qpc_frequency":1000,"replay_bytes":0}}' | Set-Content -LiteralPath (Join-Path $performanceRoot 'capture-engine.ndjson')
  'timestampUtc,qpc,qpcFrequency,cpuTotalMs,privateBytes,handles,encoderUtilizationPercent' | Set-Content -LiteralPath (Join-Path $performanceRoot 'engine-sampler.csv')
  foreach ($kind in @('baseline', 'capture')) {
    foreach ($index in 1..3) {
      Copy-Item -LiteralPath (Join-Path $repositoryRoot 'tests\data\presentmon-displayed.csv') -Destination (Join-Path $performanceRoot "$kind-$index.csv")
    }
  }
  '{}' | Set-Content -LiteralPath (Join-Path $performanceRoot 'clip-1-ffprobe.json')
  '{}' | Set-Content -LiteralPath (Join-Path $performanceRoot 'clip-2-ffprobe.json')
  $artifactNames = @('doctor.ndjson', 'capture-30m.ndjson', 'ffprobe-mp4.json',
    'ffprobe-mkv.json', 'ctest-unit.xml', 'ctest-hardware.xml', 'performance.json',
    'performance/settings-declaration.json', 'performance/capture-engine.ndjson',
    'performance/engine-sampler.csv', 'performance/baseline-1.csv',
    'performance/baseline-2.csv', 'performance/baseline-3.csv',
    'performance/capture-1.csv', 'performance/capture-2.csv',
    'performance/capture-3.csv', 'performance/clip-1-ffprobe.json',
    'performance/clip-2-ffprobe.json')
  $artifacts = @($artifactNames | ForEach-Object {
    [PSCustomObject]@{
      path = "artifacts/milestone-1/$_"
      sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $temporaryRoot $_)).Hash.ToLowerInvariant()
    }
  })
  $stream = [PSCustomObject]@{
    status = 'passed'; durationSeconds = 30; videoCodec = 'h264'; videoCount = 1
    audioCodec = 'aac'; audioCount = 3; audioNames = @('Mixed', 'System', 'Microphone')
    monotonicTimestamps = $true
  }
  $manifest = [PSCustomObject]@{
    schemaVersion = 1
    commit = [PSCustomObject]@{ sha = (& git -C $repositoryRoot rev-parse HEAD).Trim(); dirty = $false }
    host = [PSCustomObject]@{ osBuild = 'test'; gpu = 'test'; driver = 'test' }
    tools = [PSCustomObject]@{ cmake = 'test'; ninja = 'test'; msvc = 'test'; ffprobe = 'test'; presentMon = 'test' }
    commands = @('test')
    artifacts = $artifacts
    ctest = [PSCustomObject]@{
      unit = [PSCustomObject]@{ status = 'passed'; total = 1; passed = 1; failed = 0; skipped = 0 }
      hardware = [PSCustomObject]@{ status = 'passed'; total = 1; passed = 1; failed = 0; skipped = 0 }
    }
    streams = [PSCustomObject]@{ mp4 = $stream; mkv = $stream }
    capture30m = [PSCustomObject]@{ status = 'not-run'; durationMinutes = 30; avDriftMs = 0; droppedFramesPercent = 0 }
    hotkey = [PSCustomObject]@{ status = 'passed'; recognitionMs = 1; inputPassThrough = $true }
    saveContinuity = [PSCustomObject]@{ status = 'passed'; finalizeSeconds = 1; packetsIncreasedBeforeDuringAfter = $true }
    performance = [PSCustomObject]@{
      status = 'passed'; gameExecutable = 'test'; baselineRuns = 3; captureRuns = 3
      cpuAveragePercent = 1; workingMemoryExcludingReplayMiB = 1
      averageFpsRegressionPercent = 1; onePercentLowRegressionPercent = 1
    }
  }
  $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $temporaryRoot 'manifest.json')

  $output = & $verifier -EvidenceRoot $temporaryRoot
  if ($LASTEXITCODE -eq 0) { throw 'Verifier accepted not-run capture evidence.' }
  $result = $output | ConvertFrom-Json
  if (-not (@($result.failures) -contains 'capture30m.status must be passed, found: not-run')) {
    throw 'Verifier did not report the not-run capture gate.'
  }

  $manifest.capture30m.status = 'passed'
  $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $temporaryRoot 'manifest.json')
  $output = & $verifier -EvidenceRoot $temporaryRoot
  if ($LASTEXITCODE -eq 0) { throw 'Verifier accepted fabricated raw evidence.' }
  $result = $output | ConvertFrom-Json
  if (-not (@($result.failures) -contains 'capture-30m.ndjson has no usable metrics records')) {
    throw 'Verifier did not reject the fabricated capture log.'
  }
  if (@($result.failures | Where-Object { $_ -like 'ffprobe-*.json:*' }).Count -ne 2) {
    throw 'Verifier did not reject both fabricated ffprobe artifacts.'
  }
  if (-not (@($result.failures) -contains 'performance.json lacks auditable benchmark fields')) {
    throw 'Verifier did not reject the fabricated performance summary.'
  }

  @(
    '{"type":"recording_started","qpc":0,"fields":{}}',
    '{"type":"metrics","qpc":1800000,"fields":{"qpc_frequency":1000,"dropped_frames_percent":0,"hotkey_recognition_ms":1,"completed_saves":2,"failed_saves":0,"rejected_saves":0,"pipeline_errors":0}}'
  ) | Set-Content -LiteralPath (Join-Path $temporaryRoot 'capture-30m.ndjson')
  $ffprobe = [PSCustomObject]@{
    streams = @(
      [PSCustomObject]@{ index = 0; codec_type = 'video'; codec_name = 'h264'; duration = '30'; tags = [PSCustomObject]@{ handler_name = 'Video' } }
      [PSCustomObject]@{ index = 1; codec_type = 'audio'; codec_name = 'aac'; duration = '30'; tags = [PSCustomObject]@{ handler_name = 'Mixed' } }
      [PSCustomObject]@{ index = 2; codec_type = 'audio'; codec_name = 'aac'; duration = '30'; tags = [PSCustomObject]@{ handler_name = 'System' } }
      [PSCustomObject]@{ index = 3; codec_type = 'audio'; codec_name = 'aac'; duration = '30'; tags = [PSCustomObject]@{ handler_name = 'Microphone' } }
    )
    packets = @(0..3 | ForEach-Object {
      [PSCustomObject]@{ stream_index = $_; pts_time = '0.0'; dts_time = '0.0'; duration_time = '30.0' }
    })
  }
  foreach ($name in @('ffprobe-mp4.json', 'ffprobe-mkv.json')) {
    $ffprobe | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $temporaryRoot $name)
  }
  foreach ($name in @('clip-1-ffprobe.json', 'clip-2-ffprobe.json')) {
    $ffprobe | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $performanceRoot $name)
  }
  @(
    '{"type":"metrics","qpc":0,"fields":{"qpc_frequency":1000,"replay_bytes":0,"video_packets":1,"audio_packets":3,"save_requests":0,"completed_saves":0,"dropped_frames_percent":0,"hotkey_recognition_ms":0}}',
    '{"type":"metrics","qpc":1000,"fields":{"qpc_frequency":1000,"replay_bytes":0,"video_packets":2,"audio_packets":6,"save_requests":1,"completed_saves":1,"dropped_frames_percent":0,"hotkey_recognition_ms":1}}',
    '{"type":"metrics","qpc":2000,"fields":{"qpc_frequency":1000,"replay_bytes":0,"video_packets":3,"audio_packets":9,"save_requests":1,"completed_saves":1,"dropped_frames_percent":0,"hotkey_recognition_ms":1}}',
    '{"type":"metrics","qpc":3000,"fields":{"qpc_frequency":1000,"replay_bytes":0,"video_packets":4,"audio_packets":12,"save_requests":2,"completed_saves":2,"dropped_frames_percent":0,"hotkey_recognition_ms":1}}',
    '{"type":"metrics","qpc":4000,"fields":{"qpc_frequency":1000,"replay_bytes":0,"video_packets":5,"audio_packets":15,"save_requests":2,"completed_saves":2,"dropped_frames_percent":0,"hotkey_recognition_ms":1}}'
  ) | Set-Content -LiteralPath (Join-Path $performanceRoot 'capture-engine.ndjson')
  $cpuEnd = 10 * [Environment]::ProcessorCount
  @(
    [PSCustomObject]@{ timestampUtc='2026-01-01T00:00:00Z'; qpc=0; qpcFrequency=1000; cpuTotalMs=0; privateBytes=1048576; handles=10; encoderUtilizationPercent=1 },
    [PSCustomObject]@{ timestampUtc='2026-01-01T00:00:01Z'; qpc=1000; qpcFrequency=1000; cpuTotalMs=$cpuEnd; privateBytes=1048576; handles=10; encoderUtilizationPercent=1 }
  ) | Export-Csv -LiteralPath (Join-Path $performanceRoot 'engine-sampler.csv') -NoTypeInformation
  $settingsDeclaration = 'preset=test; resolution=1920x1080; route=deterministic'
  [PSCustomObject]@{ declaration = $settingsDeclaration } | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $performanceRoot 'settings-declaration.json')
  $runAnalysis = (& $collector -AnalyzeCsv (Join-Path $performanceRoot 'baseline-1.csv')) | ConvertFrom-Json
  $runs = @(
    foreach ($kind in @('baseline', 'capture')) {
      foreach ($index in 1..3) {
        [PSCustomObject]@{ kind=$kind; index=$index; averageFps=[double]$runAnalysis.averageFps; onePercentLowFps=[double]$runAnalysis.onePercentLowFps }
      }
    }
  )
  [PSCustomObject]@{
    status = 'passed'; sourceCommit = $manifest.commit.sha; gameExecutable = 'test'; baselineRuns = 3; captureRuns = 3
    cpuAveragePercent = 1; workingMemoryExcludingReplayMiB = 1
    averageFpsRegressionPercent = 0; onePercentLowRegressionPercent = 0
    droppedFramesPercent = 0; avDriftMs = 0; hotkeyRecognitionMs = 1
    settingsDeclaration = $settingsDeclaration; runs = $runs
    saveContinuity = [PSCustomObject]@{ status = 'passed'; finalizeSeconds = 1; packetsContinuous = $true }
  } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $temporaryRoot 'performance.json')
  Remove-Item -LiteralPath (Join-Path $temporaryRoot 'manifest.json')

  # CI configures with Visual Studio's CMake without downloading another copy.
  # Exercise the real writer in that layout, independently of local tool caches.
  $toolchainFixture = Join-Path $temporaryRoot 'toolchain-fixture'
  $fixtureScripts = Join-Path $toolchainFixture 'scripts'
  $fixtureBuild = Join-Path $toolchainFixture 'build/windows-debug'
  New-Item -ItemType Directory -Force -Path $fixtureScripts, $fixtureBuild,
    (Join-Path $toolchainFixture '.tools/vcpkg/downloads/tools') | Out-Null
  $fixtureWriter = Join-Path $fixtureScripts 'write-milestone-manifest.ps1'
  Copy-Item -LiteralPath $manifestWriter -Destination $fixtureWriter
  # Application discovery may return several PATH matches; mirror executable lookup.
  $configuredCmake = (Get-Command cmake.exe -CommandType Application | Select-Object -First 1).Source
  $configuredNinja = (Get-Command ninja.exe -CommandType Application | Select-Object -First 1).Source
  $configuredCompiler = (Get-Command cl.exe -CommandType Application | Select-Object -First 1).Source
  @(
    "CMAKE_COMMAND:INTERNAL=$configuredCmake"
    "CMAKE_MAKE_PROGRAM:FILEPATH=$configuredNinja"
    "CMAKE_CXX_COMPILER:FILEPATH=$configuredCompiler"
  ) | Set-Content -LiteralPath (Join-Path $fixtureBuild 'CMakeCache.txt')
  $expectedCmakeVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($configuredCmake).ProductVersion
  if ([string]::IsNullOrWhiteSpace($expectedCmakeVersion)) { $expectedCmakeVersion = $configuredCmake }
  $originalGitDirectory = $env:GIT_DIR
  $sourceGitDirectory = (& git -C $repositoryRoot rev-parse --absolute-git-dir).Trim()
  if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve source Git directory for manifest fixture.' }
  try {
    # Keep commit validation real while resolving tool paths from the fixture.
    $env:GIT_DIR = $sourceGitDirectory
    try { & $fixtureWriter -EvidenceRoot $temporaryRoot | Out-Null }
    catch { throw "Manifest writer failed with configured Visual Studio CMake and an empty optional vcpkg tool cache: $($_.Exception.Message)" }
    $written = Get-Content -LiteralPath (Join-Path $temporaryRoot 'manifest.json') -Raw | ConvertFrom-Json
    if ($written.tools.cmake -ne $expectedCmakeVersion) {
      throw "Manifest omitted the configured CMake identity: expected $expectedCmakeVersion, found $($written.tools.cmake)."
    }
  } finally { $env:GIT_DIR = $originalGitDirectory }
  Remove-Item -LiteralPath (Join-Path $temporaryRoot 'manifest.json')

  & $manifestWriter -EvidenceRoot $temporaryRoot | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'Manifest writer rejected valid raw evidence.' }
  $output = & $verifier -EvidenceRoot $temporaryRoot
  if ($LASTEXITCODE -ne 0) {
    throw "Verifier rejected valid raw evidence: $($output -join [Environment]::NewLine)"
  }
  Write-Output 'milestone verifier rejection passed'
} finally {
  $resolved = [IO.Path]::GetFullPath($temporaryRoot)
  $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
  if ($resolved.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -and
      (Test-Path -LiteralPath $resolved)) {
    Remove-Item -LiteralPath $resolved -Recurse -Force
  }
}
