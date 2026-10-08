param([Parameter(Mandatory)][string] $Engine)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Windows.Forms
$Engine=(Resolve-Path -LiteralPath $Engine).Path
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('RebellioCap-session-'+[guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $testRoot)
$children=New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$originalCursorPosition=$null
function Assert($condition,[string] $message) { if(-not $condition) { throw $message } }
function Start-Engine([string] $mode,[string] $config='') {
  $info=New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName=$Engine
  $info.Arguments=$mode
  if($config) { $info.Arguments+=' --config "'+$config+'"' }
  $info.UseShellExecute=$false
  $info.CreateNoWindow=$true
  $info.RedirectStandardInput=$true
  $info.RedirectStandardOutput=$true
  $info.RedirectStandardError=$true
  $info.StandardOutputEncoding=New-Object System.Text.UTF8Encoding($false)
  $process=New-Object System.Diagnostics.Process
  $process.StartInfo=$info
  [void]$process.Start()
  $children.Add($process)
  return @{ Process=$process; Errors=$process.StandardError.ReadToEndAsync() }
}
function Read-Event($child) {
  $read=$child.Process.StandardOutput.ReadLineAsync()
  Assert ($read.Wait(20000)) 'Engine event timed out'
  $line=$read.Result
  Assert ($null -ne $line) 'Engine exited before expected event'
  Assert ([Text.Encoding]::UTF8.GetByteCount($line) -le 65536) 'Unbounded protocol output'
  $event=ConvertFrom-Json -InputObject $line
  Assert ($event.protocolVersion -eq 1) 'Protocol version mismatch'
  return $event
}
function Until($child,[string] $type,[string] $id='') {
  for($i=0;$i -lt 100;$i++) {
    $event=Read-Event $child
    if($event.type -eq 'fatal_error') { throw ($event | ConvertTo-Json -Depth 8 -Compress) }
    if($event.type -eq 'error' -and $type -ne 'error') { throw ($event | ConvertTo-Json -Depth 8 -Compress) }
    if($event.type -eq $type -and (-not $id -or $event.requestId -eq $id)) { return $event }
  }
  throw "Did not observe $type/$id"
}
function Send($child,[string] $id,[string] $command) {
  $child.Process.StandardInput.WriteLine((@{protocolVersion=1;type='command';requestId=$id;command=$command}|ConvertTo-Json -Compress))
  $child.Process.StandardInput.Flush()
}
function Until-VideoPacket($child) {
  for($i=0;$i -lt 20;$i++) {
    $event=Until $child 'snapshot'
    if($event.snapshot.metrics.videoPackets -gt 0) { return $event }
  }
  throw 'Capture never became video-ready within 20 snapshot intervals'
}
function Stimulate-DesktopDuplication {
  $script:originalCursorPosition=[System.Windows.Forms.Cursor]::Position
  $screen=[System.Windows.Forms.Screen]::FromPoint($script:originalCursorPosition)
  $x=if($script:originalCursorPosition.X + 1 -lt $screen.Bounds.Right) {
    $script:originalCursorPosition.X + 1
  } else {
    $script:originalCursorPosition.X - 1
  }
  [System.Windows.Forms.Cursor]::Position=New-Object System.Drawing.Point($x,$script:originalCursorPosition.Y)
}
function Exited($child,[int] $code) {
  Assert ($child.Process.WaitForExit(20000)) 'Engine did not terminate'
  Assert ($child.Process.ExitCode -eq $code) "Wrong exit code: $($child.Process.ExitCode); $($child.Errors.Result)"
}
try {
  $catalogChild=Start-Engine 'catalog-json'
  $catalog=Read-Event $catalogChild
  Exited $catalogChild 0
  Assert ($catalog.monitors.Count -gt 0) 'No monitor for hardware protocol test'
  $cursorScreen=[System.Windows.Forms.Screen]::FromPoint([System.Windows.Forms.Cursor]::Position)
  $captureMonitor=@($catalog.monitors | Where-Object { $_.name -eq $cursorScreen.DeviceName })[0]
  Assert ($null -ne $captureMonitor) 'Cursor monitor is absent from the engine catalog'
  $config=@{
    protocolVersion=1;monitorId=$captureMonitor.id
    systemAudioEnabled=$false;systemAudioId='';microphoneEnabled=$false;microphoneId=''
    width=1280;height=720;fps=30;bitrate=8000000;replaySeconds=30;clipSeconds=1
    container='mp4';outputDirectory=$testRoot;saveReplayHotkey='F8'
    toggleRecordingHotkey='Ctrl+Shift+R';continuousRecordingEnabled=$false
  }
  $configPath=Join-Path $testRoot 'host.json'
  $utf8=New-Object System.Text.UTF8Encoding($false)
  [IO.File]::WriteAllText($configPath,($config|ConvertTo-Json -Compress),$utf8)
  $child=Start-Engine 'session-json' $configPath
  $ready=Until $child 'ready'
  Assert ($ready.snapshot.replayActive) 'Replay is not active'
  $null=Until $child 'snapshot'
  $periodic=Until $child 'snapshot'
  Assert ($periodic.snapshot.revision -gt $ready.snapshot.revision) 'No periodic snapshot revision'
  Send $child 'toggle-1' 'toggle_recording'
  $toggle=Until $child 'command_result' 'toggle-1'
  Assert ($toggle.snapshot.continuousRecordingActive) 'Toggle did not start real recording'
  Assert ($toggle.snapshot.revision -gt $ready.snapshot.revision) 'Toggle did not advance revision'
  Send $child 'toggle-1' 'toggle_recording'
  $duplicate=Until $child 'command_result' 'toggle-1'
  Assert ($duplicate.error.code -eq 'protocol.duplicate_request_id') 'Duplicate request was not rejected'
  Assert ($duplicate.snapshot.continuousRecordingActive) 'Duplicate request toggled recording'
  Send $child 'unknown-1' 'erase_everything'
  $unknown=Until $child 'error' 'unknown-1'
  Assert ($unknown.error.code -eq 'protocol.unknown_command') 'Unknown command did not complete its pending request'
  Send $child 'after-rejection' 'get_snapshot'
  $afterRejection=Until $child 'command_result' 'after-rejection'
  Assert ($afterRejection.snapshot.lifecycle -eq 'ready') 'Rejected command broke later pending request completion'
  $child.Process.StandardInput.WriteLine(('x'*65537))
  $oversize=Until $child 'error'
  Assert ($oversize.error.code -eq 'protocol.message_too_large') 'Oversized request accepted'
  Send $child 'snapshot-1' 'get_snapshot'
  $snapshot=Until $child 'command_result' 'snapshot-1'
  Assert ($snapshot.snapshot.continuousRecordingActive) 'Authoritative snapshot lost continuous state'
  Stimulate-DesktopDuplication
  $videoReady=Until-VideoPacket $child
  Assert ($videoReady.snapshot.metrics.pipelineErrors -eq 0) 'Video readiness reported a pipeline error'
  Send $child 'save-1' 'save_replay'
  $saved=Until $child 'clip_saved' 'save-1'
  Assert (Test-Path -LiteralPath $saved.outputPath) 'Saved replay output missing'
  Send $child 'stop-1' 'stop'
  $stopped=Until $child 'command_result' 'stop-1'
  Assert ($stopped.snapshot.lifecycle -eq 'stopped') 'Stop did not publish stopped lifecycle'
  Exited $child 0
  $eof=Start-Engine 'session-json' $configPath
  $null=Until $eof 'ready'
  $eof.Process.StandardInput.Close()
  Exited $eof 0
  Write-Output 'PASS session: ready, periodic snapshot, toggle, duplicate ID, 64 KiB bound, save, stop and EOF; no orphan process'
  foreach($mode in @('none','system','microphone','both')) {
    if(($mode -in @('system','both')) -and $catalog.systemAudio.Count -eq 0) { continue }
    if(($mode -in @('microphone','both')) -and $catalog.microphones.Count -eq 0) { continue }
    $testConfig=$config.Clone()
    $testConfig.systemAudioEnabled=$mode -in @('system','both')
    $testConfig.microphoneEnabled=$mode -in @('microphone','both')
    if($testConfig.systemAudioEnabled) { $testConfig.systemAudioId=$catalog.systemAudio[0].id }
    if($testConfig.microphoneEnabled) { $testConfig.microphoneId=$catalog.microphones[0].id }
    $testConfig.durationSeconds=5
    $testConfig.outputPath=Join-Path $testRoot ("test-$mode.mp4")
    [IO.File]::WriteAllText($configPath,($testConfig|ConvertTo-Json -Compress),$utf8)
    $test=Start-Engine 'test-json' $configPath
    $result=Until $test 'test_result'
    Exited $test 0
    Assert ($result.passed -and $result.muxCompleted -and $result.fileSize -gt 0) 'Test result did not validate output'
    Assert ($result.outputPath -eq $testConfig.outputPath) 'Test ignored exact output path'
    Assert ($result.videoPackets -gt 0) 'No real video packets'
    Assert ($result.durationSeconds -eq 5) 'Test did not report the fixed five-second duration'
    Assert ($result.metrics.videoTicks -ge 145 -and $result.metrics.videoTicks -le 160) 'Capture metrics exceeded the bounded test interval'
    Write-Output "PASS bounded recording: $mode; video=$($result.videoPackets); audio=$($result.audioPackets); bytes=$($result.fileSize)"
  }
  $config.systemAudioEnabled=$true; $config.systemAudioId='missing-pinned-endpoint'
  [IO.File]::WriteAllText($configPath,($config|ConvertTo-Json -Compress),$utf8)
  $missing=Start-Engine 'session-json' $configPath
  $failure=Read-Event $missing
  Assert ($failure.type -eq 'fatal_error' -and $failure.error.code -eq 'audio.endpoint_unavailable') 'Missing pinned endpoint silently replaced'
  Exited $missing 4
  Write-Output 'PASS missing pinned endpoint: structured failure, no default substitution'
} finally {
  if($null -ne $originalCursorPosition) {
    [System.Windows.Forms.Cursor]::Position=$originalCursorPosition
  }
  foreach($process in $children) {
    if(-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $process.Dispose()
  }
  # Keep recordings as inspectable hardware evidence; no shared user paths are removed.
  Write-Output "Evidence: $testRoot"
}
