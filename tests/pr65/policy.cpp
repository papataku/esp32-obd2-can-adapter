#include "diagnostic_policy.h"
#include <cassert>
#include <iostream>
using namespace m5can;
int main(){
 DiagnosticRequest q{};q.can_id=0x18DB33F1;q.length=2;q.data[0]=1;q.data[1]=0x0C;
#if defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)
 assert(!DiagnosticPolicy::allowedReadRequest(q));q.signal_v2=true;q.length=3;q.data[2]=0x0D;
 assert(!DiagnosticPolicy::allowedReadRequest(q));
 q.can_id=0x18DA01F1;
#if defined(M5CAN_SIGNAL_V2_RP8_PROFILE)
 assert(DiagnosticPolicy::allowedReadRequest(q));
#else
 assert(!DiagnosticPolicy::allowedReadRequest(q));
#endif
 q.can_id=0x18DA06F1;assert(!DiagnosticPolicy::allowedReadRequest(q));
 std::cout<<"PASS V2 profile-only physical policy, legacy rejected; functional rejected\n";
#else
 assert(DiagnosticPolicy::allowedReadRequest(q));
 q.can_id=0x18DA01F1;q.length=3;q.data[0]=0x22;q.data[1]=0xF1;q.data[2]=0x90;
 assert(DiagnosticPolicy::allowedReadRequest(q));
 q.data[0]=1;q.length=2;assert(!DiagnosticPolicy::allowedReadRequest(q));
#if defined(M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL)
 q.can_id=0x18DB33F1;q.length=3;q.data[0]=1;q.data[1]=0x0C;q.data[2]=0x0D;
 assert(DiagnosticPolicy::allowedReadRequest(q));
#endif
 std::cout<<"PASS legacy/default functional Mode01 and physical UDS preserved\n";
#endif
}
