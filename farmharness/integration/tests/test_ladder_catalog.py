from __future__ import annotations

from pathlib import Path
import copy
import hashlib
import json

import pytest

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest
from farmharness.integration import receipt_window_matrix
from farmharness.integration.events import EventProducer
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.farm_spec import FarmSpec
from farmharness.integration.images import RecordingTransport
from farmharness.integration.scenario_spec import ScenarioSpecError, load_scenario_spec
from farmharness.integration.suite_spec import (
    CONTROL_SCENARIO_IDS,
    S70_SCENARIO_IDS,
    SMOKE_SCENARIO_IDS,
    load_suite_spec,
)


INTEGRATION = Path(__file__).resolve().parents[1]


def test_s95_plan_preserves_client_canary_output_for_failure_diagnostics() -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "S95-cache-disk-full.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="s95-diagnostic-capture")

    assert plan["diagnostic_capture_client_output"] is True
    assert plan["diagnostic_capture_client_output_kind"] == "s95-canary"

    d18 = load_scenario_spec(INTEGRATION / "scenarios" / "D18-P29V1.json", farm)
    d18_plan = farmtest.build_plan(farm, d18, run_id="d18-diagnostic-capture")
    assert d18_plan["diagnostic_capture_client_output"] is True
    assert d18_plan["diagnostic_capture_client_output_kind"] == "d18-workload"

    smoke = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    smoke_plan = farmtest.build_plan(farm, smoke, run_id="s00-no-diagnostic-capture")
    assert "diagnostic_capture_client_output" not in smoke_plan


def test_p51_receipt_window_plan_stages_pinned_helper_and_scopes_net_admin(tmp_path: Path) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm_data = copy.deepcopy(farm.data)
    farm_data["authority"]["topologies"]["C1F1"]["slots_per_f"] = 31
    farm = FarmSpec(farm.path, farm_data)
    helper = Path("/bin/true")
    scenario_data = json.loads(
        (INTEGRATION / "scenarios" / "S00-smoke.json").read_text(encoding="utf-8")
    )
    scenario_data["id"] = "P51-receipt-window-test"
    scenario_data["instances"][0].setdefault("env", {})["ICECC_P51_MODE"] = "on"
    scenario_data["instances"][1]["slots"] = 31
    scenario_data["instances"][1].setdefault("env", {})["ICECC_P51_MODE"] = "on"
    scenario_data["instances"][2]["env"]["ICECC_P51_MODE"] = "on"
    scenario_data["workload"].update(
        {
            "driver": "p51-receipt-window",
            "jobs": 30,
            "receipt_gate": {
                "binary": str(helper),
                "binary_sha256": hashlib.sha256(helper.read_bytes()).hexdigest(),
                "expected_commits": 30,
                "negotiated_window": 30,
                "expect_observed": True,
            },
        }
    )
    scenario_path = tmp_path / "p51-receipt-window.json"
    scenario_path.write_text(json.dumps(scenario_data), encoding="utf-8")
    scenario = load_scenario_spec(scenario_path, farm)
    plan = farmtest.build_plan(farm, scenario, run_id="p51-receipt-window-plan")

    helper_command = next(
        command for command in plan["commands"]
        if command["phase"] == "up.stage-p51-receipt-gate"
    )
    start_client = next(
        command for command in plan["commands"] if command["phase"] == "up.start-c"
    )
    start_worker = next(
        command for command in plan["commands"] if command["phase"] == "up.start-f"
    )
    assert helper_command["transport"] == "rsync-ssh"
    assert helper_command["argv"][-1].endswith("/output/p50daemonpositive")
    assert "NET_ADMIN" in start_client["argv"]
    assert "ICECC_P50_DIAGNOSTICS=1" in start_client["argv"]
    assert "ICECC_P50_DIAGNOSTICS=1" in start_worker["argv"]
    assert plan["p51_receipt_gate"]["binary_sha256"] == hashlib.sha256(
        helper.read_bytes()
    ).hexdigest()
    assert plan["diagnostic_capture_client_output"] is True
    assert plan["diagnostic_capture_client_output_kind"] == "p51-receipt-window"
    assert plan["topology"]["relationships"][0]["cache_expected"] is True

    scenario_data["workload"]["receipt_gate"].update(
        {"expected_commits": 30, "negotiated_window": 1, "expect_observed": False}
    )
    scenario_path.write_text(json.dumps(scenario_data), encoding="utf-8")
    negative = load_scenario_spec(scenario_path, farm)
    negative_plan = farmtest.build_plan(farm, negative, run_id="p51-window1-negative")
    client = next(
        item for item in negative_plan["commands"] if item["phase"] == "up.start-c"
    )
    assert "ICECC_P50_PIPELINE_WINDOW=1" in client["argv"]


