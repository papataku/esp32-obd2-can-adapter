#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
struct SerialStub {
  void begin(uint32_t){}
  int printf(const char*,...){return 0;}
  void println(const char*){}
};
extern SerialStub Serial;
