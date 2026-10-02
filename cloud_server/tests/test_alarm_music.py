from __future__ import annotations

import io
import re
import shutil
import subprocess
import tempfile
import unittest
import wave
from pathlib import Path
from unittest.mock import patch

from fastapi.testclient import TestClient
from cryptography.fernet import Fernet

from cloud_server.admin import hash_admin_password
from cloud_server.alarm_music import AlarmMusicError, AlarmMusicRepository, MAX_DEVICE_UPLOAD_BYTES, MAX_UPLOAD_BYTES, validate_wav, normalize_admin_music
from cloud_server.app import create_app
from cloud_server.config import Settings


def wav_audio(*, rate=16000, channels=1, width=2, frames=1600):
    output = io.BytesIO()
    with wave.open(output, 'wb') as sound:
        sound.setparams((channels, width, rate, 0, 'NONE', 'not compressed'))
        sound.writeframes(bytes(frames * width * channels))
    return output.getvalue()


class AlarmMusicTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.settings = Settings.for_test(Path(self.temp.name), admin_password_hash=hash_admin_password('music-test-password'),
                                          config_encryption_key=Fernet.generate_key().decode('ascii'))
        self.client = TestClient(create_app(self.settings))
        for device in ('music-a', 'music-b'):
            self.client.app.state.store.register_device(device, device, device + '-token')
        self.auth = {'Authorization': 'Bearer music-a-token', 'X-Device-ID': 'music-a'}
        self.other_auth = {'Authorization': 'Bearer music-b-token', 'X-Device-ID': 'music-b'}

    def tearDown(self):
        self.client.close()
        self.temp.cleanup()

    def unlock(self):
        unlocked = self.client.post('/admin/unlock', data={'password': 'music-test-password'})
        self.assertEqual(unlocked.status_code, 200)
        return re.search(r'name="csrf" value="([^"]+)"', unlocked.text).group(1)

    def upload(self, csrf, payload=None, title='My music'):
        return self.client.post('/admin/alarm-music/upload', data={'csrf': csrf, 'title': title},
                                files={'music': ('../../outside.wav', payload or wav_audio(), 'audio/wav')}, follow_redirects=False)

    def test_authenticated_presets_pcm_and_isolated_selection_persist(self):
        for route in ('/voice-api/v1/alarm-music', '/voice-api/v1/alarm-music/selection', '/voice-api/v1/alarm-music/morning/pcm'):
            self.assertEqual(self.client.get(route).status_code, 401)
        tracks = self.client.get('/voice-api/v1/alarm-music', headers=self.auth).json()['tracks']
        self.assertEqual({track['id'] for track in tracks}, {'morning', 'breeze', 'starlight'})
        for track in tracks:
            pcm = self.client.get(f"/voice-api/v1/alarm-music/{track['id']}/pcm", headers=self.auth)
            self.assertEqual(pcm.status_code, 200)
            self.assertEqual(len(pcm.content), track['bytes'])
            self.assertEqual(int(pcm.headers['content-length']), track['bytes'])
            self.assertTrue(0 < len(pcm.content) <= 640000)
            self.assertNotEqual(pcm.content[:4], b'RIFF')
        selection = '/voice-api/v1/alarm-music/selection'
        self.assertEqual(self.client.get(selection, headers=self.auth).json(), {'id': 'morning'})
        self.assertEqual(self.client.put(selection, json={'id': 'breeze'}, headers=self.auth).status_code, 200)
        self.assertEqual(self.client.get('/voice-api/v1/alarm-music', headers=self.auth).json()['selected'], 'breeze')
        self.assertEqual(self.client.get('/voice-api/v1/alarm-music', headers=self.other_auth).json()['selected'], 'morning')
        self.assertEqual(self.client.get(selection, headers=self.other_auth).json(), {'id': 'morning'})
        self.assertEqual(self.client.put(selection, json={'id': 'missing'}, headers=self.auth).status_code, 404)
        with TestClient(create_app(self.settings)) as restarted:
            self.assertEqual(restarted.get(selection, headers=self.auth).json(), {'id': 'breeze'})

    def test_mp3_converter_errors_are_safe_and_release_slot(self):
        for error in (FileNotFoundError(), subprocess.CalledProcessError(1, 'ffmpeg'),
                      subprocess.TimeoutExpired('ffmpeg', 20)):
            with patch('cloud_server.alarm_music.subprocess.run', side_effect=error):
                with self.assertRaises(AlarmMusicError):
                    normalize_admin_music(b'not-mp3', 'music.mp3')
        self.assertEqual(normalize_admin_music(wav_audio(), 'music.wav'), wav_audio())

    @unittest.skipUnless(shutil.which('ffmpeg'), 'FFmpeg not installed locally')
    def test_real_mp3_upload_decodes_stereo_and_clips_long_song(self):
        mp3 = Path(self.temp.name) / 'test.mp3'
        subprocess.run(['ffmpeg', '-nostdin', '-loglevel', 'error', '-f', 'lavfi',
                        '-i', 'sine=frequency=440:duration=31', '-ar', '44100', '-ac', '2',
                        '-c:a', 'libmp3lame', str(mp3)], check=True)
        csrf = self.unlock()
        response = self.client.post('/admin/alarm-music/upload',
                                   data={'csrf': csrf, 'title': 'MP3 铃声'},
                                   files={'music': ('music.MP3', mp3.read_bytes(), 'audio/mpeg')},
                                   follow_redirects=False)
        self.assertEqual(response.status_code, 303, response.text)
        tracks = self.client.get('/voice-api/v1/alarm-music', headers=self.auth).json()['tracks']
        track = next(track for track in tracks if track['title'] == 'MP3 铃声')
        self.assertEqual(track['sample_rate'], 16000)
        self.assertEqual(track['bytes'], 30 * 32000)
        pcm = self.client.get('/voice-api/v1/alarm-music/' + track['id'] + '/pcm', headers=self.auth)
        self.assertEqual(pcm.status_code, 200)
        self.assertNotEqual(pcm.content, bytes(len(pcm.content)))
        self.assertEqual(len(validate_wav(normalize_admin_music(mp3.read_bytes(), 'renamed.wav'))), 30 * 32000)
        invalid = self.client.post('/admin/alarm-music/upload',
                                   data={'csrf': csrf, 'title': '损坏'},
                                   files={'music': ('bad.mp3', b'not audio', 'audio/mpeg')})
        self.assertEqual(invalid.status_code, 422)

    def test_device_upload_private_ownership_and_size_bound(self):
        route = '/voice-api/v1/alarm-music-upload?title=Private'
        self.assertEqual(self.client.post(route, content=wav_audio()).status_code, 401)
        uploaded = self.client.post(route, content=wav_audio(), headers=self.auth)
        self.assertEqual(uploaded.status_code, 201, uploaded.text)
        track_id = uploaded.json()['id']
        self.assertEqual(uploaded.json()['bytes'], 3200)
        self.assertIn(track_id, {track['id'] for track in self.client.get('/voice-api/v1/alarm-music', headers=self.auth).json()['tracks']})
        self.assertNotIn(track_id, {track['id'] for track in self.client.get('/voice-api/v1/alarm-music', headers=self.other_auth).json()['tracks']})
        self.assertEqual(self.client.get(f'/voice-api/v1/alarm-music/{track_id}/pcm', headers=self.other_auth).status_code, 404)
        self.assertEqual(self.client.put('/voice-api/v1/alarm-music/selection', headers=self.other_auth, json={'id': track_id}).status_code, 404)
        self.assertEqual(self.client.post(route, content=b'x' * (MAX_DEVICE_UPLOAD_BYTES + 1), headers=self.auth).status_code, 413)
        self.assertEqual(self.client.post(route, content=wav_audio(rate=44100), headers=self.auth).status_code, 422)
        self.assertEqual(self.client.post('/voice-api/v1/alarm-music-upload?title=' + 'a' * 61, content=wav_audio(), headers=self.auth).status_code, 422)

    def test_admin_upload_preview_range_delete_and_csrf(self):
        self.assertEqual(self.client.get('/admin/alarm-music').status_code, 401)
        self.assertEqual(self.upload('none').status_code, 401)
        self.assertEqual(self.client.get('/admin/alarm-music/morning/wav', headers={'Range': 'bytes=0-20'}).status_code, 401)
        csrf = self.unlock()
        self.assertEqual(self.upload('wrong').status_code, 403)
        self.assertEqual(self.upload(csrf, title='<b>Music</b>').status_code, 303)
        tracks = self.client.get('/voice-api/v1/alarm-music', headers=self.other_auth).json()['tracks']
        track_id = next(track['id'] for track in tracks if not track['preset'])
        self.assertIn('&lt;b&gt;Music&lt;/b&gt;', self.client.get('/admin/alarm-music').text)
        response = self.client.get(f'/admin/alarm-music/{track_id}/wav', headers={'Range': 'bytes=0-43'})
        self.assertEqual(response.status_code, 206)
        self.assertEqual(response.content[:4], b'RIFF')
        self.assertEqual(len(response.content), 44)
        self.assertEqual(self.client.get(f'/admin/alarm-music/{track_id}/wav', headers={'Range': 'bytes=999999-'}).status_code, 416)
        self.client.put('/voice-api/v1/alarm-music/selection', headers=self.auth, json={'id': track_id})
        self.assertEqual(self.client.post(f'/admin/alarm-music/{track_id}/delete', data={'csrf': 'wrong'}).status_code, 403)
        self.assertEqual(self.client.post(f'/admin/alarm-music/{track_id}/delete', data={'csrf': csrf}, follow_redirects=False).status_code, 303)
        self.assertEqual(self.client.get('/voice-api/v1/alarm-music/selection', headers=self.auth).json(), {'id': 'morning'})
        self.assertEqual(self.client.post('/admin/alarm-music/morning/delete', data={'csrf': csrf}).status_code, 422)
        self.assertEqual(self.client.get(f'/voice-api/v1/alarm-music/{track_id}/pcm', headers=self.auth).status_code, 404)
        self.assertFalse((Path(self.temp.name) / 'outside.wav').exists())

    def test_bad_audio_oversized_chunked_upload_and_path_guard(self):
        csrf = self.unlock()
        for payload in (b'invalid', wav_audio(rate=44100), wav_audio(channels=2), wav_audio(width=1), wav_audio(frames=480001), wav_audio()[:-4]):
            self.assertEqual(self.upload(csrf, payload=payload).status_code, 422)
        chunks = (b'x' * 8192 for _ in range(MAX_UPLOAD_BYTES // 8192 + 1))
        response = self.client.post('/admin/alarm-music/upload', content=chunks, headers={'Content-Type': 'multipart/form-data; boundary=test'})
        self.assertEqual(response.status_code, 413)
        repository = self.client.app.state.alarm_music_repository
        for track_id in ('../catalog', '/etc/passwd', '..', 'C:\\bad'):
            with self.assertRaises(AlarmMusicError):
                repository.wav(track_id)

    def test_limits_and_restart_preserve_uploaded_music(self):
        repository = self.client.app.state.alarm_music_repository
        for index in range(6):
            repository.upload(wav_audio(), str(index), 'music-a')
        with self.assertRaises(AlarmMusicError):
            repository.upload(wav_audio(), 'seventh', 'music-a')
        for index in range(3):
            repository.upload(wav_audio(), str(index))
        with self.assertRaises(AlarmMusicError):
            repository.upload(wav_audio(), 'thirteenth')
        restarted = AlarmMusicRepository(Path(self.temp.name) / 'alarm-music')
        self.assertEqual(len(restarted.catalog()), 12)
        self.assertEqual(len(restarted.catalog('music-b')), 6)


if __name__ == '__main__':
    unittest.main()
