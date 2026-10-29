#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <mutex>
#include <thread>
#include <deque>
#include <vector>
using TickType_t=uint32_t;
struct Q {size_t capacity,size;std::deque<std::vector<uint8_t>> entries;};
using QueueHandle_t=Q*;
using TaskHandle_t=void*;
struct portMUX_TYPE{std::recursive_mutex lock;};
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
inline void portENTER_CRITICAL(portMUX_TYPE* m){m->lock.lock();}
inline void portEXIT_CRITICAL(portMUX_TYPE* m){m->lock.unlock();}
inline QueueHandle_t xQueueCreate(size_t c,size_t s){return new Q{c,s,{}};}
inline void vQueueDelete(QueueHandle_t){}
inline int xQueueSend(QueueHandle_t q,const void* p,TickType_t){if(q->entries.size()==q->capacity)return 0;auto b=static_cast<const uint8_t*>(p);q->entries.emplace_back(b,b+q->size);return pdTRUE;}
inline int xQueueReceive(QueueHandle_t q,void* p,TickType_t){if(q->entries.empty())return 0;memcpy(p,q->entries.front().data(),q->size);q->entries.pop_front();return pdTRUE;}
inline void xQueueReset(QueueHandle_t q){q->entries.clear();}
inline size_t uxQueueMessagesWaiting(QueueHandle_t q){return q->entries.size();}
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
  int availableForWrite(){return 4096;}
  size_t write(const uint8_t* p,size_t n){lines.append(reinterpret_cast<const char*>(p),n);return n;}
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

  void flush(){}
  int available(){return 0;}
  int read(){return -1;}
};
extern SerialStub Serial;
