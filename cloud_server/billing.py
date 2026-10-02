from __future__ import annotations

import json
import re
from datetime import datetime, timedelta, timezone
from decimal import Decimal, InvalidOperation
from urllib.parse import urlsplit

CHECKED_AT = '2026-10-01'
QWEN_SOURCE = 'https://help.aliyun.com/zh/model-studio/model-pricing'
DEEPSEEK_SOURCE = 'https://api-docs.deepseek.com/quick_start/pricing/'
GLM_SOURCE = 'https://www.zhipuai.cn/zh/news/148'
SEARCH_SOURCE = 'https://help.aliyun.com/zh/model-studio/web-search/'
CACHE_SOURCE = 'https://help.aliyun.com/zh/model-studio/context-cache'
HOLIDAY_SOURCE = 'https://www.beijing.gov.cn/cs/gncs/zcwj/202603/t20260327_4568275.html'
HOLIDAYS_2026 = (('01-01', '01-03'), ('02-15', '02-23'), ('04-04', '04-06'),
                 ('05-01', '05-05'), ('06-19', '06-21'), ('09-25', '09-27'), ('10-01', '10-07'))


def money(value):
    try:
        amount = Decimal(str(value))
    except (InvalidOperation, ValueError):
        raise ValueError('单价须为非负数字') from None
    if not amount.is_finite() or not 0 <= amount <= 100000 or amount.as_tuple().exponent < -8:
        raise ValueError('单价须在 0～100000 之间，最多 8 位小数')
    return format(amount, 'f')


def manual_rule(data):
    currency = data.get('currency', 'CNY')
    if currency not in {'CNY', 'USD'}:
        raise ValueError('币种无效')
    tiers = data.get('tiers')
    if not isinstance(tiers, list) or not 1 <= len(tiers) <= 8:
        raise ValueError('需要 1～8 档单价')
    normalized, previous = [], 0
    for tier in tiers:
        limit = tier.get('max_input_tokens')
        if type(limit) is not int or not previous < limit <= 10000000:
            raise ValueError('输入 Token 档位须递增且不超过 10000000')
        previous = limit
        normalized.append({'max_input_tokens': limit, 'input': money(tier.get('input')),
                           'output': money(tier.get('output')),
                           'cached_input': money(tier['cached_input']) if tier.get('cached_input') not in (None, '') else None})
    return {'currency': currency, 'tiers': normalized, 'search': money(data['search']) if data.get('search') not in (None, '') else None,
            'mode': 'manual', 'source': '管理员手动设置',
            'checked_at': datetime.now(timezone(timedelta(hours=8))).date().isoformat()}


def rule_key(provider, model):
    if provider not in {'deepseek', 'qwen', 'glm', 'custom', 'local_llm'} or not re.fullmatch(r'[A-Za-z0-9_./:-]{1,128}', model):
        raise ValueError('Provider 或模型名称无效')
    return provider + ':' + model


def _rule(currency, source, tiers, *, cache_ratio=None, search=None):
    return {'currency': currency, 'mode': 'official', 'source': source, 'checked_at': CHECKED_AT,
            'search': search, 'tiers': [dict(max_input_tokens=limit, input=inp, output=out,
                cached_input=money(Decimal(inp) * Decimal(cache_ratio)) if cache_ratio else None)
                for limit, inp, out in tiers]}


