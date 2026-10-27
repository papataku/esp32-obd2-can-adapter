import copy
import json
from pathlib import Path
import pytest
from tools.elm327_sim.model import load_model, parse_model

PATH = Path(__file__).parent / "fixtures" / "synthetic_ecus.json"


def original():
    return json.loads(PATH.read_text())


def test_fixture_is_synthetic_and_can11_can29():
    model = load_model(PATH)
    assert model.provenance == "synthetic"
    assert len(model.ecus) == 4
    assert len(model.ecus[0].responses) == 5
    assert model.ecus[-1].id == "ECUEF"
    assert model.ecus[-1].responses[0].response_id_11 is None


@pytest.mark.parametrize("key", ["VIN", "latitude", "longitude", "gps", "timestamp",
                                 "datetime", "ble_id", "absolute_time", "device_id"])
def test_forbid_any_new_top_level_personal_or_absolute_field(key):
    obj = original()
    obj[key] = "forbidden"
    with pytest.raises(ValueError):
        parse_model(obj)


@pytest.mark.parametrize("change", [
    {"schema": "ttm.ecu-model.v0"},
    {"provenance": "raw_vehicle_capture"},
    {"ecus": []},
])
def test_invalid_metadata_fails_closed(change):
    obj = original()
    obj.update(change)
    with pytest.raises(ValueError):
        parse_model(obj)


@pytest.mark.parametrize("field,value", [
    ("request", "2E2012"), ("request", "010"), ("request", "010c"),
    ("payload_hex", "410D28"), ("payload_hex", ""),
    ("response_id_11", "7FF"), ("response_id_29", "18DAF102"),
    ("delay_ms", -1), ("delay_ms", 1.1), ("drop_every", 0),
    ("relative_timestamps_ms", [20, 10]), ("relative_timestamps_ms", [-1]),
])
def test_invalid_per_ecu_response_fails_closed(field, value):
    obj = original()
    obj["ecus"][0]["responses"][0][field] = value
    with pytest.raises(ValueError):
        parse_model(obj)


def test_duplicate_ecu_denied():
    obj = original()
    obj["ecus"].append(copy.deepcopy(obj["ecus"][0]))
    with pytest.raises(ValueError):
        parse_model(obj)
