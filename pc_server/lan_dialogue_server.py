"""LAN dialogue server for the ESP32-S3 standalone voice device.

The ESP32 uploads raw 16-kHz mono PCM. This server performs local Zipformer
ASR, calls the configured DeepSeek API, synthesizes speech locally on Windows,
and exposes the result as a raw PCM file that the board can stream without
holding the complete answer in ESP32 RAM or flash.
"""

from __future__ import annotations

import argparse
from collections import defaultdict, deque
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
import wave

import numpy as np
import sherpa_onnx
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, HTMLResponse
import uvicorn


SERVER_DIR = Path(__file__).resolve().parent
from audio_frontend import MicTuning, process_pcm  # noqa: E402


CONFIG_PATH = SERVER_DIR / "server_config.local.json"
JOBS_DIR = SERVER_DIR / "jobs"
SPEECH_HELPER = SERVER_DIR / "windows_speech.ps1"
DEFAULT_MODEL_DIR = (
    SERVER_DIR / "models" / "sherpa-onnx-zipformer-ctc-zh-int8-2025-07-03"
)
DISCOVERY_MESSAGE = b"ESP32_AI_DISCOVER_V1"
DISCOVERY_REPLY = "ESP32_AI_SERVER_V1"
PCM_SAMPLE_RATE = 16000
PCM_BYTES_PER_SECOND = PCM_SAMPLE_RATE * 2
DEEPSEEK_CHAT_URL = "https://api.deepseek.com/chat/completions"


