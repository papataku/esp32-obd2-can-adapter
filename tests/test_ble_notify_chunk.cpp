#include <cassert>
#include <cstddef>
#include "../include/ble_notify_chunk.h"

int main() {
  assert(ble_notify_chunk_size(0) == 20);
  assert(ble_notify_chunk_size(23) == 20);
  assert(ble_notify_chunk_size(64) == 61);
  assert(ble_notify_chunk_size(185) == 182);
  assert(ble_notify_chunk_size(247) == 244);
  assert(ble_notify_chunk_size(517) == 244);
  // 512-byte batch is 26 notifications on the legacy link and 3 at MTU 247.
  const auto chunks = [](size_t n, size_t chunk) { return (n + chunk - 1) / chunk; };
  assert(chunks(512, ble_notify_chunk_size(23)) == 26);
  assert(chunks(512, ble_notify_chunk_size(247)) == 3);
}
