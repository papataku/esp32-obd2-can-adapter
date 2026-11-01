#pragma once
#include "Arduino.h"
#include <mutex>
using SemaphoreHandle_t=std::mutex*;
static const TickType_t portMAX_DELAY=UINT32_MAX;
void pr70Boundary(const char*);
extern unsigned pr70GateDepth;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){static std::mutex mutex;return &mutex;}
inline int xSemaphoreTake(SemaphoreHandle_t m,TickType_t){
  pr70Boundary("before gate");m->lock();++pr70GateDepth;return pdTRUE;
}
inline int xSemaphoreGive(SemaphoreHandle_t m){
  --pr70GateDepth;m->unlock();pr70Boundary("after gate");return pdTRUE;
}
