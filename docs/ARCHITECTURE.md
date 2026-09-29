# Architecture

## Design goal

M5CAN is not an ELM327 clone with extra commands. It is a **CAN data and diagnostic platform** that happens to provide an ELM327-compatible interface.

The architecture must preserve low-level observability and make failures diagnosable.

## Layer model

```text
+------------------------------------------------------+
| Host applications                                    |
| - Honda e:HEV Analyzer                              |
| - terminal / compatibility applications             |
+----------------------+-------------------------------+
                       |
              USB Native / ELM text
                       |
+----------------------v-------------------------------+
| Host protocol layer                                  |
| - Native framing / commands                          |
| - ELM327-compatible parser                           |
+----------------------+-------------------------------+
                       |
+----------------------v-------------------------------+
| Diagnostic services                                  |
| - OBD-II                                             |
| - UDS                                                |
| - ECU discovery                                      |
+----------------------+-------------------------------+
                       |
+----------------------v-------------------------------+
| ISO-TP                                               |
| - Single Frame                                       |
| - First / Consecutive Frame                          |
| - Flow Control                                       |
| - timeout/session tracking                           |
+----------------------+-------------------------------+
                       |
+----------------------v-------------------------------+
| RAW CAN core                                         |
| - timestamp                                          |
| - RX/TX accounting                                   |
| - filtering                                          |
| - ring buffer / stream                               |
+----------------------+-------------------------------+
                       |
+----------------------v-------------------------------+
| ESP32-S3 TWAI                                        |
+----------------------+-------------------------------+
                       |
+----------------------v-------------------------------+
| U179 / TJA1051T/3                                    |
+----------------------+-------------------------------+
                       |
                    Vehicle CAN
```

## Principle: RAW CAN first

Every higher-level feature should be explainable from RAW CAN evidence.

Examples:

- An ELM query timed out:
  - Did TX occur?
  - Was a response CAN frame received?
  - Did ISO-TP reject it?
  - Did ELM formatting drop it?
- A multi-frame response failed:
  - Which sequence number was expected?
  - Was Flow Control sent?
  - Was the conversation actually ours?
- A signal stopped updating:
  - Did the raw CAN ID disappear?
  - Did only decoding fail?

This is why full host logging is a first-class feature.

## Firmware components

Recommended module boundaries:

```text
src/
  app/
    app_state.*
    safety_service.*

  can/
    twai_driver.*
    can_frame.*
    can_monitor.*
    can_stats.*

  transport/
    usb_console.*
    native_protocol.*
    native_framing.*

  diagnostic/
    tx_lease.*
    obd.*
    isotp.*
    uds.*
    ecu_discovery.*

  elm/
    elm_parser.*
    elm_session.*
    elm_formatter.*

  ui/
    dial_status_ui.*
```

Exact paths may evolve, but responsibility separation should remain.

## Modes

### Listen-Only / Monitor

Default after boot.

Characteristics:

- no intentional CAN TX
- no ACK from TWAI Listen-Only mode
- raw CAN RX and statistics allowed
- host may connect/disconnect freely

### Diagnostic TX enabled

Requires a valid TX Lease.

The lease is a safety capability, not simply a UI flag.

Properties:

- bounded duration
- renewed explicitly
- revoked explicitly or automatically
- checked at the actual TX boundary
- invalid immediately when its deadline passes

A TX path should conceptually require:

```text
request validated
AND mode allows TX
AND lease valid now
AND CAN driver healthy
AND request rate allowed
```

### ELM compatibility

ELM compatibility is an adapter over the existing diagnostic stack.

Do not create a second independent CAN/ISO-TP implementation for ELM mode.

## Native protocol

The Native protocol is preferred for development and analysis because it can preserve more information than ELM text.

Desired properties:

- binary-safe framing
- packet type
- version
- sequence number
- payload length
- CRC/integrity
- explicit timestamps
- forward-compatible feature discovery

Current design direction:

- COBS framing
- CRC32
- monotonically advancing sequence number
- capture encoded/raw Native packets on the host before decoding

The protocol should support at least:

- device info
- stats
- start/stop RAW CAN stream
- mode/status
- lease acquire/renew/revoke
- controlled diagnostic request
- error/event notifications

## ISO-TP scope progression

Do not implement every ISO-TP feature at once.

Initial scope:

- Classical CAN
- normal addressing
- 11-bit response IDs
- Single Frame
- multi-frame response reassembly
- Flow Control only for a response conversation known to belong to our request
- bounded payload
- session timeout
- sequence number validation

Then extend to:

- 29-bit addressing used by Honda diagnostics
- request-side multi-frame only when actually required
- additional addressing modes if needed

## OBD-II and UDS

The stack should support two classes of data:

### Periodic/raw vehicle signals

Preferred when the signal is already broadcast on CAN.

Examples may include RPM, vehicle speed, motor data, pedal data, etc., once identified.

Advantages:

- no polling overhead
- high update frequency
- multiple signals observed concurrently

### Diagnostic values

Use OBD-II/UDS when the value is not available or not yet identified in periodic CAN.

Examples may include lower-rate temperatures, status values, identification data, and DIDs.

The final meter path can therefore be hybrid:

```text
fast values  -> RAW CAN
slow values  -> OBD-II / UDS
```

## Host relationship

The firmware should expose enough information that the Mac analyzer can distinguish:

```text
KW905
  -> ELM-compatible transport

M5CAN
  -> Native transport preferred
  -> ELM-compatible transport available
```

Native mode should not require the Mac analyzer to parse ELM-formatted strings.

## Concurrency

Safety-related driver transitions require explicit synchronization.

Particular attention:

- RX task vs TWAI stop/uninstall/reinstall
- Native/console input vs lease expiration
- UI update vs high-rate RX
- logging backpressure vs CAN RX draining

No host workload should be able to starve the safety service indefinitely.

## Storage strategy

ESP32 flash is not the primary RAW CAN store.

Preferred flow:

```text
CAN RX
  -> bounded RAM queue/ring
  -> USB Native stream
  -> Mac append-only raw capture
  -> decoder / database / analysis
```

On-device flash may store:

- configuration
- firmware metadata
- small fault/event captures

but not continuous full-bus logging by default.
