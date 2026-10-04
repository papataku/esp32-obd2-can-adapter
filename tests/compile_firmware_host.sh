#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
for source in "$ROOT"/src/*.cpp; do
  echo "syntax: $source"
  g++ -std=c++17 -Wall -Wextra -Werror     -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src"     -fsyntax-only "$source"
done

g++ -std=c++17 -Wall -Wextra -Werror   -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src"   "$ROOT/tests/test_native_protocol.cpp"   "$ROOT/src/native_protocol.cpp"   "$ROOT/tests/host_stubs/stubs.cpp"   -o /tmp/m5can-native-test
/tmp/m5can-native-test

g++ -std=c++17 -Wall -Wextra -Werror   -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src"   "$ROOT/tests/test_elm_compat.cpp"   "$ROOT/src/elm_compat.cpp"   "$ROOT/tests/host_stubs/stubs.cpp"   -o /tmp/m5can-elm-test
/tmp/m5can-elm-test

g++ -std=c++17 -Wall -Wextra -Werror   -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src"   "$ROOT/tests/test_diagnostic_policy.cpp"   "$ROOT/src/diagnostic_policy.cpp"   "$ROOT/tests/host_stubs/stubs.cpp"   -o /tmp/m5can-policy-test
/tmp/m5can-policy-test

echo "PASS all Phase 3B firmware translation units"
