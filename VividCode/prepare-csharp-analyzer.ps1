param([string]$Destination = "$PSScriptRoot\build\Release\csharp-analyzer", [switch]$RestoreDependencies)
$ErrorActionPreference = 'Stop'
$project = Join-Path $PSScriptRoot 'csharp-analyzer\vividcode-csharp.csproj'
# Fixed Roslyn packages, dependency lock committed alongside source. Runtime is
# framework-dependent: the analyzed project's .NET SDK remains a prerequisite.
if ($RestoreDependencies) {
    & dotnet restore $project --locked-mode
    if ($LASTEXITCODE -ne 0) { throw 'C# analyzer dependency restore failed' }
}
& dotnet publish $project -c Release --no-restore --self-contained false -o $Destination
if ($LASTEXITCODE -ne 0) { throw 'C# analyzer publish failed' }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'csharp-analyzer\packages.lock.json') -Destination $Destination -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'csharp-analyzer\THIRD_PARTY_NOTICES.md') -Destination $Destination -Force
Write-Output "C# analyzer ready: $Destination"
