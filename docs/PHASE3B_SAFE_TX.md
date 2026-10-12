# Phase 3B — Safe 29-bit Read-Only Diagnostic TX

Phase 3B is a **software implementation gate**. Vehicle TX remains blocked until Issues #1 and #3 are reviewed.

## Goal

Make M5Dial capable of executing the same basic read commands currently sent through KW905 while preserving the safety architecture.

The current Honda Analyzer uses:

- ISO 15765-4
- 29-bit identifiers
- 500 kbit/s
- functional headers `18DB33F1` and `18DBEFF1`

## Explicit TX activation

AT commands are always available in ELM mode, but vehicle traffic is locked until:

```text
ATM5TX1
```

This grants a short 3000 ms diagnostic lease.

```text
ATM5TX0
```

revokes immediately.

```text
ATM5STAT
```

reports the remaining lease and effective header.

A lease by itself does **not** put TWAI into Normal mode.

## Per-query mode sequence

For every accepted read request:

```text
LISTEN_ONLY
  -> validate header/service/length
  -> validate remaining lease
  -> enforce minimum request interval
  -> NORMAL
  -> exactly one single-shot CAN request
  -> receive bounded response window
  -> LISTEN_ONLY
```

Even if the lease remains active, the adapter stays Listen-Only between requests.

## Single TWAI owner

Only `CanMonitor::rxTask()` / its internal helpers own TWAI data-plane and mode APIs.

Other modules:

- never call `twai_transmit()`
- never stop/uninstall/install/start the TWAI driver
- submit a validated `DiagnosticRequest` through the command queue
- wait for a `DiagnosticResult`

This removes the prior RX-task vs driver-reconfiguration race.

## Allowed request scope

Headers:

```text
18DB33F1
18DBEFF1
```

Read request forms:

```text
01xx
09xx
22xxxx
```

Examples:

```text
0100
010C
010D
0105
015B
019A
222012
```

Rejected by policy:

- 0x10 DiagnosticSessionControl
- 0x11 ECUReset
- 0x14 ClearDiagnosticInformation
- 0x27 SecurityAccess
- 0x28 CommunicationControl
- 0x2E WriteDataByIdentifier
- 0x31 RoutineControl
- arbitrary raw CAN IDs/payloads

## TX behavior

- CAN request uses 29-bit extended ID
- request is ISO-TP Single Frame encoded
- TX uses TWAI single-shot
- no automatic retry
- minimum diagnostic request interval: 50 ms
- one active transaction at a time

Initial theoretical ceiling is therefore 20 requests/s before response-time effects.

## Response ownership

Only response IDs matching the observed/expected physical form:

```text
18DAF1xx
```

are eligible.

The response payload must also match the active request:

- `01xx -> 41xx`
- `09xx -> 49xx`
- `22xxxx -> 62xxxx`
- or matching UDS negative response `7F <service> <NRC>`

Unrelated frames are still copied into the RAW CAN queue but are not returned as query results.

## Phase 3B ISO-TP limit

Phase 3B returns only ISO-TP Single Frame responses.

If a matching First Frame is detected, the result is reported as multi-frame-required and ELM mode currently returns:

```text
BUFFER FULL
```

No Flow Control is transmitted in Phase 3B.

That means commands such as `019A` or `222012` may require Phase 3C before they fully match KW905 behavior.

## ELM example

```text
ATZ
ATE0
ATL0
ATS0
ATH1
ATAL
ATCAF1
ATCFC1
ATSP7
ATCP18
ATSHDB33F1
ATM5TX1
010C
```

Expected successful response form with ATS0/ATH1/CAF1:

```text
18DAF1xx410C....
>
```

## Vehicle test gate

Do not use Phase 3B vehicle TX until:

1. Issue #1 listen-only hardware validation is reviewed.
2. Issue #3 Native capture validation is reviewed.
3. Phase 3B software/static tests are green.
4. actual M5Dial PlatformIO build is green.
5. isolated/bench CAN validates exactly-one TX and return to Listen-Only.

The first vehicle request should be `0100`, parked, before higher-frequency polling.
