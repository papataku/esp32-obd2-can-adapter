#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
c++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_PASSIVE_TX_ISOLATED=1 \
  -Itests/passive_stubs -Iinclude -include include/passive_build_guard.h \
  tests/test_passive_runtime.cpp -o /tmp/m5can-passive-runtime
for fault in display display-clobber display-clobber-stuck display-low display-route install start task before-pad before-rtc before-sleep before-mux before-input stopped-rx start-nolom status-boot output pad enable inversion oen mux input sleep rtc open_drain rx mode self_test state reset status late; do
  /tmp/m5can-passive-runtime "$fault"
done
for pin in $(seq 0 48); do /tmp/m5can-passive-runtime "route$pin"; done
echo 'PASS actual passive runtime: boot ordering, ARM/lease rejection, all 49 TX routes and fault teardown'
