from __future__ import annotations

import json
import math
import re
import threading
from collections.abc import Iterator
from dataclasses import dataclass

import httpx
import numpy as np

from .config import Settings


class ProviderError(RuntimeError):
    pass


class MockASR:
    def recognize(self, pcm: bytes, sample_rate: int) -> str:
        return "电脑模拟语音问题"


class SenseVoiceASR:
    def __init__(self, settings: Settings):
        if not settings.sensevoice_model or not settings.sensevoice_tokens:
            raise ProviderError("SENSEVOICE_MODEL/SENSEVOICE_TOKENS 未配置")
        import sherpa_onnx

        self.recognizer = sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=str(settings.sensevoice_model), tokens=str(settings.sensevoice_tokens), num_threads=2, use_itn=True
        )

    def recognize(self, pcm: bytes, sample_rate: int) -> str:
        samples = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
        stream = self.recognizer.create_stream()
        stream.accept_waveform(sample_rate, samples)
        self.recognizer.decode_stream(stream)
        return stream.result.text.strip()


@dataclass(frozen=True)
class LLMConfig:
    name: str
    api_key: str
    base_url: str
    model: str


class MockLLM:
    name = "mock"

    def stream(self, question: str, system_prompt: str) -> Iterator[str]:
        answer = f"已收到你的问题：{question}。这是电脑模拟生成的第一段回答。第二段用于验证流式播放。"
        for start in range(0, len(answer), 7):
            yield answer[start : start + 7]


class OpenAICompatibleLLM:
    def __init__(self, config: LLMConfig):
        self.config = config
        self.name = config.name

    def stream(self, question: str, system_prompt: str) -> Iterator[str]:
        if not self.config.api_key:
            raise ProviderError(f"{self.name} API Key 未配置")
        url = self.config.base_url.rstrip("/") + "/chat/completions"
        payload = {"model": self.config.model, "messages": [{"role": "system", "content": system_prompt}, {"role": "user", "content": question}], "stream": True, "temperature": 0.6}
        if self.name == "deepseek":
            # DeepSeek V4 defaults to high-effort thinking. Voice dialogue
            # favors time-to-first-audio, so explicitly use its official
            # non-thinking Chat Completions mode.
            payload["thinking"] = {"type": "disabled"}
        try:
            with httpx.stream("POST", url, headers={"Authorization": f"Bearer {self.config.api_key}"}, json=payload, timeout=90) as response:
                response.raise_for_status()
                for line in response.iter_lines():
                    if not line.startswith("data:"):
                        continue
                    data = line[5:].strip()
                    if data == "[DONE]":
                        break
                    content = json.loads(data).get("choices", [{}])[0].get("delta", {}).get("content")
                    if content:
                        yield content
        except Exception as exc:
            raise ProviderError(f"{self.name} 调用失败: {exc}") from exc


class QwenDashScopeLLM:
    """Qwen's native DashScope HTTP/SSE protocol on the China endpoint."""

    name = "qwen"

    def __init__(self, config: LLMConfig):
        self.config = config

    def stream(self, question: str, system_prompt: str) -> Iterator[str]:
        if not self.config.api_key:
            raise ProviderError("qwen API Key 未配置")
        payload = {
            "model": self.config.model,
            "input": {
                "messages": [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": question},
                ]
            },
            "parameters": {
                "result_format": "message",
                "incremental_output": True,
                "enable_thinking": False,
            },
        }
        headers = {
            "Authorization": f"Bearer {self.config.api_key}",
            "Content-Type": "application/json",
            "X-DashScope-SSE": "enable",
        }
        try:
            with httpx.stream("POST", self.config.base_url, headers=headers, json=payload, timeout=90) as response:
                response.raise_for_status()
                for line in response.iter_lines():
                    if not line.startswith("data:"):
                        continue
                    data = line[5:].strip()
                    if not data or data == "[DONE]":
                        continue
                    content = (
                        json.loads(data)
                        .get("output", {})
                        .get("choices", [{}])[0]
                        .get("message", {})
                        .get("content")
                    )
                    if content:
                        yield content
        except Exception as exc:
            raise ProviderError(f"qwen 调用失败: {exc}") from exc


class MockTTS:
    sample_rate = 16_000
    speed = 1.0

    def synthesize(
        self,
        text: str,
        speaker_id: int | None = None,
        speed: float | None = None,
        english_speaker_id: int | None = None,
    ) -> bytes:
        duration = max(0.18, min(2.0, len(text) * 0.045))
        count = int(self.sample_rate * duration)
        t = np.arange(count, dtype=np.float32) / self.sample_rate
        samples = (np.sin(2 * math.pi * 440 * t) * 0.08 * 32767).astype("<i2")
        return samples.tobytes()


