param([Parameter(Mandatory=$true)][string]$RuntimeDirectory)
$ErrorActionPreference = 'Stop'
$testBase = Join-Path (Split-Path $RuntimeDirectory) 'local-environment-test-work'
$testRoot = Join-Path $testBase ([guid]::NewGuid().ToString('N'))
$previousHome = $env:VIVIDCODE_HOME
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
function Assert($condition,$message) { if (!$condition) { throw "FAIL: $message" } }
function RunCheck([string]$testHome,[string]$mode) {
    $env:VIVIDCODE_HOME = $testHome
    $start = [Diagnostics.ProcessStartInfo]::new(); $start.FileName = Join-Path $testHome 'runtime\vividcode-viewer.exe'
    $start.Arguments = '--environment-test -platform offscreen' + $(if ($mode -eq 'javascript') {' --javascript-only'} elseif ($mode -eq 'python') {' --python-only'} else {''})
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true; $start.RedirectStandardError = $true
    $start.EnvironmentVariables['QT_FORCE_STDERR_LOGGING'] = '1'
    $process = [Diagnostics.Process]::Start($start); $output = $process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Environment test timed out' }
    $text = $output.GetAwaiter().GetResult(); Assert ($process.ExitCode -eq 0) "viewer: $text"; $process.Dispose(); Write-Output $text
}
try {
    $core = Join-Path $testRoot 'core'; $js = Join-Path $testRoot 'javascript'
    & (Join-Path $PSScriptRoot '../install-runtime.ps1') -RuntimeDirectory $RuntimeDirectory -HomeDirectory $core
    Assert (!(Test-Path -LiteralPath "$core\runtime\analyzer")) 'core has no Python/C++ component'
    Assert (!(Test-Path -LiteralPath "$core\runtime\csharp-analyzer")) 'core has no C# component'
    Assert (!(Test-Path -LiteralPath "$core\runtime\javascript-analyzer")) 'core has no JS component'
    Assert (!(Test-Path -LiteralPath "$core\runtime\java-analyzer")) 'core has no Java component'
    RunCheck $core 'core'
    & (Join-Path $PSScriptRoot '../install-runtime.ps1') -RuntimeDirectory $RuntimeDirectory -HomeDirectory $js -Languages @('javascript')
    Assert (!(Test-Path -LiteralPath "$js\runtime\analyzer")) 'JS-only has no Python/C++ component'
    Assert (!(Test-Path -LiteralPath "$js\runtime\csharp-analyzer")) 'JS-only has no C# component'
    Assert (!(Test-Path -LiteralPath "$js\runtime\javascript-analyzer\node.exe")) 'old build Node is not reinstalled'
    Assert (!(Test-Path -LiteralPath "$js\runtime\javascript-analyzer\Node-LICENSE.txt")) 'Node distribution not copied'
    Assert (Test-Path -LiteralPath "$js\runtime\javascript-analyzer\typescript\lib\typescript.js") 'JS parser included'
    RunCheck $js 'javascript'
    $pythonOnly = Join-Path $testRoot 'python'
    & (Join-Path $PSScriptRoot '../install-runtime.ps1') -RuntimeDirectory $RuntimeDirectory -HomeDirectory $pythonOnly -Languages @('python')
    Assert (!(Test-Path -LiteralPath "$pythonOnly\runtime\analyzer\python")) 'Python runtime is not copied'
    Assert (!(Test-Path -LiteralPath "$pythonOnly\runtime\analyzer\lib")) 'LLVM libraries are not copied'
    Assert (!(Test-Path -LiteralPath "$pythonOnly\runtime\analyzer\clang_bridge.py")) 'unrelated C++ bridge not installed'
    Assert (!(Test-Path -LiteralPath "$pythonOnly\runtime\javascript-analyzer")) 'unrelated JS component not installed'
    RunCheck $pythonOnly 'python'
    $javaOnly = Join-Path $testRoot 'java'
    & (Join-Path $PSScriptRoot '../install-runtime.ps1') -RuntimeDirectory $RuntimeDirectory -HomeDirectory $javaOnly -Languages @('java')
    Assert (!(Test-Path -LiteralPath "$javaOnly\runtime\analyzer")) 'Java-only has no Python/C++ component'
    Assert (!(Test-Path -LiteralPath "$javaOnly\runtime\javascript-analyzer")) 'Java-only has no JS component'
    Assert (!(Test-Path -LiteralPath "$javaOnly\runtime\csharp-analyzer")) 'Java-only has no C# component'
    Assert (!(Test-Path -LiteralPath "$javaOnly\runtime\java-analyzer\java.exe")) 'JDK is not distributed'
    $javaProject = Join-Path $testRoot 'java-project'; New-Item -ItemType Directory -Path $javaProject | Out-Null
    [IO.File]::WriteAllText((Join-Path $javaProject 'A.java'),'class A {static int helper(){return 1;} int caller(){return helper();}}')
    New-Item -ItemType Directory -Path (Join-Path $javaProject '.vividcode') | Out-Null
    [IO.File]::WriteAllText((Join-Path $javaProject '.vividcode/project.json'),'{"app":"vividcode","schema":1}')
    $env:VIVIDCODE_HOME = $javaOnly
    & "$javaOnly\runtime\vividcode.exe" --java-analyzer index --project $javaProject | Out-Null
    Assert ($LASTEXITCODE -eq 0) 'Java-only uses local JDK'
    $javaResult = & "$javaOnly\runtime\vividcode.exe" --java-analyzer export --project $javaProject | ConvertFrom-Json
    Assert ($javaResult.state.last_status -eq 'ok' -and $javaResult.units.Count -eq 1) 'Java-only committed complete graph'
    $javaSettings = @{java=$javaResult.state.java} | ConvertTo-Json
    [IO.File]::WriteAllText((Join-Path $javaOnly 'environments.json'),$javaSettings,[Text.UTF8Encoding]::new($false))
    & (Join-Path $PSScriptRoot '../install-runtime.ps1') -RuntimeDirectory $RuntimeDirectory -HomeDirectory $javaOnly
    Assert (!(Test-Path -LiteralPath "$javaOnly\runtime\java-analyzer")) 'reinstall removes deselected Java'
    Assert ((Get-Content -Raw -Encoding UTF8 -LiteralPath "$javaOnly\environments.json") -eq $javaSettings) 'reinstall preserves Java settings'
    $settings = Get-Content -Raw -LiteralPath "$js\environments.json"
    & (Join-Path $PSScriptRoot '../install-runtime.ps1') -RuntimeDirectory $RuntimeDirectory -HomeDirectory $js
    Assert (!(Test-Path -LiteralPath "$js\runtime\javascript-analyzer")) 'core-only reinstall removes deselected component'
    Assert ((Get-Content -Raw -LiteralPath "$js\environments.json") -eq $settings) 'reinstall preserves environment choices'
    Write-Output 'PASS: core-only / selected JS install, local runtime only, environment settings preserved'
} finally {
    $env:VIVIDCODE_HOME = $previousHome
    $resolved = [IO.Path]::GetFullPath($testRoot); $boundary = [IO.Path]::GetFullPath($testBase).TrimEnd('\') + '\'
    if (!$resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
