#!/usr/bin/env python3
"""Serial HIL virtual ECU for the m5dial_signal_v2_can_29bit_serial_hil image.

N1 PR-D substitute (owner decision 2026-10-29: no external CAN analyzer).
The M5Dial runs the production V2-2 CAN stack; its TWAI driver is a USB-serial
bridge. This script plays the RP8 ECU01 (18DAF101), injects faults, and checks
the adapter's reaction from its own serial output plus an independent PC-side
count of every bridged frame (TX reported by the adapter + RX injected here).

Prerequisite (operator): a V2 Central (iPhone or SmartRing) is connected,
secured and demanding signals, so that "SIGV2 CAN ARM" is accepted.

Usage: signal_v2_hil_ecu.py --port /dev/cu.usbmodem1101 --scenario normal
"""
import argparse
import json
import re
import sys
import time

ECU_RX = 0x18DAF101          # ECU01 response ID (RP8 evidence)
ECU_TX = 0x18DA01F1          # physical request to ECU01
BITS_PER_FRAME = 200         # conservative accounting used by the firmware
CAP_100MS, CAP_1S = 5000, 25000

PROBE = bytes([0x03, 0x01, 0x0C, 0x0D])
GROUP = bytes([0x06, 0x01, 0x0D, 0x0C, 0x05, 0x5B, 0x9A])
RPM_1000 = bytes([0x0F, 0xA0])
PAIR_SF = bytes([0x06, 0x41, 0x0C]) + RPM_1000 + bytes([0x0D, 0x00])
GROUP_FF = bytes([0x10, 0x11, 0x41, 0x0D, 0x00, 0x0C]) + RPM_1000
GROUP_CF = [bytes([0x21, 0x05, 0x80, 0x5B, 0x7F, 0x9A, 0x00, 0x00]),
            bytes([0x22, 0x4B, 0x00, 0x00, 0x64])]

# scenario -> (expect_stop, description)
SCENARIOS = {
    "normal": (False, "combined SF probe, FF/CF group after FC"),
    "split": (False, "probe answered by two per-PID SFs"),
    "noreply": (True, "candidate silent -> NO_VERIFIED_ECU"),
    "extra": (True, "third SF on the probe route"),
    "ff_probe": (True, "First Frame answering the probe; no FC allowed"),
    "late": (True, "probe reply after the 120 ms window"),
    "duplicate": (True, "same SF twice"),
    "wrong_src": (True, "reply from 18DAF102 only"),
    "eleven_bit": (True, "11bit 7E8 OBD reply"),
    "negative": (True, "7F 01 11 negative response"),
    "txfail": (True, "driver TX failure on the first probe"),
    "busoff": (True, "BUS_OFF after a healthy pin"),
    "rxmiss": (True, "RX missed counter after a healthy pin"),
}

TX_RE = re.compile(r"#HIL_TX,t_us=(\d+),id=([0-9A-F]{8}),ext=(\d),dlc=(\d),data=([0-9A-F]*)")


def rx_line(can_id, data, ext=True):
    return "HIL RX %X %d %s" % (can_id, 1 if ext else 0, data.hex().upper())


class VirtualEcu:
    """Pure scenario logic: adapter TX frame -> list of (delay_s, line)."""

    def __init__(self, scenario):
        if scenario not in SCENARIOS:
            raise ValueError(scenario)
        self.scenario = scenario
        self.probes = 0
        self.groups = 0
        self.fc = 0
        self.pending_cf = False

    def setup(self):
        return ["HIL TXFAIL 1"] if self.scenario == "txfail" else []

    def on_tx(self, can_id, data):
        s = self.scenario
        if can_id != ECU_TX:
            return []
        if data[:1] == b"\x30":                      # Flow Control from adapter
            self.fc += 1
            if not self.pending_cf:
                return []
            self.pending_cf = False
            return [(0.002 * (i + 1), rx_line(ECU_RX, cf)) for i, cf in enumerate(GROUP_CF)]
        if data[:4] == PROBE:                        # discovery probe / essential read
            self.probes += 1
            if self.probes > 1 or s in ("normal", "busoff", "rxmiss"):
                return [(0.005, rx_line(ECU_RX, PAIR_SF))]
            if s == "split":
                return [(0.005, rx_line(ECU_RX, bytes([3, 0x41, 0x0D, 0]))),
                        (0.010, rx_line(ECU_RX, bytes([4, 0x41, 0x0C]) + RPM_1000))]
            if s == "extra":
                return [(0.005 * (i + 1), rx_line(ECU_RX, PAIR_SF if i == 0 else bytes([3, 0x41, 0x0D, 0])))
                        for i in range(3)]
            if s == "ff_probe":
                return [(0.005, rx_line(ECU_RX, bytes([0x10, 0x08, 0x41, 0x0C]) + RPM_1000 + bytes([0x0D, 0x00])))]
            if s == "late":
                return [(0.150, rx_line(ECU_RX, PAIR_SF))]
            if s == "duplicate":
                return [(0.005, rx_line(ECU_RX, PAIR_SF)), (0.010, rx_line(ECU_RX, PAIR_SF))]
            if s == "wrong_src":
                return [(0.005, rx_line(0x18DAF102, PAIR_SF))]
            if s == "eleven_bit":
                return [(0.005, rx_line(0x7E8, PAIR_SF, ext=False))]
            if s == "negative":
                return [(0.005, rx_line(ECU_RX, bytes([3, 0x7F, 0x01, 0x11])))]
            return []                                # noreply, txfail
        if data[:7] == GROUP:                        # regular 5-PID group
            self.groups += 1
            out = [(0.005, rx_line(ECU_RX, GROUP_FF))]
            self.pending_cf = True
            if self.groups == 2 and s == "busoff":
                out.append((0.050, "HIL BUSOFF"))
            if self.groups == 2 and s == "rxmiss":
                out.append((0.050, "HIL RXMISS 1"))
            return out
        return []


