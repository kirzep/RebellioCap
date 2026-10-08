Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$collector = Join-Path $repositoryRoot 'scripts\collect-performance.ps1'
$verifier = Join-Path $repositoryRoot 'scripts\verify-milestone-1.ps1'
$json = & $collector -PreflightOnly
if ($LASTEXITCODE -ne 0) { throw 'Performance preflight failed.' }
$result = $json | ConvertFrom-Json
if ($result.status -ne 'passed') { throw "Unexpected preflight status: $($result.status)." }
if ([string]::IsNullOrWhiteSpace($result.primaryMonitor)) {
  throw 'Performance preflight did not resolve the primary monitor.'
}
if ($result.presentMonVersion -ne '2.5.1') {
  throw "Unexpected PresentMon version: $($result.presentMonVersion)."
}
$planJson = & $collector -PlanOnly -SettingsDeclaration 'preset=High; resolution=1920x1080; mode=fullscreen'
if ($LASTEXITCODE -ne 0) { throw 'Performance collection plan failed.' }
$plan = $planJson | ConvertFrom-Json
$customPlanJson = & $collector -PlanOnly -GameExecutable (Join-Path $PSHOME 'pwsh.exe') -SceneDeclaration 'fixed replay route' -SettingsDeclaration 'test settings'
if ($LASTEXITCODE -ne 0) { throw 'Custom game plan failed.' }
$customPlan = $customPlanJson | ConvertFrom-Json
if ($customPlan.schemaVersion -ne 2 -or
    [string]::IsNullOrWhiteSpace($customPlan.buildIdentity.sourceCommit) -or
    $customPlan.buildIdentity.binaries[0].sha256 -ne (Get-FileHash $customPlan.engine).Hash) {
  throw 'Collection plan lacks the current dirty-tree/build identity.'
}
if ($customPlan.sceneDeclaration -ne 'fixed replay route' -or
    $customPlan.runs[0].presentMonArguments[1] -ne 'pwsh.exe' -or
    $customPlan.gameExecutable -ne (Join-Path $PSHOME 'pwsh.exe')) {
  throw 'Custom game/scene did not reach the collection plan.'
}
if ($plan.runs.Count -ne 6) { throw "Expected six benchmark runs, got $($plan.runs.Count)." }
if (($plan.runs | ForEach-Object { "$($_.kind)-$($_.index)" }) -join ',' -ne
    'baseline-1,capture-1,baseline-2,capture-2,baseline-3,capture-3') {
  throw 'Benchmark plan must alternate baseline and capture within each pair.'
}
if ($plan.sessionName -ne 'RebellioCapBenchmark') {
  throw "Unexpected PresentMon session: $($plan.sessionName)."
}
if ($plan.secondSaveAfterSeconds -ne 1770) {
  throw "Expected the second continuity save at 29:30, found $($plan.secondSaveAfterSeconds) seconds."
}
$capturePlanJson = & $verifier -UsePrimaryMonitor -CapturePlanOnly -DurationMinutes 30
if ($LASTEXITCODE -ne 0) { throw 'Milestone capture plan failed.' }
$capturePlan = $capturePlanJson | ConvertFrom-Json
if ($capturePlan.primaryMonitor -ne $result.primaryMonitor) {
  throw 'Milestone verifier selected a different primary monitor.'
}
if (-not (@($capturePlan.arguments) -contains '1800')) {
  throw 'Milestone verifier did not plan a 30-minute capture.'
}
if ([string]::IsNullOrWhiteSpace($capturePlan.ffprobe) -or
    -not (@($capturePlan.mkvSmokeArguments) -contains 'mkv')) {
  throw 'Milestone verifier did not plan MP4/MKV ffprobe evidence.'
}
Write-Output 'performance preflight passed'
