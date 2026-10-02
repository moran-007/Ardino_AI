param(
 [string]$Gxx = 'D:/msys64/ucrt64/bin/g++.exe',
 [string]$ArduinoLibraries = 'E:/Users/moran/Documents/Arduino/libraries'
)
$ErrorActionPreference = 'Stop'
$testOutput = Join-Path $PSScriptRoot 'output'
$null = New-Item -ItemType Directory -Path $testOutput -Force
$sketchPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$jsonPath = Join-Path $ArduinoLibraries 'ArduinoJson/src'
$testExe = Join-Path $testOutput 'test_custom_wake.exe'
& $Gxx '-std=c++17' '-O1' '-Wall' '-Wextra' '-Werror' `
 '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0' '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0' `
 '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0' '-DARDUINOJSON_ENABLE_PROGMEM=0' `
 "-I$PSScriptRoot" "-I$jsonPath" (Join-Path $PSScriptRoot 'test_custom_wake.cpp') `
 (Join-Path $sketchPath 'custom_wake.cpp') '-o' $testExe
if ($LASTEXITCODE -ne 0) { throw 'Custom wake host compilation failed.' }
& $testExe (Join-Path $testOutput 'custom_wake.html')
if ($LASTEXITCODE -ne 0) { throw 'Custom wake behavioral checks failed.' }
& node (Join-Path $PSScriptRoot 'verify_ui.mjs') (Join-Path $testOutput 'custom_wake.html')
if ($LASTEXITCODE -ne 0) { throw 'Custom wake UI checks failed.' }
$offlineExe = Join-Path $testOutput 'test_offline_wake.exe'
& $Gxx '-std=c++17' '-O1' '-Wall' '-Wextra' '-Werror' `
 '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0' '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0' `
 '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0' '-DARDUINOJSON_ENABLE_PROGMEM=0' `
 "-I$PSScriptRoot" "-I$jsonPath" (Join-Path $PSScriptRoot 'test_offline_wake.cpp') `
 (Join-Path $sketchPath 'custom_wake.cpp') (Join-Path $sketchPath 'offline_wake.cpp') '-o' $offlineExe
if ($LASTEXITCODE -ne 0) { throw 'Offline/custom wake integration compilation failed.' }
& $offlineExe
if ($LASTEXITCODE -ne 0) { throw 'Offline custom fallback checks failed.' }
& $offlineExe 'official-models-present'
if ($LASTEXITCODE -ne 0) { throw 'Offline official/custom mode checks failed.' }
& node (Join-Path $sketchPath 'tests/wake_event_check.js')
if ($LASTEXITCODE -ne 0) { throw 'Legacy official wake-event regression check failed.' }
