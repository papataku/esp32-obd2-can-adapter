"""Synthetic time-series ECU replay: *only* monotonic relative model time.

No original RP8 model, conversion script, VIN, positions or absolute time.
"""
import copy
import json
from pathlib import Path

import pytest

from tools.elm327_sim import CANFrame, ECUSimulator, ELM327Session, load_model
from tools.elm327_sim.model import parse_model

PATH = Path(__file__).parent / "fixtures" / "synthetic_timeseries.json"


def raw_model():
    return json.loads(PATH.read_text(encoding="utf-8"))


def session(protocol=7, ecu="01"):
    device = ELM327Session(load_model(PATH))
    assert device.request("ATE0") == b"ATE0\rOK\r>"
    assert device.request("ATAT0") == b"OK\r>"
    assert device.request("ATSP" + str(protocol)) == b"OK\r>"
    assert device.request("ATH1") == b"OK\r>"
    target = f"ATSH18DA{ecu}F1" if protocol in (7, 9) else "ATSH7E0" if ecu == "01" else "ATSH7E2"
    assert device.request(target) == b"OK\r>"
    return device


def at(device, now_ms):
    assert now_ms >= device.virtual_now_ms
    device.advance_virtual_time(now_ms - device.virtual_now_ms)


@pytest.mark.parametrize("clock,pdu", [
    (0, "410C0000"), (99, "410C0000"),
    (100, "410C1000"), (199, "410C1000"),
    (300, "410C3000"), (399, "410C3000"),
    (400, "410C4000"), (499, "410C4000"),
    (500, "410C0000"), (599, "410C0000"),
    (600, "410C1000"), (999, "410C4000"),
    (1000, "410C0000")
])
@pytest.mark.parametrize("proto,header", [(6, "7E8"), (7, "18DAF101"),
                                           (8, "7E8"), (9, "18DAF101")])
def test_clocked_loop_boundaries_on_all_four_can_protocols(proto, header, clock, pdu):
    dev = session(proto)
    at(dev, clock)
    expected = f"{header} " + " ".join(
        pdu[i:i + 2] for i in range(0, len(pdu), 2)
    ) + "\r>"
    assert dev.request("010C") == expected.encode("ascii")


@pytest.mark.parametrize("clock", [200, 299, 700, 799])
@pytest.mark.parametrize("proto", [6, 7, 8, 9])
def test_clocked_loop_no_data_intervals(proto, clock):
    dev = session(proto)
    at(dev, clock)
    assert dev.request("010C") == b"NO DATA\r>"


def test_sequence_sample_and_hold_after_final_time():
    dev = session(7, "02")
    assert dev.request("010C") == b"18DAF102 41 0C 00 50\r>"
    at(dev, 119)
    assert dev.request("010C") == b"18DAF102 41 0C 00 50\r>"
    at(dev, 120)
    assert dev.request("010C") == b"NO DATA\r>"
    at(dev, 239)
    assert dev.request("010C") == b"NO DATA\r>"
    at(dev, 240)
    assert dev.request("010C") == b"18DAF102 41 0C 00 F0\r>"
    at(dev, 20_000)
    assert dev.request("010C") == b"18DAF102 41 0C 00 F0\r>"


def test_fixed_payload_legacy_record_stays_fixed_at_late_clock():
    dev = session()
    assert dev.request("0105") == b"18DAF101 41 05 50\r>"
    at(dev, 100_000)
    assert dev.request("0105") == b"18DAF101 41 05 50\r>"


def test_single_sample_per_pid_is_repeatable_at_same_clock():
    dev = session()
    at(dev, 100)
    for _ in range(4):
        assert dev.request("010C") == b"18DAF101 41 0C 10 00\r>"
    assert dev.virtual_now_ms == 100


@pytest.mark.parametrize("clock,result", [
    (100, b"NO DATA\r>"),  # sample delays 30ms; ST04 gives 16ms
    (200, b"NO DATA\r>"),  # no_data sample, independent of timeouts
    (300, b"18DAF101 41 0C 30 00\r>"),
    (400, b"18DAF101 41 0C 40 00\r>"),
])
def test_per_sample_delay_and_no_data_with_st_deadline(clock, result):
    dev = session()
    assert dev.request("ATST04") == b"OK\r>"
    at(dev, clock)
    assert dev.request("010C") == result


def test_same_sample_delay_succeeds_when_timeout_increased():
    dev = session()
    at(dev, 100)
    assert dev.request("ATST07") == b"OK\r>"  # 28ms < sample delay 30ms
    assert dev.request("010C") == b"NO DATA\r>"
    assert dev.request("ATST08") == b"OK\r>"  # 32ms >= delay
    assert dev.request("010C") == b"18DAF101 41 0C 10 00\r>"


