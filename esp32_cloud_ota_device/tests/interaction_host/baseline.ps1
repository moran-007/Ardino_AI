$ErrorActionPreference = 'Stop'
$testOutput = Join-Path $PSScriptRoot 'output'
$null = New-Item -ItemType Directory -Path $testOutput -Force
$sketch = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$exe = Join-Path $testOutput 'baseline.exe'
& 'D:/msys64/ucrt64/bin/g++.exe' '-std=c++17' '-O1' '-Wall' '-Wextra' '-Werror' "-I$PSScriptRoot" `
 (Join-Path $PSScriptRoot 'baseline.cpp') (Join-Path $sketch 'robot_servos.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Interaction baseline compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 1) { throw 'Expected missing greeting gesture baseline was not observed.' }
