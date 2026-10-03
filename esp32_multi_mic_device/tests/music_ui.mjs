import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';

class Node {
  constructor(id='') { this.id=id;this.value='';this.textContent='';this.disabled=false;this.hidden=false;this.files=[];this.children=[];this.attrs={};this.dataset={}; }
  append(...children){this.children.push(...children);}
  replaceChildren(...children){this.children=children;if(this.id==='full-music-upload-track')this.value=children[0]?.value||'';}
  add(child){this.children.push(child);}
  setAttribute(key,value){this.attrs[key]=value;}
  focus(){} remove(){this.removed=true;}
  set innerHTML(_){throw new Error('music metadata must use textContent');}
}
const header=fs.readFileSync(new URL('../web_music_ui.h',import.meta.url),'utf8');
const source=header.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(source);
const nodes=new Map([...header.matchAll(/id='([^']+)'/g)].map(m=>[m[1],new Node(m[1])]));
const actions=['previous','pause','resume','next','stop'].map(action=>{const node=new Node();node.dataset.fullMusicAction=action;return node;});
const panel={hidden:false},calls=[],uploads=[],tracks=[{owner:'ESP32_TEST',id:'one',title:'<img src=x onerror=alert(1)>中文歌',artist:'阎维文',duration_ms:114312,lyrics_available:true}];
const document={getElementById:id=>{assert.ok(nodes.has(id),'unknown UI id '+id);return nodes.get(id);},querySelector:()=>panel,querySelectorAll:()=>actions,createElement:tag=>new Node(tag)};
class Option extends Node {constructor(text,value){super();this.textContent=text;this.value=value;}}
class FormData {constructor(){this.entries=[];}append(...args){this.entries.push(args);}}
let catalogCalls=0;
async function adminFetch(path,options={}){
  calls.push({path,options});
  if(path==='/music/status')return {ok:true,json:async()=>({id:'one',title:tracks[0].title,artist:'阎维文',state:'playing',volume:35,position_ms:6000,duration_ms:114312,lyric:'第一句',next_lyric:'第二句'})};
  if(path==='/music/control')return {ok:true,json:async()=>({})};
  if(path==='/music/upload-ticket'){const data=JSON.parse(options.body);assert.equal(data.origin,'http://192.168.3.247');return {ok:true,json:async()=>({url:'https://voice.example/music/browser-upload',token:'one-use',expires_in:600})};}
  assert.equal(path,'/music/cloud');const body=JSON.parse(options.body);
  if(body.path==='/music/playlists')return {ok:true,json:async()=>({playlists:[{id:'list',title:'最爱',count:1}]})};
  if(body.path.startsWith('/music/playlists/list?'))return {ok:true,json:async()=>({id:'list',title:'最爱',tracks,next_cursor:null})};
  if(body.path.startsWith('/music?')){catalogCalls++;return {ok:true,json:async()=>({tracks,next_cursor:catalogCalls===1?20:null})};}
  return {ok:true,status:204,json:async()=>({})};
}
class XMLHttpRequest {
  constructor(){this.upload={};this.status=201;this.responseText='{}';this.headers={};}
  open(method,url){this.method=method;this.url=url;}
  setRequestHeader(key,value){this.headers[key]=value;}
  send(form){uploads.push({method:this.method,url:this.url,headers:this.headers,form});this.upload.onprogress({lengthComputable:true,loaded:10,total:10});setImmediate(()=>this.onload());}
  abort(){this.onabort?.();}
}
vm.runInNewContext(source,{document,Option,FormData,XMLHttpRequest,adminFetch,window:{location:{origin:'http://192.168.3.247'},addEventListener(){}},setInterval(){},confirm:()=>true,Promise,Error,Math,Number,String});
const flush=async()=>{for(let i=0;i<5;i++)await new Promise(setImmediate);};await flush();
nodes.get('full-music-audio').files=[{name:'random123.mp3',size:100}];nodes.get('full-music-audio').onchange();
assert.equal(nodes.get('full-music-upload-title').value,'','automatic filename overrode server ID3 title');
assert.equal(nodes.get('full-music-title').textContent,tracks[0].title);
assert.equal(nodes.get('full-music-lyric').textContent,'第一句');assert.equal(nodes.get('full-music-next-lyric').textContent,'第二句');
assert.ok(nodes.get('full-music-tracks').children.length===1);assert.equal(nodes.get('full-music-more').hidden,false);
await nodes.get('full-music-more').onclick();assert.ok(calls.some(c=>c.options.body?.includes('cursor=20')));assert.equal(nodes.get('full-music-more').hidden,true);
for(const button of actions)await button.onclick();assert.equal(calls.filter(c=>c.path==='/music/control').length,5);
nodes.get('full-music-query').value='阎维文';await nodes.get('full-music-search-play').onclick();assert.ok(calls.some(c=>c.options.body?.includes('"action":"search"')));
nodes.get('full-music-playlist').value='list';await nodes.get('full-music-playlist').onchange();await nodes.get('full-music-playlist-show').onclick();assert.ok(calls.some(c=>c.options.body?.includes('/music/playlists/list?')));
nodes.get('full-music-audio').files=[{name:'完整超大音乐.mp3',size:100*1024*1024}];nodes.get('full-music-lrc').files=[{name:'歌词.lrc',size:900}];nodes.get('full-music-upload-title').value='强军战歌';await nodes.get('full-music-upload').onclick();
assert.equal(uploads.length,1);assert.equal(uploads[0].headers['X-Music-Upload-Token'],'one-use');assert.equal(uploads[0].headers.Authorization,undefined);
assert.equal(uploads[0].url,'https://voice.example/music/browser-upload');assert.ok(uploads[0].form.entries.some(e=>e[0]==='audio'&&e[1].size===100*1024*1024));assert.ok(uploads[0].form.entries.some(e=>e[0]==='lyrics'));
nodes.get('full-music-upload-track').value='one';nodes.get('full-music-upload-track').onchange();assert.equal(nodes.get('full-music-audio').disabled,true);assert.equal(nodes.get('full-music-upload-title').disabled,true);nodes.get('full-music-audio').files=[{name:'stale.mp3',size:500}];nodes.get('full-music-lrc').files=[{name:'补歌词.lrc',size:900}];await nodes.get('full-music-upload').onclick();
assert.equal(uploads.length,2);assert.ok(uploads[1].form.entries.some(e=>e[0]==='track_id'&&e[1]==='one'));assert.ok(!uploads[1].form.entries.some(e=>e[0]==='audio'));
assert.ok(calls.some(c=>c.path==='/music/upload-ticket'&&JSON.parse(c.options.body).track_id==='one'));
assert.equal(nodes.get('full-music-upload-status').textContent,'上传完成');assert.equal(nodes.get('full-music-upload').disabled,false);
assert.ok(!uploads[1].form.entries.some(e=>e[0]==='title'||e[0]==='artist'));
assert.equal(nodes.get('full-music-audio').disabled,false,'completed upload should return to new-song mode');
assert.ok(header.includes("accept='audio/mpeg,audio/wav,.mp3,.wav'"));
tracks[0].owner='';await nodes.get('full-music-search').onclick();
const sharedButtons=nodes.get('full-music-tracks').children[0].children[1].children.map(n=>n.textContent);
assert.ok(!sharedButtons.includes('删除歌曲')&&!sharedButtons.includes('补歌词'));
assert.equal(nodes.get('full-music-upload-track').children.length,1,'shared song cannot be an LRC upload target');
// Parse every HTML header's embedded scripts; catch malformed production JS.
for(const file of fs.readdirSync(new URL('../',import.meta.url)).filter(f=>f.startsWith('web_')&&f.endsWith('.h'))){const text=fs.readFileSync(new URL('../'+file,import.meta.url),'utf8');for(const m of text.matchAll(/<script>([\s\S]*?)<\/script>/g))new vm.Script(m[1],{filename:file});}
const usageHeader=fs.readFileSync(new URL('../web_usage_ui.h',import.meta.url),'utf8'),usageNodes=new Map([...usageHeader.matchAll(/id='([^']+)'/g)].map(m=>[m[1],new Node(m[1])]));
usageNodes.get('usage-period').value='7d';let usageUrl;
vm.runInNewContext(usageHeader.match(/<script>([\s\S]*?)<\/script>/)[1],{document:{getElementById:id=>usageNodes.get(id),createElement:tag=>new Node(tag),createElementNS:(_,tag)=>new Node(tag)},adminFetch:async url=>{usageUrl=url;return {ok:true,json:async()=>({cost_chart:{grain:'day',rows:[{date:'2026-10-02',costCNY:'0.02',calls:1}]},expense_rows:[],totals:{total:'0.02'},updated_at:'2026-10-02T00:00:00Z'})};},Date,Math,Number,Object});
await usageNodes.get('usage-load').onclick();assert.equal(usageUrl,'/ai/usage?period=7d');assert.ok(usageNodes.get('usage-grain').textContent.includes('日'));assert.ok(usageNodes.get('usage-chart').children.some(n=>n.id==='circle'));
assert.equal(usageNodes.get('usage-grain').onchange,undefined);assert.ok(!usageHeader.includes("<select id='usage-grain'"));
console.log('PASS: production music web XHR/direct 100 MiB upload/LRC attach/safe metadata/queue pagination/actions and automatic cost_chart checks');
