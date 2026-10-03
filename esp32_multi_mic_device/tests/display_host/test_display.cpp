// Run the production renderer and real Adafruit/U8g2 rasterizers on the host.
// Only hardware, storage and the QR encoder entry point are replaced.
#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>
#include "../../robot_display.cpp"
#include "../../web_display_ui.h"
#include "../../web_admin_ui.h"
#include "../../web_expressions_ui.h"
#include "../../robot_expression_web.h"

namespace RobotPhoto {
unsigned photoReads=0;
bool available(){return true;}
uint32_t revision(){return 1;}
bool copyTo(uint16_t *pixels,uint16_t w,uint16_t h){
  ++photoReads;
  for(size_t i=0;i<size_t(w)*h;++i)pixels[i]=static_cast<uint16_t>(0x07E0 | (i%31));
  return true;
}
}

using namespace RobotDisplay;
class BoundsCanvas:public Adafruit_GFX {
 public:
  BoundsCanvas(int w,int h):Adafruit_GFX(w,h),pixels(size_t(w)*h,COLOR_BG){}
  void drawPixel(int16_t x,int16_t y,uint16_t color) override {
    assert(x>=8 && y>=8 && x<width()-8 && y<height()-8);
    pixels[size_t(y)*width()+x]=color;
  }
  std::vector<uint16_t> pixels;
};
void saveFrame(const std::string &name){
  std::ofstream out(name+".ppm",std::ios::binary);
  out<<"P6\n"<<screenWidth<<" "<<screenHeight<<"\n255\n";
  for(uint16_t p:sentPixels){
    unsigned char rgb[]={static_cast<unsigned char>((p>>11)*255/31),
                         static_cast<unsigned char>(((p>>5)&63)*255/63),
                         static_cast<unsigned char>((p&31)*255/31)};
    out.write(reinterpret_cast<char*>(rgb),3);
  }
}
std::vector<uint16_t> frameCopy(){return {frame->getBuffer(),frame->getBuffer()+size_t(screenWidth)*screenHeight};}
void geometry(Panel panel,uint8_t rotation){
  configurePanelGeometry(panel,rotation);
  delete frame;
  frame=new DisplayCanvas16(screenWidth,screenHeight,false);
  tft=highResPanel?static_cast<Adafruit_ST77xx*>(&tft7789):static_cast<Adafruit_ST77xx*>(&tft7735);
  tft->setRotation(rotation);
  assert(tft->width()==screenWidth && tft->height()==screenHeight);
  textRenderer.begin(*frame);
  textRenderer.setFontMode(1);textRenderer.setFontDirection(0);restoreBodyFont();
  lastIdleMask=0;lastIdlePage=0;lastFrameScene=Scene::Booting;
  renderedRevision=0;textPageOffset=0;photoPainted=false;
}

void checkTransparentText(){
  const size_t count=size_t(screenWidth)*screenHeight;
  for(size_t i=0;i<count;++i)frame->getBuffer()[i]=static_cast<uint16_t>(0x1000+(i%1024));
  const auto before=frameCopy();
  // A contrasting background exposes any opaque glyph writes after switching
  // between clock, ASCII and Chinese fonts on a nonuniform page background.
  textRenderer.setBackgroundColor(0xF800);
  drawClock(5,45);
  drawFittedUtf8(5,70,screenWidth-10,"上海 26 C");
  drawClock(5,110);
  size_t changed=0;
  for(size_t i=0;i<count;++i)if(frame->getBuffer()[i]!=before[i]){
    ++changed;assert(frame->getBuffer()[i]==COLOR_TEXT);
  }
  assert(changed>50);
  textRenderer.setBackgroundColor(COLOR_BG);
}

