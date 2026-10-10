#include <cassert>
#include <cstring>
#include <iostream>

#include "elm_response_formatter.h"

using namespace m5can;

static DiagnosticRawFrame frame(
    uint32_t id, std::initializer_list<uint8_t> data) {
  DiagnosticRawFrame f{};
  f.can_id = id;
  f.dlc = static_cast<uint8_t>(data.size());
  size_t i = 0;
  for (uint8_t b : data) f.data[i++] = b;
  return f;
}

int main() {
  char out[128]{};

  const auto rpm =
      frame(0x18DAF1EFU,{0x04,0x41,0x0C,0x16,0x34,0,0,0});
  assert(ElmResponseFormatter::formatRawFrame(
      rpm,true,false,out,sizeof(out)) > 0);
  assert(std::strcmp(out,"18DAF1EF04410C1634") == 0);

  const auto ff =
      frame(0x18DAF101U,{0x10,0x08,0x41,0x9A,0x07,0x00,0x3D,0xBD});
  ElmResponseFormatter::formatRawFrame(
      ff,true,false,out,sizeof(out));
  assert(std::strcmp(out,"18DAF1011008419A07003DBD") == 0);

  const auto cf =
      frame(0x18DAF101U,{0x21,0xFF,0x70,0x55,0x55,0x55,0x55,0x55});
  ElmResponseFormatter::formatRawFrame(
      cf,true,false,out,sizeof(out));
  assert(std::strcmp(out,"18DAF10121FF705555555555") == 0);

  ElmResponseFormatter::formatRawFrame(
      rpm,true,true,out,sizeof(out));
  assert(std::strcmp(out,"18DAF1EF 04 41 0C 16 34") == 0);

  std::cout << "PASS ELM raw response formatter golden tests\n";
  return 0;
}
