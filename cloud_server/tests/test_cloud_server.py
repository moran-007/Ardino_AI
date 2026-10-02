from __future__ import annotations

import tempfile
import base64
import re
import time
import unittest
from pathlib import Path
from urllib.parse import quote
from unittest.mock import patch

from fastapi.testclient import TestClient
from cryptography.fernet import Fernet

from cloud_server.admin import hash_admin_password
from cloud_server.app import create_app
from cloud_server.config import Settings
from cloud_server.providers import LLMConfig, MixedLanguageTTS, MockASR, MockLLM, MockTTS, OpenAICompatibleLLM, QwenDashScopeLLM, mixed_language_parts, sentence_segments
from cloud_server.service import VoiceService
from cloud_server.store import Store


class UnavailableLLM:
    name = "unavailable"

    def stream(self, question: str, system_prompt: str):
        raise RuntimeError("provider unavailable")
        yield ""


class FakeStreamResponse:
    def __init__(self, lines: list[str]):
        self.lines = lines

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def raise_for_status(self) -> None:
        return None

    def iter_lines(self):
        yield from self.lines


class CapturingLLM:
    name = "capture"

    def __init__(self):
        self.calls = []

    def stream(self, question: str, system_prompt: str):
        self.calls.append((question, system_prompt))
        yield "这是设备专属回答。"


class CapturingTTS(MockTTS):
    def __init__(self):
        self.calls = []

    def synthesize(self, text: str, **kwargs) -> bytes:
        self.calls.append((text, kwargs))
        return super().synthesize(text, **kwargs)


class FakeLanguageTTS:
    sample_rate = 16_000
    speed = 1.0

    def __init__(self, marker: bytes):
        self.marker = marker
        self.calls = []

    def synthesize(self, text: str, *args, **kwargs) -> bytes:
        self.calls.append((text, args, kwargs))
        return self.marker * 16


class CloudServerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.settings = Settings.for_test(Path(self.temp.name))
        self.store = Store(self.settings.database_path, self.settings.auth_pepper)
        self.token = self.store.register_device("pc-simulator-01", "test simulator", "secret-token")
        self.service = VoiceService(self.settings, self.store, MockASR(), [MockLLM()], MockTTS())
        self.client = TestClient(create_app(self.settings, self.service))
        simulated_text = base64.urlsafe_b64encode("测试分段流式输出".encode("utf-8")).decode("ascii")
        self.headers = {"X-Device-ID": "pc-simulator-01", "Authorization": f"Bearer {self.token}", "X-Simulated-Text-B64": simulated_text}

    def tearDown(self) -> None:
        self.client.close()
        self.temp.cleanup()

    def submit_and_wait(self) -> tuple[str, dict]:
        response = self.client.post("/voice-api/v1/dialogue", headers=self.headers, content=bytes(3200))
        self.assertEqual(response.status_code, 202, response.text)
        job_id = response.json()["job_id"]
        for _ in range(100):
            payload = self.client.get(f"/voice-api/v1/jobs/{job_id}/segments?after=-1", headers=self.headers).json()
            if payload["status"] in {"completed", "failed"}:
                return job_id, payload
            time.sleep(0.02)
        self.fail("job timeout")

    def test_health_and_authentication(self) -> None:
        self.assertTrue(self.client.get("/voice-api/v1/health").json()["ok"])
        response = self.client.post("/voice-api/v1/dialogue", content=bytes(3200))
        self.assertEqual(response.status_code, 401)

    def test_sentence_segments_and_audio(self) -> None:
        job_id, payload = self.submit_and_wait()
        self.assertEqual(payload["status"], "completed", payload)
        self.assertGreaterEqual(len(payload["segments"]), 2)
        job = self.client.get(f"/voice-api/v1/jobs/{job_id}", headers=self.headers).json()
        self.assertEqual(job["recognized_text"], job["question"])
        self.assertEqual(job["answer_text"], job["answer"])
        total = 0
        for expected, segment in enumerate(payload["segments"]):
            self.assertEqual(segment["index"], expected)
            audio = self.client.get(segment["audio_url"], headers=self.headers)
            self.assertEqual(audio.status_code, 200)
            self.assertEqual(len(audio.content), segment["byte_count"])
            total += len(audio.content)
        full = self.client.get(f"/voice-api/v1/jobs/{job_id}/audio", headers=self.headers)
        self.assertEqual(full.status_code, 200)
        self.assertEqual(len(full.content), total)

    def test_device_cannot_read_another_devices_job(self) -> None:
        job_id, _ = self.submit_and_wait()
        other = self.store.register_device("pc-simulator-02", "other", "other-token")
        response = self.client.get(f"/voice-api/v1/jobs/{job_id}", headers={"X-Device-ID": "pc-simulator-02", "Authorization": f"Bearer {other}"})
        self.assertEqual(response.status_code, 404)

    def test_device_memory_and_persona_are_isolated(self) -> None:
        device_a = "memory-device-a"
        device_b = "memory-device-b"
        self.store.register_device(device_a, "用户甲", "token-a")
        self.store.register_device(device_b, "用户乙", "token-b")
        self.store.update_device_profile(device_a, "称呼用户为小明，喜欢恐龙。", True, 4)
        self.store.update_device_profile(device_b, "称呼用户为小红，喜欢绘画。", True, 4)
        self.store.add_memory(device_a, "我喜欢什么？", "你喜欢恐龙。")
        self.store.add_memory(device_b, "我喜欢什么？", "你喜欢绘画。")

        llm = CapturingLLM()
        service = VoiceService(self.settings, self.store, MockASR(), [llm], MockTTS())
        job_id = service.submit_text(device_a, "还记得我吗？")
        for _ in range(100):
            job = self.store.get_job(job_id, device_a)
            if job["status"] in {"completed", "failed"}:
                break
            time.sleep(0.02)
        self.assertEqual(job["status"], "completed", job)
        question, system_prompt = llm.calls[0]
        self.assertIn("你喜欢恐龙", question)
        self.assertNotIn("你喜欢绘画", question)
        self.assertIn("小明", system_prompt)
        self.assertIn("本地解析并执行设置、取消和查询闹钟、计时器、便签或待办", system_prompt)
        self.assertEqual(len(self.store.list_memories(device_a)), 2)
        self.assertEqual(len(self.store.list_memories(device_b)), 1)
        self.assertEqual(self.store.clear_memories(device_a), 2)
        self.assertEqual(self.store.list_memories(device_a), [])
        self.assertEqual(len(self.store.list_memories(device_b)), 1)

        self.store.set_device_enabled(device_a, False)
        self.assertFalse(self.store.authenticate(device_a, "token-a"))
        self.store.set_device_enabled(device_a, True)
        self.assertTrue(self.store.authenticate(device_a, "token-a"))

    def test_device_context_reaches_llm(self) -> None:
        llm = CapturingLLM()
        service = VoiceService(self.settings, self.store, MockASR(), [llm], MockTTS())
        client = TestClient(create_app(self.settings, service))
        headers = dict(self.headers)
        weather = "地点：上海；当前：晴 25℃；三天预报：今天 晴 20~28℃，明天 多云 21~27℃"
        headers["X-Device-Weather"] = quote(weather, safe="")
        memos = "便签 1 条，待办 1 条（未完成 1 条）；便签：1、买电池；待办：1、未完成 检查接线"
        headers["X-Device-Memos"] = quote(memos, safe="")
        try:
            response = client.post("/voice-api/v1/dialogue", headers=headers, content=bytes(3200))
            self.assertEqual(response.status_code, 202, response.text)
            payload = response.json()
            self.assertEqual(
                payload["context"], {"schema": 1, "weather": True, "memos": True}
            )
            job_id = payload["job_id"]
            for _ in range(100):
                job = self.store.get_job(job_id, "pc-simulator-01")
                if job["status"] in {"completed", "failed"}:
                    break
                time.sleep(0.02)
            self.assertEqual(job["status"], "completed", job)
            self.assertEqual(len(llm.calls), 1)
            question, system_prompt = llm.calls[0]
            self.assertIn(weather, question)
            self.assertIn("只根据这份设备天气数据", system_prompt)
            self.assertIn("不得回答‘无法查询实时天气’", system_prompt)
            self.assertIn(memos, question)
            self.assertIn("根据这份快照简洁列出真实内容", system_prompt)
            self.assertNotIn(weather, system_prompt)
            self.assertNotIn(memos, system_prompt)
        finally:
            client.close()

    def test_device_context_rejects_invalid_or_oversized_utf8(self) -> None:
        invalid = dict(self.headers)
        invalid["X-Device-Memos"] = "%FF"
        response = self.client.post(
            "/voice-api/v1/dialogue", headers=invalid, content=bytes(3200)
        )
        self.assertEqual(response.status_code, 400, response.text)

        oversized = dict(self.headers)
        oversized["X-Device-Weather"] = quote("晴" * 200, safe="")
        response = self.client.post(
            "/voice-api/v1/dialogue", headers=oversized, content=bytes(3200)
        )
        self.assertEqual(response.status_code, 400, response.text)

    def test_device_voice_settings_are_authenticated_and_isolated(self) -> None:
        other_token = self.store.register_device("voice-device-02", "other", "voice-token-02")
        response = self.client.put(
            "/voice-api/v1/device-settings",
            headers=self.headers,
            json={"chinese_speaker_id": 3, "english_speaker_id": 527, "speed": 0.85},
        )
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json()["chinese_speaker_id"], 3)
        self.assertEqual(response.json()["english_speaker_id"], 527)
        self.assertEqual(response.json()["speed"], 0.85)
        other = self.client.get(
            "/voice-api/v1/device-settings",
            headers={"X-Device-ID": "voice-device-02", "Authorization": f"Bearer {other_token}"},
        )
        self.assertEqual(other.status_code, 200, other.text)
        self.assertEqual(other.json()["chinese_speaker_id"], 0)
        self.assertEqual(other.json()["english_speaker_id"], 0)
        self.assertEqual(other.json()["speed"], 1.0)
        invalid = self.client.put(
            "/voice-api/v1/device-settings",
            headers=self.headers,
            json={"chinese_speaker_id": 5, "english_speaker_id": 904, "speed": 0.2},
        )
        self.assertEqual(invalid.status_code, 422)

    def test_voice_service_passes_device_voice_settings_to_tts(self) -> None:
        self.store.update_device_voice("pc-simulator-01", 4, 321, 1.25)
        tts = CapturingTTS()
        service = VoiceService(self.settings, self.store, MockASR(), [CapturingLLM()], tts)
        job_id = service.submit_text("pc-simulator-01", "测试设备语音设置")
        for _ in range(100):
            job = self.store.get_job(job_id, "pc-simulator-01")
            if job["status"] in {"completed", "failed"}:
                break
            time.sleep(0.02)
        self.assertEqual(job["status"], "completed", job)
        self.assertTrue(tts.calls)
        self.assertEqual(tts.calls[0][1]["speaker_id"], 4)
        self.assertEqual(tts.calls[0][1]["english_speaker_id"], 321)
        self.assertEqual(tts.calls[0][1]["speed"], 1.25)

    def test_mixed_language_tts_routes_english_without_dropping_it(self) -> None:
        self.assertEqual(
            mixed_language_parts("你好，welcome to ESP32 voice assistant，版本 three point five。"),
            [
                ("zh", "你好，"),
                ("en", "welcome to ESP32 voice assistant"),
                ("zh", "，版本 "),
                ("en", "three point five。"),
            ],
        )
        chinese = FakeLanguageTTS(b"Z")
        english = FakeLanguageTTS(b"E")
        tts = MixedLanguageTTS(chinese, english)
        audio = tts.synthesize(
            "你好，welcome to ESP32 voice assistant。",
            speaker_id=2,
            english_speaker_id=456,
            speed=0.9,
        )
        self.assertGreater(len(audio), 64)
        self.assertEqual(chinese.calls[0][2]["speaker_id"], 2)
        self.assertEqual(english.calls[0][1], (456, 0.9))

    def test_mixed_language_tts_routes_standalone_numbers_to_chinese(self) -> None:
        self.assertEqual(
            mixed_language_parts("数字 0、1 和 3.14，设备 ESP32。"),
            [("zh", "数字 0、1 和 3.14，设备 "), ("en", "ESP32。")],
        )
        chinese = FakeLanguageTTS(b"Z")
        english = FakeLanguageTTS(b"E")
        tts = MixedLanguageTTS(chinese, english)

        tts.synthesize("0、1、3.14。", speaker_id=2, english_speaker_id=456)
        tts.synthesize("日期 2026-08-30。", speaker_id=2, english_speaker_id=456)

        self.assertEqual(
            [call[0] for call in chinese.calls],
            ["0、1、3.14。", "日期 2026年08月30日。"],
        )
        self.assertEqual(english.calls, [])

    def test_mixed_language_tts_speaks_basic_arithmetic_in_chinese(self) -> None:
        chinese = FakeLanguageTTS(b"Z")
        english = FakeLanguageTTS(b"E")
        tts = MixedLanguageTTS(chinese, english)

        tts.synthesize(
            "1 + 2 = 3；8-3=5；4*2=8；8/2=4；6×2=12；12÷3=4。",
            speaker_id=2,
            english_speaker_id=456,
        )

        self.assertEqual(
            [call[0] for call in chinese.calls],
            ["1加2等于3；8减3等于5；4乘以2等于8；8除以2等于4；6乘以2等于12；12除以3等于4。"],
        )
        self.assertEqual(english.calls, [])

    def test_mixed_language_tts_does_not_treat_phone_numbers_or_ranges_as_subtraction(self) -> None:
        chinese = FakeLanguageTTS(b"Z")
        english = FakeLanguageTTS(b"E")
        tts = MixedLanguageTTS(chinese, english)

        tts.synthesize(
            "客服电话 400-123-4567，适合 8-10 岁。",
            speaker_id=2,
            english_speaker_id=456,
        )

        self.assertEqual(
            [call[0] for call in chinese.calls],
            ["客服电话 400-123-4567，适合 8-10 岁。"],
        )
        self.assertEqual(english.calls, [])

    def test_mixed_language_tts_normalizes_common_spoken_domains(self) -> None:
        chinese = FakeLanguageTTS(b"Z")
        english = FakeLanguageTTS(b"E")
        tts = MixedLanguageTTS(chinese, english)

        tts.synthesize(
            "价格 ￥12.5，浓度 50%，温度 25°C，距离 3.2km，频率 16kHz，"
            "电压 5V，水 H2O，二氧化碳 CO2。",
            speaker_id=2,
            english_speaker_id=456,
        )

        self.assertEqual(
            [call[0] for call in chinese.calls],
            [
                "价格 12.5元，浓度 百分之50，温度 25摄氏度，距离 3.2千米，"
                "频率 16千赫兹，电压 5伏特，水 水，二氧化碳 二氧化碳。"
            ],
        )
        self.assertEqual(english.calls, [])

    def test_rejects_malformed_pcm(self) -> None:
        response = self.client.post("/voice-api/v1/dialogue", headers=self.headers, content=b"x")
        self.assertEqual(response.status_code, 413)

    def test_translation_uses_configured_provider(self) -> None:
        response = self.client.post("/voice-api/v1/translate", headers=self.headers, json={"text": "hello", "target_language": "简体中文"})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json()["provider"], "mock")
        self.assertTrue(response.json()["translation"])

    def test_text_dialogue_uses_text_without_audio_upload(self) -> None:
        response = self.client.post(
            "/voice-api/v1/text-dialogue",
            headers=self.headers,
            json={"text": "电脑文字提问"},
        )
        self.assertEqual(response.status_code, 202, response.text)
        job_id = response.json()["job_id"]
        for _ in range(100):
            job = self.client.get(f"/voice-api/v1/jobs/{job_id}", headers=self.headers).json()
            if job["status"] in {"completed", "failed"}:
                break
            time.sleep(0.02)
        self.assertEqual(job["status"], "completed", job)
        self.assertEqual(job["question"], "电脑文字提问")
        empty = self.client.post(
            "/voice-api/v1/text-dialogue",
            headers=self.headers,
            json={"text": "   "},
        )
        self.assertEqual(empty.status_code, 422, empty.text)

    def test_sentence_segments_keep_tts_chunks_short_and_natural(self) -> None:
        story = (
            "从前，有一只小蜗牛，它总觉得自己的壳又重又难看。"
            "它羡慕小鸟能飞，羡慕兔子能跳。"
            "有一天，它问妈妈：“为什么我们要背着这么重的壳呢？”"
            "妈妈笑着说：“因为我们要靠自己保护自己呀。”"
        )
        chunks = (story[start : start + 7] for start in range(0, len(story), 7))
        segments = list(sentence_segments(chunks, minimum=8, maximum=40))
        self.assertEqual("".join(segments), story)
        self.assertTrue(all(len(item) <= 40 for item in segments), segments)
        self.assertGreaterEqual(len(segments), 4, segments)

    def test_short_leading_sentence_does_not_block_later_boundaries(self) -> None:
        text = "好的。接下来我会用一个较完整的句子说明具体原因。然后给出处理建议。"
        segments = list(sentence_segments(iter([text]), minimum=8, maximum=40))
        self.assertEqual("".join(segments), text)
        self.assertGreaterEqual(len(segments), 2, segments)

    def test_llm_falls_back_before_stream_starts(self) -> None:
        fallback_service = VoiceService(self.settings, self.store, MockASR(), [UnavailableLLM(), MockLLM()], MockTTS())
        client = TestClient(create_app(self.settings, fallback_service))
        try:
            response = client.post("/voice-api/v1/translate", headers=self.headers, json={"text": "hello", "target_language": "简体中文"})
            self.assertEqual(response.status_code, 200, response.text)
            self.assertEqual(response.json()["provider"], "mock")
        finally:
            client.close()

    def test_deepseek_v4_uses_direct_api_and_disables_thinking(self) -> None:
        captured = {}

        def fake_stream(method, url, **kwargs):
            captured.update({"method": method, "url": url, **kwargs})
            return FakeStreamResponse(['data: {"choices":[{"delta":{"content":"你好"}}]}', "data: [DONE]"])

        provider = OpenAICompatibleLLM(
            LLMConfig("deepseek", "secret", "https://api.deepseek.com", "deepseek-v4-flash")
        )
        with patch("cloud_server.providers.httpx.stream", side_effect=fake_stream):
            self.assertEqual(list(provider.stream("测试", "简洁回答")), ["你好"])
        self.assertEqual(captured["url"], "https://api.deepseek.com/chat/completions")
        self.assertEqual(captured["json"]["thinking"], {"type": "disabled"})

    def test_qwen_native_dashscope_stream_is_incremental(self) -> None:
        captured = {}

        def fake_stream(method, url, **kwargs):
            captured.update({"method": method, "url": url, **kwargs})
            return FakeStreamResponse([
                "event:result",
                'data:{"output":{"choices":[{"message":{"content":"第一段"}}]}}',
                'data:{"output":{"choices":[{"message":{"content":"第二段"}}]}}',
            ])

        url = "https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation"
        provider = QwenDashScopeLLM(LLMConfig("qwen", "secret", url, "qwen-plus"))
        with patch("cloud_server.providers.httpx.stream", side_effect=fake_stream):
            self.assertEqual(list(provider.stream("测试", "简洁回答")), ["第一段", "第二段"])
        self.assertEqual(captured["url"], url)
        self.assertEqual(captured["headers"]["X-DashScope-SSE"], "enable")
        self.assertTrue(captured["json"]["parameters"]["incremental_output"])
        self.assertFalse(captured["json"]["parameters"]["enable_thinking"])

    def test_password_only_admin_saves_encrypted_config(self) -> None:
        settings = Settings.for_test(
            Path(self.temp.name),
            admin_password_hash=hash_admin_password("test-admin-password"),
            config_encryption_key=Fernet.generate_key().decode("ascii"),
        )
        client = TestClient(create_app(settings, self.service))
        try:
            locked = client.get("/admin/")
            self.assertIn("管理员密码", locked.text)
            denied = client.post("/admin/unlock", data={"password": "wrong-password"})
            self.assertIn("密码错误", denied.text)
            unlocked = client.post("/admin/unlock", data={"password": "test-admin-password"}, follow_redirects=True)
            self.assertEqual(unlocked.status_code, 200)
            self.assertIn("Token 用量", unlocked.text)
            settings_page = client.get("/admin/settings").text
            self.assertIn("<details", settings_page)
            self.assertIn("恢复推荐默认配置", settings_page)
            key_page = client.get("/admin/api-keys").text
            self.assertIn('list="deepseek-model-options"', key_page)
            self.assertIn('list="qwen-model-options"', key_page)
            self.assertIn('list="glm-model-options"', key_page)
            devices_page = client.get("/admin/devices").text
            self.assertIn("创建设备凭证", devices_page)
            self.assertIn('name="memory_turns"', devices_page)
            self.assertIn('name="tts_english_speaker_id"', devices_page)
            self.assertIn('value="clear_memory"', devices_page)
            self.assertIn('value="disable"', devices_page)
            match = re.search(r'name="csrf" value="([^"]+)"', unlocked.text)
            self.assertIsNotNone(match)
            saved = client.post(
                "/admin/config",
                data={
                    "csrf": match.group(1),
                    "voice_api_enabled": "on",
                    "translation_api_enabled": "on",
                    "deepseek_enabled": "on",
                    "deepseek_api_key": "secret-deepseek-key",
                    "deepseek_base_url": "https://api.deepseek.com",
                    "deepseek_model": "deepseek-chat",
                    "qwen_base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
                    "qwen_model": "qwen-plus",
                    "glm_base_url": "https://open.bigmodel.cn/api/paas/v4",
                    "glm_model": "glm-4-flash",
                    "asr_provider": "mock",
                    "tts_provider": "mock",
                    "sherpa_tts_speaker_id": "2",
                    "sherpa_tts_speed": "1.1",
                    "segment_min_chars": "9",
                    "segment_max_chars": "36",
                    "new_admin_password": "changed-admin-password",
                    "confirm_admin_password": "changed-admin-password",
                },
                follow_redirects=True,
            )
            self.assertEqual(saved.status_code, 200, saved.text)
            encrypted = (Path(self.temp.name) / "admin_config.enc").read_bytes()
            self.assertNotIn(b"secret-deepseek-key", encrypted)
            self.assertIn("已配置", saved.text)
            self.assertEqual(client.app.state.effective_settings.segment_min_chars, 9)
            self.assertEqual(client.app.state.effective_settings.segment_max_chars, 36)
            next_csrf = re.search(r'name="csrf" value="([^"]+)"', saved.text).group(1)
            client.post("/admin/lock", data={"csrf": next_csrf}, follow_redirects=True)
            old_password = client.post("/admin/unlock", data={"password": "test-admin-password"})
            self.assertIn("密码错误", old_password.text)
            new_password = client.post("/admin/unlock", data={"password": "changed-admin-password"}, follow_redirects=True)
            self.assertIn("保存并重载", client.get("/admin/settings").text)
            reset_csrf = re.search(r'name="csrf" value="([^"]+)"', new_password.text).group(1)
            reset = client.post(
                "/admin/reset-recommended",
                data={"csrf": reset_csrf, "confirm_reset": "on"},
                follow_redirects=True,
            )
            self.assertEqual(reset.status_code, 200, reset.text)
            self.assertEqual(client.app.state.runtime_config["deepseek_api_key"], "secret-deepseek-key")
            self.assertEqual(client.app.state.effective_settings.segment_min_chars, 8)
            self.assertEqual(client.app.state.effective_settings.segment_max_chars, 40)
        finally:
            client.close()

    def test_admin_creates_device_token_once(self) -> None:
        settings = Settings.for_test(
            Path(self.temp.name),
            admin_password_hash=hash_admin_password("test-admin-password"),
            config_encryption_key=Fernet.generate_key().decode("ascii"),
        )
        client = TestClient(create_app(settings, self.service))
        try:
            unlocked = client.post("/admin/unlock", data={"password": "test-admin-password"}, follow_redirects=True)
            csrf = re.search(r'name="csrf" value="([^"]+)"', unlocked.text).group(1)
            created = client.post(
                "/admin/devices",
                data={
                    "csrf": csrf,
                    "device_id": "desktop-text-client",
                    "device_name": "电脑文字测试",
                    "persona": "称呼用户为小蓝。",
                    "memory_enabled": "on",
                    "memory_turns": "3",
                    "tts_speaker_id": "2",
                    "tts_english_speaker_id": "128",
                    "tts_speed": "0.9",
                },
            )
            self.assertEqual(created.status_code, 200, created.text)
            token = re.search(r'id="new-device-token">([^<]+)<', created.text).group(1)
            self.assertTrue(self.store.authenticate("desktop-text-client", token))
            device = self.store.get_device("desktop-text-client")
            self.assertEqual(device["persona"], "称呼用户为小蓝。")
            self.assertEqual(device["memory_turns"], 3)
            self.assertTrue(device["memory_enabled"])
            self.assertEqual(device["tts_speaker_id"], 2)
            self.assertEqual(device["tts_english_speaker_id"], 128)
            self.assertEqual(device["tts_speed"], 0.9)
            home = client.get("/admin/")
            self.assertIn("desktop-text-client", home.text)
            self.assertNotIn(token, home.text)

            disabled = client.post(
                "/admin/device-action",
                data={"csrf": csrf, "device_id": "desktop-text-client", "action": "disable"},
                follow_redirects=True,
            )
            self.assertEqual(disabled.status_code, 200, disabled.text)
            self.assertFalse(self.store.authenticate("desktop-text-client", token))
            client.post(
                "/admin/device-action",
                data={"csrf": csrf, "device_id": "desktop-text-client", "action": "enable"},
            )
            self.assertTrue(self.store.authenticate("desktop-text-client", token))
            self.store.add_memory("desktop-text-client", "旧问题", "旧回答")
            cleared = client.post(
                "/admin/device-action",
                data={
                    "csrf": csrf,
                    "device_id": "desktop-text-client",
                    "action": "clear_memory",
                    "confirm_clear": "on",
                },
                follow_redirects=True,
            )
            self.assertEqual(cleared.status_code, 200, cleared.text)
            self.assertEqual(self.store.list_memories("desktop-text-client"), [])

            reset = client.post("/admin/reset-recommended", data={"csrf": csrf, "confirm_reset": "on"}, follow_redirects=True)
            self.assertEqual(reset.status_code, 200, reset.text)
            self.assertEqual(reset.url.path, "/admin/")
        finally:
            client.close()


if __name__ == "__main__":
    unittest.main()
