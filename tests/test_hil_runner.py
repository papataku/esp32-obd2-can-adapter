"""Exercise real run() phases with a serial/time fixture; never opens a device."""
import pathlib, sys, tempfile, types, unittest
from unittest.mock import patch
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
import signal_v2_hil_ecu as h

class Clock:
    now=0.
    def monotonic(self):return self.now
    def sleep(self,seconds):self.now+=seconds

class Port:
    def __init__(self,clock,short=False):
        self.clock=clock;self.commands=[];self.pending=[];self.seq=0;self.epoch=1;self.short=short;self.closed=False
    def reset_input_buffer(self):pass
    def close(self):self.closed=True
    def event(self,delay,kind,**fields):
        self.pending.append((self.clock.now+delay,dict(kind=kind,**fields)))
    def write(self,wire):
        if self.short:return len(wire)-1
        line=wire.decode().strip();self.commands.append(line)
        if line=='SIGV2 CAN STOP':
            self.epoch+=1
            self.event(.001,'stop_request',epoch=self.epoch)
            self.event(.002,'stopped',epoch=self.epoch)
        elif line.startswith('HIL FENCE '):self.event(.001,'fence',nonce=line.split()[-1])
        elif line=='SIGV2 CAN ARM':
            self.epoch+=1;self.event(.001,'arm',ok=1,epoch=self.epoch)
            for gate in ('owner','monitor'):
                self.event(.005,'gate',gate=gate,cost=800,used100=800,used1000=800,discovery=1)
            self.event(.006,'tx',id='18DA01F1',ext=1,dlc=8,data=h.PROBE.ljust(8,b'\0').hex().upper(),ok=1)
            self.event(.14,'fault',epoch=self.epoch,reason='NO_VERIFIED_ECU')
            self.event(.15,'stopped',epoch=self.epoch+1)
            self.event(.16,'raw',text='#SIGNALV2_LOAD,acquired=0,denied=0')
        return len(wire)
    def read(self,n):
        self.clock.now+=.005
        out=[]
        for at,e in sorted(self.pending,key=lambda x:x[0]):
            if at>self.clock.now:continue
            self.pending.remove((at,e))
            if e['kind']=='raw':out.append(e['text']);continue
            self.seq+=1
            fields=dict(seq=self.seq,t_us=round(at*1e6),gen=1,lost=0,boot='12345678',**e)
            out.append('#HIL_EVENT,'+','.join('%s=%s'%pair for pair in fields.items()))
        return ('\n'.join(out)+ ('\n' if out else '')).encode()

class Runner(unittest.TestCase):
    def test_m3_stop_ack_quiet_and_trace_retained(self):
        clock=Clock();port=Port(clock)
        with tempfile.TemporaryDirectory() as d, patch.object(h,'time',clock),patch.dict(sys.modules,serial=types.SimpleNamespace(Serial=lambda *a,**k:port,SerialException=OSError)):
            path=pathlib.Path(d)/'trace.jsonl'
            result,events=h.run('fixture','noreply',60,str(path))
            self.assertTrue(result['pass'],result)
            self.assertTrue(port.closed)
            self.assertIn('#SIGNALV2_LOAD',path.read_text())
            self.assertEqual(port.commands[0],'SIGV2 CAN STOP')
            self.assertTrue(port.commands[1].startswith('HIL FENCE '))
            self.assertEqual(port.commands[2],'SIGV2 CAN ARM')
            self.assertEqual(port.commands[-2],'SIGV2 CAN STOP')
            self.assertTrue(port.commands[-1].endswith('end'))
            self.assertGreaterEqual(clock.now,61)
    def test_m3_reboot_waits_for_secured_central(self):
        source=pathlib.Path(h.__file__).read_text()
        # Reconnect readiness is queried without attempting ARM. A rejected ARM
        # remains a failure; reconnect cannot be hidden by retrying ARM.
        phase=source.split('if scenario == "reboot" and reboot_seen:')[1].split('continue')[0]
        ready=phase.split('else:')[0]
        self.assertIn('phase="reboot_ready"',ready)
        self.assertIn('send("SIGV2 CAN READY")',ready)
        self.assertNotIn('send("SIGV2 CAN ARM")',ready)
        self.assertIn('phase == "reboot_ready" and k == "ready" and e.get("ok") == "1"',source)
    def test_m4_short_write_rejected(self):
        clock=Clock();port=Port(clock,short=True)
        with tempfile.TemporaryDirectory() as d,patch.object(h,'time',clock),patch.dict(sys.modules,serial=types.SimpleNamespace(Serial=lambda *a,**k:port,SerialException=OSError)):
            with self.assertRaisesRegex(RuntimeError,'short serial write'):
                h.run('fixture','noreply',60,str(pathlib.Path(d)/'trace.jsonl'))
            self.assertTrue(port.closed)
            self.assertTrue((pathlib.Path(d)/'trace.jsonl').exists())

if __name__=='__main__':unittest.main()
