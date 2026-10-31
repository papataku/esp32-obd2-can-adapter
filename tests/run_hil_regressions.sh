#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
 -DM5CAN_SIGNAL_V2_SERIAL_HIL=1 -DM5CAN_HIL_HOST_TEST=1 -DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1 \
 -DM5CAN_ECU_SELECTION_EXPERIMENTAL=1 -DM5CAN_SIGNAL_V2_RP8_PROFILE=1 \
 -Itests/hil/stubs -Itests/host_stubs -Iinclude -Isrc \
 tests/hil/driver.cpp src/twai_hil.cpp src/can_monitor.cpp \
 src/diagnostic_policy.cpp src/diagnostic_transaction.cpp src/isotp_reassembler.cpp \
 -o "$build/driver"
for test in drain model race transport; do "$build/driver" "$test"; done
python3 tests/test_hil_review.py
