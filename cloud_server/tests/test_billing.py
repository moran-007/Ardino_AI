import json
import re
import tempfile
import unittest
from dataclasses import replace
from datetime import datetime, timezone
from decimal import Decimal
from pathlib import Path
from unittest.mock import patch

from cryptography.fernet import Fernet
from fastapi.testclient import TestClient
from cloud_server.admin import hash_admin_password
from cloud_server.app import create_app
from cloud_server.billing import calculate, manual_rule, official_rule
from cloud_server.config import Settings
from cloud_server.providers import LLMConfig, OpenAICompatibleLLM, TokenUsage
from cloud_server.tests.test_usage import Response


class BillingTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.settings = Settings.for_test(Path(self.temp.name),
            admin_password_hash=hash_admin_password('billing-test-password'),
            config_encryption_key=Fernet.generate_key().decode(), deepseek_model='deepseek-flash')
        self.client = TestClient(create_app(self.settings))
        self.store = self.client.app.state.store
        self.store.register_device('billing-a', 'A', 'token-a')
        self.csrf = re.search(r'name="csrf" value="([^"]+)"', self.client.post('/admin/unlock',
            data={'password': 'billing-test-password'}).text).group(1)
        self.at = datetime(2026, 10, 1, 2, tzinfo=timezone.utc)

    def tearDown(self):
        self.client.close()
        self.temp.cleanup()

    def test_actual_cache_and_output_plus_peak_holiday(self):
        usage = TokenUsage(1000, 100, 1100, 800)
        result = calculate(self.settings, 'deepseek', 'deepseek-flash', 'https://api.deepseek.com', usage, self.at)
        self.assertEqual(Decimal(result['cost_amount']), Decimal('0.0000924'))
        self.assertEqual(result['cost_status'], 'priced')
        self.assertEqual(result['cost_currency'], 'USD')
        peak = datetime(2026, 10, 8, 2, tzinfo=timezone.utc)
        self.assertEqual(official_rule('deepseek', 'deepseek-flash', 'https://api.deepseek.com', peak)['tiers'][0]['input'], '0.30')
        weekend = datetime(2026, 10, 10, 2, tzinfo=timezone.utc)
        self.assertEqual(official_rule('deepseek', 'deepseek-flash', 'https://api.deepseek.com', weekend)['tiers'][0]['input'], '0.15')

    def test_qwen_tier_boundary_cache_and_search(self):
        for tokens, price in [(128000, '0.8'), (128001, '2.4'), (256001, '4.8')]:
            usage = TokenUsage(tokens, 2, tokens + 2, 0, 2)
            result = calculate(self.settings, 'qwen', 'qwen-plus', 'https://dashscope.aliyuncs.com/api/v1/x', usage, self.at)
            tier = next(r for r in json.loads(result['price_snapshot'])['tiers'] if tokens <= r['max_input_tokens'])
            self.assertEqual(tier['input'], price)
            self.assertEqual(result['cost_status'], 'priced')
            self.assertGreaterEqual(Decimal(result['cost_amount']), Decimal('0.006'))
        self.assertIsNone(official_rule('qwen', 'qwen-plus', 'https://dashscope-intl.aliyuncs.com'))
        self.assertIsNone(official_rule('deepseek', 'deepseek-flash', 'https://proxy.example'))

    def test_missing_usage_cache_search_are_not_zero_complete_cost(self):
        self.assertIsNone(calculate(self.settings, 'qwen', 'qwen-plus', 'https://dashscope.aliyuncs.com', TokenUsage(), self.at)['cost_amount'])
        result = calculate(self.settings, 'qwen', 'qwen-plus', 'https://dashscope.aliyuncs.com', TokenUsage(10, 2, 12), self.at)
        self.assertEqual(result['cost_status'], 'estimated')
        result = calculate(self.settings, 'qwen', 'qwen-plus', 'https://dashscope.aliyuncs.com', TokenUsage(10, 2, 12, 0, None), self.at)
        self.assertEqual(result['cost_status'], 'partial')
        self.assertEqual(calculate(self.settings, 'qwen', 'custom', 'https://dashscope.aliyuncs.com', TokenUsage(10, 2, 12), self.at)['cost_status'], 'unpriced')
        self.assertEqual(calculate(replace(self.settings, billing_enabled=False), 'qwen', 'qwen-plus', '', TokenUsage(), self.at)['cost_status'], 'disabled')

    def test_manual_validation(self):
        for invalid in ('NaN', 'Infinity', '-1', '100001', '0.000000001', 'oops'):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                manual_rule({'currency': 'CNY', 'tiers': [{'max_input_tokens': 1000, 'input': invalid, 'output': '1'}]})
        with self.assertRaises(ValueError):
            manual_rule({'tiers': [{'max_input_tokens': 10, 'input': '1', 'output': '1'}, {'max_input_tokens': 9, 'input': '1', 'output': '1'}]})

    def form(self, **kwargs):
        return dict(csrf=self.csrf, provider='deepseek', model='deepseek-flash', mode='manual',
                    currency='CNY', limit_0='1000000', input_0='2', output_0='8', cache_0='0.1', search='0', **kwargs)

    def test_protected_admin_manual_rule_persists_reset_and_snapshot_immutable(self):
        self.store.record_usage(device_id='billing-a', provider='deepseek', key_id='old', model='deepseek-flash', operation='dialogue', success=True, input_tokens=100)
        self.assertEqual(self.store.billing_summary()['statuses']['historical'], 1)
        response = self.client.post('/admin/billing', data=self.form())
        self.assertEqual(response.status_code, 200, response.text)
        current = self.client.app.state.effective_settings
        self.assertEqual(json.loads(current.billing_rules)['deepseek:deepseek-flash']['currency'], 'CNY')
        event = calculate(current, 'deepseek', 'deepseek-flash', 'https://api.deepseek.com', TokenUsage(100, 10, 110, 40), self.at)
        self.store.record_usage(device_id='billing-a', provider='deepseek', key_id='id', model='deepseek-flash', operation='dialogue', success=False, input_tokens=100, output_tokens=10, **event)
        before = self.store.recent_usage()[0]['price_snapshot']
        form = self.form(); form.update(input_0='20', currency='USD')
        self.assertEqual(self.client.post('/admin/billing', data=form).status_code, 200)
        self.assertEqual(self.store.recent_usage()[0]['price_snapshot'], before)
        self.assertEqual(self.store.billing_summary()['totals'], {'CNY': event['cost_amount']})
        self.assertEqual(self.store.billing_summary(since='9999')['totals'], {})
        form.update(mode='official')
        self.assertEqual(self.client.post('/admin/billing', data=form).status_code, 200)
        self.assertEqual(json.loads(self.client.app.state.effective_settings.billing_rules), {})
        form.update(csrf='bad')
        self.assertEqual(self.client.post('/admin/billing', data=form).status_code, 403)
        form.update(csrf=self.csrf, mode='manual', input_0='NaN')
        self.assertEqual(self.client.post('/admin/billing', data=form).status_code, 422)
        self.client.cookies.clear()
        self.assertIn('管理员密码', self.client.get('/admin/billing').text)
        self.assertEqual(self.client.post('/admin/billing', data=form).status_code, 401)

    def test_provider_returned_usage_records_cost_once_even_failure(self):
        service = self.client.app.state.voice_service
        service.llms = [OpenAICompatibleLLM(LLMConfig('deepseek', 'fake-key', 'https://api.deepseek.com', 'deepseek-flash'))]
        packets = [{'choices': [{'delta': {'content': '翻译结果'}}],
                    'usage': {'prompt_tokens': 10, 'completion_tokens': 5, 'prompt_cache_hit_tokens': 8}}]
        with patch('cloud_server.providers.httpx.stream', return_value=Response(packets, fail=True)):
            response = self.client.post('/voice-api/v1/translate', headers={'X-Device-ID': 'billing-a', 'Authorization': 'Bearer token-a'}, json={'text': 'hi'})
        self.assertEqual(response.status_code, 502)
        events = self.store.recent_usage()
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]['cached_input_tokens'], 8)
        self.assertIsNotNone(events[0]['cost_amount'])
        self.assertEqual(events[0]['success'], 0)
        self.assertNotIn('fake-key', self.client.get('/admin/billing').text)


if __name__ == '__main__':
    unittest.main()