def window_peaks(times_s):
    """Independent PC-side load: every bridged frame at 200 bits, sliding windows."""
    times = sorted(times_s)
    peaks = {}
    for width, key in ((0.1, "peak100_bits"), (1.0, "peak1000_bits")):
        best, j = 0, 0
        for i, t in enumerate(times):
            while times[j] < t - width:
                j += 1
            best = max(best, (i - j + 1) * BITS_PER_FRAME)
        peaks[key] = best
    return peaks


def evaluate(scenario, events):
    """events: list of dicts {t, kind: tx|rx|line, text}."""
    expect_stop, _ = SCENARIOS[scenario]
    lines = [e["text"] for e in events if e["kind"] == "line"]
    stop_idx = next((i for i, e in enumerate(events)
                     if e["kind"] == "line" and "#SIGNALV2_CAN,stopped=1" in e["text"]), None)
    tx_after_stop = 0 if stop_idx is None else sum(1 for e in events[stop_idx:] if e["kind"] == "tx")
    functional = sum(1 for e in events if e["kind"] == "tx" and e["id"] == 0x18DB33F1)
    fc_in_probe = sum(1 for e in events if e["kind"] == "tx" and e.get("data", b"")[:1] == b"\x30"
                      and scenario == "ff_probe")
    pinned = any("#SIGNALV2_ECU,physical_response=18DAF101" in l for l in lines)
    stopped = stop_idx is not None
    peaks = window_peaks([e["t"] for e in events if e["kind"] in ("tx", "rx")])
    checks = {
        "stop_as_expected": stopped == expect_stop,
        "no_tx_after_stop": tx_after_stop == 0,
        "no_functional_tx": functional == 0,
        "no_fc_on_probe_ff": fc_in_probe == 0,
        "pinned_when_healthy": pinned or expect_stop,
        "within_100ms_cap": peaks["peak100_bits"] <= CAP_100MS,
        "within_1s_cap": peaks["peak1000_bits"] <= CAP_1S,
    }
    reason = None
    if stopped:
        m = re.search(r"reason=([^,]+)", events[stop_idx]["text"])
        reason = m.group(1) if m else None
    return {"scenario": scenario, "pass": all(checks.values()), "checks": checks,
            "stop_reason": reason, "tx_after_stop": tx_after_stop, "pinned": pinned,
            "independent_load": peaks,
            "note": "load counted from bridged frames at 200 bits; not a wire measurement"}


def run(port, scenario, seconds):
    import serial  # pyserial, operator machine only
    ecu = VirtualEcu(scenario)
    ser = serial.Serial(port, 115200, timeout=0.001)
    events, scheduled, t0 = [], [], time.monotonic()

    def send(line):
        ser.write((line + "\n").encode())
        now = time.monotonic() - t0
        if line.startswith("HIL RX "):
            events.append({"t": now, "kind": "rx", "text": line})

    for cmd in ["SIGV2 CAN STOP", "HIL RESET"] + ecu.setup() + ["SIGV2 CAN ARM"]:
        send(cmd)
        time.sleep(0.05)
    buf = b""
    while time.monotonic() - t0 < seconds:
        now = time.monotonic() - t0
        due = [s for s in scheduled if s[0] <= now]
        for s in due:
            scheduled.remove(s)
            send(s[1])
        buf += ser.read(4096)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            text = raw.decode(errors="replace").strip()
            m = TX_RE.match(text)
            if m:
                cid, data = int(m.group(2), 16), bytes.fromhex(m.group(5))
                events.append({"t": now, "kind": "tx", "id": cid, "data": data, "text": text})
                for delay, line in ecu.on_tx(cid, data):
                    scheduled.append((now + delay, line))
            elif text.startswith("#"):
                events.append({"t": now, "kind": "line", "text": text})
    send("SIGV2 CAN STOP")
    ser.close()
    result = evaluate(scenario, events)
    result["fc_seen"] = ecu.fc
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", required=True)
    p.add_argument("--scenario", default="normal", choices=sorted(SCENARIOS))
    p.add_argument("--seconds", type=float, default=8.0)
    a = p.parse_args()
    result = run(a.port, a.scenario, a.seconds)
    print(json.dumps(result, indent=2))
    if result["stop_reason"] and SCENARIOS[a.scenario][0]:
        print("Note: a latched driver fault (busoff/rxmiss) needs a reboot before the next scenario.",
              file=sys.stderr)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
