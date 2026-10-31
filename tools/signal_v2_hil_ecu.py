#!/usr/bin/env python3
"""Serial HIL virtual ECU for the m5dial_signal_v2_can_29bit_serial_hil image.

N1 PR-D substitute (owner decision 2026-10-29: no external CAN analyzer).
The M5Dial runs the production V2-2 CAN stack; its TWAI driver is a USB-serial
bridge. This script plays the RP8 ECU01 (18DAF101), injects faults, and checks
the adapter's reaction using sequenced board timestamps for each virtual TX
attempt and RX admission, reconciled against host writes in a saved trace.

Prerequisite (operator): a V2 Central (iPhone or SmartRing) is connected,
secured and demanding signals, so that "SIGV2 CAN ARM" is accepted.

Usage: signal_v2_hil_ecu.py --port /dev/cu.usbmodem1101 --scenario normal
"""
import argparse
import json
import re
import sys
import time
import secrets
from collections import Counter

ECU_RX = 0x18DAF101          # ECU01 response ID (RP8 evidence)
ECU_TX = 0x18DA01F1          # physical request to ECU01
BITS_PER_FRAME = 200         # conservative accounting used by the firmware
CAP_100MS, CAP_1S = 5000, 25000
# A fault or disconnect must reach its completed STOP within the same 3s bound
# used for detection; the final operator STOP is evaluated separately.
STOP_COMPLETION_S = 3.0

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
    "rxmiss": (True, "RX missed counter after a healthy acquisition"),
    "cf_probe": (True, "unsolicited CF during discovery"),
    "malformed": (True, "invalid SF length during discovery"),
    "overflow": (True, "atomic configured RX queue overflow"),
    "buserr": (True, "bus error after a healthy acquisition"),
    "rearm": (False, "STOP pending group; delayed old CF; fresh re-ARM"),
    "overlap": (True, "old CF overlaps a new-epoch probe"),
    "admission": (False, "finite probe/group admission envelope then quiet soak"),
    "disconnect": (True, "operator disconnects BLE central during acquisition"),
    "reboot": (False, "operator reboots during acquisition; new boot/fence/epoch"),
}


def rx_line(can_id, data, ext=True, generation=0):
    # generation=0 is a pure scenario template, bound to the observed TX
    # generation by run(). Firmware rejects unbound or stale injections.
    return "HIL RX %d %X %d %s" % (generation, can_id, 1 if ext else 0, data.hex().upper())


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
            if s == "overlap" and self.probes > 1:
                return [(0.001,rx_line(ECU_RX,GROUP_CF[0])),(0.005,rx_line(ECU_RX,PAIR_SF))]
            if s in ("normal", "busoff", "rxmiss", "buserr", "overflow", "rearm", "overlap", "admission", "disconnect", "reboot"):
                return [(0.005, rx_line(ECU_RX, PAIR_SF))]
            if s == "split":
                return [(0.005, rx_line(ECU_RX, bytes([3, 0x41, 0x0D, 0]))),
                        (0.010, rx_line(ECU_RX, bytes([4, 0x41, 0x0C]) + RPM_1000))]
            if s == "extra":
                return [(0.005 * (i + 1), rx_line(ECU_RX, PAIR_SF if i == 0 else bytes([3, 0x41, 0x0D, 0])))
                        for i in range(3)]
            if s == "cf_probe":
                return [(0.005, rx_line(ECU_RX, GROUP_CF[0]))]
            if s == "malformed":
                return [(0.005, rx_line(ECU_RX, bytes([7, 0x41, 0x0C])))]
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
            if self.groups == 2 and s in ("rxmiss", "buserr", "overflow"):
                out.append((0.050, {"rxmiss":"HIL RXMISS 1", "buserr":"HIL BUSERR 1", "overflow":"HIL OVERFLOW"}[s]))
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


def fields(text):
    return dict(part.split("=", 1) for part in text.split(",") if "=" in part)


def parse_event(text):
    """Board clock is the sole load/deadline clock. Host arrival is separate."""
    if not text.startswith("#HIL_EVENT,"):
        return None
    f = fields(text)
    e = dict(f, text=text, t=int(f["t_us"]) / 1e6,
             seq=int(f["seq"]), gen=int(f["gen"]), lost=int(f["lost"]))
    if e["kind"] in ("tx", "rx"):
        e.update(id=int(f["id"], 16), ext=int(f["ext"]), dlc=int(f["dlc"]),
                 data=bytes.fromhex(f["data"]), ok=int(f["ok"]))
        if len(e["data"]) != e["dlc"] or e["dlc"] > 8:
            raise ValueError("invalid DLC")
    return e


FAULT_REASONS = {
    "noreply": {"NO_VERIFIED_ECU", "DISCOVERY_TIMEOUT"},
    "txfail": {"DISCOVERY_RESPONSE_CONTRACT", "RX_ACCOUNTING_OR_BUS_FAULT"},
    "extra": {"DISCOVERY_RESPONSE_CONTRACT", "RX_ACCOUNTING_OR_BUS_FAULT"},
    "ff_probe": {"DISCOVERY_RESPONSE_CONTRACT", "RX_ACCOUNTING_OR_BUS_FAULT"},
    "cf_probe": {"DISCOVERY_RESPONSE_CONTRACT", "RX_ACCOUNTING_OR_BUS_FAULT"},
    "malformed": {"DISCOVERY_RESPONSE_CONTRACT", "RX_ACCOUNTING_OR_BUS_FAULT"},
    "late": {"NO_VERIFIED_ECU", "RX_ACCOUNTING_OR_BUS_FAULT"},
    "duplicate": {"DISCOVERY_RESPONSE_CONTRACT", "RX_ACCOUNTING_OR_BUS_FAULT"},
    "wrong_src": {"DISCOVERY_RESPONSE_CONTRACT", "RX_ACCOUNTING_OR_BUS_FAULT"},
    "eleven_bit": {"11BIT_OBD_RESPONSE", "DISCOVERY_RESPONSE_CONTRACT", "RX_ACCOUNTING_OR_BUS_FAULT"},
    "negative": {"DISCOVERY_RESPONSE_CONTRACT", "RX_ACCOUNTING_OR_BUS_FAULT"},
    "busoff": {"RX_ACCOUNTING_OR_BUS_FAULT", "TWAI_BUS_ERROR"},
    "rxmiss": {"RX_ACCOUNTING_OR_BUS_FAULT", "TWAI_BUS_ERROR"},
    "buserr": {"RX_ACCOUNTING_OR_BUS_FAULT", "TWAI_BUS_ERROR"},
    "overflow": {"RX_ACCOUNTING_OR_BUS_FAULT", "TWAI_BUS_ERROR"},
    "overlap": {"RX_ACCOUNTING_OR_BUS_FAULT", "DISCOVERY_RESPONSE_CONTRACT"},
}


