param(
    [string]$QtRoot = 'C:\Qt\6.11.2\msvc2022_64',
    [string]$CMake = 'C:\Qt\Tools\CMake_64\bin\cmake.exe'
)
$ErrorActionPreference = 'Stop'
$buildDir = Join-Path $PSScriptRoot 'build'
& "$PSScriptRoot\prepare-analyzer.ps1"
if (!(Test-Path -LiteralPath "$QtRoot\bin\windeployqt.exe")) { throw "Qt MSVC kit missing: $QtRoot" }
if (!(Test-Path -LiteralPath $CMake)) { throw "CMake missing: $CMake" }
# Qt bundled CMake 3.30.5 crashed on cached configuration on this machine.
# Refresh only CMake configuration; compiled objects remain available.
& $CMake --fresh -S $PSScriptRoot -B $buildDir -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$QtRoot"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $CMake --build $buildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& "$QtRoot\bin\windeployqt.exe" --release --no-translations --no-opengl-sw --no-system-d3d-compiler "$buildDir\Release\vividcode-viewer.exe"
if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed' }
Copy-Item -LiteralPath "$QtRoot\plugins\platforms\qoffscreen.dll" -Destination "$buildDir\Release\platforms\qoffscreen.dll" -Force
& (Join-Path (Split-Path $CMake) 'ctest.exe') --test-dir $buildDir -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Smoke test failed' }
Write-Host "Ready: $buildDir\Release\vividcode.exe and vividcode-viewer.exe"
