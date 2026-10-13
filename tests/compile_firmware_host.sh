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

g++ -std=c++17 -Wall -Wextra -Werror   -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src"   "$ROOT/tests/test_isotp_reassembler.cpp"   "$ROOT/src/isotp_reassembler.cpp"   "$ROOT/src/diagnostic_policy.cpp"   "$ROOT/tests/host_stubs/stubs.cpp"   -o /tmp/m5can-isotp-test
/tmp/m5can-isotp-test

g++ -std=c++17 -Wall -Wextra -Werror   -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src"   "$ROOT/tests/test_diagnostic_transaction.cpp"   "$ROOT/src/diagnostic_transaction.cpp"   "$ROOT/src/isotp_reassembler.cpp"   "$ROOT/src/diagnostic_policy.cpp"   "$ROOT/tests/host_stubs/stubs.cpp"   -o /tmp/m5can-transaction-test
/tmp/m5can-transaction-test

g++ -std=c++17 -Wall -Wextra -Werror   -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src"   "$ROOT/tests/test_elm_response_formatter.cpp"   "$ROOT/src/elm_response_formatter.cpp"   "$ROOT/tests/host_stubs/stubs.cpp"   -o /tmp/m5can-formatter-test
/tmp/m5can-formatter-test

g++ -std=c++17 -Wall -Wextra -Werror   -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src"   "$ROOT/tests/test_kw905_golden_multiframe.cpp"   "$ROOT/src/diagnostic_transaction.cpp"   "$ROOT/src/isotp_reassembler.cpp"   "$ROOT/src/diagnostic_policy.cpp"   "$ROOT/src/elm_response_formatter.cpp"   "$ROOT/tests/host_stubs/stubs.cpp"   -o /tmp/m5can-golden-test
/tmp/m5can-golden-test

g++ -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_diagnostic_request_pacer.cpp" \
  -o /tmp/m5can-request-pacer-test
/tmp/m5can-request-pacer-test

g++ -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_demo_drive.cpp" \
  -o /tmp/m5can-demo-drive-test
/tmp/m5can-demo-drive-test

g++ -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/src" \
  "$ROOT/tests/test_demo_touch_control.cpp" \
  -o /tmp/m5can-demo-touch-test
/tmp/m5can-demo-touch-test

