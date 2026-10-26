$ErrorActionPreference = 'Stop'

function Get-ProductIdentityViolations([string[]] $Hits) {
  $oldName='scope'+'clipper'
  foreach ($hit in $Hits) {
    $parts=$hit -split ':',3
    $path=$parts[0].Replace('\','/')
    if ($path.StartsWith('tests/') -or $path.Contains('/e2e/') -or $path -match '\.(test|spec)\.[^/]+$') { continue }
    # Keep persisted localStorage compatibility without allowing visible old
    # product branding elsewhere on the same source line.
    $content=$parts[-1].Replace("'$oldName.clips.view'",'').Replace('"'+$oldName+'.clips.view"','')
    if ($content -match [regex]::Escape($oldName)) { $hit }
  }
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $repositoryRoot
try {
  # Product identity applies to shipped code and build inputs. Historical
  # development notes can retain earlier names without affecting the product.
  $previousName = 'scope' + 'clipper'
  $hits = @(git grep -I -n -i $previousName -- src apps cmake contracts scripts tests CMakeLists.txt CMakePresets.json vcpkg.json package.json)
  $searchExitCode = $LASTEXITCODE
  if ($searchExitCode -gt 1) { throw 'Unable to check tracked product files.' }
  $hits = @(Get-ProductIdentityViolations $hits)
  if ($hits.Count -gt 0) {
    $hits
    exit 1
  }
}
finally {
  Pop-Location
}
