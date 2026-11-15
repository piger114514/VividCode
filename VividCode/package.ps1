param(
    [Parameter(Mandatory=$true)][ValidatePattern('^\d+\.\d+\.\d+(\.\d+)?$')][string]$Version,
    [string]$RuntimeDirectory,
    [string]$OutputDirectory,
    [ValidateSet('cpp','csharp','python','javascript','java')][string[]]$Languages = @('cpp','csharp','python','javascript','java'),
    [string]$Compiler,
    [switch]$StageOnly
)
$ErrorActionPreference = 'Stop'
if (!$RuntimeDirectory) { $RuntimeDirectory = Join-Path $PSScriptRoot 'build\Release' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot 'dist' }
if (!$StageOnly) {
    if (!$Compiler) {
        $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
        if ($command) { $Compiler = $command.Source }
        foreach ($candidate in @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")) {
            if (!$Compiler -and (Test-Path -LiteralPath $candidate)) { $Compiler = $candidate }
        }
    }
    if (!$Compiler -or !(Test-Path -LiteralPath $Compiler -PathType Leaf)) {
        throw 'Inno Setup compiler not found. Install it yourself or supply -Compiler path\ISCC.exe. No tools are downloaded automatically.'
    }
    $Compiler = [IO.Path]::GetFullPath($Compiler)
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$stage = Join-Path $output ('stage-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
# Explicit HomeDirectory: packaging never touches the current user's installation or PATH.
& (Join-Path $PSScriptRoot 'install-runtime.ps1') -RuntimeDirectory $RuntimeDirectory -HomeDirectory $stage -Languages $Languages
$metadataPath = Join-Path $stage 'runtime\installation.json'
$metadata = Get-Content -LiteralPath $metadataPath -Raw -Encoding UTF8 | ConvertFrom-Json
$metadata.source = 'packaged-release'
$metadata | Add-Member -NotePropertyName packageVersion -NotePropertyValue $Version -Force
$metadata | ConvertTo-Json | Set-Content -LiteralPath $metadataPath -Encoding UTF8
foreach ($name in @('install-runtime.ps1','install-command.ps1','install-location.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'installer\install-package.ps1') -Destination $stage
if ($StageOnly) { Write-Output "Staged package (not an EXE): $stage"; return }
$filename = "vividcode-Setup-$Version-x64"
& $Compiler "/DPackageVersion=$Version" "/DPayload=$stage" "/O$output" "/F$filename" (Join-Path $PSScriptRoot 'installer\vividcode.iss')
if ($LASTEXITCODE -ne 0) { throw "Installer compiler failed: $LASTEXITCODE (staging retained at $stage)" }
$exe = Join-Path $output "$filename.exe"
if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Compiler did not produce the expected EXE.' }
Get-FileHash -LiteralPath $exe -Algorithm SHA256 | Format-List
Write-Output "Installer: $exe"
# Keep the exact staged payload for inspection; never recursively delete a caller-supplied directory.
