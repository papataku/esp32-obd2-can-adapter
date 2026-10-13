#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include "evidence_store.h"
using namespace m5can;

struct Flash final:EvidenceFlash {
  std::vector<uint8_t> bytes=std::vector<uint8_t>(40*4096,0xFF);
  std::array<unsigned,40> erases{};
  int cut=-1;
  bool fail_read=false;
  unsigned writes=0;
  uint32_t size() const override{return bytes.size();}
  bool read(uint32_t offset,void* p,size_t n) override {
    if(fail_read){fail_read=false;return false;}
    if(offset>bytes.size() || n>bytes.size()-offset)return false;
    std::memcpy(p,bytes.data()+offset,n);return true;
  }
  bool write(uint32_t offset,const void* p,size_t n) override {
    if(offset>bytes.size() || n>bytes.size()-offset)return false;
    const auto* source=static_cast<const uint8_t*>(p);
    const bool torn=cut==0;
    if(cut>0)--cut;
    const size_t size=torn?n/2:n;
    for(size_t i=0;i<size;++i) {
      if((bytes[offset+i]&source[i])!=source[i])return false;
      bytes[offset+i]&=source[i];
    }
    ++writes;return !torn;
  }
  bool erase(uint32_t offset) override {
    if(offset%4096 || offset+4096>bytes.size())return false;
    const bool torn=cut==0;if(cut>0)--cut;
    std::fill_n(bytes.begin()+offset,torn?2048:4096,0xFF);
    ++erases[offset/4096];return !torn;
  }
};
int main() {
  Flash foreign;foreign.bytes[12000]=0x81;
  const auto before=foreign.bytes;
  EvidenceStore blocked;
  assert(!blocked.begin(foreign,true) && blocked.status().foreign);
  assert(foreign.bytes==before && foreign.writes==0);
  Flash blank;EvidenceStore inspect;
  assert(!inspect.begin(blank,false) && blank.writes==0);
  EvidenceStore store;assert(store.begin(blank,true));
  std::array<uint8_t,2304> payload{};
  for(unsigned key=0;key<20;++key) {
    payload.fill(static_cast<uint8_t>(key));
    const auto type=key<8?EvidenceStore::Probe:key<16?EvidenceStore::Failure:EvidenceStore::Ble;
    const unsigned slot=key<8?key:key<16?key-8:key-16;
    assert(store.put(type,slot,payload.data(),payload.size()));
  }
  // More than a complete flash rotation, with all20 protected latest keys.
  std::array<uint8_t,20> expected{};
  for(unsigned i=0;i<20;++i)expected[i]=i;
  for(unsigned turn=0;turn<800;++turn) {
    const unsigned key=turn%20;
    expected[key]=static_cast<uint8_t>(turn);
    payload.fill(expected[key]);
    const auto type=key<8?EvidenceStore::Probe:key<16?EvidenceStore::Failure:EvidenceStore::Ble;
    assert(store.put(type,key<8?key:key<16?key-8:key-16,payload.data(),payload.size()));
  }
  EvidenceStore reboot;assert(reboot.begin(blank,false));
  for(unsigned key=0;key<20;++key) {
    const auto type=key<8?EvidenceStore::Probe:key<16?EvidenceStore::Failure:EvidenceStore::Ble;
    assert(reboot.get(type,key<8?key:key<16?key-8:key-16,payload.data(),payload.size())==payload.size());
    for(const auto b:payload)assert(b==expected[key]);
  }
  // Owner sectors never erased; data erases distribute across38 sectors.
  assert(blank.erases[0]==0 && blank.erases[1]==0);
  const auto range=std::minmax_element(blank.erases.begin()+2,blank.erases.end());
  assert(*range.second-*range.first<=2);
  Flash base;EvidenceStore first;assert(first.begin(base,true));
  std::array<uint8_t,64> old{},next{},out{};old.fill(0xA5);next.fill(0xF0);
  assert(first.put(EvidenceStore::Ble,0,old.data(),old.size()));
  for(int step=0;step<4;++step) {
    Flash cut=base;EvidenceStore running;assert(running.begin(cut,false));
    cut.cut=step;
    assert(!running.put(EvidenceStore::Ble,0,next.data(),next.size()));
    cut.cut=-1;
    EvidenceStore recovered;assert(recovered.begin(cut,false));
    assert(recovered.get(EvidenceStore::Ble,0,out.data(),out.size())==out.size());
    assert(out==old);
  }
  // CRC corruption of the newest committed blob recovers its prior version.
  Flash corrupt=base;EvidenceStore newer;assert(newer.begin(corrupt,false));
  assert(newer.put(EvidenceStore::Ble,0,next.data(),next.size()));
  corrupt.bytes[3*4096+32]^=0x01;
  EvidenceStore checked;assert(checked.begin(corrupt,false));
  assert(checked.status().corrupt==1);
  assert(checked.get(EvidenceStore::Ble,0,out.data(),out.size())==out.size() && out==old);
  // One owner metadata copy can be lost without losing journal records.
  corrupt.bytes[0]^=0x01;
  EvidenceStore owner_recovery;assert(owner_recovery.begin(corrupt,false));
  assert(owner_recovery.get(EvidenceStore::Ble,0,out.data(),out.size())==out.size() && out==old);
  assert(!owner_recovery.put(EvidenceStore::Ble,4,old.data(),old.size()));
  assert(!owner_recovery.put(EvidenceStore::Probe,0,old.data(),2305));
  std::cout<<"PASS dedicated flash:20 keys, wear rotation, torn erase/header/data/commit, CRC, owner redundancy, foreign/readonly preservation\n";
}
