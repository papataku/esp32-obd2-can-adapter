#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
./tests/compile_firmware_host.sh
python3 tests/static_safety_check.py
python3 tests/test_capture_parser.py
PYTHONPATH=tools python3 tools/test_protocol.py
PYTHONPATH=tools python3 tests/test_validate_capture.py
python3 -m compileall -q tools tests
