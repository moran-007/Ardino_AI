param([string]$ArduinoLibraries = 'E:\Users\moran\Documents\Arduino\libraries',
      [string]$Gxx = 'D:\msys64\ucrt64\bin\g++.exe')
$ErrorActionPreference = 'Stop'
$output = Join-Path $PSScriptRoot 'output'
$null = New-Item -ItemType Directory -Path $output -Force
$sketch = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$json = Join-Path $ArduinoLibraries 'ArduinoJson\src'
$exe = Join-Path $output 'test_reminders.exe'
& $Gxx '-std=c++17' '-O1' '-Wall' '-Wextra' '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1' `
    '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0' '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0' '-DARDUINOJSON_ENABLE_PROGMEM=0' `
    "-I$PSScriptRoot" "-I$json" (Join-Path $PSScriptRoot 'test_reminders.cpp') `
    (Join-Path $sketch 'memo_manager.cpp') (Join-Path $sketch 'alarm_manager.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Reminder host compilation failed' }
$previousPath = $env:PATH
try {
    $env:PATH = (Split-Path $Gxx) + ';' + $env:PATH
    & $exe (Join-Path $output 'reminders.html') (Join-Path $output 'cases.json')
    if ($LASTEXITCODE -ne 0) { throw 'Reminder host checks failed' }
} finally { $env:PATH = $previousPath }
