# Signal V2-2 serial HIL runbook (N1 PR-D substitute)

Owner decision, 2026-10-29: no external CAN analyzer will be bought. This runbook replaces the "isolated instrumented bench" from `docs/protocol/signal-v2-2-bounded-ecu-discovery-pr-a.md` §6 for **software behavior**.

## What the image is

The image is `m5dial_signal_v2_can_29bit_serial_hil`.

- **Same production code:** it runs the production V2-2 stack on the M5Dial: BLE V2 sessions, `CanOwner`, `CanMonitor`, both budget gates, epochs and STOP.
- **TWAI replaced:** only the TWAI driver calls in `src/can_monitor.cpp` are redirected to `src/twai_hil.*`.
- **No physical CAN:** the TWAI peripheral and CAN GPIOs are never initialized. CI proves the ELF links no `twai_transmit`, `twai_driver_install` or `twai_start` (`tools/check_signal_v2_elf.py`).
- **Not a vehicle image:** it cannot read or write a real bus.
- **Wire protocol:**
  - Adapter TX → USB line `#HIL_TX,t_us=…,id=…,ext=…,dlc=…,data=…`.
  - PC → adapter: `HIL RX <id> <ext> <hex>`, `HIL TXFAIL n`, `HIL BUSERR n`, `HIL RXMISS n`, `HIL BUSOFF`, `HIL RESET`, `HIL STATUS`.

## Procedure (PM/Codex at the desk; never in a car)

1. Back up the M5Dial flash first, then write this image. Use the same backup commands as the passive kit's flash plan; no whole-chip erase.
2. Connect a V2 Central (iPhone or SmartRing) securely and make it demand signals. This is needed because `SIGV2 CAN ARM` requires a secured, active peer.
3. Install pyserial on the PC (`pip install pyserial`). Then run each scenario, rebooting the M5Dial between scenarios:
   `python3 tools/signal_v2_hil_ecu.py --port /dev/cu.usbmodem1101 --scenario <name> --seconds 8`
4. Scenarios:
   - Healthy: `normal`, `split`.
   - Must STOP: `noreply`, `extra`, `ff_probe`, `late`, `duplicate`, `wrong_src`, `eleven_bit`, `negative`, `txfail`, `busoff`, `rxmiss`.
5. Each run prints JSON. A pass requires all of:
   - STOP exactly when expected;
   - zero TX after STOP;
   - zero functional `18DB33F1` TX;
   - no Flow Control answering a probe First Frame;
   - the healthy run pins `18DAF101`;
   - independent PC-side load ≤ 5,000 bits per 100 ms and ≤ 25,000 bits per 1 s.
6. Keep the JSON outputs and the adapter `#SIGNALV2_LOAD` lines as evidence. Do not commit raw logs that contain identifiers.

## Limits (state these in every report)

- The PC-side load counts only the bridged frames, at 200 bits each. It is not a wire measurement: no bit stuffing, retransmits or other nodes.
- USB-serial latency (a few ms) is added to the response timing. Timing-edge results near 120 ms are indicative only.
- What a real ECU actually does (how many replies it sends, its timing) is verified only in the staged real-car tests S1/S2/S3 in the vehicle runbook, by the adapter's own counters.
