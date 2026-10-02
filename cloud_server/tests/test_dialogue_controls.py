import tempfile
import unittest
import time
from unittest.mock import patch
from pathlib import Path

from fastapi.testclient import TestClient
from cloud_server.app import create_app
from cloud_server.config import Settings
from cloud_server.providers import LLMConfig, MockLLM, QwenDashScopeLLM, TokenUsage
from cloud_server.tests.test_usage import Response


class DialogueControlsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.client = TestClient(create_app(Settings.for_test(Path(self.temp.name))))
        self.store = self.client.app.state.store
        self.store.register_device('controls-a', 'A', 'token-a')
        self.store.register_device('controls-b', 'B', 'token-b')
        self.headers = {'X-Device-ID': 'controls-a', 'Authorization': 'Bearer token-a'}

    def tearDown(self):
        self.client.close()
        self.temp.cleanup()

    def test_visible_profile_and_updates_are_device_scoped(self):
        response = self.client.get('/voice-api/v1/dialogue-settings', headers=self.headers)
        self.assertEqual(response.status_code, 200)
        self.assertIn('system_prompt', response.json())
        self.assertIn('reply_style', response.json())
        self.assertNotIn('api_key', response.text)
        self.assertFalse(response.json()['web_search_enabled'])
        saved = self.client.put('/voice-api/v1/dialogue-settings', headers=self.headers,
                                json={'reply_style': 'detailed', 'persona': '认真解释',
                                      'memory_enabled': True, 'memory_turns': 6, 'web_search_enabled': True})
        self.assertEqual(saved.status_code, 200, saved.text)
        self.assertEqual(saved.json()['reply_style'], 'detailed')
        self.assertEqual(self.store.get_device('controls-b')['reply_style'], 'default')
        self.assertFalse(self.store.get_device('controls-b')['web_search_enabled'])
        self.assertTrue(saved.json()['web_search_enabled'])
        self.assertEqual(self.client.put('/voice-api/v1/dialogue-settings', headers=self.headers,
                                        json={'reply_style': 'bad'}).status_code, 422)
        self.assertEqual(self.client.get('/voice-api/v1/dialogue-settings').status_code, 401)

    def test_explicit_memory_clear_is_scoped_and_busy_safe(self):
        self.store.add_memory('controls-a', 'a', 'a')
        self.store.add_memory('controls-b', 'b', 'b')
        self.assertEqual(self.client.post('/voice-api/v1/memory/clear', headers=self.headers,
                                         json={'confirm': False}).status_code, 422)
        self.client.app.state.voice_service.active_jobs = 1
        self.assertEqual(self.client.post('/voice-api/v1/memory/clear', headers=self.headers,
                                         json={'confirm': True}).status_code, 409)
        self.client.app.state.voice_service.active_jobs = 0
        response = self.client.post('/voice-api/v1/memory/clear', headers=self.headers,
                                    json={'confirm': True})
        self.assertEqual(response.status_code, 200)
        self.assertEqual(self.store.list_memories('controls-a'), [])
        self.assertEqual(len(self.store.list_memories('controls-b')), 1)

    def finish(self, service, question):
        job = service.submit_text('controls-a', question)
        for _ in range(200):
            if not service.active_jobs:
                return self.store.get_job(job, 'controls-a')
            time.sleep(.01)
        self.fail('job did not finish')

    def test_search_off_by_default_on_is_device_scoped_and_actual_search_counted(self):
        service = self.client.app.state.voice_service
        provider = QwenDashScopeLLM(LLMConfig('qwen', 'fake-key', 'https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation', 'qwen-plus'))
        service.llms = [MockLLM(), provider]
        with patch('cloud_server.providers.httpx.stream') as stream:
            self.assertEqual(self.finish(service, '北京天气怎么样')['provider'], 'mock')
        stream.assert_not_called()
        self.store.update_dialogue_profile('controls-a', 'detailed', '', False, 4, True)
        packets = [{'output': {'choices': [{'message': {'content': '北京天气晴朗。'}}]},
                    'usage': {'input_tokens': 10, 'output_tokens': 5, 'plugins': {'search': {'count': 1}}, 'prompt_tokens_details': {'cached_tokens': 0}}}]
        with patch('cloud_server.providers.httpx.stream', return_value=Response(packets)) as stream:
            job = self.finish(service, '北京天气怎么样')
        self.assertEqual(job['provider'], 'qwen')
        payload = stream.call_args.kwargs['json']
        self.assertTrue(payload['parameters']['enable_search'])
        self.assertTrue(payload['parameters']['search_options']['forced_search'])
        self.assertIn('详细展开', payload['input']['messages'][0]['content'])
        self.assertEqual(self.store.recent_usage()[0]['search_calls'], 1)
        self.assertEqual(self.store.recent_usage()[0]['cost_status'], 'priced')
        with patch('cloud_server.providers.httpx.stream', return_value=Response(packets)) as stream:
            self.finish(service, '讲一个故事')
        stream.assert_not_called()  # Normal provider priority remains unchanged.

    def test_search_request_without_provider_confirmation_does_not_read_fake_weather(self):
        provider = QwenDashScopeLLM(LLMConfig('qwen', 'key', 'https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation', 'qwen-plus'))
        packets = [{'output': {'choices': [{'message': {'content': '未核实天气'}}]}, 'usage': {'input_tokens': 1, 'output_tokens': 2}}]
        usage = TokenUsage()
        with patch('cloud_server.providers.httpx.stream', return_value=Response(packets)):
            with self.assertRaises(RuntimeError):
                list(provider.stream_with_usage('天气', '系统', usage, web_search=True))
        self.assertEqual(usage.search_calls, 0)
        with patch('cloud_server.providers.httpx.stream', return_value=Response(packets)) as stream:
            list(provider.stream_with_usage('你好', '系统', TokenUsage()))
        self.assertNotIn('enable_search', stream.call_args.kwargs['json']['parameters'])


if __name__ == '__main__':
    unittest.main()
