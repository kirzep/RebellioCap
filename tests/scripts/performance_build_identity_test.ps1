Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../scripts/performance-build-identity.ps1')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('performance-identity-' + [guid]::NewGuid())
try {
  New-Item -ItemType Directory -Path $fixture | Out-Null
  & git -C $fixture init -q
  Set-Content -LiteralPath (Join-Path $fixture 'source.txt') -Value 'original'
  & git -C $fixture add source.txt
  & git -C $fixture -c user.name=Test -c user.email=test@example.invalid commit -qm fixture
  if ($LASTEXITCODE -ne 0) { throw 'Fixture commit failed.' }
  $engine = Join-Path $fixture 'engine.exe'
  Set-Content -LiteralPath $engine -Value 'engine one'
  Set-Content -LiteralPath (Join-Path $fixture 'runtime.dll') -Value 'runtime'
  Set-Content -LiteralPath (Join-Path $fixture 'source.txt') -Value 'modified'
  $identity = Get-PerformanceBuildIdentity $fixture $engine
  if (-not $identity.sourceTreeDirty -or $identity.sourceStatus.Count -lt 3) { throw 'Dirty tree evidence missing.' }
  if ($identity.binaries.Count -ne 2) { throw 'Runtime DLL evidence missing.' }
  if ($identity.binaries[0].sha256 -ne (Get-FileHash $engine).Hash) { throw 'Wrong engine hash.' }
  Set-Content -LiteralPath $engine -Value 'engine two'
  $changed = Get-PerformanceBuildIdentity $fixture $engine
  if ($changed.binaries[0].sha256 -eq $identity.binaries[0].sha256) { throw 'Replaced engine was not detected.' }
  & git -C $fixture add .
  & git -C $fixture -c user.name=Test -c user.email=test@example.invalid commit -qm clean
  $clean = Get-PerformanceBuildIdentity $fixture $engine
  if ($clean.sourceTreeDirty) { throw 'Clean tree reported dirty.' }
  $evidence = Join-Path $fixture 'artifacts/milestone-1/performance'
  New-Item -ItemType Directory -Path $evidence -Force | Out-Null
  Set-Content -LiteralPath (Join-Path $evidence 'run.json') -Value '{}'
  if ((Get-PerformanceBuildIdentity $fixture $engine).sourceTreeDirty) { throw 'Generated evidence counted as source.' }
  Write-Output 'performance build identity passed'
} finally {
  $resolved = [IO.Path]::GetFullPath($fixture)
  $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
  if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
      (Split-Path -Leaf $resolved).StartsWith('performance-identity-')) {
    Remove-Item -LiteralPath $resolved -Recurse -Force
  }
}
