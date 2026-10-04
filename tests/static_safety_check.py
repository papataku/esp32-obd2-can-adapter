#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
src = "\n".join(p.read_text() for p in (root / "src").glob("*.[ch]pp"))
cfg = (root / "include" / "app_config.h").read_text()
serial = (root / "src" / "serial_protocol.cpp").read_text()
elm = (root / "src" / "elm_compat.cpp").read_text()

required_elm = (
    "ATZ", "ATE0", "ATL0", "ATS0", "ATH1", "ATAL", "ATCAF1", "ATCFC1",
    "ATSP7", "ATI", "ATDP", "ATDPN", "AT@1", "ATCP", "ATSH", "ATAT", "ATST",
)

checks = {
    "listen-only present": "TWAI_MODE_LISTEN_ONLY" in src,
    "TX queue constant disabled": "kTwaiTxQueueLen = 0" in cfg,
    "TWAI config TX queue disabled": "general.tx_queue_len = 0" in src,
    "Phase 3A has no twai_transmit": "twai_transmit(" not in src,
    "Phase 3A has no normal mode": "TWAI_MODE_NORMAL" not in src,
    "host input bounded": "kMaxBytesPerPoll = 256" in serial,
    "ELM auto-entry exists": "looksLikeElmCommand" in serial,
    "ELM mode suppresses raw stream": "if (elm_mode_ || !stream_enabled_) return;" in serial,
    "vehicle reads remain TX locked": "M5CAN TX LOCKED" in elm,
    "UDS write 2E not whitelisted": "bytes[0] == 0x2E" not in elm,
    "UDS session 10 not whitelisted": "bytes[0] == 0x10" not in elm,
    "all current Analyzer AT commands implemented": all(command in elm for command in required_elm),
}

failed = []
for name, ok in checks.items():
    print(("PASS" if ok else "FAIL"), name)
    if not ok:
        failed.append(name)

if failed:
    raise SystemExit("failed: " + ", ".join(failed))

print(f"{len(checks)} Phase 3A safety/compatibility checks passed")
