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

c++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  src/signal_v2_codec.cpp src/signal_v2_session.cpp \
  tests/test_signal_v2_demo.cpp -o /tmp/m5can-signal-v2-demo5
/tmp/m5can-signal-v2-demo5

c++ -std=c++17 -Wall -Wextra -Werror -Iinclude -Isrc \
  tests/test_signal_v2_can_policy.cpp -o /tmp/m5can-signal-v2-can-policy
/tmp/m5can-signal-v2-can-policy

# Review-only V2-2 host requirements: 10Hz union, snapshot, split design,
# explicit CAN provenance and strict V1/11bit/UDS rejection.
c++ -std=c++17 -Wall -Wextra -Werror \
  -DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1 -DM5CAN_SIGNAL_V2_10HZ_LAB=1 \
  -Iinclude -Isrc src/signal_v2_codec.cpp src/signal_v2_session.cpp \
  tests/test_signal_v2_can_session.cpp -o /tmp/m5can-v2-can-session
/tmp/m5can-v2-can-session

c++ -std=c++17 -Wall -Wextra -Werror \
  -DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1 -DM5CAN_ECU_SELECTION_EXPERIMENTAL=1 \
  -Itests/host_stubs -Iinclude -Isrc \
  tests/test_signal_v2_can_policy_gate.cpp src/diagnostic_policy.cpp \
  tests/host_stubs/stubs.cpp -o /tmp/m5can-v2-can-policy-gate
/tmp/m5can-v2-can-policy-gate

c++ -std=c++17 -Wall -Wextra -Werror \
  -DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1 \
  -DM5CAN_ECU_SELECTION_EXPERIMENTAL=1 -DM5CAN_SIGNAL_V2_RP8_PROFILE=1 \
  -Itests/host_stubs -Iinclude -Isrc \
  tests/test_signal_v2_can_isotp.cpp src/diagnostic_policy.cpp \
  src/isotp_reassembler.cpp src/diagnostic_transaction.cpp \
  tests/host_stubs/stubs.cpp -o /tmp/m5can-v2-can-isotp
/tmp/m5can-v2-can-isotp

python3 tests/test_signal_v2_scan_advertisement_policy.py
python3 tests/test_signal_v2_hil_ecu.py

# Independent PR63 review reproductions: bounded FC/RAW/PDU and arm epochs.
for fixture in fc owner; do
  c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1 -DM5CAN_ECU_SELECTION_EXPERIMENTAL=1 \
    -Itests/host_stubs -Iinclude -Isrc \
    $([ "$fixture" = fc ] && echo -DM5CAN_SIGNAL_V2_RP8_PROFILE=1) \
    tests/test_signal_v2_review_${fixture}.cpp src/diagnostic_policy.cpp \
    src/diagnostic_transaction.cpp src/isotp_reassembler.cpp \
    $([ "$fixture" = fc ] && echo tests/host_stubs/stubs.cpp) \
    -o /tmp/m5can-v22-review-${fixture}
  /tmp/m5can-v22-review-${fixture}
done
# N1 PR-B: approved-candidate physical discovery, no functional broadcast.
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1 -DM5CAN_ECU_SELECTION_EXPERIMENTAL=1 \
  -DM5CAN_SIGNAL_V2_RP8_PROFILE=1 \
  -Itests/host_stubs -Iinclude -Isrc \
  tests/test_signal_v2_bounded_discovery.cpp src/diagnostic_policy.cpp \
  src/diagnostic_transaction.cpp src/isotp_reassembler.cpp \
  -o /tmp/m5can-v22-bounded-discovery
/tmp/m5can-v22-bounded-discovery
c++ -std=c++17 -Wall -Wextra -Werror \
  -DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1 -DM5CAN_SIGNAL_V2_RP8_PROFILE=1 \
  -Itests/host_stubs -Iinclude -Isrc \
  tests/test_signal_v2_stage_s1.cpp src/diagnostic_policy.cpp \
  src/diagnostic_transaction.cpp src/isotp_reassembler.cpp \
  -o /tmp/m5can-v22-stage-s1
/tmp/m5can-v22-stage-s1
c++ -std=c++17 -Wall -Wextra -Werror \
  -DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1 -DM5CAN_SIGNAL_V2_RP8_PROFILE=1 \
  -DM5CAN_SIGNAL_V2_STAGE_GATED=1 \
  -Itests/host_stubs -Iinclude -Isrc \
  tests/test_signal_v2_stage_s1.cpp src/diagnostic_policy.cpp \
  src/diagnostic_transaction.cpp src/isotp_reassembler.cpp \
  -o /tmp/m5can-v22-stage-s1-gated
/tmp/m5can-v22-stage-s1-gated
c++ -std=c++11 -Wall -Wextra -Wno-unused-function -Werror -pthread \
  -DM5CAN_SIGNAL_V2_CAN_EXPERIMENTAL=1 -DM5CAN_SIGNAL_V2_RP8_PROFILE=1 \
  -DM5CAN_ECU_SELECTION_EXPERIMENTAL=1 -DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1 \
  -DM5CAN_TEST_S1_STOP_RACE=1 -DM5CAN_TEST_S1_BLIND_SPAN=1 \
  -DM5CAN_TEST_S1_OWNER_BARRIER=1 \
  -DM5CAN_SIGNAL_V2_STAGE_GATED=1 \
  -Itests/pr65/stubs -Itests/host_stubs -Iinclude -Isrc \
  -include tests/s1_rx_interleaving_hook.h \
  tests/test_signal_v2_stage_s1_monitor.cpp src/can_monitor.cpp src/diagnostic_policy.cpp \
  src/diagnostic_transaction.cpp src/isotp_reassembler.cpp \
  src/signal_v2_can_samples.cpp src/signal_v2_discovery_profile.cpp \
  tests/host_stubs/stubs.cpp \
  -o /tmp/m5can-v22-stage-s1-monitor
/tmp/m5can-v22-stage-s1-monitor
python3 tests/test_signal_v2_elf_policy.py
python3 tests/test_signal_v2_b01_policy.py
python3 tests/test_signal_v2_arm_disconnect.py
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Itests/host_stubs -Iinclude -Isrc tests/test_diagnostic_review_capacity.cpp \
  src/diagnostic_policy.cpp src/diagnostic_transaction.cpp src/isotp_reassembler.cpp \
  tests/host_stubs/stubs.cpp -o /tmp/m5can-review-capacity
/tmp/m5can-review-capacity

# Independent PR65 safety review: production monitor + owner, policy and C++11 ODR.
bash tests/run_pr65_regressions.sh

# PR66 stopped drain, lifecycle/admission and complete board-timed oracle.
bash tests/run_hil_regressions.sh
python3 tests/test_hil_evidence.py
python3 tests/test_hil_runner.py
python3 tests/test_hil_oracle_r66c.py

# Dedicated physical-TX-isolated receiver safety policy.
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude tests/test_passive_policy.cpp -o /tmp/m5can-passive-policy
/tmp/m5can-passive-policy
python3 tests/test_passive_profile.py
./tests/run_passive_runtime.sh
python3 tests/test_passive_flash_check.py

python3 tests/test_owner_live_usb.py

python3 tests/test_signal_v2_auto_arm.py
