#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CXX="${CXX:-g++}"
CXXFLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Werror
  -fsanitize=address,undefined -fno-omit-frame-pointer
  -DM5CAN_DUAL_BLE_EXPERIMENTAL=1
  -DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src")
# The NimBLE host stand-in creates globally retained test servers; leakage
# from stub lifetime is not evidence of firmware heap ownership. Everything
# else (buffer accesses, invalid references, arithmetic UB) remains enabled.
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT/tests/test_diagnostic_pid_cache.cpp" \
  "$ROOT/src/diagnostic_policy.cpp" "$ROOT/tests/host_stubs/stubs.cpp" \
  -o /tmp/m5can-pid-cache-sanitized
/tmp/m5can-pid-cache-sanitized

"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT/tests/test_diagnostic_request_pacer.cpp" \
  -o /tmp/m5can-pacer-sanitized
/tmp/m5can-pacer-sanitized

"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT/tests/test_dual_ble_adaptive.cpp" \
  "$ROOT/src/dual_ble_elm.cpp" "$ROOT/src/ble_elm_transport.cpp" "$ROOT/src/drive_log.cpp" \
  "$ROOT/src/elm_compat.cpp" "$ROOT/src/elm_response_formatter.cpp" \
  "$ROOT/src/diagnostic_transaction.cpp" "$ROOT/src/isotp_reassembler.cpp" \
  "$ROOT/src/diagnostic_policy.cpp" "$ROOT/tests/host_stubs/stubs.cpp" \
  -o /tmp/m5can-dual-adaptive-sanitized
/tmp/m5can-dual-adaptive-sanitized
echo "PASS ASan+UBSan PID cache, pacing, and BLE/CAN coordinator simulation"

# Opt-in Issue54 coordinator end-to-end through real session dispatch,
# physical request conversion and CAN-owner provenance policy (host mock).
"$CXX" "${CXXFLAGS[@]}" -DM5CAN_ECU_SELECTION_EXPERIMENTAL=1 \
  "$ROOT/tests/test_dual_ble_adaptive.cpp" \
  "$ROOT/src/dual_ble_elm.cpp" "$ROOT/src/ble_elm_transport.cpp" "$ROOT/src/drive_log.cpp" \
  "$ROOT/src/elm_compat.cpp" "$ROOT/src/elm_response_formatter.cpp" \
  "$ROOT/src/diagnostic_transaction.cpp" "$ROOT/src/isotp_reassembler.cpp" \
  "$ROOT/src/diagnostic_policy.cpp" "$ROOT/tests/host_stubs/stubs.cpp" \
  -o /tmp/m5can-issue54-dual-coordinator-sanitized
/tmp/m5can-issue54-dual-coordinator-sanitized
