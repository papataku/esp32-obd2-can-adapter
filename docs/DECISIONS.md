# Architecture Decision Log

This file records decisions that future changes should not silently undo.

## D001 - Repository scope

**Decision:** The repository is named `esp32-obd2-can-adapter`, while the initial firmware/product working name is **M5CAN**.

**Reason:** The first hardware is M5Dial, but the architecture should not be permanently tied to one enclosure/board.

## D002 - M5Dial + U179 is the initial hardware baseline

**Decision:** Initial bring-up uses M5Dial (ESP32-S3) and M5Stack Mini CAN Unit U179.

Initial pins:

- GPIO13 -> CAN TX
- GPIO15 <- CAN RX

## D003 - Vehicle power comes from switched USB, not OBD Pin 16

**Decision:** Normal vehicle configuration uses ACC/IG-switched USB 5V.

- OBD Pin 16 is not used.
- U179 HPWR+ is not used.
- U179 is powered from the M5Dial side.

**Reason:** Deterministic power-off and no dependency on an adapter sleep mode while parked.

## D004 - U179 termination must not be ignored

**Decision:** U179's onboard 120-ohm termination is treated as a hardware integration concern.

**Reason:** A third termination on an already terminated CAN bus can change the effective bus impedance. Vehicle topology must be measured/verified rather than assumed.

## D005 - Boot is Listen-Only

**Decision:** Firmware never boots directly into diagnostic TX mode.

**Reason:** A reboot, brownout, software crash, or saved configuration must not cause unexpected vehicle traffic.

## D006 - Diagnostic TX uses a Lease

**Decision:** TX permission is time-limited and must be checked at the transmit boundary.

**Reason:** Host/app failure should naturally cause TX permission to disappear.

## D007 - No unrestricted raw TX in early public interfaces

**Decision:** Early phases expose only reviewed diagnostic request forms.

**Reason:** “OBD-only” commands can still become arbitrary diagnostic writers if payloads are insufficiently validated.

## D008 - RAW CAN is the source of truth

**Decision:** Higher-level parsing never replaces raw evidence.

**Reason:** Required for debugging ISO-TP, ELM compatibility, timing, and signal discovery.

## D009 - Full continuous logs live on the Mac

**Decision:** ESP32 does not continuously store full-bus RAW CAN in flash.

**Reason:** bandwidth, flash endurance, capacity, and better reproducibility on the host.

ESP32 may keep bounded RAM history and selected fault captures.

## D010 - Native USB protocol is preferred for our own analyzer

**Decision:** Honda Analyzer should use M5CAN Native protocol when talking to M5CAN.

ELM327 compatibility remains available for interoperability and Golden comparison.

## D011 - KW905 remains a Golden Reference

**Decision:** Keep KW905 for controlled compatibility/performance tests.

**Reason:** It provides a known external reference while M5CAN evolves.

## D012 - Build capability in narrow phases

**Decision:** Sequence is approximately:

```text
Listen-Only
 -> Native stream
 -> leased read-only TX
 -> ISO-TP
 -> read-only UDS
 -> ELM compatibility
 -> optimization
```

**Reason:** Each layer can be validated independently and failures remain attributable.

## D013 - Initial diagnostic baseline is 11-bit / 500 kbit/s

**Decision:** Basic OBD validation is performed before Honda-specific 29-bit/UDS expansion.

**Reason:** Avoid mixing addressing/protocol complexity into basic CAN/TX safety bring-up.

## D014 - High-rate meter signals should prefer periodic CAN when validated

**Decision:** Do not assume the final meter must poll every displayed value through OBD/UDS.

**Reason:** Periodic CAN can provide higher update rates and allows multiple signals to be observed concurrently.

OBD/UDS remains appropriate for values that are not available as validated periodic CAN signals.

## D015 - Prior prototype work is input, not unquestioned source code

**Decision:** Earlier Phase 1-4A.1 prototypes created during design discussions may be reused, but must be reviewed and revalidated before entering this repository.

Known review lessons are listed in `AGENTS.md`.

**Reason:** This repository is now the durable source of truth.
