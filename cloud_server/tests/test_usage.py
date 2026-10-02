from __future__ import annotations

import json
import tempfile
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import patch

from fastapi.testclient import TestClient

from cloud_server.app import create_app
from cloud_server.config import Settings
from cloud_server.providers import LLMConfig, MockASR, MockLLM, MockTTS, OpenAICompatibleLLM, QwenDashScopeLLM
from cloud_server.service import VoiceService
from cloud_server.store import Store


class Response:
    def __init__(self, packets, fail=False):
        self.packets, self.fail, self.closed = packets, fail, False

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.closed = True

    def raise_for_status(self):
        pass

    def iter_lines(self):
        for packet in self.packets:
            yield 'data: ' + json.dumps(packet, ensure_ascii=False)
        if self.fail:
            raise RuntimeError('interrupted')
        yield 'data: [DONE]'


def openai_packets(tokens=15):
    return [{"choices": [{"delta": {"content": "正常回答。"}}]},
            {"choices": [], "usage": {"prompt_tokens": tokens - 5, "completion_tokens": 5, "total_tokens": tokens}}]


class UsageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.settings = Settings.for_test(Path(self.temp.name))
        self.store = Store(self.settings.database_path, self.settings.auth_pepper)
        self.store.register_device("device-a", "A", "token-a")
        self.store.register_device("device-b", "B", "token-b")

    def tearDown(self):
        self.temp.cleanup()

    def provider(self, key="fake-key"):
        return OpenAICompatibleLLM(LLMConfig("deepseek", key, "https://api.deepseek.com", "test-model"))

    def service(self, providers):
        return VoiceService(self.settings, self.store, MockASR(), providers, MockTTS())

    def translate(self, service, device="device-a"):
        with TestClient(create_app(self.settings, service)) as client:
            response = client.post("/voice-api/v1/translate", json={"text": "hello", "target_language": "中文"},
                                   headers={"X-Device-ID": device, "Authorization": "Bearer token-" + device[-1]})
            return response

    def test_usage_only_tail_is_accepted_without_text_choices(self):
        response = Response(openai_packets())
        with patch('cloud_server.providers.httpx.stream', return_value=response):
            self.assertEqual(list(self.provider().stream("问", "简洁回答")), ["正常回答。"])
        self.assertTrue(response.closed)

    def test_translation_records_usage_and_device_and_survives_restart(self):
        with patch('cloud_server.providers.httpx.stream', return_value=Response(openai_packets())) as stream:
            response = self.translate(self.service([self.provider()]))
        self.assertEqual(response.status_code, 200, response.text)
        self.assertTrue(stream.call_args.kwargs['json']['stream_options']['include_usage'])
        fresh = Store(self.settings.database_path, self.settings.auth_pepper)
        stats = fresh.usage_summary()
        self.assertEqual(stats['totals']['total_tokens'], 15)
        self.assertEqual(stats['by_device'][0]['device_id'], 'device-a')
        event = fresh.recent_usage()[0]
        self.assertEqual(event['operation'], 'translation')
        self.assertEqual(event['input_tokens'], 10)
        self.assertEqual(event['output_tokens'], 5)
        self.assertTrue(event['success'])
        self.assertNotIn('fake-key', json.dumps(stats))

    def test_qwen_cumulative_snapshots_count_final_usage_once(self):
        provider = QwenDashScopeLLM(LLMConfig('qwen', 'fake-key', 'https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation', 'qwen-plus'))
        packets = [
            {'output': {'choices': [{'message': {'content': '第一'}}]}, 'usage': {'input_tokens': 10, 'output_tokens': 2, 'total_tokens': 12}},
            {'output': {'choices': [{'message': {'content': '第二'}}]}, 'usage': {'input_tokens': 10, 'output_tokens': 4, 'total_tokens': 14}},
            {'output': {'choices': []}, 'usage': {'input_tokens': 10, 'output_tokens': 4, 'total_tokens': 14}},
        ]
        with patch('cloud_server.providers.httpx.stream', return_value=Response(packets)):
            self.assertEqual(self.translate(self.service([provider])).status_code, 200)
        summary = self.store.usage_summary()['totals']
        self.assertEqual(summary['total_tokens'], 14)
        self.assertEqual(summary['attempts'], 1)

    def test_qwen_native_multimodal_models_use_text_blocks(self):
        base = 'https://workspace.cn-beijing.maas.aliyuncs.com/api/v1/services/aigc/text-generation/generation'
        for model in ('qwen3.5-plus', 'qwen3.6-plus', 'qwen3.7-max-2026-06-08', 'qwen3.8-max'):
            with self.subTest(model=model):
                provider = QwenDashScopeLLM(LLMConfig('qwen', 'fake-key', base, model))
                packet = {'output': {'choices': [{'message': {'content': [{'text': '回答。'}]}}]},
                          'usage': {'input_tokens': 10, 'output_tokens': 2, 'total_tokens': 12}}
                with patch('cloud_server.providers.httpx.stream', return_value=Response([packet])) as stream:
                    self.assertEqual(list(provider.stream('问题', '系统')), ['回答。'])
                self.assertIn('/multimodal-generation/', stream.call_args.args[1])
                self.assertEqual(stream.call_args.kwargs['json']['input']['messages'][1]['content'], [{'text': '问题'}])

    def test_qwen_text_only_exceptions_keep_text_endpoint(self):
        base = 'https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation'
        for model in ('qwen-plus', 'qwen3.7-max', 'qwen3.7-max-2026-05-20', 'qwen3.6-max-preview', 'qwen3.8-2.4t-a95b'):
            with self.subTest(model=model):
                provider = QwenDashScopeLLM(LLMConfig('qwen', 'fake-key', base, model))
                with patch('cloud_server.providers.httpx.stream', return_value=Response([])) as stream:
                    list(provider.stream('问题', '系统'))
                self.assertEqual(stream.call_args.args[1], base)
                self.assertEqual(stream.call_args.kwargs['json']['input']['messages'][1]['content'], '问题')

    def test_qwen_explicit_multimodal_usage_and_multiple_text_blocks(self):
        url = 'https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation'
        provider = QwenDashScopeLLM(LLMConfig('qwen', 'fake-key', url, 'custom-model'))
        packets = [
            {'output': {'choices': [{'message': {'content': [{'text': '第一'}, {'text': '第二'}]}}]}},
            {'output': {'choices': [{'message': {'content': [], 'reasoning_content': 'hidden'}}]},
             'usage': {'input_tokens': 8, 'output_tokens': 12, 'total_tokens': 20}},
        ]
        with patch('cloud_server.providers.httpx.stream', return_value=Response(packets)) as stream:
            response = self.translate(self.service([provider]))
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(response.json()['translation'], '第一第二')
        self.assertEqual(stream.call_args.args[1], url)
        self.assertEqual(self.store.usage_summary()['totals']['total_tokens'], 20)

    def test_provider_errors_do_not_return_secret_header(self):
        with patch('cloud_server.providers.httpx.stream', side_effect=ValueError('Illegal header Bearer fake-key')):
            response = self.translate(self.service([self.provider()]))
        self.assertEqual(response.status_code, 502)
        self.assertNotIn('fake-key', response.text)

    def test_failed_primary_and_missing_usage_are_explicit(self):
        class Unavailable:
            name = 'unavailable'
            def stream(self, *_):
                raise RuntimeError('unavailable')
                yield ''
        self.assertEqual(self.translate(self.service([Unavailable(), MockLLM()])).status_code, 200)
        summary = self.store.usage_summary()['totals']
        self.assertEqual((summary['attempts'], summary['successes'], summary['failures']), (2, 1, 1))
        self.assertEqual(summary['unreported_calls'], 2)
        self.assertEqual(summary['reported_calls'], 0)
        self.assertIsNone(self.store.recent_usage()[0]['total_tokens'])

    def test_rotation_keeps_old_key_usage_separate(self):
        for key in ('old-key', 'new-key'):
            with patch('cloud_server.providers.httpx.stream', return_value=Response(openai_packets())):
                self.translate(self.service([self.provider(key)]))
        summary = self.store.usage_summary()
        self.assertEqual(len(summary['by_key']), 2)
        self.assertEqual(summary['totals']['total_tokens'], 30)
        self.assertEqual(self.store.api_key_id('deepseek', 'key', 'https://api.deepseek.com/'), self.store.api_key_id('deepseek', 'key', 'https://api.deepseek.com'))

    def test_interruption_retains_reported_usage_without_fallback(self):
        broken = Response(openai_packets(), fail=True)
        with patch('cloud_server.providers.httpx.stream', return_value=broken):
            response = self.translate(self.service([self.provider(), MockLLM()]))
        self.assertEqual(response.status_code, 502)
        totals = self.store.usage_summary()['totals']
        self.assertEqual((totals['attempts'], totals['failures'], totals['total_tokens']), (1, 1, 15))
        self.assertTrue(broken.closed)

    def test_concurrent_calls_do_not_share_usage(self):
        service = self.service([self.provider()])
        def fake_stream(*_, **kwargs):
            text = kwargs['json']['messages'][-1]['content']
            count = 20 if text.endswith('small') else 100
            return Response(openai_packets(count))
        with patch('cloud_server.providers.httpx.stream', side_effect=fake_stream):
            with ThreadPoolExecutor(max_workers=2) as pool:
                futures = [pool.submit(service.translate, text, '中文', device_id=device) for text, device in [('small', 'device-a'), ('large', 'device-b')]]
                for future in futures:
                    future.result()
        groups = {item['device_id']: item['total_tokens'] for item in self.store.usage_summary()['by_device']}
        self.assertEqual(groups, {'device-a': 20, 'device-b': 100})

    def test_voice_counts_tokens_and_closes_stream_when_tts_fails(self):
        class FailingTTS(MockTTS):
            def synthesize(self, *_args, **_kwargs):
                raise RuntimeError('tts failed')
        service = VoiceService(self.settings, self.store, MockASR(), [self.provider()], FailingTTS())
        response = Response([{'choices': [{'delta': {'content': '这段话需要足够长，来生成语音句子。'}}],
                              'usage': {'prompt_tokens': 8, 'completion_tokens': 12, 'total_tokens': 20}}])
        with patch('cloud_server.providers.httpx.stream', return_value=response):
            job_id = service.submit_text('device-a', '你好')
            for _ in range(100):
                if service.active_jobs == 0:
                    break
                time.sleep(.02)
        self.assertEqual(self.store.get_job(job_id, 'device-a')['status'], 'failed')
        self.assertTrue(response.closed)
        self.assertEqual(self.store.usage_summary()['totals']['attempts'], 1)
        self.assertEqual(self.store.recent_usage()[0]['operation'], 'dialogue')

    def test_time_filter_and_empty_summary(self):
        self.assertEqual(self.store.usage_summary()['totals']['total_tokens'], 0)
        with patch('cloud_server.providers.httpx.stream', return_value=Response(openai_packets())):
            self.translate(self.service([self.provider()]))
        summary = self.store.usage_summary(since='9999-01-01T00:00:00+00:00')
        self.assertEqual(summary['totals']['attempts'], 0)
        self.assertEqual(summary['by_device'], [])
        self.assertEqual(self.store.recent_usage(since='9999-01-01T00:00:00+00:00'), [])


if __name__ == '__main__':
    unittest.main()
