param(
    [string]$QtRoot = 'C:\Qt\6.11.2\msvc2022_64',
    [string]$CMake = 'C:\Qt\Tools\CMake_64\bin\cmake.exe',
    [ValidateSet('cpp','csharp','python','javascript','java')][string[]]$Languages = @(),
    [switch]$DownloadDependencies,
    [switch]$RunTests
)
$ErrorActionPreference = 'Stop'
$buildDir = Join-Path $PSScriptRoot 'build'
& "$PSScriptRoot\prepare-pi-renderer.ps1" -DownloadDependencies:$DownloadDependencies
if ($Languages -contains 'cpp' -or $Languages -contains 'python') { & "$PSScriptRoot\prepare-analyzer.ps1" }
if ($Languages -contains 'csharp') { & "$PSScriptRoot\prepare-csharp-analyzer.ps1" -RestoreDependencies:$DownloadDependencies }
if ($Languages -contains 'javascript') { & "$PSScriptRoot\prepare-javascript-analyzer.ps1" -DownloadDependencies:$DownloadDependencies }
if ($Languages -contains 'java') { & "$PSScriptRoot\prepare-java-analyzer.ps1" }
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
if ($RunTests) {
    $tests = @('prototype_smoke','ai_explanations','pi_agent','cli_lifecycle','generation_commands','local_environment')
    if ($Languages -contains 'cpp') { $tests += @('semantic_backend','semantic_ui') }
    if ($Languages -contains 'csharp') { $tests += @('csharp_backend','csharp_ui') }
    if ($Languages -contains 'python') { $tests += @('python_backend','python_ui') }
    if ($Languages -contains 'javascript') { $tests += @('javascript_backend','javascript_ui') }
    if ($Languages -contains 'java') { $tests += @('java_backend','java_ui') }
    & (Join-Path (Split-Path $CMake) 'ctest.exe') --test-dir $buildDir -C Release -R ('^(' + ($tests -join '|') + ')$') --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Selected checks failed; verify required local environments.' }
}
Write-Host "Ready: $buildDir\Release\vividcode.exe and vividcode-viewer.exe"
