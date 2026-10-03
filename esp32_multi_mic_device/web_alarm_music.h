#pragma once
#include <Arduino.h>
namespace AlarmMusicWeb {
template <typename Page>
inline void appendControls(Page &page) {
  page += F(R"HTML(<h3>音乐闹钟</h3><p class='meta'>响铃时说唤醒词，随后说“停止闹钟”或“再等五分钟”。断网时播放本机提示音。</p>
<div class='actions'><button type='button' id='music-load'>读取云端铃声</button><select id='music-selection' aria-label='铃声'></select><button type='button' id='music-save'>保存铃声</button><button type='button' id='alarm-dismiss'>停止闹钟</button><button type='button' id='alarm-snooze'>稍后5分钟</button></div>
<p id='music-current' role='status'>当前铃声：正在读取…</p><div class='actions'><button id='alarm-preview' type='button'>试听当前铃声</button><button id='alarm-preview-stop' type='button' class='secondary'>结束试听</button></div>
<label>上传自己的音乐（截取前15秒，转为单声道铃声）<input id='music-file' type='file' accept='audio/*'></label><button id='music-upload' type='button'>上传到云端</button><p id='music-status' role='status'></p>
<script>(()=>{const $=id=>document.getElementById(id),msg=$('music-status');
async function request(path,options){const r=await fetch(path,options);if(!r.ok)throw new Error(await r.text());return r.json();}
async function load(){const d=await request('/alarm/music');$('music-selection').replaceChildren(...d.tracks.map(t=>{const o=document.createElement('option');o.value=t.id;o.textContent=t.title;o.selected=t.id===d.selected;return o;}));const selected=d.tracks.find(t=>t.id===d.selected);$('music-current').textContent='当前铃声：'+(selected?selected.title:'未设置');msg.textContent='已读取云端铃声';}
function action(fn){return async()=>{try{await fn();}catch(e){msg.textContent=e.message;}};}
$('music-load').onclick=action(load);$('music-save').onclick=action(async()=>{await request('/alarm/music',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:$('music-selection').value})});await load();msg.textContent='铃声已保存';});
for(const [id,path] of [['alarm-preview','/alarm/music/preview'],['alarm-preview-stop','/alarm/music/preview/stop']])$(id).onclick=action(async()=>{await request(path,{method:'POST'});msg.textContent=id==='alarm-preview'?'正在试听当前铃声':'试听已结束';});
for(const [id,path] of [['alarm-dismiss','dismiss'],['alarm-snooze','snooze']])$(id).onclick=action(async()=>{await request('/alarm/'+path,{method:'POST'});msg.textContent=path==='dismiss'?'闹钟已停止':'已延后5分钟';});
$('music-upload').onclick=action(async()=>{const file=$('music-file').files[0];if(!file)throw new Error('请先选择音乐');if(file.size>20*1024*1024)throw new Error('源音乐最大20MB，请先截取');const button=$('music-upload');button.disabled=true;let ctx;
try{msg.textContent='正在转换音乐…';ctx=new AudioContext();const decoded=await ctx.decodeAudioData(await file.arrayBuffer());const frames=Math.min(240000,Math.floor(decoded.duration*16000));if(frames<1600)throw new Error('音乐过短');const offline=new OfflineAudioContext(1,frames,16000),source=offline.createBufferSource();source.buffer=decoded;source.connect(offline.destination);source.start();const rendered=await offline.startRendering(),samples=rendered.getChannelData(0),buffer=new ArrayBuffer(44+frames*2),v=new DataView(buffer);
function str(at,s){for(let i=0;i<s.length;i++)v.setUint8(at+i,s.charCodeAt(i));}str(0,'RIFF');v.setUint32(4,36+frames*2,true);str(8,'WAVEfmt ');v.setUint32(16,16,true);v.setUint16(20,1,true);v.setUint16(22,1,true);v.setUint32(24,16000,true);v.setUint32(28,32000,true);v.setUint16(32,2,true);v.setUint16(34,16,true);str(36,'data');v.setUint32(40,frames*2,true);
for(let i=0;i<frames;i++)v.setInt16(44+i*2,Math.round(Math.max(-1,Math.min(1,samples[i]))*32767),true);
const form=new FormData();form.append('music',new Blob([buffer],{type:'audio/wav'}),file.name.replace(/\.[^.]+$/,'').slice(0,40)+'.wav');msg.textContent='正在上传…';await request('/alarm/music/upload',{method:'POST',body:form});await load();msg.textContent='已上传；选择新铃声后点击保存';
}finally{if(ctx)await ctx.close();button.disabled=false;}});
action(load)();})();</script>)HTML");
}
}
