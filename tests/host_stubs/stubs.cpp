#include "Arduino.h"
#include "M5Dial.h"
#if __cplusplus < 201703L
bool SerialStub::capture=false;
std::string SerialStub::lines{};
std::string& SerialStub::captured=SerialStub::lines;
int fonts::Font4=0;
int fonts::Font2=0;
#endif
uint32_t g_test_millis=0;
SerialStub Serial;
M5Stub M5;
M5DialStub M5Dial;
