param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("asr", "tts", "voices")]
    [string]$Mode,
    [string]$InputPath = "",
    [string]$OutputPath = "",
    [string]$TextFile = "",
    [string]$Voice = "Microsoft Huihui Desktop"
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName System.Speech

function Write-JsonResult {
    param([hashtable]$Value)
    $Value | ConvertTo-Json -Compress -Depth 4
}

$recognitionEngine = $null
$synthesizer = $null

try {
    if ($Mode -eq "voices") {
        $synthesizer = [System.Speech.Synthesis.SpeechSynthesizer]::new()
        $voices = @($synthesizer.GetInstalledVoices() | ForEach-Object {
            @{
                name = $_.VoiceInfo.Name
                culture = $_.VoiceInfo.Culture.Name
                gender = $_.VoiceInfo.Gender.ToString()
                enabled = $_.Enabled
            }
        })
        Write-JsonResult @{ ok = $true; voices = $voices }
        exit 0
    }

    if ($Mode -eq "asr") {
        if (-not (Test-Path -LiteralPath $InputPath -PathType Leaf)) {
            throw "Input WAV file not found: $InputPath"
        }
        $recognizer = [System.Speech.Recognition.SpeechRecognitionEngine]::InstalledRecognizers() |
            Where-Object { $_.Culture.Name -eq "zh-CN" } |
            Select-Object -First 1
        if ($null -eq $recognizer) {
            throw "Windows zh-CN offline speech recognizer is not installed"
        }

        $recognitionEngine = [System.Speech.Recognition.SpeechRecognitionEngine]::new($recognizer)
        $recognitionEngine.LoadGrammar([System.Speech.Recognition.DictationGrammar]::new())
        $recognitionEngine.SetInputToWaveFile($InputPath)
        $result = $recognitionEngine.Recognize()
        if ($null -eq $result -or [string]::IsNullOrWhiteSpace($result.Text)) {
            Write-JsonResult @{ ok = $false; text = ""; confidence = 0; error = "No speech was recognized" }
        } else {
            Write-JsonResult @{
                ok = $true
                text = $result.Text.Trim()
                confidence = [Math]::Round($result.Confidence, 4)
                recognizer = $recognizer.Name
            }
        }
        exit 0
    }

    if (-not (Test-Path -LiteralPath $TextFile -PathType Leaf)) {
        throw "TTS text file not found: $TextFile"
    }
    if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        throw "TTS output WAV path is missing"
    }

    $text = Get-Content -LiteralPath $TextFile -Raw -Encoding UTF8
    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "TTS text is empty"
    }

    $synthesizer = [System.Speech.Synthesis.SpeechSynthesizer]::new()
    $installed = @($synthesizer.GetInstalledVoices() | Where-Object { $_.Enabled })
    $selected = $installed | Where-Object { $_.VoiceInfo.Name -eq $Voice } | Select-Object -First 1
    if ($null -eq $selected) {
        $selected = $installed | Where-Object { $_.VoiceInfo.Culture.Name -eq "zh-CN" } | Select-Object -First 1
    }
    if ($null -eq $selected) {
        throw "Windows Chinese offline TTS voice is not installed"
    }

    $synthesizer.SelectVoice($selected.VoiceInfo.Name)
    $format = [System.Speech.AudioFormat.SpeechAudioFormatInfo]::new(
        16000,
        [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
        [System.Speech.AudioFormat.AudioChannel]::Mono
    )
    $synthesizer.SetOutputToWaveFile($OutputPath, $format)
    $synthesizer.Speak($text)
    $synthesizer.SetOutputToNull()
    Write-JsonResult @{
        ok = $true
        output = $OutputPath
        voice = $selected.VoiceInfo.Name
        sample_rate = 16000
    }
    exit 0
} catch {
    Write-JsonResult @{ ok = $false; error = $_.Exception.Message }
    exit 1
} finally {
    if ($null -ne $recognitionEngine) { $recognitionEngine.Dispose() }
    if ($null -ne $synthesizer) { $synthesizer.Dispose() }
}
