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
OWNER = "env:m5dial_signal_v2_can_29bit_5hz_owner_live"
OWNER_MACRO = "M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE"

def flags_by_env(path=None):
    p=configparser.ConfigParser(interpolation=None)
    p.read(path or ROOT/"platformio.ini")
    cache={}
    def resolve(env,ancestors=()):
        if env in ancestors:raise AssertionError("cyclic flag reference: "+env)
        if env in cache:return cache[env]
        tokens=[]
        raw=p.get(env,"build_flags",fallback=None)
        if raw is None:
            # A new env may inherit the owner macro without its own flags.
            for parent in p.get(env,"extends",fallback="").split(","):
                if parent.strip():tokens+=resolve(parent.strip(),(*ancestors,env))
        for line in (raw or "").splitlines():
            line=line.split(";",1)[0].strip()
            for token in line.split():
                if token.startswith(chr(36)+"{env:") and token.endswith(".build_flags}"):
                    ref=token[2:-len(".build_flags}")]
                    tokens+=resolve(ref,(*ancestors,env))
                else:
                    tokens.append(token)
        parent=env
        while not p.has_option(parent,"build_unflags"):
            parent=p.get(parent,"extends",fallback="").strip()
            if not parent:break
        unflags=p.get(parent,"build_unflags",fallback="").split() if parent else []
        tokens=[t for t in tokens if t not in unflags]
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

    def assert_vehicle_policy(self,flags):
        exceptions=[]
        for env,defs in flags.items():
            v2=active(defs,"M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL")
            hil=active(defs,"M5CAN_SIGNAL_V2_SERIAL_HIL")
            owner=active(defs,OWNER_MACRO)
            disabled=active(defs,"M5CAN_CAN_TX_HARD_DISABLED")
            if owner:
                self.assertEqual(env,OWNER,"owner macro must not leak")
                self.assertEqual(defs.count("-D"+OWNER_MACRO+"=1"),1)
                self.assertTrue(active(defs,"M5CAN_SIGNAL_V2_RP8_PROFILE"))
                self.assertFalse(hil or disabled)
                self.assertFalse(active(defs,"M5CAN_SIGNAL_V2_STAGE_GATED"))
            if hil or owner:
                exceptions.append(env)
                self.assertEqual(env,HIL if hil else OWNER)
                self.assertTrue(v2)
            elif v2 and not disabled:
                self.assertTrue(stage_one(defs),(env,"requires STAGE_GATED=1"))
        self.assertCountEqual(exceptions,[HIL,OWNER])

    def test_b01_vehicle_capable_v2_envs_have_exact_exceptions(self):
        config,flags=flags_by_env()
        self.assert_vehicle_policy(flags)
        # All other effective 5Hz definitions stay the same.
        self.assertEqual([f for f in flags[OWNER] if OWNER_MACRO not in f],
                         [f for f in flags["env:m5dial_signal_v2_can_29bit_5hz_lab"]
                          if "M5CAN_SIGNAL_V2_STAGE_GATED" not in f])
        self.assertEqual(config.get(OWNER,"extends"),
                         "env:m5dial_signal_v2_can_29bit_5hz_lab")
        self.assertIn("pio run -e "+OWNER[4:],
                      (ROOT/".github/workflows/signal-v2-lab.yml").read_text())
        for env in ("env:m5dial_signal_v2_can_29bit_5hz_lab",
                    "env:m5dial_signal_v2_can_29bit_10hz_lab"):
            self.assertTrue(stage_one(flags[env]))

    def test_b01_other_unstaged_env_rejected_by_name_and_macro(self):
        _,flags=flags_by_env()
        rogue="env:m5dial_signal_v2_can_29bit_other_unstaged"
        for has_macro in (False,True):
            mutated=dict(flags)
            mutated[rogue]=[f for f in flags[OWNER]
                            if has_macro or OWNER_MACRO not in f]
            with self.assertRaisesRegex(AssertionError,
                                        "owner macro must not leak" if has_macro
                                        else "requires STAGE_GATED=1"):
                self.assert_vehicle_policy(mutated)

    def test_b01_inherited_owner_macro_cannot_leak_to_another_env(self):
        with tempfile.TemporaryDirectory() as d:
            ini=pathlib.Path(d)/"platformio.ini"
            ini.write_text((ROOT/"platformio.ini").read_text()+
                "\n[env:other_unstaged_vehicle]\nextends = "+OWNER+"\n")
            _,flags=flags_by_env(ini)
            with self.assertRaisesRegex(AssertionError,"owner macro must not leak"):
                self.assert_vehicle_policy(flags)

    def test_b01_compile_guard_rejects_other_unstaged_vehicle(self):
        # Compile the actual production guard, without BLE SDK dependencies.
        source=(ROOT/"src/signal_v2_gatt.cpp").read_text()
        guard=source[source.index("#if !defined(M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL)"):
                     source.index("namespace signal_v2 {")]
        _,flags=flags_by_env()
        base=[f for f in flags[OWNER] if f.startswith(("-D","-U"))]
        cases=[("owner",base,True),
               ("other_unstaged_vehicle",[f for f in base if OWNER_MACRO not in f],False),
               ("owner_zero",[f for f in base if OWNER_MACRO not in f]+["-D"+OWNER_MACRO+"=0"],False),
               ("owner_no_rp8",[f for f in base if "M5CAN_SIGNAL_V2_RP8_PROFILE" not in f],False),
               ("owner_10hz",base+["-DM5CAN_SIGNAL_V2_10HZ_LAB=1"],False),
               ("owner_hil",base+["-DM5CAN_SIGNAL_V2_SERIAL_HIL=1"],False),
               ("owner_staged",base+["-DM5CAN_SIGNAL_V2_STAGE_GATED=1"],False)]
        with tempfile.TemporaryDirectory() as d:
            src=pathlib.Path(d)/"guard.cpp"
            src.write_text(guard+"int main(){}\n")
            for name,defs,ok in cases:
                p=subprocess.run(["c++","-std=c++17",*defs,"-fsyntax-only",str(src)],
                                 capture_output=True,text=True)
                self.assertEqual(p.returncode==0,ok,(name,p.stderr))
                if not ok:self.assertIn("#error",p.stderr)
                print("PASS production guard",name,"accepted" if ok else "rejected")

    def test_b01_production_gatt_arm_branch_accepts_secured_usb(self):
        _, flags = flags_by_env()
        source = (ROOT / "src/signal_v2_gatt.cpp").read_text()
        begin = '  if(!std::strcmp(cmd,"SIGV2 CAN ARM")) {'
        end = '  // PR-C2 stage S1'
        self.assertEqual(source.count(begin), 1, "production GATT ARM branch changed")
        part = begin + source.split(begin, 1)[1].split(end, 1)[0]
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
Link link;
bool is_authorized=true, session_active=true;
Link snapshot(unsigned) { return link; }
bool authorized(const Link&) { return is_authorized; }
struct Session { bool active(uint32_t)const{return session_active;} };
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
#if defined(M5CAN_SIGNAL_V2_STAGE_GATED)
  assert(!can_armed.load());
  assert(can_authorization_epoch.load()==2);
  assert(std::strstr(Serial.text,"reason=STAGE_AUTHORIZATION_REQUIRED"));
