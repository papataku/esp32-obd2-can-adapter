# Host-only ELM327 conformance and Virtual CAN simulator (Draft)

**NOT hardware or vehicle qualification.** Python code contains no SocketCAN/USB-CAN/TWAI transport and does not write to CAN. The only optional real port is an explicitly selected USB CDC **Virtual-only** interface with a fixed versioned HELLO, tested on a disconnected bench after separate operator acknowledgement.

## Use (Python 3.9 or 3.12; CI matrix)

~~~sh
python3 -m pip install pytest==8.3.5 pyserial==3.5
PYTHONPATH=. python3 -m pytest -q tests/elm327_conformance
python3 -m tools.elm327_sim --model tests/elm327_conformance/fixtures/synthetic_ecus.json --mode elm-pty
~~~

The PTY path is printed to stderr; connect an ELM client to it. No hardware is opened. This emulator intentionally supports the tested CAN 11/29 subset; do **not** call it a complete ELM327 clone.

Same conformance cases against an optional physical target (do not connect a vehicle):

~~~sh
TTM_ISOLATED_BENCH=YES PYTHONPATH=. python3 -m pytest -q tests/elm327_conformance/test_elm.py \
  --elm-target=kw905 --elm-port=/dev/cu.EXPLICIT-ISOLATED-PORT --isolated-bench-ack
~~~

Optional Mac BLE transport (explicit macOS CoreBluetooth UUID or other Bleak identifier; never scan or select automatically):

~~~sh
python3 -m pip install bleak
TTM_ISOLATED_BENCH=YES PYTHONPATH=. python3 -m pytest -q tests/elm327_conformance/test_elm.py \\
  --elm-target=kw905 --elm-transport=ble --elm-ble-id=EXPLICIT_COREBLUETOOTH_ID \\
  --isolated-bench-ack
~~~

The transport profiles are verified against iOS CoreBluetoothDriver.swift: M5CAN NUS RX write / TX notify; KW905 FFF0, FFF2 write-without-response, FFF1 notify. No BLE device IDs are checked into the repository. Physical OBD requests still require a separately approved, **disconnected, isolated ECU test bench**.

Without `--bus-bench-ack`, OBD requests are **NOT_RUN** on external devices. To test those cases, a separately approved, physically isolated ECU bench is required. Selecting `m5dial` instead of `kw905` uses the same adapter/cases; its separate Virtual-only firmware has **not** been implemented in this PR. The external serial adapter uses the target's ELM UART-like transport and does not perform automatic BLE selection or certification; a BLE bridge will be required for BLE-only KW905.

Virtual-only USB CDC mode (future M5Dial FW; **do not run on an unknown device**):

~~~sh
TTM_VIRTUAL_ONLY_ACK=YES python3 -m tools.elm327_sim \
  --mode virtual-cdc --port /dev/cu.EXPLICIT-VIRTUAL-ONLY-PORT \
  --virtual-only-ack \
  --model tests/elm327_conformance/fixtures/synthetic_ecus.json
~~~

The bridge expects the first inbound `HELLO` packet with payload `VIRTONLY` and then monotonic sequence numbers. No request is sent before the handshake. Invalid CRC/version/sequence closes the program. It does not automatically identify the firmware by BLE name. Manual USB port identity verification is still required. The wire proposal will be frozen against Golden/offsets in the future FW counterpart; no FW source, PlatformIO config or hardware was changed here.

## JSON model schema v1

Allowed top-level keys: ``schema` = "ttm.ecu-model.v1", `provenance` = "synthetic" or "anonymized_observation", `ecus[]`. ECU fields: ``id` ("ECUxx"), `responses[]`. Response fields: `request` (uppercase service 01 PID or UDS22 DID), `payload_hex` (complete diagnostic PDU without ISO-TP PCI), `response_id_11` (hex 7E8..7EF or null), `response_id_29` (hex 18DAF1xx), `delay_ms` (nonnegative relative delay), optional `drop_every` (every Nth response omitted), `relative_timestamps_ms` (nonnegative monotonic list; reserved for PM import/replay adapter). All fields are strictly validated; unknown fields including VIN/location/absolute timestamps are rejected. 11bit and 29bit responder mappings need not have identical ECU indices.

