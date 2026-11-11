"""Static (no BLE connection) checks of explicit macOS ELM adapter profiles."""
import pytest
from tools.elm327_sim.ble_adapter import GATT, ExplicitBLEELMAdapter


def test_ble_elm_profiles_match_ios_adapter():
    assert GATT["m5dial"]["service"] == "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
    assert GATT["m5dial"]["write"] == "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
    assert GATT["m5dial"]["notify"] == "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
    assert GATT["m5dial"]["response"] is True
    assert GATT["kw905"] == {"service": "FFF0", "write": "FFF2", "notify": "FFF1", "response": False}


@pytest.mark.parametrize("profile,identifier", [
    ("unknown", "test"), ("kw905", ""), ("m5dial", ""), ("kw905", "a" * 129)
])
def test_no_implicit_ble_selection(profile, identifier):
    with pytest.raises(ValueError):
        ExplicitBLEELMAdapter(profile, identifier)
