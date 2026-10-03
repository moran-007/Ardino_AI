"""Full songs remain on disk; SQLite stores metadata, timed lyrics and playlists."""
from __future__ import annotations

import asyncio
import anyio
import json
import os
import re
import secrets
import shutil
import sqlite3
import subprocess
import tempfile
import threading
import time
from contextlib import contextmanager
from pathlib import Path
from urllib.parse import urlsplit

SAMPLE_RATE = 48000
LYRICS_LIMIT = 1024 * 1024
DISK_RESERVE = 256 * 1024 * 1024
ID_PATTERN = re.compile(r'^[a-f0-9]{32}$')
TIMESTAMP = re.compile(r'\[(\d{1,6}):([0-5]\d)(?:[.:](\d{1,3}))?\]')
TAG = re.compile(r'\[(ti|ar|offset):([^\]\r\n]*)\]', re.I)


class MusicError(ValueError):
    pass


def clean_text(value: object, limit: int = 160) -> str:
    return re.sub(r'[\x00-\x1f\x7f]', ' ', str(value or '')).strip()[:limit]


def parse_lyrics(payload: bytes) -> tuple[list[dict], dict]:
    if not payload or len(payload) > LYRICS_LIMIT:
        raise MusicError('歌词不能为空且不能超过 1 MB')
    encodings = ['utf-16'] if payload.startswith((b'\xff\xfe', b'\xfe\xff')) else ['utf-8-sig', 'gb18030']
    for encoding in encodings:
        try:
            text = payload.decode(encoding)
            break
        except UnicodeError:
            continue
    else:
        raise MusicError('歌词编码无效，请使用 UTF-8、UTF-16 或 GB18030')
    # Some lyric exporters append a local /name/... filename after the content.
    text = text.split('/name/', 1)[0]
    tags = {match[1].lower(): clean_text(match[2]) for match in TAG.finditer(text)}
    try:
        offset = int(tags.get('offset', '0'))
    except ValueError:
        raise MusicError('歌词 offset 必须是整数毫秒') from None
    if abs(offset) > 2**63 - 1:
        raise MusicError('歌词时间偏移超出范围')
    text = TAG.sub('', text)
    matches = list(TIMESTAMP.finditer(text))
    cues, pending = {}, []
    for index, match in enumerate(matches):
        milliseconds = (int(match[1]) * 60 + int(match[2])) * 1000 + int((match[3] or '0').ljust(3, '0'))
        moment = max(0, milliseconds + offset)
        if moment > 2**63 - 1:
            raise MusicError('歌词时间超出范围')
        pending.append(moment)
        following = text[match.end():matches[index + 1].start() if index + 1 < len(matches) else len(text)]
        line = clean_text(following, 256)
        if line:
            for moment in pending:
                cues[moment] = line
            pending.clear()
        elif '\n' in following or '\r' in following:
            for moment in pending:
                cues[moment] = ''
            pending.clear()
    for moment in pending:
        cues[moment] = ''
    if not matches:
        raise MusicError('歌词需要 LRC 时间标签，例如 [00:12.30]')
    return [{'time_ms': moment, 'text': line} for moment, line in sorted(cues.items())], tags


def filename_metadata(filename: str) -> tuple[str, str]:
    # Ignore paths and understand common title - artist filenames without fixing a particular song.
    stem = Path(filename.replace('\\', '/')).stem
    title, separator, artist = stem.rpartition(' - ')
    return (clean_text(title), clean_text(artist)) if separator else (clean_text(stem), '')


