# Short experiments are exploratory. The 30-minute acceptance collector owns its gates.
function Assert-ShortInteractiveInput([bool] $InputRedirected = [Console]::IsInputRedirected) {
  if ($InputRedirected) { throw 'Short game experiments require interactive stdin (TTY); redirected or closed stdin can skip the scene gates.' }
}

function Test-ShortGateEnter {
  return [Console]::KeyAvailable -and [Console]::ReadKey($true).Key -eq [ConsoleKey]::Enter
}

function Wait-ShortSceneGate([object] $Session, [object] $GameProcess, [string] $Prompt) {
  Write-Host ($Prompt + ': ') -NoNewline
  while (-not (Test-ShortGateEnter)) {
    # Keep stdout drained and surface engine/game failures even while the human
    # is preparing the scene. Read-Host here leaves a live session unattended.
    Wait-ShortPhase $Session $GameProcess .1
  }
  Write-Host ''
}

function Get-ShortForegroundProcessId {
  if (-not ('RebellioCapExperimentForeground' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RebellioCapExperimentForeground {
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
  public static uint ProcessId() { uint id; GetWindowThreadProcessId(GetForegroundWindow(), out id); return id; }
}
'@
  }
  return [RebellioCapExperimentForeground]::ProcessId()
}

function Wait-ShortGameForeground([object] $Session, [object] $GameProcess, [double] $TimeoutSeconds = 30) {
  $timer=[Diagnostics.Stopwatch]::StartNew()
  while ((Get-ShortForegroundProcessId) -ne $GameProcess.Id) {
    if ($timer.Elapsed.TotalSeconds -ge $TimeoutSeconds) { throw 'Timed out waiting for game foreground before measurement.' }
    Wait-ShortPhase $Session $GameProcess .1
  }
}

function Get-ShortExperimentPlan {
  param([string] $Mode, [string] $OutputRoot, [string] $Settings, [string] $Scene,
        [string] $Game, [string] $ConfigPath, [int] $Seconds = 60, [int] $Pairs = 3,
        [string] $Repository)
  if ($Mode -notin @('game-only','replay-only','continuous-only','combined','save-burst')) { throw "Unknown experiment mode: $Mode" }
  if ($Pairs -lt 3 -or $Pairs -gt 20) { throw 'Short experiments require 3..20 alternating pairs.' }
  if ($Seconds -lt 30 -or $Seconds -gt 600) { throw 'Short measurement duration must be 30..600 seconds.' }
  foreach ($value in @($OutputRoot,$Settings,$Scene,$Game,$Repository)) {
    if ([string]::IsNullOrWhiteSpace($value)) { throw 'Output root, settings, scene, game and repository must be explicit.' }
  }
  $root = [IO.Path]::GetFullPath($OutputRoot)
  $repositoryPath = [IO.Path]::GetFullPath($Repository).TrimEnd([IO.Path]::DirectorySeparatorChar)
  if ($root.Equals($repositoryPath, [StringComparison]::OrdinalIgnoreCase) -or
      $root.StartsWith($repositoryPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    $relative = [IO.Path]::GetRelativePath($repositoryPath, $root).Replace('\','/')
    & git -C $repositoryPath check-ignore --quiet -- $relative
    if ($LASTEXITCODE -ne 0) { throw 'Experiment evidence inside the repository must use an ignored subtree such as build/optimization, or a directory outside the repository.' }
  }
  $runs = [Collections.Generic.List[object]]::new()
  foreach ($pair in 1..$Pairs) {
    foreach ($side in @('A','B')) {
      $runMode = if ($side -eq 'A') { 'game-only' } else { $Mode }
      $id = '{0:D2}-{1}-{2}' -f $pair,$side,$runMode
      $directory = Join-Path $root $id
      $runs.Add([pscustomobject]@{id=$id; pair=$pair; side=$side; mode=$runMode; directory=$directory; csv=(Join-Path $directory 'presentmon.csv')})
    }
  }
  [pscustomobject]@{
    schemaVersion=1; status='planned-unmeasured'; purpose='short-experiment'; acceptanceStatus='not-evaluated'
    mode=$Mode; warmupSeconds=60; measurementSeconds=$Seconds; pairs=$Pairs; burstCount=3
    burstOffsetSeconds=[int][Math]::Floor($Seconds/2); saveCompletionTimeoutSeconds=30
    outputRoot=$root; settingsDeclaration=$Settings; sceneDeclaration=$Scene
    gameExecutable=[IO.Path]::GetFullPath($Game); configPath=$ConfigPath
    engine=(Join-Path $Repository 'build/windows-hardware-release/RebellioCap.Engine.exe')
    presentMon=(Join-Path $Repository '.tools/presentmon/PresentMon.exe')
    sessionName=('RebellioCapShort-' + [guid]::NewGuid().ToString('N')); runs=@($runs)
    resourceClock='collector-receive-qpc'; encoderUtilizationScope='nvidia-smi first GPU, device-wide'
    verifyGameForeground=$true
    limitation='Short experiments do not evaluate 30-minute acceptance or A/V drift. NVENC diagnostics require pipelineTelemetryEnabled; endpoint latency histograms include warmup.'
  }
}

function New-ShortExperimentEvidence([object] $Plan) {
  # CreateDirectory is idempotent; mkdir without -Force rejects an existing leaf/root.
  if (Test-Path -LiteralPath $Plan.outputRoot) { throw "Refusing to overwrite existing evidence: $($Plan.outputRoot)" }
  New-Item -ItemType Directory -Path $Plan.outputRoot -ErrorAction Stop | Out-Null
  foreach ($run in $Plan.runs) { New-Item -ItemType Directory -Path $run.directory -ErrorAction Stop | Out-Null }
}

function Get-ShortRunConfiguration([System.Collections.IDictionary] $Configuration, [object] $Run) {
  $copy = @{}
  foreach ($key in $Configuration.Keys) { $copy[$key] = $Configuration[$key] }
  $copy.continuousRecordingEnabled = $false
  $copy.outputDirectory = Join-Path $Run.directory 'clips'
  return $copy
}

function Get-ShortModeCommands([string] $Mode) {
  switch ($Mode) {
    'continuous-only' { 'stop_replay'; 'toggle_recording' }
    'combined' { 'toggle_recording' }
    'save-burst' { 'toggle_recording' }
    'replay-only' { }
    'game-only' { }
    default { throw "Unknown mode: $Mode" }
  }
}

function Measure-ShortRunResources([object[]] $Samples, [object[]] $Snapshots,
                                   [string] $Mode, [double] $ExpectedSeconds) {
  if ($Mode -notin @('game-only','replay-only','continuous-only','combined','save-burst')) { throw 'Unknown resource mode.' }
  if ($Samples.Count -lt 2) { throw 'Each experiment run requires at least two resource samples.' }
  $frequency = [double]$Samples[0].qpcFrequency
  if (-not [double]::IsFinite($frequency) -or $frequency -le 0) { throw 'Invalid sampler QPC frequency.' }
  $previous = $null
  $encoderValues = [Collections.Generic.List[double]]::new()
  foreach ($sample in $Samples) {
    foreach ($field in @('qpc','qpcFrequency','cpuTotalMs','privateBytes','handles','encoderUtilizationPercent')) {
      if ([string]::IsNullOrWhiteSpace([string]$sample.$field)) { throw "Missing resource value: $field" }
      $number = [double]::Parse([string]$sample.$field, [Globalization.CultureInfo]::InvariantCulture)
      if (-not [double]::IsFinite($number) -or $number -lt 0) { throw "Invalid resource value: $field" }
    }
    if ([double]$sample.qpcFrequency -ne $frequency -or [double]$sample.encoderUtilizationPercent -gt 100) { throw 'Invalid sampler clock or encoder utilization.' }
    if ($null -ne $previous -and ([double]$sample.qpc -le [double]$previous.qpc -or
      ([double]$sample.qpc-[double]$previous.qpc)/$frequency -gt 5 -or [double]$sample.cpuTotalMs -lt [double]$previous.cpuTotalMs)) { throw 'Discontinuous resource samples.' }
    $previous = $sample
    $encoderValues.Add([double]$sample.encoderUtilizationPercent)
  }
  $seconds = ([double]$Samples[-1].qpc-[double]$Samples[0].qpc)/$frequency
  if ($seconds -le 0 -or $seconds -lt $ExpectedSeconds-5) { throw 'Resource evidence does not cover the measurement.' }
  $maximumPrivate = [double](($Samples | ForEach-Object {[double]$_.privateBytes} | Measure-Object -Maximum).Maximum)/1MB
  $withoutReplay = $null
  if ($Mode -ne 'game-only') {
    if ($Snapshots.Count -lt 2) { throw 'Each recorder mode requires its own snapshot evidence.' }
    foreach ($item in $Snapshots) {
      if ([string]::IsNullOrWhiteSpace([string]$item.qpc) -or -not [double]::IsFinite([double]$item.qpc) -or [double]$item.qpc -lt 0) { throw 'Invalid snapshot clock.' }
    }
    if ([Math]::Abs([double]$Snapshots[0].qpc-[double]$Samples[0].qpc)/$frequency -gt 5 -or
        [Math]::Abs([double]$Snapshots[-1].qpc-[double]$Samples[-1].qpc)/$frequency -gt 5 -or
        ([double]$Snapshots[-1].qpc-[double]$Snapshots[0].qpc)/$frequency -lt $ExpectedSeconds-5) { throw 'Snapshots do not cover the measurement.' }
    $previousQpc = $null
    foreach ($item in $Snapshots) {
      if ($null -ne $previousQpc -and ([double]$item.qpc -lt $previousQpc -or ([double]$item.qpc-$previousQpc)/$frequency -gt 5)) { throw 'Discontinuous snapshots.' }
      $previousQpc = [double]$item.qpc
      $snapshot = $item.snapshot
      $replayExpected = $Mode -ne 'continuous-only'
      $continuousExpected = $Mode -in @('continuous-only','combined','save-burst')
      if ($snapshot.replayActive -ne $replayExpected -or $snapshot.continuousRecordingActive -ne $continuousExpected) { throw 'Engine state does not match the experiment mode.' }
      foreach ($field in @('replayBytes','videoPackets','continuousPackets','pipelineErrors','continuousFailures','failedSaves','rejectedSaves')) {
        if ([string]::IsNullOrWhiteSpace([string]$snapshot.metrics.$field)) { throw "Missing snapshot resource value: $field" }
        $number = [double]$snapshot.metrics.$field
        if (-not [double]::IsFinite($number) -or $number -lt 0) { throw "Invalid snapshot resource value: $field" }
      }
      foreach ($field in @('pipelineErrors','continuousFailures','failedSaves','rejectedSaves')) {
        if ([double]$snapshot.metrics.$field -ne 0) { throw "Engine failure during experiment: $field" }
      }
    }
    if ([double]$Snapshots[-1].snapshot.metrics.videoPackets -le [double]$Snapshots[0].snapshot.metrics.videoPackets) { throw 'No video packet progress during measurement.' }
    if ($Mode -in @('continuous-only','combined','save-burst') -and [double]$Snapshots[-1].snapshot.metrics.continuousPackets -le [double]$Snapshots[0].snapshot.metrics.continuousPackets) { throw 'No continuous packet progress during measurement.' }
    $maximum = 0.0
    $metricIndex = 0
    foreach ($sample in $Samples) {
      while ($metricIndex+1 -lt $Snapshots.Count -and [double]$Snapshots[$metricIndex+1].qpc -le [double]$sample.qpc) { ++$metricIndex }
      $maximum = [Math]::Max($maximum, [Math]::Max(0,[double]$sample.privateBytes-[double]$Snapshots[$metricIndex].snapshot.metrics.replayBytes))
    }
    $withoutReplay = $maximum/1MB
  }
  $diagnostics=$null; $dropPercent=$null
  $unavailable=@('NVENC submitted/backpressure drops','NVENC latency','A/V drift')
  if ($Mode -ne 'game-only' -and $Snapshots.Count -ge 2 -and
      $null -ne $Snapshots[0].snapshot.metrics.PSObject.Properties['diagnostics'] -and
      $null -ne $Snapshots[-1].snapshot.metrics.PSObject.Properties['diagnostics']) {
    $first=$Snapshots[0].snapshot.metrics.diagnostics
    $last=$Snapshots[-1].snapshot.metrics.diagnostics
    foreach ($field in @('video_ticks','missed_video_deadlines','submitted_frames')) {
      foreach ($endpoint in @($first,$last)) {
        if ([string]::IsNullOrWhiteSpace([string]$endpoint.$field) -or
            -not [double]::IsFinite([double]$endpoint.$field) -or [double]$endpoint.$field -lt 0) { throw "Invalid diagnostic counter: $field" }
      }
      if ([double]$last.$field -lt [double]$first.$field) { throw "Diagnostic counter reset during measurement: $field" }
    }
    $ticks=[double]$last.video_ticks-[double]$first.video_ticks
    $missed=[double]$last.missed_video_deadlines-[double]$first.missed_video_deadlines
    $submitted=[double]$last.submitted_frames-[double]$first.submitted_frames
    if ($submitted -gt $ticks -or $ticks+$missed -le 0) { throw 'Diagnostic counters do not describe a valid measurement interval.' }
    $dropPercent=100*($missed+$ticks-$submitted)/($ticks+$missed)
    $diagnostics=$last
    $unavailable=@('A/V drift')
  }
  [pscustomobject]@{
    evidenceStatus='complete'; acceptanceStatus='not-evaluated'; sampledProcess=$(if ($Mode -eq 'game-only') {'game'} else {'engine'})
    cpuAveragePercent=([double]$Samples[-1].cpuTotalMs-[double]$Samples[0].cpuTotalMs)/($seconds*1000*[Environment]::ProcessorCount)*100
    maximumPrivateMemoryMiB=$maximumPrivate; workingMemoryExcludingReplayMiB=$withoutReplay
    maximumHandleCount=($Samples | ForEach-Object {[int]$_.handles} | Measure-Object -Maximum).Maximum
    encoderUtilizationAveragePercent=($encoderValues | Measure-Object -Average).Average
    encoderUtilizationScope='nvidia-smi first GPU, device-wide'; sampledSeconds=$seconds; sampleCount=$Samples.Count
    snapshotCount=$Snapshots.Count; droppedFramesPercent=$dropPercent
    pipelineDiagnostics=$diagnostics; diagnosticHistogramsScope='session cumulative including warmup'
    unavailableMetrics=$unavailable
  }
}

function Invoke-ShortExperimentCollection([object] $Plan) {
  foreach ($run in $Plan.runs) {
    Assert-ShortBuildUnchanged $Plan
    Invoke-ShortExperimentRun $Plan $run
    Assert-ShortBuildUnchanged $Plan
  }
}

function Assert-ShortBuildUnchanged([object] $Plan) {
  if ($null -eq $Plan.PSObject.Properties['buildIdentity']) { return }
  foreach ($binary in $Plan.buildIdentity.binaries) {
    if ((Get-FileHash -LiteralPath $binary.path -Algorithm SHA256).Hash -ne $binary.sha256) { throw 'Build binary changed after experiment planning.' }
  }
}

function Start-ShortProtocolProcess([string] $Engine, [string] $ConfigPath, [string] $Directory) {
  $start = [Diagnostics.ProcessStartInfo]::new()
  $start.FileName=$Engine; $start.UseShellExecute=$false; $start.CreateNoWindow=$true
  $start.RedirectStandardInput=$true; $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
  $start.StandardOutputEncoding=[Text.UTF8Encoding]::new($false)
  foreach ($argument in @('session-json','--config',$ConfigPath)) { $start.ArgumentList.Add($argument) }
  $process = [Diagnostics.Process]::new(); $process.StartInfo=$start
  $writer = [IO.StreamWriter]::new([IO.File]::Open((Join-Path $Directory 'session.ndjson'), [IO.FileMode]::CreateNew), [Text.UTF8Encoding]::new($false))
  $writer.AutoFlush=$true
  try {
    if (-not $process.Start()) { throw 'Engine session failed to start.' }
    [pscustomobject]@{
      process=$process; errors=$process.StandardError.ReadToEndAsync(); pending=$process.StandardOutput.ReadLineAsync()
      writer=$writer; records=[Collections.Generic.List[object]]::new(); responses=@{}; saved=@{}; sentCommands=@{}; sequence=0; latest=$null; eof=$false
    }
  } catch { $writer.Dispose(); $process.Dispose(); throw }
}

function Receive-ShortSession([object] $Session, [int] $WaitMilliseconds = 100) {
  if ($Session.eof) { throw 'Engine protocol stream has ended.' }
  if (-not $Session.pending.Wait($WaitMilliseconds)) { return }
  $line = $Session.pending.GetAwaiter().GetResult()
  if ($null -eq $line) { $Session.eof=$true; throw 'Engine exited before the experiment completed.' }
  # Save raw protocol output before parsing so malformed output remains diagnosable.
  $Session.writer.WriteLine($line)
  if ([Text.Encoding]::UTF8.GetByteCount($line) -gt 65536) { throw 'Engine protocol event exceeds 64 KiB.' }
  $event = $line | ConvertFrom-Json
  $record = [pscustomobject]@{qpc=[Diagnostics.Stopwatch]::GetTimestamp(); qpcFrequency=[Diagnostics.Stopwatch]::Frequency; event=$event}
  $Session.records.Add($record)
  $Session.pending=$Session.process.StandardOutput.ReadLineAsync()
  if ($event.protocolVersion -ne 1) { throw 'Unsupported engine protocol version.' }
  if ($null -ne $event.PSObject.Properties['snapshot']) { $Session.latest=$event.snapshot }
  if ($event.type -in @('fatal_error','error') -or ($null -ne $event.PSObject.Properties['error'] -and $null -ne $event.error)) { throw ('Engine protocol error: ' + $line) }
  if ($null -ne $event.PSObject.Properties['requestId']) {
    if ($event.type -eq 'command_result') { $Session.responses[$event.requestId]=$event }
    if ($event.type -eq 'clip_saved') { $Session.saved[$event.requestId]=$event }
  }
}

function Send-ShortCommand([object] $Session, [string] $Command, [switch] $NoWait) {
  if ($Session.process.HasExited) { throw 'Engine exited before command submission.' }
  ++$Session.sequence
  $id = 'experiment-' + $Session.sequence
  $Session.sentCommands[$id]=[pscustomobject]@{requestId=$id; command=$Command; submittedQpc=[Diagnostics.Stopwatch]::GetTimestamp(); qpcFrequency=[Diagnostics.Stopwatch]::Frequency}
  $Session.process.StandardInput.WriteLine((@{protocolVersion=1;type='command';requestId=$id;command=$Command} | ConvertTo-Json -Compress))
  $Session.process.StandardInput.Flush()
  if (-not $NoWait) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (-not $Session.responses.ContainsKey($id)) {
      if ($timer.Elapsed.TotalSeconds -gt 30) { throw "Engine command timed out: $Command" }
      Receive-ShortSession $Session
    }
  }
  return $id
}

function Start-ShortSession([object] $Plan, [object] $Run) {
  $config = Get-ShortRunConfiguration $Plan.sourceConfiguration $Run
  New-Item -ItemType Directory -Path $config.outputDirectory -ErrorAction Stop | Out-Null
  $configPath = Join-Path $Run.directory 'session-config.json'
  $config | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $configPath -Encoding utf8NoBOM
  $session = Start-ShortProtocolProcess $Plan.engine $configPath $Run.directory
  try {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($null -eq $session.latest) {
      if ($timer.Elapsed.TotalSeconds -gt 30) { throw 'Engine ready timed out.' }
      Receive-ShortSession $session
    }
    foreach ($command in @(Get-ShortModeCommands $Run.mode)) { [void](Send-ShortCommand $session $command) }
    $replayExpected = $Run.mode -ne 'continuous-only'
    $continuousExpected = $Run.mode -in @('continuous-only','combined','save-burst')
    if ($session.latest.lifecycle -ne 'ready' -or $session.latest.replayActive -ne $replayExpected -or $session.latest.continuousRecordingActive -ne $continuousExpected) { throw 'Engine did not enter the declared experiment mode.' }
    return $session
  } catch {
    try { Stop-ShortSession $session $Run } catch { Write-Warning "Session setup cleanup: $($_.Exception.Message)" }
    throw
  }
}

function Stop-ShortSession([object] $Session, [object] $Run) {
  $failure = $null
  try {
    if (-not $Session.process.HasExited) {
      try { [void](Send-ShortCommand $Session 'stop') } catch { $failure=$_.Exception.Message }
      $Session.process.StandardInput.Close()
      if (-not $Session.process.WaitForExit(10000)) {
        Stop-Process -Id $Session.process.Id -Force
        if (-not $Session.process.WaitForExit(5000)) { throw 'Engine remained alive after force-stop.' }
        $failure='Engine required force-stop.'
      }
    }
    # Preserve the unconsumed tail, including any final protocol failure.
    if (-not $Session.eof) {
      if (-not $Session.pending.Wait(5000)) { throw 'Engine stdout did not close after exit.' }
      $line=$Session.pending.GetAwaiter().GetResult()
      while ($null -ne $line) {
        $Session.writer.WriteLine($line)
        $event=$line | ConvertFrom-Json
        if ($event.type -in @('fatal_error','error') -or ($null -ne $event.PSObject.Properties['error'] -and $null -ne $event.error)) { $failure='Engine reported an error during shutdown.' }
        $tail=$Session.process.StandardOutput.ReadLineAsync()
        if (-not $tail.Wait(5000)) { throw 'Engine stdout tail did not close.' }
        $line=$tail.GetAwaiter().GetResult()
      }
    }
    if ($Session.process.ExitCode -ne 0) { $failure="Engine exited with code $($Session.process.ExitCode)." }
  } finally {
    if (-not $Session.process.HasExited) { Stop-Process -Id $Session.process.Id -Force; [void]$Session.process.WaitForExit(5000) }
    try {
      if (-not $Session.errors.Wait(5000)) { throw 'Engine stderr did not close after exit.' }
      $Session.errors.GetAwaiter().GetResult() | Set-Content -LiteralPath (Join-Path $Run.directory 'engine.stderr.log') -Encoding utf8NoBOM
    } finally {
      $Session.writer.Dispose()
      try {
        @($Session.sentCommands.Values) | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Run.directory 'sent-commands.json') -Encoding utf8NoBOM
        $Session.records | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $Run.directory 'received-events.json') -Encoding utf8NoBOM
      } finally { $Session.process.Dispose() }
    }
  }
  if ($null -ne $failure) { throw $failure }
}

