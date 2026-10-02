[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^https://')]
    [string]$DownloadUrl,

    [Parameter(Mandatory = $true)]
    [string]$FirmwareBin,

    [Parameter(Mandatory = $true)]
    [string]$OutputManifest,

    [string]$OutputPackage,

    [ValidateSet('esp32s3-cloud-voice-v1', 'esp32s3-cloud-voice-multimic-v1')]
    [string]$HardwareId = 'esp32s3-cloud-voice-multimic-v1',

    [string]$PrivateKey = (Join-Path $PSScriptRoot '..\esp32_cloud_ota_device\ota_release_private\firmware_signing_private.pem'),

    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$firmwarePath = (Resolve-Path -LiteralPath $FirmwareBin).Path
$privateKeyPath = (Resolve-Path -LiteralPath $PrivateKey).Path
$outputPath = [System.IO.Path]::GetFullPath($OutputManifest)
$packagePath = if ($OutputPackage) { [System.IO.Path]::GetFullPath($OutputPackage) } else { $null }
$comparison = [System.StringComparison]::OrdinalIgnoreCase
if ($outputPath.Equals($firmwarePath, $comparison) -or $outputPath.Equals($privateKeyPath, $comparison)) {
    throw 'OutputManifest must not overwrite the firmware image or signing private key.'
}
if ($packagePath -and (
    $packagePath.Equals($firmwarePath, $comparison) -or
    $packagePath.Equals($privateKeyPath, $comparison) -or
    $packagePath.Equals($outputPath, $comparison)
)) {
    throw 'OutputPackage must be different from the firmware, private key, and manifest paths.'
}
$firmwareInfo = Get-Item -LiteralPath $firmwarePath
if ($firmwareInfo.Length -lt 65536 -or $firmwareInfo.Length -gt 0x300000) {
    throw "Firmware size $($firmwareInfo.Length) is outside the allowed 64 KiB..3 MiB range."
}
if ([System.IO.File]::ReadAllBytes($firmwarePath)[0] -ne 0xE9) {
    throw 'Firmware does not start with the ESP32 application image magic byte 0xE9.'
}

$sha256 = (Get-FileHash -LiteralPath $firmwarePath -Algorithm SHA256).Hash.ToLowerInvariant()
$hardware = $HardwareId
$forceText = if ($Force) { 'true' } else { 'false' }
$canonical = "$Version`n$hardware`n$($firmwareInfo.Length)`n$sha256`n$DownloadUrl`n$forceText"
$temporaryRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$temporaryDirectory = Join-Path $temporaryRoot ("esp32-ota-sign-" + [guid]::NewGuid().ToString('N'))
if (-not ([System.IO.Path]::GetFullPath($temporaryDirectory).StartsWith($temporaryRoot, [System.StringComparison]::OrdinalIgnoreCase))) {
    throw "Temporary signing directory escaped the system temporary directory."
}
New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null

try {
    $canonicalPath = Join-Path $temporaryDirectory 'manifest-canonical.txt'
    $signaturePath = Join-Path $temporaryDirectory 'manifest-signature.bin'
    [System.IO.File]::WriteAllText($canonicalPath, $canonical, [System.Text.UTF8Encoding]::new($false))
    & openssl dgst -sha256 -sign $privateKeyPath -out $signaturePath $canonicalPath
    if ($LASTEXITCODE -ne 0) {
        throw "OpenSSL signing failed with exit code $LASTEXITCODE."
    }
    $signature = [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($signaturePath))
    $manifest = [ordered]@{
        version = $Version
        hardware = $hardware
        size = $firmwareInfo.Length
        sha256 = $sha256
        url = $DownloadUrl
        signature = $signature
        force = [bool]$Force
    } | ConvertTo-Json -Depth 3

    $outputParent = Split-Path -Parent $outputPath
    if (-not (Test-Path -LiteralPath $outputParent)) {
        New-Item -ItemType Directory -Path $outputParent | Out-Null
    }
    [System.IO.File]::WriteAllText($outputPath, $manifest + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
    Write-Host "Signed OTA manifest: $outputPath"
    Write-Host "Firmware SHA-256: $sha256"
    if ($OutputPackage) {
        $packageParent = Split-Path -Parent $packagePath
        if (-not (Test-Path -LiteralPath $packageParent)) {
            New-Item -ItemType Directory -Path $packageParent | Out-Null
        }
        $packageDirectory = Join-Path $temporaryDirectory 'package'
        New-Item -ItemType Directory -Path $packageDirectory | Out-Null
        Copy-Item -LiteralPath $firmwarePath -Destination (Join-Path $packageDirectory 'firmware.bin')
        [System.IO.File]::WriteAllText(
            (Join-Path $packageDirectory 'manifest.json'),
            $manifest + [Environment]::NewLine,
            [System.Text.UTF8Encoding]::new($false)
        )
        Compress-Archive -LiteralPath @(
            (Join-Path $packageDirectory 'manifest.json'),
            (Join-Path $packageDirectory 'firmware.bin')
        ) -DestinationPath $packagePath -CompressionLevel Optimal -Force
        Write-Host "Upload-ready OTA package: $packagePath"
    }
}
finally {
    if (Test-Path -LiteralPath $temporaryDirectory) {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
    }
}
