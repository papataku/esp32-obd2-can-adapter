#!/usr/bin/env python3
"""Fixed P3-main scope and preprocessor isolation; no devices, no TX."""
import os, pathlib, re, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
BASE='0424017cb6bdb1446609d7f1318d98fd2a9ef125'
LAB='env:m5dial_signal_v2_can_11bit_discovery_lab'
SOURCES={'src/can_monitor.cpp','src/can_monitor.h','src/signal_v2_can_owner.h','tests/test_signal_v2_b01_policy.py'}
def strip_p5(text):
    return re.sub(r'(?:[ ]*#|//) P5_BEGIN\n.*?(?:[ ]*#|//) P5_END\n','',text,flags=re.S)
def original(path):
    return subprocess.check_output(['git','show',BASE+':'+path],cwd=ROOT)
def check_tree(root=ROOT):
    if subprocess.run(['git','cat-file','-e',BASE+'^{commit}'],cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode:
        subprocess.run(['git','fetch','--no-tags','origin',BASE],cwd=ROOT,check=True)
    old=subprocess.check_output(['git','ls-tree','-r','--name-only',BASE],cwd=ROOT,text=True).splitlines()
    for path in old:
        value=(root/path).read_bytes()
        if path in SOURCES:value=strip_p5(value.decode()).encode()
        elif path=='platformio.ini':value=value.split(b'\n; P5 host/serial-HIL ONLY.')[0]
        elif path=='tests/run_all.sh':value=value.split(b'\n# P5 isolated')[0]
        elif path=='tools/test_p3_scope.py':continue # P3 still checks its own full legacy contract below.
        assert value==original(path),path
    import configparser
    cfg=configparser.ConfigParser(interpolation=None);cfg.read(root/'platformio.ini')
    for section in cfg.sections():
        if section!=LAB:
            assert '11BIT_DISCOVERY' not in str(dict(cfg[section])),section
    assert 'M5CAN_SIGNAL_V2_11BIT_DISCOVERY_LAB=1' in cfg[LAB]['build_flags']
    assert '+<discovery11_lab.cpp>' in cfg[LAB]['build_src_filter']
    print('PASS P5 fixed-base scope: existing production files/config restored exactly after removing guarded additions')

def preprocess(root=ROOT):
    import configparser
    cfg=configparser.ConfigParser(interpolation=None);cfg.read(root/'platformio.ini')
    import sys
    sys.path.insert(0,str(root/'tests'))
    from test_signal_v2_b01_policy import flags_by_env
    _,effective=flags_by_env(root/'platformio.ini')
    cxx=os.environ.get('CXX','c++')
    with tempfile.TemporaryDirectory() as d:
        base=pathlib.Path(d)
        for path in SOURCES:
            dest=base/path;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(original(path))
        common=[cxx,'-std=c++17','-E','-P','-x','c++','-I'+str(root/'tests/hil/stubs'),'-I'+str(root/'tests/host_stubs'),'-I'+str(root/'include'),'-I'+str(root/'src')]
        for section in cfg.sections():
            if not section.startswith('env:'):continue
            flags=[x for x in effective[section] if x.startswith(('-D','-U'))]
            for path in ('src/can_monitor.cpp','src/signal_v2_can_owner.h','src/discovery11_lab.cpp'):
                new=subprocess.check_output([*common,*flags,str(root/path)],stderr=subprocess.DEVNULL)
                if section!=LAB:
                    assert not re.search(rb'Discovery11|discovery11_|P5_',new),(section,path)
                    if path in SOURCES:
                        old=subprocess.check_output([*common,*flags,str(base/path)],stderr=subprocess.DEVNULL)
                        # __FILE__ in assertions can differ by checkout location.
                        old=old.replace(str(base).encode(),str(root).encode())
                        assert new.split()==old.split(),(section,path,'preprocessor token drift')
            print('PASS preprocessor',section, 'discovery='+('lab-only' if section==LAB else '0'))
        flags=['-DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1','-DM5CAN_SIGNAL_V2_RP8_PROFILE=1','-DM5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE=1','-DM5CAN_SIGNAL_V2_OWNER_AUTO_ARM=1','-DM5CAN_SIGNAL_V2_11BIT_DISCOVERY_LAB=1']
        result=subprocess.run([*common,*flags,str(root/'src/can_monitor.cpp')],capture_output=True)
        assert result.returncode and b'P5 discovery requires' in result.stderr
        print('PASS owner_live + discovery compile rejection')
if __name__=='__main__':check_tree();preprocess()
