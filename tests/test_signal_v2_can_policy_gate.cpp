#include "diagnostic_policy.h"
#include <cassert>
using namespace m5can;
int main(){
  DiagnosticRequest functional{};
  functional.signal_v2=true;functional.can_id=0x18DB33F1;
  functional.length=3;functional.data[0]=1;
  functional.data[1]=0x0C;functional.data[2]=0x0D;
  assert(DiagnosticPolicy::allowedReadRequest(functional));
  DiagnosticRequest wrong=functional;
  wrong.signal_v2=false;
  assert(!DiagnosticPolicy::allowedReadRequest(wrong)); // V1 TX locked
  wrong=functional;wrong.can_id=0x7DF;
  assert(!DiagnosticPolicy::allowedReadRequest(wrong)); // 11bit TX locked
  DiagnosticResult response{};
  response.status=DiagnosticStatus::Ok;
  response.response_count=1;
  response.responses[0].can_id=0x18DAF106;
  response.responses[0].length=6;
  const uint8_t reply[6]={0x41,0x0C,0x13,0x88,0x0D,0x40};
  for(unsigned i=0;i<6;++i)response.responses[0].data[i]=reply[i];
  response.raw_frame_count=1;
  response.raw_frames[0].can_id=0x18DAF106;
  response.raw_frames[0].dlc=7;
  response.raw_frames[0].data[0]=6;
  for(unsigned i=0;i<6;++i)response.raw_frames[0].data[i+1]=reply[i];
  DiagnosticPolicy::observeFunctionalPairs(functional,response);
  DiagnosticRequest physical{};
  physical.signal_v2=true;physical.can_id=0x18DA06F1;
  physical.length=6;
  const uint8_t group[6]={1,0x0D,0x0C,0x05,0x5B,0x9A};
  for(unsigned i=0;i<6;++i)physical.data[i]=group[i];
  assert(DiagnosticPolicy::observedPhysicalEcu(physical.can_id));
  assert(DiagnosticPolicy::allowedReadRequest(physical));
  assert(DiagnosticPolicy::responseBelongsToRequest(physical,0x18DAF106));
  assert(!DiagnosticPolicy::responseBelongsToRequest(physical,0x18DAF107));
  assert(!DiagnosticPolicy::responseBelongsToRequest(physical,0x7E8));
  physical.data[5]=0xFF;
  assert(!DiagnosticPolicy::allowedReadRequest(physical)); // unapproved PID
  physical.data[5]=0x9A;physical.data[0]=0x22;
  assert(!DiagnosticPolicy::allowedReadRequest(physical)); // no UDS
  physical.data[0]=1;physical.signal_v2=false;
  assert(!DiagnosticPolicy::allowedReadRequest(physical)); // source tag
}
