from __future__ import annotations

import hashlib
import hmac
import secrets
import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


class ClosingConnection(sqlite3.Connection):
    """Commit/rollback like sqlite3's context manager, then release Windows locks."""

    def __exit__(self, exc_type, exc_value, traceback):
        try:
            return super().__exit__(exc_type, exc_value, traceback)
        finally:
            self.close()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


class Store:
    def __init__(self, path: Path, pepper: str):
        self.path = path
        self.pepper = pepper.encode("utf-8")
        path.parent.mkdir(parents=True, exist_ok=True)
        self._initialize()

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.path, timeout=10, factory=ClosingConnection)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys=ON")
        connection.execute("PRAGMA journal_mode=WAL")
        return connection

    def _initialize(self) -> None:
        with self._connect() as db:
            db.executescript(
                """
                CREATE TABLE IF NOT EXISTS devices (
                    device_id TEXT PRIMARY KEY,
                    name TEXT NOT NULL,
                    token_digest TEXT NOT NULL,
                    enabled INTEGER NOT NULL DEFAULT 1,
                    created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS jobs (
                    job_id TEXT PRIMARY KEY,
                    device_id TEXT NOT NULL,
                    status TEXT NOT NULL,
                    question TEXT NOT NULL DEFAULT '',
                    answer TEXT NOT NULL DEFAULT '',
                    provider TEXT NOT NULL DEFAULT '',
                    error TEXT NOT NULL DEFAULT '',
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL,
                    FOREIGN KEY(device_id) REFERENCES devices(device_id)
                );
                CREATE TABLE IF NOT EXISTS segments (
                    job_id TEXT NOT NULL,
                    segment_index INTEGER NOT NULL,
                    text TEXT NOT NULL,
                    audio_path TEXT NOT NULL,
                    byte_count INTEGER NOT NULL,
                    sample_rate INTEGER NOT NULL,
                    created_at TEXT NOT NULL,
                    PRIMARY KEY(job_id, segment_index),
                    FOREIGN KEY(job_id) REFERENCES jobs(job_id)
                );
                CREATE TABLE IF NOT EXISTS device_memories (
                    memory_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT NOT NULL,
                    question TEXT NOT NULL,
                    answer TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    FOREIGN KEY(device_id) REFERENCES devices(device_id) ON DELETE CASCADE
                );
                CREATE INDEX IF NOT EXISTS idx_device_memories_device
                    ON device_memories(device_id, memory_id);
                """
            )
            columns = {row["name"] for row in db.execute("PRAGMA table_info(devices)").fetchall()}
            if "persona" not in columns:
                db.execute("ALTER TABLE devices ADD COLUMN persona TEXT NOT NULL DEFAULT ''")
            if "memory_enabled" not in columns:
                db.execute("ALTER TABLE devices ADD COLUMN memory_enabled INTEGER NOT NULL DEFAULT 0")
            if "memory_turns" not in columns:
                db.execute("ALTER TABLE devices ADD COLUMN memory_turns INTEGER NOT NULL DEFAULT 4")
            if "tts_speaker_id" not in columns:
                db.execute("ALTER TABLE devices ADD COLUMN tts_speaker_id INTEGER NOT NULL DEFAULT 0")
            if "tts_english_speaker_id" not in columns:
                db.execute("ALTER TABLE devices ADD COLUMN tts_english_speaker_id INTEGER NOT NULL DEFAULT 0")
            if "tts_speed" not in columns:
                db.execute("ALTER TABLE devices ADD COLUMN tts_speed REAL NOT NULL DEFAULT 1.0")

    def _digest(self, token: str) -> str:
        return hmac.new(self.pepper, token.encode("utf-8"), hashlib.sha256).hexdigest()

    def register_device(self, device_id: str, name: str, token: str | None = None) -> str:
        token = token or secrets.token_urlsafe(32)
        with self._connect() as db:
            db.execute(
                "INSERT INTO devices(device_id,name,token_digest,created_at) VALUES(?,?,?,?) "
                "ON CONFLICT(device_id) DO UPDATE SET name=excluded.name, token_digest=excluded.token_digest, enabled=1",
                (device_id, name, self._digest(token), utc_now()),
            )
        return token

    def create_device(
        self,
        device_id: str,
        name: str,
        persona: str = "",
        memory_enabled: bool = False,
        memory_turns: int = 4,
        tts_speaker_id: int = 0,
        tts_english_speaker_id: int = 0,
        tts_speed: float = 1.0,
    ) -> str:
        token = secrets.token_urlsafe(32)
        try:
            with self._connect() as db:
                db.execute(
                    "INSERT INTO devices(device_id,name,token_digest,enabled,created_at,persona,memory_enabled,memory_turns,tts_speaker_id,tts_english_speaker_id,tts_speed) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                    (
                        device_id,
                        name,
                        self._digest(token),
                        1,
                        utc_now(),
                        persona,
                        int(memory_enabled),
                        memory_turns,
                        tts_speaker_id,
                        tts_english_speaker_id,
                        tts_speed,
                    ),
                )
        except sqlite3.IntegrityError as exc:
            raise ValueError("设备 ID 已存在") from exc
        return token

    def list_devices(self) -> list[dict[str, Any]]:
        with self._connect() as db:
            rows = db.execute(
                "SELECT d.device_id,d.name,d.enabled,d.created_at,d.persona,d.memory_enabled,d.memory_turns,"
                "d.tts_speaker_id,d.tts_english_speaker_id,d.tts_speed,"
                "COUNT(m.memory_id) AS memory_count FROM devices d "
                "LEFT JOIN device_memories m ON m.device_id=d.device_id "
                "GROUP BY d.device_id ORDER BY d.created_at DESC"
            ).fetchall()
        return [dict(row) for row in rows]

    def get_device(self, device_id: str) -> dict[str, Any] | None:
        with self._connect() as db:
            row = db.execute(
                "SELECT device_id,name,enabled,created_at,persona,memory_enabled,memory_turns,"
                "tts_speaker_id,tts_english_speaker_id,tts_speed FROM devices WHERE device_id=?",
                (device_id,),
            ).fetchone()
        return dict(row) if row else None

    def update_device_profile(self, device_id: str, persona: str, memory_enabled: bool, memory_turns: int = 4) -> None:
        memory_turns = max(1, min(10, int(memory_turns)))
        with self._connect() as db:
            cursor = db.execute(
                "UPDATE devices SET persona=?,memory_enabled=?,memory_turns=? WHERE device_id=?",
                (persona, int(memory_enabled), memory_turns, device_id),
            )
            if not cursor.rowcount:
                raise ValueError("设备不存在")

    def update_device_voice(
        self, device_id: str, tts_speaker_id: int, tts_english_speaker_id: int, tts_speed: float
    ) -> None:
        tts_speaker_id = max(0, min(4, int(tts_speaker_id)))
        tts_english_speaker_id = max(0, min(903, int(tts_english_speaker_id)))
        tts_speed = max(0.5, min(2.0, float(tts_speed)))
        with self._connect() as db:
            cursor = db.execute(
                "UPDATE devices SET tts_speaker_id=?,tts_english_speaker_id=?,tts_speed=? WHERE device_id=?",
                (tts_speaker_id, tts_english_speaker_id, tts_speed, device_id),
            )
            if not cursor.rowcount:
                raise ValueError("设备不存在")

    def set_device_enabled(self, device_id: str, enabled: bool) -> None:
        with self._connect() as db:
            cursor = db.execute("UPDATE devices SET enabled=? WHERE device_id=?", (int(enabled), device_id))
            if not cursor.rowcount:
                raise ValueError("设备不存在")

    def set_memory_enabled(self, device_id: str, enabled: bool) -> None:
        with self._connect() as db:
            cursor = db.execute(
                "UPDATE devices SET memory_enabled=? WHERE device_id=?", (int(enabled), device_id)
            )
            if not cursor.rowcount:
                raise ValueError("设备不存在")

    def add_memory(self, device_id: str, question: str, answer: str) -> None:
        with self._connect() as db:
            db.execute(
                "INSERT INTO device_memories(device_id,question,answer,created_at) VALUES(?,?,?,?)",
                (device_id, question[:2000], answer[:6000], utc_now()),
            )

    def list_memories(self, device_id: str, limit: int = 6) -> list[dict[str, Any]]:
        limit = max(1, min(20, int(limit)))
        with self._connect() as db:
            rows = db.execute(
                "SELECT * FROM (SELECT memory_id,question,answer,created_at FROM device_memories "
                "WHERE device_id=? ORDER BY memory_id DESC LIMIT ?) ORDER BY memory_id",
                (device_id, limit),
            ).fetchall()
        return [dict(row) for row in rows]

    def clear_memories(self, device_id: str) -> int:
        with self._connect() as db:
            cursor = db.execute("DELETE FROM device_memories WHERE device_id=?", (device_id,))
            return cursor.rowcount

    def authenticate(self, device_id: str, token: str) -> bool:
        with self._connect() as db:
            row = db.execute("SELECT token_digest,enabled FROM devices WHERE device_id=?", (device_id,)).fetchone()
        return bool(row and row["enabled"] and hmac.compare_digest(row["token_digest"], self._digest(token)))

    def create_job(self, job_id: str, device_id: str) -> None:
        now = utc_now()
        with self._connect() as db:
            db.execute("INSERT INTO jobs(job_id,device_id,status,created_at,updated_at) VALUES(?,?, 'queued',?,?)", (job_id, device_id, now, now))

    def update_job(self, job_id: str, **fields: str) -> None:
        allowed = {"status", "question", "answer", "provider", "error"}
        clean = {key: value for key, value in fields.items() if key in allowed}
        clean["updated_at"] = utc_now()
        sql = ",".join(f"{key}=?" for key in clean)
        with self._connect() as db:
            db.execute(f"UPDATE jobs SET {sql} WHERE job_id=?", (*clean.values(), job_id))

    def get_job(self, job_id: str, device_id: str) -> dict[str, Any] | None:
        with self._connect() as db:
            row = db.execute("SELECT * FROM jobs WHERE job_id=? AND device_id=?", (job_id, device_id)).fetchone()
        return dict(row) if row else None

    def add_segment(self, job_id: str, index: int, text: str, audio_path: Path, byte_count: int, sample_rate: int) -> None:
        with self._connect() as db:
            db.execute("INSERT INTO segments VALUES(?,?,?,?,?,?,?)", (job_id, index, text, str(audio_path), byte_count, sample_rate, utc_now()))

    def list_segments(self, job_id: str, after: int = -1) -> list[dict[str, Any]]:
        with self._connect() as db:
            rows = db.execute("SELECT * FROM segments WHERE job_id=? AND segment_index>? ORDER BY segment_index", (job_id, after)).fetchall()
        return [dict(row) for row in rows]

    def get_segment(self, job_id: str, index: int) -> dict[str, Any] | None:
        with self._connect() as db:
            row = db.execute("SELECT * FROM segments WHERE job_id=? AND segment_index=?", (job_id, index)).fetchone()
        return dict(row) if row else None
