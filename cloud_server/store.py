from __future__ import annotations

import hashlib
import hmac
import json
import secrets
import sqlite3
from decimal import Decimal
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .personal_profile import PersonalProfileConflict, canonical_snapshot, validated_snapshot


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
                CREATE TABLE IF NOT EXISTS device_personal_profiles (
                    device_id TEXT PRIMARY KEY,
                    revision TEXT NOT NULL,
                    snapshot_json TEXT NOT NULL,
                    fingerprint TEXT NOT NULL,
                    updated_at TEXT NOT NULL,
                    FOREIGN KEY(device_id) REFERENCES devices(device_id) ON DELETE CASCADE
                );
                CREATE TABLE IF NOT EXISTS llm_usage (
                    usage_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT NOT NULL,
                    provider TEXT NOT NULL,
                    key_id TEXT NOT NULL,
                    model TEXT NOT NULL,
                    operation TEXT NOT NULL CHECK(operation IN ('dialogue','translation')),
                    success INTEGER NOT NULL CHECK(success IN (0,1)),
                    input_tokens INTEGER,
                    output_tokens INTEGER,
                    total_tokens INTEGER,
                    created_at TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_llm_usage_created ON llm_usage(created_at);
                CREATE INDEX IF NOT EXISTS idx_llm_usage_key ON llm_usage(provider,key_id,created_at);
                CREATE INDEX IF NOT EXISTS idx_llm_usage_device ON llm_usage(device_id,created_at);
                CREATE TABLE IF NOT EXISTS speech_usage (
                    usage_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT NOT NULL, operation TEXT NOT NULL, model TEXT NOT NULL,
                    success INTEGER NOT NULL, quantity TEXT NOT NULL, unit TEXT NOT NULL,
                    estimated_cost TEXT, price_snapshot TEXT NOT NULL, created_at TEXT NOT NULL
                );
                """
            )
            columns = {row["name"] for row in db.execute("PRAGMA table_info(devices)").fetchall()}
            if "persona" not in columns:
                db.execute("ALTER TABLE devices ADD COLUMN persona TEXT NOT NULL DEFAULT ''")
            if "reply_style" not in columns:
                db.execute("ALTER TABLE devices ADD COLUMN reply_style TEXT NOT NULL DEFAULT 'default'")
            if "web_search_enabled" not in columns:
                db.execute("ALTER TABLE devices ADD COLUMN web_search_enabled INTEGER NOT NULL DEFAULT 0")
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
            usage_columns = {row['name'] for row in db.execute('PRAGMA table_info(llm_usage)')}
            for name, definition in {
                'cached_input_tokens': 'INTEGER', 'search_calls': 'INTEGER',
                'cost_amount': 'TEXT', 'cost_currency': "TEXT NOT NULL DEFAULT ''",
                'cost_status': "TEXT NOT NULL DEFAULT 'historical'", 'price_snapshot': "TEXT NOT NULL DEFAULT ''",
                'cost_note': "TEXT NOT NULL DEFAULT ''",
            }.items():
                if name not in usage_columns:
                    db.execute(f'ALTER TABLE llm_usage ADD COLUMN {name} {definition}')
            speech_columns = {row['name'] for row in db.execute('PRAGMA table_info(speech_usage)')}
            if 'key_id' not in speech_columns:
                db.execute("ALTER TABLE speech_usage ADD COLUMN key_id TEXT NOT NULL DEFAULT ''")
            db.execute('CREATE INDEX IF NOT EXISTS idx_speech_usage_device ON speech_usage(device_id,created_at)')

    def _digest(self, token: str) -> str:
        return hmac.new(self.pepper, token.encode("utf-8"), hashlib.sha256).hexdigest()

    def record_speech_usage(self, device_id, operation, model, success, quantity, unit, cost, snapshot, *, key_id=''):
        with self._connect() as db:
            db.execute('INSERT INTO speech_usage(device_id,operation,model,success,quantity,unit,estimated_cost,price_snapshot,created_at,key_id) VALUES(?,?,?,?,?,?,?,?,?,?)',
                       (device_id, operation, model, int(success), quantity, unit, cost,
                        json.dumps(snapshot, ensure_ascii=False), utc_now(), key_id))

    def api_usage_summary(self, since=None, device_id=None):
        filters, params = [], []
        if since:
            filters.append('created_at>=?')
            params.append(since)
        if device_id is not None:
            filters.append('device_id=?')
            params.append(device_id)
        where = ' WHERE ' + ' AND '.join(filters) if filters else ''
        groups, totals = {}, {'llm': {}, 'speech': {}}
        with self._connect() as db:
            db.execute('BEGIN')
            for kind, query in (
                ('llm', 'SELECT provider,key_id,model,operation,success,input_tokens,output_tokens,total_tokens,cost_amount,cost_currency,cost_status,created_at FROM llm_usage'),
                ('speech', 'SELECT key_id,model,operation,success,quantity,unit,estimated_cost,created_at FROM speech_usage'),
            ):
                for record in db.execute(query + where, params):
                    provider = record['provider'] if kind == 'llm' else 'cloud_speech'
                    key = (kind, provider, record['key_id'], record['model'], record['operation'])
                    row = groups.setdefault(key, {
                        'kind': kind, 'provider': provider, 'key_id': record['key_id'],
                        'model': record['model'], 'operation': record['operation'],
                        'attempts': 0, 'successes': 0, 'failures': 0, 'unpriced_calls': 0,
                        'unreported_calls': 0, 'input_tokens': 0, 'output_tokens': 0,
                        'total_tokens': 0, 'quantity': Decimal(0),
                        'unit': 'Token' if kind == 'llm' else record['unit'],
                        'costs': {}, 'statuses': {}, 'last_used': '',
                    })
                    row['attempts'] += 1
                    row['successes'] += record['success']
                    row['failures'] += 1 - record['success']
                    row['last_used'] = max(row['last_used'], record['created_at'])
                    if kind == 'llm':
                        for field in ('input_tokens', 'output_tokens', 'total_tokens'):
                            row[field] += record[field] or 0
                        row['unreported_calls'] += record['total_tokens'] is None
                        amount, currency, status = record['cost_amount'], record['cost_currency'], record['cost_status']
                    else:
                        row['quantity'] += Decimal(record['quantity'])
                        amount, currency = record['estimated_cost'], 'CNY'
                        status = 'estimated' if amount is not None else 'unpriced'
                    row['statuses'][status] = row['statuses'].get(status, 0) + 1
                    row['unpriced_calls'] += amount is None
                    if amount is not None:
                        value = Decimal(amount)
                        row['costs'][currency] = row['costs'].get(currency, Decimal(0)) + value
                        totals[kind][currency] = totals[kind].get(currency, Decimal(0)) + value
        rows = sorted(groups.values(), key=lambda row: row['last_used'], reverse=True)
        for row in rows:
            row['quantity'] = format(row['quantity'], 'f')
            row['costs'] = {currency: format(value, 'f') for currency, value in row['costs'].items()}
        return {'by_api': rows, 'totals': {kind: {currency: format(value, 'f') for currency, value in amounts.items()}
                                         for kind, amounts in totals.items()}}

    def recent_speech_usage(self, since=None):
        with self._connect() as db:
            return [dict(row) for row in db.execute('SELECT * FROM speech_usage' +
                    (' WHERE created_at>=?' if since else '') + ' ORDER BY usage_id DESC LIMIT 30',
                    (since,) if since else ())]

    def api_usage_records(self, since=None, device_id=None):
        """Read the full filtered ledger for date charts, without recent-row limits."""
        filters, params = [], []
        if since:
            filters.append('created_at>=?')
            params.append(since)
        if device_id is not None:
            filters.append('device_id=?')
            params.append(device_id)
        where = ' WHERE ' + ' AND '.join(filters) if filters else ''
        records = []
        with self._connect() as db:
            db.execute('BEGIN')
            for kind, table in (('llm', 'llm_usage'), ('speech', 'speech_usage')):
                records.extend(dict(row, kind=kind) for row in db.execute(
                    'SELECT * FROM ' + table + where + ' ORDER BY created_at', params))
        return records

    def api_key_id(self, provider: str, api_key: str, base_url: str) -> str:
        if not api_key:
            return ""
        # Namespace this identity separately from device authentication digests.
        message = json.dumps(["llm-usage", provider, base_url.rstrip("/"), api_key], separators=(",", ":"))
        return hmac.new(self.pepper, message.encode("utf-8"), hashlib.sha256).hexdigest()

    def record_usage(
        self, *, device_id: str, provider: str, key_id: str, model: str,
        operation: str, success: bool, input_tokens: int | None = None,
        output_tokens: int | None = None, total_tokens: int | None = None,
        cached_input_tokens: int | None = None, search_calls: int | None = None,
        cost_amount: str | None = None, cost_currency: str = '', cost_status: str = 'historical',
        price_snapshot: str = '', cost_note: str = '',
    ) -> None:
        with self._connect() as db:
            db.execute(
                "INSERT INTO llm_usage(device_id,provider,key_id,model,operation,success,"
                "input_tokens,output_tokens,total_tokens,created_at,cached_input_tokens,search_calls,"
                "cost_amount,cost_currency,cost_status,price_snapshot,cost_note) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (device_id, provider, key_id, model, operation, int(success),
                 input_tokens, output_tokens, total_tokens, utc_now(), cached_input_tokens, search_calls,
                 cost_amount, cost_currency, cost_status, price_snapshot, cost_note),
            )

    def billing_summary(self, since: str | None = None) -> dict:
        totals, devices, statuses = {}, {}, {}
        where, params = (' WHERE created_at>=?', (since,)) if since else ('', ())
        with self._connect() as db:
            for row in db.execute('SELECT device_id,cost_amount,cost_currency,cost_status FROM llm_usage' + where, params):
                statuses[row['cost_status']] = statuses.get(row['cost_status'], 0) + 1
                if row['cost_amount'] is not None:
                    currency, amount = row['cost_currency'], Decimal(row['cost_amount'])
                    totals[currency] = totals.get(currency, Decimal(0)) + amount
                    key = (row['device_id'], currency)
                    devices[key] = devices.get(key, Decimal(0)) + amount
        return {'totals': {currency: format(amount, 'f') for currency, amount in totals.items()},
                'by_device': [{'device_id': key[0], 'currency': key[1], 'amount': format(value, 'f')}
                              for key, value in sorted(devices.items())], 'statuses': statuses}

    def usage_summary(self, since: str | None = None) -> dict:
        columns = (
            "COUNT(*) AS attempts, COALESCE(SUM(success),0) AS successes, "
            "COALESCE(SUM(1-success),0) AS failures, "
            "COUNT(total_tokens) AS reported_calls, COUNT(*)-COUNT(total_tokens) AS unreported_calls, "
            "COALESCE(SUM(input_tokens),0) AS input_tokens, "
            "COALESCE(SUM(output_tokens),0) AS output_tokens, "
            "COALESCE(SUM(total_tokens),0) AS total_tokens, MAX(created_at) AS last_used"
        )
        where, params = (" WHERE created_at>=?", (since,)) if since else ("", ())
        with self._connect() as db:
            # All three aggregates come from one snapshot under concurrent writes.
            db.execute("BEGIN")
            totals = dict(db.execute(f"SELECT {columns} FROM llm_usage{where}", params).fetchone())
            by_key = [dict(row) for row in db.execute(
                f"SELECT provider,key_id,{columns} FROM llm_usage{where} GROUP BY provider,key_id ORDER BY total_tokens DESC", params
            ).fetchall()]
            by_device = [dict(row) for row in db.execute(
                f"SELECT device_id,{columns} FROM llm_usage{where} GROUP BY device_id ORDER BY total_tokens DESC", params
            ).fetchall()]
        return {"totals": totals, "by_key": by_key, "by_device": by_device}

    def recent_usage(self, limit: int = 20, since: str | None = None) -> list[dict]:
        limit = max(1, min(100, int(limit)))
        where, params = (" WHERE created_at>=?", (since,)) if since else ("", ())
        with self._connect() as db:
            return [dict(row) for row in db.execute(
                f"SELECT * FROM llm_usage{where} ORDER BY usage_id DESC LIMIT ?", (*params, limit)
            ).fetchall()]

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
                "SELECT d.device_id,d.name,d.enabled,d.created_at,d.persona,d.reply_style,d.web_search_enabled,d.memory_enabled,d.memory_turns,"
                "d.tts_speaker_id,d.tts_english_speaker_id,d.tts_speed,"
                "COUNT(m.memory_id) AS memory_count FROM devices d "
                "LEFT JOIN device_memories m ON m.device_id=d.device_id "
                "GROUP BY d.device_id ORDER BY d.created_at DESC"
            ).fetchall()
        return [dict(row) for row in rows]

    def get_device(self, device_id: str) -> dict[str, Any] | None:
        with self._connect() as db:
            row = db.execute(
                "SELECT device_id,name,enabled,created_at,persona,reply_style,web_search_enabled,memory_enabled,memory_turns,"
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

    def update_dialogue_profile(self, device_id: str, reply_style: str, persona: str,
                                memory_enabled: bool, memory_turns: int, web_search_enabled: bool = False) -> None:
        if reply_style not in {'default', 'brief', 'normal', 'detailed'} or len(persona) > 800 or not 1 <= memory_turns <= 10:
            raise ValueError('对话设置无效')
        with self._connect() as db:
            db.execute('UPDATE devices SET reply_style=?,persona=?,memory_enabled=?,memory_turns=?,web_search_enabled=? WHERE device_id=?',
                       (reply_style, persona, int(memory_enabled), memory_turns, int(web_search_enabled), device_id))

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

    def get_personal_profile(self, device_id: str) -> dict:
        with self._connect() as db:
            row = db.execute('SELECT snapshot_json FROM device_personal_profiles WHERE device_id=?',
                             (device_id,)).fetchone()
        return json.loads(row['snapshot_json']) if row else validated_snapshot({'revision': 0})

    def save_personal_profile(self, device_id: str, value) -> dict:
        snapshot = validated_snapshot(value)
        full_json = canonical_snapshot(snapshot)
        fingerprint = hmac.new(self.pepper, ('personal-profile:' + full_json).encode('utf-8'),
                               hashlib.sha256).hexdigest()
        backup = {**snapshot, 'items': snapshot['items'] if snapshot['backup_enabled'] else []}
        with self._connect() as db:
            # Compare and replace under a writer lock, including concurrent jobs.
            db.execute('BEGIN IMMEDIATE')
            current = db.execute('SELECT revision,snapshot_json,fingerprint FROM device_personal_profiles WHERE device_id=?',
                                 (device_id,)).fetchone()
            if current:
                revision = int(current['revision'])
                if snapshot['revision'] < revision:
                    raise PersonalProfileConflict('个人特征版本过旧，请以设备当前版本重新同步')
                if snapshot['revision'] == revision:
                    if not hmac.compare_digest(current['fingerprint'], fingerprint):
                        raise PersonalProfileConflict('个人特征同一版本内容冲突，请增加版本后同步')
                    return json.loads(current['snapshot_json'])
            old_items = json.loads(current['snapshot_json'])['items'] if current else []
            removed = any(item not in snapshot['items'] for item in old_items)
            if removed or not snapshot['enabled'] or not snapshot['backup_enabled'] or not snapshot['items']:
                # Raw history could otherwise reintroduce a forgotten trait.
                db.execute('DELETE FROM device_memories WHERE device_id=?', (device_id,))
            db.execute('INSERT INTO device_personal_profiles(device_id,revision,snapshot_json,fingerprint,updated_at) '
                       'VALUES(?,?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET '
                       'revision=excluded.revision,snapshot_json=excluded.snapshot_json,'
                       'fingerprint=excluded.fingerprint,updated_at=excluded.updated_at',
                       (device_id, str(snapshot['revision']), canonical_snapshot(backup), fingerprint, utc_now()))
        return backup

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
