[CmdletBinding()]
param(
  [string] $EnginePath,
  [string] $EvidenceDirectory
)

# Windows PowerShell turns cargo's ordinary compilation stderr into terminating
# NativeCommandError when it is merged into Tee-Object under ErrorAction=Stop.
# Run the captured suites in PowerShell 7, including callers using powershell.exe.
if ($PSVersionTable.PSVersion.Major -lt 7) {
  $shell = Get-Command pwsh -ErrorAction Stop
  $forward = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
  if ($EnginePath) { $forward += @('-EnginePath', $EnginePath) }
  if ($EvidenceDirectory) { $forward += @('-EvidenceDirectory', $EvidenceDirectory) }
  & $shell.Source @forward
  exit $LASTEXITCODE
}

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (!$EnginePath) { $EnginePath = Join-Path $repositoryRoot 'build/windows-release/RebellioCap.Engine.exe' }
$EnginePath = (Resolve-Path -LiteralPath $EnginePath).Path
$manifestPath = Join-Path $repositoryRoot 'apps/desktop/src-tauri/Cargo.toml'
$resources = Join-Path $repositoryRoot 'apps/desktop/src-tauri/resources/engine'
$stagedManifest = Join-Path $resources 'manifest.json'
foreach ($required in @($manifestPath, $stagedManifest,
    (Join-Path $repositoryRoot '.tools/rust/cargo/bin/cargo.exe'),
    (Join-Path $resources 'ffmpeg.exe'), (Join-Path $resources 'ffprobe.exe'))) {
  if (!(Test-Path -LiteralPath $required -PathType Leaf)) {
    throw "Native test prerequisite missing: $required. Build the Release engine and run scripts/stage-desktop-engine.ps1 first."
  }
}
$staged = Get-Content -LiteralPath $stagedManifest -Raw | ConvertFrom-Json
foreach ($entry in $staged.files) {
  if ([IO.Path]::GetFileName($entry.name) -ne $entry.name) { throw 'Invalid staged resource name.' }
  if ((Get-FileHash -LiteralPath (Join-Path $resources $entry.name) -Algorithm SHA256).Hash -ne $entry.sha256) {
    throw "Staged resource hash mismatch: $($entry.name). Restage the tested resources."
  }
}
. (Join-Path $PSScriptRoot 'performance-build-identity.ps1')
$identity = Get-PerformanceBuildIdentity $repositoryRoot $EnginePath
if (!$EvidenceDirectory) {
  $EvidenceDirectory = Join-Path $repositoryRoot ('build/native-desktop-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
if (Test-Path -LiteralPath $EvidenceDirectory) { throw 'Evidence directory already exists; choose a fresh path.' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$EvidenceDirectory = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$startedUtc = [DateTime]::UtcNow.ToString('o')
[ordered]@{ startedUtc = $startedUtc; buildIdentity = $identity; stagedResources = $staged.files } |
  ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'manifest.json') -Encoding UTF8
$previousOverride = $env:REBELLIOCAP_ENGINE_PATH
$results = [System.Collections.Generic.List[object]]::new()
$passed = $false
$failure = $null
try {
  $env:REBELLIOCAP_ENGINE_PATH = $EnginePath
  foreach ($suite in @(
      @{name='editor'; args=@('--lib','editor::tests')},
      @{name='native-engine'; args=@('--test','native_engine')})) {
    $taskArguments = @('cargo','test','--manifest-path',$manifestPath) + $suite.args +
      @('--','--ignored','--test-threads=1','--nocapture')
    $logPath = Join-Path $EvidenceDirectory ($suite.name + '.log')
    & (Join-Path $PSScriptRoot 'invoke-rust.ps1') @taskArguments 2>&1 | Tee-Object -FilePath $logPath
    $exitCode = $LASTEXITCODE
    $results.Add([ordered]@{ suite=$suite.name; arguments=$taskArguments; exitCode=$exitCode; log=$logPath })
    if ($exitCode -ne 0) { throw "Native $($suite.name) tests failed; inspect $logPath" }
    $log = Get-Content -LiteralPath $logPath -Raw
    if ($log -notmatch 'test result: ok\. 2 passed; 0 failed; 0 ignored;') {
      throw "Native $($suite.name) suite did not run both required tests; inspect $logPath"
    }
  }
  foreach ($entry in $identity.binaries) {
    if ((Get-FileHash -LiteralPath $entry.path -Algorithm SHA256).Hash -ne $entry.sha256) {
      throw "Engine runtime changed during native tests: $($entry.path)"
    }
  }
  foreach ($entry in $staged.files) {
    if ((Get-FileHash -LiteralPath (Join-Path $resources $entry.name) -Algorithm SHA256).Hash -ne $entry.sha256) {
      throw "Staged resource changed during native tests: $($entry.name)"
    }
  }
  $passed = $true
} catch {
  $failure = $_.Exception.Message
  throw
} finally {
  $env:REBELLIOCAP_ENGINE_PATH = $previousOverride
  [ordered]@{ startedUtc=$startedUtc; endedUtc=[DateTime]::UtcNow.ToString('o'); passed=$passed; failure=$failure; suites=@($results.ToArray()) } |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'results.json') -Encoding UTF8
}
Write-Output "Four native desktop tests passed; evidence: $EvidenceDirectory"
