param([Parameter(Mandatory=$true)][string]$RuntimeDirectory)
$ErrorActionPreference = 'Stop'
$workspace = Join-Path (Split-Path $PSScriptRoot) 'build\location-tests'
$testRoot = Join-Path $workspace ([guid]::NewGuid().ToString('N'))
$key = 'HKCU:\Software\vividcode-test-' + [guid]::NewGuid().ToString('N')
$first = Join-Path $testRoot 'first 中文'
$second = Join-Path $testRoot 'second space'
$legacy = Join-Path $testRoot 'legacy'
$script = Join-Path (Split-Path $PSScriptRoot) 'install-location.ps1'
$envBefore = $env:VIVIDCODE_HOME
try {
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
    & $script -RuntimeDirectory $RuntimeDirectory -HomeDirectory $first -RegistrationKey $key -LegacyHome $legacy -SkipUserEnvironment
    if ((Get-ItemProperty $key).InstallHome -ine $first) { throw 'First location not registered' }
    New-Item -ItemType Directory -Path (Join-Path $first 'semantic\example') -Force | Out-Null
    $data = Join-Path $first 'semantic\example\cache.bin'
    [IO.File]::WriteAllBytes($data,[byte[]](1,2,3,4))
    $hash = (Get-FileHash -LiteralPath $data).Hash; $stamp = (Get-Item -LiteralPath $data).LastWriteTimeUtc.Ticks
    $occupied = Join-Path $testRoot 'occupied'; New-Item -ItemType Directory -Path $occupied | Out-Null
    Set-Content -LiteralPath (Join-Path $occupied 'keep.txt') 'unrelated'
    $rejected = $false
    try { & $script -RuntimeDirectory $RuntimeDirectory -HomeDirectory $occupied -RegistrationKey $key -LegacyHome $legacy -SkipUserEnvironment } catch { $rejected = $true }
    if (!$rejected -or !(Test-Path -LiteralPath (Join-Path $occupied 'keep.txt'))) { throw 'Nonempty destination not protected' }
    $lock = [IO.File]::Open((Join-Path $first 'runtime.lock'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try {
        $rejected = $false
        try { & $script -RuntimeDirectory $RuntimeDirectory -HomeDirectory $second -RegistrationKey $key -LegacyHome $legacy -SkipUserEnvironment } catch { $rejected = $true }
        if (!$rejected -or (Test-Path -LiteralPath $second)) { throw 'Active runtime was not protected' }
    } finally { $lock.Dispose() }
    & $script -RuntimeDirectory $RuntimeDirectory -HomeDirectory $second -RegistrationKey $key -LegacyHome $legacy -SkipUserEnvironment
    if (Test-Path -LiteralPath $first) { throw 'Old installation left behind' }
    $copied = Join-Path $second 'semantic\example\cache.bin'
    if ((Get-FileHash -LiteralPath $copied).Hash -ne $hash -or (Get-Item -LiteralPath $copied).LastWriteTimeUtc.Ticks -ne $stamp) { throw 'Cache changed during move' }
    if ((Get-ItemProperty $key).InstallHome -ine $second) { throw 'New location not registered' }
    $env:VIVIDCODE_HOME = $second
    $project = Join-Path $testRoot 'project'; New-Item -ItemType Directory -Path $project | Out-Null
    Set-Content -LiteralPath (Join-Path $project 'main.cpp') 'int main() { return 0; }'
    Push-Location $project
    try { & (Join-Path $second 'runtime\vividcode.exe') -g; if ($LASTEXITCODE) { throw 'CLI generation in relocated runtime failed' } } finally { Pop-Location }
    $p = Start-Process -FilePath (Join-Path $project '.vividcode\vividcode-viewer.exe') -ArgumentList '--verify-startup','-platform','offscreen' -WindowStyle Hidden -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw 'Project launcher after relocation failed' }
    & $script -RuntimeDirectory $RuntimeDirectory -HomeDirectory $second -RegistrationKey $key -LegacyHome $legacy -SkipUserEnvironment
    if ((Get-FileHash -LiteralPath $copied).Hash -ne $hash -or (Get-ItemProperty $key).InstallHome -ine $second) { throw 'Repeated installation changed data/location' }
    Write-Output 'PASS: custom path, nonempty destination, runtime lock, migration cleanup, cache hashes/timestamps, CLI, launcher and repeated installation'
} finally {
    $env:VIVIDCODE_HOME = $envBefore
    if ($key -notlike 'HKCU:\Software\vividcode-test-*') { throw 'Unsafe test registry cleanup' }
    Remove-Item -LiteralPath $key -Recurse -Force -ErrorAction SilentlyContinue
    # Keep the isolated filesystem evidence for inspection.
}
