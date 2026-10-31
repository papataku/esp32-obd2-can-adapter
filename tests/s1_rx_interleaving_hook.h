#pragma once
// Host-only red/green reproduction for S1 audit M-07.
// This preincluded file replaces xQueueSend only in this one test target.
// No hook, wait, queue operation, or instrumented branch enters firmware.
#include <Arduino.h>
int s1TestFrameQueueSend(QueueHandle_t queue,const void* value,TickType_t ticks);
#define xQueueSend s1TestFrameQueueSend
