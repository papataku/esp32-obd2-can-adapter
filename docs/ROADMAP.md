# Development Roadmap

The project advances by acceptance gates rather than by feature count.

## Phase 0 - Repository and design baseline

Goal:

- establish project structure
- capture architecture/safety decisions
- define target hardware and acceptance process

Exit criteria:

- README/AGENTS/architecture/safety/roadmap exist
- hardware wiring assumptions reviewed
- PlatformIO target selected

## Phase 1 - Listen-Only RAW CAN probe

Goal:

- M5Dial + U179 receives 500 kbit/s Classical CAN
- no external CAN transmission path
- USB text/debug output for bring-up
- basic on-device status display

Required checks:

- boot into Listen-Only
- RX counters
- timestamp
- dropped/missed frame counters
- no CAN TX API reachable from host
- sustained vehicle RX without crashes

## Phase 2 - Native USB protocol

Goal:

- efficient and replayable host transport
- RAW CAN captured on Mac

Target features:

- COBS + CRC32 framing
- packet version/type
- sequence number
- GET_INFO
- GET_STATS
- START_STREAM
- STOP_STREAM
- raw `.m5can` capture
- offline decode/replay

Exit criteria:

- sequence-gap detection
- corruption/CRC tests
- malformed-input tests
- capture/replay round-trip tests

CAN TX remains disabled.

## Phase 3 - Safe diagnostic TX

Goal:

- add controlled CAN TX without opening unrestricted raw TX

Features:

- TX Lease
- explicit revoke
- immediate logical expiry
- synchronized Listen-Only <-> Normal transition
- request rate limit
- known read-only OBD single-frame request

Initial scope:

- 11-bit
- 500 kbit/s
- functional request 0x7DF
- reviewed OBD read requests only

Exit criteria:

- lease timeout tests
- disconnect/failure tests
- no arbitrary CAN ID/payload public API
- bench test before vehicle TX
- known vehicle OBD responses confirmed

## Phase 4A - ISO-TP response receive/reassembly

Goal:

- receive diagnostic multi-frame responses safely

Features:

- SF
- FF
- CF
- FC for conversations owned by this adapter
- sequence checking
- bounded payload
- session timeout
- multi-ECU sessions

Important tests:

- sequence mismatch
- timeout
- oversized length
- session collision
- unrelated PID/service traffic
- another tester's traffic
- stale queue frames
- response queued near query timeout

## Phase 4B - Honda 29-bit / UDS read support

Goal:

- add the read-only UDS paths required by Honda e:HEV analysis

Start with:

- 29-bit addressing used by observed vehicle diagnostics
- ReadDataByIdentifier (0x22)
- known/explicit target addressing
- negative-response handling
- Response Pending handling where needed

Do not add write/control/programming services.

## Phase 5 - ELM327 compatibility

Goal:

- allow existing ELM-oriented software and enable KW905 comparison

Initial command set:

- ATZ / ATWS / ATI
- ATE0/1
- ATL0/1
- ATS0/1
- ATH0/1
- ATSP0/6/7 as supported
- ATDP / ATDPN
- ATSH / ATCRA where safely mapped
- ATCAF behavior needed by our host
- OBD requests used by the project

ELM compatibility must reuse the same underlying CAN/ISO-TP stack.

## Phase 6 - Honda Analyzer integration

Goal:

Mac application can select:

```text
KW905 -> ELM transport
M5CAN -> Native transport
```

Functions:

- connection inspection
- live RAW CAN
- ECU discovery
- OBD/UDS request
- logging
- replay
- payload analysis

Native mode is preferred for M5CAN.

## Phase 7 - KW905 Golden Benchmark

Run identical controlled tests.

Measure:

- payload equivalence
- requests/sec
- latency P50/P95/P99
- jitter
- timeout/error rate
- long-run stability

Do not claim a performance multiplier before this measurement.

## Phase 8 - Signal discovery and meter integration

Goal:

Reduce polling where periodic CAN signals can be identified safely.

Target architecture:

```text
high-rate values -> RAW periodic CAN
low-rate values  -> OBD/UDS
```

Examples to investigate:

- engine RPM
- vehicle speed
- motor/generator RPM
- HV battery current/voltage/power
- SOC
- temperatures

Signal definitions must be versioned and backed by captured evidence.

## General gate for every phase

A phase is not complete until applicable items pass:

1. unit tests
2. malformed/boundary tests
3. safety regression tests
4. C++ compilation
5. real PlatformIO target build
6. bench CAN validation
7. vehicle validation where required
8. documentation update
9. reproducible logs/results

Actual hardware-only validation must be clearly marked as pending rather than silently assumed.
