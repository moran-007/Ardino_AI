param(
    [string]$ArduinoLibraries = 'E:\Users\moran\Documents\Arduino\libraries',
    [string]$Gxx = 'D:\msys64\ucrt64\bin\g++.exe',
    [string]$Node = 'node'
)
$ErrorActionPreference = 'Stop'
$testOutput = Join-Path $PSScriptRoot 'output'
$null = New-Item -ItemType Directory -Path $testOutput -Force
$sketchPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$jsonPath = Join-Path $ArduinoLibraries 'ArduinoJson\src'
$testExe = Join-Path $testOutput 'test_settings.exe'
& $Gxx '-std=c++17' '-O1' '-Wall' '-Wextra' '-Werror' `
    '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0' '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0' `
    '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0' '-DARDUINOJSON_ENABLE_PROGMEM=0' `
    "-I$PSScriptRoot" "-I$jsonPath" (Join-Path $PSScriptRoot 'test_settings.cpp') `
    (Join-Path $sketchPath 'voice_capture_settings.cpp') '-o' $testExe
if ($LASTEXITCODE -ne 0) { throw 'Voice settings host compilation failed.' }
$previousPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $Gxx) + ';' + $env:PATH
    & $testExe (Join-Path $testOutput 'capture.html')
    if ($LASTEXITCODE -ne 0) { throw 'Voice settings host checks failed.' }
} finally { $env:PATH = $previousPath }
& $Node (Join-Path $PSScriptRoot 'verify_ui.mjs') (Join-Path $testOutput 'capture.html')
if ($LASTEXITCODE -ne 0) { throw 'Voice settings UI checks failed.' }
