from __future__ import annotations

from pathlib import Path

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest
from farmharness.integration.events import EventProducer
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.images import RecordingTransport
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness.integration.suite_spec import (
    CONTROL_SCENARIO_IDS,
    S70_SCENARIO_IDS,
    SMOKE_SCENARIO_IDS,
    load_suite_spec,
)


INTEGRATION = Path(__file__).resolve().parents[1]


def test_checked_in_harness_gate_suites_are_exact_and_complete() -> None:
    controls = load_suite_spec(INTEGRATION / "suites" / "controls.json")
    smoke = load_suite_spec(INTEGRATION / "suites" / "smoke.json")

    assert tuple(controls.data["scenarios"]) == CONTROL_SCENARIO_IDS
    assert tuple(smoke.data["scenarios"]) == SMOKE_SCENARIO_IDS


def test_s20_scheduler_first_is_a_real_restart_cell() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S20-scheduler-first.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s20-dry-plan")

    assert scenario.data["shape"] == "S'CF"
    assert scenario.data["workload"]["corpus"] == "firefox-1000"
    assert scenario.data["workload"]["turns"] == ["A", "B"]
    assert scenario.data["timeline"] == [
        {"action": "restart", "instance": "S1", "trigger": "job 100"}
    ]
    versions = {
        instance["name"]: instance["version"]
        for instance in plan["topology"]["instances"]
    }
    assert versions == {"C1": 43, "F1": 43, "S1": 50}

    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(),
        job_reader=lambda: 0,
    )
    assert [(event.action, event.instance) for event in producer.events] == [
        ("restart", "S1")
    ]


def test_s20_suite_runs_fmt_then_three_fresh_firefox_restart_pairs() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    smoke = load_scenario_spec(
        INTEGRATION / "scenarios" / "S20-scheduler-first-smoke.json", farm
    )
    assert smoke.data["shape"] == "S'CF"
    assert smoke.data["workload"]["corpus"] == "fmt-100"
    assert smoke.data["workload"]["turns"] == ["A"]
    assert smoke.data["timeline"] == []

    suite = load_suite_spec(INTEGRATION / "suites" / "s20.json")
    assert suite.expanded_scenario_ids() == (
        ("S20-scheduler-first-smoke", 1),
        ("S20-scheduler-first", 1),
        ("S20-scheduler-first", 2),
        ("S20-scheduler-first", 3),
    )


def test_s30_old_f_suite_is_legacy_for_new_client_and_runs_three_pairs() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    for scenario_id, corpus, turns in (
        ("S30-old-f-smoke", "fmt-100", ["A"]),
        ("S30-old-f-firefox", "firefox-1000", ["A", "B"]),
    ):
        scenario = load_scenario_spec(
            INTEGRATION / "scenarios" / f"{scenario_id}.json", farm
        )
        plan = farmtest.build_plan(farm, scenario, run_id=f"{scenario_id}-dry")
        versions = {
            item["role"]: item["version"] for item in plan["topology"]["instances"]
        }
        assert scenario.data["shape"] == "S'FC'"
        assert scenario.data["workload"]["corpus"] == corpus
        assert scenario.data["workload"]["turns"] == turns
        assert versions == {"S": 50, "F": 43, "C": 50}
        assert plan["topology"]["relationships"][0]["cache_expected"] is False

    suite = load_suite_spec(INTEGRATION / "suites" / "s30.json")
    assert suite.expanded_scenario_ids() == (
        ("S30-old-f-smoke", 1),
        ("S30-old-f-firefox", 1),
        ("S30-old-f-firefox", 2),
        ("S30-old-f-firefox", 3),
        ("S30-old-f-client-restart", 1),
    )


def test_s30_client_route_owner_restart_is_a_real_midbuild_legacy_cell() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S30-old-f-client-restart.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s30-client-restart-dry")

    assert scenario.data["shape"] == "S'FC'"
    assert scenario.data["workload"]["corpus"] == "firefox-1000"
    assert scenario.data["workload"]["turns"] == ["A"]
    assert scenario.data["timeline"] == [
        {"action": "restart", "instance": "C1", "trigger": "job 100"}
    ]
    assert plan["topology"]["relationships"][0]["cache_expected"] is False

    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(),
        job_reader=lambda: 0,
    )
    assert [(event.action, event.instance) for event in producer.events] == [
        ("restart", "C1")
    ]


def test_s10_firefox_baseline_is_the_two_turn_legacy_pair() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S10-baseline-firefox.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s10-firefox-dry-plan")

    assert scenario.data["shape"] == "SCF"
    assert scenario.data["workload"]["corpus"] == "firefox-1000"
    assert scenario.data["workload"]["turns"] == ["A", "B"]
    assert {item["version"] for item in plan["topology"]["instances"]} == {43}
    assert all(
        relationship["cache_expected"] is False
        for relationship in plan["topology"]["relationships"]
    )


