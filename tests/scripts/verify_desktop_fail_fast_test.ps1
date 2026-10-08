$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('rebcap-verify-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path (Join-Path $testRoot 'scripts')
$null = New-Item -ItemType Directory -Path (Join-Path $testRoot 'apps/desktop/dist')
$null = New-Item -ItemType Directory -Path (Join-Path $testRoot 'apps/desktop/src-tauri')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'scripts/verify-desktop.ps1') -Destination (Join-Path $testRoot 'scripts/verify-desktop.ps1')
'{"app":{"security":{"csp":"default-src ''self''"}}}' | Set-Content -LiteralPath (Join-Path $testRoot 'apps/desktop/src-tauri/tauri.conf.json')
function global:powershell { $global:VerificationTestCalls++; $global:LASTEXITCODE = $(if ($global:VerificationTestCalls -eq $global:VerificationTestFailureAt) { 17 } else { 0 }) }
function global:npm { $global:VerificationTestCalls++; $global:LASTEXITCODE = $(if ($global:VerificationTestCalls -eq $global:VerificationTestFailureAt) { 17 } else { 0 }) }
function global:git { if ($args -contains 'rev-parse') { 'test-head' }; $global:LASTEXITCODE = 0 }
try {
  for ($failureAt = 1; $failureAt -le 9; $failureAt++) {
    $global:VerificationTestCalls = 0; $global:VerificationTestFailureAt = $failureAt
    $threw = $false
    try { & (Join-Path $testRoot 'scripts/verify-desktop.ps1') } catch { $threw = $true }
    if (!$threw -or $global:VerificationTestCalls -ne $failureAt) {
      throw "Verification did not stop at failed command $failureAt; calls=$global:VerificationTestCalls threw=$threw"
    }
  }
  $global:VerificationTestCalls = 0; $global:VerificationTestFailureAt = -1
  & (Join-Path $testRoot 'scripts/verify-desktop.ps1')
  if ($global:VerificationTestCalls -ne 9) { throw 'Successful verification omitted required commands.' }
  for ($failureAt = 10; $failureAt -le 14; $failureAt++) {
    $global:VerificationTestCalls = 0; $global:VerificationTestFailureAt = $failureAt
    $threw = $false
    try { & (Join-Path $testRoot 'scripts/verify-desktop.ps1') -Hardware -EvidenceRoot (Join-Path $testRoot 'hardware') } catch { $threw = $true }
    if (!$threw -or $global:VerificationTestCalls -ne $failureAt) {
      throw "Hardware verification did not run and stop at failed command $failureAt; calls=$global:VerificationTestCalls threw=$threw"
    }
  }
  $global:VerificationTestCalls = 0; $global:VerificationTestFailureAt = -1
  & (Join-Path $testRoot 'scripts/verify-desktop.ps1') -Hardware -EvidenceRoot (Join-Path $testRoot 'hardware')
  if ($global:VerificationTestCalls -ne 14) { throw 'Successful hardware verification omitted required suites.' }
  Write-Output 'PASS: all 14 failing commands stop verification; successful ordinary/hardware paths run every required command.'
} finally {
  Remove-Item -LiteralPath 'function:global:powershell','function:global:npm','function:global:git' -ErrorAction SilentlyContinue
  $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
  $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
  if (!$resolvedTestRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolvedTestRoot) -notlike 'rebcap-verify-*') { throw 'Unsafe test cleanup path.' }
  Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
}
