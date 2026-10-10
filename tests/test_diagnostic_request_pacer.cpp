#include <cassert>
#include <cstdint>
#include <iostream>

#include "diagnostic_request_pacer.h"

int main() {
  m5can::DiagnosticRequestPacer pacer;
  constexpr uint64_t t = 1000000ULL;
  constexpr uint32_t functional_obd = 0x18DB33F1U;
  constexpr uint32_t functional_uds = 0x18DBEFF1U;
  constexpr uint32_t physical_ecu01 = 0x18DA01F1U;

  assert(pacer.remainingUs(functional_obd, t) == 0);
  assert(pacer.remainingUs(functional_uds, t) == 0);
  assert(pacer.remainingUs(physical_ecu01, t) == 0);

  pacer.noteSuccessfulTx(functional_obd, t);
  assert(pacer.remainingUs(functional_obd, t) == 50000);
  assert(pacer.remainingUs(functional_obd, t + 10000) == 40000);
  assert(pacer.remainingUs(functional_obd, t + 49999) == 1);
  assert(pacer.remainingUs(functional_obd, t + 50000) == 0);

  // Crucial requirement: another CAN arbitration ID has NO shared 50ms gap.
  assert(pacer.remainingUs(functional_uds, t + 1) == 0);
  assert(pacer.remainingUs(physical_ecu01, t + 1) == 0);
  pacer.noteSuccessfulTx(physical_ecu01, t + 1000);
  assert(pacer.remainingUs(physical_ecu01, t + 2000) == 49000);
  assert(pacer.remainingUs(functional_obd, t + 2000) == 48000);
  assert(pacer.remainingUs(functional_uds, t + 2000) == 0);

  pacer.noteSuccessfulTx(functional_uds, t + 2000);
  assert(pacer.remainingUs(functional_uds, t + 2001) == 49999);
  assert(pacer.remainingUs(functional_uds, t + 52000) == 0);
  assert(pacer.remainingUs(physical_ecu01, t + 52000) == 0);

  // Failed/invalid TX must never rewrite an approved ID's throttle state.
  pacer.noteSuccessfulTx(0x18DA02F1U, t + 52001);
  assert(pacer.remainingUs(functional_uds, t + 53000) == 0);
  // A decreasing timestamp cannot make the rate limit negative.
  assert(pacer.remainingUs(functional_obd, t - 1) == 50000);

  std::cout << "PASS independent CAN-ID diagnostic pacing\n";
  return 0;
}
