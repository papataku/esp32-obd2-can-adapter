#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1 -DM5CAN_SIGNAL_V2_RP8_PROFILE=1 -Iinclude -Isrc)
c++ "${flags[@]}" -fsanitize=address,undefined tests/p3/binding.cpp src/signal_v2_codec.cpp src/signal_v2_session.cpp -o "$build/host"
"$build/host"
c++ "${flags[@]}" -pthread -DM5CAN_SIGNAL_V2_SERIAL_HIL=1 -DM5CAN_HIL_HOST_TEST=1 \
  -DM5CAN_ECU_SELECTION_EXPERIMENTAL=1 -DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1 \
  -Itests/hil/stubs -Itests/host_stubs tests/p3/binding.cpp \
  src/signal_v2_codec.cpp src/signal_v2_session.cpp src/twai_hil.cpp src/can_monitor.cpp \
  src/diagnostic_policy.cpp src/diagnostic_transaction.cpp src/isotp_reassembler.cpp -o "$build/serial"
"$build/serial"
