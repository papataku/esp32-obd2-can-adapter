#include "raw_can_ble_protocol.h"
#include "raw_can_codec.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
using namespace m5can;

int main() {
  uint32_t block=0,offset=0;
  assert(raw_ble::parseReadArgs("0,0",block,offset));
  assert(block==0&&offset==0);
  assert(raw_ble::parseReadArgs("4294967295,4294967295",block,offset));
  assert(block==UINT32_MAX && offset==UINT32_MAX);
  for(const char* bad: {"",",0","0,","-1,0","1,-1","0,0,","1,2a",
                        "4294967296,0","0,4294967296","0, 1",
                        "+1,2","1.2,3","0;1"}) {
    assert(!raw_ble::parseReadArgs(bad,block,offset));
  }
  const uint8_t check[]={'1','2','3','4','5','6','7','8','9'};
  assert(raw_ble::crc32(check,sizeof(check))==0xCBF43926UL);
  assert(raw_ble::crc32(check,0)==0);
  uint8_t input[raw_ble::kChunkBytes]{};
  for(size_t i=0;i<sizeof(input);++i)
    input[i]=static_cast<uint8_t>(i*5);
  char hex[raw_ble::kChunkBytes*2+1]{};
  assert(raw_ble::hexEncode(input,sizeof(input),hex,sizeof(hex)));
  for(size_t i=0;i<sizeof(input);++i) {
    constexpr char digits[]="0123456789ABCDEF";
    assert(hex[i*2]==digits[input[i]>>4]);
    assert(hex[i*2+1]==digits[input[i]&0xF]);
  }
  assert(hex[sizeof(input)*2]=='\0');
  assert(!raw_ble::hexEncode(input,sizeof(input),hex,sizeof(hex)-1));
  assert(!raw_ble::hexEncode(input,1,nullptr,0));
  // Split a real lossless codec block into independent validated chunks;
  // copy only after CRC verification. Duplicate or reordered chunks must
  // not silently advance the collector's next expected offset.
  RawCanBlockCodec encoder;
  for(uint32_t i=0;i<120;++i) {
    RawCanFrame f{};
    f.sequence=i+1;
    f.timestamp_us=1000000ULL+1234ULL*i;
    f.identifier=i%2?0x18DAF101:0x7E8;
    f.flags=i%2?1:0;
    f.dlc=8;
    f.data[0]=static_cast<uint8_t>(i);
    if(!encoder.append(f))break;
  }
  assert(encoder.finalize());
  std::vector<uint8_t> recovered(encoder.size(),0xEE);
  size_t next=0;
  while(next<encoder.size()) {
    const size_t n=(encoder.size()-next>raw_ble::kChunkBytes)?
        raw_ble::kChunkBytes:encoder.size()-next;
    const uint32_t expected=raw_ble::crc32(encoder.data()+next,n);
    std::memcpy(recovered.data()+next,encoder.data()+next,n);
    assert(expected==raw_ble::crc32(recovered.data()+next,n));
    if(n) {
      recovered[next]^=1;
      assert(expected!=raw_ble::crc32(recovered.data()+next,n));
      recovered[next]^=1;
    }
    next+=n;
  }
  assert(!std::memcmp(recovered.data(),encoder.data(),encoder.size()));
  RawCanFrame frames[120]{};
  uint16_t count=0;
  assert(RawCanBlockCodec::decode(recovered.data(),recovered.size(),
                                  frames,120,count));
  assert(count>0&&frames[0].sequence==1);
  std::cout<<"BLE RAW export decimal/CRC/hex/resume chunk gate PASS\n";
}
