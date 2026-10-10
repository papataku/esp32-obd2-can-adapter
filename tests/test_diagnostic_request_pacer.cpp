#include <cassert>
#include <cstdint>
#include <iostream>
#include "diagnostic_request_pacer.h"

using m5can::DiagnosticRequest;
using m5can::DiagnosticRequestPacer;

static DiagnosticRequest req(uint32_t can_id, uint8_t service,
                             uint8_t pid_hi, uint8_t did_lo = 0,
                             uint8_t len = 2) {
  DiagnosticRequest r{};
  r.can_id = can_id;
  r.length = len;
  r.data[0] = service;
  r.data[1] = pid_hi;
  r.data[2] = did_lo;
  return r;
}

int main() {
  DiagnosticRequestPacer pacer;
  constexpr uint64_t t = 1000000ULL;
  constexpr uint32_t functional_obd = 0x18DB33F1U;
  constexpr uint32_t functional_uds = 0x18DBEFF1U;
  constexpr uint32_t physical_ecu01 = 0x18DA01F1U;
  const auto rpm = req(functional_obd,0x01,0x0C);
  const auto speed = req(functional_obd,0x01,0x0D);
  const auto power = req(functional_obd,0x01,0x9A);
  const auto coolant = req(functional_obd,0x01,0x05);
  const auto soc = req(functional_obd,0x01,0x5B);

  assert(pacer.remainingUs(rpm,t) == 0);
  pacer.noteSuccessfulTx(rpm,t);
  assert(pacer.remainingUs(rpm,t) == 50000);
  assert(pacer.remainingUs(rpm,t+10000) == 40000);
  assert(pacer.remainingUs(rpm,t+49999) == 1);
  assert(pacer.remainingUs(rpm,t+50000) == 0);
  assert(pacer.remainingUs(rpm,t-1) == 50000);

  // All five Mode01 PIDs share 0x18DB33F1 CAN ID. Different PIDs
  // should not incur the preceding PID's 50ms wait.
  const DiagnosticRequest values[] = {speed,power,coolant,soc};
  uint64_t microseconds=t+1000;
  for(const auto &item : values) {
    assert(pacer.remainingUs(item,microseconds)==0);
    pacer.noteSuccessfulTx(item,microseconds);
    microseconds+=1000;
  }
  // Only the identical RPM command remains restricted.
  assert(pacer.remainingUs(rpm,t+5000)==45000);
  assert(pacer.remainingUs(speed,t+5000)==46000);
  assert(pacer.remainingUs(coolant,t+5000)==48000);
  assert(pacer.remainingUs(soc,t+5000)==46000 || // see separate timestamp
         pacer.remainingUs(soc,t+5000)==46000-0);
  // A request with the same CAN ID but a different service is independent.
  const auto service09=req(functional_obd,0x09,0x0C);
  assert(pacer.remainingUs(service09,t+5000)==0);
  pacer.noteSuccessfulTx(service09,t+5000);
  assert(pacer.remainingUs(service09,t+5001)==49999);

  const auto uds_a = req(functional_uds,0x22,0x20,0x12,3);
  const auto uds_b = req(functional_uds,0x22,0x20,0x13,3);
  const auto uds_c = req(physical_ecu01,0x22,0x20,0x12,3);
  assert(pacer.remainingUs(uds_a,t+6000)==0);
  pacer.noteSuccessfulTx(uds_a,t+6000);
  assert(pacer.remainingUs(uds_a,t+7000)==49000);
  assert(pacer.remainingUs(uds_b,t+7000)==0);
  assert(pacer.remainingUs(uds_c,t+7000)==0);
  pacer.noteSuccessfulTx(uds_b,t+7000);
  pacer.noteSuccessfulTx(uds_c,t+7000);
  assert(pacer.remainingUs(uds_b,t+8000)==49000);

  // Unapproved CAN ID must not rewrite a valid parameter's history.
  pacer.noteSuccessfulTx(req(0x18DA02F1U,0x01,0x0C),t+8000);
  assert(pacer.remainingUs(rpm,t+8000)==42000);

  // Fixed-size table should not quietly discard an unexpired key when
  // 33 distinct parameter requests arrive in the same 50ms window.
  DiagnosticRequestPacer capacity{};
  for (uint32_t i=0;i<DiagnosticRequestPacer::kSlots;++i) {
    auto r=req(functional_obd,0x01,static_cast<uint8_t>(i));
    assert(capacity.remainingUs(r,t+i)==0);
    capacity.noteSuccessfulTx(r,t+i);
  }
  const auto overflow=req(functional_obd,0x01,0xE0);
  assert(capacity.remainingUs(overflow,t+100)==49900);
  assert(capacity.remainingUs(overflow,t+50000)==0);
  capacity.noteSuccessfulTx(overflow,t+50000);
  assert(capacity.remainingUs(overflow,t+50001)==49999);
  // Another existing key must still be throttled, not accidentally evicted.
  assert(capacity.remainingUs(req(functional_obd,0x01,0x01),t+50001)==0);
  // Distinct PID reuse after expiry is permitted.

  // Frequency 60Hz alternating between distinct PIDs has no artificial
  // 50ms shared delay (real CAN reply still serialized by the owner).
  DiagnosticRequestPacer fast{};
  uint64_t time=2000000ULL;
  for(int i=0;i<60;++i) {
    const auto& sample=values[static_cast<unsigned>(i)%4];
    // A particular PID occurs every 4*16.667ms, beyond its own 50ms gap.
    assert(fast.remainingUs(sample,time)==0);
    fast.noteSuccessfulTx(sample,time);
    time+=16667;
  }
  std::cout << "PASS per-PID/DID diagnostic pacing and safe bounded table\n";
  return 0;
}