def test_p51_multilink_matrix_generates_all_required_portable_cells(tmp_path: Path) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm_data = copy.deepcopy(farm.data)
    for row in receipt_window_matrix.TOPOLOGY_ROWS:
        farm_data["authority"]["topologies"][row["id"]] = {
            "f_relationships": row["workers"],
            "slots_per_f": row["clients"] * 30 + int(row["clients"] == 1),
        }
    farm_path = tmp_path / "authorized-farm.json"
    farm_path.write_text(json.dumps(farm_data), encoding="utf-8")
    output = tmp_path / "matrix"
    generated = receipt_window_matrix.generate_matrix(
        farm_path=farm_path,
        base_path=INTEGRATION / "scenarios" / "D18-P29V1.json",
        helper_path=Path("/bin/true"),
        output_dir=output,
    )
    assert len(generated) == 6 * 2 * 3
    assert receipt_window_matrix._load_json(
        receipt_window_matrix.TEMPLATE_PATH
    )["restart_extension"] == "pending-not-run"
    assert {path.name for path in generated} == {
        f"P51-receipt-{row['id']}-{profile}-W{window}.json"
        for row in receipt_window_matrix.TOPOLOGY_ROWS
        for profile in receipt_window_matrix.PROFILE_NAMES
        for window in (1, 30)
    }

    over_budget = json.loads(generated[0].read_text())
    over_budget["workload"]["receipt_gate"]["command_timeout_s"] += 1
    over_budget_path = tmp_path / "over-budget.json"
    over_budget_path.write_text(json.dumps(over_budget), encoding="utf-8")
    with pytest.raises(ScenarioSpecError, match="may not exceed the turn budget"):
        load_scenario_spec(over_budget_path, load_farm_spec(farm_path))

    max_plan = None
    for path in generated:
        scenario = load_scenario_spec(path, load_farm_spec(farm_path))
        gate = scenario.data["workload"]["receipt_gate"]
        window = gate["negotiated_window"]
        assert gate["command_timeout_s"] == scenario.data["timeouts"]["turn_s"]
        clients = [item for item in scenario.data["instances"] if item["role"] == "C"]
        assert all(item["image"] == "new" for item in clients)
        assert all(item["env"]["ICECC_P50_MODE"] == "on" for item in clients)
        assert all(item["env"]["ICECC_P51_MODE"] == "on" for item in clients)
        workers = [item for item in scenario.data["instances"] if item["role"] == "F"]
        assert all(item["image"] == "new" for item in workers)
        assert all(item["env"]["ICECC_P51_MODE"] == "on" for item in workers)
        scheduler = next(item for item in scenario.data["instances"] if item["role"] == "S")
        assert scheduler["image"] == "new"
        assert scheduler["env"]["ICECC_P51_MODE"] == "on"
        assert "d18_roles" not in scenario.data["workload"]
        plan = farmtest.build_plan(
            load_farm_spec(farm_path), scenario, run_id=f"p51-matrix-{scenario.data['id']}"
        )
        scheduler_command = next(
            command for command in plan["commands"]
            if command["phase"] == "up.start-s"
        )
        scheduler_argv = scheduler_command["argv"]
        workers = [item for item in scenario.data["instances"] if item["role"] == "F"]
        expected_credit = (
            len(workers) * window
            if window == 30 and len(clients) == 1 and len(workers) > 1
            else None
        )
        if expected_credit is None:
            assert "/opt/icecream/entry-scheduler.sh" in scheduler_argv
            assert "--max-outstanding-dispatches" not in scheduler_argv
        else:
            assert "/opt/icecream/sbin/icecc-scheduler" in scheduler_argv
            assert "-u" in scheduler_argv
            assert scheduler_argv[scheduler_argv.index("-u") + 1] == "nobody"
            assert scheduler_argv[scheduler_argv.index("--max-outstanding-dispatches") + 1] == str(expected_credit)
        assert gate["expect_observed"] is True
        assert scenario.data["workload"]["jobs"] == len(gate["links"]) // len(clients) * window
        assert len(gate["links"]) == len(clients) * len(workers)
        for client in clients:
            spans = sorted(
                (link["first_job"], link["last_job"])
                for link in gate["links"] if link["client"] == client["name"]
            )
            assert spans[0][0] == 1
            assert spans[-1][1] == 100 * scenario.data["workload"]["repeat"]
            assert all(a[1] + 1 == b[0] for a, b in zip(spans, spans[1:]))
            assert all(last - first + 1 >= window for first, last in spans)
        if scenario.data["id"] == "P51-receipt-C4F1-ZSTD_ROUTE-W30":
            max_plan = farmtest.build_plan(load_farm_spec(farm_path), scenario, run_id="p51-matrix-c4f1")
            assert [link["slots"] for link in max_plan["topology"]["instances"] if link["role"] == "F"] == [120]
            assert len(max_plan["p51_receipt_gate"]["links"]) == 4
    assert max_plan is not None


