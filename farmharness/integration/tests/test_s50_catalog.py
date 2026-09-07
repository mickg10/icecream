from __future__ import annotations

from pathlib import Path

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness.integration.suite_spec import load_suite_spec


INTEGRATION = Path(__file__).resolve().parents[1]
S50_WORKER_HOSTS = {
    "F1": "tt-quietbox2",
    "F2": "tt-quietbox2",
    "F3": "tt-quietbox2",
    "F4": "tt-quietbox3",
    "F5": "tt-quietbox2",
    "F6": "tt-quietbox3",
    "F7": "tt-quietbox3",
    "F8": "tt-quietbox3",
    "F9": "tt-quietbox3",
}


def test_s50_catalogue_has_exact_worker_fractions_and_safe_placements() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
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
        # S50's strict 5% old-client fairness comparison uses the two
        # CPU-homogeneous quietboxes.  research6 remains in Farm E and in
        # the compatibility/resilience/performance catalogue, but its
        # exact-TU GCC throughput is about 20% lower and is therefore not a
        # comparable S50 timing worker.
        assert {item["host"] for item in workers} == {
            "tt-quietbox2",
            "tt-quietbox3",
        }
        assert "research6" in farm.hosts
        assert {item["name"]: item["host"] for item in workers} == S50_WORKER_HOSTS
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
        assert scenario.data["workload"]["jobs"] == 18


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
    farm = load_farm_spec(farm_fixture.example_farm_path())
    for scenario_id in (
        suite.data["fairness"]["control"],
        *suite.data["fairness"]["mixed"],
    ):
        scenario = load_scenario_spec(
            INTEGRATION / "scenarios" / f"{scenario_id}.json", farm
        )
        assert scenario.data["workload"]["jobs"] == 18
        workers = [
            item for item in scenario.data["instances"] if item["role"] == "F"
        ]
        assert {item["name"]: item["host"] for item in workers} == S50_WORKER_HOSTS
    s80 = load_suite_spec(INTEGRATION / "suites" / "twobuild.json")
    assert set(suite.data["scenarios"]).isdisjoint(s80.data["scenarios"])


def test_s50_all_new_workers_mixed_clients_is_explicit_and_exact() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S50-all-f-new-mixed-c.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s50-all-f-new-dry")
    workers = [item for item in scenario.data["instances"] if item["role"] == "F"]
    clients = [item for item in scenario.data["instances"] if item["role"] == "C"]
    assert len(workers) == 9
    assert all(item["image"] == "new" for item in workers)
    assert sum(item["slots"] for item in workers) == 36
    assert {item["name"]: item["host"] for item in workers} == S50_WORKER_HOSTS
    assert {item["image"] for item in clients} == {"old", "new"}
    assert scenario.data["workload"]["jobs"] == 100
    assert scenario.data["workload"]["clients"] == ["C1", "C2"]
    assert plan["topology"]["topology"] == "C2F9"
    assert scenario.data["expect"]["tail_to_incapable"] == 0