@dataclass
class ServerConfig:
    bind_host: str = "0.0.0.0"
    port: int = 8765
    discovery_port: int = 8764
    api_url: str = DEEPSEEK_CHAT_URL
    api_key: str = ""
    model: str = "deepseek-v4-flash"
    max_tokens: int = 4096
    thinking: bool = False
    system_prompt: str = "你是一个简洁、友好、可靠的中文语音助手。回答适合直接朗读。"
    voice: str = "Microsoft Huihui Desktop"
    max_record_seconds: int = 20
    job_ttl_hours: int = 12
    device_token: str = ""
    model_dir: str = str(DEFAULT_MODEL_DIR)

    @classmethod
    def load(cls, path: Path) -> "ServerConfig":
        if not path.exists():
            return cls()
        raw = json.loads(path.read_text(encoding="utf-8-sig"))
        config = cls(**{key: value for key, value in raw.items() if key in cls.__dataclass_fields__})
        config.validate()
        return config

    def validate(self) -> None:
        if not 1 <= self.port <= 65535 or not 1 <= self.discovery_port <= 65535:
            raise ValueError("server ports must be in 1..65535")
        if not 128 <= self.max_tokens <= 384000:
            raise ValueError("max_tokens must be in 128..384000")
        if not 2 <= self.max_record_seconds <= 60:
            raise ValueError("max_record_seconds must be in 2..60")
        if self.model not in {"deepseek-v4-flash", "deepseek-v4-pro"}:
            raise ValueError("model must be deepseek-v4-flash or deepseek-v4-pro")
        if self.api_url != DEEPSEEK_CHAT_URL:
            raise ValueError("api_url must be the official DeepSeek chat endpoint")
        if not self.system_prompt.strip() or len(self.system_prompt) > 12000:
            raise ValueError("system_prompt must contain 1..12000 characters")
        if not self.voice.strip() or len(self.voice) > 200:
            raise ValueError("voice must contain 1..200 characters")

    def public_dict(self) -> dict:
        data = asdict(self)
        data["api_key"] = "configured" if self.effective_api_key() else "missing"
        data["device_token"] = "configured" if self.device_token else "disabled"
        return data

    def effective_api_key(self) -> str:
        return os.environ.get("DEEPSEEK_API_KEY", "").strip() or self.api_key.strip()

    def save(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(
            json.dumps(asdict(self), ensure_ascii=False, indent=2), encoding="utf-8"
        )
        temporary.replace(path)


class ZipformerRecognizer:
    def __init__(self, model_dir: Path, threads: int = 4) -> None:
        model = model_dir / "model.int8.onnx"
        tokens = model_dir / "tokens.txt"
        if not model.is_file() or not tokens.is_file():
            raise RuntimeError(f"Zipformer model is incomplete: {model_dir}")
        self.recognizer = sherpa_onnx.OfflineRecognizer.from_zipformer_ctc(
            model=str(model),
            tokens=str(tokens),
            num_threads=max(1, threads),
            debug=False,
            provider="cpu",
        )
        self.lock = threading.Lock()

    def recognize(self, pcm: bytes) -> str:
        samples = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
        with self.lock:
            stream = self.recognizer.create_stream()
            stream.accept_waveform(PCM_SAMPLE_RATE, samples)
            self.recognizer.decode_stream(stream)
            result = stream.result
        text = result.text if hasattr(result, "text") else str(result)
        return re.sub(r"<\|[^|]+\|>", "", text).strip()


def write_pcm_wav(path: Path, pcm: bytes) -> None:
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(PCM_SAMPLE_RATE)
        output.writeframes(pcm)


def wav_to_pcm(wav_path: Path, pcm_path: Path) -> int:
    with wave.open(str(wav_path), "rb") as source:
        if source.getnchannels() != 1 or source.getsampwidth() != 2:
            raise RuntimeError("TTS output must be 16-bit mono PCM")
        if source.getframerate() != PCM_SAMPLE_RATE:
            raise RuntimeError(f"TTS output sample rate must be {PCM_SAMPLE_RATE} Hz")
        total = 0
        with pcm_path.open("wb") as output:
            while True:
                chunk = source.readframes(8192)
                if not chunk:
                    break
                output.write(chunk)
                total += len(chunk)
    return total


def clean_for_tts(text: str) -> str:
    text = re.sub(r"```.*?```", "代码内容请查看电脑服务端记录。", text, flags=re.S)
    text = re.sub(r"https?://\S+", "链接请查看电脑服务端记录", text)
    text = re.sub(r"[*_#>`~|]", "", text)
    return re.sub(r"\s+", " ", text).strip()


def run_windows_tts(text: str, wav_path: Path, voice: str) -> None:
    if not SPEECH_HELPER.is_file():
        raise RuntimeError(f"missing Windows speech helper: {SPEECH_HELPER}")
    text_path = wav_path.with_suffix(".txt")
    text_path.write_text(clean_for_tts(text), encoding="utf-8")
    command = [
        "powershell.exe",
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(SPEECH_HELPER),
        "-Mode",
        "tts",
        "-TextFile",
        str(text_path),
        "-OutputPath",
        str(wav_path),
        "-Voice",
        voice,
    ]
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=900,
        check=False,
    )
    lines = [line.strip().lstrip("\ufeff") for line in completed.stdout.splitlines() if line.strip()]
    if completed.returncode or not lines:
        detail = completed.stderr.strip() or "Windows TTS failed"
        raise RuntimeError(detail)
    try:
        result = json.loads(lines[-1])
    except json.JSONDecodeError as error:
        raise RuntimeError(lines[-1]) from error
    if not result.get("ok") or not wav_path.is_file():
        raise RuntimeError(str(result.get("error", "Windows TTS did not create audio")))


def call_deepseek(config: ServerConfig, messages: list[dict]) -> str:
    api_key = config.effective_api_key()
    if not api_key:
        raise RuntimeError("DeepSeek API key is not configured on the LAN server")
    payload = {
        "model": config.model,
        "messages": messages,
        "max_tokens": config.max_tokens,
        "stream": False,
        "thinking": {"type": "enabled" if config.thinking else "disabled"},
    }
    request = urllib.request.Request(
        config.api_url,
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "User-Agent": "ESP32-LAN-Dialogue/1.0",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=300) as response:
            result = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")[:1000]
        raise RuntimeError(f"DeepSeek HTTP {error.code}: {detail}") from error
    answer = str(result["choices"][0]["message"]["content"]).strip()
    if not answer:
        raise RuntimeError("DeepSeek returned an empty answer")
    return answer


