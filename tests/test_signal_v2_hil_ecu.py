#!/usr/bin/env python3
"""Host checks for the serial HIL virtual ECU (no device, no pyserial)."""
import pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import signal_v2_hil_ecu as h

def tx(ecu, data):
    return ecu.on_tx(h.ECU_TX, data)

# Healthy: probe -> one combined SF; group -> FF, CFs only after the adapter's FC.
e = h.VirtualEcu("normal")
assert [l for _, l in tx(e, h.PROBE + b"\0" * 4)] == [h.rx_line(h.ECU_RX, h.PAIR_SF)]
out = tx(e, h.GROUP + b"\0")
assert len(out) == 1 and out[0][1].startswith("HIL RX 18DAF101 1 1011")
cfs = tx(e, bytes([0x30, 0, 0]))
assert [l for _, l in cfs] == [h.rx_line(h.ECU_RX, cf) for cf in h.GROUP_CF]
assert tx(e, bytes([0x30, 0, 0])) == []          # no CF without a pending FF
assert h.VirtualEcu("normal").on_tx(0x18DB33F1, h.PROBE) == []  # functional ignored

# Each fault scenario produces its fault on the first probe.
assert len(tx(h.VirtualEcu("extra"), h.PROBE)) == 3
assert tx(h.VirtualEcu("ff_probe"), h.PROBE)[0][1].split()[-1].startswith("1008")
assert tx(h.VirtualEcu("late"), h.PROBE)[0][0] > 0.120
assert "7E8 0" in tx(h.VirtualEcu("eleven_bit"), h.PROBE)[0][1]
assert tx(h.VirtualEcu("noreply"), h.PROBE) == []
assert h.VirtualEcu("txfail").setup() == ["HIL TXFAIL 1"]
b = h.VirtualEcu("busoff"); tx(b, h.PROBE); tx(b, h.GROUP); assert "HIL BUSOFF" in [l for _, l in tx(b, h.GROUP)]
assert set(h.SCENARIOS) >= {"normal", "split", "noreply", "extra", "ff_probe", "late",
                            "duplicate", "wrong_src", "eleven_bit", "negative", "txfail", "busoff", "rxmiss"}

# Evaluation: healthy pin passes; TX after STOP, functional TX or cap overrun fail.
ok = [{"t": 0.0, "kind": "tx", "id": h.ECU_TX, "data": h.PROBE, "text": ""},
      {"t": 0.005, "kind": "rx", "text": ""},
      {"t": 0.01, "kind": "line", "text": "#SIGNALV2_ECU,physical_response=18DAF101,probes=1"}]
assert h.evaluate("normal", ok)["pass"]
bad = [{"t": 0.0, "kind": "line", "text": "#SIGNALV2_CAN,stopped=1,reason=NO_VERIFIED_ECU,invalid=1"},
       {"t": 0.2, "kind": "tx", "id": h.ECU_TX, "data": h.PROBE, "text": ""}]
r = h.evaluate("noreply", bad)
assert not r["pass"] and r["tx_after_stop"] == 1 and r["stop_reason"] == "NO_VERIFIED_ECU"
func = [{"t": 0.0, "kind": "tx", "id": 0x18DB33F1, "data": h.PROBE, "text": ""}]
assert not h.evaluate("normal", func)["checks"]["no_functional_tx"]
burst = [{"t": i * 0.001, "kind": "rx", "text": ""} for i in range(26)]
assert h.window_peaks([e["t"] for e in burst])["peak100_bits"] == 5200
assert not h.evaluate("noreply", burst)["checks"]["within_100ms_cap"]
print("Serial HIL virtual ECU scenarios and evaluator PASS")
