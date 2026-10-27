#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
./tests/compile_firmware_host.sh
bash ./tests/run_sanitized_coordinator.sh
python3 tests/static_safety_check.py
python3 tests/test_capture_parser.py
python3 tests/test_tri_device_soak.py
python3 tests/test_two_meter_analysis.py
python3 tests/test_evidence_inspector.py
python3 tests/test_recover_raw_can_log.py
PYTHONPATH=tools python3 tools/test_protocol.py
PYTHONPATH=tools python3 tests/test_validate_capture.py
python3 -m compileall -q tools tests

python3 tests/test_signal_v2_wire.py
python3 tests/test_signal_v2_session.py
python3 tests/test_signal_v2_review_policy.py

c++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_CAN_TX_HARD_DISABLED=1 \
  -Itests/host_stubs -Iinclude -Isrc tests/test_signal_v2_can_lock.cpp \
  src/can_monitor.cpp src/diagnostic_policy.cpp src/diagnostic_transaction.cpp \
  src/isotp_reassembler.cpp tests/host_stubs/stubs.cpp -o /tmp/m5can-signal-v2-can-lock
/tmp/m5can-signal-v2-can-lock
