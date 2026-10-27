"""Data-sheet-grounded ELM basics plus CAN 11/29, same tests on all targets.

For non-synthetic targets only explicitly acknowledged, isolated bench may run
CAN queries. NOT_RUN is never relabeled PASS by CI.
"""
import pytest


def _lines(raw):
    assert raw.endswith(b">")
    return raw.replace(b"\n", b"").decode("ascii", errors="strict")


def _setup(elm, protocol=7, headers=True, spaces=True):
    _lines(elm.exchange("ATZ"))
    for cmd in ("ATE0", "ATL0", "ATS1" if spaces else "ATS0",
                "ATH1" if headers else "ATH0", f"ATSP{protocol}"):
        assert "OK" in _lines(elm.exchange(cmd))


@pytest.mark.parametrize("command", [
    "ATE0", "ATE1", "ATL0", "ATL1", "ATS0", "ATS1",
    "ATH0", "ATH1", "ATCAF0", "ATCAF1", "ATCFC0", "ATCFC1",
    "ATAT0", "ATAT1", "ATAT2", "ATAL", "ATNL",
    "ATSP0", "ATSP6", "ATSP7", "ATSP8", "ATSP9",
    "ATST0F", "ATST32", "ATCP18", "ATSHDB33F1",
])
def test_supported_at_command_returns_prompt(elm, command):
    response = _lines(elm.exchange(command))
    assert response.endswith(">")
    assert "OK" in response


@pytest.mark.parametrize("command", ["ATSP5", "ATSPX", "ATAT4", "ATSTGG",
                                     "ATCPGG", "ATSHZZZZZZ", "ATNOTREAL"])
def test_unknown_or_malformed_command_returns_question(elm, command):
    _lines(elm.exchange("ATE0"))
    response = _lines(elm.exchange(command))
    assert "?" in response


@pytest.mark.parametrize("cmd", ["ATI", "AT@1", "ATDP", "ATDPN"])
def test_identification_and_protocol_metadata(elm, cmd):
    _lines(elm.exchange("ATE0"))
    response = _lines(elm.exchange(cmd))
    assert response.strip(" \r>") not in ("", cmd)


@pytest.mark.parametrize("proto,expected", [(6, "6"), (7, "7"), (8, "8"), (9, "9")])
def test_can_protocol_selection(elm, proto, expected):
    _setup(elm, protocol=proto)
    response = _lines(elm.exchange("ATDPN"))
    assert expected in response
    response = _lines(elm.exchange("ATDP"))
    assert "CAN" in response


def test_reset_restores_echo(elm):
    _lines(elm.exchange("ATE0"))
    assert not _lines(elm.exchange("ATI")).startswith("ATI")
    assert "ELM327" in _lines(elm.exchange("ATZ"))
    assert _lines(elm.exchange("ATI")).startswith("ATI")


def test_echo_and_lf_bytes(elm):
    _lines(elm.exchange("ATZ"))
    raw = elm.exchange("ATL1")
    assert b"\r" in raw and b">" in raw
    raw = elm.exchange("ATI")
    assert b"\r\n" in raw
    _lines(elm.exchange("ATE0"))
    assert not elm.exchange("ATI").startswith(b"ATI")


@pytest.mark.parametrize("protocol,ids", [
    (6, (b"7E8", b"7EA")),
    (7, (b"18DAF101", b"18DAF102", b"18DAF10E", b"18DAF1EF")),
    (8, (b"7E8", b"7EA")),
    (9, (b"18DAF101", b"18DAF102", b"18DAF10E", b"18DAF1EF")),
])
def test_canned_multi_ecu_rpm(elm_can, protocol, ids):
    _setup(elm_can, protocol=protocol)
    raw = elm_can.exchange("010C")
    assert b"41 0C" in raw
    for source in ids:
        assert source in raw


@pytest.mark.parametrize("protocol,can_id", [(6, b"7E8"), (7, b"18DAF101")])
def test_iso_tp_long_response_from_matched_ecu(elm_can, protocol, can_id):
    _setup(elm_can, protocol=protocol, headers=True)
    assert "OK" in _lines(elm_can.exchange("ATCAF0"))
    raw = elm_can.exchange("019A")
    assert can_id in raw
    assert b"10 0A 41 9A" in raw
    assert b"21" in raw


def test_no_matching_request_is_no_data(elm_can):
    _setup(elm_can)
    assert "NO DATA" in _lines(elm_can.exchange("012F"))


def test_spaces_and_headers_are_independent(elm_can):
    _setup(elm_can, protocol=7, headers=False, spaces=False)
    raw = elm_can.exchange("010D")
    assert b"410D28" in raw
    assert b"18DAF101" not in raw


def test_monitor_stop_command_boundary(elm):
    _lines(elm.exchange("ATE0"))
    response = elm.exchange("ATMA")
    assert response.endswith(b">")
    assert "STOPPED" in _lines(elm.exchange("ATI"))
