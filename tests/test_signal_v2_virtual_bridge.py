#!/usr/bin/env python3
"""V2-2 host adapter compatibility smoke against #61's virtual time-series.
The #61 simulator only natively routes single-PID Service01. This verifies
clocked 29bit physical ECU provenance and invalids, not five-PID grouped CAN.
"""
import pathlib
import sys
ROOT = pathlib.Path(sys.argv[1]).resolve()
sys.path.insert(0, str(ROOT))
from tools.elm327_sim import ELM327Session, load_model
MODEL = ROOT / "tests/elm327_conformance/fixtures/synthetic_timeseries.json"

def device():
    client = ELM327Session(load_model(MODEL))
    assert client.request("ATE0") == b"ATE0\rOK\r>"
    assert client.request("ATAT0") == b"OK\r>"
    assert client.request("ATSP7") == b"OK\r>"
    assert client.request("ATH1") == b"OK\r>"
    assert client.request("ATSH18DA01F1") == b"OK\r>"
    return client

for interval in (200, 100):
    ecu = device()
    good = invalid = 0
    for now in range(0, 1000, interval):
        ecu.advance_virtual_time(now - ecu.virtual_now_ms)
        result = ecu.request("010C")
        if result == b"NO DATA\r>":
            invalid += 1
        else:
            assert result.startswith(b"18DAF101 41 0C ")
            good += 1
    assert good > 0 and invalid > 0, (interval, good, invalid)
    print("virtual 29bit time-series", interval, "ms valid", good,
          "invalid", invalid)
# 11bit response is present in the model but MUST be rejected by V2-2 policy
# (the C++ isolated CAN policy test directly enforces that rejection).
model11 = ELM327Session(load_model(MODEL))
assert model11.request("ATE0") == b"ATE0\rOK\r>"
assert model11.request("ATSP6") == b"OK\r>"
assert model11.request("ATH1") == b"OK\r>"
assert b"7E8" in model11.request("010C")
print("#61 virtual ECU time-series and 11bit detection evidence PASS")
