#!/usr/bin/env python3
"""Offline, read-only validator of multi-ECU CAN/BLE bench evidence.

This tool does not open a CAN/BLE interface, produce messages, or approve a
vehicle release. Synthetic fixtures are *never* hardware acceptance evidence.

Input: newline-delimited JSON. First line metadata:
  {"type":"meta","schema":1,"profile":"passive_tx_off"|"isolated_can_bench",
   "evidence":"synthetic"|"hardware","firmware_sha":"40-char SHA",
   "capture_sha256":"64-char SHA", "observer":"...", "interface":"..."}
Following lines:
  {"type":"event","t_us":0,"kind":"ble_connect","conn":7,"generation":1}
  {"type":"event","t_us":10,"kind":"request_begin","id":"r1",
   "conn":7,"generation":1,"can_id":"18DA02F1","service":"22","did":"2012"}
  {"type":"event","t_us":20,"kind":"can_tx","id":"r1","frame":"request",
   "can_id":"18DA02F1"}
  {"type":"event","t_us":30,"kind":"can_rx","can_id":"18DAF102",
   "pci":"ff","service":"62","did":"2012"}
  {"type":"event","t_us":40,"kind":"can_tx","id":"r1","frame":"fc",
   "can_id":"18DA02F1"}
  {"type":"event","t_us":50,"kind":"can_rx","can_id":"18DAF102",
   "pci":"cf","seq":1,"complete":true}
  {"type":"event","t_us":60,"kind":"request_finish","id":"r1","status":"ok"}
  {"type":"event","t_us":70,"kind":"ble_deliver","id":"r1",
   "conn":7,"generation":1,"status":"ok"}

Validation is intentionally narrow: 29-bit physical UDS22 read-only, exact
ECU pairing, and BLE session generation. Other frame layouts require a new
schema/version with independent review, never silent permissive fallback.
"""

import argparse
import json
import re
import sys
from pathlib import Path

ALLOWED = {"18DA01F1", "18DA02F1", "18DA06F1", "18DA0EF1", "18DAEFF1"}
HEX4 = re.compile(r"^[0-9A-F]{4}$")
HEX8 = re.compile(r"^[0-9A-F]{8}$")
SHA40 = re.compile(r"^[0-9a-fA-F]{40}$")
SHA64 = re.compile(r"^[0-9a-fA-F]{64}$")
# See kDiagnosticMaxPayload and IsoTpAssembler default CF timeout.
MAX_REASSEMBLED_PAYLOAD = 96
CF_TIMEOUT_US = 100_000


class EvidenceError(ValueError):
    pass


def need(condition, message, line):
    if not condition:
        raise EvidenceError(f"line {line}: {message}")


def int_field(event, name, line, minimum=0):
    value = event.get(name)
    need(type(value) is int and value >= minimum,
         f"{name} must be an integer >= {minimum}", line)
    return value


def pair(req_id):
    return "18DAF1" + req_id[4:6]


def raw_can_payload(event, evidence, line):
    """For hardware evidence, derive PCI/services from actual captured bytes.

    Descriptive event labels are not adequate to authenticate CAN payload
    contents. Synthetic fixtures may omit bytes; hardware evidence may not.
    """
    if evidence != "hardware":
        return None
    value = event.get("data_hex")
    need(isinstance(value, str) and len(value) % 2 == 0 and
         2 <= len(value) <= 16 and
         all(char in "0123456789ABCDEF" for char in value),
         "hardware CAN frame needs 1-8 uppercase hex data bytes", line)
    data = bytes.fromhex(value)
    need(int_field(event, "dlc", line) == len(data),
         "hardware CAN DLC does not match bytes", line)
    need(event.get("extended") is True and event.get("rtr") is False,
         "hardware diagnostic CAN must be 29-bit data frame", line)
    return data


