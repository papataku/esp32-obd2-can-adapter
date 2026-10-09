# Phase 3D — BLE ELM transport for iPad

Phase 3D makes M5Dial usable in the vehicle without a USB data cable.

## Transport

M5Dial advertises as:

```text
M5CAN-Dial
```

BLE GATT uses the Nordic-UART-compatible UUID layout:

- service: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- RX/write: `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- TX/notify: `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`

The existing iPad `KW905BLETransport` is generic: it scans all GATT
services and selects a write characteristic plus a notify characteristic.
No fixed KW905 UUID is required, so this layout is directly compatible.

## BLE protocol

BLE carries the same ELM text protocol as the KW905 path:

```text
ATZ\r
ATE0\r
...
010C\r
222012\r
```

Responses are notified back as ELM text ending in the normal `>` prompt.

Notifications are split into 20-byte chunks so operation is safe before a
larger ATT MTU is negotiated. The iPad ELM prompt framer already reassembles
arbitrary BLE notification chunks.

Native binary M5CAN remains USB-only in Phase 3D. BLE is deliberately ELM
only so the first vehicle path stays compatible with the already working
iPad Analyzer.

## Session ownership

Only one ELM session owns the shared ELM parser at a time:

```text
USB ELM  xor  BLE ELM
```

A BLE command claims an idle ELM session. USB debug remains available when
there is no conflicting USB ELM/Native session.

This prevents two hosts from changing ELM header/timing state concurrently.

## Disconnect safety

BLE disconnect immediately:

1. clears queued BLE command bytes,
2. revokes the TX Lease,
3. releases BLE ELM ownership.

If a bounded CAN transaction was already in progress, the CAN owner finishes
or aborts safely and its result is drained internally. A new BLE connection
does not inherit the old authorization.

## Display

The M5Dial top line shows:

- `BLE WAIT`: advertising / no connection
- `BLE LINK`: iPad connected
- `BLE ELM`: iPad owns the active ELM session
- `BLE ERROR`: BLE initialization failed

The existing CAN status, frame/s, DIAG/s, TX/s, lease and error counters stay
visible.

## iPad lease behavior

The M5CAN diagnostic path still requires `ATM5TX1`.

The iPad Analyzer must recognize `ATI` containing `M5CAN` and renew the
lease before vehicle read commands when the previous renewal is older than
about two seconds. This avoids doubling every diagnostic request with an
extra lease command while still keeping the firmware's short lease.

## Safety invariants

BLE does not bypass any CAN policy:

- boot remains Listen-Only
- allowed request headers remain `18DB33F1` / `18DBEFF1`
- allowed read forms remain `01xx` / `09xx` / `22xxxx`
- all vehicle TX still passes through the single CAN-owner transmit site
- ISO-TP Flow Control still requires an owned matching FF
- BLE disconnect revokes authorization
- no public raw CAN TX API is exposed