def official_rule(provider, model, base_url, at=None):
    host = (urlsplit(base_url).hostname or '').lower()
    at = at or datetime.now(timezone.utc)
    if provider == 'qwen' and (host == 'dashscope.aliyuncs.com' or host.endswith('.cn-beijing.maas.aliyuncs.com')):
        prices = {
            'qwen-plus': [(128000, '0.8', '2'), (256000, '2.4', '20'), (1000000, '4.8', '48')],
            'qwen-plus-latest': [(128000, '0.8', '2'), (256000, '2.4', '20'), (1000000, '4.8', '48')],
            'qwen3.5-plus': [(128000, '0.8', '4.8'), (256000, '2', '12'), (1000000, '4', '24')],
            'qwen3.6-plus': [(256000, '2', '12'), (1000000, '8', '48')],
            'qwen-max': [(32768, '2.4', '9.6')],
        }
        if model in prices:
            rule = _rule('CNY', QWEN_SOURCE, prices[model], cache_ratio='0.2', search='0.003')
            rule['note'] = '北京地域、非思考、实时调用；隐式缓存按输入价 20%；turbo 搜索每次 0.003 元。'
            rule['cache_source'], rule['search_source'] = CACHE_SOURCE, SEARCH_SOURCE
            return rule
    if provider == 'glm' and host == 'open.bigmodel.cn' and model.lower() == 'glm-4.7-flash':
        return _rule('CNY', GLM_SOURCE, [(10000000, '0', '0')], cache_ratio='1')
    if provider == 'deepseek' and host == 'api.deepseek.com' and model in {
        'deepseek-flash', 'deepseek-v4-flash', 'deepseek-v4-flash-vision-exp', 'deepseek-v4-pro'}:
        local = at.astimezone(timezone(timedelta(hours=8)))
        holiday = any(start <= local.strftime('%m-%d') <= end for start, end in HOLIDAYS_2026)
        candidate = local.weekday() < 5 and (9 <= local.hour < 12 or 14 <= local.hour < 18)
        if local.year != 2026 and candidate:
            return None  # Do not guess future holiday peak pricing.
        peak = candidate and not holiday
        inp, out, cached = ('0.66', '1.98', '0.022') if model == 'deepseek-v4-pro' else ('0.15', '0.6', '0.003')
        factor = Decimal(2 if peak else 1)
        rule = _rule('USD', DEEPSEEK_SOURCE, [(1000000, money(Decimal(inp) * factor), money(Decimal(out) * factor))])
        rule['tiers'][0]['cached_input'] = money(Decimal(cached) * factor)
        rule['note'] = ('高峰' if peak else '空闲') + '时段；采用官方美元刊例价，节假日表已核对 2026 年；不是人民币账户扣款。'
        rule['holiday_source'] = HOLIDAY_SOURCE
        return rule
    return None


def selected_rule(settings, provider, model, base_url, at=None):
    override = json.loads(settings.billing_rules).get(provider + ':' + model)
    return override or official_rule(provider, model, base_url, at)


def calculate(settings, provider, model, base_url, usage, at):
    result = {'cost_amount': None, 'cost_currency': '', 'cost_status': 'unpriced',
              'price_snapshot': '', 'cost_note': ''}
    if not settings.billing_enabled:
        return dict(result, cost_status='disabled', cost_note='本次调用时费用统计关闭')
    rule = selected_rule(settings, provider, model, base_url, at)
    if not rule:
        return dict(result, cost_note='没有可核实的官方规则，请手动设置；不按零费用处理')
    result.update(cost_currency=rule['currency'], price_snapshot=json.dumps(rule, ensure_ascii=False, separators=(',', ':')))
    if usage.input_tokens is None or usage.output_tokens is None:
        return dict(result, cost_note='厂商未回传完整 Token 用量')
    tier = next((r for r in rule['tiers'] if usage.input_tokens <= r['max_input_tokens']), None)
    if tier is None:
        return dict(result, cost_note='输入 Token 超过已配置档位')
    notes, status = [], 'priced'
    cached = usage.cached_input_tokens
    if cached is None:
        cached = 0
        if Decimal(tier['input']) != 0:
            notes.append('未回传缓存命中量，输入按未命中估算')
            status = 'estimated'
    cache_rate = tier.get('cached_input')
    if cached and cache_rate is None:
        return dict(result, cost_note='回传了缓存命中量，但没有缓存单价')
    amount = (Decimal(usage.input_tokens - cached) * Decimal(tier['input'])
              + Decimal(cached) * Decimal(cache_rate or '0')
              + Decimal(usage.output_tokens) * Decimal(tier['output'])) / Decimal(1000000)
    if usage.search_calls is None or (usage.search_calls and rule.get('search') is None):
        status = 'partial'
        notes.append('仅计模型费用，搜索次数或搜索单价未确认')
    elif usage.search_calls:
        amount += Decimal(usage.search_calls) * Decimal(rule['search'])
    return dict(result, cost_amount=format(amount, 'f'), cost_status=status, cost_note='；'.join(notes))
