"""Host-only production GATT auto-ARM lifecycle; no device access."""
import pathlib
import subprocess
import tempfile
import unittest
from test_signal_v2_b01_policy import ROOT, OWNER, flags_by_env

class AutoArm(unittest.TestCase):
    def test_production_lifecycle(self):
        source=(ROOT/'src/signal_v2_gatt.cpp').read_text()
        def block(start,end):
            return start+source.split(start,1)[1].split(end,1)[0]
        parts={
            '@GRANT@':block('bool grantCanArm(uint32_t now,uint32_t seen) {','\n#endif\nvoid manage('),
            '@STOP@':block('  if(!std::strcmp(cmd,"SIGV2 CAN STOP")) {','\n#if defined(M5CAN_SIGNAL_V2_SERIAL_HIL)\n  if(!std::strcmp(cmd,"SIGV2 CAN READY"))'),
            '@ARM@':block('  if(!std::strcmp(cmd,"SIGV2 CAN ARM")) {','  // PR-C2 stage S1'),
            '@DISCONNECT@':block('void disconnected(uint16_t handle) {','void gap('),
            '@TTL@':block('  if(can_armed && int32_t(armed_until-now)<=0){','  owner.tick(now,demand,can_armed,'),
            '@AUTO@':block('void manualCanStop() {','\n#endif\nbool usbCommand('),
        }
        ui=(ROOT/'src/ui.cpp').read_text()
        begin='bool Ui::pollDemoToggle() {'
        parts['@TOUCH@']=begin+ui.split(begin,1)[1].split('\nvoid Ui::update(',1)[0]
        fixture=(ROOT/'tests/owner_auto_arm_fixture.cpp.in').read_text()
        for k,v in parts.items():fixture=fixture.replace(k,v)
        _,flags=flags_by_env()
        with tempfile.TemporaryDirectory() as d:
            cpp=pathlib.Path(d)/'auto.cpp';cpp.write_text(fixture)
            exe=pathlib.Path(d)/'auto'
            result=subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',
                *[f for f in flags[OWNER] if f.startswith(('-D','-U'))],
                '-Itests/host_stubs','-Iinclude','-Isrc',str(cpp),
                'src/diagnostic_policy.cpp','src/diagnostic_transaction.cpp',
                'src/isotp_reassembler.cpp','src/signal_v2_session.cpp',
                'src/signal_v2_codec.cpp','src/signal_v2_can_samples.cpp',
                '-o',str(exe)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            for scenario in ('lifecycle','touch','ttl','fault'):
                with self.subTest(scenario=scenario):
                    result=subprocess.run([str(exe),scenario],capture_output=True,text=True)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    print(result.stdout.strip())

if __name__=='__main__':unittest.main()
