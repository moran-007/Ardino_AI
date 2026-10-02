param(
    [string]$ArduinoData = "$env:LOCALAPPDATA\Arduino15",
    [switch]$Restore
)

$ErrorActionPreference = 'Stop'
$sketchRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceModel = Join-Path $sketchRoot 'wake_models\srmodels.bin'
$toolBase = Join-Path $ArduinoData 'packages\esp32\tools\esp32s3-libs'

if (-not (Test-Path -LiteralPath $toolBase)) {
    throw "未找到 ESP32-S3 Arduino 工具目录：$toolBase"
}

$versions = Get-ChildItem -LiteralPath $toolBase -Directory | Sort-Object {
    try { [version]$_.Name } catch { [version]'0.0.0' }
} -Descending
if (-not $versions) {
    throw "未找到已安装的 esp32s3-libs 版本。"
}

$selected = $versions[0]
$targetModel = Join-Path $selected.FullName 'esp_sr\srmodels.bin'
$backupModel = Join-Path $selected.FullName 'esp_sr\srmodels.hiesp.original.bin'

if (-not (Test-Path -LiteralPath $targetModel)) {
    throw "Arduino ESP-SR 默认模型不存在：$targetModel"
}

if ($Restore) {
    if (-not (Test-Path -LiteralPath $backupModel)) {
        throw "没有可恢复的默认模型备份：$backupModel"
    }
    Copy-Item -LiteralPath $backupModel -Destination $targetModel -Force
    Write-Host "已恢复 Arduino 默认 Hi ESP 模型：$targetModel"
    exit 0
}

if (-not (Test-Path -LiteralPath $sourceModel)) {
    throw "项目中文模型包不存在：$sourceModel"
}
if ((Get-Item -LiteralPath $sourceModel).Length -gt 0x3E0000) {
    throw "模型包超过 0x3E0000 字节，无法写入 model 分区。"
}
if (-not (Test-Path -LiteralPath $backupModel)) {
    Copy-Item -LiteralPath $targetModel -Destination $backupModel
    Write-Host "已备份 Arduino 默认模型：$backupModel"
}

Copy-Item -LiteralPath $sourceModel -Destination $targetModel -Force
$sourceHash = (Get-FileHash -LiteralPath $sourceModel -Algorithm SHA256).Hash
$targetHash = (Get-FileHash -LiteralPath $targetModel -Algorithm SHA256).Hash
if ($sourceHash -ne $targetHash) {
    throw "模型复制后 SHA-256 不一致。"
}

Write-Host "已安装 13 个中文离线唤醒模型到 Arduino ESP32-S3 $($selected.Name)。"
Write-Host "SHA-256: $targetHash"
Write-Host "Arduino IDE 必须选择：Partition Scheme -> ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)。"
Write-Host "首次必须使用 USB 上传；1.3.x 不能通过应用 OTA 直接变更分区表。"
