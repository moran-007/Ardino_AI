"""Server-rendered admin views. Secrets are never passed to HTML inputs."""
from __future__ import annotations

import html
import json
from datetime import datetime, timedelta, timezone

from .model_catalog import PROVIDERS
from .billing import CHECKED_AT, selected_rule


def esc(value) -> str:
    return html.escape(str(value if value is not None else ""), quote=True)


def csrf_input(csrf: str) -> str:
    return f'<input type="hidden" name="csrf" value="{esc(csrf)}">'


def number(value) -> str:
    return f"{int(value or 0):,}"


def local_time(value) -> str:
    if not value:
        return "尚无调用"
    return datetime.fromisoformat(value).astimezone(timezone(timedelta(hours=8))).strftime("%m-%d %H:%M:%S")


def field(name, label, value="", kind="text", extra="") -> str:
    return f'<label for="{esc(name)}">{esc(label)}</label><input id="{esc(name)}" name="{esc(name)}" type="{kind}" value="{esc(value)}" {extra}>'


def role(name, order) -> str:
    return ("主用", "第一备用", "第二备用")[order.index(name)] if name in order and order.index(name) < 3 else "未启用"


CSS = """
*{box-sizing:border-box} :root{font-family:Inter,"Segoe UI","Microsoft YaHei",sans-serif;color:#172a35;background:#f3f6f8;font-size:14px;line-height:1.6}
body{margin:0}a{color:#007f78;text-decoration:none}a:hover{text-decoration:underline}button,input,select,textarea{font:inherit}button,.button{display:inline-flex;align-items:center;justify-content:center;padding:10px 17px;background:#087d76;color:white;border:1px solid transparent;border-radius:8px;font-weight:600;cursor:pointer;line-height:1.5;text-decoration:none}button:hover,.button:hover{filter:brightness(.94);text-decoration:none}button.secondary,.button.secondary{background:white;color:#334e5c;border-color:#ccd8df}button.danger{background:#fff0ee;color:#ad3328;border-color:#f1d0c9}button:disabled{opacity:.5;cursor:default}a:focus-visible,button:focus-visible,input:focus-visible,select:focus-visible,textarea:focus-visible,summary:focus-visible{outline:3px solid #4cafaa;outline-offset:3px}
.sidebar{position:fixed;inset:0 auto 0 0;width:220px;background:#112e38;color:#d6e4e7;padding:32px 20px;display:flex;flex-direction:column}.brand{font-size:20px;font-weight:750;letter-spacing:1px;color:white}.brand small{display:block;letter-spacing:3px;font-size:10px;color:#8dacb5;font-weight:500;margin-top:4px}.brand-mark{display:inline-block;width:24px;height:24px;background:#2eb9a9;border-radius:8px;margin-right:9px;vertical-align:-4px}.nav{display:grid;gap:7px;margin-top:40px}.nav a{color:#a9c0c8;border-radius:8px;padding:12px 16px}.nav a[aria-current=page]{background:#24505a;color:white;font-weight:650}.sidebar-foot{margin-top:auto;color:#9ab2bb;font-size:12px}.sidebar-foot button{margin-top:14px;width:100%;background:transparent;color:#d6e4e7;border-color:#38515a}
main{margin-left:220px;padding:36px 40px 50px;max-width:1740px}.standalone{margin:70px auto;max-width:560px;padding:24px}.eyebrow{font-size:11px;letter-spacing:2px;color:#70838d;margin-bottom:5px}h1{font-size:28px;line-height:1.35;margin:0 0 9px;font-weight:700;letter-spacing:-.5px}h2{font-size:19px;margin:0}h3{font-size:16px;margin:0}.muted{color:#6c808a;font-size:12px}p{margin:9px 0 16px}.page-head,.section-head,.card-head,.toolbar{display:flex;align-items:center;justify-content:space-between;gap:16px}.page-head{margin-bottom:24px}.section-head{margin:28px 0 14px}.section-head p{margin:3px 0 0}.toolbar{justify-content:flex-start;flex-wrap:wrap;margin-bottom:20px}.periods{display:flex;padding:4px;background:#e7eef1;border-radius:9px;gap:3px;flex-wrap:wrap}.periods a{color:#5d747e;padding:6px 14px;border-radius:6px;font-size:12px}.periods a[aria-current=true]{background:white;color:#193d46;box-shadow:0 1px 3px #1232;font-weight:650}.toolbar .muted{margin-left:auto}
.metrics{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:16px;margin:18px 0}.metric,.card,fieldset,.panel{background:white;border:1px solid #dce5e9;border-radius:12px;padding:22px}.metric strong{display:block;font-size:30px;line-height:1.2;font-weight:650;letter-spacing:-1px;margin:14px 0 10px;font-variant-numeric:tabular-nums}.metric.featured{background:#143e46;color:white;border-color:#143e46}.metric.featured .muted{color:#b4d0d6}.metric.featured strong{color:#8eecdc}.grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:18px}.device-grid{grid-template-columns:repeat(2,minmax(0,1fr))}.card{min-width:0}.card-head{align-items:flex-start;margin-bottom:16px}.provider-mark{width:36px;height:36px;display:flex;align-items:center;justify-content:center;border-radius:10px;background:#e5f3f0;color:#087d76;font-size:17px;font-weight:700;flex:none}.card-title{display:flex;gap:11px;align-items:center;min-width:0}.card-title div{min-width:0}.card-title h3{overflow-wrap:anywhere}.badge{display:inline-flex;align-items:center;gap:5px;border-radius:5px;padding:3px 8px;background:#eff3f5;color:#637985;font-size:11px;white-space:nowrap}.badge.ok{background:#e9f6ef;color:#257551}.badge.active{background:#e4f1ef;color:#087d76}.badge.bad{background:#fff0ec;color:#b24434}.dot{width:6px;height:6px;border-radius:100%;background:currentColor;display:inline-block}.key-id{font-size:11px;letter-spacing:.4px;color:#83959d}.usage-total{font-size:30px;font-weight:650;letter-spacing:-.6px;margin:14px 0 12px;font-variant-numeric:tabular-nums}.usage-total small{font-size:11px;font-weight:400;letter-spacing:0;color:#83959d;margin-left:7px}.mini-stats{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px;margin:12px 0}.mini-stats span{display:block;font-size:11px;color:#82929a}.mini-stats strong{font-size:14px;font-weight:650;font-variant-numeric:tabular-nums}.card-foot{border-top:1px solid #eaf0f2;padding-top:13px;margin-top:16px;font-size:11px;display:flex;justify-content:space-between;gap:6px;flex-wrap:wrap;color:#71858e}.bar{height:5px;background:#edf2f4;border-radius:4px;overflow:hidden;margin:15px 0}.bar span{display:block;height:100%;background:#2da69a}.notice{border:1px solid #cfe5e2;border-left:3px solid #46a79b;padding:12px 16px;border-radius:8px;background:#f1f9f7;color:#527770;font-size:12px;margin:18px 0}.notice.bad{border-color:#eab7ad;background:#fff4f0;color:#ab3d2b}.empty{text-align:center;padding:36px;color:#728892}.empty strong{display:block;color:#425d67;margin-bottom:5px}.table-wrap{overflow-x:auto;background:white;border:1px solid #dce5e9;border-radius:12px}table{border-collapse:collapse;width:100%;font-size:12px}th{background:#f8fafb;color:#6f828d;font-weight:500;white-space:nowrap}th,td{text-align:left;padding:13px 18px;border-bottom:1px solid #edf1f3}td{font-variant-numeric:tabular-nums}tr:last-child td{border:0}td.numeric,th.numeric{text-align:right}.ok{color:#23855d}.bad{color:#b44333}code{overflow-wrap:anywhere;font-size:12px}.nowrap{white-space:nowrap}
fieldset{margin:18px 0;min-width:0}legend{font-weight:650;padding:0 8px}label{display:block;font-size:12px;color:#4a646f;margin:14px 0 6px}input[type=text],input[type=password],input[type=number],input[type=url],input[type=file],select,textarea{width:100%;min-width:0;background:#fff;border:1px solid #cbd8df;border-radius:7px;padding:10px 12px;color:#233f4b}textarea{resize:vertical;min-height:85px}input[type=checkbox]{accent-color:#087d76}input::placeholder{color:#93a3aa}.row{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px}.row.three{grid-template-columns:repeat(3,minmax(0,1fr))}details{border-top:1px solid #e6edf0;margin-top:16px;padding-top:15px}summary{cursor:pointer;color:#466775;font-size:12px;font-weight:600}details[open]>summary{margin-bottom:14px}.actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:20px}.config-card{margin-bottom:18px}.config-card .card-head{margin-bottom:8px}.model-source{padding:8px 0}.settings-form{max-width:1080px}.config-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:0 22px}.submit-bar{position:sticky;bottom:12px;z-index:2;padding:14px 18px;background:#fffffff5;box-shadow:0 4px 22px #1b343b16;border:1px solid #dce5e9;border-radius:10px;display:flex;align-items:center;gap:18px;margin:22px 0}.submit-bar p{margin:0}.create-device{background:white;padding:19px 22px;border:1px solid #dce5e9;border-radius:12px;margin:22px 0}.create-device>summary{font-size:14px}.safety-note{margin-top:24px;font-size:11px;color:#90a0a7}
@media(min-width:1600px){main{padding:42px 56px}.grid{gap:22px}}@media(max-width:1180px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}.metrics{grid-template-columns:repeat(2,minmax(0,1fr))}main{padding:28px 24px}.device-grid{grid-template-columns:1fr}}
@media(max-width:760px){.sidebar{position:static;width:auto;padding:18px 20px}.brand{font-size:17px}.brand small,.sidebar-foot{display:none}.nav{display:flex;gap:4px;flex-wrap:wrap;margin-top:18px}.nav a{padding:7px 11px;font-size:12px}main{margin:0;padding:22px 16px}h1{font-size:24px}.page-head{align-items:flex-start;flex-wrap:wrap}.grid,.row,.row.three,.config-grid{grid-template-columns:1fr}.metrics{gap:10px}.metric{padding:16px}.metric strong{font-size:25px}.card{padding:19px}.toolbar .muted{margin-left:0}.submit-bar{position:static;flex-wrap:wrap}.section-head{align-items:flex-start}.standalone{margin:30px auto}.nav-lock{display:inline!important}.config-grid{gap:0}}
.nav-lock{display:none}.nav-lock button{font-size:11px;padding:6px 10px;background:transparent;border-color:#38515a;color:#c2d4da}
.firmware-table td:first-child{min-width:200px}.firmware-table td:nth-child(5){min-width:180px;white-space:pre-wrap;overflow-wrap:anywhere}
@media(max-width:760px){.firmware-table,.firmware-table tbody{display:block}.firmware-table thead{display:none}.firmware-table tr{display:grid;grid-template-columns:1fr 1fr;border-bottom:1px solid #dce5e9;padding:12px}.firmware-table tr:last-child{border-bottom:0}.firmware-table td{display:block;border:0;padding:8px;min-width:0!important;white-space:normal}.firmware-table td:first-child,.firmware-table td:nth-child(5),.firmware-table td:last-child{grid-column:1/-1}.firmware-table td:nth-child(3)::before{content:"发布时间";display:block;color:#6f828d}.firmware-table td:nth-child(4)::before{content:"大小";display:block;color:#6f828d}.firmware-table td:last-child a{display:inline-block;padding:8px 0}}
"""


