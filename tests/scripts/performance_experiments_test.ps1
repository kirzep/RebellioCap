Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$helper = Join-Path $repositoryRoot 'scripts/performance-experiments.ps1'
if (-not (Test-Path -LiteralPath $helper)) { throw 'Short experiment tooling is missing.' }
. $helper
function Assert($condition, [string] $message) { if (-not $condition) { throw $message } }
function Reject([scriptblock] $action, [string] $message) {
  $rejected = $false
  try { & $action | Out-Null } catch { $rejected = $true }
  Assert $rejected $message
}
Reject { Assert-ShortInteractiveInput -InputRedirected $true } 'Closed or redirected stdin must not skip the scene gates.'
Assert-ShortInteractiveInput -InputRedirected $false
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('performance-experiments-' + [guid]::NewGuid().ToString('N'))
try {
  $plan = Get-ShortExperimentPlan -Mode combined -OutputRoot $fixture -Settings 'Ultra; HAGS off; VRR off' -Scene 'fixture' -Game 'C:/fixture/game.exe' -ConfigPath '' -Seconds 60 -Pairs 3 -Repository $repositoryRoot
  Assert ($plan.warmupSeconds -eq 60 -and $plan.runs.Count -eq 6) 'Short experiments require 60s warmup and three pairs.'
  Assert (($plan.runs.mode -join ',') -eq 'game-only,combined,game-only,combined,game-only,combined') 'Short experiments must alternate baseline and mode.'
  Assert (($plan.runs.directory | Sort-Object -Unique).Count -eq 6) 'Each run needs its own evidence directory.'
  foreach ($mode in @('game-only','replay-only','continuous-only','combined','save-burst')) {
    $modePlan = Get-ShortExperimentPlan $mode $fixture 'fixture' 'fixture' 'C:/fixture/game.exe' '' 60 4 $repositoryRoot
    Assert ($modePlan.runs.Count -eq 8 -and $modePlan.runs[-1].mode -eq $mode) "Mode missing: $mode"
  }
  Reject { Get-ShortExperimentPlan combined $fixture '' scene game '' 60 3 $repositoryRoot } 'Settings must be explicit.'
  Reject { Get-ShortExperimentPlan combined $fixture settings scene game '' 60 2 $repositoryRoot } 'Fewer than three pairs must fail.'
  Reject { Get-ShortExperimentPlan unknown $fixture settings scene game '' 60 3 $repositoryRoot } 'Unknown modes must fail.'
  Assert (-not (Test-Path -LiteralPath $fixture)) 'Planning must be offline and not create evidence.'
  New-ShortExperimentEvidence $plan
  [IO.File]::WriteAllText((Join-Path $fixture 'sentinel'), 'preserve')
  Reject { New-ShortExperimentEvidence $plan } 'Existing evidence root must never be reused.'
  Assert ((Get-Content -Raw -LiteralPath (Join-Path $fixture 'sentinel')) -eq 'preserve') 'Collision changed existing evidence.'

  $configuration = @{ protocolVersion=1; continuousRecordingEnabled=$true; outputDirectory='C:/old'; monitorId='fixture'; width=1920; height=1080; fps=60; bitrate=30000000; replaySeconds=30; clipSeconds=30 }
  $config = Get-ShortRunConfiguration $configuration $plan.runs[1]
  Assert (-not $config.continuousRecordingEnabled -and $config.outputDirectory -eq (Join-Path $plan.runs[1].directory 'clips')) 'Config must start without continuous capture and isolate outputs.'
  Assert ($configuration.outputDirectory -eq 'C:/old' -and $configuration.continuousRecordingEnabled) 'Config input was mutated.'
  Assert ((Get-ShortModeCommands 'continuous-only') -join ',' -eq 'stop_replay,toggle_recording') 'Continuous-only must disable Replay.'
  Assert (@(Get-ShortModeCommands 'replay-only').Count -eq 0) 'Replay-only must preserve initial Replay.'
  Assert ((Get-ShortModeCommands 'save-burst') -join ',' -eq 'toggle_recording') 'Burst must run alongside combined capture.'

  $script:events = [Collections.Generic.List[string]]::new()
  function Invoke-ShortExperimentRun($plan, $run) {
    $script:events.Add($run.id)
    if ($run.id -eq $plan.runs[1].id) { throw 'injected failure' }
    [pscustomobject]@{ id=$run.id }
  }
  Reject { Invoke-ShortExperimentCollection $plan } 'Run failure must abort the collection.'
  Assert ($script:events.Count -eq 2) 'A failed run must not permit the next baseline.'
  $samples = @(
    [pscustomobject]@{qpc=1000; qpcFrequency=1000; cpuTotalMs=0; privateBytes=100MB; handles=80; encoderUtilizationPercent=4},
    [pscustomobject]@{qpc=2000; qpcFrequency=1000; cpuTotalMs=10; privateBytes=110MB; handles=90; encoderUtilizationPercent=6}
  )
  $snapshots = @(
    [pscustomobject]@{qpc=1000; snapshot=[pscustomobject]@{replayActive=$true; continuousRecordingActive=$true; metrics=[pscustomobject]@{replayBytes=10MB; videoPackets=1; continuousPackets=1; pipelineErrors=0; continuousFailures=0; failedSaves=0; rejectedSaves=0}}},
    [pscustomobject]@{qpc=2000; snapshot=[pscustomobject]@{replayActive=$true; continuousRecordingActive=$true; metrics=[pscustomobject]@{replayBytes=20MB; videoPackets=2; continuousPackets=2; pipelineErrors=0; continuousFailures=0; failedSaves=0; rejectedSaves=0}}}
  )
  $resources = Measure-ShortRunResources $samples $snapshots combined 1
  Assert ($resources.workingMemoryExcludingReplayMiB -eq 90 -and $resources.maximumHandleCount -eq 90) 'Resources must use their own matched run metrics.'
  Assert ($resources.droppedFramesPercent -eq $null -and $resources.acceptanceStatus -eq 'not-evaluated') 'Unavailable session drops must not become a passing gate.'
  $snapshots[0].snapshot.metrics | Add-Member diagnostics ([pscustomobject]@{video_ticks=60;missed_video_deadlines=1;submitted_frames=59;encode_latency_semantics='cpu_submit_to_bitstream_collection'})
  $snapshots[1].snapshot.metrics | Add-Member diagnostics ([pscustomobject]@{video_ticks=120;missed_video_deadlines=2;submitted_frames=117;encode_latency_semantics='cpu_submit_to_bitstream_collection'})
  $measuredResources=Measure-ShortRunResources $samples $snapshots combined 1
  Assert ([Math]::Abs($measuredResources.droppedFramesPercent - 300.0/61) -lt 0.0001) 'Diagnostic drop ratios must subtract the warmup counters and avoid double-counting backpressure.'
  Assert ($measuredResources.pipelineDiagnostics.submitted_frames -eq 117) 'Keep the cumulative endpoint diagnostics and their latency semantics.'
  $snapshots[0].snapshot.metrics.PSObject.Properties.Remove('diagnostics')
  $snapshots[1].snapshot.metrics.PSObject.Properties.Remove('diagnostics')
  $gameResources = Measure-ShortRunResources $samples @() game-only 1
  Assert ($gameResources.sampledProcess -eq 'game' -and $gameResources.maximumPrivateMemoryMiB -eq 110) 'Game-only must collect its own game resources.'
  Reject { Measure-ShortRunResources @() $snapshots combined 1 } 'Missing samples must fail.'
  Reject { Measure-ShortRunResources $samples @() combined 1 } 'Missing snapshots must fail.'
  Reject { Measure-ShortRunResources $samples $snapshots combined 60 } 'Truncated data must fail.'
  $snapshots[1].qpc='NaN'
  Reject { Measure-ShortRunResources $samples $snapshots combined 1 } 'Nonfinite snapshot clocks must fail.'
  $snapshots[1].qpc=2000
  $snapshots[1].snapshot.metrics.replayBytes=$null
  Reject { Measure-ShortRunResources $samples $snapshots combined 1 } 'Null engine resource metrics must fail.'
  $snapshots[1].snapshot.metrics.replayBytes=20MB
  $snapshots[1].snapshot.replayActive=$false
  Reject { Measure-ShortRunResources $samples $snapshots combined 1 } 'Wrong mode must fail.'
  $snapshots[1].snapshot.replayActive=$true
  $snapshots[1].snapshot.metrics.pipelineErrors=1
  Reject { Measure-ShortRunResources $samples $snapshots combined 1 } 'Engine errors must fail.'
  $samples[1].cpuTotalMs='NaN'
  Reject { Measure-ShortRunResources $samples @() game-only 1 } 'Nonfinite resources must fail.'
} finally {
  $resolved = [IO.Path]::GetFullPath($fixture)
  if ($resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Leaf $resolved).StartsWith('performance-experiments-')) {
    Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
  }
}
Write-Output 'performance short experiment plans, modes, evidence and resource fixtures passed'


