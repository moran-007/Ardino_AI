import base64
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from fastapi.testclient import TestClient
from cloud_server.app import create_app
from cloud_server.config import Settings


class LocalControlTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.client = TestClient(create_app(Settings.for_test(Path(self.temp.name))))
        self.store = self.client.app.state.store
        self.service = self.client.app.state.voice_service
        self.store.register_device('local-device', 'Local', 'local-token')
        self.store.register_device('other-device', 'Other', 'other-token')
        self.headers = {'X-Device-ID': 'local-device', 'Authorization': 'Bearer local-token'}

    def tearDown(self):
        for _ in range(100):
            if not self.service.active_jobs:
                break
            time.sleep(.01)
        self.client.close()
        self.temp.cleanup()

    def submit(self, question, capable=True):
        headers = {**self.headers, 'X-Simulated-Text-B64': base64.urlsafe_b64encode(question.encode()).decode()}
        if capable:
            headers['X-Device-Local-Control'] = '1'
        response = self.client.post('/voice-api/v1/dialogue', content=b'\0' * 3200, headers=headers)
        self.assertEqual(response.status_code, 202, response.text)
        self.assertEqual(response.json()['context'].get('local_control', False), capable)
        job = response.json()['job_id']
        for _ in range(100):
            if self.store.get_job(job, 'local-device')['question']:
                return job
            time.sleep(.01)
        self.fail('ASR question not published for device execution')

    def finish(self, job):
        for _ in range(200):
            row = self.store.get_job(job, 'local-device')
            if row['status'] in {'completed', 'failed'}:
                return row
            time.sleep(.01)
        self.fail('job not finished')

    def test_actual_compound_result_bypasses_llm_and_is_spoken(self):
        provider = Mock(name='unused', stream=Mock(side_effect=AssertionError('LLM must not run')))
        self.service.llms = [provider]
        job = self.submit('明天早上9:35提醒我做其他事情，加一个待办')
        message = '待办已添加：明天早上9:35提醒我做其他事情；已设置10-02 09:35的闹钟'
        response = self.client.post(f'/voice-api/v1/jobs/{job}/local-result', headers=self.headers,
                                    json={'handled': True, 'message': message})
        self.assertEqual(response.status_code, 200, response.text)
        row = self.finish(job)
        self.assertEqual((row['status'], row['provider'], row['answer']), ('completed', 'device_result', message))
        self.assertGreater(len(self.store.list_segments(job)), 0)
        provider.stream.assert_not_called()
        self.assertEqual(self.store.recent_usage(), [])

    def test_storage_failure_not_overwritten_by_success_and_other_device_denied(self):
        job = self.submit('一会儿需要去买东西，加个待办')
        url = f'/voice-api/v1/jobs/{job}/local-result'
        self.assertEqual(self.client.post(url, json={'handled': True, 'message': '已保存'}).status_code, 401)
        other = {'X-Device-ID': 'other-device', 'Authorization': 'Bearer other-token'}
        self.assertEqual(self.client.post(url, headers=other, json={'handled': True, 'message': '已保存'}).status_code, 404)
        self.assertEqual(self.client.post(url, headers=self.headers, json={'handled': True, 'message': ''}).status_code, 422)
        message = '保存失败：列表已满或存储写入失败'
        self.assertEqual(self.client.post(url, headers=self.headers, json={'handled': True, 'message': message}).status_code, 200)
        self.assertEqual(self.finish(job)['answer'], message)
        self.assertEqual(self.client.post(url, headers=self.headers, json={'handled': True, 'message': '已保存'}).status_code, 409)

    def test_non_command_and_legacy_device_keep_normal_dialogue(self):
        job = self.submit('给我讲个故事')
        self.assertEqual(self.client.post(f'/voice-api/v1/jobs/{job}/local-result', headers=self.headers,
                                         json={'handled': False, 'message': ''}).status_code, 200)
        self.assertEqual(self.finish(job)['provider'], 'mock')
        legacy = self.submit('你好', capable=False)
        self.assertEqual(self.finish(legacy)['status'], 'completed')

    def test_missing_device_confirmation_fails_without_success_speech(self):
        with patch('cloud_server.service.LOCAL_RESULT_TIMEOUT_SECONDS', .01):
            job = self.service.submit('local-device', b'\0' * 3200, '添加待办买水', local_control=True)
            row = self.finish(job)
        self.assertEqual(row['status'], 'failed')
        self.assertIn('未收到设备执行确认', row['error'])
        self.assertEqual(self.store.list_segments(job), [])
        self.assertEqual(self.store.recent_usage(), [])
        self.assertEqual(self.service._local_results, {})