def page_shell(content: str, title="ESP32 语音服务", *, active="", csrf="", refresh=False) -> str:
    navigation = ""
    if active:
        links = (("overview", "/admin/", "用量总览"), ("devices", "/admin/devices", "设备 KEY"),
                 ("api-keys", "/admin/api-keys", "API Key 配置"), ("firmware", "/admin/firmware", "固件版本"),
                 ("billing", "/admin/billing", "计费与费用"), ("alarm-music", "/admin/alarm-music", "闹钟音乐"),
                 ("music", "/admin/music", "音乐库"),
                 ("settings", "/admin/settings", "服务设置"))
        nav = "".join(f'<a href="{url}" aria-current="{"page" if key == active else "false"}">{label}</a>' for key, url, label in links)
        lock = f'<form method="post" action="/admin/lock">{csrf_input(csrf)}<button type="submit">锁定页面</button></form>'
        navigation = f'<aside class="sidebar"><div class="brand"><span class="brand-mark"></span>VOICE CLOUD<small>ESP32 · 服务控制台</small></div><nav class="nav" aria-label="管理导航">{nav}<div class="nav-lock">{lock}</div></nav><div class="sidebar-foot">让每一次调用清晰可见<br>设备 · 模型 · Token{lock}</div></aside>'
    return f'<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>{esc(title)} · VOICE CLOUD</title>{"<meta http-equiv=refresh content=30>" if refresh else ""}<style>{CSS}</style></head><body>{navigation}<main class="{"" if active else "standalone"}">{content}</main></body></html>'


