#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <iostream>

#include "diagnostic_transaction.h"

// This test is exclusively for the artificial, physical-TX-disabled build.
#if !defined(M5CAN_MULTI_ECU_POLICY_DRY_BENCH) || !defined(M5CAN_CAN_MODE_DRY_RUN)
#error "Multi-ECU routing test must never run in a CAN-TX-enabled build"
#endif

using namespace m5can;

static DiagnosticRequest readDid(uint32_t header) {
  DiagnosticRequest r{};
  r.can_id = header;
  r.length = 3;
  r.data[0] = 0x22;
  r.data[1] = 0x20;
  r.data[2] = 0x12;
  return r;
}

static TransactionFrame frame(uint32_t id, uint64_t micros,
                              std::initializer_list<uint8_t> bytes) {
  TransactionFrame f{};
  f.can_id = id;
  f.timestamp_us = micros;
  f.extended = true;
  f.dlc = static_cast<uint8_t>(bytes.size());
  size_t i = 0;
  for (const auto b : bytes) f.data[i++] = b;
  return f;
}

int main() {
  const uint32_t headers[] = {
    0x18DA01F1U, 0x18DA02F1U, 0x18DA06F1U,
    0x18DA0EF1U, 0x18DAEFF1U
  };
  const uint8_t sources[] = {0x01, 0x02, 0x06, 0x0E, 0xEF};
  for (size_t i = 0; i < 5; ++i) {
    const DiagnosticRequest req = readDid(headers[i]);
    const uint32_t expectedReply = 0x18DAF100U | sources[i];
    assert(DiagnosticPolicy::allowedHeader(headers[i]));
    assert(DiagnosticPolicy::allowedReadRequest(req));
    assert(DiagnosticPolicy::responseBelongsToRequest(req, expectedReply));

    // Only UDS ReadDataByIdentifier is allowed in a physical address.
    auto wrong = req;
    wrong.data[0] = 0x2E;
    assert(!DiagnosticPolicy::allowedReadRequest(wrong));
    wrong.data[0] = 0x10;
    assert(!DiagnosticPolicy::allowedReadRequest(wrong));
    wrong.data[0] = 0x27;
    assert(!DiagnosticPolicy::allowedReadRequest(wrong));
    wrong = req;
    wrong.length = 2;
    assert(!DiagnosticPolicy::allowedReadRequest(wrong));

    const auto unrelatedReply =
        0x18DAF100U | sources[(i + 1) % 5];
    assert(!DiagnosticPolicy::responseBelongsToRequest(req, unrelatedReply));

    DiagnosticTransaction transaction;
    transaction.begin(req, 1'000'000);
    // Valid first frame from another ECU must not start a session or trigger FC.
    auto result = transaction.onFrame(
        frame(unrelatedReply, 1'000'050,
              {0x10, 0x0A, 0x62, 0x20, 0x12, 1, 2, 3}));
    assert(result.event == TransactionEvent::Ignored);
    assert(result.flow_control_can_id == 0);
    assert(!transaction.hasActiveIsoTp());

    // Same ECU but wrong DID must also be ignored.
    result = transaction.onFrame(
        frame(expectedReply, 1'000'070,
              {0x10, 0x0A, 0x62, 0x20, 0x13, 1, 2, 3}));
    assert(result.event == TransactionEvent::Ignored);
    assert(!transaction.hasActiveIsoTp());

    result = transaction.onFrame(
        frame(expectedReply, 1'000'100,
              {0x10, 0x0A, 0x62, 0x20, 0x12, 1, 2, 3}));
    assert(result.event == TransactionEvent::NeedFlowControl);
    // Check exact reverse fixed-address CAN ID for Flow Control.
    assert(result.flow_control_can_id == headers[i]);
    assert(transaction.hasActiveIsoTp());

    result = transaction.onFrame(
        frame(unrelatedReply, 1'000'150,
              {0x21, 4, 5, 6, 7}));
    assert(result.event == TransactionEvent::Ignored);

    result = transaction.onFrame(
        frame(expectedReply, 1'000'200,
              {0x21, 4, 5, 6, 7}));
    assert(result.event == TransactionEvent::Completed);
    assert(transaction.result().response_count == 1);
    assert(transaction.result().responses[0].can_id == expectedReply);
    assert(transaction.result().responses[0].length == 10);
    assert(!transaction.hasActiveIsoTp());
  }

  // A single wildcard can never slip into the experimental whitelist.
  for (auto id : {0x18DA03F1U, 0x18DA07F1U, 0x18DA10F1U,
                  0x18DAFFF1U, 0x18DA02F2U, 0x7E0U}) {
    assert(!DiagnosticPolicy::allowedHeader(id));
    assert(!DiagnosticPolicy::allowedReadRequest(readDid(id)));
  }
  std::cout << "PASS dry-run multi-ECU physical UDS22 routing/ISO-TP safety\n";
  return 0;
}