def verify(records):
    need(bool(records), "empty trace", 1)
    meta = records[0]
    need(isinstance(meta, dict) and meta.get("type") == "meta", "missing meta", 1)
    need(meta.get("schema") == 1, "unsupported schema", 1)
    profile = meta.get("profile")
    need(profile in {"passive_tx_off", "isolated_can_bench"}, "invalid profile", 1)
    evidence = meta.get("evidence")
    need(evidence in {"synthetic", "hardware"}, "invalid evidence class", 1)
    need(bool(SHA40.fullmatch(str(meta.get("firmware_sha", "")))),
         "firmware SHA required", 1)
    if evidence == "hardware":
        need(bool(SHA64.fullmatch(str(meta.get("capture_sha256", "")))),
             "hardware provenance needs capture_sha256", 1)
        need(bool(meta.get("observer")) and bool(meta.get("interface")),
             "hardware provenance needs observer and interface", 1)

    connected = {}
    last_generations = {}
    requests = {}
    active_id = None
    fault_locked = False
    last_t = -1
    counts = {"events": 0, "requests": 0, "can_tx": 0, "fc": 0,
              "foreign_rx": 0, "delivered": 0, "denied": 0}

    for line, e in enumerate(records[1:], start=2):
        need(isinstance(e, dict) and e.get("type") == "event",
             "expected event object", line)
        t = int_field(e, "t_us", line)
        need(t >= last_t, "event timestamps are not monotonic", line)
        last_t = t
        counts["events"] += 1
        kind = e.get("kind")
        if kind == "ble_connect":
            conn = int_field(e, "conn", line)
            generation = int_field(e, "generation", line, 1)
            need(conn not in connected, "duplicate active BLE connection", line)
            need(generation > last_generations.get(conn, 0),
                 "BLE generation reused/regressed", line)
            connected[conn] = generation
            last_generations[conn] = generation

        elif kind == "ble_disconnect":
            conn = int_field(e, "conn", line)
            generation = int_field(e, "generation", line, 1)
            need(connected.get(conn) == generation,
                 "disconnect does not match active generation", line)
            del connected[conn]

        elif kind == "request_denied":
            # Used for passive (TX=OFF) request intent; no real CAN TX.
            need(profile == "passive_tx_off", "denied marker in active bench", line)
            need(e.get("reason") == "tx_off", "denial must be TX=OFF", line)
            counts["denied"] += 1

        elif kind == "request_begin":
            need(profile == "isolated_can_bench",
                 "passive profile cannot begin diagnostic TX", line)
            reqid = e.get("id")
            need(isinstance(reqid, str) and 0 < len(reqid) <= 64,
                 "invalid request ID", line)
            need(active_id is None, "overlapping physical CAN requests", line)
            need(reqid not in requests, "duplicate request ID", line)
            conn = int_field(e, "conn", line)
            generation = int_field(e, "generation", line, 1)
            need(connected.get(conn) == generation, "request owner not connected", line)
            can_id = e.get("can_id")
            did = e.get("did")
            need(isinstance(can_id, str) and can_id in ALLOWED,
                 "unapproved physical request CAN ID", line)
            need(e.get("service") == "22" and isinstance(did, str)
                 and bool(HEX4.fullmatch(did)), "only exact read-only UDS22 DID", line)
            requests[reqid] = {"can_id": can_id, "did": did,
                               "subscribers": {(conn, generation)},
                               "delivered": set(),
                               "tx": False, "ff": False, "fc": False,
                               "cf_seq": 0, "valid": False, "finished": False,
                               "expected_length": None, "received_length": 0,
                               "last_cf_or_ff_us": None, "status": None}
            active_id = reqid
            counts["requests"] += 1

        elif kind == "ble_join":
            need(active_id is not None, "join without active request", line)
            r = requests[active_id]
            conn = int_field(e, "conn", line)
            generation = int_field(e, "generation", line, 1)
            need(connected.get(conn) == generation, "joined peer disconnected", line)
            need(e.get("can_id") == r["can_id"] and
                 e.get("service") == "22" and e.get("did") == r["did"],
                 "joined peer request identity differs", line)
            r["subscribers"].add((conn, generation))

        elif kind == "can_tx":
            need(profile == "isolated_can_bench",
                 "CAN TX or Flow Control seen in TX=OFF trace", line)
            need(not fault_locked, "CAN TX after fault lock", line)
            reqid = e.get("id")
            need(reqid == active_id, "CAN TX has no matching active owner", line)
            r = requests[reqid]
            need(e.get("can_id") == r["can_id"], "CAN TX to wrong ECU", line)
            raw = raw_can_payload(e, evidence, line)
            frame = e.get("frame")
            need(frame in {"request", "fc"}, "only UDS22 request/FC TX allowed", line)
            if frame == "request":
                need(not r["tx"], "duplicate diagnostic request TX", line)
                if raw is not None:
                    expected = bytes([3, 0x22]) + bytes.fromhex(r["did"])
                    need(len(raw) == 8 and raw[:4] == expected,
                         "TX bytes are not exact UDS22 DID request", line)
                r["tx"] = True
            else:
                need(r["tx"] and r["ff"] and not r["fc"],
                     "Flow Control without matching first frame", line)
                if raw is not None:
                    need(len(raw) == 8 and raw[:3] == bytes((0x30, 0, 0)),
                         "FC bytes are not 30 00 00", line)
                r["fc"] = True
                counts["fc"] += 1
            counts["can_tx"] += 1

        elif kind == "can_rx":
            can_id = e.get("can_id")
            need(isinstance(can_id, str) and bool(HEX8.fullmatch(can_id)),
                 "invalid received CAN ID", line)
            need(e.get("pci") in {"sf", "ff", "cf"},
                 "invalid/missing ISO-TP PCI label", line)
            raw = raw_can_payload(e, evidence, line)
            if raw is not None:
                pci_type = {"sf": 0, "ff": 1, "cf": 2}[e["pci"]]
                need(raw[0] >> 4 == pci_type,
                     "PCI type label differs from captured CAN bytes", line)
                if pci_type in {0, 1}:
                    start = 1 if pci_type == 0 else 2
                    need(len(raw) >= start + 3,
                         "CAN positive response too short", line)
                    if pci_type == 0:
                        need((raw[0] & 0x0F) >= 3,
                             "single-frame payload length too short", line)
                    else:
                        need(len(raw) == 8 and
                             ((raw[0] & 0x0F) << 8 | raw[1]) > 7,
                             "invalid first-frame total length", line)
                    need(raw[start] == 0x62 and
                         raw[start+1:start+3].hex().upper() == e.get("did") and
                         e.get("service") == "62",
                         "response service/DID label differs from CAN bytes", line)
                else:
                    need(len(raw) >= 2 and
                         (raw[0] & 0x0F) == e.get("seq"),
                         "CF sequence label differs from CAN bytes", line)
            if active_id is None or can_id != pair(requests[active_id]["can_id"]):
                counts["foreign_rx"] += 1
                continue
            r = requests[active_id]
            need(r["tx"], "matched ECU response before request TX", line)
            pci = e["pci"]
            if pci in {"sf", "ff"}:
                need(e.get("service") == "62" and e.get("did") == r["did"],
                     "mismatched UDS positive response DID/service", line)
                need(not r["ff"] and not r["valid"],
                     "duplicate/colliding positive response", line)
                if pci == "sf":
                    # Real frame length is determined by PCI, not by the
                    # independent label declaring a successful read.
                    if raw is not None:
                        declared_length = raw[0] & 0x0F
                        need(declared_length <= len(raw) - 1,
                             "single-frame payload incomplete", line)
                    r["valid"] = True
                else:
                    r["ff"] = True
                    if raw is not None:
                        length = ((raw[0] & 0x0F) << 8) | raw[1]
                        need(8 <= length <= MAX_REASSEMBLED_PAYLOAD,
                             "first-frame declared payload length outside buffer", line)
                        r["expected_length"] = length
                        r["received_length"] = len(raw) - 2
                        r["last_cf_or_ff_us"] = t
            else:
                seq = int_field(e, "seq", line, 0)
                need(seq <= 15, "ISO-TP CF sequence must be a 4-bit value", line)
                need(r["ff"] and r["fc"], "CF without matching FF and FC", line)
                need(seq == (r["cf_seq"] + 1) % 16,
                     "CF sequence gap/duplicate", line)
                r["cf_seq"] = seq
                need(type(e.get("complete")) is bool, "CF complete flag required", line)
                if raw is not None:
                    need(r["expected_length"] is not None and
                         r["received_length"] < r["expected_length"],
                         "consecutive frame after complete payload", line)
                    need(t >= r["last_cf_or_ff_us"] and
                         t - r["last_cf_or_ff_us"] < CF_TIMEOUT_US,
                         "consecutive frame arrived after ISO-TP deadline", line)
                    remaining = r["expected_length"] - r["received_length"]
                    need(len(raw) - 1 >= min(7, remaining),
                         "consecutive frame payload incomplete", line)
                    r["received_length"] += min(len(raw) - 1, remaining)
                    r["last_cf_or_ff_us"] = t
                    derived_complete = r["received_length"] == r["expected_length"]
                    need(e["complete"] is derived_complete,
                         "claimed CF complete differs from ISO-TP byte count", line)
                    r["valid"] = derived_complete
                elif e["complete"]:
                    r["valid"] = True

        elif kind == "fault_lock":
            fault_locked = True

        elif kind == "lease_revoked":
            # Evidence-only marker; TX after a fault still prohibited.
            pass

        elif kind == "request_finish":
            reqid = e.get("id")
            need(reqid == active_id, "finish not active request", line)
            r = requests[reqid]
            status = e.get("status")
            need(status in {"ok", "error", "timeout"}, "invalid request result", line)
            if status == "ok":
                need(r["valid"], "successful request without matched ISO-TP data", line)
            r["finished"] = True
            r["status"] = status
            active_id = None

        elif kind == "ble_deliver":
            reqid = e.get("id")
            need(reqid in requests, "unknown request in BLE delivery", line)
            r = requests[reqid]
            conn = int_field(e, "conn", line)
            generation = int_field(e, "generation", line, 1)
            need(r["finished"], "BLE delivery before CAN transaction finish", line)
            need(connected.get(conn) == generation,
                 "BLE delivery to disconnected or reused generation", line)
            need((conn, generation) in r["subscribers"],
                 "BLE delivery to a peer not subscribed to this request", line)
            need((conn, generation) not in r["delivered"],
                 "duplicate BLE delivery for one request/generation", line)
            need(e.get("status") == r["status"],
                 "BLE delivery mismatches transaction result", line)
            r["delivered"].add((conn, generation))
            counts["delivered"] += 1

        else:
            raise EvidenceError(f"line {line}: unknown event kind {kind!r}")

    need(counts["events"] > 0, "trace has no events", len(records))
    need(active_id is None, "trace ends with active CAN request", len(records))
    if profile == "passive_tx_off":
        need(counts["denied"] > 0, "passive trace lacks TX=OFF denial", len(records))
    else:
        need(counts["requests"] > 0, "bench trace lacks actual test request", len(records))
        need(counts["can_tx"] > 0, "bench trace has no CAN TX evidence", len(records))
        need(counts["delivered"] > 0, "bench trace lacks BLE delivery", len(records))
    return {"profile": profile, "evidence": evidence, **counts}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="JSONL evidence (read only)")
    args = parser.parse_args()
    try:
        records = [json.loads(line) for line in args.trace.read_text(encoding="utf-8").splitlines()
                   if line.strip()]
        summary = verify(records)
    except (EvidenceError, ValueError, OSError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    prefix = "SYNTHETIC (NOT HARDWARE EVIDENCE)" if summary["evidence"] == "synthetic" else "HARDWARE TRACE STRUCTURE"
    print(f"PASS {prefix}: {json.dumps(summary, sort_keys=True)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
