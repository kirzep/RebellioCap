[CmdletBinding()]
param(
  [switch] $Hardware,
  [string] $EvidenceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-CommandSucceeded([string] $Step) {
  if ($LASTEXITCODE -ne 0) { throw "$Step failed with exit code $LASTEXITCODE." }
}

$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $workspace
try {
  Write-Host "==> Step 1: Verifying RebellioCap Identity" -ForegroundColor Cyan
  powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'verify-rebelliocap-identity.ps1')
  Assert-CommandSucceeded "Identity"

  Write-Host "==> Step 2: Running Native Engine CMake & CTest" -ForegroundColor Cyan
  $invokeDev = Join-Path $PSScriptRoot 'invoke-dev.ps1'
  powershell -NoProfile -ExecutionPolicy Bypass -File $invokeDev -Command @('cmake', '--preset', 'windows-debug')
  Assert-CommandSucceeded "Native configure/build/test"
  powershell -NoProfile -ExecutionPolicy Bypass -File $invokeDev -Command @('cmake', '--build', '--preset', 'windows-debug')
  Assert-CommandSucceeded "Native configure/build/test"
  powershell -NoProfile -ExecutionPolicy Bypass -File $invokeDev -Command @('ctest', '--preset', 'windows-debug', '--output-on-failure')
  Assert-CommandSucceeded "Native configure/build/test"

  Write-Host "==> Step 3: Running Tauri Rust Host Tests" -ForegroundColor Cyan
  powershell -NoProfile -ExecutionPolicy Bypass -File $invokeDev -Command @('cargo', 'test', '--manifest-path', 'apps/desktop/src-tauri/Cargo.toml')
  Assert-CommandSucceeded "Rust host tests"

  Write-Host "==> Step 4: Typecheck Desktop Frontend" -ForegroundColor Cyan
  npm run typecheck -w @rebelliocap/desktop
  Assert-CommandSucceeded "npm run typecheck -w @rebelliocap/desktop"

  Write-Host "==> Step 5: Lint Desktop Frontend" -ForegroundColor Cyan
  npm run lint -w @rebelliocap/desktop
  Assert-CommandSucceeded "npm run lint -w @rebelliocap/desktop"

  Write-Host "==> Step 6: Running Desktop Vitest Suite" -ForegroundColor Cyan
  npm run test -w @rebelliocap/desktop -- --run
  Assert-CommandSucceeded "npm run test -w @rebelliocap/desktop -- --run"

  Write-Host "==> Step 7: Building Desktop Frontend Production Bundle" -ForegroundColor Cyan
  npm run build -w @rebelliocap/desktop
  Assert-CommandSucceeded "npm run build -w @rebelliocap/desktop"

  Write-Host "==> Step 8: Verifying Offline Security & CSP" -ForegroundColor Cyan
  $distDir = Join-Path $workspace 'apps/desktop/dist'
  if (-not (Test-Path -LiteralPath $distDir)) {
    throw "Production build directory not found at $distDir"
  }
  $tauriConfPath = Join-Path $workspace 'apps/desktop/src-tauri/tauri.conf.json'
  if (-not (Test-Path -LiteralPath $tauriConfPath)) {
    throw "tauri.conf.json not found at $tauriConfPath"
  }
  $tauriConf = Get-Content -LiteralPath $tauriConfPath -Raw | ConvertFrom-Json
  $csp = $tauriConf.app.security.csp
  if (-not $csp -or $csp -notmatch "default-src 'self'") {
    throw "Security check failed: tauri.conf.json missing strict CSP"
  }

  if ($Hardware) {
    Write-Host "==> Step 9: Running Hardware and Native Smoke Suites" -ForegroundColor Cyan
    if ([string]::IsNullOrWhiteSpace($EvidenceRoot)) {
      $EvidenceRoot = Join-Path $workspace 'artifacts/desktop-first-run'
    }
    New-Item -ItemType Directory -Path $EvidenceRoot -Force | Out-Null
    powershell -NoProfile -ExecutionPolicy Bypass -File $invokeDev -Command @('cmake', '--preset', 'windows-hardware-release')
    Assert-CommandSucceeded 'Hardware configure'
    powershell -NoProfile -ExecutionPolicy Bypass -File $invokeDev -Command @('cmake', '--build', '--preset', 'windows-hardware-release')
    Assert-CommandSucceeded 'Hardware build'
    powershell -NoProfile -ExecutionPolicy Bypass -File $invokeDev -Command @('ctest', '--preset', 'windows-hardware-release', '--output-on-failure')
    Assert-CommandSucceeded 'Hardware CTest'
    $hardwareBuild = Join-Path $workspace 'build/windows-hardware-release'
    powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'stage-desktop-engine.ps1') -BuildDirectory $hardwareBuild
    Assert-CommandSucceeded 'Hardware engine staging'
    $nativeEvidence = Join-Path $EvidenceRoot ('native-' + [guid]::NewGuid().ToString('N'))
    powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'test-native-desktop.ps1') -EnginePath (Join-Path $hardwareBuild 'RebellioCap.Engine.exe') -EvidenceDirectory $nativeEvidence
    Assert-CommandSucceeded 'Native desktop suites'
    $commit = (& git rev-parse HEAD).Trim()
    $isDirty = (@(& git status --porcelain).Length -gt 0)
    
    $manifestObj = [ordered]@{
      schemaVersion = 2
      timestamp = (Get-Date).ToUniversalTime().ToString('o')
      gitCommit = $commit
      isDirty = $isDirty
      environment = [ordered]@{
        os = [System.Environment]::OSVersion.VersionString
        arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
      }
      hardwareSmokePassed = $true
      suitesExecuted = @('windows-hardware-release CTest', 'native editor and engine suites')
      nativeEvidenceDirectory = $nativeEvidence
      limitations = @('Does not establish gaming performance or cross-PC/device/HDR acceptance.', 'Does not exercise actual Tauri WebView2 UI or installer lifecycle.')
    }
    $manifestPath = Join-Path $EvidenceRoot 'manifest.json'
    [IO.File]::WriteAllText($manifestPath, ($manifestObj | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
    Write-Host "Generated evidence manifest at $manifestPath" -ForegroundColor Green
  }

  Write-Host "==> All RebellioCap desktop verification checks PASSED!" -ForegroundColor Green
} finally {
  Pop-Location
}
