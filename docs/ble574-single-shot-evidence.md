# 574 single-shot evidence and stop gates (bench only)

Baseline devices: M5 deployed da7656 -> app-only observer 371445bd; Ring
8ea099e. The observer binary is locally built with the previously approved
private BLE peer identity, not the test identity used in GitHub CI. This
document does not authorize a new image or suggest a full-chip erase.

## Scope and sequencing
1. Record current Ring/M5 link from natural USB output only. A planned
   M5 app update breaks the previously stable connection; record that fact
   as expected, not an RF-failure reproduction.
2. Confirm offline manifest for exactly M5 firmware SHA
   f45798c70164058d9147a4444f5015ba098213adefd8b11e01fc6841756defff,
   Git commit 371445bd007779fc8b037faa2294e207ea2616fc,
   archive partition SHA
   7891dd9f69f2da4a49b4ed63ec4bd1651f9258d0f0787249e3f7bae65591645f.
   Before any hardware write: independent read-only address/layout check:
   factory app 0x10000, length 0x200000; expected 875472-byte image fits.
3. Under the user's bench-wide approval, flash that app region only, once.
   The accompanying planned M5 reboot is expected. Do not use a PlatformIO
   generic upload target that might write bootloader, partition table, boot
   parameters, otadata or NVS. No migration, erase, format, bootloader/
   partition/otadata/canlog/spiffs/coredump/SD writes. Preserve A2/104-files/
   2-part and all RAW evidence. If memory layout or manifest differs from the
   installed archive, stop before write.
4. Verify read-back SHA of app and exact pre-/post non-app MD5 equality. On
   disagreement, stop; no blind retry or repair by erase. Do not mark
   firmware update PASS from boot messages alone.
5. Observe M5's complete natural #BLE_EDGE and #BLE_EDGE_TASK lines.
   Expect initial con=0,ok=0,err=0,disc=0; verify normal #BLE_DUAL
   clients=0 and no surprise auto-connect before any Ring command.
   If spontaneous connection, overflow, too many clients (>2) or missing
   evidence: stop without another command.
6. Capture both USB byte streams exclusively, with no DTR/RTS toggles.
   Ring command 'b' once; require complete SCAN V3, matching the known
   M5 address/type0 and an unambiguous first ELM target for '|'. If uncertain,
   stop. Otherwise send '|' once (never 'c').
7. Record complete RING_EST_TRACE_V2, BLE diagnostics (partial lines do NOT
   count), M5 #BLE_EDGE, #BLE_EDGE_TASK, #BLE_DUAL, and natural events.
   Stop after first successful attempt or any failure including 574; no
   connection retry, no extra scan/connect, no log export, no additional flash.
   Keep maximum 2 BLE peers and no vehicle/OBD connected.
8. Preserve unmodified private RAW and SHA256. Separately, run read-only:
   python3 tools/analyze_ble_linkedge_single.py --ring /private/path/SmartRing.raw --m5 /private/path/M5Dial.raw --output /private/path/ble574-summary.json

## Evidence interpretation

- GAP CONNECT on Ring with status 0 proves that event occurred; it is not
  evidence of peer application connection by itself.
- V2 final=574 or linked=0 is FAILURE and triggers stop.
- V2 success and M5 client count increment establish only this single
  connection; no RF root-cause or endurance PASS follows.
- Missing/partial serial lines, no joint timebase, and unobserved M5 GAP
  events must be explicitly labelled unknown. Model counters are NOT
  over-the-air packet capture.
- On any gate failure, stop and return the raw evidence and SHA hashes to
  the Web-side investigation. Do not auto-correct, reflash or retry.

This analysis branch is separate from the installed and validated M5
371445bd image. It changes no product firmware and must not be flashed.
