$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('rebcap-rust-env-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path (Join-Path $fixture 'scripts') -Force
$oldCargo = $env:CARGO_HOME
$oldRustup = $env:RUSTUP_HOME
try {
  Copy-Item -LiteralPath (Join-Path $repository 'scripts/invoke-rust.ps1') -Destination (Join-Path $fixture 'scripts/invoke-rust.ps1')
  $helper = Join-Path $repository 'scripts/rust-environment.ps1'
  if (Test-Path -LiteralPath $helper) { Copy-Item -LiteralPath $helper -Destination (Join-Path $fixture 'scripts/rust-environment.ps1') }
  $probe = Join-Path $fixture 'probe.ps1'
  [IO.File]::WriteAllText($probe, '[ordered]@{cargo=$env:CARGO_HOME;rustup=$env:RUSTUP_HOME;path=$env:PATH} | ConvertTo-Json -Compress; exit 0')
  $env:CARGO_HOME = 'inherited-cargo'
  $env:RUSTUP_HOME = 'inherited-rustup'
  $shell = (Get-Process -Id $PID).Path
  $actual = & $shell -NoProfile -File (Join-Path $fixture 'scripts/invoke-rust.ps1') $shell -NoProfile -File $probe | ConvertFrom-Json
  if ($LASTEXITCODE -ne 0 -or $actual.cargo -ne 'inherited-cargo' -or $actual.rustup -ne 'inherited-rustup') { throw 'Clean workspace must preserve inherited Rust environment.' }
  $localBin = Join-Path $fixture '.tools\rust\cargo\bin'
  $null = New-Item -ItemType Directory -Path $localBin -Force
  [IO.File]::WriteAllText((Join-Path $localBin 'cargo.exe'), '')
  $actual = & $shell -NoProfile -File (Join-Path $fixture 'scripts/invoke-rust.ps1') $shell -NoProfile -File $probe | ConvertFrom-Json
  $normalizedPaths = @($actual.path.Split(';') | Where-Object { $_ } | ForEach-Object { [IO.Path]::GetFullPath($_) })
  if ([IO.Path]::GetFullPath($actual.cargo) -ne [IO.Path]::GetFullPath((Join-Path $fixture '.tools\rust\cargo')) -or [IO.Path]::GetFullPath($actual.rustup) -ne [IO.Path]::GetFullPath((Join-Path $fixture '.tools\rust\rustup')) -or $normalizedPaths -notcontains [IO.Path]::GetFullPath($localBin)) { throw 'Installed workspace Rust must take precedence.' }
  [IO.File]::WriteAllText($probe, 'exit 17')
  & $shell -NoProfile -File (Join-Path $fixture 'scripts/invoke-rust.ps1') $shell -NoProfile -File $probe
  if ($LASTEXITCODE -ne 17) { throw 'Child failure exit code must propagate.' }
  Write-Host 'Rust environment: inherited fallback, local precedence, and child failure passed.'
} finally {
  $env:CARGO_HOME = $oldCargo
  $env:RUSTUP_HOME = $oldRustup
  $resolvedFixture = [IO.Path]::GetFullPath($fixture)
  $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
  if (!$resolvedFixture.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture cleanup target.' }
  Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
