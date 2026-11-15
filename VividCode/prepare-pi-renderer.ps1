param([switch]$DownloadDependencies)
$ErrorActionPreference='Stop'
$renderer=Join-Path $PSScriptRoot 'pi-renderer'
if (!(Test-Path (Join-Path $renderer 'node_modules/mathjax-full/package.json')) -or !(Test-Path (Join-Path $renderer 'node_modules/markdown-it/package.json'))) {
    if(!$DownloadDependencies){throw 'Offline pi renderer missing. Run build.ps1 -DownloadDependencies once.'}
    Push-Location $renderer
    try { & npm.cmd ci --ignore-scripts --no-audit --no-fund; if($LASTEXITCODE -ne 0){throw 'pi renderer preparation failed'} } finally {Pop-Location}
}
