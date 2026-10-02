param([string]$Gxx='D:\msys64\ucrt64\bin\g++.exe')
$ErrorActionPreference='Stop'
$sketch=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$stubs=Join-Path $sketch 'tests\reminder_host'
$json='E:\Users\moran\Documents\Arduino\libraries\ArduinoJson\src'
$out=Join-Path $PSScriptRoot 'output'
$null=New-Item -ItemType Directory -Path $out -Force
$exe=Join-Path $out 'test_profile.exe'
& $Gxx '-std=c++17' '-O1' '-Wall' '-Wextra' '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1' '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0' '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0' '-DARDUINOJSON_ENABLE_PROGMEM=0' "-I$PSScriptRoot" "-I$stubs" "-I$sketch" "-I$json" (Join-Path $PSScriptRoot 'test_profile.cpp') (Join-Path $sketch 'personal_profile.cpp') '-o' $exe
if($LASTEXITCODE -ne 0){throw 'Profile compilation failed'}
$savedPath=$env:PATH
try{$env:PATH=(Split-Path $Gxx)+';'+$savedPath;& $exe;if($LASTEXITCODE -ne 0){throw 'Profile checks failed'}}finally{$env:PATH=$savedPath}
