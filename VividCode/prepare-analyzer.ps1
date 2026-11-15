param([string]$Destination = "$PSScriptRoot\build\Release\analyzer")
$ErrorActionPreference = 'Stop'
# Source only. Python and LLVM remain in the user's installation.
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
foreach ($name in @('analyzer.py','extractor.py','clang_bridge.py','python_indexer.py')) {
    Copy-Item -LiteralPath (Join-Path "$PSScriptRoot\analyzer" $name) -Destination (Join-Path $Destination $name) -Force
}
Write-Output "Analyzer source ready; use local Python 3.12+ x64 and LLVM: $Destination"
