"""Strict host-only ELM327 behavior regression assertions.

Specification: ELM327DSK https://www.elmelectronics.com/wp-content/uploads/2017/01/ELM327DS.pdf
Printed pp.8-9 (echo, CR, prompt), pp.10-29 (AT E/L/D/RV/PC/BRD/CRA/SH),
pp.37-38 (SP0), pp.40-48 (headers/filter/order/multiframe),
pp.51-53 (baud handshake and ST/AT), pp.88-90 (error messages).
This file tests *synthetic* timing and faults, not real adapter certification.
"""
from dataclasses import replace
from pathlib import Path

import pytest
from tools.elm327_sim import ELM327Session, load_model

MODEL = Path(__file__).parent / "fixtures" / "synthetic_ecus.json"


@pytest.fixture
def host():
    return ELM327Session(load_model(MODEL))


def setup(host, proto=7, header=True, space=True):
    host.request("ATE0")
    assert host.request("ATL0") == b"OK\r>"
    assert host.request("ATS1" if space else "ATS0") == b"OK\r>"
    assert host.request("ATH1" if header else "ATH0") == b"OK\r>"
    assert host.request("ATSP" + str(proto)) == b"OK\r>"


def test_e1_restores_echo_without_reset(host):
    # ELM327DSK pp.8-10: new E1 setting affects subsequent command echoes.
    assert host.request("ATE0") == b"ATE0\rOK\r>"
    assert host.request("ATE1") == b"OK\r>"
    assert host.request("ATI") == b"ATI\rELM327 SIMULATOR v0.1\r>"


def test_l0_removes_lf_byte_exclusively(host):
    # ELM327DSK pp.8-10: L1 CR LF; L0 CR only.
    host.request("ATE0")
    assert host.request("ATL1") == b"OK\r\n>"
    assert host.request("ATI") == b"ELM327 SIMULATOR v0.1\r\n>"
    assert host.request("ATL0") == b"OK\r>"
    assert host.request("ATI") == b"ELM327 SIMULATOR v0.1\r>"


def test_d_restores_elm_defaults_and_clears_filter(host):
    # ELM327DSK pp.10, 13-14: defaults, not arbitrary retained customizations.
    setup(host)
    host.request("ATCRA18DAF102")
    assert host.request("ATD") == b"OK\r>"
    assert host.request("ATI") == b"ATI\rELM327 SIMULATOR v0.1\r>"
    assert host.request("ATDPN") == b"ATDPN\r0\r>"
    assert host.rx_filter is None
    assert host.spaces and not host.headers and host.echo


def test_rv_reports_explicit_synthetic_voltage(host):
    # ELM327DSK p.30: input voltage; no claim of a real vehicle reading.
    setup(host)
    assert host.request("ATRV") == b"12.6V\r>"


@pytest.mark.parametrize("value", ["7EA", "7EX", "18DAF102", "18DAF10X"])
def test_cra_accepts_valid_filter_and_reset(host, value):
    # ELM327DSK pp.47-48: nibble-level receive address and reset.
    setup(host)
    assert host.request("ATCRA" + value) == b"OK\r>"
    assert host.rx_filter == value
    assert host.request("ATCRA") == b"OK\r>"
    assert host.rx_filter is None


@pytest.mark.parametrize("value", ["1234", "GGG", "000000000", "7EQ"])
def test_cra_rejects_bad_filter_form(host, value):
    setup(host)
    assert host.request("ATCRA" + value) == b"?\r>"


def test_cra_filters_29bit_ecu_responses_not_transmitted_requests(host):
    setup(host)
    assert host.request("ATCRA18DAF102") == b"OK\r>"
    assert host.request("010C") == b"18DAF102 41 0C 01 A8\r>"
    assert host.request("ATCRA") == b"OK\r>"
    assert host.request("010C") == (
        b"18DAF101 41 0C 1A F8\r"
        b"18DAF102 41 0C 01 A8\r"
        b"18DAF10E 41 0C 02 C4\r"
        b"18DAF1EF 41 0C 01 A8\r>"
    )


