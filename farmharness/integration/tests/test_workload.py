from __future__ import annotations

import json
from pathlib import Path

import pytest

from farmharness.integration import farmtest
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.images import RecordingTransport
from farmharness.integration.remote import CommandResult, PlannedCommand, decode_ssh_payload
from farmharness.integration.scenario_spec import ScenarioSpecError, load_scenario_spec
from farmharness.integration.workload import (
    MANIFEST_DRIVER,
    WORKLOAD_SCHEMA,
    WorkloadError,
    run_workload,
)


INTEGRATION = Path(__file__).resolve().parents[1]


def _farm_scenario_plan(tmp_path: Path):
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    farm.data["hub"]["results_root"] = str(tmp_path)
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    plan = farmtest.build_plan(farm, scenario, run_id="workload-unit")
    return farm, scenario, plan


class WorkloadRecorder:
    def __init__(self, stdout: str = "ICEFARM_WORKLOAD jobs=100 failures=0 samples=3\n") -> None:
        self.stdout = stdout
        self.commands: list[PlannedCommand] = []

    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        return CommandResult(0, self.stdout, "")


def test_plan_mounts_authenticated_corpus_read_only_and_closure_scoped_oracle(
    tmp_path: Path,
) -> None:
    _farm, _scenario, plan = _farm_scenario_plan(tmp_path)
    client = next(item for item in plan["commands"] if item["phase"] == "up.start-c")
    argv = client["argv"]
    corpus_mount = next(
        item for item in argv if item.endswith("dst=/corpus,readonly")
    )
    oracle_mount = next(item for item in argv if item.endswith("dst=/oracle"))
    assert "/corpora/fmt-100," in corpus_mount
    assert plan["topology"]["instances"][1]["image"]["closure_sha256"] in oracle_mount
    for environment in (
        "ICECC_P50_COMPILE_IDENTITY_TRACE=/results/compile-identity.jsonl",
        "ICECC_P50_C_ACTION_TRACE=/results/c-action.jsonl",
        "ICECC_P50_C_LEGACY_WIRE_TRACE=/results/c-legacy-wire.jsonl",
    ):
        assert environment in argv


def test_manifest_driver_is_one_fixed_program_with_all_spec_values_in_argv(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    scripted = WorkloadRecorder()
    transport = RecordingTransport(scripted)
    receipt = run_workload(
        farm,
        scenario,
        plan,
        recorder=transport,
        require_up=False,
    )
    assert receipt["schema"] == WORKLOAD_SCHEMA
    assert receipt["status"] == "COMPLETE"
    assert receipt["clients"] == [
        {"client": "C1", "failures": 0, "jobs": 100, "samples": 3}
    ]
    assert len(scripted.commands) == 1
    argv = scripted.commands[0].argv
    assert MANIFEST_DRIVER in argv
    assert argv[-2:] == ("0", "A") or argv[-2:] == ("1", "A")
    assert "fmt-100" not in MANIFEST_DRIVER
    assert "oracle_command='g++-11 -O2 -fdiagnostics-color=never -c'" in MANIFEST_DRIVER
    assert "'building myself, but telling localhost'" in MANIFEST_DRIVER
    assert '"$remote" -eq 1' in MANIFEST_DRIVER
    persisted = json.loads((tmp_path / "results" / "workload-unit" / "workload.json").read_text())
    assert persisted == receipt


def test_workload_summary_is_fail_closed(tmp_path: Path) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    transport = RecordingTransport(WorkloadRecorder("not a summary\n"))
    with pytest.raises(WorkloadError, match="no unique workload summary"):
        run_workload(
            farm,
            scenario,
            plan,
            recorder=transport,
            require_up=False,
        )


def test_workload_requires_authenticated_up_receipt(tmp_path: Path) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    with pytest.raises(WorkloadError, match="cannot load UP lifecycle receipt"):
        run_workload(
            farm,
            scenario,
            plan,
            recorder=RecordingTransport(WorkloadRecorder()),
        )


def test_single_manifest_refuses_an_undefined_turn(tmp_path: Path) -> None:
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    scenario = json.loads((INTEGRATION / "scenarios" / "S00-smoke.json").read_text())
    scenario["workload"]["turns"] = ["B"]
    path = tmp_path / "scenario.json"
    path.write_text(json.dumps(scenario), encoding="utf-8")
    with pytest.raises(ScenarioSpecError, match="does not define turns"):
        load_scenario_spec(path, farm)


def test_ssh_transport_keeps_driver_values_inside_encoded_argv(tmp_path: Path) -> None:
    farm, scenario, plan = _farm_scenario_plan(tmp_path)
    farm.hosts["tt-quietbox3"].pop("docker_context")
    plan = farmtest.build_plan(farm, scenario, run_id="workload-ssh")
    scripted = WorkloadRecorder()
    run_workload(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        require_up=False,
    )
    decoded = decode_ssh_payload(scripted.commands[0].argv)
    assert decoded[:3] == ("docker", "exec", "--user")
    assert MANIFEST_DRIVER in decoded
