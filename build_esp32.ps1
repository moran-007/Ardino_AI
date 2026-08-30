$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$experimentDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$sketchDir = Join-Path $experimentDir "esp32_lan_device"
$outputDir = Join-Path $experimentDir "build-esp32-lan"
$buildPath = Join-Path $experimentDir "build-work-esp32-lan"
$fqbn = "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi"

$arduinoCli = (Get-Command arduino-cli -ErrorAction Stop).Source
& $arduinoCli compile --fqbn $fqbn --build-path $buildPath --output-dir $outputDir $sketchDir
if ($LASTEXITCODE -ne 0) {
    throw "ESP32-S3 compilation failed with exit code $LASTEXITCODE."
}

Write-Host "ESP32-S3 LAN firmware compiled. No serial port was opened and nothing was flashed."
