param(
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
    [string]$HomeDirectory,
    [ValidateSet('cpp','csharp','python','javascript','java')][string[]]$Languages = @()
)
$ErrorActionPreference = 'Stop'
if (!$HomeDirectory) { $HomeDirectory = $env:VIVIDCODE_HOME }
if (!$HomeDirectory) { $HomeDirectory = (Get-ItemProperty 'HKCU:\Software\vividcode' -ErrorAction SilentlyContinue).InstallHome }
if (!$HomeDirectory) { $HomeDirectory = Join-Path $env:LOCALAPPDATA 'vividcode' }
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
    'Qt6Sql.dll','Qt6Network.dll','sqldrivers\qsqlite.dll','platforms\qwindows.dll','tls\qschannelbackend.dll','Qt6Svg.dll','imageformats\qsvg.dll','pi-renderer\render.cjs','pi-renderer\node_modules\mathjax-full\package.json','pi-renderer\node_modules\markdown-it\package.json')
if ($Languages -contains 'python') { $required += @('analyzer\python_indexer.py','analyzer\analyzer.py') }
if ($Languages -contains 'java') { $required += @('java-analyzer\VividCodeJava.java') }
if ($Languages -contains 'cpp') { $required += @('analyzer\analyzer.py','analyzer\extractor.py','analyzer\clang_bridge.py') }
if ($Languages -contains 'csharp') { $required += @('csharp-analyzer\vividcode-csharp.dll',
    'csharp-analyzer\vividcode-csharp.runtimeconfig.json','csharp-analyzer\vividcode-csharp.deps.json',
    'csharp-analyzer\Microsoft.CodeAnalysis.CSharp.Workspaces.dll','csharp-analyzer\Microsoft.CodeAnalysis.Workspaces.MSBuild.dll',
    'csharp-analyzer\BuildHost-netcore\Microsoft.CodeAnalysis.Workspaces.MSBuild.BuildHost.dll') }
if ($Languages -contains 'javascript') { $required += @('javascript-analyzer\indexer.cjs','javascript-analyzer\runtime.json',
    'javascript-analyzer\typescript\LICENSE.txt','javascript-analyzer\typescript\lib\typescript.js','javascript-analyzer\typescript\lib\lib.d.ts') }
$required = @($required | Select-Object -Unique)
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
    foreach ($name in @('platforms','sqldrivers','styles','imageformats','iconengines','networkinformation','tls','generic')) {
        $folder = Join-Path $source $name
        if (Test-Path -LiteralPath $folder) { AssertPlainTree $folder; $files += Get-ChildItem -LiteralPath $folder -Recurse -File -Force }
    }
    # Analyzer sources are selected by language. Never sweep old build runtime
    # directories: that would silently reinstall Node/Python/libclang leftovers.
    $renderer = Join-Path $source 'pi-renderer'; AssertPlainTree $renderer
    $files += Get-ChildItem -LiteralPath $renderer -Recurse -File
    foreach ($name in @($required | Where-Object { $_ -like 'analyzer\*' })) { $files += Get-Item -LiteralPath (Join-Path $source $name) }
    if ($Languages -contains 'java') { $files += Get-Item -LiteralPath (Join-Path $source 'java-analyzer\VividCodeJava.java') }
    if ($Languages -contains 'javascript') {
        $files += Get-Item -LiteralPath (Join-Path $source 'javascript-analyzer\indexer.cjs')
        $files += Get-Item -LiteralPath (Join-Path $source 'javascript-analyzer\runtime.json')
        $compiler = Join-Path $source 'javascript-analyzer\typescript'; AssertPlainTree $compiler
        $files += Get-ChildItem -LiteralPath $compiler -Recurse -File -Force
    }
    if ($Languages -contains 'csharp') {
        $component = Join-Path $source 'csharp-analyzer'; AssertPlainTree $component
        $files += Get-ChildItem -LiteralPath $component -Recurse -File -Force | Where-Object { $_.Extension -in @('.exe','.dll','.json','.config','.md','.txt') }
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
    $metadata = @{app='vividcode';schema=1;source=$source;languages=@($Languages | Select-Object -Unique);localEnvironments=$true;installedUtc=[DateTime]::UtcNow.ToString('o')}
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
