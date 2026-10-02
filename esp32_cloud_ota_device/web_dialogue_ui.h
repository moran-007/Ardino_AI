#pragma once
#include <Arduino.h>

namespace DialogueWeb {
template <typename Page>
inline void appendControls(Page &page) {
  page += F(R"HTML(<h3>AI 对话设置</h3>
<button id='ai-load' type='button'>读取当前设置</button>
<p id='ai-message' role='status' aria-live='polite'></p>
<form id='ai-form'><fieldset id='ai-fields' disabled>
<label for='ai-style'>回答风格</label><select id='ai-style'><option value='default'>跟随服务器</option><option value='brief'>简短</option><option value='normal'>适中</option><option value='detailed'>详细</option></select>
<label for='ai-persona'>本设备角色设定</label><textarea id='ai-persona' maxlength='800' style='width:100%;min-height:90px;padding:10px;font:inherit;border:1px solid #b8ceca;border-radius:6px;resize:vertical'></textarea>
<label><input id='ai-search' type='checkbox'> 联网搜索（默认关闭）</label>
<p id='ai-search-state' class='meta'></p>
<label><input id='ai-memory' type='checkbox'> 对话记忆</label>
<label for='ai-turns'>最近记忆轮数</label><input id='ai-turns' type='number' min='1' max='10' value='4' required>
<button type='submit' style='margin-top:12px'>保存 AI 设置</button></fieldset></form>
<details><summary>当前生效规则</summary><p id='ai-default'></p><label>全局系统提示词</label><pre id='ai-prompt' style='white-space:pre-wrap;overflow-wrap:anywhere'></pre><ul id='ai-policies'></ul></details>
<script>(()=>{const $=id=>document.getElementById(id),message=$('ai-message');let saving=false;
function show(d){$('ai-style').value=d.reply_style;$('ai-persona').value=d.persona;$('ai-memory').checked=d.memory_enabled;$('ai-turns').value=d.memory_turns;$('ai-search').checked=d.web_search_enabled;
$('ai-search-state').textContent=d.web_search_available?'千问搜索可用；开启后实时问题可能产生搜索费用。':'未配置支持搜索的千问模型，当前不能联网查询。';
$('ai-default').textContent='服务器默认风格：'+({brief:'简短',normal:'适中',detailed:'详细'}[d.default_reply_style]||d.default_reply_style)+'；播报分段 '+d.segment_min_chars+'～'+d.segment_max_chars+' 字，不限制总回答长度。';
$('ai-prompt').textContent=d.system_prompt;$('ai-policies').replaceChildren(...d.policies.map(t=>{const li=document.createElement('li');li.textContent=t;return li;}));$('ai-fields').disabled=false;}
async function request(method,body){const r=await adminFetch('/ai/settings',{method,headers:body?{'Content-Type':'application/json'}:{},body:body?JSON.stringify(body):undefined});if(!r.ok)throw new Error(await r.text());return r.json();}
$('ai-load').onclick=async()=>{if(saving)return;saving=true;$('ai-load').disabled=true;message.textContent='正在读取…';try{show(await request('GET'));message.textContent='已读取当前设置。';}catch(e){message.textContent='读取失败：'+e.message;}finally{saving=false;$('ai-load').disabled=false;}};
$('ai-form').onsubmit=async e=>{e.preventDefault();if(saving)return;saving=true;$('ai-fields').disabled=true;$('ai-load').disabled=true;try{const d=await request('POST',{reply_style:$('ai-style').value,persona:$('ai-persona').value,memory_enabled:$('ai-memory').checked,memory_turns:Number($('ai-turns').value),web_search_enabled:$('ai-search').checked});show(d);message.textContent='已保存，仅影响本设备后续对话。';}catch(e){message.textContent='保存失败：'+e.message;}finally{saving=false;$('ai-fields').disabled=false;$('ai-load').disabled=false;}};
})();</script>)HTML");
}
}
