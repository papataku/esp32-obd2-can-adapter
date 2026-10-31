#include "signal_v2_codec.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
using namespace signal_v2;
int main(int argc, char **argv) {
  if (argc == 3) {
    std::vector<uint8_t> b;
    std::string s = argv[2];
    for (size_t i = 0; i < s.size(); i += 2)
      b.push_back(std::stoul(s.substr(i, 2), nullptr, 16));
    Packet p;
    auto e = decode(b.data(), b.size(), p);
    if (std::string(argv[1]) == "badcrc") {
      assert(e == Error::Crc);
      return 0;
    }
    assert(e == Error::None);
    auto r = encode(b[4], u32(b.data() + 8), u32(b.data() + 12),
                    u32(b.data() + 16), p.payload(), p.size - Header);
    assert(r.size == b.size() &&
           std::equal(b.begin(), b.end(), r.bytes.begin()));
    for (size_t mtu : {23, 247}) {
      Reassembler a;
      Packet o;
      uint8_t f[260];
      for (size_t off = 0; off < p.size;) {
        auto n = fragment(p, 1, 1, off, mtu, f);
        assert(n <= mtu - 3);
        auto e2 = a.feed(f, n, 1, 100, o);
        off += n - 16;
        assert(e2 == (off == p.size ? Error::None : Error::Pending));
      }
      assert(o.bytes == p.bytes);
    }
    // Negative lengths, reserved bits, CRC and fragment ordering are
    // independent.
    for (size_t i = 0; i < b.size(); ++i) {
      auto bad = b;
      bad[i] ^= 0x80;
      assert(decode(bad.data(), bad.size(), r) != Error::None);
    }
    Reassembler a;
    Packet o;
    uint8_t f[260];
    auto n = fragment(p, 1, 1, 4, 23, f);
    assert(a.feed(f, n, 1, 100, o) == Error::Fragment);
    n = fragment(p, 1, 1, 0, 23, f);
    assert(a.feed(f, n, 1, 100, o) == Error::Pending);
    assert(a.feed(f, n, 1, 101, o) == Error::Fragment);
    return 0;
  }
  return 1;
}
