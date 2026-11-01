# P3: optional O-2 profile-binding metadata / explicit ACK v1

Design: [iOS 24fbd7d8308729ecfa69cae3cdc3fd402af1cff0](https://github.com/papataku/tacotaco-meter-ios/blob/24fbd7d8308729ecfa69cae3cdc3fd402af1cff0/docs/protocol/vehicle-profile-ecu-id.md), §1/2 receiver-binding/7 P3–P4/8/9 O-2.
P1: [TacoTacoMeter cc8caca63db99e51dde7a5518c9a939d29a5ec6a](https://github.com/papataku/TacoTacoMeter/commit/cc8caca63db99e51dde7a5518c9a939d29a5ec6a), PR #143.
P2 / immutable compatibility base: [18188eed1b45534a7aadc6a2c390f2d594d92921](https://github.com/papataku/esp32-obd2-can-adapter/commit/18188eed1b45534a7aadc6a2c390f2d594d92921), PR #73.

## Scope and legacy isolation

Only CAN + RP8 images implement optional Control **op11**. Other images return
Unsupported. Existing HELLO, PROFILE (subscription plan), DATA, Snapshot,
heartbeat, Control ops1–10, service/advertising and fragmentation bytes stay
unchanged. No unsolicited capability PDU is sent. Today's Ring
0332168/cc8caca never sends op11 and follows the exact existing RP8-only path.
The existing V2 PROFILE revision is a subscription revision, **never vehicle
identity**. It remains the expectedRevision in the Control/ACK envelope.

op11 is a **read-only metadata capability**, with an explicit version and
single-use ACK of an adapter-issued tuple. It cannot import/select/switch a
profile, renew a lease, ARM, request acquisition, or grant dynamic LIVE/TX.
Only built-in `legacy-rp8` exists. Its agreed opaque local alias is not a
vehicle identifier. No random vehicle ID, VIN, BLE address or raw fingerprint
is sent or stored. Capability flags explicitly state **dynamic activation
unsupported**. This is deliberate: sufficient O-1 fingerprint evidence and
current-session stationary owner confirmation do not exist in adapter P3.
An ACK is receipt of metadata, not approval, proof of vehicle identity, or Bind.

A client can inspect/ACK metadata alongside an already active *legacy* session;
legacy authority remains governed by its existing independent policy. Sending
op11 does not convert that session to a dynamic profile or suspend/regrant its
legacy authorization. Unknown version/revision/ID never yields a dynamic
profile, including after ACK. A capable P4 receiver must keep dynamic binding
Unbound when the necessary evidence/owner confirmation is absent; it must not
silently interpret the legacy path as successful dynamic binding.

## Wire

Use the existing framed/CRC32 Control PDU type1, same secured registered link,
STATUS indication ACK type80, requestID, adapter boot/session and
expectedRevision checks. All integers little endian; reserved bytes zero.
No new characteristic, larger MTU, NUS, or discovery TX. Max op stays 11.

op11 body: `bindingVersion:u16=1, action:u8, reserved:u8=0, body...`.

| action | request length | result body on Accept |
| --- | --- | --- |
| 0 Negotiate | 4 | version:u16=1, tupleLength:u16=68, capabilities:u32=1 |
| 1 Offer | 4 | the 68-byte current tuple |
| 2 ACK | 72 | exact 68-byte offer echo; result repeats that same tuple |

Capability bit0 means metadata offer/ACK; bit1 would mean dynamic activation
and is **0**. Unknown version/action => Unsupported; bad length/reserved =>
ProtocolError or Reject. Offer/ACK require negotiation. One negotiation,
one offer, one accepted ACK per Session. Every op11 duplicate rid is rejected
(ProtocolError while cached, ResultUnknown after history eviction); a new-rid
second ACK or offer is Reject. Normal legacy duplicate handling is unchanged.
All existing authorization, boot/session, revision and monotonic rid checks
precede this extension. ACK compares **all bytes** to both the issued offer
and freshly projected current context. Epoch/source/mode/evidence changes,
forged references, profile/version/schema/revision changes are Reject.
Reconnect/invalidate clears all extension state. Refresh requires a new session;
there is no in-session profile switch or downgrade operation.

| offset | bytes | tuple field |
| --- | --- | --- |
| 0 | 2 | bindingVersion=1 |
| 2 | 2 | profile schemaVersion=1 |
| 4 | 4 | profile revision=1 (independent of V2 PROFILE revision) |
| 8 | 16 | opaque local profileId, `legacy-rp8` then six zero bytes |
| 24 | 4 | adapter boot |
| 28 | 4 | adapter protocol session |
| 32 | 4 | adapter authorizationEpoch |
| 36 | 4 | sourceGeneration |
| 40 | 4 | modeEpoch |
| 44 | 1 | evidenceKind: 0 unavailable, 1 current-session legacy acquisition reference |
| 45 | 3 | reserved zero |
| 48 | 4 | acquisition ordinal (existing CanSamples.acquired) |
| 52 | 4 | existing RPM sample sequence |
| 56 | 8 | existing RPM observed monotonic microseconds |
| 64 | 4 | full Extended29 response route (never truncated) |

Kind1 is only projected when this exact authenticated active Session/link
generation owns the current armed epoch, owner is not faulted, and its existing
RP8 RPM sample is valid and 0–600ms old. Otherwise **all evidence fields are
zero**. Another peer's samples, saved profile evidence hash and firmware hint
cannot become current-session evidence. This reference locates an acquisition;
it **does not assert O-1 fingerprint match**, supported bitmap, physical vehicle
continuity, stationary confirmation or signal correctness. The existing
acquisition may include a probe/group already permitted by legacy policy;
P3 never sends requests to gather missing data. No sample bytes/raw logs are
carried. ACK validity is checked at processing time; consumers must invalidate
on subsequent context change rather than caching ACK as ongoing authority.

## P4 receiver requirements

1. Keep current Ring legacy behavior explicit. Use authenticated op11 only when
   implementing this capability; validate version, capabilities, exact lengths,
   reserved bytes, opaque profileId/schema/revision independently of subscription
   revision. Unsupported/unknown/mismatch => no dynamic profile, Unbound/Invalid.
2. Bind the whole tuple to the receiver's own connectionGeneration. Echo it with
   action2/new rid and require matching Accept ACK and envelope boot/session /
   subscription revision. ATT indication confirmation alone is not tuple ACK.
   Discard stale/duplicate ACK, DATA/Snapshot and prior UI events on disconnect,
   session/epoch/source/mode/profile change; clear caches/peaks/pins immediately.
3. Independently verify the full 29bit route/PID, current-session provenance,
   freshness and profile store revision. Kind0 is missing evidence. Kind1 is
   only an acquisition reference, insufficient for O-1 composite fingerprint.
   Do not turn firmware provenance, subscription revision or matching CAN ID
   alone into a vehicle identity or profile-match result.
4. Dynamic rebind still requires sufficient current-session compatibility
   evidence and stationary explicit owner confirmation. Unknown/stale/moving
   speed keeps confirmation locked. No automatic rebind/continuity exception.
   P3 advertises dynamic activation unsupported; P4 cannot enable it by ACK.
   Additional evidence acquisition or adapter dynamic activation needs a separate
   adapter review; it must not be invented through new CAN requests in P4.
5. Preserve 400/600ms Ring quality limits, existing five-PID/29bit allowlist,
   registration/MAC privacy, epoch/STOP behavior. Recheck #142 Snapshot adoption
   at P4 start. RF/LCD/touch/vehicle acceptance remains separately required.

## Reproduction / evidence

- `bash tests/run_p3_binding.sh`: actual codec/fragmentation + Session, host
  sanitizers and host-executed real CanOwner/CanMonitor/serial-TWAI shim;
  adversarial ACK cases and dynamic demand/queue/driver TX attempts all zero.
  No port or device is opened.
- `python3 tests/test_p3_binding_context.py`: execute the exact production GATT
  evidence projection; reject cross-owner/session/generation/epoch, stale,
  future, invalid, wrong route and stopped evidence.
- `python3 tools/test_p3_legacy_equivalence.py --output ../evidence/p3-legacy`:
  separately archive/compile fixed base 18188ee and head; compare full legacy
  ACK/DATA/Snapshot/heartbeat bytes plus active/lease/revision/demand across
  CAN and DEMO, duplicates, expiry, pause/resume and disconnect.
- `python3 tools/test_p3_legacy_equivalence.py --mutation-proof`: expected exit1;
  temporary HELLO maxPDU 512→511 changes the compared bytes. See
  [failing log](../evidence/p3/mutation-fail.txt). Source tree is untouched.
- `python3 tools/test_p3_scope.py`: fixed-base allowlist + reconstruct existing
  Session/GATT by removing precisely the metadata additions; freeze all other
  production files, TX/auth/lease/budget/settle/STOP/advertising/envs.
- Existing `bash tests/run_all.sh` includes P2 TX equivalence. New P3 suite also
  runs there. Build-only matrix: `python3 docs/evidence/p2/build_matrix.py`.

All fixtures are synthetic. Physical serial HIL, BLE/RF, devices, flashing,
bench CAN and vehicle tests are **NOT RUN**. Host execution of the serial shim
is software evidence only. See [recorded results](../evidence/p3/results.md).
