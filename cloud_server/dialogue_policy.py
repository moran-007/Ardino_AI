from datetime import datetime, timedelta, timezone
import re
from decimal import Decimal, InvalidOperation


STYLE_INSTRUCTIONS = {
    'brief': '一般问题简短回答，但用户明确要求详细、故事或步骤时按要求完整展开。',
    'normal': '一般问题按需要完整回答，不要强制压缩成一两句；简单问题仍简洁。',
    'detailed': '需要解释的问题详细展开，给出原因、步骤或例子；简单问候和设备操作仍简短。',
}


def reply_instruction(device, default_style):
    style = (device or {}).get('reply_style', 'default')
    if style == 'default':
        style = default_style
    return '\n\n回答风格（优先于上文的简洁偏好）：' + STYLE_INSTRUCTIONS.get(style, STYLE_INSTRUCTIONS['normal'])


WEATHER_WORDS = ('天气', '气温', '温度', '下雨')
LOCAL_WEATHER_QUERY = re.compile(
    r'(?:你好|请问|告诉我|帮我|查一下|查询|报一下|看一下|现在|今天|明天|后天|'
    r'当地|本地|这里|这边|的|天气|气温|温度|多少度|多少|几度|怎么样|怎样|如何|'
    r'好不好|会不会|是否|下雨|需要|不需要|带伞|冷不冷|热不热|啊|呀|呢|吗|还有|最新|实时)*')


def local_weather_covers(question, context, now=None):
    # Only reuse clearly fresh, matching-location data for known query shapes.
    if any(word in context for word in ('过期', '缺失', '尚未', '失败', '无效')):
        return False
    age = re.search(r'缓存年龄[：:]\s*(\d+)分钟', context)
    location = re.search(r'^地点[：:]([^；;\n]+)', context)
    if not age or int(age[1]) >= 90 or not location:
        return False
    if not re.search(r'当前[：:][^；;\n]*\d+(?:\.\d+)?(?:℃|度)', context):
        return False
    now = (now or datetime.now(timezone.utc)).astimezone(timezone(timedelta(hours=8)))
    for day, offset in (('明天', 1), ('后天', 2)):
        if day in question and day not in context and (now + timedelta(days=offset)).strftime('%m-%d') not in context:
            return False
    normalized = re.sub(r'[\s，。！？,.!?]', '', question)
    name = location[1].strip()
    names = [name, name[:-1]] if name.endswith('市') else [name, name + '市']
    for candidate in names:
        if candidate in normalized:
            normalized = normalized.replace(candidate, '', 1)
            break
    return bool(LOCAL_WEATHER_QUERY.fullmatch(normalized))


def needs_search(question, weather_context='', memo_context=''):
    weather = any(word in question for word in WEATHER_WORDS)
    other_realtime = any(word in question for word in ('新闻', '股价', '实时汇率'))
    if other_realtime:
        return True
    if weather:
        return not local_weather_covers(question, weather_context)
    if memo_context and any(word in question for word in ('便签', '待办')):
        return False
    return any(word in question for word in ('最新', '实时'))


