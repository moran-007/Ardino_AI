param(
    [Parameter(Mandatory=$true)][string]$ArduinoLibraries,
    [string]$Python = 'python',
    [string]$Gcc = 'gcc',
    [string]$Gxx = 'g++',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$repoPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoPath 'build-display-controls' }
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$testOutput = (Resolve-Path -LiteralPath $OutputDirectory).Path
$sketchPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$gfxPath = Join-Path $ArduinoLibraries 'Adafruit_GFX_Library'
$fontPath = Join-Path $ArduinoLibraries 'U8g2_for_Adafruit_GFX\src'
& $Python (Join-Path $PSScriptRoot 'prepare.py') $testOutput $ArduinoLibraries
if ($LASTEXITCODE -ne 0) { throw 'Prepare failed. Install Pillow, qrcode and zxing-cpp into this Python, or into OutputDirectory/pythonlibs.' }
& $Gcc '-O1' "-I$fontPath" '-c' (Join-Path $testOutput 'test_fonts.c') '-o' (Join-Path $testOutput 'test_fonts.o')
if ($LASTEXITCODE -ne 0) { throw 'Font compilation failed.' }
& $Gxx '-std=c++17' '-O1' '-DARDUINO=10800' "-I$PSScriptRoot" "-I$testOutput" "-I$gfxPath" "-I$fontPath" `
    (Join-Path $PSScriptRoot 'test_display.cpp') (Join-Path $sketchPath 'robot_avatar.cpp') (Join-Path $sketchPath 'robot_expression_catalog.cpp') (Join-Path $sketchPath 'robot_expression_web.cpp') `
    (Join-Path $gfxPath 'Adafruit_GFX.cpp') (Join-Path $fontPath 'U8g2_for_Adafruit_GFX.cpp') `
    (Join-Path $testOutput 'test_fonts.o') '-o' (Join-Path $testOutput 'test_display.exe')
if ($LASTEXITCODE -ne 0) { throw 'Host test compilation failed.' }
Push-Location $testOutput
try {
    & (Join-Path $testOutput 'test_display.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Renderer checks failed.' }
} finally { Pop-Location }
& $Python (Join-Path $PSScriptRoot 'verify_images.py') $testOutput
if ($LASTEXITCODE -ne 0) { throw 'QR image validation failed.' }
