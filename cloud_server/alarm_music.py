"""Bounded, local alarm music library. Presets are original synthesized melodies."""
from __future__ import annotations

import html
import io
import json
import math
import os
import re
import secrets
import struct
import subprocess
import tempfile
import threading
import wave
from pathlib import Path

SAMPLE_RATE = 16000
MAX_PCM_BYTES = 960000  # 30 seconds, signed 16-bit mono
MAX_WAV_BYTES = 1000000
MAX_SOURCE_BYTES = 20 * 1024 * 1024
MAX_UPLOAD_BYTES = MAX_SOURCE_BYTES + 16384
MAX_DEVICE_UPLOAD_BYTES = 512 * 1024
MAX_TRACKS = 12
DEFAULT_TRACK = "morning"
TRACK_ID = re.compile(r"^[a-z0-9_-]{1,48}$")
PRESETS = {
    "morning": ("晨光 · 原创轻音乐", (60, 64, 67, 72, 69, 67, 64, 62, 65, 69, 72, 74, 72, 69, 67, 60)),
    "breeze": ("微风 · 原创轻音乐", (67, 69, 71, 74, 72, 71, 69, 67, 64, 67, 69, 72, 71, 69, 67, 64)),
    "starlight": ("星光 · 原创轻音乐", (57, 60, 64, 69, 67, 64, 60, 57, 55, 59, 62, 67, 64, 62, 59, 57)),
}
_conversion_slot = threading.BoundedSemaphore(1)


class AlarmMusicError(ValueError):
    pass


def normalize_admin_music(payload: bytes, filename: str) -> bytes:
    """Decode only a local MP3, retaining its first 30 seconds as a bounded WAV."""
    mp3_signature = payload.startswith(b'ID3') or (len(payload) >= 2 and payload[0] == 0xff and payload[1] & 0xe0 == 0xe0)
    if Path(filename).suffix.lower() != '.mp3' and not mp3_signature:
        validate_wav(payload)
        return payload
    if not payload or len(payload) > MAX_SOURCE_BYTES:
        raise AlarmMusicError('MP3 文件不能超过 20 MB')
    if not _conversion_slot.acquire(blocking=False):
        raise AlarmMusicError('正在转换另一首音乐，请稍后重试')
    try:
        with tempfile.TemporaryDirectory(prefix='alarm-music-') as directory:
            source, output = Path(directory) / 'source.mp3', Path(directory) / 'clip.wav'
            source.write_bytes(payload)
            try:
                subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error',
                                '-protocol_whitelist', 'file', '-f', 'mp3', '-i', str(source),
                                '-map', '0:a:0', '-vn', '-t', '30', '-ac', '1', '-ar', '16000',
                                '-threads', '1', '-c:a', 'pcm_s16le', '-fs', str(MAX_WAV_BYTES),
                                '-y', str(output)], check=True, timeout=20,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except FileNotFoundError as exc:
                raise AlarmMusicError('服务器尚未安装 FFmpeg，暂时可上传 WAV') from exc
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
                raise AlarmMusicError('MP3 无法解码或转换超时，请换一个文件') from exc
            with output.open('rb') as stream:
                converted = stream.read(MAX_WAV_BYTES + 1)
            validate_wav(converted)
            return converted
    finally:
        _conversion_slot.release()


def validate_wav(payload: bytes) -> bytes:
    if not payload or len(payload) > MAX_WAV_BYTES:
        raise AlarmMusicError("WAV 文件不能超过 1 MB")
    try:
        with wave.open(io.BytesIO(payload), "rb") as sound:
            if (sound.getnchannels(), sound.getsampwidth(), sound.getframerate(), sound.getcomptype()) != (1, 2, SAMPLE_RATE, "NONE"):
                raise AlarmMusicError("请选择 16000 Hz、单声道、16 位 PCM WAV")
            frames = sound.getnframes()
            if not 0 < frames <= SAMPLE_RATE * 30:
                raise AlarmMusicError("音乐时长必须在 0 至 30 秒之间")
            pcm = sound.readframes(frames)
            if len(pcm) != frames * 2 or len(pcm) > MAX_PCM_BYTES:
                raise AlarmMusicError("WAV 音频数据不完整或过长")
            return pcm
    except (wave.Error, EOFError, struct.error) as exc:
        raise AlarmMusicError("WAV 文件无效") from exc


