from __future__ import annotations

import re
import base64
import threading
import time
from collections import defaultdict, deque
from pathlib import Path

from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request, status
from fastapi.responses import FileResponse, StreamingResponse
from pydantic import BaseModel, Field
from starlette.requests import ClientDisconnect

from .admin import AdminSessions, ConfigVault, effective_settings, install_admin_routes
from .config import Settings
from .service import VoiceService
from .store import Store

DEVICE_ID_PATTERN = re.compile(r"^[A-Za-z0-9_.:-]{3,64}$")


class TranslateRequest(BaseModel):
    text: str = Field(min_length=1, max_length=8000)
    target_language: str = Field(default="简体中文", min_length=2, max_length=40)


class TextDialogueRequest(BaseModel):
    text: str = Field(min_length=1, max_length=8000)


class DeviceVoiceSettings(BaseModel):
    chinese_speaker_id: int = Field(default=0, ge=0, le=4)
    english_speaker_id: int = Field(default=0, ge=0, le=903)
    speed: float = Field(default=1.0, ge=0.5, le=2.0)


class RateLimiter:
    def __init__(self, limit: int):
        self.limit = limit
        self.events: dict[str, deque[float]] = defaultdict(deque)
        self.lock = threading.Lock()

    def check(self, key: str) -> bool:
        now = time.monotonic()
        with self.lock:
            events = self.events[key]
            while events and now - events[0] > 60:
                events.popleft()
            if len(events) >= self.limit:
                return False
            events.append(now)
            return True


