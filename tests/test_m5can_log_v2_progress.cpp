#include "../src/m5can_log_v2_progress.h"
#include "../src/m5can_log_stream_v2_sender.h"
#include <cassert>
#include <cstdint>
#include <iostream>
using namespace m5can_log_stream_v2;
int main() {
  ProgressWatchdog timer;
  timer.reset(100);
  assert(!timer.elapsed(1299,1200));
  assert(timer.elapsed(1300,1200));
  // Duplicate/stale ACK must not indefinitely postpone retransmission.
  assert(!timer.noteAck(0,0,1300));
  assert(timer.elapsed(1300,1200));
  assert(timer.noteAck(0,1,1300));
  assert(!timer.elapsed(2499,1200));
  assert(timer.elapsed(2500,1200));
  // Two-peer scheduling may enqueue the 16th frame >1200ms after the
  // first; only the last accepted notification starts the ACK wait.
  timer.reset(0);
  for(uint32_t i=0;i<16;++i) {
    const uint32_t now=100+i*110;
    timer.noteNotification(now);
    assert(!timer.elapsed(now,1200));
  }
  assert(!timer.elapsed(2949,1200));
  assert(timer.elapsed(2950,1200)); // last accepted at 1750
  // millis() wrap after ~49.7 days.
  timer.reset(UINT32_MAX-100);
  assert(!timer.elapsed(48,150));
  assert(timer.elapsed(49,150));
  // Actual journal sender integration: valid duplicate ACK is harmless,
  // but it must not advance the watchdog or reset its retry deadline.
  BlockSender sender;
  const uint8_t payload[3]{0x12,0x34,0x56};
  assert(sender.begin(0xDEADBEEFu,2));
  assert(sender.stage(0,payload,sizeof(payload)));
  uint8_t wire[kMaxNotification]{};
  assert(sender.peek(wire,sizeof(wire))>0);
  assert(sender.queued());
  Frame ack{};
  ack.kind=Kind::Ack;ack.session=0xDEADBEEFu;
  ack.sequence=0;ack.block=0;ack.offset=0;
  const uint32_t before=sender.durableSequence();
  assert(sender.acknowledge(ack));
  assert(!timer.noteAck(before,sender.durableSequence(),200));
  ack.sequence=1;ack.block=1;ack.offset=0;
  assert(sender.acknowledge(ack));
  assert(timer.noteAck(before,sender.durableSequence(),300));
  assert(sender.needsBlock());
  assert(sender.stage(1,nullptr,0,true));
  assert(sender.peek(wire,sizeof(wire))>0);
  assert(sender.queued());
  assert(!timer.elapsed(30299,30000));
  assert(timer.elapsed(30300,30000));
  ack.sequence=2;
  assert(sender.acknowledge(ack));
  assert(sender.completed());
  std::cout<<"V2 progress timer duplicate-ACK, pacing, wrap and END PASS\n";
}
