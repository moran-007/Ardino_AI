from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


def _bool(name: str, default: bool = False) -> bool:
    return os.getenv(name, str(default)).strip().lower() in {"1", "true", "yes", "on"}


def _int(name: str, default: int) -> int:
    return int(os.getenv(name, str(default)))


@dataclass(frozen=True)
class Settings:
    data_dir: Path
    database_path: Path
    auth_pepper: str
    allow_simulated_input: bool
    max_audio_bytes: int
    request_limit_per_minute: int
    worker_concurrency: int
    llm_provider_order: tuple[str, ...]
    tts_provider: str
    asr_provider: str
    system_prompt: str
    segment_min_chars: int
    segment_max_chars: int
    deepseek_api_key: str
    deepseek_base_url: str
    deepseek_model: str
    qwen_api_key: str
    qwen_base_url: str
    qwen_model: str
    glm_api_key: str
    glm_base_url: str
    glm_model: str
    sherpa_tts_model: Path | None
    sherpa_tts_tokens: Path | None
    sherpa_tts_lexicon: Path | None
    sherpa_tts_data_dir: Path | None
    sherpa_english_tts_model: Path | None
    sherpa_english_tts_tokens: Path | None
    sherpa_english_tts_data_dir: Path | None
    sherpa_tts_speaker_id: int
    sherpa_tts_speed: float
    sensevoice_model: Path | None
    sensevoice_tokens: Path | None
    voice_api_enabled: bool
    translation_api_enabled: bool
    admin_password_hash: str
    config_encryption_key: str
    admin_cookie_secure: bool

    @classmethod
    def from_env(cls) -> "Settings":
        root = Path(__file__).resolve().parent
        data_dir = Path(os.getenv("VOICE_DATA_DIR", root / "data")).resolve()

        def optional_path(name: str) -> Path | None:
            value = os.getenv(name, "").strip()
            return Path(value).resolve() if value else None

        return cls(
            data_dir=data_dir,
            database_path=Path(os.getenv("VOICE_DATABASE_PATH", data_dir / "voice.db")).resolve(),
            auth_pepper=os.getenv("VOICE_AUTH_PEPPER", "dev-only-change-me"),
            allow_simulated_input=_bool("VOICE_ALLOW_SIMULATED_INPUT"),
            max_audio_bytes=_int("VOICE_MAX_AUDIO_BYTES", 16_000 * 2 * 25),
            request_limit_per_minute=_int("VOICE_REQUEST_LIMIT_PER_MINUTE", 12),
            worker_concurrency=_int("VOICE_WORKER_CONCURRENCY", 1),
            llm_provider_order=tuple(x.strip() for x in os.getenv("VOICE_LLM_ORDER", "deepseek,qwen,glm").split(",") if x.strip()),
            tts_provider=os.getenv("VOICE_TTS_PROVIDER", "sherpa_vits").strip(),
            asr_provider=os.getenv("VOICE_ASR_PROVIDER", "sensevoice").strip(),
            system_prompt=os.getenv("VOICE_SYSTEM_PROMPT", "你是一个简洁、友好的中文语音助手。回答适合直接朗读。"),
            segment_min_chars=_int("VOICE_SEGMENT_MIN_CHARS", 8),
            segment_max_chars=_int("VOICE_SEGMENT_MAX_CHARS", 40),
            deepseek_api_key=os.getenv("DEEPSEEK_API_KEY", ""),
            deepseek_base_url=os.getenv("DEEPSEEK_BASE_URL", "https://api.deepseek.com"),
            deepseek_model=os.getenv("DEEPSEEK_MODEL", "deepseek-chat"),
            qwen_api_key=os.getenv("QWEN_API_KEY", ""),
            qwen_base_url=os.getenv("QWEN_BASE_URL", "https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation"),
            qwen_model=os.getenv("QWEN_MODEL", "qwen-plus"),
            glm_api_key=os.getenv("GLM_API_KEY", ""),
            glm_base_url=os.getenv("GLM_BASE_URL", "https://open.bigmodel.cn/api/paas/v4"),
            glm_model=os.getenv("GLM_MODEL", "glm-4.7-flash"),
            sherpa_tts_model=optional_path("SHERPA_TTS_MODEL"),
            sherpa_tts_tokens=optional_path("SHERPA_TTS_TOKENS"),
            sherpa_tts_lexicon=optional_path("SHERPA_TTS_LEXICON"),
            sherpa_tts_data_dir=optional_path("SHERPA_TTS_DATA_DIR"),
            sherpa_english_tts_model=optional_path("SHERPA_ENGLISH_TTS_MODEL"),
            sherpa_english_tts_tokens=optional_path("SHERPA_ENGLISH_TTS_TOKENS"),
            sherpa_english_tts_data_dir=optional_path("SHERPA_ENGLISH_TTS_DATA_DIR"),
            sherpa_tts_speaker_id=_int("SHERPA_TTS_SPEAKER_ID", 0),
            sherpa_tts_speed=float(os.getenv("SHERPA_TTS_SPEED", "1.0")),
            sensevoice_model=optional_path("SENSEVOICE_MODEL"),
            sensevoice_tokens=optional_path("SENSEVOICE_TOKENS"),
            voice_api_enabled=_bool("VOICE_API_ENABLED", True),
            translation_api_enabled=_bool("VOICE_TRANSLATION_API_ENABLED", True),
            admin_password_hash=os.getenv("VOICE_ADMIN_PASSWORD_HASH", ""),
            config_encryption_key=os.getenv("VOICE_CONFIG_KEY", ""),
            admin_cookie_secure=_bool("VOICE_ADMIN_COOKIE_SECURE", False),
        )

    @classmethod
    def for_test(cls, directory: Path, **overrides: object) -> "Settings":
        base = cls.from_env()
        values = {**base.__dict__, "data_dir": directory, "database_path": directory / "voice.db", "auth_pepper": "test-pepper", "allow_simulated_input": True, "llm_provider_order": ("mock",), "tts_provider": "mock", "asr_provider": "mock", "request_limit_per_minute": 100}
        values.update(overrides)
        return cls(**values)
