#pragma once
// C++11 host implementation backed by a real task mutex, not an always-pass
// semaphore stub. One global serializes the test monitor's TX and STOP.
#include "Arduino.h"
#include <mutex>
using SemaphoreHandle_t = std::mutex*;
static const TickType_t portMAX_DELAY = UINT32_MAX;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){
  static std::mutex tx_mutex;
  return &tx_mutex;
}
inline int xSemaphoreTake(SemaphoreHandle_t s,TickType_t){
  if(!s)return 0;
  s->lock();return pdTRUE;
}
inline int xSemaphoreGive(SemaphoreHandle_t s){
  if(!s)return 0;
  s->unlock();return pdTRUE;
}
