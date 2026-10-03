from __future__ import annotations

import hmac
import html
from typing import Literal

from fastapi import Depends, HTTPException, Query, Request
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, StreamingResponse
from pydantic import BaseModel, Field
from starlette.background import BackgroundTask
from starlette.concurrency import run_in_threadpool

from .admin_views import csrf_input, heading, page_shell
from .music_library import MusicError, SAMPLE_RATE, UploadTickets
from .music_upload import DiskUpload


class PlaylistName(BaseModel):
    title: str = Field(min_length=1, max_length=80)


class PlaylistTrack(BaseModel):
    track_id: str = Field(pattern=r'^[a-f0-9]{32}$')


class TicketRequest(BaseModel):
    origin: str = Field(min_length=1, max_length=300)
    track_id: str = Field(default='', max_length=32)


def music_error(exc: MusicError, code: int = 422):
    return HTTPException(code, str(exc))


async def import_form(library, request, owner: str, required_track: str = ''):
    upload = DiskUpload(library.root, request.headers.get('content-type', ''))
    try:
        await upload.receive(request)
        track_id = upload.fields.get('track_id', '')
        if required_track and track_id != required_track:
            raise MusicError('歌词上传目标与凭据不一致')
        if not required_track and track_id:
            # A general upload ticket cannot be reused to edit a different song.
            raise MusicError('补充歌词请从该歌曲重新申请上传')
        lyrics_path = upload.files.get('lyrics')
        lyrics = lyrics_path.read_bytes() if lyrics_path else None
        return await run_in_threadpool(library.import_upload, upload.files.get('audio'),
                                      upload.filenames.get('audio', ''), lyrics, upload.filenames.get('lyrics', ''),
                                      owner, upload.fields.get('title', ''), upload.fields.get('artist', ''), track_id)
    finally:
        await run_in_threadpool(upload.close)


