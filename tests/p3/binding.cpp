#include "signal_v2_session.h"
#include <cassert>
#include <cstring>
#include <iostream>
#if defined(M5CAN_HIL_HOST_TEST)
#include <Arduino.h>
#define private public
#include "can_monitor.h"
#undef private
#include "signal_v2_can_owner.h"
namespace hil { void admissionHook() {} }
SerialStub Serial;
uint32_t g_test_millis=1000;
#endif
using namespace signal_v2;
static uint32_t rid=0, revision=0;
static Packet request(uint8_t op,const uint8_t* b,size_t n,bool hello=false) {
  uint8_t p[486]{};w32(p,++rid);w32(p+4,revision);p[8]=op;w16(p+10,n);
  if(n)std::memcpy(p+12,b,n);
  return encode(1,hello?0:7,hello?0:8,rid,p,n+12);
}
// Exercise actual codec + fragmented Control/ACK, including ATT MTU23.
static Packet roundtrip(const Packet& input,uint8_t lane) {
  Reassembler r;Packet out;uint8_t f[260];
  for(size_t off=0;off<input.size;) {
    auto n=fragment(input,lane,1,off,23,f);assert(n);
    auto e=r.feed(f,n,lane,100,out);off+=f[14];
    assert(e==(off==input.size?Error::None:Error::Pending));
  }
  assert(out.size==input.size && out.bytes==input.bytes);return out;
}
static Packet call(Session& s,const Packet& in,uint8_t expected,uint32_t now=100) {
  Packet ack;s.control(roundtrip(in,1),true,now,ack);
  ack=roundtrip(ack,2);assert(ack.payload()[5]==expected);return ack;
}
static void hello(Session& s) {
  rid=revision=0;s.connect(7,8);uint8_t b[12]={1,2};w16(b+4,200);
  call(s,request(1,b,12,true),Accept);
}
static void negotiate(Session& s) {
  uint8_t b[4]={1,0,0,0};auto a=call(s,request(11,b,4),Accept);
  assert(u16(a.payload()+16)==1 && u16(a.payload()+18)==68);
  assert(u32(a.payload()+20)==1); // metadata only, dynamic activation absent
}
static Packet offer(Session& s) {
  uint8_t b[4]={1,0,1,0};return call(s,request(11,b,4),Accept);
}
static void noDynamic(Session& s) {
  Simulator::Demand d{};s.demand(100,d);
  for(auto v:d)assert(v==0);
  assert(!s.active(100));
#if defined(M5CAN_HIL_HOST_TEST)
  static m5can::CanMonitor monitor;static CanOwner owner;
  static bool initialized=false;
  if(!initialized){assert(monitor.begin());owner.attach(&monitor);initialized=true;}
  const auto before=monitor.stats().tx_attempts;
  for(unsigned i=0;i<4000;++i) {
    s.demand(g_test_millis,d);owner.tick(g_test_millis,d,true,9);++g_test_millis;
    m5can::DiagnosticRequest q{};assert(xQueueReceive(monitor.query_queue_,&q,0)!=pdTRUE);
  }
  assert(monitor.stats().tx_attempts==before && before==0);
#endif
}
int main() {
  Session s;hello(s);uint8_t b[72]={1,0,2,0};
  call(s,request(11,b,72),Unsupported);noDynamic(s);
  b[0]=2;b[2]=0;call(s,request(11,b,4),Unsupported);noDynamic(s);
  b[0]=1;b[2]=1;call(s,request(11,b,4),Unsupported);
  call(s,request(11,b,3),ProtocolError);
  b[3]=1;call(s,request(11,b,4),ProtocolError);b[3]=0;
  b[2]=0;call(s,request(11,b,5),Reject);
  negotiate(s);auto offered=offer(s);
  assert(offered.payload()[16+44]==0); // no invented evidence
  std::memcpy(b+4,offered.payload()+16,68);b[2]=2;
  // Independent adversarial ACK mutations: version, schema, profile revision,
  // profile ID/switch, boot, session, epoch, source/mode, forged evidence.
  for(unsigned off:{0U,2U,4U,8U,24U,28U,32U,36U,40U,44U,48U,52U,56U,64U}) {
    b[4+off]^=1;call(s,request(11,b,72),Reject);b[4+off]^=1;noDynamic(s);
  }
  auto wrong_revision=request(11,b,72);w32(wrong_revision.payload()+4,1);
  // Re-encode after intentional header/body changes for a valid CRC.
  wrong_revision=encode(1,7,8,rid,wrong_revision.payload(),84);
  call(s,wrong_revision,BadRevision);
  auto ack_request=request(11,b,72);call(s,ack_request,Accept);noDynamic(s);
  call(s,ack_request,ProtocolError); // exact replay cannot ACK twice
  call(s,request(11,b,72),Reject); // new rid replay
  for(unsigned i=0;i<10;++i)call(s,request(8,nullptr,0),Accept);
  call(s,ack_request,ResultUnknown); // replay after bounded-history eviction
  b[2]=3;call(s,request(11,b,4),Unsupported);noDynamic(s); // no activation op
  // No profile switch / renegotiation within a metadata session.
  b[2]=0;call(s,request(11,b,4),Reject);
  // New adapter session, old ACK and old epoch must not apply.
  hello(s);negotiate(s);offer(s);b[2]=2;b[4+32]=1;
  call(s,request(11,b,72),Reject);noDynamic(s);
  // Real current-session acquisition reference, not saved/firmware evidence.
  hello(s);BindingContext c;c.authorization_epoch=9;c.source_generation=2;c.mode_epoch=3;
  c.current_session_evidence=true;c.acquisition=4;c.sequence=5;c.observed_us=100000;c.route=0x18DAF101;
  s.bindingContext(c);negotiate(s);offered=offer(s);
  const auto t=offered.payload()+16;assert(t[44]==1 && u32(t+48)==4 && u32(t+64)==0x18DAF101);
  std::memcpy(b+4,t,68);b[2]=2;
  c.authorization_epoch=10;s.bindingContext(c);call(s,request(11,b,72),Reject);noDynamic(s);
  // Losing evidence or changing source/mode also invalidates an issued offer.
  for(unsigned field=0;field<3;++field) {
    hello(s);s.bindingContext(c);negotiate(s);offered=offer(s);std::memcpy(b+4,offered.payload()+16,68);
    auto changed=c;
    if(field==0)changed.current_session_evidence=false;
    if(field==1)++changed.source_generation;
    if(field==2)++changed.mode_epoch;
    s.bindingContext(changed);call(s,request(11,b,72),Reject);noDynamic(s);
  }
  std::cout<<"PASS codec/session: unknown version, revision/schema/ID mismatch, old boot/session/epoch, replay, switch, forged/lost evidence, source/mode change; dynamic LIVE/TX=0 before and after ACK\n";
}
