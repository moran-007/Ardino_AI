#pragma once
#include <Arduino.h>

namespace FocusWeb {
template <typename Page>
inline void appendControls(Page &page) {
  page += F(R"HTML(<fieldset id='focus-controls'><legend>番茄钟与专注</legend>
<p id='focus-status' role='status' aria-live='polite'>正在读取番茄钟…</p>
<p id='focus-countdown' style='font-size:32px;font-variant-numeric:tabular-nums;margin:8px 0'>--:--</p>
<p id='focus-completed' class='meta'></p>
<div style='display:flex;flex-wrap:wrap;gap:8px;margin-bottom:16px'>
<button type='button' data-focus-action='start' disabled>开始专注</button>
<button type='button' data-focus-action='pause' class='secondary' disabled>暂停</button>
<button type='button' data-focus-action='resume' class='secondary' disabled>继续</button>
<button type='button' data-focus-action='skip' class='secondary' disabled>跳过阶段</button>
<button type='button' data-focus-action='stop' class='danger' disabled>结束</button>
<button type='button' id='focus-refresh' class='secondary'>刷新</button></div>
<form id='focus-config'><fieldset id='focus-fields' disabled>
<label for='focus-minutes'>专注时长（分钟）</label><input id='focus-minutes' type='number' min='1' max='120' step='1' value='25' required>
<label for='focus-short'>短休息（分钟）</label><input id='focus-short' type='number' min='1' max='60' step='1' value='5' required>
<label for='focus-long'>长休息（分钟）</label><input id='focus-long' type='number' min='1' max='120' step='1' value='15' required>
<label for='focus-rounds'>每几次专注后长休息</label><input id='focus-rounds' type='number' min='1' max='12' step='1' value='4' required>
<button type='submit' style='margin-top:12px'>保存设置</button></fieldset></form>
<p class='meta'>完成专注后自动休息，休息结束后自动开始下一轮。跳过专注不计完成次数。断电后从最近一分钟的保存点暂停恢复，继续前请确认剩余时间。</p>
<p class='meta'>语音示例：“开始专注二十五分钟”“暂停专注”“继续专注”“结束专注”“专注状态”。</p>
<p id='focus-message' role='status' aria-live='polite'></p>
</fieldset><script>(()=>{const $=id=>document.getElementById(id),fields=$('focus-fields'),message=$('focus-message'),panel=$('focus-controls').closest('[data-panel]');
const buttons=Array.from(document.querySelectorAll('[data-focus-action]'));let state=null,busy=false,dirty=false,lastSync=0,nextPoll=0,polling=false,readFailed=false;
function visible(){return !document.hidden&&(typeof window.activeAdminPanel==='string'?window.activeAdminPanel==='focus':(!panel||!panel.hidden));}
function config(){return{focus_minutes:Number($('focus-minutes').value),short_break_minutes:Number($('focus-short').value),long_break_minutes:Number($('focus-long').value),rounds_before_long_break:Number($('focus-rounds').value)};}
function render(){if(!state)return;const running=state.status==='running',active=state.status!=='idle',labels={focus:'专注中',short_break:'短休息',long_break:'长休息',idle:'待开始'};
const status=(labels[state.phase]||'状态未知')+(state.status==='paused'?' · 已暂停':'')+(state.storage_error?' · 存储异常，请重试':'');if($('focus-status').textContent!==status)$('focus-status').textContent=status;
const elapsed=running?Math.floor((performance.now()-lastSync)/1000):0,seconds=Math.max(0,state.remaining_seconds-elapsed);
$('focus-countdown').textContent=Math.floor(seconds/60)+':'+String(seconds%60).padStart(2,'0');$('focus-completed').textContent='累计完成 '+state.completed_sessions+' 次专注 · 本周期 '+state.cycle_sessions+' 次';
buttons.forEach(b=>{const a=b.dataset.focusAction;b.disabled=busy||!state.initialized||(a==='start'?active:a==='pause'?!running:a==='resume'?state.status!=='paused':!active);});fields.disabled=busy||!state.initialized;$('focus-refresh').disabled=busy;}
function show(d,force){if(!d||!Number.isFinite(d.remaining_seconds)||d.remaining_seconds<0)throw Error('设备返回的倒计时无效');state=d;lastSync=performance.now();nextPoll=Date.now()+5000;if(force||!dirty){$('focus-minutes').value=d.focus_minutes;$('focus-short').value=d.short_break_minutes;$('focus-long').value=d.long_break_minutes;$('focus-rounds').value=d.rounds_before_long_break;if(force)dirty=false;}render();}
async function request(method,body){const r=await adminFetch('/focus',{method,headers:body?{'Content-Type':'application/json'}:{},body:body?JSON.stringify(body):undefined}),text=await r.text();let d;try{d=JSON.parse(text);}catch(e){throw Error(!r.ok&&text?text:'设备响应无效（HTTP '+r.status+'）');}if(!r.ok||d.ok===false)throw Error(d.message||('HTTP '+r.status));return d;}
async function load(manual){if(busy||polling)return;polling=true;if(manual)message.textContent='正在读取…';try{show(await request('GET'),false);if(manual||readFailed)message.textContent='已读取当前状态。';readFailed=false;}catch(e){message.textContent='读取失败：'+e.message;readFailed=true;nextPoll=Date.now()+10000;}finally{polling=false;}}
async function act(action){if(busy||!state)return;if(polling){message.textContent='正在刷新状态，请稍后重试。';return;}if((action==='start'||action==='configure')&&!$('focus-config').reportValidity())return;busy=true;render();message.textContent='正在保存…';try{const d=await request('POST',Object.assign({action},action==='start'||action==='configure'?config():{}));show(d,true);message.textContent=d.message||'操作成功。';}catch(e){message.textContent='操作失败：'+e.message;}finally{busy=false;render();}}
buttons.forEach(b=>b.onclick=()=>act(b.dataset.focusAction));$('focus-config').oninput=()=>{dirty=true;};$('focus-config').onsubmit=e=>{e.preventDefault();act('configure');};$('focus-refresh').onclick=()=>load(true);
window.addEventListener('hashchange',()=>{if(location.hash==='#focus')load(false);});const nav=document.querySelector('[data-nav="focus"]');if(nav)nav.addEventListener('click',()=>load(false));
setInterval(()=>{if(!visible())return;render();if(Date.now()>=nextPoll)load(false);},1000);if(visible()||location.hash==='#focus')load(false);
})();</script>)HTML");
}
}  // namespace FocusWeb
