#pragma once
#include <Arduino.h>
namespace ProfileWeb {
template <typename Page> void appendControls(Page &page) {
  page += F(R"HTML(<h2>个人记忆</h2><p class='intro'>记录你主动告诉机器人的称呼、兴趣、交流偏好、学习目标和日常习惯，让回答更贴合你。</p>
<p class='hint'>当前保存在设备内置存储，可备份到云端。关闭云端备份只停止持久保存；使用云端对话时，已启用的个人信息仍会作为回答上下文发送。请不要填写密码、证件、联系方式或健康信息；已识别的常见敏感内容会拒绝保存。</p>
<p id='profile-message' role='status' aria-live='polite'></p>
<form id='profile-policy'><fieldset id='profile-policy-fields' disabled><legend>记忆偏好</legend>
<label><input id='profile-enabled' type='checkbox'> 对话使用个人记忆</label>
<label><input id='profile-auto' type='checkbox'> 从明确的自我描述自动记录</label>
<label><input id='profile-backup' type='checkbox'> 在云端保存备份</label><button>保存偏好</button></fieldset></form>
<fieldset><legend>已记住的信息</legend><p id='profile-sync' class='meta'></p><div id='profile-items'></div><div class='actions'><button id='profile-reload' type='button' class='secondary'>刷新</button><button id='profile-clear' type='button' class='danger'>清空个人记忆</button><button id='profile-sync-now' type='button' class='secondary'>同步云端备份</button><button id='profile-restore' type='button' class='secondary'>从云端恢复</button></div></fieldset>
<form id='profile-add'><fieldset id='profile-add-fields' disabled><legend>添加或更新信息</legend><label for='profile-category'>类别</label><select id='profile-category'><option value='name'>称呼</option><option value='interest'>兴趣</option><option value='communication'>交流偏好</option><option value='goal'>学习目标</option><option value='habit'>日常习惯</option></select><label for='profile-value'>内容</label><input id='profile-value' maxlength='180' required><button>保存信息</button></fieldset></form>
<p class='hint'>也可以说“记住我喜欢机器人”“你记住了什么”“忘记我的名字”“清空个人记忆”。本设备按同一位使用者保存，暂不区分说话人。</p>
<script>(()=>{const $=id=>document.getElementById(id),message=$('profile-message'),labels={name:'称呼',interest:'兴趣',communication:'交流偏好',goal:'学习目标',habit:'习惯'};let working=false;
async function request(body){const r=await adminFetch('/profile',{method:body?'POST':'GET',headers:body?{'Content-Type':'application/json'}:{},body:body?JSON.stringify(body):undefined});const raw=await r.text();let d;try{d=JSON.parse(raw);}catch(_){throw new Error(raw||'个人记忆响应无效。');}if(!r.ok)throw new Error(d.message||raw);return d;}
function show(d){const p=d.profile;$('profile-enabled').checked=p.enabled;$('profile-auto').checked=p.auto_collect;$('profile-backup').checked=p.backup_enabled;$('profile-policy-fields').disabled=!d.ready;$('profile-add-fields').disabled=!d.ready;
$('profile-sync').textContent='设备版本 '+p.revision+' · '+(d.synced?'云端已同步':(d.sync_message||'等待同步'))+' · 存储：'+d.storage;
const nodes=p.items.map(item=>{const row=document.createElement('div');row.className='alarm-row';const text=document.createElement('p');text.textContent=item.id+'号 '+labels[item.category]+'：'+item.value+'（'+(item.source==='explicit'?'主动保存':'自我描述')+'）';const edit=document.createElement('button');edit.type='button';edit.className='secondary';edit.textContent='修改';edit.onclick=()=>{const v=prompt('修改这条信息：',item.value);if(v!==null)act({action:'edit',id:item.id,category:item.category,value:v});};const del=document.createElement('button');del.type='button';del.className='danger';del.textContent='忘记';del.onclick=()=>act({action:'remove',id:item.id});row.append(text,edit,del);return row;});$('profile-items').replaceChildren(...nodes);if(!nodes.length)$('profile-items').textContent='还没有保存个人信息。';}
async function act(body){if(working)return;working=true;message.textContent='处理中…';try{const d=await request(body);show(d);message.textContent=d.message||'已读取。';}catch(e){message.textContent=e.message;}finally{working=false;}}
$('profile-reload').onclick=()=>act();$('profile-sync-now').onclick=()=>act({action:'sync'});$('profile-clear').onclick=()=>{if(confirm('清空本机个人记忆，并在联网后覆盖清除云端备份？'))act({action:'clear'});};$('profile-restore').onclick=()=>{if(confirm('用当前云端备份替换本机个人记忆？已关闭备份时没有可恢复的信息。'))act({action:'restore'});};
$('profile-policy').onsubmit=e=>{e.preventDefault();act({action:'policy',enabled:$('profile-enabled').checked,auto_collect:$('profile-auto').checked,backup_enabled:$('profile-backup').checked});};$('profile-add').onsubmit=e=>{e.preventDefault();act({action:'add',category:$('profile-category').value,value:$('profile-value').value});};
document.addEventListener('click',e=>{if(e.target.closest('[data-nav="personal"]'))act();});act();})();</script>)HTML");
}
}
