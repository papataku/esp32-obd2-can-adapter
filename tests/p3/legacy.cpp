// Same driver compiled against immutable base AND head production sources.
#include "signal_v2_session.h"
#include <cstring>
#include <iomanip>
#include <iostream>
using namespace signal_v2;
static void dump(const Packet &p) {
  for(size_t i=0;i<p.size;++i)std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(p.bytes[i]);
  std::cout<<'\n';
}
int main() {
  Session s;s.connect(7,8);uint32_t rid=0;
  auto run=[&](unsigned op,uint32_t rev,const uint8_t *b,size_t n,uint32_t now,bool auth=true) {
    uint8_t p[486]{};w32(p,++rid);w32(p+4,rev);p[8]=op;w16(p+10,n);
    if(n)std::memcpy(p+12,b,n);
    auto in=encode(1,op==1?0:7,op==1?0:8,rid,p,n+12);Packet out;
    s.control(in,auth,now,out);dump(out);
    s.control(in,auth,now+1,out);dump(out); // old duplicate semantics
  };
  uint8_t hello[12]={1,2};w16(hello+4,200);run(1,0,hello,12,0);
  uint8_t b[128]{};w32(b,1);b[4]=5;
  for(unsigned i=0;i<5;++i) {
    auto e=b+8+24*i;w16(e,i+1);e[2]=1;w16(e+4,CanSamples::Pids[i]);e[6]=i?1:0;
    w32(e+12,1);w16(e+16,200);w16(e+18,400);w16(e+20,600);
  }
  run(2,0,b,128,100);
  CanSamples can;can.source_id=0x18DAF101;
  for(unsigned i=0;i<5;++i){auto &v=can.values[i];v.length=CanSamples::Lengths[i];v.quality=0;v.sequence=1;v.observed_us=100000;v.raw[0]=0x41;v.raw[1]=CanSamples::Pids[i];}
  auto produce=[&](uint32_t now) {
    Packet out;
    auto ok=s.produce(now,out,nullptr,true,
#ifdef M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL
      &can
#else
      nullptr
#endif
    );
    std::cout<<"produce "<<ok<<'\n';if(ok)dump(out);
    Simulator::Demand d{};s.demand(now,d);
    std::cout<<"active "<<s.active(now)<<" revision "<<s.revision()<<" deadline "<<s.leaseDeadline()<<" demand ";
    for(auto v:d)std::cout<<v<<',';
    std::cout<<'\n';
  };
  produce(100);produce(101);produce(299);produce(300);
  run(4,1,nullptr,0,350);w32(b+4,1);run(7,1,b,8,400);produce(401);
  run(8,1,nullptr,0,500);run(9,1,b,8,501);
  w32(b,2);run(5,1,b,4,600);produce(601);
  w32(b,3);run(6,2,b,4,700);produce(701);produce(702);
  run(4,2,nullptr,0,800); // mismatched current V2 PROFILE revision
  run(4,3,nullptr,0,4000);produce(4001); // expired
  run(10,3,nullptr,0,4002);run(2,3,b,7,4003); // bad length
  s.invalidate();produce(4100);run(8,0,nullptr,0,4200,false);
}
