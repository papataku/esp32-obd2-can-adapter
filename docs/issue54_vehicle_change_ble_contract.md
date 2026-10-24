# Vehicle changes and logical BLE contract — stacked on Issue #54 PR #56

Status: **review-only, opt-in firmware candidate**, not a vehicle-qualified release. Implemented in PR #57. M5Dial must never emit diagnostic TX at boot. All CAN TX is gated by existing read-only policy, finite TX Lease, 50ms same-PID pacing, one CAN owner, Listen-Only mode between transactions, and BUS-OFF lockout.

## Confirmed client wire behavior (no client update)

| Client | Current logical request | Routing after ECU selection |
| --- | --- | --- |
| TacoTacoMeter SmartRing/T4-S3/JC | After strict `ATM5CAP` handshake, `ATM5B00:010C,010D,0105,015B,019A` or an ordered due-PID subset | Only Mode01 RPM/speed are translated internally by M5Dial; `M5ITEM`/`M5DONE` remain unchanged |
| iPad Honda e:HEV Analyzer | After M5CAN handshake, `ATM5B00:010C,010D,0105,015B,019A` | Same as above |
| ELM-compatible clients | Existing `ATSHDB33F1` plus `010C`/`010D` | M5Dial handles the selected physical address internally on its experimental dual-BLE path |

**Do not send `ATSHDAxxF1`, ECU addresses, VIN requests or new UDS services from the meter.** The meter works with logical RPM/speed, even after changing vehicles. The strict batch parser accepts only requested `M5ITEM` tags; it cannot safely accept unsolicited status lines.

## Vehicle-change policy: a fingerprint is NOT a vehicle identity

The ECU reply-source set is not necessarily unique: two Honda vehicles may have the same responders, gateways may alter visibility, and CAN has no inherent VIN proof. This feature therefore saves **only an advisory cache**. There is no claim of reliable vehicle identification.

- New dedicated NVS namespace `m5can_ecu`, single `hint` record; schema v1, max eight **sorted full 29-bit response CAN IDs**, selected responder, CRC32, strict size/version/ordering/membership validation. It does not touch `m5can_trip`, `m5can_blelog`, CANLOG, bonds, OTA or any existing log key.
- Saving occurs **only after** the eight-probe fresh selection AND first **validated physical** result. BLE worker stages an immutable RAM snapshot; the Arduino/UI loop acquires the existing CAN-idle checkpoint gate, writes once, reads back and validates CRC. No flash work on the CAN task or BLE callback. Saving failure does not authorize additional CAN traffic.
- On every reboot, no physical address is allowed from NVS. At the **first actual RPM/vehicle-speed demand**, send the already-authorized functional `18DB33F1:01 0C 0D` (never at power-on). Validate every returned ECU has a complete, RAW-backed pair, no duplicates or partial/negative returns, and **the exact same full responder set** as the NVS hint.
- If and only if the complete current-boot set matches and the selected ECU replied, reuse the saved selection provisionally and skip the remaining seven selection probes. That reply also populates the existing **boot-local CAN-owner observed-ECU allowlist**. Next request uses the physical CAN ID; successful positive physical result confirms it.
- On mismatch, bad CRC, old schema, incomplete functional response, or an unconfirmed physical reply failure, discard the cached choice in RAM and run the original bounded eight-probe selection. Physical BUS-OFF/lease/driver errors remain fail-closed; they do not trigger additional probing. After confirmed selection, retain PR #56's three-failure / one recheck / legacy fallback behavior.
- This optimization is not guaranteed: identical ECU sets across different cars cannot be distinguished by this evidence alone. The physical request remains **read-only to a source actually observed in the current boot** and is always checked for response provenance. A VIN-based identity or manual vehicle selection would need a separately reviewed product and safety decision; **neither is added**.

## Optional read-only BLE status (no UI clutter)

In `m5dial_ecu_hint_lab`, a peer can issue **`ATM5ECU\r`** after its normal identity handshake. M5Dial responds with exactly one separately framed line and ELM prompt:

`M5CAN-ECU V=1 STATE=DISCOVER|FIXED|RECHECK|LEGACY SOURCE=XXXXXXXX PROBES=n RECHECKS=n CONFIRMED=0|1\r>`

- The query is **read-only**: no TX lease, no CAN request, no NVS mutation and no cross-peer data. `SOURCE=00000000` means no pinned source. `CONFIRMED=0` while a cached selection awaits its first physical proof.
- No unsolicited status is inserted into `ATM5B`, `M5ITEM`, `M5DONE` or `ATM5CAP`. The existing capability version, BLE GATT contract and legacy meter parsers are unchanged. Optional diagnostic UI can be a future issue if desired.
- This command is gated by the new build flag, not advertised as a standard ELM327 command, and is **not** used for selecting or authorizing an ECU.

## Review / acceptance gates

1. Host tests: CRC/torn record/unknown version/empty/duplicate sets; byte order, value disagreement, RAW-vs-virtual, saved vs current mismatches; CAN-owner current-boot authorization; initial physical timeout immediately returns to bounded discovery; confirmed selection fallback unchanged.
2. Simulated dual BLE host test: initial 8-probe discovery, physical confirmation and one NVS write, reboot with identical set -> exactly one functional probe -> physical request; changed set -> normal 8 probes; status command must not transmit or interfere with another peer.
3. PlatformIO **build only**: `m5dial_ecu_hint_lab`, existing M5Dial, dual-BLE, V2, full stack budgets. Never upload CI artifacts automatically.
4. Isolated physical CAN bench (owner-operated): verify boot=0 TX, 50ms preservation, authentic frame count (do not equate UART notifications with CAN frames), NVS power-cycle retention, flash checkpoint while both BLE peers remain connected, stale/corrupt cache fails closed, and bus-off/lease refusal.
5. Owner-operated parked vehicle trial *only after independent approval*: instrument `#ECU_HINT`, `#ECU_STATE`, `#ECU_CAND`, `#ECU_TRAFFIC`; compare boot A vs boot B, then another vehicle (after cold power cycle). Evaluate actual physical response IDs/latencies, RPM/speed integrity and missing frames. Maintain full immutable RAW logging and protect NVS, BLE bonding, existing partition/SPIFFS/CANLOG.

No production merge, OTA, erase, flash migration or real vehicle transmit is permitted by this PR.
