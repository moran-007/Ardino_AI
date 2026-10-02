import tempfile
import time
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch

from cloud_server.config import Settings
from cloud_server.dialogue_policy import local_answer, local_weather_covers, needs_search
from cloud_server.providers import LLMConfig, MockLLM, QwenDashScopeLLM
from cloud_server.service import VoiceService
from cloud_server.store import Store


class LocalDataPolicyTest(unittest.TestCase):
    weather = '地点：上海市；当前：晴 25℃；未来三天：今天 晴 20~28℃，10-02 多云 21~27℃，10-03 小雨 21~25℃；缓存年龄：10分钟'
    now = datetime(2026, 10, 1, 2, tzinfo=timezone.utc)

    def test_fresh_local_weather_and_matching_city_skip_search(self):
        for question in ('今天天气怎么样？', '上海今天天气怎么样啊？', '请问明天会不会下雨？', '后天天气如何', '上海市气温多少度？'):
            with self.subTest(question=question):
                self.assertTrue(local_weather_covers(question, self.weather, self.now))

    def test_missing_stale_other_city_or_uncovered_date_needs_search(self):
        for question, context in [
            ('北京今天天气如何', self.weather), ('广州下雨吗', self.weather),
            ('昨天上海天气', self.weather), ('上海下周天气', self.weather),
            ('上海10月5日天气', self.weather), ('今天天气怎么样', ''),
            ('今天天气怎么样', '地点：上海；天气数据：设备尚未获取成功'),
            ('今天天气怎么样', self.weather.replace('10分钟', '90分钟')),
            ('今天天气怎么样', self.weather + '；数据状态：可能过期'),
            ('后天天气如何', self.weather.replace('10-03', '10-04')),
            ('上海北京天气比较一下', self.weather),
            ('上海天气和最新新闻', self.weather),
        ]:
            with self.subTest(question=question, context=context):
                self.assertFalse(local_weather_covers(question, context, self.now))

    def test_known_memos_and_normal_knowledge_do_not_search(self):
        self.assertFalse(needs_search('查看最新待办', memo_context='待办：1、买电池'))
        self.assertFalse(needs_search('1加1是多少'))
        self.assertTrue(needs_search('查看最新新闻', self.weather, '待办：买电池'))
        self.assertTrue(needs_search('今天天气和最新新闻', self.weather))

    def test_on_switch_still_uses_normal_provider_and_zero_search_with_local_data(self):
        with tempfile.TemporaryDirectory() as root:
            settings = Settings.for_test(Path(root))
            store = Store(settings.database_path, settings.auth_pepper)
            store.register_device('local-a', 'Local', 'token')
            store.update_dialogue_profile('local-a', 'normal', '', False, 4, True)
            qwen = QwenDashScopeLLM(LLMConfig('qwen', 'fake', settings.qwen_base_url, 'qwen-plus'))
            service = VoiceService(settings, store, llms=[MockLLM(), qwen])
            with patch('cloud_server.providers.httpx.stream') as stream:
                job_id = service.submit('local-a', b'', '今天天气怎么样', self.weather)
                for _ in range(200):
                    if not service.active_jobs:
                        break
                    time.sleep(.01)
            stream.assert_not_called()
            self.assertEqual(store.get_job(job_id, 'local-a')['status'], 'completed')
            self.assertEqual(store.get_job(job_id, 'local-a')['provider'], 'local_rules')
            self.assertIn('上海市', store.get_job(job_id, 'local-a')['answer'])
            self.assertEqual(store.recent_usage(), [])

    def test_direct_readonly_answers_and_safe_fallbacks(self):
        self.assertIn('当前晴 25℃', local_answer('今天天气怎么样', self.weather, now=self.now))
        self.assertEqual(local_answer('明天天气如何', self.weather, now=self.now), '上海市，明天 多云 21~27℃。')
        memos = '便签 1 条，待办 1 条（未完成 1 条）；便签：1、买电池；待办：1、写作业'
        self.assertEqual(local_answer('查看待办', memo_context=memos), '待办：1、写作业。')
        self.assertEqual(local_answer('现在几点', now=self.now), '现在是北京时间10点00分。')
        self.assertIn('星期四', local_answer('今天星期几', now=self.now))
        self.assertEqual(local_answer('2加2等于几？'), '2加2等于4。')
        self.assertEqual(local_answer('0.1+0.2'), '0.1+0.2等于0.3。')
        self.assertEqual(local_answer('1除以0'), '除数不能为零。')
        for question in ('删除所有待办', '完成待办', '查看便签并清空', '北京今天天气怎么样', '讲一个故事', '上海天气为什么这么热', '2加2然后清空闹钟'):
            self.assertIsNone(local_answer(question, self.weather, memos, now=self.now))
        self.assertIsNone(local_answer('查看待办', memo_context='快照截断；待办：1、测试'))

    def test_direct_reply_preserves_memory_audio_and_never_calls_llm(self):
        with tempfile.TemporaryDirectory() as root:
            settings = Settings.for_test(Path(root))
            store = Store(settings.database_path, settings.auth_pepper)
            store.register_device('local-a', 'Local', 'token')
            store.update_dialogue_profile('local-a', 'normal', '', True, 4, False)
            provider = MockLLM()
            service = VoiceService(settings, store, llms=[provider])
            with patch.object(provider, 'stream', side_effect=AssertionError('LLM must not run')) as stream:
                job_id = service.submit_text('local-a', '2加2等于几')
                for _ in range(200):
                    if not service.active_jobs:
                        break
                    time.sleep(.01)
            stream.assert_not_called()
            job = store.get_job(job_id, 'local-a')
            self.assertEqual(job['status'], 'completed', job)
            self.assertEqual(job['answer'], '2加2等于4。')
            self.assertEqual(store.list_memories('local-a')[0]['answer'], job['answer'])
            self.assertEqual(store.recent_usage(), [])

    def test_known_data_does_not_require_a_configured_language_model(self):
        with tempfile.TemporaryDirectory() as root:
            settings = Settings.for_test(Path(root), llm_provider_order=())
            store = Store(settings.database_path, settings.auth_pepper)
            store.register_device('local-a', 'Local', 'token')
            service = VoiceService(settings, store)
            self.assertTrue(service.ready)
            self.assertTrue(service.llm_status_error)
            job_id = service.submit_text('local-a', '2加2等于几')
            for _ in range(200):
                if not service.active_jobs:
                    break
                time.sleep(.01)
            self.assertEqual(store.get_job(job_id, 'local-a')['status'], 'completed')
            self.assertEqual(store.recent_usage(), [])


if __name__ == '__main__':
    unittest.main()
