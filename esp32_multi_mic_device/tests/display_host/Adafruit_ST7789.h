#pragma once
#include "Adafruit_ST7735.h"
class Adafruit_ST7789:public Adafruit_ST77xx {
 public: Adafruit_ST7789(SPIClass*,int,int,int):Adafruit_ST77xx(240,320){} void init(int,int){}
};