class SherpaVitsTTS:
    def __init__(self, settings: Settings):
        if not settings.sherpa_tts_model or not settings.sherpa_tts_tokens:
            raise ProviderError("SHERPA_TTS_MODEL/SHERPA_TTS_TOKENS 未配置")
        import sherpa_onnx

        model_dir = settings.sherpa_tts_model.parent
        vits = sherpa_onnx.OfflineTtsVitsModelConfig(
            model=str(settings.sherpa_tts_model),
            tokens=str(settings.sherpa_tts_tokens),
            lexicon=str(settings.sherpa_tts_lexicon or ""),
            data_dir="",
        )
        model_config = sherpa_onnx.OfflineTtsModelConfig(vits=vits, num_threads=2)
        rule_fsts = ",".join(
            str(path)
            for path in (model_dir / "phone.fst", model_dir / "date.fst", model_dir / "number.fst")
            if path.exists()
        )
        config = sherpa_onnx.OfflineTtsConfig(model=model_config, rule_fsts=rule_fsts)
        if not config.validate():
            raise ProviderError(
                "sherpa-onnx VITS 配置无效: "
                f"model={settings.sherpa_tts_model}, tokens={settings.sherpa_tts_tokens}, "
                f"lexicon={settings.sherpa_tts_lexicon}, rule_fsts={rule_fsts}"
            )
        self.tts = sherpa_onnx.OfflineTts(config)
        self.speaker_id = settings.sherpa_tts_speaker_id
        self.speed = settings.sherpa_tts_speed
        self.source_sample_rate = int(self.tts.sample_rate)
        self.sample_rate = 16_000
        self._lock = threading.Lock()

    def synthesize(
        self,
        text: str,
        speaker_id: int | None = None,
        speed: float | None = None,
        english_speaker_id: int | None = None,
    ) -> bytes:
        active_speaker = self.speaker_id if speaker_id is None else int(speaker_id)
        active_speed = self.speed if speed is None else float(speed)
        with self._lock:
            audio = self.tts.generate(text, sid=active_speaker, speed=active_speed)
        return _samples_to_pcm(audio.samples, self.source_sample_rate, self.sample_rate)


class PiperEnglishTTS:
    """Free local Piper English TTS, normalized to the gateway's 16 kHz PCM."""

    sample_rate = 16_000

    def __init__(self, settings: Settings):
        if not settings.sherpa_english_tts_model or not settings.sherpa_english_tts_tokens:
            raise ProviderError("SHERPA_ENGLISH_TTS_MODEL/SHERPA_ENGLISH_TTS_TOKENS 未配置")
        import sherpa_onnx

        vits = sherpa_onnx.OfflineTtsVitsModelConfig(
            model=str(settings.sherpa_english_tts_model),
            tokens=str(settings.sherpa_english_tts_tokens),
            data_dir=str(settings.sherpa_english_tts_data_dir or ""),
        )
        model_config = sherpa_onnx.OfflineTtsModelConfig(vits=vits, num_threads=2)
        config = sherpa_onnx.OfflineTtsConfig(model=model_config)
        if not config.validate():
            raise ProviderError("Piper 英文 VITS 配置无效")
        self.tts = sherpa_onnx.OfflineTts(config)
        self.source_sample_rate = int(self.tts.sample_rate)
        self._lock = threading.Lock()

    def synthesize(self, text: str, speaker_id: int = 0, speed: float = 1.0) -> bytes:
        with self._lock:
            audio = self.tts.generate(text, sid=int(speaker_id), speed=float(speed))
        return _samples_to_pcm(audio.samples, self.source_sample_rate, self.sample_rate)


def _samples_to_pcm(samples, source_sample_rate: int, target_sample_rate: int) -> bytes:
    samples = np.clip(samples, -1.0, 1.0)
    if source_sample_rate != target_sample_rate and len(samples) > 1:
        output_count = round(len(samples) * target_sample_rate / source_sample_rate)
        source_positions = np.arange(len(samples), dtype=np.float64)
        output_positions = np.linspace(0, len(samples) - 1, output_count)
        samples = np.interp(output_positions, source_positions, samples).astype(np.float32)
    return (samples * 32767).astype("<i2").tobytes()


_ENGLISH_RUN = re.compile(
    r"[A-Za-z0-9][A-Za-z0-9'’._+\-/]*(?:[ \t]+[A-Za-z0-9][A-Za-z0-9'’._+\-/]*)*"
)


