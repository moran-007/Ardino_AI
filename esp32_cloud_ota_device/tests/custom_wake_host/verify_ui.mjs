import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const html=fs.readFileSync(process.argv[2], 'utf8');
const code=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m=>m[1]).join('\n');
new vm.Script(code);
const controls=new Map();
for(const id of ['label','enable','status','message','save','train','cancel','reset'])controls.set('custom-wake-'+id,{value:'',checked:false,disabled:false,hidden:false,textContent:''});
let server={label:'小默小默',enabled:false,ready:false,samples:0,training:'idle',message:'请录制样本'};
let reject=false;const calls=[];let timer;const events={};
const browser={activeAdminPanel:'voice',addEventListener:(name,callback)=>{events[name]=callback;}};
const context=vm.createContext({
 document:{activeElement:null,getElementById:id=>controls.get(id)},
 window:browser,
 adminFetch:async(path, options)=>{
  calls.push({path,...options});
  if(reject)return {ok:false,text:async()=>'设备正忙'};
  if(options.method==='POST'){
   const d=JSON.parse(options.body);
   if(d.action==='label')server={...server,label:d.label,samples:0,ready:false};
   if(d.action==='train')server={...server,training:'queued'};
   if(d.action==='cancel')server={...server,training:'cancelled'};
   if(d.action==='enable')server={...server,enabled:d.enabled};
   if(d.action==='reset')server={...server,samples:0,ready:false,enabled:false,training:'idle'};
  }
  return {ok:true,json:async()=>({...server})};
 },
 setTimeout:fn=>{timer=fn;return 1;},clearTimeout:()=>{},confirm:()=>true,
});
vm.runInContext(code,context);
const flush=async()=>{for(let i=0;i<10;i++)await Promise.resolve();};
const el=id=>controls.get('custom-wake-'+id);
await flush();
assert.equal(el('enable').disabled,true);assert.equal(el('cancel').hidden,true);
browser.activeAdminPanel='home';const reads=calls.length;timer();await flush();
assert.equal(calls.length,reads,'inactive panels must not poll status');
browser.activeAdminPanel='voice';events.hashchange();timer();await flush();
assert.equal(calls.length,reads+1,'opening voice panel must refresh status');
el('label').value='<自定义>';el('save').onclick();await flush();
assert.equal(JSON.parse(calls.at(-1).body).label,'<自定义>');assert.equal(el('label').value,'<自定义>');
el('train').onclick();await flush();
assert.equal(el('label').disabled,true);assert.equal(el('train').disabled,true);assert.equal(el('cancel').hidden,false);
assert.match(el('status').textContent,/尚未开始录音/);
browser.activeAdminPanel='home';const queuedReads=calls.length;timer();await flush();
assert.equal(calls.length,queuedReads+1,'pending training must keep polling outside voice panel');
server={...server,training:'recording'};timer();await flush();
assert.match(el('status').textContent,/正在录音/);assert.equal(el('enable').disabled,true);
el('cancel').onclick();await flush();
assert.equal(el('cancel').hidden,true);assert.match(el('status').textContent,/已取消/);
browser.activeAdminPanel='voice';
server={...server,training:'success',samples:1,ready:true};timer();await flush();
assert.equal(el('enable').disabled,false);assert.match(el('status').textContent,/保存成功.*1\/3/);
el('enable').checked=true;el('enable').onchange();await flush();
assert.equal(JSON.parse(calls.at(-1).body).enabled,true);assert.equal(el('enable').checked,true);
reject=true;el('reset').onclick();await flush();
assert.equal(el('message').textContent,'设备正忙');
reject=false;el('reset').onclick();await flush();
assert.equal(el('enable').checked,false);assert.equal(el('enable').disabled,true);
server={...server,initialized:false,cleanupPending:true,message:'样本损坏，请清空'};timer();await flush();
assert.equal(el('reset').disabled,false,'corruption or incomplete purge must leave explicit reset available');
assert.ok(calls.every(c=>c.path==='/wake/custom'));
console.log('PASS: custom wake UI real JavaScript queue/record/cancel/result/enable/error/reset flows.');
