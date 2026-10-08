# Preserve an installed system toolchain on clean workspaces. A complete local
# cargo installation opts into the repository's isolated Rust homes.
function Get-WorkspaceRustEnvironment([string] $Workspace) {
  $cargoHome = Join-Path $Workspace '.tools\rust\cargo'
  $rustupHome = Join-Path $Workspace '.tools\rust\rustup'
  $cargoBin = Join-Path $cargoHome 'bin'
  if (!(Test-Path -LiteralPath (Join-Path $cargoBin 'cargo.exe') -PathType Leaf)) { return $null }
  return [pscustomobject]@{ CargoHome = $cargoHome; RustupHome = $rustupHome; CargoBin = $cargoBin }
}
