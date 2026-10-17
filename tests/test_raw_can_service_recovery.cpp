#include "raw_can_service.h"
#include <cassert>
#include <cstring>
#include <vector>
using namespace m5can;

class Flash final : public RawCanFlash {
 public:
  std::vector<uint8_t> data=std::vector<uint8_t>(4096*6,0xff);
  bool readable=true;
  uint32_t size()const override{return data.size();}
  bool read(uint32_t off,void* p,size_t n)override {
    if(!readable || size_t(off)+n>data.size())return false;
    std::memcpy(p,data.data()+off,n);return true;
  }
  bool write(uint32_t off,const void* p,size_t n)override {
    if(size_t(off)+n>data.size())return false;
    const auto* bytes=static_cast<const uint8_t*>(p);
    for(size_t i=0;i<n;i++) {
      if((data[off+i]&bytes[i])!=bytes[i])return false;
      data[off+i]&=bytes[i];
    }
    return true;
  }
};
static Flash* flash;
namespace m5can {
RawCanJournal& rawCanJournal(){static RawCanJournal journal;return journal;}
bool initializeRawCanJournal(){return rawCanJournal().begin(*flash);}
}
static void retained_session(bool complete) {
  Flash storage;flash=&storage;
  CanMonitor can;
  RawCanRecordingService first(can);
  assert(first.begin() && first.state()==RawCanRecordingService::State::Idle);
  assert(first.session()==0 && first.dropped()==0);
  RawCanBlockCodec codec;RawCanFrame frame{};
  frame.sequence=1;frame.timestamp_us=123456789;
  frame.identifier=0x7e8;frame.dlc=8;
  assert(codec.append(frame) && codec.finalize());
  assert(rawCanJournal().append(32011917,RawCanJournal::Data,
      codec.data(),codec.size(),7));
  if(complete)assert(rawCanJournal().append(32011917,RawCanJournal::End,nullptr,0,9));
  // A new service and rescan model reboot: live RAM counters start at zero.
  CanMonitor rebooted_can;
  RawCanRecordingService rebooted(rebooted_can);
  assert(rebooted.begin());
  assert(rebooted.session()==32011917);
  assert(rebooted.dropped()==(complete?9U:7U));
  assert(rebooted.state()==(complete?RawCanRecordingService::State::Complete:
                                      RawCanRecordingService::State::RecoveryRequired));
  assert(!rebooted.recording() && !rebooted.stopping());
  assert(!rebooted.start(99)); // Never overwrite retained records or auto-resume.
  storage.readable=false;
  assert(!rebooted.begin());
  assert(rebooted.state()==RawCanRecordingService::State::Unavailable);
  assert(rebooted.session()==0 && rebooted.dropped()==0);
}
int main(){retained_session(true);retained_session(false);}