def heading(title, subtitle, action="") -> str:
    return f'<header class="page-head"><div><div class="eyebrow">WORKSPACE / ESP32 CLOUD</div><h1>{title}</h1><p class="muted">{subtitle}</p></div>{action}</header>'


def firmware_content(releases, current, csrf, channel='single'):
    content = heading("固件版本", "已发布版本与升级包")
    content += f'<form method="post" action="/admin/firmware/upload" enctype="multipart/form-data">{csrf_input(csrf)}{field("package", "签名 OTA 包", kind="file", extra="accept=.zip,application/zip required")}{field("minimum_version", "最低来源版本（可留空）", current.minimum_version if current else "", extra="placeholder=1.4.1")}<label for="release-notes">更新说明</label><textarea id="release-notes" name="notes" maxlength="2000"></textarea><div class="actions"><button type="submit">校验并发布固件</button></div></form>'
    content += '<div class="section-head"><h2>版本历史</h2></div>'
    if not releases:
        return content + '<p class="empty">尚无发布版本</p>'
    content += '<div class="table-wrap"><table class="firmware-table"><thead><tr><th>版本</th><th>状态</th><th>发布时间</th><th>大小</th><th>更新说明</th><th>升级包</th></tr></thead><tbody>'
    for release in releases:
        state = "已撤回" if release.withdrawn else "当前发布" if current and release.version == current.version else "历史版本"
        download = "不可安装" if release.withdrawn else f'<a href="/admin/firmware/package/{esc(release.version)}?channel={channel}">下载 ZIP</a>'
        try:
            published = datetime.fromisoformat(release.published_at).astimezone(timezone(timedelta(hours=8))).strftime("%Y-%m-%d %H:%M")
        except (ValueError, TypeError):
            published = "未知"
        minimum = f'<br>最低来源版本：{esc(release.minimum_version)}' if release.minimum_version else ''
        content += f'<tr><td><strong>{esc(release.version)}</strong><br><code>{esc(release.hardware)}</code></td><td>{state}</td><td class="nowrap">{esc(published)}</td><td class="nowrap">{number(release.size)} B</td><td>{esc(release.notes) or "暂无说明"}{minimum}<details><summary>SHA-256</summary><code>{esc(release.sha256)}</code></details></td><td class="nowrap">{download}</td></tr>'
    return content + '</tbody></table></div>'


def period_toolbar(period, path, *, refresh=False):
    options = (("all", "累计"), ("24h", "近 24 小时"), ("7d", "近 7 天"), ("30d", "近 30 天"))
    tabs = "".join(f'<a href="{path}?period={key}" aria-current="{str(key == period).lower()}">{label}</a>' for key, label in options)
    return f'<div class="toolbar"><div class="periods" aria-label="统计周期">{tabs}</div><span class="muted">北京时间 · {"每 30 秒刷新" if refresh else "刷新页面更新统计"}</span></div>'


def metrics(stats):
    return '<div class="metrics">' + "".join(
        f'<div class="metric {"featured" if i == 0 else ""}"><span class="muted">{label}</span><strong>{number(value)}</strong><span class="muted">{note}</span></div>'
        for i, (label, value, note) in enumerate((
            ("Token 用量", stats.get("total_tokens"), "厂商已返回的 Token 合计"),
            ("输入 / 输出 Token", stats.get("input_tokens"), f'输出 {number(stats.get("output_tokens"))} Token'),
            ("上游调用次数", stats.get("attempts"), f'成功 {number(stats.get("successes"))} · 失败 {number(stats.get("failures"))}'),
            ("未返回用量", stats.get("unreported_calls"), f'已返回用量 {number(stats.get("reported_calls"))} 次'),
        ))
    ) + '</div>'


def usage_numbers(stats, total=0):
    ratio = min(100, round(100 * (stats.get("total_tokens") or 0) / total, 1)) if total else 0
    return f'''<div class="usage-total">{number(stats.get('total_tokens'))}<small>TOKENS</small></div>
<div class="mini-stats"><div><span>输入</span><strong>{number(stats.get('input_tokens'))}</strong></div><div><span>输出</span><strong>{number(stats.get('output_tokens'))}</strong></div><div><span>调用次数</span><strong>{number(stats.get('attempts'))}</strong></div></div>
<div class="bar" title="占已知总量 {ratio}%"><span style="width:{ratio}%"></span></div>
<div class="mini-stats"><div><span>成功 / 失败</span><strong>{number(stats.get('successes'))} / {number(stats.get('failures'))}</strong></div><div><span>未返回用量</span><strong>{number(stats.get('unreported_calls'))}</strong></div><div><span>用量占比</span><strong>{ratio}%</strong></div></div>'''


