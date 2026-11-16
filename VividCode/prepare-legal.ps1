param([string]$RuntimeDirectory = (Join-Path $PSScriptRoot 'build\Release'))
$ErrorActionPreference = 'Stop'
$manifestPath = Join-Path $PSScriptRoot 'licenses\manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($entry in $manifest.files) {
    $file = Join-Path (Join-Path $PSScriptRoot 'licenses') $entry.path
    if (!(Test-Path -LiteralPath $file) -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ine $entry.sha256) {
        throw "Missing or changed third-party license: $($entry.path)"
    }
}
New-Item -ItemType Directory -Force -Path $RuntimeDirectory | Out-Null
Copy-Item -LiteralPath (Join-Path (Split-Path $PSScriptRoot) 'LICENSE') -Destination (Join-Path $RuntimeDirectory 'LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.txt') -Destination $RuntimeDirectory -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses') -Destination $RuntimeDirectory -Recurse -Force
Write-Output 'Project license and verified third-party notices staged.'