def test_s10_suite_runs_fmt_then_three_fresh_firefox_pairs() -> None:
    suite = load_suite_spec(INTEGRATION / "suites" / "s10.json")

    assert suite.expanded_scenario_ids() == (
        ("S10-baseline-smoke", 1),
        ("S10-baseline-firefox", 1),
        ("S10-baseline-firefox", 2),
        ("S10-baseline-firefox", 3),
    )


def test_s70_b5_is_a_serial_one_shot_interner_failure_cell() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b5-interner-failure.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s70-b5-dry-plan")

    assert scenario.data["shape"] == "S'C'F'"
    assert scenario.data["workload"]["clients"] == ["C1"]
    assert scenario.data["workload"]["jobs"] == 1
    assert scenario.data["expect"]["engagement"] == "s70-b5-interner-downgrade"
    assert scenario.data["expect"]["error106_max"] == 1
    scheduler = next(
        item for item in scenario.data["instances"] if item["role"] == "S"
    )
    assert "ICECC_P50_PROFILE" not in scheduler["env"]
    assert scenario.data["timeline"] == [
        {
            "action": "env_set",
            "env": {
                "ICECC_P50_FAULT_INJECTION": "P29_INTERNER_FAIL_ONCE"
            },
            "instance": "C1",
            "trigger": "job 1",
        }
    ]
    assert {item["version"] for item in plan["topology"]["instances"]} == {50}


def test_s70_b4_scheduler_active_loss_is_the_single_f_job2_cell() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b4-scheduler-active-loss.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s70-b4-active-loss-dry")

    assert scenario.data["id"] == "S70-b4-scheduler-active-loss"
    assert scenario.data["shape"] == "S'C'F'"

    instances = {item["name"]: item for item in scenario.data["instances"]}
    assert set(instances) == {"S1", "F1", "C1"}
    assert sum(item["role"] == "F" for item in instances.values()) == 1
    assert instances["F1"]["role"] == "F"
    assert instances["F1"]["slots"] == 1

    assert scenario.data["workload"]["corpus"] == "fmt-100"
    assert scenario.data["workload"]["jobs"] == 6
    assert scenario.data["workload"]["clients"] == ["C1"]

    assert scenario.data["timeline"] == [
        {
            "action": "scheduler-loss-active",
            "instance": "S1",
            "trigger": "job 2",
        }
    ]
    assert scenario.data["expect"]["engagement"] == (
        "s70-b4-scheduler-active-loss"
    )
    assert {item["version"] for item in plan["topology"]["instances"]} == {50}

    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(),
        job_reader=lambda: 0,
    )
    assert [
        (event.action, event.instance, event.trigger.kind, event.trigger.value)
        for event in producer.events
    ] == [("scheduler-loss-active", "S1", "job", 2)]


def test_s70_b4_client_restart_is_serial_and_warm_before_the_event() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b4-client-route-restart.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s70-b4-c-dry-plan")

    assert scenario.data["shape"] == "S'C'F'"
    assert scenario.data["workload"]["corpus"] == "fmt-100"
    assert scenario.data["workload"]["jobs"] == 1
    assert scenario.data["expect"]["engagement"] == (
        "s70-b4-client-route-restart"
    )
    assert scenario.data["timeline"] == [
        {"action": "restart", "instance": "C1", "trigger": "job 75"}
    ]
    assert {item["version"] for item in plan["topology"]["instances"]} == {50}


def test_s70_b4_worker_bounce_runs_three_30_second_flaps() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b4-worker-bounces.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s70-b4-f-dry-plan")

    assert scenario.data["shape"] == "S'C'F'"
    assert scenario.data["workload"]["corpus"] == "firefox-1000"
    assert scenario.data["workload"]["turns"] == ["A"]
    assert scenario.data["workload"]["repeat"] == 6
    assert scenario.data["expect"]["engagement"] == "s70-b4-worker-bounces"
    assert scenario.data["expect"]["worker_cold_witness"] == (
        "p29-action-lineage-v1"
    )
    assert scenario.data["timeline"] == [
        {"action": "restart", "instance": "F1", "trigger": f"t+{seconds}"}
        for seconds in (30, 60, 90)
    ]
    assert {item["version"] for item in plan["topology"]["instances"]} == {50}


def test_s70_b6_is_a_serial_scheduler_kill_switch_cycle() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S70-b6-kill-switch.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s70-b6-dry-plan")

    assert scenario.data["shape"] == "S'C'F'"
    assert scenario.data["workload"]["clients"] == ["C1"]
    assert scenario.data["workload"]["jobs"] == 1
    assert (
        scenario.data["expect"]["engagement"]
        == "s70-b6-drained-kill-switch-cycle"
    )
    assert scenario.data["expect"]["error106_max"] == 0
    scheduler = next(
        item for item in scenario.data["instances"] if item["role"] == "S"
    )
    assert scheduler["env"] == {"ICECC_P50_PROFILE": "P29V1"}
    assert scenario.data["timeline"] == [
        {
            "action": "env_set",
            "env": {"ICECC_P50_PROFILE": "OFF"},
            "instance": "S1",
            "trigger": "job 1",
        },
        {
            "action": "env_set",
            "env": {"ICECC_P50_PROFILE": "P29V1"},
            "instance": "S1",
            "trigger": "job 3",
        },
    ]
    assert {item["version"] for item in plan["topology"]["instances"]} == {50}


