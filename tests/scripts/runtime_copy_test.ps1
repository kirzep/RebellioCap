$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '../../scripts/runtime-copy.ps1')
function Get-FileHash { throw 'Unavailable in the Tauri build environment.' }
$directory = Join-Path ([IO.Path]::GetTempPath()) ('rebcap-runtime-copy-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $directory
$source = Join-Path $directory 'source.dll'
$destination = Join-Path $directory 'destination.dll'
$locked = $null
function Assert([bool] $Condition, [string] $Message) { if (!$Condition) { throw $Message } }
try {
    [IO.File]::WriteAllBytes($source, [byte[]](1, 2, 3, 4))
    Copy-RuntimeFile $source $destination
    Assert ([Convert]::ToBase64String([IO.File]::ReadAllBytes($destination)) -eq 'AQIDBA==') 'Missing destination was not copied.'
    $before = (Get-Item -LiteralPath $destination).LastWriteTimeUtc
    # Windows permits hashing a loaded/read-shared DLL but forbids overwriting it.
    $locked = [IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    Copy-RuntimeFile $source $destination
    Assert ((Get-Item -LiteralPath $destination).LastWriteTimeUtc -eq $before) 'Identical locked runtime was rewritten.'
    [IO.File]::WriteAllBytes($source, [byte[]](4, 3, 2, 1))
    $rejected = $false
    try { Copy-RuntimeFile $source $destination } catch { $rejected = $true }
    Assert $rejected 'Changed locked runtime must fail rather than silently use stale bytes.'
    Assert ([Convert]::ToBase64String([IO.File]::ReadAllBytes($destination)) -eq 'AQIDBA==') 'Rejected copy changed the locked runtime.'
    $locked.Dispose(); $locked = $null
    Copy-RuntimeFile $source $destination
    Assert ([Convert]::ToBase64String([IO.File]::ReadAllBytes($destination)) -eq 'BAMCAQ==') 'Changed unlocked runtime was not copied.'
    $rejected = $false
    try { Copy-RuntimeFile (Join-Path $directory 'missing.dll') $destination } catch { $rejected = $true }
    Assert $rejected 'Missing source was accepted.'
    Write-Output 'Runtime copy checks passed: new, identical locked, changed locked, changed unlocked, missing source.'
} finally {
    if ($locked) { $locked.Dispose() }
    $resolvedDirectory = [IO.Path]::GetFullPath($directory)
    $expectedParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (!$resolvedDirectory.StartsWith($expectedParent, [StringComparison]::OrdinalIgnoreCase) -or
        !([IO.Path]::GetFileName($resolvedDirectory)).StartsWith('rebcap-runtime-copy-')) { throw 'Unsafe test cleanup path.' }
    Remove-Item -LiteralPath $resolvedDirectory -Recurse -Force
}
