#include "../src/m5can_log_stream_v2_sender.h"
#include "../src/m5can_log_v2_peer_policy.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
using namespace m5can_log_stream_v2;

int main() {
  constexpr uint16_t kDataBlocks=24;
  constexpr uint16_t kBlocks=kDataBlocks+1; // final committed journal END
  constexpr uint32_t kSession=0xA5B6C7D8U;
  std::vector<uint8_t> expected;
  std::array<uint16_t,kDataBlocks> lengths{};
  for(uint16_t block=0;block<kDataBlocks;++block) {
    lengths[block]=block%5==0?17:3616;
    for(uint16_t off=0;off<lengths[block];++off)
      expected.push_back(static_cast<uint8_t>(block*31u+off*7u));
  }
  BlockSender sender;
  VerifiedCursor receiver;
  assert(sender.begin(kSession,kBlocks));
  assert(receiver.reset(kSession));
  assert(!sender.stage(1,expected.data(),17)); // cannot skip first block
  assert(!sender.stage(0,nullptr,0)); // data must be nonempty
  std::vector<uint8_t> actual;
  actual.reserve(expected.size());
  std::vector<uint8_t> errored(65536,0);
  uint16_t next_block=0;
  size_t staged_offset=0;
  unsigned dropped=0,corrupted=0,skipped=0,rewinds=0,acked=0;
  for(unsigned cycles=0;!sender.completed() && cycles<100000;++cycles) {
    if(sender.needsBlock()) {
      if(next_block==kDataBlocks) {
        assert(sender.stage(next_block,nullptr,0,true));
      } else {
        std::array<uint8_t,kMaxBlockBytes> scratch{};
        std::memcpy(scratch.data(),expected.data()+staged_offset,lengths[next_block]);
        assert(sender.stage(next_block,scratch.data(),lengths[next_block]));
        std::memset(scratch.data(),0,scratch.size()); // sender owns a copy
        staged_offset+=lengths[next_block];
      }
      ++next_block;
    }
    std::array<uint8_t,kMaxNotification> wire{};
    const size_t n=sender.peek(wire.data(),wire.size());
    if(n) {
      Frame original{};
      assert(decode(wire.data(),n,original));
      assert(sender.queued());
      const uint32_t seq=original.sequence;
      assert(seq<errored.size());
      if(seq%61==9 && !(errored[seq]&1)) {
        errored[seq]|=1;++dropped;continue;
      }
      if(seq%47==13 && !(errored[seq]&2)) {
        errored[seq]|=2;wire[5]^=1;++corrupted;
      }
      Frame frame{};
      if(!decode(wire.data(),n,frame))continue;
      auto check=receiver.check(frame);
      if(check==VerifiedCursor::Check::Gap) {++skipped;continue;}
      if(check==VerifiedCursor::Check::Duplicate)continue;
      assert(check==VerifiedCursor::Check::Next);
      if(frame.kind==Kind::Data) {
        assert(frame.block<kDataBlocks);
        const size_t prefix=[&]() {
          size_t sum=0;
          for(unsigned b=0;b<frame.block;++b)sum+=lengths[b];
          return sum;
        }();
        assert(actual.size()==prefix+frame.offset);
        for(unsigned i=0;i<frame.length;++i)
          assert(frame.data[i]==expected[prefix+frame.offset+i]);
        actual.insert(actual.end(),frame.data,frame.data+frame.length);
      }
      // Persistent storage check is modeled before advancing the SD cursor.
      assert(receiver.verified(frame));
    }
    // ACK only after the receiver has durably verified the accepted prefix.
    if(sender.outstanding()==kWindowFrames ||
       (!n && sender.outstanding())) {
      Frame ack=receiver.ack();
      if(ack.sequence) {
        auto forged=ack;
        forged.offset=static_cast<uint16_t>(ack.offset^1U);
        assert(!sender.acknowledge(forged));
      }
      assert(sender.acknowledge(ack));++acked;
      if(sender.outstanding()) {
        ++rewinds;
        assert(sender.retryUnacked());
      }
    }
  }
  assert(!sender.awaitingFinalAck());
  assert(sender.completed() && !sender.failed());
  assert(receiver.complete() && receiver.block()==kBlocks-1);
  assert(actual==expected);
  assert(dropped>0 && corrupted>0 && skipped>0 && rewinds>0 && acked>0);
  // After final ACK, no more block or packet may be enqueued.
  uint8_t out[kMaxNotification]{};
  assert(!sender.needsBlock() && sender.peek(out,sizeof(out))==0);
  assert(sender.begin(0x12345678,2)); // new bounded session resets complete
  assert(!sender.completed() && sender.needsBlock());
  sender.cancel();assert(sender.failed() && !sender.needsBlock());

  // The runtime may submit at most 3 eligible notifications on each BLE
  // application tick. The sender must be able to consume successive credits
  // without changing sequence/offset, jumping a block, or releasing durable
  // storage early. This is an instantaneous-memory-ACK model, not RF timing.
  static_assert(PeerQosPolicy::kMaxLogFramesPerPoll==3,
                "bounded three-frame host-credit burst expected");
  BlockSender fast{};
  VerifiedCursor stored{};
  constexpr uint32_t kFastSession=0xC0FFEE11U;
  std::array<uint8_t,kMaxBlockBytes> test_block{};
  for(size_t i=0;i<test_block.size();++i)
    test_block[i]=static_cast<uint8_t>((i*37+13)&255);
  assert(fast.begin(kFastSession,2));
  assert(stored.reset(kFastSession));
  assert(fast.stage(0,test_block.data(),test_block.size()));
  unsigned polls=0,fast_packets=0,fast_bytes=0;
  while(!fast.completed() && polls<12) {
    ++polls;
    if(fast.needsBlock()) {
      assert(stored.block()==1);
      assert(fast.stage(1,nullptr,0,true));
    }
    unsigned accepted=0;
    for(;accepted<PeerQosPolicy::kMaxLogFramesPerPoll;++accepted) {
      std::array<uint8_t,kMaxNotification> packet{};
      const size_t count=fast.peek(packet.data(),packet.size());
      if(!count)break;
      Frame received{};
      assert(decode(packet.data(),count,received));
      assert(received.sequence==fast_packets);
      assert(fast.queued());
      assert(stored.check(received)==VerifiedCursor::Check::Next);
      if(received.kind==Kind::Data) {
        for(size_t i=0;i<received.length;++i)
          assert(received.data[i]==test_block[fast_bytes+i]);
        fast_bytes+=received.length;
      }
      assert(stored.verified(received));
      ++fast_packets;
    }
    assert(accepted<=PeerQosPolicy::kMaxLogFramesPerPoll);
    if(fast.outstanding())
      assert(fast.acknowledge(stored.ack()));
  }
  assert(fast.completed() && fast_packets==18);
  assert(fast_bytes==test_block.size() && polls<=8);
  std::cout<<"V2 bounded 3-frame burst FIFO/END/SD-ACK model PASS polls="
           <<polls<<" frames="<<fast_packets<<"\\n";

  std::cout<<"journal V2 sender staged/SD-ACK/loss/corruption PASS bytes="
           <<actual.size()<<" dropped="<<dropped<<" corrupted="<<corrupted
           <<" rewind="<<rewinds<<"\n";
}