def test_p51_multilink_matrix_selects_one_authorized_w1_cell(tmp_path: Path) -> None:
    farm_data = json.loads(farm_fixture.example_farm_path().read_text())
    farm_data["authority"]["topologies"] = {
        "C1F2": {"f_relationships": 2, "slots_per_f": 24}
    }
    farm_path = tmp_path / "selected-farm.json"
    farm_path.write_text(json.dumps(farm_data), encoding="utf-8")
    output = tmp_path / "selected-matrix"

    generated = receipt_window_matrix.generate_matrix(
        farm_path=farm_path,
        base_path=INTEGRATION / "scenarios" / "D18-P29V1.json",
        helper_path=Path("/bin/true"),
        output_dir=output,
        topologies=("C1F2",),
        windows=(1,),
        profiles=("P29V1",),
    )
    assert [path.name for path in generated] == [
        "P51-receipt-C1F2-P29V1-W1.json"
    ]
    scenario = load_scenario_spec(generated[0], load_farm_spec(farm_path))
    assert scenario.data["workload"]["receipt_gate"]["negotiated_window"] == 1
    assert scenario.data["workload"]["receipt_gate"]["expected_commits"] == 1
    assert scenario.data["workload"]["jobs"] == 2
    assert {item["name"]: item["slots"] for item in scenario.data["instances"] if item["role"] == "F"} == {
        "F1": 2, "F2": 1,
    }


@pytest.mark.parametrize(
    ("selection", "message"),
    [
        ({"topologies": ("C9F1",)}, "unsupported topology selector"),
        ({"windows": (2,)}, "unsupported window selector"),
        ({"profiles": ("UNKNOWN",)}, "unsupported profile selector"),
    ],
)
def test_p51_multilink_matrix_rejects_invalid_selectors_before_output(
    tmp_path: Path, selection: dict[str, tuple[object, ...]], message: str,
) -> None:
    farm_data = json.loads(farm_fixture.example_farm_path().read_text())
    farm_data["authority"]["topologies"] = {
        "C1F2": {"f_relationships": 2, "slots_per_f": 24}
    }
    farm_path = tmp_path / "selected-farm.json"
    farm_path.write_text(json.dumps(farm_data), encoding="utf-8")
    output = tmp_path / "invalid-matrix"
    with pytest.raises(receipt_window_matrix.MatrixError, match=message):
        receipt_window_matrix.generate_matrix(
            farm_path=farm_path,
            base_path=INTEGRATION / "scenarios" / "D18-P29V1.json",
            helper_path=Path("/bin/true"),
            output_dir=output,
            **selection,
        )
    assert not output.exists()


def test_p51_multilink_matrix_selected_w30_still_requires_full_capacity(
    tmp_path: Path,
) -> None:
    farm_data = json.loads(farm_fixture.example_farm_path().read_text())
    farm_data["authority"]["topologies"] = {
        "C1F2": {"f_relationships": 2, "slots_per_f": 24}
    }
    farm_path = tmp_path / "selected-farm.json"
    farm_path.write_text(json.dumps(farm_data), encoding="utf-8")
    output = tmp_path / "under-capacity"
    with pytest.raises(receipt_window_matrix.MatrixError, match="C1F2 needs slots_per_f >= 31"):
        receipt_window_matrix.generate_matrix(
            farm_path=farm_path,
            base_path=INTEGRATION / "scenarios" / "D18-P29V1.json",
            helper_path=Path("/bin/true"),
            output_dir=output,
            topologies=("C1F2",),
            windows=(30,),
            profiles=("P29V1",),
        )
    assert not output.exists()


