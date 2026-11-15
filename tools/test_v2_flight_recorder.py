#!/usr/bin/env python3
"""Signal V2 flight recorder tests (PM Claude 2026-11-15). Host only: no devices, no CAN.

1. tests/v2_flight_recorder.cpp (ASan/UBSan): SPSC ring, producer records,
   real EvidenceStore retention/rotation over a 1.5 MiB image, own-sector-only
   erase, failure injection, boot-loop limiter, RAM eviction policy, and
   "no flash access while armed" (the fake flash counts violations).
2. Offline decoder: the C++ test writes a partition image; the Python decoder
   must reproduce the records the firmware text formatter printed.
3. Fixed-base TX/RX trace equivalence for every existing owner scenario of
   tools/test_pid_s1.py: base c3843a6 == head without recorder == head with the
   recorder macro (result tap compiled into CanOwner::tick) == head + producer/
   consumer after EVERY tick with (a) normal store, (b) failing+slow store,
   (c) all recorder counters forced to UINT32_MAX and a never-drained ring.
   Recorder stores report puts and armed-write violations on stderr: the
   violation count must be 0 (writes only after a latched stop).
4. Positive control: a hook that calls owner.invalidate() changes the trace.
"""
import json, os, pathlib, re, subprocess, sys, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_v2_flight_recorder_scope import ROOT, check as scope_check
from test_pid_s1 import SCENARIOS, fixture, build, run, copy_tree, WARN, CXX

BASE = 'c3843a6'
REC = '-DM5CAN_SIGNAL_V2_FLIGHT_RECORDER=1'
HOOK = r'''
#include "signal_v2_flight_recorder.h"
#include <cstdlib>
#include <unistd.h>
namespace v2rectest {
using namespace signal_v2::recorder;
SpscRing ring; Producer producer; Consumer consumer;
struct Store final : PageStore {
  bool armed_now = false; unsigned puts = 0, violations = 0;
  bool ready() const override { return true; }
  bool put(unsigned, const uint8_t *, size_t) override {
    ++puts; if (armed_now) ++violations;
    if (V2REC_MODE == 2) { usleep(300); return false; }
    return true;
  }
  size_t length(unsigned) const override { return 0; }
  size_t get(unsigned, uint8_t *, size_t) override { return 0; }
} store;
struct Cp final : Checkpoint { bool begin() override { return true; } void end() override {} } cp;
bool booted = false;
void report() { std::fprintf(stderr, "V2REC_TRACE puts=%u armed_violations=%u ring_drops=%u\n",
                             store.puts, store.violations, ring.drops()); }
}
template <class O> void v2recAfterTick(O &owner, uint32_t now, bool armed, uint32_t epoch) {
  using namespace v2rectest;
  if (!booted) { booted = true; producer.attach(&ring, nullptr, nullptr); BootInfo i; consumer.boot(&store, i, now); std::atexit(report); }
  OwnerView v; v.armed = armed; v.stopped = owner.stopped(); v.busy = owner.busy(); v.epoch = epoch;
#if defined(M5CAN_SIGNAL_V2_OWNER_CONTINUOUS_LIVE)
  v.reason = owner.stopReason();
#endif
  v.link[0] = v.subscribed[0] = true; v.generation[0] = 1;
  producer.control(4, armed ? 0 : 7); producer.produced(0x21, 0); producer.discarded(0, 0x20);
  producer.observe(now, v, owner.pidDiagnostics(), owner.samples(), resultTap());
  // A write is legal only before TWAI/arm (boot) or after a latched stop.
  store.armed_now = armed && !v.reason;
  if (V2REC_MODE == 3) {
    std::memset(&producer.testCum(), 0xFF, sizeof(producer.testCum()));
    ResultTap &t = resultTap();
    for (auto &x : t.latency) x = 0xFFFFFFFFu;
    for (auto &x : t.status) x = 0xFFFFFFFFu;
    t.results = t.nodata_run = t.nodata_run_max = t.multi_frame = 0xFFFFFFFFu;
  } else {
    consumer.drain(ring, now); consumer.flushStep(now, cp);
  }
#if V2REC_MODE == 9
  owner.invalidate(10);  // positive control only: a recorder that mutates the owner
#endif
}
'''


