# Adaptive multi-PID fallback pilot (explicit experimental build only)

## Why
The `ATM5B00` command currently saves BLE command overhead, but normally sends
separate serialized CAN requests for each Mode 01 PID. ECU support for grouping
may vary, and no in-vehicle support has been demonstrated for the Honda RP8.

## Conservative scope
- Only `m5dial_dual_ble_multipid_bench`, with
  `M5CAN_ADAPTIVE_MULTIPID_EXPERIMENTAL=1`, may probe. Existing normal,
  dual 20-byte, MTU247 and mode-switch dry-run profiles retain individual CAN
  requests. This opt-in isolates the unqualified CAN pilot from stable users.
- Only *adjacent* `010C,010D` (RPM then speed) inside `ATM5B00`,
  same functional CAN request ID `18DB33F1`. All other items are unchanged.
- Never probe in synthetic DEMO mode; normal `m5dial` image is unchanged.
- First eligible batch sends the single-frame CAN payload `01 0C 0D`
  (a single **CAN transaction**, not merely an `ATM5B` BLE batch).
- Require a **fully decoded**, positive response containing exactly RPM
  (two data bytes) and speed (one byte) from one allowed responder, in either
  PID order. Bind subsequent grouped replies to that responder.
- Success: group further adjacent pairs until firmware reboot.
  The BLE response preserves `M5ITEM:00:010C` and `M5ITEM:00:010D`,
  using decoded ELM-compatible virtual SF lines. These are not RAW bus frames.
  Original physical CAN RX frames remain available in the RAW logger.
- Unsupported / negative / partial / malformed / timeout / ISO-TP error:
  **disable this grouping profile for the rest of the boot** and immediately
  fetch the requested items using the existing serialized per-PID mechanism.
  The next batches go directly to the old path (no probe loop).
- Driver, lease and bus faults: disable grouping, abort the remaining
  pending BLE batch and report the error without generating additional CAN TX.
  Existing TX fault lockout wins. Fail closed when the probing BLE peer drops.
- The known RPM+speed response is exactly six diagnostic bytes. Reject any
  claimed ISO-TP First Frame for this profile without sending Flow Control,
  even if its prefix resembles a matching PID (other testers may be active).
- The two BLE clients share one capability decision and one serial CAN owner.
- Retain the 50ms minimum **for each PID** across group and individual TX.
- Do not cache a partly successful group; validated single-item positive
  results still retain the existing 100ms cache behavior.
- Do not extend the general ELM command parser with unrestricted multi-PID TX.

## What this DOES NOT implement
- UDS `22` multiple DID requests. The decoder needs a verified layout
  (variable-length DID records are not self-delimiting).
- Arbitrary 2–6 PID combinations; every allowed combination needs a
  well-defined parser and evidence that the ECU provides correct responses.
- Auto-probing after vehicle ECU or wiring changes without a power cycle.
- Any new ECU write/control commands or relaxed TX lease/diagnostic policy.

## Host/bench acceptance
1. Run `./tests/run_all.sh` and all PlatformIO environments in CI.
2. Host tests: exact safety allowlist, accepted/reordered pair, missing PID,
   duplicate/unexpected PID, foreign ECU, negative/timeout and disabled latch;
   per-PID 50ms pacing even after grouped TX; unrelated frames rejected.
3. **Physical CAN bench, before connecting vehicle:** with a simulated ECU
   that supports `01 0C 0D`, verify first probe, subsequent grouping,
   original two `M5ITEM` replies, one actual CAN request per grouped pair.
4. Set a simulated ECU to reject `01 0C 0D`, return only RPM, or time out.
   Verify exactly one probe per boot, automatic old-mode fallback, no missing
   RPM/speed, and no subsequent group attempts.
5. Two BLE clients: verify results belong only to the requesting peer,
   rate limiter is preserved, no TX overlaps, no overflow/reconnects;
   check DEMO and production images behave exactly as before.
6. Confirm run-time telemetry and collected RAW CAN frames. Compare CAN TX
   attempts, p50/p95/p99 batch RTT, visible signal Hz and timeout/error rates.
7. Only after all gates and explicit vehicle approval, do a short stationary
   read-only test; retain previous firmware binary for immediate rollback.

No claim of improved actual performance or vehicle ECU compatibility is made
until the above observations exist.
