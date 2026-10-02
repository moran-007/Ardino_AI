param([string]$Gxx = 'g++')
$ErrorActionPreference = 'Stop'
$sketchPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$testOutput = Join-Path $sketchPath '..\build-size-fix\wifi-host'
New-Item -ItemType Directory -Path $testOutput -Force | Out-Null
$exePath = Join-Path $testOutput 'test_wifi.exe'
& $Gxx '-std=c++17' '-O1' "-I$PSScriptRoot" "-I$PSScriptRoot\..\display_host" "-I$sketchPath" `
    (Join-Path $PSScriptRoot 'test_wifi.cpp') (Join-Path $sketchPath 'wifi_scan.cpp') '-o' $exePath
if ($LASTEXITCODE -ne 0) { throw 'Wi-Fi host test compilation failed.' }
& $exePath
if ($LASTEXITCODE -ne 0) { throw 'Wi-Fi scan/candidate checks failed.' }
