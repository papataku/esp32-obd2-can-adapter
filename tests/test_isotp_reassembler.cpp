#include <cassert>
#include <cstdint>
#include <iostream>

#include "isotp_reassembler.h"

using namespace m5can;

static DiagnosticRequest did_request() {
  DiagnosticRequest r{};
  r.can_id = 0x18DBEFF1U;
  r.length = 3;
  r.data[0] = 0x22;
  r.data[1] = 0x20;
  r.data[2] = 0x12;
  return r;
}

int main() {
  IsoTpAssembler iso;
  const auto req = did_request();
  iso.begin(req, 100);

  const uint8_t sf[] = {0x04,0x62,0x20,0x12,0xAA,0,0,0};
  auto e = iso.feed(0x18DAF116U, true, false, sf, 8, 1000);
  assert(e.type == IsoTpEventType::Complete);
  assert(e.message.length == 4 && e.message.data[3] == 0xAA);

  iso.begin(req, 100);
  const uint8_t unrelated_ff[] = {0x10,0x0C,0x62,0x20,0x13,1,2,3};
  e = iso.feed(0x18DAF116U, true, false, unrelated_ff, 8, 2000);
  assert(e.type == IsoTpEventType::None);
  assert(!iso.hasActiveSessions());

  iso.begin(req, 100);
  const uint8_t ff[] = {0x10,0x0C,0x62,0x20,0x12,1,2,3};
  e = iso.feed(0x18DAF116U, true, false, ff, 8, 3000);
  assert(e.type == IsoTpEventType::NeedFlowControl);
  assert(e.flow_control_id == 0x18DA16F1U);
  assert(iso.hasActiveSessions());

  const uint8_t cf1[] = {0x21,4,5,6,7,8,9,0};
  e = iso.feed(0x18DAF116U, true, false, cf1, 8, 4000);
  assert(e.type == IsoTpEventType::Complete);
  assert(e.message.length == 12);
  assert(e.message.data[0] == 0x62 && e.message.data[2] == 0x12);
  assert(e.message.data[11] == 9);
  assert(!iso.hasActiveSessions());

  iso.begin(req, 100);
  e = iso.feed(0x18DAF116U, true, false, ff, 8, 5000);
  assert(e.type == IsoTpEventType::NeedFlowControl);
  const uint8_t bad_cf[] = {0x22,4,5,6,7,8,9,0};
  e = iso.feed(0x18DAF116U, true, false, bad_cf, 8, 6000);
  assert(e.type == IsoTpEventType::SequenceError);
  assert(!iso.hasActiveSessions());

  iso.begin(req, 100);
  e = iso.feed(0x18DAF116U, true, false, ff, 8, 7000);
  assert(e.type == IsoTpEventType::NeedFlowControl);
  e = iso.feed(0x18DAF116U, true, false, ff, 8, 8000);
  assert(e.type == IsoTpEventType::Collision);
  assert(!iso.hasActiveSessions());

  iso.begin(req, 100);
  const uint8_t oversized[] = {0x10,0x61,0x62,0x20,0x12,1,2,3};
  e = iso.feed(0x18DAF116U, true, false, oversized, 8, 9000);
  assert(e.type == IsoTpEventType::Overflow);
  assert(e.flow_control_id == 0x18DA16F1U);

  iso.begin(req, 100);
  const uint8_t pending[] = {0x03,0x7F,0x22,0x78,0,0,0,0};
  e = iso.feed(0x18DAF116U, true, false, pending, 8, 10000);
  assert(e.type == IsoTpEventType::ResponsePending);

  iso.begin(req, 100);
  e = iso.feed(0x18DAF116U, true, false, ff, 8, 11000);
  assert(e.type == IsoTpEventType::NeedFlowControl);
  assert(iso.expire(111000) == 1);
  assert(!iso.hasActiveSessions());

  iso.begin(req, 100);
  e = iso.feed(0x18DAF110U, true, false, ff, 8, 20000);
  assert(e.type == IsoTpEventType::NeedFlowControl);
  e = iso.feed(0x18DAF116U, true, false, ff, 8, 21000);
  assert(e.type == IsoTpEventType::NeedFlowControl);
  e = iso.feed(0x18DAF110U, true, false, cf1, 8, 22000);
  assert(e.type == IsoTpEventType::Complete && e.response_id == 0x18DAF110U);
  e = iso.feed(0x18DAF116U, true, false, cf1, 8, 23000);
  assert(e.type == IsoTpEventType::Complete && e.response_id == 0x18DAF116U);

  std::cout << "PASS ISO-TP reassembly tests\n";
  return 0;
}
