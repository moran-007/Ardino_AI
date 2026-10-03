from __future__ import annotations

import asyncio
import io
import json
import math
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import wave
from pathlib import Path
from unittest.mock import patch

from fastapi.testclient import TestClient
from cryptography.fernet import Fernet

from cloud_server.admin import hash_admin_password
from cloud_server.app import create_app
from cloud_server.config import Settings
from cloud_server.music_library import MusicError, MusicLibrary, UploadTickets, parse_lyrics
from cloud_server.music_upload import DiskUpload


def wav_payload(frames=4800, rate=48000, frequency=440):
    target = io.BytesIO()
    with wave.open(target, 'wb') as sound:
        sound.setparams((1, 2, rate, 0, 'NONE', 'not compressed'))
        sound.writeframes(b''.join(struct.pack('<h', round(12000*math.sin(2*math.pi*frequency*i/rate))) for i in range(frames)))
    return target.getvalue()


class LyricsTest(unittest.TestCase):
    def test_multiple_timestamps_same_line_fraction_offset_and_export_suffix(self):
        raw = '[ti:标题][ar:歌手][offset:-100]\n[00:01.2][00:02.34]第一句[00:03.456]第二句/name/private.lrc'.encode()
        cues, tags = parse_lyrics(raw)
        self.assertEqual(cues, [{'time_ms': 1100, 'text': '第一句'}, {'time_ms': 2240, 'text': '第一句'},
                                {'time_ms': 3356, 'text': '第二句'}])
        self.assertEqual(tags['ti'], '标题')
        self.assertEqual(tags['ar'], '歌手')

    def test_utf8_bom_utf16_and_legacy_encoding(self):
        text = '[00:00.00]你好\n[00:12.01]歌词'
        expected = [{'time_ms': 0, 'text': '你好'}, {'time_ms': 12010, 'text': '歌词'}]
        for encoding in ('utf-8-sig', 'utf-16', 'gb18030'):
            self.assertEqual(parse_lyrics(text.encode(encoding))[0], expected)

    def test_empty_cue_dedup_sort_and_no_time_rejection(self):
        self.assertEqual(parse_lyrics(b'[00:03]third\n[00:01]first\n[00:02]\n[00:03]last')[0],
                         [{'time_ms': 1000, 'text': 'first'}, {'time_ms': 2000, 'text': ''}, {'time_ms': 3000, 'text': 'last'}])
        for payload in (b'', b'plain lyric', b'[offset:no][00:01]line', b'x'*(1024*1024+1)):
            with self.assertRaises(MusicError):
                parse_lyrics(payload)

    def test_tickets_origin_expiry_one_use_and_wrong_origin_does_not_consume(self):
        tickets = UploadTickets()
        for origin in ('null', 'file://x', 'http://user:pass@host', 'http://host/path', 'https://host?a=1'):
            with self.assertRaises(MusicError):
                tickets.issue('a', origin)
        token = tickets.issue('a', 'http://192.168.3.247')
        with self.assertRaises(MusicError):
            tickets.consume(token, 'http://evil')
        self.assertEqual(tickets.consume(token, 'http://192.168.3.247'), ('a', ''))
        with self.assertRaises(MusicError):
            tickets.consume(token, 'http://192.168.3.247')
        token = tickets.issue('a', 'http://host')
        with patch('cloud_server.music_library.time.monotonic', return_value=10**15):
            self.assertFalse(tickets.allows_origin('http://host'))
            with self.assertRaises(MusicError):
                tickets.consume(token, 'http://host')

    def test_stream_leases_do_not_allow_old_cleanup_to_release_new_stream(self):
        with tempfile.TemporaryDirectory() as root:
            library = MusicLibrary(Path(root))
            old = library.claim_stream('a')
            with self.assertRaises(MusicError):
                library.claim_stream('a')
            library.release_stream('a', old)
            newer = library.claim_stream('a')
            library.release_stream('a', old)
            with self.assertRaises(MusicError):
                library.claim_stream('a')
            for name in ('b', 'c', 'd'):
                library.claim_stream(name)
            with self.assertRaises(MusicError):
                library.claim_stream('e')
            library.release_stream('a', newer)


class MusicApiTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        settings = Settings.for_test(Path(self.temp.name), admin_password_hash=hash_admin_password('player-test-password'),
                                     config_encryption_key=Fernet.generate_key().decode())
        self.client = TestClient(create_app(settings))
        self.library = self.client.app.state.music_library
        for identifier in ('music-a', 'music-b'):
            self.client.app.state.store.register_device(identifier, identifier, identifier+'-token')
        self.auth = {'Authorization': 'Bearer music-a-token', 'X-Device-ID': 'music-a'}
        self.other = {'Authorization': 'Bearer music-b-token', 'X-Device-ID': 'music-b'}
        self.origin = 'http://192.168.3.247'
        # On the deployment host the actual FFprobe is used. Windows metadata-only cases
        # still exercise the real incremental parser, DB, routes and ownership checks.
        self.probe_patch = None
        if not shutil.which('ffprobe'):
            self.probe_patch = patch('cloud_server.music_library.probe_audio', return_value=dict(
                format='wav', duration_ms=100, source_sample_rate=48000, channels=1, title='', artist=''))
            self.probe_patch.start()

    def tearDown(self):
        if self.probe_patch:
            self.probe_patch.stop()
        self.client.close()
        self.temp.cleanup()

    def ticket(self, track_id=''):
        result = self.client.post('/voice-api/v1/music/upload-tickets', headers=self.auth,
                                  json={'origin': self.origin, 'track_id': track_id})
        self.assertEqual(result.status_code, 200, result.text)
        return result.json()['token']

    def upload(self, title='', lyrics=None, audio=None, track_id='', filename='track.wav'):
        files = {}
        if not track_id:
            files['audio'] = (filename, audio or wav_payload(), 'audio/wav')
        if lyrics is not None:
            files['lyrics'] = ('track.lrc', lyrics, 'text/plain')
        return self.client.post('/voice-api/v1/music/browser-upload',
                    headers={'Origin': self.origin, 'X-Music-Upload-Token': self.ticket(track_id)},
                    data={'title': title, 'track_id': track_id}, files=files)

    def unlock(self):
        result = self.client.post('/admin/unlock', data={'password': 'player-test-password'})
        self.assertEqual(result.status_code, 200)
        return re.search(r'name="csrf" value="([^"]+)"', result.text).group(1)

    def test_authentication_catalog_paging_and_admin_all_visibility(self):
        self.assertEqual(self.client.get('/voice-api/v1/music').status_code, 401)
        first = self.upload(title='<script>private</script>', lyrics=b'[00:01]one')
        self.assertEqual(first.status_code, 201, first.text)
        second = self.upload(title='second')
        self.assertEqual(second.status_code, 201, second.text)
        identifier = first.json()['id']
        self.assertEqual(self.client.get('/voice-api/v1/music', headers=self.other).json()['tracks'], [])
        for suffix in ('', '/lyrics', '/pcm'):
            self.assertEqual(self.client.get('/voice-api/v1/music/'+identifier+suffix, headers=self.other).status_code, 404)
        paged = self.client.get('/voice-api/v1/music?limit=1', headers=self.auth).json()
        self.assertEqual(len(paged['tracks']), 1)
        self.assertEqual(paged['next_cursor'], 1)
        self.assertIsNone(self.client.get('/voice-api/v1/music?limit=1&cursor=1', headers=self.auth).json()['next_cursor'])
        self.assertEqual(self.client.get('/admin/music').status_code, 401)
        self.unlock()
        page = self.client.get('/admin/music').text
        self.assertIn('&lt;script&gt;private&lt;/script&gt;', page)
        self.assertNotIn('<script>private</script>', page)
        self.assertIn('music-a', page)
        self.assertEqual(self.client.get('/admin/music/'+identifier+'/source').content, wav_payload())

    def test_general_filename_and_paired_lyric_artist_detection(self):
        token = self.ticket()
        result = self.client.post('/voice-api/v1/music/browser-upload',
                    headers={'Origin': self.origin, 'X-Music-Upload-Token': token},
                    files={'audio': ('Another song.wav', wav_payload(), 'audio/wav'),
                           'lyrics': ('Another song - Artist.lrc', b'[00:01]line', 'text/plain')})
        self.assertEqual(result.status_code, 201, result.text)
        self.assertEqual(result.json()['title'], 'Another song')
        self.assertEqual(result.json()['artist'], 'Artist')
        self.assertEqual(result.json()['sample_rate'], 48000)

    def test_cors_one_use_ticket_and_no_permanent_credential_in_response(self):
        token = self.ticket()
        preflight = self.client.options('/voice-api/v1/music/browser-upload', headers={'Origin': self.origin})
        self.assertEqual(preflight.headers['access-control-allow-origin'], self.origin)
        self.assertEqual(self.client.options('/voice-api/v1/music/browser-upload', headers={'Origin':'http://evil'}).status_code, 403)
        headers = {'Origin': self.origin, 'X-Music-Upload-Token': token}
        first = self.client.post('/voice-api/v1/music/browser-upload', headers=headers,
                                 files={'audio': ('test.wav', wav_payload(), 'audio/wav')})
        self.assertEqual(first.status_code, 201, first.text)
        self.assertEqual(first.headers['access-control-allow-origin'], self.origin)
        self.assertNotIn('music-a-token', first.text)
        self.assertEqual(self.client.post('/voice-api/v1/music/browser-upload', headers=headers).status_code, 403)
        self.assertEqual(list((self.library.root/'staging').iterdir()), [])

    def test_playlist_order_wrap_privacy_dedup_and_delete_cleanup(self):
        identifiers = [self.upload(title=title).json()['id'] for title in ('first', 'second', 'third')]
        playlist = self.client.post('/voice-api/v1/music/playlists', headers=self.auth, json={'title':'Own list'}).json()['id']
        for identifier in reversed(identifiers):
            result = self.client.post(f'/voice-api/v1/music/playlists/{playlist}/tracks', headers=self.auth, json={'track_id':identifier})
            self.assertEqual(result.status_code, 200)
        self.client.post(f'/voice-api/v1/music/playlists/{playlist}/tracks', headers=self.auth, json={'track_id':identifiers[0]})
        self.assertEqual(self.client.get('/voice-api/v1/music/playlists', headers=self.auth).json()['playlists'][0]['count'], 3)
        self.assertEqual(self.client.get(f'/voice-api/v1/music/playlists/{playlist}', headers=self.other).status_code, 404)
        self.assertEqual(self.client.get('/voice-api/v1/music/playlists', headers=self.other).json()['playlists'], [])
        for current, direction, expected in ((identifiers[0], 'next', identifiers[2]), (identifiers[2], 'previous', identifiers[0])):
            response = self.client.get('/voice-api/v1/music/queue', headers=self.auth,
                    params={'playlist_id':playlist, 'current':current, 'direction':direction})
            self.assertEqual(response.json()['track']['id'], expected)
        self.assertEqual(self.client.delete('/voice-api/v1/music/'+identifiers[0], headers=self.other).status_code, 404)
        self.assertEqual(self.client.delete('/voice-api/v1/music/'+identifiers[0], headers=self.auth).status_code, 200)
        self.assertFalse((self.library.root/identifiers[0]).exists())
        self.assertEqual(self.client.get('/voice-api/v1/music/playlists', headers=self.auth).json()['playlists'][0]['count'], 2)
        self.assertEqual(self.client.delete(f'/voice-api/v1/music/playlists/{playlist}', headers=self.auth).status_code, 200)

    def test_lyrics_pagination_attach_owner_check_and_persistence(self):
        identifier = self.upload(title='persist').json()['id']
        lyric = ''.join(f'[{i//60:02}:{i%60:02}.00]line{i}\n' for i in range(120)).encode()
        result = self.upload(track_id=identifier, lyrics=lyric)
        self.assertEqual(result.status_code, 201, result.text)
        page = self.client.get(f'/voice-api/v1/music/{identifier}/lyrics', headers=self.auth).json()
        self.assertEqual(len(page['cues']), 100)
        self.assertEqual(page['next_cursor'], 100)
        self.assertEqual(page['total_cues'], 120)
        self.assertEqual(self.client.get(f'/voice-api/v1/music/{identifier}/lyrics?cursor=100', headers=self.auth).json()['cues'][0]['text'],'line100')
        self.assertEqual(MusicLibrary(self.library.root).track(identifier, 'music-a')['title'], 'persist')
        self.assertTrue(MusicLibrary(self.library.root).track(identifier, 'music-a')['lyrics_available'])
        self.assertEqual(self.client.post('/voice-api/v1/music/upload-tickets', headers=self.other,
                    json={'origin':self.origin,'track_id':identifier}).status_code, 422)

    def test_long_unicode_lyric_pages_fit_device_budget_and_do_not_skip_cues(self):
        lyrics = ''.join(f'[00:{i%60:02}.{i//60:02}]'+'字'*256+'\n' for i in range(120)).encode()
        identifier = self.upload(lyrics=lyrics).json()['id']
        cursor, collected = 0, []
        while cursor is not None:
            response = self.client.get(f'/voice-api/v1/music/{identifier}/lyrics?cursor={cursor}', headers=self.auth)
            self.assertLess(len(response.content),65536)
            page=response.json()
            collected.extend(page['cues'])
            cursor=page['next_cursor']
        self.assertEqual(len(collected),120)
        self.assertTrue(all(len(cue['text'])==256 for cue in collected))

    def test_invalid_upload_cleanup_and_admin_csrf_early_check(self):
        self.assertEqual(self.upload(lyrics=b'no timestamps').status_code, 422)
        self.assertEqual(list((self.library.root/'staging').iterdir()), [])
        csrf = self.unlock()
        self.assertEqual(self.client.post('/admin/music/upload',headers={'X-CSRF-Token':csrf},content=b'not multipart').status_code,422)
        self.assertEqual(self.client.post('/admin/music/upload', files={'audio':('x.wav', wav_payload())}).status_code, 403)
        response = self.client.post('/admin/music/upload', headers={'X-CSRF-Token':csrf},
                                   files={'audio':('../../x.wav', wav_payload())})
        self.assertEqual(response.status_code, 201, response.text)
        self.assertEqual(response.json()['owner'], '')
        shared_id = response.json()['id']
        self.assertEqual(self.client.get('/voice-api/v1/music/'+shared_id, headers=self.other).status_code,200)
        self.assertEqual(self.client.delete('/voice-api/v1/music/'+shared_id, headers=self.auth).status_code,404)

    def test_admin_music_and_billing_allow_only_nonce_scripts(self):
        self.unlock()
        nonces = []
        for url in ('/admin/music', '/admin/billing?period=24h', '/admin/billing?period=7d'):
            response = self.client.get(url)
            nonce = re.search(r'<script nonce="([^"]+)"', response.text).group(1)
            self.assertIn("script-src 'nonce-" + nonce + "'", response.headers['content-security-policy'])
            self.assertIn("connect-src 'self'", response.headers['content-security-policy'])
            self.assertNotIn('<script>', response.text)
            self.assertEqual(int(response.headers['content-length']),len(response.content))
            nonces.append(nonce)
        self.assertEqual(len(set(nonces)),3)

    def test_incremental_truncated_duplicate_form_and_low_disk_cleanup(self):
        async def receive_bytes(raw, content_type):
            class Request:
                async def stream(self):
                    for offset in range(0,len(raw),3):
                        yield raw[offset:offset+3]
            upload = DiskUpload(self.library.root, content_type)
            try:
                await upload.receive(Request())
                return upload.fields.copy()
            finally:
                upload.close()
        body = b'--boundary\r\nContent-Disposition: form-data; name="title"\r\n\r\nvalid\r\n--boundary--\r\n'
        self.assertEqual(asyncio.run(receive_bytes(body,'multipart/form-data; boundary=boundary')), {'title':'valid'})
        with self.assertRaises(MusicError):
            asyncio.run(receive_bytes(body[:-17], 'multipart/form-data; boundary=boundary'))
        duplicate = body.replace(b'--boundary--\r\n', body)
        with self.assertRaises(MusicError):
            asyncio.run(receive_bytes(duplicate,'multipart/form-data; boundary=boundary'))
        with patch('cloud_server.music_upload.shutil.disk_usage', return_value=shutil._ntuple_diskusage(100,99,1)):
            with self.assertRaises(MusicError):
                asyncio.run(receive_bytes(body,'multipart/form-data; boundary=boundary'))
        self.assertEqual(list((self.library.root/'staging').iterdir()), [])

    @unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'Real audio tools required; deployment host runs this')
    def test_actual_pcm_full_song_high_frequency_offset_and_stream_release(self):
        payload = wav_payload(frames=48000, frequency=10000)
        identifier = self.upload(audio=payload, filename='misnamed.mp3').json()['id']
        response = self.client.get(f'/voice-api/v1/music/{identifier}/pcm', headers=self.auth)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.headers['x-audio-sample-rate'], '48000')
        self.assertNotIn('content-length', response.headers)
        self.assertEqual(response.content, payload[44:])
        offset = 48000  # half a second at 48kHz/16bit
        resumed = self.client.get(f'/voice-api/v1/music/{identifier}/pcm?offset_bytes={offset}', headers=self.auth)
        self.assertEqual(resumed.content, payload[44+offset:])
        self.assertEqual(self.client.get(f'/voice-api/v1/music/{identifier}/pcm?offset_bytes=3', headers=self.auth).status_code,422)
        self.assertEqual(self.library._streams,{})

    @unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'Real audio tools required; deployment host runs this')
    def test_over_twenty_megabyte_audio_upload_preserves_original_and_duration(self):
        # A real 21 MiB WAV, longer than a short alarm and valid at 48kHz.
        payload = io.BytesIO()
        with wave.open(payload, 'wb') as sound:
            sound.setparams((1, 2, 48000,0,'NONE','not compressed'))
            for _ in range(21):
                sound.writeframesraw(bytes(1024*1024))
        raw = payload.getvalue()
        result = self.upload(audio=raw)
        self.assertEqual(result.status_code,201,result.text)
        self.assertEqual(result.json()['bytes'],len(raw))
        self.assertGreater(result.json()['duration_ms'],220000)
        path,_ = self.library.source(result.json()['id'],'music-a')
        self.assertEqual(path.stat().st_size,len(raw))

    @unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'Real audio tools required; deployment host runs this')
    def test_actual_mp3_metadata_wrong_extension_and_complete_decode(self):
        source = Path(self.temp.name)/'song.mp3'
        subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','sine=frequency=440:duration=2',
                        '-metadata','title=Embedded title','-metadata','artist=Embedded artist',
                        '-ar','44100','-ac','2','-c:a','libmp3lame',str(source)],check=True)
        result = self.upload(audio=source.read_bytes(),filename='wrong.wav')
        self.assertEqual(result.status_code,201,result.text)
        self.assertEqual(result.json()['format'],'mp3')
        self.assertEqual(result.json()['title'],'Embedded title')
        self.assertEqual(result.json()['artist'],'Embedded artist')
        response = self.client.get('/voice-api/v1/music/'+result.json()['id']+'/pcm',headers=self.auth)
        self.assertEqual(len(response.content),192000)
        self.assertEqual(self.library._streams,{})

    @unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'Real audio tools required; deployment host runs this')
    def test_actual_stream_disconnect_stops_ffmpeg_and_drains_pipe(self):
        identifier = self.upload(audio=wav_payload(frames=48000*10)).json()['id']
        path, track = self.library.source(identifier, 'music-a')
        processes = []
        original = asyncio.create_subprocess_exec

        async def create(*args, **kwargs):
            process = await original(*args, **kwargs)
            processes.append(process)
            return process

        async def run():
            generator = self.library.pcm(path, track, 0)
            first = await anext(generator)
            self.assertGreater(len(first), 0)
            await asyncio.wait_for(generator.aclose(), 4)
            self.assertIsNotNone(processes[0].returncode)

        with patch('cloud_server.music_library.asyncio.create_subprocess_exec', side_effect=create):
            asyncio.run(run())