def _melody(notes: tuple[int, ...]) -> bytes:
    pcm = bytearray()
    # A soft fundamental plus two quiet harmonics, 0.6 seconds per note.
    for note in notes:
        frequency = 440 * 2 ** ((note - 69) / 12)
        for sample in range(9600):
            t = sample / SAMPLE_RATE
            envelope = min(1.0, t / .025) * max(0.0, 1.0 - t / .6) ** 1.7
            signal = math.sin(2 * math.pi * frequency * t) + .22 * math.sin(4 * math.pi * frequency * t) + .08 * math.sin(6 * math.pi * frequency * t)
            pcm.extend(struct.pack("<h", round(10000 * envelope * signal)))
    output = io.BytesIO()
    with wave.open(output, "wb") as sound:
        sound.setparams((1, 2, SAMPLE_RATE, 0, "NONE", "not compressed"))
        sound.writeframes(pcm)
    return output.getvalue()


class AlarmMusicRepository:
    def __init__(self, directory: Path):
        self.directory = directory
        self.directory.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.catalog_path = directory / "catalog.json"
        self.selection_path = directory / "selections.json"
        self.tracks = self._load(self.catalog_path, {})
        self.selections = self._load(self.selection_path, {})
        for track_id, (title, notes) in PRESETS.items():
            path = self._path(track_id)
            if not path.is_file():
                self._atomic_write(path, _melody(notes))
            self.tracks[track_id] = {"title": title, "preset": True}
        self._save(self.catalog_path, self.tracks)

    @staticmethod
    def _load(path: Path, fallback: dict) -> dict:
        if not path.exists():
            return fallback
        try:
            value = json.loads(path.read_text("utf-8"))
            if not isinstance(value, dict):
                raise ValueError("expected object")
            return value
        except (ValueError, OSError) as exc:
            raise RuntimeError("闹钟音乐索引无法读取") from exc

    @staticmethod
    def _atomic_write(path: Path, payload: bytes) -> None:
        temporary = path.with_name(path.name + "." + secrets.token_hex(8) + ".tmp")
        try:
            with temporary.open("xb") as stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)

    def _save(self, path: Path, value: dict) -> None:
        self._atomic_write(path, json.dumps(value, ensure_ascii=False).encode("utf-8"))

    def _path(self, track_id: str) -> Path:
        if not TRACK_ID.fullmatch(track_id):
            raise AlarmMusicError("音乐标识无效")
        path = self.directory / (track_id + ".wav")
        if path.resolve().parent != self.directory.resolve():
            raise AlarmMusicError("音乐路径无效")
        return path

    def _wav(self, track_id: str, device_id: str | None = None) -> bytes:
        path = self._path(track_id)
        if track_id not in self.tracks or not path.is_file():
            raise AlarmMusicError("音乐不存在")
        if device_id is not None and self.tracks[track_id].get("owner") not in (None, device_id):
            raise AlarmMusicError("音乐不存在")
        with path.open("rb") as source:
            payload = source.read(MAX_WAV_BYTES + 1)
        validate_wav(payload)
        return payload

    def catalog(self, device_id: str | None = None) -> list[dict]:
        with self.lock:
            return [{"id": track_id, "title": metadata["title"], "bytes": len(validate_wav(self._wav(track_id))),
                     "preset": bool(metadata.get("preset")), "sample_rate": SAMPLE_RATE}
                    for track_id, metadata in self.tracks.items()
                    if device_id is None or metadata.get("owner") in (None, device_id)]

    def wav(self, track_id: str, device_id: str | None = None) -> bytes:
        with self.lock:
            return self._wav(track_id, device_id)

    def pcm(self, track_id: str, device_id: str | None = None) -> bytes:
        return validate_wav(self.wav(track_id, device_id))

    def upload(self, payload: bytes, title: str, device_id: str | None = None) -> str:
        validate_wav(payload)
        title = title.strip()
        if not title or len(title) > 80 or any(ord(character) < 32 for character in title):
            raise AlarmMusicError("音乐名称须为 1 至 80 个字符")
        with self.lock:
            if len(self.tracks) >= MAX_TRACKS:
                raise AlarmMusicError("最多保存 12 首音乐，请先删除不用的音乐")
            if device_id is not None and sum(track.get("owner") == device_id for track in self.tracks.values()) >= 6:
                raise AlarmMusicError("每个设备最多上传 6 首音乐")
            track_id = "upload_" + secrets.token_hex(12)
            self._atomic_write(self._path(track_id), payload)
            updated = dict(self.tracks)
            updated[track_id] = {"title": title, "preset": False, "owner": device_id}
            try:
                self._save(self.catalog_path, updated)
            except Exception:
                self._path(track_id).unlink(missing_ok=True)
                raise
            self.tracks = updated
            return track_id

    def delete(self, track_id: str) -> None:
        with self.lock:
            self._path(track_id)
            if track_id not in self.tracks:
                raise AlarmMusicError("音乐不存在")
            if self.tracks[track_id].get("preset"):
                raise AlarmMusicError("内置原创音乐保留作为离线回退选项")
            updated = dict(self.tracks)
            del updated[track_id]
            self._save(self.catalog_path, updated)
            self.tracks = updated
            self._path(track_id).unlink(missing_ok=True)

    def selection(self, device_id: str) -> str:
        with self.lock:
            selected = self.selections.get(device_id, DEFAULT_TRACK)
            return selected if selected in self.tracks else DEFAULT_TRACK

    def select(self, device_id: str, track_id: str) -> str:
        with self.lock:
            self._wav(track_id, device_id)
            updated = dict(self.selections)
            updated[device_id] = track_id
            self._save(self.selection_path, updated)
            self.selections = updated
            return track_id