def dashboard_content(current, store, summary, recent, period, ready):
    state = '<span class="badge ok"><span class="dot"></span>服务就绪</span>' if ready else '<a class="badge bad" href="/admin/settings">服务未就绪 · 查看设置</a>'
    content = heading("用量总览", "集中查看每个 KEY 的调用与 Token 消耗。", state)
    content += period_toolbar(period, "/admin/", refresh=True) + metrics(summary["totals"])
    content += '<p class="notice">仅统计本服务启用监测后发起、已结束的上游调用。Token 以厂商返回为准，缺失用量单独标记；主备重试分别记次。不包含厂商账户在其他应用中的消耗。</p>'
    content += '<div class="section-head"><div><h2>API Key 用量</h2><p class="muted">每个凭证独立统计，换 Key 后保留历史记录</p></div><a href="/admin/api-keys">配置 API Key →</a></div><div class="grid">'
    entries = {(item["provider"], item["key_id"]): item for item in summary["by_key"]}
    cards = []
    for name, spec in PROVIDERS.items():
        key_id = store.api_key_id(name, getattr(current, f"{name}_api_key"), getattr(current, f"{name}_base_url"))
        stats = entries.pop((name, key_id), {})
        cards.append((name, key_id, stats, role(name, current.llm_provider_order), getattr(current, f"{name}_model")))
    for (name, key_id), stats in entries.items():
        cards.append((name, key_id, stats, "历史 KEY" if key_id else "测试 / 未配置", "历史调用"))
    for name, key_id, stats, label, model in cards:
        spec = PROVIDERS.get(name, {"label": name})
        key_label = f'KEY · {key_id[:12]}' if key_id else "未配置凭证" if name in PROVIDERS else "测试 Provider"
        content += f'''<article class="card"><div class="card-head"><div class="card-title"><span class="provider-mark">{esc(name[:1].upper())}</span><div><h3>{esc(spec['label'])}</h3><span class="muted">{esc(model)}</span></div></div><span class="badge {"active" if label == "主用" else ""}">{label}</span></div><div class="key-id">{esc(key_label)}</div>{usage_numbers(stats, summary['totals']['total_tokens'])}<div class="card-foot"><span>{"已配置" if key_id else "未配置"}</span><span>最近调用 {local_time(stats.get('last_used'))}</span></div></article>'''
    content += '</div><div class="section-head"><div><h2>设备 KEY 使用情况</h2><p class="muted">按设备 ID 汇总语音、文字对话与翻译消耗</p></div><a href="/admin/devices">管理设备 KEY →</a></div>'
    rows = ""
    device_stats = {item["device_id"]: item for item in summary["by_device"]}
    for device in store.list_devices():
        item = device_stats.get(device["device_id"], {})
        rows += f'<tr><td><strong>{esc(device["name"])}</strong><br><code>{esc(device["device_id"])}</code></td><td><span class="badge {"ok" if device["enabled"] else ""}">{"启用" if device["enabled"] else "停用"}</span></td><td class="numeric">{number(item.get("total_tokens"))}</td><td class="numeric">{number(item.get("attempts"))}</td><td class="numeric">{number(item.get("failures"))}</td><td class="nowrap">{local_time(item.get("last_used"))}</td></tr>'
    content += f'<div class="table-wrap"><table><thead><tr><th>设备</th><th>状态</th><th class="numeric">Token</th><th class="numeric">调用</th><th class="numeric">失败</th><th>最近调用</th></tr></thead><tbody>{rows or "<tr><td colspan=6 class=empty>尚未创建设备，可前往设备 KEY 页面添加。</td></tr>"}</tbody></table></div>'
    content += '<div class="section-head"><div><h2>最近调用</h2><p class="muted">当前周期最近 20 次 · 每次主备尝试各一条</p></div></div>'
    rows = ""
    for item in recent:
        tokens = number(item['total_tokens']) if item['total_tokens'] is not None else '<span class="muted">未返回</span>'
        rows += f'<tr><td class="nowrap">{local_time(item["created_at"])}</td><td><code>{esc(item["device_id"] or "未归属设备")}</code></td><td>{esc(PROVIDERS.get(item["provider"], {}).get("label", item["provider"]))}<br><span class="key-id">{esc(item["key_id"][:12])}</span></td><td>{esc(item["model"])}</td><td>{"翻译" if item["operation"] == "translation" else "对话"}</td><td class="{ "ok" if item["success"] else "bad"}">{"成功" if item["success"] else "失败"}</td><td class="numeric">{tokens}</td></tr>'
    content += f'<div class="table-wrap"><table><thead><tr><th>时间</th><th>设备 ID</th><th>API Key</th><th>模型</th><th>类型</th><th>结果</th><th class="numeric">Token</th></tr></thead><tbody>{rows or "<tr><td colspan=7 class=empty>当前周期暂无调用记录，完成一次对话或翻译后会在此显示。</td></tr>"}</tbody></table></div>'
    return content


def device_fields(prefix, device):
    # IDs are unique per card; names stay compatible with the action endpoints.
    inputs = (("memory_turns", "携带最近轮数", 4, 'min="1" max="10"'), ("tts_speaker_id", "中文音色（0～4）", 0, 'min="0" max="4"'), ("tts_english_speaker_id", "英文音色（0～903）", 0, 'min="0" max="903"'), ("tts_speed", "语速（0.5～2.0）", 1, 'min="0.5" max="2" step="0.05"'))
    content = f'<label for="{prefix}-persona">设备专属角色设定</label><textarea id="{prefix}-persona" name="persona" maxlength="800" placeholder="例如：称呼用户为小明，用适合儿童的方式回答。">{esc(device.get("persona", ""))}</textarea><div class="row">'
    styles = ''.join(f'<option value="{value}"{" selected" if device.get("reply_style", "default") == value else ""}>{label}</option>' for value, label in [('default', '跟随服务器'), ('brief', '简短'), ('normal', '适中'), ('detailed', '详细')])
    content = content.removesuffix('<div class="row">') + f'<label for="{prefix}-style">回答风格</label><select id="{prefix}-style" name="reply_style">{styles}</select><label><input type="checkbox" name="web_search_enabled"{" checked" if device.get("web_search_enabled") else ""}> 允许本设备联网搜索（默认关闭）</label><div class="row">'
    for name, label, default, limits in inputs:
        content += f'<div><label for="{prefix}-{name}">{label}</label><input id="{prefix}-{name}" name="{name}" type="number" value="{esc(device.get(name, default))}" {limits}></div>'
    return content + '</div>'


def devices_content(devices, summary, period, csrf):
    content = heading("设备 KEY", "每台设备一个独立卡片，查看用量并管理访问、记忆和语音设置。")
    content += period_toolbar(period, "/admin/devices")
    content += f'<details class="create-device"><summary>＋ 创建设备凭证</summary><form method="post" action="/admin/devices">{csrf_input(csrf)}<div class="row"><div>{field("device_id", "设备 ID（留空自动生成）", extra="maxlength=64 placeholder=例如：esp32-01")}</div><div>{field("device_name", "设备名称", extra="maxlength=80 placeholder=例如：桌面语音助手")}</div></div>{device_fields("new-device", {})}<label><input type="checkbox" name="memory_enabled"> 创建后启用独立对话记忆</label><div class="actions"><button type="submit">创建设备凭证</button></div><p class="muted">Token 创建后仅显示一次，请妥善保存。</p></form></details>'
    stats_map = {item["device_id"]: item for item in summary["by_device"]}
    content += '<div class="grid device-grid">'
    for index, device in enumerate(devices):
        stats = stats_map.get(device["device_id"], {})
        prefix = f"device-{index}"
        content += f'''<article class="card"><div class="card-head"><div class="card-title"><span class="provider-mark">{index + 1:02d}</span><div><h3>{esc(device['name'])}</h3><code class="muted">{esc(device['device_id'])}</code></div></div><span class="badge {"ok" if device['enabled'] else ""}"><span class="dot"></span>{"已启用" if device['enabled'] else "已停用"}</span></div>{usage_numbers(stats, summary['totals']['total_tokens'])}<div class="card-foot"><span>记忆 {"开启" if device['memory_enabled'] else "关闭"} · {device['memory_count']} 轮</span><span>最近调用 {local_time(stats.get('last_used'))}</span></div><details><summary>个性化设置与访问管理</summary><form method="post" action="/admin/device-action">{csrf_input(csrf)}<input type="hidden" name="device_id" value="{esc(device['device_id'])}">{device_fields(prefix, device)}<div class="actions"><button name="action" value="save_profile">保存个性化设置</button><button class="secondary" name="action" value="{'disable' if device['enabled'] else 'enable'}">{'停用设备' if device['enabled'] else '启用设备'}</button><button class="secondary" name="action" value="{'memory_disable' if device['memory_enabled'] else 'memory_enable'}">{'关闭记忆' if device['memory_enabled'] else '开启记忆'}</button></div><label><input type="checkbox" name="confirm_clear"> 确认清除该设备的全部对话记忆</label><button class="danger" name="action" value="clear_memory">清除记忆</button></form></details></article>'''
    content += '</div>'
    if not devices:
        content += '<div class="panel empty"><strong>还没有设备 KEY</strong>点击上方“创建设备凭证”，添加第一台设备。</div>'
    content += '<p class="safety-note">Token 用量以厂商返回为准。卡片按设备 ID 统计；重置同一设备的凭证不会清空该设备历史用量。凭证明文不回显。</p>'
    return content