def probe_audio(path: Path) -> dict:
    with path.open('rb') as stream:
        head = stream.read(16)
    if head[:4] in (b'RIFF', b'RF64', b'BW64') and head[8:12] == b'WAVE':
        kind = 'wav'
    elif head.startswith(b'ID3') or (len(head) >= 2 and head[0] == 255 and head[1] & 224 == 224):
        kind = 'mp3'
    else:
        raise MusicError('实际音频内容需要为 MP3 或 WAV；文件扩展名不作为唯一依据')
    try:
        with tempfile.TemporaryFile() as output:
            subprocess.run(['ffprobe', '-v', 'error', '-protocol_whitelist', 'file', '-f', kind,
                            '-select_streams', 'a:0', '-show_entries',
                            'format=duration:format_tags=title,artist:stream=codec_name,sample_rate,channels',
                            '-of', 'json', str(path)], stdout=output, stderr=subprocess.DEVNULL,
                           check=True, timeout=30)
            output.seek(0)
            raw = output.read(65537)
        if len(raw) > 65536:
            raise MusicError('音频元数据过长')
        result = json.loads(raw)
        audio = result['streams'][0]
        duration = float(result['format']['duration'])
        if not duration > 0 or not duration < float('inf'):
            raise ValueError()
        if kind == 'mp3' and audio['codec_name'] != 'mp3':
            raise ValueError()
        if kind == 'wav' and not audio['codec_name'].startswith('pcm_'):
            raise ValueError()
        tags = {key.lower(): clean_text(value) for key, value in result['format'].get('tags', {}).items()}
        return dict(format=kind, duration_ms=round(duration * 1000), source_sample_rate=int(audio['sample_rate']),
                    channels=int(audio['channels']), title=tags.get('title', ''), artist=tags.get('artist', ''))
    except FileNotFoundError:
        raise MusicError('服务器未安装 FFmpeg / FFprobe') from None
    except (subprocess.SubprocessError, ValueError, KeyError, IndexError, OverflowError):
        raise MusicError('音频无有效音轨、格式不支持或解析超时') from None


