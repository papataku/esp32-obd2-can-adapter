#include <cassert>
#include <iostream>

#include "diagnostic_transaction.h"

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

int main() {
  DiagnosticTransaction transaction;
  transaction.begin(req(0x18DB33F1U, {0x01,0x9A}), 1000);

  auto step = transaction.onFrame(
      frame(0x18DAF101U,1100,
            {0x10,0x08,0x41,0x9A,0x07,0x00,0x3D,0xBD}));
  assert(step.event == TransactionEvent::NeedFlowControl);
  assert(step.flow_control_can_id == 0x18DA01F1U);

  step = transaction.onFrame(
      frame(0x18DAF101U,1200,
            {0x21,0xFF,0x70,0x55,0x55,0x55,0x55,0x55}));
  assert(step.event == TransactionEvent::Completed);
  assert(transaction.result().raw_frame_count == 2);
  assert(transaction.result().response_count == 1);
  assert(transaction.result().tx_to_first_response_us == 100);
  assert(transaction.result().responses[0].length == 8);

  const uint8_t before = transaction.result().raw_frame_count;
  assert(transaction.onFrame(
      frame(0x18DAF102U,900,
            {0x03,0x41,0x9A,0x00})).event ==
         TransactionEvent::Ignored);
  assert(transaction.result().raw_frame_count == before);

  assert(transaction.onFrame(
      frame(0x18DAF102U,1300,
            {0x03,0x41,0x0D,0x00})).event ==
         TransactionEvent::Ignored);
  assert(transaction.result().raw_frame_count == before);

  transaction.begin(
      req(0x18DBEFF1U, {0x22,0x20,0x12}), 2000);
  assert(transaction.onFrame(
      frame(0x18DAF101U,2100,
            {0x10,0x10,0x62,0x20,0x12,1,2,3})).event ==
         TransactionEvent::NeedFlowControl);
  step = transaction.onFrame(
      frame(0x18DAF101U,2200,
            {0x22,4,5,6,7,8,9,10}));
  assert(step.event == TransactionEvent::ProtocolError);
  assert(transaction.result().status == DiagnosticStatus::IsoTpError);

  std::cout << "PASS diagnostic transaction tests\n";
  return 0;
}
