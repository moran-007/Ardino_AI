from __future__ import annotations

import re
import base64
import threading
import time
from collections import defaultdict, deque
from pathlib import Path
from typing import Literal
from urllib.parse import unquote_to_bytes

from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request, status
from fastapi.responses import FileResponse, JSONResponse, Response, StreamingResponse
from pydantic import BaseModel, Field
from starlette.requests import ClientDisconnect

from .admin import AdminSessions, ConfigVault, effective_settings, install_admin_routes
from .alarm_music import AlarmMusicError, AlarmMusicRepository, MAX_DEVICE_UPLOAD_BYTES
from .config import Settings
from .firmware import FirmwareRepository
from .personal_profile import PersonalProfileConflict, PersonalProfileSnapshot
from .service import VoiceService
from .store import Store
from .usage_report import compact_device_usage, usage_report

DEVICE_ID_PATTERN = re.compile(r"^[A-Za-z0-9_.:-]{3,64}$")
DEVICE_CONTEXT_HEADER_MAX_BYTES = 512


def decode_device_context_header(value: str, label: str) -> str:
    if not value:
        return ""
    if len(value) > DEVICE_CONTEXT_HEADER_MAX_BYTES * 3:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, f"设备{label}上下文过长")
    try:
        raw = unquote_to_bytes(value)
        if len(raw) > DEVICE_CONTEXT_HEADER_MAX_BYTES:
            raise ValueError("decoded weather context is too long")
        decoded = raw.decode("utf-8")
    except (ValueError, UnicodeError):
        raise HTTPException(status.HTTP_400_BAD_REQUEST, f"设备{label}上下文编码无效")
    return re.sub(r"[\x00-\x1f\x7f]+", " ", decoded).strip()


class TranslateRequest(BaseModel):
    text: str = Field(min_length=1, max_length=8000)
    target_language: str = Field(default="简体中文", min_length=2, max_length=40)


class AlarmMusicSelection(BaseModel):
    id: str = Field(min_length=1, max_length=48, pattern=r'^[a-z0-9_-]+$')


class TextDialogueRequest(BaseModel):
    text: str = Field(min_length=1, max_length=8000)


class DeviceVoiceSettings(BaseModel):
    chinese_speaker_id: int = Field(default=0, ge=0, le=4)
    english_speaker_id: int = Field(default=0, ge=0, le=903)
    speed: float = Field(default=1.0, ge=0.5, le=2.0)


class DialogueSettings(BaseModel):
    reply_style: Literal['default', 'brief', 'normal', 'detailed'] = 'default'
    persona: str = Field(default='', max_length=800)
    memory_enabled: bool = False
    web_search_enabled: bool = False
    memory_turns: int = Field(default=4, ge=1, le=10)


class ClearMemoryRequest(BaseModel):
    confirm: Literal[True]


