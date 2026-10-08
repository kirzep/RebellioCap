$ErrorActionPreference = 'Stop'

$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'rust-environment.ps1')
$workspaceRust = Get-WorkspaceRustEnvironment $workspace
if ($workspaceRust) {
  $env:CARGO_HOME = $workspaceRust.CargoHome
  $env:RUSTUP_HOME = $workspaceRust.RustupHome
  $env:PATH = "$($workspaceRust.CargoBin);$env:PATH"
}

$commandToRun = @($args)
if ($commandToRun.Count -gt 0 -and ($commandToRun[0] -eq '-Command' -or $commandToRun[0] -eq '-c')) {
  $commandToRun = @($commandToRun | Select-Object -Skip 1)
}

if ($commandToRun.Count -gt 0) {
  & $commandToRun[0] @($commandToRun | Select-Object -Skip 1)
  exit $LASTEXITCODE
} else {
  & cargo --version
}
