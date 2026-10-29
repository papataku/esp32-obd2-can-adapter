# Signal V2-2 N1 / PR-A — evidence-bounded physical ECU discovery (docs-only)

Status: **SPECIFICATION ONLY / NO-GO FOR ACTIVE VEHICLE CAN TX**. This document does not authorize ARM, CAN TX, firmware installation or vehicle use. PR-B/PR-C must implement and independently validate it. Scope: owner's RP8 evidence, 29-bit Classical CAN at 500 kbit/s, initial 5 Hz trial only; other vehicles have **no approved candidates**. Neither a historical source ID nor a stored hint is a physical-TX authorization.

## 1. Exclusive provenance and exactly observed 29-bit responders

Only these three owner-log-derived records are admissible evidence for *candidate identity*, not permission to transmit or proof of the physical response ceiling:

1. [Owner DB re-verification (2026-10-18)](https://github.com/papataku/tacotaco-meter-ios/blob/main/docs/qa/rp8-log-verification.md): read-only `immutable=1` recount of the supplied DB. It reports 4,593 parent BATCH calls; a parent BATCH can be a multi-ECU, multi-PID exchange. No raw vehicle/owner identifiers are reproduced here.
2. [ECU selection/provenance](https://github.com/papataku/tacotaco-meter-ios/blob/main/docs/protocol/ecu-selection-provenance.md): actual five 29-bit reply IDs, per-source PID counts, RPM contradictions and provisional preference for ECU01. All ECU/PID mappings remain **unapproved** until independent correctness and selection proof.
3. [OBD-II standard PID bitmap checks](https://github.com/papataku/tacotaco-meter-ios/blob/main/docs/protocol/obd2-standard-pids.md): `0100` support for RPM `0C` and speed `0D` is present on all five observed sources; only ECU01/02/06 advertise water `05`. Bitmap support is not proof of valid physical acquisition.

| Observed responder (29 bit) | Owner-log evidence, Service01 | PR-A physical exploration disposition |
| --- | --- | --- |
| `18DAF101` | `010C` 4,593, `010D` 4,590, `0105` 4,591 completed PID replies; `015B` 4,589; `019A` 4,593 (multi-frame). Only documented provisional RP8 source preference. | **Only proposed RP8 candidate**; physical request `18DA01F1`; **not authorized for TX yet**. |
| `18DAF102` | `010C` 3,828, `0105` 4,584; `0100` advertises `0C/0D/05`. | Observed, not in this PR-A active candidate set. |
| `18DAF106` | `010C` 3,828, `0105` 4,583; `0100` advertises `0C/0D/05`. | Observed, not in this PR-A active candidate set. |
| `18DAF10E` | `010C` 3,831; `015B` 4,589; `0100` advertises `0C/0D`, but not `05`. | Observed, not in this PR-A active candidate set. |
| `18DAF1EF` | `010C` 3,831; `0100` advertises `0C/0D` but not `05`. The DB has one BATCH where EF alone supplied a speed reply. | Observed, not in this PR-A active candidate set; **not evidence that EF is irrelevant**. |

In the cited DB, other-ECU speed counts are approximately 3,828 per ECU, not a verified per-physical-query response bound. In one BATCH (the re-verification notes five cases of ECU01 reporting RPM 0 while others reported nonzero), candidate RPM readings disagree; the independently correct RPM is **unknown**. No shortest-ID, first-arrival or majority vote is permitted.

**Proposed eligible candidate set for this RP8 profile: `{18DAF101}`, cardinality 1 of permitted maximum 4.** This is a *reviewable selection preference*, not a verified/pinned ECU and not a transmit allowlist. All five observed IDs remain disclosed to reviewers. Four other observed responders are deliberately **not silently discarded as non-ECUs**; selecting any other subset, adding candidates or replacing ECU01 requires separate owner approval with an evidence-bound profile revision. No arbitrary choice of four among the five. Unknown vehicles: empty set, `UNCONFIRMED`, fail closed. Do not infer additional addresses by sweeping an ID range.

## 2. Induced-response contract and evidence gap

Proposed bounded transaction (subject to a separately verified physical contract): **one** 29-bit physical request to a single, individually authorized candidate, Service01 `0C/0D` only, at most **two total CAN Single Frame responses** from that candidate to this request (either one combined 6-byte `41 0C A B 0D X` response or separate correct one-PID replies). Reserve four 8-byte frames: TX request 1 + RX maximum 2 + one safety allowance = **800 bits**. Do not send any ISO-TP Flow Control in discovery. First Frame, Consecutive Frame, unexpected extra response, duplicate PID/source, unsupported negative result, wrong reply ID/format or capacity overflow => transaction invalid, revoke TX, STOP; do not assemble a success.

**Evidence status: UNCONFIRMED.** The three cited documents count *decoded PID responses* inside historical functional BATCH commands, explicitly counting `FF/CF` as one completed response. They do **not** provide a per-request, physical-addressed RAW CAN capture proving a maximum of two Single Frames, a maximum of one physical ECU transmitting, or absence of late/duplicate replies. In particular, `019A` has observed multi-frame replies and is excluded from discovery. It is incorrect to infer "two SF guaranteed" from `010C/010D` support bits, completed-response counts or a selector array limited to eight items. The proposed cap remains a **contract requiring proof**, not an established fact or an instruction to reserve less.

PR-B must NOT enable vehicle TX solely from this document. Before owner approval, an independent isolated instrumented bench must establish the physical source, frame layout and bounded response per request, including late/duplicate/error injections; a vehicle-specific contract/proof version must be explicitly approved. If evidence is unavailable, mark `UNCONFIRMED` and STOP. The adapter cannot prevent a defective external ECU from over-responding: count actual received traffic including late frames, lock out further TX when the bound is broken, and do not claim guaranteed global bus-load control.

## 3. Admission and worst-case 29-bit / 500 kbit/s calculation

For one Classical CAN extended 8-byte data frame: bit-stuffed region is 118 bits (SOF through CRC sequence), maximum additional stuff bits `floor((118-1)/4)=29`; CRC delimiter + ACK + EOF + 3-bit intermission = 13 bits. Hence **160 bits/frame** is the theoretical per-frame upper bound, including worst-case bit stuffing and intermission. Retain the existing **200 bits/frame** conservative admission cost, with no reductions.

| Item | Maximum reserved frames | At 200 bits/frame | At 160 bits/frame |
| --- | ---: | ---: | ---: |
| One physical discovery | 4 | 800 bits | 640 bits |
| Four discoveries in one admission sequence | 16 | 3,200 bits | 2,560 bits |
| First regular physical acquisition (separately admitted) | 12 | 2,400 bits | 1,920 bits |
| Conservative 100 ms rolling-window overlap envelope (two discovery reservations + regular acquisition) | 20 | **4,000 / 5,000 bits** | 3,200 bits |
| Conservative 1,000 ms rolling-window cumulative envelope (four discoveries + regular acquisition) | 28 | **5,600 / 25,000 bits** | 4,480 bits |

The **4,000 / 5,000** and **5,600 / 25,000** figures are envelopes, not measured vehicle traffic and not a budget exemption. Bound requests to at least **200 ms between starts**, individual response window <=120 ms, strictly sequential completion/drain before another query, and the existing reservation's duration carried across rolling windows. Enforce both existing admission controllers: upper CanOwner and CAN Monitor before actual TX, counting other admitted activity, actual candidate RX/FC/late traffic, prior reservations and failed TX conservatively. If the rolling remaining budget cannot admit the full next transaction, do not transmit; STOP/Invalid as specified, not a partial charge. There is no functional discovery escape hatch. The previously rejected broadcast reservation is `(1+256*2)*200=102,600 bits` and remains prohibited.

## 4. Approval, prohibitions, and current-epoch pin

Distinct gates (all must pass):
- **Candidate-list approval:** owner signs profile version and applicability, `{18DAF101}` or a separately reviewed expansion to **no more than four observed IDs**. Record sanitized evidence references, supported PIDs, intended use and an independent reason to prefer the source; log frequency / minimum ID alone is insufficient. Vehicle identity, BLE address, bond, VIN, private raw DB and owner metadata are not committed.
- **Physical response-cap approval:** independent bench RAW/analyzer proves one physical request cannot normally induce more than the allowed two SF, no FC; verify the same contract on fault injections and have a safe abort for violations. **Not yet proven from cited files.**
- **Source correctness approval:** independently validate that the candidate reports physically meaningful speed and RPM despite ECU01-vs-other ECU disagreements. Support bit / decoded `41` data alone is insufficient. If more than one authorized ECU meets the selection contract without a justified profile preference, `Rejected(Ambiguous)`, Invalid and STOP.
- **Transaction/runtime approval:** explicit stationary owner ARM, secure peer and accepted profile, 29-bit route, source-specific physical request/response binding, zero other CAN TX until permitted, both rolling budget checks, fault/error counters clean; lock out while driving or if safety state is unknown.

After each genuinely new ARM epoch, clear selection, current pin and freshness; never reuse a pre-STOP pin, outstanding response, NVS or cached physical source as authorization. Only current-epoch RAW-backed same-source observations with matching response CAN ID, PID, lengths, frames, acquisition time and selector/profile make a **new** pin. Reject old/late responses, duplicate requests and replayed/uncorrelated Snapshot. Pin loss, bus fault, budget violation, ambiguity, wrong width, response-cap breach or three-second timeout latches STOP and Invalid. No automatic functional fallback, rearm, repin, inferred default ECU, hardcoded physical address outside the approved profile, or admission bypass. V1/default and V2-1 hard CAN-disabled builds retain their safety boundaries.

The separate iOS G0 source-approval thresholds (10 consecutive valid and 95/100 within a defined window) are **not proven by the historical whole-log aggregate** and cannot be completed in a 3-second trial. Do not portray the first acquired sample as completing full G0 source approval. Keep research-only signals `015B/019A` from automatic LIVE promotion.

## 5. First three seconds: owner runbook handoff, future gate only

Prerequisites: PR-B implementation and PR-C epoch/Snapshot review, green host/virtual tests, independent CAN analyzer (stuffed bits and all rolling windows) on isolated bench, documented physical two-SF response evidence, independent ECU correctness proof, exact FW/profile/hashes and explicit owner review. **Until then STOP at passive verification (phase 3), NO vehicle ARM.**

At approved stationary trial: boot unarmed with every CAN signal Invalid; vehicle P/brake/stationary and independent monitor present. At explicit owner ARM start the 3,000 ms monotonic deadline and a fresh generation. For the current proposed single-candidate profile, check only its individually admitted physical `0C/0D` request; no functional TX; if a future approved set has up to four, poll serially >=200 ms apart, no more than four. Establish exactly one separately approved, freshly RAW-verified ECU pin. Then separately admit the first regular physical acquisition; deliver current-epoch CAN sample with acquisition age/provenance and require genuinely Fresh speed **zero** and required RPM. A diagnostic reply is not itself proof the vehicle is parked.

If no approved bounded candidate/response contract, if source ambiguity is unresolved, if budget is denied, or if there is no verified Fresh zero speed by 3,000 ms, issue STOP, revoke TX, keep Invalid, record only redacted evidence. No retry loop and no rolling back into functional discovery. Passing stationary verification is not automatic permission for idle, low-speed, 10 Hz or unsupervised trials.

## 6. PR-B / PR-C obligations and test plan

- **Host (PR-B):** authorize only explicit profile candidate(s), cap <=4 and no ranges, no implicit functional; enforce one request and at most two SF/no FC, 4-frame precharge at BOTH gate sites, 200 ms separation, 120 ms bound, rolling 100/1,000 ms windows with retained prior reservations, preexisting load, overflow, late traffic, 0/1/4/5 candidates and STOP/ARM edges.
- **Virtual ECU (PR-B+C):** 0/1/2/3 replies; unexpected FF/CF, duplicate, wrong-source/width, ambiguous known responders, delayed/late reply crossing window and STOP, no response and 11-bit responder; assert no incorrect Fresh, no automatic functional, no unintended TX or repin.
- **Isolated instrumented bench (PR-D acceptance):** separate physical simulator + independent CAN analyzer with loss/error tracking; verify actual wire bit stuffing, retransmits and 100 ms/1 s sliding maxima, FC TX exactly zero during discovery, epoch/STOP no-TX, request-to-first-Fresh within 3 s where possible, and fail-closed outside the approved bound.
- **PR-C:** pin/generation/source-epoch and current-frame evidence binding, cross-session Snapshot age/replay guards; run matched SmartRing #139 acceptance. Only a separately reviewed bench and owner approval can lift this document's NO-GO.

## 7. Open questions / STOP reasons

**UNCONFIRMED:** max two physical SF and no additional ECU RX per query; validity/correctness of ECU01 amid observed conflicting RPM; whether excluding other four observed responder IDs could hide a needed ECU; representative physical response timing; late/duplicate response bound; independently measured CAN load under realistic competing traffic. Every uncertainty fails closed; **this PR-A is a specification, not a completed ECU or bus-load approval**.
