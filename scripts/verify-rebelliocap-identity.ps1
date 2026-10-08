$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $repositoryRoot
try {
  # Product identity applies to shipped code and build inputs. Historical
  # development notes can retain earlier names without affecting the product.
  $previousName = 'scope' + 'clipper'
  $hits = @(git grep -I -n -i $previousName -- src apps cmake contracts scripts tests CMakeLists.txt CMakePresets.json vcpkg.json package.json)
  $searchExitCode = $LASTEXITCODE
  if ($searchExitCode -gt 1) { throw 'Unable to check tracked product files.' }
  if ($hits.Count -gt 0) {
    $hits
    exit 1
  }
}
finally {
  Pop-Location
}
