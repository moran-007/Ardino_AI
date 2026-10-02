from __future__ import annotations

import hashlib
import json
import os
import tempfile
import unittest
import wave
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import patch

from cloud_server.latest_recording import LatestRecording


class LatestRecordingTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.slot = LatestRecording(self.root, "authorized")

    def read_slot(self):
        directory = self.root / "diagnostics"
        self.assertEqual({p.name for p in directory.iterdir()}, {"latest.wav", "latest.json"})
        record = json.loads((directory / "latest.json").read_text())
        with wave.open(str(directory / "latest.wav"), "rb") as wav:
            self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, 16000))
            pcm = wav.readframes(wav.getnframes())
        self.assertEqual(record["pcm_bytes"], len(pcm))
        self.assertEqual(record["pcm_sha256"], hashlib.sha256(pcm).hexdigest())
        return record, pcm

    def test_disabled_and_other_devices_do_not_write(self):
        LatestRecording(self.root, "").save("authorized", "job", b"\x01\x00")
        self.slot.save("other", "job", b"\x01\x00")
        self.slot.save("authorized", "job", b"")
        self.assertFalse((self.root / "diagnostics").exists())

    def test_only_latest_clip_is_retained(self):
        for i in range(3):
            pcm = bytes([i, 0]) * (1600 + i)
            self.slot.save("authorized", str(i), pcm)
            record, actual = self.read_slot()
            self.assertEqual(record["job_id"], str(i))
            self.assertEqual(actual, pcm)

    def test_concurrent_writes_keep_audio_and_metadata_together(self):
        with ThreadPoolExecutor(max_workers=4) as executor:
            list(executor.map(lambda i: self.slot.save("authorized", str(i), bytes([i, 0]) * 100), range(10)))
        record, pcm = self.read_slot()
        self.assertEqual(pcm, bytes([int(record["job_id"]), 0]) * 100)

    def test_invalid_pcm_does_not_overwrite(self):
        self.slot.save("authorized", "valid", b"\x01\x00")
        with self.assertRaises(ValueError):
            self.slot.save("authorized", "invalid", b"\x01")
        self.assertEqual(self.read_slot()[0]["job_id"], "valid")

    def test_failed_save_does_not_leave_old_or_partial_recording(self):
        self.slot.save("authorized", "old", b"\x01\x00")
        with patch("cloud_server.latest_recording.wave.open", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                self.slot.save("authorized", "new", b"\x02\x00")
        self.assertEqual(list((self.root / "diagnostics").iterdir()), [])

    @unittest.skipIf(os.name == "nt", "POSIX file modes")
    def test_recordings_are_private(self):
        self.slot.save("authorized", "job", b"\x01\x00")
        directory = self.root / "diagnostics"
        self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
        for file in directory.iterdir():
            self.assertEqual(file.stat().st_mode & 0o777, 0o600)


if __name__ == "__main__":
    unittest.main()
