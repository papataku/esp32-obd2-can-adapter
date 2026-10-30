#pragma once
// Deterministic, single-thread host stand-in. The B-03 scheduling hook
// injects STOP at the preflight boundary before task-mutex acquisition.
#include "Arduino.h"
using SemaphoreHandle_t = void*;
static const TickType_t portMAX_DELAY = UINT32_MAX;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return reinterpret_cast<void*>(2);}
inline int xSemaphoreTake(SemaphoreHandle_t s,TickType_t){return s?pdTRUE:0;}
inline int xSemaphoreGive(SemaphoreHandle_t s){return s?pdTRUE:0;}
