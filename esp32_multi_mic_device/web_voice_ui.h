#pragma once

#include <Arduino.h>
#include "voice_capture_settings.h"

namespace VoiceWeb {
inline void appendControls(String &page) {
  const auto &config = VoiceCapture::settings();
  page += F("<h3>手动收音</h3><div class='actions'>"
            "<button id='capture-start' type='button' disabled>开始收音</button>"
            "<button id='capture-finish' type='button' disabled>结束并发送</button>"
            "<button id='capture-stop' class='danger' type='button'>结束当前对话</button></div>"
            "<p id='capture-recording-state' role='status' aria-live='polite'></p>"
            "<p id='capture-message' role='status' aria-live='polite'></p>"
            "<h3>收音与连续对话</h3><form id='capture-form'>"
            "<label for='capture-gain'>录音增益 <output id='capture-gain-value' for='capture-gain'>");
  page += String(config.gain);
  page += F(" 倍</output></label><input id='capture-gain' name='mic_gain' type='range' min='1' max='12' step='1' value='");
  page += String(config.gain);
  page += F("'><label for='capture-sensitivity'>收音灵敏度 <output id='capture-sensitivity-value' for='capture-sensitivity'>");
  page += String(config.sensitivity);
  page += F("</output></label><input id='capture-sensitivity' name='mic_sensitivity' type='range' min='0' max='100' step='1' value='");
  page += String(config.sensitivity);
  page += F("'><div class='row'><div><label for='capture-silence'>停顿发送（毫秒）</label>"
            "<input id='capture-silence' name='silence_ms' type='number' min='600' max='3000' step='1' required value='");
  page += String(config.silenceMs);
  page += F("'></div><div><label for='capture-listen'>最长等待说话（秒）</label>"
            "<input id='capture-listen' name='listen_seconds' type='number' min='3' max='20' step='1' required value='");
  page += String(config.listenSeconds);
  page += F("'></div></div><label for='capture-noise'><input id='capture-noise' name='noise_suppression' type='checkbox'");
  if (config.noiseSuppression) page += F(" checked");
  page += F("> 录音降噪</label><label for='capture-continuous'><input id='capture-continuous' name='continuous_conversation' type='checkbox'");
  if (config.continuous) page += F(" checked");
  page += F("> 连续对话</label><div class='actions'><button id='capture-save' type='submit'>保存收音设置</button></div></form>"
            "<p id='capture-diagnostics' class='meta' aria-live='polite'></p>");
  page += F("<p id='capture-noise-advice' role='status'></p><button id='capture-calibrate' type='button'>重新校准环境底噪</button><p class='meta'>待机时自动估计背景噪声；收音、播放和响铃时冻结基线。</p>");
  page += F(R"HTML(<script>
(()=>{
const form=document.getElementById('capture-form');
const message=document.getElementById('capture-message');
const stop=document.getElementById('capture-stop');
const start=document.getElementById('capture-start'),finish=document.getElementById('capture-finish');
let lastStatus={},controlRequest=false;
function buttons(){start.disabled=controlRequest||!!(lastStatus.busy||lastStatus.manual_recording_pending||lastStatus.alarm_ringing)||lastStatus.audio_ready===false;finish.disabled=controlRequest||!lastStatus.manual_recording_active;}
const gain=form.elements.mic_gain,sensitivity=form.elements.mic_sensitivity;
function values(){document.getElementById('capture-gain-value').textContent=gain.value+' 倍';document.getElementById('capture-sensitivity-value').textContent=sensitivity.value;}
gain.addEventListener('input',values);sensitivity.addEventListener('input',values);
async function post(path,data){const response=await fetch(path,{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});if(!response.ok){if(response.status===401)throw new Error('登录已过期，请重新登录');throw new Error(await response.text());}return response.json();}
window.updateVoiceCaptureStatus=function(d){
lastStatus=d;buttons();
document.getElementById('capture-noise-advice').textContent=(d.microphone_advice||'正在校准底噪')+' · 底噪 '+Math.round(d.ambient_noise_floor||0)+' · 语音门槛 '+(d.ambient_speech_threshold||0);
document.getElementById('capture-recording-state').textContent=d.manual_recording_pending?'准备收音…':d.manual_recording_active?'收音中':d.busy?'正在处理对话…':'待机';
document.getElementById('capture-diagnostics').textContent=(d.conversation_active?'连续对话中':'连续对话未进行')+' · 最近录音：音量 '+(d.last_recording_rms||0)+' / 峰值 '+(d.last_recording_peak||0)+' / 有效语音 '+(d.last_recording_speech_ms||0)+' 毫秒 / 削波 '+(d.last_recording_clipped_samples||0)+' / 降噪 '+(d.last_recording_noise_suppression?'开':'关');
};
async function recordingCommand(path){if(controlRequest)return;controlRequest=true;buttons();message.textContent='';try{window.updateVoiceCaptureStatus(await post(path,{}));}catch(error){message.textContent=error.message;}finally{controlRequest=false;buttons();}}
start.addEventListener('click',()=>recordingCommand('/voice/record/start'));
document.getElementById('capture-calibrate').onclick=async()=>{try{await post('/voice/noise/reset',{});message.textContent='校准已重新开始，请安静两秒';}catch(e){message.textContent=e.message;}};
finish.addEventListener('click',()=>recordingCommand('/voice/record/finish'));
form.addEventListener('submit',async event=>{event.preventDefault();if(!form.reportValidity())return;
const data={mic_gain:Number(gain.value),mic_sensitivity:Number(sensitivity.value),noise_suppression:form.elements.noise_suppression.checked,silence_ms:Number(form.elements.silence_ms.value),listen_seconds:Number(form.elements.listen_seconds.value),continuous_conversation:form.elements.continuous_conversation.checked};
const controls=Array.from(form.elements);controls.forEach(c=>c.disabled=true);message.textContent='正在保存…';
try{const d=await post('/voice/settings',data);for(const key of Object.keys(data)){const field=form.elements[key];if(field.type==='checkbox')field.checked=d[key];else field.value=d[key];}values();window.updateVoiceCaptureStatus(d);message.textContent='收音设置已保存';}
catch(error){message.textContent=error.message;}finally{controls.forEach(c=>c.disabled=false);}});
stop.addEventListener('click',async()=>{stop.disabled=true;try{await post('/voice/stop',{});lastStatus.manual_recording_pending=false;lastStatus.manual_recording_active=false;window.updateVoiceCaptureStatus(lastStatus);message.textContent='已请求结束当前对话';}catch(error){message.textContent=error.message;}finally{stop.disabled=false;}});
})();
</script>)HTML");
}
}  // namespace VoiceWeb
