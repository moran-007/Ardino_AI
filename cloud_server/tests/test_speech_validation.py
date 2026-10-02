from __future__ import annotations

import tempfile
import time
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

from cloud_server.config import Settings
from cloud_server.service import VoiceService
from cloud_server.store import Store


class FixedASR:
    def __init__(self):
        self.text = ""
        self.calls = 0

    def recognize(self, pcm: bytes, sample_rate: int) -> str:
        self.calls += 1
        return self.text


class RecordingLLM:
    name = "test"

    def __init__(self):
        self.questions = []

    def stream(self, question: str, system_prompt: str):
        self.questions.append(question)
        yield "This is a valid response."


class RecordingTTS:
    sample_rate = 16_000

    def __init__(self):
        self.texts = []

    def synthesize(self, text: str, **kwargs) -> bytes:
        self.texts.append(text)
        return bytes(320)


class SpeechValidationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.settings = Settings.for_test(Path(self.temp.name))
        self.store = Store(self.settings.database_path, self.settings.auth_pepper)
        self.device_id = "speech-validation"
        self.store.register_device(self.device_id, "test", "test-token")
        self.store.update_device_profile(self.device_id, "", True)
        self.asr = FixedASR()
        self.llm = RecordingLLM()
        self.tts = RecordingTTS()
        self.service = VoiceService(
            self.settings, self.store, self.asr, [self.llm], self.tts
        )

    def wait_for_job(self, job_id: str) -> dict:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            job = self.store.get_job(job_id, self.device_id)
            if job["status"] in {"completed", "failed"} and not self.service.active_jobs:
                return job
            time.sleep(0.01)
        self.fail("voice job did not finish")

    def assert_rejected_asr(self, text: str) -> None:
        self.asr.text = text
        job_id = self.service.submit(self.device_id, bytes(3200))
        job = self.wait_for_job(job_id)
        self.assertEqual(job["status"], "failed", job)
        self.assertEqual(job["error"], "\u672a\u8bc6\u522b\u5230\u6709\u6548\u8bed\u97f3")
        self.assertEqual(job["question"], "")
        self.assertEqual(job["answer"], "")
        self.assertEqual(self.llm.questions, [])
        self.assertEqual(self.tts.texts, [])
        self.assertEqual(self.store.list_segments(job_id), [])
        self.assertEqual(self.store.list_memories(self.device_id), [])
        self.assertFalse((self.settings.data_dir / "jobs" / job_id).exists())

    def test_blank_asr_has_no_downstream_effects(self) -> None:
        for text in ("", " \t\n\u3000"):
            with self.subTest(text=text):
                self.assert_rejected_asr(text)

    def test_punctuation_and_symbols_asr_have_no_downstream_effects(self) -> None:
        for text in (".", "\u3002", "... ?!\u2026", "$+=", "\U0001f642"):
            with self.subTest(text=text):
                self.assert_rejected_asr(text)

    def test_short_words_and_unicode_text_are_valid_speech(self) -> None:
        for text in ("I.", "The.", "So.", "Yes", "No", "1", "\u597d", "\u00e9", "\u0661"):
            with self.subTest(text=text):
                self.store.clear_memories(self.device_id)
                self.asr.text = text
                job_id = self.service.submit(self.device_id, bytes(3200))
                job = self.wait_for_job(job_id)
                self.assertEqual(job["status"], "completed", job)
                self.assertEqual(job["question"], text)
                self.assertEqual(self.llm.questions[-1], text)
                self.assertTrue(self.store.list_segments(job_id))
                self.assertEqual(self.store.list_memories(self.device_id)[0]["question"], text)

    def test_asr_whitespace_is_trimmed(self) -> None:
        self.asr.text = " \u4f60\u597d\u3002\n"
        job_id = self.service.submit(self.device_id, bytes(3200))
        job = self.wait_for_job(job_id)
        self.assertEqual(job["status"], "completed", job)
        self.assertEqual(job["question"], "\u4f60\u597d\u3002")

    def test_explicit_symbol_text_bypasses_speech_validation(self) -> None:
        for simulated in (False, True):
            with self.subTest(simulated=simulated):
                self.store.clear_memories(self.device_id)
                if simulated:
                    job_id = self.service.submit(self.device_id, bytes(3200), " ?! ")
                else:
                    job_id = self.service.submit_text(self.device_id, " ?! ")
                job = self.wait_for_job(job_id)
                self.assertEqual(job["status"], "completed", job)
                self.assertEqual(job["question"], "?!")
                self.assertEqual(self.llm.questions[-1], "?!")
                self.assertEqual(self.asr.calls, 0)

    def test_diagnostic_capture_matches_the_submitted_job(self) -> None:
        settings = replace(self.settings, diagnostic_device_id=self.device_id)
        self.service = VoiceService(settings, self.store, self.asr, [self.llm], self.tts)
        self.asr.text = "test speech"
        with patch.object(self.service.latest_recording, "save") as save:
            pcm = bytes(3200)
            job_id = self.service.submit(self.device_id, pcm)
            self.assertEqual(self.wait_for_job(job_id)["status"], "completed")
            save.assert_called_once_with(self.device_id, job_id, pcm)

    def test_diagnostic_failure_does_not_fail_voice_job(self) -> None:
        self.asr.text = "test speech"
        with patch.object(self.service.latest_recording, "save", side_effect=OSError("disk full")):
            with self.assertLogs("cloud_server.service", level="WARNING"):
                job_id = self.service.submit(self.device_id, bytes(3200))
            self.assertEqual(self.wait_for_job(job_id)["status"], "completed")

    def test_text_and_simulated_requests_do_not_save_diagnostic_audio(self) -> None:
        with patch.object(self.service.latest_recording, "save") as save:
            job_id = self.service.submit_text(self.device_id, "text only")
            self.assertEqual(self.wait_for_job(job_id)["status"], "completed")
            job_id = self.service.submit(self.device_id, bytes(3200), "simulated")
            self.assertEqual(self.wait_for_job(job_id)["status"], "completed")
            save.assert_not_called()


if __name__ == "__main__":
    unittest.main()
