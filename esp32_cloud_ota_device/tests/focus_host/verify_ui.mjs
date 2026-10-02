import fs from 'node:fs';
import vm from 'node:vm';
const html = fs.readFileSync(process.argv[2], 'utf8');
const source = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(match => match[1]).join('\n');
new vm.Script(source);
const nodes = new Map();
for (const match of html.matchAll(/id='([^']+)'/g)) nodes.set(match[1], {id: match[1], textContent: '', disabled: false, value: ''});
const buttons = [...html.matchAll(/data-focus-action='([^']+)'/g)].map(match => ({dataset: {focusAction: match[1]}, disabled: true}));
const byAction = action => buttons.find(button => button.dataset.focusAction === action);
nodes.get('focus-config').reportValidity = () => true;
nodes.get('focus-controls').closest = () => ({hidden:true});
const events = new Map();
let navClick;
const appWindow = {activeAdminPanel:'home',addEventListener:(type,fn)=>events.set(type,fn)};
const appLocation = {hash:''};
let now = 0, interval, failure = '', malformed = false;
let checks = 0;
const calls = [];
let device = {initialized:true,phase:'idle',status:'idle',remaining_seconds:0,phase_total_seconds:0,
  completed_sessions:0,cycle_sessions:0,storage_error:false,focus_minutes:25,short_break_minutes:5,long_break_minutes:15,rounds_before_long_break:4};
function require(value, message) { ++checks; if (!value) throw Error(message); }
const context = vm.createContext({
  document: {hidden:false,getElementById:id=>nodes.get(id),querySelectorAll:()=>buttons,
    querySelector:()=>({addEventListener:(_,fn)=>{navClick=fn;}})},window:appWindow,location:appLocation,
  performance: {now:()=>now}, Date:{now:()=>now}, Number, String, Math, Object, Array, Error,
  setInterval: fn => {interval=fn;},
  adminFetch: async (url, options) => {
    calls.push({url,...options});
    if (failure === options.method) return {ok:false,status:503,text:async()=>JSON.stringify({ok:false,message:'模拟存储或网络失败'})};
    if (failure === 'plain') return {ok:false,status:409,text:async()=>'请先结束当前对话或声音录入。'};
    if (options.method==='POST') {
      const body=JSON.parse(options.body);
      if (body.action==='start'||body.action==='configure') Object.assign(device,{focus_minutes:body.focus_minutes,
        short_break_minutes:body.short_break_minutes,long_break_minutes:body.long_break_minutes,rounds_before_long_break:body.rounds_before_long_break});
      if (body.action==='start') Object.assign(device,{phase:'focus',status:'running',remaining_seconds:device.focus_minutes*60,phase_total_seconds:device.focus_minutes*60});
      if (body.action==='pause') device.status='paused';
      if (body.action==='resume') device.status='running';
      if (body.action==='stop') Object.assign(device,{phase:'idle',status:'idle',remaining_seconds:0,phase_total_seconds:0});
    }
    return {ok:true,status:200,text:async()=>JSON.stringify({...device,remaining_seconds:malformed?'bad':device.remaining_seconds,message:'已保存',ok:true})};
  }
});
async function flush() { for(let n=0;n<6;++n) await new Promise(resolve=>setImmediate(resolve)); }
async function tick(amount) {now+=amount;interval();await flush();}
vm.runInContext(source,context);
await flush();
require(calls.length===0,'hidden focus panel must not request state on initial home');
await tick(10000);
require(calls.length===0,'hidden focus panel must not poll');
appWindow.activeAdminPanel='focus';
navClick();
await flush();
require(calls.length===1&&calls[0].url==='/focus'&&calls[0].method==='GET','initial authenticated focus request');
require(nodes.get('focus-status').textContent==='待开始'&&nodes.get('focus-countdown').textContent==='0:00','idle state loaded');
require(!byAction('start').disabled&&byAction('pause').disabled&&byAction('resume').disabled,'idle button availability');
require(nodes.get('focus-minutes').value===25&&!nodes.get('focus-fields').disabled,'configuration populated');
await byAction('start').onclick();
require(JSON.parse(calls.at(-1).body).action==='start'&&JSON.parse(calls.at(-1).body).focus_minutes===25,'start sends configuration');
require(byAction('start').disabled&&!byAction('pause').disabled&&nodes.get('focus-countdown').textContent==='25:00','start renders running timer');
await tick(2000);
require(nodes.get('focus-countdown').textContent==='24:58','local second countdown');
device.remaining_seconds=1498;
await byAction('pause').onclick();
require(byAction('pause').disabled&&!byAction('resume').disabled&&nodes.get('focus-status').textContent.includes('已暂停'),'pause button/state');
await tick(2000);
require(nodes.get('focus-countdown').textContent==='24:58','paused display freezes');
await byAction('resume').onclick();
require(!byAction('pause').disabled&&byAction('resume').disabled,'resume buttons');
nodes.get('focus-minutes').value='60';
nodes.get('focus-config').oninput();
await tick(6000);
require(nodes.get('focus-minutes').value==='60','background poll must not discard unsaved input');
await nodes.get('focus-config').onsubmit({preventDefault(){}});
await flush();
require(device.focus_minutes===60&&device.remaining_seconds===1498,'configure does not reset active countdown');
require(nodes.get('focus-message').textContent==='已保存','configure success visible');
failure='POST';
await byAction('pause').onclick();
require(nodes.get('focus-message').textContent.includes('操作失败')&&device.status==='running','failed write visible without fabricated pause');
require(!nodes.get('focus-fields').disabled&&!byAction('pause').disabled,'failed operation releases busy controls');
failure='plain';
await byAction('pause').onclick();
require(nodes.get('focus-message').textContent.includes('请先结束当前对话'),'plain text firmware rejection visible');
failure='GET';
await nodes.get('focus-refresh').onclick();
require(nodes.get('focus-message').textContent.includes('读取失败'),'failed read visible');
failure='';
malformed=true;
await nodes.get('focus-refresh').onclick();
require(nodes.get('focus-message').textContent.includes('倒计时无效'),'malformed countdown rejected');
malformed=false;
device.remaining_seconds=1;
await nodes.get('focus-refresh').onclick();
await tick(4000);
require(nodes.get('focus-countdown').textContent==='0:00','countdown clamps at zero');
await byAction('stop').onclick();
require(!byAction('start').disabled&&byAction('stop').disabled&&nodes.get('focus-status').textContent==='待开始','stop renders idle');
appWindow.activeAdminPanel='home';
let callCount=calls.length;
await tick(20000);
require(calls.length===callCount,'leaving focus panel suspends network polling');
appLocation.hash='#focus';
events.get('hashchange')();
await flush();
require(calls.length===callCount+1,'hash navigation reads focus immediately');
appWindow.activeAdminPanel='focus';
context.document.hidden=true;
callCount=calls.length;
await tick(20000);
require(calls.length===callCount,'background document suspends network polling');
console.log(`${checks} focus UI DOM checks passed`);