function Assert-ShortSaveDeadlines([object] $Session, [double] $TimeoutSeconds) {
  $now = [Diagnostics.Stopwatch]::GetTimestamp()
  foreach ($command in $Session.sentCommands.Values) {
    if ($command.command -ne 'save_replay') { continue }
    $replies = @($Session.records | Where-Object { $_.event.type -eq 'clip_saved' -and $_.event.requestId -eq $command.requestId })
    if ($replies.Count -gt 1) { throw 'Save Replay has duplicate correlated completions.' }
    $end = if ($replies.Count -eq 1) { $replies[0].qpc } else { $now }
    $seconds = ($end - $command.submittedQpc) / $command.qpcFrequency
    if (-not [double]::IsFinite($seconds) -or $seconds -lt 0 -or $seconds -gt $TimeoutSeconds) {
      throw "Save Replay request $($command.requestId) exceeded its submission deadline."
    }
  }
}

function Measure-ShortFrameTimes([string] $Path, [double] $Seconds, [string] $PrimaryCsv = '') {
  $rows = @(Import-Csv -LiteralPath $Path)
  if ($rows.Count -lt 2) { throw 'Short frame evidence needs multiple timestamped presents.' }
  $groups = @($rows | Group-Object -Property {
    $process = $_.PSObject.Properties['ProcessID']; $chain = $_.PSObject.Properties['SwapChainAddress']
    "$(if ($null -ne $process) { $process.Value })/$(if ($null -ne $chain) { $chain.Value })"
  } | Sort-Object -Property @{Expression='Count';Descending=$true}, Name)
  $primary = @($groups[0].Group)
  $timestamps = @($primary | ForEach-Object {
    $value = Convert-ToInvariantDouble $_.TimeInSeconds
    if (-not [double]::IsFinite($value) -or $value -lt 0) { throw 'Invalid PresentMon trace timestamp.' }
    $value
  })
  $span = $timestamps[-1] - $timestamps[0]
  if ($span -lt $Seconds - [Math]::Max(2.0, $Seconds * 0.05) -or $span -gt $Seconds + 5) {
    throw 'PresentMon frame timestamps do not cover the timed measurement window.'
  }
  $intervalSum = 0.0
  for ($i=1; $i -lt $primary.Count; ++$i) {
    $interval = Convert-ToInvariantDouble $primary[$i].msBetweenPresents
    if (-not [double]::IsFinite($interval) -or $interval -lt 0 -or $timestamps[$i] -lt $timestamps[$i-1]) { throw 'Invalid PresentMon frame interval ordering.' }
    $intervalSum += $interval
  }
  if ([Math]::Abs($intervalSum - $span * 1000) -gt [Math]::Max(25.0, $span * 10)) {
    throw 'PresentMon frame rows are missing inside the primary swap-chain trace.'
  }
  $analysisPath = $Path
  if ($PrimaryCsv) {
    $stream = [IO.StreamWriter]::new([IO.File]::Open($PrimaryCsv,[IO.FileMode]::CreateNew),[Text.UTF8Encoding]::new($false))
    try { $primary | ConvertTo-Csv -NoTypeInformation | ForEach-Object { $stream.WriteLine($_) } } finally { $stream.Dispose() }
    $analysisPath = $PrimaryCsv
  } elseif ($groups.Count -ne 1) { throw 'Multiple swap chains require a separate primary-trace evidence path.' }
  $result = Measure-PresentMonCsv $analysisPath
  $result | Add-Member frameTraceSeconds $span
  $result | Add-Member primarySwapChain $groups[0].Name
  $result | Add-Member primaryTraceCsv $analysisPath
  return $result
}