def api_keys_content(current, config, csrf, catalogs, notice="", error=""):
    content = heading("语言模型配置", "外部模型 API 与服务器本地语言模型独立配置；本地 ASR／TTS 在服务设置中管理。")
    if notice:
        content += f'<p class="notice" role="status">{esc(notice)}</p>'
    if error:
        content += f'<p class="notice bad" role="alert">{esc(error)}</p>'
    content += f'<form id="api-config" method="post" action="/admin/api-keys">{csrf_input(csrf)}<div class="panel"><h2>调用顺序</h2><p class="muted">主用在输出内容前失败时，按顺序使用备用。选择“不启用”可关闭对应位置。</p><div class="row three">'
    for index, (name, label) in enumerate((("primary_provider", "主用"), ("secondary_provider", "第一备用"), ("tertiary_provider", "第二备用"))):
        selected = current.llm_provider_order[index] if len(current.llm_provider_order) > index else ""
        options = '<option value="">不启用</option>' + "".join(f'<option value="{key}"{" selected" if key == selected else ""}>{esc(spec["label"])}</option>' for key, spec in PROVIDERS.items())
        content += f'<div><label for="{name}">{label}</label><select id="{name}" name="{name}">{options}</select></div>'
    content += '</div></div><div class="section-head"><div><h2>厂商与模型</h2><p class="muted">API Key 留空保留原值；获取模型列表前先保存 Key 和接口地址。</p></div></div>'
    for name, spec in PROVIDERS.items():
        configured = bool(getattr(current, f"{name}_api_key"))
        catalog = catalogs.get(name, {})
        candidates = catalog["models"] if catalog else spec["suggested_models"]
        current_model = getattr(current, f"{name}_model")
        models = list(dict.fromkeys([current_model, *candidates]))
        options = ''.join(f'<option value="{esc(model)}"></option>' for model in models if model)
        source = f'接口返回 {len(candidates)} 个模型 · {local_time(catalog["fetched_at"])}' if catalog else '文档候选 · 尚未在线获取，不代表当前 Key 已开通'
        protocol = spec['protocol_label']
        custom_control = ''
        if name in {'custom', 'local_llm'}:
            choices = ''.join(f'<option value="{key}"{" selected" if getattr(current, name + "_protocol") == key else ""}>{label}</option>' for key, label in [('openai', 'OpenAI 兼容 Chat Completions'), ('ollama', 'Ollama 原生 HTTP／NDJSON')])
            custom_control = f'<label for="{name}_protocol">接口协议</label><select id="{name}_protocol" name="{name}_protocol">{choices}</select><p class="muted">API Key 可选；地址含端口与路径。不会自动安装或下载模型；任意私有 HTTP 协议需要单独适配。</p>'
            protocol = 'OpenAI 兼容 SSE' if getattr(current, name + '_protocol') == 'openai' else 'Ollama 原生 HTTP／NDJSON'
        if name == 'qwen' and '/api/v1/services/' not in current.qwen_base_url:
            protocol = 'OpenAI 兼容接口 · 建议使用下方官方原生地址'
        elif name == 'qwen':
            protocol = 'DashScope 原生 SSE · 已知模型自动选择文本 / 多模态接口'
        content += f'''<section class="card config-card"><div class="card-head"><div class="card-title"><span class="provider-mark">{name[:1].upper()}</span><div><h3>{esc(spec['label'])}</h3><span class="muted">{esc(protocol)}</span></div></div><span class="badge {"ok" if configured else ""}">{"已配置 Key" if configured else "可免 Key" if name in {"custom", "local_llm"} else "未配置"} · {role(name, current.llm_provider_order)}</span></div>{custom_control}<div class="config-grid"><div>{field(name + '_api_key', 'API Key', kind='password', extra='autocomplete="new-password" placeholder="留空保持不变"')}<label><input type="checkbox" name="clear_{name}_api_key"> 清除该 Key（包括环境变量配置）</label></div><div>{field(name + '_model', '模型 ID（可选择或手动填写）', current_model, extra=f'list="{name}-model-options" maxlength="160"')}<datalist id="{name}-model-options">{options}</datalist><div class="model-source muted">{esc(source)}</div></div><div>{field(name + '_base_url', '调用接口地址', getattr(current, name + '_base_url'), kind='url', extra='required maxlength="500"')}<p class="muted">{"示例地址" if name in {"custom", "local_llm"} else "官方推荐"}：<code>{esc(spec['base_url'])}</code></p></div><div>{field(name + '_models_url', '模型列表地址（可选，需与调用接口同源）', config.get(name + '_models_url', ''), kind='url', extra='maxlength="500" placeholder="留空按协议推导"')}<p class="muted"><a href="{esc(spec['docs_url'])}" target="_blank" rel="noreferrer">查看接口文档 ↗</a></p></div></div><button class="secondary" type="submit" form="fetch-{name}">获取 {esc(spec['label'])} 模型列表</button></section>'''
    content += '<p class="notice">千问推荐 DashScope 原生 SSE，已适配已知文本与多模态模型的文字回复；未识别的新模型请按<a href="https://help.aliyun.com/zh/model-studio/text-generation" target="_blank" rel="noreferrer">官方文档</a>填写对应接口地址。DeepSeek 和智谱直接调用官方 HTTP 接口。目录可用性以地域、业务空间和 Key 权限为准；智谱未提供通用目录时可从文档候选选择或手填。</p><div class="submit-bar"><button type="submit">保存 API Key 与主备配置</button><p class="muted">保存后立即应用。查询模型只读取已保存配置。</p></div></form>'
    for name in PROVIDERS:
        content += f'<form id="fetch-{name}" method="post" action="/admin/api-keys/{name}/models">{csrf_input(csrf)}</form>'
    return content


