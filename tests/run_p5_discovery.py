#!/usr/bin/env python3
"""Host-driven serial-HIL, real owner/monitor and real serial TWAI shim."""
import pathlib, shutil, subprocess, tempfile, os
ROOT=pathlib.Path(__file__).resolve().parents[1]
FLAGS=['-DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1','-DM5CAN_SIGNAL_V2_STAGE_GATED=1','-DM5CAN_SIGNAL_V2_SERIAL_HIL=1','-DM5CAN_SIGNAL_V2_11BIT_DISCOVERY_LAB=1','-DM5CAN_HIL_HOST_TEST=1']
SOURCES=['tests/p5_discovery.cpp','src/can_monitor.cpp','src/twai_hil.cpp','src/diagnostic_policy.cpp','src/diagnostic_transaction.cpp','src/isotp_reassembler.cpp']
def build(root,work,fixture="tests/p5_discovery.cpp"):
    work.mkdir(parents=True,exist_ok=True)
    stub=work/'stubs';shutil.copytree(root/'tests/hil/stubs',stub)
    p=stub/'Arduino.h';s=p.read_text().replace('inline void vTaskDelay(uint32_t ms){g_test_millis+=ms;}','void p5TimeHook();\ninline void vTaskDelay(uint32_t ms){g_test_millis+=ms;p5TimeHook();}');p.write_text(s)
    exe=work/'p5'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror','-pthread',*FLAGS,'-I'+str(stub),'-I'+str(root/'tests/host_stubs'),'-I'+str(root/'include'),'-I'+str(root/'src'),*[str(root/(fixture if s==SOURCES[0] else s)) for s in SOURCES],'-o',str(exe)],check=True)
    return exe
if __name__=='__main__':
    with tempfile.TemporaryDirectory() as tmp:
        exe=build(ROOT,pathlib.Path(tmp))
        for case in ['limits','double-entry','reentry','single','eight','ninth','duplicate','ff','cf','rxfc','range','29bit','late','timeout','txfail','epoch','profile','overrun','fc','ownerbudget','lowerbudget','deadline','delayed','buserr','drop','busoff','result-race']:
            subprocess.run([str(exe),case],check=True,timeout=15)

        serial=build(ROOT,pathlib.Path(tmp)/"serial", "tests/p5_serial.cpp")
        subprocess.run([str(serial)],check=True,timeout=15)
