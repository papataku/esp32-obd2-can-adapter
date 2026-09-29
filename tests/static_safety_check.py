#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
src = "\n".join(p.read_text() for p in (root / "src").glob("*.[ch]pp"))
cfg = (root / "include" / "app_config.h").read_text()
serial = (root / "src" / "serial_protocol.cpp").read_text()

checks = {
    "listen-only present": "TWAI_MODE_LISTEN_ONLY" in src,
    "TX queue constant disabled": "kTwaiTxQueueLen = 0" in cfg,
    "TWAI config TX queue disabled": "general.tx_queue_len = 0" in src,
    "no twai_transmit": "twai_transmit(" not in src,
    "no normal mode": "TWAI_MODE_NORMAL" not in src,
    "no TX lease": "TX_LEASE" not in src,
    "no OBD command": "OBD " not in src,
    "host input bounded": "kMaxBytesPerPoll = 256" in serial,
    "native switch final text ACK": "#OK,NATIVE,ON" in serial,
    "native starts stream disabled": "stream_enabled_ = false" in serial,
}

failed = []
for name, ok in checks.items():
    print(("PASS" if ok else "FAIL"), name)
    if not ok:
        failed.append(name)

if failed:
    raise SystemExit("failed: " + ", ".join(failed))

print(f"{len(checks)} Phase 2 safety checks passed")
