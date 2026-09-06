from __future__ import annotations

import json
from pathlib import Path

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness.integration.suite_spec import load_suite_spec


INTEGRATION = Path(__file__).resolve().parents[1]


def test_s30_mutant_f_refusal_is_a_checked_in_single_cell() -> None:
    suite = load_suite_spec(INTEGRATION / "suites" / "s30-mutant-f.json")

    assert suite.expanded_scenario_ids() == (("S30-mutant-f-refusal", 1),)
    scenario = json.loads(
        (INTEGRATION / "scenarios" / "S30-mutant-f-refusal.json").read_text(
            encoding="utf-8"
        )
    )
    assert scenario["shape"] == "S'C'F'"
    assert scenario["images"]["new"] == "p50s4-0c820e79"
    assert scenario["images"]["mutant"] == "p50s30-f-refusal-0c820e79"
    assert scenario["expect"]["exact"] == "all"
    assert scenario["expect"]["error106_max"] == 100
    assert scenario["expect"]["reuse"] == "none-when-legacy"
    assert scenario["expect"]["tail_to_incapable"] == 0


def test_s30_mutant_f_resolves_the_hash_bound_unpromoted_candidate() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    path = INTEGRATION / "scenarios" / "S30-mutant-f-refusal.json"
    scenario = load_scenario_spec(path, farm)
    plan = farmtest.build_plan(farm, scenario, run_id="s30-mutant-dry-plan")
    worker = next(
        item for item in plan["topology"]["instances"] if item["role"] == "F"
    )

    assert worker["image"]["label"] == "p50s30-f-refusal-0c820e79"
    assert worker["image"]["kind"] == "daemon-mutant"
    assert "closure_sha256" not in worker["image"]
