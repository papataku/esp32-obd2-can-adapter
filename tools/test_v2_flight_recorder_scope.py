#!/usr/bin/env python3
"""V2 flight recorder exact scope (PM Claude decision 2026-11-15, narrow allowlist).

Every existing file changed by the recorder must be byte-identical to the fixed
base c3843a6 after removing ONLY the exact (new -> old) pairs pinned in PAIRS.
Added lines and the new recorder sources are additionally constrained by
content: no TWAI/TX/lease/admission/authorization API, no flash write/erase
outside EvidenceStore::put via Consumer::writePage, no peer identity.
Other slice checkers (P3, P5, PID S1) call strip_v2rec() before their own
fixed-base comparisons; nothing else of theirs is relaxed.
"""
import pathlib, re, subprocess, sys
ROOT = pathlib.Path(__file__).resolve().parents[1]
BASE_REF = 'c3843a6'


class _New(frozenset):
    def __contains__(self, x):
        return frozenset.__contains__(self, x) or str(x).startswith('docs/evidence/v2rec/')


NEW = _New({
    'include/signal_v2_flight_recorder.h', 'include/signal_v2_flight_recorder_tap.h',
    'src/signal_v2_flight_recorder.cpp', 'tests/v2_flight_recorder.cpp',
    'tools/decode_v2_flight_recorder.py', 'tools/test_v2_flight_recorder.py',
    'tools/test_v2_flight_recorder_scope.py', 'docs/v2_flight_recorder.md',
})
RECORDER_SOURCES = ['include/signal_v2_flight_recorder.h', 'include/signal_v2_flight_recorder_tap.h',
                    'src/signal_v2_flight_recorder.cpp']

