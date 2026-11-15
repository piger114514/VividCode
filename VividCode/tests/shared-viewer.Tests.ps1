param([Parameter(Mandatory=$true)][string]$RuntimeDirectory, [switch]$CSharp, [switch]$Python, [switch]$JavaScript, [switch]$Java)
$ErrorActionPreference = 'Stop'
$testBase = Join-Path (Split-Path $RuntimeDirectory) 'shared-viewer-test-work'
$testRoot = Join-Path $testBase ([guid]::NewGuid().ToString('N'))
$previousHome = $env:VIVIDCODE_HOME
$env:VIVIDCODE_HOME = Join-Path $testRoot 'shared home'
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
try {
    $languages = if ($Java) { @('java','python') } elseif ($JavaScript) { @('javascript','python') } elseif ($Python) { @('python','cpp') } elseif ($CSharp) { @('csharp','cpp') } else { @('cpp') }
    & (Join-Path $PSScriptRoot '../install-runtime.ps1') -RuntimeDirectory $RuntimeDirectory -HomeDirectory $env:VIVIDCODE_HOME -Languages $languages
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $env:VIVIDCODE_HOME 'runtime\vividcode-viewer.exe'
    $start.Arguments = if ($Java) { '--java-test -platform offscreen' } elseif ($JavaScript) { '--javascript-test -platform offscreen' } elseif ($Python) { '--python-test -platform offscreen' } elseif ($CSharp) { '--csharp-test -platform offscreen' } else { '--semantic-test -platform offscreen' }
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['QT_FORCE_STDERR_LOGGING'] = '1'
    $process = [Diagnostics.Process]::Start($start)
    $outputTask = $process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit(100000)) { $process.Kill(); throw 'Shared semantic viewer timed out' }
    $output = $outputTask.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) { throw "Shared viewer failed (exit $($process.ExitCode)): $output" }
    $process.Dispose()
    if ($Python) {
        Copy-Item -LiteralPath (Join-Path $env:VIVIDCODE_HOME 'runtime\python-relation-preview.png') -Destination $RuntimeDirectory -Force
        Copy-Item -LiteralPath (Join-Path $env:VIVIDCODE_HOME 'runtime\symbol-files-preview.png') -Destination $RuntimeDirectory -Force
    }
    if ($Java) { Copy-Item -LiteralPath (Join-Path $env:VIVIDCODE_HOME 'runtime\java-relation-preview.png') -Destination $RuntimeDirectory -Force }
    if ($JavaScript) {
        Copy-Item -LiteralPath (Join-Path $env:VIVIDCODE_HOME 'runtime\javascript-relation-preview.png') -Destination $RuntimeDirectory -Force
    }
    if ($CSharp) {
        foreach ($name in @('csharp-relation-preview.png','csharp-solution-preview.png','mixed-language-preview.png','source-popup-preview.png')) {
            Copy-Item -LiteralPath (Join-Path $env:VIVIDCODE_HOME "runtime\$name") -Destination $RuntimeDirectory -Force
        }
    }
    Write-Output $output
    Write-Output 'PASS: shared runtime viewer, project cache and semantic UI'
} finally {
    $env:VIVIDCODE_HOME = $previousHome
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $boundary = [IO.Path]::GetFullPath($testBase).TrimEnd('\') + '\'
    if (!$resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