class LocalResultRequest(BaseModel):
    handled: bool
    message: str = Field(default='', max_length=600)
    personal_profile: PersonalProfileSnapshot | None = None


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
    app.state.alarm_music_repository = AlarmMusicRepository(base_settings.data_dir / "alarm-music")
    app.state.voice_service = service
    app.state.firmware_repository = FirmwareRepository(
        base_settings.data_dir / "firmware",
        base_settings.firmware_public_key_path,
        base_settings.firmware_public_base_url,
        base_settings.firmware_hardware_id,
    )
    app.state.multi_firmware_repository = FirmwareRepository(
        base_settings.data_dir / "firmware-multimic",
        base_settings.firmware_public_key_path,
        base_settings.firmware_public_base_url,
        "esp32s3-cloud-voice-multimic-v1",
    )

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
        return {"ok": active_service.ready, "service": "esp32-cloud-voice", "version": "0.1.0", "voice_api_enabled": active_settings.voice_api_enabled, "translation_api_enabled": active_settings.translation_api_enabled, "asr": active_settings.asr_provider, "tts": active_settings.tts_provider, "llm_order": list(active_settings.llm_provider_order), "llm_ready": not active_service.llm_status_error, "llm_error": active_service.llm_status_error or None, "asr_route": active_settings.asr_route, "tts_route": active_settings.tts_route, "error": active_service.status_error or None}

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

    @app.get('/voice-api/v1/alarm-music')
    def alarm_music_catalog(device_id: str = Depends(authenticated_device)):
        return JSONResponse({'tracks': app.state.alarm_music_repository.catalog(device_id),
                             'selected': app.state.alarm_music_repository.selection(device_id)}, headers={'Cache-Control': 'no-store'})

    # Literal selection route precedes any route accepting a music identifier.
    @app.get('/voice-api/v1/alarm-music/selection')
    def alarm_music_selection(device_id: str = Depends(authenticated_device)):
        return JSONResponse({'id': app.state.alarm_music_repository.selection(device_id)}, headers={'Cache-Control': 'no-store'})

    @app.put('/voice-api/v1/alarm-music/selection')
    def set_alarm_music_selection(selection: AlarmMusicSelection, device_id: str = Depends(authenticated_device)):
        try:
            selected = app.state.alarm_music_repository.select(device_id, selection.id)
        except AlarmMusicError as exc:
            raise HTTPException(404, str(exc)) from exc
        return {'id': selected}

    @app.post('/voice-api/v1/alarm-music-upload', status_code=201)
    async def device_alarm_music_upload(request: Request, title: str = Query(min_length=1, max_length=60),
                                      device_id: str = Depends(authenticated_device)):
        payload = bytearray()
        async for chunk in request.stream():
            if len(payload) + len(chunk) > MAX_DEVICE_UPLOAD_BYTES:
                raise HTTPException(413, '设备音乐上传不能超过 512 KB')
            payload.extend(chunk)
        try:
            track_id = app.state.alarm_music_repository.upload(bytes(payload), title, device_id)
            track = next(item for item in app.state.alarm_music_repository.catalog(device_id) if item['id'] == track_id)
        except AlarmMusicError as exc:
            raise HTTPException(422, str(exc)) from exc
        return track

    @app.get('/voice-api/v1/alarm-music/{track_id}/pcm')
    def alarm_music_pcm(track_id: str, device_id: str = Depends(authenticated_device)):
        try:
            payload = app.state.alarm_music_repository.pcm(track_id, device_id)
        except AlarmMusicError as exc:
            raise HTTPException(404, str(exc)) from exc
        return Response(payload, media_type='application/octet-stream', headers={
            'Cache-Control': 'private, no-store', 'X-Audio-Sample-Rate': '16000', 'X-Audio-Channels': '1', 'X-Audio-Bits': '16'})

    @app.get('/voice-api/v1/device-usage')
    def device_usage(period: Literal['all', '24h', '7d', '30d'] = '30d',
                     view: Literal['full', 'compact'] = 'full', device_id: str = Depends(authenticated_device)):
        report = usage_report(store, current_settings(), period, device_id)
        if view == 'compact':
            report = compact_device_usage(report)
        report['omitted_rows'] = report.get('omitted_rows', 0) + max(0, len(report['by_api']) - 40)
        report['by_api'] = report['by_api'][:40]
        return JSONResponse(report, headers={'Cache-Control': 'no-store'})

    @app.get('/voice-api/v1/dialogue-settings')
    def dialogue_settings(device_id: str = Depends(authenticated_device)):
        device = store.get_device(device_id)
        settings = current_settings()
        return {'reply_style': device['reply_style'], 'default_reply_style': settings.reply_style,
                'persona': device['persona'], 'memory_enabled': bool(device['memory_enabled']),
                'memory_turns': device['memory_turns'], 'system_prompt': settings.system_prompt,
                'web_search_enabled': bool(device['web_search_enabled']),
                'web_search_available': any(getattr(p, 'supports_search', False) for p in current_service().llms),
                'segment_min_chars': settings.segment_min_chars, 'segment_max_chars': settings.segment_max_chars,
                'total_answer_limit': None,
                'policies': ['回答详略按设备或服务器选择；明确要求详细时按用户要求展开。',
                             '设备闹钟、计时器、便签操作用简短确认；不能编造执行结果。',
                             '本地天气使用设备数据；跨城市实时信息需启用可用模型的联网搜索。',
                             '明确的本地天气、便签待办查询、北京时间日期和简单算术直接按数据或规则回答，不调用语言模型；复杂或不匹配问题才调用模型。',
                             '过期、缺失或跨城市天气需要开启搜索才能获取；本地规则回复不产生模型 Token 用量。',
                             'DeepSeek、千问关闭思考，GLM 使用厂商默认；未指定模型总输出 Token 上限。',
                             'DeepSeek、GLM 温度参数为 0.6，千问使用厂商默认；播报分段不限制总回答长度。',
                             '开启记忆后最多携带所选最近 10 轮，总上下文最多 1600 个字符。']}

    @app.put('/voice-api/v1/dialogue-settings')
    def save_dialogue_settings(payload: DialogueSettings, device_id: str = Depends(authenticated_device)):
        store.update_dialogue_profile(device_id, payload.reply_style, payload.persona,
                                      payload.memory_enabled, payload.memory_turns, payload.web_search_enabled)
        return dialogue_settings(device_id)

    @app.get('/voice-api/v1/personal-profile')
    def personal_profile(device_id: str = Depends(authenticated_device)):
        return JSONResponse(store.get_personal_profile(device_id), headers={'Cache-Control': 'private, no-store'})

    @app.put('/voice-api/v1/personal-profile')
    def save_personal_profile(payload: PersonalProfileSnapshot, device_id: str = Depends(authenticated_device)):
        try:
            backup = store.save_personal_profile(device_id, payload.model_dump(by_alias=True))
        except PersonalProfileConflict as exc:
            raise HTTPException(409, str(exc)) from None
        return JSONResponse(backup, headers={'Cache-Control': 'private, no-store'})

    @app.post('/voice-api/v1/memory/clear')
    def clear_device_memory(payload: ClearMemoryRequest, device_id: str = Depends(authenticated_device)):
        try:
            count = current_service().clear_memory(device_id)
        except RuntimeError:
            raise HTTPException(409, '请等待当前语音任务结束后再清除记忆')
        return {'ok': True, 'cleared': count}

    @app.get("/voice-api/v1/firmware/manifest")
    def firmware_manifest(
        _device_id: str = Depends(authenticated_device),
        x_firmware_version: str = Header(default="", alias="X-Firmware-Version"),
        x_hardware_id: str = Header(default="", alias="X-Hardware-ID"),
    ):
        repository = app.state.multi_firmware_repository if x_hardware_id == "esp32s3-cloud-voice-multimic-v1" else app.state.firmware_repository
        manifest = repository.manifest_for(
            x_firmware_version, x_hardware_id
        )
        if manifest is None:
            return Response(status_code=status.HTTP_204_NO_CONTENT)
        return JSONResponse(manifest, headers={"Cache-Control": "private, no-store"})

    @app.get("/voice-api/v1/firmware/download/{version}")
    def firmware_download(version: str, _device_id: str = Depends(authenticated_device),
                          x_hardware_id: str = Header(default="", alias="X-Hardware-ID")):
        repository = app.state.multi_firmware_repository if x_hardware_id == "esp32s3-cloud-voice-multimic-v1" else app.state.firmware_repository
        path = repository.firmware_path(version)
        if path is None:
            raise HTTPException(status.HTTP_404_NOT_FOUND, "固件版本不存在或未发布")
        return FileResponse(
            path,
            media_type="application/octet-stream",
            filename=f"esp32-firmware-{version}.bin",
            headers={"Cache-Control": "private, no-store"},
        )

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
        x_device_weather: str = Header(default="", alias="X-Device-Weather"),
        x_device_memos: str = Header(default="", alias="X-Device-Memos"),
        x_local_control: str = Header(default="", alias="X-Device-Local-Control"),
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
        weather_context = decode_device_context_header(x_device_weather, "天气")
        memo_context = decode_device_context_header(x_device_memos, "便签")
        job_id = active_service.submit(
            device_id, pcm, simulated_text, weather_context, memo_context, x_local_control == '1'
        )
        return {
            "job_id": job_id,
            "status": "queued",
            "poll_url": f"/voice-api/v1/jobs/{job_id}/segments?after=-1",
            # The device uses this acknowledgement to distinguish "weather is
            # cached locally" from "the cloud actually accepted that cache".
            # Older servers omit this object, which the firmware reports as an
            # actionable version mismatch instead of a false success.
            "context": {
                "schema": 1,
                "weather": bool(weather_context),
                "memos": bool(memo_context),
                **({'local_control': True} if x_local_control == '1' else {}),
            },
        }

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

    @app.post('/voice-api/v1/jobs/{job_id}/local-result')
    def local_result(job_id: str, payload: LocalResultRequest, device_id: str = Depends(authenticated_device)):
        owned_job(job_id, device_id)
        if payload.handled and not payload.message.strip():
            raise HTTPException(422, '执行结果不能为空')
        try:
            current_service().report_local_result(
                job_id, device_id, payload.handled, payload.message,
                payload.personal_profile.model_dump(by_alias=True) if payload.personal_profile is not None else None,
            )
        except RuntimeError as exc:
            raise HTTPException(409, str(exc)) from None
        return {'accepted': True, **({'profile_revision': payload.personal_profile.revision}
                                    if payload.personal_profile is not None else {})}

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
        if active_service.llm_status_error:
            raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, active_service.llm_status_error)
        if not active_service.ready:
            raise HTTPException(status.HTTP_503_SERVICE_UNAVAILABLE, active_service.status_error or "服务未就绪")
        if not limiter.check(device_id):
            raise HTTPException(status.HTTP_429_TOO_MANY_REQUESTS, "请求过于频繁")
        try:
            translated, provider = active_service.translate(payload.text.strip(), payload.target_language.strip(), device_id=device_id)
        except RuntimeError as exc:
            raise HTTPException(status.HTTP_502_BAD_GATEWAY, str(exc)) from exc
        return {"translation": translated, "provider": provider, "target_language": payload.target_language}

    install_admin_routes(app, base_settings, vault, AdminSessions())
    return app


app = create_app()
