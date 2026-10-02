from decimal import Decimal
from pathlib import Path
import tempfile
import re
import time
from urllib.parse import urlsplit

from .billing import CHECKED_AT, QWEN_SOURCE

ROUTES = {'local_only', 'local_first', 'cloud_first', 'cloud_only'}
ASR_MODELS = {'paraformer-realtime-v2', 'paraformer-realtime-v1',
              'qwen-audio-3.0-asr-flash-streaming', 'qwen-audio-3.1-asr-flash-streaming'}
TEST_PHRASE = '今天天气怎么样'
SAMBERT_VOICES = {
    '男声': [('zhida', '知达'), ('zhide', '知德'), ('zhishu', '知树'), ('zhiye', '知晔'),
           ('zhihao', '知浩'), ('zhilun', '知伦'), ('zhichu', '知厨'), ('zhinan', '知楠'),
           ('zhimo', '知墨'), ('zhiming', '知茗'), ('zhixiang', '知祥'), ('zhifei', '知飞'), ('zhishuo', '知硕')],
    '女声与童声': [('zhiyue', '知悦'), ('zhiya', '知雅'), ('zhiying', '知颖（童声）'),
                 ('zhistella', '知莎'), ('zhimao', '知猫'), ('zhigui', '知柜'), ('zhixiao', '知笑'),
                 ('zhiru', '知茹'), ('zhiqi', '知琪'), ('zhiting', '知婷'), ('zhiyuan', '知媛'),
                 ('zhijia', '知佳'), ('zhiwei', '知薇'), ('zhijing', '知婧'),
                 ('zhimiao-emo', '知妙（多情感）'), ('zhiqian', '知倩'), ('zhina', '知娜')],
    '其他语言': [(name, name.capitalize()) for name in
                 ('eva', 'beth', 'indah', 'cindy', 'perla', 'brian', 'betty', 'donna', 'hanna', 'clara', 'waan', 'camila', 'cally')],
}


class SpeechTestError(RuntimeError):
    """A diagnostic safe to show without echoing SDK messages or credentials."""


def validate_cloud_fields(data, current):
    values = {name: data.get(name, getattr(current, name)).strip()
              for name in ('cloud_speech_url', 'cloud_asr_model', 'cloud_tts_model')}
    if 'cloud_tts_model' in data and not values['cloud_tts_model']:
        values['cloud_tts_model'] = data.get('cloud_tts_voice', current.cloud_tts_model).strip()
    url = values['cloud_speech_url']
    parsed = urlsplit(url)
    if len(url) > 500 or parsed.scheme != 'wss' or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment or any(c.isspace() for c in url):
        raise ValueError('云端语音需要不含凭据的 WSS 地址')
    for name in ('cloud_asr_model', 'cloud_tts_model'):
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.:-]{0,99}', values[name]):
            raise ValueError('模型 ID 须为 1～100 个字母、数字或 _ . : -，不是显示名称')
    key = data.get('cloud_speech_api_key', '').strip()
    if len(key) > 4096 or (key and (not key.isascii() or any(c.isspace() for c in key))):
        raise ValueError('云端语音 Key 格式无效')
    values['cloud_speech_api_key'] = key or current.cloud_speech_api_key
    if data.get('clear_cloud_speech_api_key') == 'on':
        values['cloud_speech_api_key'] = ''
    return values


def check_protocol(operation, model, sample_rate):
    if operation == 'tts' and not model.startswith('sambert-'):
        raise SpeechTestError('当前合成接口是 Sambert；CosyVoice／Qwen TTS 需要另一套接口及音色参数，不能只替换模型 ID')
    if operation == 'asr':
        if model.startswith(('sambert-', 'cosyvoice-')) or 'livetranslate' in model:
            raise SpeechTestError('该模型用于合成或实时翻译，不是语音识别模型')
        if '8k' in model and sample_rate != 8000:
            raise SpeechTestError('该模型要求 8k 音频，当前设备和测试录音为 16k；请选择非 8k 实时识别模型')
        if ('filetrans' in model or model in {'paraformer-v1', 'paraformer-v2', 'paraformer-mtl-v1'}
                or ('qwen' in model and not model.endswith('-streaming'))):
            raise SpeechTestError('该模型不是当前实时识别协议；文件转写、短音频和翻译模型不能直接替代实时 ASR')


