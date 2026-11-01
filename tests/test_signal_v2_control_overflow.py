"""PR71 reviewer reproduction: production GATT, owner and monitor; IO only stubbed."""
import pathlib, sys, subprocess, tempfile, os, unittest
root=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tests'))
from test_signal_v2_b01_policy import flags_by_env, OWNER
s=pathlib.Path(os.environ.get('SIGNAL_V2_GATT_SOURCE', root/'src/signal_v2_gatt.cpp')).read_text()
def block(a,b): return a+s.split(a,1)[1].split(b,1)[0]
parts={
'@GRANT@':block('bool grantCanArm(uint32_t now,uint32_t seen) {','\n#endif\nvoid manage('),
'@STOP@':block('  if(!std::strcmp(cmd,"SIGV2 CAN STOP")) {','\n#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)\n  if(!std::strcmp(cmd,"SIGV2 CAN READY"))'),
'@ARM@':block('  if(!std::strcmp(cmd,"SIGV2 CAN ARM")) {','  // PR-C2 stage S1'),
'@DISCONNECT@':block('void disconnected(uint16_t handle) {','void gap('),
'@TTL@':block('  if(can_armed && int32_t(armed_until-now)<=0){','  owner.tick(now,demand,can_armed,'),
'@AUTO@':block('void manualCanStop() {','\n#endif\nbool usbCommand(')}
u=(root/'src/ui.cpp').read_text(); a='bool Ui::pollDemoToggle() {'
parts['@TOUCH@']=a+u.split(a,1)[1].split('\nvoid Ui::update(',1)[0]
f=(root/'tests/owner_auto_arm_fixture.cpp.in').read_text()
for k,v in parts.items():f=f.replace(k,v)
f=f.replace('static m5can::CanStats health{};','')
a=f.index('namespace m5can {'); b=f.index('\nCanOwner owner;',a)
f=f[:a]+(root/'tests/pr70/runtime.inc').read_text()+f[b:]
f=f[:f.index('int main(')]
f=f.replace('int pr70QueueReceive(QueueHandle_t,void*,TickType_t){return 0;}', '''bool completion_ready=false;
int pr70QueueReceive(QueueHandle_t q,void* out,TickType_t){
 if(q==reinterpret_cast<void*>(2) && completion_ready){
  completion_ready=false;*static_cast<m5can::DiagnosticResult*>(out)={};return pdTRUE;
 }
 return 0;
}''')
f=f.replace('Session session;Packet output;size_t dataoff=0;', 'Session session;Packet output,ack;Reassembler rx;bool waiting=false;size_t dataoff=0,ackoff=0;')
helper=block('// Sticky per-slot notification:', 'class Callbacks :') if '// Sticky per-slot notification:' in s else 'void serviceControlRevocations(uint32_t) {}'
f+=helper

