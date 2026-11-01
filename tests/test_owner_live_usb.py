"""USB helper tests use only an in-memory serial fake; no device access."""
import io
import pathlib
import sys
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
from owner_live_arm import command


class UsbHelper(unittest.TestCase):
    def run_command(self, action, lines):
        class Serial:
            def __init__(self, **kwargs):
                self.lines=iter(lines)
                self.written=[]
                self.closed=False
                self.dtr=self.rts=True
            def open(self):
                assert not self.dtr and not self.rts
            def write(self, value): self.written.append(value)
            def reset_input_buffer(self): pass
            def flush(self): pass
            def readline(self): return next(self.lines,b'')
            def close(self): self.closed=True
        conn=Serial()
        out=io.StringIO()
        result=command('/fake/never-opened',action,
                       serial_factory=lambda **kw:conn, output=out, timeout=0.01)
        self.assertEqual(conn.written,[("SIGV2 CAN "+action.upper()+"\n").encode()])
        self.assertTrue(conn.closed)
        return result,out.getvalue()

    def test_arm_verified(self):
        result,out=self.run_command('arm',[
            b'#SIGNALV2_ADMIN_PRIVATE,redacted\n',
            b'#SIGNALV2_CAN,armed=1,reason=USB_VERIFIED,ttl_ms=600000\n'])
        self.assertEqual(result,0)
        self.assertNotIn('PRIVATE',out)

    def test_lab_arm_refused(self):
        result,_=self.run_command('arm',[
            b'#SIGNALV2_CAN,armed=0,reason=STAGE_AUTHORIZATION_REQUIRED\n'])
        self.assertEqual(result,1)

    def test_stop_waits_for_drained_ack(self):
        result,out=self.run_command('stop',[
            b'#SIGNALV2_CAN,armed=0,reason=OPERATOR_STOP,ack=REQUESTED\n',
            b'#SIGNALV2_CAN,ack=STOPPED,epoch=4\n'])
        self.assertEqual(result,0)
        self.assertIn('ack=STOPPED',out)

    def test_stale_stopped_ack_is_unconfirmed(self):
        result,_=self.run_command('stop',[
            b'#SIGNALV2_CAN,ack=STOPPED,epoch=1\n'])
        self.assertEqual(result,1)

    def test_requested_only_is_unconfirmed(self):
        result,_=self.run_command('stop',[
            b'#SIGNALV2_CAN,armed=0,reason=OPERATOR_STOP,ack=REQUESTED\n'])
        self.assertEqual(result,1)


if __name__=='__main__': unittest.main()
