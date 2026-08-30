$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$serverDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$configPath = Join-Path $serverDir "server_config.local.json"
$existing = $null
if (Test-Path -LiteralPath $configPath) {
    $existing = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
}

function Read-Default {
    param([string]$Prompt, [string]$DefaultValue)
    $value = Read-Host "$Prompt [$DefaultValue]"
    if ([string]::IsNullOrWhiteSpace($value)) { return $DefaultValue }
    return $value.Trim()
}

$defaultPort = if ($existing) { [string]$existing.port } else { "8765" }
$defaultDiscovery = if ($existing) { [string]$existing.discovery_port } else { "8764" }
$defaultModel = if ($existing) { [string]$existing.model } else { "deepseek-v4-flash" }
$defaultTokens = if ($existing) { [string]$existing.max_tokens } else { "4096" }
$defaultUrl = if ($existing) { [string]$existing.api_url } else { "https://api.deepseek.com/chat/completions" }
$defaultPrompt = if ($existing) { [string]$existing.system_prompt } else { "You are a concise, friendly and reliable Chinese voice assistant." }
$defaultVoice = if ($existing) { [string]$existing.voice } else { "Microsoft Huihui Desktop" }

$secureKey = Read-Host "DeepSeek API key (leave empty to keep the saved key)" -AsSecureString
$keyPtr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secureKey)
try {
    $apiKey = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($keyPtr)
}
finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($keyPtr)
}
if ([string]::IsNullOrWhiteSpace($apiKey) -and $existing) {
    $apiKey = [string]$existing.api_key
}

$modelDir = Join-Path $serverDir "models\sherpa-onnx-zipformer-ctc-zh-int8-2025-07-03"
$config = [ordered]@{
    bind_host = "0.0.0.0"
    port = [int](Read-Default "HTTP port" $defaultPort)
    discovery_port = [int](Read-Default "UDP discovery port" $defaultDiscovery)
    api_url = Read-Default "DeepSeek chat completion URL" $defaultUrl
    api_key = $apiKey
    model = Read-Default "DeepSeek model" $defaultModel
    max_tokens = [int](Read-Default "Maximum output tokens" $defaultTokens)
    thinking = $false
    system_prompt = Read-Default "System prompt" $defaultPrompt
    voice = Read-Default "Windows TTS voice" $defaultVoice
    max_record_seconds = 20
    job_ttl_hours = 12
    device_token = ""
    model_dir = [IO.Path]::GetFullPath($modelDir)
}

$json = $config | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText($configPath, $json, [Text.UTF8Encoding]::new($false))
Write-Host "Server configuration saved. The API key was not displayed."
