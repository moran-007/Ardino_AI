"""Local browser fixture; no device or cloud credentials are read."""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from cloud_server.admin_views import firmware_content, page_shell
from cloud_server.firmware import FirmwareRelease

state = dict(mic_gain=9, mic_sensitivity=72, noise_suppression=False, silence_ms=1700,
             listen_seconds=12, continuous_conversation=True, conversation_active=True,
             last_recording_rms=480, last_recording_peak=2400, last_recording_speech_ms=1860,
             last_recording_clipped_samples=0, last_recording_noise_suppression=True,
             busy=False, audio_ready=True, manual_recording_pending=False,
             manual_recording_active=False)
busy = False
dialogue = dict(reply_style='default', default_reply_style='normal', persona='', memory_enabled=False,
                memory_turns=4, web_search_enabled=False, web_search_available=True,
                system_prompt='你是一个简洁、友好的中文语音助手。回答适合直接朗读。',
                policies=['设备操作简短确认。', '联网搜索默认关闭。'], segment_min_chars=8, segment_max_chars=40)
releases = [FirmwareRelease(f"1.4.{n}", "esp32s3-cloud-voice-v1", 2280000, "ab"*32,
             "https://example.test", "", False, "2026-09-30T12:00:00+00:00",
             "收音增益、语音降噪与手动连续对话。" if n == 2 else "离线中文唤醒。") for n in (2, 1)]

class Handler(BaseHTTPRequestHandler):
    def send(self, code, body, kind="application/json"):
        if not isinstance(body, bytes):
            body = (json.dumps(body, ensure_ascii=False) if kind == "application/json" else body).encode()
        self.send_response(code)
        self.send_header("Content-Type", kind + "; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/status":
            return self.send(200, state)
        if self.path == '/ai/settings':
            return self.send(200, dialogue)
        if self.path == "/history":
            return self.send(200, page_shell(firmware_content(releases, releases[0], "fixture"),
                                            "固件版本", active="firmware", csrf="fixture"), "text/html")
        if self.path in ("/", "/capture"):
            return self.send(200, (Path(__file__).parent / "output/capture.html").read_bytes(), "text/html")
        self.send(404, "Not found", "text/plain")

    def do_POST(self):
        global busy
        data = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
        if self.path == '/ai/settings':
            if busy:
                return self.send(409, '请先结束当前对话。', 'text/plain')
            dialogue.update(data)
            return self.send(200, dialogue)
        if self.path == "/fixture/busy":
            busy = bool(data.get("busy"))
            state["busy"] = busy
            return self.send(200, {"ok": True})
        if self.path == "/voice/record/start":
            if busy:
                return self.send(409, "设备正在处理任务，请稍后开始收音。", "text/plain")
            busy = state["busy"] = state["manual_recording_active"] = True
            state["conversation_active"] = False
            return self.send(202, state)
        if self.path == "/voice/record/finish":
            if not state["manual_recording_active"]:
                return self.send(409, "当前没有网页手动录音。", "text/plain")
            busy = state["busy"] = state["manual_recording_active"] = False
            return self.send(200, state)
        if self.path == "/voice/settings":
            if busy:
                return self.send(409, "请先结束当前对话再保存。", "text/plain")
            state.update(data)
            return self.send(200, state)
        if self.path == "/voice/stop":
            busy = state["busy"] = state["manual_recording_active"] = False
            state["manual_recording_pending"] = False
            state["conversation_active"] = False
            return self.send(200, {"ok": True})
        self.send(404, "Not found", "text/plain")

    def log_message(self, *args):
        pass

if __name__ == "__main__":
    print("Voice UI fixture at http://127.0.0.1:18791", flush=True)
    ThreadingHTTPServer(("127.0.0.1", 18791), Handler).serve_forever()
