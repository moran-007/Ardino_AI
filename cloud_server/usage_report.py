import json
from datetime import datetime, timedelta, timezone
from decimal import Decimal, ROUND_HALF_UP, localcontext

from .model_catalog import PROVIDERS


DISPLAY_ZONE = timezone(timedelta(hours=8))


def recorded_price(record):
    """Display the rule applied at call time, never today's configured price."""
    try:
        snapshot = json.loads(record.get('price_snapshot') or '{}')
    except (ValueError, TypeError):
        return '未记录单价'
    if not isinstance(snapshot, dict):
        return '未记录单价'
    if record['kind'] == 'speech':
        price = snapshot.get('unit_price')
        return str(price) + ' CNY／' + record['unit'] if price is not None else '未记录单价'
    tokens = record.get('input_tokens')
    tier = next((tier for tier in snapshot.get('tiers', [])
                 if tokens is not None and tokens <= tier['max_input_tokens']), None)
    if tier is None:
        return '未记录单价'
    prices = '输入 ' + str(tier['input']) + '／输出 ' + str(tier['output'])
    if tier.get('cached_input') is not None:
        prices += '／缓存 ' + str(tier['cached_input'])
    prices += ' ' + snapshot.get('currency', record.get('cost_currency', '')) + '／百万 Token'
    if record.get('search_calls'):
        price = snapshot.get('search')
        prices += '；搜索 ' + (str(price) + '／次' if price is not None else '单价未确认')
    return prices


def expense_history(records):
    """Aggregate all ledger rows in China local dates, keeping currencies apart."""
    groups, buckets = {}, {name: {} for name in ('day', 'week', 'month')}
    for record in records:
        date = datetime.fromisoformat(record['created_at'].replace('Z', '+00:00'))
        if date.tzinfo is None:
            date = date.replace(tzinfo=timezone.utc)
        date = date.astimezone(DISPLAY_ZONE).date()
        speech = record['kind'] == 'speech'
        currency = 'CNY' if speech else record.get('cost_currency', '')
        amount = record.get('estimated_cost') if speech else record.get('cost_amount')
        price = recorded_price(record)
        unit = record['unit'] if speech else 'Token'
        key = (date.isoformat(), record['kind'], record['model'], record['operation'],
               currency, price, unit)
        row = groups.setdefault(key, dict(date=key[0], kind=record['kind'], model=record['model'],
                                operation=record['operation'], currency=currency, unit_price=price,
                                input_tokens=0, output_tokens=0, quantity=Decimal(0), unit=unit,
                                cost=None, calls=0, unpriced_calls=0, unreported_calls=0, partial_calls=0))
        row['calls'] += 1
        row['unpriced_calls'] += amount is None
        row['partial_calls'] += record.get('cost_status') == 'partial'
        if speech:
            row['quantity'] += Decimal(record['quantity'])
        else:
            row['input_tokens'] += record.get('input_tokens') or 0
            row['output_tokens'] += record.get('output_tokens') or 0
            row['unreported_calls'] += record.get('total_tokens') is None
        if amount is not None:
            row['cost'] = (row['cost'] or Decimal(0)) + Decimal(amount)
        labels = {'day': date.isoformat(), 'week': (date - timedelta(days=date.weekday())).isoformat(),
                  'month': date.replace(day=1).isoformat()}
        for granularity, label in labels.items():
            bucket = buckets[granularity].setdefault(label, {'date': label, 'costs': {}, 'calls': 0,
                                                             'unpriced_calls': 0, 'partial_calls': 0})
            bucket['calls'] += 1
            bucket['unpriced_calls'] += amount is None
            bucket['partial_calls'] += record.get('cost_status') == 'partial'
            if amount is not None:
                bucket['costs'][currency] = bucket['costs'].get(currency, Decimal(0)) + Decimal(amount)
    rows = sorted(groups.values(), key=lambda row: (row['date'], row['model']), reverse=True)
    for row in rows:
        row['quantity'] = format(row['quantity'], 'f')
        row['cost'] = format(row['cost'], 'f') if row['cost'] is not None else None
    series = {}
    for granularity, items in buckets.items():
        series[granularity] = []
        if not items:
            continue
        date, last = min(items), max(items)
        while date <= last:
            bucket = items.get(date, {'date': date, 'costs': {}, 'calls': 0, 'unpriced_calls': 0, 'partial_calls': 0})
            series[granularity].append({**bucket, 'costs': {c: format(v, 'f') for c, v in bucket['costs'].items()}})
            start = datetime.fromisoformat(date).date()
            if granularity == 'month':
                start = start.replace(year=start.year + (start.month == 12), month=start.month % 12 + 1)
            else:
                start += timedelta(days=7 if granularity == 'week' else 1)
            date = start.isoformat()
    return {'expense_rows': rows, 'cost_series': series, 'date_timezone': 'Asia/Shanghai'}