def upstream_error(operation, response):
    code = getattr(response, 'code', None)
    reasons = {'InvalidApiKey': 'Key 无效或地域不匹配', 'InvalidAPIKey': 'Key 无效或地域不匹配',
               'ModelNotFound': '模型不存在或当前业务空间未开放',
               'InvalidParameter': '模型或参数不匹配', 'Forbidden': '无模型访问权限',
               'AccessDenied': '无模型访问权限', 'Throttling': '调用限流',
               'QuotaExhausted': '额度不足', 'BadRequest': '接口或参数不匹配'}
    status = getattr(response, 'status_code', None)
    status_text = str(status) if isinstance(status, int) else '未知'
    reason = reasons.get(code, '请检查模型权限、接口类型、地域或额度') if isinstance(code, str) else '请检查模型权限、接口类型、地域或额度'
    return SpeechTestError(f'云端{operation}失败（状态 {status_text}）：{reason}')


def cloud_asr(settings, pcm, sample_rate):
    check_protocol('asr', settings.cloud_asr_model, sample_rate)
    from dashscope.audio.asr import Recognition
    # The SDK requires a file; this private temporary copy is removed on exit.
    with tempfile.TemporaryDirectory(prefix='voice-asr-') as directory:
        path = Path(directory) / 'input.pcm'
        path.write_bytes(pcm)
        recognizer = Recognition(model=settings.cloud_asr_model, format='pcm',
                                 sample_rate=sample_rate, callback=None,
                                 api_key=settings.cloud_speech_api_key,
                                 base_address=settings.cloud_speech_url, request_timeout=45)
        result = recognizer.call(str(path))
    if result.status_code != 200:
        raise upstream_error('识别', result)
    return ''.join(item.get('text', '') for item in (result.get_sentence() or []) if isinstance(item, dict)).strip()


def cloud_tts(settings, text, sample_rate, speed):
    check_protocol('tts', settings.cloud_tts_model, sample_rate)
    from dashscope.audio.tts import SpeechSynthesizer
    result = SpeechSynthesizer.call(model=settings.cloud_tts_model, text=text,
                                   format='pcm', sample_rate=sample_rate, rate=speed,
                                   api_key=settings.cloud_speech_api_key,
                                   base_address=settings.cloud_speech_url, request_timeout=45)
    response = result.get_response()
    if response is not None and response.status_code != 200:
        raise upstream_error('合成', response)
    return result.get_audio_data()


def _price(settings, operation, model):
    manual = getattr(settings, 'cloud_' + operation + '_unit_price')
    if manual:
        return manual, '手动单价'
    host = (urlsplit(settings.cloud_speech_url).hostname or '').lower()
    if host != 'dashscope.aliyuncs.com' and not host.endswith('.cn-beijing.maas.aliyuncs.com'):
        return None, '未确认地域单价'
    if operation == 'asr' and model in {'paraformer-realtime-v2', 'paraformer-realtime-v1'}:
        return '.00024', QWEN_SOURCE
    if operation == 'asr' and model == 'qwen-audio-3.0-asr-flash-streaming':
        return '.00033', QWEN_SOURCE
    if operation == 'tts' and model.startswith('sambert-'):
        return '.0001', QWEN_SOURCE
    return None, '未确认模型单价'


def record_cloud_usage(settings, store, operation, device_id, model, success, quantity):
    price, source = _price(settings, operation, model)
    amount = format(Decimal(str(quantity)) * Decimal(price), 'f') if price and success and settings.billing_enabled else None
    store.record_speech_usage(device_id, operation, model, success, str(quantity),
                              '秒' if operation == 'asr' else '字符', amount,
                              {'unit_price': price, 'source': source, 'checked_at': CHECKED_AT,
                               'note': '按发送时长或字数估算原价；未扣免费额度，不是厂商账单'},
                              key_id=store.api_key_id('cloud_speech', settings.cloud_speech_api_key, settings.cloud_speech_url))