def test_cra_filters_11bit_ecu_responses(host):
    setup(host, proto=6)
    assert host.request("ATCRA7EA") == b"OK\r>"
    assert host.request("010C") == b"7EA 41 0C 01 A8\r>"
    assert host.request("ATCRA7EX") == b"OK\r>"
    assert host.request("010C") == b"7E8 41 0C 1A F8\r7EA 41 0C 01 A8\r>"


@pytest.mark.parametrize("header,expected", [
    ("7E0", b"7E8 41 0C 1A F8\r>"),
    ("7E2", b"7EA 41 0C 01 A8\r>"),
    ("7E1", b"NO DATA\r>"),
])
def test_sh_changes_actual_11bit_can_destination(host, header, expected):
    # ELM327DSK pp.40-42: SH must change the routed request, not merely OK.
    setup(host, proto=6)
    assert host.request("ATSH" + header) == b"OK\r>"
    assert host.request("010C") == expected


@pytest.mark.parametrize("header,expected", [
    ("18DA01F1", b"18DAF101 41 0C 1A F8\r>"),
    ("18DA02F1", b"18DAF102 41 0C 01 A8\r>"),
    ("18DA03F1", b"NO DATA\r>"),
])
def test_sh_changes_actual_29bit_can_destination(host, header, expected):
    setup(host)
    assert host.request("ATSH" + header) == b"OK\r>"
    assert host.request("010C") == expected


def test_pc_closes_protocol_but_preserves_saved_selection(host):
    # ELM327DSK pp.24-26: PC closes connection, unlike SP0 selection.
    setup(host)
    assert host.request("ATDPN") == b"7\r>"
    assert host.request("ATPC") == b"OK\r>"
    assert host.request("ATDPN") == b"7\r>"
    assert host.request("010D") == b"18DAF101 41 0D 28\r>"


def test_sp0_defers_virtual_can_search_until_obd_request(host):
    # ELM327DSK pp.37-38: 0 searches; mock only has CAN 6/7.
    setup(host, proto=0)
    assert host.request("ATDPN") == b"0\r>"
    assert host.request("ATDP") == b"AUTO\r>"
    assert host.request("010D") == b"18DAF101 41 0D 28\r>"
    assert host.request("ATDPN") == b"A7\r>"
    assert host.request("ATDP") == b"AUTO, ISO 15765-4 (CAN 29/500)\r>"
    assert host.request("ATPC") == b"OK\r>"
    assert host.request("ATDPN") == b"0\r>"


@pytest.mark.parametrize("st,expected", [
    ("01", b"NO DATA\r>"),  # 4ms < first ECU 8ms
    ("02", b"18DAF101 41 0C 1A F8\r>"),  # 8ms inclusive
    ("04", b"18DAF101 41 0C 1A F8\r18DAF102 41 0C 01 A8\r>"),
])
def test_st_changes_real_virtual_deadline(host, st, expected):
    # ELM327DSK p.53: timeout = hh x 4ms, compare exact returned ECUs.
    setup(host)
    assert host.request("ATAT0") == b"OK\r>"
    assert host.request("ATST" + st) == b"OK\r>"
    assert host.request("010C") == expected


def test_at0_versus_adaptive_1_changes_virtual_collection_deadline(host):
    # ATAT algorithm is not public; assert a documented deterministic mock
    # adjustment, not ELM's proprietary numerical tuning (DSK p.53).
    setup(host)
    assert host.request("ATST32") == b"OK\r>"
    assert host.request("ATCRA18DAF101") == b"OK\r>"
    assert host.request("010C") == b"18DAF101 41 0C 1A F8\r>"
    assert host.request("ATCRA") == b"OK\r>"
    assert host.request("ATAT1") == b"OK\r>"
    assert host.request("010C") == (
        b"18DAF101 41 0C 1A F8\r18DAF102 41 0C 01 A8\r>"
    )
    assert host.request("ATAT0") == b"OK\r>"
    # Isolate timing behavior from ECU02's intentional every-third-response drop.
    host.bus.attempts.clear()
    assert host.request("010C") == (
        b"18DAF101 41 0C 1A F8\r"
        b"18DAF102 41 0C 01 A8\r"
        b"18DAF10E 41 0C 02 C4\r"
        b"18DAF1EF 41 0C 01 A8\r>"
    )