def install_music_api(app, authenticated_device):
    library = app.state.music_library
    tickets = app.state.music_upload_tickets = UploadTickets()

    @app.get('/voice-api/v1/music')
    def catalog(q: str = Query(default='', max_length=160), cursor: int = Query(default=0, ge=0),
                limit: int = Query(default=20, ge=1, le=100), owner: str = Depends(authenticated_device)):
        return library.catalog(owner, q, cursor, limit)

    @app.get('/voice-api/v1/music/queue')
    def queue(playlist_id: str = Query(default='', max_length=32), current: str = Query(default='', max_length=32),
              direction: Literal['next', 'previous'] = 'next', owner: str = Depends(authenticated_device)):
        try:
            return library.queue(owner, playlist_id, current, direction)
        except MusicError as exc:
            raise music_error(exc, 404) from exc

    @app.get('/voice-api/v1/music/playlists')
    def playlists(owner: str = Depends(authenticated_device)):
        return {'playlists': library.playlists(owner)}

    @app.post('/voice-api/v1/music/playlists', status_code=201)
    def create_playlist(payload: PlaylistName, owner: str = Depends(authenticated_device)):
        try:
            return library.create_playlist(owner, payload.title)
        except MusicError as exc:
            raise music_error(exc) from exc

    @app.get('/voice-api/v1/music/playlists/{playlist_id}')
    def playlist(playlist_id: str, cursor: int = Query(default=0, ge=0),
                 limit: int = Query(default=20, ge=1, le=100), owner: str = Depends(authenticated_device)):
        try:
            return library.playlist(playlist_id, owner, cursor, limit)
        except MusicError as exc:
            raise music_error(exc, 404) from exc

    @app.delete('/voice-api/v1/music/playlists/{playlist_id}')
    def delete_playlist(playlist_id: str, owner: str = Depends(authenticated_device)):
        try:
            library.playlist(playlist_id, owner, limit=1)
            with library.connect() as db:
                db.execute('DELETE FROM playlists WHERE id=? AND owner=?', [playlist_id, owner])
            return {'ok': True}
        except MusicError as exc:
            raise music_error(exc, 404) from exc

    @app.post('/voice-api/v1/music/playlists/{playlist_id}/tracks')
    def add_track(playlist_id: str, payload: PlaylistTrack, owner: str = Depends(authenticated_device)):
        try:
            library.add_to_playlist(playlist_id, payload.track_id, owner)
            return {'ok': True}
        except MusicError as exc:
            raise music_error(exc, 404) from exc

    @app.delete('/voice-api/v1/music/playlists/{playlist_id}/tracks/{track_id}')
    def remove_track(playlist_id: str, track_id: str, owner: str = Depends(authenticated_device)):
        try:
            library.remove_from_playlist(playlist_id, track_id, owner)
            return {'ok': True}
        except MusicError as exc:
            raise music_error(exc, 404) from exc

    @app.post('/voice-api/v1/music/upload-tickets')
    def ticket(payload: TicketRequest, owner: str = Depends(authenticated_device)):
        try:
            if payload.track_id:
                track = library.track(payload.track_id, owner)
                if track['owner'] != owner:
                    raise MusicError('只能修改自己的歌曲')
            token = tickets.issue(owner, payload.origin, payload.track_id)
        except MusicError as exc:
            raise music_error(exc) from exc
        base = app.state.effective_settings.firmware_public_base_url.rstrip('/')
        return {'url': base + '/voice-api/v1/music/browser-upload', 'token': token, 'expires_in': 600}

    @app.options('/voice-api/v1/music/browser-upload')
    def upload_preflight(request: Request):
        origin = request.headers.get('origin', '')
        if not tickets.allows_origin(origin):
            raise HTTPException(403, '上传页面来源未授权')
        return JSONResponse({}, headers={'Access-Control-Allow-Origin': origin,
                            'Access-Control-Allow-Methods': 'POST', 'Access-Control-Allow-Headers': 'X-Music-Upload-Token',
                            'Access-Control-Max-Age': '0', 'Vary': 'Origin'})

    @app.post('/voice-api/v1/music/browser-upload', status_code=201)
    async def browser_upload(request: Request):
        origin = request.headers.get('origin', '')
        # Only a live ticket enables CORS. Invalid requests receive no permissive CORS header.
        try:
            owner, track_id = tickets.consume(request.headers.get('x-music-upload-token', ''), origin)
        except MusicError as exc:
            raise music_error(exc, 403) from exc
        headers = {'Access-Control-Allow-Origin': origin, 'Vary': 'Origin', 'Cache-Control': 'no-store'}
        try:
            result = await import_form(library, request, owner, track_id)
        except MusicError as exc:
            return JSONResponse({'detail': str(exc)}, status_code=422, headers=headers)
        except OSError:
            return JSONResponse({'detail': '服务器存储写入失败，请稍后重新上传'}, status_code=507, headers=headers)
        return JSONResponse(result, status_code=201, headers=headers)

    @app.get('/voice-api/v1/music/{track_id}/lyrics')
    def lyrics(track_id: str, cursor: int = Query(default=0, ge=0), limit: int = Query(default=100, ge=1, le=100),
               owner: str = Depends(authenticated_device)):
        try:
            return library.lyrics(track_id, owner, cursor, limit)
        except MusicError as exc:
            raise music_error(exc, 404) from exc

    @app.get('/voice-api/v1/music/{track_id}/pcm')
    def pcm(track_id: str, offset_bytes: int = Query(default=0, ge=0, le=2**63-2),
            owner: str = Depends(authenticated_device)):
        if offset_bytes % 2:
            raise HTTPException(422, 'PCM 偏移必须是完整 16 位采样')
        try:
            path, track = library.source(track_id, owner)
            lease = library.claim_stream(owner)
        except MusicError as exc:
            raise music_error(exc, 429 if '音乐流' in str(exc) else 404) from exc

        async def body():
            try:
                async for chunk in library.pcm(path, track, offset_bytes):
                    yield chunk
            finally:
                library.release_stream(owner, lease)
        return StreamingResponse(body(), media_type='application/octet-stream',
                                 headers={'Cache-Control': 'private, no-store', 'X-Audio-Sample-Rate': str(SAMPLE_RATE),
                                          'X-Audio-Channels': '1', 'X-Audio-Bits': '16', 'X-Accel-Buffering': 'no'},
                                 background=BackgroundTask(library.release_stream, owner, lease))

    @app.delete('/voice-api/v1/music/{track_id}')
    def delete(track_id: str, owner: str = Depends(authenticated_device)):
        try:
            library.delete(track_id, owner)
            return {'ok': True}
        except MusicError as exc:
            raise music_error(exc, 404) from exc

    @app.get('/voice-api/v1/music/{track_id}')
    def track(track_id: str, owner: str = Depends(authenticated_device)):
        try:
            return library.track(track_id, owner)
        except MusicError as exc:
            raise music_error(exc, 404) from exc