def settings_content(current, csrf, firmware_release, ready, status_error):
    status = '服务运行就绪' if ready else f'服务未就绪：{esc(status_error)}'
    content = heading("服务设置", "管理接口开关、本地语音模型、管理员密码与 OTA 固件。")
    content += f'<p class="notice {"" if ready else "bad"}">{status}</p><form class="settings-form" method="post" action="/admin/config">{csrf_input(csrf)}<input type="hidden" name="section" value="service"><fieldset><legend>接口开关</legend>'
    for name, label in (("voice_api_enabled", "启用语音对话 API"), ("translation_api_enabled", "启用文字翻译 API")):
        content += f'<label><input type="checkbox" name="{name}"{" checked" if getattr(current, name) else ""}> {label}</label>'
    styles = ''.join(f'<option value="{k}"{" selected" if current.reply_style == k else ""}>{v}</option>' for k, v in [('brief','简短'),('normal','自然完整'),('detailed','详细')])
    content += f'</fieldset><fieldset><legend>AI 回答规则</legend><label for="reply_style">默认回答风格</label><select id="reply_style" name="reply_style">{styles}</select><label for="system_prompt">全局系统提示词</label><textarea id="system_prompt" name="system_prompt" maxlength="4000" required>{esc(current.system_prompt)}</textarea><p class="muted">联网搜索按设备设置，默认关闭。当前没有固定总回答字数限制，分段字数只控制播报分段。</p><details><summary>内置行为规则</summary><p>新固件的设备操作依据实际执行结果播报，不调用聊天模型；未收到确认不声称操作成功。旧固件只说明指令交给设备处理。天气与便签快照只作数据，不作为指令。有有效的匹配本地数据时不额外搜索。DeepSeek／千问关闭思考，GLM 使用厂商默认。DeepSeek／GLM 温度参数 0.6，千问使用厂商默认；未指定总输出 Token 上限。记忆按所选轮数、最多 1600 个字符携带。设备角色、记忆、联网与回答风格在设备 KEY 设置中查看。</p></details>'
    content += '</fieldset><fieldset><legend>语音识别与合成主备</legend><div class="row">'
    for name, label in [('asr_route', '识别方案'), ('tts_route', '合成方案')]:
        options = ''.join(f'<option value="{key}"{" selected" if getattr(current, name) == key else ""}>{text}</option>' for key, text in [('local_only', '仅本地'), ('local_first', '本地主用，云端备用'), ('cloud_first', '云端主用，本地备用'), ('cloud_only', '仅云端')])
        content += f'<div><label for="{name}">{label}</label><select id="{name}" name="{name}">{options}</select></div>'
    content += '</div><p class="muted">默认仅本地；备用只在主用异常或未返回有效内容时调用。正常但识别错字不会自动重试云端。云端调用会发送录音或播报文字并可能产生费用，与联网搜索开关独立。</p>'
    content += field('cloud_speech_api_key', '百炼语音 API Key（留空保留）', kind='password', extra='autocomplete="new-password"')
    content += f'<label><input type="checkbox" name="clear_cloud_speech_api_key"> 清除语音 Key</label><p class="muted">{"已配置" if current.cloud_speech_api_key else "未配置"}；加密保存，不回显。</p>'
    content += field('cloud_speech_url', '语音 WSS 接口', current.cloud_speech_url, extra='maxlength="500" required')
    from .speech_fallback import ASR_MODELS, SAMBERT_VOICES
    content += field('cloud_asr_model', '云端识别模型 ID（可手动填写）', current.cloud_asr_model, extra='list="speech-asr-models" maxlength="100" required')
    content += '<datalist id="speech-asr-models">' + ''.join(f'<option value="{esc(model)}"></option>' for model in sorted(ASR_MODELS)) + '</datalist>'
    voice_ids = {f'sambert-{name}-v1' for voices in SAMBERT_VOICES.values() for name, _ in voices}
    content += '<label for="cloud_tts_voice">云端播报音色（Sambert）</label><select id="cloud_tts_voice" name="cloud_tts_voice">'
    if current.cloud_tts_model not in voice_ids:
        content += f'<option selected value="{esc(current.cloud_tts_model)}">自定义：{esc(current.cloud_tts_model)}</option>'
    for group, voices in SAMBERT_VOICES.items():
        content += f'<optgroup label="{esc(group)}">'
        for name, label in voices:
            model = f'sambert-{name}-v1'
            content += f'<option value="{esc(model)}"{" selected" if current.cloud_tts_model == model else ""}>{esc(label)} · {esc(model)}</option>'
        content += '</optgroup>'
    content += '</select>'
    content += field('cloud_tts_model', '自定义合成模型 ID（留空使用上方音色）', extra='maxlength="100"')
    content += '<p class="muted">音色选择仅影响云端合成；本地主用成功时仍使用本地音色。当前接入 16k 实时 ASR 与 Sambert；其他接口类型会在测试中提示。列表不代表实时权限或剩余额度。</p>'
    content += '<details><summary>小样本连通与试听</summary><label><input type="checkbox" name="confirm_speech_test"> 同意本次云端调用消耗额度或产生费用</label><div class="actions"><button class="secondary" formaction="/admin/speech-test" formnovalidate name="test_operation" value="asr">测试识别</button><button class="secondary" formaction="/admin/speech-test" formnovalidate name="test_operation" value="tts">测试与试听音色</button></div><p class="muted">固定短句“今天天气怎么样”，识别测试使用本地合成样本。测试不保存配置、不改变主备，也不替换设备诊断录音；实际云端尝试单独记入语音用量。</p></details>'
    content += '<details><summary>语音费用估算规则</summary><p class="muted">北京 Paraformer 实时识别默认 0.00024 元／秒；Qwen Audio 3.0 为 0.00033 元／秒；Sambert 为 0.0001 元／字符。Qwen Audio 3.1 按 Token 计费，当前不套用秒数价格，标为未计价。下方为手动发送量估算单价，非厂商实际账单；未抵扣免费额度，失败调用费用未确认。</p>'
    content += field('cloud_asr_unit_price', '手动识别估算价（元／录音秒，留空官方规则）', current.cloud_asr_unit_price, kind='number', extra='min="0" max="100000" step="0.00000001"')
    content += field('cloud_tts_unit_price', '手动合成估算价（元／字符，留空官方规则）', current.cloud_tts_unit_price, kind='number', extra='min="0" max="100000" step="0.00000001"')
    content += '<p><a href="https://help.aliyun.com/zh/model-studio/model-pricing" target="_blank" rel="noreferrer">官方价格来源</a></p></details></fieldset>'
    content += '<fieldset><legend>管理员密码</legend><p class="muted">留空不修改，至少 10 个字符。</p><div class="row"><div>' + field('new_admin_password', '新密码', kind='password', extra='autocomplete="new-password"') + '</div><div>' + field('confirm_admin_password', '再次输入', kind='password', extra='autocomplete="new-password"') + '</div></div></fieldset>'
    content += '<fieldset><legend>本地 ASR / TTS</legend><div class="row">'
    for name, label, options in (("asr_provider", "语音识别", (("sensevoice", "SenseVoice"), ("mock", "Mock（仅诊断）"))), ("tts_provider", "语音合成", (("sherpa_vits", "sherpa-onnx VITS"), ("mock", "Mock（仅诊断）")))):
        choices = ''.join(f'<option value="{value}"{" selected" if getattr(current, name) == value else ""}>{text}</option>' for value, text in options)
        content += f'<div><label for="{name}">{label}</label><select id="{name}" name="{name}">{choices}</select></div>'
    content += '</div><details><summary>高级：本地模型路径</summary>'
    paths = (("sensevoice_model", "SenseVoice ONNX"), ("sensevoice_tokens", "SenseVoice tokens.txt"), ("sherpa_tts_model", "中文 VITS ONNX"), ("sherpa_tts_tokens", "中文 tokens.txt"), ("sherpa_tts_lexicon", "中文 lexicon.txt"), ("sherpa_tts_data_dir", "中文 data_dir（vits-zh-ll 留空）"), ("sherpa_english_tts_model", "英文 Piper ONNX"), ("sherpa_english_tts_tokens", "英文 tokens.txt"), ("sherpa_english_tts_data_dir", "英文 espeak-ng-data"))
    for name, label in paths:
        content += field(name, label, getattr(current, name) or "")
    content += '</details><div class="row">'
    for name, label, limits in (("sherpa_tts_speaker_id", "回退中文音色（0～4）", 'min="0" max="4"'), ("sherpa_tts_speed", "回退语速（0.5～2.0）", 'min="0.5" max="2" step="0.05"'), ("segment_min_chars", "最短分段字数", 'min="4" max="20"'), ("segment_max_chars", "最长分段字数", 'min="20" max="80"')):
        content += '<div>' + field(name, label, getattr(current, name), kind='number', extra=limits) + '</div>'
    content += '</div><p class="muted">音色、语速以各设备设置为准。实时分段建议 8 / 40。</p></fieldset><p><button type="submit">保存并重载</button></p></form>'
    firmware = f'当前发布 <strong>{esc(firmware_release.version)}</strong> · {number(firmware_release.size)} 字节' if firmware_release else '尚未发布线上固件。'
    content += f'<fieldset><legend>ESP32 线上固件</legend><p>{firmware}</p><a href="/admin/firmware">版本历史与固件发布</a></fieldset>'
    content += f'<fieldset><legend>配置恢复</legend><p class="muted">恢复接口、模型和主备顺序推荐值；保留 API Key、管理员密码、设备和用量历史。</p><form method="post" action="/admin/reset-recommended">{csrf_input(csrf)}<label><input type="checkbox" name="confirm_reset" required> 我确认恢复推荐配置</label><button class="danger">恢复推荐默认配置</button></form></fieldset>'
    return content


