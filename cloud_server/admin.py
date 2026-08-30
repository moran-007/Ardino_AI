from __future__ import annotations

import base64
import hashlib
import hmac
import html
import json
import os
import gc
import re
import secrets
import threading
import time
from dataclasses import replace
from pathlib import Path
from urllib.parse import parse_qs

from cryptography.fernet import Fernet, InvalidToken
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import HTMLResponse, RedirectResponse

from .config import Settings
from .service import VoiceService

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
    if any(f"{name}_enabled" in config for name in ("deepseek", "qwen", "glm")):
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
    response.headers["Content-Security-Policy"] = "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'"
    return response


def page_shell(content: str, title: str = "ESP32 语音服务配置") -> str:
    return f"""<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>{html.escape(title)}</title><style>
body{{font-family:system-ui,sans-serif;max-width:1280px;margin:32px auto;padding:0 18px;background:#f4f7fb;color:#172033}}fieldset,details{{background:white;border:1px solid #d9e0ea;border-radius:10px;padding:18px;margin:16px 0}}details>summary{{font-weight:650;cursor:pointer}}label{{display:block;margin:10px 0 4px}}input[type=text],input[type=password],input[type=number],select,textarea{{width:100%;box-sizing:border-box;padding:10px;border:1px solid #aeb8c8;border-radius:6px}}textarea{{min-width:260px;min-height:80px;resize:vertical}}button{{padding:10px 18px;border:0;border-radius:6px;background:#1267d6;color:white;cursor:pointer;margin:2px}}button.secondary{{background:#5c6677}}button.danger{{background:#b42318}}.row{{display:grid;grid-template-columns:1fr 1fr;gap:14px}}.ok{{color:#08783f}}.bad{{color:#b42318}}.muted{{color:#657084;font-size:.92rem}}code{{overflow-wrap:anywhere}}.device-table{{overflow-x:auto}}table{{width:100%;border-collapse:collapse}}th,td{{text-align:left;padding:8px;border-bottom:1px solid #e3e8ef;vertical-align:top}}@media(max-width:650px){{.row{{grid-template-columns:1fr}}}}
</style></head><body>{content}</body></html>"""


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
        if old_service.active_jobs and (not asr_unchanged or not tts_unchanged):
            raise HTTPException(409, "有语音任务正在运行，请任务结束后再切换本地模型")
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

    @app.get("/admin/", response_class=HTMLResponse)
    def admin_home(request: Request):
        if not base.admin_password_hash or not vault:
            return response_headers(HTMLResponse(page_shell("<h1>管理员页面尚未在服务器环境中启用</h1>"), status_code=503))
        try:
            _, csrf = require_session(request)
        except HTTPException:
            return unlock_page()
        config = app.state.runtime_config
        current = app.state.effective_settings
        service = app.state.voice_service
        devices = app.state.store.list_devices()

        def value(name: str, fallback: object = "") -> str:
            return html.escape(str(config.get(name, fallback) or ""), quote=True)

        def checked(name: str, fallback: bool) -> str:
            return " checked" if bool(config.get(name, fallback)) else ""

        def key_status(name: str, fallback: str) -> str:
            return "已配置（留空保持不变）" if config.get(name) or fallback else "未配置"

        state_class = "ok" if service.ready else "bad"
        state_text = "运行就绪" if service.ready else f"未就绪：{service.status_error}"
        device_rows = ""
        for index, item in enumerate(devices):
            form_id = f"device-form-{index}"
            device_id = html.escape(item["device_id"], quote=True)
            persona = html.escape(item.get("persona", ""), quote=True)
            enabled_action = "disable" if item["enabled"] else "enable"
            enabled_label = "停用设备" if item["enabled"] else "启用设备"
            memory_action = "memory_disable" if item["memory_enabled"] else "memory_enable"
            memory_label = "关闭记忆" if item["memory_enabled"] else "开启记忆"
            device_rows += f"""<tr><td><code>{device_id}</code><br>{html.escape(item['name'])}</td><td>{'启用' if item['enabled'] else '停用'}</td><td>{'开启' if item['memory_enabled'] else '关闭'} · 已存 {item['memory_count']} 轮<label>携带最近轮数</label><input form="{form_id}" type="number" name="memory_turns" min="1" max="10" value="{item['memory_turns']}"></td><td><textarea form="{form_id}" name="persona" maxlength="800" placeholder="例如：称呼用户为小明；回答适合儿童；喜欢恐龙。">{persona}</textarea></td><td><label>中文音色（0～4）</label><input form="{form_id}" type="number" name="tts_speaker_id" min="0" max="4" value="{item['tts_speaker_id']}"><label>英文音色（0～903）</label><input form="{form_id}" type="number" name="tts_english_speaker_id" min="0" max="903" value="{item['tts_english_speaker_id']}"><label>语速（0.5～2.0）</label><input form="{form_id}" type="number" name="tts_speed" min="0.5" max="2" step="0.05" value="{item['tts_speed']}"></td><td><form id="{form_id}" method="post" action="/admin/device-action"><input type="hidden" name="csrf" value="{csrf}"><input type="hidden" name="device_id" value="{device_id}"><button name="action" value="save_profile" type="submit">保存个性化设置</button><button class="secondary" name="action" value="{enabled_action}" type="submit">{enabled_label}</button><button class="secondary" name="action" value="{memory_action}" type="submit">{memory_label}</button><label class="muted"><input type="checkbox" name="confirm_clear">确认清除</label><button class="danger" name="action" value="clear_memory" type="submit">清除记忆</button></form></td></tr>"""
        if not device_rows:
            device_rows = '<tr><td colspan="6" class="muted">尚未创建设备</td></tr>'
        content = f"""
<h1>ESP32 语音服务配置</h1><p class="{state_class}">{html.escape(state_text)}</p><p class="muted">保存后自动重载模型。API Key 不会在页面回显，配置文件使用 Fernet 加密。</p>
<fieldset><legend>设备凭证、记忆与个性化语音</legend><p class="muted">角色、记忆、中文/英文音色和语速均按设备 ID 隔离。ESP32 可使用自己的 Token 修改本设备语音设置，不会改变其他设备或服务器全局配置。英文使用 904 音色的本地 Piper 模型。凭证 Token 只显示一次。</p><form method="post" action="/admin/devices"><input type="hidden" name="csrf" value="{csrf}"><div class="row"><div><label>设备 ID（留空自动生成）</label><input type="text" name="device_id" maxlength="64" placeholder="例如 pc-text-01"></div><div><label>设备名称</label><input type="text" name="device_name" maxlength="80" value="电脑文字测试"></div></div><label>设备专属角色设定</label><textarea name="persona" maxlength="800" placeholder="例如：这是小明的设备；称呼他为小明；用适合 8 岁儿童的方式回答。"></textarea><div class="row"><label><input type="checkbox" name="memory_enabled"> 创建后启用独立对话记忆</label><div><label>携带最近轮数</label><input type="number" name="memory_turns" min="1" max="10" value="4"></div></div><div class="row"><div><label>默认中文音色（0～4）</label><input type="number" name="tts_speaker_id" min="0" max="4" value="0"></div><div><label>默认英文音色（0～903）</label><input type="number" name="tts_english_speaker_id" min="0" max="903" value="0"></div></div><label>默认语速（仅本设备）</label><input type="number" name="tts_speed" min="0.5" max="2" step="0.05" value="1.0"><p><button type="submit">创建设备凭证</button></p></form><div class="device-table"><table><thead><tr><th>设备</th><th>访问状态</th><th>记忆</th><th>专属角色</th><th>个性化语音</th><th>操作</th></tr></thead><tbody>{device_rows}</tbody></table></div></fieldset>
<form method="post" action="/admin/config"><input type="hidden" name="csrf" value="{csrf}">
<fieldset><legend>接口开关</legend><label><input type="checkbox" name="voice_api_enabled"{checked('voice_api_enabled', current.voice_api_enabled)}> 启用语音对话 API</label><label><input type="checkbox" name="translation_api_enabled"{checked('translation_api_enabled', current.translation_api_enabled)}> 启用文字翻译 API</label></fieldset>
<fieldset><legend>管理员密码</legend><p class="muted">留空表示不修改。修改后下一次解锁生效，至少 10 个字符。</p><div class="row"><div><label>新密码</label><input type="password" name="new_admin_password" autocomplete="new-password"></div><div><label>再次输入</label><input type="password" name="confirm_admin_password" autocomplete="new-password"></div></div></fieldset>
<fieldset><legend>DeepSeek（主用）</legend><label><input type="checkbox" name="deepseek_enabled"{checked('deepseek_enabled', 'deepseek' in current.llm_provider_order)}> 启用</label><label>API Key · {key_status('deepseek_api_key', base.deepseek_api_key)}</label><input type="password" name="deepseek_api_key" autocomplete="new-password"><label><input type="checkbox" name="clear_deepseek_api_key"> 清除已保存 Key</label><div class="row"><div><label>Base URL</label><input type="text" name="deepseek_base_url" value="{value('deepseek_base_url', current.deepseek_base_url)}"></div><div><label>模型（可选择或自定义）</label><input list="deepseek-model-options" type="text" name="deepseek_model" value="{value('deepseek_model', current.deepseek_model)}"><datalist id="deepseek-model-options"><option value="deepseek-v4-flash"><option value="deepseek-v4-pro"></datalist></div></div></fieldset>
<fieldset><legend>通义千问（第一备用）</legend><label><input type="checkbox" name="qwen_enabled"{checked('qwen_enabled', 'qwen' in current.llm_provider_order)}> 启用</label><label>API Key · {key_status('qwen_api_key', base.qwen_api_key)}</label><input type="password" name="qwen_api_key" autocomplete="new-password"><label><input type="checkbox" name="clear_qwen_api_key"> 清除已保存 Key</label><p class="muted">推荐使用阿里云 DashScope 中国站原生 SSE 地址；兼容模式地址仍可继续使用。</p><div class="row"><div><label>Base URL</label><input type="text" name="qwen_base_url" value="{value('qwen_base_url', current.qwen_base_url)}"></div><div><label>模型（可选择或自定义）</label><input list="qwen-model-options" type="text" name="qwen_model" value="{value('qwen_model', current.qwen_model)}"><datalist id="qwen-model-options"><option value="qwen-plus"><option value="qwen3.7-max"><option value="qwen3.7-max-2026-06-08"></datalist></div></div></fieldset>
<fieldset><legend>智谱 GLM（第二备用）</legend><label><input type="checkbox" name="glm_enabled"{checked('glm_enabled', 'glm' in current.llm_provider_order)}> 启用</label><label>API Key · {key_status('glm_api_key', base.glm_api_key)}</label><input type="password" name="glm_api_key" autocomplete="new-password"><label><input type="checkbox" name="clear_glm_api_key"> 清除已保存 Key</label><div class="row"><div><label>Base URL</label><input type="text" name="glm_base_url" value="{value('glm_base_url', current.glm_base_url)}"></div><div><label>模型（可选择或自定义）</label><input list="glm-model-options" type="text" name="glm_model" value="{value('glm_model', current.glm_model)}"><datalist id="glm-model-options"><option value="glm-4.7-flash"><option value="glm-5-turbo"><option value="glm-5.2"></datalist></div></div></fieldset>
<details><summary>高级：本地 ASR/TTS 模型配置（通常无需修改）</summary><fieldset><legend>本地语音识别 ASR</legend><label>Provider</label><select name="asr_provider"><option value="sensevoice"{' selected' if current.asr_provider == 'sensevoice' else ''}>SenseVoice</option><option value="mock"{' selected' if current.asr_provider == 'mock' else ''}>Mock（仅诊断）</option></select><label>模型 ONNX 绝对路径</label><input type="text" name="sensevoice_model" value="{value('sensevoice_model', current.sensevoice_model)}"><label>tokens.txt 绝对路径</label><input type="text" name="sensevoice_tokens" value="{value('sensevoice_tokens', current.sensevoice_tokens)}"></fieldset>
<fieldset><legend>本地语音合成 TTS</legend><label>Provider</label><select name="tts_provider"><option value="sherpa_vits"{' selected' if current.tts_provider == 'sherpa_vits' else ''}>sherpa-onnx VITS</option><option value="mock"{' selected' if current.tts_provider == 'mock' else ''}>Mock（仅诊断）</option></select><label>中文模型 ONNX 绝对路径</label><input type="text" name="sherpa_tts_model" value="{value('sherpa_tts_model', current.sherpa_tts_model)}"><label>中文 tokens.txt 绝对路径</label><input type="text" name="sherpa_tts_tokens" value="{value('sherpa_tts_tokens', current.sherpa_tts_tokens)}"><label>中文 lexicon.txt 绝对路径</label><input type="text" name="sherpa_tts_lexicon" value="{value('sherpa_tts_lexicon', current.sherpa_tts_lexicon)}"><label>中文 data_dir（当前 vits-zh-ll 必须留空）</label><input type="text" name="sherpa_tts_data_dir" value="{value('sherpa_tts_data_dir', current.sherpa_tts_data_dir)}"><label>英文 Piper 模型 ONNX 绝对路径</label><input type="text" name="sherpa_english_tts_model" value="{value('sherpa_english_tts_model', current.sherpa_english_tts_model)}"><label>英文 tokens.txt 绝对路径</label><input type="text" name="sherpa_english_tts_tokens" value="{value('sherpa_english_tts_tokens', current.sherpa_english_tts_tokens)}"><label>英文 espeak-ng-data 目录</label><input type="text" name="sherpa_english_tts_data_dir" value="{value('sherpa_english_tts_data_dir', current.sherpa_english_tts_data_dir)}"><div class="row"><div><label>服务器回退中文音色</label><input type="number" min="0" max="4" name="sherpa_tts_speaker_id" value="{current.sherpa_tts_speaker_id}"></div><div><label>服务器回退语速</label><input type="number" min="0.5" max="2" step="0.05" name="sherpa_tts_speed" value="{current.sherpa_tts_speed}"></div></div><p class="muted">音色和语速以设备设置为准；这里只是模型路径及无设备配置时的回退值。实时分段建议 8/40。</p><div class="row"><div><label>最短分段字数</label><input type="number" min="4" max="20" name="segment_min_chars" value="{current.segment_min_chars}"></div><div><label>最长分段字数</label><input type="number" min="20" max="80" name="segment_max_chars" value="{current.segment_max_chars}"></div></div></fieldset></details>
<p><button type="submit">保存并重载</button></p></form><form method="post" action="/admin/reset-recommended"><input type="hidden" name="csrf" value="{csrf}"><fieldset><legend>配置恢复</legend><p class="muted">恢复接口、厂商地址、模型名称、本地模型路径、音色和语速的服务器推荐值；保留 API Key、管理员密码和设备凭证。</p><label><input type="checkbox" name="confirm_reset" required> 我确认恢复推荐配置</label><button class="danger" type="submit">恢复推荐默认配置</button></fieldset></form><form method="post" action="/admin/lock"><input type="hidden" name="csrf" value="{csrf}"><button class="secondary" type="submit">锁定页面</button></form>"""
        return response_headers(HTMLResponse(page_shell(content)))

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
        new_password = data.get("new_admin_password", "")
        confirmation = data.get("confirm_admin_password", "")
        if new_password or confirmation:
            if new_password != confirmation:
                raise HTTPException(422, "两次管理员密码不一致")
            try:
                updated["admin_password_hash"] = hash_admin_password(new_password)
            except ValueError as exc:
                raise HTTPException(422, str(exc)) from exc
        for name in ("voice_api_enabled", "translation_api_enabled", "deepseek_enabled", "qwen_enabled", "glm_enabled"):
            updated[name] = name in data
        for name in ("deepseek_api_key", "qwen_api_key", "glm_api_key"):
            if f"clear_{name}" in data:
                updated.pop(name, None)
            elif data.get(name, "").strip():
                updated[name] = data[name].strip()
        allowed_text = ("deepseek_base_url", "deepseek_model", "qwen_base_url", "qwen_model", "glm_base_url", "glm_model", "asr_provider", "sensevoice_model", "sensevoice_tokens", "tts_provider", "sherpa_tts_model", "sherpa_tts_tokens", "sherpa_tts_lexicon", "sherpa_tts_data_dir", "sherpa_english_tts_model", "sherpa_english_tts_tokens", "sherpa_english_tts_data_dir")
        for name in allowed_text:
            value = data.get(name, "").strip()
            if len(value) > 500:
                raise HTTPException(422, f"{name} 过长")
            updated[name] = value
        if updated.get("asr_provider") not in {"sensevoice", "mock"} or updated.get("tts_provider") not in {"sherpa_vits", "mock"}:
            raise HTTPException(422, "Provider 无效")
        try:
            updated["sherpa_tts_speaker_id"] = max(0, min(1000, int(data.get("sherpa_tts_speaker_id", "0"))))
            updated["sherpa_tts_speed"] = max(0.5, min(2.0, float(data.get("sherpa_tts_speed", "1.0"))))
            updated["segment_min_chars"] = max(4, min(20, int(data.get("segment_min_chars", str(base.segment_min_chars)))))
            updated["segment_max_chars"] = max(20, min(80, int(data.get("segment_max_chars", str(base.segment_max_chars)))))
        except ValueError as exc:
            raise HTTPException(422, "音色、语速或分段字数格式无效") from exc
        if updated["segment_max_chars"] <= updated["segment_min_chars"]:
            raise HTTPException(422, "最长分段字数必须大于最短分段字数")
        activate_config(updated)
        return response_headers(RedirectResponse("/admin/", status_code=303))

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
        content = f"""<h1>设备凭证已创建</h1><p class="bad">Token 只显示这一次，请立即复制到电脑客户端或 ESP32 NVS。</p><fieldset><label>设备 ID</label><code>{html.escape(device_id)}</code><label>设备 Token</label><code id="new-device-token">{html.escape(device_token)}</code></fieldset><p><a href="/admin/">返回管理页面</a></p>"""
        return response_headers(HTMLResponse(page_shell(content, "设备凭证已创建")))

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
                app.state.store.update_device_profile(
                    device_id, persona, bool(device["memory_enabled"]), memory_turns
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
                app.state.store.clear_memories(device_id)
            else:
                raise HTTPException(422, "设备操作无效")
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from exc
        return response_headers(RedirectResponse("/admin/", status_code=303))

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
