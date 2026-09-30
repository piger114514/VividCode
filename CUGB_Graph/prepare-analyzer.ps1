param(
    [string]$LlvmRoot = 'D:\LLVM',
    [string]$Destination = "$PSScriptRoot\build\Release\analyzer"
)
$ErrorActionPreference = 'Stop'
$Destination = [IO.Path]::GetFullPath($Destination)
$cache = Join-Path $PSScriptRoot 'build\analyzer-downloads'
New-Item -ItemType Directory -Force -Path $cache,$Destination | Out-Null
$archive = Join-Path $cache 'python-3.13.7-embed-amd64.zip'
$url = 'https://www.python.org/ftp/python/3.13.7/python-3.13.7-embed-amd64.zip'
if (!(Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri $url -OutFile $archive }
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne 'F6CCA216A359BE84797CABB54149CE5E062AFB16CC7567EB7FC51CACB2D86B65') { throw 'Python archive SHA256 mismatch' }
$pythonDir = Join-Path $Destination 'python'
if (!(Test-Path -LiteralPath "$pythonDir\python.exe")) { Expand-Archive -LiteralPath $archive -DestinationPath $pythonDir }
@('python313.zip','.', '..') | Set-Content -LiteralPath "$pythonDir\python313._pth" -Encoding ascii
Copy-Item -Path "$PSScriptRoot\analyzer\*.py" -Destination $Destination -Force
New-Item -ItemType Directory -Force -Path "$Destination\lib\clang", "$Destination\licenses" | Out-Null
$targetDll = "$Destination\lib\libclang.dll"
if (!(Test-Path -LiteralPath $targetDll) -or (Get-FileHash -LiteralPath $targetDll).Hash -ne (Get-FileHash -LiteralPath "$LlvmRoot\bin\libclang.dll").Hash) {
    Copy-Item -LiteralPath "$LlvmRoot\bin\libclang.dll" -Destination $targetDll -Force
}
$resourceRoot = [IO.Path]::GetFullPath("$LlvmRoot\lib\clang\23")
Get-ChildItem -LiteralPath $resourceRoot -File -Recurse | ForEach-Object {
    $relative = $_.FullName.Substring($resourceRoot.Length).TrimStart('\')
    $target = Join-Path "$Destination\lib\clang\23" $relative
    if (!(Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $_.FullName).Hash) {
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $target -Force
    }
}
$license = Join-Path $cache 'LLVM-LICENSE.txt'
if (!(Test-Path -LiteralPath $license)) {
    Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.0/LICENSE.TXT' -OutFile $license
}
Copy-Item -LiteralPath $license -Destination "$Destination\licenses\LLVM-LICENSE.txt" -Force
Copy-Item -LiteralPath "$pythonDir\LICENSE.txt" -Destination "$Destination\licenses\Python-LICENSE.txt" -Force
$manifest = @{
    python='3.13.7'; pythonUrl=$url; pythonSha256=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash;
    llvm='23.1.2'; libclangSha256=(Get-FileHash -LiteralPath "$Destination\lib\libclang.dll" -Algorithm SHA256).Hash
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath "$Destination\runtime.json" -Encoding utf8
& "$pythonDir\python.exe" -c 'import sys, sqlite3; sys.path.insert(0, sys.argv[1]); import clang_bridge; print(clang_bridge.get_libclang().version())' $Destination
if ($LASTEXITCODE -ne 0) { throw 'Analyzer runtime validation failed' }
Write-Host "Analyzer ready: $Destination"
