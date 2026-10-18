#include "../src/m5can_log_stream_v2.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
using namespace m5can_log_stream_v2;
struct Packet {std::array<uint8_t,244> data{};size_t len=0;};
int main() {
  constexpr uint32_t session=0xACCE551u;
  constexpr unsigned blocks=64;
  constexpr unsigned block_len=1024;
  std::vector<uint8_t> source(blocks*block_len);
  for(size_t i=0;i<source.size();++i)
    source[i]=static_cast<uint8_t>((i*37u+(i>>3))&255u);
  std::vector<Packet> packets;
  for(unsigned b=0;b<blocks;++b) {
    for(unsigned offset=0;offset<block_len;) {
      Frame frame{};frame.kind=Kind::Data;frame.session=session;
      frame.sequence=static_cast<uint32_t>(packets.size());
      frame.block=static_cast<uint16_t>(b);
      frame.offset=static_cast<uint16_t>(offset);
      frame.total=block_len;
      frame.length=static_cast<uint16_t>(
        (block_len-offset)>kMaxPayload?kMaxPayload:(block_len-offset));
      frame.data=source.data()+b*block_len+offset;
      Packet packet{};packet.len=encode(frame,packet.data.data(),packet.data.size());
      assert(packet.len && packet.len<=244);
      packets.push_back(packet);offset+=frame.length;
    }
  }
  Frame end{};end.kind=Kind::End;end.session=session;
  end.sequence=static_cast<uint32_t>(packets.size());end.block=blocks;
  Packet terminator{};terminator.len=encode(end,terminator.data.data(),terminator.data.size());
  assert(terminator.len==28);packets.push_back(terminator);
  VerifiedCursor receiver;assert(receiver.reset(session));
  WindowGate sender;sender.reset(session);
  std::vector<uint8_t> actual;actual.reserve(source.size());
  std::vector<uint8_t> dropped_once(packets.size(),0);
  unsigned corrupt=0,missing=0,gaps=0,retransmit=0;
  for(unsigned attempts=0;sender.durableNext()<packets.size() && attempts<10000;++attempts) {
    const uint32_t seq=sender.nextToSend();
    if(seq<packets.size() && sender.maySend()) {
      assert(sender.sent(seq));
      if(seq%53==17 && !dropped_once[seq]) {
        dropped_once[seq]=1;++missing;continue;
      }
      Packet p=packets[seq];
      if(seq%47==23 && !dropped_once[seq]) {
        dropped_once[seq]=1;p.data[25]^=1;++corrupt;
      }
      Frame received{};
      if(!decode(p.data.data(),p.len,received))continue;
      auto result=receiver.check(received);
      if(result==VerifiedCursor::Check::Gap){++gaps;continue;}
      if(result==VerifiedCursor::Check::Duplicate)continue;
      assert(result==VerifiedCursor::Check::Next);
      if(received.kind==Kind::Data) {
        const size_t data_offset=received.block*block_len+received.offset;
        assert(actual.size()==data_offset);
        for(size_t i=0;i<received.length;++i)
          assert(received.data[i]==source[data_offset+i]);
        actual.insert(actual.end(),received.data,received.data+received.length);
      }
      // Mock durable-verified SD append; only then ACK the verified cursor.
      assert(receiver.verified(received));
    } else {
      const Frame ack=receiver.ack();
      assert(sender.acknowledge(ack));
      if(sender.outstanding()) {++retransmit;sender.rewindUnacked();}
    }
  }
  assert(sender.durableNext()==packets.size());
  assert(receiver.complete() && receiver.block()==blocks);
  assert(actual==source);
  assert(missing>0 && corrupt>0 && gaps>0 && retransmit>0);
  std::cout<<"V2 64KiB stream, loss, corruption, go-back-N and END PASS"
           <<" frames="<<packets.size()<<" retries="<<retransmit<<"\n";
}
