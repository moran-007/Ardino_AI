from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
import wave
from unittest.mock import patch

from fastapi.testclient import TestClient

SERVER_DIR = Path(__file__).resolve().parent
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

import lan_dialogue_server
from lan_dialogue_server import (
    PCM_SAMPLE_RATE,
    ServerConfig,
    create_app,
    wav_to_pcm,
    write_pcm_wav,
)


class FakeRecognizer:
    def recognize(self, pcm: bytes) -> str:
        return "测试问题"


class ServerComponentTests(unittest.TestCase):
    def test_config_rejects_excessive_tokens(self) -> None:
        config = ServerConfig(max_tokens=384001)
        with self.assertRaises(ValueError):
            config.validate()

    def test_async_dialogue_job_and_audio_download(self) -> None:
        answer = "这是一个用于测试流式播放的回答。" * 20
        answer_pcm = (b"\x20\x00\xe0\xff") * 1600

        def fake_tts(_text: str, wav_path: Path, _voice: str) -> None:
            write_pcm_wav(wav_path, answer_pcm)

        with tempfile.TemporaryDirectory() as directory:
            jobs_dir = Path(directory) / "jobs"
            config_path = Path(directory) / "server_config.local.json"
            config = ServerConfig(api_key="test-key", device_token="device-secret")
            with (
                patch.object(lan_dialogue_server, "JOBS_DIR", jobs_dir),
                patch.object(lan_dialogue_server, "call_deepseek", return_value=answer),
                patch.object(lan_dialogue_server, "run_windows_tts", side_effect=fake_tts),
            ):
                app = create_app(config, FakeRecognizer(), config_path)
                headers = {
                    "X-Device-ID": "unit-test-device",
                    "X-Device-Token": "device-secret",
                    "X-Sample-Rate": str(PCM_SAMPLE_RATE),
                }
                pcm = (b"\x10\x00\xf0\xff") * 1600
                with TestClient(app) as client:
                    self.assertEqual(client.get("/config/public").status_code, 401)
                    response = client.post("/v1/dialogue", headers=headers, content=pcm)
                    self.assertEqual(response.status_code, 202)
                    job_id = response.json()["job_id"]

                    deadline = time.monotonic() + 3
                    state = {"status": "queued"}
                    while time.monotonic() < deadline:
                        state_response = client.get(f"/v1/job/{job_id}", headers=headers)
                        self.assertEqual(state_response.status_code, 200)
                        state = state_response.json()
                        if state["status"] in {"done", "error"}:
                            break
                        time.sleep(0.02)

                    self.assertEqual(state["status"], "done", json.dumps(state, ensure_ascii=False))
                    self.assertEqual(state["recognized_text"], "测试问题")
                    self.assertEqual(state["answer_chars"], len(answer))
                    audio_response = client.get(f"/v1/audio/{job_id}", headers=headers)
                    self.assertEqual(audio_response.status_code, 200)
                    self.assertEqual(audio_response.content, answer_pcm)

                    history_response = client.get(
                        "/v1/history/unit-test-device", headers=headers
                    )
                    self.assertEqual(history_response.status_code, 200)
                    self.assertEqual(
                        history_response.json()["turns"],
                        [{"user": "测试问题", "assistant": answer}],
                    )
                    self.assertEqual(
                        client.get("/v1/history/another-device", headers=headers).status_code,
                        403,
                    )
                    page_response = client.get("/device/unit-test-device")
                    self.assertEqual(page_response.status_code, 200)
                    self.assertIn("ESP32 AI 对话记录", page_response.text)
                    self.assertNotIn("test-key", page_response.text)

                    settings_response = client.get("/v1/settings", headers=headers)
                    self.assertEqual(settings_response.status_code, 200)
                    self.assertTrue(settings_response.json()["api_key_configured"])
                    self.assertNotIn("api_key", settings_response.json())
                    update_response = client.put(
                        "/v1/settings",
                        headers=headers,
                        json={
                            "api_url": "https://api.deepseek.com/chat/completions",
                            "api_key": "",
                            "model": "deepseek-v4-pro",
                            "max_tokens": 8000,
                            "thinking": True,
                            "system_prompt": "请用中文回答。",
                            "voice": "Microsoft Huihui Desktop",
                        },
                    )
                    self.assertEqual(update_response.status_code, 200)
                    self.assertEqual(config.model, "deepseek-v4-pro")
                    saved = json.loads(config_path.read_text(encoding="utf-8"))
                    self.assertEqual(saved["api_key"], "test-key")
                    self.assertEqual(saved["max_tokens"], 8000)
                    self.assertEqual(
                        client.put(
                            "/v1/settings",
                            headers=headers,
                            json={"api_url": "http://insecure.example/v1"},
                        ).status_code,
                        400,
                    )
                    self.assertEqual(
                        client.put(
                            "/v1/settings",
                            headers=headers,
                            json={"api_url": "https://attacker.example/collect"},
                        ).status_code,
                        400,
                    )
                    self.assertEqual(
                        client.delete("/v1/history", headers=headers).status_code,
                        200,
                    )
                    self.assertEqual(
                        client.get("/v1/history/unit-test-device", headers=headers).json()["turns"],
                        [],
                    )

    def test_pcm_wav_round_trip(self) -> None:
        pcm = (b"\x10\x00\xf0\xff") * 800
        with tempfile.TemporaryDirectory() as directory:
            wav_path = Path(directory) / "sample.wav"
            pcm_path = Path(directory) / "sample.pcm"
            write_pcm_wav(wav_path, pcm)
            self.assertEqual(wav_to_pcm(wav_path, pcm_path), len(pcm))
            self.assertEqual(pcm_path.read_bytes(), pcm)
            with wave.open(str(wav_path), "rb") as source:
                self.assertEqual(source.getframerate(), PCM_SAMPLE_RATE)


if __name__ == "__main__":
    unittest.main()
