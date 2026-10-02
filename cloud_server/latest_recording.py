"""Opt-in, single-device diagnostics. Never retain a recording history."""
from __future__ import annotations

import hashlib
import json
import os
import threading
import wave
from datetime import datetime, timezone
from pathlib import Path


class LatestRecording:
    def __init__(self, data_dir: Path, device_id: str):
        self.directory = data_dir / "diagnostics"
        self.device_id = device_id
        self._lock = threading.Lock()

    def save(self, device_id: str, job_id: str, pcm: bytes) -> None:
        if not self.device_id or device_id != self.device_id or not pcm:
            return
        if len(pcm) % 2:
            raise ValueError("PCM must contain complete 16-bit samples")
        with self._lock:
            self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
            audio = self.directory / "latest.wav"
            metadata = self.directory / "latest.json"
            # Truncate the single slot; no backup or per-job audio is created.
            try:
                with os.fdopen(os.open(audio, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "wb") as file:
                    with wave.open(file, "wb") as wav:
                        wav.setnchannels(1)
                        wav.setsampwidth(2)
                        wav.setframerate(16_000)
                        wav.writeframes(pcm)
                record = {
                    "job_id": job_id,
                    "received_at": datetime.now(timezone.utc).isoformat(),
                    "pcm_bytes": len(pcm),
                    "pcm_sha256": hashlib.sha256(pcm).hexdigest(),
                    "sample_rate": 16_000,
                }
                with os.fdopen(os.open(metadata, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w", encoding="utf-8") as file:
                    json.dump(record, file)
            except OSError:
                audio.unlink(missing_ok=True)
                metadata.unlink(missing_ok=True)
                raise