# Exercise the actual run lifecycle with all process/UI boundaries replaced.
. $helper
$script:lifecycle = [Collections.Generic.List[string]]::new()
$GameWaitMinutes = 10
$script:fakeSession = [pscustomobject]@{ process=[pscustomobject]@{Id=123}; records=[Collections.Generic.List[object]]::new(); saved=@{} }
function Wait-ForGame { [pscustomobject]@{Id=123;HasExited=$false} }
function Assert-ShortInteractiveInput {}
function Read-Host { 'fixture' }
function Wait-ShortSceneGate { $script:lifecycle.Add('scene-gate') }
function Wait-ShortGameForeground { $script:lifecycle.Add('focus-handoff') }
function Start-ShortSession { $script:lifecycle.Add('session-start'); $script:fakeSession }
function Wait-ShortPhase { $script:lifecycle.Add('warmup') }
function Send-ShortCommand { 'fixture-command-id' }
function Start-EngineSampler { $script:lifecycle.Add('sampler-start'); [pscustomobject]@{Id=42} }
function Start-ShortPresentMon { $script:lifecycle.Add('presentmon-start'); throw 'injected PM failure' }
function Stop-ShortPresentMon { $script:lifecycle.Add('presentmon-stop') }
function Stop-ShortSession { $script:lifecycle.Add('session-stop') }
function Complete-ShortSampler { $script:lifecycle.Add('sampler-stop'); @() }
function Get-Command { [pscustomobject]@{Source='fixture-smi'} }
$failureRoot = Join-Path ([IO.Path]::GetTempPath()) ('performance-experiments-' + [guid]::NewGuid().ToString('N'))
try {
  $plan = Get-ShortExperimentPlan combined $failureRoot settings scene 'C:/fixture/game.exe' '' 60 3 $repositoryRoot
  New-ShortExperimentEvidence $plan
  $plan | Add-Member -NotePropertyName sourceConfiguration -NotePropertyValue @{}
  Reject { Invoke-ShortExperimentRun $plan $plan.runs[1] } 'Measurement startup failure must reject the run.'
  Assert (($script:lifecycle -join ',') -eq 'session-start,warmup,scene-gate,focus-handoff,sampler-start,presentmon-start,sampler-stop,session-stop') 'Failure must wait for game focus, then stop both sampler and engine before leaving the run.'
  $failure = Get-Content -Raw -LiteralPath (Join-Path $plan.runs[1].directory 'result.json') | ConvertFrom-Json
  Assert ($failure.status -eq 'failed' -and $failure.failure -match 'injected PM failure') 'Failure must retain an explicit failed evidence record.'
} finally {
  if ([IO.Path]::GetFullPath($failureRoot).StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Leaf $failureRoot).StartsWith('performance-experiments-')) {
    Remove-Item -LiteralPath $failureRoot -Recurse -Force -ErrorAction SilentlyContinue
  }
}
Write-Output 'performance short experiment failure cleanup passed'


