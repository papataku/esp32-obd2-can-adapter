"""Strict, privacy-minimal, host-only ECU model schema.

Each response payload is a complete *diagnostic PDU*, without ISO-TP PCI.
No absolute timestamps or vehicle identifiers are accepted.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import json
import re

SCHEMA = "ttm.ecu-model.v1"
_HEX = re.compile(r"^[0-9A-F]+$")
_RESTRICTED = ("vin", "location", "latitude", "longitude", "gps", "timestamp",
               "datetime", "date_utc", "mac", "address", "ble_id", "serial_number")


def _exact_keys(value, required, optional=()):
    if not isinstance(value, dict):
        raise ValueError("expected JSON object")
    unknown = set(value) - set(required) - set(optional)
    missing = set(required) - set(value)
    if missing or unknown:
        raise ValueError(f"missing={sorted(missing)} unexpected={sorted(unknown)}")


def _hex(value, byte_min=1, byte_max=4095):
    if not isinstance(value, str) or not _HEX.fullmatch(value):
        raise ValueError("uppercase hexadecimal bytes required")
    if len(value) % 2 or not byte_min * 2 <= len(value) <= byte_max * 2:
        raise ValueError("invalid hex payload length")
    return bytes.fromhex(value)


def _can_id(value, extended):
    if not isinstance(value, str) or not _HEX.fullmatch(value):
        raise ValueError("CAN ID must be uppercase hexadecimal string")
    if len(value) != (8 if extended else 3):
        raise ValueError("incorrect CAN ID width")
    n = int(value, 16)
    if n > (0x1FFFFFFF if extended else 0x7FF):
        raise ValueError("CAN ID out of range")
    return n


@dataclass(frozen=True)
class Response:
    request: bytes
    payload: bytes
    response_id_11: int
    response_id_29: int
    delay_ms: int
    drop_every: int | None
    relative_timestamps_ms: tuple[int, ...]


@dataclass(frozen=True)
class ECU:
    id: str
    responses: tuple[Response, ...]


@dataclass(frozen=True)
class ECUModel:
    schema: str
    provenance: str
    ecus: tuple[ECU, ...]


def parse_model(raw: dict) -> ECUModel:
    _exact_keys(raw, ("schema", "provenance", "ecus"))
    if raw["schema"] != SCHEMA:
        raise ValueError("unsupported ECU model version")
    if raw["provenance"] not in ("synthetic", "anonymized_observation"):
        raise ValueError("invalid provenance")
    if not isinstance(raw["ecus"], list) or not 1 <= len(raw["ecus"]) <= 32:
        raise ValueError("ECU model requires 1..32 ECUs")
    ecus = []
    names = set()
    for item in raw["ecus"]:
        _exact_keys(item, ("id", "responses"))
        name = item["id"]
        if not isinstance(name, str) or not re.fullmatch(r"ECU[0-9A-F]{2}", name) or name in names:
            raise ValueError("ECU id must be unique ECUxx")
        names.add(name)
        if not isinstance(item["responses"], list) or len(item["responses"]) > 256:
            raise ValueError("responses must be bounded array")
        records = []
        seen = set()
        for obj in item["responses"]:
            _exact_keys(obj, ("request", "payload_hex", "response_id_11", "response_id_29",
                              "delay_ms"), ("drop_every", "relative_timestamps_ms"))
            request = _hex(obj["request"], 2, 3)
            if not ((len(request) == 2 and request[0] == 1) or
                    (len(request) == 3 and request[0] == 0x22)):
                raise ValueError("only reviewed read-only Service01 / UDS22 requests")
            if request in seen:
                raise ValueError("duplicate request for ECU")
            seen.add(request)
            payload = _hex(obj["payload_hex"], 2, 4095)
            if not (payload.startswith(bytes([request[0] + 0x40]) + request[1:]) or
                    (len(payload) == 3 and payload[:2] == bytes([0x7F, request[0]]))):
                raise ValueError("response service/PID/DID mismatch")
            rid11 = _can_id(obj["response_id_11"], False)
            rid29 = _can_id(obj["response_id_29"], True)
            ecu_n = int(name[3:], 16)
            if not (0 <= ecu_n <= 7 and rid11 == 0x7E8 + ecu_n):
                raise ValueError("11-bit ECU ID must be 0..7 and matched to 7E8..7EF")
            if rid29 != 0x18DAF100 | ecu_n:
                raise ValueError("29-bit response ECU ID mismatch")
            delay = obj["delay_ms"]
            if type(delay) is not int or not 0 <= delay <= 60_000:
                raise ValueError("delay_ms out of range")
            drop = obj.get("drop_every")
            if drop is not None and (type(drop) is not int or not 1 <= drop <= 10000):
                raise ValueError("drop_every invalid")
            times = obj.get("relative_timestamps_ms", [])
            if not isinstance(times, list) or len(times) > 10000 or any(
                type(t) is not int or t < 0 for t in times
            ) or times != sorted(times):
                raise ValueError("relative timestamps must be monotonic nonnegative integers")
            records.append(Response(request, payload, rid11, rid29, delay, drop, tuple(times)))
        ecus.append(ECU(name, tuple(records)))
    return ECUModel(SCHEMA, raw["provenance"], tuple(ecus))


def load_model(path: str | Path) -> ECUModel:
    path = Path(path)
    if path.stat().st_size > 2_000_000:
        raise ValueError("model too large")
    return parse_model(json.loads(path.read_text(encoding="utf-8")))
