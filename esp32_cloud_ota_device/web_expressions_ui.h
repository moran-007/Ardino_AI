#pragma once
#include <Arduino.h>

static const char EXPRESSION_SETTINGS_PAGE[] PROGMEM=R"HTML(
<section class="panel" data-panel="expressions" hidden>
<h2>表情库</h2><p class="intro">8 类、48 组动态表情。天气主页和待机表情页共用你的选择。</p>
<div class="card">
<div class="row"><div><label for="expression-category">分类</label><select id="expression-category"><option value="all">全部分类</option></select></div>
<div><label for="expression-search">搜索名称或动作</label><input id="expression-search" type="search" placeholder="例如：眨眼、托腮、挥手"></div></div>
<label for="expression-style">缩略图样式</label><select id="expression-style"><option value="eyes">发光表情（横竖屏通用）</option><option value="body">全身角色（竖屏可选）</option></select>
<p class="meta">勾选要随机出现的表情。缩略图展示静态一帧；“屏幕预览”按设备当前方向和造型展示 8 秒，然后返回原页面。竖屏造型在“屏幕与相册”设置。</p>
<div class="actions"><button class="secondary" type="button" id="expression-enable-group">启用当前筛选</button>
<button class="secondary" type="button" id="expression-disable-group">停用当前筛选</button>
<button class="secondary" type="button" id="expression-enable-all">全部启用</button>
<button class="secondary" type="button" id="expression-reset">撤销未保存修改</button></div>
<p id="expression-count" role="status">正在读取表情库…</p><p id="expression-result" role="status"></p>
<div id="expression-grid" class="expression-grid"></div>
<div class="actions"><button type="button" id="expression-save">保存表情选择</button>
<button type="button" class="secondary" id="expression-stop">结束屏幕预览</button>
<button type="button" class="secondary" id="expression-retry">重新读取表情库</button></div>
<p class="meta">至少启用一组；只选一组时固定表情，动作继续播放。设备状态的聆听、思考、说话、闹钟和错误提示会优先显示。</p>
</div></section>
<style>
.expression-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(145px,1fr));gap:12px;margin:16px 0}
.expression-card{border:1px solid var(--line);border-radius:10px;padding:10px;background:#fff;min-width:0}
.expression-card[hidden]{display:none}.expression-card img{display:block;width:96px;height:96px;border-radius:8px;margin:0 auto 8px;image-rendering:pixelated;background:#081018}
.expression-card label{display:flex;align-items:center;gap:6px;margin:6px 0}.expression-card input{width:auto;margin:0;flex-shrink:0}.expression-card small{display:block;color:var(--muted);margin-bottom:8px}.expression-card button{width:100%;font-size:13px;padding:8px 4px}.expression-card[data-enabled="false"]{background:#f4f6f5}.expression-card[data-enabled="false"] img{opacity:.5}
</style>
<script>
let expressionItems=[],expressionGroups=[],expressionSaved=new Set(),expressionSelected=new Set();
let expressionLoaded=false,expressionLoading=false,expressionSaving=false;
const expressionResult=document.getElementById('expression-result');
function expressionMessage(text,error=false){expressionResult.textContent=text;expressionResult.className=error?'warn':'ok';}
function expressionChanged(){return expressionSaved.size!==expressionSelected.size||[...expressionSelected].some(id=>!expressionSaved.has(id));}
function updateExpressionCount(){
  document.getElementById('expression-count').textContent=!expressionLoaded?'正在读取表情库…':`已启用 ${expressionSelected.size} / ${expressionItems.length} 组`+(expressionChanged()?' · 有未保存修改':' · 已保存');
  document.getElementById('expression-save').disabled=!expressionLoaded||expressionLoading||expressionSaving;
  document.getElementById('expression-retry').disabled=expressionLoading||expressionSaving;
}
function expressionMatches(item){
  const category=document.getElementById('expression-category').value;
  const query=document.getElementById('expression-search').value.trim();
  return (category==='all'||item.category===Number(category))&&(!query||(item.name+' '+item.gesture).includes(query));
}
function filterExpressions(){document.querySelectorAll('[data-expression]').forEach(card=>card.hidden=!expressionMatches(expressionItems[Number(card.dataset.expression)]));}
function syncExpressionCards(){document.querySelectorAll('[data-expression]').forEach(card=>{
  const enabled=expressionSelected.has(Number(card.dataset.expression));card.dataset.enabled=String(enabled);card.querySelector('input').checked=enabled;
});updateExpressionCount();}
function expressionImageUrl(id){return `/display/expressions/image?id=${id}&body=${document.getElementById('expression-style').value==='body'?'1':'0'}&v=2`;}
async function loadExpressions(force=false){
  if(expressionLoading||expressionSaving||(expressionLoaded&&!force))return;
  if(force&&expressionChanged()&&!confirm('重新读取会丢弃未保存的表情选择，继续吗？'))return;
  expressionLoading=true;updateExpressionCount();
  try{
    const response=await fetch('/display/expressions',{cache:'no-store'});
    if(!response.ok)throw new Error(response.status===401?'请先登录后台。':await response.text());
    const data=await response.json();expressionItems=data.items;expressionGroups=data.categories;
    expressionSaved=new Set(data.items.filter(item=>item.enabled).map(item=>item.id));expressionSelected=new Set(expressionSaved);
    const categories=document.getElementById('expression-category');categories.replaceChildren(new Option('全部分类','all'));
    expressionGroups.forEach((name,id)=>categories.add(new Option(name,String(id))));
    const grid=document.getElementById('expression-grid');grid.replaceChildren();
    for(const item of expressionItems){
      const card=document.createElement('div');card.className='expression-card';card.dataset.expression=item.id;
      const picture=document.createElement('img');picture.loading='lazy';picture.width=96;picture.height=96;picture.alt=item.name;picture.src=expressionImageUrl(item.id);
      const label=document.createElement('label'),check=document.createElement('input');check.type='checkbox';check.checked=expressionSelected.has(item.id);
      check.addEventListener('change',()=>{if(check.checked)expressionSelected.add(item.id);else expressionSelected.delete(item.id);syncExpressionCards();});
      label.append(check,document.createTextNode(item.name));
      const detail=document.createElement('small');detail.textContent=expressionGroups[item.category]+' · '+item.gesture;
      const preview=document.createElement('button');preview.type='button';preview.className='secondary';preview.textContent='屏幕预览';
      preview.addEventListener('click',()=>previewExpressionOnDevice(item.id,preview));
      card.append(picture,label,detail,preview);grid.append(card);
    }
    expressionLoaded=true;syncExpressionCards();filterExpressions();expressionMessage('已读取已保存的表情选择。');
  }catch(error){expressionMessage(error.message||'表情库读取失败，请重新读取。',true);}
  finally{expressionLoading=false;updateExpressionCount();}
}
async function expressionPost(path,fields){
  const response=await fetch(path,{method:'POST',body:new URLSearchParams(fields)});
  if(!response.ok)throw new Error(response.status===401?'登录已失效，请重新登录。':await response.text());
  return response;
}
async function previewExpressionOnDevice(id,button){
  button.disabled=true;
  try{await expressionPost('/display/expressions/preview',{id:String(id)});expressionMessage('正在屏幕上预览“'+expressionItems[id].name+'”，8 秒后返回。');}
  catch(error){expressionMessage(error.message,true);}finally{button.disabled=false;}
}
document.getElementById('expression-category').addEventListener('change',filterExpressions);
document.getElementById('expression-search').addEventListener('input',filterExpressions);
document.getElementById('expression-style').addEventListener('change',()=>document.querySelectorAll('[data-expression] img').forEach(img=>img.src=expressionImageUrl(Number(img.parentElement.dataset.expression))));
for(const enabled of [true,false])document.getElementById(enabled?'expression-enable-group':'expression-disable-group').addEventListener('click',()=>{
  expressionItems.filter(expressionMatches).forEach(item=>{if(enabled)expressionSelected.add(item.id);else expressionSelected.delete(item.id);});syncExpressionCards();
});
document.getElementById('expression-enable-all').addEventListener('click',()=>{expressionSelected=new Set(expressionItems.map(item=>item.id));syncExpressionCards();});
document.getElementById('expression-reset').addEventListener('click',()=>{expressionSelected=new Set(expressionSaved);syncExpressionCards();expressionMessage('未保存修改已撤销。');});
document.getElementById('expression-retry').addEventListener('click',()=>loadExpressions(true));
document.getElementById('expression-save').addEventListener('click',async()=>{
  if(expressionSaving||expressionLoading||!expressionLoaded)return;
  if(!expressionSelected.size){expressionMessage('请至少启用一组表情。',true);return;}
  expressionSaving=true;updateExpressionCount();
  // Save the submitted set, not changes made while the request is in flight.
  const submitted=new Set(expressionSelected);
  try{await expressionPost('/display/expressions',{ids:[...submitted].sort((a,b)=>a-b).join(',')});expressionSaved=submitted;expressionMessage('表情选择已保存，天气主页和待机表情页已应用。');}
  catch(error){expressionMessage(error.message,true);}finally{expressionSaving=false;updateExpressionCount();}
});
document.getElementById('expression-stop').addEventListener('click',async()=>{
  try{await expressionPost('/display/expressions/preview',{id:'stop'});expressionMessage('屏幕预览已结束。');}catch(error){expressionMessage(error.message,true);}
});
</script>
)HTML";
