from __future__ import annotations

from pathlib import Path

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness.integration.suite_spec import S60_SCENARIO_IDS, load_suite_spec


INTEGRATION = Path(__file__).resolve().parents[1]

_EDGES = (
    ("S1", "upgrade", "new", ("old", "old", "old", "old", "old"), ("new", "old", "old", "old", "old")),
    ("F1", "upgrade", "new", ("new", "old", "old", "old", "old"), ("new", "new", "old", "old", "old")),
    ("F2", "upgrade", "new", ("new", "new", "old", "old", "old"), ("new", "new", "new", "old", "old")),
    ("C1", "upgrade", "new", ("new", "new", "new", "old", "old"), ("new", "new", "new", "new", "old")),
    ("C2", "upgrade", "new", ("new", "new", "new", "new", "old"), ("new", "new", "new", "new", "new")),
    ("C2", "downgrade", "old", ("new", "new", "new", "new", "new"), ("new", "new", "new", "new", "old")),
    ("C1", "downgrade", "old", ("new", "new", "new", "new", "old"), ("new", "new", "new", "old", "old")),
    ("F2", "downgrade", "old", ("new", "new", "new", "old", "old"), ("new", "new", "old", "old", "old")),
    ("F1", "downgrade", "old", ("new", "new", "old", "old", "old"), ("new", "old", "old", "old", "old")),
    ("S1", "downgrade", "old", ("new", "old", "old", "old", "old"), ("old", "old", "old", "old", "old")),
    ("F2", "downgrade", "old", ("new", "new", "new", "new", "new"), ("new", "new", "old", "new", "new")),
    ("F2", "upgrade", "new", ("new", "new", "old", "new", "new"), ("new", "new", "new", "new", "new")),
    ("S1", "downgrade", "old", ("new", "new", "new", "new", "new"), ("old", "new", "new", "new", "new")),
    ("S1", "upgrade", "new", ("old", "new", "new", "new", "new"), ("new", "new", "new", "new", "new")),
)


def _state(scenario) -> tuple[str, ...]:
    instances = {item["name"]: item for item in scenario.data["instances"]}
    return tuple(instances[name]["image"] for name in ("S1", "F1", "F2", "C1", "C2"))


def test_s60_suite_is_exactly_ordered_and_fresh() -> None:
    suite = load_suite_spec(INTEGRATION / "suites" / "s60.json")
    assert tuple(suite.data["scenarios"]) == S60_SCENARIO_IDS
    assert suite.expanded_scenario_ids() == tuple((item, 1) for item in S60_SCENARIO_IDS)


def test_s60_edges_have_one_directional_instance_change_and_resolve() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    for index, (scenario_id, edge) in enumerate(zip(S60_SCENARIO_IDS, _EDGES), start=1):
        target, action, image, before, after = edge
        scenario = load_scenario_spec(
            INTEGRATION / "scenarios" / f"{scenario_id}.json", farm
        )
        assert _state(scenario) == before
        assert len(scenario.data["instances"]) == 5
        assert {item["role"] for item in scenario.data["instances"]} == {"S", "F", "C"}
        event = scenario.data["timeline"]
        assert len(event) == 1
        assert event[0] == {
            "trigger": "job 24",
            "action": action,
            "instance": target,
            "image": image,
        }
        projected = list(before)
        projected[("S1", "F1", "F2", "C1", "C2").index(target)] = image
        assert tuple(projected) == after
        assert scenario.data["expect"]["exact"] == "all"
        assert scenario.data["expect"]["engagement"] == "expected(c,f)"
        assert scenario.data["expect"]["wedges"] == 0
        assert scenario.data["expect"]["error106_max"] == 0
        assert scenario.data["expect"]["tail_to_incapable"] == 0
        assert scenario.data["expect"]["reuse"] == "none-when-legacy"
        farm.data["hub"]["results_root"] = "/tmp/i/s60-catalog"
        plan = farmtest.build_plan(farm, scenario, run_id=f"s60-edge-{index}")
        assert plan["topology"]["topology_digest"]
        starts = [
            command
            for command in plan["commands"]
            if command["phase"].startswith("up.start-")
        ]
        assert sum(command["phase"] == "up.start-f" for command in starts) == 2
        for command in starts:
            if command["phase"] == "up.start-f":
                ulimit = command["argv"].index("--ulimit")
                assert command["argv"][ulimit + 1] == "nofile=65536:65536"
            else:
                assert "--ulimit" not in command["argv"]


def test_s60_explicit_all_f_new_mixed_c_and_warm_pairs() -> None:
    all_f_new = load_scenario_spec(
        INTEGRATION / "scenarios" / "S60-04-c1-up.json",
        load_farm_spec(farm_fixture.example_farm_path()),
    )
    mixed_c = load_scenario_spec(
        INTEGRATION / "scenarios" / "S60-05-c2-up.json",
        load_farm_spec(farm_fixture.example_farm_path()),
    )
    assert all(item["image"] == "new" for item in all_f_new.data["instances"] if item["role"] == "F")
    assert {item["image"] for item in mixed_c.data["instances"] if item["role"] == "C"} == {"old", "new"}
    assert all_f_new.data["shape"] == "mixed"
    assert mixed_c.data["shape"] == "mixed"
    assert all(
        load_scenario_spec(
            INTEGRATION / "scenarios" / scenario_id,
            load_farm_spec(farm_fixture.example_farm_path()),
        ).data["workload"]["jobs"] >= 24
        for scenario_id in ("S60-06-c2-down.json", "S60-11-warm-f2-down.json", "S60-13-warm-s-down.json")
    )
    for scenario_id in ("S60-12-warm-f2-up.json", "S60-14-warm-s-up.json"):
        scenario = load_scenario_spec(
            INTEGRATION / "scenarios" / scenario_id,
            load_farm_spec(farm_fixture.example_farm_path()),
        )
        assert scenario.data["expect"]["reuse"] == "none-when-legacy"
        assert "reuse_pairs" not in scenario.data["expect"]
