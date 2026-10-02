param([string]$ArduinoLibraries = 'E:\Users\moran\Documents\Arduino\libraries',
      [string]$Gxx = 'D:\msys64\ucrt64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$output = Join-Path $PSScriptRoot 'output'
$null = New-Item -ItemType Directory -Path $output -Force
$sketch = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$json = Join-Path $ArduinoLibraries 'ArduinoJson\src'
$exe = Join-Path $output 'test_focus.exe'
& $Gxx '-std=c++17' '-O1' '-Wall' '-Wextra' '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1' `
    '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0' '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0' '-DARDUINOJSON_ENABLE_PROGMEM=0' `
    "-I$PSScriptRoot" "-I$json" (Join-Path $PSScriptRoot 'test_focus.cpp') `
    (Join-Path $sketch 'focus_timer.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Focus host compilation failed' }
$previousPath = $env:PATH
try {
    $env:PATH = (Split-Path $Gxx) + ';' + $env:PATH
    & $exe (Join-Path $output 'focus.html')
    if ($LASTEXITCODE -ne 0) { throw 'Focus host checks failed' }
    & node (Join-Path $PSScriptRoot 'verify_ui.mjs') (Join-Path $output 'focus.html')
    if ($LASTEXITCODE -ne 0) { throw 'Focus UI checks failed' }
} finally { $env:PATH = $previousPath }
