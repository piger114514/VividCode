param([string]$HomeDirectory)
$ErrorActionPreference = 'Stop'
try {
    Remove-Item Env:VIVIDCODE_HOME -ErrorAction SilentlyContinue
    $package = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'runtime\installation.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if (!$HomeDirectory) { $HomeDirectory = (Get-ItemProperty 'HKCU:\Software\vividcode' -ErrorAction SilentlyContinue).InstallHome }
    if (!$HomeDirectory) { $HomeDirectory = Join-Path $env:LOCALAPPDATA 'vividcode' }
    & (Join-Path $PSScriptRoot 'install-location.ps1') -RuntimeDirectory (Join-Path $PSScriptRoot 'runtime') -HomeDirectory $HomeDirectory -Languages @($package.languages)
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'install-error.txt'), $_.Exception.Message, [Text.UTF8Encoding]::new($false))
    exit 1
}
