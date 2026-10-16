#include "raw_can_capture.h"
#include "raw_can_codec.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using namespace m5can;

static RawCanFrame sample(uint32_t seq,uint64_t ts,uint32_t id=0x7E8,
                          bool extended=false,uint8_t dlc=8) {
  RawCanFrame f{};
  f.sequence=seq;f.timestamp_us=ts;f.identifier=id;
  f.dlc=dlc;f.flags=extended?1:0;
  for(unsigned i=0;i<dlc;++i)f.data[i]=static_cast<uint8_t>(i*13+seq/8);
  return f;
}
static bool same(const RawCanFrame& a,const RawCanFrame& b) {
  return a.sequence==b.sequence&&a.timestamp_us==b.timestamp_us &&
      a.identifier==b.identifier&&a.dlc==b.dlc&&a.flags==b.flags &&
      a.direction==b.direction&&!std::memcmp(a.data,b.data,8);
}

static void test_ring() {
  RawCanCapture q;
  assert(!q.start(0));
  assert(q.start(0x20261016));
  assert(!q.start(2));
  assert(q.active());
  for(uint32_t i=0;i<RawCanCapture::kCapacity;++i) {
    auto f=sample(0,1000U+i*100U);
    assert(q.push(f));
  }
  assert(q.queued()==RawCanCapture::kCapacity);
  assert(!q.push(sample(0,99999)));
  assert(q.lost()==1&&q.attempted()==RawCanCapture::kCapacity+1);
  RawCanFrame f{};
  assert(q.pop(f)&&f.sequence==1);
  assert(q.push(sample(0,100000)));
  // The dropped source observation sequence must create a visible gap.
  for(uint32_t seq=2;seq<=RawCanCapture::kCapacity;++seq) {
    assert(q.pop(f)&&f.sequence==seq);
  }
  assert(q.pop(f)&&f.sequence==RawCanCapture::kCapacity+2);
  assert(!q.pop(f));
  q.stop();
  assert(!q.active());
  assert(!q.push(sample(0,123)));
  assert(q.start(2));
  assert(q.push(sample(0,123)));
  assert(q.pop(f)&&f.sequence==1);
  q.stop();
  assert(!q.pop(f));
}

static void test_reversible() {
  RawCanBlockCodec codec;
  std::vector<RawCanFrame> source;
  for(uint32_t i=0;i<144;++i) {
    RawCanFrame f=sample(100+i+(i>70?3:0),
        12'345'678ULL+i*10'000ULL,
        i%7?0x18DAF101:0x7E8, i%7!=0,
        static_cast<uint8_t>(i%9));
    if(i%4==0)f.direction=RawCanDirection::TxSubmitted;
    if(i%13==0)f.direction=RawCanDirection::TxSubmitFailed;
    f.flags|=i%9==0?2U:0U;
    f.flags|=i%17==0?4U:0U;
    if(i%5==0 && !source.empty()&&source.back().dlc==f.dlc)
      std::memcpy(f.data,source.back().data,f.dlc);
    if(!codec.append(f)) {
      assert(codec.count()>0);
      break;
    }
    source.push_back(f);
  }
  assert(codec.finalize());
  RawCanFrame out[144]{};
  uint16_t n=0;
  assert(RawCanBlockCodec::decode(codec.data(),codec.size(),out,144,n));
  assert(n==source.size());
  for(unsigned i=0;i<n;++i)assert(same(out[i],source[i]));
  assert(codec.size()<source.size()*sizeof(RawCanFrame));

  std::vector<uint8_t> corrupted(codec.data(),codec.data()+codec.size());
  corrupted.back()^=0x10U;
  assert(!RawCanBlockCodec::decode(corrupted.data(),corrupted.size(),out,144,n));
  assert(n==0);
  assert(!RawCanBlockCodec::decode(codec.data(),codec.size()-1,out,144,n));
  assert(!RawCanBlockCodec::decode(codec.data(),codec.size(),out,1,n));
  codec.reset();
  assert(codec.count()==0&&!codec.finalize());
}

static void test_same_and_rejections() {
  RawCanBlockCodec codec;
  RawCanFrame a=sample(1,UINT64_C(9223372036854775800),0x7E8,false,8);
  assert(codec.append(a));
  RawCanFrame b=a;b.sequence=2;b.timestamp_us+=7;
  assert(codec.append(b)); // same ID and payload
  assert(codec.finalize());
  assert(codec.size()<RawCanBlockCodec::kHeader+32);
  RawCanFrame result[2]{};
  uint16_t n=0;
  assert(RawCanBlockCodec::decode(codec.data(),codec.size(),result,2,n));
  assert(n==2&&same(a,result[0])&&same(b,result[1]));

  codec.reset();
  assert(codec.append(a));
  b.sequence=1;
  assert(!codec.append(b));
  b.sequence=2;b.timestamp_us=a.timestamp_us-1;
  assert(!codec.append(b));
  b=a;b.sequence=2;b.dlc=9;
  assert(!codec.append(b));
  b=a;b.sequence=2;b.flags=8;
  assert(!codec.append(b));
}

int main() {
  test_ring();
  test_reversible();
  test_same_and_rejections();
  std::cout<<"raw CAN ring and reversible codec PASS\n";
}
