#include "raw_can_journal.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
using namespace m5can;

// NOR-flash semantics: never allow 0 -> 1 without a physical erase.
// Failure injection models torn owner/header/data/commit write.
class MockFlash final : public RawCanFlash {
 public:
  explicit MockFlash(unsigned sectors) : bytes(sectors*4096,0xFF) {}
  std::vector<uint8_t> bytes;
  int fail_at = -1, calls = 0;
  uint32_t size() const override {return static_cast<uint32_t>(bytes.size());}
  bool read(uint32_t off,void* p,size_t n) override {
    if(size_t(off)+n>bytes.size())return false;
    std::memcpy(p,bytes.data()+off,n);
    return true;
  }
  bool write(uint32_t off,const void* p,size_t n) override {
    ++calls;
    if(size_t(off)+n>bytes.size())return false;
    auto* source=static_cast<const uint8_t*>(p);
    // A write can tear after some bytes, including the commit operation.
    const size_t written=(calls==fail_at)?n/2:n;
    for(size_t i=0;i<written;++i) {
      if((bytes[off+i]&source[i])!=source[i])return false;
      bytes[off+i]&=source[i];
    }
    return calls!=fail_at;
  }
};
static RawCanBlockCodec block(uint32_t first) {
  RawCanBlockCodec encoder;
  for(uint32_t i=0;i<40;++i) {
    RawCanFrame f{};f.sequence=first+i;
    f.timestamp_us=1000000ULL+i*20000;
    f.identifier=(i%3)?0x7E8:0x18DAF101;
    f.flags=(i%3)?0:1;f.dlc=8;
    f.direction=RawCanDirection::Rx;
    f.data[0]=0x04;f.data[1]=0x41;f.data[2]=0x0C;
    f.data[3]=static_cast<uint8_t>(i);
    assert(encoder.append(f));
  }
  assert(encoder.finalize());
  return encoder;
}
static void test_blank_initialization_and_readback() {
  MockFlash raw(8);
  RawCanJournal journal;
  assert(journal.begin(raw));
  assert(journal.status().ready);
  const auto encoded=block(1);
  assert(journal.append(20261016,RawCanJournal::Data,
         encoded.data(),static_cast<uint16_t>(encoded.size()),0));
  assert(journal.append(20261016,RawCanJournal::End,nullptr,0,2));
  assert(journal.status().blocks==2);
  assert(journal.status().completed);
  assert(journal.status().last_dropped==2);
  std::vector<uint8_t> out(RawCanBlockCodec::kMaxBytes);
  uint16_t bytes=0;RawCanJournal::RecordType type=RawCanJournal::End;
  assert(journal.readBlock(0,out.data(),out.size(),bytes,type));
  assert(type==RawCanJournal::Data && bytes==encoded.size());
  uint16_t decoded=0;RawCanFrame frames[40]{};
  assert(RawCanBlockCodec::decode(out.data(),bytes,frames,40,decoded));
  assert(decoded==40 && frames[0].sequence==1 &&
         frames[39].sequence==40);
  assert(journal.readBlock(1,out.data(),out.size(),bytes,type));
  assert(type==RawCanJournal::End && bytes==0);
  assert(!journal.append(20261016,RawCanJournal::Data,
         encoded.data(),static_cast<uint16_t>(encoded.size()),0));

  RawCanJournal recovered;
  assert(recovered.begin(raw));
  assert(recovered.status().session==20261016);
  assert(recovered.status().completed);
  assert(recovered.status().blocks==2);
  assert(recovered.readBlock(0,out.data(),out.size(),bytes,type));
}
static void test_fail_closed_foreign_and_torn() {
  MockFlash old_ota(8);
  old_ota.bytes[7*4096+1]=0x42; // first sectors look empty
  RawCanJournal failed;
  assert(!failed.begin(old_ota));
  assert(failed.status().foreign);
  assert(old_ota.bytes[0]==0xFF); // no ownership write attempted

  MockFlash raw(8);
  RawCanJournal first;
  assert(first.begin(raw));
  auto encoded=block(1);
  // Data write completed but commit marker tears: recover must not
  // interpret it as committed or silently skip it to append elsewhere.
  raw.fail_at=raw.calls+4;
  assert(!first.append(9,RawCanJournal::Data,
      encoded.data(),static_cast<uint16_t>(encoded.size()),0));
  RawCanJournal reboot;
  assert(!reboot.begin(raw));
  assert(reboot.status().corrupt);
  // Already-written evidence must never be erased as recovery.
  assert(raw.bytes[2*4096]=='C');
}
static void test_full_and_session_guard() {
  MockFlash raw(4); // owner x2, data x2
  RawCanJournal journal;
  assert(journal.begin(raw));
  auto codec=block(17);
  assert(journal.append(123,RawCanJournal::Data,
      codec.data(),static_cast<uint16_t>(codec.size()),3));
  assert(!journal.append(124,RawCanJournal::Data,
      codec.data(),static_cast<uint16_t>(codec.size()),4));
  assert(journal.append(123,RawCanJournal::End,nullptr,0,3));
  assert(journal.status().full && journal.status().completed);
}
int main() {
  test_blank_initialization_and_readback();
  test_fail_closed_foreign_and_torn();
  test_full_and_session_guard();
  std::cout<<"raw journal CRC, ownership, reboot, power-cut PASS\n";
}
