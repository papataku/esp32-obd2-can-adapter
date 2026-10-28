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
class TimedSample:
    """At model-clock ms, use a diagnostic PDU or emit no response."""
    at_ms: int
    payload: bytes | None
    delay_ms: int | None


@dataclass(frozen=True)
class Response:
    request: bytes
    payload: bytes
    response_id_11: int | None
    response_id_29: int
    delay_ms: int
    drop_every: int | None
    relative_timestamps_ms: tuple[int, ...]
    replay_mode: str = "fixed"
    samples: tuple[TimedSample, ...] = ()
    loop_period_ms: int | None = None


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
                              "delay_ms"), ("drop_every", "relative_timestamps_ms",
                                            "replay_mode", "samples", "loop_period_ms"))
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
            rid11 = (_can_id(obj["response_id_11"], False) if obj["response_id_11"] is not None else None)
            rid29 = _can_id(obj["response_id_29"], True)
            ecu_n = int(name[3:], 16)
            if rid11 is not None and not 0x7E8 <= rid11 <= 0x7EF:
                raise ValueError("11-bit response must be 7E8..7EF")
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
            mode = obj.get("replay_mode", "fixed")
            if mode not in ("fixed", "sequence", "loop") or type(mode) is not str:
                raise ValueError("replay_mode must be fixed, sequence or loop")
            sample_data = obj.get("samples", [])
            if (not isinstance(sample_data, list) or
                    len(sample_data) > 10_000):
                raise ValueError("samples must be a bounded array")
            if mode == "fixed" and (sample_data or "samples" in obj):
                raise ValueError("fixed mode must not specify samples")
            if mode != "fixed" and not sample_data:
                raise ValueError("time-series replay needs at least one sample")
            if mode != "fixed" and times:
                raise ValueError("legacy relative_timestamps_ms conflicts with samples.at_ms")
            samples = []
            last_at = -1
            for sample in sample_data:
                _exact_keys(sample, ("at_ms",), ("payload_hex", "delay_ms", "no_data"))
                at = sample["at_ms"]
                if type(at) is not int or at < 0 or at <= last_at or at > 2**53:
                    raise ValueError("sample at_ms must increase strictly")
                if not samples and at != 0:
                    raise ValueError("first sample must start at model time zero")
                last_at = at
                no_data = sample.get("no_data", False)
                if type(no_data) is not bool:
                    raise ValueError("no_data must be boolean")
                if no_data:
                    if "payload_hex" in sample:
                        raise ValueError("no_data sample must not carry payload")
                    sample_payload = None
                else:
                    if "payload_hex" not in sample:
                        raise ValueError("sample needs payload_hex or no_data=true")
                    sample_payload = _hex(sample["payload_hex"], 2, 4095)
                    if not (sample_payload.startswith(bytes([request[0] + 0x40]) + request[1:]) or
                            (len(sample_payload) == 3 and
                             sample_payload[:2] == bytes([0x7F, request[0]]))):
                        raise ValueError("sample response service/PID/DID mismatch")
                sample_delay = sample.get("delay_ms")
                if sample_delay is not None and (
                    type(sample_delay) is not int or not 0 <= sample_delay <= 60_000
                ):
                    raise ValueError("sample delay_ms out of range")
                samples.append(TimedSample(at, sample_payload, sample_delay))
            period = obj.get("loop_period_ms")
            if mode == "loop":
                if (type(period) is not int or period <= last_at or
                        period > 2**53):
                    raise ValueError("loop period must exceed final sample at_ms")
            elif "loop_period_ms" in obj:
                raise ValueError("loop_period_ms only allowed for loop mode")
            records.append(Response(request, payload, rid11, rid29, delay, drop,
                                    tuple(times), mode, tuple(samples), period))
        ecus.append(ECU(name, tuple(records)))
    return ECUModel(SCHEMA, raw["provenance"], tuple(ecus))


DEFAULT_MAX_MODEL_BYTES = 2_000_000
MAX_CONFIGURABLE_MODEL_BYTES = 128_000_000


def load_model(path: str | Path, *, max_bytes: int = DEFAULT_MAX_MODEL_BYTES) -> ECUModel:
    """Read a *converted* privacy-minimal model, never the original SQLite.

    Default 2 MB gate retained. The caller must explicitly opt in to larger
    files; a finite ceiling still protects against accidental unbounded reads.
    Unknown fields, original schema_version/sessions, identifiers and raw data
    are rejected by parse_model regardless of size allowance.
    """
    if type(max_bytes) is not int or not 1 <= max_bytes <= MAX_CONFIGURABLE_MODEL_BYTES:
        raise ValueError("max_bytes must be a positive integer <= 128000000")
    path = Path(path)
    if path.stat().st_size > max_bytes:
        raise ValueError("model too large")
    return parse_model(json.loads(path.read_text(encoding="utf-8")))