# The public short PlanOnly entry point must work without a game, engine or monitor query.
$offlineRoot = Join-Path ([IO.Path]::GetTempPath()) ('performance-experiments-' + [guid]::NewGuid().ToString('N'))
$offlineJson = & (Join-Path $repositoryRoot 'scripts/collect-performance.ps1') -PlanOnly -ExperimentMode continuous-only -ExperimentOutputRoot $offlineRoot -GameExecutable 'C:/not-installed/game.exe' -SettingsDeclaration 'offline fixture'
Assert ($LASTEXITCODE -eq 0) 'Offline short PlanOnly failed.'
$offline = $offlineJson | ConvertFrom-Json
Assert ($offline.status -eq 'planned-unmeasured' -and $offline.runs.Count -eq 6) 'Offline short plan is missing its paired design.'
Assert (-not (Test-Path -LiteralPath $offlineRoot)) 'Offline PlanOnly wrote evidence.'
Write-Output 'performance short offline CLI plan passed'

# Protocol boundaries are exercised with in-memory streams; no executable is started.
. $helper
$script:protocolInput=[IO.StringWriter]::new()
$script:protocolOutput=[IO.StringWriter]::new()
$protocol=[pscustomobject]@{
  process=[pscustomobject]@{HasExited=$false; StandardInput=$script:protocolInput; StandardOutput=[IO.StringReader]::new('')}
  pending=[Threading.Tasks.Task]::FromResult('{"protocolVersion":1,"type":"command_result","requestId":"experiment-1","snapshot":{"lifecycle":"ready"}}')
  writer=$script:protocolOutput; records=[Collections.Generic.List[object]]::new(); responses=@{}; saved=@{}; sentCommands=@{}; sequence=0; latest=$null; eof=$false
}
Receive-ShortSession $protocol 0
Assert ($protocol.responses.ContainsKey('experiment-1') -and $protocol.latest.lifecycle -eq 'ready') 'Protocol must correlate command results and retain authoritative snapshots.'
$id=Send-ShortCommand $protocol save_replay -NoWait
Assert ($id -eq 'experiment-1' -and $protocol.sentCommands.ContainsKey($id)) 'Save commands need correlated QPC submission evidence.'
$request=$script:protocolInput.ToString().Trim() | ConvertFrom-Json
Assert ($request.command -eq 'save_replay' -and $request.protocolVersion -eq 1) 'Session command envelope is invalid.'
$protocol.pending=[Threading.Tasks.Task]::FromResult('{"protocolVersion":1,"type":"command_result","requestId":"experiment-1","error":{"code":"fixture"}}')
Reject { Receive-ShortSession $protocol 0 } 'A correlated command error must fail immediately.'
Assert ($script:protocolOutput.ToString() -match 'fixture') 'Protocol failure must preserve raw output.'
Write-Output 'performance short protocol correlation passed'

