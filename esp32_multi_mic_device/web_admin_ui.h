#pragma once
#include <Arduino.h>

static const char ADMIN_PAGE_HEAD[] PROGMEM = R"HTML(
<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'><title>机器人控制台</title>
<style>
:root{color-scheme:light;--ink:#17302c;--muted:#647772;--line:#dce6e1;--accent:#126b56;--paper:#fff}
*{box-sizing:border-box}body{margin:0;background:#f1f5f2;color:var(--ink);font:15px/1.6 system-ui,'Microsoft YaHei',sans-serif}
button,input,select{font:inherit}button,a,input,select,summary{outline-offset:4px}button:focus-visible,a:focus-visible{outline:3px solid #d6a434}
header{max-width:1180px;margin:auto;padding:28px 24px 20px;display:flex;align-items:center;justify-content:space-between;gap:16px}
header h1{margin:0;font-size:26px;letter-spacing:1px}header p{margin:2px 0;color:var(--muted);font-size:13px}
.shell{max-width:1180px;margin:auto;padding:0 24px 40px;display:grid;grid-template-columns:190px minmax(0,1fr);gap:28px}
nav{position:sticky;top:20px;align-self:start;display:grid;gap:6px}nav a{text-decoration:none;color:var(--muted);padding:12px 16px;border-radius:9px}nav a:hover{background:#e5ede8}nav a[aria-current=page]{background:var(--accent);color:white;font-weight:600}
main{min-width:0}.panel[hidden]{display:none!important}h2{font-size:24px;margin:0 0 6px}h3{font-size:17px}.intro{color:var(--muted);margin:0 0 22px}
fieldset,.card{min-width:0;background:var(--paper);border:1px solid var(--line);border-radius:12px;padding:22px;margin:0 0 20px}
legend{font-size:18px;font-weight:650;padding:0 7px}p{overflow-wrap:anywhere}label{display:block;margin:14px 0 6px;font-weight:550}
input,select{width:100%;padding:11px 12px;border:1px solid #bccdc4;border-radius:7px;background:#fff;color:var(--ink)}input[type=checkbox]{width:auto;accent-color:var(--accent)}input[readonly]{background:#f3f6f4;color:var(--muted)}input[type=file]{border-style:dashed}
button{border:1px solid transparent;border-radius:7px;padding:10px 17px;background:var(--accent);color:white;cursor:pointer}button:hover{filter:brightness(.94)}button:disabled{opacity:.45;cursor:not-allowed}.secondary{color:var(--accent);background:#e9f2ec;border-color:#cdded3}.danger{background:#fff1ef;color:#a23d32;border-color:#edccc6}
.actions,.weather-actions{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:12px 0}.actions form{margin:0}.row,.photo-grid,.quick-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}.quick-grid a{padding:20px;background:#fff;border:1px solid var(--line);border-radius:10px;text-decoration:none;color:var(--ink)}.quick-grid span{display:block;font-size:13px;color:var(--muted);margin-top:5px}
.photo-card{border:1px solid var(--line);padding:16px;border-radius:9px;background:white}.photo-card.selected{border:2px solid var(--accent);background:#f4faf6}.photo-card .actions button{padding:7px 12px}.photo-card p{margin:6px 0}.hint,.meta,small{color:var(--muted);font-size:13px}.ok{color:#08783f}.warn{color:#9d4e18}.meta{margin-bottom:4px}
details{border:1px solid var(--line);border-radius:10px;padding:14px;margin:16px 0;background:#fafcfb}summary{cursor:pointer;font-weight:600}details fieldset{margin-top:18px}details p:last-child{margin-bottom:0}.weather-result button{display:block;width:100%;margin:6px 0;text-align:left;background:#e9f2ec;color:var(--ink)}
.chat{display:flex;flex-direction:column;gap:10px;max-height:420px;overflow:auto}.msg{padding:10px 12px;border-radius:9px;white-space:pre-wrap;overflow-wrap:anywhere}.user{background:#e7f0ff;margin-left:10%}.assistant{background:#eef7ee;margin-right:10%}.alarm-row{border-bottom:1px solid var(--line);padding:12px 0}.alarm-row form{display:inline}code{overflow-wrap:anywhere}pre{font-size:12px}canvas{border-radius:8px}
@media(max-width:720px){header{padding:20px 16px}header h1{font-size:22px}.shell{display:block;padding:0 16px 24px}nav{position:static;display:flex;overflow-x:auto;gap:6px;margin-bottom:22px;padding-bottom:6px}nav a{white-space:nowrap;padding:9px 12px}fieldset,.card{padding:16px}.row,.quick-grid{grid-template-columns:1fr}.photo-grid{grid-template-columns:1fr}.user,.assistant{margin:0}}
</style></head><body>
<header><div><h1>机器人控制台</h1><p>管理屏幕、相册和日常提醒</p></div><form method='post' action='/logout'><button class='secondary' type='submit'>退出登录</button></form></header>
<div class='shell'><nav aria-label='控制台分区'>
<a href='#home' data-nav='home'>首页</a><a href='#music' data-nav='music'>音乐与歌词</a><a href='#display' data-nav='display'>屏幕与相册</a><a href='#expressions' data-nav='expressions'>表情库</a><a href='#reminders' data-nav='reminders'>提醒事项</a><a href='#focus' data-nav='focus'>番茄钟</a><a href='#personal' data-nav='personal'>个人记忆</a><a href='#voice' data-nav='voice'>语音与音量</a><a href='#network' data-nav='network'>联网与天气</a><a href='#system' data-nav='system'>设备与更新</a><a href='#diagnostics' data-nav='diagnostics'>运行记录</a>
</nav><main>
)HTML";

static const char ADMIN_NAV_SCRIPT[] PROGMEM = R"HTML(
<script>
window.activeAdminPanel='home';let networkOpened=false;
function showAdminPanel(name){
  const names=['home','music','display','expressions','reminders','focus','personal','voice','network','system','diagnostics'];
  if(!names.includes(name))name='home';window.activeAdminPanel=name;
  document.querySelectorAll('[data-panel]').forEach(panel=>panel.hidden=panel.dataset.panel!==name);
  document.querySelectorAll('[data-nav]').forEach(link=>{if(link.dataset.nav===name)link.setAttribute('aria-current','page');else link.removeAttribute('aria-current');});
  try{sessionStorage.setItem('robot-panel',name);}catch(_){}
  if(name==='network'&&!networkOpened){networkOpened=true;loadSavedWifi();scanWifi(true);}
  if(name==='display')refreshStandbyPhotoStatus();
  if(name==='expressions')loadExpressions();
}
window.addEventListener('hashchange',()=>showAdminPanel(location.hash.slice(1)));
let initialPanel=location.hash.slice(1);if(!initialPanel)try{initialPanel=sessionStorage.getItem('robot-panel');}catch(_){}
showAdminPanel(initialPanel||'home');
</script></main></div></body></html>
)HTML";