def billing_rule_form(provider, model, rule, csrf, *, new=False):
    content = f'<form method="post" action="/admin/billing"><fieldset><legend>{esc(model or "添加模型单价")}</legend>{csrf_input(csrf)}'
    if new:
        content += '<label>Provider</label><select name="provider">' + ''.join(f'<option value="{key}">{esc(spec["label"])}</option>' for key, spec in PROVIDERS.items()) + '</select><label>模型名称</label><input name="model" maxlength="128" required>'
    else:
        content += f'<input type="hidden" name="provider" value="{esc(provider)}"><input type="hidden" name="model" value="{esc(model)}"><p class="muted">{esc(provider)}</p>'
    mode = (rule or {}).get('mode', 'official')
    content += f'<label>计费规则</label><select name="mode"><option value="official"{" selected" if mode == "official" else ""}>官方默认</option><option value="manual"{" selected" if mode == "manual" else ""}>手动设置</option></select><label>币种</label><select name="currency">'
    for currency in ('CNY', 'USD'):
        content += f'<option{" selected" if (rule or {}).get("currency", "CNY") == currency else ""}>{currency}</option>'
    content += '</select><div class="table-wrap"><table><thead><tr><th>输入 Token 上限</th><th>输入／百万</th><th>输出／百万</th><th>缓存命中／百万</th></tr></thead><tbody>'
    tiers = (rule or {}).get('tiers') or [{'max_input_tokens': 1000000}]
    for index in range(8):
        tier = tiers[index] if index < len(tiers) else {}
        row = '<tr>'
        for name, attribute in [('limit', 'max_input_tokens'), ('input', 'input'), ('output', 'output'), ('cache', 'cached_input')]:
            maximum = 10000000 if name == 'limit' else 100000
            step = '1' if name == 'limit' else '0.00000001'
            row += f'<td><input aria-label="第{index + 1}档 {name}" name="{name}_{index}" type="number" min="{1 if name == "limit" else 0}" max="{maximum}" step="{step}" style="min-width:100px" value="{esc(tier.get(attribute))}"></td>'
        row += '</tr>'
        content += row
    content += f'</tbody></table></div><label>每次搜索费用（留空为未确认）</label><input name="search" type="number" min="0" max="100000" step="0.00000001" value="{esc((rule or {}).get("search"))}">'
    if rule:
        content += f'<p class="muted">核对日期 {esc(rule.get("checked_at"))} · {esc(rule.get("note", ""))}</p>'
        source = rule.get('source', '')
        if source.startswith('https://'):
            content += f'<p><a href="{esc(source)}" target="_blank" rel="noopener">官方价格来源</a></p>'
    else:
        content += '<p class="notice bad">此模型或接入地域没有已核实规则，不会自动按零元计费。</p>'
    return content + '<button type="submit">保存规则</button></fieldset></form>'


