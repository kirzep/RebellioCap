[CmdletBinding(SupportsShouldProcess)]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

try {
    if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        throw 'LOCALAPPDATA is unavailable.'
    }
    $configDirectory = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'RebellioCap'))
    $names = @('config.json', 'config.last-good.json', 'config.json.tmp', 'config.last-good.json.tmp')
    $files = @($names | ForEach-Object { Join-Path $configDirectory $_ } |
        Where-Object { Test-Path -LiteralPath $_ })

    if ($files.Count -eq 0) {
        Write-Host 'Setup is already reset. Open RebellioCap to start again.'
        exit 0
    }

    if (-not $WhatIfPreference) {
        $running = @(Get-Process -Name 'RebellioCap', 'RebellioCap.Engine' -ErrorAction SilentlyContinue)
        if ($running.Count -gt 0) {
            throw 'Close RebellioCap normally before resetting setup. Stop any active recording first.'
        }
    }

    foreach ($path in @($configDirectory) + $files) {
        $item = Get-Item -LiteralPath $path -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to reset a linked path: $path"
        }
    }

    $backupDirectory = Join-Path $configDirectory ('setup-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    if ($PSCmdlet.ShouldProcess($configDirectory, "Archive setup files to $backupDirectory")) {
        New-Item -ItemType Directory -Path $backupDirectory | Out-Null
        foreach ($file in $files) {
            Move-Item -LiteralPath $file -Destination $backupDirectory
        }
        Write-Host 'Setup reset. Open RebellioCap to start from the welcome screen.'
        Write-Host "Previous settings: $backupDirectory"
        Write-Host 'Video clips and installed dependencies were preserved.'
    }
    exit 0
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
