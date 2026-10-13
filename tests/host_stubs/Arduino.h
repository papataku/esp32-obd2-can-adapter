#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
using TickType_t=uint32_t;
using QueueHandle_t=void*;
using TaskHandle_t=void*;
struct portMUX_TYPE{};
#define portMUX_INITIALIZER_UNLOCKED portMUX_TYPE{}
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define configMAX_PRIORITIES 25
extern uint32_t g_test_millis;
inline uint32_t millis(){return g_test_millis;}
inline uint32_t micros(){return g_test_millis*1000U;}
inline void delay(uint32_t ms){g_test_millis+=ms;}
inline void taskYIELD(){}
inline void vTaskDelay(uint32_t ms){g_test_millis+=ms;}
inline void portENTER_CRITICAL(portMUX_TYPE*){}
inline void portEXIT_CRITICAL(portMUX_TYPE*){}
inline QueueHandle_t xQueueCreate(size_t,size_t){return reinterpret_cast<void*>(1);}
inline void vQueueDelete(QueueHandle_t){}
inline int xQueueSend(QueueHandle_t,const void*,TickType_t){return pdTRUE;}
inline int xQueueReceive(QueueHandle_t,void*,TickType_t){return 0;}
inline int xTaskCreatePinnedToCore(void(*)(void*),const char*,uint32_t,void*,int,TaskHandle_t* h,int){*h=reinterpret_cast<void*>(1);return pdPASS;}
inline void vTaskDelete(TaskHandle_t){}
inline int xPortGetCoreID(){return 1;}
inline const char* pcTaskGetName(TaskHandle_t){return "host-test";}
inline unsigned uxTaskPriorityGet(TaskHandle_t){return 4;}
inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t){return 4096;}
struct SerialStub {
  inline static bool capture=false;
  inline static std::string lines{};
  inline static std::string& captured=lines;
  void begin(uint32_t){}
  int printf(const char* fmt,...) {
    if(!capture)return 0;
    char buffer[512]{};
    va_list args;va_start(args,fmt);
    const int n=std::vsnprintf(buffer,sizeof(buffer),fmt,args);
    va_end(args);
    if(n>0)lines+=buffer;
    return n;
  }
  void println(const char* value) {
    if(capture) {lines+=(value?value:"");lines+='\n';}
  }
  size_t write(const uint8_t*,size_t n){return n;}
  void flush(){}
  int available(){return 0;}
  int read(){return -1;}
};
extern SerialStub Serial;