def test_p51_multilink_matrix_preserves_explicit_caps_into_docker_plan(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm_data = copy.deepcopy(farm.data)
    for row in receipt_window_matrix.TOPOLOGY_ROWS:
        farm_data["authority"]["topologies"][row["id"]] = {
            "f_relationships": row["workers"],
            "slots_per_f": row["clients"] * 30 + int(row["clients"] == 1),
        }
    farm_path = tmp_path / "authorized-farm.json"
    farm_path.write_text(json.dumps(farm_data), encoding="utf-8")

    base = json.loads((INTEGRATION / "scenarios" / "D18-P29V1.json").read_text())
    role_caps = {"S1": (1, 2048), "C_R2": (1, 2048), "F_R2": (2, 32768)}
    for name, (cpus, memory_mb) in role_caps.items():
        instance = next(item for item in base["instances"] if item["name"] == name)
        instance.update({"cpus": cpus, "memory_mb": memory_mb})
    base_path = tmp_path / "capped-base.json"
    base_path.write_text(json.dumps(base), encoding="utf-8")

    generated = receipt_window_matrix.generate_matrix(
        farm_path=farm_path,
        base_path=base_path,
        helper_path=Path("/bin/true"),
        output_dir=tmp_path / "capped-matrix",
    )
    scenario_path = next(
        path for path in generated
        if path.name == "P51-receipt-C1F2-P29V1-W30.json"
    )
    scenario = load_scenario_spec(scenario_path, load_farm_spec(farm_path))
    expected = {
        "S1": (1, 2048), "F1": (2, 32768), "F2": (2, 32768), "C1": (1, 2048)
    }
    assert {
        item["name"]: (item["cpus"], item["memory_mb"])
        for item in scenario.data["instances"]
    } == expected

    plan = farmtest.build_plan(
        load_farm_spec(farm_path), scenario, run_id="p51-capped-matrix-plan"
    )
    starts = [
        command for command in plan["commands"]
        if command["phase"] in {"up.start-s", "up.start-f", "up.start-c"}
    ]
    assert len(starts) == len(expected)
    for command in starts:
        argv = command["argv"]
        image_index = argv.index("--entrypoint") + 2
        assert "--cpus" in argv[:image_index]
        assert "--memory" in argv[:image_index]
        instance = next(
            item for item in scenario.data["instances"]
            if item["name"] == command["instance"]
        )
        assert argv[argv.index("--cpus") + 1] == str(instance["cpus"])
        assert argv[argv.index("--memory") + 1] == f"{instance['memory_mb']}m"


@pytest.mark.parametrize("role", ["S", "F"])
@pytest.mark.parametrize("selection", ["missing", "ambiguous"])
def test_p51_multilink_matrix_requires_one_p51_role_template(
    tmp_path: Path, role: str, selection: str,
) -> None:
    farm_data = json.loads(farm_fixture.example_farm_path().read_text())
    for row in receipt_window_matrix.TOPOLOGY_ROWS:
        farm_data["authority"]["topologies"][row["id"]] = {
            "f_relationships": row["workers"],
            "slots_per_f": row["clients"] * 30 + int(row["clients"] == 1),
        }
    farm_path = tmp_path / "authorized-farm.json"
    farm_path.write_text(json.dumps(farm_data), encoding="utf-8")
    base = json.loads((INTEGRATION / "scenarios" / "D18-P29V1.json").read_text())
    if role == "S":
        selected = next(item for item in base["instances"] if item["role"] == "S")
        env_key = "ICECC_P51_MODE"
    else:
        selected = next(item for item in base["instances"] if item["name"] == "F_R2")
        env_key = "ICECC_P51_MODE"
    if selection == "missing":
        selected["env"][env_key] = "off"
    else:
        duplicate = copy.deepcopy(selected)
        duplicate["name"] = f"{selected['name']}_DUP"
        base["instances"].append(duplicate)
    base_path = tmp_path / f"base-{role}-{selection}.json"
    base_path.write_text(json.dumps(base), encoding="utf-8")
    output = tmp_path / f"matrix-{role}-{selection}"

    with pytest.raises(
        receipt_window_matrix.MatrixError,
        match="exactly one P51-enabled (scheduler|worker) template",
    ):
        receipt_window_matrix.generate_matrix(
            farm_path=farm_path,
            base_path=base_path,
            helper_path=Path("/bin/true"),
            output_dir=output,
        )
    assert not output.exists()


def test_p51_multilink_matrix_refuses_unavailable_authority(tmp_path: Path) -> None:
    farm_path = farm_fixture.example_farm_path()
    with pytest.raises(receipt_window_matrix.MatrixError, match="must explicitly authorize C1F3"):
        receipt_window_matrix.generate_matrix(
            farm_path=farm_path,
            base_path=INTEGRATION / "scenarios" / "S00-smoke.json",
            helper_path=Path("/bin/true"),
            output_dir=tmp_path / "matrix",
        )


@pytest.mark.parametrize("selection", ["missing", "ambiguous"])
def test_p51_multilink_matrix_requires_one_explicit_r2_client_template(
    tmp_path: Path, selection: str,
) -> None:
    farm_data = json.loads(farm_fixture.example_farm_path().read_text())
    for row in receipt_window_matrix.TOPOLOGY_ROWS:
        farm_data["authority"]["topologies"][row["id"]] = {
            "f_relationships": row["workers"],
            "slots_per_f": row["clients"] * 30 + int(row["clients"] == 1),
        }
    farm_path = tmp_path / "authorized-farm.json"
    farm_path.write_text(json.dumps(farm_data), encoding="utf-8")

    base = json.loads((INTEGRATION / "scenarios" / "D18-P29V1.json").read_text())
    if selection == "missing":
        next(item for item in base["instances"] if item["name"] == "C_R2")["env"][
            "ICECC_P51_MODE"
        ] = "off"
    else:
        duplicate = copy.deepcopy(
            next(item for item in base["instances"] if item["name"] == "C_R2")
        )
        duplicate["name"] = "C_R2_DUP"
        base["instances"].append(duplicate)
    base_path = tmp_path / f"base-{selection}.json"
    base_path.write_text(json.dumps(base), encoding="utf-8")
    output = tmp_path / f"matrix-{selection}"

    with pytest.raises(
        receipt_window_matrix.MatrixError,
        match="exactly one P50/R2 client template",
    ):
        receipt_window_matrix.generate_matrix(
            farm_path=farm_path,
            base_path=base_path,
            helper_path=Path("/bin/true"),
            output_dir=output,
        )
    assert not output.exists()


def test_p51_multilink_matrix_refuses_aggregate_slots_at_dispatch_credit_clamp(
    tmp_path: Path,
) -> None:
    farm_data = json.loads(farm_fixture.example_farm_path().read_text())
    for row in receipt_window_matrix.TOPOLOGY_ROWS:
        farm_data["authority"]["topologies"][row["id"]] = {
            "f_relationships": row["workers"],
            "slots_per_f": 30 if row["id"] == "C1F2" else 120,
        }
    farm_path = tmp_path / "authorized-farm.json"
    farm_path.write_text(json.dumps(farm_data), encoding="utf-8")
    with pytest.raises(receipt_window_matrix.MatrixError, match="C1F2 needs slots_per_f >= 31"):
        receipt_window_matrix.generate_matrix(
            farm_path=farm_path,
            base_path=INTEGRATION / "scenarios" / "S00-smoke.json",
            helper_path=Path("/bin/true"),
            output_dir=tmp_path / "matrix",
        )


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
        {"action": "disk_fill", "instance": "F2", "trigger": "job 12"}
    ]
    starts = {
        item["instance"]: item["argv"]
        for item in plan["commands"]
        if item["phase"] == "up.start-f"
    }
    bounded = (
        "/var/cache/icecream:rw,exec,nosuid,nodev,"
        "size=536870912,mode=0700"
    )
    assert bounded in starts["F2"]
    assert bounded not in starts["F1"]
    assert not any(
        item.startswith("type=bind,") and item.endswith("dst=/var/cache/icecream")
        for item in starts["F2"]
    )
    assert any(
        item.startswith("type=bind,") and item.endswith("dst=/var/cache/icecream")
        for item in starts["F1"]
    )
    assert load_suite_spec(
        INTEGRATION / "suites" / "s95-disk-full.json"
    ).expanded_scenario_ids() == (
        ("H5-worker-kill", 1),
        ("S95-cache-disk-full", 1),
    )
