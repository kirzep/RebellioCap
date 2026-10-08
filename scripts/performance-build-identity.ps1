function New-PerformanceSourceArchive([string] $RepositoryRoot, [string] $Destination) {
  $manifestPath = $Destination + '.manifest.json'
  foreach ($path in @($Destination, $manifestPath)) {
    if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite existing evidence: $path" }
  }
  $commit = & git -C $RepositoryRoot rev-parse HEAD
  if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the source commit.' }
  # Read NUL-delimited paths directly: native line splitting loses newline filenames.
  $start = [Diagnostics.ProcessStartInfo]::new('git')
  $start.UseShellExecute = $false
  $start.RedirectStandardOutput = $true
  $start.StandardOutputEncoding = [Text.Encoding]::UTF8
  foreach ($argument in @('-C', $RepositoryRoot, 'ls-files', '-z', '--cached', '--others', '--exclude-standard')) {
    $start.ArgumentList.Add($argument)
  }
  $process = [Diagnostics.Process]::Start($start)
  try {
    $listing = $process.StandardOutput.ReadToEnd()
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw 'Cannot enumerate source files.' }
  } finally { $process.Dispose() }
  $paths = @($listing.Split([char]0, [StringSplitOptions]::RemoveEmptyEntries) | Sort-Object -Unique)
  $files = [Collections.Generic.List[object]]::new()
  $output = [IO.File]::Open($Destination, [IO.FileMode]::CreateNew)
  $zip = [IO.Compression.ZipArchive]::new($output, [IO.Compression.ZipArchiveMode]::Create)
  try {
    foreach ($relative in $paths) {
      if ($relative -match '^artifacts/milestone-1/(manifest\.json|doctor\.ndjson|capture-30m\.ndjson|capture-30m\.stderr\.log|ffprobe-(mp4|mkv)\.json|performance\.json|capture-clips/|performance/)') { continue }
      $path = Join-Path $RepositoryRoot $relative
      if (Test-Path -LiteralPath $path -PathType Container) { throw "Source submodules require separate packaging: $relative" }
      if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
      $item = Get-Item -LiteralPath $path
      if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Source links require separate packaging: $relative" }
      $inputStream = [IO.File]::OpenRead($path)
      $entryStream = $zip.CreateEntry($relative, [IO.Compression.CompressionLevel]::Optimal).Open()
      $hasher = [Security.Cryptography.SHA256]::Create()
      $hashStream = [Security.Cryptography.CryptoStream]::new($entryStream, $hasher, [Security.Cryptography.CryptoStreamMode]::Write)
      try {
        $inputStream.CopyTo($hashStream)
        $hashStream.FlushFinalBlock()
        $files.Add([PSCustomObject]@{
          path = $relative
          bytes = $inputStream.Position
          sha256 = [Convert]::ToHexString($hasher.Hash)
        })
      } finally {
        $hashStream.Dispose()
        $hasher.Dispose()
        $inputStream.Dispose()
      }
    }
  } finally { $zip.Dispose(); $output.Dispose() }
  $result = [PSCustomObject]@{
    path = [IO.Path]::GetFullPath($Destination)
    sha256 = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    manifestPath = [IO.Path]::GetFullPath($manifestPath)
    fileCount = $files.Count
  }
  [PSCustomObject]@{
    schemaVersion = 1
    sourceCommit = ([string]$commit).Trim()
    archiveSha256 = $result.sha256
    files = @($files)
    scope = 'Existing tracked and nonignored untracked files; benchmark evidence excluded. External dependencies and binary/source correspondence are not verified.'
  } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding utf8
  return $result
}

function Get-PerformanceBuildIdentity([string] $RepositoryRoot, [string] $EnginePath) {
  $commit = & git -C $RepositoryRoot rev-parse HEAD
  if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the source commit.' }
  $status = @(& git -C $RepositoryRoot -c core.quotepath=false status --porcelain=v1 --untracked-files=all)
  if ($LASTEXITCODE -ne 0) { throw 'Cannot read the source tree status.' }
  $sourceChanges = @($status | Where-Object {
    $_.Length -le 3 -or $_.Substring(3).Replace('\', '/') -notmatch '^artifacts/milestone-1/(manifest\.json|doctor\.ndjson|capture-30m\.ndjson|capture-30m\.stderr\.log|ffprobe-(mp4|mkv)\.json|performance\.json|capture-clips/|performance/)'
  })
  if (-not (Test-Path -LiteralPath $EnginePath -PathType Leaf)) {
    throw "Engine binary was not found: $EnginePath"
  }
  $binaryPaths = @((Get-Item -LiteralPath $EnginePath).FullName) +
    @(Get-ChildItem -LiteralPath (Split-Path -Parent $EnginePath) -Filter '*.dll' -File |
      Sort-Object Name | ForEach-Object { $_.FullName })
  $binaries = @($binaryPaths | ForEach-Object {
    $file = Get-Item -LiteralPath $_
    [PSCustomObject]@{
      name = $file.Name
      path = $file.FullName
      bytes = $file.Length
      sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
  })
  [PSCustomObject]@{
    sourceCommit = ([string]$commit).Trim()
    sourceTreeDirty = $sourceChanges.Count -gt 0
    sourceStatus = $sourceChanges
    binaries = $binaries
  }
}
