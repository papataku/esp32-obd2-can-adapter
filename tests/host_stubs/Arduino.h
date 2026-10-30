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
#if defined(M5CAN_TEST_S1_OWNER_BARRIER)
void s1TestOnAuditDelay(uint32_t);
inline void vTaskDelay(uint32_t ms){s1TestOnAuditDelay(ms);g_test_millis+=ms;}
#else
inline void vTaskDelay(uint32_t ms){g_test_millis+=ms;}
#endif
inline void portENTER_CRITICAL(portMUX_TYPE*){}
#if defined(M5CAN_TEST_S1_STOP_RACE)
void s1TestOnCriticalExit(portMUX_TYPE*);
inline void portEXIT_CRITICAL(portMUX_TYPE* m){s1TestOnCriticalExit(m);}
#else
inline void portEXIT_CRITICAL(portMUX_TYPE*){}
#endif
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
#if __cplusplus >= 201703L
  inline static bool capture=false;
  inline static std::string lines{};
  inline static std::string& captured=lines;
#else
  static bool capture;
  static std::string lines;
  static std::string& captured;
#endif
  void begin(uint32_t){}
  int printf(const char* fmt,...) {
    if(!capture)return 0;
    char buffer[1024]{};
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