class DiscoveryResponder(threading.Thread):
    def __init__(self, http_port: int, discovery_port: int) -> None:
        super().__init__(name="esp32-lan-discovery", daemon=True)
        self.http_port = http_port
        self.discovery_port = discovery_port
        self.stop_event = threading.Event()
        self.sock: socket.socket | None = None

    def run(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock = sock
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.settimeout(0.5)
        sock.bind(("", self.discovery_port))
        while not self.stop_event.is_set():
            try:
                data, address = sock.recvfrom(256)
            except socket.timeout:
                continue
            except OSError:
                break
            if data.strip() == DISCOVERY_MESSAGE:
                reply = f"{DISCOVERY_REPLY} {self.http_port}".encode("ascii")
                sock.sendto(reply, address)
        sock.close()

    def close(self) -> None:
        self.stop_event.set()
        if self.sock:
            self.sock.close()


def local_ipv4_addresses() -> list[str]:
    addresses: set[str] = set()
    try:
        for item in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            address = item[4][0]
            if not address.startswith("127."):
                addresses.add(address)
    except socket.gaierror:
        pass
    return sorted(addresses)


def create_app(
    config: ServerConfig, recognizer: ZipformerRecognizer, config_path: Path = CONFIG_PATH
) -> FastAPI:
    app = FastAPI(title="ESP32 LAN Dialogue Server", version="1.0")
    histories: dict[str, deque[tuple[str, str]]] = defaultdict(lambda: deque(maxlen=4))
    jobs_lock = threading.Lock()
    settings_lock = threading.Lock()
    JOBS_DIR.mkdir(parents=True, exist_ok=True)

    def require_device(request: Request) -> str:
        if config.device_token:
            supplied = request.headers.get("X-Device-Token", "")
            if supplied != config.device_token:
                raise HTTPException(status_code=401, detail="invalid device token")
        return request.headers.get("X-Device-ID", "esp32-s3")[:80]

    def write_job_state(job_dir: Path, state: dict) -> None:
        temporary = job_dir / "state.tmp"
        temporary.write_text(json.dumps(state, ensure_ascii=False), encoding="utf-8")
        with jobs_lock:
            temporary.replace(job_dir / "state.json")

    def process_dialogue_job(job_id: str, device_id: str, pcm: bytes) -> None:
        job_dir = JOBS_DIR / job_id
        started = time.monotonic()
        try:
            write_job_state(job_dir, {"ok": True, "job_id": job_id, "status": "processing"})
            raw_wav = job_dir / "input_raw.wav"
            processed_wav = job_dir / "input_processed.wav"
            write_pcm_wav(raw_wav, pcm)
            processed, audio_stats = process_pcm(pcm, PCM_SAMPLE_RATE, MicTuning())
            write_pcm_wav(processed_wav, processed)
            recognized = recognizer.recognize(processed)
            if not recognized:
                raise RuntimeError("no speech was recognized")

            messages: list[dict] = [{"role": "system", "content": config.system_prompt}]
            with jobs_lock:
                previous_turns = list(histories[device_id])
            for user_text, assistant_text in previous_turns:
                messages.append({"role": "user", "content": user_text})
                messages.append({"role": "assistant", "content": assistant_text})
            messages.append({"role": "user", "content": recognized})

            answer = call_deepseek(config, messages)
            wav_path = job_dir / "answer.wav"
            pcm_path = job_dir / "answer.pcm"
            run_windows_tts(answer, wav_path, config.voice)
            audio_bytes = wav_to_pcm(wav_path, pcm_path)

            with jobs_lock:
                histories[device_id].append((recognized, answer))
            metadata = {
                "ok": True,
                "job_id": job_id,
                "status": "done",
                "recognized_text": recognized,
                "answer": answer,
                "answer_chars": len(answer),
                "audio_bytes": audio_bytes,
                "sample_rate": PCM_SAMPLE_RATE,
                "elapsed_seconds": round(time.monotonic() - started, 2),
                "audio_stats": audio_stats,
                "created_at": int(time.time()),
            }
            (job_dir / "metadata.json").write_text(
                json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8"
            )
            write_job_state(
                job_dir,
                {
                    "ok": True,
                    "job_id": job_id,
                    "status": "done",
                    "recognized_text": recognized[:300],
                    "answer_chars": len(answer),
                    "audio_bytes": audio_bytes,
                    "audio_path": f"/v1/audio/{job_id}",
                },
            )
            print(f"\n[{device_id}] 你> {recognized}\n[{device_id}] AI> {answer}\n", flush=True)
        except Exception as error:
            detail = str(error)
            (job_dir / "error.txt").write_text(detail, encoding="utf-8")
            write_job_state(
                job_dir,
                {"ok": False, "job_id": job_id, "status": "error", "error": detail[:500]},
            )

    @app.get("/health")
    def health() -> dict:
        return {
            "ok": True,
            "service": "esp32-lan-dialogue",
            "asr": "zipformer-ctc-zh-int8-2025-07-03",
            "api_configured": bool(config.effective_api_key()),
            "model": config.model,
            "max_tokens": config.max_tokens,
        }

    @app.get("/config/public")
    def public_config(request: Request) -> dict:
        require_device(request)
        return config.public_dict()

    @app.get("/v1/settings")
    def read_settings(request: Request) -> dict:
        require_device(request)
        return {
            "ok": True,
            "api_url": config.api_url,
            "api_key_configured": bool(config.effective_api_key()),
            "model": config.model,
            "max_tokens": config.max_tokens,
            "thinking": config.thinking,
            "system_prompt": config.system_prompt,
            "voice": config.voice,
        }

    @app.put("/v1/settings")
    async def update_settings(request: Request) -> dict:
        require_device(request)
        try:
            payload = await request.json()
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise HTTPException(status_code=400, detail="invalid JSON settings") from error
        if not isinstance(payload, dict):
            raise HTTPException(status_code=400, detail="settings must be a JSON object")
        allowed = {
            "api_url", "api_key", "model", "max_tokens", "thinking", "system_prompt", "voice"
        }
        unknown = set(payload) - allowed
        if unknown:
            raise HTTPException(status_code=400, detail=f"unknown settings: {sorted(unknown)}")

        candidate = ServerConfig(**asdict(config))
        try:
            for field in ("api_url", "model", "system_prompt", "voice"):
                if field in payload:
                    if not isinstance(payload[field], str):
                        raise ValueError(f"{field} must be text")
                    setattr(candidate, field, payload[field].strip())
            if "max_tokens" in payload:
                if isinstance(payload["max_tokens"], bool):
                    raise ValueError("max_tokens must be an integer")
                candidate.max_tokens = int(payload["max_tokens"])
            if "thinking" in payload:
                if not isinstance(payload["thinking"], bool):
                    raise ValueError("thinking must be true or false")
                candidate.thinking = payload["thinking"]
            if "api_key" in payload:
                if not isinstance(payload["api_key"], str):
                    raise ValueError("api_key must be text")
                replacement_key = payload["api_key"].strip()
                if replacement_key:
                    candidate.api_key = replacement_key
            candidate.validate()
            with settings_lock:
                candidate.save(config_path)
                for field in config.__dataclass_fields__:
                    setattr(config, field, getattr(candidate, field))
        except (OSError, TypeError, ValueError) as error:
            raise HTTPException(status_code=400, detail=str(error)) from error
        return read_settings(request)

    @app.get("/device/{device_id}", response_class=HTMLResponse)
    def phone_page(device_id: str) -> HTMLResponse:
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", device_id):
            raise HTTPException(status_code=400, detail="invalid device id")
        page = """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <link rel="icon" href="data:,">
  <title>ESP32 AI 对话</title>
  <style>
    :root { color-scheme: light dark; font-family: system-ui, sans-serif; }
    body { margin: 0; background: #f4f6f8; color: #17202a; }
    header { position: sticky; top: 0; padding: 14px 16px; background: #123a63; color: white; }
    header h1 { margin: 0 0 4px; font-size: 19px; }
    #status { font-size: 13px; opacity: .9; }
    main { max-width: 760px; margin: auto; padding: 12px; }
    article { margin: 10px 0; padding: 12px; border-radius: 12px; background: white;
              box-shadow: 0 2px 9px #00000015; }
    .who { margin: 0 0 5px; color: #42617e; font-size: 12px; font-weight: 700; }
    .text { margin: 0; white-space: pre-wrap; overflow-wrap: anywhere; line-height: 1.55; }
    .answer { margin-top: 10px; padding-top: 10px; border-top: 1px solid #dfe5ea; }
    button { border: 0; border-radius: 8px; padding: 8px 12px; color: #123a63; }
    section { max-width: 760px; margin: 12px auto 0; padding: 0 12px; box-sizing: border-box; }
    details { padding: 12px; border-radius: 12px; background: white; box-shadow: 0 2px 9px #00000015; }
    summary { cursor: pointer; font-weight: 700; }
    form { display: grid; gap: 10px; margin-top: 12px; }
    label { display: grid; gap: 4px; font-size: 13px; }
    input, select, textarea { box-sizing: border-box; width: 100%; padding: 9px; border: 1px solid #aeb9c4;
                              border-radius: 7px; font: inherit; background: inherit; color: inherit; }
    .check { display: flex; align-items: center; gap: 8px; }
    .check input { width: auto; }
    .hint { margin: 5px 0 0; font-size: 12px; color: #60758a; }
    @media (prefers-color-scheme: dark) {
      body { background: #111820; color: #e8edf2; } article, details { background: #1c2732; }
      .answer { border-color: #3a4855; } .who { color: #91b9df; }
    }
  </style>
</head>
<body>
  <header><h1>ESP32 AI 对话记录</h1><span id="status">正在连接电脑服务端…</span>
    <button id="clear" type="button">清除记录</button></header>
  <section><details><summary>AI 预设与配置</summary>
    <form id="settings">
      <input type="text" autocomplete="username" value="esp32-device" hidden>
      <label>DeepSeek 预设模型<select id="model">
        <option value="deepseek-v4-flash">DeepSeek V4 Flash（快速）</option>
        <option value="deepseek-v4-pro">DeepSeek V4 Pro（更强）</option>
      </select></label>
      <label>API 地址（DeepSeek 官方固定）<input id="api-url" type="url" readonly required></label>
      <label>API Key（留空保持原值）<input id="api-key" type="password" autocomplete="new-password"></label>
      <p class="hint" id="key-status"></p>
      <label>最大输出 tokens<input id="max-tokens" type="number" min="128" max="384000" required></label>
      <label class="check"><input id="thinking" type="checkbox">启用思考模式</label>
      <label>系统提示词<textarea id="system-prompt" rows="4" maxlength="12000" required></textarea></label>
      <label>Windows TTS 声音<input id="voice" maxlength="200" required></label>
      <button type="submit">安全保存并立即生效</button><p class="hint" id="save-status"></p>
    </form>
  </details></section>
  <main id="turns"><article>还没有对话。按住 ESP32 的 BOOT 说话，松开后等待回答。</article></main>
  <script>
    const DEVICE_ID = __DEVICE_ID_JSON__;
    const TOKEN = decodeURIComponent(location.hash.replace(/^#token=/, ''));
    const headers = {'X-Device-ID': DEVICE_ID, 'X-Device-Token': TOKEN};
    const turns = document.getElementById('turns');
    const status = document.getElementById('status');
    const settingsForm = document.getElementById('settings');
    function paragraph(label, value, extraClass='') {
      const box = document.createElement('div');
      if (extraClass) box.className = extraClass;
      const who = document.createElement('p'); who.className = 'who'; who.textContent = label;
      const text = document.createElement('p'); text.className = 'text'; text.textContent = value;
      box.append(who, text); return box;
    }
    async function refresh() {
      try {
        const response = await fetch('/v1/history/' + encodeURIComponent(DEVICE_ID),
                                     {headers, cache: 'no-store'});
        if (!response.ok) throw new Error('HTTP ' + response.status);
        const data = await response.json(); turns.replaceChildren();
        if (!data.turns.length) {
          const empty = document.createElement('article');
          empty.textContent = '还没有对话。按住 ESP32 的 BOOT 说话，松开后等待回答。';
          turns.append(empty);
        }
        data.turns.forEach((turn) => {
          const card = document.createElement('article');
          card.append(paragraph('你（离线识别）', turn.user));
          card.append(paragraph('AI', turn.assistant, 'answer'));
          turns.append(card);
        });
        status.textContent = '已连接 · ' + new Date().toLocaleTimeString();
      } catch (error) { status.textContent = '读取失败：' + error.message; }
    }
    async function loadSettings() {
      const response = await fetch('/v1/settings', {headers, cache: 'no-store'});
      if (!response.ok) throw new Error('读取配置失败：HTTP ' + response.status);
      const data = await response.json();
      document.getElementById('api-url').value = data.api_url;
      document.getElementById('model').value = data.model;
      document.getElementById('max-tokens').value = data.max_tokens;
      document.getElementById('thinking').checked = data.thinking;
      document.getElementById('system-prompt').value = data.system_prompt;
      document.getElementById('voice').value = data.voice;
      document.getElementById('key-status').textContent =
        data.api_key_configured ? 'API Key 已配置，原值不会回显。' : 'API Key 尚未配置。';
    }
    settingsForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      const saveStatus = document.getElementById('save-status');
      saveStatus.textContent = '正在保存…';
      const payload = {
        api_url: document.getElementById('api-url').value.trim(),
        api_key: document.getElementById('api-key').value.trim(),
        model: document.getElementById('model').value,
        max_tokens: Number(document.getElementById('max-tokens').value),
        thinking: document.getElementById('thinking').checked,
        system_prompt: document.getElementById('system-prompt').value.trim(),
        voice: document.getElementById('voice').value.trim()
      };
      try {
        const response = await fetch('/v1/settings', {
          method: 'PUT', headers: {...headers, 'Content-Type': 'application/json'},
          body: JSON.stringify(payload)
        });
        const data = await response.json();
        if (!response.ok) throw new Error(data.detail || ('HTTP ' + response.status));
        document.getElementById('api-key').value = '';
        saveStatus.textContent = '已保存并立即生效。'; await loadSettings();
      } catch (error) { saveStatus.textContent = '保存失败：' + error.message; }
    });
    document.getElementById('clear').addEventListener('click', async () => {
      if (!confirm('清除这台设备在电脑内存中的对话记录？')) return;
      await fetch('/v1/history', {method: 'DELETE', headers}); await refresh();
    });
    refresh(); loadSettings().catch((error) => {
      document.getElementById('save-status').textContent = error.message;
    }); setInterval(refresh, 1500);
  </script>
</body></html>"""
        return HTMLResponse(page.replace("__DEVICE_ID_JSON__", json.dumps(device_id)))

    @app.get("/v1/history/{device_id}")
    def read_history(device_id: str, request: Request) -> dict:
        requested_by = require_device(request)
        if requested_by != device_id:
            raise HTTPException(status_code=403, detail="device id mismatch")
        with jobs_lock:
            turns = list(histories.get(device_id, ()))
        return {
            "ok": True,
            "device_id": device_id,
            "turns": [
                {"user": user_text, "assistant": assistant_text}
                for user_text, assistant_text in turns
            ],
        }

    @app.post("/v1/dialogue", status_code=202)
    async def dialogue(request: Request) -> dict:
        device_id = require_device(request)
        content_length = int(request.headers.get("content-length", "0") or 0)
        maximum = config.max_record_seconds * PCM_BYTES_PER_SECOND
        if content_length > maximum:
            raise HTTPException(status_code=413, detail="recording is too long")
        pcm = await request.body()
        if not pcm or len(pcm) & 1 or len(pcm) > maximum:
            raise HTTPException(status_code=400, detail="invalid 16-bit PCM body")
        sample_rate = int(request.headers.get("X-Sample-Rate", str(PCM_SAMPLE_RATE)))
        if sample_rate != PCM_SAMPLE_RATE:
            raise HTTPException(status_code=400, detail="sample rate must be 16000 Hz")

        job_id = uuid.uuid4().hex
        job_dir = JOBS_DIR / job_id
        job_dir.mkdir(parents=True, exist_ok=False)
        write_job_state(job_dir, {"ok": True, "job_id": job_id, "status": "queued"})
        threading.Thread(
            target=process_dialogue_job,
            args=(job_id, device_id, pcm),
            name=f"dialogue-{job_id[:8]}",
            daemon=True,
        ).start()
        return {
            "ok": True,
            "job_id": job_id,
            "status": "queued",
            "status_path": f"/v1/job/{job_id}",
        }

    @app.get("/v1/job/{job_id}")
    def job_status(job_id: str, request: Request) -> dict:
        require_device(request)
        if not re.fullmatch(r"[0-9a-f]{32}", job_id):
            raise HTTPException(status_code=400, detail="invalid job id")
        path = JOBS_DIR / job_id / "state.json"
        if not path.is_file():
            raise HTTPException(status_code=404, detail="job not found")
        try:
            with jobs_lock:
                return json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise HTTPException(status_code=503, detail="job state is updating") from error

    @app.get("/v1/audio/{job_id}")
    def audio(job_id: str, request: Request) -> FileResponse:
        require_device(request)
        if not re.fullmatch(r"[0-9a-f]{32}", job_id):
            raise HTTPException(status_code=400, detail="invalid job id")
        path = JOBS_DIR / job_id / "answer.pcm"
        if not path.is_file():
            raise HTTPException(status_code=404, detail="audio not found")
        return FileResponse(
            path,
            media_type="application/octet-stream",
            filename=f"{job_id}.pcm",
            headers={
                "X-Audio-Sample-Rate": str(PCM_SAMPLE_RATE),
                "X-Audio-Format": "s16le-mono",
                "Cache-Control": "no-store",
            },
        )

    @app.delete("/v1/history")
    def clear_history(request: Request) -> dict:
        device_id = require_device(request)
        histories.pop(device_id, None)
        return {"ok": True}

    return app


def cleanup_old_jobs(config: ServerConfig) -> None:
    cutoff = time.time() - config.job_ttl_hours * 3600
    if not JOBS_DIR.exists():
        return
    for path in JOBS_DIR.iterdir():
        if not path.is_dir() or path.stat().st_mtime >= cutoff:
            continue
        for child in path.iterdir():
            if child.is_file():
                child.unlink(missing_ok=True)
        try:
            path.rmdir()
        except OSError:
            pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ESP32-S3 LAN offline-ASR dialogue server")
    parser.add_argument("--config", type=Path, default=CONFIG_PATH)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--check", action="store_true", help="load config/model and exit")
    return parser.parse_args()


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    args = parse_args()
    config = ServerConfig.load(args.config)
    config.validate()
    recognizer = ZipformerRecognizer(Path(config.model_dir), args.threads)
    cleanup_old_jobs(config)
    if args.check:
        print(json.dumps({"ok": True, "config": config.public_dict()}, ensure_ascii=False, indent=2))
        return 0

    responder = DiscoveryResponder(config.port, config.discovery_port)
    responder.start()
    addresses = local_ipv4_addresses()
    print("ESP32 LAN dialogue server is ready:")
    for address in addresses or ["<PC-LAN-IP>"]:
        print(f"  http://{address}:{config.port}")
    print(f"UDP discovery port: {config.discovery_port}")
    print(f"DeepSeek API: {'configured' if config.effective_api_key() else 'MISSING'}")
    print("The terminal must remain open. Ctrl+C stops the server.\n")
    try:
        uvicorn.run(
            create_app(config, recognizer, args.config),
            host=config.bind_host,
            port=config.port,
            log_level="info",
            access_log=False,
        )
        return 0
    finally:
        responder.close()


if __name__ == "__main__":
    raise SystemExit(main())