def recorder_fixture(text, mode):
    a = 'owner.tick(now,d,armed,epoch);'
    assert text.count(a) == 1
    text = text.replace(a, 'owner.tick(now,d,armed,epoch);v2recAfterTick(owner,now,armed,epoch);')
    pos = text.index('static void traceOwnerTick(')
    return '#define V2REC_MODE %d\n' % mode + text[:pos] + HOOK + text[pos:]


def run_unit(tmp):
    exe = tmp / 'v2rec'
    run([CXX, *WARN, '-fsanitize=address,undefined', REC, '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'src'),
         str(ROOT / 'tests/v2_flight_recorder.cpp'), str(ROOT / 'src/evidence_store.cpp'), '-o', str(exe)])
    out = run([str(exe)])
    assert 'PASS V2 flight recorder host tests' in out, out
    print(out.strip())
    # C++11 compatibility of the tap included by CanOwner in every C++11 fixture.
    t = tmp / 'tap11.cpp'
    t.write_text('#include "signal_v2_flight_recorder_tap.h"\nstruct S{int state()const{return 1;}unsigned char consecutiveBad()const{return 2;}};\n'
                 'struct F{unsigned char dlc=1,data[8]={0x10};};struct Q{int status=1;unsigned char response_count=0,raw_frame_count=1;'
                 'unsigned tx_to_first_response_us=12000,transaction_us=0;F raw_frames[1];unsigned raw_rx_offsets_us[1]={7};};\n'
                 'int main(){Q q;S s;signal_v2::recorder::tapResult(q,s);auto&t=signal_v2::recorder::resultTap();'
                 'return !(t.nodata_run==1&&t.last_ff==1&&t.last_ff_us==7&&t.latency[1]==1&&t.ecu_bad==2&&t.multi_frame==1);}\n')
    run([CXX, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'include'), str(t), '-o', str(tmp / 'tap11')])
    run([str(tmp / 'tap11')])
    print('PASS result tap: C++11, NoData run, FF offset, latency bucket, ECU consecutive_bad')
    return exe


def run_decoder(tmp, exe):
    img = tmp / 'spiffs.bin'
    text = run([str(exe), 'image', str(img)])
    out = json.loads(run([sys.executable, str(ROOT / 'tools/decode_v2_flight_recorder.py'), str(img),
                          '--csv', str(tmp / 'rec.csv')]))
    stops = [r for b in out['boots'] for r in b['records'] if r['type'] == 'STOP']
    assert len(stops) == 1 and stops[0]['reason'] == 'QUERY_ADMISSION_FAILED', stops
    assert stops[0]['can_last_error'] == 'running'
    # Every SUMMARY 'acquired' the firmware printed appears in the decoder output.
    fw = [int(m) for m in re.findall(r'type=SUMMARY,.*?,acquired=(\d+)', text)]
    dec = [r['acquired'] for b in out['boots'] for r in b['records'] if r['type'] == 'SUMMARY']
    assert fw and sorted(set(fw)) == sorted(set(dec)), (fw, dec)
    assert all('length_mismatch' not in r for b in out['boots'] for r in b['records'])
    boots = [b['boot'] for b in out['boots']]
    assert boots == [1, 2], boots
    full = tmp / 'full8m.bin'
    full.write_bytes(b'\xff' * 0x670000 + img.read_bytes() + b'\xff' * (0x800000 - 0x670000 - 0x180000))
    out2 = json.loads(run([sys.executable, str(ROOT / 'tools/decode_v2_flight_recorder.py'), str(full)]))
    assert out2['boots'] == out['boots']
    rows = (tmp / 'rec.csv').read_text().splitlines()
    assert len(rows) > 1 and 'discard_transition_data' in rows[0] and 'latency_lt10' in rows[0]
    print('PASS decoder: partition and full 8MiB dump decode identically; STOP reason, CAN last error, '
          'SUMMARY values match firmware text; CSV %d rows' % (len(rows) - 1))


