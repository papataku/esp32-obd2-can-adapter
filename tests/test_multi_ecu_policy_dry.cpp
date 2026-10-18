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


static void testAbnormalTraffic(uint32_t header,
                                uint32_t reply, uint32_t otherReply) {
  const auto request = readDid(header);
  DiagnosticTransaction tx;

  // A stale frame or an unrelated ECU's negative response never belongs to us.
  tx.begin(request, 1'000'000);
  auto step = tx.onFrame(frame(reply, 999'999, {0x03, 0x7F, 0x22, 0x31}));
  assert(step.event == TransactionEvent::Ignored);
  step = tx.onFrame(frame(otherReply, 1'000'001, {0x03, 0x7F, 0x22, 0x78}));
  assert(step.event == TransactionEvent::Ignored);
  assert(tx.result().raw_frame_count == 0);

  // Impossible Classical CAN DLC must not overflow an eight-byte raw
  // frame buffer or be interpreted as a First Frame.
  auto oversizedDlc = frame(reply, 1'000'005,
                           {0x10, 0x0A, 0x62, 0x20, 0x12, 1, 2, 3});
  oversizedDlc.dlc = 9;
  step = tx.onFrame(oversizedDlc);
  assert(step.event == TransactionEvent::Ignored);
  assert(tx.result().raw_frame_count == 0);
  oversizedDlc.dlc = 255;
  step = tx.onFrame(oversizedDlc);
  assert(step.event == TransactionEvent::Ignored);
  assert(!tx.hasActiveIsoTp());

  // 7F 22 without its NRC is malformed and must not be marked complete.
  step = tx.onFrame(frame(reply, 1'000'008, {0x02, 0x7F, 0x22}));
  assert(step.event == TransactionEvent::Ignored);
  assert(tx.result().response_count == 0);

  // NRC 0x78 is pending from our ECU, not a completed usable DID.
  step = tx.onFrame(frame(reply, 1'000'010, {0x03, 0x7F, 0x22, 0x78}));
  assert(step.event == TransactionEvent::ResponsePending);
  assert(step.flow_control_can_id == 0);
  assert(tx.result().response_count == 0);

  // Another tester's 7F with the wrong service must be ignored.
  step = tx.onFrame(frame(reply, 1'000'020, {0x03, 0x7F, 0x2E, 0x31}));
  assert(step.event == TransactionEvent::Ignored);
  step = tx.onFrame(frame(reply, 1'000'030, {0x03, 0x7F, 0x22, 0x31}));
  assert(step.event == TransactionEvent::Completed);
  assert(step.flow_control_can_id == 0);
  assert(tx.result().responses[0].can_id == reply);

  // Too-small, too-large and malformed FFs must not emit Flow Control.
  tx.begin(request, 2'000'000);
  for (auto length : {0x07, 0x61}) {
    const auto firstFrame = frame(reply, 2'000'010,
        {0x10, static_cast<uint8_t>(length), 0x62, 0x20, 0x12, 1, 2, 3});
    step = tx.onFrame(firstFrame);
    assert(step.event == TransactionEvent::ProtocolError);
    assert(step.flow_control_can_id == 0);
    assert(!tx.hasActiveIsoTp());
    tx.begin(request, 2'000'000);
  }
  auto standardFrame = frame(reply, 2'000'030,
                             {0x10, 0x0A, 0x62, 0x20, 0x12, 1, 2, 3});
  standardFrame.extended = false;
  assert(tx.onFrame(standardFrame).event == TransactionEvent::Ignored);
  standardFrame.extended = true;
  standardFrame.rtr = true;
  assert(tx.onFrame(standardFrame).event == TransactionEvent::Ignored);
  assert(!tx.hasActiveIsoTp());

  // Strict CF sequence checking and no old result carried into next request.
  tx.begin(request, 3'000'000);
  step = tx.onFrame(frame(reply, 3'000'010,
                          {0x10, 0x0A, 0x62, 0x20, 0x12, 1, 2, 3}));
  assert(step.event == TransactionEvent::NeedFlowControl);
  assert(step.flow_control_can_id == header);
  step = tx.onFrame(frame(reply, 3'000'020, {0x22, 4, 5, 6, 7}));
  assert(step.event == TransactionEvent::ProtocolError);
  assert(step.flow_control_can_id == 0);
  assert(!tx.hasActiveIsoTp());
  tx.begin(request, 3'100'000);
  assert(tx.result().response_count == 0);
  assert(tx.result().raw_frame_count == 0);
  assert(tx.onFrame(frame(reply, 3'100'010, {0x21, 4, 5, 6, 7})).event ==
         TransactionEvent::Ignored);

  // A timed-out FF must not accept late CFs or issue another FC.
  tx.begin(request, 4'000'000);
  step = tx.onFrame(frame(reply, 4'000'010,
                          {0x10, 0x0A, 0x62, 0x20, 0x12, 1, 2, 3}));
  assert(step.event == TransactionEvent::NeedFlowControl);
  assert(tx.expireIsoTp(4'200'000) == 1);
  assert(!tx.hasActiveIsoTp());
  step = tx.onFrame(frame(reply, 4'200'010, {0x21, 4, 5, 6, 7}));
  assert(step.event == TransactionEvent::Ignored);
  assert(step.flow_control_can_id == 0);

  // A second matching FF during an active assembly is a collision, never FC.
  tx.begin(request, 5'000'000);
  const auto ff = frame(reply, 5'000'010,
                        {0x10, 0x0A, 0x62, 0x20, 0x12, 1, 2, 3});
  assert(tx.onFrame(ff).event == TransactionEvent::NeedFlowControl);
  step = tx.onFrame(frame(reply, 5'000'020,
                          {0x10, 0x0A, 0x62, 0x20, 0x12, 1, 2, 3}));
  assert(step.event == TransactionEvent::ProtocolError);
  assert(step.flow_control_can_id == 0);
  assert(!tx.hasActiveIsoTp());
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
    testAbnormalTraffic(headers[i], expectedReply, unrelatedReply);
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
