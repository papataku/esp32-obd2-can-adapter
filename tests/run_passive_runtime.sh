#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
c++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_PASSIVE_TX_ISOLATED=1 \
  -Itests/passive_stubs -Iinclude -include include/passive_build_guard.h \
  tests/test_passive_runtime.cpp -o /tmp/m5can-passive-runtime
for fault in display install start task output pad enable inversion oen mux input sleep rtc open_drain rx mode self_test state reset status late; do
  /tmp/m5can-passive-runtime "$fault"
done
for pin in $(seq 0 48); do /tmp/m5can-passive-runtime "route$pin"; done
echo 'PASS actual passive runtime: boot ordering, ARM/lease rejection, all 49 TX routes and fault teardown'
