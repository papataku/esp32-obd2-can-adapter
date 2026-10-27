#include "signal_v2_registry.h"
#include "signal_v2_session.h"
#include "signal_v2_sequence.h"
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
  // ACK stalls may delay a delivery, but must not accumulate phase drift.
  Session cadence;
  cadence.connect(0x11223344, 0xa1b2c3d4);
  cadence.control(hello, true, 0, out);
  cadence.control(profile, true, 100, out);
  assert(cadence.produce(100, out) && out.bytes[4] == 0x21);
  assert(cadence.produce(150, out) && out.bytes[4] == 0x20);
  assert(cadence.produce(300, out) &&
         out.bytes[4] == 0x20); // not deferred to 350
  assert(!cadence.produce(499, out));
  assert(cadence.produce(510, out) && out.bytes[4] == 0x20);
  assert(cadence.produce(700, out) &&
         out.bytes[4] == 0x20); // not deferred to 710
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
  Session speed;
  speed.connect(0x11223344, 0xa1b2c3d4);
  speed.control(hello, true, 0, out);
  uint8_t speed_body[32];
  std::copy(profile.payload() + 12, profile.payload() + 44, speed_body);
  w16(speed_body + 12, 0x0d);
  speed_body[14] = 3;
  speed.control(request(2, 0, 2, speed_body, 32), true, 1, out);
  assert(out.payload()[5] == Reject && speed.revision() == 0);
  speed_body[14] = 0;
  speed.control(request(3, 0, 2, speed_body, 32), true, 2, out);
  assert(out.payload()[5] == Accept && speed.revision() == 1);

  // N-01: Priority 0 is reserved only for speed PID 010D, regardless of
  // meter placement. Invalid requests must not change the existing profile.
  const uint16_t otherPids[] = {0x000c, 0x0005, 0x005b, 0x009a};
  for (uint16_t pid : otherPids) {
    Session check;
    check.connect(0x11223344, 0xa1b2c3d4);
    check.control(hello, true, 0, out);
    uint8_t body[32]{};
    std::copy(profile.payload() + 12, profile.payload() + 44, body);
    w16(body + 12, pid);
    body[14] = 0;
    check.control(request(2, 0, 2, body, sizeof(body)), true, 10, out);
    assert(out.payload()[5] == Reject && check.revision() == 0 &&
           check.reservations(10) == 0);

    // A valid main-meter subscription still works after rejection.
    body[14] = 1;
    check.control(request(3, 0, 2, body, sizeof(body)), true, 11, out);
    assert(out.payload()[5] == Accept && check.revision() == 1 &&
           check.reservations(11) == 1);

    // A rejected replacement must retain the previously accepted profile.
    body[14] = 0;
    w32(body, 2);
    check.control(request(4, 1, 2, body, sizeof(body)), true, 12, out);
    assert(out.payload()[5] == Reject && check.revision() == 1 &&
           check.reservations(12) == 1);
  }
  for (uint8_t priority = 1; priority <= 3; ++priority) {
    Session check;
    check.connect(0x11223344, 0xa1b2c3d4);
    check.control(hello, true, 0, out);
    uint8_t body[32]{};
    std::copy(profile.payload() + 12, profile.payload() + 44, body);
    w16(body + 12, 0x000d);
    body[14] = priority;
    check.control(request(2, 0, 2, body, sizeof(body)), true, 10, out);
    assert(out.payload()[5] == Reject && check.revision() == 0);
    body[14] = 0;
    check.control(request(3, 0, 2, body, sizeof(body)), true, 11, out);
    assert(out.payload()[5] == Accept && check.revision() == 1);
  }
  Simulator source;
  Simulator::Demand demand{};
  planner.demand(2, demand);
  planner.demand(2, demand);
  source.tick(2, demand);
  assert(source.reservations() == 1 && source.acquisitions() == 1);
  source.tick(203, demand);
  assert(source.acquisitions() == 2);
  source.tick(402, demand);
  assert(source.acquisitions() == 3); // original 2+200n phase
  source.tick(403, demand);
  assert(source.acquisitions() == 3);
  demand.fill(0);
  planner.demand(3001, demand);
  source.tick(3001, demand);
  assert(source.reservations() == 0 && source.acquisitions() == 3);

  // Independent Central sequence acceptance: dropped DATA11 followed by
  // HEARTBEAT12 MUST require a snapshot; DATA13 cannot clear that fault.
  assert(acceptsSequential(10, 11, false));
  assert(!acceptsSequential(10, 12, false));
  assert(!acceptsSequential(10, 13, true));
  assert(acceptsSequential(12, 13, false)); // only after validated snapshot
  assert(!acceptsSequential(0, 1, false));
  assert(!acceptsSequential(UINT32_MAX, 0, false));

  // The HELLO-advertised 16 is a DESCRIPTOR bound, not a promise that every
  // combination can fit a 486-byte atomic SNAPSHOT payload.
  Session multi;
  multi.connect(0x11223344, 0xa1b2c3d4);
  multi.control(hello, true, 0, out);
  assert(out.payload()[16 + 8] == MaxSubscriptions);
  auto makeMulti = [&](unsigned n) {
    std::vector<uint8_t> b(8 + 24 * n, 0);
    w32(b.data(), 1);
    b[4] = static_cast<uint8_t>(n);
    for (unsigned i = 0; i < n; ++i) {
      uint8_t *e = b.data() + 8 + 24 * i;
      std::copy(profile.payload() + 20, profile.payload() + 44, e);
      w16(e, static_cast<uint16_t>(i + 1));
    }
    return b;
  };
  auto nine = makeMulti(9); // 16 + 9*(44+4) = 448 <= 486
  multi.control(request(2, 0, 2, nine.data(), nine.size()), true, 100, out);
  assert(out.payload()[5] == Accept && multi.revision() == 1);
  assert(multi.reservations(100) == 9);
  assert(multi.produce(100, out) && out.bytes[4] == 0x21); // empty initial
  assert(multi.produce(101, out) && out.bytes[4] == 0x20 &&
         out.payload()[0] == 9);
  Packet multiData = out;
  for (size_t mtu : {23U, 247U}) {
    Reassembler reassembled;
    Packet decoded;
    uint8_t fragmentBytes[260]{};
    for (size_t off = 0; off < multiData.size;) {
      const size_t used =
          fragment(multiData, 2, 19, off, mtu, fragmentBytes);
      assert(used >= 17 && used <= mtu - 3);
      const auto result =
          reassembled.feed(fragmentBytes, used, 2, 101, decoded);
      off += used - 16;
      assert(result == (off == multiData.size ? Error::None : Error::Pending));
    }
    assert(decoded.size == multiData.size && decoded.bytes == multiData.bytes);
  }
  uint8_t snap9[8]{};
  w32(snap9 + 4, 1);
  multi.control(request(3, 1, 7, snap9, 8), true, 102, out);
  assert(out.payload()[5] == Accept);
  assert(multi.produce(103, out) && out.bytes[4] == 0x21 &&
         out.payload()[12] == 9 && out.size <= MaxPdu);
  Session overflow;
  overflow.connect(0x11223344, 0xa1b2c3d4);
  overflow.control(hello, true, 0, out);
  auto ten = makeMulti(10); // 16 + 10*48 = 496 > 486
  overflow.control(request(2, 0, 2, ten.data(), ten.size()), true, 100, out);
  assert(out.payload()[5] == Reject && overflow.revision() == 0 &&
         !overflow.active(100));
  auto sixteen = makeMulti(16);
  overflow.control(request(3, 0, 2, sixteen.data(), sixteen.size()),
                   true, 101, out);
  assert(out.payload()[5] == Reject && overflow.revision() == 0);
  // No partial commit/previous profile destruction on oversized proposal.
  multi.control(request(4, 1, 2, ten.data(), ten.size()), true, 105, out);
  assert(out.payload()[5] == Reject && multi.revision() == 1 &&
         multi.reservations(105) == 9);

  // Once the 3s lease expires, SNAPSHOT_REQ must NOT recycle a VALID cache.
  Session expired;
  expired.connect(0x11223344, 0xa1b2c3d4);
  expired.control(hello, true, 0, out);
  expired.control(profile, true, 100, out);
  assert(expired.produce(100, out) && out.bytes[4] == 0x21);
  assert(expired.produce(101, out) && out.bytes[4] == 0x20);
  const uint32_t storedSeq = u32(out.payload() + 2 + 38);
  const uint32_t storedTime = u32(out.payload() + 2 + 26);
  expired.control(request(3, 1, 7, snap9, 8), true, 3100, out);
  assert(out.payload()[5] == TxLocked && expired.revision() == 1);
  assert(expired.produce(3101, out) && out.bytes[4] == 0x22);
  assert(!expired.active(3101) && expired.reservations(3101) == 0);
  expired.control(request(4, 1, 7, snap9, 8), true, 3102, out);
  assert(out.payload()[5] == TxLocked);
  assert(expired.produce(4102, out) && out.bytes[4] == 0x22);
  assert(storedSeq != 0 && storedTime != 0);

  // Per-Central leases are independent even when both demand the same PID.
  Session peerA, peerB;
  peerA.connect(0x11223344, 0xa1b2c3d4);
  peerB.connect(0x11223344, 0xa1b2c3d5);
  peerA.control(hello, true, 0, out);
  peerB.control(hello, true, 0, out);
  peerA.control(profile, true, 0, out);
  auto forB = encode(1, 0x11223344, 0xa1b2c3d5, 2,
                     profile.payload(), profile.size - Header);
  peerB.control(forB, true, 0, out);
  assert(out.payload()[5] == Accept);
  auto renewB = request(3, 1, 4);
  renewB = encode(1, 0x11223344, 0xa1b2c3d5, 3, renewB.payload(),
                  renewB.size - Header);
  peerB.control(renewB, true, 2500, out);
  assert(out.payload()[5] == Accept);
  assert(!peerA.active(3000) && peerB.active(3000));
  Simulator::Demand onlyB{};
  peerA.demand(3000, onlyB);
  peerB.demand(3000, onlyB);
  Simulator sharedSource;
  sharedSource.tick(3000, onlyB);
  assert(sharedSource.reservations() == 1 &&
         sharedSource.acquisitions() == 1);

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
