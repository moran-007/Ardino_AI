#pragma once
#include <Adafruit_GFX.h>
#include "SPI.h"
#include <vector>
#define ST77XX_WHITE 0xFFFF
#define ST77XX_BLACK 0x0000
#define INITR_BLACKTAB 0
inline std::vector<uint16_t> sentPixels;
class Adafruit_ST77xx:public Adafruit_GFX {
 public:
  Adafruit_ST77xx(int w,int h):Adafruit_GFX(w,h){}
  void drawPixel(int16_t,int16_t,uint16_t) override {}
  void startWrite() override {sentPixels.clear();}
  void setAddrWindow(int,int,int,int){}
  void writePixels(uint16_t *data,int n,bool){sentPixels.insert(sentPixels.end(),data,data+n);}
};
class Adafruit_ST7735:public Adafruit_ST77xx {
 public: Adafruit_ST7735(SPIClass*,int,int,int):Adafruit_ST77xx(128,160){} void initR(int){}
};