# A burst submits three distinct commands together; it does not serialize on completion.
. $helper
function Receive-ShortSession { Start-Sleep -Milliseconds 10 }
$burstInput=[IO.StringWriter]::new()
$burstSession=[pscustomobject]@{
  process=[pscustomobject]@{HasExited=$false;StandardInput=$burstInput}
  sentCommands=@{};sequence=0
  latest=[pscustomobject]@{metrics=[pscustomobject]@{pipelineErrors=0;continuousFailures=0;failedSaves=0;rejectedSaves=0}}
  records=@()
}
$fakeGame=[pscustomobject]@{HasExited=$false}
$fakeGame | Add-Member ScriptMethod Refresh {}
Wait-ShortPhase $burstSession $fakeGame 0.06 ([pscustomobject]@{burstOffsetSeconds=0;burstCount=3;saveCompletionTimeoutSeconds=30}) ([pscustomobject]@{mode='save-burst'})
Assert ($burstSession.sentCommands.Count -eq 3 -and ($burstSession.sentCommands.Values.command | Sort-Object -Unique) -eq 'save_replay') 'Burst must submit exactly three independent replay saves.'
Assert (($burstInput.ToString().Trim() -split '\r?\n').Count -eq 3) 'Burst must not await each completion before submitting the next save.'
Write-Output 'performance short burst submission passed'

