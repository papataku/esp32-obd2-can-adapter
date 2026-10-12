#include <cassert>
#include <cstdint>
#include <iostream>

#include "diagnostic_pid_cache.h"

using namespace m5can;

static DiagnosticRequest read(uint8_t pid) {
  DiagnosticRequest q{};
  q.can_id=0x18DB33F1U;q.length=2;q.data[0]=1;q.data[1]=pid;
  return q;
}
static DiagnosticResult reply(uint8_t pid,uint8_t val,uint32_t ecu=0x18DAF101U) {
  DiagnosticResult r{};r.status=DiagnosticStatus::Ok;
  r.response_count=1;
  r.responses[0].can_id=ecu;
  r.responses[0].length=3;
  r.responses[0].data[0]=0x41;r.responses[0].data[1]=pid;
  r.responses[0].data[2]=val;
  r.raw_frame_count=1;
  r.raw_frames[0].can_id=ecu;
  r.raw_frames[0].dlc=8;
  r.raw_frames[0].data[0]=3;
  r.raw_frames[0].data[1]=0x41;
  r.raw_frames[0].data[2]=pid;
  r.raw_frames[0].data[3]=val;
  return r;
}
int main() {
  DiagnosticPidCache cache;
  const auto speed=read(0x0D),coolant=read(0x05),rpm=read(0x0C);
  DiagnosticResult out{};
  constexpr uint64_t t=1000000;
  assert(!cache.lookup(speed,t,out));
  assert(cache.rememberSingle(speed,reply(0x0D,80),t));
  assert(!cache.lookup(coolant,t,out));
  assert(cache.lookup(speed,t,out) && out.responses[0].data[2]==80);
  assert(out.transaction_us==0 && out.tx_to_first_response_us==0);
  assert(cache.lookup(speed,t+99999,out));
  assert(!cache.lookup(speed,t+100000,out));
  assert(!cache.lookup(speed,t-1,out));
  assert(!cache.lookup(speed,t+1,out,100001)); // Max-age cap not bypassed

  // A malformed response invalidates only that parameter.
  assert(cache.rememberSingle(coolant,reply(0x05,90),t));
  DiagnosticResult negative=reply(0x0D,83);
  negative.responses[0].data[0]=0x7F;
  assert(!cache.rememberSingle(speed,negative,t+1000));
  assert(!cache.lookup(speed,t+2000,out));
  assert(cache.lookup(coolant,t+2000,out));
  assert(out.responses[0].data[2]==90);

  // Functional request responses cannot be mixed from multiple ECUs.
  auto ambiguous=reply(0x0C,12);
  ambiguous.responses[1]=reply(0x0C,13,0x18DAF102U).responses[0];
  ambiguous.response_count=2;
  assert(!cache.rememberSingle(rpm,ambiguous,t));
  assert(!cache.lookup(rpm,t+1000,out));

  DiagnosticRawFrame frames[2]{};
  frames[0].can_id=frames[1].can_id=0x18DAF101U;
  frames[0].dlc=frames[1].dlc=8;
  frames[0].data[0]=4;frames[0].data[1]=0x41;
  frames[0].data[2]=0x0C;frames[0].data[3]=0x13;
  frames[0].data[4]=0x88;
  frames[1].data[0]=3;frames[1].data[1]=0x41;
  frames[1].data[2]=0x0D;frames[1].data[3]=80;
  assert(cache.rememberApprovedPair(rpm,speed,frames,t+3000));
  assert(cache.lookup(rpm,t+3001,out));
  assert(out.responses[0].can_id==0x18DAF101U);
  assert(out.responses[0].length==4);
  assert(out.responses[0].data[3]==0x88);
  assert(cache.lookup(speed,t+3001,out));
  assert(out.responses[0].data[2]==80);
  // Simulated frame data is an ELM reply; it cannot be confused with
  // original RAW CAN capture evidence.
  assert(out.raw_frame_count==1 && out.raw_frames[0].data[2]==0x0D);

  // Reject all-or-nothing invalid group inserts.
  frames[1].can_id=0x18DAF102U;
  assert(!cache.rememberApprovedPair(rpm,speed,frames,t+5000));
  assert(cache.lookup(speed,t+4000,out));
  assert(out.responses[0].can_id==0x18DAF101U);

  // Replay more keys than fixed cache slots without heap allocation.
  DiagnosticPidCache bounded;
  for(unsigned i=0;i<DiagnosticPidCache::kSlots+6;++i)
    assert(bounded.rememberSingle(
        read(static_cast<uint8_t>(0x80+i)),
        reply(static_cast<uint8_t>(0x80+i),11),
        t+i*1000));
  assert(bounded.stats().evictions>=6);
  assert(!bounded.lookup(read(0x80),t+20000,out));
  assert(bounded.lookup(read(static_cast<uint8_t>(
      0x80+DiagnosticPidCache::kSlots+5)),t+20000,out));

  // Status and read allowlist: never cache a diagnostic write transaction.
  auto denied=read(0x0D);denied.data[0]=0x2E;
  assert(!cache.rememberSingle(denied,reply(0x0D,80),t));
  assert(!cache.lookup(denied,t+1,out));
  const auto stats=cache.stats();
  assert(stats.hits>0 && stats.misses>0 && stats.inserts>=4 &&
         stats.invalidations>0);
  std::cout << "PASS independent bounded per-PID cache, ECU identity, TTL, "
               "negative/partial protection and eviction\\n";
}
