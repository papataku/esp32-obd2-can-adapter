#include <cassert>
#include <cstdint>
#include <iostream>
#include <initializer_list>

#include "adaptive_multi_pid.h"
#include "diagnostic_transaction.h"

using namespace m5can;

static DiagnosticRequest req(uint8_t pid) {
  DiagnosticRequest r{};
  r.can_id=0x18DB33F1U;
  r.length=2;
  r.data[0]=0x01;
  r.data[1]=pid;
  return r;
}

static DiagnosticResult reply(uint32_t can_id,
                              std::initializer_list<uint8_t> data) {
  DiagnosticResult r{};
  r.status=DiagnosticStatus::Ok;
  r.response_count=1;
  auto& m=r.responses[0];
  m.can_id=can_id;
  m.length=static_cast<uint16_t>(data.size());
  size_t i=0;
  for(uint8_t b : data) m.data[i++]=b;
  return r;
}

int main() {
  const auto rpm=req(0x0C),speed=req(0x0D);
  AdaptiveMultiPid cap;
  assert(cap.eligible(rpm,speed));
  assert(!cap.eligible(speed,rpm));
  const auto grouped=AdaptiveMultiPid::group(rpm,speed);
  assert(grouped.length==3 && grouped.data[0]==0x01 &&
         grouped.data[1]==0x0C && grouped.data[2]==0x0D);
  assert(DiagnosticPolicy::allowedReadRequest(grouped));
  const uint8_t partial[]={0x41,0x0C,0x10,0x20};
  assert(DiagnosticPolicy::responseMatches(grouped,partial,sizeof(partial)));

  DiagnosticRawFrame frames[2]{};
  uint32_t responder=0;
  const auto good=reply(0x18DAF101U,{0x41,0x0C,0x13,0x88,0x0D,0x50});
  assert(cap.decode(good,frames,responder));
  assert(responder==0x18DAF101U);
  assert(frames[0].data[0]==4 && frames[0].data[2]==0x0C &&
         frames[0].data[3]==0x13 && frames[0].data[4]==0x88);
  assert(frames[1].data[0]==3 && frames[1].data[2]==0x0D &&
         frames[1].data[3]==0x50);
  cap.markSupported(responder);
  assert(cap.support()==AdaptiveMultiPid::Support::Supported);
  assert(cap.decode(reply(0x18DAF101U,{0x41,0x0D,0x60,0x0C,0x10,0x00}),
                    frames,responder)); // reply need not match PID order
  assert(frames[0].data[3]==0x10 && frames[1].data[3]==0x60);
  assert(!cap.decode(reply(0x18DAF102U,{0x41,0x0C,1,2,0x0D,3}),
                     frames,responder));
  assert(!cap.decode(reply(0x18DAF101U,{0x41,0x0C,1,2}),frames,responder));
  assert(!cap.decode(reply(0x18DAF101U,{0x41,0x0C,1,2,0x0C,3}),
                     frames,responder));
  assert(!cap.decode(reply(0x18DAF101U,{0x7F,0x01,0x12}),frames,responder));
  auto timedout=good;
  timedout.status=DiagnosticStatus::NoData;
  assert(!cap.decode(timedout,frames,responder));
  cap.disable();
  assert(!cap.eligible(rpm,speed));

  // Ensure a grouped Single Frame reaches the ISO-TP transaction layer
  // but an unrelated mode or CAN responder does not.
  DiagnosticTransaction t;
  t.begin(grouped,1000);
  TransactionFrame wrong{};
  wrong.extended=true;
  wrong.timestamp_us=1100;
  wrong.can_id=0x18DAF102U;
  wrong.dlc=8;
  wrong.data[0]=0x04;
  wrong.data[1]=0x42; // unexpected service, ignored
  wrong.data[2]=0x0C;
  assert(t.onFrame(wrong).event==TransactionEvent::Ignored);
  TransactionFrame sf{};
  sf.extended=true;
  sf.timestamp_us=1200;
  sf.can_id=0x18DAF101U;
  sf.dlc=8;
  const uint8_t bytes[]={6,0x41,0x0C,0x13,0x88,0x0D,0x50,0};
  for(unsigned i=0;i<8;++i)sf.data[i]=bytes[i];
  assert(t.onFrame(sf).event==TransactionEvent::Completed);
  assert(t.result().response_count==1);
  responder=0;
  AdaptiveMultiPid fresh;
  DiagnosticResult completed=t.result();
  completed.status=DiagnosticStatus::Ok;
  assert(fresh.decode(completed,frames,responder));

  std::cout << "PASS bounded automatic multi-PID probe and response validation\n";
  return 0;
}
