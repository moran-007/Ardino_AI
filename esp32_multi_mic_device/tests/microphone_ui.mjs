import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';

const source = fs.readFileSync(new URL('../web_multi_mic_ui.h', import.meta.url), 'utf8').match(/<script>([\s\S]*?)<\/script>/)[1];
const nodes = new Map(['mic-form','mic-count','mic-preferred','mic-message','mic-diagnostics','mic-probe'].map(id=>[id,{value:'1',disabled:false,textContent:'',listeners:{},addEventListener(type,fn){this.listeners[type]=fn;}}]));
const form=nodes.get('mic-form'),count=nodes.get('mic-count'),preferred=nodes.get('mic-preferred');
preferred.options=[0,1,2,3].map(value=>({value:String(value),disabled:false}));
form.elements=[count,preferred,nodes.get('mic-probe'),{disabled:false}];
let device={mask:1,preferred:0,diagnostics:'MIC1'},failure=false,pending=false;
const calls=[];
vm.runInNewContext(source,{document:{getElementById:id=>nodes.get(id)},fetch:async(path,options={})=>{
  calls.push({path,options});
  if(options.method==='POST'){
    assert.ok(form.elements.every(c=>c.disabled),'controls enabled during detection');
    const body=JSON.parse(options.body);
    if(pending)await new Promise(resolve=>setImmediate(resolve));
    if(!failure&&body.action==='config')device={mask:(1<<body.count)-1,preferred:body.preferred,diagnostics:'MIC1 MIC2'};
    return {ok:!failure,headers:{get:()=> 'application/json'},json:async()=>({...device,message:failure?'检测失败，恢复原配置':'已保存'})};
  }
  return {ok:true,json:async()=>device};
}});
async function flush(){for(let i=0;i<8;i++)await new Promise(resolve=>setImmediate(resolve));}
await flush();
assert.equal(count.value,'1');assert.equal(preferred.value,'0');
count.value='2';count.listeners.change();
assert.ok(preferred.options[3].disabled);assert.ok(!preferred.options[2].disabled);
preferred.value='2';pending=true;
form.listeners.submit({preventDefault(){}});form.listeners.submit({preventDefault(){}});
await flush();
assert.equal(calls.filter(c=>c.options.method==='POST').length,1,'duplicate request during detection');
assert.deepEqual(JSON.parse(calls.find(c=>c.options.method==='POST').options.body),{action:'config',count:2,preferred:2});
assert.equal(count.value,'2');assert.equal(preferred.value,'2');
assert.ok(form.elements.every(c=>!c.disabled));
failure=true;count.value='3';preferred.value='3';form.listeners.submit({preventDefault(){}});
await flush();
assert.equal(count.value,'2','failed selection restored device count');
assert.equal(preferred.value,'2','failed selection restored fixed channel');
assert.match(nodes.get('mic-message').textContent,/恢复原配置/);
nodes.get('mic-probe').listeners.click();await flush();
assert.equal(JSON.parse(calls.filter(c=>c.options.method==='POST').at(-1).options.body).action,'probe');
console.log('PASS: microphone UI detection requests, duplicate guard, channel validation and rollback display.');
