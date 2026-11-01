"""Phase A regression: production GATT + Session/CanOwner/monitor/ELM, IO stubbed.

DISCONNECT_SOURCE_ROOT selects base production GATT/ELM for before/after runs.
"""
import os
import pathlib
import subprocess
import tempfile
import unittest
from test_signal_v2_b01_policy import ROOT, OWNER, flags_by_env

class DisconnectOwnership(unittest.TestCase):
    def test_regressions(self):
        source_root=pathlib.Path(os.environ.get('DISCONNECT_SOURCE_ROOT',ROOT))
        source=(source_root/'src/signal_v2_gatt.cpp').read_text()
        def block(a,b):return a+source.split(a,1)[1].split(b,1)[0]
        parts={
            '@GRANT@':block('bool grantCanArm(uint32_t now,uint32_t seen) {','\n#endif\nvoid manage('),
            '@STOP@':block('  if(!std::strcmp(cmd,"SIGV2 CAN STOP")) {','\n#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)\n  if(!std::strcmp(cmd,"SIGV2 CAN READY"))'),
            '@ARM@':block('  if(!std::strcmp(cmd,"SIGV2 CAN ARM")) {','  // PR-C2 stage S1'),
            '@DISCONNECT@':block('void disconnected(uint16_t handle) {','void gap('),
            '@TTL@':block('  if(can_armed && int32_t(armed_until-now)<=0){','  owner.tick(now,demand,can_armed,'),
            '@AUTO@':block('void manualCanStop() {','\n#endif\nbool usbCommand('),
        }
        ui=(ROOT/'src/ui.cpp').read_text();a='bool Ui::pollDemoToggle() {'
        parts['@TOUCH@']=a+ui.split(a,1)[1].split('\nvoid Ui::update(',1)[0]
        f=(ROOT/'tests/owner_auto_arm_fixture.cpp.in').read_text()
        for k,v in parts.items():f=f.replace(k,v)
        f=f.replace('static m5can::CanStats health{};','')
        a=f.index('namespace m5can {');b=f.index('\nCanOwner owner;',a)
        f=f[:a]+(ROOT/'tests/pr70/runtime.inc').read_text()+f[b:]
        f=f[:f.index('int main(')]
        if 'struct DisconnectPublication {' in source:
            f+=block('struct DisconnectPublication {','\n#endif\nvoid poll()')
        else:f+='struct DisconnectPublication {};\n'
        f+='''
int main(int argc,char** argv){
 assert(argc==2);const std::string scenario=argv[1];
 m5can::CanMonitor m;CanOwnerTestAccess::setup(m);owner.attach(&m);
 owner.tick(0,{},false,1);connectSession(19,100);
 serviceAutoArm(100);serviceAutoArm(2101);assert(can_armed);
 g_test_millis=2101;
 const uint32_t epoch=can_authorization_epoch.load();
 owner.tick(2101,{},true,epoch);
 assert(m.acquireLease(550,epoch));
 // Seed the previously acquired sample cache; acquisition IO is outside this test.
 auto &samples=const_cast<CanSamples&>(owner.samples());
 samples.source_id=0x18DAF101;
 for(auto &v:samples.values){v.quality=0;v.length=3;v.sequence=4;v.observed_us=2101000;}
 const auto before=samples;
 links[1].active=true;links[1].handle=8;
 links[1].registered=links[1].encrypted=links[1].bonded=false;
 links[1].status=links[1].data=false;
 {DisconnectPublication publish; (void)publish;}
 if(scenario=="lease") {
   m5can::ElmCompat elm;auto result=elm.execute("ATM5TX0");
   // Both production ELM dispatchers execute revoke only for this action.
   if(result.action==m5can::ElmAction::LeaseRevoke)m.revokeLease();
   assert(m.leaseRemainingMs()>0 && can_armed && can_authorization_epoch==epoch);
   assert(result.action==m5can::ElmAction::ReplyOnly && !std::strcmp(result.reply,"?"));
 } else {
   if(scenario=="transition"){DisconnectPublication in_progress;(void)in_progress;disconnected(8);}
   else {
     if(scenario=="expired")g_test_millis=3100;
     if(scenario=="generation")++links[0].generation;
     if(scenario=="unsubscribe")links[0].status=false;
     disconnected(scenario=="owner"?7:scenario=="unknown"?99:8);
   }
   const bool preserve=scenario=="legacy";
   assert(bool(can_armed)==preserve);
   assert(can_authorization_epoch==epoch+(preserve?0:1));
   assert((m.leaseRemainingMs()>0)==preserve);
   owner.tick(g_test_millis,{},can_armed,can_authorization_epoch);
   if(preserve){
     assert(links[0].active && samples.source_id==before.source_id);
     assert(samples.invalidations==before.invalidations && samples.mode_epoch==before.mode_epoch);
     for(unsigned i=0;i<5;++i){assert(samples.values[i].quality==0);
       assert(samples.values[i].sequence==before.values[i].sequence);
       assert(samples.values[i].observed_us==before.values[i].observed_us);}
   } else {assert(samples.source_id==0);for(const auto &v:samples.values)assert(v.quality!=0);}
   if(scenario=="owner"){
     serviceAutoArm(2102);connectSession(20,2200);serviceAutoArm(2200);
     serviceAutoArm(4199);assert(!can_armed);
     serviceAutoArm(4200);assert(can_armed && can_authorization_epoch>epoch);
   }
 }
 std::cout<<"PASS "<<scenario<<"\\n";
}
'''
        f='#include "elm_compat.h"\n'+f
        _,flags=flags_by_env()
        with tempfile.TemporaryDirectory() as d:
            cpp=pathlib.Path(d)/'ownership.cpp';cpp.write_text(f)
            exe=pathlib.Path(d)/'ownership'
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',
                '-DM5CAN_TEST_S1_STOP_RACE=1','-DPR70_EPOCH_API=1',
                *[x for x in flags[OWNER] if x.startswith(('-D','-U'))],
                '-Itests/pr70/stubs','-Itests/pr65/stubs','-Itests/host_stubs','-Iinclude','-Isrc',
                str(cpp),'src/can_monitor.cpp',str(source_root/'src/elm_compat.cpp'),
                'src/diagnostic_policy.cpp','src/diagnostic_transaction.cpp','src/isotp_reassembler.cpp',
                'src/signal_v2_session.cpp','src/signal_v2_codec.cpp','src/signal_v2_can_samples.cpp',
                'src/signal_v2_discovery_profile.cpp','-o',str(exe)],cwd=ROOT,check=True)
            for scenario in ('legacy','owner','unknown','lease','transition','expired','generation','unsubscribe'):
                with self.subTest(scenario=scenario):
                    r=subprocess.run([str(exe),scenario],capture_output=True,text=True)
                    print(scenario, 'PASS' if r.returncode==0 else 'FAIL',flush=True)
                    self.assertEqual(r.returncode,0,r.stdout+r.stderr)

if __name__=='__main__':unittest.main()
