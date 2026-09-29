#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
for source in "$ROOT"/src/*.cpp; do
  echo "syntax: $source"
  g++ -std=c++17 -Wall -Wextra -Werror     -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src"     -fsyntax-only "$source"
done
echo "PASS all Phase 1 firmware translation units"