def test_drop_every_plus_sample_no_data_and_delay():
    dev = session()
    # ECU01 speed 010D: drop_every 3; sample 100ms delay40; 200ms no data.
    assert dev.request("010D") == b"18DAF101 41 0D 0A\r>"  # attempt 1
    at(dev, 100)
    assert dev.request("ATST08") == b"OK\r>"
    assert dev.request("010D") == b"NO DATA\r>"  # attempt 2, delayed past 32ms
    at(dev, 150)
    assert dev.request("ATST32") == b"OK\r>"
    assert dev.request("010D") == b"NO DATA\r>"  # attempt 3, drop_every
    at(dev, 200)
    assert dev.request("010D") == b"NO DATA\r>"  # attempt 4, explicit no_data
    at(dev, 300)
    assert dev.request("010D") == b"18DAF101 41 0D 1E\r>"  # attempt 5, delay 1ms
    at(dev, 400)
    assert dev.request("010D") == b"NO DATA\r>"  # attempt 6, drop_every
    at(dev, 401)
    assert dev.request("010D") == b"18DAF101 41 0D 1E\r>"  # attempt 7, held


def test_no_stale_delayed_response_leaks_to_later_pid():
    dev = session()
    at(dev, 100)
    assert dev.request("ATST01") == b"OK\r>"
    assert dev.request("010C") == b"NO DATA\r>"
    assert dev.request("0105") == b"18DAF101 41 05 50\r>"
    assert dev.request("010C") == b"NO DATA\r>"
    at(dev, 300)
    assert dev.request("010C") == b"NO DATA\r>"  # delay5ms exceeds ST01 4ms
    assert dev.request("0105") == b"18DAF101 41 05 50\r>"


def test_multiple_ecus_have_independent_clocked_sequences():
    dev = ELM327Session(load_model(PATH))
    dev.request("ATE0")
    dev.request("ATSP7")
    dev.request("ATH1")
    dev.request("ATAT0")
    assert dev.request("010C") == (
        b"18DAF101 41 0C 00 00\r"
        b"18DAF102 41 0C 00 50\r>"
    )
    at(dev, 150)
    assert dev.request("010C") == b"18DAF101 41 0C 10 00\r>"
    at(dev, 250)
    assert dev.request("010C") == b"18DAF102 41 0C 00 F0\r>"
    at(dev, 500)
    assert dev.request("010C") == (
        b"18DAF101 41 0C 00 00\r"
        b"18DAF102 41 0C 00 F0\r>"
    )


def test_direct_can_frame_replay_on_virtual_clock():
    bus = ECUSimulator(load_model(PATH))
    f = CANFrame(0x18DA01F1, b"\x02\x01\x0c", True)
    assert bus.on_frame(f, 0) == []
    assert bus.tick(9) == []
    assert bus.tick(10) == [CANFrame(0x18DAF101, b"\x04\x41\x0c\x00\x00", True)]
    assert bus.on_frame(f, 100) == []
    assert bus.tick(129) == []
    assert bus.tick(130) == [CANFrame(0x18DAF101, b"\x04\x41\x0c\x10\x00", True)]
    assert bus.on_frame(f, 200) == []
    assert bus.tick(1000) == []


@pytest.mark.parametrize("bad_mode", ["stepped", "LOOP", "", 42, None, True])
def test_replay_mode_is_strictly_versioned(bad_mode):
    data = raw_model()
    data["ecus"][0]["responses"][0]["replay_mode"] = bad_mode
    with pytest.raises(ValueError):
        parse_model(data)


@pytest.mark.parametrize("bad_samples", [[], "series", {}, None])
def test_sequence_or_loop_requires_nonempty_bounded_array(bad_samples):
    data = raw_model()
    data["ecus"][0]["responses"][0]["samples"] = bad_samples
    with pytest.raises(ValueError):
        parse_model(data)


@pytest.mark.parametrize("mutation", [
    [{"at_ms": 5, "payload_hex": "410C0000"}],
    [{"at_ms": 0, "payload_hex": "410C0000"}, {"at_ms": 0, "payload_hex": "410C0001"}],
    [{"at_ms": 0, "payload_hex": "410C0000"}, {"at_ms": -1, "payload_hex": "410C0001"}],
    [{"at_ms": 0, "payload_hex": "410C0000"}, {"at_ms": 2, "payload_hex": "410C0001"},
     {"at_ms": 1, "payload_hex": "410C0002"}],
    [{"at_ms": False, "payload_hex": "410C0000"}],
    [{"at_ms": 2.0, "payload_hex": "410C0000"}],
    [{"at_ms": 2**53 + 1, "payload_hex": "410C0000"}],
])
def test_time_series_rejects_nonmonotonic_or_noninteger_times(mutation):
    data = raw_model()
    data["ecus"][0]["responses"][0]["samples"] = mutation
    with pytest.raises(ValueError):
        parse_model(data)


@pytest.mark.parametrize("sample", [
    {"at_ms": 0},
    {"at_ms": 0, "payload_hex": "410CFFFF", "no_data": True},
    {"at_ms": 0, "payload_hex": "410D0000"},
    {"at_ms": 0, "payload_hex": "410CZZZZ"},
    {"at_ms": 0, "payload_hex": "410C0000", "delay_ms": -1},
    {"at_ms": 0, "payload_hex": "410C0000", "delay_ms": True},
    {"at_ms": 0, "payload_hex": "410C0000", "delay_ms": 60_001},
    {"at_ms": 0, "no_data": "true"},
    {"at_ms": 0, "no_data": True, "VIN": "BAD"},
    {"at_ms": 0, "payload_hex": "410C0000", "timestamp": 12345}
])
def test_sample_payload_delay_no_data_and_private_fields_are_validated(sample):
    data = raw_model()
    data["ecus"][0]["responses"][0]["samples"] = [sample]
    with pytest.raises(ValueError):
        parse_model(data)


