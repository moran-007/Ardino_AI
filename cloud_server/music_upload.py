"""Incremental multipart upload: no whole-song body or RAM copy, no fixed audio size cap."""
from __future__ import annotations

import os
import shutil
import tempfile
from pathlib import Path

from python_multipart import MultipartParser
from python_multipart.multipart import parse_options_header
from starlette.concurrency import run_in_threadpool

from .music_library import DISK_RESERVE, LYRICS_LIMIT, MusicError


class DiskUpload:
    def __init__(self, root: Path, content_type: str):
        kind, options = parse_options_header(content_type)
        boundary = options.get(b'boundary', b'')
        if kind != b'multipart/form-data' or not boundary or len(boundary) > 200:
            raise MusicError('上传需要 multipart/form-data')
        self.directory = Path(tempfile.mkdtemp(prefix='upload-', dir=root / 'staging'))
        self.files, self.filenames, self.fields = {}, {}, {}
        self.stream = None
        self.part_count = self.total_bytes = self.disk_checked = 0
        self.done = False
        callbacks = {name: getattr(self, name) for name in ('on_part_begin', 'on_header_field', 'on_header_value',
                    'on_header_end', 'on_headers_finished', 'on_part_data', 'on_part_end', 'on_end')}
        self.parser = MultipartParser(boundary, callbacks)

    def on_part_begin(self):
        self.part_count += 1
        if self.part_count > 8:
            raise MusicError('上传表单项目过多')
        self.headers, self.header_field, self.header_value = {}, bytearray(), bytearray()
        self.name, self.value, self.part_bytes, self.header_bytes = '', bytearray(), 0, 0

    def header_append(self, target, data, start, end):
        self.header_bytes += end - start
        if self.header_bytes > 4096:
            raise MusicError('文件名或表单头过长')
        target.extend(data[start:end])

    def on_header_field(self, data, start, end):
        self.header_append(self.header_field, data, start, end)

    def on_header_value(self, data, start, end):
        self.header_append(self.header_value, data, start, end)

    def on_header_end(self):
        self.headers[bytes(self.header_field).lower()] = bytes(self.header_value)
        self.header_field.clear()
        self.header_value.clear()

    def on_headers_finished(self):
        kind, options = parse_options_header(self.headers.get(b'content-disposition', b''))
        if kind != b'form-data':
            raise MusicError('表单文件描述无效')
        self.name = options.get(b'name', b'').decode('ascii', errors='replace')
        if self.name not in {'audio', 'lyrics', 'csrf', 'title', 'artist', 'track_id'} or self.name in self.files or self.name in self.fields:
            raise MusicError('表单项目无效或重复')
        if b'filename' in options:
            filename = options[b'filename'].decode('utf-8', errors='replace')
            if self.name not in {'audio', 'lyrics'}:
                raise MusicError('文件项目无效')
            # Browsers send empty optional file inputs; ignore them at part_end.
            self.filenames[self.name] = filename
            path = self.directory / self.name
            self.files[self.name] = path
            self.stream = path.open('wb')
        elif self.name in {'audio', 'lyrics'}:
            raise MusicError('请选择实际文件')

    def on_part_data(self, data, start, end):
        length = end - start
        self.total_bytes += length
        self.part_bytes += length
        if self.stream:
            if self.name == 'lyrics' and self.part_bytes > LYRICS_LIMIT:
                raise MusicError('歌词不能超过 1 MB')
            if self.total_bytes - self.disk_checked >= 1024 * 1024:
                if shutil.disk_usage(self.directory).free < DISK_RESERVE + length:
                    raise MusicError('服务器存储空间不足，请清理音乐后重试')
                self.disk_checked = self.total_bytes
            self.stream.write(memoryview(data)[start:end])
        else:
            if self.part_bytes > 4096:
                raise MusicError('表单文字过长')
            self.value.extend(data[start:end])

    def on_part_end(self):
        if self.stream:
            self.stream.close()
            self.stream = None
            if not self.filenames[self.name] and self.part_bytes == 0:
                self.files.pop(self.name).unlink()
                self.filenames.pop(self.name)
        else:
            try:
                self.fields[self.name] = self.value.decode('utf-8')
            except UnicodeError:
                raise MusicError('表单文字编码无效') from None

    def on_end(self):
        self.done = True

    async def receive(self, request):
        if shutil.disk_usage(self.directory).free < DISK_RESERVE:
            raise MusicError('服务器存储空间不足')
        async for chunk in request.stream():
            if chunk:
                # Bound callback copies and disk writes even if ASGI provides a large chunk.
                for offset in range(0, len(chunk), 65536):
                    await run_in_threadpool(self.parser.write, chunk[offset:offset + 65536])
        await run_in_threadpool(self.parser.finalize)
        if not self.done:
            raise MusicError('上传未完整结束，请重新上传')
        return self

    def close(self):
        if self.stream:
            self.stream.close()
        shutil.rmtree(self.directory)