def alarm_music_content(tracks: list[dict], csrf: str) -> str:
    rows = []
    for track in tracks:
        track_id = html.escape(track["id"], quote=True)
        delete = "内置原创" if track["preset"] else f'<form method="post" action="/admin/alarm-music/{track_id}/delete"><input type="hidden" name="csrf" value="{html.escape(csrf, quote=True)}"><button>删除</button></form>'
        rows.append(f'<tr><td>{html.escape(track["title"])}</td><td>{track["bytes"] / 32000:.1f} 秒</td><td><audio controls preload="none" src="/admin/alarm-music/{track_id}/wav"></audio></td><td>{delete}</td></tr>')
    return ('<h1>闹钟音乐</h1><p>设备在闹钟响起时循环播放，可语音结束或延后。内置三首原创纯音乐，也可上传自己的音乐。</p>'
            '<table><thead><tr><th>音乐</th><th>时长</th><th>试听</th><th>管理</th></tr></thead><tbody>' + ''.join(rows) + '</tbody></table>'
            f'<form method="post" action="/admin/alarm-music/upload" enctype="multipart/form-data"><fieldset><input type="hidden" name="csrf" value="{html.escape(csrf, quote=True)}">'
            '<label>音乐名称</label><input name="title" maxlength="80" required><label>音乐文件</label><input type="file" name="music" accept=".mp3,.wav,audio/mpeg,audio/wav" required>'
            '<p>支持 MP3（最大 20 MB，自动截取前 30 秒）或 16000 Hz、单声道、16 位 PCM WAV（最长 30 秒、最大 1 MB）。最多 12 首。</p><button>上传音乐</button></fieldset></form>')