def install_music_admin(app, require_session, response_headers):
    library = app.state.music_library

    @app.get('/admin/music')
    def music_page(request: Request, q: str = Query(default='', max_length=160), cursor: int = Query(default=0, ge=0)):
        _, csrf = require_session(request)
        result = library.catalog(None, q, cursor, 20)
        escape = html.escape
        content = heading('音乐库', '完整歌曲、同步歌词与用户歌单；管理员可查看所有歌曲。')
        content += f'''<form id="song-upload" enctype="multipart/form-data">
          {csrf_input(csrf)}<label>音乐（MP3 / WAV）<input type="file" name="audio" accept=".mp3,.wav,audio/mpeg,audio/wav" required></label>
          <label>歌词（可选 LRC）<input type="file" name="lyrics" accept=".lrc,text/plain"></label>
          <label>歌曲名（可留空自动识别）<input name="title" maxlength="160"></label>
          <label>歌手（可留空自动识别）<input name="artist" maxlength="160"></label>
          <button>上传到共用音乐库</button><progress id="song-progress" max="100" value="0"></progress>
          <p id="song-result" role="status"></p><p class="muted">完整音频不设固定大小或时长上限，受服务器可用存储和网络影响；歌词最多 1 MB。</p></form>
          <form method="get"><label>查找歌曲 / 歌手<input name="q" value="{escape(q, quote=True)}"></label><button>搜索</button></form>
          <div class="table-wrap"><table><thead><tr><th>歌曲</th><th>歌手</th><th>归属</th><th>时长</th><th>文件</th><th>歌词 / 操作</th></tr></thead><tbody>'''
        for track in result['tracks']:
            identifier = track['id']
            duration = track['duration_ms'] // 1000
            content += f'''<tr><td>{escape(track['title'])}</td><td>{escape(track['artist'])}</td>
              <td>{escape(track['owner']) or '共用'}</td><td>{duration//60}:{duration%60:02d}</td>
              <td>{track['bytes']/1048576:.2f} MB · {escape(track['format'])}<br>
              <audio controls preload="none" src="/admin/music/{identifier}/source"></audio></td><td>
              <a href="/admin/music/{identifier}/lyrics">{'查看歌词' if track['lyrics_available'] else '无歌词'}</a>
              <form class="lyric-upload" data-track="{identifier}"><input type="file" name="lyrics" accept=".lrc" required>
              <input type="hidden" name="track_id" value="{identifier}"><button>补充歌词</button></form>
              <form method="post" action="/admin/music/{identifier}/delete">{csrf_input(csrf)}<button>删除歌曲</button></form></td></tr>'''
        content += '</tbody></table></div>'
        if result['next_cursor'] is not None:
            from urllib.parse import urlencode
            content += '<p><a href="/admin/music?' + escape(urlencode({'q': q, 'cursor': result['next_cursor']}), quote=True) + '">下一页</a></p>'
        content += '<h2>用户歌单</h2><div class="table-wrap"><table><tr><th>歌单</th><th>归属</th><th>歌曲数</th></tr>'
        for playlist in library.playlists(None):
            content += f'<tr><td><a href="/admin/music/playlists/{playlist["id"]}">{escape(playlist["title"])}</a></td><td>{escape(playlist["owner"])}</td><td>{playlist["count"]}</td></tr>'
        content += '''</table></div><script>
        (()=>{
          const csrf=document.querySelector('input[name=csrf]').value;
          function send(form,url){
            const xhr=new XMLHttpRequest(),data=new FormData(form),button=form.querySelector('button');
            button.disabled=true;xhr.open('POST',url);xhr.setRequestHeader('X-CSRF-Token',csrf);
            xhr.upload.onprogress=e=>{if(e.lengthComputable)document.getElementById('song-progress').value=e.loaded/e.total*100};
            xhr.onload=()=>{button.disabled=false;let d={};try{d=JSON.parse(xhr.responseText)}catch{}
              document.getElementById('song-result').textContent=xhr.status<300?'上传成功，请刷新查看':(d.detail||'上传失败');};
            xhr.onerror=()=>{button.disabled=false;document.getElementById('song-result').textContent='网络中断，请重新上传'};
            xhr.send(data);
          }
          document.getElementById('song-upload').onsubmit=e=>{e.preventDefault();send(e.target,'/admin/music/upload')};
          document.querySelectorAll('.lyric-upload').forEach(form=>form.onsubmit=e=>{e.preventDefault();send(form,'/admin/music/upload')});
        })();</script>'''
        return response_headers(HTMLResponse(page_shell(content, '音乐库', active='music', csrf=csrf)))

    @app.post('/admin/music/upload', status_code=201)
    async def admin_upload(request: Request):
        _, csrf = require_session(request)
        if not hmac.compare_digest(request.headers.get('x-csrf-token', ''), csrf):
            raise HTTPException(403, '表单校验失败')
        upload = None
        try:
            upload = DiskUpload(library.root, request.headers.get('content-type', ''))
            await upload.receive(request)
            identifier = upload.fields.get('track_id', '')
            if identifier:
                # Administrators may attach lyrics to any owner's song; keep its ownership.
                owner = library.track(identifier, None)['owner']
            else:
                owner = ''
            lyrics_path = upload.files.get('lyrics')
            result = await run_in_threadpool(library.import_upload, upload.files.get('audio'),
                                             upload.filenames.get('audio', ''), lyrics_path.read_bytes() if lyrics_path else None,
                                             upload.filenames.get('lyrics', ''), owner, upload.fields.get('title', ''),
                                             upload.fields.get('artist', ''), identifier)
            return JSONResponse(result, status_code=201)
        except MusicError as exc:
            raise music_error(exc) from exc
        except OSError:
            raise HTTPException(507, '服务器存储写入失败') from None
        finally:
            if upload is not None:
                await run_in_threadpool(upload.close)

    @app.get('/admin/music/playlists/{playlist_id}')
    def admin_playlist(playlist_id: str, request: Request, cursor: int = Query(default=0, ge=0)):
        require_session(request)
        try:
            return library.playlist(playlist_id, None, cursor, 100)
        except MusicError as exc:
            raise music_error(exc, 404) from exc

    @app.get('/admin/music/{track_id}/source')
    def source(track_id: str, request: Request):
        require_session(request)
        try:
            path, track = library.source(track_id, None)
            return FileResponse(path, media_type='audio/mpeg' if track['format'] == 'mp3' else 'audio/wav',
                                headers={'Cache-Control': 'private, no-store'})
        except MusicError as exc:
            raise music_error(exc, 404) from exc

    @app.get('/admin/music/{track_id}/lyrics')
    def admin_lyrics(track_id: str, request: Request, cursor: int = Query(default=0, ge=0)):
        require_session(request)
        try:
            return library.lyrics(track_id, None, cursor, 100)
        except MusicError as exc:
            raise music_error(exc, 404) from exc

    @app.post('/admin/music/{track_id}/delete')
    async def admin_delete(track_id: str, request: Request):
        _, csrf = require_session(request)
        # Tiny URL-encoded action only; do not allocate an arbitrary request body.
        from urllib.parse import parse_qs
        raw = bytearray()
        async for chunk in request.stream():
            if len(raw) + len(chunk) > 4096:
                raise HTTPException(413, '表单过长')
            raw.extend(chunk)
        values = parse_qs(raw.decode('utf-8'))
        if not hmac.compare_digest(values.get('csrf', [''])[0], csrf):
            raise HTTPException(403, '表单校验失败')
        try:
            await run_in_threadpool(library.delete, track_id, None)
        except MusicError as exc:
            raise music_error(exc, 404) from exc
        from fastapi.responses import RedirectResponse
        return RedirectResponse('/admin/music', status_code=303)
