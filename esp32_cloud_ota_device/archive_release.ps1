[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [string]$FirmwareBin,

    [Parameter(Mandatory = $true)]
    [string]$BuildOptionsJson,

    [string]$DownloadBaseUrl = 'https://voice.bsnlch.xyz'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$sketchRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$releasesRoot = Join-Path $sketchRoot 'releases'
$releaseRoot = Join-Path $releasesRoot $Version
$stagingRoot = Join-Path $releasesRoot ('.archive-' + $Version + '-' + [guid]::NewGuid().ToString('N'))
$firmwarePath = (Resolve-Path -LiteralPath $FirmwareBin).Path
$buildOptionsPath = (Resolve-Path -LiteralPath $BuildOptionsJson).Path

if (Test-Path -LiteralPath $releaseRoot) {
    throw "Release $Version already exists at $releaseRoot. Published versions are immutable; increase the version."
}

$sourcePath = Join-Path $sketchRoot 'esp32_cloud_ota_device.ino'
$sourceText = [System.IO.File]::ReadAllText($sourcePath)
$sourceVersion = [regex]::Match($sourceText, 'FIRMWARE_VERSION\[\]\s*=\s*"(\d+\.\d+\.\d+)"').Groups[1].Value
if ($sourceVersion -ne $Version) {
    throw "Source version is '$sourceVersion', but archive version is '$Version'."
}

$buildOptions = Get-Content -Raw -LiteralPath $buildOptionsPath | ConvertFrom-Json
$fqbn = [string]$buildOptions.fqbn
$requiredOptions = @(
    'USBMode=hwcdc',
    'CDCOnBoot=default',
    'FlashSize=16M',
    'PSRAM=opi'
)
foreach ($required in $requiredOptions) {
    if ($fqbn -notmatch ('(?:^|[,:])' + [regex]::Escape($required) + '(?:,|$)')) {
        throw "Unsafe build options: required '$required' is missing from FQBN '$fqbn'."
    }
}
$allowedPartitionSchemes = @(
    'PartitionScheme=esp_sr_16'
)
$hasAllowedPartitionScheme = $false
foreach ($partitionScheme in $allowedPartitionSchemes) {
    if ($fqbn -match ('(?:^|[,:])' + [regex]::Escape($partitionScheme) + '(?:,|$)')) {
        $hasAllowedPartitionScheme = $true
        break
    }
}
if (-not $hasAllowedPartitionScheme) {
    throw "Unsafe build options: select ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL). FQBN: '$fqbn'."
}
if ($fqbn -match '(?:^|[,:])EraseFlash=all(?:,|$)') {
    throw 'Unsafe build options: Erase All Flash Before Sketch Upload must be Disabled.'
}

$firmwareInfo = Get-Item -LiteralPath $firmwarePath
if ($firmwareInfo.Length -lt 65536 -or $firmwareInfo.Length -gt 0x300000) {
    throw "Firmware size $($firmwareInfo.Length) is outside the allowed 64 KiB..3 MiB range."
}
$firmwareBytes = [System.IO.File]::ReadAllBytes($firmwarePath)
if ($firmwareBytes[0] -ne 0xE9) {
    throw 'Firmware is not an ESP32 application image.'
}

New-Item -ItemType Directory -Path (Join-Path $stagingRoot 'source') -Force | Out-Null
try {
    $sourceFiles = @(
        'esp32_cloud_ota_device.ino',
        'hardware_pins.h',
        'alarm_manager.cpp',
        'alarm_manager.h',
        'memo_manager.cpp',
        'memo_manager.h',
        'offline_wake.cpp',
        'offline_wake.h',
        'firmware_public_key.h',
        'firmware_update.cpp',
        'firmware_update.h',
        'robot_controls.cpp',
        'robot_controls.h',
        'robot_display.cpp',
        'robot_display.h',
        'robot_avatar.cpp',
        'robot_avatar.h',
        'robot_expression_catalog.cpp',
        'robot_expression_catalog.h',
        'robot_expression_web.cpp',
        'robot_expression_web.h',
        'robot_photo.cpp',
        'robot_photo.h',
        'robot_photo_ui.h',
        'robot_servos.cpp',
        'robot_servos.h',
        'web_admin.cpp',
        'web_admin.h',
        'web_admin_ui.h',
        'web_display_ui.h',
        'web_expressions_ui.h',
        'web_network_ui.h',
        'wifi_profiles.cpp',
        'wifi_profiles.h',
        'wifi_scan.cpp',
        'wifi_scan.h',
        'voice_input.cpp',
        'voice_input.h',
        'voice_capture_settings.h',
        'voice_capture_settings.cpp',
        'web_voice_ui.h',
        'conversation_session.h',
        'partitions.esp_sr_16.csv',
        'secrets.example.h',
        'HARDWARE_WIRING.md',
        'WAKE_WORD_MODELS.md',
        'README.md',
        'tests/VOICE_CAPTURE_VERIFICATION.md',
        'install_offline_wake_models.ps1',
        'verify_offline_wake_build.ps1',
        'sign_firmware.ps1',
        'archive_release.ps1'
    )
    foreach ($file in $sourceFiles) {
        $destinationDirectory = Split-Path -Parent (Join-Path $stagingRoot "source/$file")
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $sketchRoot $file) -Destination (Join-Path $stagingRoot "source/$file")
    }
    $modelSource = Join-Path $sketchRoot 'wake_models\srmodels.bin'
    $modelReadme = Join-Path $sketchRoot 'wake_models\README.md'
    $modelStaging = Join-Path $stagingRoot 'source\wake_models'
    New-Item -ItemType Directory -Path $modelStaging -Force | Out-Null
    Copy-Item -LiteralPath $modelSource -Destination $modelStaging
    Copy-Item -LiteralPath $modelReadme -Destination $modelStaging
    Copy-Item -LiteralPath $firmwarePath -Destination (Join-Path $stagingRoot 'firmware.bin')
    Copy-Item -LiteralPath $buildOptionsPath -Destination (Join-Path $stagingRoot 'build.options.json')

    $sha256 = (Get-FileHash -LiteralPath (Join-Path $stagingRoot 'firmware.bin') -Algorithm SHA256).Hash.ToLowerInvariant()
    $partitionSha256 = (Get-FileHash -LiteralPath (Join-Path $sketchRoot 'partitions.esp_sr_16.csv') -Algorithm SHA256).Hash.ToLowerInvariant()
    $modelSha256 = (Get-FileHash -LiteralPath $modelSource -Algorithm SHA256).Hash.ToLowerInvariant()
    $releaseInfo = [ordered]@{
        version = $Version
        archived_at = [DateTimeOffset]::UtcNow.ToString('o')
        firmware_size = $firmwareInfo.Length
        firmware_sha256 = $sha256
        partition_table_sha256 = $partitionSha256
        wake_models_sha256 = $modelSha256
        fqbn = $fqbn
        source_snapshot = 'source/'
        status = 'candidate'
    } | ConvertTo-Json -Depth 4
    [System.IO.File]::WriteAllText(
        (Join-Path $stagingRoot 'release-info.json'),
        $releaseInfo + [Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false)
    )

    & (Join-Path $sketchRoot 'sign_firmware.ps1') `
        -Version $Version `
        -FirmwareBin (Join-Path $stagingRoot 'firmware.bin') `
        -DownloadUrl ($DownloadBaseUrl.TrimEnd('/') + "/voice-api/v1/firmware/download/$Version") `
        -OutputManifest (Join-Path $stagingRoot 'manifest.json') `
        -OutputPackage (Join-Path $stagingRoot "esp32-ota-$Version.zip")
    if ($LASTEXITCODE -ne 0) {
        throw "Firmware signing failed with exit code $LASTEXITCODE."
    }

    Move-Item -LiteralPath $stagingRoot -Destination $releaseRoot
    Write-Host "Immutable release archive created: $releaseRoot"
    Write-Host "Firmware SHA-256: $sha256"
}
finally {
    if (Test-Path -LiteralPath $stagingRoot) {
        $resolvedStaging = [System.IO.Path]::GetFullPath($stagingRoot)
        $resolvedReleases = [System.IO.Path]::GetFullPath($releasesRoot) + [System.IO.Path]::DirectorySeparatorChar
        if (-not $resolvedStaging.StartsWith($resolvedReleases, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing to clean a staging path outside the releases directory.'
        }
        Remove-Item -LiteralPath $resolvedStaging -Recurse -Force
    }
}
