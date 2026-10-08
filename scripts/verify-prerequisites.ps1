[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Add-Check([string] $name, [bool] $passed, [string] $detail) {
  [PSCustomObject]@{ name = $name; passed = $passed; detail = $detail }
}

function Get-ToolVersion([string] $path, [string] $argument) {
  try {
    return (& $path $argument 2>$null | Select-Object -First 1).Trim()
  } catch {
    return $null
  }
}

$checks = [System.Collections.Generic.List[object]]::new()
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = $null
if (Test-Path -LiteralPath $vswhere) {
  $vsPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
}

$clPath = $null
$msvcVersion = $null
if ($vsPath) {
  $msvcRoot = Join-Path $vsPath 'VC\Tools\MSVC'
  $toolset = Get-ChildItem -LiteralPath $msvcRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
  if ($toolset) {
    $msvcVersion = $toolset.Name
    $clPath = Join-Path $toolset.FullName 'bin\Hostx64\x64\cl.exe'
  }
}
$msvcDetail = if ($msvcVersion) { "MSVC $msvcVersion" } else { 'x64 MSVC was not found' }
$checks.Add((Add-Check 'msvc_x64' ([bool]($clPath -and (Test-Path -LiteralPath $clPath))) $msvcDetail))

$kitsRoot = $null
try { $kitsRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10 } catch {}
$sdkVersion = $null
if ($kitsRoot -and (Test-Path -LiteralPath $kitsRoot)) {
  $sdk = Get-ChildItem -LiteralPath (Join-Path $kitsRoot 'Lib') -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' -and (Test-Path -LiteralPath (Join-Path $_.FullName 'um\x64')) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
  if ($sdk) { $sdkVersion = $sdk.Name }
}
$sdkDetail = if ($sdkVersion) { "Windows SDK $sdkVersion" } else { 'Windows SDK x64 libraries were not found' }
$checks.Add((Add-Check 'windows_sdk_x64' ([bool]$sdkVersion) $sdkDetail))

$cmakePath = $null
$ninjaPath = $null
if ($vsPath) {
  $cmakePath = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
  $ninjaPath = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
}
$cmakeVersion = if ($cmakePath -and (Test-Path -LiteralPath $cmakePath)) { Get-ToolVersion $cmakePath '--version' } else { $null }
$ninjaVersion = if ($ninjaPath -and (Test-Path -LiteralPath $ninjaPath)) { Get-ToolVersion $ninjaPath '--version' } else { $null }
$cmakeDetail = if ($cmakeVersion) { $cmakeVersion } else { 'Visual Studio-bundled CMake was not found' }
$ninjaDetail = if ($ninjaVersion) { "Ninja $ninjaVersion" } else { 'Visual Studio-bundled Ninja was not found' }
$checks.Add((Add-Check 'visual_studio_cmake' ([bool]$cmakeVersion) $cmakeDetail))
$checks.Add((Add-Check 'visual_studio_ninja' ([bool]$ninjaVersion) $ninjaDetail))

$os = Get-CimInstance Win32_OperatingSystem
$build = [int]($os.Version.Split('.')[2])
$supportedWindows = ([version]$os.Version -ge [version]'10.0.19045')
$checks.Add((Add-Check 'windows_10_22h2_or_newer' $supportedWindows "$($os.Caption) $($os.Version) (build $build)"))

$nvidiaSmi = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
if (-not $nvidiaSmi) { $nvidiaSmi = Get-Command nvidia-smi -ErrorAction SilentlyContinue }
$gpuLine = $null
if ($nvidiaSmi) {
  try { $gpuLine = (& $nvidiaSmi.Source '--query-gpu=name,driver_version' '--format=csv,noheader' 2>$null | Select-Object -First 1).Trim() } catch {}
}
$driverVersion = $null
if ($gpuLine -match '^(.*),\s*([0-9]+\.[0-9]+)$') {
  $driverVersion = $matches[2]
}
$nvidiaDetail = if ($gpuLine) { $gpuLine } else { 'nvidia-smi was not found or returned no GPU' }
$checks.Add((Add-Check 'nvidia_smi' ([bool]$gpuLine) $nvidiaDetail))
$driverSupported = $driverVersion -and ([version]$driverVersion -ge [version]'570.00')
$driverDetail = if ($driverVersion) { "NVIDIA driver $driverVersion" } else { 'NVIDIA driver version was not found' }
$checks.Add((Add-Check 'nvidia_driver_570_or_newer' ([bool]$driverSupported) $driverDetail))

$passed = -not ($checks.passed -contains $false)
[PSCustomObject]@{
  passed = $passed
  checks = $checks
} | ConvertTo-Json -Depth 4 -Compress

if (-not $passed) { exit 1 }