@pytest.mark.parametrize("bad", [0, 400, -1, True, "500", 2**53 + 1, None])
def test_loop_period_must_be_integer_larger_than_last_at_ms(bad):
    data = raw_model()
    data["ecus"][0]["responses"][0]["loop_period_ms"] = bad
    with pytest.raises(ValueError):
        parse_model(data)


def test_sequence_mode_rejects_loop_period_and_legacy_time_conflict():
    data = raw_model()
    target = data["ecus"][0]["responses"][1]
    target["loop_period_ms"] = 500
    with pytest.raises(ValueError):
        parse_model(data)
    del target["loop_period_ms"]
    target["relative_timestamps_ms"] = [0, 100]
    with pytest.raises(ValueError):
        parse_model(data)


def test_fixed_mode_rejects_samples_but_accepts_legacy_times():
    data = raw_model()
    fixed = data["ecus"][0]["responses"][2]
    fixed["relative_timestamps_ms"] = [0, 100]
    assert parse_model(data).ecus[0].responses[2].replay_mode == "fixed"
    fixed["samples"] = [{"at_ms": 0, "payload_hex": "410550"}]
    with pytest.raises(ValueError):
        parse_model(data)


def test_provenance_restriction_unchanged_for_series():
    data = raw_model()
    data["provenance"] = "anonymized_observation"
    assert parse_model(data).provenance == "anonymized_observation"
    data["provenance"] = "raw_vehicle"
    with pytest.raises(ValueError):
        parse_model(data)


def test_default_loader_size_limit_unchanged_for_series(tmp_path):
    from tools.elm327_sim.model import DEFAULT_MAX_MODEL_BYTES
    path = tmp_path / "synthetic_only.json"
    path.write_text(json.dumps(raw_model()) + (" " * DEFAULT_MAX_MODEL_BYTES))
    with pytest.raises(ValueError, match="too large"):
        load_model(path)
    assert load_model(path, max_bytes=3_000_000).ecus[0].responses[0].replay_mode == "loop"


def test_virtual_cdc_29bit_uses_timestamp_for_loop_and_silence():
    """USB CDC packet timestamps drive same ECU simulation without CAN hardware."""
    from tools.elm327_sim.vcan import (
        VirtualCDCBridge, VirtualFrame, CAN_TX, CAN_RX, HELLO,
        HELLO_TOKEN, encode_packet, decode_packet
    )
    bridge = VirtualCDCBridge(ECUSimulator(load_model(PATH)))
    hello = VirtualFrame(HELLO, 1, 0, data=HELLO_TOKEN)
    assert decode_packet(bridge.handle(encode_packet(hello))[0]).kind == HELLO
    def tx(seq, t_ms):
        packet = VirtualFrame(CAN_TX, seq, t_ms * 1000, 0x18DA01F1,
                              b"\x02\x01\x0C", True)
        return [decode_packet(b) for b in bridge.handle(encode_packet(packet))]
    assert tx(2, 0) == []
    assert bridge.tick(9_000) == []
    frames = [decode_packet(b) for b in bridge.tick(10_000)]
    assert len(frames) == 1
    assert frames[0].kind == CAN_RX and frames[0].can_id == 0x18DAF101
    assert frames[0].data == b"\x04\x41\x0C\x00\x00"
    assert tx(3, 100) == []
    assert bridge.tick(129_000) == []
    assert [decode_packet(b).data for b in bridge.tick(130_000)] == [
        b"\x04\x41\x0C\x10\x00"
    ]
    assert tx(4, 200) == []
    assert bridge.tick(299_000) == []
    assert tx(5, 300) == []
    assert [decode_packet(b).data for b in bridge.tick(305_000)] == [
        b"\x04\x41\x0C\x30\x00"
    ]
    assert tx(6, 500) == []
    assert [decode_packet(b).data for b in bridge.tick(510_000)] == [
        b"\x04\x41\x0C\x00\x00"
    ]


def test_model_rejects_more_than_10000_samples_without_committing_large_fixture():
    model = raw_model()
    target = model["ecus"][0]["responses"][0]
    target["samples"] = [
        {"at_ms": n, "payload_hex": "410C0000"}
        for n in range(10_001)
    ]
    with pytest.raises(ValueError, match="bounded array"):
        parse_model(model)


def test_loop_exact_period_and_no_data_sample_remain_stable():
    parsed = load_model(PATH)
    rpm = parsed.ecus[0].responses[0]
    assert rpm.loop_period_ms == 500
    assert rpm.samples[0].at_ms == 0
    assert rpm.samples[2].payload is None
    assert rpm.samples[2].delay_ms is None
    assert parsed.provenance == "synthetic"
