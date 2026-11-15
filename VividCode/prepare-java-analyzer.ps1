param([string]$Destination = (Join-Path $PSScriptRoot 'build/Release/java-analyzer'))
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'java-analyzer/VividCodeJava.java') -Destination $Destination -Force
Write-Output "Java analyzer prepared: $Destination (requires local JDK 17+)"
