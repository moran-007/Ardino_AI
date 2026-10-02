param(
    [string]$BuildDirectory = ''
)

$ErrorActionPreference = 'Stop'
$expectedModelSha256 = 'B55E4F1C3EE4267762577B19485A79B1A1EE84DA166E1F0626596B1A2C035843'

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $buildRoot = Join-Path $env:LOCALAPPDATA 'arduino\sketches'
    $candidate = Get-ChildItem -LiteralPath $buildRoot -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'esp32_cloud_ota_device.ino.bin') } |
        Sort-Object { (Get-Item -LiteralPath (Join-Path $_.FullName 'esp32_cloud_ota_device.ino.bin')).LastWriteTime } -Descending |
        Select-Object -First 1
    if (-not $candidate) {
        throw '未找到 Arduino IDE 的 esp32_cloud_ota_device 构建目录。'
    }
    $BuildDirectory = $candidate.FullName
}

$BuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
$optionsPath = Join-Path $BuildDirectory 'build.options.json'
$modelPath = Join-Path $BuildDirectory 'srmodels.bin'
$flashArgsPath = Join-Path $BuildDirectory 'flash_args'
$firmwarePath = Join-Path $BuildDirectory 'esp32_cloud_ota_device.ino.bin'
$bootloaderPath = Join-Path $BuildDirectory 'esp32_cloud_ota_device.ino.bootloader.bin'

foreach ($requiredPath in @($optionsPath, $modelPath, $flashArgsPath, $firmwarePath, $bootloaderPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "构建结果不完整，缺少：$requiredPath"
    }
}

$options = Get-Content -Raw -LiteralPath $optionsPath | ConvertFrom-Json
$fqbn = [string]$options.fqbn
if ($fqbn -notmatch '(?:^|[,:])PartitionScheme=esp_sr_16(?:,|$)') {
    throw "分区方案仍然错误。当前是 '$fqbn'；请在 Arduino IDE 选择 ESP SR 16M 后重新编译。"
}
if ($fqbn -notmatch '(?:^|[,:])FlashSize=16M(?:,|$)') {
    throw "Flash 容量设置错误。当前是 '$fqbn'；请在 Arduino IDE 选择 Flash Size -> 16MB 后重新编译。"
}
if ($fqbn -notmatch '(?:^|[,:])PSRAM=opi(?:,|$)') {
    throw "PSRAM 设置错误。当前是 '$fqbn'；请在 Arduino IDE 选择 PSRAM -> OPI PSRAM 后重新编译。"
}

$actualModelSha256 = (Get-FileHash -LiteralPath $modelPath -Algorithm SHA256).Hash
if ($actualModelSha256 -ne $expectedModelSha256) {
    throw "构建目录中的 srmodels.bin 不是本项目的 13 模型中文包。实际 SHA-256：$actualModelSha256"
}

$flashArgs = Get-Content -Raw -LiteralPath $flashArgsPath
$modelUploadSource = 'flash_args'
if ($flashArgs -notmatch '(?i)0xC10000\s+[^\r\n]*srmodels\.bin') {
    # Core 3.3.12's flash_args omits upload.extra_flags, while IDE/CLI upload
    # still expands them from boards.txt. Verify that actual upload recipe.
    $boardId = $fqbn.Split(':')[2]
    $modelUploadSource = ''
    foreach ($platformDirectory in ([string]$options.hardwareFolders).Split(',')) {
        $boardsPath = Join-Path $platformDirectory 'boards.txt'
        $platformPath = Join-Path $platformDirectory 'platform.txt'
        if (!(Test-Path -LiteralPath $boardsPath) -or !(Test-Path -LiteralPath $platformPath)) { continue }
        $boardsText = Get-Content -Raw -LiteralPath $boardsPath
        $platformText = Get-Content -Raw -LiteralPath $platformPath
        $property = [regex]::Escape("$boardId.menu.PartitionScheme.esp_sr_16.upload.extra_flags")
        if ($boardsText -match "(?m)^$property=0xC10000\s+\{build.path\}/srmodels\.bin\s*$" -and
            $platformText -match '(?m)^tools\.esptool_py\.upload\.pattern_args=.*\{upload.extra_flags\}') {
            $modelUploadSource = 'Arduino IDE/CLI upload.extra_flags（flash_args 不包含模型，不能用于完整首刷）'
            break
        }
    }
    if (!$modelUploadSource) {
        throw '上传参数缺少 0xC10000 srmodels.bin；此时上传仍会得到空 model 分区。'
    }
}

$bootloader = [IO.File]::ReadAllBytes($bootloaderPath)
if ($bootloader.Length -lt 24 -or $bootloader[0] -ne 0xE9 -or ($bootloader[3] -shr 4) -ne 4) {
    throw 'Bootloader 镜像未声明 16MB Flash，使用 ESP SR 16M 分区会导致反复重启。'
}

$firmwareSize = (Get-Item -LiteralPath $firmwarePath).Length
if ($firmwareSize -gt 0x300000) {
    throw "应用程序大小 $firmwareSize 超过 3 MiB OTA 槽。"
}

Write-Host '离线唤醒构建检查通过。'
Write-Host "构建目录：$BuildDirectory"
Write-Host '分区方案：ESP SR 16M'
Write-Host 'Flash / PSRAM：16MB / OPI PSRAM'
Write-Host '模型写入：0xC10000 srmodels.bin'
Write-Host "上传参数来源：$modelUploadSource"
Write-Host "中文模型 SHA-256：$actualModelSha256"
Write-Host "应用程序大小：$firmwareSize 字节"
