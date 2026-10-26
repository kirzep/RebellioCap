Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'scripts/verify-rebelliocap-identity.ps1'),[ref]$tokens,[ref]$errors)
$function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-ProductIdentityViolations'},$true)
if ($null -eq $function) { throw 'Identity check cannot distinguish compatible storage keys from product branding.' }
. ([scriptblock]::Create($function.Extent.Text))
$name='scope'+'clipper'
$hits=@(
  "apps/desktop/src/screens/ClipsScreen.tsx:13:const key = '$name.clips.view';",
  "apps/desktop/src/screens/ClipsScreen.test.tsx:1:assert('$name');",
  "apps/desktop/e2e/example.spec.ts:1:<title>$name QA</title>",
  "tests/unit/game_category_test.cpp:8:const auto fixture = L'$name-icon-test';"
)
if (@(Get-ProductIdentityViolations $hits).Count -ne 0) { throw 'Compatibility keys and test-only fixture names are not product branding violations.' }
$bad=@("apps/desktop/src/main.tsx:1:const title = '$name';", "apps/desktop/src/screens/ClipsScreen.tsx:13:const key = '$name.clips.view'; const title = '$name';")
if (@(Get-ProductIdentityViolations $bad).Count -ne 2) { throw 'Visible old branding must remain rejected, even on a compatibility-key line.' }
Write-Output 'product identity compatibility policy passed'
