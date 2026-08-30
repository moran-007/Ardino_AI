$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$serverDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$venvDir = Join-Path $serverDir ".venv"
$pythonExe = Join-Path $venvDir "Scripts\python.exe"
$modelsDir = Join-Path $serverDir "models"
$modelName = "sherpa-onnx-zipformer-ctc-zh-int8-2025-07-03"
$modelDir = Join-Path $modelsDir $modelName
$modelArchive = Join-Path $modelsDir ($modelName + ".tar.bz2")
$modelUrl = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/$modelName.tar.bz2"

if (-not (Test-Path -LiteralPath $pythonExe)) {
    py -3.11 -m venv $venvDir
    if ($LASTEXITCODE -ne 0) {
        throw "Python 3.11 virtual environment creation failed with exit code $LASTEXITCODE."
    }
}

& $pythonExe -m pip install --disable-pip-version-check -r (Join-Path $serverDir "requirements.txt")
if ($LASTEXITCODE -ne 0) {
    throw "Python dependency installation failed with exit code $LASTEXITCODE."
}
New-Item -ItemType Directory -Force -Path $modelsDir | Out-Null

$modelFile = Join-Path $modelDir "model.int8.onnx"
$tokensFile = Join-Path $modelDir "tokens.txt"
if (-not (Test-Path -LiteralPath $modelFile) -or -not (Test-Path -LiteralPath $tokensFile)) {
    Write-Host "Downloading the 350 MB Chinese Zipformer CTC INT8 model..."
    & curl.exe -L --fail --retry 3 --continue-at - --output $modelArchive $modelUrl
    if ($LASTEXITCODE -ne 0) {
        throw "Zipformer model download failed with exit code $LASTEXITCODE."
    }
    & tar.exe -xf $modelArchive -C $modelsDir
    if ($LASTEXITCODE -ne 0) {
        throw "Zipformer model extraction failed with exit code $LASTEXITCODE."
    }
    Remove-Item -LiteralPath $modelArchive -Force
}

if (-not (Test-Path -LiteralPath $modelFile) -or -not (Test-Path -LiteralPath $tokensFile)) {
    throw "Zipformer model is incomplete: $modelDir"
}

& $pythonExe -B (Join-Path $serverDir "test_audio_frontend.py")
if ($LASTEXITCODE -ne 0) {
    throw "Audio frontend tests failed with exit code $LASTEXITCODE."
}
& $pythonExe -B -m unittest -v (Join-Path $serverDir "test_server_components.py")
if ($LASTEXITCODE -ne 0) {
    throw "LAN server tests failed with exit code $LASTEXITCODE."
}
& $pythonExe -B (Join-Path $serverDir "lan_dialogue_server.py") --check
if ($LASTEXITCODE -ne 0) {
    throw "LAN server environment check failed with exit code $LASTEXITCODE."
}
Write-Host "LAN server environment check passed."