PAIRS = {'platformio.ini': [('    -DM5CAN_SIGNAL_V2_OWNER_AUTO_ARM=1\n'
                     '    -DCONFIG_BT_NIMBLE_MAX_BONDS=4\n'
                     '    ; Read-only Signal V2 flight recorder (PM Claude 2026-11-15). No CAN TX change.\n'
                     '    -DM5CAN_SIGNAL_V2_FLIGHT_RECORDER=1\n',
                     '    -DM5CAN_SIGNAL_V2_OWNER_AUTO_ARM=1\n    -DCONFIG_BT_NIMBLE_MAX_BONDS=4\n')],
 'src/ecu_physical_selector.h': [('  bool fixed() const { return state_==State::Fixed; }\n'
                                  '  uint8_t consecutiveBad() const { return consecutive_bad_; } // '
                                  'read-only (V2 recorder)\n',
                                  '  bool fixed() const { return state_==State::Fixed; }\n')],
 'src/evidence_store.cpp': [('  if(type==Ble && slot<4)return 16+slot;\n'
                             '#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)\n'
                             '  if(type==V2Rec && slot<kV2RecSlots)return static_cast<int>(20+slot);\n'
                             '#endif\n',
                             '  if(type==Ble && slot<4)return 16+slot;\n'),
                            ('  return false;\n'
                             '}\n'
                             '#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)\n'
                             'bool EvidenceStore::reusableByV2Rec(uint32_t offset) {\n'
                             '  Header h{};\n'
                             '  if(!flash_->read(offset,&h,sizeof(h)))return false;\n'
                             '  return h.magic==UINT32_MAX || (h.magic==kRecord && h.type==V2Rec);\n'
                             '}\n'
                             '#endif\n',
                             '  return false;\n}\n'),
                            ("    // Preserve every key's latest valid record until the NEW record commits.\n"
                             '    if(!protectedSector(candidate)\n'
                             '#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)\n'
                             '       && (type!=V2Rec || reusableByV2Rec(candidate)) // PM 2026-11-15: erase '
                             'only own/blank sectors\n'
                             '#endif\n'
                             '       ){offset=candidate;break;}\n',
                             "    // Preserve every key's latest valid record until the NEW record commits.\n"
                             '    if(!protectedSector(candidate)){offset=candidate;break;}\n')],
 'src/evidence_store.h': [(' public:\n'
                           '#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)\n'
                           '  static constexpr uint32_t kV2RecSlots=48; // Signal V2 flight recorder pages\n'
                           '#else\n'
                           '  static constexpr uint32_t kV2RecSlots=0;\n'
                           '#endif\n'
                           '  static constexpr uint32_t kSector=4096,kMaxBlob=2304,kKeys=20+kV2RecSlots;\n'
                           '  enum Type:uint16_t {Probe=1,Failure=2,Ble=3,V2Rec=4};\n',
                           ' public:\n'
                           '  static constexpr uint32_t kSector=4096,kMaxBlob=2304,kKeys=20;\n'
                           '  enum Type:uint16_t {Probe=1,Failure=2,Ble=3};\n'),
                          ('  bool protectedSector(uint32_t offset) const;\n'
                           '#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)\n'
                           '  bool reusableByV2Rec(uint32_t offset); // blank or already a V2Rec sector\n'
                           '#endif\n',
                           '  bool protectedSector(uint32_t offset) const;\n')],
 'src/main.cpp': [('#include "evidence_store.h"\n'
                   '#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)\n'
                   '#include "signal_v2_flight_recorder.h"\n'
                   '#endif\n',
                   '#include "evidence_store.h"\n'),
                  ('                                    M5CAN_BUILD_SHA);\n'
                   '#endif\n'
                   '\n'
                   '#if defined(M5CAN_V2REC_LINKED)\n'
                   '  // Boot record before TWAI/BLE start: the only pre-stop flash write.\n'
                   '  '
                   'signal_v2::recorder::begin(&g_can,M5CAN_BUILD_SHA,static_cast<uint32_t>(esp_reset_reason()),\n'
                   '      static_cast<uint32_t>(rtc_get_reset_reason(0)));\n'
                   '#endif\n',
                   '                                    M5CAN_BUILD_SHA);\n#endif\n\n'),
                  ('    m5can::bleEvidenceJournal().append(event,g_ble.diagnosticDrops());\n'
                   '#if defined(M5CAN_V2REC_LINKED)\n'
                   '    signal_v2::recorder::bleEvent(event);\n'
                   '#endif\n',
                   '    m5can::bleEvidenceJournal().append(event,g_ble.diagnosticDrops());\n'),
                  ('  // Never block NimBLE GAP callbacks or CPU0 CAN owner with NVS writes.\n'
                   '  m5can::bleEvidenceJournal().poll(g_can);\n'
                   '#if defined(M5CAN_V2REC_LINKED)\n'
                   '  signal_v2::recorder::poll(g_can); // drains owner ring; flash only after latched stop\n'
                   '#endif\n',
                   '  // Never block NimBLE GAP callbacks or CPU0 CAN owner with NVS writes.\n'
                   '  m5can::bleEvidenceJournal().poll(g_can);\n')],
 'src/serial_protocol.cpp': [('#include "signal_v2_gatt.h"\n'
                              '#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)\n'
                              '#include "signal_v2_flight_recorder.h"\n'
                              '#endif\n',
                              '#include "signal_v2_gatt.h"\n'),
                             ('  if(signal_v2::usbCommand(line))return;\n'
                              '#endif\n'
                              '#if defined(M5CAN_V2REC_LINKED)\n'
                              '  if(signal_v2::recorder::usbCommand(line))return; // read-only, no CAN '
                              'access\n',
                              '  if(signal_v2::usbCommand(line))return;\n')],
 'src/signal_v2_can_owner.h': [('#include "signal_v2_simulator.h"\n'
                                '#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)\n'
                                '#include "signal_v2_flight_recorder_tap.h"\n'
                                '#endif\n',
                                '#include "signal_v2_simulator.h"\n'),
                               ('        else samples_.invalidate(10); // Never promote disarmed '
                                'completion.\n'
                                '#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)\n'
                                '        recorder::tapResult(incoming,ecu_); // const read after handling\n'
                                '#endif\n',
                                '        else samples_.invalidate(10); // Never promote disarmed '
                                'completion.\n')],
 'src/signal_v2_gatt.cpp': [('#include "signal_v2_can_owner.h"\n'
                             '#endif\n'
                             '#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)\n'
                             '#include "signal_v2_flight_recorder.h"\n',
                             '#include "signal_v2_can_owner.h"\n'),
                            ('  owner.noteAuthorizationEnd("CONTROL_FIFO_OVERFLOW");\n'
                             '#if defined(M5CAN_V2REC_LINKED)\n'
                             '  recorder::authEnd(now,recorder::AuthControlFifoOverflow);\n'
                             '#endif\n',
                             '  owner.noteAuthorizationEnd("CONTROL_FIFO_OVERFLOW");\n'),
                            ('  owner.noteAuthorizationEnd("CENTRAL_UNSUBSCRIBE");\n'
                             '#if defined(M5CAN_V2REC_LINKED)\n'
                             '  recorder::authEnd(now,recorder::AuthCentralUnsubscribe);\n'
                             '#endif\n',
                             '  owner.noteAuthorizationEnd("CENTRAL_UNSUBSCRIBE");\n'),
                            ('    owner.noteAuthorizationEnd("OPERATOR_STOP");\n'
                             '#if defined(M5CAN_V2REC_LINKED)\n'
                             '    recorder::authEnd(now,recorder::AuthOperatorStop);\n'
                             '#endif\n',
                             '    owner.noteAuthorizationEnd("OPERATOR_STOP");\n'),
                            ('     !p.session.canPacketCurrent(now,p.output,owner.samples())){\n'
                             '#if defined(M5CAN_V2REC_LINKED)\n'
                             '    recorder::discarded(recorder::DiscardStaleInSend,p.output.bytes[4]);\n'
                             '#endif\n',
                             '     !p.session.canPacketCurrent(now,p.output,owner.samples())){\n'),
                            ('      owner.noteAuthorizationEnd("SESSION_ENDED");\n'
                             '#if defined(M5CAN_V2REC_LINKED)\n'
                             '      recorder::authEnd(now,recorder::AuthSessionEnded);\n'
                             '#endif\n',
                             '      owner.noteAuthorizationEnd("SESSION_ENDED");\n'),
                            ('    if(grantCanArm(now,seen))auto_session.epoch=seen+1;\n'
                             '#if defined(M5CAN_V2REC_LINKED)\n'
                             '    if(can_armed)recorder::authEnd(now,recorder::AuthAutoArmGranted);\n'
                             '#endif\n',
                             '    if(grantCanArm(now,seen))auto_session.epoch=seen+1;\n'),
                            ('    owner.noteAuthorizationEnd("USB_ARM_EXPIRED");\n'
                             '#if defined(M5CAN_V2REC_LINKED)\n'
                             '    recorder::authEnd(now,recorder::AuthUsbArmExpired);\n'
                             '#endif\n',
                             '    owner.noteAuthorizationEnd("USB_ARM_EXPIRED");\n'),
                            ('  display_busy.store(owner.busy());\n'
                             '#endif\n'
                             '#if defined(M5CAN_V2REC_LINKED)\n'
                             '  { // Flight recorder: copies values / const refs only, after the owner '
                             'tick.\n'
                             '    recorder::OwnerView view;\n'
                             '    '
                             'view.armed=can_armed.load();view.manual=manual_stop.load();view.epoch=can_authorization_epoch.load();\n'
                             '    '
                             'view.stopped=owner.stopped();view.busy=owner.busy();view.reason=owner.stopReason();\n'
                             '    for(unsigned i=0;i<2;++i){const auto l=snapshot(i);view.link[i]=l.active;\n'
                             '      view.subscribed[i]=l.status&&l.data;view.generation[i]=l.generation;}\n'
                             '    recorder::observe(now,view,owner.pidDiagnostics(),owner.samples());\n'
                             '  }\n',
                             '  display_busy.store(owner.busy());\n'),
                            ('    for(auto &p:peers){\n'
                             '#if defined(M5CAN_V2REC_LINKED)\n'
                             '      '
                             'if(p.output.size)recorder::discarded(recorder::DiscardSourceTransition,p.output.bytes[4]);\n'
                             '#endif\n',
                             '    for(auto &p:peers){\n'),
                            ('        (p.output.bytes[4] == 0x20 || p.output.bytes[4] == 0x21)) {\n'
                             '#if defined(M5CAN_V2REC_LINKED)\n'
                             '      recorder::discarded(recorder::DiscardLeaseExpired,p.output.bytes[4]);\n'
                             '#endif\n',
                             '        (p.output.bytes[4] == 0x20 || p.output.bytes[4] == 0x21)) {\n'),
                            ('      p.data_transfer = ++p.transfer;\n'
                             '#if defined(M5CAN_V2REC_LINKED)\n'
                             '      '
                             'recorder::produced(p.output.bytes[4],p.output.bytes[4]==0x21?p.output.payload()[12]:p.output.payload()[0]);\n'
                             '#endif\n',
                             '      p.data_transfer = ++p.transfer;\n'),
                            ('      auto op = p.input.payload()[8];\n'
                             '#if defined(M5CAN_V2REC_LINKED)\n'
                             '      recorder::control(op, p.ack.size ? p.ack.payload()[5] : 0xFF);\n'
                             '#endif\n',
                             '      auto op = p.input.payload()[8];\n'),
                            ('          (op == 2 || op == 3 || op == 5 || op == 6 || op == 7 || op == 10)) '
                             '{\n'
                             '#if defined(M5CAN_V2REC_LINKED)\n'
                             '        '
                             'if(p.output.size)recorder::discarded(recorder::DiscardControlAccept,p.output.bytes[4]);\n'
                             '#endif\n',
                             '          (op == 2 || op == 3 || op == 5 || op == 6 || op == 7 || op == 10)) '
                             '{\n')],
 'tests/run_all.sh': [('python3 tools/test_pid_s1.py\n'
                       '\n'
                       '# Signal V2 flight recorder (read-only, owner_live only), PM Claude 2026-11-15.\n'
                       'python3 tools/test_v2_flight_recorder_scope.py\n'
                       'python3 tools/test_v2_flight_recorder.py\n',
                       'python3 tools/test_pid_s1.py\n')],
 'tests/test_signal_v2_b01_policy.py': [('AUTO_MACRO = "M5CAN_SIGNAL_V2_OWNER_AUTO_ARM"\n'
                                         'RECORDER_MACRO = "M5CAN_SIGNAL_V2_FLIGHT_RECORDER"  # PM Claude '
                                         '2026-11-15: read-only, owner_live only\n',
                                         'AUTO_MACRO = "M5CAN_SIGNAL_V2_OWNER_AUTO_ARM"\n'),
                                        ('        # All other effective 5Hz definitions stay the same.\n'
                                         '        self.assertEqual([f for f in flags[OWNER] if OWNER_MACRO '
                                         'not in f and AUTO_MACRO not in f and RECORDER_MACRO not in f],\n',
                                         '        # All other effective 5Hz definitions stay the same.\n'
                                         '        self.assertEqual([f for f in flags[OWNER] if OWNER_MACRO '
                                         'not in f and AUTO_MACRO not in f],\n')],
 'tools/test_p3_scope.py': [('from test_p5_scope import strip_p5, check_tree as check_p5\n'
                             '# PM Claude 2026-11-15 (V2 flight recorder): exact pinned hooks removed first; '
                             'new files allowlisted.\n'
                             'from test_v2_flight_recorder_scope import strip_v2rec, NEW as V2REC_NEW\n',
                             'from test_p5_scope import strip_p5, check_tree as check_p5\n'),
                            ('for path in old_files-modified:\n'
                             '    value=strip_v2rec(path,(ROOT/path).read_text())\n',
                             'for path in old_files-modified:\n    value=(ROOT/path).read_text()\n'),
                            ("assert s==git('show',BASE+':src/signal_v2_session.cpp'),'legacy Session body "
                             "changed'\n"
                             "s=strip_v2rec('src/signal_v2_gatt.cpp',(ROOT/'src/signal_v2_gatt.cpp').read_text())\n",
                             "assert s==git('show',BASE+':src/signal_v2_session.cpp'),'legacy Session body "
                             "changed'\n"
                             "s=(ROOT/'src/signal_v2_gatt.cpp').read_text()\n"),
                            ('# PM Claude 2026-11-03 (S1): only S1_ALLOWED_NEW is added to the new-file '
                             'allowlist.\n'
                             'assert all(x in V2REC_NEW or x in S1_ALLOWED_NEW or x in allowed_new or x in '
                             "p5_new or x.startswith(('docs/evidence/p3/','docs/evidence/p5/')) for x in "
                             'added),added-allowed_new-p5_new\n',
                             '# PM Claude 2026-11-03 (S1): only S1_ALLOWED_NEW is added to the new-file '
                             'allowlist.\n'
                             'assert all(x in S1_ALLOWED_NEW or x in allowed_new or x in p5_new or '
                             "x.startswith(('docs/evidence/p3/','docs/evidence/p5/')) for x in "
                             'added),added-allowed_new-p5_new\n')],
 'tools/test_p5_scope.py': [('        value=(root/path).read_bytes()\n'
                             '        # PM Claude 2026-11-15 (V2 flight recorder): exact pinned hooks only.\n'
                             '        from test_v2_flight_recorder_scope import strip_v2rec\n'
                             '        value=strip_v2rec(path,value.decode()).encode() if '
                             "path.endswith(('.h','.cpp','.py','.ini','.sh')) else value\n",
                             '        value=(root/path).read_bytes()\n'),
                            ("                if path=='src/signal_v2_can_owner.h':\n"
                             '                    from test_v2_flight_recorder_scope import strip_v2rec  # '
                             'PM Claude 2026-11-15: exact recorder hooks\n'
                             '                    '
                             "source=base/'s1-owner.h';source.write_text(strip_s1(path,strip_v2rec(path,(root/path).read_text())))\n",
                             "                if path=='src/signal_v2_can_owner.h':\n"
                             '                    '
                             "source=base/'s1-owner.h';source.write_text(strip_s1(path,(root/path).read_text()))\n")],
 'tools/test_pid_s1_scope.py': [('        '
                                 "old=subprocess.check_output(['git','show',BASE+':'+path],cwd=ROOT,text=True)\n"
                                 '        # PM Claude 2026-11-15 (V2 flight recorder): its exact pinned '
                                 'hooks are removed first.\n'
                                 '        from test_v2_flight_recorder_scope import strip_v2rec\n'
                                 '        assert '
                                 'strip_s1(path,strip_v2rec(path,(ROOT/path).read_text()))==old,path\n',
                                 '        '
                                 "old=subprocess.check_output(['git','show',BASE+':'+path],cwd=ROOT,text=True)\n"
                                 '        assert strip_s1(path,(ROOT/path).read_text())==old,path\n')]}


