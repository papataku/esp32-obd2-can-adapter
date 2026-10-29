#include "signal_v2_session.h"
#include "signal_v2_group_plan.h"
#include <cassert>
#include <array>
using namespace signal_v2;
static Packet command(uint32_t id,uint32_t revision,uint8_t op,
                      const uint8_t* body,size_t len,uint32_t session=19) {
  uint8_t bytes[400]{};
  w32(bytes,id);w32(bytes+4,revision);bytes[8]=op;w16(bytes+10,len);
  for(size_t i=0;i<len;++i)bytes[12+i]=body[i];
  return encode(1,op==1?0:17,op==1?0:session,id,bytes,12+len);
}
static void profile(Session& s,uint16_t period,uint32_t sid=19) {
  Packet ack;
  uint8_t hello[12]{1,2};
  w16(hello+4,512);
  s.control(command(1,0,1,hello,12,sid),true,10,ack);
  assert(ack.payload()[5]==Accept);
  uint8_t p[8+24*5]{};
  w32(p,1);p[4]=5;
  for(unsigned i=0;i<5;++i){
    uint8_t *e=p+8+i*24;
    w16(e,i+1);e[2]=1;w16(e+4,CanSamples::Pids[i]);
    e[6]=i==0?0:1;
    w32(e+8,0);w32(e+12,1);
    w16(e+16,period);w16(e+18,400);w16(e+20,600);
  }
  s.control(command(2,0,2,p,sizeof(p),sid),true,100,ack);
  assert(ack.payload()[5]==Accept);
}
int main(){
  Session a,b; a.connect(17,19);b.connect(17,20);
  profile(a,100);profile(b,200,20);
  Simulator::Demand unionDemand{};
  a.demand(101,unionDemand);b.demand(101,unionDemand);
  for(auto p:unionDemand)assert(p==100); // two-central MIN union
  b.invalidate();unionDemand={};
  a.demand(101,unionDemand);b.demand(101,unionDemand);
  for(auto p:unionDemand)assert(p==100); // second peer expiry isolated
  CanSamples can;
  can.source_id=0x18DAF106;
  for(unsigned i=0;i<5;++i){
    auto &v=can.values[i];
    v.length=CanSamples::Lengths[i];
    v.raw[0]=0x41;v.raw[1]=CanSamples::Pids[i];
    for(unsigned j=2;j<v.length;++j)v.raw[j]=1;
    v.observed_us=100000;v.sequence=1;v.quality=0;
  }
  Packet data;
  assert(a.produce(100,data,nullptr,false,&can)&&data.bytes[4]==0x21);
  assert(a.produce(101,data,nullptr,false,&can)&&data.bytes[4]==0x20);
  assert(data.payload()[0]==5);
  assert(a.canPacketCurrent(101,data,can));
  assert(!a.canPacketCurrent(702,data,can)); // delayed BLE queue cannot be Fresh
  size_t offset=2;
  for(unsigned i=0;i<5;++i){
    auto r=data.payload()+offset;
    assert(r[2]==2 && r[3]==1 && r[7]==0);
    assert(u32(r+34)==1000);
    assert(u32(r+22)==0x18DAF106 && u32(r+38)==1);
    assert(u16(r+8)==CanSamples::Pids[i]);
    assert(r[42]==CanSamples::Lengths[i] && r[44]==0x41);
    offset+=u16(r);
  }
  assert(offset==data.size-Header);
  uint8_t snapshotReq[8]{};
  w32(snapshotReq,u32(data.bytes.data()+16));w32(snapshotReq+4,1);
  a.control(command(3,1,7,snapshotReq,8),true,110,data);
  assert(data.payload()[5]==Accept);
  assert(a.produce(111,data,nullptr,false,&can)&&data.bytes[4]==0x21);
  assert(data.payload()[12]==5);
  assert(data.payload()[13]==2 && u32(data.payload()+16)==3);
  assert(u32(data.payload()+20+34)==11000);
  assert(u32(data.payload()+20+38)==1); // snapshot never reobserves
  assert(a.canPacketCurrent(111,data,can));
  const Packet oldRequest=data;
  a.control(command(4,1,7,snapshotReq,8),true,120,data);
  assert(a.produce(121,data,nullptr,false,&can)&&data.bytes[4]==0x21);
  assert(data.payload()[12]==5 && u32(data.payload()+16)==4);
  assert(u32(data.payload()+20+34)==21000);
  assert(u32(data.payload()+20+38)==1); // recompute age without re-observing
  assert(!a.canPacketCurrent(121,oldRequest,can)); // same epoch, different request
  assert(a.canPacketCurrent(121,data,can));
  const Packet oldEpoch=data;
  ++can.source_generation;++can.mode_epoch;
  assert(!a.canPacketCurrent(111,oldEpoch,can));
  a.control(command(5,1,7,snapshotReq,8),true,130,data);
  assert(a.produce(131,data,nullptr,false,&can)&&data.bytes[4]==0x21);
  assert(data.payload()[12]==0); // old-epoch cached Snapshot cannot survive
  assert(data.payload()[13]==2 && u32(data.payload()+16)==5);
  assert(!a.canPacketCurrent(121,oldEpoch,can)); // old request echo is stale
  assert(a.produce(201,data,nullptr,false,&can)&&data.bytes[4]==0x20);
  assert(u32(data.payload()+2+14)==can.source_generation);
  assert(u32(data.payload()+2+18)==can.mode_epoch);
  assert(a.produce(702,data,nullptr,false,&can)&&data.bytes[4]==0x20);
  assert(data.payload()[2+7]==8 && data.payload()[2+42]==0);
  can.invalidate(3);
  assert(a.produce(802,data,nullptr,false,&can)&&data.payload()[2+7]==8);
  // Structural 16 entries do not authorize a >486-byte Snapshot.
  Session oversize;oversize.connect(17,19);
  Packet reply;uint8_t hello[12]{1,2};w16(hello+4,512);
  oversize.control(command(1,0,1,hello,12),true,10,reply);
  uint8_t tooLarge[8+24*10]{};w32(tooLarge,1);tooLarge[4]=10;
  for(unsigned i=0;i<10;++i){
    auto e=tooLarge+8+i*24;w16(e,i+1);
    e[2]=1;w16(e+4,0x9A);e[6]=1;
    w32(e+8,0);w32(e+12,1);
    w16(e+16,200);w16(e+18,400);w16(e+20,600);
  }
  oversize.control(command(2,0,2,tooLarge,sizeof(tooLarge)),true,100,reply);
  assert(reply.payload()[5]==Reject && reply.payload()[16]==1);
  // Ten speed records fit legacy prefix16 exactly but not correlated prefix20.
  for(unsigned i=0;i<10;++i){auto e=tooLarge+8+i*24;w16(e+4,0x0D);e[6]=0;}
  oversize.control(command(3,0,2,tooLarge,sizeof(tooLarge)),true,110,reply);
  assert(reply.payload()[5]==Reject && reply.payload()[16]==1);
  const uint16_t future[7]={0x0D,0x0C,0x05,0x5B,0x9A,0x10,0x11};
  const GroupPlan split=planSplit(future,7);
  assert(split.count==2 && split.slots[0].count==6 &&
         split.slots[1].count==3);
  for(unsigned i=0;i<split.count;++i){
    assert(split.slots[i].pids[0]==0x0D);
    assert(split.slots[i].pids[1]==0x0C);
  }
}