def create_app(settings: Settings | None = None, service: VoiceService | None = None) -> FastAPI:
    base_settings = settings or Settings.from_env()
    base_settings.data_dir.mkdir(parents=True, exist_ok=True)
    vault = ConfigVault(base_settings.data_dir / "admin_config.enc", base_settings.config_encryption_key) if base_settings.config_encryption_key else None
    runtime_config = vault.load() if vault else {}
    settings = effective_settings(base_settings, runtime_config)
    store = service.store if service else Store(settings.database_path, settings.auth_pepper)
    service = service or VoiceService(settings, store)
    limiter = RateLimiter(settings.request_limit_per_minute)
    app = FastAPI(title="ESP32 Cloud Voice Gateway", version="0.1.0", docs_url=None, redoc_url=None)
    app.state.settings = base_settings
    app.state.effective_settings = settings
    app.state.runtime_config = runtime_config
    app.state.store = store
    app.state.voice_service = service

    def current_service() -> VoiceService:
        return app.state.voice_service

    def current_settings() -> Settings:
        return app.state.effective_settings

    def authenticated_device(
        authorization: str = Header(default=""),
        x_device_id: str = Header(default="", alias="X-Device-ID"),
    ) -> str:
        if not DEVICE_ID_PATTERN.fullmatch(x_device_id):
            raise HTTPException(status.HTTP_401_UNAUTHORIZED, "设备标识无效")
        scheme, _, token = authorization.partition(" ")
        if scheme.lower() != "bearer" or not token or not store.authenticate(x_device_id, token):
            raise HTTPException(status.HTTP_401_UNAUTHORIZED, "设备凭证无效")
        return x_device_id

    def owned_job(job_id: str, device_id: str):
        job = store.get_job(job_id, device_id)
        if not job:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "任务不存在")
        return job

    @app.get("/voice-api/v1/health")
    def health() -> dict:
        active_service, active_settings = current_service(), current_settings()
        return {"ok": active_service.ready, "service": "esp32-cloud-voice", "version": "0.1.0", "voice_api_enabled": active_settings.voice_api_enabled, "translation_api_enabled": active_settings.translation_api_enabled, "asr": active_settings.asr_provider, "tts": active_settings.tts_provider, "llm_order": list(active_settings.llm_provider_order), "error": active_service.status_error or None}

    @app.get("/voice-api/v1/device-settings")
    def get_device_settings(device_id: str = Depends(authenticated_device)) -> dict:
        device = store.get_device(device_id)
        if not device:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "设备不存在")
        return {
            "device_id": device_id,
            "chinese_speaker_id": device["tts_speaker_id"],
            "english_speaker_id": device["tts_english_speaker_id"],
            "speed": device["tts_speed"],
        }

    @app.put("/voice-api/v1/device-settings")
    def update_device_settings(
        payload: DeviceVoiceSettings, device_id: str = Depends(authenticated_device)
    ) -> dict:
        store.update_device_voice(
            device_id, payload.chinese_speaker_id, payload.english_speaker_id, payload.speed
        )
        return get_device_settings(device_id)

    @app.post("/voice-api/v1/dialogue", status_code=202)
    async def dialogue(
        request: Request,
        device_id: str = Depends(authenticated_device),
        x_simulated_text_b64: str = Header(default="", alias="X-Simulated-Text-B64"),
    ) -> dict:
        active_service, active_settings = current_service(), current_settings()
        if not active_settings.voice_api_enabled:
            raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, "语音 API 已由管理员停用")
        if not active_service.ready:
            raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, active_service.status_error or "服务未就绪")
        if not limiter.check(device_id):
            raise HTTPException(status.HTTP_429_TOO_MANY_REQUESTS, "请求过于频繁")
        if x_simulated_text_b64 and not active_settings.allow_simulated_input:
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "当前环境禁止模拟文本")
        simulated_text = ""
        if x_simulated_text_b64:
            try:
                simulated_text = base64.urlsafe_b64decode(x_simulated_text_b64.encode("ascii")).decode("utf-8")
            except (ValueError, UnicodeError):
                raise HTTPException(status.HTTP_400_BAD_REQUEST, "模拟文本编码无效")
        try:
            pcm = await request.body()
        except ClientDisconnect as exc:
            raise HTTPException(status.HTTP_400_BAD_REQUEST, "设备在录音上传完成前断开连接") from exc
        if not pcm or len(pcm) > active_settings.max_audio_bytes or len(pcm) % 2:
            raise HTTPException(413, "PCM 必须为非空 16kHz/16-bit/mono，且不超过配置上限")
        job_id = active_service.submit(device_id, pcm, simulated_text)
        return {"job_id": job_id, "status": "queued", "poll_url": f"/voice-api/v1/jobs/{job_id}/segments?after=-1"}

    @app.post("/voice-api/v1/text-dialogue", status_code=202)
    def text_dialogue(payload: TextDialogueRequest, device_id: str = Depends(authenticated_device)) -> dict:
        active_service, active_settings = current_service(), current_settings()
        if not active_settings.voice_api_enabled:
            raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, "对话 API 已由管理员停用")
        if not active_service.ready:
            raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, active_service.status_error or "服务未就绪")
        if not limiter.check(device_id):
            raise HTTPException(status.HTTP_429_TOO_MANY_REQUESTS, "请求过于频繁")
        text = payload.text.strip()
        if not text:
            raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "文字内容不能为空")
        job_id = active_service.submit_text(device_id, text)
        return {"job_id": job_id, "status": "queued", "poll_url": f"/voice-api/v1/jobs/{job_id}/segments?after=-1"}

    @app.get("/voice-api/v1/jobs/{job_id}")
    def job_status(job_id: str, device_id: str = Depends(authenticated_device)) -> dict:
        job = owned_job(job_id, device_id)
        # Keep the database field names while also exposing the stable client
        # aliases consumed by the ESP32 and the Windows LAN implementation.
        job["recognized_text"] = job["question"]
        job["answer_text"] = job["answer"]
        job["segment_count"] = len(store.list_segments(job_id))
        return job

    @app.get("/voice-api/v1/jobs/{job_id}/segments")
    def job_segments(job_id: str, after: int = Query(default=-1, ge=-1), device_id: str = Depends(authenticated_device)) -> dict:
        job = owned_job(job_id, device_id)
        items = []
        for segment in store.list_segments(job_id, after):
            index = segment["segment_index"]
            items.append({"index": index, "text": segment["text"], "byte_count": segment["byte_count"], "sample_rate": segment["sample_rate"], "audio_url": f"/voice-api/v1/jobs/{job_id}/segments/{index}/audio"})
        return {"job_id": job_id, "status": job["status"], "segments": items, "error": job["error"] or None}

    @app.get("/voice-api/v1/jobs/{job_id}/segments/{index}/audio")
    def segment_audio(job_id: str, index: int, device_id: str = Depends(authenticated_device)):
        owned_job(job_id, device_id)
        segment = store.get_segment(job_id, index)
        if not segment:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "音频分段不存在")
        path = Path(segment["audio_path"])
        if not path.is_file():
            raise HTTPException(status.HTTP_410_GONE, "音频分段已清理")
        return FileResponse(path, media_type="audio/L16", headers={"X-Audio-Sample-Rate": str(segment["sample_rate"]), "Cache-Control": "private, no-store"})

    @app.get("/voice-api/v1/jobs/{job_id}/audio")
    def full_audio(job_id: str, device_id: str = Depends(authenticated_device)):
        job = owned_job(job_id, device_id)
        if job["status"] != "completed":
            raise HTTPException(status.HTTP_409_CONFLICT, "任务尚未完成")
        segments = store.list_segments(job_id)

        def body():
            for segment in segments:
                yield Path(segment["audio_path"]).read_bytes()

        total = sum(item["byte_count"] for item in segments)
        return StreamingResponse(body(), media_type="audio/L16", headers={"Content-Length": str(total), "X-Audio-Sample-Rate": str(segments[0]["sample_rate"]), "Cache-Control": "private, no-store"})

    @app.post("/voice-api/v1/translate")
    def translate(payload: TranslateRequest, device_id: str = Depends(authenticated_device)) -> dict:
        active_service, active_settings = current_service(), current_settings()
        if not active_settings.translation_api_enabled:
            raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, "翻译 API 已由管理员停用")
        if not active_service.ready:
            raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, active_service.status_error or "服务未就绪")
        if not limiter.check(device_id):
            raise HTTPException(status.HTTP_429_TOO_MANY_REQUESTS, "请求过于频繁")
        try:
            translated, provider = active_service.translate(payload.text.strip(), payload.target_language.strip())
        except RuntimeError as exc:
            raise HTTPException(status.HTTP_502_BAD_GATEWAY, str(exc)) from exc
        return {"translation": translated, "provider": provider, "target_language": payload.target_language}

    install_admin_routes(app, base_settings, vault, AdminSessions())
    return app


app = create_app()