for source in "$ROOT"/src/*.cpp; do
  echo "dual BLE syntax: $source"
  g++ -std=c++17 -Wall -Wextra -Werror \
    -DM5CAN_DUAL_BLE_EXPERIMENTAL=1 \
    -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
    -fsyntax-only "$source"
done

g++ -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/src" \
  "$ROOT/tests/test_diagnostic_result_cache.cpp" \
  "$ROOT/src/diagnostic_policy.cpp" \
  -o /tmp/m5can-diagnostic-cache-test
/tmp/m5can-diagnostic-cache-test

g++ -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_diagnostic_pid_cache.cpp" \
  "$ROOT/src/diagnostic_policy.cpp" "$ROOT/tests/host_stubs/stubs.cpp" \
  -o /tmp/m5can-pid-cache-test
/tmp/m5can-pid-cache-test

g++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1 \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_diagnostic_batch_planner.cpp" \
  "$ROOT/src/diagnostic_policy.cpp" "$ROOT/tests/host_stubs/stubs.cpp" \
  -o /tmp/m5can-batch-planner-test
/tmp/m5can-batch-planner-test

g++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_DUAL_BLE_EXPERIMENTAL=1 -DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1 \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_adaptive_multi_pid.cpp" \
  "$ROOT/src/diagnostic_transaction.cpp" "$ROOT/src/isotp_reassembler.cpp" \
  "$ROOT/src/diagnostic_policy.cpp" "$ROOT/tests/host_stubs/stubs.cpp" \
  -o /tmp/m5can-adaptive-multipid-test
/tmp/m5can-adaptive-multipid-test

g++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_DUAL_BLE_EXPERIMENTAL=1 -DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1 \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_adaptive_multipid_pipeline.cpp" \
  "$ROOT/src/elm_compat.cpp" "$ROOT/src/elm_response_formatter.cpp" \
  "$ROOT/src/diagnostic_transaction.cpp" "$ROOT/src/isotp_reassembler.cpp" \
  "$ROOT/src/diagnostic_policy.cpp" "$ROOT/tests/host_stubs/stubs.cpp" \
  -o /tmp/m5can-adaptive-multipid-pipeline-test
/tmp/m5can-adaptive-multipid-pipeline-test

g++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_DUAL_BLE_EXPERIMENTAL=1 -DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1 \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_diagnostic_policy.cpp" \
  "$ROOT/src/diagnostic_policy.cpp" "$ROOT/tests/host_stubs/stubs.cpp" \
  -o /tmp/m5can-dual-policy-test
/tmp/m5can-dual-policy-test

echo "PASS all Phase 3C firmware translation units + demo drive + screen toggle"

g++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_DUAL_BLE_EXPERIMENTAL=1 \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  -DM5CAN_SYNC_NOTIFY_DIAGNOSTIC=1 \
  "$ROOT/tests/test_dual_ble_transport.cpp" "$ROOT/src/ble_elm_transport.cpp" \
  "$ROOT/tests/host_stubs/stubs.cpp" -o /tmp/m5can-dual-transport-test
/tmp/m5can-dual-transport-test
g++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_DUAL_BLE_EXPERIMENTAL=1 \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_ble_evidence_journal.cpp" "$ROOT/src/ble_evidence_journal.cpp" \
  "$ROOT/tests/host_stubs/stubs.cpp" -o /tmp/m5can-ble-evidence-journal-test
/tmp/m5can-ble-evidence-journal-test
g++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_DUAL_BLE_EXPERIMENTAL=1 \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_log_storage_probe.cpp" "$ROOT/src/log_storage_probe.cpp" \
  "$ROOT/tests/host_stubs/stubs.cpp" -o /tmp/m5can-logcap-test
/tmp/m5can-logcap-test
echo "PASS read-only LOGCAP sizing with legacy NVS settings unchanged"

echo "PASS BLE persisted disconnect/CRC/reboot/overflow and CAN-idle checkpoint"

echo "PASS dual BLE targeted chunks and disconnect isolation"

g++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_DUAL_BLE_EXPERIMENTAL=1 \
  -DM5CAN_DUAL_NOTIFY_CHUNK_BYTES=244 \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  -DM5CAN_SYNC_NOTIFY_DIAGNOSTIC=1 \
  "$ROOT/tests/test_dual_ble_transport.cpp" "$ROOT/src/ble_elm_transport.cpp" \
  "$ROOT/tests/host_stubs/stubs.cpp" -o /tmp/m5can-dual-mtu-transport-test
/tmp/m5can-dual-mtu-transport-test
echo "PASS opt-in 244-byte unicast and exact retry payload ordering"

g++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_DUAL_BLE_EXPERIMENTAL=1 -DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1 \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_dual_ble_adaptive.cpp" "$ROOT/src/dual_ble_elm.cpp" \
  "$ROOT/src/ble_elm_transport.cpp" "$ROOT/src/drive_log.cpp" "$ROOT/src/elm_compat.cpp" \
  "$ROOT/src/elm_response_formatter.cpp" "$ROOT/src/diagnostic_transaction.cpp" \
  "$ROOT/src/isotp_reassembler.cpp" "$ROOT/src/diagnostic_policy.cpp" \
  "$ROOT/tests/host_stubs/stubs.cpp" -o /tmp/m5can-dual-adaptive-test
/tmp/m5can-dual-adaptive-test

g++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_DUAL_BLE_EXPERIMENTAL=1 \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_dual_ble_adaptive.cpp" "$ROOT/src/dual_ble_elm.cpp" \
  "$ROOT/src/ble_elm_transport.cpp" "$ROOT/src/drive_log.cpp" "$ROOT/src/elm_compat.cpp" \
  "$ROOT/src/elm_response_formatter.cpp" "$ROOT/src/diagnostic_transaction.cpp" \
  "$ROOT/src/isotp_reassembler.cpp" "$ROOT/src/diagnostic_policy.cpp" \
  "$ROOT/tests/host_stubs/stubs.cpp" -o /tmp/m5can-dual-legacy-test
/tmp/m5can-dual-legacy-test

g++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_DUAL_BLE_EXPERIMENTAL=1 -DM5CAN_CAN_MODE_DRY_RUN=1 -DM5CAN_DEMO_FAULT_FIXTURE=1 \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_dual_ble_adaptive.cpp" "$ROOT/src/dual_ble_elm.cpp" \
  "$ROOT/src/ble_elm_transport.cpp" "$ROOT/src/drive_log.cpp" "$ROOT/src/elm_compat.cpp" \
  "$ROOT/src/elm_response_formatter.cpp" "$ROOT/src/diagnostic_transaction.cpp" \
  "$ROOT/src/isotp_reassembler.cpp" "$ROOT/src/diagnostic_policy.cpp" \
  "$ROOT/tests/host_stubs/stubs.cpp" -o /tmp/m5can-dual-fault-test
/tmp/m5can-dual-fault-test

# Real flash backend tests; existing Preferences tests retain legacy ABI coverage.
g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$ROOT/src" "$ROOT/tests/test_evidence_store.cpp" "$ROOT/src/evidence_store.cpp" \
  -o /tmp/m5can-evidence-store-test
/tmp/m5can-evidence-store-test
g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DM5CAN_DUAL_BLE_EXPERIMENTAL=1 -DM5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1 \
  -DM5CAN_EVIDENCE_FLASH_TEST=1 \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_combined_evidence.cpp" "$ROOT/src/drive_log.cpp" \
  "$ROOT/src/ble_evidence_journal.cpp" "$ROOT/src/evidence_store.cpp" \
  "$ROOT/src/ble_elm_transport.cpp" "$ROOT/tests/host_stubs/stubs.cpp" \
  -o /tmp/m5can-combined-evidence-test
/tmp/m5can-combined-evidence-test
g++ -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/include" \
  "$ROOT/tests/test_ble_notify_chunk.cpp" \
  -o /tmp/m5can-ble-notify-chunk-test
/tmp/m5can-ble-notify-chunk-test

echo "PASS all Phase 3C firmware translation units + demo drive + screen toggle + BLE notify chunk"


g++ -std=c++17 -Wall -Wextra -Werror -DM5CAN_DUAL_BLE_EXPERIMENTAL=1 \
  -I"$ROOT/tests/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/test_ble_tx_flow.cpp" "$ROOT/src/ble_elm_transport.cpp" \
  "$ROOT/tests/host_stubs/stubs.cpp" -o /tmp/m5can-tx-flow-test
/tmp/m5can-tx-flow-test
echo "PASS bounded BLE output, fairness, pool reserve, exact recovery, generation, overflow and stall"
