async page => {
  const pngs = await page.evaluate(() => [0,1].map(index => {
    const c=document.createElement('canvas');c.width=800;c.height=400;
    const x=c.getContext('2d');
    for(let col=0;col<800;col++){x.fillStyle='rgb('+Math.round(col*255/799)+','+(index?220:40)+','+Math.round(255-col*255/799)+')';x.fillRect(col,0,1,400);}
    x.fillStyle=index?'#ffff00':'#00ff00';x.fillRect(40,40,120,120);
    return c.toDataURL('image/png').split(',')[1];
  }));
  await page.getByLabel('选择图片（每张最大 8 MB、2400 万像素）').setInputFiles(pngs.map((b,i)=>({name:'crop-'+i+'.png',mimeType:'image/png',buffer:Buffer.from(b,'base64')})));
  await page.getByRole('slider',{name:'裁剪缩放'}).waitFor({state:'visible'});
  await page.waitForFunction(()=>!standbyPhotoPreviewBusy && !!standbyPhotoPrepared);
  return {ready:true,selected:2};
}