def connectivity_test(settings, store, operation, local_tts):
    model = getattr(settings, 'cloud_' + operation + '_model')
    rate = getattr(local_tts, 'sample_rate', 16000)
    start, attempted, success, quantity = time.monotonic(), False, False, 0
    result = {'operation': operation, 'model': model, 'phrase': TEST_PHRASE, 'success': False}
    try:
        check_protocol(operation, model, rate)
        if not settings.cloud_speech_api_key:
            raise SpeechTestError('云端语音 API Key 未配置')
        if operation == 'asr':
            if settings.tts_provider == 'mock':
                raise SpeechTestError('本地 TTS 为 Mock 提示音，不能生成识别测试语音；请先配置真实本地 TTS')
            pcm = local_tts.synthesize(TEST_PHRASE)
            if not isinstance(pcm, bytes) or not pcm or len(pcm) % 2 or len(pcm) > rate * 2 * 10:
                raise SpeechTestError('本地测试音频无效或超过 10 秒；未调用云端')
            quantity = Decimal(len(pcm)) / Decimal(rate * 2)
            attempted = True
            text = cloud_asr(settings, pcm, rate)
            if not text or not any(c.isalnum() for c in text):
                raise SpeechTestError('连接成功但未识别到有效文字')
            result['detail'] = '识别结果：' + text[:500].replace(settings.cloud_speech_api_key, '[已隐藏]')
        else:
            quantity, attempted = len(TEST_PHRASE), True
            pcm = cloud_tts(settings, TEST_PHRASE, rate, 1.0)
            if not isinstance(pcm, bytes) or not pcm or len(pcm) % 2:
                raise SpeechTestError('连接成功但未返回有效 PCM 音频')
            if len(pcm) > rate * 2 * 30:
                raise SpeechTestError('短句返回音频异常长，未保留试听；请检查模型和接口')
            result['detail'] = f'返回 {len(pcm)} 字节 PCM，{len(pcm) / (rate * 2):.2f} 秒，{rate} Hz'
            result['_audio'] = pcm
            result['_sample_rate'] = rate
        result['success'] = success = True
    except SpeechTestError as exc:
        result['detail'] = str(exc)
    except Exception as exc:
        # SDK exceptions can contain URLs, request bodies or keys. Never echo them.
        name = type(exc).__name__.lower()
        result['detail'] = ('连接超时，请检查语音 WSS 地址与网络' if 'timeout' in name else
                            '网络或 SDK 调用失败，请检查 WSS 地址、模型权限和参数；敏感错误内容未回显')
    finally:
        if attempted:
            record_cloud_usage(settings, store, operation, 'admin-connectivity-test', model, success, quantity)
    result['elapsed'] = round(time.monotonic() - start, 2)
    return result


def route(settings, store, operation, device_id, primary, cloud, quantity):
    mode = getattr(settings, operation + '_route')
    if mode not in ROUTES:
        raise RuntimeError('语音主备方案无效')
    if mode == 'local_only':
        return primary()
    order = {'local_only': ('local',), 'local_first': ('local', 'cloud'),
             'cloud_first': ('cloud', 'local'), 'cloud_only': ('cloud',)}[mode]
    for index, source in enumerate(order):
        model = getattr(settings, 'cloud_' + operation + '_model')
        attempted, success = False, False
        try:
            if source == 'cloud':
                if not settings.cloud_speech_api_key:
                    raise RuntimeError('云端语音 API Key 未配置')
                attempted = True
            value = cloud() if source == 'cloud' else primary()
            valid = (isinstance(value, str) and any(c.isalnum() for c in value)) if operation == 'asr' else (isinstance(value, bytes) and len(value) > 0 and len(value) % 2 == 0)
            if not valid:
                raise RuntimeError('未识别到有效语音' if operation == 'asr' else '合成音频无效')
            success = True
            return value
        except Exception:
            if index == len(order) - 1:
                raise RuntimeError(('语音识别' if operation == 'asr' else '语音合成') + '主备均失败（请检查配置、网络或模型权限）') from None
        finally:
            if attempted:
                record_cloud_usage(settings, store, operation, device_id, model, success, quantity)


def recognize(settings, store, primary, pcm, sample_rate, device_id):
    return route(settings, store, 'asr', device_id,
                 lambda: primary.recognize(pcm, sample_rate),
                 lambda: cloud_asr(settings, pcm, sample_rate),
                 Decimal(len(pcm)) / Decimal(sample_rate * 2))


def synthesize(settings, store, primary, text, device_id, **options):
    sample_rate = getattr(primary, 'sample_rate', 16000)
    return route(settings, store, 'tts', device_id,
                 lambda: primary.synthesize(text, **options),
                 lambda: cloud_tts(settings, text, sample_rate, options.get('speed', 1.0)), len(text))
