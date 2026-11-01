#pragma once
#include "../../host_stubs/Arduino.h"
int pr70QueueSend(QueueHandle_t,const void*,TickType_t);
int pr70QueueReceive(QueueHandle_t,void*,TickType_t);
#define xQueueSend pr70QueueSend
#define xQueueReceive pr70QueueReceive