# Failed graceful shutdown still waits, preserves stderr/evidence and disposes the child.
. $helper
function Send-ShortCommand { throw 'injected stop command failure' }
$cleanupRoot=Join-Path ([IO.Path]::GetTempPath()) ('performance-experiments-' + [guid]::NewGuid().ToString('N'))
try {
  New-Item -ItemType Directory -Path $cleanupRoot | Out-Null
  $script:disposed=$false
  $fakeProcess=[pscustomobject]@{HasExited=$false;ExitCode=0;StandardInput=[IO.StringWriter]::new()}
  $fakeProcess | Add-Member ScriptMethod WaitForExit { param($milliseconds) $this.HasExited=$true; return $true }
  $fakeProcess | Add-Member ScriptMethod Dispose { $script:disposed=$true }
  $eof=[Threading.Tasks.TaskCompletionSource[string]]::new();$eof.SetResult($null)
  $cleanupSession=[pscustomobject]@{
    process=$fakeProcess; eof=$false; pending=$eof.Task; writer=[IO.StringWriter]::new()
    errors=[Threading.Tasks.Task]::FromResult('fixture stderr');sentCommands=@{};records=[Collections.Generic.List[object]]::new()
  }
  Reject { Stop-ShortSession $cleanupSession ([pscustomobject]@{directory=$cleanupRoot}) } 'Failed graceful stop must reject the run.'
  Assert ($script:disposed -and $fakeProcess.HasExited) 'Failed stop must still reap and dispose the engine.'
  Assert ((Get-Content -Raw -LiteralPath (Join-Path $cleanupRoot 'engine.stderr.log')) -match 'fixture stderr') 'Failed stop must preserve stderr.'
} finally {
  if ([IO.Path]::GetFullPath($cleanupRoot).StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Leaf $cleanupRoot).StartsWith('performance-experiments-')) {
    Remove-Item -LiteralPath $cleanupRoot -Recurse -Force -ErrorAction SilentlyContinue
  }
}
Write-Output 'performance short shutdown preservation passed'

. $helper
$frequency = [Diagnostics.Stopwatch]::Frequency
$now = [Diagnostics.Stopwatch]::GetTimestamp()
$lateSession = [pscustomobject]@{sentCommands=@{one=[pscustomobject]@{command='save_replay';requestId='one';submittedQpc=$now-40*$frequency;qpcFrequency=$frequency}};records=@()}
Reject { Assert-ShortSaveDeadlines $lateSession 30 } 'A pending save must expire from its own submission time.'
$lateSession.records=@([pscustomobject]@{qpc=$now;event=[pscustomobject]@{type='clip_saved';requestId='one';outputPath='fixture'}})
Reject { Assert-ShortSaveDeadlines $lateSession 30 } 'A correlated late reply must still fail its save deadline.'
$lateSession.sentCommands.one.submittedQpc=$now-2*$frequency
Assert-ShortSaveDeadlines $lateSession 30

$coverageCsv=Join-Path ([IO.Path]::GetTempPath()) ('performance-coverage-'+[guid]::NewGuid()+'.csv')
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $repositoryRoot 'scripts/collect-performance.ps1'),[ref]$tokens,[ref]$errors)
foreach ($name in @('Convert-ToInvariantDouble','Get-Percentile','Measure-PresentMonCsv')) {
  $function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
  . ([scriptblock]::Create($function.Extent.Text))
}
try {
  @('msBetweenPresents,Dropped,TimeInSeconds','16,0,0','16,0,1') | Set-Content $coverageCsv
  Reject { Measure-ShortFrameTimes $coverageCsv 60 } 'One second of frame evidence cannot describe a 60-second run.'
  @('msBetweenPresents,Dropped,TimeInSeconds') + @(0..60 | ForEach-Object { "1000,0,$_" }) | Set-Content $coverageCsv
  Assert ((Measure-ShortFrameTimes $coverageCsv 60).frameTraceSeconds -eq 60) 'Frame coverage must use trace timestamps and complete per-chain intervals.'
  @('msBetweenPresents,Dropped,TimeInSeconds','16,0,0','16,0,59.5') | Set-Content $coverageCsv
  Reject { Measure-ShortFrameTimes $coverageCsv 60 } 'Timestamp span alone must not hide missing per-chain rows.'
  @('msBetweenPresents,Dropped,TimeInSeconds,ProcessID,SwapChainAddress') + @(0..60 | ForEach-Object { "1000,0,$_,1,A" }) + @('16,0,0,1,B','60000,0,60,1,B') | Set-Content $coverageCsv
  $primaryCsv=$coverageCsv+'.primary.csv'
  $primary=Measure-ShortFrameTimes $coverageCsv 60 $primaryCsv
  Assert ($primary.displayedFrames -eq 61 -and $primary.primarySwapChain -eq '1/A') 'Multiple swap chains must not be combined into fake FPS.'
  Assert (@(Import-Csv $coverageCsv).Count -eq 63) 'Raw PresentMon evidence must remain intact.'
  Reject { Measure-ShortFrameTimes $coverageCsv 60 $primaryCsv } 'Primary evidence must not overwrite an existing file.'
} finally {
  Remove-Item -LiteralPath $coverageCsv,($coverageCsv+'.primary.csv') -ErrorAction SilentlyContinue
}
$nonignored=Join-Path $repositoryRoot ('unignored-experiment-'+[guid]::NewGuid())
Reject { Get-ShortExperimentPlan combined $nonignored 'settings' 'scene' 'C:/game.exe' '' 60 3 $repositoryRoot } 'Nonignored evidence inside the repository must not enter future archives.'
Write-Output 'performance short deadline coverage and archive isolation passed'

