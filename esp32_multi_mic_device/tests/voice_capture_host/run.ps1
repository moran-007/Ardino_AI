param(
    [string]$Gxx = 'D:\msys64\ucrt64\bin\g++.exe',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$sketchPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $sketchPath 'build-host' }
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$outputPath = (Resolve-Path -LiteralPath $OutputDirectory).Path
$executable = Join-Path $outputPath 'test_capture.exe'
& $Gxx '-std=c++17' '-O1' '-g' '-Wall' '-Wextra' "-I$PSScriptRoot" "-I$sketchPath" `
    '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0' '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0' `
    '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0' '-DARDUINOJSON_ENABLE_PROGMEM=0' `
    '-IE:\Users\moran\Documents\Arduino\libraries\ArduinoJson\src' `
    (Join-Path $PSScriptRoot 'test_capture.cpp') (Join-Path $sketchPath 'voice_input.cpp') `
    (Join-Path $PSScriptRoot 'fake_music_echo.cpp') `
    (Join-Path $sketchPath 'multi_mic_config.cpp') (Join-Path $sketchPath 'music_player.cpp') `
    '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'Capture host test compilation failed.' }
$previousPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $Gxx) + ';' + $previousPath
    $downloadExecutable = Join-Path $outputPath 'test_download.exe'
    & $Gxx '-std=c++17' '-O1' '-Wall' '-Wextra' "-I$PSScriptRoot" "-I$sketchPath" `
        (Join-Path $PSScriptRoot 'test_download.cpp') '-o' $downloadExecutable
    if ($LASTEXITCODE -ne 0) { throw 'Audio download host compilation failed.' }
    & $downloadExecutable
    if ($LASTEXITCODE -ne 0) { throw 'Audio download host checks failed.' }
    & $executable
    if ($LASTEXITCODE -ne 0) { throw 'Capture host checks failed.' }
} finally {
    $env:PATH = $previousPath
}
