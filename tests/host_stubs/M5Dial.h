#pragma once
#include "Arduino.h"
#define TFT_BLACK 0
#define TFT_WHITE 1
#define TFT_GREEN 2
#define TFT_LIGHTGREY 3
#define TFT_RED 4
#define middle_center 0
namespace fonts { inline int Font4=0; inline int Font2=0; }
struct DisplayStub{
  void setRotation(int){}
  void fillScreen(int){}
  void fillRect(int,int,int,int,int){}
  void setTextDatum(int){}
  void setTextColor(int,int){}
  void drawString(const char*,int,int,const int*){}
};
struct M5Config{int serial_baudrate=0;bool clear_display=false;bool output_power=false;};
struct M5Stub{M5Config config(){return {};}};
extern M5Stub M5;
struct TouchDetailStub {
  int32_t x=0, y=0;
  bool pressed=false;
  bool wasPressed() const {return pressed;}
};
struct TouchStub {
  TouchDetailStub detail{};
  TouchDetailStub getDetail() const {return detail;}
};
struct M5DialStub {
  DisplayStub Display;
  TouchStub Touch;
  void begin(M5Config,bool=false,bool=false){}
  void update(){}
};
extern M5DialStub M5Dial;
