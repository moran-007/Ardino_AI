#include "robot_avatar.h"

#include <algorithm>
#include <math.h>

namespace RobotAvatar {
namespace {
using RobotDisplay::Scene;
constexpr uint16_t BG = 0x0863, FACE = 0x10C5, CYAN = 0x05FF;
constexpr uint16_t PINK = 0xFAD4, YELLOW = 0xFFE0, GREEN = 0x07E0;

using namespace RobotExpressions;
uint16_t blend(uint16_t a, uint16_t b, int amount) {
  const int inv=100-amount;
  return ((((a>>11)*inv+(b>>11)*amount)/100)<<11) |
         (((((a>>5)&63)*inv+((b>>5)&63)*amount)/100)<<5) |
         (((a&31)*inv+(b&31)*amount)/100);
}
Expression appearance(Scene scene, uint8_t id) {
  Expression e=get(id);
  switch(scene) {
    case Scene::Listening: e=get(0);e.gesture=Gesture::Think;e.eyes=Eyes::Happy;e.mouth=Mouth::Small;break;
    case Scene::Thinking: case Scene::Connecting: case Scene::Updating: e=get(12);e.color=0xB5DF;break;
    case Scene::Speaking: e.mouth=Mouth::Laugh;break;
    case Scene::Alarm: e=get(35);break;
    case Scene::Error: e=get(37);e.eyes=Eyes::Cross;break;
    case Scene::Booting: e=get(0);break;
    default:break;
  }
  return e;
}
void ellipse(Adafruit_GFX &g,int x,int y,int rx,int ry,uint16_t color) {
  rx=std::max(1,rx);ry=std::max(1,ry);
  for(int dy=-ry;dy<=ry;++dy){
    const float f=1.0f-static_cast<float>(dy*dy)/(ry*ry);
    const int dx=static_cast<int>(rx*sqrtf(std::max(0.0f,f)));
    g.drawFastHLine(x-dx,y+dy,dx*2+1,color);
  }
}
void thickLine(Adafruit_GFX &g, int x1, int y1, int x2, int y2, int r, uint16_t color) {
  for (int i = -r; i <= r; ++i) {
    g.drawLine(x1 + i, y1, x2 + i, y2, color);
    g.drawLine(x1, y1 + i, x2, y2 + i, color);
  }
  g.fillCircle(x1, y1, r, color);
  g.fillCircle(x2, y2, r, color);
}

void heart(Adafruit_GFX &g, int x, int y, int r, uint16_t color) {
  g.fillCircle(x - r / 2, y - r / 3, std::max(1, r / 2), color);
  g.fillCircle(x + r / 2, y - r / 3, std::max(1, r / 2), color);
  g.fillTriangle(x - r, y, x + r, y, x, y + r, color);
}

void sparkle(Adafruit_GFX &g,int x,int y,int r,uint16_t color){
  g.fillTriangle(x,y-r,x-r/2,y,x+r/2,y,color);
  g.fillTriangle(x,y+r,x-r/2,y,x+r/2,y,color);
  g.fillTriangle(x-r,y,x,y-r/2,x,y+r/2,color);
  g.fillTriangle(x+r,y,x,y-r/2,x,y+r/2,color);
}

void eyes(Adafruit_GFX &g,Scene scene,int x,int y,int w,int h,uint32_t now,
          const Expression &e,uint16_t background){
  const uint16_t color=blend(e.color,0xFFFF,55);
  const int ew=std::max(4,w*30/100),eh=std::max(3,std::min(h*9/10,ew));
  const int stroke=std::max(1,std::min(ew/12,eh/8));
  const bool blink=now%4200U>=3980U;
  const bool thinking=scene==Scene::Thinking||scene==Scene::Connecting||scene==Scene::Updating;
  const int gaze=(thinking||e.eyes==Eyes::Look||e.eyes==Eyes::Side)
      ? static_cast<int>(sinf(now/650.0f)*ew/8) : static_cast<int>(sinf(now/1500.0f)*ew/18);
  for(int side=-1;side<=1;side+=2){
    const int cx=x+w/2+side*w/4;
    const int cy=y+h/2+(e.eyes==Eyes::Uneven&&side==1?eh/8:0);
    const int r=std::max(2,std::min(ew,eh)/2);
    if(e.eyes==Eyes::Cross){
      thickLine(g,cx-r/2,cy-r/2,cx+r/2,cy+r/2,stroke/2,color);
      thickLine(g,cx-r/2,cy+r/2,cx+r/2,cy-r/2,stroke/2,color);
    } else if(e.eyes==Eyes::Spiral&&!blink){
      int lx=cx,ly=cy;
      for(int i=1;i<48;++i){const float a=i*0.30f;const float rr=r*i/48.0f;
        int nx=cx+cosf(a)*rr,ny=cy+sinf(a)*rr;g.drawLine(lx,ly,nx,ny,color);lx=nx;ly=ny;}
    } else if(e.eyes==Eyes::Heart&&!blink){heart(g,cx,cy,r*3/4,color);
    } else if(e.eyes==Eyes::Star&&!blink){sparkle(g,cx,cy,r,color);
    } else if(e.eyes==Eyes::Money&&!blink){
      ellipse(g,cx,cy,r,r,color);
      g.drawFastVLine(cx,cy-r*2/3,r*4/3,background);
      g.drawLine(cx+r/2,cy-r/3,cx-r/2,cy-r/3,background);
      g.drawLine(cx-r/2,cy-r/3,cx+r/2,cy+r/3,background);
      g.drawLine(cx+r/2,cy+r/3,cx-r/2,cy+r/3,background);
    } else if(blink||e.eyes==Eyes::Closed||e.eyes==Eyes::Sleep||(e.eyes==Eyes::Wink&&side==1)){
      for(int i=-ew/2;i<=ew/2;++i){
        const int arc=e.eyes==Eyes::Sleep?static_cast<int>(eh/7.0f*(1.0f-4.0f*i*i/(ew*ew))):0;
        g.drawFastVLine(cx+i,cy+arc,stroke+1,color);
      }
    } else if(e.eyes==Eyes::Happy||e.eyes==Eyes::Squeeze){
      if(e.eyes==Eyes::Squeeze){
        thickLine(g,cx-side*ew/2,cy-eh/4,cx+side*ew/5,cy,stroke/2,color);
        thickLine(g,cx+side*ew/5,cy,cx-side*ew/2,cy+eh/4,stroke/2,color);
      }else for(int i=-ew/2;i<=ew/2;++i){
        const int arc=static_cast<int>(-eh/3.0f*sqrtf(std::max(0.0f,1.0f-4.0f*i*i/(ew*ew))));
        g.drawFastVLine(cx+i,cy+arc,stroke+1,color);
      }
    }else{
      const int rx=ew/2,ry=std::max(2,eh/2-(e.eyes==Eyes::Half?eh/6:0));
      // Soft outer rim and bright iris; tiny native panels use the same geometry.
      const int glowSize=std::min(6,std::max(2,ew/8));
      if(eh>=14) for(int glow=glowSize;glow>0;--glow)
        ellipse(g,cx,cy,rx+glow,ry+glow,blend(background,color,3+(glowSize-glow)*3));
      ellipse(g,cx,cy,rx,ry,color);
      ellipse(g,cx,cy+std::max(1,ry/5),std::max(1,rx-1),std::max(1,ry*3/4),blend(color,0xFFFF,30));
      const int pupilX=cx+gaze+(e.eyes==Eyes::Side?side*ew/10:ew/12);
      ellipse(g,pupilX,cy-ry/12,std::max(1,rx*3/5),std::max(1,ry*3/5),0x0043);
      ellipse(g,pupilX,cy+ry/6,std::max(1,rx/2),std::max(1,ry/3),blend(0x0043,e.color,25));
      g.fillCircle(pupilX+rx/5,cy-ry/3,std::max(1,rx/6),0xFFFF);
      if(e.eyes==Eyes::Angry){
        const int outer=cx+side*(rx+stroke*2),inner=cx-side*(rx+stroke*2);
        g.fillTriangle(outer,cy-ry-stroke*2,inner,cy-ry-stroke*2,inner,cy,background);
      }else if(e.eyes==Eyes::Half){
        g.fillRect(cx-rx-stroke*2,cy-ry-stroke*2,2*(rx+stroke*2)+1,ry/2+stroke*2,background);
        g.drawFastHLine(cx-rx,cy-ry/2,ew,color);
      }
      if(e.eyes==Eyes::Sad||e.eyes==Eyes::Question){
        g.drawLine(cx-side*ew/2,cy-ry-stroke*2,cx+side*ew/2,cy-ry+(e.eyes==Eyes::Sad?eh/6:-eh/12),color);
      }
      if(e.eyes==Eyes::Tear||e.accent==Accent::Tears){
        const int drop=std::max(1,eh/8);ellipse(g,cx+side*ew/4,cy+ry,drop,drop*2,0x64BF);
      }
    }
  }
}

void mouth(Adafruit_GFX &g,Scene scene,int cx,int cy,int size,uint32_t now,const Expression &e) {
  const int r=std::max(2,size),stroke=std::max(1,r/6);
  const uint16_t lip=blend(e.color,0xFFFF,65);
  if(e.mouth==Mouth::Laugh||e.mouth==Mouth::Open||e.mouth==Mouth::Yawn) {
    const int height=e.mouth==Mouth::Yawn?r:scene==Scene::Speaking?r*2/3+static_cast<int>(sinf(now/160.0f)*r/4):r*2/3;
    ellipse(g,cx,cy,r,std::max(2,height),0x7A6B);
    ellipse(g,cx,cy+height/3,std::max(1,r*4/5),std::max(1,height*2/3),0xFBB4);
    g.drawFastHLine(cx-r/2,cy-height,r,0xEF7D);
  } else if(e.mouth==Mouth::Small||e.mouth==Mouth::Pout) {
    ellipse(g,cx,cy,std::max(1,r/3),std::max(1,r/3),lip);
  } else {
    int lastX=cx-r,lastY=cy;
    for(int i=-r;i<=r;++i) {
      int yy=cy;
      if(e.mouth==Mouth::Flat) yy=cy;
      else if(e.mouth==Mouth::Wobble) yy=cy+static_cast<int>(sinf(i*5.0f/r)*r/4);
      else yy=cy+static_cast<int>((1.0f-static_cast<float>(i*i)/(r*r))*r/2)*(e.mouth==Mouth::Frown?-1:1);
      thickLine(g,lastX,lastY,cx+i,yy,stroke/2,lip);lastX=cx+i;lastY=yy;
    }
    if(e.mouth==Mouth::Tongue)ellipse(g,cx+r/3,cy+r/2,std::max(1,r/3),std::max(1,r/2),PINK);
  }
}
}  // namespace

void draw(Adafruit_GFX &g, Scene scene, bool fullBody, int x, int y, int w, int h,
          uint32_t now, uint8_t expression) {
  if (w < 16 || h < 16) return;
  const Expression e=appearance(scene,expression);
  const uint16_t color=e.color;
  if(!fullBody){
    const int breath=static_cast<int>(sinf(now/850.0f)*h/40);
    eyes(g,scene,x+w/12,y+h/10+breath,w*5/6,h*3/5,now,e,BG);
    mouth(g,scene,x+w/2,y+h*7/10+breath,std::min(w/16,h/10),now,e);
    const int r=std::max(1,std::min(w/30,h/12));
    if(e.accent==Accent::Sparkle||e.accent==Accent::Confetti){
      sparkle(g,x+w/10,y+h/5,r,YELLOW);sparkle(g,x+w*9/10,y+h*4/5,r,color);
    }else if(e.accent==Accent::Hearts){
      heart(g,x+w/10,y+h/5,r,PINK);heart(g,x+w*9/10,y+h*4/5,r,PINK);
    }else if(e.accent==Accent::Blush){
      for(int side:{-1,1})ellipse(g,x+w/2+side*w/4,y+h*3/5,w/22,std::max(1,h/32),blend(BG,PINK,45));
    }else if(e.accent==Accent::Sweat){
      ellipse(g,x+w*9/10,y+h/4,r/2,r,0x64BF);
    }else if(e.accent==Accent::Anger){
      for(int side:{-1,1})g.drawLine(x+w/2+side*w*4/10,y+h/4,x+w/2+side*w*9/20,y+h/6,color);
    }else if(e.accent==Accent::Question){
      g.drawCircle(x+w*9/10,y+h/5,r,color);g.fillCircle(x+w*9/10,y+h/5+r*2,std::max(1,r/3),color);
    }else if(e.accent==Accent::Zzz){
      for(int i=0;i<2;++i){const int zx=x+w*7/10+i*r*3,zy=y+h/6-i*r;
        g.drawFastHLine(zx,zy,r*2,color);g.drawLine(zx+r*2-1,zy,zx,zy+r*2,color);g.drawFastHLine(zx,zy+r*2,r*2,color);}
    }
    if(scene==Scene::Listening||scene==Scene::Speaking){
      for(int i=-4;i<=4;++i){
        const int bar=std::max(2,static_cast<int>(h*(0.05f+0.10f*(1+sinf(now/160.0f+i)))));
        const int bx=x+w/2+i*std::max(2,w/25);
        g.fillRoundRect(bx,y+h*7/8-bar/2,std::max(1,w/80),bar,1,blend(color,0xFFFF,35));
      }
    }else if(scene==Scene::Thinking||scene==Scene::Connecting||scene==Scene::Updating){
      for(int i=0;i<3;++i)g.fillCircle(x+w/2+(i-1)*std::max(4,w/16),y+h*9/10,
          std::max(1,h/30),i==static_cast<int>((now/300)%3)?color:blend(BG,color,25));
    }
    return;
  }

  // A 100 x 154 figure fits the rectangle uniformly; gestures stay within it.
  const float scale = std::min(w / 100.0f, h / 154.0f);
  const int ox = x + (w - static_cast<int>(100 * scale)) / 2;
  const int oy = y + (h - static_cast<int>(154 * scale)) / 2;
  const bool alarm=scene==Scene::Alarm;
  const bool sleepy=e.eyes==Eyes::Sleep||e.gesture==Gesture::Rest;
  const bool joyful=e.gesture==Gesture::Cheer||e.gesture==Gesture::Dance;
  const float wave = sinf(now / (alarm ? 100.0f : 310.0f));
  const float bob = sinf(now / (joyful ? 170.0f : 700.0f)) * (joyful ? 3 : 1.5f);
  const float sway = sinf(now / 1100.0f) * (sleepy ? 3 : 1.5f);
  auto px = [&](float value) { return ox + static_cast<int>((value + sway) * scale); };
  auto py = [&](float value) { return oy + static_cast<int>((value + bob) * scale); };
  auto size = [&](float value) { return std::max(1, static_cast<int>(value * scale)); };
  auto limb = [&](float x1, float y1, float x2, float y2) {
    thickLine(g, px(x1), py(y1), px(x2), py(y2), size(1), color);
  };

  // Body and feet; small alternating foot movement is visible while chatting.
  g.fillRoundRect(px(30), py(78), size(40), size(49), size(13), FACE);
  g.drawRoundRect(px(30), py(78), size(40), size(49), size(13), color);
  const float step = scene == Scene::Speaking || joyful ? wave * 2 : 0;
  limb(40, 126, 38, 139 + step);
  limb(60, 126, 62, 139 - step);
  g.fillRoundRect(px(29), py(137 + step), size(16), size(6), size(3), color);
  g.fillRoundRect(px(56), py(137 - step), size(16), size(6), size(3), color);
  g.fillCircle(px(50), py(99), size(3), color);

  // Large rounded head with brows, cheeks and a mouth, like the references.
  g.fillRoundRect(px(18), py(10), size(64), size(64), size(28), FACE);
  g.drawRoundRect(px(18), py(10), size(64), size(64), size(28), color);
  eyes(g,scene,px(25),py(25),size(50),size(28),now,e,FACE);
  switch(e.mouth){
    case Mouth::Frown: limb(43,63,50,59);limb(50,59,57,63);break;
    case Mouth::Flat: limb(44,61,56,61);break;
    case Mouth::Small: g.drawCircle(px(50),py(61),size(2),color);break;
    case Mouth::Kiss: limb(47,58,53,60);limb(53,60,48,62);limb(48,62,53,64);break;
    case Mouth::Pout: ellipse(g,px(50),py(61),size(7),size(3),color);break;
    case Mouth::Wobble: for(int i=0;i<4;++i)limb(42+i*4,60+(i%2)*3,46+i*4,60+((i+1)%2)*3);break;
    case Mouth::Grin:
      g.fillRoundRect(px(41),py(57),size(18),size(9),size(3),color);
      for(int i=0;i<3;++i)g.drawFastVLine(px(45+i*4),py(58),size(6),FACE);break;
    case Mouth::Tongue:
      limb(42,58,46,61);limb(46,61,57,61);
      g.fillRoundRect(px(50),py(61),size(7),size(8),size(3),PINK);break;
    case Mouth::Laugh:case Mouth::Open:case Mouth::Yawn:{
      const int mh=size(e.mouth==Mouth::Yawn?13:scene==Scene::Speaking?7+3*wave:10);
      g.fillRoundRect(px(43),py(55),size(14),mh,size(5),color);
      if(mh>5)ellipse(g,px(50),py(55)+mh-2,size(5),size(2),PINK);break;}
    default:limb(43,58,47,62);limb(47,62,53,62);limb(53,62,57,58);break;
  }
  g.fillRoundRect(px(27), py(52), size(9), size(3), size(1), PINK);
  g.fillRoundRect(px(64), py(52), size(9), size(3), size(1), PINK);

  if(scene==Scene::Listening){
    limb(31,85,22,103);limb(22,103,26,112);
    limb(69,85,87,66);limb(87,66,83,48+wave*2);
  }else switch(e.gesture){
    case Gesture::Think:
      limb(31,86,19,72);limb(19,72,30,58);limb(69,87,74,105);limb(74,105,50,107);break;
    case Gesture::Cheer:case Gesture::Stretch:
      limb(31,85,12,72);limb(12,72,9,48+wave*5);limb(69,85,88,72);limb(88,72,91,48-wave*5);break;
    case Gesture::Clap:
      limb(31,85,33,105);limb(33,105,47+wave*3,92);limb(69,85,67,105);limb(67,105,53-wave*3,92);break;
    case Gesture::Heart:
      limb(31,85,29,102);limb(29,102,47,96);limb(69,85,71,102);limb(71,102,53,96);
      heart(g,px(50),py(92),size(6),PINK);break;
    case Gesture::Hug:
      limb(31,85,15,85-wave*4);limb(15,85-wave*4,9,76);limb(69,85,85,85+wave*4);limb(85,85+wave*4,91,76);break;
    case Gesture::Shy:case Gesture::Surprise:
      limb(31,85,20,75);limb(20,75,31,55);limb(69,85,80,75);limb(80,75,69,55);break;
    case Gesture::Point:
      limb(31,85,24,108);limb(69,85,91,88);limb(91,88,94,81);break;
    case Gesture::Shrug:
      limb(31,85,18,100);limb(18,100,10,82+wave*2);limb(69,85,82,100);limb(82,100,90,82-wave*2);break;
    case Gesture::Rest:
      limb(31,85,22,114);limb(69,85,78,114);break;
    case Gesture::Wipe:
      limb(31,85,18,76);limb(18,76,30+wave*3,50);limb(69,85,78,114);break;
    case Gesture::CrossArms:
      limb(31,85,26,105);limb(26,105,66,100);limb(69,85,74,110);limb(74,110,34,104);break;
    case Gesture::Shake:
      limb(31,85,17,90);limb(17,90,15,71+wave*5);limb(69,85,83,90);limb(83,90,85,71-wave*5);
      g.fillCircle(px(15),py(71+wave*5),size(3),color);g.fillCircle(px(85),py(71-wave*5),size(3),color);break;
    case Gesture::Dance:
      limb(31,85,11,85+wave*15);limb(69,85,89,85-wave*15);break;
    case Gesture::Thumbs:case Gesture::Peace:
      limb(31,85,22,108);limb(69,85,86,80);limb(86,80,86,65);
      if(e.gesture==Gesture::Thumbs){g.drawRoundRect(px(82),py(62),size(9),size(12),size(3),color);limb(84,65,84,56);}
      else{limb(86,65,82,54);limb(86,65,91,54);}break;
    default:
      limb(31,85,17,98);limb(17,98,10,93+wave*3);limb(69,85,86,74);limb(86,74,90+wave*3,58+wave*7);break;
  }
  // Accents occupy the free space beside the head, not the text/status region.
  if(e.accent==Accent::Hearts){heart(g,px(9),py(27),size(5),PINK);heart(g,px(91),py(16),size(4),PINK);}
  else if(e.accent==Accent::Sparkle||e.accent==Accent::Confetti){sparkle(g,px(9),py(27),size(5),YELLOW);sparkle(g,px(91),py(16),size(4),color);}
  else if(e.accent==Accent::Sweat){ellipse(g,px(84),py(29),size(2),size(5),0x64BF);}
  else if(e.accent==Accent::Anger){limb(84,18,84,27);limb(84,27,91,27);limb(89,15,89,22);limb(89,22,94,22);}
  else if(e.accent==Accent::Zzz){limb(86,15,93,15);limb(93,15,86,24);limb(86,24,93,24);}
  else if(e.accent==Accent::Question){g.drawCircle(px(89),py(19),size(4),color);g.fillCircle(px(89),py(29),size(1),color);}
  if(scene==Scene::Thinking||scene==Scene::Connecting||scene==Scene::Updating){
    for(int i=0;i<3;++i)g.fillCircle(px(40+i*10),py(149),size(1),i==static_cast<int>((now/300)%3)?color:FACE);
  }

}
}  // namespace RobotAvatar
