param(
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
    [string]$HomeDirectory = $(if ($env:VIVIDCODE_HOME) { $env:VIVIDCODE_HOME } else { Join-Path $env:LOCALAPPDATA 'vividcode' })
)
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath($RuntimeDirectory)
$sharedHome = [IO.Path]::GetFullPath($HomeDirectory)
$destination = Join-Path $sharedHome 'runtime'
if ($source.TrimEnd('\') -ieq $destination.TrimEnd('\')) { throw 'Install from a built runtime, not the active shared runtime.' }
function AssertPlainParents([string]$path) {
    $current = [IO.Path]::GetFullPath($path)
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked path rejected: $current" }
        }
        $parent = [IO.Path]::GetDirectoryName($current)
        if ($parent -eq $current) { break }; $current = $parent
    }
}
function AssertPlainTree([string]$path) {
    AssertPlainParents $path
    if (Test-Path -LiteralPath $path) {
        Get-ChildItem -LiteralPath $path -Recurse -Force | ForEach-Object {
            if ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked entry rejected: $($_.FullName)" }
        }
    }
}
function RemoveOwnedTemporary([string]$path) {
    $resolved = [IO.Path]::GetFullPath($path)
    $boundary = $sharedHome.TrimEnd('\') + '\'
    $leaf = [IO.Path]::GetFileName($resolved)
    if (!$resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase) -or $leaf -notmatch '^runtime-(staging|previous)-[a-f0-9]{32}$') { throw 'Unsafe temporary cleanup path' }
    AssertPlainTree $resolved
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
function Digest([string]$path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($path)
    try { [Convert]::ToBase64String($algorithm.ComputeHash($stream)) } finally { $stream.Dispose(); $algorithm.Dispose() }
}
$required = @('vividcode.exe','vividcode-viewer.exe','vividcode-launcher.exe','Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll',
    'platforms\qwindows.dll','analyzer\analyzer.py','analyzer\extractor.py','analyzer\clang_bridge.py',
    'analyzer\python\python.exe','analyzer\python\python313.dll','analyzer\python\python313.zip',
    'analyzer\python\python313._pth','analyzer\python\_sqlite3.pyd','analyzer\python\sqlite3.dll',
    'analyzer\lib\libclang.dll','analyzer\lib\clang\23\include\stddef.h','analyzer\runtime.json')
AssertPlainParents $sharedHome
foreach ($name in $required) {
    if (!(Test-Path -LiteralPath (Join-Path $source $name) -PathType Leaf)) { throw "Incomplete source runtime: $name" }
}
AssertPlainTree $destination
if (Test-Path -LiteralPath $destination) {
    $installation = Get-Content -LiteralPath (Join-Path $destination 'installation.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($installation.app -ne 'vividcode' -or $installation.schema -ne 1) { throw 'Unknown existing shared runtime; not overwritten.' }
}
New-Item -ItemType Directory -Path $sharedHome -Force | Out-Null
$lockPath = Join-Path $sharedHome 'runtime.lock'
AssertPlainParents $lockPath
$guard = $null
$stage = Join-Path $sharedHome ('runtime-staging-' + [guid]::NewGuid().ToString('N'))
$previous = Join-Path $sharedHome ('runtime-previous-' + [guid]::NewGuid().ToString('N'))
$swapped = $false
try {
    try { $guard = [IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None) }
    catch { throw 'Shared vividcode runtime is in use. Close its viewers and CLI commands before installing.' }
    New-Item -ItemType Directory -Path $stage | Out-Null
    $files = @()
    foreach ($name in @('vividcode.exe','vividcode-viewer.exe','vividcode-launcher.exe')) { $files += Get-Item -LiteralPath (Join-Path $source $name) }
    $files += Get-ChildItem -LiteralPath $source -File -Filter '*.dll'
    foreach ($name in @('analyzer','platforms','styles','imageformats','iconengines','networkinformation','tls','generic')) {
        $folder = Join-Path $source $name
        if (Test-Path -LiteralPath $folder) { AssertPlainTree $folder; $files += Get-ChildItem -LiteralPath $folder -Recurse -File -Force }
    }
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($source.TrimEnd('\').Length).TrimStart('\')
        if ($relative -match '(^|\\)(__pycache__|\.vividcode)(\\|$)') { continue }
        AssertPlainParents $file.FullName
        $target = Join-Path $stage $relative
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target
        if ((Digest $file.FullName) -ne (Digest $target)) { throw "Installed file hash mismatch: $relative" }
    }
    $metadata = @{app='vividcode';schema=1;source=$source;installedUtc=[DateTime]::UtcNow.ToString('o')}
    [IO.File]::WriteAllText((Join-Path $stage 'installation.json'),($metadata | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
    foreach ($name in $required) { if (!(Test-Path -LiteralPath (Join-Path $stage $name))) { throw "Incomplete staged runtime: $name" } }
    if (Test-Path -LiteralPath $destination) { Move-Item -LiteralPath $destination -Destination $previous }
    try { Move-Item -LiteralPath $stage -Destination $destination; $swapped = $true }
    catch {
        if (Test-Path -LiteralPath $previous) { Move-Item -LiteralPath $previous -Destination $destination }
        throw
    }
    RemoveOwnedTemporary $previous
    Write-Output "Shared vividcode runtime installed: $destination"
} finally {
    if (!$swapped) { RemoveOwnedTemporary $stage }
    if ($guard) { $guard.Dispose() }
}
