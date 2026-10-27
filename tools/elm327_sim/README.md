# Host-only ELM327 conformance and Virtual CAN simulator (Draft)

**NOT hardware or vehicle qualification.** Python code contains no SocketCAN/USB-CAN/TWAI transport and does not write to CAN. The only optional real port is an explicitly selected USB CDC **Virtual-only** interface with a fixed versioned HELLO, tested on a disconnected bench after separate operator acknowledgement.

## Use (Python 3.12)

~~~sh
python3 -m pip install pytest pyserial
PYTHONPATH=. python3 -m pytest -q tests/elm327_conformance
python3 -m tools.elm327_sim --model tests/elm327_conformance/fixtures/synthetic_ecus.json --mode elm-pty
~~~

The PTY path is printed to stderr; connect an ELM client to it. No hardware is opened. This emulator intentionally supports the tested CAN 11/29 subset; do **not** call it a complete ELM327 clone.

Same conformance cases against an optional physical target (do not connect a vehicle):

~~~sh
TTM_ISOLATED_BENCH=YES PYTHONPATH=. python3 -m pytest -q tests/elm327_conformance/test_elm.py \
  --elm-target=kw905 --elm-port=/dev/cu.EXPLICIT-ISOLATED-PORT --isolated-bench-ack
~~~

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