def strip_v2rec(path, text):
    for new, old in reversed(PAIRS.get(path, [])):
        assert text.count(new) == 1, ('V2 recorder exact hook', path, new[:120])
        text = text.replace(new, old, 1)
    return text


FORBIDDEN_TX = re.compile(
    r'twai_\w+\s*\(|submitQuery|transmitOwnedFrame|acquireLease|revokeLease|revokeAuthorization|'
    r'beginStageS1|switchDriverMode|enterFaultLocked|noteAuthorizationEnd|grantCanArm|manualCanStop|'
    r'\.reserve\s*\(|setObservationWindow|\binvalidate\s*\(|\bstop\s*\(\s*"|requestStageS1|'
    r'\bcan_armed\s*(=[^=]|\.store|\.exchange)|can_authorization_epoch\s*(\+\+|=[^=]|\.store|\.compare)|'
    r'\+\+\s*can_authorization_epoch|\bmanual_stop\s*(=[^=]|\.store)|esp_partition_(write|erase)|'
    r'\bPreferences\b|\bnvs_\w+\(|disconnect\s*\(|ble_gattc_|\bperiod_\b|\bnext_\s*=|budget_')
FORBIDDEN_ID = re.compile(r'address_suffix|peer_id_addr|peer_ota_addr|getAddress|ble_gap_conn_find|'
                          r'\bltk\b|\birk\b|\bVIN\b|latitude|longitude|macAddress')


