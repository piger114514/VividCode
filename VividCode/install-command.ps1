param([string]$RuntimeDirectory = (Join-Path $PSScriptRoot 'build\Release'),
    [ValidateSet('cpp','csharp','python','javascript','java')][string[]]$Languages = @())
# Install one shared runtime and register the current user's CLI.
$ErrorActionPreference = 'Stop'
if (!$env:VIVIDCODE_HOME) {
    $homePath = (Get-ItemProperty 'HKCU:\Software\vividcode' -ErrorAction SilentlyContinue).InstallHome
    if (!$homePath) { $homePath = Join-Path $env:LOCALAPPDATA 'vividcode' }
    & (Join-Path $PSScriptRoot 'install-location.ps1') -RuntimeDirectory $RuntimeDirectory -HomeDirectory $homePath -Languages $Languages
    return
}
& (Join-Path $PSScriptRoot 'install-runtime.ps1') -RuntimeDirectory $RuntimeDirectory -Languages $Languages
$sharedHome = if ($env:VIVIDCODE_HOME) { $env:VIVIDCODE_HOME } else { Join-Path $env:LOCALAPPDATA 'vividcode' }
$runtime = [IO.Path]::GetFullPath((Join-Path $sharedHome 'runtime'))
$oldRuntimes = @(
    [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'build\Release')),
    [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'x64\Release')),
    [IO.Path]::GetFullPath((Join-Path (Split-Path $PSScriptRoot) 'CUGB_Graph\build\Release'))
)
function RegisterEntries([string]$path) {
    $entries = @($path -split ';' | Where-Object { $_ -and $oldRuntimes -inotcontains $_.TrimEnd('\') -and $_.TrimEnd('\') -ine $runtime.TrimEnd('\') })
    @($runtime) + $entries
}
$userPath = [Environment]::GetEnvironmentVariable('Path','User')
$newPath = (RegisterEntries $userPath) -join ';'
if ($newPath -cne $userPath) { [Environment]::SetEnvironmentVariable('Path',$newPath,'User') }
$env:Path = (RegisterEntries $env:Path) -join ';'
Write-Host "Registered vividcode: $runtime"
Write-Host 'Reopen existing terminals/IDEs to pick up the updated user PATH.'