def test_s70_b7_is_an_ordered_rollback_then_rollforward_pair() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    expected = {
        "S70-b7-rollback": (
            "s70-b7-rollback",
            "downgrade",
            "new",
            "old",
            (75, 100, 125),
        ),
        "S70-b7-rollforward": (
            "s70-b7-rollforward",
            "upgrade",
            "old",
            "new",
            (25, 50, 75),
        ),
    }
    for scenario_id, (engagement, action, initial, target, triggers) in expected.items():
        scenario = load_scenario_spec(
            INTEGRATION / "scenarios" / f"{scenario_id}.json", farm
        )
        plan = farmtest.build_plan(farm, scenario, run_id=f"{scenario_id}-dry")
        assert scenario.data["shape"] == "mixed"
        assert scenario.data["workload"] == {
            "clients": ["C1"],
            "corpus": "fmt-100",
            "driver": "tu-manifest",
            "jobs": 1,
            "oracle": "local-sha",
            "repeat": 4,
            "turns": ["A"],
        }
        assert scenario.data["expect"]["engagement"] == engagement
        assert [item["image"] for item in scenario.data["instances"]] == [
            initial,
            initial,
            initial,
        ]
        assert scenario.data["timeline"] == [
            {
                "action": action,
                "image": target,
                "instance": name,
                "trigger": f"job {trigger}",
            }
            for name, trigger in zip(("S1", "F1", "C1"), triggers, strict=True)
        ]
        assert {item["version"] for item in plan["topology"]["instances"]} == {
            50 if initial == "new" else 43
        }

    suite = load_suite_spec(INTEGRATION / "suites" / "s70.json")
    assert tuple(suite.data["scenarios"]) == S70_SCENARIO_IDS


def test_s90_revision_skew_resolves_one_compatible_and_one_legacy_pair() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S90-revision-skew.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s90-dry-plan")

    assert scenario.data["shape"] == "S'[F'F''][C']"
    assert scenario.data["workload"] == {
        "clients": ["C1"],
        "corpus": "fmt-100",
        "driver": "tu-manifest",
        "jobs": 2,
        "oracle": "local-sha",
        "repeat": 1,
        "turns": ["A"],
    }
    revisions = {
        item["name"]: item["cache_wire_revision"]
        for item in plan["topology"]["instances"]
    }
    assert revisions == {"S1": 1, "C1": 1, "F1": 1, "F2": 2}
    relationships = {
        (item["c"], item["f"]): (item["cache_expected"], item["profile"])
        for item in plan["topology"]["relationships"]
    }
    assert relationships == {
        ("C1", "F1"): (True, "P29V1"),
        ("C1", "F2"): (False, None),
    }
    assert load_suite_spec(
        INTEGRATION / "suites" / "s90.json"
    ).expanded_scenario_ids() == (
        ("S90-revision-skew", 1),
        ("S90-revision-refusal-retry", 1),
    )

    refusal = load_scenario_spec(
        INTEGRATION / "scenarios" / "S90-revision-refusal-retry.json", farm
    )
    refusal_plan = farmtest.build_plan(farm, refusal, run_id="s90-refusal-dry-plan")
    assert refusal.data["shape"] == "S'[F~][C']"
    assert refusal.data["workload"]["jobs"] == 1
    target = next(
        item for item in refusal_plan["topology"]["instances"] if item["name"] == "F1"
    )
    assert target["cache_wire_revision"] == 1
    assert target["image"]["kind"] == "daemon-mutant"


def test_s95_disk_fill_mount_is_bounded_and_only_replaces_the_target_cache() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S95-cache-disk-full.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s95-dry-plan")

    assert scenario.data["timeline"] == [
        {"action": "disk_fill", "instance": "F1", "trigger": "job 12"}
    ]
    starts = {
        item["instance"]: item["argv"]
        for item in plan["commands"]
        if item["phase"] == "up.start-f"
    }
    bounded = (
        "/var/cache/icecream:rw,exec,nosuid,nodev,"
        "size=134217728,mode=0700"
    )
    assert bounded in starts["F1"]
    assert bounded not in starts["F2"]
    assert not any(
        item.startswith("type=bind,") and item.endswith("dst=/var/cache/icecream")
        for item in starts["F1"]
    )
    assert any(
        item.startswith("type=bind,") and item.endswith("dst=/var/cache/icecream")
        for item in starts["F2"]
    )
    assert load_suite_spec(
        INTEGRATION / "suites" / "s95-disk-full.json"
    ).expanded_scenario_ids() == (
        ("H5-worker-kill", 1),
        ("S95-cache-disk-full", 1),
    )