def git(*a):
    return subprocess.check_output(['git', *a], cwd=ROOT, text=True)


def added_lines(path):
    out = []
    for new, old in PAIRS.get(path, []):
        old_lines = old.splitlines()
        out += [l for l in new.splitlines() if l not in old_lines]
    return out


def check():
    if subprocess.run(['git', 'cat-file', '-e', BASE_REF + '^{commit}'], cwd=ROOT,
                      capture_output=True).returncode:
        subprocess.run(['git', 'fetch', '--no-tags', 'origin', 'main'], cwd=ROOT, check=True)
    base = git('rev-parse', BASE_REF + '^{commit}').strip()
    old_files = set(git('ls-tree', '-r', '--name-only', base).splitlines())
    tracked = set(git('ls-files', '--cached', '--others', '--exclude-standard').splitlines())
    changed = []
    for path in sorted(old_files):
        p = ROOT / path
        if not p.exists():
            raise AssertionError(('deleted file', path))
        cur = p.read_bytes()
        old = subprocess.check_output(['git', 'show', base + ':' + path], cwd=ROOT)
        if cur == old:
            assert path not in PAIRS, ('stale pin', path)
            continue
        changed.append(path)
        assert path in PAIRS, ('unexpected change outside recorder allowlist', path)
        # Other slices may have their own allowlisted lines on top of this file
        # only in a later PR; at this base the strip must be exact.
        assert strip_v2rec(path, cur.decode()).encode() == old, ('not exact after strip', path)
    assert set(changed) == set(PAIRS), set(PAIRS) - set(changed)
    added = tracked - old_files
    assert all(x in NEW for x in added), sorted(x for x in added if x not in NEW)
    # Content constraints on every added hook line and recorder source.
    lines = [(path, l) for path in PAIRS if path.startswith(('src/', 'include/')) for l in added_lines(path)]
    for path in RECORDER_SOURCES:
        lines += [(path, l) for l in (ROOT / path).read_text().splitlines()]
    for path, line in lines:
        code = line.split('//')[0]
        assert not FORBIDDEN_TX.search(code), ('recorder line touches TX/admission/auth/flash', path, line)
        assert not FORBIDDEN_ID.search(line), ('recorder line touches identity', path, line)
    # Hooks in production files are all guarded and only call recorder:: / read-only state.
    # With the recorder macros undefined, every hooked production file is
    # token-identical to the base: hooks live only inside recorder guards.
    guard = re.compile(r'^#if defined\(M5CAN_(V2REC_LINKED|SIGNAL_V2_FLIGHT_RECORDER)\)$')
    for path in ('src/signal_v2_gatt.cpp', 'src/main.cpp', 'src/serial_protocol.cpp',
                 'src/evidence_store.cpp', 'src/signal_v2_can_owner.h'):
        kept, depth = [], 0
        for l in (ROOT / path).read_text().splitlines():
            if depth == 0 and guard.match(l.strip()):
                depth = 1; continue
            if depth:
                if l.strip().startswith('#if'): depth += 1
                elif l.strip() == '#endif': depth -= 1
                assert not l.strip().startswith('#else'), (path, 'no #else in recorder guards')
                continue
            kept.append(l)
        old = subprocess.check_output(['git', 'show', base + ':' + path], cwd=ROOT, text=True)
        assert ''.join('\n'.join(kept).split()) == ''.join(old.split()), ('unguarded change', path)
    owner_add = added_lines('src/signal_v2_can_owner.h')
    assert [l.strip() for l in owner_add] == [
        '#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)', '#include "signal_v2_flight_recorder_tap.h"',
        '#endif', '#if defined(M5CAN_SIGNAL_V2_FLIGHT_RECORDER)',
        'recorder::tapResult(incoming,ecu_); // const read after handling', '#endif'], owner_add
    tap = (ROOT / 'include/signal_v2_flight_recorder_tap.h').read_text()
    assert 'inline void tapResult(const Result &r, const Selector &ecu)' in tap
    assert not re.search(r'\bmutable\b|const_cast', tap)
    # Flash mutation path: exactly one store put, only in writePage, reached
    # from boot() (before TWAI) and flushStep() after window_open + checkpoint.
    rec = (ROOT / 'include/signal_v2_flight_recorder.h').read_text()
    assert rec.count('store_->put(') == 1 and rec.count('writePage(') == 3
    flush = rec[rec.index('  bool flushStep('):rec.index('  const Status &status() const')]
    assert flush.index('!status_.window_open') < flush.index('cp.begin()') < flush.index('writePage(')
    assert 'open_pending_ = true' in rec and rec.count('open_pending_ = true') == 1
    assert "rec[6] == EvStopIdle" in rec, 'flush window opens only on STOP_IDLE'
    glue = (ROOT / 'src/signal_v2_flight_recorder.cpp').read_text()
    assert glue.count('evidenceStore().put(') == 1
    assert '#error "flight recorder requires the owner_live image' in glue
    # Boot write happens before BLE and TWAI start.
    main = (ROOT / 'src/main.cpp').read_text()
    assert main.index('signal_v2::recorder::begin(') < main.index('g_ble_ok = g_ble.begin();') < \
        main.index('if (!g_can.begin()) {')
    # Macro is exclusive to the owner_live environment.
    sys.path.insert(0, str(ROOT / 'tests'))
    from test_signal_v2_b01_policy import flags_by_env, OWNER
    _, flags = flags_by_env(ROOT / 'platformio.ini')
    users = sorted(e for e, f in flags.items() if any('M5CAN_SIGNAL_V2_FLIGHT_RECORDER' in x for x in f))
    assert users == [OWNER], users
    # Mutation self-test: a TX-relevant edit in a pinned hook is rejected.
    bad = (ROOT / 'src/signal_v2_gatt.cpp').read_text().replace(
        'recorder::observe(now,view,', 'owner.invalidate(10);recorder::observe(now,view,', 1)
    try:
        strip_v2rec('src/signal_v2_gatt.cpp', bad)
        raise SystemExit('mutation survived')
    except AssertionError:
        pass
    print('PASS V2 recorder scope: %d existing files exact after pinned strips, %d new files allowlisted, '
          '%d added/recorder lines free of TX/admission/auth/flash/identity APIs, macro only in %s'
          % (len(changed), len(added), len(lines), OWNER[4:]))


if __name__ == '__main__':
    check()