# An arbitrarily long human scene gate must not stop protocol/error servicing.
. $helper
$script:gatePolls=0;$script:gatePumps=0
$gateSession=[pscustomobject]@{identity='scene-gate'}
$gateGame=[pscustomobject]@{HasExited=$false}
function Test-ShortGateEnter { ++$script:gatePolls -ge 3 }
function Wait-ShortPhase {
 param($Session,$GameProcess,$Seconds)
 Assert ([object]::ReferenceEquals($Session,$gateSession)) 'Gate must pump its running session.'
 Assert ([object]::ReferenceEquals($GameProcess,$gateGame)) 'Gate must check its game.'
 Assert ($Seconds -gt 0 -and $Seconds -le .1) 'Gate servicing must stay responsive.'
 ++$script:gatePumps
}
Wait-ShortSceneGate $gateSession $gateGame 'fixture scene gate'
Assert ($script:gatePumps -eq 2) 'Human scene gate blocked protocol pumping before Enter.'
function Test-ShortGateEnter { $false }
function Wait-ShortPhase { throw 'injected scene-gate engine failure' }
Reject { Wait-ShortSceneGate $gateSession $gateGame 'fixture failure' } 'Engine failure must escape while waiting for human input.'
Write-Output 'performance short scene gate pumping passed'

. $helper
$foregroundGame=[pscustomobject]@{Id=123;HasExited=$false}
$foregroundGame|Add-Member ScriptMethod Refresh {}
$foregroundPlan=[pscustomobject]@{verifyGameForeground=$true}
function Get-ShortForegroundProcessId { 456 }
Reject { Wait-ShortPhase $null $foregroundGame .1 $foregroundPlan } 'A game that lost foreground must not produce accepted measurement evidence.'
function Get-ShortForegroundProcessId { 123 }
Wait-ShortPhase $null $foregroundGame .1 $foregroundPlan
Write-Output 'performance short foreground measurement guard passed'

. $helper
$script:handoffPolls=0;$script:handoffPumps=0
function Get-ShortForegroundProcessId { if(++$script:handoffPolls -lt 3){456}else{123} }
function Wait-ShortPhase { ++$script:handoffPumps }
Wait-ShortGameForeground $null $foregroundGame
Assert ($script:handoffPumps -eq 2) 'Console-to-game handoff must service the session until focus arrives.'
function Get-ShortForegroundProcessId { 456 }
function Wait-ShortPhase { throw 'injected handoff engine failure' }
Reject { Wait-ShortGameForeground $null $foregroundGame } 'Handoff must propagate engine/game failure.'
. $helper
function Get-ShortForegroundProcessId { 456 }
Reject { Wait-ShortGameForeground $null $foregroundGame .01 } 'Focus handoff must time out instead of waiting forever.'
Write-Output 'performance short foreground handoff passed'
