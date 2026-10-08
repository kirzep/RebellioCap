[CmdletBinding(DefaultParameterSetName = 'Public')]
param(
  [Parameter(Mandatory, ParameterSetName = 'Public')]
  [ValidateNotNullOrEmpty()]
  [string[]] $Command,
  [Parameter(ValueFromRemainingArguments, ParameterSetName = 'Public')]
  [string[]] $RemainingCommand,
  [Parameter(Mandatory, ParameterSetName = 'Internal')]
  [ValidateNotNullOrEmpty()]
  [string] $InternalInvocation
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
  throw "Visual Studio locator was not found at $vswhere."
}

$installationPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if ([string]::IsNullOrWhiteSpace($installationPath)) {
  throw 'No Visual Studio installation with x64 MSVC tools was found.'
}

$devCommand = Join-Path $installationPath 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $devCommand)) {
  throw "Visual Studio developer command script was not found at $devCommand."
}

$cmakeDirectory = Join-Path $installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaDirectory = Join-Path $installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$bundledTools = @{
  cmake = Join-Path $cmakeDirectory 'cmake.exe'
  ctest = Join-Path $cmakeDirectory 'ctest.exe'
  ninja = Join-Path $ninjaDirectory 'ninja.exe'
}
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'rust-environment.ps1')
$workspaceRust = Get-WorkspaceRustEnvironment $workspace
$rustCargoHome = Join-Path $workspace '.tools\rust\cargo'
$rustUpHome = Join-Path $workspace '.tools\rust\rustup'
$rustCargoBin = Join-Path $rustCargoHome 'bin'
if ($workspaceRust) {
  $bundledTools['cargo'] = Join-Path $rustCargoBin 'cargo.exe'
  $bundledTools['rustc'] = Join-Path $rustCargoBin 'rustc.exe'
}

if ($PSCmdlet.ParameterSetName -eq 'Public') {
  $invocation = @($Command)
  if ($PSBoundParameters.ContainsKey('RemainingCommand')) {
    $invocation += @($RemainingCommand)
  }
} else {
  try {
    $payloadJson = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($InternalInvocation))
    $payload = ConvertFrom-Json -InputObject $payloadJson
    $invocation = @($payload.Invocation | ForEach-Object { [string] $_ })
  } catch {
    [Console]::Error.WriteLine("Unable to decode the internal command invocation: $($_.Exception.Message)")
    exit 1
  }
}

if ($invocation.Count -eq 0) {
  [Console]::Error.WriteLine('No command was provided.')
  exit 1
}

if ($PSCmdlet.ParameterSetName -eq 'Public') {
  $payloadJson = ConvertTo-Json -InputObject @{ Invocation = [string[]] $invocation } -Compress
  $encodedInvocation = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($payloadJson))
  $powerShellExecutable = if ($PSVersionTable.PSEdition -eq 'Core') {
    Join-Path $PSHOME 'pwsh.exe'
  } else {
    Join-Path $PSHOME 'powershell.exe'
  }
  if (-not (Test-Path -LiteralPath $powerShellExecutable -PathType Leaf)) {
    throw "PowerShell executable was not found at $powerShellExecutable."
  }

  $developerInvocation = @(
    "call `"$devCommand`" -arch=x64 -host_arch=x64 >nul"
    if ($workspaceRust) { "set `"CARGO_HOME=$rustCargoHome`""; "set `"RUSTUP_HOME=$rustUpHome`"" }
    "set `"PATH=$(if ($workspaceRust) { $rustCargoBin + ';' })$cmakeDirectory;$ninjaDirectory;!PATH!`""
    "`"$powerShellExecutable`" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$PSCommandPath`" -InternalInvocation $encodedInvocation"
  ) -join ' && '

  & cmd.exe /d /v:on /s /c $developerInvocation
  $childExitCode = $LASTEXITCODE
  if ($childExitCode -ne 0) {
    exit $childExitCode
  }
  return
}

$commandToken = $invocation[0]
$isBareToolName = $commandToken -eq [IO.Path]::GetFileName($commandToken)
$commandName = [IO.Path]::GetFileNameWithoutExtension($commandToken).ToLowerInvariant()
$resolvedCommand = if ($isBareToolName -and $bundledTools.ContainsKey($commandName)) {
  $bundledTools[$commandName]
} else {
  if (-not $isBareToolName) {
    if (-not (Test-Path -LiteralPath $commandToken -PathType Leaf)) {
      [Console]::Error.WriteLine("Explicit command path was not found after entering the Visual Studio x64 developer environment: $commandToken")
      exit 127
    }
    $commandToken
  } else {
    $commandInfo = Get-Command -Name $commandToken -ErrorAction SilentlyContinue
    if (-not $commandInfo) {
      [Console]::Error.WriteLine("Command was not found after entering the Visual Studio x64 developer environment: $commandToken")
      exit 127
    }
    $commandInfo
  }
}

$commandArguments = @($invocation | Select-Object -Skip 1)
try {
  & $resolvedCommand @commandArguments
  $commandSucceeded = $?
  $commandExitCode = $LASTEXITCODE
} catch {
  [Console]::Error.WriteLine($_.ToString())
  exit 1
}

if (-not $commandSucceeded) {
  if ($commandExitCode -eq 0 -or $null -eq $commandExitCode) {
    exit 1
  }
  exit $commandExitCode
}
if ($commandExitCode -ne 0) {
  exit $commandExitCode
}