function Wait-ShortPhase([object] $Session, [object] $GameProcess, [double] $Seconds,
                         [object] $Plan = $null, [object] $Run = $null, [object] $PresentMon = $null) {
  $timer = [Diagnostics.Stopwatch]::StartNew()
  $burstSent = $false
  while ($timer.Elapsed.TotalSeconds -lt $Seconds) {
    $GameProcess.Refresh()
    if ($GameProcess.HasExited) { throw 'Game exited during the experiment.' }
    if ($null -ne $Plan -and $null -ne $Plan.PSObject.Properties['verifyGameForeground'] -and $Plan.verifyGameForeground -and
        (Get-ShortForegroundProcessId) -ne $GameProcess.Id) { throw 'Game lost foreground during measurement; discard this run.' }
    if ($null -ne $PresentMon -and $PresentMon.HasExited) { throw 'PresentMon exited before the measurement ended.' }
    if ($null -ne $Session) {
      Receive-ShortSession $Session 100
      if ($null -ne $Plan -and $null -ne $Run -and $Run.mode -eq 'save-burst') {
        Assert-ShortSaveDeadlines $Session $Plan.saveCompletionTimeoutSeconds
      }
      if ($null -ne $Session.latest) {
        foreach ($field in @('pipelineErrors','continuousFailures','failedSaves','rejectedSaves')) {
          if ($Session.latest.metrics.$field -gt 0) { throw "Engine reported $field during the experiment." }
        }
      }
      if ($null -ne $Run -and $Run.mode -eq 'save-burst' -and -not $burstSent -and $timer.Elapsed.TotalSeconds -ge $Plan.burstOffsetSeconds) {
        foreach ($index in 1..$Plan.burstCount) { [void](Send-ShortCommand $Session 'save_replay' -NoWait) }
        $burstSent=$true
      }
    } else { Start-Sleep -Milliseconds 100 }
  }
}

