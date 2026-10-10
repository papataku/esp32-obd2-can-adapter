#pragma once

#include <cstddef>

#include "diagnostic_policy.h"

namespace m5can {

class ElmResponseFormatter {
 public:
  static size_t formatRawFrame(const DiagnosticRawFrame& frame,
                               bool headers, bool spaces,
                               char* output, size_t capacity);
};

}  // namespace m5can
