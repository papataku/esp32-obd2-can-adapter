#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
for profile in empty rp8; do
  flags=(-DM5CAN_P2_HOST_TEST=1)
  if [[ "$profile" == rp8 ]]; then flags=(-DM5CAN_SIGNAL_V2_RP8_PROFILE=1); fi
  c++ -std=c++11 -Wall -Wextra -Werror "${flags[@]}" -Iinclude \
    tests/p2/profile.cpp src/signal_v2_discovery_profile.cpp -o "$build/$profile"
  "$build/$profile"
done
python3 tools/test_p2_profile_equivalence.py
