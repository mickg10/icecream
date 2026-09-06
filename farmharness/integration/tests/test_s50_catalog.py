from __future__ import annotations

from pathlib import Path

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness.integration.suite_spec import load_suite_spec


INTEGRATION = Path(__file__).resolve().parents[1]


def test_s50_catalogue_has_exact_worker_fractions_and_safe_placements() -> None:
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    expected = {
        "S50-mixed-pool-1of9": 1,
        "S50-mixed-pool-5of9": 5,
        "S50-mixed-pool-8of9": 8,
    }
    for scenario_id, new_count in expected.items():
        scenario = load_scenario_spec(
            INTEGRATION / "scenarios" / f"{scenario_id}.json", farm
        )
        plan = farmtest.build_plan(farm, scenario, run_id=f"{scenario_id}-dry")
        workers = [item for item in scenario.data["instances"] if item["role"] == "F"]
        clients = [item for item in scenario.data["instances"] if item["role"] == "C"]
        assert len(workers) == 9
        assert sum(item["image"] == "new" for item in workers) == new_count
        assert sum(item["slots"] for item in workers) == 36
        assert {item["host"] for item in workers} == {
            "tt-quietbox2",
            "tt-quietbox3",
            "research6",
        }
        assert {item["name"] for item in clients} == {"C1", "C2"}
        assert {item["image"] for item in clients} == {"old", "new"}
        assert all(
            item["role"] in farm.hosts[item["host"]]["roles_allowed"]
            for item in scenario.data["instances"]
        )
        assert plan["topology"]["topology"] == "C2F9"
        assert plan["topology"]["topology_slots"] == {
            "f_relationships": 9,
            "slots_per_f": 4,
        }
        relationships = plan["topology"]["relationships"]
        assert len(relationships) == 18
        assert {
            (item["c"], item["f"], item["cache_expected"])
            for item in relationships
        } == {
            ("C1", worker["name"], False)
            for worker in workers
        } | {
            ("C2", worker["name"], worker["image"] == "new")
            for worker in workers
        }
        assert scenario.data["expect"]["engagement"] == "expected(c,f)"
        assert scenario.data["expect"]["exact"] == "all"
        assert scenario.data["expect"]["wedges"] == 0


def test_s50_suite_is_fixed_cheap_to_expensive_and_disjoint_from_s80() -> None:
    suite = load_suite_spec(INTEGRATION / "suites" / "s50.json")
    assert suite.expanded_scenario_ids() == (
        ("S50-all-legacy-control", 1),
        ("S50-mixed-pool-1of9", 1),
        ("S50-mixed-pool-5of9", 1),
        ("S50-mixed-pool-8of9", 1),
        ("S50-all-f-new-mixed-c", 1),
    )
    assert suite.data["kind"] == "s50-fairness"
    assert suite.data["fairness"] == {
        "control": "S50-all-legacy-control",
        "mixed": [
            "S50-mixed-pool-1of9",
            "S50-mixed-pool-5of9",
            "S50-mixed-pool-8of9",
        ],
        "ratio_limit": 1.05,
    }
    s80 = load_suite_spec(INTEGRATION / "suites" / "twobuild.json")
    assert set(suite.data["scenarios"]).isdisjoint(s80.data["scenarios"])


def test_s50_all_new_workers_mixed_clients_is_explicit_and_exact() -> None:
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S50-all-f-new-mixed-c.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s50-all-f-new-dry")
    workers = [item for item in scenario.data["instances"] if item["role"] == "F"]
    clients = [item for item in scenario.data["instances"] if item["role"] == "C"]
    assert len(workers) == 9
    assert all(item["image"] == "new" for item in workers)
    assert sum(item["slots"] for item in workers) == 36
    assert {item["image"] for item in clients} == {"old", "new"}
    assert scenario.data["workload"]["jobs"] == 100
    assert scenario.data["workload"]["clients"] == ["C1", "C2"]
    assert plan["topology"]["topology"] == "C2F9"
    assert scenario.data["expect"]["tail_to_incapable"] == 0
