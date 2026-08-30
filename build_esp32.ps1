param(
    [ValidateSet("lan", "cloud")]
    [string]$Target = "lan"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$experimentDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$sketchName = if ($Target -eq "cloud") { "esp32_cloud_device" } else { "esp32_lan_device" }
$sketchDir = Join-Path $experimentDir $sketchName
$outputDir = Join-Path $experimentDir "build-esp32-$Target"
$buildPath = Join-Path $experimentDir "build-work-esp32-$Target"
$fqbn = "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi"

$arduinoCli = (Get-Command arduino-cli -ErrorAction Stop).Source
& $arduinoCli compile --fqbn $fqbn --build-path $buildPath --output-dir $outputDir $sketchDir
if ($LASTEXITCODE -ne 0) {
    throw "ESP32-S3 compilation failed with exit code $LASTEXITCODE."
}

Write-Host "ESP32-S3 $Target firmware compiled. No serial port was opened and nothing was flashed."
