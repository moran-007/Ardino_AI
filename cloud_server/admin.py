from __future__ import annotations

import base64
import hashlib
import hmac
import html
import json
import os
import gc
import io
import wave
import re
import secrets
import threading
import time
from dataclasses import replace
from datetime import datetime, timedelta, timezone
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from cryptography.fernet import Fernet, InvalidToken
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import HTMLResponse, RedirectResponse, Response
from .alarm_music import AlarmMusicError, MAX_UPLOAD_BYTES, MAX_SOURCE_BYTES, alarm_music_content, normalize_admin_music
from starlette.concurrency import run_in_threadpool

from .admin_views import api_keys_content, billing_content, dashboard_content, devices_content, firmware_content, page_shell, settings_content
from .billing import manual_rule, rule_key
from .usage_report import usage_report
from .config import Settings
from .firmware import MAX_PACKAGE_BYTES, FirmwarePackageError
from .model_catalog import PROVIDERS, ModelCatalogError, fetch_models
from .service import VoiceService
from .music_routes import install_music_admin

PASSWORD_ITERATIONS = 310_000
SESSION_COOKIE = "voice_admin_session"
DEVICE_ID_PATTERN = re.compile(r"^[A-Za-z0-9_.:-]{3,64}$")


def hash_admin_password(password: str, salt: bytes | None = None) -> str:
    if len(password) < 10:
        raise ValueError("管理员密码至少需要 10 个字符")
    salt = salt or secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, PASSWORD_ITERATIONS)
    return f"pbkdf2_sha256${PASSWORD_ITERATIONS}${base64.urlsafe_b64encode(salt).decode()}${base64.urlsafe_b64encode(digest).decode()}"


def verify_admin_password(password: str, encoded: str) -> bool:
    try:
        algorithm, iterations, salt, expected = encoded.split("$", 3)
        if algorithm != "pbkdf2_sha256":
            return False
        actual = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), base64.urlsafe_b64decode(salt), int(iterations))
        return hmac.compare_digest(actual, base64.urlsafe_b64decode(expected))
    except (ValueError, TypeError):
        return False