def evaluate(scenario, events):
    """Fail closed on missing prerequisites, unacknowledged injections or loss.

    Only parsed #HIL_EVENT records are timed evidence. Raw serial lines and
    host writes remain in the artifact but cannot stand in for board events.
    """
    expect_stop, _ = SCENARIOS[scenario]
    board = [e for e in events if "seq" in e]
    kind = lambda k: [e for e in board if e["kind"] == k]
    arms = kind("arm")
    frames = kind("tx") + kind("rx")
    txs, rxs, faults, stops = kind("tx"), kind("rx"), kind("fault"), kind("stopped")
    raw_txs = [e for e in events if e["kind"] == "tx"]
    samples, gates = kind("sample"), kind("gate")
    fences = kind("fence")
    reason = faults[0].get("reason") if faults else None
    # Synthetic host-arrival timestamps cannot qualify as timed evidence.
    complete = bool(board) and all(e.get("lost") == 0 and "t_us" in e and bool(e.get("boot")) for e in board)
    complete &= all((b["seq"] == a["seq"] + 1 or (scenario == "reboot" and b.get("boot") != a.get("boot") and b["kind"] == "fence")) and b["t"] >= a["t"]
                    for a, b in zip(board, board[1:]))
    lines = [e.get("text", "") for e in events]
    errors = any("#HIL_ERR" in l or "kind=error" in l or "reason=NOT_READY" in l or
                 "admin=BUSY" in l or e.get("kind") == "parse_error"
                 for e, l in zip(events, lines))
    writes = [e for e in events if e["kind"] == "write" and e["text"].startswith("HIL RX ")]
    expected = Counter("HIL RX 0 " + " ".join(e["text"].split()[3:]) for e in writes)
    accepted = Counter(rx_line(e["id"], e["data"], bool(e["ext"])) for e in rxs)
    # A fault or a central disconnect ends authorization (B-10); its first
    # completed STOP is the boundary after which scheduled RX may be refused.
    triggers = sorted(faults + kind("disconnect"), key=lambda x: x["seq"])
    trigger = triggers[0] if triggers else None
    first_fault_stop = next((x for x in stops if x["seq"] > trigger["seq"]), None) if trigger else None
    # Responses the virtual ECU model actually schedules for each successful
    # adapter TX, keyed by the cause's board time and generation (B-09).
    # M-19 (reboot): board seq resets on each boot; a global seq sort
    # interleaves two independent ISO-TP conversations and can make a valid
    # second-boot FC appear unscheduled. Replay in recorded board order with
    # a distinct virtual ECU state for each boot identity.
    models = {}
    predicted = Counter()
    for t in txs:
        if t["ok"]:
            boot = t.get("boot")
            if boot not in models:
                models[boot] = VirtualEcu(scenario)
            for _, line in models[boot].on_tx(t["id"], t["data"]):
                if line.startswith("HIL RX "):
                    predicted[(t["t"], t.get("gen"), " ".join(line.split()[3:]))] += 1
    # One-to-one causal reconciliation in host arrival order (B-11..B-13):
    # every board RX consumes the earliest unconsumed preceding host write
    # with the same payload; a runner-scheduled write also consumes one
    # modelled response slot of its causing TX, whether admitted or refused.
    # from_gen is the causing TX generation; the injection generation may
    # differ after a post-STOP reinstall and is checked only for admissions.
    # One pass per boot: authorization (arm/stopped) and the shim's driver
    # lifecycle (install -> start -> stop -> uninstall, BUS_OFF) at each RX.
    # An impossible transition or a generation other than the installed one
    # cannot admit (M-02/M-04/M-06). No driver events since the fence means
    # the driver was started before the trace began.
    admissible = {}
    tx_running = {}
    life = {}
    for x in board:
        boot = x.get("boot")
        st = life.setdefault(boot, {"auth": None, "state": "running", "gen": None, "broken": False})
        k = "busoff" if x["kind"] == "command" and x.get("command") == "BUSOFF" else x["kind"]
        if k in ("install", "start", "stop", "uninstall", "busoff"):
            # M-07: every lifecycle event carries the current driver
            # generation; install must advance it. A driver started before the
            # fence adopts the first observed event's generation.
            if k == "install":
                st["broken"] |= st["state"] != "uninstalled" or (
                    st["gen"] is not None and x.get("gen", 0) <= st["gen"])
                st["state"], st["gen"] = "stopped", x.get("gen")
                continue
            if st["gen"] is None:
                st["gen"] = x.get("gen")
            st["broken"] |= x.get("gen") != st["gen"]
        if k in ("arm", "stopped"):
            st["auth"] = k
        elif k == "start":
            st["broken"] |= st["state"] != "stopped"
            st["state"] = "running"
        elif k == "stop":
            st["broken"] |= st["state"] != "running"
            st["state"] = "stopped"
        elif k == "uninstall":
            st["broken"] |= st["state"] not in ("stopped", "busoff")
            st["state"] = "uninstalled"
        elif k == "busoff":
            st["state"] = "busoff"
        elif k in ("rx", "tx"):
            running = (not st["broken"] and st["state"] == "running"
                       and (st["gen"] is None or x.get("gen") == st["gen"]))
            if k == "rx":
                # M-12: the shim admits whenever its driver is RUNNING with
                # the current generation, independent of the logical CAN
                # authorization; a late frame after STOP may be received but
                # must never become Fresh data (sample/packet checks below).
                admissible[(boot, x["seq"])] = running
            else:
                tx_running[(boot, x["seq"])] = running
    def admitted_while_running(e):
        return admissible.get((e.get("boot"), e["seq"]), False)
    order = {id(x): i for i, x in enumerate(events)}
    rx_causes = {}  # (boot, RX seq) -> causally paired adapter TX (seq resets on reboot).
    def reconcile(extra_refusal=lambda e, w: False):
        slots = Counter(predicted)
        pending, ok, deliberate = [], True, 0
        for e in events:
            if e["kind"] == "write" and e["text"].startswith("HIL RX "):
                pending.append(e)
                continue
            if e["kind"] != "rx" or "seq" not in e:
                continue
            line = rx_line(e["id"], e["data"], bool(e["ext"]))[len("HIL RX 0 "):]
            match = next((i for i, w in enumerate(pending) if " ".join(w["text"].split()[3:]) == line), None)
            if match is None:
                ok = False
                continue
            w = pending.pop(match)
            cause = None
            if "from_t" not in w:
                # Only the single deliberate operator injection (rearm) lacks a
                # cause, and it must be refused (review m-01).
                deliberate += 1
                ok &= not e["ok"] and extra_refusal(e, w) and deliberate == 1
                continue
            if True:
                key = (w["from_t"], w.get("from_gen"), line)
                cause = next((t for t in txs if t["t"] == w["from_t"] and t.get("gen") == w.get("from_gen")), None)
                # M-11: the causing TX must be recorded before the write.
                if cause is not None and order[id(cause)] > order[id(w)]:
                    cause = None
                if cause is None or slots[key] <= 0:
                    ok = False
                    continue
                slots[key] -= 1
            if e["ok"]:
                rx_causes[(e.get("boot"), e["seq"])] = cause
                # The admitted RX must carry its own write's injection
                # generation (M-01) and arrive while running (M-02).
                ok &= w["text"].split()[2] == str(e.get("gen")) and admitted_while_running(e)
                continue
            fail_closed = bool(first_fault_stop and e["seq"] > first_fault_stop["seq"] and
                               cause and cause["seq"] < trigger["seq"])
            ok &= fail_closed or extra_refusal(e, w)
        return ok and not pending
    acknowledged = reconcile()
    # The generation is part of the injection contract: an admitted RX must
    # consume a host write bound to the board's generation at admission time.
    bound = Counter((e["text"].split()[2], " ".join(e["text"].split()[3:])) for e in writes)
    admitted = Counter((str(e.get("gen")), rx_line(e["id"], e["data"], bool(e["ext"]))[len("HIL RX 0 "):])
                       for e in rxs if e["ok"])
    generation_bound = not (admitted - bound)
    tx_allowed = all(tx_running.get((e.get("boot"), e["seq"]), False) for e in txs if e["ok"])
    tx_allowed &= all(e.get("ext") == 1 and e["id"] == ECU_TX and e.get("dlc") == 8 and
                     e["data"] in (PROBE.ljust(8, b"\0"), GROUP.ljust(8, b"\0"), b"\x30" + b"\0"*7)
                     for e in txs)
    probe_txs = [e for e in txs if e["data"][:4] == PROBE]
    fc_txs = [e for e in txs if e["data"][:1] == b"\x30"]
    group_txs = [e for e in txs if e["data"][:7] == GROUP]
    # An essential read shares the probe bytes; count discovery by gate marker.
    discovery_gates = [e for e in gates if e.get("discovery") == "1"]
    epochs = [int(e.get("epoch", 0)) for e in arms]
    epoch_ok = bool(arms) and all(e.get("ok") == "1" for e in arms) and all(epochs)
    epoch_ok &= len(set((e.get("boot"), e.get("epoch")) for e in arms)) == len(epochs)
    # M-09: discovery runs from each ARM until that epoch's first group TX;
    # an FC there is never valid, whatever the scenario.
    discovery_fc = False
    in_discovery = False
    for x in board:
        if x["kind"] == "arm":
            in_discovery = True
        elif x["kind"] == "tx" and x["data"][:7] == GROUP:
            in_discovery = False
        elif x["kind"] == "tx" and x["data"][:1] == b"\x30" and in_discovery:
            discovery_fc = True
    # M-14/M-16: board-sequenced, causal GROUP state machine. An FF must
    # belong to the current GROUP TX, CFs to its sole FC, and the completed
    # CF sequence must precede exactly one Fresh sample from that group.
    # These are driver-admitted RX records, not just aggregate RX counts.
    group_fc_ok = True
    group_sample_ok = True
    completed_samples = []
    active_group = None
    latest_arm = {}
    # M-17: generation continuity belongs to the owner's CAN sample stream,
    # not to the TWAI shim's install generation. STOP ACK records the last
    # invalidated source/mode generations and ARM starts a new authorization.
    last_stopped = {}
    epoch_sample_generations = {}
    last_sample_by_boot = {}
    sample_generation_ok = True

    # m-08: canonicalize each observed uint32 generation exactly once.
    # Accept native ints and ASCII decimal strings, never booleans, negative,
    # zero, floats, out-of-range values, missing fields or arbitrary text.
    # All later safety checks reuse these pairs, including Fresh PDU matching.
    def uint32_generation(value):
        if type(value) is int:
            number = value
        elif (type(value) is str and len(value) <= 10 and
              re.fullmatch(r"[0-9]+", value)):
            number = int(value)
        else:
            return None
        return number if 1 <= number <= 0xFFFFFFFF else None

    def generation_pair(event, first, second):
        a = uint32_generation(event.get(first))
        b = uint32_generation(event.get(second))
        return (a, b) if a is not None and b is not None else None

    generation_pairs = {}
    for event in board:
        if event["kind"] in ("sample", "stopped"):
            generation_pairs[id(event)] = generation_pair(
                event, "source_generation", "mode_epoch")
        elif event["kind"] == "packet":
            generation_pairs[id(event)] = generation_pair(
                event, "pdu_source_generation", "pdu_mode_epoch")
    # M-19/M-20: never infer initial generation 2. The runner supplies the
    # pre-fence STOP request + ACK as one unsequenced evidence record; board
    # accounting starts at the FENCE as before. Verify the entire boot-local
    # counter lifecycle independently of whether any sample was acquired.
    # FW: CanSamples starts (1,1) [include/signal_v2_can_samples.h:17],
    # CAN GATT authorization starts epoch 1 [src/signal_v2_gatt.cpp:47-48],
    # STOP/ARM increment the epoch [gatt.cpp:166-205], and each owner.tick
    # epoch transition increments BOTH counters once [can_owner.h:182-186].
    preludes = [e for e in events if e.get("kind") == "initial_stop_evidence"]
    proofs = {}
    first_fences = {}
    for x in board:
        if x["kind"] == "fence" and x.get("boot") not in first_fences:
            first_fences[x["boot"]] = x
    boot_samples = {a.get("boot") for a in arms}
    prelude_ok = True
    for boot in boot_samples:
        candidates = [e for e in preludes if e.get("boot") == boot]
        proof = candidates[0] if len(candidates) == 1 else None
        fence = first_fences.get(boot)
        pair = generation_pair(proof, "source_generation", "mode_epoch") if proof else None
        epoch = uint32_generation(proof.get("epoch")) if proof else None
        req_epoch = uint32_generation(proof.get("request_epoch")) if proof else None
        try:
            req_seq = int(proof["request_seq"]) if proof else -1
            ack_seq = int(proof["ack_seq"]) if proof else -1
            req_us = int(proof["request_t_us"]) if proof else -1
            ack_us = int(proof["ack_t_us"]) if proof else -1
        except (KeyError, ValueError, TypeError):
            req_seq = ack_seq = req_us = ack_us = -1
        ordered = bool(proof and fence and
                       events.index(proof) < events.index(fence) and
                       0 < req_seq < ack_seq < fence["seq"] and
                       0 <= req_us < ack_us < int(fence["t_us"]))
        valid = bool(ordered and pair and epoch and epoch >= 2 and
                     req_epoch == epoch and
                     epoch < 0xFFFFFFFF and pair == (epoch + 1, epoch + 1))
        prelude_ok &= valid
        if valid:
            proofs[boot] = (pair, epoch)
    prelude_ok &= len(preludes) == len(boot_samples)

    arm_expected = {}
    stop_generation_ok = prelude_ok
    generation_state = {}
    for x in board:
        boot = x.get("boot")
        if boot not in generation_state:
            proof = proofs.get(boot)
            generation_state[boot] = {
                "generation": proof[0] if proof else None,
                "epoch": proof[1] if proof else None,
                "cause": None, "fault_idle_advance": False}
        state = generation_state[boot]
        kind_ = x["kind"]
        if kind_ == "arm" and x.get("ok") == "1":
            # After internal owner.stop(), the GATT epoch increments AFTER
            # owner.tick; the next idle tick consumes that change before ARM.
            # [src/signal_v2_gatt.cpp:599-611, can_owner.h:182-186]
            if state["fault_idle_advance"] and state["generation"]:
                state["generation"] = tuple(g + 1 for g in state["generation"])
            state["fault_idle_advance"] = False
            old = state["generation"]
            expect = tuple(g + 1 for g in old) if old else None
            epoch = uint32_generation(x.get("epoch"))
            good = bool(expect and epoch and state["epoch"] and
                        epoch == state["epoch"] + 1 and
                        max(expect) <= 0xFFFFFFFF)
            stop_generation_ok &= good
            arm_expected[(boot, x["seq"])] = expect if good else None
            state["generation"] = expect if good else None
            state["epoch"] = epoch if good else None
            state["cause"] = None
        elif kind_ in ("stop_request", "disconnect", "fault"):
            if kind_ == "stop_request" and state["fault_idle_advance"] and state["generation"]:
                state["generation"] = tuple(g + 1 for g in state["generation"])
                state["fault_idle_advance"] = False
            state["cause"] = kind_
        elif kind_ == "stopped":
            previous = state["generation"]
            expect = tuple(g + 1 for g in previous) if previous else None
            pair = generation_pairs.get(id(x))
            epoch = uint32_generation(x.get("epoch"))
            requested = state["cause"] in ("stop_request", "disconnect", "fault")
            good = bool(requested and expect and pair and epoch and state["epoch"] and
                        epoch == state["epoch"] + 1 and
                        (state["cause"] != "stop_request" or
                         any(r.get("boot") == boot and r.get("epoch") == x.get("epoch")
                             and r["seq"] < x["seq"] for r in kind("stop_request"))) and
                        max(expect) <= 0xFFFFFFFF and pair == expect)
            stop_generation_ok &= good
            state["generation"] = pair if good else None
            state["epoch"] = epoch if good else None
            state["fault_idle_advance"] = bool(good and state["cause"] == "fault")
            state["cause"] = None

    for x in board:
        k = x["kind"]
        boot = x.get("boot")
        if k == "arm":
            latest_arm[boot] = x if x.get("ok") == "1" else None
            active_group = None
        elif k in ("stop_request", "stopped", "fault", "disconnect", "fence"):
            if k == "stopped":
                last_stopped[boot] = x
            latest_arm[boot] = None
            active_group = None
        elif k == "tx":
            if x["data"][:7] == GROUP:
                active_group = {"tx": x, "arm": latest_arm.get(boot),
                                "stage": "group", "fc": None,
                                "last_cf": None, "sampled": False}
            elif x["data"][:1] == b"\x30":
                legal = bool(active_group and active_group["stage"] == "ff" and
                             active_group["fc"] is None and
                             boot == active_group["tx"].get("boot") and
                             x.get("gen") == active_group["tx"].get("gen"))
                group_fc_ok &= legal
                if legal:
                    active_group["fc"] = x
                    active_group["stage"] = "fc"
            else:
                active_group = None  # Essential/probe TX is not a GROUP FC.
        elif (k == "rx" and active_group is not None and x.get("ok") == 1
              and x.get("id") == ECU_RX and x.get("ext") == 1):
            cause = rx_causes.get((x.get("boot"), x["seq"]))
            if (x["data"] == GROUP_FF and active_group["stage"] == "group"
                    and cause is active_group["tx"]):
                active_group["stage"] = "ff"
            elif (x["data"] == GROUP_CF[0] and active_group["stage"] == "fc"
                  and cause is active_group["fc"]):
                active_group["stage"] = "cf0"
            elif (x["data"] == GROUP_CF[1] and active_group["stage"] == "cf0"
                  and cause is active_group["fc"]):
                active_group["last_cf"] = x
                active_group["stage"] = "complete"
        elif k == "sample":
            arm = active_group.get("arm") if active_group else None
            last_cf = active_group.get("last_cf") if active_group else None
            # Each sample must use newer generations than the most recent
            # same-boot STOP ACK. Within one ARM interval, generation changes
            # without another authorization boundary are unexplained and fail.
            generations = generation_pairs[id(x)]
            # M-19/M-20: ARM expectation is reconstructed from the verified
            # pre-FENCE STOP baseline and every intervening STOP ACK. Never
            # substitute a guessed first-ARM generation.
            expected_generations = arm_expected.get((boot, arm["seq"])) if arm else None
            epoch_key = (boot, arm["seq"]) if arm else None
            initial = epoch_sample_generations.get(epoch_key)
            continuous = initial is None or generations == initial
            last_sample = last_sample_by_boot.get(boot)
            newer_than_previous_epoch = bool(
                last_sample is None or last_sample[0] == epoch_key or
                (generations is not None and
                 generations[0] > last_sample[1][0] and
                 generations[1] > last_sample[1][1]))
            generation_valid = bool(
                arm and generations is not None and expected_generations is not None
                and expected_generations[0] <= 0xFFFFFFFF and expected_generations[1] <= 0xFFFFFFFF
                and generations == expected_generations and generations[0] == generations[1]
                and continuous and newer_than_previous_epoch)
            sample_generation_ok &= generation_valid
            valid = bool(
                active_group and arm and last_cf
                and active_group["stage"] == "complete"
                and not active_group["sampled"]
                and x.get("boot") == arm.get("boot") == active_group["tx"].get("boot")
                and x.get("epoch") == arm.get("epoch")
                and x.get("source") == "%08X" % ECU_RX
                and x.get("quality") == "0"
                and generation_valid
                and int(x.get("observed_us", 0)) >= int(last_cf["t"] * 1e6)
                and x["seq"] > last_cf["seq"])
            group_sample_ok &= valid
            if valid:
                completed_samples.append(x)
                active_group["sampled"] = True
                if initial is None:
                    epoch_sample_generations[epoch_key] = generations
                last_sample_by_boot[boot] = (epoch_key, generations)
    tx_after_stop = 0
    for e in txs:
        previous = [x for x in board if x.get("boot") == e.get("boot") and x["seq"] < e["seq"] and x["kind"] in ("stopped", "stop_request", "arm", "fault", "disconnect")]
        if not previous or previous[-1]["kind"] in ("stopped", "stop_request", "fault", "disconnect"):
            tx_after_stop += 1
    first_arm = arms[0]["t"] if arms else 0
    # Every fault run observes 60s, including the post-fault quiet interval.
    duration = fences[-1]["t"] - (arms[-1]["t"] if scenario == "reboot" and arms else first_arm) if fences else 0
    # The final operator STOP is separate evidence from any fault STOP: its
    # request follows every fault/disconnect, then completes, then >=1s quiet.
    requests_all = kind("stop_request")
    final_request = requests_all[-1] if requests_all else None
    # Firmware logs the incremented epoch on the request and acknowledges that
    # same epoch once the owner is idle; a stale ACK cannot complete it.
    final_done = next((x for x in stops if final_request and x["seq"] > final_request["seq"] and
                       x.get("boot") == final_request.get("boot") and
                       x.get("epoch") == final_request.get("epoch")), None)
    # B-08: correlate the operator's recorded host STOP write (events are in
    # host arrival order) after every fault/disconnect and before the request.
    position = {id(e): i for i, e in enumerate(events)}
    stop_writes = [i for i, e in enumerate(events) if e["kind"] == "write" and e["text"] == "SIGV2 CAN STOP"]
    last_trigger = max([position[id(f)] for f in faults + kind("disconnect")], default=-1)
    host_stop = bool(final_request and any(last_trigger < i < position[id(final_request)] for i in stop_writes))
    final_quiet = bool(final_request and final_done and fences and host_stop and
                       all(f["seq"] < final_request["seq"] for f in faults + kind("disconnect")) and
                       fences[-1]["t"] - final_done["t"] >= 1)
    sample_ok = bool(samples) and all(
        e.get("quality") == "0" and e.get("speed") == "0" and e.get("rpm") == "1000" and
        e.get("source") == "18DAF101" and 0 <= e["t"] - int(e.get("observed_us", 0))/1e6 <= .25 and
        int(e.get("epoch", 0)) in epochs for e in samples)
    healthy_first = bool(samples and samples[0]["t"] - first_arm <= 3)
    sustained = len(samples) >= 250 and all(b["t"]-a["t"] <= .5 for a,b in zip(samples,samples[1:]) if b.get("epoch") == a.get("epoch") and b.get("boot") == a.get("boot"))
    if scenario in ("normal","split","rearm"):
        requests=kind("stop_request")
        sustained &= bool(requests and samples and 0 <= requests[-1]["t"]-samples[-1]["t"] <= .5)
    if samples:
        sample_ok &= len(fc_txs) >= len(samples)
        for data in (GROUP_FF,*GROUP_CF):
            sample_ok &= sum(e["id"] == ECU_RX and e["ext"] == 1 and e["data"] == data for e in rxs) >= len(samples)
    after_healthy = scenario in ("busoff", "rxmiss", "buserr", "overflow", "overlap", "disconnect", "reboot")
    fault_injected = True
    if scenario in ("busoff", "rxmiss", "buserr", "overflow", "txfail"):
        command = {"busoff":"BUSOFF", "rxmiss":"RXMISS 1", "buserr":"BUSERR 1", "overflow":"OVERFLOW", "txfail":"TXFAIL 1"}[scenario]
        commands = [e for e in kind("command") if e.get("command") == command]
        fault_injected = len(commands) == 1
        if after_healthy:
            fault_injected &= bool(commands and samples and samples[0]["t"] < commands[0]["t"])
    elif expect_stop and scenario not in ("noreply", "disconnect"):
        model = VirtualEcu(scenario)
        required = [line for _, line in model.on_tx(ECU_TX, PROBE) if line.startswith("HIL RX ")]
        fault_injected = bool(required) and all(expected[l] >= n for l,n in Counter(required).items())
        if scenario == "overlap":
            fault_injected = expected[rx_line(ECU_RX, GROUP_CF[0])] >= 2 and len(arms) == 2
        if scenario == "late":
            fault_injected &= bool(rxs and probe_txs and rxs[0]["t"] - probe_txs[0]["t"] > .120)
    # Exact initial RX forms/counts, not merely the presence of a selected
    # injection somewhere in a run. Later regular CFs belong to group traffic.
    probe_limit=group_txs[0]["t"] if group_txs else float("inf")
    initial_rx=Counter(rx_line(e["id"],e["data"],bool(e["ext"])) for e in rxs if e["t"]<probe_limit)
    initial_model=VirtualEcu(scenario)
    required_rx=Counter(line for _,line in initial_model.on_tx(ECU_TX,PROBE) if line.startswith("HIL RX "))
    exact_initial_rx=initial_rx==required_rx
    def completed_stop(trigger):
        # First completion after the trigger, before any operator STOP request,
        # into a later authorization epoch. A final operator ACK cannot stand in.
        if not trigger:
            return False
        later = [x for x in board if x["seq"] > trigger["seq"] and x.get("boot") == trigger.get("boot")
                 and x["kind"] in ("stopped", "stop_request")]
        if not later or later[0]["kind"] != "stopped":
            return False
        done = later[0]
        armed = [int(a.get("epoch", 0)) for a in arms if a["seq"] < trigger["seq"] and a.get("boot") == trigger.get("boot")]
        return bool(armed) and 0 <= done["t"] - trigger["t"] <= STOP_COMPLETION_S and int(done.get("epoch", 0)) > armed[-1]
    stop_ok = bool(faults and reason in FAULT_REASONS.get(scenario, set()) and
                   faults[0]["t"] - first_arm <= 3 and completed_stop(faults[0])) if expect_stop else not faults
    if expect_stop and scenario in ("busoff","buserr","rxmiss","overflow"):
        stop_ok &= bool(commands and faults and 0 <= faults[0]["t"]-commands[0]["t"] <= .25)
    if scenario == "disconnect":
        disconnects = kind("disconnect")
        stop_ok = bool(disconnects and samples and samples[0]["t"] < disconnects[0]["t"] and
                       completed_stop(disconnects[0]))
    if scenario == "noreply":
        fault_injected = not rxs
    if scenario == "overlap":
        fault_injected = expected[rx_line(ECU_RX, GROUP_CF[0])] >= 2 and len(arms) == 2
    retry_ok = len(discovery_gates) == 2*len(arms) and len(probe_txs) >= len(arms)
    if expect_stop:
        retry_ok &= len(probe_txs) == (2 if scenario == "overlap" else 1)
    if scenario == "txfail":
        retry_ok &= len(txs) == 1 and txs[0]["ok"] == 0
    else:
        retry_ok &= all(e["ok"] == 1 for e in txs)
    for gate in ("owner","monitor"):
        retry_ok &= len([e for e in gates if e.get("gate") == gate]) == len(txs)-len(fc_txs)
    # M-10: each non-FC TX is preceded (since the previous non-FC TX) by
    # exactly one owner and one monitor reservation of the request's cost:
    # 800 bits for a probe/essential read, 2,400 bits for a group.
    pending_gates = []
    paired_gates = True
    for x in board:
        if x["kind"] == "gate":
            pending_gates.append(x)
        elif x["kind"] == "tx" and x["data"][:1] != b"\x30":
            cost = "800" if x["data"][:4] == PROBE else "2400"
            got = sorted((g.get("gate"), g.get("cost")) for g in pending_gates)
            paired_gates &= got == [("monitor", cost), ("owner", cost)]
            pending_gates = []
    paired_gates &= not pending_gates or scenario == "admission"
    gate_ok = paired_gates and all(e.get("cost") in ("800", "2400") and int(e.get("used100", 99999)) <= 5000 and
                  int(e.get("used1000", 99999)) <= 25000 for e in gates)
    for boot in {e.get("boot") for e in arms}:
        for gate in ("owner", "monitor"):
            first = next((e for e in gates if e.get("gate") == gate and e.get("boot") == boot), {})
            gate_ok &= first.get("cost") == "800" and first.get("used100") == "800" and first.get("used1000") == "800"
    if scenario == "admission":
        gate_ok &= all(int(e["used100"]) <= 4000 and int(e["used1000"]) <= 5600 for e in gates)
    # M-15: a Fresh PDU must be backed by a finished GROUP's own valid
    # sample in this authorization epoch, after the last ARM/STOP boundary.
    # A PDU's declared generations are not evidence on their own.
    packet_ok = all(e.get("current") == "1" for e in kind("packet"))
    for e in kind("packet"):
        if not int(e.get("fresh", 0)):
            continue
        active = [a for a in arms if a["seq"] < e["seq"] and a.get("boot") == e.get("boot")]
        closed = [x for x in stops if x["seq"] < e["seq"] and x.get("boot") == e.get("boot")]
        packet_ok &= bool(active) and int(e.get("epoch", 0)) == int(active[-1].get("epoch", -1))
        # Compare the generations carried in the notified PDU itself (B-03).
        pdu_generations = generation_pairs.get(id(e))
        packet_ok &= bool(pdu_generations)
        packet_ok &= e.get("pdu_mixed") == "0" and e.get("pdu_source") == "%08X" % ECU_RX
        if closed:
            stopped_generations = generation_pairs.get(id(closed[-1]))
            packet_ok &= bool(
                pdu_generations and stopped_generations and
                pdu_generations[0] > stopped_generations[0] and
                pdu_generations[1] > stopped_generations[1])
        boundary = next((x for x in reversed(board) if
                         x.get("boot") == e.get("boot") and x["seq"] < e["seq"]
                         and x["kind"] in ("arm", "stop_request", "stopped", "fault", "disconnect")), None)
        matching = any(
            sample["seq"] > boundary["seq"] and sample["seq"] < e["seq"]
            and sample.get("boot") == e.get("boot")
            and sample.get("epoch") == e.get("epoch")
            and sample.get("source") == e.get("pdu_source")
            and pdu_generations is not None
            and generation_pairs.get(id(sample)) == pdu_generations
            for sample in completed_samples) if boundary and boundary["kind"] == "arm" else False
        packet_ok &= matching
    for e in samples + [x for x in kind("packet") if int(x.get("fresh", 0))]:
        previous = [x for x in board if x.get("boot") == e.get("boot") and x["seq"] < e["seq"] and x["kind"] in ("stopped", "stop_request", "arm", "fault", "disconnect")]
        packet_ok &= bool(previous and previous[-1]["kind"] == "arm")
    rearm_ok = True
    if scenario == "overlap":
        rearm_ok = len(arms)==2 and len(stops)>=2 and epochs[1]>int(stops[0]["epoch"])>epochs[0]
        rearm_ok &= bool(faults and faults[0]["t"]>arms[-1]["t"])
        # The old CF must actually overlap the new epoch's probe: after that
        # probe TX and at/before the fault, admitted under its generation.
        new_probe = next((x for x in probe_txs if x["seq"] > arms[-1]["seq"]), None) if arms else None
        rearm_ok &= bool(new_probe and faults and any(
            x["data"] == GROUP_CF[0] and new_probe["seq"] < x["seq"] < faults[0]["seq"] and
            x["ok"] and x.get("gen") == new_probe.get("gen") for x in rxs))
        # Missing Snapshot evidence prevents qualification (runbook): Invalid
        # across the first STOP before re-ARM, and Invalid after the fault.
        snapshots = [e for e in kind("packet") if e.get("type") == "33"]
        rearm_ok &= bool(stops) and any(stops[0]["t"] < e["t"] < arms[-1]["t"] and int(e.get("fresh",0)) == 0 for e in snapshots)
        rearm_ok &= bool(faults) and any(e["t"] > faults[0]["t"] and int(e.get("fresh",0)) == 0 for e in snapshots)
    if scenario == "rearm":
        rearm_ok = len(arms) == 2 and len(stops) >= 2 and epochs[1] > int(stops[0]["epoch"]) > epochs[0]
        old_cf=[e for e in rxs if e["id"]==ECU_RX and e["ext"]==1 and e["data"]==GROUP_CF[0]
                and stops[0]["t"] < e["t"] < arms[-1]["t"]]
        rearm_ok &= len(old_cf)==1 and old_cf[0]["ok"]==0
        # The deliberate stopped-generation RX must be refused, without loss.
        # M-03: the deliberate old CF carries the generation of the group TX
        # pending at the race STOP and is written after that STOP completed
        # and before the re-ARM (host order), then refused in the same interval.
        position = {id(x): i for i, x in enumerate(events)}
        race_tx = [t for t in txs if t["seq"] < stops[0]["seq"] and t["data"][:7] == GROUP and t["ok"]]
        race_gen = str(race_tx[-1].get("gen")) if race_tx else None
        # M-05: run() stops right after the second group TX. Require that
        # group to be still pending at the recorded race STOP write: no final
        # CF and no sample between it and that write.
        race_writes = [i for i, x in enumerate(events) if x["kind"] == "write" and x["text"] == "SIGV2 CAN STOP"
                       and race_tx and position[id(race_tx[-1])] < i < position[id(stops[0])]]
        pending = bool(len(race_tx) >= 2 and race_writes) and not any(
            position[id(race_tx[-1])] < position[id(x)] < race_writes[0] and
            (x["kind"] == "sample" or (x["kind"] == "rx" and x["data"] == GROUP_CF[-1]))
            for x in board)
        rearm_ok &= pending
        acknowledged = reconcile(lambda e, w: e["id"] == ECU_RX and e["ext"] == 1 and e["data"] == GROUP_CF[0]
                                 and stops[0]["t"] < e["t"] < arms[-1]["t"]
                                 and w["text"].split()[2] == race_gen
                                 and position[id(stops[0])] < position[id(w)] < position[id(arms[-1])])
        rearm_ok &= any(int(e["epoch"]) == epochs[-1] for e in samples)
        snapshots = [e for e in kind("packet") if e.get("type") == "33"]
        rearm_ok &= any(stops[0]["t"] < e["t"] < arms[-1]["t"] and int(e.get("fresh",0)) == 0 for e in snapshots)
        rearm_ok &= any(e["t"]>arms[-1]["t"] and int(e.get("epoch",0))==epochs[-1] and int(e.get("fresh",0))>0
                        and generation_pairs.get(id(e)) is not None
                        and generation_pairs.get(id(stops[0])) is not None
                        and generation_pairs[id(e)][0] > generation_pairs[id(stops[0])][0]
                        and generation_pairs[id(e)][1] > generation_pairs[id(stops[0])][1]
                        for e in snapshots)
        rearm_ok &= all(int(e["epoch"]) == max(int(a["epoch"]) for a in arms if a["t"] <= e["t"]) for e in samples)
    # Reboot cannot be inferred from an ordinary STOP. A new boot identity and
    # reconnected secured central are separate evidence supplied by the operator.
    if scenario == "reboot":
        rearm_ok = len(arms) == 2 and len(set(e.get("boot") for e in arms)) == 2
        rearm_ok &= all(any(s.get("boot") == a.get("boot") and 0 <= s["t"]-a["t"] <= 3 for s in samples) for a in arms)
        rearm_ok &= all(e.get("boot") for e in arms)
    peaks = {"peak100_bits":0,"peak1000_bits":0}
    for boot in {e.get("boot") for e in frames}:
        times=[e["t"] for e in frames if e.get("boot") == boot]
        for overflow in kind("overflow"):
            if overflow.get("boot") == boot:
                times += [overflow["t"]] * (int(overflow.get("accepted",0))+int(overflow.get("rejected",0)))
        bp=window_peaks(times)
        for k,v in bp.items():peaks[k]=max(peaks[k],v)
    checks = {
        "transport_complete": complete and not errors and acknowledged and generation_bound,
        "fresh_fenced_epoch": epoch_ok and len(fences) >= 2 and fences[-1].get("nonce") == fences[0].get("nonce", "")+"end" and fences[0]["seq"] < arms[0]["seq"] if arms else False,
        "scenario_exercised": bool(probe_txs) and fault_injected and exact_initial_rx and (not after_healthy or healthy_first),
        "stop_as_expected": stop_ok,
        "stop_ack_and_quiet": final_quiet,
        "observation_60s": duration >= 60,
        "no_tx_after_stop": tx_after_stop == 0,
        "no_functional_tx": not any(e["id"] in (0x7DF, 0x18DB33F1) for e in raw_txs),
        "tx_allowlist": tx_allowed,
        "group_fc_causality": group_fc_ok,
        "group_sample_causality": group_sample_ok,
        "no_fc_on_probe_ff": (scenario not in ("ff_probe", "cf_probe", "malformed", "extra", "duplicate", "wrong_src", "eleven_bit", "negative", "late", "noreply") or not fc_txs) and not discovery_fc,
        "positive_acquisition": (sample_ok and group_sample_ok and healthy_first and bool(group_txs) and bool(fc_txs)) if not expect_stop or after_healthy else not samples,
        "sustained_cadence": sustained if scenario in ("normal", "split", "rearm") else True,
        "both_admission_gates": gate_ok and retry_ok,
        "epoch_separation": rearm_ok and packet_ok and sample_generation_ok and stop_generation_ok,
        "within_100ms_cap": peaks["peak100_bits"] <= CAP_100MS,
        "within_1s_cap": peaks["peak1000_bits"] <= CAP_1S,
    }
    if scenario == "overflow":
        # This scenario deliberately exceeds the RX envelope. Preserve the high
        # measured load; qualify only the expected fail-closed reaction.
        checks["within_100ms_cap"] = peaks["peak100_bits"] > CAP_100MS and stop_ok
        checks["within_1s_cap"] = peaks["peak1000_bits"] > CAP_1S and stop_ok
        checks["intentional_rx_overload_stopped"] = stop_ok
        checks["overflow_observed"] = any(int(e.get("accepted", 0)) == 256 and e.get("rejected") == "1" for e in kind("overflow"))
    if scenario == "overflow":
        checks["expected_100ms_rx_overload"] = checks.pop("within_100ms_cap")
        checks["expected_1s_rx_overload"] = checks.pop("within_1s_cap")
    return {"scenario": scenario, "pass": all(checks.values()), "checks": checks,
            "stop_reason": reason, "tx_after_stop": tx_after_stop, "pinned": bool(samples),
            "independent_load": peaks,
            "note": "Board-timed virtual events at 200 bits/frame; no electrical/retry proof."}


