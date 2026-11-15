param(
    [string]$Destination = "$PSScriptRoot\build\Release\javascript-analyzer",
    [string]$TypeScriptDirectory = '',
    [switch]$DownloadDependencies
)
$ErrorActionPreference = 'Stop'
$Destination = [IO.Path]::GetFullPath($Destination)
$cache = Join-Path $PSScriptRoot 'build\analyzer-downloads'
New-Item -ItemType Directory -Force -Path $cache,$Destination | Out-Null
if (!$TypeScriptDirectory) {
    $archive = Join-Path $cache 'typescript-5.9.3.tgz'
    if (!(Test-Path -LiteralPath $archive)) {
        if (!$DownloadDependencies) { throw 'Provide -TypeScriptDirectory or explicitly use -DownloadDependencies for the JS/TS component. Node is never downloaded or copied.' }
        Invoke-WebRequest -Uri 'https://registry.npmjs.org/typescript/-/typescript-5.9.3.tgz' -OutFile $archive
    }
    $hasher = [Security.Cryptography.SHA512]::Create(); $stream = [IO.File]::OpenRead($archive)
    try { $integrity = [Convert]::ToBase64String($hasher.ComputeHash($stream)) } finally { $stream.Dispose(); $hasher.Dispose() }
    if ($integrity -ne 'jl1vZzPDinLr9eUt3J/t7V6FgNEw9QjvBPdysz9KfQDD41fQrC2Y4vKQdiaUpFT4bXlb1RHhLpp8wtm6M5TgSw==') { throw 'TypeScript SHA512 mismatch' }
    $unpack = Join-Path $cache 'javascript-unpack'; New-Item -ItemType Directory -Force -Path $unpack | Out-Null
    & tar.exe -xzf $archive -C $unpack
    if ($LASTEXITCODE -ne 0) { throw 'TypeScript extraction failed' }
    $TypeScriptDirectory = Join-Path $unpack 'package'
} else { $integrity = 'local fixed-version compiler'; $TypeScriptDirectory = [IO.Path]::GetFullPath($TypeScriptDirectory) }
if ((Get-Content -Raw -LiteralPath "$TypeScriptDirectory\package.json" | ConvertFrom-Json).version -ne '5.9.3') { throw 'Expected TypeScript 5.9.3' }
New-Item -ItemType Directory -Force -Path "$Destination\typescript" | Out-Null
Copy-Item -Path "$TypeScriptDirectory\*" -Destination "$Destination\typescript" -Recurse -Force
Copy-Item -LiteralPath "$PSScriptRoot\javascript-analyzer\indexer.cjs" -Destination "$Destination\indexer.cjs" -Force
@{app='vividcode';component='javascript';typescript='5.9.3';typescriptSha512=$integrity;runtime='local Node.js 22/24 x64'} |
    ConvertTo-Json | Set-Content -LiteralPath "$Destination\runtime.json" -Encoding UTF8
Write-Output "Optional JS/TS component ready; no Node runtime copied: $Destination"
