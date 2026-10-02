from __future__ import annotations

import json
import math
import re
import threading
from urllib.parse import urlsplit, urlunsplit
from collections.abc import Iterator
from dataclasses import dataclass

import httpx
import numpy as np

from .config import Settings


class ProviderError(RuntimeError):
    pass


def provider_error(name: str, exc: Exception) -> ProviderError:
    # httpx header/URL validation errors can include the credential itself.
    if isinstance(exc, httpx.HTTPStatusError):
        detail = f"HTTP {exc.response.status_code}"
    elif isinstance(exc, httpx.TimeoutException):
        detail = "请求超时"
    else:
        detail = type(exc).__name__
    return ProviderError(f"{name} 调用失败（{detail}）")


def qwen_native_endpoint(config: "LLMConfig") -> tuple[str, bool]:
    """Use the documented native protocol for known Qwen model families.

    Explicit multimodal/custom URLs remain authoritative. Model exceptions are
    from https://help.aliyun.com/zh/model-studio/text-generation (2026-09-30).
    """
    parts = urlsplit(config.base_url)
    text_suffix = "/api/v1/services/aigc/text-generation/generation"
    multimodal_suffix = "/api/v1/services/aigc/multimodal-generation/generation"
    model = config.model.lower()
    text_only = {"qwen3.7-max", "qwen3.7-max-2026-05-20", "qwen3.6-max-preview", "qwen3.8-2.4t-a95b"}
    multimodal = (
        model not in text_only
        and "omni" not in model and "realtime" not in model
        and (model.startswith(("qwen3.5-", "qwen3.6-", "qwen3.8-", "qwen3.7-plus", "qwen3.7-flash", "qwen3-vl", "qwen2.5-vl", "qwen-vl", "qvq"))
             or model == "qwen3.7-max-2026-06-08")
    )
    if multimodal and parts.path.rstrip("/").endswith(text_suffix):
        path = parts.path.rstrip("/")[:-len(text_suffix)] + multimodal_suffix
        parts = parts._replace(path=path)
    return urlunsplit(parts), parts.path.rstrip("/").endswith(multimodal_suffix)


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


@dataclass
class TokenUsage:
    """One request's latest cumulative provider usage, never estimated."""

    input_tokens: int | None = None
    output_tokens: int | None = None
    total_tokens: int | None = None
    cached_input_tokens: int | None = None
    search_calls: int | None = 0

    def update(self, payload) -> None:
        if not isinstance(payload, dict):
            return
        inputs = payload.get("prompt_tokens", payload.get("input_tokens"))
        outputs = payload.get("completion_tokens", payload.get("output_tokens"))
        if not all(type(value) is int and 0 <= value < 2**60 for value in (inputs, outputs)):
            return
        total = payload.get("total_tokens", inputs + outputs)
        if type(total) is not int or total < inputs + outputs or total >= 2**61:
            return
        self.input_tokens, self.output_tokens, self.total_tokens = inputs, outputs, total
        details = payload.get('prompt_tokens_details')
        details = details if isinstance(details, dict) else {}
        cached = payload.get('prompt_cache_hit_tokens', details.get('cached_tokens'))
        if type(cached) is int and 0 <= cached <= inputs:
            self.cached_input_tokens = cached
        plugins = payload.get('plugins')
        search_info = plugins.get('search') if isinstance(plugins, dict) else None
        search = search_info.get('count') if isinstance(search_info, dict) else None
        if type(search) is int and 0 <= search < 2**30:
            self.search_calls = search


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
        return self.stream_with_usage(question, system_prompt, TokenUsage())

    def stream_with_usage(self, question: str, system_prompt: str, usage: TokenUsage) -> Iterator[str]:
        if not self.config.api_key and self.name not in {'custom', 'local_llm'}:
            raise ProviderError(f"{self.name} API Key 未配置")
        url = self.config.base_url.rstrip("/")
        if not url.endswith('/chat/completions'):
            url += '/chat/completions'
        payload = {"model": self.config.model, "messages": [{"role": "system", "content": system_prompt}, {"role": "user", "content": question}], "stream": True, "temperature": 0.6}
        if self.name != "glm":
            # GLM's native stream already includes usage; do not send an
            # OpenAI-only request option to that official endpoint.
            payload["stream_options"] = {"include_usage": True}
        if self.name == "deepseek":
            # DeepSeek V4 defaults to high-effort thinking. Voice dialogue
            # favors time-to-first-audio, so explicitly use its official
            # non-thinking Chat Completions mode.
            payload["thinking"] = {"type": "disabled"}
        try:
            headers = {"Authorization": f"Bearer {self.config.api_key}"} if self.config.api_key else {}
            with httpx.stream("POST", url, headers=headers, json=payload, timeout=90) as response:
                response.raise_for_status()
                for line in response.iter_lines():
                    if not line.startswith("data:"):
                        continue
                    data = line[5:].strip()
                    if data == "[DONE]":
                        break
                    if not data:
                        continue
                    packet = json.loads(data)
                    if packet.get("error"):
                        raise ProviderError(f"{self.name} 返回接口错误")
                    usage.update(packet.get("usage"))
                    choices = packet.get("choices") or []
                    content = choices[0].get("delta", {}).get("content") if choices else None
                    if content:
                        yield content
        except Exception as exc:
            raise provider_error(self.name, exc) from exc


