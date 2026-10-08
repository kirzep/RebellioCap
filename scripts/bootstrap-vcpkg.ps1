[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$baseline = '118bba14b94bc040c098c0c15e63c142148c05ca'
$repository = 'https://github.com/microsoft/vcpkg.git'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$toolsRoot = Join-Path $repositoryRoot '.tools'
$vcpkgRoot = Join-Path $toolsRoot 'vcpkg'
$stagingRoot = "$vcpkgRoot.bootstrap-$PID"
foreach ($bootstrapTarget in @($vcpkgRoot, $stagingRoot)) {
  $resolvedTarget = [IO.Path]::GetFullPath($bootstrapTarget)
  if (!$resolvedTarget.StartsWith($repositoryRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Unsafe bootstrap move/cleanup target outside workspace.'
  }
}

function Get-CheckedOutCommit([string] $path) {
  return (& git -C $path rev-parse HEAD).Trim().ToLowerInvariant()
}

if (Test-Path -LiteralPath $vcpkgRoot) {
  $actual = Get-CheckedOutCommit $vcpkgRoot
  if ($actual -ne $baseline) {
    throw "Refusing vcpkg checkout at $actual; expected pinned $baseline. Remove only $vcpkgRoot to bootstrap the pinned checkout."
  }
} else {
  New-Item -ItemType Directory -Force -Path $toolsRoot | Out-Null
  try {
    & git init $stagingRoot
    if ($LASTEXITCODE -ne 0) { throw 'Unable to initialize vcpkg staging repository.' }

    & git -C $stagingRoot remote add origin $repository
    if ($LASTEXITCODE -ne 0) { throw 'Unable to configure the vcpkg staging remote.' }

    & git -C $stagingRoot fetch --depth 1 origin $baseline
    if ($LASTEXITCODE -ne 0) { throw "Unable to fetch pinned vcpkg commit $baseline." }

    & git -C $stagingRoot checkout --detach $baseline
    if ($LASTEXITCODE -ne 0) { throw "Unable to checkout pinned vcpkg commit $baseline." }

    if ((Get-CheckedOutCommit $stagingRoot) -ne $baseline) {
      throw "vcpkg staging checkout verification failed; expected pinned $baseline."
    }
    Move-Item -LiteralPath $stagingRoot -Destination $vcpkgRoot
  } catch {
    if (Test-Path -LiteralPath $stagingRoot) {
      Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
    throw
  }
}

$verified = Get-CheckedOutCommit $vcpkgRoot
if ($verified -ne $baseline) {
  throw "vcpkg checkout verification failed: got $verified, expected $baseline."
}

$previousMetrics = $env:VCPKG_DISABLE_METRICS
$env:VCPKG_DISABLE_METRICS = '1'
try {
  & (Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
  if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed.' }
} finally {
  $env:VCPKG_DISABLE_METRICS = $previousMetrics
}

Write-Host "vcpkg is pinned at $verified with telemetry disabled."
