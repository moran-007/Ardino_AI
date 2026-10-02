async page => {
  const check=(v,m)=>{if(!v)throw new Error(m);};
  const signature=()=>page.evaluate(async()=>{
    const bytes=await photoCanvasBlob(standbyPhotoPrepared.canvas).arrayBuffer();
    return Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)),b=>b.toString(16).padStart(2,'0')).join('');
  });
  const original=await signature();
  await page.getByRole('slider',{name:'裁剪缩放'}).fill('2');
  const canvas=page.getByLabel('图片裁剪预览，可拖动或使用方向键调整位置');
  await canvas.scrollIntoViewIfNeeded();
  const box=await canvas.boundingBox();
  await page.mouse.move(box.x+box.width/2,box.y+box.height/2);
  await page.mouse.down();await page.mouse.move(box.x+box.width/2+45,box.y+box.height/2+20,{steps:6});await page.mouse.up();
  const first=await signature();check(first!==original,'drag/zoom must change crop');
  await page.getByRole('button',{name:'2. crop-1.png',exact:true}).click();
  await page.waitForFunction(()=>!standbyPhotoPreviewBusy && standbyPhotoEditor?.file.name==='crop-1.png');
  await page.getByRole('slider',{name:'裁剪缩放'}).fill('1.65');
  await canvas.focus();await page.keyboard.press('Shift+ArrowLeft');
  const second=await signature();
  await page.getByRole('button',{name:'1. crop-0.png',exact:true}).click();
  await page.waitForFunction(()=>!standbyPhotoPreviewBusy && standbyPhotoEditor?.file.name==='crop-0.png');
  check(await signature()===first,'first image crop must survive switching');
  check(await page.getByRole('slider',{name:'裁剪缩放'}).inputValue()==='2','restore zoom');
  await page.setViewportSize({width:390,height:844});
  check(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'mobile overflow');
  await canvas.scrollIntoViewIfNeeded();
  await page.screenshot({path:'output/playwright/photo-crop-mobile.png',fullPage:true});
  const bounds=await page.evaluate(()=>{
    for(const [w,h] of [[320,240],[240,320],[160,128],[128,160]])for(const [sw,sh] of [[800,400],[400,800],[33,27]])for(const zoom of [1,2,4])for(const c of [-5,0,.5,1,5]){
      const r=photoCoverRect(sw,sh,w,h,{zoom,cx:c,cy:c});
      if(r.x<0||r.y<0||r.x+r.width>sw+1e-8||r.y+r.height>sh+1e-8||Math.abs(r.width/r.height-w/h)>1e-8)throw new Error('invalid crop bounds');
    }return true;
  });check(bounds,'crop bounds');
  await page.getByRole('button',{name:'上传所选图片',exact:true}).click();
  await page.waitForFunction(()=>!standbyPhotoBusy && document.getElementById('photo-result').textContent.includes('已添加 2 张'));
  const uploaded=await (await page.request.get('http://127.0.0.1:8791/fixture/uploads')).json();
  check(uploaded.length===2,'two uploads');
  check(uploaded[0].sha256===first && uploaded[1].sha256===second,'uploaded RGB565 must match each reviewed crop');
  check(uploaded.every(v=>v.bytes===153608),'unchanged firmware format/size');
  return {passed:true,images:2,bytesPerImage:153608,mobile:'390px',bounds:'4 screen sizes, 3 source sizes, 3 zooms, 5 centers',previewMatchesUploads:true};
}
