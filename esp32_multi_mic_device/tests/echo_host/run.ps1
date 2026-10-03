param([string]$Gxx = 'D:\msys64\ucrt64\bin\g++.exe')
$ErrorActionPreference='Stop'
$sketch=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$output=Join-Path $sketch 'build-host/test_music_echo.exe'
$previousPath=$env:PATH
try {
    $env:PATH=(Split-Path $Gxx)+';'+$env:PATH
    & $Gxx '-std=c++17' '-O1' '-Wall' '-Wextra' "-I$PSScriptRoot" "-I$sketch" `
        "-I$sketch/tests/music_host" (Join-Path $PSScriptRoot 'test_echo.cpp') '-o' $output
    if ($LASTEXITCODE -ne 0) { throw 'Echo capture test compilation failed' }
    & $output
    if ($LASTEXITCODE -ne 0) { throw 'Echo capture checks failed' }
} finally { $env:PATH=$previousPath }