Example (fully synthetic; no original vehicle data):

~~~json
{"schema":"ttm.ecu-model.v1","provenance":"synthetic",
 "ecus":[{"id":"ECU01","responses":[
  {"request":"010C","payload_hex":"410C1AF8","response_id_11":"7E8",
   "response_id_29":"18DAF101","delay_ms":8}]}]}
~~~

The PM's **existing** ECU model has not been provided in this conversation, so no speculative conversion is applied. Codex should compare its field names, cardinality, timestamp meaning, payload scope and 29bit ECU IDs with this schema and introduce a reviewed import adapter; use SQLite immutable/read-only and never commit source DB/RAW logs. All current fixture data are explicitly `synthetic`, not RP8-derived.

## Limits / next steps

- Host tests only. 11/29 bit Classic CAN, Single Frame and ECU outgoing multi-frame ISO-TP with tester FC are implemented. Inbound multi-frame tester requests are assembled only for a single addressed ECU.
- PC ELM emulator does not yet implement every AT command. An exhaustive expectation catalog is a separate data-driven surface, not proof that unimplemented features work. Unsupported AT commands answer `?`.
- Simulator sample fields `relative_timestamps_ms` are validated but replay scheduling and PM-specific conversion remain gated pending the actual PM JSON.
- No dynamic timing model (ATAT / ATST) fidelity claims beyond parameter parsing. No CAN monitor completeness claims.
- No automatic real BLE connection, iPhone execution, SmartRing test, physical CAN safety qualification, or 10Hz signal freshness claim.
- Existing FW `main` protocol and safety tests are untouched.

Design: https://github.com/papataku/tacotaco-meter-ios/pull/25


## 2026-10-28 PM audit follow-up

**Host CI**: Python 3.9 and 3.12, pinned pytest==8.3.5 and pyserial==3.5. The preceding instructions are not hardware verification.

**Strict host-only regressions** in `tests/elm327_conformance/test_elm_strict.py` compare exact bytes and response ordering, rather than substring matches, for echo E1, LF L0, D resets, synthetic RV, PC, CRA filters in CAN 11/29bit, SH changes *actual* recipient, SP0 virtual CAN-only auto-selection, ST deadlines, approximated AT adaptive deadlines, CAN ERROR / BUFFER FULL fault injection, ISO-TP PCI frame order and BRD virtual baud handshaking. These test synthetic data in-process; external target comparisons remain separate and require an isolated bench. In particular, BRD changes **no real UART baud**. ST uses virtual elapsed ms; AT adaptive timing is a deterministic approximation, not ELM's proprietary exact tuning.

**Specification:** ELM327DSK https://www.elmelectronics.com/wp-content/uploads/2017/01/ELM327DS.pdf . Printed pp.8-10 (serial echo/LF/CR/prompt), pp.12-30 (AT commands/D/SH/CRA/PC/RV/BRD), pp.37-38 (SP0), pp.40-48 (CAN/ISO-TP/CRA), pp.51-53 (BRD, ST/AT), pp.88-90 (CAN ERROR, BUFFER FULL).

**PM model:** Existing ~15.8 MB source JSON has top-level `schema_version` and `sessions`, which are **not directly supported**. PM/Codex will convert it outside the repository to `ttm.ecu-model.v1`, remove identifiers/absolute time, and review it. Nothing from the original or converted real data is committed here.

`load_model(path)` still defaults to **2,000,000 bytes**, with opt-in `load_model(path, max_bytes=20_000_000)` (positive integer <= 128,000,000). Equivalent CLI flag: `--model-max-bytes 20000000`. Larger size does not weaken schema validation. Provenance must be exactly `synthetic` or `anonymized_observation`; a declared provenance is **not independently verified evidence of anonymization**. Unknown fields, raw `schema_version/sessions`, and private fields continue to fail closed. Tests only construct oversized synthetic whitespace JSON in the pytest temporary directory, never in the repo.

