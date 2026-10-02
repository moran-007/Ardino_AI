#include "robot_expression_web.h"
#include "robot_avatar.h"
#include <esp_heap_caps.h>

namespace ExpressionWeb {
namespace {
class Thumbnail : public GFXcanvas16 {
 public:
  Thumbnail(): GFXcanvas16(96,96,false) {
    // A temporary 18 KiB thumbnail, never a second full screen buffer.
    buffer = static_cast<uint16_t *>(heap_caps_malloc(96U*96U*2U,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    buffer_owned = buffer != nullptr;
  }
};
void le32(uint8_t *p,uint32_t v){for(int i=0;i<4;++i)p[i]=static_cast<uint8_t>(v>>(i*8));}
String jsonText(const char *value) {
  String text = "\"";
  while (*value) {
    if (*value == '"' || *value == '\\') text += '\\';
    text += *value++;
  }
  return text + "\"";
}
}
void sendCatalog(WebServer &server,uint64_t enabled) {
  server.sendHeader("Cache-Control","no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200,"application/json; charset=utf-8","");
  server.sendContent("{\"categories\":[");
  for(uint8_t i=0;i<RobotExpressions::CATEGORY_COUNT;++i) {
    if(i) server.sendContent(",");
    server.sendContent(jsonText(RobotExpressions::CATEGORIES[i]));
  }
  server.sendContent("],\"items\":[");
  for(uint8_t i=0;i<RobotExpressions::COUNT;++i) {
    const auto &entry=RobotExpressions::get(i);
    if(i) server.sendContent(",");
    String row="{\"id\":"+String(i)+",\"name\":"+jsonText(entry.name)+
        ",\"category\":"+String(entry.category)+",\"gesture\":"+
        jsonText(RobotExpressions::GESTURE_LABELS[static_cast<uint8_t>(entry.gesture)])+
        ",\"enabled\":"+((enabled&(uint64_t{1}<<i))?"true":"false")+"}";
    server.sendContent(row);
  }
  server.sendContent("]}");
  server.sendContent("");
}
void sendThumbnail(WebServer &server,uint8_t id,bool body){
  if(id>=RobotExpressions::COUNT){server.send(422,"text/plain","Invalid expression");return;}
  Thumbnail canvas;
  if(!canvas.getBuffer()){server.send(503,"text/plain","Thumbnail memory unavailable");return;}
  canvas.fillScreen(0x0863);
  RobotAvatar::draw(canvas,RobotDisplay::Scene::Idle,body,5,5,86,86,1200,id);
  constexpr uint32_t pixelBytes=96U*96U*3U;
  uint8_t header[54]={};
  header[0]='B';header[1]='M';le32(header+2,pixelBytes+54);le32(header+10,54);
  le32(header+14,40);le32(header+18,96);le32(header+22,96);
  header[26]=1;header[28]=24;le32(header+34,pixelBytes);
  server.sendHeader("Cache-Control","private, max-age=3600");
  server.setContentLength(pixelBytes+sizeof(header));
  server.send(200,"image/bmp","");
  server.sendContent(reinterpret_cast<const char *>(header),sizeof(header));
  uint8_t row[96*3];
  for(int y=95;y>=0;--y){
    for(int x=0;x<96;++x){const uint16_t p=canvas.getBuffer()[y*96+x];
      row[x*3]=(p&31)*255/31;row[x*3+1]=((p>>5)&63)*255/63;row[x*3+2]=(p>>11)*255/31;}
    server.sendContent(reinterpret_cast<const char *>(row),sizeof(row));
  }
}
}
