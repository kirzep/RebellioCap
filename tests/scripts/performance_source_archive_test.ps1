Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../scripts/performance-build-identity.ps1')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('performance-source-' + [guid]::NewGuid())
try {
  New-Item -ItemType Directory $fixture | Out-Null
  & git -C $fixture init -q
  Set-Content (Join-Path $fixture '.gitignore') 'ignored/'
  Set-Content (Join-Path $fixture 'source.txt') 'original'
  Set-Content (Join-Path $fixture 'deleted.txt') 'delete me'
  & git -C $fixture add .
  & git -C $fixture -c user.name=Test -c user.email=test@example.invalid commit -qm fixture
  Set-Content (Join-Path $fixture 'source.txt') 'working tree'
  Remove-Item -LiteralPath (Join-Path $fixture 'deleted.txt')
  Set-Content (Join-Path $fixture 'новый файл.txt') 'untracked'
  New-Item -ItemType Directory (Join-Path $fixture 'ignored') | Out-Null
  Set-Content (Join-Path $fixture 'ignored/secret.txt') 'excluded'
  $evidence = Join-Path $fixture 'artifacts/milestone-1/performance'
  New-Item -ItemType Directory $evidence -Force | Out-Null
  Set-Content (Join-Path $evidence 'run.json') '{}'
  $destination = Join-Path $evidence 'source.zip'
  $result = New-PerformanceSourceArchive $fixture $destination
  if ($result.sha256 -ne (Get-FileHash $destination).Hash) { throw 'Archive hash mismatch.' }
  $restore = Join-Path $fixture 'ignored/restored'
  Expand-Archive -LiteralPath $destination -DestinationPath $restore
  if ((Get-Content (Join-Path $restore 'source.txt')).Trim() -ne 'working tree') { throw 'Archived HEAD instead of dirty source.' }
  if ((Get-Content (Join-Path $restore 'новый файл.txt')).Trim() -ne 'untracked') { throw 'Untracked Unicode file lost.' }
  if (Test-Path (Join-Path $restore 'deleted.txt')) { throw 'Deleted file restored.' }
  if (Test-Path (Join-Path $restore 'ignored')) { throw 'Ignored files leaked.' }
  if (Test-Path (Join-Path $restore 'artifacts')) { throw 'Evidence recursively archived.' }
  $manifest = Get-Content -Raw ($destination + '.manifest.json') | ConvertFrom-Json
  foreach ($file in $manifest.files) {
    if ((Get-FileHash -LiteralPath (Join-Path $restore $file.path)).Hash -ne $file.sha256) { throw 'File hash mismatch.' }
  }
  $refused = $false
  try { New-PerformanceSourceArchive $fixture $destination } catch { $refused = $true }
  if (-not $refused) { throw 'Existing archive overwritten.' }
  Write-Output 'performance source archive passed'
} finally {
  $resolved = [IO.Path]::GetFullPath($fixture)
  if ($resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase) -and
      (Split-Path -Leaf $resolved).StartsWith('performance-source-')) {
    Remove-Item -LiteralPath $resolved -Recurse -Force
  }
}
