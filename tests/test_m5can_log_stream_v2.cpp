#include "../src/m5can_log_stream_v2.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
using namespace m5can_log_stream_v2;
int main() {
  static_assert(kMaxNotification==244 && kMaxPayload==216,"MTU limits");
  uint8_t payload[216]{};
  for(size_t i=0;i<sizeof(payload);++i)
    payload[i]=static_cast<uint8_t>(i*17u);
  uint8_t wire[244]{};
  Frame f{};f.kind=Kind::Data;f.session=0x11223344;
  f.sequence=0;f.block=0;f.offset=0;f.total=300;
  f.length=216;f.data=payload;
  assert(encode(f,wire,sizeof(wire))==244);
  Frame parsed{};
  assert(decode(wire,sizeof(wire),parsed));
  assert(parsed.sequence==0 && parsed.length==216 &&
         !std::memcmp(parsed.data,payload,216));
  wire[29]^=4;
  assert(!decode(wire,sizeof(wire),parsed));
  wire[29]^=4;
  assert(!decode(wire,243,parsed));
  assert(decode(wire,sizeof(wire),parsed));
  VerifiedCursor receiver;
  assert(receiver.reset(f.session));
  assert(!receiver.reset(f.session,1,0,216));
  assert(receiver.reset(f.session));
  assert(receiver.check(parsed)==VerifiedCursor::Check::Next);
  // No ACK until SD durable verification, even if the radio got the frame.
  assert(receiver.sequence()==0);
  assert(receiver.verified(parsed));
  assert(receiver.sequence()==1 && receiver.offset()==216);
  auto mid_ack=receiver.ack();
  assert(mid_ack.offset==216 && mid_ack.sequence==1);
  VerifiedCursor resumed;
  assert(resumed.reset(f.session,1,0,216,300));
  assert(receiver.check(parsed)==VerifiedCursor::Check::Duplicate);
  Frame bad=parsed;bad.sequence=3;
  assert(receiver.check(bad)==VerifiedCursor::Check::Gap);
  bad=parsed;bad.sequence=1;bad.offset=216;bad.total=300;
  bad.length=84;bad.data=payload;
  uint8_t small[244]{};
  const size_t n=encode(bad,small,sizeof(small));
  assert(n==112 && decode(small,n,parsed));
  assert(receiver.check(parsed)==VerifiedCursor::Check::Next);
  assert(receiver.verified(parsed));
  assert(resumed.check(parsed)==VerifiedCursor::Check::Next);
  assert(resumed.verified(parsed));
  assert(resumed.block()==1 && resumed.offset()==0);
  assert(receiver.block()==1 && receiver.offset()==0);
  Frame end{};end.kind=Kind::End;end.session=f.session;
  end.sequence=2;end.block=1;
  assert(encode(end,small,sizeof(small))==28);
  assert(decode(small,28,parsed) && receiver.verified(parsed));
  assert(receiver.complete() && receiver.sequence()==3);
  assert(receiver.check(parsed)==VerifiedCursor::Check::Invalid);
  Frame ack=receiver.ack();
  assert(encode(ack,small,sizeof(small))==28);
  assert(decode(small,28,parsed) && parsed.kind==Kind::Ack);
  WindowGate gate;
  assert(gate.reset(f.session));
  auto make_data=[&](uint32_t seq) {
    Frame data{};data.kind=Kind::Data;data.session=f.session;
    data.sequence=seq;data.block=seq<=16?0:1;
    data.offset=seq<=16?static_cast<uint16_t>(seq*216U):
                          static_cast<uint16_t>((seq-17U)*216U);
    data.total=3616;data.length=seq==16?160:216;data.data=payload;
    return data;
  };
  for(uint32_t i=0;i<kWindowFrames;++i)
    assert(gate.sent(make_data(i)));
  assert(!gate.maySend() && !gate.sent(make_data(kWindowFrames)));
  Frame a{};a.kind=Kind::Ack;a.session=f.session;a.sequence=8;
  a.block=0;a.offset=8*216;
  auto forged=a;forged.offset=2000;
  assert(!gate.acknowledge(forged));
  forged=a;forged.block=1;
  assert(!gate.acknowledge(forged));
  assert(gate.acknowledge(a) && gate.outstanding()==8);
  assert(gate.durableBlock()==0 && gate.durableOffset()==1728);
  assert(gate.acknowledge(a)); // idempotent, never adds credit
  for(uint32_t i=16;i<24;++i)assert(gate.sent(make_data(i)));
  assert(!gate.maySend());
  a.sequence=25;assert(!gate.acknowledge(a));
  a.sequence=7;assert(!gate.acknowledge(a));
  a.sequence=8;a.session=5;assert(!gate.acknowledge(a));
  gate.rewindUnacked();
  assert(gate.durableNext()==8 && gate.nextToSend()==8);
  assert(gate.nextBlock()==0 && gate.nextOffset()==1728);
  assert(gate.sent(make_data(8)));
  // A sender cannot accept out-of-order, session-switched, offset-switched,
  // or malformed frames even when the sequence number appears plausible.
  Frame wrong=make_data(9);wrong.offset=0;
  assert(!gate.sent(wrong));
  wrong=make_data(9);wrong.session=9;
  assert(!gate.sent(wrong));
  assert(gate.sent(make_data(9)));
  // Never allow premature END / forged completion of an unverified block.
  Frame premature{};premature.kind=Kind::End;
  premature.session=f.session;premature.sequence=10;premature.block=0;
  assert(!gate.sent(premature));
  // Terminal journal cursor 4096 is END-only. Rejected DATA must not
  // mutate the pending window, even if its declared block completes.
  WindowGate terminal;
  assert(terminal.reset(f.session,0,4096));
  Frame terminal_data=f;terminal_data.block=4096;
  terminal_data.total=216;terminal_data.length=216;
  assert(!terminal.sent(terminal_data));
  assert(terminal.nextBlock()==4096 && terminal.nextOffset()==0);
  assert(terminal.outstanding()==0);
  Frame terminal_end=end;terminal_end.block=4096;terminal_end.sequence=0;
  assert(terminal.sent(terminal_end));
  Frame terminal_ack{};terminal_ack.kind=Kind::Ack;
  terminal_ack.session=f.session;terminal_ack.sequence=1;
  terminal_ack.block=4096;
  assert(terminal.acknowledge(terminal_ack));
  assert(terminal.completeAcked());
  uint8_t invalid[244]{};
  assert(encode(f,invalid,sizeof(invalid))==244);
  invalid[6]=1;assert(!decode(invalid,244,parsed));
  Frame no_session=f;no_session.session=0;
  assert(!encode(no_session,wire,sizeof(wire)));
  Frame invalid_end=end;invalid_end.offset=1;
  assert(!encode(invalid_end,wire,sizeof(wire)));
  std::cout<<"M5CAN stream v2 framing, CRC, cursor, ACK credit and retry PASS\n";
}
