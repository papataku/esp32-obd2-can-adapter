# Phase 3C — ISO-TP / KW905 multi-frame parity

Phase 3C completes the multi-frame receive path required by the current
Honda Analyzer while preserving the Phase 3 safety contract.

## Implemented

- existing `IsoTpAssembler` extended with accepted-CF progress reporting
- SF / FF / CF reassembly
- up to 8 simultaneous response ECU sessions
- sequence checking and per-session CF timeout
- UDS Response Pending (`7F <service> 78`)
- active-query ownership check before Flow Control
- stale pre-query frame rejection
- Flow Control target derived from actual responder:
  `18DAF1xx -> 18DAxxF1`
- FC payload `30 00 00`
- FC uses the same single owner-only `twai_transmit()` call site
- lease is checked again immediately before every FC TX
- raw response CAN frames retained separately from reassembled payloads
- ELM output reproduces the observed KW905 raw-line representation
- ELM diagnostic requests are asynchronous so M5Dial UI and RAW queue
  draining continue while a query is active

## Golden examples

### 019A

```text
18DAF1011008419A07003DBD
18DAF10121FF705555555555
```

### 222012

```text
18DAF101102762201270000F
18DAF1012100001A05AA0000
18DAF1012200000000000000
18DAF1012300000000000000
18DAF101240E4D0EBE000000
18DAF1012500000000005555
```

Internally `222012` is also reconstructed as a 0x27-byte payload beginning
with `62 20 12`.

## Safety invariants

Vehicle TX remains explicitly leased through `ATM5TX1`.

Between requests the adapter is Listen-Only. During a request:

```text
LISTEN_ONLY
 -> validate request and remaining lease
 -> NORMAL
 -> one single-shot diagnostic request
 -> matching response only
 -> matching FF may receive FC
 -> bounded CF / response-pending window
 -> LISTEN_ONLY
```

No FC is sent for an unrelated service/PID/DID FF or a stale frame.

Allowed request headers remain only `18DB33F1` and `18DBEFF1`.
Allowed read forms remain only `01xx`, `09xx`, and `22xxxx`.
No public raw CAN TX API is added.

## Hardware gate

Software/CI success does not replace the vehicle gate. Before sustained
vehicle use, verify Issues #1/#3 and start parked with known read-only
requests. Compare M5CAN and KW905 using identical commands and record
request/s, latency percentiles, timeout rate, and CAN error counters.
