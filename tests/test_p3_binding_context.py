"""Run the exact production GATT evidence projection with synthetic identities."""
import pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
s=(root/'src/signal_v2_gatt.cpp').read_text()
block='// Read-only O-2 metadata.'+s.split('      // Read-only O-2 metadata.',1)[1].split('\n#endif',1)[0]
source=r'''
#include "signal_v2_session.h"
#include <atomic>
#include <cassert>
using namespace signal_v2;
struct Link {uint16_t handle=1;uint32_t generation=2;} l;
struct Peer {Session session;} p;
struct Owner {CanSamples values;bool fault=false;const CanSamples& samples(){return values;}bool stopped(){return fault;}} owner;
struct Arm {bool known=true;uint32_t epoch=9,handle=1,generation=2,session=8;} arm_owner;
std::atomic<bool> can_armed{true};std::atomic<uint32_t> can_authorization_epoch{9};
uint32_t now=100,rid=0;
void project(){@BLOCK@}
Packet command(unsigned op,const uint8_t* body,size_t n,bool hello=false){
 uint8_t b[100]{};w32(b,++rid);b[8]=op;w16(b+10,n);if(n)std::copy(body,body+n,b+12);
 return encode(1,hello?0:7,hello?0:8,rid,b,n+12);
}
unsigned evidence(){
 // Activate only the explicitly legacy PROFILE path; binding itself never does.
 p.session.connect(7,8);rid=0;Packet out;uint8_t h[12]={1,2};w16(h+4,200);
 p.session.control(command(1,h,12,true),true,0,out);
 uint8_t profile[32]{};w32(profile,1);profile[4]=1;auto e=profile+8;
 w16(e,1);e[2]=1;w16(e+4,0x0d);w32(e+12,1);w16(e+16,200);w16(e+18,400);w16(e+20,600);
 p.session.control(command(2,profile,32),true,0,out);assert(out.payload()[5]==Accept);
 project();uint8_t b[16]{};w16(b,1);
 auto send=[&]{auto q=command(11,b,4);w32(q.payload()+4,1);p.session.control(q,true,now,out);assert(out.payload()[5]==Accept);};
 send();b[2]=1;send();return out.payload()[16+44];
}
int main(){
 owner.values.source_id=0x18DAF101;owner.values.acquired=4;auto &v=owner.values.values[1];
 v.observed_us=100000;v.quality=0;v.sequence=5;
 assert(evidence()==1);
 arm_owner.session=99;assert(evidence()==0);arm_owner.session=8;
 arm_owner.generation=99;assert(evidence()==0);arm_owner.generation=2;
 arm_owner.epoch=99;assert(evidence()==0);arm_owner.epoch=9;
 arm_owner.handle=99;assert(evidence()==0);arm_owner.handle=1;
 arm_owner.known=false;assert(evidence()==0);arm_owner.known=true;
 can_armed=false;assert(evidence()==0);can_armed=true;
 owner.fault=true;assert(evidence()==0);owner.fault=false;
 now=701;assert(evidence()==0);now=99;assert(evidence()==0);now=100;
 v.quality=2;assert(evidence()==0);v.quality=0;
 owner.values.source_id=0x18DAF102;assert(evidence()==0);
}
'''.replace('@BLOCK@',block)
with tempfile.TemporaryDirectory() as d:
    cpp=pathlib.Path(d)/'context.cpp';exe=pathlib.Path(d)/'context';cpp.write_text(source)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1','-DM5CAN_SIGNAL_V2_RP8_PROFILE=1','-Iinclude',str(cpp),
      'src/signal_v2_session.cpp','src/signal_v2_codec.cpp','-o',str(exe)],cwd=root,check=True)
    subprocess.run([str(exe)],check=True)
print('PASS production GATT evidence: same owner/link/session/epoch, freshness/quality/route; no cross-session/stale/fault evidence')
