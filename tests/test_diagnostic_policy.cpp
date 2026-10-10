#include <cassert>
#include <cstdint>
#include <iostream>

#include "diagnostic_policy.h"

using namespace m5can;

static DiagnosticRequest request(uint32_t id, std::initializer_list<uint8_t> bytes) {
  DiagnosticRequest r{};
  r.can_id = id;
  r.length = static_cast<uint8_t>(bytes.size());
  size_t i = 0;
  for (uint8_t b : bytes) r.data[i++] = b;
  return r;
}

int main() {
  assert(DiagnosticPolicy::allowedHeader(0x18DB33F1U));
  assert(DiagnosticPolicy::allowedHeader(0x18DBEFF1U));
  assert(DiagnosticPolicy::allowedHeader(0x18DA01F1U));
  assert(!DiagnosticPolicy::allowedHeader(0x7DFU));
  assert(!DiagnosticPolicy::allowedHeader(0x18DA10F1U));

  const auto rpm = request(0x18DB33F1U, {0x01, 0x0C});
  const auto speed = request(0x18DB33F1U, {0x01, 0x0D});
  const auto vin = request(0x18DB33F1U, {0x09, 0x02});
  const auto did = request(0x18DBEFF1U, {0x22, 0x20, 0x12});
  assert(DiagnosticPolicy::allowedReadRequest(rpm));
  assert(DiagnosticPolicy::allowedReadRequest(speed));
  assert(DiagnosticPolicy::allowedReadRequest(vin));
  assert(DiagnosticPolicy::allowedReadRequest(did));

  const auto physical = request(0x18DA01F1U, {0x22, 0x20, 0x12});
  assert(DiagnosticPolicy::allowedReadRequest(physical));
  // ECU 01 physical addressing is restricted to approved read-only UDS.
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DA01F1U, {0x01,0x0C})));
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DA01F1U, {0x10,0x03})));
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DA01F1U, {0x2E,0x20,0x12})));
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DA02F1U, {0x22,0x20,0x12})));
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DA10F1U, {0x22,0x20,0x12})));
  assert(DiagnosticPolicy::responseBelongsToRequest(physical, 0x18DAF101U));
  assert(!DiagnosticPolicy::responseBelongsToRequest(physical, 0x18DAF102U));
  assert(!DiagnosticPolicy::responseBelongsToRequest(physical, 0x18DAF10EU));
  assert(DiagnosticPolicy::responseBelongsToRequest(did, 0x18DAF101U));
  assert(DiagnosticPolicy::responseBelongsToRequest(did, 0x18DAF102U));

  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DBEFF1U, {0x10,0x03})));
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DBEFF1U, {0x11,0x01})));
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DBEFF1U, {0x14,0xFF})));
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DBEFF1U, {0x27,0x01})));
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DBEFF1U, {0x28,0x00})));
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DBEFF1U, {0x2E,0x20,0x12})));
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DBEFF1U, {0x31,0x01})));
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DB33F1U, {0x01})));
  assert(!DiagnosticPolicy::allowedReadRequest(request(0x18DB33F1U, {0x01,0x0C,0x00})));

  assert(DiagnosticPolicy::allowedResponseId(0x18DAF110U));
  assert(DiagnosticPolicy::allowedResponseId(0x18DAF116U));
  assert(!DiagnosticPolicy::allowedResponseId(0x18DA10F1U));

  const uint8_t rpm_ok[] = {0x41,0x0C,0x13,0x88};
  const uint8_t speed_other[] = {0x41,0x0D,0x20};
  const uint8_t neg_rpm[] = {0x7F,0x01,0x12};
  const uint8_t did_ok[] = {0x62,0x20,0x12,0xAA};
  const uint8_t did_other[] = {0x62,0x20,0x13,0xAA};
  assert(DiagnosticPolicy::responseMatches(rpm, rpm_ok, sizeof(rpm_ok)));
  assert(!DiagnosticPolicy::responseMatches(rpm, speed_other, sizeof(speed_other)));
  assert(DiagnosticPolicy::responseMatches(rpm, neg_rpm, sizeof(neg_rpm)));
  assert(DiagnosticPolicy::responseMatches(did, did_ok, sizeof(did_ok)));
  assert(!DiagnosticPolicy::responseMatches(did, did_other, sizeof(did_other)));

  assert(DiagnosticPolicy::transactionBudgetMs(60) == 210);
  assert(DiagnosticPolicy::transactionBudgetMs(200) == 350);
  assert(DiagnosticPolicy::transactionBudgetMs(5000) == 1150);

  std::cout << "PASS diagnostic safety policy tests\n";
  return 0;
}