def local_answer(question, weather_context='', memo_context='', now=None, personal_profile=None):
    """Answer only bounded, unambiguous read-only queries without an LLM."""
    normalized = re.sub(r'[\s，。！？,.!?]', '', question)
    now = (now or datetime.now(timezone.utc)).astimezone(timezone(timedelta(hours=8)))
    if normalized in ('早上好', '每日简报', '今天有什么安排'):
        name = ''
        if personal_profile and personal_profile.get('enabled'):
            for item in personal_profile.get('items', []):
                if (item.get('category') == 'name' and item.get('source') == 'explicit' and
                        re.fullmatch(r'[\u4e00-\u9fffA-Za-z0-9 _-]{1,16}', item.get('value', ''))):
                    name = item['value'].strip()
        parts = [(name + '，' if name else '') + '早上好。']
        parts.append(f'今天是{now.month}月{now.day}日，星期{"一二三四五六日"[now.weekday()]}，现在{now.hour}点{now.minute:02d}分。')
        weather = local_answer('今天天气怎么样', weather_context, now=now)
        if weather is not None:
            parts.append('天气：' + weather)
        else:
            age = re.search(r'缓存年龄[：:]\s*(\d+)分钟', weather_context)
            stale = '过期' in weather_context or bool(age and int(age[1]) >= 90)
            parts.append('天气数据已过期，暂不能确认当前天气。' if stale else
                         '设备未提供有效天气数据，暂不能确认当前天气。')
        snapshot = re.fullmatch(r'便签 \d+ 条，待办 \d+ 条[^；]*；便签：(.*?)；待办：(.*)', memo_context)
        if snapshot:
            # These are current lists, not evidence of appointments on today's date.
            parts.append('设备当前便签：' + snapshot[1][:512] + '；待办：' + snapshot[2][:512] + '。')
        else:
            parts.append('设备未提供完整便签和待办数据，无法确认今天的安排。')
        return ''.join(parts)
    if local_weather_covers(question, weather_context, now):
        location = re.search(r'^地点[：:]([^；;\n]+)', weather_context)[1].strip()
        current = re.search(r'当前[：:]([^；;\n]+)', weather_context)[1].strip()
        requested = [(label, offset) for label, offset in (('明天', 1), ('后天', 2)) if label in question]
        forecast = re.search(r'未来三天[：:]([^；;\n]+)', weather_context)
        days = forecast[1].split('，') if forecast else []
        if requested:
            selected = []
            for label, offset in requested:
                date = (now + timedelta(days=offset)).strftime('%m-%d')
                entry = next((item.strip() for item in days if re.match(rf'^(?:{label}|{date})\s', item.strip())), None)
                if entry is None:
                    return None
                selected.append(re.sub(r'^' + re.escape(date), label, entry))
            return location + '，' + '；'.join(selected) + '。'
        today = next((item.strip() for item in days if item.strip().startswith('今天 ')), '')
        return location + '当前' + current + ('；' + today if today else '') + '。'
    memo_query = re.fullmatch(r'(?:请|帮我|查一下|查询|查看|看看|列出|读一下|我的|当前|最新|有哪些|有什么|是什么|的|便签|待办|事项|和|及|全部|所有|吗|呢|呀)+', normalized)
    if memo_query and ('便签' in question or '待办' in question) and memo_context:
        snapshot = re.fullmatch(r'便签 \d+ 条，待办 \d+ 条[^；]*；便签：(.*?)；待办：(.*)', memo_context)
        if snapshot:
            return '；'.join(label + '：' + snapshot[index] for label, index in (('便签', 1), ('待办', 2)) if label in question) + '。'
    if re.fullmatch(r'(?:你好|请问|告诉我|现在|当前|是|几点|几点钟|时间|多少|了|啊|呀|呢)+', normalized) and ('几点' in normalized or '时间' in normalized):
        return f'现在是北京时间{now.hour}点{now.minute:02d}分。'
    if normalized in ('今天几号', '今天是几号', '今天星期几', '今天是星期几', '今天日期', '今天日历'):
        return f'今天是{now.year}年{now.month}月{now.day}日，星期{"一二三四五六日"[now.weekday()]}。'
    calculation = re.fullmatch(r'(\d{1,9}(?:\.\d{1,6})?)(加|减|乘以|乘|除以|除|\+|-|×|\*|÷|/)(\d{1,9}(?:\.\d{1,6})?)(?:等于几|等于多少|是多少|等于|=)?', question.strip().rstrip('。？！?！'))
    if calculation:
        left, operator, right = calculation.groups()
        a, b = Decimal(left), Decimal(right)
        if operator in ('除以', '除', '÷', '/') and b == 0:
            return '除数不能为零。'
        try:
            if operator in ('加', '+'):
                value = a + b
            elif operator in ('减', '-'):
                value = a - b
            elif operator in ('乘以', '乘', '×', '*'):
                value = a * b
            else:
                value = a / b
            text = format(value, 'f').rstrip('0').rstrip('.') if '.' in format(value, 'f') else format(value, 'f')
            if len(text) > 24:
                return None
            return f'{left}{operator}{right}等于{text}。'
        except InvalidOperation:
            return None
    return None


def realtime_instruction(search):
    now = datetime.now(timezone(timedelta(hours=8))).strftime('%Y-%m-%d %H:%M')
    capability = ('本次已启用联网搜索，实时问题以搜索结果为依据，注意目标城市和预报日期。'
                  if search else '本次未启用联网搜索；实时信息只能依据本轮提供的设备数据，没有对应数据就如实说明，不能猜测。')
    return '\n\n当前北京时间：' + now + '。' + capability