class OllamaLLM:
    """Native NDJSON chat protocol; no model installation or implicit pulling."""

    def __init__(self, config: LLMConfig):
        self.config, self.name = config, config.name

    def stream(self, question, system_prompt):
        return self.stream_with_usage(question, system_prompt, TokenUsage())

    def stream_with_usage(self, question, system_prompt, usage):
        url = self.config.base_url.rstrip('/')
        if not url.endswith('/api/chat'):
            url += '/chat' if url.endswith('/api') else '/api/chat'
        payload = {'model': self.config.model, 'messages': [
            {'role': 'system', 'content': system_prompt}, {'role': 'user', 'content': question}],
            'stream': True, 'think': False, 'options': {'temperature': .6}}
        headers = {'Authorization': 'Bearer ' + self.config.api_key} if self.config.api_key else {}
        completed = False
        try:
            with httpx.stream('POST', url, headers=headers, json=payload, timeout=90) as response:
                response.raise_for_status()
                for line in response.iter_lines():
                    if not line.strip():
                        continue
                    packet = json.loads(line)
                    if not isinstance(packet, dict) or packet.get('error'):
                        raise ProviderError(f'{self.name} 返回接口错误')
                    content = (packet.get('message') or {}).get('content')
                    if isinstance(content, str) and content:
                        yield content
                    if packet.get('done') is True:
                        usage.update({'input_tokens': packet.get('prompt_eval_count'),
                                      'output_tokens': packet.get('eval_count'),
                                      'prompt_cache_hit_tokens': packet.get('prompt_eval_cached_count')})
                        completed = True
                        break
            if not completed:
                raise ProviderError(f'{self.name} 流式响应未完成')
        except Exception as exc:
            raise provider_error(self.name, exc) from exc


