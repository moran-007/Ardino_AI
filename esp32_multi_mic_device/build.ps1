param(
    [string]$ArduinoCli = '',
    [string]$LibrariesDirectory = '',
    [string]$BuildDirectory = (Join-Path $PSScriptRoot 'build')
)
$ErrorActionPreference = 'Stop'
if (!$ArduinoCli) {
    $command = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($command) { $ArduinoCli = $command.Source }
    elseif (Test-Path -LiteralPath 'D:\Arduino CLI\arduino-cli.exe') {
        $ArduinoCli = 'D:\Arduino CLI\arduino-cli.exe'
    } else { throw '请通过 -ArduinoCli 指定 arduino-cli.exe 路径。' }
}
if (!$LibrariesDirectory -and (Test-Path -LiteralPath 'E:\Users\moran\Documents\Arduino\libraries')) {
    $LibrariesDirectory = 'E:\Users\moran\Documents\Arduino\libraries'
}
$fqbn = 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=default,FlashSize=16M,PartitionScheme=esp_sr_16,PSRAM=opi'
$arguments = @('compile', '--fqbn', $fqbn, '--build-path', $BuildDirectory)
if ($LibrariesDirectory) { $arguments += @('--libraries', $LibrariesDirectory) }
$arguments += $PSScriptRoot
& $ArduinoCli @arguments
if ($LASTEXITCODE -ne 0) { throw '多麦克风工程编译失败。' }
& (Join-Path $PSScriptRoot 'verify_offline_wake_build.ps1') -BuildDirectory $BuildDirectory
if ($LASTEXITCODE -ne 0) { throw '离线唤醒模型检查失败。' }
Write-Host '构建完成。此脚本只编译和检查，不连接串口或烧录设备。'