@pytest.mark.parametrize("fault", ["CAN ERROR", "BUFFER FULL"])
def test_injected_error_exact_wire_then_recovers(host, fault):
    # ELM327DSK pp.88-90: distinguish bus initialization from UART overflow.
    setup(host)
    host.inject_fault_once(fault)
    assert host.request("010D") == fault.encode("ascii") + b"\r>"
    assert host.request("010D") == b"18DAF101 41 0D 28\r>"


def test_buffer_full_on_synthetic_uart_capacity_then_recovers(host):
    # ELM327DSK pp.88-90; synthetic capacity, not genuine chip buffer size.
    setup(host)
    host.max_response_bytes = 25
    assert host.request("010C") == b"BUFFER FULL\r>"
    assert host.request("010D") == b"18DAF101 41 0D 28\r>"


def test_strict_11bit_output_sequence_without_extra_bytes(host):
    setup(host, proto=6)
    assert host.request("010C") == (
        b"7E8 41 0C 1A F8\r"
        b"7EA 41 0C 01 A8\r>"
    )


def test_strict_29bit_no_spaces_no_headers(host):
    setup(host, proto=7, header=False, space=False)
    assert host.request("010D") == b"410D28\r>"


def test_strict_raw_isotp_frame_order_with_headers(host):
    # ELM327DSK pp.43-46, CAF0 + H1 retains PCI and CAN address.
    setup(host, proto=7)
    assert host.request("ATCAF0") == b"OK\r>"
    assert host.request("019A") == (
        b"18DAF101 10 0A 41 9A 07 00 3D BD\r"
        b"18DAF101 21 FF 70 AA BB\r>"
    )


def test_strict_caf1_multiframe_output_without_headers(host):
    setup(host, proto=7, header=False)
    assert host.request("019A") == (
        b"00A\r0: 41 9A 07 00 3D BD\r1: FF 70 AA BB\r>"
    )


def test_model_delay_determines_frame_order_not_can_id_sort(host):
    # ECU02 responds 1ms, ECU01 responds 8ms; order must follow arrivals.
    model = load_model(MODEL)
    second = replace(model.ecus[1], responses=tuple(
        replace(r, delay_ms=1) for r in model.ecus[1].responses
    ))
    altered = replace(model, ecus=(model.ecus[0], second, *model.ecus[2:]))
    device = ELM327Session(altered)
    setup(device, proto=7)
    raw = device.request("010C")
    assert raw.startswith(b"18DAF102 41 0C 01 A8\r18DAF101 41 0C 1A F8\r")


def test_brd_virtual_handshake_requires_confirmation(host):
    # ELM327DSK pp.51-52: BRD OK then test at candidate rate, host CR.
    assert host.request("ATBRD45") == b"ATBRD45\rOK\r"
    assert host.baud_divisor is None
    assert host.baud_pending == 0x45
    assert host.baud_test_identity() == b"ELM327 SIMULATOR v0.1\r"
    host.advance_virtual_time(25)
    assert host.request("") == b"\r>"
    assert host.baud_divisor == 0x45
    assert host.baud_pending is None


@pytest.mark.parametrize("bad", ["ATBRD00", "ATBRDGG", "ATBRD", "ATBRD100"])
def test_brd_invalid_divisor_does_not_change_rate(host, bad):
    assert host.request(bad) == bad.encode() + b"\r?\r>"
    assert host.baud_divisor is None
    assert host.baud_pending is None


def test_brd_timeout_rolls_back_without_uart_open(host):
    # DSK pp.51-52; BRT default 0F*5=75ms, reset rate on timeout.
    assert host.request("ATBRD45") == b"ATBRD45\rOK\r"
    host.advance_virtual_time(76)
    assert host.baud_pending is None
    assert host.baud_divisor is None


def test_brt_changes_virtual_brd_timeout(host):
    assert host.request("ATBRT02") == b"ATBRT02\rOK\r>"
    assert host.baud_timeout_ms == 10
    assert host.request("ATBRD45") == b"ATBRD45\rOK\r"
    host.advance_virtual_time(11)
    assert host.baud_pending is None
