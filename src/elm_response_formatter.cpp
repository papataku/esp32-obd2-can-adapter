#include "elm_response_formatter.h"

#include <algorithm>
#include <cstdio>

namespace m5can {

size_t ElmResponseFormatter::formatRawFrame(const DiagnosticRawFrame& frame,
                                            bool headers, bool spaces,
                                            char* output, size_t capacity) {
  if (!output || capacity == 0 || frame.dlc > 8) return 0;
  output[0] = '\0';
  size_t pos = 0;

  auto append = [&](const char* text, size_t n) -> bool {
    if (pos + n + 1 > capacity) return false;
    for (size_t i = 0; i < n; ++i) output[pos++] = text[i];
    output[pos] = '\0';
    return true;
  };

  if (headers) {
    char id[9]{};
    std::snprintf(id, sizeof(id), "%08lX",
                  static_cast<unsigned long>(frame.can_id));
    if (!append(id, 8)) return 0;
    if (spaces && frame.dlc && !append(" ", 1)) return 0;
  }

  uint8_t display_dlc = frame.dlc;
  if (frame.dlc && (frame.data[0] >> 4) == 0x0) {
    const uint8_t sf_len = frame.data[0] & 0x0F;
    if (sf_len <= 7) {
      display_dlc =
          std::min<uint8_t>(frame.dlc, static_cast<uint8_t>(sf_len + 1));
    }
  }

  static constexpr char kHex[] = "0123456789ABCDEF";
  for (uint8_t i = 0; i < display_dlc; ++i) {
    const char byte[2] = {
        kHex[(frame.data[i] >> 4) & 0x0F],
        kHex[frame.data[i] & 0x0F],
    };
    if (!append(byte, 2)) return 0;
    if (spaces && i + 1 < display_dlc && !append(" ", 1)) return 0;
  }
  return pos;
}

}  // namespace m5can
