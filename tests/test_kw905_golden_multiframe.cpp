#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "diagnostic_transaction.h"
#include "elm_response_formatter.h"

using namespace m5can;

static DiagnosticRequest req(
    uint32_t id, std::initializer_list<uint8_t> data) {
  DiagnosticRequest r{};
  r.can_id = id;
  r.length = static_cast<uint8_t>(data.size());
  size_t i = 0;
  for (uint8_t b : data) r.data[i++] = b;
  return r;
}

static TransactionFrame frame(
    uint32_t id, uint64_t ts,
    std::initializer_list<uint8_t> data) {
  TransactionFrame f{};
  f.can_id = id;
  f.timestamp_us = ts;
  f.dlc = static_cast<uint8_t>(data.size());
  size_t i = 0;
  for (uint8_t b : data) f.data[i++] = b;
  return f;
}

static std::vector<std::string> lines(
    const DiagnosticResult& result) {
  std::vector<std::string> output;
  char line[128]{};
  for (uint8_t i = 0; i < result.raw_frame_count; ++i) {
    assert(ElmResponseFormatter::formatRawFrame(
        result.raw_frames[i],true,false,line,sizeof(line)));
    output.emplace_back(line);
  }
  return output;
}

int main() {
  DiagnosticTransaction transaction;

  transaction.begin(
      req(0x18DB33F1U,{0x01,0x9A}),1000);
  assert(transaction.onFrame(
      frame(0x18DAF101U,1100,
            {0x10,0x08,0x41,0x9A,0x07,0x00,0x3D,0xBD}))
             .event == TransactionEvent::NeedFlowControl);
  assert(transaction.onFrame(
      frame(0x18DAF101U,1200,
            {0x21,0xFF,0x70,0x55,0x55,0x55,0x55,0x55}))
             .event == TransactionEvent::Completed);

  const auto hybrid = lines(transaction.result());
  assert(hybrid == std::vector<std::string>({
      "18DAF1011008419A07003DBD",
      "18DAF10121FF705555555555"}));

  transaction.begin(
      req(0x18DBEFF1U,{0x22,0x20,0x12}),2000);
  assert(transaction.onFrame(
      frame(0x18DAF101U,2100,
            {0x10,0x27,0x62,0x20,0x12,0x70,0x00,0x0F}))
             .event == TransactionEvent::NeedFlowControl);
  transaction.onFrame(
      frame(0x18DAF101U,2200,
            {0x21,0x00,0x00,0x1A,0x05,0xAA,0x00,0x00}));
  transaction.onFrame(
      frame(0x18DAF101U,2300,
            {0x22,0,0,0,0,0,0,0}));
  transaction.onFrame(
      frame(0x18DAF101U,2400,
            {0x23,0,0,0,0,0,0,0}));
  transaction.onFrame(
      frame(0x18DAF101U,2500,
            {0x24,0x0E,0x4D,0x0E,0xBE,0,0,0}));
  assert(transaction.onFrame(
      frame(0x18DAF101U,2600,
            {0x25,0,0,0,0,0,0x55,0x55}))
             .event == TransactionEvent::Completed);

  const auto did = lines(transaction.result());
  const std::vector<std::string> expected = {
      "18DAF101102762201270000F",
      "18DAF1012100001A05AA0000",
      "18DAF1012200000000000000",
      "18DAF1012300000000000000",
      "18DAF101240E4D0EBE000000",
      "18DAF1012500000000005555",
  };
  assert(did == expected);
  assert(transaction.result().responses[0].length == 0x27);
  assert(transaction.result().responses[0].data[0] == 0x62);
  assert(transaction.result().responses[0].data[1] == 0x20);
  assert(transaction.result().responses[0].data[2] == 0x12);

  std::cout << "PASS KW905 multiframe golden-output tests\n";
  return 0;
}
