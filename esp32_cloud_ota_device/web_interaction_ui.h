#pragma once
#include <Arduino.h>

namespace InteractionWeb {
template <typename Page>
inline void appendControls(Page &page) {
  page += F(R"HTML(<fieldset id='interaction-controls'><legend>回应时的小动作</legend>
<p class='meta'>默认关闭。先确认机构的安全活动范围，再开启一只舵机的小幅动作；问候、鼓励和庆祝时执行，完成后回到原指令角度。专注时不执行。</p>
<form id='interaction-form'><fieldset id='interaction-fields' disabled>
<label><input id='interaction-enabled' type='checkbox'> 开启回应动作</label>
<label for='interaction-channel'>舵机通道</label><select id='interaction-channel'><option value='1'>1 号</option><option value='2'>2 号</option><option value='3'>3 号</option><option value='4'>4 号</option></select>
<div class='row'><div><label for='interaction-low'>安全角度下限</label><input id='interaction-low' type='number' min='0' max='179' step='1' value='0' required></div>
<div><label for='interaction-high'>安全角度上限</label><input id='interaction-high' type='number' min='1' max='180' step='1' value='20' required></div></div>
<p class='meta'>跨度最多 30 度。当前角度在范围外或 PWM 关闭时会跳过动作；手动改动角度会取消动作。这里显示的是指令角度，舵机没有位置反馈。</p>
<div class='actions'><button type='submit'>保存动作设置</button><button id='interaction-preview' type='button' class='secondary' disabled>预览一次小动作</button></div>
</fieldset></form><p id='interaction-status' role='status' aria-live='polite'>正在读取动作设置…</p><p id='interaction-message' role='status' aria-live='polite'></p>
</fieldset><script>(()=>{
const $=id=>document.getElementById('interaction-'+id),form=$('form'),fields=$('fields'),message=$('message'),panel=$('controls').closest('[data-panel]');
let state=null,pending=false,polling=false,dirty=false,nextRead=0;
const visible=()=>!document.hidden&&(!panel||!panel.hidden);
function render(d,force=false){state=d;fields.disabled=pending||!!d.busy||!d.initialized;
 $('preview').disabled=pending||!!d.busy||!d.initialized||!d.enabled||dirty;
 if(force||!dirty){$('enabled').checked=!!d.enabled;$('channel').value=String(d.channel);$('low').value=String(d.low_degrees);$('high').value=String(d.high_degrees);if(force)dirty=false;}
 $('status').textContent=(d.busy?'正在执行小动作':d.enabled?'回应动作已开启':'回应动作已关闭')+' · '+d.channel+' 号指令角度 '+d.current_degrees+'°'+(d.servo_output_active?'':' · PWM 未开启');
}
async function request(method,body){const r=await adminFetch('/interaction',{method,headers:body?{'Content-Type':'application/json'}:{},body:body?JSON.stringify(body):undefined});
 let d;const raw=await r.text();try{d=JSON.parse(raw);}catch(_){throw Error(raw||'动作设置响应无效');}if(!r.ok||d.ok===false)throw Error(d.message||raw);return d;}
async function load(){if(pending||polling)return;polling=true;try{const d=await request('GET');render(d);message.textContent=d.message||'';nextRead=Date.now()+(d.busy?400:4000);}catch(e){message.textContent=e.message;nextRead=Date.now()+8000;}finally{polling=false;}}
async function act(action){if(pending||!state)return;if(polling){message.textContent='正在刷新设置，请稍后重试。';return;}
 const body={action};if(action==='config'){if(!form.reportValidity())return;const low=Number($('low').value),high=Number($('high').value);if(high<=low||high-low>30){message.textContent='上限需大于下限，跨度最多 30 度。';return;}
 Object.assign(body,{enabled:$('enabled').checked,channel:Number($('channel').value),low_degrees:low,high_degrees:high});}
 pending=true;render(state);try{const d=await request('POST',body);render(d,true);message.textContent=d.message||'操作成功。';nextRead=Date.now()+400;}
 catch(e){message.textContent=e.message;}finally{pending=false;render(state);}}
form.oninput=()=>{dirty=true;if(state)render(state);};form.onsubmit=e=>{e.preventDefault();act('config');};$('preview').onclick=()=>act('preview');
window.addEventListener('load',()=>{if(visible())load();});window.addEventListener('hashchange',()=>{nextRead=0;});
setInterval(()=>{if(visible()&&Date.now()>=nextRead)load();},500);if(visible())load();
})();</script>)HTML");
}
}  // namespace InteractionWeb
