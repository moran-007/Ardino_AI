param([string]$Gxx = 'D:\msys64\ucrt64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$sketch = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$output = Join-Path $sketch 'build-host'
$null = New-Item -ItemType Directory -Path $output -Force
$previousPath = $env:PATH
try {
    $env:PATH = (Split-Path $Gxx) + ';' + $env:PATH
    & $Gxx '-std=c++17' '-O1' '-Wall' '-Wextra' "-I$sketch" (Join-Path $PSScriptRoot 'test_core.cpp') '-o' (Join-Path $output 'test_music_core.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Music core compilation failed' }
    & (Join-Path $output 'test_music_core.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Music core checks failed' }
    & $Gxx '-std=c++17' '-O1' '-Wall' '-Wextra' '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1' `
        '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0' '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0' '-DARDUINOJSON_ENABLE_PROGMEM=0' `
        "-I$PSScriptRoot" "-I$sketch" '-IE:\Users\moran\Documents\Arduino\libraries\ArduinoJson\src' `
        (Join-Path $PSScriptRoot 'test_player.cpp') '-o' (Join-Path $output 'test_full_player.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Production player compilation failed' }
    & (Join-Path $output 'test_full_player.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Production player checks failed' }
} finally { $env:PATH = $previousPath }
