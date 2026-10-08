[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$dependency = Get-Content -Raw (Join-Path $PSScriptRoot 'dependencies.json') | ConvertFrom-Json
$presentMon = $dependency.presentmon
$destinationDirectory = Join-Path $repositoryRoot '.tools\presentmon'
$destination = Join-Path $destinationDirectory 'PresentMon.exe'
$temporary = Join-Path $destinationDirectory 'PresentMon-2.5.1-x64.exe.download'
$expectedHash = $presentMon.sha256.ToLowerInvariant()

function Test-ExpectedHash([string] $path) {
  return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() -eq $expectedHash
}

New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null

if (Test-Path -LiteralPath $destination) {
  if (Test-ExpectedHash $destination) {
    Write-Host "PresentMon $($presentMon.version) already matches the pinned SHA-256."
    exit 0
  }
  throw "Existing $destination does not match the pinned SHA-256; remove it explicitly before bootstrapping."
}

try {
  Invoke-WebRequest -Uri $presentMon.url -OutFile $temporary
  if (-not (Test-ExpectedHash $temporary)) {
    Remove-Item -LiteralPath $temporary -Force
    throw "PresentMon SHA-256 did not match $expectedHash; deleted the temporary download."
  }
  Move-Item -LiteralPath $temporary -Destination $destination
} catch {
  if (Test-Path -LiteralPath $temporary) {
    Remove-Item -LiteralPath $temporary -Force
  }
  throw
}

Write-Host "PresentMon $($presentMon.version) downloaded and hash-verified."
