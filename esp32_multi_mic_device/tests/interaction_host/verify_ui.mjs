import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const html=fs.readFileSync(process.argv[2],'utf8');
const code=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m=>m[1]).join('\n');new vm.Script(code);
const panel={hidden:false},nodes=new Map();
for(const id of ['form','fields','message','controls','enabled','channel','low','high','status','preview'])nodes.set('interaction-'+id,{value:'',checked:false,disabled:false,textContent:'',closest:()=>panel,reportValidity:()=>true});
const el=id=>nodes.get('interaction-'+id);
let time=1000,interval,reject=false;
let state={initialized:true,enabled:false,busy:false,channel:1,low_degrees:0,high_degrees:20,current_degrees:10,servo_output_active:true,message:'关闭'};
const calls=[];
const context=vm.createContext({
 document:{hidden:false,getElementById:id=>nodes.get(id)},window:{addEventListener:()=>{}},
 Date:{now:()=>time},setInterval:fn=>{interval=fn;return 1;},
 adminFetch:async(path,options)=>{
  calls.push({path,...options});if(reject)return{ok:false,text:async()=>JSON.stringify({message:'存储失败'})};
  if(options.method==='POST'){
   const body=JSON.parse(options.body);if(body.action==='config')state={...state,...body,message:'已保存'};
   else if(body.action==='preview')state={...state,busy:true,message:'正在执行'};
  }
  return{ok:true,text:async()=>JSON.stringify(state)};
 }
});
vm.runInContext(code,context);
const flush=async()=>{for(let i=0;i<10;i++)await Promise.resolve();};
await flush();assert.equal(el('preview').disabled,true);assert.equal(el('fields').disabled,false);
el('enabled').checked=true;el('channel').value='2';el('low').value='10';el('high').value='20';el('form').oninput();
assert.equal(el('preview').disabled,true,'unsaved changes must block preview');
el('form').onsubmit({preventDefault(){}});await flush();
const config=JSON.parse(calls.at(-1).body);assert.equal(config.action,'config');assert.equal(config.enabled,true);assert.equal(config.channel,2);assert.equal(config.low_degrees,10);assert.equal(el('preview').disabled,false);
el('preview').onclick();await flush();assert.equal(el('fields').disabled,true);assert.match(el('status').textContent,/正在执行/);
state={...state,busy:false};time+=1000;interval();await flush();assert.equal(el('fields').disabled,false);
el('low').value='0';el('high').value='40';el('form').oninput();const before=calls.length;el('form').onsubmit({preventDefault(){}});await flush();
assert.equal(calls.length,before,'wide range must be rejected before POST');assert.match(el('message').textContent,/30 度/);
el('high').value='20';reject=true;el('form').onsubmit({preventDefault(){}});await flush();assert.equal(el('message').textContent,'存储失败');
panel.hidden=true;time+=10000;const hidden=calls.length;interval();await flush();assert.equal(calls.length,hidden,'hidden panel must not poll');
assert.ok(calls.every(c=>c.path==='/interaction'));
console.log('PASS: interaction UI config/dirty/preview/busy/validation/error/hidden polling flows.');
