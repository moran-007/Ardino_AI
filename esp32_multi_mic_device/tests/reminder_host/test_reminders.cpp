#include "Arduino.h"
#include "Preferences.h"
#include "FS.h"
#include "../../reminder_commands.h"
#include "../../web_memo_ui.h"
#include "../../web_admin.h"
#include "../../web_admin_ui.h"
#include "../../web_alarm_music.h"
#include <ArduinoJson.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

size_t checks = 0;
void require(bool value, const char *message) {
  ++checks;
  if(!value)throw std::runtime_error(message);
}
String escape(String text) {
  text.replace("&","&amp;");text.replace("<","&lt;");text.replace(">","&gt;");text.replace("\"","&quot;");text.replace("'","&#39;");return text;
}
int main(int argc,char **argv) {
  try {
  fs::FS storage;
  require(MemoManager::begin(storage),"memo init");
  require(AlarmManager::begin(),"alarm init");
  tm clock{};clock.tm_year=126;clock.tm_mon=9;clock.tm_mday=1;clock.tm_hour=21;
  const time_t now=mktime(&clock);
  auto result=ReminderCommands::execute("一会儿需要去买东西，加个待办。",now);
  const auto suffixResult=result;
  require(result.memoChanged&&!result.alarmChanged,"suffix todo must not create alarm");
  require(MemoManager::count(MemoManager::Kind::Todo)==1,"todo missing");
  MemoManager::Entry memo;
  require(MemoManager::get(0,memo)&&memo.text=="一会儿需要去买东西","todo content includes command suffix");
  result=ReminderCommands::execute("明天早上 9:35 提醒我做其他事情，加一个待办。",now);
  const auto compoundResult=result;
  require(result.memoChanged&&result.alarmChanged,"compound command must create both");
  require(MemoManager::count(MemoManager::Kind::Todo)==2&&AlarmManager::count()==1,"compound counts wrong");
  AlarmManager::Entry alarm;
  require(AlarmManager::get(0,alarm)&&alarm.label=="做其他事情","reminder label missing");
  const time_t trigger=static_cast<time_t>(alarm.triggerAt);
  tm *at=localtime(&trigger);
  require(at&&at->tm_mday==2&&at->tm_hour==9&&at->tm_min==35,"reminder time wrong");
  result=ReminderCommands::execute("给我定个明天早上9点半的闹钟。",now);
  require(result.alarmChanged&&!result.memoChanged&&AlarmManager::count()==2,"standalone alarm wrong");
  result=ReminderCommands::execute("记一下，明天我要去超市。",now);
  require(result.memoChanged&&!result.alarmChanged,"note command failed");
  result=ReminderCommands::execute("明天我要去超市。",now);
  require(!result.handled,"casual speech must not create anything");
  result=ReminderCommands::execute("添加待办完成作业",now);
  require(result.memoChanged,"completion word in todo content consumed as operation");
  result=ReminderCommands::execute("记一下删除所有闹钟",now);
  require(result.memoChanged&&AlarmManager::count()==2,"note content executed destructive alarm");
  result=ReminderCommands::execute("明天10点提醒我取消会议，加个待办",now);
  require(result.memoChanged&&result.alarmChanged&&AlarmManager::count()==3,"reminder label treated as cancel");
  result=ReminderCommands::execute("明天10点提醒我买菜，加个待办",0);
  require(result.memoChanged&&!result.alarmChanged&&result.message.indexOf("提醒未设置")>=0,"missing clock falsely confirmed");
  result=ReminderCommands::execute("下午买电池，加个代办",now);
  require(result.memoChanged&&!result.alarmChanged,"ASR todo spelling variant failed");
  failMemoWrite=true;
  const size_t before=MemoManager::count();
  result=ReminderCommands::execute("添加待办买水",now);
  require(result.handled&&!result.memoChanged&&MemoManager::count()==before,"memo write failure not rolled back");
  require(result.message.indexOf("保存失败")>=0,"memo write failure falsely acknowledged");
  failMemoWrite=false;
  failNvsWrite=true;
  const size_t alarms=AlarmManager::count();
  result=ReminderCommands::execute("明天11点提醒我喝水，加一个待办",now);
  require(result.memoChanged&&!result.alarmChanged&&AlarmManager::count()==alarms,"partial failure hidden");
  require(result.message.indexOf("保存失败")>=0,"alarm failure not visible");
  failNvsWrite=false;
  require(MemoManager::begin(storage)&&MemoManager::count()==before+1,"memo reload lost items");
  result=ReminderCommands::execute("完成第2个待办",now);
  require(result.memoChanged,"ordinal complete failed");
  require(MemoManager::add(MemoManager::Kind::Note,"<script>alert('x')</script>长中文便签长中文便签长中文便签",now),"fixture note add");
  for(size_t cursor=MemoManager::count();cursor>0;--cursor) {
    MemoManager::Entry latest;
    if(MemoManager::get(cursor-1,latest)&&latest.kind==MemoManager::Kind::Todo) {
      require(MemoManager::setCompleted(latest.id,true),"complete latest todo for screen fixture");
      break;
    }
  }
  const String notes=MemoManager::displaySummary(MemoManager::Kind::Note);
  const String todos=MemoManager::displaySummary(MemoManager::Kind::Todo);
  require(notes.indexOf("<script>")>=0&&notes.indexOf("买水")<0,"note screen summary mixes todo content");
  require(todos.indexOf("<script>")<0&&todos.indexOf("已完成")>=0,"todo screen summary omits completed state or mixes notes");
  String page("<!doctype html><html lang='zh'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>*{box-sizing:border-box}body{font:15px/1.6 Arial,sans-serif;margin:0;background:#f4f7f9;color:#233d4b}main{max-width:900px;margin:auto;padding:24px 16px}fieldset{border:1px solid #dbe3e9;border-radius:8px;background:white;padding:20px;min-width:0}input,select{width:100%;max-width:100%;padding:10px;border:1px solid #ccd8df;border-radius:6px;font:inherit}label{display:block;margin:10px 0}button{font:inherit;padding:8px 12px;border:1px solid #d6e1e7;border-radius:6px;background:#087e75;color:white;cursor:pointer}button.secondary{background:#fff;color:#285565}button.danger{background:#fff2ef;color:#a43c30}p{margin:10px 0}.warn{color:#9a7626}.ok{color:#23855d}details{margin-top:20px}h1{font-size:24px}</style><main><h1>提醒事项</h1>");
  MemoWeb::append(page,escape);
  page+="</main></html>";
  require(page.indexOf("&lt;script&gt;")>=0,"memo HTML injection");
  require(page.indexOf("role='tablist'")>=0&&page.indexOf("id='memo-view-todo'")>=0,"memo tabs missing");
  WebServer web;
  WebAdmin::PageStream stream(web);
  stream += ADMIN_PAGE_HEAD;
  stream += F("<h2>提醒事项</h2>");
  MemoWeb::append(stream,escape);
  stream += F("</main></div></body></html>");
  AlarmMusicWeb::appendControls(stream);
  stream.finish();
  require(web.html.find("id='music-upload'")!=std::string::npos,"music controls cannot render through production PageStream");
  require(web.html.find("id='memo-view-todo'")!=std::string::npos,"production PageStream output missing");
  if(argc>1)std::ofstream(argv[1])<<web.html;
  if(argc>2) {
    JsonDocument doc;
    JsonArray list=doc.to<JsonArray>();
    auto first=list.add<JsonObject>();
    first["question"]="一会儿需要去买东西，加个待办。";
    first["message"]=suffixResult.message;
    auto second=list.add<JsonObject>();
    second["question"]="明天早上 9:35 提醒我做其他事情，加一个待办。";
    second["message"]=compoundResult.message;
    std::ofstream output(argv[2]);
    serializeJson(doc,output);
  }
  AlarmManager::clearAll();
  require(AlarmManager::addTimer(1,"ring test",now),"ring timer add");
  AlarmManager::Entry fired;
  require(AlarmManager::poll(now+2,fired)&&AlarmManager::ringing(),"ring timer missing");
  auto ringResult=AlarmManager::handleNaturalLanguage("再等三分钟",now+2);
  require(ringResult.handled&&ringResult.changed&&!AlarmManager::ringing(),"spoken snooze failed");
  require(!AlarmManager::poll(now+181,fired)&&AlarmManager::poll(now+182,fired),"spoken snooze duration wrong");
  ringResult=AlarmManager::handleNaturalLanguage("结束闹钟",now+183);
  require(ringResult.handled&&ringResult.changed&&!AlarmManager::ringing(),"spoken dismiss failed");
  require(AlarmManager::addTimer(1,"full list",now),"full list timer add");
  for(size_t i=1;i<AlarmManager::MAX_ENTRIES;i++)require(AlarmManager::addTimer(600,"later",now),"fill list");
  require(AlarmManager::poll(now+2,fired)&&AlarmManager::snooze(now+2,300),"full list one-shot snooze lost alarm");
  require(AlarmManager::count()==AlarmManager::MAX_ENTRIES,"snooze duplicated entry");
  std::cout<<checks<<" reminder host checks passed\n";
  } catch(const std::exception &error) {
    std::cerr<<"FAIL: "<<error.what()<<'\n';
    return 1;
  }
}
