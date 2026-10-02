import json
import re
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from cryptography.fernet import Fernet
from fastapi.testclient import TestClient

from cloud_server.admin import hash_admin_password
from cloud_server.app import create_app
from cloud_server.config import Settings
from cloud_server.model_catalog import fetch_models
from cloud_server.providers import build_llms, OpenAICompatibleLLM, OllamaLLM, TokenUsage
from cloud_server import speech_fallback as speech
from cloud_server.store import Store
from cloud_server.tests.test_usage import Response, openai_packets


class NativeResponse(Response):
    def iter_lines(self):
        yield from (json.dumps(packet) for packet in self.packets)


class CustomAndSpeechTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.settings = Settings.for_test(Path(self.temp.name))
        self.store = Store(self.settings.database_path, self.settings.auth_pepper)

    def tearDown(self):
        self.temp.cleanup()

    def test_custom_openai_full_url_no_key_and_local_ollama_usage(self):
        settings = Settings.for_test(Path(self.temp.name), llm_provider_order=('custom', 'local_llm'),
                                    custom_base_url='https://custom.example/v1/chat/completions')
        external, local = build_llms(settings)
        self.assertIsInstance(external, OpenAICompatibleLLM)
        self.assertIsInstance(local, OllamaLLM)
        with patch('cloud_server.providers.httpx.stream', return_value=Response(openai_packets())) as request:
            self.assertEqual(''.join(external.stream('问', '规则')), '正常回答。')
        self.assertEqual(request.call_args.args[1], settings.custom_base_url)
        self.assertNotIn('Authorization', request.call_args.kwargs['headers'])
        usage = TokenUsage()
        packets = [{'message': {'thinking': '不要播报', 'content': '本地回答。'}, 'done': False},
                   {'done': True, 'prompt_eval_count': 8, 'eval_count': 4, 'prompt_eval_cached_count': 3}]
        with patch('cloud_server.providers.httpx.stream', return_value=NativeResponse(packets)) as request:
            self.assertEqual(''.join(local.stream_with_usage('问', '规则', usage)), '本地回答。')
        self.assertEqual(request.call_args.args[1], 'http://127.0.0.1:11434/api/chat')
        self.assertEqual((usage.input_tokens, usage.output_tokens, usage.cached_input_tokens), (8, 4, 3))
        with patch('cloud_server.providers.httpx.stream', return_value=NativeResponse(packets[:1])):
            with self.assertRaises(RuntimeError):
                list(local.stream('问', '规则'))

    def test_native_model_catalog_without_key(self):
        payload = {'models': [{'name': 'qwen3:8b'}, {'name': 'qwen3:8b'}, {'model': 'glm:latest'}]}
        with patch('cloud_server.model_catalog.httpx.get', return_value=Mock(status_code=200, json=lambda: payload)) as get:
            self.assertEqual(fetch_models('local_llm', '', 'http://127.0.0.1:11434', protocol='ollama'), ['qwen3:8b', 'glm:latest'])
        self.assertEqual(get.call_args.args[0], 'http://127.0.0.1:11434/api/tags')
        self.assertNotIn('Authorization', get.call_args.kwargs['headers'])

    def test_local_first_off_defaults_and_cloud_first_fallback(self):
        primary = Mock(recognize=Mock(return_value='本地识别'))
        with patch.object(speech, 'cloud_asr', return_value='云端识别') as cloud:
            self.assertEqual(speech.recognize(self.settings, self.store, primary, b'\0' * 32000, 16000, 'd'), '本地识别')
            cloud.assert_not_called()
            settings = Settings.for_test(Path(self.temp.name), asr_route='local_first', cloud_speech_api_key='test')
            self.assertEqual(speech.recognize(settings, self.store, primary, b'\0' * 32000, 16000, 'd'), '本地识别')
            cloud.assert_not_called()
            primary.recognize.return_value = '.'
            self.assertEqual(speech.recognize(settings, self.store, primary, b'\0' * 32000, 16000, 'd'), '云端识别')
        row = self.store.recent_speech_usage()[0]
        self.assertEqual((row['quantity'], row['unit'], row['estimated_cost']), ('1', '秒', '0.00024'))
        self.assertEqual(self.store.recent_usage(), [])
        settings = Settings.for_test(Path(self.temp.name), asr_route='cloud_first', cloud_speech_api_key='test')
        primary.recognize.return_value = '本地回退'
        with patch.object(speech, 'cloud_asr', side_effect=RuntimeError('secret-do-not-echo')):
            self.assertEqual(speech.recognize(settings, self.store, primary, b'\0' * 32000, 16000, 'd'), '本地回退')
        self.assertIsNone(self.store.recent_speech_usage()[0]['estimated_cost'])

    def test_tts_cloud_only_uses_matching_pcm_rate_and_sanitizes_error(self):
        settings = Settings.for_test(Path(self.temp.name), tts_route='cloud_only', cloud_speech_api_key='test')
        primary = Mock(sample_rate=16000)
        with patch.object(speech, 'cloud_tts', return_value=b'\0' * 400) as cloud:
            self.assertEqual(len(speech.synthesize(settings, self.store, primary, '你好', 'd', speed=1.2)), 400)
        cloud.assert_called_once_with(settings, '你好', 16000, 1.2)
        primary.synthesize.assert_not_called()
        self.assertEqual(self.store.recent_speech_usage()[0]['estimated_cost'], '0.0002')
        with patch.object(speech, 'cloud_tts', side_effect=RuntimeError('secret-do-not-echo')):
            with self.assertRaisesRegex(RuntimeError, '主备均失败') as error:
                speech.synthesize(settings, self.store, primary, '你好', 'd')
        self.assertNotIn('secret', str(error.exception))

    def test_sdk_adapters_request_pcm_and_cleanup_private_input(self):
        result = Mock(status_code=200, get_sentence=lambda: [{'text': '测试'}])
        with patch('dashscope.audio.asr.Recognition', return_value=Mock(call=Mock(return_value=result))) as sdk:
            self.assertEqual(speech.cloud_asr(self.settings, b'\0' * 320, 16000), '测试')
        self.assertFalse(Path(sdk.return_value.call.call_args.args[0]).exists())
        self.assertEqual(sdk.call_args.kwargs['request_timeout'], 45)
        tts_result = Mock(get_response=lambda: SimpleNamespace(status_code=200), get_audio_data=lambda: b'\0' * 400)
        with patch('dashscope.audio.tts.SpeechSynthesizer.call', return_value=tts_result) as sdk:
            self.assertEqual(len(speech.cloud_tts(self.settings, '测试', 16000, 1)), 400)
        self.assertEqual(sdk.call_args.kwargs['format'], 'pcm')

    def test_admin_persists_routes_protocols_and_never_echoes_keys(self):
        settings = Settings.for_test(Path(self.temp.name), admin_password_hash=hash_admin_password('test-password-123'),
                                    config_encryption_key=Fernet.generate_key().decode())
        with TestClient(create_app(settings)) as client:
            client.post('/admin/unlock', data={'password': 'test-password-123'})
            page = client.get('/admin/settings').text
            csrf = re.search(r'name="csrf" value="([^"]+)"', page)[1]
            self.assertIn('本地主用，云端备用', page)
            response = client.post('/admin/config', data={'csrf': csrf, 'section': 'service',
                'voice_api_enabled': 'on', 'translation_api_enabled': 'on', 'asr_route': 'local_first',
                'tts_route': 'cloud_first', 'cloud_speech_api_key': 'secret-test-key'})
            self.assertEqual(response.status_code, 200, response.text)
            self.assertEqual(client.app.state.effective_settings.asr_route, 'local_first')
            self.assertNotIn('secret-test-key', client.get('/admin/settings').text)
            response = client.post('/admin/api-keys', data={'csrf': csrf, 'primary_provider': 'local_llm',
                'secondary_provider': 'custom', 'local_llm_protocol': 'ollama', 'custom_protocol': 'openai'})
            self.assertEqual(response.status_code, 200, response.text)
            self.assertEqual(client.app.state.effective_settings.llm_provider_order, ('local_llm', 'custom'))
            self.assertIn('服务器本地语言模型', response.text)
            self.assertEqual(client.post('/admin/config', data={'csrf': 'bad', 'section': 'service'}).status_code, 403)

    def test_manual_models_and_grouped_voice_selection(self):
        settings = Settings.for_test(Path(self.temp.name), admin_password_hash=hash_admin_password('test-password-123'),
                                    config_encryption_key=Fernet.generate_key().decode())
        with TestClient(create_app(settings)) as client:
            client.post('/admin/unlock', data={'password': 'test-password-123'})
            page = client.get('/admin/settings').text
            csrf = re.search(r'name="csrf" value="([^"]+)"', page)[1]
            self.assertIn('<optgroup label="男声">', page)
            self.assertIn('知悦', page)
            self.assertIn('sambert-zhimiao-emo-v1', page)
            response = client.post('/admin/config', data={'csrf': csrf, 'section': 'service',
                'cloud_asr_model': 'new-realtime-model-v3', 'cloud_tts_model': '',
                'cloud_tts_voice': 'sambert-zhiyue-v1'})
            self.assertEqual(response.status_code, 200, response.text)
            self.assertEqual(client.app.state.effective_settings.cloud_asr_model, 'new-realtime-model-v3')
            self.assertEqual(client.app.state.effective_settings.cloud_tts_model, 'sambert-zhiyue-v1')
            response = client.post('/admin/config', data={'csrf': csrf, 'section': 'service',
                'cloud_tts_voice': 'sambert-zhida-v1', 'cloud_tts_model': 'sambert-custom-v9'})
            self.assertEqual(response.status_code, 200, response.text)
            self.assertEqual(client.app.state.effective_settings.cloud_tts_model, 'sambert-custom-v9')
            self.assertIn('自定义：sambert-custom-v9', response.text)
            self.assertEqual(client.post('/admin/config', data={'csrf': csrf, 'section': 'service',
                'cloud_asr_model': '<script>'}).status_code, 422)

    def test_connectivity_auth_confirmation_preview_and_config_unchanged(self):
        settings = Settings.for_test(Path(self.temp.name), admin_password_hash=hash_admin_password('test-password-123'),
                                    config_encryption_key=Fernet.generate_key().decode())
        with TestClient(create_app(settings)) as client:
            self.assertEqual(client.post('/admin/speech-test').status_code, 401)
            client.post('/admin/unlock', data={'password': 'test-password-123'})
            csrf = re.search(r'name="csrf" value="([^"]+)"', client.get('/admin/settings').text)[1]
            data = {'csrf': csrf, 'test_operation': 'tts', 'cloud_speech_api_key': 'secret-unsaved-key',
                    'cloud_tts_model': '', 'cloud_tts_voice': 'sambert-zhiyue-v1'}
            with patch.object(speech, 'cloud_tts', return_value=b'\0' * 1600) as cloud:
                self.assertEqual(client.post('/admin/speech-test', data=data).status_code, 422)
                self.assertEqual(client.post('/admin/speech-test', data={**data, 'csrf': 'bad'}).status_code, 403)
                cloud.assert_not_called()
                client.app.state.voice_service.active_jobs = 1
                self.assertEqual(client.post('/admin/speech-test', data={**data, 'confirm_speech_test': 'on'}).status_code, 409)
                client.app.state.voice_service.active_jobs = 0
                before = dict(client.app.state.runtime_config)
                response = client.post('/admin/speech-test', data={**data, 'confirm_speech_test': 'on'})
                self.assertEqual(response.status_code, 200, response.text)
                self.assertIn('连通测试成功', response.text)
                self.assertNotIn('secret-unsaved-key', response.text)
                self.assertEqual(client.app.state.runtime_config, before)
                self.assertEqual(client.app.state.effective_settings.tts_route, 'local_only')
                self.assertEqual(client.app.state.effective_settings.cloud_tts_model, settings.cloud_tts_model)
                self.assertFalse(client.app.state.effective_settings.cloud_speech_api_key)
                url = re.search(r'<audio[^>]+src="([^"]+)"', response.text)[1]
                audio = client.get(url)
                self.assertEqual(audio.status_code, 200)
                self.assertEqual(audio.content[:4], b'RIFF')
                self.assertEqual(audio.headers['cache-control'], 'no-store')
                client.get('/admin/speech-test/result')
                cloud.assert_called_once()
                self.assertEqual(client.post('/admin/speech-test', data={**data, 'confirm_speech_test': 'on'}).status_code, 429)
                self.assertEqual(client.get('/admin/speech-test/audio?sample=bad').status_code, 404)
                with TestClient(client.app) as stranger:
                    self.assertEqual(stranger.get(url).status_code, 401)
                    stranger.post('/admin/unlock', data={'password': 'test-password-123'})
                    self.assertEqual(stranger.get(url).status_code, 404)
                import time
                with patch('cloud_server.admin.time.monotonic', return_value=time.monotonic() + 600):
                    self.assertEqual(client.get(url).status_code, 404)
            row = client.app.state.store.recent_speech_usage()[0]
            self.assertEqual((row['device_id'], row['model']), ('admin-connectivity-test', 'sambert-zhiyue-v1'))

    def test_connectivity_reports_protocol_sdk_errors_and_separate_asr(self):
        from dataclasses import replace
        settings = replace(self.settings, cloud_speech_api_key='secret-do-not-echo', tts_provider='sherpa_vits')
        primary = Mock(sample_rate=16000, synthesize=Mock(return_value=b'\0' * 32000))
        with patch.object(speech, 'cloud_asr', return_value='今天天气怎么样？') as cloud:
            result = speech.connectivity_test(settings, self.store, 'asr', primary)
            self.assertTrue(result['success'])
            self.assertIn('今天天气怎么样', result['detail'])
            cloud.assert_called_once()
        with patch.object(speech, 'cloud_tts', side_effect=RuntimeError('secret-do-not-echo')):
            result = speech.connectivity_test(settings, self.store, 'tts', primary)
            self.assertFalse(result['success'])
            self.assertNotIn('secret', result['detail'])
        rows = len(self.store.recent_speech_usage())
        for operation, model in [('asr', 'paraformer-v2'), ('asr', 'paraformer-realtime-8k-v2'),
                                 ('asr', 'qwen-audio-3.1-asr-flash'), ('tts', 'cosyvoice-v1')]:
            with patch.object(speech, 'cloud_' + operation) as cloud:
                result = speech.connectivity_test(replace(settings, **{'cloud_' + operation + '_model': model}), self.store, operation, primary)
                self.assertFalse(result['success'])
                cloud.assert_not_called()
        self.assertEqual(len(self.store.recent_speech_usage()), rows)
        result = Mock(status_code=401, code='InvalidApiKey', message='secret-do-not-echo')
        with patch('dashscope.audio.asr.Recognition', return_value=Mock(call=Mock(return_value=result))):
            with self.assertRaisesRegex(speech.SpeechTestError, 'Key 无效'):
                speech.cloud_asr(settings, b'\0' * 320, 16000)
