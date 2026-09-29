#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = "\n".join(p.read_text() for p in (root / "src").glob("*.[ch]pp"))
config = (root / "src" / "can_monitor.cpp").read_text()

checks = {
    "boot driver is LISTEN_ONLY": "TWAI_MODE_LISTEN_ONLY" in config,
    "TWAI TX queue is disabled": "general.tx_queue_len = 0" in config,
    "no twai_transmit call exists": "twai_transmit(" not in source,
    "no TWAI normal mode exists": "TWAI_MODE_NORMAL" not in source,
    "no public TX command marker": "TX LEASE" not in source and "OBD " not in source,
}

failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(("PASS" if ok else "FAIL"), name)
if failed:
    raise SystemExit(f"Phase 1 safety checks failed: {', '.join(failed)}")
print(f"{len(checks)} Phase 1 safety checks passed")
