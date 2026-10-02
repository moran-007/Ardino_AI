#pragma once
#include <Arduino.h>

static const char PHOTO_SETTINGS_PAGE[] PROGMEM = R"HTML(
<fieldset id='photo-settings'><legend>我的相册</legend>
<p id='photo-summary'>正在读取相册…</p>
<div class='actions'><button type='button' data-photo-action onclick="selectStandbyPhoto('previous')">上一张</button><button type='button' data-photo-action onclick="selectStandbyPhoto('next')">下一张</button><button type='button' data-photo-action onclick='refreshStandbyPhotoStatus()'>刷新相册</button></div>
<div id='photo-list' class='photo-grid'></div>
<p class='hint'>说“打开相册”“下一张图片”“上一张图片”“显示第三张图片”或“退出相册”。编号与下方卡片一致，删除后其余图片不改号；语音识别需要联网。</p>
</fieldset>
<fieldset><legend>添加图片</legend>
<p>最多保存 8 张，可一次选择多张。逐张点击文件名，拖动和缩放预览调整裁剪；相册满时不会覆盖旧图。</p>
<form id='photo-form' onsubmit='uploadStandbyPhoto(event)'>
<label for='photo-file'>选择图片（每张最大 8 MB、2400 万像素）</label>
<input id='photo-file' type='file' accept='image/jpeg,image/png,image/webp' multiple onchange='previewStandbyPhoto()'>
<div id='photo-queue' class='actions'></div>
<button id='photo-upload' type='submit' disabled>上传所选图片</button>
</form>
<canvas id='photo-preview' tabindex='0' style='display:none;max-width:100%;height:auto;margin-top:12px;touch-action:none' aria-label='图片裁剪预览，可拖动或使用方向键调整位置'></canvas>
<div id='photo-crop' hidden>
<label for='photo-zoom'>缩放 <output id='photo-zoom-value'>100%</output></label>
<input id='photo-zoom' data-photo-crop type='range' min='1' max='4' step='0.01' value='1' oninput='zoomStandbyPhoto(this.value)' aria-label='裁剪缩放'>
<button type='button' data-photo-crop onclick='resetStandbyPhotoCrop()'>居中重置</button>
<p class='hint'>拖动画面选择保留区域；方向键可微调。预览框即屏幕显示范围，每张图片单独保留裁剪设置。上传后的图片无法恢复已裁掉的部分，请保留原图。</p>
</div>
<p id='photo-result' role='status'></p>
<p class='hint'>点击相册中的“显示”可固定看图；在“屏幕显示”启用照片页可随其他待机页轮播。照片覆盖顶部 IP，其他页面保留 IP。</p>
</fieldset>
<script>
let standbyPhotoBusy=false,standbyPhotoPreviewBusy=false,standbyPhotoGeneration=0;
let standbyPhotoPrepared=null,standbyPhotoFiles=[];
let standbyPhotoEditor=null,standbyPhotoCrops=new WeakMap(),standbyPhotoCropSize='',standbyPhotoDrag=null;
async function photoFetch(url,options={}){
  const response=await fetch(url,{cache:'no-store',credentials:'same-origin',...options});
  if(response.status===401){location.replace('/login');throw new Error('请重新登录后台');}
  if(!response.ok)throw new Error(await response.text());
  return response;
}
function photoUpdateControls(){
  document.getElementById('photo-file').disabled=standbyPhotoBusy;
  document.getElementById('photo-upload').disabled=standbyPhotoBusy||standbyPhotoPreviewBusy||!standbyPhotoPrepared;
  for(const button of document.querySelectorAll('[data-photo-action]'))button.disabled=standbyPhotoBusy||standbyPhotoPreviewBusy;
  for(const control of document.querySelectorAll('[data-photo-crop]'))control.disabled=standbyPhotoBusy||standbyPhotoPreviewBusy||!standbyPhotoEditor;
}
function photoSetBusy(busy){standbyPhotoBusy=busy;photoUpdateControls();}
function photoFeedback(message){
  document.getElementById('photo-result').textContent=message;
  const home=document.getElementById('home-action');if(home)home.textContent=message;
}
async function photoGetStatus(){
  const state=await(await photoFetch('/display/photo/status')).json();
  if(!state.ready)throw new Error('相册存储未就绪');
  if(!photoDimensionsSupported(state.width,state.height))throw new Error('不支持当前屏幕尺寸');
  return state;
}
function photoButton(label,action){const b=document.createElement('button');b.type='button';b.textContent=label;b.dataset.photoAction='';b.onclick=action;return b;}
function renderPhotoList(state){
  const list=document.getElementById('photo-list');list.replaceChildren();
  document.getElementById('photo-summary').textContent='已保存 '+state.count+' / '+state.capacity+' 张'+(state.current?' · 当前 #'+state.current:'')+' · 全屏 '+state.width+' × '+state.height;
  for(const item of state.items){
    const card=document.createElement('article');card.className='photo-card'+(item.id===state.current?' selected':'');
    const title=document.createElement('strong');title.textContent='#'+item.id+' 图片'+(item.id===state.current?' · 当前':'');
    const meta=document.createElement('p');meta.className='meta';meta.textContent=item.width+' × '+item.height+' · '+Math.ceil(item.bytes/1024)+' KB';
    const actions=document.createElement('div');actions.className='actions';
    actions.append(photoButton('显示',()=>selectStandbyPhoto(item.id)),photoButton('预览',()=>previewSavedPhoto(item.id)));
    const remove=photoButton('删除',()=>deleteStandbyPhoto(item.id));remove.className='danger';actions.append(remove);
    card.append(title,meta,actions);
    if(item.needs_reupload){const hint=document.createElement('p');hint.className='hint';hint.textContent='旧图片可能带留边，可删除后重新上传原图。';card.append(hint);}
    list.append(card);
  }
  if(!state.count)list.textContent='相册为空，先在下方添加图片。';
  photoUpdateControls();
}
async function refreshStandbyPhotoStatus(){
  try{renderPhotoList(await photoGetStatus());}catch(e){document.getElementById('photo-summary').textContent=e.message;}
}
function photoDimensionsSupported(width,height){
  return Number.isInteger(width)&&Number.isInteger(height)&&
    ((width===320&&height===240)||(width===160&&height===128)||
     (width===240&&height===320)||(width===128&&height===160));
}
function photoCoverRect(sourceWidth,sourceHeight,width,height,crop={zoom:1,cx:0.5,cy:0.5}){
  const zoom=Math.max(1,Math.min(4,Number(crop.zoom)||1));
  const inverseScale=Math.min(sourceWidth/width,sourceHeight/height)/zoom;
  const cropWidth=width*inverseScale,cropHeight=height*inverseScale;
  const cx=Number.isFinite(crop.cx)?crop.cx:0.5,cy=Number.isFinite(crop.cy)?crop.cy:0.5;
  const x=Math.max(0,Math.min(sourceWidth-cropWidth,cx*sourceWidth-cropWidth/2));
  const y=Math.max(0,Math.min(sourceHeight-cropHeight,cy*sourceHeight-cropHeight/2));
  return {x,y,width:cropWidth,height:cropHeight,cx:(x+cropWidth/2)/sourceWidth,cy:(y+cropHeight/2)/sourceHeight};
}
function encodePhotoRgb565(rgba,width,height){
  if(!photoDimensionsSupported(width,height))throw new Error('图片尺寸不支持');
  if(rgba.length!==width*height*4)throw new Error('图片像素不完整');
  const bytes=new Uint8Array(8+width*height*2);
  bytes.set([82,53,54,53,width&255,width>>8,height&255,height>>8]);
  for(let pixel=0,offset=8;pixel<rgba.length;pixel+=4,offset+=2){
    const color=((rgba[pixel]>>3)<<11)|((rgba[pixel+1]>>2)<<5)|(rgba[pixel+2]>>3);
    bytes[offset]=color&255;bytes[offset+1]=color>>8;
  }
  return bytes;
}
async function loadPhotoImage(file){
  if(!file||!['image/jpeg','image/png','image/webp'].includes(file.type))throw new Error('请选择 JPG、PNG 或 WebP 图片');
  if(file.size>8*1024*1024)throw new Error('图片超过 8 MB，请先缩小后上传');
  const url=URL.createObjectURL(file),image=new Image();
  try{
    await new Promise((resolve,reject)=>{image.onload=resolve;image.onerror=()=>reject(new Error('无法读取此图片，请换一张重试'));image.src=url;});
    if(!image.naturalWidth||!image.naturalHeight||image.naturalWidth*image.naturalHeight>24000000)throw new Error('图片超过 2400 万像素，请先缩小');
    return image;
  }catch(e){image.src='';throw e;}finally{URL.revokeObjectURL(url);}
}
function photoCropCanvas(image,width,height,crop){
    const canvas=document.createElement('canvas');canvas.width=width;canvas.height=height;
    const context=canvas.getContext('2d',{alpha:false});
    if(!context)throw new Error('浏览器不支持图片转换，请换用新版浏览器');
    context.fillStyle='#080c08';context.fillRect(0,0,width,height);
    context.imageSmoothingEnabled=true;context.imageSmoothingQuality='high';
    const rect=photoCoverRect(image.naturalWidth,image.naturalHeight,width,height,crop);
    context.drawImage(image,rect.x,rect.y,rect.width,rect.height,0,0,width,height);
    return canvas;
}
function photoCanvasBlob(canvas){
  return new Blob([encodePhotoRgb565(canvas.getContext('2d').getImageData(0,0,canvas.width,canvas.height).data,canvas.width,canvas.height)],{type:'application/octet-stream'});
}
async function prepareStandbyPhoto(file,width,height){
  const image=await loadPhotoImage(file);
  try{const canvas=photoCropCanvas(image,width,height,standbyPhotoCrops.get(file));return {canvas,blob:photoCanvasBlob(canvas)};}
  finally{image.src='';}
}
function releasePhotoEditor(){
  if(standbyPhotoEditor)standbyPhotoEditor.image.src='';
  standbyPhotoEditor=null;standbyPhotoDrag=null;
  document.getElementById('photo-crop').hidden=true;
  document.getElementById('photo-preview').style.cursor='';
}
function updatePhotoCrop(){
  const e=standbyPhotoEditor;if(!e)return;
  const rect=photoCoverRect(e.image.naturalWidth,e.image.naturalHeight,e.width,e.height,e.crop);
  e.crop.cx=rect.cx;e.crop.cy=rect.cy;standbyPhotoCrops.set(e.file,{...e.crop});
  const canvas=photoCropCanvas(e.image,e.width,e.height,e.crop);paintPhotoPreview(canvas);
  standbyPhotoPrepared={file:e.file,width:e.width,height:e.height,canvas};
  document.getElementById('photo-zoom').value=e.crop.zoom;
  document.getElementById('photo-zoom-value').textContent=Math.round(e.crop.zoom*100)+'%';
}
function zoomStandbyPhoto(value){
  if(standbyPhotoBusy||standbyPhotoPreviewBusy||!standbyPhotoEditor)return;
  standbyPhotoEditor.crop.zoom=Math.max(1,Math.min(4,Number(value)||1));updatePhotoCrop();
}
function resetStandbyPhotoCrop(){
  if(standbyPhotoBusy||standbyPhotoPreviewBusy||!standbyPhotoEditor)return;
  standbyPhotoEditor.crop={zoom:1,cx:0.5,cy:0.5};updatePhotoCrop();
}
function movePhotoCrop(dx,dy){
  const e=standbyPhotoEditor;if(!e)return;
  const view=document.getElementById('photo-preview').getBoundingClientRect();
  if(!view.width||!view.height)return;
  const rect=photoCoverRect(e.image.naturalWidth,e.image.naturalHeight,e.width,e.height,e.crop);
  e.crop.cx-=dx/view.width*rect.width/e.image.naturalWidth;
  e.crop.cy-=dy/view.height*rect.height/e.image.naturalHeight;updatePhotoCrop();
}
function paintPhotoPreview(canvas){
  const preview=document.getElementById('photo-preview');preview.width=canvas.width;preview.height=canvas.height;
  preview.getContext('2d',{alpha:false}).drawImage(canvas,0,0);preview.style.display='block';
}
async function prepareSelectedPreview(index,generation){
  releasePhotoEditor();
  standbyPhotoPrepared=null;standbyPhotoPreviewBusy=true;photoUpdateControls();
  let image=null;
  try{
    const file=standbyPhotoFiles[index];if(!file)return;
    document.getElementById('photo-result').textContent='正在预览 '+file.name+'…';
    const state=await photoGetStatus();if(generation!==standbyPhotoGeneration)return;
    image=await loadPhotoImage(file);if(generation!==standbyPhotoGeneration)return;
    const size=state.width+'x'+state.height;
    if(standbyPhotoCropSize!==size){standbyPhotoCrops=new WeakMap();standbyPhotoCropSize=size;}
    standbyPhotoEditor={file,image,width:state.width,height:state.height,crop:{...(standbyPhotoCrops.get(file)||{zoom:1,cx:0.5,cy:0.5})}};
    image=null;updatePhotoCrop();document.getElementById('photo-crop').hidden=false;
    document.getElementById('photo-preview').style.cursor='grab';
    document.getElementById('photo-result').textContent='正在裁剪：'+file.name+'；共选择 '+standbyPhotoFiles.length+' 张，可逐张调整后上传。';
  }catch(e){if(generation===standbyPhotoGeneration){document.getElementById('photo-preview').style.display='none';document.getElementById('photo-result').textContent=e.message;}}
  finally{if(image)image.src='';if(generation===standbyPhotoGeneration){standbyPhotoPreviewBusy=false;photoUpdateControls();}}
}
function renderPhotoQueue(){
  const box=document.getElementById('photo-queue');box.replaceChildren();
  standbyPhotoFiles.forEach((file,index)=>box.append(photoButton((index+1)+'. '+file.name,()=>{
    if(!standbyPhotoBusy)prepareSelectedPreview(index,++standbyPhotoGeneration);
  })));
}
async function previewStandbyPhoto(){
  if(standbyPhotoBusy)return;
  const generation=++standbyPhotoGeneration;standbyPhotoPrepared=null;
  releasePhotoEditor();standbyPhotoCrops=new WeakMap();
  standbyPhotoFiles=Array.from(document.getElementById('photo-file').files);
  document.getElementById('photo-preview').style.display='none';
  if(standbyPhotoFiles.length>8){standbyPhotoFiles=[];document.getElementById('photo-file').value='';document.getElementById('photo-result').textContent='一次最多选择8张图片。';}
  renderPhotoQueue();
  if(!standbyPhotoFiles.length){standbyPhotoPreviewBusy=false;photoUpdateControls();return;}
  await prepareSelectedPreview(0,generation);
}
async function uploadStandbyPhoto(event){
  event.preventDefault();if(standbyPhotoBusy||standbyPhotoPreviewBusy||!standbyPhotoPrepared)return;
  const files=[...standbyPhotoFiles],reviewed=standbyPhotoPrepared,result=document.getElementById('photo-result');
  photoSetBusy(true);let saved=0;
  try{
    const state=await photoGetStatus();
    if(state.width!==reviewed.width||state.height!==reviewed.height){standbyPhotoPrepared=null;throw new Error('屏幕尺寸已变化，请重新选择图片预览。');}
    if(files.length>state.capacity-state.count)throw new Error('剩余 '+(state.capacity-state.count)+' 个位置，请减少选择或先删除旧图。');
    for(const file of files){
      result.textContent='正在上传 '+(saved+1)+' / '+files.length+'：'+file.name;
      const prepared=file===reviewed.file?reviewed:await prepareStandbyPhoto(file,state.width,state.height);
      const form=new FormData();form.append('photo',prepared.blob||photoCanvasBlob(prepared.canvas),'standby.r565');
      await photoFetch('/display/photo',{method:'POST',body:form});saved++;
    }
    result.textContent='已添加 '+saved+' 张图片，可点击相册中的“显示”或语音切换。';
  }catch(e){result.textContent=(saved?'已保存 '+saved+' 张；剩余未上传。':'')+e.message;}
  finally{
    if(saved){standbyPhotoFiles=files.slice(saved);standbyPhotoPrepared=null;releasePhotoEditor();document.getElementById('photo-file').value='';renderPhotoQueue();}
    photoSetBusy(false);await refreshStandbyPhotoStatus();
  }
}
async function selectStandbyPhoto(id){
  if(standbyPhotoBusy||standbyPhotoPreviewBusy)return;photoSetBusy(true);
  try{
    const body=new URLSearchParams(typeof id==='number'?{id:String(id)}:{action:id});
    await photoFetch('/display/photo/select',{method:'POST',body});
    photoFeedback('屏幕已切换图片。');await refreshStandbyPhotoStatus();
  }catch(e){photoFeedback(e.message);}finally{photoSetBusy(false);}
}
async function deleteStandbyPhoto(id){
  if(standbyPhotoBusy||standbyPhotoPreviewBusy||!confirm('删除相册中的 #'+id+' 图片？'))return;photoSetBusy(true);
  try{
    await photoFetch('/display/photo/delete',{method:'POST',body:new URLSearchParams({id:String(id)})});
    document.getElementById('photo-result').textContent='已删除 #'+id+'。';await refreshStandbyPhotoStatus();
  }catch(e){document.getElementById('photo-result').textContent=e.message;}finally{photoSetBusy(false);}
}
async function previewSavedPhoto(id){
  if(standbyPhotoBusy||standbyPhotoPreviewBusy)return;
  standbyPhotoPrepared=null;++standbyPhotoGeneration;photoSetBusy(true);
  releasePhotoEditor();
  try{
    const state=await photoGetStatus();
    const buffer=await(await photoFetch('/display/photo/image?id='+id)).arrayBuffer();
    if(buffer.byteLength<8||buffer.byteLength>153608)throw new Error('图片长度无效');
    const data=new DataView(buffer),w=data.getUint16(4,true),h=data.getUint16(6,true);
    const legacy=(w===320&&h===212)||(w===240&&h===292)||(w===160&&h===110)||(w===128&&h===142);
    if(data.getUint32(0,false)!==0x52353635||(!photoDimensionsSupported(w,h)&&!legacy)||buffer.byteLength!==8+w*h*2)throw new Error('图片格式无效');
    const raw=document.createElement('canvas');raw.width=w;raw.height=h;const rc=raw.getContext('2d');const pixels=rc.createImageData(w,h);
    for(let i=0;i<w*h;i++){const c=data.getUint16(8+i*2,true);pixels.data[i*4]=Math.round((c>>11)*255/31);pixels.data[i*4+1]=Math.round(((c>>5)&63)*255/63);pixels.data[i*4+2]=Math.round((c&31)*255/31);pixels.data[i*4+3]=255;}
    rc.putImageData(pixels,0,0);const target=document.createElement('canvas');target.width=state.width;target.height=state.height;
    const crop=photoCoverRect(w,h,state.width,state.height);target.getContext('2d').drawImage(raw,crop.x,crop.y,crop.width,crop.height,0,0,state.width,state.height);
    paintPhotoPreview(target);document.getElementById('photo-result').textContent='已保存图片 #'+id+' 的全屏裁剪预览。';
  }catch(e){document.getElementById('photo-result').textContent=e.message;}finally{photoSetBusy(false);}
}
const photoPreviewControl=document.getElementById('photo-preview');
photoPreviewControl.addEventListener('pointerdown',event=>{
  if(standbyPhotoBusy||standbyPhotoPreviewBusy||!standbyPhotoEditor||event.button!==0)return;
  event.preventDefault();photoPreviewControl.focus();photoPreviewControl.setPointerCapture(event.pointerId);
  standbyPhotoDrag={id:event.pointerId,x:event.clientX,y:event.clientY};photoPreviewControl.style.cursor='grabbing';
});
photoPreviewControl.addEventListener('pointermove',event=>{
  if(!standbyPhotoDrag||event.pointerId!==standbyPhotoDrag.id||standbyPhotoBusy)return;
  movePhotoCrop(event.clientX-standbyPhotoDrag.x,event.clientY-standbyPhotoDrag.y);
  standbyPhotoDrag.x=event.clientX;standbyPhotoDrag.y=event.clientY;
});
for(const name of ['pointerup','pointercancel','lostpointercapture'])photoPreviewControl.addEventListener(name,()=>{standbyPhotoDrag=null;photoPreviewControl.style.cursor=standbyPhotoEditor?'grab':'';});
photoPreviewControl.addEventListener('keydown',event=>{
  if(standbyPhotoBusy||standbyPhotoPreviewBusy||!standbyPhotoEditor)return;
  const moves={ArrowLeft:[-1,0],ArrowRight:[1,0],ArrowUp:[0,-1],ArrowDown:[0,1]},step=event.shiftKey?20:4;
  if(moves[event.key]){event.preventDefault();movePhotoCrop(moves[event.key][0]*step,moves[event.key][1]*step);}
});
window.addEventListener('pagehide',releasePhotoEditor);
refreshStandbyPhotoStatus();
</script>
)HTML";