#else
  assert(can_armed.load());
  assert(can_authorization_epoch.load()==3);
  assert(armed_until==600100);
  assert(std::strstr(Serial.text,"reason=USB_VERIFIED"));
  for(unsigned missing=0;missing<5;++missing){
    link={};is_authorized=session_active=true;can_armed=false;
    if(missing==0)link.active=false;
    if(missing==1)link.status=false;
    if(missing==2)link.data=false;
    if(missing==3)is_authorized=false;
    if(missing==4)session_active=false;
    manage("SIGV2 CAN ARM",200);
    assert(!can_armed.load());
    assert(std::strstr(Serial.text,"reason=NOT_READY"));
  }
#endif
}
"""
        with tempfile.TemporaryDirectory() as d:
            source_file = pathlib.Path(d) / "gatt-arm.cpp"
            source_file.write_text(harness)
            # Same actual GATT ARM branch accepts HIL and explicit owner live;
            # the physical 5Hz profile MUST still reject a generic ARM.
            for env in (HIL, OWNER, "env:m5dial_signal_v2_can_29bit_5hz_lab"):
                effective = [x for x in flags[env] if x.startswith("-D") or x.startswith("-U")]
                exe = pathlib.Path(d) / ("gatt-arm-hil" if env == HIL else "gatt-arm-vehicle")
                p = subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra",
                                    "-Werror", *effective, str(source_file), "-o",
                                    str(exe)], cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(p.returncode, 0, (env, p.stdout+p.stderr)[-4000:])
                p = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertEqual(p.returncode, 0, (env, p.stdout+p.stderr)[-4000:])
                print("PASS GATT ARM",env,"STAGE_AUTHORIZATION_REQUIRED" if stage_one(flags[env]) else "USB_VERIFIED ttl=600000")

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

    def test_owner_live_compiled_continuous_group_fresh(self):
        _,flags=flags_by_env()
        defs=[f for f in flags[OWNER] if f.startswith(("-D","-U"))]
        with tempfile.TemporaryDirectory() as d:
            exe=pathlib.Path(d)/"owner-live"
            p=subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror",*defs,
                "-Itests/hil/stubs","-Itests/host_stubs","-Iinclude","-Isrc",
                "tests/test_signal_v2_owner_live.cpp","src/diagnostic_policy.cpp",
                "src/diagnostic_transaction.cpp","src/isotp_reassembler.cpp","-o",str(exe)],
                cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            p=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            self.assertIn("PASS owner-live ARM -> GROUP -> Fresh",p.stdout)
            print(p.stdout.strip())

if __name__=="__main__":unittest.main()