class ConfigVault:
    def __init__(self, path: Path, key: str):
        self.path = path
        self.fernet = Fernet(key.encode("ascii"))
        self.lock = threading.Lock()

    def load(self) -> dict:
        if not self.path.is_file():
            return {}
        try:
            return json.loads(self.fernet.decrypt(self.path.read_bytes()).decode("utf-8"))
        except (InvalidToken, ValueError, OSError, json.JSONDecodeError) as exc:
            raise RuntimeError("管理员配置文件无法解密或已损坏") from exc

    def save(self, config: dict) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        encrypted = self.fernet.encrypt(json.dumps(config, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
        temporary = self.path.with_suffix(".tmp")
        with self.lock:
            temporary.write_bytes(encrypted)
            os.chmod(temporary, 0o600)
            os.replace(temporary, self.path)


def effective_settings(base: Settings, config: dict) -> Settings:
    if "llm_provider_order" in config:
        enabled = list(config["llm_provider_order"])
    elif any(f"{name}_enabled" in config for name in ("deepseek", "qwen", "glm")):
        enabled = [name for name in ("deepseek", "qwen", "glm") if config.get(f"{name}_enabled", False)]
    else:
        enabled = list(base.llm_provider_order)

    def path_value(name: str, fallback: Path | None) -> Path | None:
        value = str(config.get(name, "")).strip()
        return Path(value) if value else fallback

    return replace(
        base,
        voice_api_enabled=bool(config.get("voice_api_enabled", base.voice_api_enabled)),
        translation_api_enabled=bool(config.get("translation_api_enabled", base.translation_api_enabled)),
        llm_provider_order=tuple(enabled),
        deepseek_api_key=str(config.get("deepseek_api_key", base.deepseek_api_key)),
        deepseek_base_url=str(config.get("deepseek_base_url", base.deepseek_base_url)),
        deepseek_model=str(config.get("deepseek_model", base.deepseek_model)),
        qwen_api_key=str(config.get("qwen_api_key", base.qwen_api_key)),
        qwen_base_url=str(config.get("qwen_base_url", base.qwen_base_url)),
        qwen_model=str(config.get("qwen_model", base.qwen_model)),
        glm_api_key=str(config.get("glm_api_key", base.glm_api_key)),
        glm_base_url=str(config.get("glm_base_url", base.glm_base_url)),
        glm_model=str(config.get("glm_model", base.glm_model)),
        asr_provider=str(config.get("asr_provider", base.asr_provider)),
        sensevoice_model=path_value("sensevoice_model", base.sensevoice_model),
        sensevoice_tokens=path_value("sensevoice_tokens", base.sensevoice_tokens),
        tts_provider=str(config.get("tts_provider", base.tts_provider)),
        sherpa_tts_model=path_value("sherpa_tts_model", base.sherpa_tts_model),
        sherpa_tts_tokens=path_value("sherpa_tts_tokens", base.sherpa_tts_tokens),
        sherpa_tts_lexicon=path_value("sherpa_tts_lexicon", base.sherpa_tts_lexicon),
        sherpa_tts_data_dir=path_value("sherpa_tts_data_dir", base.sherpa_tts_data_dir),
        sherpa_english_tts_model=path_value("sherpa_english_tts_model", base.sherpa_english_tts_model),
        sherpa_english_tts_tokens=path_value("sherpa_english_tts_tokens", base.sherpa_english_tts_tokens),
        sherpa_english_tts_data_dir=path_value("sherpa_english_tts_data_dir", base.sherpa_english_tts_data_dir),
        sherpa_tts_speaker_id=int(config.get("sherpa_tts_speaker_id", base.sherpa_tts_speaker_id)),
        sherpa_tts_speed=float(config.get("sherpa_tts_speed", base.sherpa_tts_speed)),
        segment_min_chars=int(config.get("segment_min_chars", base.segment_min_chars)),
        segment_max_chars=int(config.get("segment_max_chars", base.segment_max_chars)),
        system_prompt=str(config.get('system_prompt', base.system_prompt)),
        reply_style=str(config.get('reply_style', base.reply_style)),
        billing_enabled=bool(config.get('billing_enabled', base.billing_enabled)),
        billing_rules=json.dumps(config.get('billing_rules', json.loads(base.billing_rules))),
        **{f'{provider}_{suffix}': str(config.get(f'{provider}_{suffix}', getattr(base, f'{provider}_{suffix}')))
           for provider in ('custom', 'local_llm') for suffix in ('api_key', 'base_url', 'model', 'protocol')},
        **{name: str(config.get(name, getattr(base, name))) for name in (
            'asr_route', 'tts_route', 'cloud_speech_api_key', 'cloud_speech_url', 'cloud_asr_model',
            'cloud_tts_model', 'cloud_asr_unit_price', 'cloud_tts_unit_price')},
    )


class AdminSessions:
    def __init__(self):
        self.sessions: dict[str, tuple[float, str]] = {}
        self.attempts: dict[str, list[float]] = {}
        self.lock = threading.Lock()

    def allow_attempt(self, address: str) -> bool:
        now = time.monotonic()
        with self.lock:
            recent = [item for item in self.attempts.get(address, []) if now - item < 300]
            if len(recent) >= 8:
                self.attempts[address] = recent
                return False
            recent.append(now)
            self.attempts[address] = recent
            return True

    def create(self) -> tuple[str, str]:
        token, csrf = secrets.token_urlsafe(32), secrets.token_urlsafe(24)
        with self.lock:
            self.sessions[token] = (time.time() + 8 * 3600, csrf)
        return token, csrf

    def csrf(self, token: str) -> str | None:
        with self.lock:
            value = self.sessions.get(token)
            if not value or value[0] < time.time():
                self.sessions.pop(token, None)
                return None
            return value[1]

    def delete(self, token: str) -> None:
        with self.lock:
            self.sessions.pop(token, None)


def response_headers(response):
    response.headers["Cache-Control"] = "no-store"
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["X-Frame-Options"] = "DENY"
    policy = "default-src 'none'; style-src 'unsafe-inline'; media-src 'self'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'"
    if isinstance(response, HTMLResponse) and re.search(rb'<script\b', response.body, re.I):
        nonce = secrets.token_urlsafe(24)
        response.body = re.sub(rb'<script\b', b'<script nonce="' + nonce.encode('ascii') + b'"', response.body, flags=re.I)
        response.headers['Content-Length'] = str(len(response.body))
        policy += f"; script-src 'nonce-{nonce}'; connect-src 'self'"
    response.headers["Content-Security-Policy"] = policy
    return response


async def form_data(request: Request) -> dict[str, str]:
    raw = (await request.body()).decode("utf-8", errors="strict")
    return {key: values[-1] for key, values in parse_qs(raw, keep_blank_values=True).items()}


def install_admin_routes(app: FastAPI, base: Settings, vault: ConfigVault | None, sessions: AdminSessions) -> None:
    def require_session(request: Request) -> tuple[str, str]:
        token = request.cookies.get(SESSION_COOKIE, "")
        csrf = sessions.csrf(token)
        if not csrf:
            raise HTTPException(401, "管理员密码未验证")
        return token, csrf

    install_music_admin(app, require_session, response_headers)

    def unlock_page(error: str = "") -> HTMLResponse:
        notice = f'<p class="bad">{html.escape(error)}</p>' if error else ""
        content = f"<h1>ESP32 语音服务</h1><p class=\"muted\">无需用户名，只输入管理员密码。</p>{notice}<form method=\"post\" action=\"/admin/unlock\"><fieldset><label>管理员密码</label><input type=\"password\" name=\"password\" autocomplete=\"current-password\" required autofocus><p><button type=\"submit\">解锁配置</button></p></fieldset></form>"
        return response_headers(HTMLResponse(page_shell(content)))

    def activate_config(updated: dict) -> None:
        settings = effective_settings(base, updated)
        old_service = app.state.voice_service
        asr_unchanged = (
            old_service.settings.asr_provider == settings.asr_provider
            and old_service.settings.sensevoice_model == settings.sensevoice_model
            and old_service.settings.sensevoice_tokens == settings.sensevoice_tokens
        )
        tts_unchanged = (
            old_service.settings.tts_provider == settings.tts_provider
            and old_service.settings.sherpa_tts_model == settings.sherpa_tts_model
            and old_service.settings.sherpa_tts_tokens == settings.sherpa_tts_tokens
            and old_service.settings.sherpa_tts_lexicon == settings.sherpa_tts_lexicon
            and old_service.settings.sherpa_tts_data_dir == settings.sherpa_tts_data_dir
            and old_service.settings.sherpa_english_tts_model == settings.sherpa_english_tts_model
            and old_service.settings.sherpa_english_tts_tokens == settings.sherpa_english_tts_tokens
            and old_service.settings.sherpa_english_tts_data_dir == settings.sherpa_english_tts_data_dir
            and old_service.settings.sherpa_tts_speaker_id == settings.sherpa_tts_speaker_id
            and old_service.settings.sherpa_tts_speed == settings.sherpa_tts_speed
        )
        if old_service.active_jobs:
            raise HTTPException(409, "有语音任务正在运行，请任务结束后再保存服务配置")
        reuse_asr = old_service.asr if asr_unchanged else None
        reuse_tts = old_service.tts if tts_unchanged else None
        if not asr_unchanged:
            old_service.asr = None
        if not tts_unchanged:
            old_service.tts = None
        gc.collect()
        new_service = VoiceService(settings, app.state.store, asr=reuse_asr, tts=reuse_tts)
        vault.save(updated)
        app.state.runtime_config = updated
        app.state.effective_settings = settings
        app.state.voice_service = new_service

    model_catalogs: dict[str, dict] = {}
    speech_test_lock = threading.Lock()
    speech_test_state = {'last_started': None, 'result': None, 'audio': None}

    @app.post('/admin/speech-test')
    async def admin_speech_test(request: Request):
        token, csrf = require_session(request)
        data = await form_data(request)
        if not hmac.compare_digest(data.get('csrf', ''), csrf):
            raise HTTPException(403, 'CSRF 校验失败')
        if data.get('confirm_speech_test') != 'on':
            raise HTTPException(422, '请先确认本次测试可能消耗额度或产生费用')
        operation = data.get('test_operation')
        if operation not in {'asr', 'tts'}:
            raise HTTPException(422, '请选择识别或合成测试')
        from .speech_fallback import connectivity_test, validate_cloud_fields
        current = app.state.effective_settings
        try:
            values = validate_cloud_fields(data, current)
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from None
        if not speech_test_lock.acquire(blocking=False):
            raise HTTPException(409, '已有语音测试正在进行')
        try:
            now = time.monotonic()
            if speech_test_state['last_started'] is not None and now - speech_test_state['last_started'] < 10:
                raise HTTPException(429, '测试间隔至少 10 秒，避免重复消耗额度')
            service = app.state.voice_service
            if service.active_jobs:
                raise HTTPException(409, '设备语音任务正在运行，请结束后再测试')
            speech_test_state['last_started'] = now
            speech_test_state['result'] = speech_test_state['audio'] = None
            result = await run_in_threadpool(connectivity_test, replace(current, **values),
                                            app.state.store, operation, service.tts)
            pcm, rate = result.pop('_audio', None), result.pop('_sample_rate', None)
            sample = secrets.token_urlsafe(24)
            if pcm:
                audio = io.BytesIO()
                with wave.open(audio, 'wb') as output:
                    output.setnchannels(1)
                    output.setsampwidth(2)
                    output.setframerate(rate)
                    output.writeframes(pcm)
                speech_test_state['audio'] = audio.getvalue()
            speech_test_state['result'] = {'owner': token, 'sample': sample, 'expires': time.monotonic() + 300,
                                         'result': result, 'values': {k: v for k, v in values.items() if k != 'cloud_speech_api_key'}}
        finally:
            speech_test_lock.release()
        return response_headers(RedirectResponse('/admin/speech-test/result', status_code=303))

    def latest_speech_test(token):
        entry = speech_test_state['result']
        if not entry or entry['expires'] < time.monotonic():
            speech_test_state['audio'] = speech_test_state['result'] = None
            raise HTTPException(404, '试听结果已过期或已被下一次测试替换')
        if not hmac.compare_digest(entry['owner'], token):
            raise HTTPException(404, '没有当前会话的测试结果')
        return entry

    @app.get('/admin/speech-test/result')
    async def speech_test_result(request: Request):
        token, csrf = require_session(request)
        entry = latest_speech_test(token)
        result = entry['result']
        label = '连通测试成功' if result['success'] else '连通测试未通过'
        content = f'<h1>{label}</h1><p><code>{html.escape(result["model"])}</code> · {result["elapsed"]} 秒</p><p>{html.escape(result["detail"])}</p>'
        if speech_test_state['audio']:
            content += f'<audio controls preload="none" src="/admin/speech-test/audio?sample={entry["sample"]}"></audio>'
        content += '<p class="muted">当前主备方案与已保存配置未修改。下面显示本次测试填写值，只有点击保存才生效。刷新此结果页不会再次调用云端。试听只保留最新一条，5 分钟后不可访问；不覆盖设备录音。</p>'
        current, service = app.state.effective_settings, app.state.voice_service
        content += settings_content(replace(current, **entry['values']), csrf,
                                    app.state.firmware_repository.current(), service.ready, service.status_error)
        return response_headers(HTMLResponse(page_shell(content, '语音连通测试', active='settings', csrf=csrf)))

    @app.get('/admin/speech-test/audio')
    async def speech_test_audio(request: Request):
        token, _ = require_session(request)
        entry = latest_speech_test(token)
        if not hmac.compare_digest(request.query_params.get('sample', ''), entry['sample']) or not speech_test_state['audio']:
            raise HTTPException(404, '试听音频不可用')
        return response_headers(Response(speech_test_state['audio'], media_type='audio/wav'))

    def visible_catalogs() -> dict:
        current = app.state.effective_settings
        config = app.state.runtime_config
        return {
            name: entry for name, entry in model_catalogs.items()
            if entry["key_id"] == app.state.store.api_key_id(
                name, getattr(current, f"{name}_api_key"), getattr(current, f"{name}_base_url")
            ) and entry["models_url"] == config.get(f"{name}_models_url", "")
        }

    def api_page(csrf: str, notice: str = "", error: str = "", status_code: int = 200):
        content = api_keys_content(
            app.state.effective_settings, app.state.runtime_config, csrf,
            visible_catalogs(), notice, error,
        )
        return response_headers(HTMLResponse(
            page_shell(content, "API Key 配置", active="api-keys", csrf=csrf), status_code=status_code
        ))

    def admin_page(request: Request, section: str):
        if not base.admin_password_hash or not vault:
            return response_headers(HTMLResponse(page_shell("<h1>管理员页面尚未在服务器环境中启用</h1>"), status_code=503))
        try:
            _, csrf = require_session(request)
        except HTTPException:
            return unlock_page()
        current, service = app.state.effective_settings, app.state.voice_service
        if section == "api-keys":
            notice = "API Key 与主备配置已保存。" if request.query_params.get("saved") == "1" else ""
            return api_page(csrf, notice)
        if section == "firmware":
            repository = app.state.firmware_repository
            content = firmware_content(repository.list_releases(), repository.current(), csrf)
            content += '<h2>多麦克风固件（主维护版本）</h2>'
            repository = app.state.multi_firmware_repository
            content += firmware_content(repository.list_releases(), repository.current(), csrf, channel='multi').replace(
                'action="/admin/firmware/upload" enctype="multipart/form-data">',
                'action="/admin/firmware/upload" enctype="multipart/form-data"><input type="hidden" name="channel" value="multi">'
            ).replace('id="package"', 'id="multi-package"').replace('for="package"', 'for="multi-package"').replace('id="minimum_version"', 'id="multi-minimum_version"').replace('for="minimum_version"', 'for="multi-minimum_version"').replace('id="release-notes"', 'id="multi-release-notes"').replace('for="release-notes"', 'for="multi-release-notes"')
            return response_headers(HTMLResponse(page_shell(content, "固件版本", active=section, csrf=csrf)))
        if section in {"overview", "devices", "billing"}:
            period = request.query_params.get("period", "all")
            windows = {"24h": 1, "7d": 7, "30d": 30}
            if period not in {"all", *windows}:
                period = "all"
            since = (datetime.now(timezone.utc) - timedelta(days=windows[period])).isoformat() if period in windows else None
            summary = app.state.store.usage_summary(since=since)
            if section == "overview":
                content = dashboard_content(current, app.state.store, summary, app.state.store.recent_usage(since=since), period, service.ready)
                title = "用量总览"
            elif section == 'billing':
                content = billing_content(current, app.state.store.billing_summary(since),
                                          app.state.store.recent_usage(since=since), period, csrf,
                                          app.state.store.recent_speech_usage(since=since),
                                          usage_report(app.state.store, current, period))
                title = '费用统计与计费规则'
                if request.query_params.get('saved') == '1':
                    content = '<p class="notice" role="status">计费规则已保存，仅对新的调用生效。</p>' + content
            else:
                content = devices_content(app.state.store.list_devices(), summary, period, csrf)
                title = "设备 KEY"
        else:
            content = settings_content(current, csrf, app.state.firmware_repository.current(), service.ready, service.status_error)
            title = "服务设置"
            if request.query_params.get("saved") == "1":
                content = '<p class="notice" role="status">服务设置已保存。</p>' + content
        return response_headers(HTMLResponse(page_shell(content, title, active=section, csrf=csrf, refresh=section == "overview")))

    @app.get("/admin/", response_class=HTMLResponse)
    def admin_home(request: Request):
        return admin_page(request, "overview")

    @app.get("/admin/devices", response_class=HTMLResponse)
    def admin_devices(request: Request):
        return admin_page(request, "devices")

    @app.get("/admin/api-keys", response_class=HTMLResponse)
    def admin_api_keys(request: Request):
        return admin_page(request, "api-keys")

    @app.get("/admin/settings", response_class=HTMLResponse)
    def admin_settings(request: Request):
        return admin_page(request, "settings")

    @app.get('/admin/billing', response_class=HTMLResponse)
    def admin_billing(request: Request):
        return admin_page(request, 'billing')

    @app.post('/admin/billing')
    async def save_billing(request: Request):
        _, csrf = require_session(request)
        data = await form_data(request)
        if not hmac.compare_digest(data.get('csrf', ''), csrf):
            raise HTTPException(403, 'CSRF 校验失败')
        updated = dict(app.state.runtime_config)
        if data.get('action') == 'toggle':
            updated['billing_enabled'] = 'billing_enabled' in data
        else:
            rules = json.loads(app.state.effective_settings.billing_rules)
            try:
                key = rule_key(data.get('provider', ''), data.get('model', '').strip())
                if data.get('mode') == 'official':
                    rules.pop(key, None)
                elif data.get('mode') == 'manual':
                    tiers = []
                    for index in range(8):
                        if data.get(f'limit_{index}', '').strip():
                            tiers.append({'max_input_tokens': int(data[f'limit_{index}']),
                                          'input': data.get(f'input_{index}'), 'output': data.get(f'output_{index}'),
                                          'cached_input': data.get(f'cache_{index}')})
                    rules[key] = manual_rule({'currency': data.get('currency'), 'tiers': tiers, 'search': data.get('search')})
                else:
                    raise ValueError('计费模式无效')
                if len(rules) > 100:
                    raise ValueError('最多保存 100 个模型的手动规则')
            except (ValueError, TypeError, AttributeError) as exc:
                raise HTTPException(422, str(exc)) from exc
            updated['billing_rules'] = rules
        activate_config(updated)
        return response_headers(RedirectResponse('/admin/billing?saved=1', status_code=303))

    @app.get("/admin/firmware", response_class=HTMLResponse)
    def admin_firmware(request: Request):
        return admin_page(request, "firmware")

    @app.get('/admin/alarm-music', response_class=HTMLResponse)
    def admin_alarm_music(request: Request):
        _, csrf = require_session(request)
        content = alarm_music_content(app.state.alarm_music_repository.catalog(), csrf)
        return response_headers(HTMLResponse(page_shell(content, '闹钟音乐', active='alarm-music', csrf=csrf)))

    @app.post('/admin/alarm-music/upload')
    async def admin_alarm_music_upload(request: Request):
        _, csrf = require_session(request)
        # Bound the full multipart stream before its parser can spool an arbitrarily large file.
        body = bytearray()
        async for chunk in request.stream():
            if len(body) + len(chunk) > MAX_UPLOAD_BYTES:
                raise HTTPException(413, '音乐上传不能超过 20 MB')
            body.extend(chunk)
        request._body = bytes(body)
        form = await request.form(max_files=1, max_fields=2, max_part_size=MAX_UPLOAD_BYTES)
        try:
            if not hmac.compare_digest(str(form.get('csrf', '')), csrf):
                raise HTTPException(403, 'CSRF 校验失败')
            upload = form.get('music')
            if upload is None or not hasattr(upload, 'read'):
                raise AlarmMusicError('请选择 MP3 或 WAV 音乐文件')
            payload = await upload.read(MAX_SOURCE_BYTES + 1)
            payload = await run_in_threadpool(normalize_admin_music, payload, upload.filename or '')
            app.state.alarm_music_repository.upload(payload, str(form.get('title', '')))
        except AlarmMusicError as exc:
            raise HTTPException(422, str(exc)) from exc
        finally:
            await form.close()
        return RedirectResponse('/admin/alarm-music', status_code=303)

    @app.post('/admin/alarm-music/{track_id}/delete')
    async def admin_alarm_music_delete(request: Request, track_id: str):
        _, csrf = require_session(request)
        data = await form_data(request)
        if not hmac.compare_digest(data.get('csrf', ''), csrf):
            raise HTTPException(403, 'CSRF 校验失败')
        try:
            app.state.alarm_music_repository.delete(track_id)
        except AlarmMusicError as exc:
            raise HTTPException(422, str(exc)) from exc
        return RedirectResponse('/admin/alarm-music', status_code=303)

    @app.get('/admin/alarm-music/{track_id}/wav')
    def admin_alarm_music_preview(request: Request, track_id: str):
        require_session(request)
        try:
            payload = app.state.alarm_music_repository.wav(track_id)
        except AlarmMusicError as exc:
            raise HTTPException(404, str(exc)) from exc
        headers = {'Cache-Control': 'private, no-store', 'Accept-Ranges': 'bytes', 'X-Content-Type-Options': 'nosniff'}
        requested = request.headers.get('range')
        if requested:
            match = re.fullmatch(r'bytes=(\d*)-(\d*)', requested) if len(requested) <= 80 else None
            if not match or not any(match.groups()):
                return Response(status_code=416, headers={**headers, 'Content-Range': f'bytes */{len(payload)}'})
            start_text, end_text = match.groups()
            start = int(start_text) if start_text else max(0, len(payload) - int(end_text))
            end = min(len(payload) - 1, int(end_text)) if start_text and end_text else len(payload) - 1
            if start > end or start >= len(payload):
                return Response(status_code=416, headers={**headers, 'Content-Range': f'bytes */{len(payload)}'})
            headers['Content-Range'] = f'bytes {start}-{end}/{len(payload)}'
            return Response(payload[start:end + 1], status_code=206, media_type='audio/wav', headers=headers)
        return Response(payload, media_type='audio/wav', headers=headers)

    @app.get("/admin/firmware/package/{version}")
    def admin_firmware_package(request: Request, version: str):
        require_session(request)
        channel = request.query_params.get('channel', 'single')
        if channel not in {'single', 'multi'}:
            raise HTTPException(422, '固件通道无效')
        repository = app.state.multi_firmware_repository if channel == 'multi' else app.state.firmware_repository
        payload = repository.package_bytes(version)
        if payload is None:
            raise HTTPException(404, "版本不存在、已撤回或升级包校验失败")
        return response_headers(Response(payload, media_type="application/zip", headers={
            "Content-Disposition": f'attachment; filename="esp32-ota-{version}.zip"',
            "Cache-Control": "private, no-store",
        }))

    def validate_url(value: str, label: str) -> None:
        try:
            parsed = urlsplit(value)
            _ = parsed.port
        except ValueError:
            raise HTTPException(422, f"{label} 地址无效") from None
        if parsed.scheme not in {"https", "http"} or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise HTTPException(422, f"{label} 需要 HTTP/HTTPS 地址，不能带用户名、密码、查询参数或片段")

    @app.post("/admin/api-keys")
    async def admin_save_api_keys(request: Request):
        _, csrf = require_session(request)
        data = await form_data(request)
        if not hmac.compare_digest(data.get("csrf", ""), csrf):
            raise HTTPException(403, "CSRF 校验失败")
        updated = dict(app.state.runtime_config)
        current = app.state.effective_settings
        slots = [data.get(name, "").strip() for name in ("primary_provider", "secondary_provider", "tertiary_provider")]
        order = [name for name in slots if name]
        try:
            if any(name not in PROVIDERS for name in order) or len(order) != len(set(order)):
                raise HTTPException(422, "主用与备用必须选择不同的厂商")
            if slots != order + [""] * (3 - len(order)):
                raise HTTPException(422, "请从主用开始按顺序配置，不能跳过前面的调用位置")
            updated["llm_provider_order"] = order
            for name in PROVIDERS:
                updated[f"{name}_enabled"] = name in order
                if name in {'custom', 'local_llm'}:
                    protocol = data.get(name + '_protocol', getattr(current, name + '_protocol'))
                    if protocol not in {'openai', 'ollama'}:
                        raise HTTPException(422, '自定义协议须为 OpenAI 兼容或 Ollama 原生')
                    updated[name + '_protocol'] = protocol
                key = f"{name}_api_key"
                if data.get(f"clear_{key}") == "on":
                    updated[key] = ""
                elif data.get(key, "").strip():
                    if len(data[key]) > 4096:
                        raise HTTPException(422, "API Key 过长")
                    updated[key] = data[key].strip()
                for suffix in ("base_url", "model", "models_url"):
                    field = f"{name}_{suffix}"
                    value = data.get(field, getattr(current, field, updated.get(field, ""))).strip()
                    if len(value) > (160 if suffix == "model" else 500):
                        raise HTTPException(422, f"{field} 过长")
                    if suffix != "model" and value:
                        validate_url(value, PROVIDERS[name]["label"])
                    if suffix in {"model", "base_url"} and not value:
                        raise HTTPException(422, f"{PROVIDERS[name]['label']} 的模型和接口地址不能为空")
                    updated[field] = value
                if updated[f"{name}_models_url"]:
                    def origin(url):
                        parsed = urlsplit(url)
                        return parsed.scheme, parsed.hostname, parsed.port or (443 if parsed.scheme == "https" else 80)
                    if origin(updated[f"{name}_base_url"]) != origin(updated[f"{name}_models_url"]):
                        raise HTTPException(422, "模型列表地址必须与调用接口同源")
            activate_config(updated)
        except HTTPException as exc:
            return api_page(csrf, error=str(exc.detail), status_code=exc.status_code)
        return response_headers(RedirectResponse("/admin/api-keys?saved=1", status_code=303))

    @app.post("/admin/api-keys/{provider}/models")
    async def admin_fetch_models(provider: str, request: Request):
        _, csrf = require_session(request)
        data = await form_data(request)
        if not hmac.compare_digest(data.get("csrf", ""), csrf):
            raise HTTPException(403, "CSRF 校验失败")
        if provider not in PROVIDERS:
            raise HTTPException(404, "厂商不存在")
        current = app.state.effective_settings
        key, url = getattr(current, f"{provider}_api_key"), getattr(current, f"{provider}_base_url")
        models_url = app.state.runtime_config.get(f"{provider}_models_url", "")
        try:
            if provider in {'custom', 'local_llm'}:
                models = await run_in_threadpool(fetch_models, provider, key, url, models_url, getattr(current, provider + '_protocol'))
            else:
                models = await run_in_threadpool(fetch_models, provider, key, url, models_url)
        except ModelCatalogError as exc:
            return api_page(csrf, error=str(exc))
        model_catalogs[provider] = {
            "key_id": app.state.store.api_key_id(provider, key, url),
            "models_url": models_url, "models": models,
            "fetched_at": datetime.now(timezone.utc).isoformat(),
        }
        return api_page(csrf, notice=f"已获取 {PROVIDERS[provider]['label']} 的 {len(models)} 个模型；可在模型输入框选择并保存。")

    @app.post("/admin/unlock")
    async def admin_unlock(request: Request):
        if not base.admin_password_hash or not vault:
            return response_headers(HTMLResponse(page_shell("<h1>管理员页面尚未启用</h1>"), status_code=503))
        address = request.client.host if request.client else "unknown"
        if not sessions.allow_attempt(address):
            return unlock_page("尝试次数过多，请 5 分钟后重试。")
        data = await form_data(request)
        password_hash = str(app.state.runtime_config.get("admin_password_hash", base.admin_password_hash))
        if not verify_admin_password(data.get("password", ""), password_hash):
            return unlock_page("密码错误。")
        token, _ = sessions.create()
        response = RedirectResponse("/admin/", status_code=303)
        response.set_cookie(SESSION_COOKIE, token, max_age=8 * 3600, httponly=True, secure=base.admin_cookie_secure, samesite="strict", path="/admin")
        return response_headers(response)

    @app.post("/admin/config")
    async def admin_save(request: Request):
        _, csrf = require_session(request)
        data = await form_data(request)
        if not hmac.compare_digest(data.get("csrf", ""), csrf):
            raise HTTPException(403, "CSRF 校验失败")
        previous = dict(app.state.runtime_config)
        updated = dict(previous)
        service_only = data.get("section") == "service"
        new_password = data.get("new_admin_password", "")
        confirmation = data.get("confirm_admin_password", "")
        if new_password or confirmation:
            if new_password != confirmation:
                raise HTTPException(422, "两次管理员密码不一致")
            try:
                updated["admin_password_hash"] = hash_admin_password(new_password)
            except ValueError as exc:
                raise HTTPException(422, str(exc)) from exc
        checkbox_fields = ("voice_api_enabled", "translation_api_enabled")
        if not service_only:
            checkbox_fields += ("deepseek_enabled", "qwen_enabled", "glm_enabled")
            updated["llm_provider_order"] = [name for name in PROVIDERS if f"{name}_enabled" in data]
        for name in checkbox_fields:
            updated[name] = name in data
        for name in ("deepseek_api_key", "qwen_api_key", "glm_api_key"):
            if service_only:
                continue
            if f"clear_{name}" in data:
                updated[name] = ""
            elif data.get(name, "").strip():
                updated[name] = data[name].strip()
        allowed_text = ("deepseek_base_url", "deepseek_model", "qwen_base_url", "qwen_model", "glm_base_url", "glm_model", "asr_provider", "sensevoice_model", "sensevoice_tokens", "tts_provider", "sherpa_tts_model", "sherpa_tts_tokens", "sherpa_tts_lexicon", "sherpa_tts_data_dir", "sherpa_english_tts_model", "sherpa_english_tts_tokens", "sherpa_english_tts_data_dir")
        for name in allowed_text:
            if service_only and name.startswith(("deepseek_", "qwen_", "glm_")):
                continue
            if name not in data:
                continue
            value = data.get(name, "").strip()
            if len(value) > 500:
                raise HTTPException(422, f"{name} 过长")
            updated[name] = value
        current = app.state.effective_settings
        if updated.get("asr_provider", current.asr_provider) not in {"sensevoice", "mock"} or updated.get("tts_provider", current.tts_provider) not in {"sherpa_vits", "mock"}:
            raise HTTPException(422, "Provider 无效")
        if service_only:
            from .speech_fallback import ROUTES, validate_cloud_fields
            from .billing import money
            for name in ('asr_route', 'tts_route'):
                value = data.get(name, getattr(current, name))
                if value not in ROUTES:
                    raise HTTPException(422, '语音主备方案无效')
                updated[name] = value
            try:
                updated.update(validate_cloud_fields(data, current))
            except ValueError as exc:
                raise HTTPException(422, str(exc)) from None
            for name in ('cloud_asr_unit_price', 'cloud_tts_unit_price'):
                try:
                    value = data.get(name, getattr(current, name)).strip()
                    updated[name] = money(value) if value else ''
                except ValueError as exc:
                    raise HTTPException(422, str(exc)) from None
            if any(updated[name] != 'local_only' for name in ('asr_route', 'tts_route')) and not updated.get('cloud_speech_api_key', current.cloud_speech_api_key):
                raise HTTPException(422, '启用云端主备前请配置语音 API Key')
            if 'system_prompt' in data:
                prompt = data['system_prompt'].strip()
                if not prompt or len(prompt) > 4000:
                    raise HTTPException(422, '系统提示词须为 1～4000 个字符')
                updated['system_prompt'] = prompt
            if 'reply_style' in data:
                if data['reply_style'] not in {'brief', 'normal', 'detailed'}:
                    raise HTTPException(422, '回答风格无效')
                updated['reply_style'] = data['reply_style']
        try:
            updated["sherpa_tts_speaker_id"] = max(0, min(4, int(data.get("sherpa_tts_speaker_id", str(current.sherpa_tts_speaker_id)))))
            updated["sherpa_tts_speed"] = max(0.5, min(2.0, float(data.get("sherpa_tts_speed", str(current.sherpa_tts_speed)))))
            updated["segment_min_chars"] = max(4, min(20, int(data.get("segment_min_chars", str(current.segment_min_chars)))))
            updated["segment_max_chars"] = max(20, min(80, int(data.get("segment_max_chars", str(current.segment_max_chars)))))
        except ValueError as exc:
            raise HTTPException(422, "音色、语速或分段字数格式无效") from exc
        if updated["segment_max_chars"] <= updated["segment_min_chars"]:
            raise HTTPException(422, "最长分段字数必须大于最短分段字数")
        activate_config(updated)
        return response_headers(RedirectResponse("/admin/settings?saved=1" if service_only else "/admin/api-keys?saved=1", status_code=303))

    @app.post("/admin/devices")
    async def admin_create_device(request: Request):
        _, csrf = require_session(request)
        data = await form_data(request)
        if not hmac.compare_digest(data.get("csrf", ""), csrf):
            raise HTTPException(403, "CSRF 校验失败")
        device_id = data.get("device_id", "").strip() or f"device-{secrets.token_hex(5)}"
        device_name = data.get("device_name", "").strip() or "未命名设备"
        persona = data.get("persona", "").strip()
        if not DEVICE_ID_PATTERN.fullmatch(device_id):
            raise HTTPException(422, "设备 ID 只能使用字母、数字、点、冒号、下划线和连字符，长度 3～64")
        if len(device_name) > 80 or len(persona) > 800:
            raise HTTPException(422, "设备名称或角色设定过长")
        if data.get('reply_style', 'default') not in {'default', 'brief', 'normal', 'detailed'}:
            raise HTTPException(422, '回答风格无效')
        try:
            memory_turns = max(1, min(10, int(data.get("memory_turns", "4"))))
            tts_speaker_id = max(0, min(4, int(data.get("tts_speaker_id", "0"))))
            tts_english_speaker_id = max(0, min(903, int(data.get("tts_english_speaker_id", "0"))))
            tts_speed = max(0.5, min(2.0, float(data.get("tts_speed", "1.0"))))
        except ValueError as exc:
            raise HTTPException(422, "记忆轮数、音色或语速格式无效") from exc
        try:
            device_token = app.state.store.create_device(
                device_id,
                device_name,
                persona,
                "memory_enabled" in data,
                memory_turns,
                tts_speaker_id,
                tts_english_speaker_id,
                tts_speed,
            )
        except ValueError as exc:
            raise HTTPException(409, str(exc)) from exc
        app.state.store.update_dialogue_profile(device_id, data.get('reply_style', 'default'), persona,
                                               'memory_enabled' in data, memory_turns, 'web_search_enabled' in data)
        content = f"""<h1>设备凭证已创建</h1><p class="bad">Token 只显示这一次，请立即复制到电脑客户端或 ESP32 NVS。</p><fieldset><label>设备 ID</label><code>{html.escape(device_id)}</code><label>设备 Token</label><code id="new-device-token">{html.escape(device_token)}</code></fieldset><p><a href="/admin/devices">返回设备 KEY</a></p>"""
        return response_headers(HTMLResponse(page_shell(content, "设备凭证已创建")))

    @app.post("/admin/firmware/upload")
    async def admin_upload_firmware(request: Request):
        _, csrf = require_session(request)
        try:
            form = await request.form(max_files=1, max_fields=4, max_part_size=MAX_PACKAGE_BYTES)
            if not hmac.compare_digest(str(form.get("csrf", "")), csrf):
                raise HTTPException(403, "CSRF 校验失败")
            upload = form.get("package")
            if upload is None or not hasattr(upload, "read"):
                raise FirmwarePackageError("请选择 OTA ZIP 包")
            try:
                payload = await upload.read(MAX_PACKAGE_BYTES + 1)
            finally:
                await upload.close()
            if len(payload) > MAX_PACKAGE_BYTES:
                raise FirmwarePackageError("OTA ZIP 包超过允许大小")
            channel = str(form.get("channel", "single"))
            if channel not in {"single", "multi"}:
                raise FirmwarePackageError("固件通道无效")
            repository = app.state.multi_firmware_repository if channel == "multi" else app.state.firmware_repository
            release = repository.publish_package(
                payload, notes=str(form.get("notes", "")),
                minimum_version=str(form.get("minimum_version", "")).strip(),
            )
        except FirmwarePackageError as exc:
            content = (
                f'<h1>固件发布失败</h1><p class="bad">{html.escape(str(exc))}</p>'
                '<p><a href="/admin/firmware">返回固件版本</a></p>'
            )
            return response_headers(HTMLResponse(page_shell(content, "固件发布失败"), status_code=422))
        return response_headers(
            HTMLResponse(
                page_shell(
                    f'<h1>固件已发布</h1><p class="ok">版本 <code>{html.escape(release.version)}</code> 已成为设备可检查的当前版本。</p><p><a href="/admin/firmware">返回固件版本</a></p>',
                    "固件已发布",
                )
            )
        )

    @app.post("/admin/device-action")
    async def admin_device_action(request: Request):
        _, csrf = require_session(request)
        data = await form_data(request)
        if not hmac.compare_digest(data.get("csrf", ""), csrf):
            raise HTTPException(403, "CSRF 校验失败")
        device_id = data.get("device_id", "").strip()
        action = data.get("action", "")
        if not DEVICE_ID_PATTERN.fullmatch(device_id):
            raise HTTPException(422, "设备 ID 无效")
        device = app.state.store.get_device(device_id)
        if not device:
            raise HTTPException(404, "设备不存在")
        try:
            if action == "enable":
                app.state.store.set_device_enabled(device_id, True)
            elif action == "disable":
                app.state.store.set_device_enabled(device_id, False)
            elif action == "memory_enable":
                app.state.store.set_memory_enabled(device_id, True)
            elif action == "memory_disable":
                app.state.store.set_memory_enabled(device_id, False)
            elif action == "save_profile":
                persona = data.get("persona", "").strip()
                if len(persona) > 800:
                    raise HTTPException(422, "角色设定不能超过 800 个字符")
                memory_turns = max(1, min(10, int(data.get("memory_turns", "4"))))
                app.state.store.update_dialogue_profile(
                    device_id, data.get('reply_style', device['reply_style']), persona,
                    bool(device["memory_enabled"]), memory_turns, 'web_search_enabled' in data
                )
                app.state.store.update_device_voice(
                    device_id,
                    int(data.get("tts_speaker_id", device["tts_speaker_id"])),
                    int(data.get("tts_english_speaker_id", device["tts_english_speaker_id"])),
                    float(data.get("tts_speed", device["tts_speed"])),
                )
            elif action == "clear_memory":
                if data.get("confirm_clear") != "on":
                    raise HTTPException(422, "请先勾选确认清除")
                try:
                    app.state.voice_service.clear_memory(device_id)
                except RuntimeError:
                    raise HTTPException(409, '请等待语音任务结束后再清除记忆')
            else:
                raise HTTPException(422, "设备操作无效")
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from exc
        return response_headers(RedirectResponse("/admin/devices", status_code=303))

    @app.post("/admin/reset-recommended")
    async def admin_reset_recommended(request: Request):
        _, csrf = require_session(request)
        data = await form_data(request)
        if not hmac.compare_digest(data.get("csrf", ""), csrf):
            raise HTTPException(403, "CSRF 校验失败")
        if data.get("confirm_reset") != "on":
            raise HTTPException(422, "请先确认恢复推荐配置")
        updated = dict(app.state.runtime_config)
        updated.update(
            {
                "voice_api_enabled": True,
                "translation_api_enabled": True,
                "deepseek_enabled": True,
                "qwen_enabled": True,
                "glm_enabled": False,
                "llm_provider_order": ["deepseek", "qwen"],
                "deepseek_models_url": "",
                "qwen_models_url": "",
                "glm_models_url": "",
                "deepseek_base_url": base.deepseek_base_url,
                "deepseek_model": base.deepseek_model,
                "qwen_base_url": base.qwen_base_url,
                "qwen_model": base.qwen_model,
                "glm_base_url": base.glm_base_url,
                "glm_model": base.glm_model,
                "asr_provider": base.asr_provider,
                "sensevoice_model": str(base.sensevoice_model or ""),
                "sensevoice_tokens": str(base.sensevoice_tokens or ""),
                "tts_provider": base.tts_provider,
                "sherpa_tts_model": str(base.sherpa_tts_model or ""),
                "sherpa_tts_tokens": str(base.sherpa_tts_tokens or ""),
                "sherpa_tts_lexicon": str(base.sherpa_tts_lexicon or ""),
                "sherpa_tts_data_dir": "",
                "sherpa_english_tts_model": str(base.sherpa_english_tts_model or ""),
                "sherpa_english_tts_tokens": str(base.sherpa_english_tts_tokens or ""),
                "sherpa_english_tts_data_dir": str(base.sherpa_english_tts_data_dir or ""),
                "sherpa_tts_speaker_id": base.sherpa_tts_speaker_id,
                "sherpa_tts_speed": base.sherpa_tts_speed,
                "segment_min_chars": 8,
                "segment_max_chars": 40,
            }
        )
        activate_config(updated)
        return response_headers(RedirectResponse("/admin/", status_code=303))

    @app.post("/admin/lock")
    async def admin_lock(request: Request):
        token, csrf = require_session(request)
        data = await form_data(request)
        if not hmac.compare_digest(data.get("csrf", ""), csrf):
            raise HTTPException(403, "CSRF 校验失败")
        sessions.delete(token)
        response = RedirectResponse("/admin/", status_code=303)
        response.delete_cookie(SESSION_COOKIE, path="/admin")
        return response_headers(response)
