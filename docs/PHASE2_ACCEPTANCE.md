# Phase 2 Acceptance - Native USB Capture

Phase 2 may be developed while Phase 1 vehicle validation is pending, because it remains CAN receive-only. Do not treat Phase 2 as accepted/merge-ready until these gates pass.

## Static/software gates

Required:

```bash
./tests/run_all.sh
pio run -e m5dial
```

Expected:

- all C++ translation units compile with host warnings-as-errors
- C++ Native protocol tests pass
- Python Native protocol tests pass
- capture-validator tests pass
- static safety check confirms:
  - Listen-Only remains present
  - TX queue constant is zero
  - effective TWAI `tx_queue_len` is zero
  - no `twai_transmit()`
  - no Normal mode
  - no TX Lease
  - no OBD command
- actual PlatformIO M5Dial build succeeds

## USB mode-switch test

1. Boot device in text mode.
2. Send `STREAM OFF`; expect `#OK,STREAM,OFF`.
3. Send `NATIVE ON`.
4. The final text line must be:

```text
#OK,NATIVE,ON,COBS+CRC32,version=1
```

5. The following bytes must be valid COBS+CRC32 Native packets.
6. Initial HELLO must report:
   - protocol 1
   - bitrate 500000
   - tx_enabled = false
   - stream_enabled = false
7. Host sends START_STREAM.
8. CAN_FRAME packets may then begin.

## Capture test

Run:

```bash
python3 tools/native_capture.py \
  --port /dev/cu.usbmodemXXXX \
  --jsonl logs/test.jsonl
```

Capture representative active vehicle traffic for at least 120 seconds.

Then run the strict acceptance validator:

```bash
PYTHONPATH=tools python3 tools/validate_capture.py \
  logs/<capture>.m5can \
  --min-duration 120 \
  --report logs/<capture>-acceptance.json
```

The validator exits 0 only when all of the following are satisfied:

- CAN frames are present
- STATS packets are present
- malformed Native packets = 0
- semantic decode errors = 0
- unexplained device sequence gaps = 0
- CAN timestamp regressions = 0
- capture timestamp span meets `--min-duration`
- app drop = 0
- driver missed = 0
- driver overrun = 0
- bus error count = 0
- arbitration-lost count = 0
- Native host-input error count = 0

The JSON report also records:

- CAN frame count
- standard / extended / RTR counts
- timestamp span
- average observed CAN frame rate
- packet type counts
- maximum observed error counters

For independent re-decoding, also run:

```bash
python3 tools/decode_log.py \
  logs/<capture>.m5can \
  --jsonl logs/<capture>.jsonl
```

Expected: `bad=0`.

## Failure handling

Do not proceed to Phase 3 if any of these occur:

- Native packet corruption under normal traffic
- sequence gaps not explained by reconnect/reset
- receive drops under expected bus load
- CAN timestamp regression without an understood device reset
- text/native mode boundary ambiguity
- host input can starve CAN draining
- any CAN TX path appears in Phase 2
