#pragma once
#include <Arduino.h>

namespace WebCustomWake {
template <typename Page>
inline void appendControls(Page &page) {
  page += F(R"HTML(
<fieldset id="custom-wake"><legend>自定义唤醒词（实验）</legend>
<p class="meta">为自己的声音录制唤醒词，建议 3 至 6 个字。只填写文字不会训练识别。样本特征保存在本机，不保存原始录音，也不上传持续监听音频。</p>
<label for="custom-wake-label">唤醒词名称</label>
<input id="custom-wake-label" maxlength="20" autocomplete="off" placeholder="例如：小默小默">
<div class="actions"><button id="custom-wake-save" type="button">保存名称</button>
<button id="custom-wake-train" type="button">录制 1 份样本</button>
<button id="custom-wake-cancel" type="button" hidden>取消录制</button></div>
<p class="meta">先保存名称，待设备空闲，点击录制后关注下方状态；显示“正在录音”时说 1 次，保持短暂停顿。每次录音 3 秒，建议分别录 3 份。改变名称会清空旧样本；准确率受说话人和环境影响。</p>
<p id="custom-wake-status" role="status" aria-live="polite">正在读取状态…</p>
<p id="custom-wake-message" role="status" aria-live="polite"></p>
<label><input id="custom-wake-enable" type="checkbox"> 开启自定义唤醒（至少 1 份有效样本）</label>
<button id="custom-wake-reset" class="danger" type="button">清空样本并停用</button>
</fieldset><script>
(()=>{
const byId=id=>document.getElementById('custom-wake-'+id);
const label=byId('label'),enable=byId('enable'),status=byId('status'),message=byId('message');
let current={},requesting=false,polling=false,timer=0;
const pending=()=>current.training==='queued'||current.training==='recording';
function render(d){
 current=d;const locked=requesting||pending();
 if(document.activeElement!==label)label.value=d.label||'';
 label.disabled=locked;byId('save').disabled=locked;
 byId('train').disabled=locked||Number(d.samples)>=3;
 byId('cancel').hidden=!pending();byId('cancel').disabled=requesting;
 byId('reset').disabled=locked||(!Number(d.samples)&&d.initialized!==false&&!d.cleanupPending);
 enable.checked=!!d.enabled;enable.disabled=locked||!d.ready;
 const state=d.training==='queued'?'等待设备空闲，尚未开始录音':d.training==='recording'?'正在录音：现在说 1 次唤醒词':d.training==='success'?'样本保存成功':d.training==='error'?'录制或保存失败':d.training==='cancelled'?'录制已取消':d.enabled?'自定义唤醒已开启':d.ready?'已有样本，可开启；建议录满 3 份':'请先录制样本';
 status.textContent=state+' · 样本 '+Number(d.samples||0)+'/3';
 message.textContent=d.message||'';
}
async function request(method,body){
 const r=await adminFetch('/wake/custom',{method,headers:body?{'Content-Type':'application/json'}:{},body:body?JSON.stringify(body):undefined});
 if(!r.ok){let detail=await r.text();try{detail=JSON.parse(detail).message||detail;}catch(_){}throw new Error(detail);}return r.json();
}
async function poll(){
 clearTimeout(timer);if(polling)return;
 if(window.activeAdminPanel!=='voice'&&!pending()){timer=setTimeout(poll,4000);return;}
 polling=true;try{render(await request('GET'));}catch(e){message.textContent='读取唤醒状态失败：'+e.message;}
 finally{polling=false;timer=setTimeout(poll,pending()?400:4000);}
}
async function act(action,extra={}){
 if(requesting)return;requesting=true;render(current);
 let failure='';
 try{render(await request('POST',{action,...extra}));}
 catch(e){failure=e.message;}
 finally{requesting=false;render(current);if(failure)message.textContent=failure;clearTimeout(timer);timer=setTimeout(poll,100);}
}
byId('save').onclick=()=>act('label',{label:label.value});
byId('train').onclick=()=>act('train');
byId('cancel').onclick=()=>act('cancel');
enable.onchange=()=>act('enable',{enabled:enable.checked});
byId('reset').onclick=()=>{if(confirm('清空本机自定义唤醒样本并停用？'))act('reset');};
window.addEventListener('load',poll);
window.addEventListener('hashchange',()=>{clearTimeout(timer);timer=setTimeout(poll,50);});
poll();
})();
</script>)HTML");
}
}  // namespace WebCustomWake
