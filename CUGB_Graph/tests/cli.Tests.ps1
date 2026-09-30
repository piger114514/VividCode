param([Parameter(Mandatory=$true)][string]$RuntimeDirectory)
$ErrorActionPreference = 'Stop'
$cli = Join-Path $RuntimeDirectory 'vividcode.exe'
$testBase = Join-Path (Split-Path $RuntimeDirectory) 'cli-test-work'
$testRoot = Join-Path $testBase ([guid]::NewGuid().ToString('N'))
$project = Join-Path $testRoot ('project space ' + [char]0x4e2d + [char]0x6587)
$outside = Join-Path $testRoot 'outside'
$store = Join-Path $project '.vividcode'
$utf8 = New-Object System.Text.UTF8Encoding($false)
New-Item -ItemType Directory -Path $project,$outside -Force | Out-Null
function Assert($condition, $message) { if (!$condition) { throw "FAIL: $message" } }
function WriteUtf8($path, $text) { [IO.File]::WriteAllText($path, $text, $utf8) }
function Command([string[]]$Arguments, [int]$Expected=0) {
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue' # Expected nonzero commands may write stderr on Windows PowerShell 5.
    $output = & $cli @Arguments 2>&1
    $actual = $LASTEXITCODE
    $ErrorActionPreference = $previousPreference
    Assert ($actual -eq $Expected) "vividcode $Arguments returned $actual (expected $Expected): $output"
}
function Manifest { Get-Content -LiteralPath (Join-Path $store 'project.json') -Raw -Encoding UTF8 | ConvertFrom-Json }
function RunViewer([bool]$Verify) {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $store 'vividcode-viewer.exe'
    $start.Arguments = if ($Verify) { '--verify-startup -platform offscreen' } else { '-platform offscreen' }
    $start.WorkingDirectory = $outside
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $process = [Diagnostics.Process]::Start($start)
    if ($Verify) {
        if (!$process.WaitForExit(15000)) { $process.Kill(); throw 'Viewer startup timed out' }
        Assert ($process.ExitCode -eq 0) 'deployed viewer starts from unrelated working directory'
        $process.Dispose()
    } else { return $process }
}
$viewer = $null
$junction = $null
Push-Location -LiteralPath $project
try {
    WriteUtf8 (Join-Path $project 'main.cpp') 'int main() { return 0; }'
    New-Item -ItemType Directory -Path (Join-Path $project 'Document') | Out-Null
    WriteUtf8 (Join-Path $project 'Document\notes.md') 'user maintained document'
    WriteUtf8 (Join-Path $outside 'sentinel.txt') 'keep outside data'
    Command -Arguments @('--help')
    Command -Arguments @('--g', '--d') -Expected 2
    Command -Arguments @('--unknown') -Expected 2
    Command -Arguments @('--c') -Expected 1
    Command -Arguments @('--d')
    Command -Arguments @('--g')
    $first = Manifest
    Assert ($first.history.Count -eq 1) 'first generation creates one record'
    Assert (Test-Path -LiteralPath (Join-Path $store 'platforms\qwindows.dll')) 'Qt runtime deployed'
    Assert (Test-Path -LiteralPath (Join-Path $store 'vividcode-viewer.exe')) 'viewer deployed'
    $index = Get-Content -LiteralPath (Join-Path $store "records\$($first.latest)\index.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert ($index.files.Count -eq 1) 'generated demo and runtime excluded from project scan'
    Command -Arguments @('--g')
    Assert ((Manifest).history.Count -eq 1) 'unchanged generation deduplicates record'

    WriteUtf8 (Join-Path $project 'main.cpp') 'int main() { return 9; }'
    RunViewer -Verify $true
    Assert ((Manifest).history.Count -eq 2) 'viewer startup records changed code'
    RunViewer -Verify $true
    Assert ((Manifest).history.Count -eq 2) 'unchanged viewer startup deduplicates record'
    WriteUtf8 (Join-Path $project 'new.h') '// new file'
    Command -Arguments @('--g')
    Remove-Item -LiteralPath (Join-Path $project 'new.h')
    Command -Arguments @('--g')
    $before = Manifest
    Assert ($before.history.Count -eq 4) 'add and delete both produce records'
    $latestFile = Join-Path $store "records\$($before.latest)\code.md"
    $latestBytes = [Convert]::ToBase64String([IO.File]::ReadAllBytes($latestFile))
    $manifestFile = Join-Path $store 'project.json'
    $originalManifest = [IO.File]::ReadAllText($manifestFile)
    $before.latest = '../outside'
    WriteUtf8 $manifestFile ($before | ConvertTo-Json -Depth 8)
    Command -Arguments @('--c') -Expected 1
    Assert ((Get-ChildItem -LiteralPath (Join-Path $store 'records') -Directory).Count -eq 4) 'invalid manifest deletes nothing'
    WriteUtf8 $manifestFile $originalManifest
    $before = Manifest
    $latestIndex = Join-Path $store "records\$($before.latest)\index.json"
    $originalIndex = [IO.File]::ReadAllText($latestIndex)
    WriteUtf8 $latestIndex '{broken'
    Command -Arguments @('--c') -Expected 1
    Assert ((Get-ChildItem -LiteralPath (Join-Path $store 'records') -Directory).Count -eq 4) 'corrupt latest preserves old records'
    WriteUtf8 $latestIndex $originalIndex
    (Get-Item -LiteralPath $latestFile).LastWriteTime = [datetime]'2000-01-01'
    Command -Arguments @('--c')
    $after = Manifest
    Assert ($after.history.Count -eq 1 -and $after.latest -eq $before.latest) 'clean preserves manifest latest, not file timestamp'
    Assert ([Convert]::ToBase64String([IO.File]::ReadAllBytes($latestFile)) -eq $latestBytes) 'latest document unchanged'
    Assert ((Get-ChildItem -LiteralPath (Join-Path $store 'records') -Directory).Count -eq 1) 'old record directories removed'
    Command -Arguments @('--c')

    $junction = Join-Path $store 'external-link'
    New-Item -ItemType Junction -Path $junction -Target $outside | Out-Null
    Command -Arguments @('--d') -Expected 1
    Assert (Test-Path -LiteralPath (Join-Path $outside 'sentinel.txt')) 'nested junction target preserved'
    [IO.Directory]::Delete($junction)
    $junction = $null

    $viewer = RunViewer -Verify $false
    $lock = Join-Path $project '.vividcode-viewer.lock'
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (!(Test-Path -LiteralPath $lock) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }
    Assert (Test-Path -LiteralPath $lock) 'running viewer holds runtime lock'
    Command -Arguments @('--d') -Expected 1
    Command -Arguments @('--g') -Expected 1
    $viewer.Kill(); $viewer.WaitForExit(); $viewer.Dispose(); $viewer = $null
    Command -Arguments @('--d')
    Assert (!(Test-Path -LiteralPath $store)) 'generated directory deleted'
    Assert (([IO.File]::ReadAllText((Join-Path $project 'main.cpp'))) -eq 'int main() { return 9; }') 'source unchanged'
    Assert (Test-Path -LiteralPath (Join-Path $project 'Document\notes.md')) 'manual documents preserved'
    Assert (Test-Path -LiteralPath (Join-Path $outside 'sentinel.txt')) 'neighbor preserved'
    Command -Arguments @('--d')
    $junction = $store
    New-Item -ItemType Junction -Path $junction -Target $outside | Out-Null
    Command -Arguments @('--d') -Expected 1
    Command -Arguments @('--g') -Expected 1
    Command -Arguments @('--c') -Expected 1
    [IO.Directory]::Delete($junction)
    $junction = $null
    New-Item -ItemType Directory -Path $store | Out-Null
    WriteUtf8 (Join-Path $store 'unknown.txt') 'unowned data'
    Command -Arguments @('--g') -Expected 1
    Assert (Test-Path -LiteralPath (Join-Path $store 'unknown.txt')) 'generation refuses unknown existing directory'
    Command -Arguments @('--d')
    Write-Output 'PASS: CLI generate/deduplicate/history/viewer/cleanup/delete/invalid input/locks/junction protection'
} finally {
    if ($viewer -and !$viewer.HasExited) { $viewer.Kill(); $viewer.WaitForExit(); $viewer.Dispose() }
    if ($junction -and (Test-Path -LiteralPath $junction)) { [IO.Directory]::Delete($junction) }
    Pop-Location
    # Only delete this test's UUID directory after resolving and checking its boundary.
    $resolvedRoot = [IO.Path]::GetFullPath($testRoot)
    $resolvedBase = [IO.Path]::GetFullPath($testBase).TrimEnd('\') + '\'
    if (!$resolvedRoot.StartsWith($resolvedBase, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup path' }
    Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
}
