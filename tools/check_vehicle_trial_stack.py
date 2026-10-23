#!/usr/bin/env python3
"""Compare safety-critical compiler stack frames against the matching V1 bench.

The trial may not increase any existing critical static frame; this does not
replace runtime HWM measurement or real vehicle safety validation.
"""
import sys
from pathlib import Path

WATCH = (
    "::bleAppTask(",
    "void setup()",
    "void loop()",
    "m5can::CanMonitor::rxTask(",
    "m5can::CanMonitor::performQuery(",
    "m5can::BleElmTransport::pollTx(",
    "m5can::DualBleElm::poll(",
    "m5can::DualBleElm::event(",
    "m5can::DualBleElm::onLine(",
    "m5can::DualBleElm::onResult(",
    "m5can::DualBleElm::dispatch(",
    "m5can::SerialProtocol::pollInput(",
    "m5can::RawCanRecordingService::",
    "m5can::DriveLog::poll(",
)


def read_frames(root: Path, name: str) -> dict[str, int]:
    files = list((root / ".pio" / "build" / name / "src").glob("*.su"))
    if not files:
        raise ValueError(f"{name}: stack usage files missing")
    frames = {}
    for file in files:
        for line in file.read_text().splitlines():
            fields = line.split("\t")
            if len(fields) != 3:
                continue
            signature, size, kind = fields
            for needle in WATCH:
                if needle in signature:
                    if kind != "static":
                        raise ValueError(f"{name}: dynamic stack frame {needle}")
                    frames[needle] = max(frames.get(needle, 0), int(size))
    return frames


def compare(base: dict[str, int], trial: dict[str, int]):
    for key in WATCH:
        if key not in base or key not in trial:
            if key == "m5can::RawCanRecordingService::":
                continue  # Raw journal helpers may inline entirely
            raise ValueError(f"missing stack evidence: {key}")
        if trial[key] > base[key]:
            raise ValueError(f"trial frame regression: {key}: {trial[key]}>{base[key]}")
    print("PASS: critical vehicle-trial frames do not exceed matched V1 base")
    print(f"BLE worker frame={trial['::bleAppTask(']} bytes, "
          f"main loop frame={trial['void loop()']} bytes, "
          f"DriveLog::poll={trial['m5can::DriveLog::poll(']} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: check_vehicle_trial_stack.py <base-env> <trial-env>")
    root = Path(__file__).resolve().parents[1]
    compare(read_frames(root, sys.argv[1]), read_frames(root, sys.argv[2]))
