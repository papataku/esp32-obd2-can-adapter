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
PYTHONPATH=tools python3 tools/test_protocol.py
PYTHONPATH=tools python3 tests/test_validate_capture.py
# Offline ISO-TP/CAN/TX-OFF acceptance checks. All fixtures are SYNTHETIC.
PYTHONPATH=tools python3 tests/test_multi_ecu_bench_trace.py
# Cross-validate independent analyzer bytes and SHA-256 against trace events.
PYTHONPATH=tools python3 tests/test_reconcile_multi_ecu_can_capture.py
python3 -m compileall -q tools tests
