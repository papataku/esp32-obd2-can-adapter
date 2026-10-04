#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
src_files = list((root / "src").glob("*.[ch]pp"))
src = "\n".join(p.read_text() for p in src_files)
cfg = (root / "include" / "app_config.h").read_text()
serial = (root / "src" / "serial_protocol.cpp").read_text()
elm = (root / "src" / "elm_compat.cpp").read_text()
can = (root / "src" / "can_monitor.cpp").read_text()
policy = (root / "src" / "diagnostic_policy.cpp").read_text()
isotp = (root / "src" / "isotp_reassembler.cpp").read_text()
transaction = (root / "src" / "diagnostic_transaction.cpp").read_text()

required_elm = (
    "ATZ", "ATE0", "ATL0", "ATS0", "ATH1", "ATAL",
    "ATCAF1", "ATCFC1", "ATSP7", "ATI", "ATDP", "ATDPN",
    "AT@1", "ATCP", "ATSH", "ATAT", "ATST",
    "ATM5TX1", "ATM5TX0", "ATM5STAT",
)

transmit_count = src.count("twai_transmit(")
normal_files = [p.name for p in src_files if "TWAI_MODE_NORMAL" in p.read_text()]
mode_call_names = (
    "twai_stop(", "twai_driver_uninstall(",
    "twai_driver_install(", "twai_start(",
)
mode_files = {
    p.name for p in src_files
    if any(call in p.read_text() for call in mode_call_names)
}

checks = {
    "boot listen-only present":
        "installDriver(TWAI_MODE_LISTEN_ONLY)" in can,
    "listen-only TX queue constant zero":
        "kTwaiListenTxQueueLen = 0" in cfg,
    "normal TX queue bounded to one":
        "kTwaiNormalTxQueueLen = 1" in cfg,
    "exactly one twai_transmit call":
        transmit_count == 1,
    "twai_transmit only in CAN owner":
        "CanMonitor::transmitOwnedFrame" in can and "twai_transmit(" in can,
    "normal mode only referenced by CAN owner":
        normal_files == ["can_monitor.cpp"],
    "TWAI state-changing calls only CAN owner":
        mode_files == {"can_monitor.cpp"},
    "single-shot TX enabled":
        "tx.ss = 1" in can,
    "flow control is 30 00 00":
        "tx.data[0] = 0x30" in can and
        "tx.data[1] = 0x00" in can and
        "tx.data[2] = 0x00" in can,
    "FC send rechecks lease":
        "NeedFlowControl" in can and "leaseAllowsBudget(50)" in can,
    "explicit short lease required":
        "leaseAllowsBudget" in can and "ATM5TX1" in elm,
    "lease alone does not switch mode":
        "acquireLease" in can and
        "switchDriverMode" not in
        can.split("bool CanMonitor::acquireLease", 1)[1]
           .split("void CanMonitor::revokeLease", 1)[0],
    "host input bounded":
        "kMaxBytesPerPoll = 256" in serial,
    "ELM query is asynchronous":
        "submitQuery" in serial and "pollQueryResult" in serial,
    "only known 29-bit headers allowed":
        "0x18DB33F1U" in policy and "0x18DBEFF1U" in policy,
    "read services whitelisted":
        "request.data[0] == 0x01" in policy and
        "request.data[0] == 0x09" in policy and
        "request.data[0] == 0x22" in policy,
    "dangerous UDS services absent from allow policy":
        all(x not in policy for x in
            ("0x10","0x11","0x14","0x27","0x28","0x2E","0x31")),
    "response IDs restricted to 18DAF1xx":
        "0x18DAF100U" in policy,
    "FF ownership checks service PID DID before FC":
        "initialPayloadMatches(&data[2], available)" in isotp,
    "FC target derived from responding ECU":
        "0x18DA00F1U | ((response_id & 0xFFU) << 8)" in isotp,
    "stale pre-query frames rejected":
        "frame.timestamp_us < tx_us_" in transaction,
    "response pending bounded":
        "kMaxResponsePendingWindowMs" in can,
    "no public RAW TX command":
        "RAW_TX" not in src and "SEND_CAN" not in src,
    "all current Analyzer AT commands implemented":
        all(command in elm for command in required_elm),
}

failed = []
for name, ok in checks.items():
    print(("PASS" if ok else "FAIL"), name)
    if not ok:
        failed.append(name)

if failed:
    raise SystemExit("failed: " + ", ".join(failed))

print(f"{len(checks)} Phase 3C safety/compatibility checks passed")
