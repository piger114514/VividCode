param(
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
    [Parameter(Mandatory=$true)][string]$HomeDirectory,
    [string[]]$Languages = @(),
    [string]$RegistrationKey = 'HKCU:\Software\vividcode',
    [string]$LegacyHome = (Join-Path $env:LOCALAPPDATA 'vividcode'),
    [switch]$SkipUserEnvironment
)
$ErrorActionPreference = 'Stop'
function Full([string]$path) {
    if (![IO.Path]::IsPathRooted($path)) { throw 'Installation directory must be absolute.' }
    $result = [IO.Path]::GetFullPath($path).TrimEnd('\')
    if ($result -eq [IO.Path]::GetPathRoot($result).TrimEnd('\')) { throw 'A drive root cannot be an installation directory.' }
    return $result
}
function Plain([string]$path) {
    $current = $path
    while ($current) {
        if ((Test-Path -LiteralPath $current) -and ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Linked path rejected: $current" }
        $current = [IO.Path]::GetDirectoryName($current)
    }
    if (Test-Path -LiteralPath $path) {
        foreach ($item in Get-ChildItem -LiteralPath $path -Force -Recurse) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked entry rejected: $($item.FullName)" }
        }
    }
}
function Metadata([string]$locationRoot) {
    $path = Join-Path $locationRoot 'runtime\installation.json'
    if (!(Test-Path -LiteralPath $path)) { throw "Installation metadata missing: $locationRoot" }
    $value = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($value.app -ne 'vividcode' -or $value.schema -ne 1) { throw "Unknown installation: $locationRoot" }
    return $value
}
function DataFiles([string]$locationRoot) {
    @(Get-ChildItem -LiteralPath $locationRoot -File -Recurse -Force | Where-Object {
        $relative = $_.FullName.Substring($locationRoot.Length + 1)
        $relative -notlike 'runtime\*' -and $relative -ne 'runtime.lock'
    })
}
function RemoveOwnedHome([string]$path, [string]$other) {
    $resolved = Full $path
    if ($resolved -ieq $other -or $other.StartsWith($resolved + '\',[StringComparison]::OrdinalIgnoreCase) -or $resolved.StartsWith($other + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe installation cleanup boundary.' }
    Plain $resolved
    [void](Metadata $resolved)
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
$target = Full $HomeDirectory
Plain $target
$registered = Get-ItemProperty -LiteralPath $RegistrationKey -ErrorAction SilentlyContinue
$old = $registered.InstallHome
if (!$old -and (Test-Path -LiteralPath (Join-Path $LegacyHome 'runtime\installation.json'))) { $old = $LegacyHome }
if ($old) { $old = Full $old; Plain $old; $previous = Metadata $old }
$moving = $old -and $old -ine $target
if ($moving -and ($target.StartsWith($old + '\',[StringComparison]::OrdinalIgnoreCase) -or $old.StartsWith($target + '\',[StringComparison]::OrdinalIgnoreCase))) { throw 'Old and new installation directories must not contain one another.' }
if ((!$old -or $moving) -and (Test-Path -LiteralPath $target) -and @(Get-ChildItem -LiteralPath $target -Force).Count) { throw 'Choose an empty destination; unrelated files will not be overwritten.' }
if ($old) {
    foreach ($language in $previous.languages) { if ($Languages -notcontains $language) { throw "Package lacks installed component: $language" } }
    $Languages = @($previous.languages)
}
# A per-user mutex also serializes installers choosing different destinations.
$mutexName = 'Local\vividcode-install-' + [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$mutex = [Threading.Mutex]::new($false, $mutexName)
$held = $false; $guard = $null; $committed = $false; $created = $false; $publishing = $false
$oldPath = [Environment]::GetEnvironmentVariable('Path','User')
$oldEnv = [Environment]::GetEnvironmentVariable('VIVIDCODE_HOME','User')
try {
    try { $held = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $held = $true }
    if (!$held) { throw 'Another vividcode installer is running.' }
    # Recheck registration after acquiring the installer mutex.
    $latest = (Get-ItemProperty -LiteralPath $RegistrationKey -ErrorAction SilentlyContinue).InstallHome
    if ($latest -and (Full $latest) -ine $old) { throw 'Installation location changed; reopen the installer.' }
    if ($moving) {
        $guard = [IO.File]::Open((Join-Path $old 'runtime.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    }
    & (Join-Path $PSScriptRoot 'install-runtime.ps1') -RuntimeDirectory $RuntimeDirectory -HomeDirectory $target -Languages $Languages
    $created = $true
    if ($moving) {
        foreach ($file in DataFiles $old) {
            $relative = $file.FullName.Substring($old.Length + 1)
            $destination = Join-Path $target $relative
            New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destination)) -Force | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $destination
            if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $destination).Hash -or $file.LastWriteTimeUtc.Ticks -ne (Get-Item -LiteralPath $destination).LastWriteTimeUtc.Ticks) { throw "Migrated data mismatch: $relative" }
        }
    }
    # Check the copied native executable before publishing this location.
    $cli = Join-Path $target 'runtime\vividcode.exe'
    $start = [Diagnostics.ProcessStartInfo]::new($cli, '-help')
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.EnvironmentVariables['VIVIDCODE_HOME'] = $target
    $process = [Diagnostics.Process]::Start($start)
    if (!$process.WaitForExit(15000)) { $process.Kill(); throw 'Installed CLI startup timed out.' }
    if ($process.ExitCode -ne 0) { throw 'Installed CLI startup failed.' }
    $process.Dispose()
    $publishing = $true
    New-Item -Path $RegistrationKey -Force | Out-Null
    New-ItemProperty -LiteralPath $RegistrationKey -Name InstallHome -Value $target -PropertyType String -Force | Out-Null
    if (!$SkipUserEnvironment) {
        $runtime = Join-Path $target 'runtime'
        $obsolete = @($runtime, (Join-Path $LegacyHome 'runtime'))
        if ($old) { $obsolete += Join-Path $old 'runtime' }
        $entries = @($oldPath -split ';' | Where-Object { $_ -and $obsolete -inotcontains $_.TrimEnd('\') })
        [Environment]::SetEnvironmentVariable('Path', (@($runtime) + $entries -join ';'), 'User')
        # Existing project launchers already understand this variable.
        [Environment]::SetEnvironmentVariable('VIVIDCODE_HOME', $target, 'User')
    }
    $committed = $true
    if ($moving) {
        Plain $old
        [void](Metadata $old)
        # The old runtime lock prevents old clients from starting during removal.
        foreach ($item in Get-ChildItem -LiteralPath $old -Force) {
            if ($item.Name -eq 'runtime.lock') { continue }
            $resolved = [IO.Path]::GetFullPath($item.FullName)
            if (!$resolved.StartsWith($old + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe old-file cleanup boundary.' }
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
        $guard.Dispose(); $guard = $null
        Remove-Item -LiteralPath (Join-Path $old 'runtime.lock') -Force
        Remove-Item -LiteralPath $old
    }
    Write-Output "Installed vividcode at $target. Reopen terminals to refresh their environment."
} catch {
    if (!$committed) {
        if ($publishing) {
            if ($registered.InstallHome) { New-ItemProperty -LiteralPath $RegistrationKey -Name InstallHome -Value $registered.InstallHome -PropertyType String -Force | Out-Null }
            else { Remove-ItemProperty -LiteralPath $RegistrationKey -Name InstallHome -ErrorAction SilentlyContinue }
            if (!$SkipUserEnvironment) {
                [Environment]::SetEnvironmentVariable('Path',$oldPath,'User')
                [Environment]::SetEnvironmentVariable('VIVIDCODE_HOME',$oldEnv,'User')
            }
        }
        if ($moving -and $created) { RemoveOwnedHome $target $old }
    }
    throw
} finally {
    if ($guard) { $guard.Dispose() }
    if ($held) { $mutex.ReleaseMutex() }; $mutex.Dispose()
}