function Start-ShortPresentMon([object] $Plan, [object] $Run) {
  Start-ArgumentProcess $Plan.presentMon @('--process_name',[IO.Path]::GetFileName($Plan.gameExecutable),'--v1_metrics','--output_file',$Run.csv,'--no_console_stats','--session_name',$Plan.sessionName)
}

function Stop-ShortPresentMon([object] $Process, [object] $Plan) {
  $failure=$null
  try {
    $terminator = Start-ArgumentProcess $Plan.presentMon @('--terminate_existing_session','--session_name',$Plan.sessionName)
    try {
      if (-not $terminator.WaitForExit(15000)) { Stop-Process -Id $terminator.Id -Force; $failure='PresentMon terminator timed out.' }
      elseif ($terminator.ExitCode -ne 0) { $failure='PresentMon terminator failed.' }
    } finally { $terminator.Dispose() }
  } catch { $failure=$_.Exception.Message }
  try {
    if (-not $Process.WaitForExit(15000)) { Stop-Process -Id $Process.Id -Force; [void]$Process.WaitForExit(5000); $failure='PresentMon required force-stop.' }
    elseif ($Process.ExitCode -ne 0) { $failure="PresentMon exited with code $($Process.ExitCode)." }
  } finally { $Process.Dispose() }
  if ($null -ne $failure) { throw $failure }
}

