function Get-RuntimeFileHash([string] $Path) {
    # Tauri's build environment can omit the module exporting Get-FileHash.
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}

function Copy-RuntimeFile([string] $Source, [string] $Destination) {
    $sourceFile = Get-Item -LiteralPath $Source -ErrorAction Stop
    if ($sourceFile.PSIsContainer -or ($sourceFile.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Runtime source must be a regular file: $Source"
    }
    if (Test-Path -LiteralPath $Destination) {
        $destinationFile = Get-Item -LiteralPath $Destination -ErrorAction Stop
        if ($destinationFile.PSIsContainer -or ($destinationFile.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Runtime destination must be a regular file: $Destination"
        }
        if ($sourceFile.Length -eq $destinationFile.Length -and
            (Get-RuntimeFileHash $Source) -eq (Get-RuntimeFileHash $Destination)) {
            return
        }
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force -ErrorAction Stop
}
