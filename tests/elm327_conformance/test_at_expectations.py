"""Check all official 10-12 summary AT command forms have expectation metadata.

This validates a specification inventory, not execution of deferred commands.
"""
import json
from pathlib import Path
import pytest

REGISTRY = Path(__file__).parent / "elm327_at_expectations.json"
DATA = json.loads(REGISTRY.read_text(encoding="utf-8"))
CASES = DATA["commands"]


def test_catalog_has_version_source_and_full_groups():
    assert DATA["schema"] == "ttm.elm327-at-expectations.v1"
    assert DATA["printed_pages"] == [10, 11, 12]
    assert len(CASES) >= 120
    assert {c["category"] for c in CASES} == {
        "general", "pp", "voltage", "obd", "j1850", "iso", "can", "j1939"
    }
    assert len({c["command"] for c in CASES}) == len(CASES)


@pytest.mark.parametrize("entry", CASES, ids=lambda c: c["id"])
def test_each_at_command_has_datasheet_page_expected_result_and_safety(entry):
    assert entry["source_page"] in (10, 11, 12)
    assert entry["command"].startswith("AT") or entry["command"] == "<CR>"
    assert entry["expected"] and len(entry["expected"]) >= 16
    assert entry["expect_kind"]
    assert entry["execution"] in ("sim_or_isolated_bench", "manual_isolated_or_sim_only")
    assert entry["implementation_status"] == "catalogued_not_automatically_certified"


def test_high_risk_commands_not_automatically_executed():
    for entry in CASES:
        if entry["expect_kind"] in ("MONITOR", "SLEEP", "BAUD_HANDSHAKE", "OBD_OR_ERROR"):
            assert entry["execution"] == "manual_isolated_or_sim_only"
