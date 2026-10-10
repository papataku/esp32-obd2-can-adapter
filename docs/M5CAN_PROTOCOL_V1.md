# M5CAN versioned BLE batch protocol — V1.0

## Capability negotiation (mandatory, every connect / initialization)

1. The iPad starts in **standard ELM327 compatibility**, regardless of the advertised BLE name.
2. Send usual ELM initialization `ATZ` / `ATI`, inspect identity for the token `M5CAN`.
3. **Only if the current adapter identifies as M5CAN**, send `ATM5CAP\r`.
4. A compatible firmware (current `FW=0.4.1-phase3d-implicit`) returns:

   ```text
   M5CAN-CAPS PROTO=1.1 FW=0.4.1-phase3d-implicit BATCH=16 OPS=OBD01,UDS22 STREAM=0 LEASE=IMPLICIT
   >
   ```

5. iPad verifies structured fields, not the device name alone:
   - `PROTO=1.x` recognized, major 1 and nonnegative minor (otherwise fallback).
   - `FW=` present; display and record exact version for regression reports.
   - `BATCH=` between 1 and 16, enforced for each request.
   - `OPS=` an explicit set: `OBD01`, `UDS22`; `STREAM=0` means **no streaming yet**.
   - Empty/malformed response, timeout, `?`, unknown major or missing required operation → **standard ELM327 fallback**, no dedicated batch queries.

Firmware version and protocol version are **independent**. A firmware bugfix can increment FW without changing PROTO; a wire-incompatible change must increment PROTO major. Minor changes within major 1 can add backward-compatible features, which iPad must independently advertise/recognize.

A new session always probes anew; do not cache support across BLE reconnects or adapters.

## One BLE write requests up to 16 IDs

Syntax:

```text
ATM5B00:010C,010D,0105,015B,019A\r    // Five supported Mode01 PIDs
ATM5B01:2012,E480,E481,E600,E602\r    // Known ECU01 UDS22 DIDs
```

One BLE command starts **1–16 sequential vehicle CAN read-only transactions on M5CAN**. It does NOT send 16 DIDs in one UDS request. Only the physically confirmed ECU01 `18DA01F1` for UDS22, and `18DB33F1` for the five explicitly supported Mode01 PIDs, are permitted. Other headers, services and non-allowlisted PIDs are rejected.

Reply example:

```text
M5ITEM:00:010C
18DAF10104410C144C
M5ITEM:00:010D
18DAF10103410D28
M5ITEM:00:0105
18DAF10103410555
M5ITEM:00:015B
18DAF10103415B89
M5ITEM:00:019A
18DAF1011008419A0700401F
18DAF1012100035555555555
M5DONE
>
```

The iPad must validate exact ID ordering and `M5DONE` before treating the batch as complete. A single item may return a negative ECU response or NO DATA; do not discard other valid items. CAN transactions remain serialized and subject to per-CAN-ID pacing, internal TX authorization, read-only allowlist, ownership checks and fault/timeout gates.

## Compatibility matrix

| Adapter / firmware | Expected |
|---|---|
| KONNWEI KW905 | Standard ELM327; never send ATM5CAP without positive M5CAN identity |
| M5CAN v0.3 without ATM5CAP | Standard ELM327; no batch |
| M5CAN v0.4.1, PROTO 1.1 with OBD01 | Dedicated five-PID batch used by iPad live and known-signal validation |
| M5CAN PROTO 1.x without UDS22 | Do not send dedicated UDS batch; keep ordinary individual read-only UDS |
| Future M5CAN PROTO 2.0 | Fail closed to standard ELM until iPad adds V2 support |

## Performance and safety

Batching saves the iPad↔BLE per-command roundtrip, not ECU processing time. As of V1.0, `STREAM=0`: subscriptions and nonblocking timestamped binary streams remain future work. This avoids claiming firmware-local continuous acquisition before it is actually implemented.

The iPad's SQLite capture records `M5CAN_PROTOCOL` and keeps original batch raw commands plus per-PID reference rows. Since only batch completion is timestamped, synthesized individual reference rows use an unknown/zero individual latency rather than faking per-ID durations.

No live vehicle test is claimed from passing build and host tests. Perform parked/bench validation first and inspect exact accepted response IDs, negative status, BLE data loss and effective throughput.


## BLE lease overhead elimination (PROTO 1.1)

ELM327 never required `ATM5TX1`; this is an M5CAN private extension. Previous M5CAN required the iPad to send `ATM5TX1` periodically (about every 2 seconds) even for safe read-only commands. Each extra command costs a full BLE ELM request/response.

Firmware `0.4.1-phase3d-implicit` announces `PROTO=1.1 ... LEASE=IMPLICIT`. iPad negotiates that feature and omits all periodic and per-batch `ATM5TX1` commands. The firmware instead creates a bounded internal TX authorization at each **validated read-only request** and refreshes it for each item within a long batch. The authorization is never granted for an unknown CAN ID or write/control service. No simultaneous CAN diagnostic transactions are introduced.

CAN still returns to Listen-Only after each query; BLE disconnect, mode exit, ATZ reset, bus-off and internal failures revoke authorization. Historical explicit commands `ATM5TX1`/`ATM5TX0` remain accepted to avoid breaking older tools, but the new iPad app does not need them.

If `LEASE` is missing or `LEASE=EXPLICIT`, iPad keeps the legacy renewal logic. A malformed/unknown LEASE value disables dedicated-mode negotiation and falls back safely. iPad must never infer implicit mode from the firmware name or version string alone.

Verification must distinguish code passing CI from actual wire traces: new device + updated iPad should show zero `ATM5TX1` commands during live acquisition, while PID/DID responses and disconnect fail-safe still work. This is a transport overhead optimization, **not** permission to relax the per-CAN-ID 50-ms guard or send unsafe services.