def mixed_language_parts(text: str) -> list[tuple[str, str]]:
    """Return ordered Chinese/English runs without losing punctuation."""
    parts: list[tuple[str, str]] = []
    cursor = 0
    for match in _ENGLISH_RUN.finditer(text):
        if match.start() > cursor:
            parts.append(("zh", text[cursor : match.start()]))
        parts.append(("en", match.group()))
        cursor = match.end()
    if cursor < len(text):
        parts.append(("zh", text[cursor:]))

    normalized: list[tuple[str, str]] = []
    for language, value in parts:
        if not value:
            continue
        if not re.search(r"[A-Za-z0-9\u3400-\u9fff]", value) and normalized:
            previous_language, previous_value = normalized[-1]
            normalized[-1] = (previous_language, previous_value + value)
        elif normalized and normalized[-1][0] == language:
            previous_language, previous_value = normalized[-1]
            normalized[-1] = (previous_language, previous_value + value)
        else:
            normalized.append((language, value))
    return normalized


class MixedLanguageTTS:
    """Route Latin runs to Piper and Chinese runs to the existing VITS model."""

    sample_rate = 16_000

    def __init__(self, chinese: SherpaVitsTTS, english: PiperEnglishTTS):
        self.chinese = chinese
        self.english = english
        self.speed = chinese.speed

    def synthesize(
        self,
        text: str,
        speaker_id: int | None = None,
        speed: float | None = None,
        english_speaker_id: int | None = None,
    ) -> bytes:
        active_speed = self.speed if speed is None else float(speed)
        output: list[bytes] = []
        last_language = ""
        for language, value in mixed_language_parts(text):
            if last_language and language != last_language:
                output.append(bytes(round(self.sample_rate * 0.02) * 2))
            if language == "en":
                output.append(
                    self.english.synthesize(value, int(english_speaker_id or 0), active_speed)
                )
            else:
                output.append(
                    self.chinese.synthesize(value, speaker_id=speaker_id, speed=active_speed)
                )
            last_language = language
        return b"".join(output)


def sentence_segments(chunks: Iterator[str], minimum: int, maximum: int) -> Iterator[str]:
    """Split streamed Chinese text into short, speakable TTS units.

    A short sentence is allowed to join the following sentence, but it must not
    pin the parser forever at its first punctuation mark.  Closing quotes are
    kept with their sentence when they arrive in the next LLM chunk.
    """
    buffer = ""
    strong = re.compile(r"[。！？!?；;\n]")
    weak = re.compile(r"[，,、：:]")
    closers = "”’」』）》】)]"
    comma_threshold = min(maximum, max(minimum * 2, 20))

    def after_closers(end: int) -> int:
        while end < len(buffer) and buffer[end] in closers:
            end += 1
        return end

    def boundary(final: bool) -> int | None:
        for match in strong.finditer(buffer):
            end = after_closers(match.end())
            if end == len(buffer) and not final and match.group() != "\n":
                continue
            if end >= minimum:
                return end

        for match in weak.finditer(buffer):
            end = after_closers(match.end())
            if end == len(buffer) and not final:
                continue
            if end >= comma_threshold:
                return end

        if len(buffer) < maximum:
            return None

        candidates = [
            after_closers(match.end())
            for match in re.finditer(r"[。！？!?；;，,、：:\s]", buffer[:maximum])
            if match.end() >= minimum
        ]
        return max(candidates) if candidates else maximum

    def drain(final: bool) -> Iterator[str]:
        nonlocal buffer
        while buffer:
            end = boundary(final)
            if end is None:
                break
            text = buffer[:end].strip()
            buffer = buffer[end:].lstrip()
            if text:
                yield text

    for chunk in chunks:
        buffer += chunk
        yield from drain(final=False)
    yield from drain(final=True)
    if buffer.strip():
        yield buffer.strip()


def build_asr(settings: Settings):
    return MockASR() if settings.asr_provider == "mock" else SenseVoiceASR(settings)


def build_tts(settings: Settings):
    if settings.tts_provider == "mock":
        return MockTTS()
    chinese = SherpaVitsTTS(settings)
    if settings.sherpa_english_tts_model and settings.sherpa_english_tts_tokens:
        return MixedLanguageTTS(chinese, PiperEnglishTTS(settings))
    return chinese


def build_llms(settings: Settings):
    configs = {
        "deepseek": LLMConfig("deepseek", settings.deepseek_api_key, settings.deepseek_base_url, settings.deepseek_model),
        "qwen": LLMConfig("qwen", settings.qwen_api_key, settings.qwen_base_url, settings.qwen_model),
        "glm": LLMConfig("glm", settings.glm_api_key, settings.glm_base_url, settings.glm_model),
    }
    providers = []
    for name in settings.llm_provider_order:
        if name == "mock":
            providers.append(MockLLM())
        elif name == "qwen" and "/api/v1/services/" in configs[name].base_url:
            providers.append(QwenDashScopeLLM(configs[name]))
        else:
            providers.append(OpenAICompatibleLLM(configs[name]))
    return providers
