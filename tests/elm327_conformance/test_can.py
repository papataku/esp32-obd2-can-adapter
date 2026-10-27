import pytest
from tools.elm327_sim import CANFrame, ECUSimulator, load_model
from pathlib import Path

PATH = Path(__file__).parent / "fixtures" / "synthetic_ecus.json"


@pytest.fixture
def bus():
    return ECUSimulator(load_model(PATH))


@pytest.mark.parametrize("extended,request_id,ids", [
    (False, 0x7DF, {0x7E8, 0x7EA}),
    (True, 0x18DB33F1, {0x18DAF101, 0x18DAF102, 0x18DAF10E, 0x18DAF1EF}),
])
def test_functional_multi_ecu_fanout(bus, extended, request_id, ids):
    bus.on_frame(CANFrame(request_id, b"\x02\x01\x0c", extended), 0)
    frames = bus.tick(100)
    assert {x.can_id for x in frames} == ids
    assert all(x.data[1:3] == b"\x41\x0c" for x in frames)


@pytest.mark.parametrize("extended,req,rid", [
    (False, 0x7E0, 0x7E8),
    (True, 0x18DA01F1, 0x18DAF101),
])
def test_physical_single_ecu_routing(bus, extended, req, rid):
    bus.on_frame(CANFrame(req, b"\x02\x01\x0d", extended), 0)
    frames = bus.tick(100)
    assert len(frames) == 1
    assert frames[0].can_id == rid
    assert frames[0].data == b"\x03\x41\x0d\x28"


def test_isotp_fc_required_and_wrong_ecu_fc_not_accepted(bus):
    bus.on_frame(CANFrame(0x18DA01F1, b"\x02\x01\x9a", True), 0)
    first = bus.tick(100)
    assert len(first) == 1
    assert first[0].data[:4] == b"\x10\x0a\x41\x9a"
    assert bus.tick(110) == []
    assert bus.on_frame(CANFrame(0x18DA02F1, b"\x30\x00\x00", True), 111) == []
    assert bus.tick(112) == []
    cf = bus.on_frame(CANFrame(0x18DA01F1, b"\x30\x00\x00", True), 115)
    assert len(cf) == 1
    assert cf[0].data[0] == 0x21
    assert cf[0].can_id == 0x18DAF101


def test_isotp_flow_control_overflow_aborts(bus):
    bus.on_frame(CANFrame(0x18DA01F1, b"\x02\x01\x9a", True), 0)
    bus.tick(100)
    assert bus.on_frame(CANFrame(0x18DA01F1, b"\x32\x00\x00", True), 101) == []
    assert bus.on_frame(CANFrame(0x18DA01F1, b"\x30\x00\x00", True), 102) == []


def test_missing_every_third_ecu02_response(bus):
    frame = CANFrame(0x18DA02F1, b"\x02\x01\x0c", True)
    counts = []
    for n in range(3):
        bus.on_frame(frame, 100 * n)
        counts.append(len(bus.tick(100 * n + 50)))
    assert counts == [1, 1, 0]


@pytest.mark.parametrize("request", [b"\x02\x2e\x20", b"\x02\x27\x01",
                                     b"\x02\x10\x03", b"\x02\x31\x01"])
def test_write_control_services_have_zero_replies(bus, request):
    assert bus.on_frame(CANFrame(0x18DA01F1, request, True), 0) == []
    assert bus.tick(1000) == []


@pytest.mark.parametrize("can_id,extended,data", [
    (0x800, False, b""), (0x20000000, True, b""), (0x7E0, False, bytes(9)),
])
def test_invalid_classic_can_frames_rejected(can_id, extended, data):
    with pytest.raises(ValueError):
        CANFrame(can_id, data, extended)
