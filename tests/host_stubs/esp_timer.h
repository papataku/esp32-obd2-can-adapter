#pragma once
#include <cstdint>
inline int64_t esp_timer_get_time(){return static_cast<int64_t>(g_test_millis)*1000;}
