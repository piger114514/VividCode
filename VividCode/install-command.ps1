# Register the built command for the current Windows user. No machine-wide PATH change.
$ErrorActionPreference = 'Stop'
$runtime = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'build\Release'))
$legacyRuntime = [IO.Path]::GetFullPath((Join-Path (Split-Path $PSScriptRoot) 'CUGB_Graph\build\Release'))
if (!(Test-Path -LiteralPath (Join-Path $runtime 'vividcode.exe'))) { throw 'Run build.ps1 first.' }
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$entries = @($userPath -split ';' | Where-Object { $_ -and $_.TrimEnd('\') -ine $legacyRuntime.TrimEnd('\') })
if (!($entries | Where-Object { $_.TrimEnd('\') -ieq $runtime.TrimEnd('\') })) { $entries += $runtime }
$newPath = $entries -join ';'
if ($newPath -cne $userPath) {
    [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
}
# Also make the command available in this PowerShell process immediately.
$processEntries = @($env:Path -split ';' | Where-Object { $_ -and $_.TrimEnd('\') -ine $legacyRuntime.TrimEnd('\') })
if (!($processEntries | Where-Object { $_.TrimEnd('\') -ieq $runtime.TrimEnd('\') })) { $processEntries += $runtime }
$env:Path = $processEntries -join ';'
Write-Host "Registered vividcode: $runtime"
Write-Host 'Open a new terminal (restart its parent app if needed), then run: vividcode --help'