def compact_device_usage(report):
    """Keep exact ledger values on the server; send only display-ready CNY fees."""
    def yuan(value):
        if value is None:
            return None
        amount = Decimal(value)
        with localcontext() as context:
            context.prec = max(28, len(amount.as_tuple().digits) + abs(amount.adjusted()) + 4)
            return format(amount.quantize(Decimal('0.01'), rounding=ROUND_HALF_UP), '.2f')

    totals = report['totals']
    known = [Decimal(values['CNY']) for values in totals.values() if 'CNY' in values]
    rows = []
    for row in report['by_api']:
        item = {key: row[key] for key in ('kind', 'provider_label', 'key_label', 'current_key',
                                         'model', 'operation', 'attempts', 'successes', 'failures',
                                         'input_tokens', 'output_tokens', 'quantity', 'unit',
                                         'unpriced_calls', 'unreported_calls')}
        item.update(cost_cny=yuan(row['costs'].get('CNY')),
                    partial_calls=row['statuses'].get('partial', 0),
                    foreign_cost=any(currency != 'CNY' for currency in row['costs']))
        rows.append(item)
    expenses = []
    for row in report['expense_rows'][:60]:
        item = {key: row[key] for key in ('date', 'model', 'operation', 'kind', 'quantity', 'unit',
                                         'input_tokens', 'output_tokens', 'unpriced_calls',
                                         'unreported_calls', 'partial_calls')}
        item.update(unit_price=row['unit_price'] if row['currency'] in ('', 'CNY') else '非人民币未计入',
                    cost_cny=yuan(row['cost']) if row['currency'] == 'CNY' else None)
        expenses.append(item)
    limits = {'day': 180, 'week': 104, 'month': 60}
    series = {name: [{'date': row['date'], 'cost': row['costs'].get('CNY'),
                      'calls': row['calls'], 'unpriced_calls': row['unpriced_calls'],
                      'partial_calls': row['partial_calls']} for row in values[-limits[name]:]]
              for name, values in report['cost_series'].items()}
    result = {'period': report['period'], 'updated_at': report['updated_at'], 'by_api': rows[:40],
            'omitted_rows': max(0, len(rows) - 40),
            'expense_rows': expenses, 'omitted_expense_rows': max(0, len(report['expense_rows']) - 60),
            'cost_series': series, 'series_truncated': any(len(report['cost_series'][name]) > limit
                                                         for name, limit in limits.items()),
            'totals': {'llm': yuan(totals['llm'].get('CNY')),
                       'speech': yuan(totals['speech'].get('CNY')),
                       'total': yuan(sum(known)) if known else None},
            'calls': sum(row['attempts'] for row in rows),
            'tokens': sum(row['total_tokens'] for row in report['by_api']),
            'unpriced_calls': sum(row['unpriced_calls'] for row in rows),
            'partial_calls': sum(row['partial_calls'] for row in rows),
            'foreign_cost': any(row['foreign_cost'] for row in rows),
            'note': '仅显示人民币已计价费用，四舍五入至两位小数；非人民币费用不换算。未扣赠送额度、折扣或资源包，实际扣费以厂商账单为准。'}
    # Leave headroom under the firmware's 32 KiB streaming limit. Trimming is
    # display-only: all accounting totals above still include the entire ledger.
    while len(json.dumps(result, ensure_ascii=False, separators=(',', ':')).encode()) > 28000:
        if len(result['expense_rows']) > 10:
            result['expense_rows'].pop()
            result['omitted_expense_rows'] += 1
            continue
        name = max(series, key=lambda key: len(series[key]))
        if len(series[name]) > 8:
            series[name].pop(0)
            result['series_truncated'] = True
            continue
        if len(result['by_api']) > 5:
            result['by_api'].pop()
            result['omitted_rows'] += 1
            continue
        if result['expense_rows']:
            result['expense_rows'].pop()
            result['omitted_expense_rows'] += 1
            continue
        if len(series[name]) > 2:
            series[name].pop(0)
            result['series_truncated'] = True
            continue
        if result['by_api']:
            result['by_api'].pop()
            result['omitted_rows'] += 1
            continue
        break
    return result


def period_since(period, now=None):
    now = now or datetime.now(timezone.utc)
    return (now - timedelta(days={'24h': 1, '7d': 7, '30d': 30}[period])).isoformat() if period != 'all' else None


def usage_report(store, settings, period='all', device_id=None):
    since = period_since(period)
    report = store.api_usage_summary(since, device_id)
    history = expense_history(store.api_usage_records(since, device_id))
    current = {name: store.api_key_id(name, getattr(settings, name + '_api_key'), getattr(settings, name + '_base_url'))
               for name in PROVIDERS}
    current['cloud_speech'] = store.api_key_id('cloud_speech', settings.cloud_speech_api_key, settings.cloud_speech_url)
    configured = [('llm', name, getattr(settings, name + '_model'), 'dialogue') for name in PROVIDERS
                  if current[name] or name in settings.llm_provider_order]
    if current['cloud_speech']:
        configured += [('speech', 'cloud_speech', getattr(settings, 'cloud_' + operation + '_model'), operation)
                       for operation in ('asr', 'tts')]
    for kind, provider, model, operation in configured:
        if any(row['provider'] == provider and row['key_id'] == current[provider] and row['model'] == model
               for row in report['by_api']):
            continue
        report['by_api'].append({'kind': kind, 'provider': provider, 'key_id': current[provider],
                                'model': model, 'operation': operation, 'attempts': 0, 'successes': 0,
                                'failures': 0, 'unpriced_calls': 0, 'unreported_calls': 0,
                                'input_tokens': 0, 'output_tokens': 0, 'total_tokens': 0,
                                'quantity': '0', 'unit': 'Token' if kind == 'llm' else '秒' if operation == 'asr' else '字符',
                                'costs': {}, 'statuses': {}, 'last_used': ''})
    for row in report['by_api']:
        row['provider_label'] = '百炼云端语音' if row['provider'] == 'cloud_speech' else PROVIDERS.get(row['provider'], {}).get('label', row['provider'])
        row['key_label'] = row['key_id'][:12] if row['key_id'] else '未归属凭证／免 Key'
        row['current_key'] = bool(row['key_id'] and current.get(row['provider']) == row['key_id'])
        del row['key_id']
    return {'period': period, 'updated_at': datetime.now(timezone.utc).isoformat(), **report, **history,
            'note': '仅统计本服务已记录的调用；费用未扣赠送额度、折扣或资源包，余额与实际扣费以厂商账单为准。'}
