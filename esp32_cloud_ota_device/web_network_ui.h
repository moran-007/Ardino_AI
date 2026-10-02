#pragma once
#include <Arduino.h>

static const char WIFI_SETTINGS_PAGE[] PROGMEM = R"HTML(
<fieldset id='network'><legend>Wi-Fi 网络</legend>
<p>最多保存 5 组网络。先扫描，再按信号强度连接附近的已保存网络；未扫描到的网络会跳过。保存第 6 组会替换最早保存的一组。</p>
<form method='post' action='/wifi/save' onsubmit='return validateWifiBytes()'>
<label>附近 Wi-Fi（点选后输入密码，也可输入正在广播的网络名称）</label>
<input id='wifi-ssid' name='ssid' list='wifi-networks' maxlength='32' required autocomplete='off'>
<datalist id='wifi-networks'></datalist>
<button type='button' onclick='scanWifi(true)'>重新扫描</button><p id='wifi-result'>正在扫描附近 Wi-Fi…</p>
<div id='wifi-choices' class='weather-result'></div>
<label>Wi-Fi 密码（已保存的网络留空即可复用）</label>
<input id='wifi-password' type='password' name='wifi_password' maxlength='63' autocomplete='new-password'>
<label><input style='width:auto' type='checkbox' name='open' value='1'>这是无密码网络</label>
<button type='submit'>保存并连接</button></form>
<h3>已保存网络</h3><div id='wifi-saved'></div>
</fieldset>
<script>
async function adminFetch(url, options={}) {
  const response=await fetch(url,{cache:'no-store',...options});
  if(response.status===401){location.replace('/login');throw new Error('请重新登录');}
  return response;
}
function validateWifiBytes(){
  const bytes=new TextEncoder();
  const ssid=document.getElementById('wifi-ssid').value;
  const password=document.getElementById('wifi-password').value;
  if(!ssid||bytes.encode(ssid).length>32||bytes.encode(password).length>63){alert('Wi-Fi 名称最多32字节，密码最多63字节。');return false;}
  return true;
}
let wifiScanBusy=false;
async function scanWifi(start=false){
  if(wifiScanBusy)return;
  wifiScanBusy=true;
  const result=document.getElementById('wifi-result');
  let retry=false;
  try{
    const r=await adminFetch('/wifi/scan'+(start?'?start=1':''));
    if(!r.ok)throw new Error(await r.text());
    const data=await r.json();
    if(data.scanning){result.textContent='正在扫描附近 Wi-Fi…';retry=true;}
    else if(data.failed){throw new Error('扫描失败，请稍后重新扫描');}
    else{
      const list=document.getElementById('wifi-networks'),choices=document.getElementById('wifi-choices');
      list.replaceChildren();choices.replaceChildren();
      for(const network of data.networks){
        const label=network.ssid+' · '+network.rssi+' dBm'+(network.saved?' · 已保存':'')+(network.open?' · 无密码':'');
        const option=document.createElement('option');option.value=network.ssid;option.label=label;list.appendChild(option);
        const button=document.createElement('button');button.type='button';button.textContent=label;
        button.onclick=()=>{document.getElementById('wifi-ssid').value=network.ssid;document.getElementById('wifi-password').value='';document.querySelector('[name=open]').checked=network.open;};choices.appendChild(button);
      }
      result.textContent=data.networks.length?'点击选择网络后填写密码。':'未找到附近网络，请确认路由器开启 2.4 GHz 和 Wi-Fi 名称广播后重新扫描。';
    }
  }catch(e){result.textContent=e.message;}
  finally{wifiScanBusy=false;if(retry)setTimeout(()=>scanWifi(false),1200);}
}
async function loadSavedWifi(){
  const box=document.getElementById('wifi-saved');
  try{
    const r=await adminFetch('/wifi/saved');if(!r.ok)throw new Error('读取失败');
    const data=await r.json();box.replaceChildren();
    for(const ssid of data.networks){
      const row=document.createElement('div');row.className='alarm-row';
      const choose=document.createElement('button');choose.type='button';choose.textContent=ssid+(ssid===data.connected?' · 已连接':'');
      choose.onclick=()=>{document.getElementById('wifi-ssid').value=ssid;document.getElementById('wifi-password').value='';document.querySelector('[name=open]').checked=false;};
      const forget=document.createElement('button');forget.type='button';forget.className='danger';forget.textContent='忘记';
      forget.onclick=async()=>{try{const r=await adminFetch('/wifi/forget',{method:'POST',body:new URLSearchParams({ssid})});if(!r.ok)throw new Error(await r.text());await loadSavedWifi();}catch(e){alert(e.message);}};
      row.append(choose,document.createTextNode(' '),forget);box.appendChild(row);
    }
    if(!data.networks.length)box.textContent='尚未保存网络。';
    if(data.connected&&!document.getElementById('wifi-ssid').value)document.getElementById('wifi-ssid').value=data.connected;
  }catch(e){box.textContent=e.message;}
}
// Started lazily when the network section is opened.
</script>
)HTML";