f+='''
struct Incoming {uint16_t handle=7;uint32_t generation=1;};
static int inbox=42;
unsigned queued_controls=0;
// Saturated callback FIFO; BLE disconnect request is asynchronous, as NimBLE is.
int xQueueSend(int, Incoming*, int){if(queued_controls==12)return 0; ++queued_controls;return pdTRUE;}
unsigned disconnect_requests=0;bool disconnect_fails=false;
struct AsyncServer {int disconnect(uint16_t){++disconnect_requests;return disconnect_fails ? -1 : 0;}} server;
struct NimBLEDevice {static AsyncServer* getServer(){return &server;}};
void enqueue_overflow(Incoming in){
'''+block('        if (xQueueSend(inbox, &in, 0)', '\n        return;')+'''
}
int main(int argc,char** argv){
 assert(argc==2);const bool failed=std::string(argv[1])=="failed"; disconnect_fails=failed;
 m5can::CanMonitor m;CanOwnerTestAccess::setup(m);owner.attach(&m);
 owner.tick(0,{},false,1);
 connectSession(19,100);serviceAutoArm(100);control(4,2000);serviceAutoArm(2100);
 assert(can_armed);
 const uint32_t epoch=can_authorization_epoch.load();
 g_test_millis=2100;
 Simulator::Demand stale_demand{};peers[0].session.demand(2100,stale_demand);
 owner.tick(2100,stale_demand,true,epoch);
 assert(has_queue);
 peers[0].output.size=peers[0].ack.size=12;
 peers[0].dataoff=peers[0].ackoff=3;peers[0].waiting=true;
 const auto invalid_before=owner.samples().invalidations;
 // The NimBLE callback cannot wait even if the CAN task owns the TX mutex.
 auto gate=xSemaphoreCreateMutex();assert(xSemaphoreTake(gate,0)==pdTRUE);
 for(unsigned i=0;i<13;++i)enqueue_overflow(Incoming{});
 xSemaphoreGive(gate);
 assert(queued_controls==12);
 assert(disconnect_requests==1);
 assert(!can_armed && can_authorization_epoch.load()>epoch);
 assert(!links[0].active && !links[0].status && !links[0].data);
 assert(!CanOwnerTestAccess::acquire(m,epoch));
 assert(m.leaseRemainingMs()==0);
 // Stale owner snapshot, queued request and FC cannot borrow an old lease.
 assert(CanOwnerTestAccess::transmit(m,epoch,false)!=ESP_OK);
 assert(CanOwnerTestAccess::transmit(m,epoch,true)!=ESP_OK);
 assert(peers[0].session.active(2100));
 assert(owner.samples().invalidations==invalid_before);
 serviceControlRevocations(2100);
 assert(!peers[0].session.active(2100));
 assert(owner.samples().invalidations>invalid_before);
 assert(!peers[0].output.size && !peers[0].ack.size);
 assert(!peers[0].waiting && !peers[0].dataoff && !peers[0].ackoff);
 for(const auto& v:owner.samples().values)assert(v.quality==10 && !v.length);
 g_test_millis=2100;

 owner.tick(2100,stale_demand,true,epoch);
 if(has_queue)CanOwnerTestAccess::execute(m,queued);
 completion_ready=true;
 owner.tick(2100,{},false,can_authorization_epoch.load());
 assert(tx==0);
 // A delayed disconnect callback is optional; failure never invokes one.
 if(!failed)disconnected(7);
 serviceAutoArm(2101);assert(!can_armed);
 connectSession(20,2200);serviceAutoArm(2200);
 control(4,4199);serviceAutoArm(4199);assert(!can_armed);
 serviceAutoArm(4200);assert(can_armed && can_authorization_epoch.load()>epoch);
 assert(CanOwnerTestAccess::acquire(m,can_authorization_epoch.load()));
 std::cout<<"PASS: overflow disconnect_requests="<<disconnect_requests
 <<" session_invalidated=1 samples_invalid=1 fresh_session_settled=1"
 <<" post-overflow actual monitor driver TX="<<tx<<"\\n";
}
'''

class ControlOverflow(unittest.TestCase):
 def test_async_and_failed_disconnect(self):
  _,flags=flags_by_env();defs=[x for x in flags[OWNER] if x.startswith(('-D','-U'))]
  base=['c++','-std=c++17','-Wall','-Wextra','-Werror','-DM5CAN_TEST_S1_STOP_RACE=1','-DPR70_EPOCH_API=1','-Itests/pr70/stubs','-Itests/pr65/stubs','-Itests/host_stubs','-Iinclude','-Isrc',*defs]
  with tempfile.TemporaryDirectory() as d:
   out=pathlib.Path(d);cpp=out/'overflow.cpp';cpp.write_text(f)
   obj=out/'monitor.o';exe=out/'overflow'
   subprocess.run([*base,'-c','src/can_monitor.cpp','-o',str(obj)],cwd=root,check=True)
   subprocess.run([*base,str(cpp),str(obj),'src/diagnostic_policy.cpp','src/diagnostic_transaction.cpp','src/isotp_reassembler.cpp','src/signal_v2_session.cpp','src/signal_v2_codec.cpp','src/signal_v2_can_samples.cpp','src/signal_v2_discovery_profile.cpp','-o',str(exe)],cwd=root,check=True)
   for mode in ('delayed','failed'):
    with self.subTest(mode=mode):
     result=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=30)
     self.assertEqual(result.returncode,0,result.stdout+result.stderr)
     print(mode,result.stdout.strip())

if __name__=='__main__':unittest.main()
