#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

// BLE ATT notifications contain at most peer_mtu-3 application bytes.
// Keep the old 20-byte fallback before/without MTU negotiation. Cap this
// experiment at 247-byte MTU even if a peer supports a larger value.
inline size_t ble_notify_chunk_size(uint16_t negotiated_peer_mtu) {
  if (negotiated_peer_mtu < 23) return 20;
  return std::min<size_t>(244, negotiated_peer_mtu - 3);
}
