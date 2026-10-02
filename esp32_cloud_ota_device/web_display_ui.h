#pragma once

#include "robot_display.h"
#include "web_admin.h"

namespace DisplayWeb {
static const char SETUP_PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>机器人初始化配置</title>
<style>body{font-family:system-ui;max-width:420px;margin:8vh auto;padding:22px;color:#203b30;background:#f5f8f5;line-height:1.6}input,button{box-sizing:border-box;width:100%;padding:12px;margin:10px 0;font:inherit}button{background:#286b4c;color:white;border:0;border-radius:8px}small{color:#566d60}</style>
<h1>机器人初始化配置</h1><p>已打开设备页面。登录后选择 Wi-Fi，再到“设备与更新”填写设备凭证。</p>
<form method="post" action="/login"><input type="hidden" name="next" value="setup">
<label for="pin">设备连接码</label><input id="pin" type="password" name="pin" inputmode="numeric" minlength="6" maxlength="6" pattern="[0-9]{6}" autocomplete="current-password" required autofocus>
<button type="submit">登录并开始配置</button></form>
<small>首次默认连接码为 123456；修改过则使用新码。连入家庭 Wi-Fi 后，配置热点会关闭；手机切回同一 Wi-Fi，用屏幕顶部的 IP 继续访问后台。</small></html>
)HTML";

inline void appendControls(WebAdmin::PageStream &page, bool volumePot) {
  page += F("<fieldset><legend>表情造型</legend><form method='post' action='/display/appearance'>"
            "<label for='portrait-style'>竖屏造型</label><select name='style' id='portrait-style'><option value='face'");
  if (!RobotDisplay::portraitBodyEnabled()) page += F(" selected");
  page += F(">大眼睛与小嘴（参考图风格）</option><option value='body'");
  if (RobotDisplay::portraitBodyEnabled()) page += F(" selected");
  page += F(">带全身动作的角色</option></select><p class='meta'>默认横竖屏均以发光表情为主，时间在上方、天气在下方。全身角色为竖屏可选造型。</p>"
            "<button type='submit'>保存表情造型</button></form></fieldset>");
  page += F("<fieldset><legend>屏幕亮度</legend><form method='post' action='/display/brightness'>"
            "<label for='display-brightness'>网页亮度：<output id='brightness-value'>");
  page += String(RobotDisplay::brightnessSetting());
  page += F("</output>%</label><input id='display-brightness' type='range' name='brightness' min='5' max='100' step='1' value='");
  page += String(RobotDisplay::brightnessSetting());
  page += F("' oninput=\"document.getElementById('brightness-value').value=this.value\">"
            "<label for='brightness-mode'>调节方式</label><select id='brightness-mode' name='mode'><option value='fixed'");
  if (!RobotDisplay::potentiometerBrightnessEnabled()) page += F(" selected");
  page += F(">网页设置</option><option value='pot'");
  if (RobotDisplay::potentiometerBrightnessEnabled()) page += F(" selected");
  if (volumePot || RobotDisplay::BATTERY_MONITOR_ENABLED) page += F(" disabled");
  page += F(">GPIO1 电位器</option></select>");
  if (volumePot || RobotDisplay::BATTERY_MONITOR_ENABLED) {
    page += F("<p class='meta'>GPIO1 已用于音量或电池检测；先将音量设为固定，且关闭电池检测，才能用它调节亮度。</p>");
  } else {
    page += F("<p class='meta'>使用旋钮时，两端接 3V3、GND，滑臂接 GPIO1；不能同时用作音量旋钮或电池检测。</p>");
  }
  page += RobotDisplay::hardwareBacklightControl()
      ? F("<p class='meta'>当前使用背光调节。</p>")
      : F("<p class='meta'>当前使用软件压暗画面。7 针 ST7789 没有独立背光脚，灯光功耗不会随之降低。</p>");
  page += F("<p class='meta'>范围 5–100%，保留最低可见亮度；配网二维码显示期间至少使用 30%。</p>"
            "<button type='submit'>应用并保存亮度</button></form></fieldset>"
            "<fieldset><legend>说话时的显示方式</legend><form method='post' action='/display/voice-layout'>"
            "<p>两种触发方式可独立设置。底部状态条保留当前页面，全屏显示会为表情和文字留出更多空间。</p>");
  const char *names[] = {"button", "wake"};
  const char *labels[] = {"按键说话", "语音唤醒"};
  for (int i = 0; i < 2; ++i) {
    page += String("<label for='voice-layout-") + names[i] + "'>" + labels[i] + "</label><select id='voice-layout-" + names[i] + "' name='" + names[i] + "'>";
    page += F("<option value='compact'");
    if (!RobotDisplay::voiceFullscreen(i == 1)) page += F(" selected");
    page += F(">底部状态条（保留当前页面）</option><option value='full'");
    if (RobotDisplay::voiceFullscreen(i == 1)) page += F(" selected");
    page += F(">全屏表情与文字</option></select>");
  }
  page += F("<p><button type='submit'>保存说话显示方式</button></p>"
            "<p class='meta'>保存后立即生效，重启及同分区 OTA 后保留。待机表情和天气主页都会自动适配横竖屏。</p>"
            "</form></fieldset>");
}
}  // namespace DisplayWeb
