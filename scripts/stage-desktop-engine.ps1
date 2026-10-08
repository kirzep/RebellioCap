[CmdletBinding()]
param([string] $BuildDirectory)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime-copy.ps1')
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $BuildDirectory) {
    # Prefer the newest release engine so an older hardware build cannot silently
    # replace freshly compiled recorder behavior during beforeBuildCommand.
    $releaseEngines = @('windows-hardware-release', 'windows-release') |
        ForEach-Object { Join-Path $workspace "build/$_/RebellioCap.Engine.exe" } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        ForEach-Object { Get-Item -LiteralPath $_ } |
        Sort-Object LastWriteTimeUtc -Descending
    if ($releaseEngines) { $BuildDirectory = $releaseEngines[0].DirectoryName }
    elseif (Test-Path -LiteralPath (Join-Path $workspace 'build/windows-debug/RebellioCap.Engine.exe')) {
        $BuildDirectory = Join-Path $workspace 'build/windows-debug'
    } else { $BuildDirectory = Join-Path $workspace 'build/windows-hardware-release' }
}
$source = (Resolve-Path -LiteralPath $BuildDirectory).Path
$destination = [IO.Path]::GetFullPath((Join-Path $workspace 'apps/desktop/src-tauri/resources/engine'))
if (-not $destination.StartsWith($workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Staging target is outside the workspace.' }
$allowlist = @('RebellioCap.Engine.exe', 'avcodec-63.dll', 'avformat-63.dll', 'avutil-61.dll')
$editorTools = Join-Path $workspace '.tools/vcpkg_installed/x64-windows/tools/ffmpeg'
$editorNames = @('ffmpeg.exe', 'ffprobe.exe', 'avfilter-12.dll', 'swresample-7.dll', 'swscale-10.dll', 'z.dll')
foreach ($name in ($editorNames + @('avcodec-63.dll', 'avformat-63.dll', 'avutil-61.dll'))) {
    Copy-RuntimeFile (Join-Path $editorTools $name) (Join-Path $source $name)
}
$allowlist += $editorNames
# App-local release CRT: friends do not need Visual Studio or a separate redistributable.
$crtNames = @('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
$redistRoot = Join-Path $vsRoot 'VC/Redist/MSVC'
$crt = Get-ChildItem -LiteralPath $redistRoot -Filter 'Microsoft.VC*.CRT' -Directory -Recurse | Where-Object { $_.FullName -match '[\\/]x64[\\/]' } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $crt) { throw 'Release Visual C++ runtime not found.' }
foreach ($name in $crtNames) { Copy-RuntimeFile (Join-Path $crt.FullName $name) (Join-Path $source $name) }
$allowlist += $crtNames
$entries = @()
foreach ($name in $allowlist) {
    $path = Join-Path $source $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required runtime is missing: $name" }
    if ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Runtime cannot be a reparse point: $name" }
    $entries += [ordered]@{ name = $name; sha256 = (Get-RuntimeFileHash $path); size = (Get-Item -LiteralPath $path).Length }
}
New-Item -ItemType Directory -Path $destination -Force | Out-Null
foreach ($file in Get-ChildItem -LiteralPath $destination -Force) {
    if ($file.Name -notin ($allowlist + @('manifest.json', '.gitkeep'))) { throw "Unexpected staged resource: $($file.Name)" }
    if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Staged resource cannot be a reparse point: $($file.Name)" }
}
foreach ($entry in $entries) {
    $target = Join-Path $destination $entry.name
    Copy-RuntimeFile (Join-Path $source $entry.name) $target
    if ((Get-RuntimeFileHash $target) -ne $entry.sha256) { throw "Staged hash mismatch: $($entry.name)" }
}
$manifest = [ordered]@{ schemaVersion = 1; files = $entries } | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText((Join-Path $destination 'manifest.json'), $manifest, [Text.UTF8Encoding]::new($false))
Write-Output "Verified $($entries.Count) engine resources in $destination"
