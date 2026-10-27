#include "signal_v2_session.h"
#include "signal_v2_demo.h"
#include <algorithm>
#include <cassert>
#include <cmath>
using namespace signal_v2;
static Packet control(uint32_t rid, uint32_t revision, uint8_t op,
                      const uint8_t *body, size_t size, bool hello = false) {
  uint8_t p[256]{};
  w32(p, rid); w32(p + 4, revision); p[8] = op; w16(p + 10, size);
  if (size) std::copy(body, body + size, p + 12);
  return encode(1, hello ? 0 : 17, hello ? 0 : 19, rid, p, size + 12);
}
int main() {
  uint8_t one[8]{}, two[8]{};
  assert(demoPayload(0x0c, 1, one) == 4);
  assert(demoPayload(0x0c, 2, two) == 4 && (one[2] != two[2] || one[3] != two[3]));
  assert(demoPayload(0x0d, 1, one) == 3 && one[2] != 0);
  assert(demoPayload(0x05, 1, one) == 3 && one[2] >= 118);
  assert(demoPayload(0x5b, 1, one) == 3 && one[2] > 0);
  assert(demoPayload(0x9a, 1, one) == 8 && one[4] != 0);
  Session s;
  s.connect(17, 19);
  Packet out;
  uint8_t hello[12]{1, 2};
  w16(hello + 4, 512);
  s.control(control(1, 0, 1, hello, 12, true), true, 10, out);
  assert(out.payload()[5] == Accept);
  uint8_t profile[8 + 24 * 5]{};
  w32(profile, 1);
  profile[4] = 5;
  const uint16_t pids[5] = {0x0d, 0x0c, 0x05, 0x5b, 0x9a};
  for (unsigned i = 0; i < 5; ++i) {
    uint8_t *e = profile + 8 + i * 24;
    w16(e, i + 1); e[2] = 1; w16(e + 4, pids[i]);
    e[6] = i == 0 ? 0 : 1;
    w32(e + 8, 0x18daf101); w32(e + 12, 1);
    w16(e + 16, 200); w16(e + 18, 400); w16(e + 20, 600);
  }
  s.control(control(2, 0, 2, profile, sizeof(profile)), true, 100, out);
  assert(out.payload()[5] == Accept && s.revision() == 1);
  Simulator source;
  Simulator::Demand demand{};
  demand.fill(200);
  // USB DEMO defaults OFF: NO_DATA has NO payload, not synthetic zero.
  assert(s.produce(100, out, &source, false) && out.bytes[4] == 0x21);
  assert(s.produce(101, out, &source, false) && out.bytes[4] == 0x20);
  assert(out.payload()[0] == 5);
  size_t off = 2;
  for (int i = 0; i < 5; ++i) {
    const uint8_t *r = out.payload() + off;
    assert(u16(r) == 44 && r[3] == 2 && r[7] == 2 &&
           r[42] == 0 && u32(r + 38) == 0);
    off += 44;
  }
  // Explicit USB ON: each of the five values is a real DEMO payload.
  s.clearDemoCache(200);
  source.tick(200, demand);
  assert(s.produce(201, out, &source, true) && out.bytes[4] == 0x20);
  assert(out.payload()[0] == 5);
  off = 2;
  uint8_t rpm_before[2]{};
  for (int i = 0; i < 5; ++i) {
    const uint8_t *r = out.payload() + off;
    assert(r[3] == 2 && r[7] <= 1 && r[6] == 1 &&
           u16(r + 8) == pids[i] && u32(r + 10) == 1 &&
           u32(r + 38) != 0 && r[44] == 0x41);
    if (pids[i] == 0x0c) { rpm_before[0] = r[46]; rpm_before[1] = r[47]; }
    off += u16(r);
  }
  // Snapshot is cached and MUST NOT invent another sample sequence.
  const Packet original = out;
  uint8_t snap[8]{};
  w32(snap, u32(original.bytes.data() + 16));
  w32(snap + 4, 1);
  s.control(control(3, 1, 7, snap, 8), true, 220, out);
  assert(out.payload()[5] == Accept);
  assert(s.produce(221, out, &source, true) && out.bytes[4] == 0x21);
  assert(out.payload()[12] == 5 && u32(out.payload()) ==
         u32(out.bytes.data() + 16) + 1);
  off = 16;
  for (int i = 0; i < 5; ++i) {
    assert(u32(out.payload() + off + 38) == 1);
    off += u16(out.payload() + off);
  }
  source.tick(401, demand);
  assert(s.produce(401, out, &source, true) && out.bytes[4] == 0x20);
  off = 2;
  for (int i = 0; i < 5; ++i) {
    const uint8_t *r = out.payload() + off;
    if (pids[i] == 0x0c)
      assert(r[46] != rpm_before[0] || r[47] != rpm_before[1]);
    off += u16(r);
  }
  // USB OFF wipes stale valid snapshot; subsequent DATA is NOT fake 0rpm.
  s.clearDemoCache(450);
  // OFF makes samples invalid without rewinding SampleSeq.
  source.tick(450, Simulator::Demand{});
  assert(s.produce(450, out, &source, false) && out.bytes[4] == 0x20);
  off = 2;
  for (int i = 0; i < 5; ++i) {
    assert(out.payload()[off + 7] == 2 && out.payload()[off + 42] == 0);
    off += u16(out.payload() + off);
  }
  Packet decoded;
  assert(decode(out.bytes.data(), out.size, decoded) == Error::None);
  // Re-enable on the SAME boot/session; sequence must stay monotonic.
  source.tick(650, demand);
  s.clearDemoCache(650);
  assert(s.produce(651, out, &source, true) && out.bytes[4] == 0x20);
  assert(out.payload()[0] == 5);
  off = 2;
  for (int i = 0; i < 5; ++i) {
    assert(out.payload()[off + 7] <= 1);
    assert(u32(out.payload() + off + 38) >= 3);
    off += u16(out.payload() + off);
  }
}
