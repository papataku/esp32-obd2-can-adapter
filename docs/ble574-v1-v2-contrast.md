# BLE 574 forensic contrast — read-only V1 failed vs V2 succeeded

The observed evidence is **one** V1 failure and **one** V2 success, not a
probabilistic retry study. The existing successful V2 integrity audit is done;
do not repeat it. No flash, reset, scan, reconnect, NVS, CAN-log or other
device action is needed for this analysis.

## Fixed observations

| Layer | Failed Ring 632 V1 | Succeeded Ring 8ea V2 |
| --- | --- | --- |
| GAP connect API return | 0 | 0 |
| GAP CONNECT callback status | 0 | 0 |
| Final NimBLEClient connect return | failed, 574 | success, 0 |
| Complete application connection | no | yes |
| GATT MTU API / event | **not instrumented by V1** | rc0, MTU247 |
| Early GAP DISCONNECT reason | **not instrumented by V1** | none recorded for success |
| M5 app slot | stayed clients=0 | transitioned clients=0 -> clients=1 |
| M5 GAP observer | **not installed in failed trial** | con=1, ok=1, err=0, disc=0 |

The HCI host namespace is +0x200, so 574 (hex 0x23e) maps to controller error
0x3e, BLE_ERR_CONN_ESTABLISHMENT. Reference:
https://mynewt.incubator.apache.org/latest/network/ble_hs/ble_hs_return_codes.html
In NimBLE-Arduino 1.4.3, the client receives the GAP CONNECT event, attempts
MTU exchange, and can receive GAP DISCONNECT before app onConnect. A V1 final
error of 574 **does not** prove exactly where in that interval a disconnect
occurred. V1 lacks the relevant event probes; V2's success does not fill them.

## Hash-pinned, private offline contrast (optional)

Run the comparator only against the existing evidence captured under the
approved bench. The tool refuses any source hash mismatch before writing
output. No raw payloads, peer identifiers, timestamps from different
devices or private BLE allowlist values are included in the report.

    python3 tools/compare_ble574_v1_v2.py \
      --failure-ring /private/Ring-trace632-human-approved-once/SmartRing.raw \
      --failure-m5 /private/Ring-trace632-human-approved-once/M5Dial.raw \
      --success-ring /private/M5-edge371-Ring8ea-single/SmartRing.raw \
      --success-m5 /private/M5-edge371-Ring8ea-single/M5Dial.raw \
      --expected-failure-ring-sha256 0921b0fb3dba67bf8070203e1210ce416631d8060f82c2bb95a53906b2a9ea55 \
      --expected-failure-m5-sha256 0dbb4378edaaaab5a932a2ebbac6d2384604c1f40e379baa43732cf382e0d21c \
      --expected-success-ring-sha256 c7de78dd9d9ec50b995ef428c7d759f234d608345a6134e5ce2d00f82a0c85fd \
      --expected-success-m5-sha256 fbb5be70d833296e5798e27d2a65232424ed4756852890656a945a59dd4ce59a \
      --output /private/ble574-v1-v2-contrast.json

Classifications:
- V1_574_VS_V2_SUCCESS_CONTRAST_ONLY: strict evidence coherence across
  two versions, **not** a root-cause or endurance pass.
- INCOMPLETE_EVIDENCE: partial lines or missing required log samples.
- INCOMPLETE_OR_NONMATCHING_EVIDENCE: facts differ from fixed case.
- STOP_ANOMALY: duplicated traces, unexpected peer count or regression.

Successful captured V2 boot-wide seq=2 **is not a second user connect**.
The separate host-command audit found only b=1 and |=1. It is not possible
to infer command count from the Ring diagnostic sequence by itself.

## Remaining unknowns and next engineering gate

- Exact HCI controller timeline, RF interference and link-layer timing
  have **not** been captured; neither phone-style signal quality nor UI
  processing time can be declared causal.
- The failed V1 trial has no MTU/disconnect probes, and the M5 firmware
  deployed then had no early GAP connect counter; the missing evidence
  cannot be reconstructed retroactively.
- New diagnostics in Ring V2 and M5 observer 371 are now available. They
  have been validated on a **successful** connection, not on a 574 failure.
- No need to risk the current stable link merely to increase the number of
  successful trials. If a failure is observed during a separately warranted
  authorized bench operation, capture **both** full traces once and stop,
  without retries. A complete Ring V2 DISCONNECT reason matching 0x23e
  would localize the failed interval; even then, that reason does not prove
  the RF root cause.

Working source is this analysis branch only. No product FW source or
partitions are changed, and this branch must not be flashed.
