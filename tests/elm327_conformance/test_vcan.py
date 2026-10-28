from pathlib import Path
import pytest
from tools.elm327_sim import load_model, ECUSimulator
from tools.elm327_sim.vcan import (
    CAN_TX, CAN_RX, HELLO, STOP, HELLO_TOKEN,
    VirtualFrame, VirtualCDCBridge, cobs_encode, cobs_decode,
    encode_packet, decode_packet
)

PATH = Path(__file__).parent / "fixtures" / "synthetic_ecus.json"


@pytest.mark.parametrize("data", [b"", b"hello", b"\x00", b"\x00\x00",
                                  bytes(range(256)), b"abc\x00def\x00ghi"])
def test_cobs_roundtrip(data):
    assert cobs_decode(cobs_encode(data)) == data


@pytest.mark.parametrize("extended,can_id", [(False, 0x7DF), (True, 0x18DB33F1)])
def test_vcan_packet_roundtrip(extended, can_id):
    f = VirtualFrame(CAN_TX, 4, 123456, can_id, b"\x02\x01\x0c", extended)
    assert decode_packet(encode_packet(f)) == f


def test_crc_rejects_mutation():
    f = encode_packet(VirtualFrame(CAN_TX, 4, 123, 0x7DF, b"\x02\x01\x0c"))
    raw = bytearray(f)
    raw[7] ^= 0x20
    with pytest.raises(ValueError):
        decode_packet(bytes(raw))


@pytest.mark.parametrize("bad", [
    b"", b"\x00", b"\x01", b"\x03\x00",
    b"\x01\x00\x00", bytes(100) + b"\x00"
])
def test_invalid_serial_frames_fail_closed(bad):
    with pytest.raises(ValueError):
        decode_packet(bad)


def test_virtual_handshake_and_can_tx_deliver_rx():
    bridge = VirtualCDCBridge(ECUSimulator(load_model(PATH)))
    before = encode_packet(VirtualFrame(CAN_TX, 1, 0, 0x18DB33F1, b"\x02\x01\x0c", True))
    with pytest.raises(ValueError, match="handshake"):
        bridge.handle(before)
    out = bridge.handle(encode_packet(VirtualFrame(HELLO, 10, 0, data=HELLO_TOKEN)))
    assert len(out) == 1 and decode_packet(out[0]).kind == HELLO
    query = encode_packet(VirtualFrame(CAN_TX, 11, 0, 0x18DB33F1, b"\x02\x01\x0c", True))
    assert bridge.handle(query) == []
    replies = bridge.tick(200_000)
    assert len(replies) == 4
    assert all(decode_packet(raw).kind == CAN_RX for raw in replies)
    assert {decode_packet(raw).can_id for raw in replies} == {
        0x18DAF101, 0x18DAF102, 0x18DAF10E, 0x18DAF1EF
    }


def test_sequence_mismatch_disarms_virtual_cdc():
    bridge = VirtualCDCBridge(ECUSimulator(load_model(PATH)))
    bridge.handle(encode_packet(VirtualFrame(HELLO, 10, 0, data=HELLO_TOKEN)))
    with pytest.raises(ValueError, match="sequence"):
        bridge.handle(encode_packet(VirtualFrame(CAN_TX, 12, 0, 0x7DF, b"\x02\x01\x0c")))
    assert bridge.armed is False


def test_stop_disarms_without_emitting_can():
    bridge = VirtualCDCBridge(ECUSimulator(load_model(PATH)))
    bridge.handle(encode_packet(VirtualFrame(HELLO, 10, 0, data=HELLO_TOKEN)))
    assert bridge.handle(encode_packet(VirtualFrame(STOP, 11, 0))) == []
    assert not bridge.armed


def test_hello_token_is_exact():
    with pytest.raises(ValueError):
        VirtualFrame(HELLO, 1, 0, data=b"CARLIVE!")


def test_no_physical_bus_transports_imported():
    from tools.elm327_sim import vcan, can
    for source in (vcan, can):
        text = Path(source.__file__).read_text()
        assert "import can" not in text
        assert "socketcan" not in text.lower()
        assert "twai_transmit(" not in text