def cost_curve(chart, totals):
    data = json.dumps(chart, ensure_ascii=False).replace('<', '\\u003c')
    options = ''.join(f'<option>{esc(currency)}</option>' for currency in totals) or '<option>CNY</option>'
    return '''<div class="panel"><div class="actions"><strong>价格曲线</strong><span id="cost-resolution" class="muted"></span><select id="cost-currency" aria-label="币种">''' + options + '''</select></div>
<svg id="cost-chart" viewBox="0 0 720 220" role="img" aria-label="已计价费用曲线" style="width:100%;max-height:240px"></svg><p id="cost-caption" class="muted"></p></div>
<script>(()=>{const data=''' + data + ''';const $=id=>document.getElementById(id),ns='http://www.w3.org/2000/svg';
function draw(){const rows=data.rows||[],currency=$('cost-currency').value,svg=$('cost-chart');svg.replaceChildren();$('cost-resolution').textContent=({'hour':'按小时','day':'按日','week':'按周','month':'按月'}[data.grain]||'')+' · 跟随上方筛选';
const values=rows.map(r=>r.costs[currency]!=null?Number(r.costs[currency]):r.calls===0?0:null),known=values.filter(v=>v!==null),max=Math.max(...known,0.000001);
function add(tag,attrs,text){const el=document.createElementNS(ns,tag);for(const [key,value] of Object.entries(attrs))el.setAttribute(key,value);if(text!=null)el.textContent=text;svg.append(el);return el;}
add('line',{x1:65,y1:180,x2:700,y2:180,stroke:'#cbd8d5'});add('text',{x:5,y:25,fill:'#6f828d','font-size':11},Number(max.toPrecision(5))+' '+currency);
let points=[];const flush=()=>{if(points.length)add('polyline',{points:points.join(' '),fill:'none',stroke:'#168e83','stroke-width':2});points=[];};
rows.forEach((row,i)=>{const v=values[i],x=65+(rows.length<2?317:i*635/(rows.length-1));if(v===null){flush();return;}const y=180-v/max*145;points.push(x+','+y);const dot=add('circle',{cx:x,cy:y,r:3,fill:'#168e83'}),title=document.createElementNS(ns,'title');title.textContent=row.date+'：'+(row.costs[currency]||'0')+' '+currency+(row.unpriced_calls?'（含未计价调用）':'')+(row.partial_calls?'（费用不完整）':'');dot.append(title);});flush();
if(rows.length){add('text',{x:65,y:210,fill:'#6f828d','font-size':11},rows[0].date);add('text',{x:700,y:210,'text-anchor':'end',fill:'#6f828d','font-size':11},rows.at(-1).date);}
$('cost-caption').textContent=known.length?'时间范围与上方筛选一致；无调用时为零，未计价时留空。鼠标指向圆点查看金额。':'所选范围暂无该币种计价金额。';}
$('cost-currency').onchange=draw;draw();})();</script>'''


def billing_content(current, summary, recent, period, csrf, speech_recent=(), api_report=None):
    content = heading('计费与费用', '日期、用量、单价与总价；按北京时间统计。')
    content += period_toolbar(period, '/admin/billing')
    totals = dict(summary['totals']) if api_report is None else {}
    if api_report:
        from decimal import Decimal
        for values in api_report['totals'].values():
            for currency, amount in values.items():
                totals[currency] = format(Decimal(totals.get(currency, '0')) + Decimal(amount), 'f')
    content += '<p><strong>已计价总额：' + (' · '.join(esc(amount) + ' ' + esc(currency) for currency, amount in totals.items()) or '暂无计价金额') + '</strong></p>'
    if api_report:
        content += cost_curve(api_report['cost_chart'], totals)
        content += '<div class="table-wrap"><table><thead><tr><th>日期</th><th>使用量</th><th>单价</th><th>总价格</th></tr></thead><tbody>'
        for row in api_report['expense_rows']:
            usage = f'输入 {number(row["input_tokens"])}／输出 {number(row["output_tokens"])} Token' if row['kind'] == 'llm' else esc(row['quantity']) + esc(row['unit'])
            warnings = []
            for field, label in (('unpriced_calls', '未计价'), ('unreported_calls', '未回传用量'), ('partial_calls', '费用不完整')):
                if row[field]:
                    warnings.append(f'{row[field]} 次{label}')
            amount = esc(row['cost']) + ' ' + esc(row['currency']) if row['cost'] is not None else '未计价'
            if warnings:
                amount += '<br><small class="bad">' + '；'.join(warnings) + '</small>'
            content += '<tr>' + ''.join('<td>' + value + '</td>' for value in
                                      (esc(row['date']), esc(row['model']) + '<br>' + usage,
                                       esc(row['unit_price']), amount)) + '</tr>'
        content += '</tbody></table></div>'
        if not api_report['expense_rows']:
            content += '<p class="muted">所选周期暂无调用。</p>'
    content += '<p class="muted">不同币种分别统计；未扣赠送额度、折扣与资源包，语音按发送量估算。实际扣费以厂商账单为准。</p>'
    content += '<details><summary>各 API 凭证费用与用量</summary><div class="table-wrap"><table><thead><tr><th>API／模型</th><th>调用</th><th>费用</th></tr></thead><tbody>'
    for row in (api_report or {}).get('by_api', []):
        amount = ' · '.join(esc(value) + ' ' + esc(currency) for currency, value in row['costs'].items()) or '暂无计价金额'
        content += f'<tr><td>{esc(row["provider_label"])} · {esc(row["model"])}<br>{esc(row["key_label"])}</td><td>{row["attempts"]}</td><td>{amount}</td></tr>'
    content += '</tbody></table></div></details><details><summary>价格配置与统计开关</summary>'
    content += f'<form method="post" action="/admin/billing">{csrf_input(csrf)}<input type="hidden" name="action" value="toggle"><label><input type="checkbox" name="billing_enabled"{" checked" if current.billing_enabled else ""}> 启用费用统计</label><button type="submit">保存统计开关</button></form>'
    content += f'<p class="muted">官方规则核对日期 {CHECKED_AT}，不自动抓取更新。修改仅影响后续调用；更换模型或使用第三方代理时请重新核对价格。</p>'
    models = [(p, getattr(current, p + '_model')) for p in PROVIDERS]
    for key in json.loads(current.billing_rules):
        provider, model = key.split(':', 1)
        if (provider, model) not in models:
            models.append((provider, model))
    for provider, model in models:
        rule = selected_rule(current, provider, model, getattr(current, provider + '_base_url'))
        content += billing_rule_form(provider, model, rule, csrf)
    content += '<details><summary>添加其他模型的手动单价</summary>' + billing_rule_form('', '', None, csrf, new=True) + '</details>'
    return content + '</details>'