class QwenDashScopeLLM:
    """Qwen's native DashScope HTTP/SSE protocol on the China endpoint."""

    name = "qwen"

    def __init__(self, config: LLMConfig):
        self.config = config

    @property
    def supports_search(self):
        model = self.config.model.lower()
        return bool(self.config.api_key) and model.startswith((
            'qwen-plus', 'qwen-max', 'qwen-turbo', 'qwen-flash', 'qwen3-max',
            'qwen3.5-plus', 'qwen3.5-flash', 'qwen3.6-plus', 'qwen3.6-flash',
            'qwen3.7-plus', 'qwen3.7-flash', 'qwen3.7-max', 'qwen3.8-max', 'qwen3.8-flash'))

    def stream(self, question: str, system_prompt: str) -> Iterator[str]:
        return self.stream_with_usage(question, system_prompt, TokenUsage())

    def stream_with_usage(self, question: str, system_prompt: str, usage: TokenUsage, *, web_search: bool = False) -> Iterator[str]:
        if not self.config.api_key:
            raise ProviderError("qwen API Key 未配置")
        url, multimodal = qwen_native_endpoint(self.config)
        payload = {
            "model": self.config.model,
            "input": {
                "messages": [
                    {"role": "system", "content": [{"text": system_prompt}] if multimodal else system_prompt},
                    {"role": "user", "content": [{"text": question}] if multimodal else question},
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
        if web_search:
            if not self.supports_search:
                raise ProviderError('当前千问模型未接入联网搜索')
            usage.search_calls = None
            payload['parameters'].update(enable_search=True, search_options={
                'forced_search': True, 'search_strategy': 'turbo', 'enable_source': True})
        pending = []
        search_confirmed = False
        try:
            with httpx.stream("POST", url, headers=headers, json=payload, timeout=90) as response:
                response.raise_for_status()
                for line in response.iter_lines():
                    if not line.startswith("data:"):
                        continue
                    data = line[5:].strip()
                    if not data or data == "[DONE]":
                        continue
                    packet = json.loads(data)
                    if packet.get("code") or packet.get("error"):
                        raise ProviderError("qwen 返回接口错误")
                    usage.update(packet.get("usage"))
                    search_confirmed = search_confirmed or bool(usage.search_calls)
                    choices = packet.get("output", {}).get("choices") or []
                    content = choices[0].get("message", {}).get("content") if choices else None
                    if isinstance(content, list):
                        content = "".join(part["text"] for part in content if isinstance(part, dict) and isinstance(part.get("text"), str))
                    if isinstance(content, str) and content:
                        if web_search and not search_confirmed:
                            pending.append(content)
                        else:
                            yield from pending
                            pending.clear()
                            yield content
                if web_search and not search_confirmed:
                    usage.search_calls = 0
                    raise ProviderError('本次搜索未执行，不能提供未核实的实时结果')
                yield from pending
        except Exception as exc:
            raise provider_error("qwen", exc) from exc


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


_ENGLISH_TOKEN = r"[A-Za-z0-9][A-Za-z0-9'’._]*(?:[-/][A-Za-z0-9]+)*"
_ENGLISH_RUN = re.compile(
    rf"(?=[A-Za-z0-9'’._\-/ \t]*[A-Za-z]){_ENGLISH_TOKEN}"
    rf"(?:[ \t]+{_ENGLISH_TOKEN})*"
)
_DATE_RUN = re.compile(
    r"(?<!\d)(\d{4})\s*[-－/]\s*(\d{1,2})\s*[-－/]\s*(\d{1,2})(?!\d)"
)
_PERCENT_RUN = re.compile(r"(\d+(?:\.\d+)?)\s*[%％]")
_CURRENCY_RUN = re.compile(r"([￥¥$€£])\s*(\d+(?:\.\d+)?)")
_CURRENCY_WORDS = {
    "￥": "元",
    "¥": "元",
    "$": "美元",
    "€": "欧元",
    "£": "英镑",
}
_CELSIUS_RUN = re.compile(r"(\d+(?:\.\d+)?)\s*(?:°\s*C\b|℃)")
_FAHRENHEIT_RUN = re.compile(r"(\d+(?:\.\d+)?)\s*(?:°\s*F\b|℉)")
_UNIT_WORDS = {
    "km/h": "千米每小时",
    "m/s": "米每秒",
    "mAh": "毫安时",
    "kWh": "千瓦时",
    "GHz": "吉赫兹",
    "MHz": "兆赫兹",
    "kHz": "千赫兹",
    "mV": "毫伏",
    "mA": "毫安",
    "kW": "千瓦",
    "GB": "吉字节",
    "MB": "兆字节",
    "KB": "千字节",
    "Hz": "赫兹",
    "kg": "千克",
    "mg": "毫克",
    "km": "千米",
    "cm": "厘米",
    "mm": "毫米",
    "mL": "毫升",
    "ms": "毫秒",
    "Ah": "安时",
    "V": "伏特",
    "A": "安培",
    "W": "瓦",
    "L": "升",
    "g": "克",
    "m": "米",
    "s": "秒",
}
_UNIT_RUN = re.compile(
    r"(\d+(?:\.\d+)?)\s*("
    + "|".join(re.escape(unit) for unit in sorted(_UNIT_WORDS, key=len, reverse=True))
    + r")(?![A-Za-z])"
)
_CHEMICAL_NAMES = {
    "C2H5OH": "乙醇",
    "C6H12O6": "葡萄糖",
    "CaCO3": "碳酸钙",
    "H2SO4": "硫酸",
    "NaOH": "氢氧化钠",
    "HNO3": "硝酸",
    "NaCl": "氯化钠",
    "CH4": "甲烷",
    "NH3": "氨",
    "HCl": "氯化氢",
    "CO2": "二氧化碳",
    "H2O": "水",
    "O2": "氧气",
    "O3": "臭氧",
    "N2": "氮气",
    "H2": "氢气",
    "CO": "一氧化碳",
    "CO₂": "二氧化碳",
    "H₂O": "水",
    "O₂": "氧气",
    "O₃": "臭氧",
    "N₂": "氮气",
    "H₂": "氢气",
}
_CHEMICAL_RUN = re.compile(
    r"(?<![A-Za-z0-9])(?:"
    + "|".join(
        re.escape(formula) for formula in sorted(_CHEMICAL_NAMES, key=len, reverse=True)
    )
    + r")(?![A-Za-z0-9])"
)
_ARITHMETIC_OPERATOR = re.compile(
    r"(?<=[0-9)）])\s*([+＋\-－−*＊×/／÷=＝])\s*(?=[0-9(（])"
)
_ARITHMETIC_WORDS = {
    "+": "加",
    "＋": "加",
    "-": "减",
    "－": "减",
    "−": "减",
    "*": "乘以",
    "＊": "乘以",
    "×": "乘以",
    "/": "除以",
    "／": "除以",
    "÷": "除以",
    "=": "等于",
    "＝": "等于",
}
_RANGE_UNIT_AFTER_OPERAND = re.compile(
    r"\s*\d+(?:\.\d+)?\s*(?:岁|年|月|天|小时|分钟|秒|个|人|次|件|元|米|千米|厘米|毫米|克|千克)"
)
_ARITHMETIC_CONTEXT_CHARS = frozenset("0123456789.()（）+＋-－−*＊×/／÷=＝ \t")


def _normalize_for_speech(text: str) -> str:
    """Normalize common numeric domains before Chinese/English TTS routing."""
    text = _DATE_RUN.sub(
        lambda match: f"{match.group(1)}年{match.group(2)}月{match.group(3)}日",
        text,
    )
    text = _CURRENCY_RUN.sub(
        lambda match: f"{match.group(2)}{_CURRENCY_WORDS[match.group(1)]}",
        text,
    )
    text = _PERCENT_RUN.sub(lambda match: f"百分之{match.group(1)}", text)
    text = _CELSIUS_RUN.sub(lambda match: f"{match.group(1)}摄氏度", text)
    text = _FAHRENHEIT_RUN.sub(lambda match: f"{match.group(1)}华氏度", text)
    text = _UNIT_RUN.sub(
        lambda match: f"{match.group(1)}{_UNIT_WORDS[match.group(2)]}",
        text,
    )
    text = _CHEMICAL_RUN.sub(lambda match: _CHEMICAL_NAMES[match.group()], text)

    def replace_arithmetic(match: re.Match[str]) -> str:
        operator = match.group(1)
        if operator not in {"-", "－", "−"}:
            return _ARITHMETIC_WORDS[operator]
        if _RANGE_UNIT_AFTER_OPERAND.match(text[match.end() :]):
            return match.group()

        context_start = match.start()
        while context_start > 0 and text[context_start - 1] in _ARITHMETIC_CONTEXT_CHARS:
            context_start -= 1
        context_end = match.end()
        while context_end < len(text) and text[context_end] in _ARITHMETIC_CONTEXT_CHARS:
            context_end += 1
        context = text[context_start:context_end]
        if "=" in context or "＝" in context or match.group() != operator:
            return _ARITHMETIC_WORDS[operator]

        left_operand = re.search(r"(\d+)\s*$", text[: match.start()])
        right_operand = re.match(r"\s*(\d+)", text[match.end() :])
        if left_operand and right_operand and max(
            len(left_operand.group(1)), len(right_operand.group(1))
        ) <= 2:
            return _ARITHMETIC_WORDS[operator]
        return match.group()

    return _ARITHMETIC_OPERATOR.sub(replace_arithmetic, text)


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
        speakable_text = _normalize_for_speech(text)
        for language, value in mixed_language_parts(speakable_text):
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
        "custom": LLMConfig('custom', settings.custom_api_key, settings.custom_base_url, settings.custom_model),
        "local_llm": LLMConfig('local_llm', settings.local_llm_api_key, settings.local_llm_base_url, settings.local_llm_model),
    }
    providers = []
    for name in settings.llm_provider_order:
        if name == "mock":
            providers.append(MockLLM())
        elif name == "qwen" and "/api/v1/services/" in configs[name].base_url:
            providers.append(QwenDashScopeLLM(configs[name]))
        elif name in {'custom', 'local_llm'} and getattr(settings, name + '_protocol') == 'ollama':
            providers.append(OllamaLLM(configs[name]))
        else:
            providers.append(OpenAICompatibleLLM(configs[name]))
    return providers
