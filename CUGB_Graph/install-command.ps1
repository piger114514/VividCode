# Register the built command for the current Windows user. No machine-wide PATH change.
$ErrorActionPreference = 'Stop'
$runtime = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'build\Release'))
if (!(Test-Path -LiteralPath (Join-Path $runtime 'vividcode.exe'))) { throw 'Run build.ps1 first.' }
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$entries = @($userPath -split ';' | Where-Object { $_ })
if (!($entries | Where-Object { $_.TrimEnd('\') -ieq $runtime.TrimEnd('\') })) {
    $newPath = ($entries + $runtime) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
}
# Also make the command available in this PowerShell process immediately.
if (!(($env:Path -split ';') | Where-Object { $_.TrimEnd('\') -ieq $runtime.TrimEnd('\') })) {
    $env:Path = $env:Path.TrimEnd(';') + ';' + $runtime
}
Write-Host "Registered vividcode: $runtime"
Write-Host 'Open a new terminal (restart its parent app if needed), then run: vividcode --help'
