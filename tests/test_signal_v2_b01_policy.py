#!/usr/bin/env python3
"""B-01 effective PlatformIO config and real CanOwner ARM/GROUP/Fresh."""
import configparser
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
HIL = "env:m5dial_signal_v2_can_29bit_serial_hil"

def flags_by_env():
    p=configparser.ConfigParser(interpolation=None)
    p.read(ROOT/"platformio.ini")
    cache={}
    def resolve(env,ancestors=()):
        if env in ancestors:raise AssertionError("cyclic flag reference: "+env)
        if env in cache:return cache[env]
        tokens=[]
        for line in p.get(env,"build_flags",fallback="").splitlines():
            line=line.split(";",1)[0].strip()
            for token in line.split():
                if token.startswith(chr(36)+"{env:") and token.endswith(".build_flags}"):
                    ref=token[2:-len(".build_flags}")]
                    tokens+=resolve(ref,(*ancestors,env))
                else:
                    tokens.append(token)
        cache[env]=tokens
        return tokens
    return p,{env:resolve(env) for env in p.sections() if env.startswith("env:")}

def active(flags,name):
    return any(re.fullmatch("-D"+re.escape(name)+r"(?:=.+)?",x) for x in flags)

def stage_one(flags):
    return (flags.count("-DM5CAN_SIGNAL_V2_STAGE_GATED=1")==1 and
            not any(x.startswith("-U" + "M5CAN_SIGNAL_V2_STAGE_GATED") for x in flags))

class SerialHilB01(unittest.TestCase):
    def test_b01_serial_hil_is_isolated_unstaged(self):
        config,flags=flags_by_env()
        current=flags[HIL]
        for macro in ("M5CAN_SIGNAL_V2_SERIAL_HIL","M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL",
                      "M5CAN_SIGNAL_V2_RP8_PROFILE","M5CAN_ECU_SELECTION_EXPERIMENTAL"):
            self.assertTrue(active(current,macro),(macro,current))
        self.assertFalse(active(current,"M5CAN_SIGNAL_V2_STAGE_GATED"),
                         "B-01 generic ARM cannot pass S1-only GATT")
        self.assertFalse(active(current,"M5CAN_CAN_TX_HARD_DISABLED"))
        self.assertNotIn("m5dial_signal_v2_can_29bit_5hz_lab",
                         config.get(HIL,"extends",fallback=""))
        workflow=(ROOT/".github/workflows/signal-v2-lab.yml").read_text()
        self.assertIn("pio run -e m5dial_signal_v2_can_29bit_serial_hil",workflow)
        self.assertRegex(workflow,
            r"check_signal_v2_elf\.py[^\n]*m5dial_signal_v2_can_29bit_serial_hil/firmware\.elf[^\n]*--hil[^\n]*--nm")
        self.assertIn("name.startswith('twai_')",
                      (ROOT/"tools/check_signal_v2_elf.py").read_text())

    def test_b01_vehicle_capable_v2_envs_are_all_stage_gated(self):
        _,flags=flags_by_env()
        exceptions=[]
        for env,defs in flags.items():
            v2=active(defs,"M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL")
            hil=active(defs,"M5CAN_SIGNAL_V2_SERIAL_HIL")
            disabled=active(defs,"M5CAN_CAN_TX_HARD_DISABLED")
            if hil:
                exceptions.append(env)
                self.assertEqual(env,HIL,"serial HIL flag must not leak")
                self.assertTrue(v2)
            elif v2 and not disabled:
                self.assertTrue(stage_one(defs),
                                (env,"vehicle-capable V2 must define STAGE_GATED=1"))
        self.assertEqual(exceptions,[HIL])
        for env in ("env:m5dial_signal_v2_can_29bit_5hz_lab",
                    "env:m5dial_signal_v2_can_29bit_10hz_lab"):
            self.assertTrue(stage_one(flags[env]))

    def test_b01_production_gatt_arm_branch_accepts_secured_usb(self):
        _, flags = flags_by_env()
        defs = [f for f in flags[HIL] if f.startswith("-D") or f.startswith("-U")]
        source = (ROOT / "src/signal_v2_gatt.cpp").read_text()
        begin = '  if(!std::strcmp(cmd,"SIGV2 CAN ARM")) {'
        end = '  // PR-C2 stage S1'
        self.assertEqual(source.count(begin), 1, "production GATT ARM branch changed")
        part = begin + source.split(begin, 1)[1].split(end, 1)[0]
        self.assertIn("can_armed=secured;", part)
        self.assertIn("M5CAN_SIGNAL_V2_STAGE_GATED", part)
        # Compile the unchanged production GATT ARM branch, not a hand-written
        # approximation. Only BLE secure-link/Serial dependencies are stubbed.
        harness = r"""
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cassert>
struct Link { bool active=true, status=true, data=true; };
Link snapshot(unsigned) { return {}; }
bool authorized(const Link&) { return true; }
struct Session { bool active(uint32_t)const{return true;} };
struct Peer { Session session; };
Peer peers[2];
std::atomic<bool> can_armed{false};
std::atomic<uint32_t> can_authorization_epoch{2};
uint32_t armed_until=0;
struct SerialOut {
  char text[512]{};
  void println(const char* s){std::snprintf(text,sizeof(text),"%s",s);}
  int printf(const char* f,...) {
    va_list a;va_start(a,f);int n=std::vsnprintf(text,sizeof(text),f,a);
    va_end(a);return n;
  }
} Serial;
namespace hil {void note(const char*,...){}}
void manage(const char* cmd,uint32_t now){
""" + part + """
}
int main(){
  manage("SIGV2 CAN ARM",100);
  assert(can_armed.load());
  assert(can_authorization_epoch.load()==3);
  assert(armed_until==600100);
  assert(std::strstr(Serial.text,"reason=USB_VERIFIED"));
}
"""
        with tempfile.TemporaryDirectory() as d:
            source_file = pathlib.Path(d) / "gatt-arm.cpp"
            exe = pathlib.Path(d) / "gatt-arm"
            source_file.write_text(harness)
            p = subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra",
                                "-Werror", *defs, str(source_file), "-o",
                                str(exe)], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, (p.stdout+p.stderr)[-4000:])
            p = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, (p.stdout+p.stderr)[-4000:])

    def test_b01_compiled_real_owner_arm_group_fresh(self):
        _,flags=flags_by_env()
        definitions=[f for f in flags[HIL] if f.startswith("-D") or f.startswith("-U")]
        with tempfile.TemporaryDirectory() as d:
            exe=pathlib.Path(d)/"b01-arm-group"
            cmd=["c++","-std=c++17","-Wall","-Wextra","-Werror",
                 *definitions,"-Itests/hil/stubs","-Itests/host_stubs",
                 "-Iinclude","-Isrc","tests/hil/arm_group_fresh.cpp",
                 "src/diagnostic_policy.cpp","src/diagnostic_transaction.cpp",
                 "src/isotp_reassembler.cpp","-o",str(exe)]
            p=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(p.returncode,0,(p.stdout+p.stderr)[-4000:])
            p=subprocess.run([str(exe)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(p.returncode,0,(p.stdout+p.stderr)[-4000:])
            self.assertIn("PASS B-01 serial-HIL ARM -> GROUP -> Fresh",p.stdout)

if __name__=="__main__":unittest.main()
