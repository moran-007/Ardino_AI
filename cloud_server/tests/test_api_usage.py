import json
import sqlite3
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from contextlib import closing

from fastapi.testclient import TestClient

from cloud_server.app import create_app
from cloud_server.config import Settings
from cloud_server.admin_views import billing_content
from cloud_server.speech_fallback import record_cloud_usage
from cloud_server.store import Store
from cloud_server.usage_report import compact_device_usage, expense_history, usage_report


class ApiUsageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.settings = replace(Settings.for_test(Path(self.temp.name)), qwen_api_key='secret-new',
                                cloud_speech_api_key='speech-secret', cloud_tts_unit_price='.0001')
        self.store = Store(self.settings.database_path, self.settings.auth_pepper)
        self.store.register_device('device-a', 'A', 'token-a')
        self.store.register_device('device-b', 'B', 'token-b')

    def tearDown(self):
        self.temp.cleanup()

    def llm(self, key='secret-new', device='device-a', currency='CNY', amount='.1', model='qwen-plus'):
        self.store.record_usage(device_id=device, provider='qwen',
                                key_id=self.store.api_key_id('qwen', key, self.settings.qwen_base_url),
                                model=model, operation='dialogue', success=True, input_tokens=10,
                                output_tokens=5, total_tokens=15, cost_amount=amount,
                                cost_currency=currency, cost_status='priced' if amount is not None else 'unpriced')

    def test_key_rotation_currency_precision_and_model_separation(self):
        self.llm(key='old-secret', amount='.1')
        self.llm(key='old-secret', amount='.2')
        self.llm(amount='.000000001')
        self.llm(model='other-model', currency='USD', amount='.5')
        self.llm(device='device-b', amount='100')
        report = usage_report(self.store, self.settings, device_id='device-a')
        rows = [row for row in report['by_api'] if row['attempts']]
        self.assertEqual(len(rows), 3)
        self.assertEqual(report['totals']['llm'], {'CNY': '0.300000001', 'USD': '0.5'})
        old = next(row for row in rows if not row['current_key'])
        self.assertEqual(old['costs'], {'CNY': '0.3'})
        self.assertEqual(old['total_tokens'], 30)
        self.assertNotIn('secret', json.dumps(report))
        self.assertNotIn('key_id', json.dumps(report))

    def test_speech_credentials_history_and_types_are_separate(self):
        record_cloud_usage(self.settings, self.store, 'tts', 'device-a', 'sambert-zhida-v1', True, 10)
        record_cloud_usage(replace(self.settings, cloud_speech_api_key='old-speech'), self.store,
                           'tts', 'device-a', 'sambert-zhida-v1', False, 20)
        self.store.record_speech_usage('device-a', 'asr', 'legacy-model', True, '2.5', '秒', None, {})
        report = usage_report(self.store, self.settings)
        rows = [row for row in report['by_api'] if row['attempts']]
        self.assertEqual(len(rows), 3)
        self.assertEqual(report['totals']['speech'], {'CNY': '0.0010'})
        self.assertEqual(report['totals']['llm'], {})
        self.assertEqual(next(row for row in rows if row['current_key'])['quantity'], '10')
        self.assertEqual(next(row for row in rows if row['model'] == 'legacy-model')['key_label'], '未归属凭证／免 Key')
        self.assertEqual(sum(row['unpriced_calls'] for row in rows), 2)
        self.assertNotIn('speech-secret', json.dumps(report))

    def test_authenticated_device_endpoint_cannot_query_other_devices(self):
        self.llm()
        self.llm(device='device-b', amount='999', model='private-other-model')
        with TestClient(create_app(self.settings)) as client:
            path = '/voice-api/v1/device-usage'
            self.assertEqual(client.get(path).status_code, 401)
            headers = {'X-Device-ID': 'device-a', 'Authorization': 'Bearer token-a'}
            self.assertEqual(client.get(path, headers={**headers, 'X-Device-ID': 'device-b'}).status_code, 401)
            response = client.get(path + '?device_id=device-b', headers=headers)
            self.assertEqual(response.status_code, 200)
            self.assertEqual(response.headers['cache-control'], 'no-store')
            self.assertEqual(response.json()['totals']['llm'], {'CNY': '0.1'})
            self.assertNotIn('private-other-model', response.text)
            self.assertNotIn('secret-new', response.text)
            self.assertEqual(client.get(path + '?period=invalid', headers=headers).status_code, 422)

    def test_time_filter_missing_usage_and_configured_zero_calls(self):
        self.llm()
        with self.store._connect() as db:
            db.execute("UPDATE llm_usage SET created_at='2000-01-01T00:00:00+00:00'")
        report = usage_report(self.store, self.settings, '24h', 'device-a')
        self.assertEqual(report['totals']['llm'], {})
        self.assertTrue(any(row['provider'] == 'qwen' and row['current_key'] and row['attempts'] == 0 for row in report['by_api']))
        self.store.record_usage(device_id='device-a', provider='qwen', key_id='unknown-key',
                                model='unknown', operation='translation', success=False)
        row = next(row for row in usage_report(self.store, self.settings, '24h')['by_api'] if row['attempts'])
        self.assertEqual((row['failures'], row['unpriced_calls'], row['unreported_calls']), (1, 1, 1))

    def test_compact_report_rounds_cny_without_converting_foreign_costs(self):
        self.llm(amount='.005')
        self.llm(amount='.0001', model='small')
        self.llm(currency='USD', amount='999', model='foreign')
        self.llm(amount=None, model='unknown')
        self.llm(device='device-b', amount='900')
        self.store.record_speech_usage('device-a', 'tts', 'voice', True, '50', '字符', '.005', {})
        report = compact_device_usage(usage_report(self.store, self.settings, device_id='device-a'))
        self.assertEqual(report['totals'], {'llm': '0.01', 'speech': '0.01', 'total': '0.01'})
        rows = {row['model']: row for row in report['by_api'] if row['attempts']}
        self.assertEqual(rows['qwen-plus']['cost_cny'], '0.01')
        self.assertEqual(rows['small']['cost_cny'], '0.00')
        self.assertIsNone(rows['foreign']['cost_cny'])
        self.assertTrue(rows['foreign']['foreign_cost'])
        self.assertEqual(report['unpriced_calls'], 1)
        self.assertTrue(report['foreign_cost'])
        self.assertEqual((report['calls'], report['tokens']), (5, 60))
        self.assertNotIn('USD', json.dumps(report))
        self.assertNotIn('statuses', rows['small'])

    def test_compact_report_empty_partial_and_zero_are_distinct(self):
        report = compact_device_usage(usage_report(self.store, self.settings, device_id='device-a'))
        self.assertEqual(report['totals'], {'llm': None, 'speech': None, 'total': None})
        self.assertEqual(report['calls'], 0)
        self.store.record_usage(device_id='device-a', provider='qwen', key_id='key', model='partial',
                                operation='dialogue', success=True, input_tokens=0, output_tokens=0,
                                total_tokens=0, cost_amount='0', cost_currency='CNY', cost_status='partial')
        report = compact_device_usage(usage_report(self.store, self.settings, device_id='device-a'))
        self.assertEqual(report['totals']['total'], '0.00')
        self.assertEqual(report['partial_calls'], 1)
        self.assertEqual(report['unpriced_calls'], 0)

    def test_compact_endpoint_keeps_auth_limits_and_full_precision_default(self):
        for index in range(41):
            self.llm(amount='.005', model=f'model-{index}')
        with TestClient(create_app(self.settings)) as client:
            path = '/voice-api/v1/device-usage?view=compact'
            headers = {'X-Device-ID': 'device-a', 'Authorization': 'Bearer token-a'}
            self.assertEqual(client.get(path).status_code, 401)
            self.assertEqual(client.get(path, headers={**headers, 'X-Device-ID': 'device-b'}).status_code, 401)
            response = client.get(path + '&device_id=device-b', headers=headers)
            self.assertEqual(response.status_code, 200)
            self.assertEqual(response.headers['cache-control'], 'no-store')
            report = response.json()
            self.assertEqual(len(report['by_api']), 40)
            self.assertGreater(report['omitted_rows'], 0)
            self.assertEqual(report['totals']['total'], '0.21')
            self.assertEqual(report['calls'], 41)
            self.assertNotIn('secret', response.text)
            full = client.get('/voice-api/v1/device-usage', headers=headers).json()
            self.assertEqual(full['totals']['llm'], {'CNY': '0.205'})
            self.assertLess(len(response.content), len(json.dumps(full).encode()))
            self.assertEqual(client.get(path + '&period=bad', headers=headers).status_code, 422)
            self.assertEqual(client.get('/voice-api/v1/device-usage?view=bad', headers=headers).status_code, 422)

    def test_old_speech_table_migration_preserves_unattributed_records(self):
        path = Path(self.temp.name) / 'legacy.db'
        with closing(sqlite3.connect(path)) as db, db:
            db.execute('CREATE TABLE speech_usage (usage_id INTEGER PRIMARY KEY,device_id TEXT,operation TEXT,model TEXT,success INTEGER,quantity TEXT,unit TEXT,estimated_cost TEXT,price_snapshot TEXT,created_at TEXT)')
            db.execute("INSERT INTO speech_usage VALUES(1,'device-a','tts','old',1,'4','字符','.01','{}','2000-01-01')")
        store = Store(path, 'pepper')
        self.assertEqual(store.recent_speech_usage()[0]['key_id'], '')
        self.assertEqual(store.api_usage_summary()['totals']['speech'], {'CNY': '0.01'})
        Store(path, 'pepper')
        self.assertEqual(len(store.recent_speech_usage()), 1)

    def test_admin_billing_renders_grouped_costs_and_escapes_model(self):
        self.llm(model='<script>unsafe</script>')
        content = billing_content(self.settings, self.store.billing_summary(), [], 'all', 'csrf',
                                  api_report=usage_report(self.store, self.settings))
        self.assertIn('各 API 凭证费用与用量', content)
        self.assertIn('0.1 CNY', content)
        self.assertIn('&lt;script&gt;unsafe&lt;/script&gt;', content)
        self.assertNotIn('<script>unsafe</script>', content)
        self.assertNotIn('secret-new', content)

    def test_expense_curves_include_full_ledger_and_use_local_calendar_dates(self):
        for _ in range(40):
            self.llm(amount='.0001')
            self.store.record_speech_usage('device-a', 'tts', 'voice', True, '2', '字符', '.0002',
                                           {'unit_price': '.0001'})
        with self.store._connect() as db:
            db.execute("UPDATE llm_usage SET created_at='2026-09-30T16:10:00+00:00',price_snapshot=?",
                       (json.dumps({'currency': 'CNY', 'tiers': [{'max_input_tokens': 100, 'input': '.8', 'output': '2', 'cached_input': '.16'}]}),))
            db.execute("UPDATE speech_usage SET created_at='2026-09-30T16:10:00+00:00'")
        self.llm(device='device-b', amount='999')
        report = usage_report(self.store, self.settings, 'all', 'device-a')
        self.assertEqual(report['cost_series']['day'], [{'date': '2026-10-01', 'costs': {'CNY': '0.0120'},
                                                       'calls': 80, 'unpriced_calls': 0, 'partial_calls': 0}])
        self.assertEqual(report['cost_series']['week'][0]['date'], '2026-09-28')
        self.assertEqual(report['cost_series']['month'][0]['date'], '2026-10-01')
        self.assertEqual(len(report['expense_rows']), 2)
        llm = next(row for row in report['expense_rows'] if row['kind'] == 'llm')
        self.assertEqual(llm['unit_price'], '输入 .8／输出 2／缓存 .16 CNY／百万 Token')
        self.assertEqual(llm['input_tokens'], 400)
        compact = compact_device_usage(report)
        self.assertEqual(compact['cost_series']['day'][0]['cost'], '0.0120')
        self.assertEqual(compact['totals']['total'], '0.01')

    def test_expense_calendar_gaps_unknown_prices_and_currency_separation(self):
        base = dict(kind='llm', model='same-model', operation='dialogue', input_tokens=10,
                    output_tokens=5, total_tokens=15, cost_currency='CNY', cost_status='priced', price_snapshot='')
        history = expense_history([
            dict(base, created_at='2025-12-31T15:59:59+00:00', cost_amount='.1'),
            dict(base, created_at='2026-01-01T16:00:00+00:00', cost_amount='.2'),
            dict(base, created_at='2026-01-01T16:00:00+00:00', cost_amount='.3', cost_currency='USD'),
            dict(base, created_at='2026-01-02T16:00:00+00:00', cost_amount=None, total_tokens=None),
        ])
        daily = history['cost_series']['day']
        self.assertEqual([row['date'] for row in daily], ['2025-12-31', '2026-01-01', '2026-01-02', '2026-01-03'])
        self.assertEqual((daily[1]['calls'], daily[1]['costs']), (0, {}))
        self.assertEqual(daily[2]['costs'], {'CNY': '0.2', 'USD': '0.3'})
        self.assertEqual((daily[3]['calls'], daily[3]['costs'], daily[3]['unpriced_calls']), (1, {}, 1))
        self.assertEqual(history['cost_series']['week'][0]['costs'], {'CNY': '0.3', 'USD': '0.3'})
        self.assertEqual([row['date'] for row in history['cost_series']['month']], ['2025-12-01', '2026-01-01'])
        unpriced = next(row for row in history['expense_rows'] if row['cost'] is None)
        self.assertEqual((unpriced['unit_price'], unpriced['unreported_calls']), ('未记录单价', 1))

    def test_date_rows_keep_historical_price_changes_and_zero_cost(self):
        base = dict(kind='speech', model='voice', operation='tts', created_at='2026-10-01T00:00:00+00:00',
                    quantity='10', unit='字符')
        history = expense_history([dict(base, estimated_cost='.001', price_snapshot='{"unit_price":".0001"}'),
                                   dict(base, estimated_cost='.002', price_snapshot='{"unit_price":".0002"}'),
                                   dict(base, estimated_cost='0', price_snapshot='{}'),
                                   dict(base, estimated_cost=None, price_snapshot='{}')])
        self.assertEqual(len(history['expense_rows']), 3)
        unknown = next(row for row in history['expense_rows'] if row['unit_price'] == '未记录单价')
        self.assertEqual((unknown['cost'], unknown['unpriced_calls']), ('0', 1))
        self.assertEqual(history['cost_series']['day'][0]['costs'], {'CNY': '0.003'})

    def test_billing_page_has_compact_table_curves_and_collapsed_price_configuration(self):
        self.llm()
        report = usage_report(self.store, self.settings)
        content = billing_content(self.settings, self.store.billing_summary(), [], 'all', 'csrf', api_report=report)
        self.assertIn('<th>日期</th><th>使用量</th><th>单价</th><th>总价格</th>', content)
        self.assertIn('<option value="day">日</option>', content)
        self.assertIn('<option value="week">周</option>', content)
        self.assertIn('<option value="month">月</option>', content)
        self.assertIn('<details><summary>价格配置与统计开关</summary>', content)
        self.assertNotIn('最近调用与单价快照', content)

    def test_compact_long_history_fits_firmware_limit_without_losing_accounting_totals(self):
        for index in range(200):
            self.llm(amount='.001', model='long-model-' + str(index % 40))
            with self.store._connect() as db:
                db.execute("UPDATE llm_usage SET created_at=datetime('2026-01-01',? || ' days') WHERE usage_id=?",
                           (index, index + 1))
        report = compact_device_usage(usage_report(self.store, self.settings, 'all', 'device-a'))
        encoded = json.dumps(report, ensure_ascii=False, separators=(',', ':')).encode()
        self.assertLessEqual(len(encoded), 28000)
        self.assertEqual(report['totals']['total'], '0.20')
        self.assertEqual((report['calls'], report['tokens']), (200, 3000))
        self.assertGreater(report['omitted_expense_rows'], 0)
        self.assertGreaterEqual(len(report['expense_rows']), 10)
        self.assertTrue(report['series_truncated'])
        self.assertEqual(report['cost_series']['month'][-1]['date'], '2026-07-01')
