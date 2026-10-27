#include "signal_v2_registry.h"
#include "signal_v2_session.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
using namespace signal_v2;
Packet hex(const std::string &s) {
  std::vector<uint8_t> b;
  for (size_t i = 0; i < s.size(); i += 2)
    b.push_back(std::stoul(s.substr(i, 2), nullptr, 16));
  Packet p;
  assert(decode(b.data(), b.size(), p) == Error::None);
  return p;
}
Packet request(uint32_t rid, uint32_t rev, uint8_t op,
               const uint8_t *body = nullptr, size_t n = 0) {
  uint8_t p[486]{};
  w32(p, rid);
  w32(p + 4, rev);
  p[8] = op;
  w16(p + 10, n);
  if (n)
    std::copy(body, body + n, p + 12);
  return encode(1, 0x11223344, 0xa1b2c3d4, rid, p, n + 12);
}
int main(int argc, char **argv) {
  assert(argc == 7);
  auto hello = hex(argv[1]), ha = hex(argv[2]), profile = hex(argv[3]),
       pa = hex(argv[4]), badrev = hex(argv[5]), bra = hex(argv[6]);
  Session s;
  Packet out;
  s.connect(0x11223344, 0xa1b2c3d4);
  s.control(hello, false, 0, out);
  assert(out.payload()[5] == NotAuthorized);
  assert(!s.active(0));
  s.control(hello, true, 0, out);
  assert(out.size == ha.size && out.bytes == ha.bytes);
  s.control(profile, true, 100, out);
  assert(out.size == pa.size && out.bytes == pa.bytes);
  assert(s.revision() == 1 && s.reservations(100) == 1);
  auto original = out;
  s.control(profile, true, 200, out);
  assert(out.bytes == original.bytes && s.revision() == 1);
  auto mutated = profile;
  mutated.payload()[12 + 24] ^= 1;
  s.control(mutated, true, 200, out);
  assert(out.payload()[5] == ProtocolError && s.revision() == 1);
  s.control(badrev, true, 300, out);
  assert(out.bytes == bra.bytes && s.revision() == 1);
  assert(s.produce(300, out) && out.bytes[4] == 0x21);
  assert(s.produce(301, out) && out.bytes[4] == 0x20 && out.payload()[5] == 2);
  Packet checked;
  assert(decode(out.bytes.data(), out.size, checked) == Error::None);
  auto seq = u32(out.payload() + 2 + 38);
  auto at = u32(out.payload() + 2 + 26);
  uint8_t b[8]{};
  w32(b + 4, 1);
  s.control(request(6, 1, 7, b, 8), true, 350, out);
  assert(out.payload()[5] == Accept);
  assert(s.produce(351, out) && out.bytes[4] == 0x21 &&
         u32(out.payload() + 16 + 38) == seq &&
         u32(out.payload() + 16 + 26) == at);
  assert(!s.active(3100) && s.reservations(3100) == 0 && s.produce(3100, out) &&
         out.bytes[4] == 0x22);
  s.control(request(7, 1, 4), true, 3101, out);
  assert(out.payload()[5] == TxLocked);
  for (unsigned i = 8; i < 20; ++i)
    s.control(request(i, 1, 8), true, 3200, out);
  s.control(profile, true, 3300, out);
  assert(out.payload()[5] == ResultUnknown && s.revision() == 1);
  auto old = request(20, 1, 8);
  w32(old.bytes.data() + 12, 9);
  s.control(old, true, 3300, out);
  assert(out.payload()[5] == ProtocolError);
  Session second;
  second.connect(7, 8);
  assert(!second.active(0));
  assert(s.revision() == 1);
  s.invalidate();
  assert(!s.active(0));
  // RELAXED plan is only committed by explicit ACCEPT_PLAN, then cached ACK.
  Session planner;
  planner.connect(0x11223344, 0xa1b2c3d4);
  planner.control(hello, true, 0, out);
  auto fast = profile;
  w16(fast.payload() + 12 + 8 + 16, 100);
  planner.control(fast, true, 0, out);
  assert(out.payload()[5] == Relaxed && planner.revision() == 0 &&
         !planner.active(0));
  uint8_t accept[8]{};
  w32(accept, 2);
  w32(accept + 4, 1);
  planner.control(request(3, 0, 3, accept, 8), true, 1, out);
  assert(out.payload()[5] == Accept && planner.revision() == 1);
  auto commit = out;
  planner.control(request(3, 0, 3, accept, 8), true, 2, out);
  assert(out.bytes == commit.bytes && planner.revision() == 1);
  Simulator source;
  Simulator::Demand demand{};
  planner.demand(2, demand);
  planner.demand(2, demand);
  source.tick(2, demand);
  assert(source.reservations() == 1 && source.acquisitions() == 1);
  source.tick(3, demand);
  assert(source.acquisitions() == 1);
  demand.fill(0);
  planner.demand(3001, demand);
  source.tick(3001, demand);
  assert(source.reservations() == 0 && source.acquisitions() == 1);
  // Alternating snapshots: partial writes never supersede a valid generation.
  Registry reg;
  Identity id{0, 1, 2, 3, 4, 5, 6};
  Binding bind{};
  bind[0] = 42;
  auto before = reg.image();
  assert(reg.approve(id, bind));
  auto after = reg.image();
  assert(reg.authorized(id, bind));
  for (unsigned cut = 0; cut < 256; ++cut) {
    RegistryImage torn{};
    std::copy(after.begin(), after.begin() + cut, torn.begin());
    Registry boot;
    assert(boot.load(before, torn));
    assert(!boot.authorized(id, bind));
  }
  Registry restored;
  assert(restored.load(before, after));
  assert(restored.authorized(id, bind));
  bind[1] = 1;
  assert(!restored.authorized(id, bind));
  bind[1] = 0;
  assert(restored.revoke(id) && !restored.authorized(id, bind));
  assert(restored.load(after, restored.image()) &&
         !restored.authorized(id, bind));
  std::cout << "session/replay/revision/lease/snapshot and 256 torn registry "
               "writes PASS\n";
}
