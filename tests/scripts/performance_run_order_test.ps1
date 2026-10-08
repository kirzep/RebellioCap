Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
  (Join-Path $repositoryRoot 'scripts/collect-performance.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Collector must parse.' }
$function = $ast.Find({ param($node)
  $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Invoke-BenchmarkCollection'
}, $true)
if ($null -eq $function) { throw 'Collector lacks alternating benchmark execution.' }
. ([scriptblock]::Create($function.Extent.Text))
$script:events = [Collections.Generic.List[string]]::new()
function Invoke-PresentMonRun($plan, $run) {
  $script:events.Add("baseline-$($run.index)")
  [pscustomobject]@{ kind = $run.kind; index = $run.index }
}
function Invoke-CaptureRun($plan, $run) {
  $script:events.Add("capture-$($run.index)")
  [pscustomobject]@{ kind = $run.kind; index = $run.index }
}
$plan = [pscustomobject]@{ runs = @(
  [pscustomobject]@{ kind = 'baseline'; index = 1 },
  [pscustomobject]@{ kind = 'capture'; index = 1 },
  [pscustomobject]@{ kind = 'baseline'; index = 2 },
  [pscustomobject]@{ kind = 'capture'; index = 2 },
  [pscustomobject]@{ kind = 'baseline'; index = 3 },
  [pscustomobject]@{ kind = 'capture'; index = 3 }
) }
$results = @(Invoke-BenchmarkCollection $plan)
if (($script:events -join ',') -ne 'baseline-1,capture-1,baseline-2,capture-2,baseline-3,capture-3' -or
    $results.Count -ne 6) { throw 'Execution must follow the alternating plan and retain all results.' }
function Invoke-CaptureRun($plan, $run) { throw 'injected capture failure' }
$script:events.Clear()
try {
  Invoke-BenchmarkCollection $plan | Out-Null
  throw 'Collection accepted a failed capture.'
} catch {
  if ($_.Exception.Message -ne 'injected capture failure') { throw }
}
if (($script:events -join ',') -ne 'baseline-1') {
  throw 'Collection continued to a baseline after capture failure.'
}
$captureFunction = $ast.Find({ param($node)
  $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Invoke-CaptureRun'
}, $true)
. ([scriptblock]::Create($captureFunction.Extent.Text))
# Replace only external process/UI operations; exercise the real lifecycle.
function Start-RedirectedArgumentProcess($file, $arguments) {
  $script:process = [pscustomobject]@{ HasExited = $false; ExitCode = 0; Id = 123 }
  $script:process | Add-Member ScriptMethod WaitForExit {
    param($timeout)
    $script:events.Add('wait-exit')
    $this.HasExited = $true
    if ($null -ne $timeout) { return $true }
  }
  [pscustomobject]@{ process = $script:process }
}
function Get-Command { [pscustomobject]@{ Source = 'fixture-nvidia-smi' } }
function Start-EngineSampler { return $null }
function Start-Sleep {}
function Read-Host { $script:events.Add('save-prompt'); return 'ignored input' }
function Wait-ForCaptureMoment { $script:events.Add('late-save-wait') }
function Stop-Process { $script:events.Add('force-stop'); $script:process.HasExited = $true }
function Complete-RedirectedProcess($running, $outputPath, $errorPath) {
  $script:events.Add([IO.Path]::GetFileName($outputPath))
}
function Invoke-PresentMonRun($plan, $run) {
  $script:events.Add("measure-$($run.index)")
  [pscustomobject]@{ kind = $run.kind; index = $run.index }
}
$capturePlan = [pscustomobject]@{
  performanceRoot = $repositoryRoot; engine = 'fixture-engine'
  captureArguments = @('fixture'); secondSaveAfterSeconds = 1770
}
$script:events.Clear()
$captureResults = @(Invoke-CaptureRun $capturePlan ([pscustomobject]@{ kind = 'capture'; index = 1 }))
if ($captureResults.Count -ne 1 -or ($script:events -join ',') -ne
    'measure-1,wait-exit,wait-exit,capture-1-engine.ndjson') {
  throw 'Capture must finish and retain its own logs before returning a measurement.'
}
$script:events.Clear()
$captureResults = @(Invoke-CaptureRun $capturePlan ([pscustomobject]@{ kind = 'capture'; index = 3 }))
if ($captureResults.Count -ne 1 -or ($script:events -join ',') -ne
    'save-prompt,measure-3,late-save-wait,save-prompt,wait-exit,wait-exit,capture-engine.ndjson') {
  throw 'Only the final capture must collect early/late continuity saves without prompt output in results.'
}
function Invoke-PresentMonRun { throw 'injected measurement failure' }
$script:events.Clear()
try {
  Invoke-CaptureRun $capturePlan ([pscustomobject]@{ kind = 'capture'; index = 2 }) | Out-Null
  throw 'Capture accepted a failed measurement.'
} catch {
  if ($_.Exception.Message -ne 'injected measurement failure') { throw }
}
if (($script:events -join ',') -ne 'force-stop,wait-exit,capture-2-engine.ndjson') {
  throw 'Failed measurement must stop its engine and preserve logs.'
}
Write-Output 'performance alternating execution passed'