int main(){
#ifdef _WIN32
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  assert(begin());
  for(Panel panel:{Panel::M200T_ST7789_240X320_7PIN,Panel::ST7735S_128X160_8PIN}){
    for(uint8_t rotation=0;rotation<4;++rotation){geometry(panel,rotation);checkTransparentText();}
  }
  assert(brightnessSetting()==100 && !potentiometerBrightnessEnabled());
  assert(!voiceFullscreen(false) && !voiceFullscreen(true));
  assert(!portraitBodyEnabled());assert(savePortraitBodyPreference(true));assert(portraitBodyEnabled());
  assert(expressionMask()==RobotExpressions::ALL);
  uint64_t parsed=99;
  for(const char *bad:{"",",","0,","0,,1","0,0","48","-1","1a"," 1","0,01,1"}) {
    assert(!RobotExpressions::parseSelection(bad,parsed));assert(parsed==99);
  }
  assert(RobotExpressions::parseSelection("0,47",parsed));
  assert(parsed==(uint64_t{1}|(uint64_t{1}<<47)));
  assert(!saveExpressionMask(0));assert(!saveExpressionMask(uint64_t{1}<<48));
  for(uint8_t id=0;id<48;++id) {
    assert(RobotExpressions::choose(uint64_t{1}<<id,id,99)==id);
    assert(RobotExpressions::choose(RobotExpressions::ALL,id,99)!=id);
  }
  assert(saveExpressionMask(parsed));
  assert(!saveBrightnessPreference(0,false));
  assert(!saveBrightnessPreference(101,false));
  assert(saveBrightnessPreference(37,false));
  failNvsWrite=true;
  assert(!savePortraitBodyPreference(false));assert(portraitBodyEnabled());
  assert(!saveExpressionMask(1));assert(expressionMask()==parsed);
  assert(!saveBrightnessPreference(90,false));
  assert(brightnessSetting()==37);
  assert(!saveVoiceLayoutPreferences(true,true));
  assert(!voiceFullscreen(false) && !voiceFullscreen(true));
  failNvsWrite=false;
  assert(savePortraitBodyPreference(false));
  for(unsigned bits=0;bits<4;++bits){
    assert(saveVoiceLayoutPreferences(bits&1,bits&2));
    assert(voiceFullscreen(false)==bool(bits&1));assert(voiceFullscreen(true)==bool(bits&2));
    assert(fakeNvs["cloud-ai/disp_voice"]==bits);
  }
  assert(saveVoiceLayoutPreferences(false,false));
  fakeNvs["cloud-ai/volume_pot"]=1;
  assert(!saveBrightnessPreference(50,true));
  fakeNvs["cloud-ai/volume_pot"]=0;
  assert(saveBrightnessPreference(50,true));
  fakeAdc=0;updateBrightness(false);assert(effectiveBrightness()==5);
  fakeAdc=4095;for(int i=0;i<80;++i)updateBrightness(false);
  assert(effectiveBrightness()==100);
  assert(saveBrightnessPreference(5,false));updateBrightness(true);
  assert(effectiveBrightness()==30 && brightnessSetting()==5);
  updateBrightness(false);assert(effectiveBrightness()==5);
  backlightPwm=true;updateBrightness(false);assert(fakeDuty==13);
  fakePwmFailure=true;updateBrightness(false);assert(!hardwareBacklightControl());
  fakePwmFailure=false;
  assert(escapeWifiQr("a;b:c,d\\e\"f")=="a\\;b\\:c\\,d\\\\e\\\"f");
  assert(saveBrightnessPreference(100,false));
  // An eight-second preview must pause, then resume the same carousel page.
  model.scene=Scene::Idle;model.pageMask=PAGE_HOME|PAGE_EXPRESSION;model.photoMode=false;
  render(model);fakeMillis+=IDLE_CAROUSEL_INTERVAL_MS+1000;render(model);
  assert(lastIdlePage==PAGE_HOME);
  assert(previewExpression(47));render(model);assert(lastIdlePage==PAGE_EXPRESSION);
  fakeMillis+=8001;render(model);assert(lastIdlePage==PAGE_HOME);
  assert(previewExpression(1));render(model);fakeMillis+=3500;
  cancelExpressionPreview();render(model);assert(lastIdlePage==PAGE_HOME);
  assert(!previewExpression(48));
  setVoiceSource(true);assert(!previewExpression(1));
  show(Scene::Idle,"idle","");assert(previewExpression(1));
  show(Scene::Alarm,"alarm","");assert(!previewExpression(1));assert(model.previewUntilMs==0);
  show(Scene::Idle,"idle","");setProvisioning(true);assert(!previewExpression(1));setProvisioning(false);
  // All emotion/motion phases must stay inside their allocated rectangle.
  for(const auto &dimensions:std::vector<std::pair<int,int>>{{320,240},{240,320},{160,128},{128,160}}){
    for(bool body:{false,true}) for(uint8_t mood=0;mood<RobotAvatar::EXPRESSION_COUNT;++mood){
      for(unsigned state=0;state<=static_cast<unsigned>(Scene::Updating);++state){
        for(uint32_t time:{0U,80U,1200U,2400U,4100U}){
          BoundsCanvas canvas(dimensions.first,dimensions.second);
          RobotAvatar::draw(canvas,static_cast<Scene>(state),body,8,8,canvas.width()-16,canvas.height()-16,time,mood);
        }
      }
    }
  }
  for(Scene state:{Scene::Idle,Scene::Listening,Scene::Thinking,Scene::Speaking,Scene::Alarm}){
    BoundsCanvas a(128,160),b(128,160);
    RobotAvatar::draw(a,state,true,8,8,112,144,0,0);
    RobotAvatar::draw(b,state,true,8,8,112,144,1200,0);
    assert(a.pixels!=b.pixels);
  }

  unsigned renders=0;
  for(Panel panel:{Panel::M200T_ST7789_240X320_7PIN,Panel::ST7735S_128X160_8PIN}){
    for(uint8_t rotation=0;rotation<4;++rotation){
      geometry(panel,rotation);
      const std::string prefix=std::string(highResPanel?"large":"small")+"-r"+std::to_string(rotation);
      assert(memoTextPage("喝水。",0,0,0,textRenderer.getUTF8Width("喝水"),1,false)==String("喝").length());
      Model demo;
      demo.scene=Scene::Idle;demo.pageMask=PAGE_HOME;
      demo.networkConnected=true;demo.rssi=-56;std::strcpy(demo.ip,"192.168.123.123");
      demo.city="上海市浦东新区";demo.weatherCondition="多云";demo.temperatureC=26.5;demo.weatherValid=true;
      demo.alarmStatus="明天 07:30 起床";demo.forecastCount=3;
      for(unsigned i=0;i<3;++i){demo.forecastDay[i]=i==0?"今天":i==1?"明天":"后天";demo.forecastCondition[i]=i==1?"小雨":"多云";demo.forecastHighC[i]=28-i;demo.forecastLowC[i]=22-i;}
      for(bool compact:{false,true}) {
        demo.scene=compact?Scene::Listening:Scene::Idle;
        demo.noteText="2. 明天我要去超市，记得带上购物清单和购物袋，到店以后再检查清单，买完东西回来整理好物品。\n1. 下午三点检查屏幕接线。";
        demo.todoText="3. 买电池和牛奶，下午三点提醒我喝水。\n2. 已完成 写作业\n1. 检查网络";
        demo.pendingTodoCount=2;
        const int margin=highResPanel?10:5,gap=highResPanel?8:6,top=headerHeight+margin;
        const int bottom=screenHeight-(compact?(highResPanel?54:32):0);
        const int leftWidth=(screenWidth-2*margin-gap)/2;
        for(unsigned state=0;state<4;++state) {
          demo.noteCount=state==0||state==1?2:0;
          demo.todoCount=state==0||state==2?3:0;
          frame->fillScreen(COLOR_BG);drawMemos(demo);
          const auto pixels=frameCopy();
          auto pixel=[&](int x,int y){return pixels[size_t(y)*screenWidth+x];};
          if(state==0) {
            assert(pixel(margin+leftWidth/2,top)==COLOR_CYAN);
            assert(pixel(margin+leftWidth+gap+(screenWidth-2*margin-leftWidth-gap)/2,top)==COLOR_GREEN);
            assert(pixel(margin+leftWidth+gap/2,top+20)==COLOR_BG);
          } else if(state!=3) {
            assert(pixel(screenWidth/2,top)==(state==1?COLOR_CYAN:COLOR_GREEN));
            assert(pixel(screenWidth/2,top+1)==COLOR_PANEL);
          } else {
            for(uint16_t color:pixels)assert(color!=COLOR_CYAN&&color!=COLOR_GREEN&&color!=COLOR_PANEL);
          }
          for(int y=bottom;y<screenHeight;++y)for(int x=0;x<screenWidth;++x)assert(pixel(x,y)==COLOR_BG);
          if(!compact) {
            sentPixels=pixels;
            saveFrame(prefix+(state==0?"-memos-both":state==1?"-memos-notes":state==2?"-memos-todos":"-memos-empty"));
          }
        }
        demo.noteCount=2;demo.todoCount=3;
        idlePageStartedAtMs=fakeMillis;
        frame->fillScreen(COLOR_BG);drawMemos(demo);const auto firstPage=frameCopy();
        fakeMillis+=TEXT_PAGE_INTERVAL_MS;
        frame->fillScreen(COLOR_BG);drawMemos(demo);assert(firstPage!=frameCopy());
      }
      demo.scene=Scene::Idle;
      for(uint8_t page:{PAGE_HOME,PAGE_EXPRESSION,PAGE_FORECAST,PAGE_MEMOS,PAGE_PHOTO}){
        demo.scene=Scene::Idle;demo.pageMask=page;demo.body="";render(demo);++renders;
        if(page==PAGE_HOME || page==PAGE_EXPRESSION)saveFrame(prefix+(page==PAGE_HOME?"-home":"-expression"));
        assert(sentPixels==frameCopy());  // 100% brightness must be exact.
        const auto standby=sentPixels;
        auto compactBase=standby;
        const auto reads=RobotPhoto::photoReads;
        for(Scene scene:{Scene::Listening,Scene::Thinking,Scene::Speaking}){
          demo.scene=scene;demo.title=scene==Scene::Listening?"正在听":scene==Scene::Thinking?"思考中":"AI 回答";
          demo.body="这是一段足够长的中文回答，用来检查底部状态条的分页以及页面恢复。";
          textPageOffset=0;
          render(demo);++renders;
          if(page==PAGE_HOME || page==PAGE_EXPRESSION){
            const size_t aboveStrip=size_t(screenHeight-(highResPanel?54:32))*screenWidth;
            if(scene==Scene::Listening)compactBase=sentPixels;
            else assert(std::equal(compactBase.begin(),compactBase.begin()+aboveStrip,sentPixels.begin()));
          }
          if(page==PAGE_FORECAST && scene==Scene::Listening)compactBase=sentPixels;
          if(page==PAGE_FORECAST || page==PAGE_PHOTO){
            const size_t stripStart=size_t(screenHeight-(highResPanel?54:32))*screenWidth;
            assert(std::equal(compactBase.begin(),compactBase.begin()+stripStart,sentPixels.begin()));
          }
          if(page==PAGE_HOME && scene==Scene::Listening)saveFrame(prefix+"-compact");
          assert(nextTextPageOffset>0);
          textPageOffset=nextTextPageOffset;render(demo);assert(textPageOffset>0);
        }
        if(page==PAGE_PHOTO)assert(RobotPhoto::photoReads==reads);
        demo.scene=Scene::Idle;demo.body="";render(demo);
        if(page==PAGE_PHOTO){assert(sentPixels==standby);assert(RobotPhoto::photoReads==reads+1);}
      }
      for(bool wake:{false,true}){
        assert(saveVoiceLayoutPreferences(!wake,wake));demo.voiceFromWake=wake;
        demo.scene=Scene::Speaking;demo.title="AI 回答";demo.body="今天多云，出门记得带伞。这里是长回答的分页测试。";
        render(demo);++renders;
        assert(lastIdlePage==0);saveFrame(prefix+"-full");
      }
      assert(saveVoiceLayoutPreferences(false,false));
      demo.provisioning=true;demo.setupSsid="ROBOT-1A2B";demo.setupPassword="12345678";
      demo.setupUrl="http://192.168.4.1/setup";demo.scene=Scene::Connecting;demo.phoneConnected=false;
      demo.networkConnected=false;std::strcpy(demo.ip,"192.168.4.1");
      render(demo);++renders;saveFrame(prefix+"-wifi-qr");assert(wifiQr.size && urlQr.size);
      const auto encodes=qrEncodeCalls;render(demo);assert(qrEncodeCalls==encodes);
      demo.phoneConnected=true;render(demo);++renders;saveFrame(prefix+"-url-qr");
      assert(qrEncodeCalls==encodes);  // Joining the AP only changes the visible cached QR.
      assert(saveBrightnessPreference(5,false));render(demo);assert(effectiveBrightness()==30);
      demo.provisioning=false;demo.scene=Scene::Idle;demo.pageMask=PAGE_PHOTO;render(demo);
      assert(effectiveBrightness()==5);assert(sentPixels!=frameCopy());
      assert(saveBrightnessPreference(100,false));render(demo);assert(sentPixels==frameCopy());
    }
  }
  setMemoStatus("只有便签", "全部已完成的待办", 1, 1, 0);
  assert(model.noteText=="只有便签"&&model.todoText=="全部已完成的待办"&&model.todoCount==1&&model.pendingTodoCount==0);
  // QR encoder failures retain a manual fallback and retry no faster than 5 s.
  Model setup;setup.provisioning=true;setup.setupSsid="ROBOT-1A2B";setup.setupPassword="12345678";
  setup.setupUrl="http://192.168.4.1/setup";
  cachedSetupUrl="";failQr=true;
  prepareSetupQr(setup);assert(!wifiQr.size && !urlQr.size);
  auto attempts=qrEncodeCalls;prepareSetupQr(setup);assert(qrEncodeCalls==attempts);
  failQr=false;fakeMillis+=5001;prepareSetupQr(setup);assert(wifiQr.size && urlQr.size);
  // Saved mode and brightness load together after a simulated cold restart.
  assert(saveBrightnessPreference(47,true));assert(saveVoiceLayoutPreferences(true,false));
  assert(savePortraitBodyPreference(true));portraitBodyConfig.store(false);
  initialized=false;brightnessConfig.store(100);voiceLayoutConfig.store(0);delete frame;frame=nullptr;
  model.expressions=1;
  assert(begin());assert(brightnessSetting()==47 && potentiometerBrightnessEnabled());
  assert(expressionMask()==parsed);
  assert(portraitBodyEnabled());assert(savePortraitBodyPreference(false));
  assert(voiceFullscreen(false) && !voiceFullscreen(true));
  assert(saveBrightnessPreference(100,false));assert(saveVoiceLayoutPreferences(false,false));
  assert(saveExpressionMask(RobotExpressions::ALL));
  for(bool body:{false,true}){
    geometry(Panel::M200T_ST7789_240X320_7PIN,body?0:1);
    for(uint8_t mood=0;mood<RobotAvatar::EXPRESSION_COUNT;++mood){
      frame->fillScreen(COLOR_BG);
      RobotAvatar::draw(*frame,Scene::Idle,body,8,8,screenWidth-16,screenHeight-16,1200,mood);
      sentPixels=frameCopy();saveFrame(std::string("mood-")+(body?"body-":"eyes-")+std::to_string(mood));
      WebServer thumbnail;
      ExpressionWeb::sendThumbnail(thumbnail,mood,body);
      assert(thumbnail.status==200 && thumbnail.html.size()==54+96*96*3);
      assert(thumbnail.html.substr(0,2)=="BM");
      std::ofstream image(std::string("thumb-")+(body?"body-":"eyes-")+std::to_string(mood)+".bmp",std::ios::binary);
      image.write(thumbnail.html.data(),thumbnail.html.size());
    }
  }
  WebServer server;WebAdmin::PageStream page(server);DisplayWeb::appendControls(page,false);page.finish();
  // Context-selected Speaking expressions remain distinct while the mouth animates.
  geometry(Panel::M200T_ST7789_240X320_7PIN,1);
  std::vector<uint16_t> firstSpeaking;
  for(uint8_t id:{uint8_t{5},uint8_t{22},uint8_t{45}}){
    frame->fillScreen(COLOR_BG);
    RobotAvatar::draw(*frame,Scene::Speaking,true,8,8,screenWidth-16,screenHeight-16,1200,id);
    const auto pixels=frameCopy();
    if(firstSpeaking.empty())firstSpeaking=pixels;else assert(pixels!=firstSpeaking);
  }
  for(Panel panel:{Panel::M200T_ST7789_240X320_7PIN,Panel::ST7735S_128X160_8PIN}){
    for(uint8_t rotation=0;rotation<4;++rotation){
      geometry(panel,rotation);
      Model focus;focus.scene=Scene::Idle;focus.focusActive=true;focus.focusStatus="专注 24:59\n第1轮 · 已完成0次";
      render(focus);saveFrame(std::string(panel==Panel::M200T_ST7789_240X320_7PIN?"large-r":"small-r")+std::to_string(rotation)+"-focus");
      Model music;music.scene=Scene::Idle;music.musicActive=true;music.musicTitle="强军战歌";music.musicArtist="阎维文";
      music.musicState="playing";music.musicLyric="听吧新征程号角吹响";music.musicNextLyric="强军目标召唤在前方";music.musicPositionMs=6000;music.musicDurationMs=114312;
      render(music);saveFrame(std::string(panel==Panel::M200T_ST7789_240X320_7PIN?"large-r":"small-r")+std::to_string(rotation)+"-music");
    }
  }
  std::ofstream("controls.html")<<ADMIN_PAGE_HEAD<<"<section class='panel' data-panel='display'><h2>屏幕与相册</h2>"<<server.html<<"</section>"<<EXPRESSION_SETTINGS_PAGE
    <<"<script>function refreshStandbyPhotoStatus(){}function loadSavedWifi(){}function scanWifi(){}</script>"<<ADMIN_NAV_SCRIPT;
  std::ofstream("setup.html")<<DisplayWeb::SETUP_PAGE;
  WebServer catalog;ExpressionWeb::sendCatalog(catalog,expressionMask());
  assert(catalog.status==200);std::ofstream("expressions.json")<<catalog.html;
  assert(server.html.find("name='wake'")!=std::string::npos);
  assert(server.html.find("name='button'")!=std::string::npos);
  std::cout<<"PASS: 48 expression selections, persistence, preview/restoration, 96 BMP thumbnails; brightness/PWM/ADC, NVS failures, 4 independent voice modes, "<<renders<<" renders across 8 panel rotations, frozen pages and photo restoration, cached QR layout.\n";
}