def run(port, scenario, seconds, trace_path):
    import serial
    ecu = VirtualEcu(scenario)
    ser = serial.Serial(port, 115200, timeout=.001, write_timeout=.1)
    ser.reset_input_buffer()
    events, scheduled = [], []
    nonce = secrets.token_hex(16)
    start = time.monotonic()
    buf = b""
    phase, phase_at, stopped_once = "initial_stop", start, False
    fence_sent, final_sent = False, False
    arm_host = None
    race_generation = 0
    active_boot, time_offset = None, 0.0
    reboot_seen = False
    ready_deadline = 0
    current_generation = 0
    initial_deadline = start + 5
    pending_initial_stop = None

    def send(line, record=True, cause=None):
        if line.startswith("HIL RX 0 "):
            line = line.replace("HIL RX 0 ", "HIL RX %d " % current_generation, 1)
        wire = (line + "\n").encode()
        if ser.write(wire) != len(wire):
            raise RuntimeError("short serial write")
        if record:
            w = {"kind":"write", "host_t":time.monotonic()-start, "text":line}
            if cause:
                w["from_t"], w["from_gen"] = cause
            events.append(w)

    # Bounded state machine: read the STOP ACK before fencing or arming; keep
    # reading through final STOP and a full quiet second before final fence.
    try:
        send("SIGV2 CAN STOP",False)
        while time.monotonic()-start < seconds+(60 if scenario == "reboot" else 20):
            now = time.monotonic()
            if phase in ("initial_stop", "fencing") and now > initial_deadline:
                events.append({"kind":"parse_error", "text":"handshake timeout"})
                break
            if phase == "initial_stop" and now-phase_at > 1:
                # Reissue STOP: idle firmware may already have acknowledged its
                # boot epoch before this process opened the port.
                send("SIGV2 CAN STOP", False); phase_at = now
            for item in list(scheduled):
                due,line,cause = item
                if due <= now:
                    scheduled.remove(item); send(line, cause=cause)
            try:
                buf += ser.read(4096)
            except serial.SerialException:
                if scenario != "reboot" or not any(e["kind"] == "sample" for e in events):
                    raise
                ser.close()
                # USB re-enumeration is expected only for this operator-driven
                # scenario. Reconnect the specified port; never reset hardware.
                reconnect_until=time.monotonic()+10
                while True:
                    try:
                        ser=serial.Serial(port,115200,timeout=.001,write_timeout=.1)
                        break
                    except serial.SerialException:
                        if time.monotonic()>=reconnect_until:raise
                        time.sleep(.05)
                phase="initial_stop";phase_at=time.monotonic();initial_deadline=phase_at+10
                pending_initial_stop = None
                scheduled.clear();send("SIGV2 CAN STOP",False)
                continue
            while b"\n" in buf:
                raw,buf = buf.split(b"\n",1)
                text = raw.decode(errors="replace").strip()
                if not text:
                    continue
                if phase == "initial_stop":
                    # M-19: do not throw away the only known starting counter
                    # state. Preserve a matched, sequenced board STOP request
                    # and STOP ACK before FENCE; a loose serial ack line or a
                    # previously buffered STOPPED event is never sufficient.
                    try:
                        pre = parse_event(text)
                    except (ValueError, KeyError):
                        events.append({"kind": "parse_error", "text": text})
                        continue
                    if pre and pre.get("kind") == "stop_request":
                        pending_initial_stop = pre
                    elif pre and pre.get("kind") == "stopped" and pending_initial_stop:
                        request = pending_initial_stop
                        if (pre.get("boot") == request.get("boot") and
                            pre.get("epoch") == request.get("epoch") and
                            pre.get("seq", 0) > request.get("seq", 0)):
                            events.append({
                                "kind": "initial_stop_evidence",
                                "boot": pre.get("boot"),
                                "epoch": pre.get("epoch"),
                                "request_epoch": request.get("epoch"),
                                "request_seq": request.get("seq"),
                                "ack_seq": pre.get("seq"),
                                "request_t_us": request.get("t_us"),
                                "ack_t_us": pre.get("t_us"),
                                "source_generation": pre.get("source_generation"),
                                "mode_epoch": pre.get("mode_epoch"),
                            })
                            pending_initial_stop = None
                            send("HIL FENCE " + nonce, False)
                            phase="fencing";phase_at=now
                    continue
                try:
                    e = parse_event(text)
                except (ValueError, KeyError):
                    e = {"kind":"parse_error", "text":text}
                if e is None:
                    e = {"kind":"line", "text":text}
                e["host_t"] = now-start
                if "gen" in e: current_generation=e["gen"]
                if scenario == "reboot" and e.get("boot") and active_boot and e["boot"] != active_boot and phase != "fencing":
                    if reboot_seen or not any(x["kind"] == "sample" for x in events):
                        events.append({"kind":"parse_error", "text":"unexpected reboot"});break
                    reboot_seen=True;phase="initial_stop";phase_at=now;initial_deadline=now+10
                    pending_initial_stop = None
                    scheduled.clear();send("SIGV2 CAN STOP",False)
                    continue
                if phase == "fencing":
                    if e.get("kind") != "fence" or e.get("nonce") != nonce:
                        continue
                    phase="running";phase_at=now
                    if active_boot and e.get("boot") != active_boot:
                        time_offset=max(x.get("t",0) for x in events)+1-e["t"]
                    active_boot=e.get("boot")
                    e["raw_t_us"]=e.get("t_us");e["t"]+=time_offset
                    events.append(e)
                    if scenario == "reboot" and reboot_seen:
                        phase="reboot_ready";phase_at=now;ready_deadline=now+10
                        send("SIGV2 CAN READY")
                    else:
                        for cmd in ecu.setup(): send(cmd)
                        send("SIGV2 CAN ARM");arm_host=now
                    continue
                if "seq" in e:
                    e["raw_t_us"]=e.get("t_us");e["t"]+=time_offset
                    if "observed_us" in e:
                        e["raw_observed_us"]=e["observed_us"]
                        e["observed_us"]=str(int(e["observed_us"])+round(time_offset*1e6))
                events.append(e)
                k=e["kind"]
                if phase == "reboot_ready" and k == "ready" and e.get("ok") == "1":
                    send("SIGV2 CAN ARM");arm_host=now;phase="running"
                if k == "tx" and e.get("ok"):
                    for delay,line in ecu.on_tx(e["id"], e["data"]):
                        scheduled.append((now+delay,line,(e["t"],e["gen"])))
                    if scenario in ("rearm", "overlap") and ecu.groups == 2 and not stopped_once:
                        race_generation=e["gen"];scheduled.clear();send("SIGV2 CAN STOP");stopped_once=True;phase="race_stop"
                if k == "stopped":
                    phase_at=now
                    if phase == "race_stop":
                        # Late completion arrives after the acknowledged stable
                        # drain boundary, then re-ARM only after its rejection.
                        if scenario == "rearm":
                            send(rx_line(ECU_RX,GROUP_CF[0],generation=race_generation));phase="old_completion"
                        else:
                            phase="old_quiet"
                    elif final_sent:
                        phase="quiet"
                if phase == "old_completion" and k == "rx":
                    phase="old_quiet";phase_at=now
                if scenario == "admission" and k == "sample" and not final_sent:
                    send("SIGV2 CAN STOP");final_sent=True;phase="final_stop"
                if fence_sent and k == "fence" and e.get("nonce") == nonce+"end":
                    return evaluate(scenario,events), events
            if phase == "reboot_ready" and now-phase_at >= .1:
                if now > ready_deadline:
                    events.append({"kind":"parse_error","text":"central reconnect timeout"});break
                send("SIGV2 CAN READY");phase_at=now
            if phase == "old_quiet" and now-phase_at >= 1:
                send("SIGV2 CAN ARM");phase="running"
            if phase not in ("initial_stop","fencing","reboot_ready") and arm_host is not None and now-arm_host >= seconds and not final_sent:
                scheduled.clear();send("SIGV2 CAN STOP");final_sent=True;phase="final_stop";phase_at=now
            if phase == "quiet" and now-phase_at >= 1 and arm_host is not None and now-arm_host >= seconds and not fence_sent:
                send("HIL FENCE " + nonce+"end");fence_sent=True
        events.append({"kind":"parse_error", "text":"phase timeout: "+phase})
        return evaluate(scenario,events), events
    finally:
        ser.close()
        # Save all records including load lines, host scheduling, rejected RX,
        # attempts, board sequence/timestamps and partial/malformed evidence.
        with open(trace_path,"w") as out:
            for e in events:
                out.write(json.dumps(e,default=lambda v:v.hex() if isinstance(v,bytes) else str(v))+"\n")
            if buf: out.write(json.dumps({"kind":"partial","text":buf.decode(errors="replace")})+"\n")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True)
    p.add_argument("--scenario", default="normal", choices=sorted(SCENARIOS))
    p.add_argument("--seconds", type=float, default=60.0)
    p.add_argument("--trace", required=True, help="complete JSONL evidence artifact")
    a = p.parse_args()
    if a.seconds < 60:
        p.error("qualification requires at least 60 seconds")
    result, _ = run(a.port,a.scenario,a.seconds,a.trace)
    print(json.dumps(result,indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