function Complete-ShortSampler([object] $Sampler, [object] $Run) {
  try {
    Stop-Job $Sampler -ErrorAction Stop
    $samples = @(Receive-Job $Sampler -ErrorAction Stop)
    $samples | Export-Csv -LiteralPath (Join-Path $Run.directory 'sampler.csv') -NoTypeInformation
    return $samples
  } finally { Remove-Job $Sampler -Force -ErrorAction SilentlyContinue }
}

function Invoke-ShortExperimentRun([object] $Plan, [object] $Run) {
  Assert-ShortInteractiveInput
  $session=$null; $presentMon=$null; $sampler=$null; $samples=@(); $failure=$null
  $result=[ordered]@{id=$Run.id; pair=$Run.pair; side=$Run.side; mode=$Run.mode; status='failed'; acceptanceStatus='not-evaluated'; failure=$null}
  try {
    $game = Wait-ForGame $Plan.gameExecutable $GameWaitMinutes
    [void](Read-Host "Prepare $($Run.id): $($Plan.sceneDeclaration), $($Plan.settingsDeclaration). Press Enter to begin 60-second warmup, followed by $($Plan.measurementSeconds)-second measurement")
    if ($Run.mode -ne 'game-only') { $session = Start-ShortSession $Plan $Run }
    Wait-ShortPhase $session $game $Plan.warmupSeconds
    Wait-ShortSceneGate $session $game "Warmup finished. Prepare the same declared measurement scene for $($Run.id). Press Enter immediately before starting the $($Plan.measurementSeconds)-second scene"
    # Enter may come from a foreground terminal. Allow a bounded handoff before
    # starting samplers/timing; never grant a grace period inside measurement.
    if ($Plan.verifyGameForeground) {
      Write-Host 'Return to the game within 30 seconds; measurement waits for its foreground window.'
      [void](Get-ShortForegroundProcessId)
      Wait-ShortGameForeground $session $game
    }
    if ($null -ne $session) { [void](Send-ShortCommand $session 'get_snapshot') }
    $targetId = if ($null -eq $session) { $game.Id } else { $session.process.Id }
    $nvidiaSmi = (Get-Command nvidia-smi.exe -ErrorAction Stop).Source
    $sampler = Start-EngineSampler $targetId $nvidiaSmi
    $presentMon = Start-ShortPresentMon $Plan $Run
    $result.measurementStartQpc=[Diagnostics.Stopwatch]::GetTimestamp()
    Wait-ShortPhase $session $game $Plan.measurementSeconds $Plan $Run $presentMon
    $result.measurementEndQpc=[Diagnostics.Stopwatch]::GetTimestamp()
    $finishedPresentMon=$presentMon; $presentMon=$null
    Stop-ShortPresentMon $finishedPresentMon $Plan
    if ($Run.mode -eq 'save-burst') {
      $timer=[Diagnostics.Stopwatch]::StartNew()
      while ($session.saved.Count -lt $Plan.burstCount) {
        if ($timer.Elapsed.TotalSeconds -gt $Plan.saveCompletionTimeoutSeconds) { throw 'Save Replay burst completion timed out.' }
        Receive-ShortSession $session
        Assert-ShortSaveDeadlines $session $Plan.saveCompletionTimeoutSeconds
      }
      Assert-ShortSaveDeadlines $session $Plan.saveCompletionTimeoutSeconds
      foreach ($saved in $session.saved.Values) {
        if (-not (Test-Path -LiteralPath $saved.outputPath -PathType Leaf) -or (Get-Item -LiteralPath $saved.outputPath).Length -eq 0) { throw 'Save burst output is missing or empty.' }
      }
      $result.savedClips=@($session.saved.Values | ForEach-Object { $_.outputPath })
      $result.saveBurst=@($session.sentCommands.Values | Where-Object { $_.command -eq 'save_replay' } | Sort-Object submittedQpc | ForEach-Object {
        $command=$_
        $completion=@($session.records | Where-Object { $_.event.type -eq 'clip_saved' -and $_.event.requestId -eq $command.requestId })
        if ($completion.Count -ne 1) { throw 'Save burst command lacks one correlated completion.' }
        [pscustomobject]@{requestId=$command.requestId; submittedQpc=$command.submittedQpc; completedQpc=$completion[0].qpc; completionSeconds=($completion[0].qpc-$command.submittedQpc)/$command.qpcFrequency; outputPath=$completion[0].event.outputPath}
      })
      if ($result.saveBurst.Count -ne $Plan.burstCount -or @($result.savedClips | Sort-Object -Unique).Count -ne $Plan.burstCount) { throw 'Save burst requires distinct outputs and a completion for every submitted request.' }
    }
    $result.status='collected'
  } catch { $failure=$_.Exception.Message } finally {
    # Each cleanup is attempted even if another cleanup fails.
    if ($null -ne $presentMon) { try { Stop-ShortPresentMon $presentMon $Plan } catch { $failure="${failure} PresentMon cleanup: $($_.Exception.Message)" } }
    if ($null -ne $sampler) { try { $samples=@(Complete-ShortSampler $sampler $Run) } catch { $failure="${failure} Sampler cleanup: $($_.Exception.Message)" } }
    if ($null -ne $session) { try { Stop-ShortSession $session $Run } catch { $failure="${failure} Engine cleanup: $($_.Exception.Message)" } }
  }
  if ($null -eq $failure) {
    try {
      $windowSamples=@($samples | Where-Object { [double]$_.qpc -ge $result.measurementStartQpc -and [double]$_.qpc -le $result.measurementEndQpc })
      $snapshots=@()
      if ($null -ne $session) {
        $snapshots=@($session.records | Where-Object {
          $_.qpc -ge $result.measurementStartQpc -and $_.qpc -le $result.measurementEndQpc -and $null -ne $_.event.PSObject.Properties['snapshot']
        } | ForEach-Object { [pscustomobject]@{qpc=$_.qpc; snapshot=$_.event.snapshot} })
      }
      $result.resources=Measure-ShortRunResources $windowSamples $snapshots $Run.mode $Plan.measurementSeconds
      $result.frameTimes=Measure-ShortFrameTimes $Run.csv $Plan.measurementSeconds (Join-Path $Run.directory 'presentmon-primary.csv')
      $result.status='measured'
    } catch { $failure=$_.Exception.Message }
  }
  if ($null -ne $failure) { $result.status='failed'; $result.failure=$failure }
  $result | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $Run.directory 'result.json') -Encoding utf8NoBOM
  if ($null -ne $failure) { throw $failure }
  return [pscustomobject]$result
}