~~~sh
python3 -m tools.elm327_sim --mode elm-pty \
 --model /PRIVATE/converted_approved_model.json --model-max-bytes 20000000
~~~

No physical CAN, firmware or device writes in this PR.


## Synthetic time-series replay contract (optional extension to `ttm.ecu-model.v1`)

Existing fixed `payload_hex`, `delay_ms`, `drop_every`, and strict top-level `schema` / `provenance` / `ecus` validation are unchanged. A response **without** replay fields has `replay_mode="fixed"` and returns its legacy `payload_hex` at every request. The original fixed representative `payload_hex` remains mandatory for schema backward compatibility even when an optional series is present.

For a PID that changes over model time, select `replay_mode: "sequence"` or `"loop"` and provide nonempty `samples`. Each sample contains `at_ms` (integer relative to the model clock, starting **exactly at 0** and strictly increasing), either `payload_hex` (complete diagnostic PDU, uppercase, validated against requested Service/PID/DID) **or** `no_data: true`, and optional `delay_ms` (0 to 60000, overriding the response's fallback delay for that sample). Other fields, absolute dates/times and identifiers are rejected.

`sequence` uses sample-and-hold: the newest sample with `at_ms <= current model clock` is selected; **after the final sample, its payload/no_data state remains held**. Repeating a request at the same timestamp selects the same sample (subject to existing `drop_every`). `loop` applies `model_clock_ms % loop_period_ms`, then uses the same sample-and-hold selection; `loop_period_ms` is mandatory, a positive integer strictly greater than the last `at_ms`. All clocks use monotonic elapsed milliseconds from the start of the emulator run, **not wall-clock timestamps**. A gap from last sample until the loop boundary holds the last sample.

Every `no_data: true` sample suppresses this ECU's response; if no other ECU responds before the ATST/ATAT deadline, the ELM interface emits `NO DATA`. `drop_every=N` continues to drop every Nth request per ECU/PID independent of sample identity; per-sample `delay_ms` is applied to the outgoing CAN response and may also lead to `NO DATA` if it exceeds the requested response timeout. Injection does not introduce any physical CAN API.

**Synthetic example** (not extracted from RP8 or PM's data):

~~~json
{
  "schema": "ttm.ecu-model.v1",
  "provenance": "synthetic",
  "ecus": [{
    "id": "ECU01",
    "responses": [{
      "request": "010C",
      "payload_hex": "410C0000",
      "response_id_11": "7E8",
      "response_id_29": "18DAF101",
      "delay_ms": 10,
      "drop_every": 7,
      "replay_mode": "loop",
      "loop_period_ms": 500,
      "samples": [
        {"at_ms": 0, "payload_hex": "410C0000"},
        {"at_ms": 100, "payload_hex": "410C1000", "delay_ms": 30},
        {"at_ms": 200, "no_data": true},
        {"at_ms": 300, "payload_hex": "410C3000", "delay_ms": 5},
        {"at_ms": 400, "payload_hex": "410C4000", "delay_ms": 0}
      ]
    }]
  }]
}
~~~

This produces RPM snapshots at 0/100/300/400ms, silence at 200–299ms, and repeats at 500ms. `relative_timestamps_ms` is a **legacy passive metadata field**; a nonempty copy of it alongside active `samples` is rejected, preventing two conflicting replay clocks. The converter owned by PM/Codex must map actual observation-relative times to `samples[].at_ms`; **no conversion script or real-derived fixture is stored here**.

Validation remains strict: at most 32 ECUs, 256 responses per ECU, 10,000 samples per response, 4095-byte PDU maximum and existing 2,000,000-byte default JSON loader cap (explicit bounded override up to 128MB). The `provenance` value remains exactly `synthetic` or `anonymized_observation` and does not independently prove privacy. Exact timing and payload tests use only `tests/elm327_conformance/fixtures/synthetic_timeseries.json`.

**11-bit response IDs in the owner's converted model are provisional.** They are not evidence of real 11-bit RP8 routing or authorization to transmit to an actual CAN bus.
