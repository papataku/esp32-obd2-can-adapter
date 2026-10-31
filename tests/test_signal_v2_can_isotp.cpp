#include "diagnostic_transaction.h"
#include "signal_v2_can_samples.h"
#include <array>
#include <cassert>
using namespace m5can;
static TransactionFrame frame(uint32_t id,bool ext,uint8_t dlc,
                              const uint8_t* data,uint64_t t){
  TransactionFrame f{};
  f.can_id=id;f.extended=ext;f.dlc=dlc;f.timestamp_us=t;
  for(unsigned i=0;i<dlc;++i)f.data[i]=data[i];
  return f;
}
int main(){
  DiagnosticRequest physical{};
  physical.signal_v2=true;physical.can_id=0x18DA01F1;
  physical.length=3;
  physical.data[0]=1;physical.data[1]=0x0C;physical.data[2]=0x0D;
  DiagnosticTransaction probe;
  probe.begin(physical,1000,100);
  const uint8_t discovery[]={6,0x41,0x0C,0x13,0x88,0x0D,0x28};
  assert(probe.onFrame(frame(0x18DAF101,true,7,discovery,2000)).event==
         TransactionEvent::Completed);
  auto found=probe.result();found.status=DiagnosticStatus::Ok;
  assert(found.response_count==1);
  assert(DiagnosticPolicy::approvedPhysicalEcu(0x18DA01F1));
  assert(!DiagnosticPolicy::observedPhysicalEcu(0x18DA01F1));
  DiagnosticRequest req{};
  req.signal_v2=true;req.can_id=0x18DA01F1;req.length=6;
  const uint8_t selection[]={1,0x0D,0x0C,0x05,0x5B,0x9A};
  for(unsigned i=0;i<6;++i)req.data[i]=selection[i];
  assert(DiagnosticPolicy::allowedReadRequest(req));
  DiagnosticTransaction valid;
  valid.begin(req,5000,100);
  // A diagnostic source/width mismatch invalidates the entire V2 transaction
  // before any further FC or otherwise valid pinned-ECU completion.
  const uint8_t wrong[]={3,0x41,0x0D,0x11};
  for(auto bad:{frame(0x18DAF102,true,4,wrong,6000),
                frame(0x7E8,false,4,wrong,6001)}){
    DiagnosticTransaction mismatch;mismatch.begin(req,5000,100);
    assert(mismatch.onFrame(bad).event==TransactionEvent::ProtocolError);
    assert(mismatch.result().response_count==0);
  }
  const uint8_t ff[]={0x10,0x11,0x41,0x0D,0x40,0x0C,0x13,0x88};
  const uint8_t cf1[]={0x21,0x05,0x80,0x5B,0x7F,0x9A,0,0};
  const uint8_t cf2[]={0x22,0x4B,0,0,0x64};
  auto need=valid.onFrame(frame(0x18DAF101,true,8,ff,6100));
  assert(need.event==TransactionEvent::NeedFlowControl);
  assert(need.flow_control_can_id==0x18DA01F1);
  assert(valid.onFrame(frame(0x18DAF101,true,8,cf1,6200)).event==
         TransactionEvent::ResponseProgress);
  assert(valid.onFrame(frame(0x18DAF101,true,5,cf2,6300)).event==
         TransactionEvent::Completed);
  assert(valid.result().response_count==1);
  const auto &message=valid.result().responses[0];
  assert(message.length==17&&message.can_id==0x18DAF101);
  std::array<signal_v2::CanSamples::Sample,5> decoded{};
  assert(signal_v2::CanSamples::decodeGroup(message.data,message.length,
                                             true,decoded));
  DiagnosticTransaction corrupt;
  corrupt.begin(req,10000,100);
  assert(corrupt.onFrame(frame(0x18DAF101,true,8,ff,11000)).event==
         TransactionEvent::NeedFlowControl);
  const uint8_t bad_cf[]={0x23,0x05,0x80};
  assert(corrupt.onFrame(frame(0x18DAF101,true,3,bad_cf,12000)).event==
         TransactionEvent::ProtocolError);
  DiagnosticTransaction timeout;
  timeout.begin(req,13000,20);
  timeout.onFrame(frame(0x18DAF101,true,8,ff,14000));
  assert(timeout.expireIsoTp(34001)>0);
  // Negative response is a completed transaction but is never a PID value.
  DiagnosticTransaction negative;
  negative.begin(req,40000,100);
  const uint8_t nrc[]={3,0x7F,0x01,0x11};
  assert(negative.onFrame(frame(0x18DAF101,true,4,nrc,41000)).event==
         TransactionEvent::Completed);
  assert(negative.result().responses[0].data[0]==0x7F);
  assert(!signal_v2::CanSamples::decodeGroup(
      negative.result().responses[0].data,
      negative.result().responses[0].length,true,decoded));
}
