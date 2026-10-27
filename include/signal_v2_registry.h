#pragma once
#include "signal_v2_codec.h"
#include <algorithm>
namespace signal_v2 {
// Explicit serialization: no compiler padding or platform-dependent NVS layout.
using RegistryImage = std::array<uint8_t, 256>;
using Identity = std::array<uint8_t, 7>;
using Binding = std::array<uint8_t, 32>;
class Registry {
  RegistryImage image_{};
  static bool valid(const RegistryImage &a) {
    return u32(a.data()) == 0x32524753 && u16(a.data() + 4) == 1 && a[6] == 4 &&
           !a[7] && u32(a.data() + 8) > 0 &&
           u32(a.data() + 252) == crc(a.data(), 252);
  }
  void seal() { w32(image_.data() + 252, crc(image_.data(), 252)); }

public:
  Registry() {
    w32(image_.data(), 0x32524753);
    w16(image_.data() + 4, 1);
    image_[6] = 4;
    w32(image_.data() + 8, 1);
    seal();
  }
  bool load(const RegistryImage &a, const RegistryImage &b) {
    bool av = valid(a), bv = valid(b);
    if (!av && !bv) {
      *this = Registry{};
      return false;
    }
    image_ = av && (!bv || u32(a.data() + 8) >= u32(b.data() + 8)) ? a : b;
    return true;
  }
  const RegistryImage &image() const { return image_; }
  uint32_t generation() const { return u32(image_.data() + 8); }
  bool authorized(const Identity &id, const Binding &binding) const {
    for (unsigned i = 0; i < 4; ++i) {
      auto r = image_.data() + 16 + 56 * i;
      if (r[0] == 1 && r[1] == 3 && std::equal(id.begin(), id.end(), r + 2) &&
          std::equal(binding.begin(), binding.end(), r + 9))
        return true;
    }
    return false;
  }
  bool approve(const Identity &id, const Binding &binding) {
    if (generation() == UINT32_MAX)
      return false;
    uint8_t *selected = nullptr;
    for (unsigned i = 0; i < 4; ++i) {
      auto r = image_.data() + 16 + 56 * i;
      if (std::equal(id.begin(), id.end(), r + 2)) {
        selected = r;
        break;
      }
      if (r[0] != 1 && !selected)
        selected = r;
    }
    if (!selected)
      return false;
    std::fill(selected, selected + 56, 0);
    selected[0] = 1;
    selected[1] = 3;
    std::copy(id.begin(), id.end(), selected + 2);
    std::copy(binding.begin(), binding.end(), selected + 9);
    w32(image_.data() + 8, generation() + 1);
    w32(selected + 41, generation());
    seal();
    return true;
  }
  bool revoke(const Identity &id) {
    if (generation() == UINT32_MAX)
      return false;
    for (unsigned i = 0; i < 4; ++i) {
      auto r = image_.data() + 16 + 56 * i;
      if (r[0] == 1 && std::equal(id.begin(), id.end(), r + 2)) {
        r[0] = 2;
        w32(image_.data() + 8, generation() + 1);
        w32(r + 41, generation());
        seal();
        return true;
      }
    }
    return false;
  }
};
} // namespace signal_v2
