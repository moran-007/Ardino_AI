$ErrorActionPreference = "Stop"
$env:VOICE_ALLOW_SIMULATED_INPUT = "true"
$env:VOICE_ASR_PROVIDER = "mock"
$env:VOICE_TTS_PROVIDER = "mock"
$env:VOICE_LLM_ORDER = "mock"
$env:VOICE_AUTH_PEPPER = "local-simulator-only-pepper"

$python = Join-Path $PSScriptRoot "..\pc_server\.venv\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $python)) {
    throw "Run pc_server\setup_server.ps1 first, or create a Python environment for cloud_server."
}
& $python -m uvicorn cloud_server.app:app --host 127.0.0.1 --port 18765
