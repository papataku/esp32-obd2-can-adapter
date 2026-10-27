"""One set of tests, three explicitly selected ELM target adapters."""
import os
from pathlib import Path
import pytest
from tools.elm327_sim import ELM327Session, load_model

FIXTURE = Path(__file__).parent / "fixtures" / "synthetic_ecus.json"


class InProcessAdapter:
    def __init__(self):
        self.device = ELM327Session(load_model(FIXTURE))

    def exchange(self, command):
        return self.device.request(command)


class SerialELMAdapter:
    """Only explicit, isolated non-vehicle bench; never automatic discovery."""
    def __init__(self, target, port, ack):
        if not ack or os.environ.get("TTM_ISOLATED_BENCH") != "YES":
            raise RuntimeError("requires --isolated-bench-ack and TTM_ISOLATED_BENCH=YES")
        if not port:
            raise RuntimeError("explicit serial port required")
        import serial
        self.handle = serial.Serial(port, baudrate=38400, timeout=0.25, write_timeout=0.5)
        self.target = target

    def exchange(self, command):
        self.handle.reset_input_buffer()
        self.handle.write(command.encode("ascii") + b"\r")
        response = bytearray()
        import time
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            part = self.handle.read(256)
            response.extend(part)
            if b">" in response:
                return bytes(response)
            if len(response) > 8192:
                raise AssertionError("ELM response exceeded 8192 byte cap")
        raise AssertionError("ELM did not terminate with >")

    def close(self):
        self.handle.close()


def pytest_addoption(parser):
    parser.addoption("--elm-target", action="store", default="sim", choices=("sim", "m5dial", "kw905"))
    parser.addoption("--elm-port", action="store", default="")
    parser.addoption("--isolated-bench-ack", action="store_true", default=False)
    parser.addoption("--bus-bench-ack", action="store_true", default=False)


@pytest.fixture
def elm(request):
    target = request.config.getoption("--elm-target")
    if target == "sim":
        return InProcessAdapter()
    port = request.config.getoption("--elm-port")
    if not port:
        pytest.skip(f"{target}: NOT_RUN; explicit --elm-port is mandatory")
    adapter = SerialELMAdapter(target, port, request.config.getoption("--isolated-bench-ack"))
    request.addfinalizer(adapter.close)
    return adapter


@pytest.fixture
def elm_can(request, elm):
    target = request.config.getoption("--elm-target")
    if target != "sim" and not request.config.getoption("--bus-bench-ack"):
        pytest.skip("CAN request NOT_RUN without independently isolated ECU bench")
    return elm
