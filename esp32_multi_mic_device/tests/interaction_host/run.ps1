param([string]$Gxx = 'D:/msys64/ucrt64/bin/g++.exe', [string]$ArduinoLibraries = 'E:/Users/moran/Documents/Arduino/libraries')
$ErrorActionPreference = 'Stop'
$testOutput = Join-Path $PSScriptRoot 'output'
$null = New-Item -ItemType Directory -Path $testOutput -Force
$sketch = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$exe = Join-Path $testOutput 'test_interaction.exe'
& $Gxx '-std=c++17' '-O1' '-Wall' '-Wextra' '-Werror' `
 '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0' '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0' `
 '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0' '-DARDUINOJSON_ENABLE_PROGMEM=0' `
 "-I$PSScriptRoot" "-I$(Join-Path $ArduinoLibraries 'ArduinoJson/src')" (Join-Path $PSScriptRoot 'test_interaction.cpp') `
 (Join-Path $sketch 'robot_interaction.cpp') (Join-Path $sketch 'robot_servos.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Interaction host compilation failed.' }
& $exe (Join-Path $testOutput 'interaction.html')
if ($LASTEXITCODE -ne 0) { throw 'Interaction behavioral checks failed.' }
& node (Join-Path $PSScriptRoot 'verify_ui.mjs') (Join-Path $testOutput 'interaction.html')
if ($LASTEXITCODE -ne 0) { throw 'Interaction UI checks failed.' }