def trace_equivalence(tmp):
    base = tmp / 'base'; base.mkdir()
    archive = subprocess.Popen(['git', 'archive', BASE], cwd=ROOT, stdout=subprocess.PIPE)
    subprocess.run(['tar', '-xf', '-', '-C', str(base)], stdin=archive.stdout, check=True)
    archive.stdout.close(); assert archive.wait() == 0
    totals = {'puts': 0, 'violations': 0, 'flush_scenarios': 0}
    for i, (source, flags, inc) in enumerate(SCENARIOS):
        plain = [f for f in flags if f != REC]
        text = fixture(ROOT / source)
        f = tmp / ('fx%d.cpp' % i); f.write_text(text)
        results = {}
        results['base'] = subprocess.run([str(build(base, f, tmp / 'b', plain, inc))], capture_output=True, text=True)
        results['head'] = subprocess.run([str(build(ROOT, f, tmp / 'h', plain, inc))], capture_output=True, text=True)
        results['head+tap'] = subprocess.run([str(build(ROOT, f, tmp / 't', plain + [REC], inc))], capture_output=True, text=True)
        for mode, label in ((1, 'rec-store-ok'), (2, 'rec-store-fail-slow'), (3, 'rec-extreme-nodrain')):
            g = tmp / ('fx%d_%d.cpp' % (i, mode)); g.write_text(recorder_fixture(text, mode))
            results[label] = subprocess.run([str(build(ROOT, g, tmp / ('r%d' % mode), plain + [REC], inc))],
                                            capture_output=True, text=True)
            m = re.search(r'V2REC_TRACE puts=(\d+) armed_violations=(\d+)', results[label].stderr)
            assert m, (source, label, results[label].stderr[-2000:])
            totals['puts'] += int(m.group(1)); totals['violations'] += int(m.group(2))
            if mode == 1 and int(m.group(1)) > 1: totals['flush_scenarios'] += 1
        ref = (results['base'].returncode, results['base'].stdout)
        assert ref[0] == 0, (source, results['base'].stderr[-2000:])
        for label, r in results.items():
            assert (r.returncode, r.stdout) == ref, (source, flags, label, r.stderr[-1500:])
        lines = ref[1].splitlines()
        tx = sum(x.startswith('TX ') for x in lines); rx = sum(x.startswith('RX ') for x in lines)
        print('PASS base == head == head+tap == rec-store-ok == rec-store-fail-slow == rec-extreme-nodrain',
              source, ' '.join(x[2:] for x in flags if 'RP8' in x or '10HZ' in x or 'STAGE' in x or 'OWNER' in x or 'HIL' in x) or '-',
              'tx=%d rx=%d' % (tx, rx))
    assert totals['violations'] == 0, totals
    assert totals['flush_scenarios'] >= 1, totals  # at least one latched-stop flush exercised
    print('PASS recorder never wrote while armed: puts=%d armed_violations=0, latched-stop flush scenarios=%d'
          % (totals['puts'], totals['flush_scenarios']))
    # Positive control: a recorder that mutates the owner must change the trace.
    source, flags, inc = SCENARIOS[2]
    text = fixture(ROOT / source)
    f = tmp / 'pc.cpp'; f.write_text(text)
    g = tmp / 'pc9.cpp'; g.write_text(recorder_fixture(text, 9))
    a = subprocess.run([str(build(base, f, tmp / 'pcb', flags, inc))], capture_output=True, text=True).stdout
    b = subprocess.run([str(build(ROOT, g, tmp / 'pcm', flags + [REC], inc))], capture_output=True, text=True).stdout
    assert a != b
    print('PASS positive control: an owner-mutating hook changes the TX/RX trace')


def main():
    scope_check()
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        exe = run_unit(tmp)
        run_decoder(tmp, exe)
        trace_equivalence(tmp)


if __name__ == '__main__':
    main()
