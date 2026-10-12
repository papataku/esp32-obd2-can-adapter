# BLE packet-pool exhaustion investigation (2026-10-13)

## Cause and correction

The dual-meter notification producer could exhaust NimBLE's shared MSYS
packet pool. `ble_gattc_notify_custom()` returning success includes packets
accepted into the connection's host queue when the controller cannot yet
send them. The old producer kept feeding that queue in synchronous 20-byte
chunks. Allocation then failed, and the whole-write 100ms deadline could
request a local disconnect. This is packet-pool pressure, not evidence of
a general heap leak. Original reason6 records alone cannot distinguish
allocation failure from an API failure; the diagnostic image separates them.

The fixed image is source `86466b8593bb2a2a4bb86280b9cd0f5a03bfed04`.
Its normal dual transport uses a fixed4096-byte FIFO per peer, alternates
service order, and attempts one chunk per peer per poll without blocking.
It stops filling NimBLE when a peer already has3 host-queued packets or8
total outstanding packets, or fewer than6 MSYS blocks remain. Producer
backpressure preserves complete response/prompt order; reconnect generation
checks discard old output. Permanent errors, overflow and a real5s stalled
peer remain explicit faults. See [flow control](ble_output_flow_control.md).

NimBLE-Arduino is pinned to1.4.3 because the isolated, read-only credit check
uses its connection state under the host mutex. The library is not patched.
The old synchronous path is available only in the TX-disabled diagnostic
baseline. Normal single-client firmware and CAN safety gates are unchanged.

## Physical comparisons

All comparisons use actual SmartRing and JC3636W518C meters with DEMO and
physical CAN TX compiled out. High preset requests20/20/20/1/1, auto0;
the current meter scheduler's100ms minimum means approximately10Hz fast
signals, not20Hz acceptance. Counters below are deltas inside each measured
window; opening resets and explicit setup/cleanup disconnects are excluded.

| Image / policy | Duration | Allocation failures | API failures | Unexpected cuts | MSYS minimum |
| --- | ---: | ---: | ---: | ---: | ---: |
| Diagnostic `ef920a1` |600.138s|6417|0|0|0|
| Synchronous backlog baseline `1a587ab` |180.184s|4665|0|0|0|
| Fixed pipeline `86466b8`, Ring first |600.039s|0|0|0|4|

The final fixed Ring-first run also had0 FIFO overflows, stalls, RX overflows
and MCU resets; controller+host backlog peaked at8 and host queue at3.
RAM output peak was203 bytes and BLE worker peak1415us. Before a UI policy
change, both meters delivered approximately9.99Hz RPM. JC switched to
SPEED/automatic acquisition at375.46s; the user confirmed screen/settings
operation. JC's overall7.094Hz RPM mixes foreground and background policies
and must not be presented as a transport regression. The conservative
365.323s matched-high interval measured9.987Hz; Ring overall measured9.986Hz.

Tighter intermediate windows removed allocation failures but reduced
throughput: window4 (Ring9.349/JC9.246Hz), window6 (9.782/9.347), window8
without the final preloaded pipeline (9.297/9.781). These are superseded
experiments, not the deployed final behavior. A later burst experiment was
reverted before flashing; final C++ remains exactly the tested86466b8 image.

## Long-run qualification

The JC-first2h attempt started2026-10-12T22:51:56Z and stopped after448.984s
because the JC USB reader reported `Device not configured`. It is INVALID
as a2h acceptance run, regardless of its0 observed cuts/allocation/API errors.
The next read recovered the same test boot's retained record through620992ms;
no crash/reset is established at the USB interruption. Subsequent USB opens
caused recorded warm recoveries. Both journals exported8 retained boots plus
RAM with CRC/ACK validation and0 collector/corruption errors; old core dumps
have different ELF prefixes from the current meter image.

A fresh JC-first2h run is in progress. Its bench preset is volatile and is
reapplied if UI operations change acquisition policy; each reapplication is
recorded. Completion, capture coverage and fault counters must be assessed
before merging. Green CI or a short error-free sample does not satisfy this.

Host regressions cover shared-pool reserve, real queue-credit predicates,
one blocked peer while the other progresses,200ms recovery, byte ordering,
wrap, stale generations, overflow/quarantine and5s stalls. The maximum RAW
fixture verifies all48 frames for16 items (768 frames) without physical CAN.
Current CI and the complete host/sanitizer suite pass at`e972a31`.

Residual qualification remains: vehicle CAN/multi-ECU/ISO-TP and electrical
bench (adapter23/24), first-critical retention/cold power loss (adapter30),
meter rate/missed/fast-disconnect retention (meter98/92), standalone@ USB
compatibility (meter97), LCD/touch/60fps observation (meter93), remote artifact
quota (meter94), and iPad/three-device investigation (adapter21/meter95).
This test excludes iPad and cannot close its original disconnect scope.
