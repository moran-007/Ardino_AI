#pragma once
#include <Arduino.h>

namespace MultiMicWeb {
inline void appendControls(String &page) {
  page += F(R"HTML(<h3>麦克风模式</h3>
<form id="mic-form"><label for="mic-count">单 / 多麦克风</label>
<select id="mic-count"><option value="1">单麦克风 · MIC1</option><option value="2">双麦克风 · MIC1 + MIC2</option><option value="3">三麦克风 · MIC1 + MIC2 + MIC3</option></select>
<label for="mic-preferred">收音通道</label><select id="mic-preferred"><option value="0">自动选择信号较强的通道</option><option value="1">固定 MIC1</option><option value="2">固定 MIC2</option><option value="3">固定 MIC3</option></select>
<p class="meta">切换多麦克风会检测 1.5 秒，请对各麦克风说话。全部通道有变化信号才保存；失败恢复原设置。当前根据音量选通道，录音中保持通道，方向辨别将在后续加入。</p>
<div class="actions"><button type="submit">检测并保存模式</button><button id="mic-probe" type="button">检测当前麦克风</button></div></form>
<p id="mic-message" role="status" aria-live="polite"></p><pre id="mic-diagnostics"></pre>
<script>(()=>{
const form=document.getElementById('mic-form'),count=document.getElementById('mic-count'),preferred=document.getElementById('mic-preferred'),message=document.getElementById('mic-message'),diagnostics=document.getElementById('mic-diagnostics');
let active=false;
function choices(){for(const o of preferred.options)o.disabled=Number(o.value)>Number(count.value);if(Number(preferred.value)>Number(count.value))preferred.value='0';}
function show(d){diagnostics.textContent=d.diagnostics||'';if(!active){count.value=String(d.mask===7?3:d.mask===3?2:1);preferred.value=String(d.preferred||0);choices();}}
count.addEventListener('change',choices);
async function run(data){if(active)return;active=true;for(const c of form.elements)c.disabled=true;message.textContent='请对麦克风说话，正在检测…';try{const r=await fetch('/microphones',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});let d;if((r.headers.get('content-type')||'').includes('application/json')){d=await r.json();diagnostics.textContent=d.diagnostics||'';message.textContent=d.message||'';}else throw new Error(await r.text());if(!r.ok&&!d.message)throw new Error('配置未保存');}catch(e){message.textContent=e.message;}finally{active=false;for(const c of form.elements)c.disabled=false;await load();}}
async function load(){try{const r=await fetch('/microphones',{credentials:'same-origin',cache:'no-store'});if(r.ok)show(await r.json());}catch(e){message.textContent=e.message;}}
form.addEventListener('submit',e=>{e.preventDefault();run({action:'config',count:Number(count.value),preferred:Number(preferred.value)});});
document.getElementById('mic-probe').addEventListener('click',()=>run({action:'probe'}));
choices();load();
})();</script>)HTML");
}
}  // namespace MultiMicWeb