function Invoke-ShortExperimentCommand([object] $Plan, [string] $Repository) {
  Assert-ShortInteractiveInput
  foreach ($file in @($Plan.gameExecutable,$Plan.engine,$Plan.presentMon)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required experiment executable not found: $file" }
  }
  $existingEngines=@(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($Plan.engine)) -ErrorAction SilentlyContinue)
  if ($existingEngines.Count -ne 0) { throw 'Close existing recorder engine sessions before collecting game-only baselines.' }
  [void](Get-Command nvidia-smi.exe -ErrorAction Stop)
  $versionLine=([string](& $Plan.presentMon --help 2>&1 | Select-Object -First 1)).Trim()
  if ($versionLine -ne 'PresentMon 2.5.1') { throw "PresentMon 2.5.1 is required, found: $versionLine" }
  $configuration=@{}
  if ($Plan.mode -ne 'game-only') {
    if ([string]::IsNullOrWhiteSpace($Plan.configPath)) { throw 'Recorder experiments require -ExperimentConfig with a validated session-json host configuration.' }
    $configuration=Get-Content -Raw -LiteralPath $Plan.configPath | ConvertFrom-Json -AsHashtable
    if ($configuration.protocolVersion -ne 1 -or [string]::IsNullOrWhiteSpace($configuration.monitorId)) { throw 'Host configuration requires protocolVersion=1 and an explicit monitorId.' }
    foreach ($field in @('width','height','fps','bitrate','replaySeconds','clipSeconds','systemAudioEnabled','systemAudioId','microphoneEnabled','microphoneId','saveReplayHotkey','toggleRecordingHotkey','container','outputDirectory','continuousRecordingEnabled')) {
      if (-not $configuration.Contains($field)) { throw "Host configuration is missing $field." }
    }
    if ($configuration.replaySeconds -gt $Plan.warmupSeconds) { throw 'Short experiments require replaySeconds <= 60 so the fixed warmup fills the Replay buffer.' }
  }
  # Identity is captured before adding any evidence files to the source tree.
  $identity=Get-PerformanceBuildIdentity $Repository $Plan.engine
  New-ShortExperimentEvidence $Plan
  $Plan | Add-Member -NotePropertyName buildIdentity -NotePropertyValue $identity
  $Plan | Add-Member -NotePropertyName sourceConfiguration -NotePropertyValue $configuration
  $archive=New-PerformanceSourceArchive $Repository (Join-Path $Plan.outputRoot 'source.zip')
  $Plan.buildIdentity | Add-Member -NotePropertyName sourceArchive -NotePropertyValue $archive
  $Plan | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $Plan.outputRoot 'plan.json') -Encoding utf8NoBOM
  $results=[Collections.Generic.List[object]]::new()
  $failure=$null
  try {
    Invoke-ShortExperimentCollection $Plan | ForEach-Object { $results.Add($_) }
  } catch { $failure=$_.Exception.Message }
  $summary=[pscustomobject]@{
    schemaVersion=1; status=$(if ($null -eq $failure) {'measured'} else {'failed'})
    acceptanceStatus='not-evaluated'; mode=$Plan.mode; buildIdentity=$identity
    warmupSeconds=$Plan.warmupSeconds; measurementSeconds=$Plan.measurementSeconds
    requestedPairs=$Plan.pairs; completedRuns=$results.Count; runs=@($results)
    failure=$failure; limitation=$Plan.limitation
  }
  $summary | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath (Join-Path $Plan.outputRoot 'summary.json') -Encoding utf8NoBOM
  if ($null -ne $failure) { throw $failure }
  return $summary
}