class MusicLibrary:
    def __init__(self, root: Path):
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True)
        (root / 'staging').mkdir(exist_ok=True)
        self.database = root / 'library.db'
        self._stream_lock = threading.Lock()
        self._streams: dict[str, str] = {}
        with self.connect() as db:
            db.executescript('''
              PRAGMA journal_mode=WAL;
              CREATE TABLE IF NOT EXISTS tracks (
                id TEXT PRIMARY KEY, owner TEXT NOT NULL, title TEXT NOT NULL, artist TEXT NOT NULL,
                filename TEXT NOT NULL, format TEXT NOT NULL, duration_ms INTEGER NOT NULL,
                source_sample_rate INTEGER NOT NULL, channels INTEGER NOT NULL, bytes INTEGER NOT NULL,
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);
              CREATE INDEX IF NOT EXISTS track_owner ON tracks(owner);
              CREATE TABLE IF NOT EXISTS lyrics (
                track_id TEXT NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
                time_ms INTEGER NOT NULL, text TEXT NOT NULL, PRIMARY KEY(track_id,time_ms));
              CREATE TABLE IF NOT EXISTS playlists (
                id TEXT PRIMARY KEY, owner TEXT NOT NULL, title TEXT NOT NULL,
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);
              CREATE TABLE IF NOT EXISTS playlist_tracks (
                playlist_id TEXT NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
                track_id TEXT NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
                position INTEGER NOT NULL, PRIMARY KEY(playlist_id,track_id));
            ''')

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.database, timeout=10)
        db.row_factory = sqlite3.Row
        db.execute('PRAGMA foreign_keys=ON')
        try:
            with db:
                yield db
        finally:
            db.close()

    @staticmethod
    def metadata(row) -> dict:
        result = dict(row)
        result['sample_rate'] = SAMPLE_RATE
        result['lyrics_available'] = bool(result.get('lyrics_available'))
        return result

    @staticmethod
    def visible(owner: str | None) -> tuple[str, list]:
        return ('1', []) if owner is None else ("(t.owner='' OR t.owner=?)", [owner])

    def track(self, track_id: str, owner: str | None) -> dict:
        if not ID_PATTERN.fullmatch(track_id):
            raise MusicError('歌曲不存在')
        visibility, args = self.visible(owner)
        with self.connect() as db:
            row = db.execute(f'''SELECT t.*, EXISTS(SELECT 1 FROM lyrics l WHERE l.track_id=t.id) lyrics_available
              FROM tracks t WHERE t.id=? AND {visibility}''', [track_id, *args]).fetchone()
        if not row:
            raise MusicError('歌曲不存在或不可访问')
        return self.metadata(row)

    def catalog(self, owner: str | None, query: str = '', cursor: int = 0, limit: int = 20) -> dict:
        visibility, args = self.visible(owner)
        with self.connect() as db:
            # instr treats % and _ as literal search characters.
            rows = db.execute(f'''SELECT t.*, EXISTS(SELECT 1 FROM lyrics l WHERE l.track_id=t.id) lyrics_available
              FROM tracks t WHERE {visibility} AND (instr(lower(t.title),lower(?))>0 OR instr(lower(t.artist),lower(?))>0)
              ORDER BY t.created_at,t.id LIMIT ? OFFSET ?''', [*args, query, query, limit + 1, cursor]).fetchall()
        return {'tracks': [self.metadata(row) for row in rows[:limit]],
                'next_cursor': cursor + limit if len(rows) > limit else None}

    def import_upload(self, audio: Path | None, filename: str, lyrics: bytes | None, lyric_filename: str,
                      owner: str, title: str = '', artist: str = '', track_id: str = '') -> dict:
        cues, tags = parse_lyrics(lyrics) if lyrics is not None else ([], {})
        if track_id:
            track = self.track(track_id, owner)
            if track['owner'] != owner:
                raise MusicError('只能修改自己的歌曲歌词')
            if audio is not None:
                raise MusicError('补充歌词时不能同时替换音频')
            if lyrics is None:
                raise MusicError('请选择歌词文件')
            with self.connect() as db:
                db.execute('DELETE FROM lyrics WHERE track_id=?', [track_id])
                db.executemany('INSERT INTO lyrics VALUES (?,?,?)', [(track_id, cue['time_ms'], cue['text']) for cue in cues])
            return self.track(track_id, owner)
        if audio is None:
            raise MusicError('请选择音乐文件')
        info = probe_audio(audio)
        guessed_title, guessed_artist = filename_metadata(filename)
        lyric_title, lyric_artist = filename_metadata(lyric_filename)
        final_title = clean_text(title) or info['title'] or tags.get('ti') or guessed_title or '未命名歌曲'
        # The paired lyric filename can fill artist only if it describes this title.
        final_artist = clean_text(artist) or info['artist'] or tags.get('ar') or guessed_artist
        if not final_artist and lyric_title == final_title:
            final_artist = lyric_artist
        identifier = secrets.token_hex(16)
        directory = self.root / identifier
        directory.mkdir()
        size = audio.stat().st_size
        try:
            os.replace(audio, directory / 'source')
            with self.connect() as db:
                db.execute('''INSERT INTO tracks(id,owner,title,artist,filename,format,duration_ms,
                  source_sample_rate,channels,bytes) VALUES (?,?,?,?,?,?,?,?,?,?)''',
                           [identifier, owner, final_title, final_artist, clean_text(Path(filename.replace('\\','/')).name),
                            info['format'], info['duration_ms'], info['source_sample_rate'], info['channels'], size])
                db.executemany('INSERT INTO lyrics VALUES (?,?,?)', [(identifier, cue['time_ms'], cue['text']) for cue in cues])
        except BaseException:
            shutil.rmtree(directory)
            raise
        return self.track(identifier, owner)

    def source(self, track_id: str, owner: str | None) -> tuple[Path, dict]:
        metadata = self.track(track_id, owner)
        path = self.root / track_id / 'source'
        if not path.is_file():
            raise MusicError('原始歌曲文件不存在')
        return path, metadata

    def lyrics(self, track_id: str, owner: str | None, cursor: int = 0, limit: int = 100) -> dict:
        track = self.track(track_id, owner)
        with self.connect() as db:
            count = db.execute('SELECT count(*) FROM lyrics WHERE track_id=?', [track_id]).fetchone()[0]
            rows = db.execute('SELECT time_ms,text FROM lyrics WHERE track_id=? ORDER BY time_ms LIMIT ? OFFSET ?',
                              [track_id, limit, cursor]).fetchall()
        # Long Unicode lyrics can exceed the device's 64 KiB JSON budget.
        # Return fewer cues with an exact cursor; keep the full stored lyrics.
        page, used = [], 0
        for row in rows:
            cue = dict(row)
            size = len(json.dumps(cue, ensure_ascii=False).encode('utf-8')) + 1
            if page and used + size > 56 * 1024:
                break
            page.append(cue)
            used += size
        following = cursor + len(page)
        return {'title': track['title'], 'artist': track['artist'], 'cues': page,
                'total_cues': count, 'next_cursor': following if following < count else None}

    def delete(self, track_id: str, owner: str | None):
        track = self.track(track_id, owner)
        if owner is not None and track['owner'] != owner:
            raise MusicError('只能删除自己的歌曲')
        with self.connect() as db:
            db.execute('DELETE FROM tracks WHERE id=?', [track_id])
        shutil.rmtree(self.root / track_id)

    def playlists(self, owner: str | None) -> list[dict]:
        with self.connect() as db:
            rows = db.execute('''SELECT p.id,p.title,p.owner,count(pt.track_id) count FROM playlists p
              LEFT JOIN playlist_tracks pt ON p.id=pt.playlist_id WHERE (? IS NULL OR p.owner=?)
              GROUP BY p.id ORDER BY p.created_at,p.id''', [owner, owner]).fetchall()
        return [dict(row) for row in rows]

    def create_playlist(self, owner: str, title: str) -> dict:
        title = clean_text(title, 80)
        if not title:
            raise MusicError('歌单名称不能为空')
        identifier = secrets.token_hex(16)
        with self.connect() as db:
            db.execute('INSERT INTO playlists(id,owner,title) VALUES (?,?,?)', [identifier, owner, title])
        return dict(id=identifier, title=title, owner=owner, count=0)

    def playlist(self, playlist_id: str, owner: str | None, cursor: int = 0, limit: int = 20) -> dict:
        visibility, args = self.visible(owner)
        with self.connect() as db:
            entry = db.execute('SELECT id,title,owner FROM playlists WHERE id=? AND (? IS NULL OR owner=?)',
                               [playlist_id, owner, owner]).fetchone()
            if not entry:
                raise MusicError('歌单不存在或不可访问')
            rows = db.execute(f'''SELECT t.*, EXISTS(SELECT 1 FROM lyrics l WHERE l.track_id=t.id) lyrics_available
               FROM playlist_tracks pt JOIN tracks t ON t.id=pt.track_id
               WHERE pt.playlist_id=? AND {visibility} ORDER BY pt.position, t.id LIMIT ? OFFSET ?''',
                              [playlist_id, *args, limit + 1, cursor]).fetchall()
        return {**dict(entry), 'tracks': [self.metadata(row) for row in rows[:limit]],
                'next_cursor': cursor + limit if len(rows) > limit else None}

    def add_to_playlist(self, playlist_id: str, track_id: str, owner: str):
        self.playlist(playlist_id, owner, limit=1)
        self.track(track_id, owner)
        with self.connect() as db:
            db.execute('''INSERT OR IGNORE INTO playlist_tracks VALUES(?,?,
              (SELECT coalesce(max(position),0)+1 FROM playlist_tracks WHERE playlist_id=?))''',
                       [playlist_id, track_id, playlist_id])

    def remove_from_playlist(self, playlist_id: str, track_id: str, owner: str):
        self.playlist(playlist_id, owner, limit=1)
        with self.connect() as db:
            db.execute('DELETE FROM playlist_tracks WHERE playlist_id=? AND track_id=?', [playlist_id, track_id])

    def queue(self, owner: str, playlist_id: str = '', current: str = '', direction: str = 'next') -> dict:
        visibility, args = self.visible(owner)
        with self.connect() as db:
            if playlist_id:
                self.playlist(playlist_id, owner, limit=1)
                rows = db.execute(f'''SELECT t.id FROM playlist_tracks pt JOIN tracks t ON t.id=pt.track_id
                    WHERE pt.playlist_id=? AND {visibility} ORDER BY pt.position,t.id''', [playlist_id, *args]).fetchall()
            else:
                rows = db.execute(f'SELECT t.id FROM tracks t WHERE {visibility} ORDER BY t.created_at,t.id', args).fetchall()
        if not rows:
            raise MusicError('歌单中没有可播放的歌曲')
        identifiers = [row['id'] for row in rows]
        if current in identifiers:
            index = (identifiers.index(current) + (-1 if direction == 'previous' else 1)) % len(identifiers)
        else:
            index = len(identifiers) - 1 if direction == 'previous' else 0
        return {'track': self.track(identifiers[index], owner)}

    def claim_stream(self, owner: str) -> str:
        with self._stream_lock:
            if owner in self._streams or len(self._streams) >= 4:
                raise MusicError('音乐流正在使用，请停止现有播放后重试')
            token = secrets.token_hex(16)
            self._streams[owner] = token
            return token

    def release_stream(self, owner: str, token: str) -> None:
        with self._stream_lock:
            if self._streams.get(owner) == token:
                del self._streams[owner]

    async def pcm(self, path: Path, track: dict, offset_bytes: int):
        # Seek slightly early to prime MP3 decoder and resampler, then discard pre-roll.
        seconds = offset_bytes / (SAMPLE_RATE * 2)
        earlier = max(0, seconds - .25)
        command = ['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-threads', '1',
                   '-filter_threads', '1', '-filter_complex_threads', '1',
                   '-protocol_whitelist', 'file', '-f', track['format']]
        if earlier:
            command += ['-ss', f'{earlier:.9f}']
        command += ['-i', str(path)]
        if seconds > earlier:
            command += ['-ss', f'{seconds-earlier:.9f}']
        command += ['-map', '0:a:0', '-vn', '-ac', '1', '-ar', str(SAMPLE_RATE),
                    '-c:a', 'pcm_s16le', '-f', 's16le', 'pipe:1']
        process = None
        try:
            # Know the process handle before cancellation can interrupt streaming setup.
            with anyio.CancelScope(shield=True):
                process = await asyncio.create_subprocess_exec(*command, stdout=asyncio.subprocess.PIPE,
                                                               stderr=asyncio.subprocess.DEVNULL, limit=16384)
            while True:
                chunk = await process.stdout.read(8192)
                if not chunk:
                    break
                yield chunk
            if await process.wait():
                raise RuntimeError('音频解码失败')
        finally:
            if process is not None:
                # ASGI disconnect cancels the consumer. Shield bounded cleanup, drain the
                # pipe after stopping FFmpeg so a full stdout buffer cannot deadlock wait().
                with anyio.CancelScope(shield=True):
                    if process.returncode is None:
                        try:
                            process.terminate()
                        except ProcessLookupError:
                            pass

                    async def reap():
                        while await process.stdout.read(8192):
                            pass
                        await process.wait()

                    try:
                        await asyncio.wait_for(reap(), 2)
                    except asyncio.TimeoutError:
                        if process.returncode is None:
                            process.kill()
                        await asyncio.wait_for(reap(), 2)


class UploadTickets:
    """Short-lived capability bound to an exact browser origin; consume once before body."""
    def __init__(self):
        self.lock = threading.Lock()
        self.entries: dict[str, tuple] = {}

    def issue(self, owner: str, origin: str, track_id: str = '') -> str:
        parsed = urlsplit(origin)
        if parsed.scheme not in ('http', 'https') or not parsed.netloc or parsed.username or parsed.password or parsed.path or parsed.query or parsed.fragment:
            raise MusicError('上传页面来源无效')
        with self.lock:
            now = time.monotonic()
            self.entries = {key: value for key, value in self.entries.items() if value[0] > now and value[1] != owner}
            token = secrets.token_urlsafe(32)
            self.entries[token] = (now + 600, owner, origin, track_id)
            return token

    def allows_origin(self, origin: str) -> bool:
        with self.lock:
            return any(value[0] > time.monotonic() and value[2] == origin for value in self.entries.values())

    def consume(self, token: str, origin: str) -> tuple[str, str]:
        with self.lock:
            value = self.entries.get(token)
            if not value or value[0] <= time.monotonic() or value[2] != origin:
                raise MusicError('上传凭据无效或已过期，请重新选择上传')
            del self.entries[token]
        return value[1], value[3]
