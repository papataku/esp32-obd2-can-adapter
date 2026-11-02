#!/usr/bin/env python3
"""Kill four safety mutations in disposable copies, never touch the checkout."""
import pathlib, shutil, subprocess, sys, tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from run_p5_discovery import build
sys.path.insert(0,str(ROOT/'tools'))
from test_p5_scope import check_tree

def replace(root,path,a,b):
    p=root/path;s=p.read_text();assert a in s,(path,a);p.write_text(s.replace(a,b))
with tempfile.TemporaryDirectory() as d:
    temp=pathlib.Path(d)
    for mutation in ('ninth','fc','one-query','isolation'):
        root=temp/mutation
        shutil.copytree(ROOT,root,ignore=shutil.ignore_patterns('.git','.pio','__pycache__'))
        if mutation=='ninth':
            replace(root,'include/signal_v2_11bit_discovery.h','count>=ResponderLimit','false')
            case='limits'
        elif mutation=='fc':
            # Remove both exact-SF gates; the test tries a real FC submission
            # through the same transmitOwnedFrame path before the discovery TX.
            replace(root,'src/can_monitor.cpp','stage_s1_consumed_ || message.extd || message.rtr || message.self || !message.ss ||','stage_s1_consumed_ || false ||')
            replace(root,'src/can_monitor.cpp','message.identifier!=0x7DF || message.data_length_code!=8 ||\n     std::memcmp(message.data,signal_v2::Discovery11::Request,8)!=0','false')
            replace(root,'src/can_monitor.cpp','if(!valid_discovery11) {xSemaphoreGive(tx_gate_);return ESP_FAIL;}','(void)valid_discovery11;')
            case='fc'
        elif mutation=='one-query':
            replace(root,'src/can_monitor.cpp','stage_s1_consumed_ ||','false ||')
            replace(root,'src/can_monitor.cpp','|| stage_s1_consumed_', '|| false')
            replace(root,'src/can_monitor.cpp','&& !stage_s1_consumed_', '&& true')
            case='reentry'
        else:
            p=root/'platformio.ini';s=p.read_text();start=s.index('[env:m5dial_signal_v2_can_29bit_5hz_owner_live]');at=s.index('build_flags =',start)+len('build_flags =');s=s[:at]+'\n    -DM5CAN_SIGNAL_V2_11BIT_DISCOVERY_LAB=1'+s[at:];p.write_text(s)
            try:check_tree(root)
            except AssertionError as e:print('PASS killed isolation:',e);continue
            raise AssertionError('isolation mutation survived')
        exe=build(root,temp/(mutation+'-build'))
        result=subprocess.run([str(exe),case],text=True,capture_output=True,timeout=15)
        assert result.returncode!=0,mutation+' survived'
        print('PASS killed',mutation,'exit='+str(result.returncode),result.stderr.strip())
print('PASS temporary mutation trees removed')
