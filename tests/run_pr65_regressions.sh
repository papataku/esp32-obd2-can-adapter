#!/usr/bin/env bash
set -eo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1 -DM5CAN_ECU_SELECTION_EXPERIMENTAL=1 -DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1 -Itests/pr65/stubs -Itests/host_stubs -Iinclude -Isrc)
sources=(src/diagnostic_policy.cpp src/diagnostic_transaction.cpp src/isotp_reassembler.cpp)
# Before-fix audit can run all findings separately, without masking later failures.
failed=0
c++ "${flags[@]}" -DM5CAN_SIGNAL_V2_RP8_PROFILE=1 tests/pr65/monitor.cpp src/can_monitor.cpp "${sources[@]}" tests/host_stubs/stubs.cpp -o "$build/monitor"
for finding in M1 M2 M3 M6 M7 m1 controls R1 R2 R3; do
  "$build/monitor" "$finding" || failed=1
done
for profile in empty rp8 rp8-10hz; do
  extra=()
  if [[ "$profile" != empty ]]; then extra+=(-DM5CAN_SIGNAL_V2_RP8_PROFILE=1); fi
  if [[ "$profile" == rp8-10hz ]]; then extra+=(-DM5CAN_SIGNAL_V2_10HZ_LAB=1); fi
  c++ "${flags[@]}" "${extra[@]}" tests/pr65/owner.cpp "${sources[@]}" -o "$build/owner"
  if [[ "$profile" != empty ]]; then
    c++ "${flags[@]}" "${extra[@]}" tests/pr65/owner-cadence.cpp "${sources[@]}" -o "$build/owner-cadence"
    "$build/owner-cadence" || failed=1
  fi
  for finding in M4 M5 M6 M7 controls; do
    "$build/owner" "$finding" || failed=1
  done
done
for mode in default v1-adaptive v2-empty v2-rp8; do
  extra=()
  case "$mode" in
    v1-adaptive) extra=(-DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1 -DM5CAN_ECU_SELECTION_EXPERIMENTAL=1);;
    v2-empty) extra=(-DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1);;
    v2-rp8) extra=(-DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1 -DM5CAN_SIGNAL_V2_RP8_PROFILE=1);;
  esac
  c++ -std=c++17 -Wall -Wextra -Werror "${extra[@]}" -Itests/host_stubs -Iinclude -Isrc tests/pr65/policy.cpp src/diagnostic_policy.cpp tests/host_stubs/stubs.cpp -o "$build/policy"
  "$build/policy" || failed=1
done
for std in c++11 c++17; do
  extra=()
  if [[ -f src/signal_v2_discovery_profile.cpp ]]; then extra+=(src/signal_v2_discovery_profile.cpp); fi
  if c++ -std="$std" -O0 -DM5CAN_SIGNAL_V2_RP8_PROFILE=1 -Iinclude tests/pr65/profile-odr.cpp tests/pr65/profile-reader.cpp "${extra[@]}" -o "$build/profile"; then
    "$build/profile" || failed=1
  else failed=1; fi
done
exit "$failed"
