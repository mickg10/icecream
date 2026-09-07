from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

from farmharness.integration import farmtest, lifecycle
from farmharness.integration.collect import _network_shaping_observation
from farmharness.integration.images import CommandFactory
from farmharness.integration.lifecycle import LifecycleError
from farmharness.integration.lifecycle import _netem_up_receipt
from farmharness.integration.remote import CommandResult, PlannedCommand
from farmharness.integration.netem import (
    NetemPlanError,
    apply_args,
    observe_args,
    remove_args,
    resolve_bindings,
    validate_receipt,
    validate_qdisc,
)
from farmharness.integration.verdict import _network_shaping_errors
from farmharness.integration.tests.test_lifecycle import _farm_scenario_plan


def _shaped_plan(tmp_path: Path):
    farm, scenario, _unused = _farm_scenario_plan(tmp_path)
    worker = next(item for item in scenario.data["instances"] if item["name"] == "F1")
    worker["host"] = "tt-quietbox3"
    client = next(item for item in scenario.data["instances"] if item["name"] == "C1")
    client["host"] = "tt-quietbox3"
    scenario.data["network"]["shaping"] = [
        {"instance": "F1", "rate": "100mbit", "delay_ms": 2}
    ]
    return farm, scenario, farmtest.build_plan(farm, scenario, run_id="shaped-run")


def _applied_receipt(scenario, plan):
    binding = lifecycle._netem_bindings(plan)[0]

    def command(phase: str) -> PlannedCommand:
        item = next(
            value
            for value in plan["commands"]
            if value["phase"] == phase and value["instance"] == binding.instance
        )
        return PlannedCommand(
            sequence=item["sequence"],
            phase=item["phase"],
            host=item["host"],
            instance=item["instance"],
            transport=item["transport"],
            timeout_s=item["timeout_s"],
            argv=tuple(item["argv"]),
        )

    return _netem_up_receipt(
        (binding,),
        [CommandResult(0, "a" * 64 + "\n", "")],
        [CommandResult(0, "", "")],
        [CommandResult(0, "qdisc netem 8001: root delay 2.0ms rate 100Mbit\n", "")],
        [command("up.network-create")],
        [command("up.netem-apply")],
        [command("up.netem-observe")],
    )


def test_shaped_worker_uses_private_bridge_and_container_tc_only(tmp_path: Path) -> None:
    _farm, _scenario, plan = _shaped_plan(tmp_path)
    binding = plan["network_shaping"]["bindings"][0]
    assert binding == {
        "bridge": "icefarm-shaped-run-F1-netem",
        "container": "icefarm-shaped-run-F1",
        "container_port": plan["ports"]["instances"]["F1"],
        "delay_ms": 2,
        "host": "tt-quietbox3",
        "host_port": plan["ports"]["instances"]["F1"],
        "instance": "F1",
        "rate": "100mbit",
        "role": "F",
    }
    phases = [item["phase"] for item in plan["commands"]]
    assert phases.count("up.network-create") == 1
    assert phases.count("up.netem-apply") == 1
    assert phases.count("up.netem-observe") == 1
    start_f = next(
        item
        for item in plan["commands"]
        if item["phase"] == "up.start-f" and item["instance"] == "F1"
    )
    assert "--network" in start_f["argv"]
    assert start_f["argv"][start_f["argv"].index("--network") + 1] == binding["bridge"]
    assert "--publish" in start_f["argv"]
    assert f"{binding['host_port']}:{binding['container_port']}" in start_f["argv"]
    assert start_f["argv"].count("NET_ADMIN") == 1
    assert "host" not in start_f["argv"]

    for item in plan["commands"]:
        if item["phase"] not in {"up.netem-apply", "up.netem-observe"}:
            continue
        argv = item["argv"]
        assert argv[0:3] == ["docker", "--context", "q3"]
        assert argv[3:5] == ["exec", "--user"]
        assert "tc" in argv
        assert argv.index("tc") > argv.index(binding["container"])
        assert "ssh" not in argv


def test_netem_witness_is_authenticated_and_tamper_fails() -> None:
    binding = resolve_bindings(
        type(
            "Farm",
            (),
            {"hosts": {"q2": {"netem": False}, "q3": {"netem": True}}},
        )(),
        {
            "network": {
                "shaping": [{"instance": "F1", "rate": "100mbit", "delay_ms": 2}]
            },
            "workload": {"clients": ["C1"]},
        },
        {
            "instances": [
                {"name": "F1", "role": "F", "host": "q3"},
                {"name": "C1", "role": "C", "host": "q3"},
            ]
        },
        {"instances": {"F1": 23102}},
        "run",
    )[0]
    witness = "qdisc netem 8001: root limit 1000 delay 2.0ms rate 100Mbit"
    assert validate_qdisc(binding, witness)["rate"] == "100mbit"
    with pytest.raises(NetemPlanError, match="wrong rate"):
        validate_qdisc(binding, witness.replace("100Mbit", "25Mbit"))
    with pytest.raises(NetemPlanError, match="wrong delay"):
        validate_qdisc(binding, witness.replace("2.0ms", "3.0ms"))
    assert apply_args(binding)[3] == binding.container
    assert observe_args(binding)[3] == binding.container
    assert remove_args(binding)[3] == binding.container


def test_netem_plan_rejects_tampered_identity() -> None:
    from farmharness.integration.lifecycle import _netem_bindings

    plan = {
        "network_shaping": {
            "schema": "icefarm-netem-plan-v1",
            "bindings": [
                {
                    "bridge": "icefarm-run-F1-netem",
                    "container": "icefarm-run-F1",
                    "container_port": 23102,
                    "delay_ms": 2,
                    "host": "q3",
                    "host_port": 23102,
                    "instance": "F1",
                    "rate": "100mbit",
                    "role": "F",
                }
            ],
        },
        "ports": {"instances": {"F1": 23102}},
        "run_id": "run",
        "topology": {"instances": [{"name": "F1", "role": "F", "host": "q3"}]},
    }
    assert _netem_bindings(plan)[0].bridge == "icefarm-run-F1-netem"
    plan["network_shaping"]["bindings"][0]["container"] = "icefarm-other-F1"
    with pytest.raises(LifecycleError, match="identity mismatch"):
        _netem_bindings(plan)


def test_netem_lifecycle_receipt_binds_application_witness_and_removal() -> None:
    binding = resolve_bindings(
        type(
            "Farm",
            (),
            {"hosts": {"q2": {"netem": False}, "q3": {"netem": True}}},
        )(),
        {
            "network": {
                "shaping": [{"instance": "F1", "rate": "100mbit", "delay_ms": 2}]
            },
            "workload": {"clients": ["C1"]},
        },
        {
            "instances": [
                {"name": "F1", "role": "F", "host": "q3"},
                {"name": "C1", "role": "C", "host": "q3"},
            ]
        },
        {"instances": {"F1": 23102}},
        "run",
    )[0]

    def command(phase: str, argv: tuple[str, ...]) -> PlannedCommand:
        return PlannedCommand(1, phase, "q3", "F1", "docker-context", 30, argv)

    receipt = _netem_up_receipt(
        (binding,),
        [CommandResult(0, "a" * 64 + "\n", "")],
        [CommandResult(0, "", "")],
        [CommandResult(0, "qdisc netem 8001: root delay 2.0ms rate 100Mbit\n", "")],
        [command("up.network-create", ("docker", "network", "create"))],
        [command("up.netem-apply", ("docker", *apply_args(binding)))],
        [command("up.netem-observe", ("docker", *observe_args(binding)))],
    )
    record = receipt["bindings"][0]
    assert receipt["status"] == "APPLIED"
    assert record["request"]["rate"] == "100mbit"
    assert record["removal"]["container_argv"] == list(remove_args(binding))
    assert record["observation"]["witness"]["delay_ms"] == 2


def test_netem_applied_receipt_is_bound_by_collector_and_pure_verdict(
    tmp_path: Path,
) -> None:
    _farm, scenario, plan = _shaped_plan(tmp_path)
    receipt = _applied_receipt(scenario, plan)
    assert validate_receipt(scenario.data, plan, receipt) == receipt
    evidence = tmp_path / "evidence"
    (evidence / "receipts").mkdir(parents=True)
    (evidence / "receipts" / "lifecycle.json").write_text(
        json.dumps({"network_shaping": receipt}), encoding="utf-8"
    )
    observation = _network_shaping_observation(scenario, plan, evidence)
    assert observation == receipt
    assert not _network_shaping_errors(
        scenario.data, {"network_shaping": observation}, plan
    )


@pytest.mark.parametrize(
    "mutation",
    (
        lambda value: value["bindings"][0]["bridge"].__setitem__(
            "network_id", "bad"
        ),
        lambda value: value["bindings"][0]["application"]["argv"].append("extra"),
        lambda value: value["bindings"][0]["observation"].__setitem__(
            "sha256", "0" * 64
        ),
        lambda value: value["bindings"][0]["observation"]["witness"].__setitem__(
            "rate", "25mbit"
        ),
        lambda value: value["bindings"][0]["removal"]["container_argv"].append(
            "extra"
        ),
    ),
)
def test_netem_receipt_tampering_fails_closed(tmp_path: Path, mutation) -> None:
    _farm, scenario, plan = _shaped_plan(tmp_path)
    receipt = _applied_receipt(scenario, plan)
    mutation(receipt)
    with pytest.raises(NetemPlanError):
        validate_receipt(scenario.data, plan, receipt)
    assert _network_shaping_errors(
        scenario.data, {"network_shaping": receipt}, plan
    ) == {"@observations:network_shaping"}


def test_netem_coherent_plan_and_receipt_command_tamper_fails_closed(
    tmp_path: Path,
) -> None:
    _farm, scenario, original_plan = _shaped_plan(tmp_path)
    plan = copy.deepcopy(original_plan)
    receipt = _applied_receipt(scenario, plan)
    command = next(
        item for item in plan["commands"] if item["phase"] == "up.netem-apply"
    )
    command["argv"].append("unsafe-extra")
    receipt["bindings"][0]["application"]["argv"].append("unsafe-extra")

    with pytest.raises(NetemPlanError, match="safe seam"):
        validate_receipt(scenario.data, plan, receipt)


def test_netem_teardown_reauthenticates_and_targets_container_id(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    farm, _scenario, plan = _shaped_plan(tmp_path)
    binding = lifecycle._netem_bindings(plan)[0]
    container_id = "b" * 64
    monkeypatch.setattr(
        lifecycle,
        "_labelled_containers",
        lambda *_args, **_kwargs: [
            {
                "created": "2026-09-07T00:00:00Z",
                "id": container_id,
                "name": binding.container,
                "run_id": plan["run_id"],
            }
        ],
    )

    class Recorder:
        def __init__(self) -> None:
            self.commands = []

        def invoke(self, command: PlannedCommand) -> CommandResult:
            self.commands.append(command)
            return CommandResult(0, "", "")

    recorder = Recorder()
    receipts, problems = lifecycle._remove_netem(
        farm, plan, (binding,), recorder, CommandFactory(), 30
    )
    assert not problems
    assert receipts[0]["container_id"] == container_id
    command = recorder.commands[-1]
    assert command.phase == "down.netem-remove"
    assert container_id in command.argv
    assert binding.container not in command.argv


def test_netem_removal_refuses_an_unauthenticated_target(tmp_path: Path) -> None:
    _farm, _scenario, plan = _shaped_plan(tmp_path)
    binding = lifecycle._netem_bindings(plan)[0]
    with pytest.raises(NetemPlanError, match="authenticated container id"):
        remove_args(binding, "other-container")


@pytest.mark.parametrize(
    ("shapes", "message"),
    [
        ([{"instance": "F1", "rate": "25mbit", "delay_ms": 2}], "100mbit"),
        ([{"instance": "C1", "rate": "100mbit", "delay_ms": 2}], "only an F"),
        (
            [
                {"instance": "F1", "rate": "100mbit", "delay_ms": 2},
                {"instance": "F2", "rate": "100mbit", "delay_ms": 2},
            ],
            "exactly one",
        ),
    ],
)
def test_netem_scope_is_fail_closed(shapes, message: str) -> None:
    farm = type(
        "Farm",
        (),
        {"hosts": {"q2": {"netem": False}, "q3": {"netem": True}}},
    )()
    topology = {
        "instances": [
            {"name": "F1", "role": "F", "host": "q3"},
            {"name": "F2", "role": "F", "host": "q3"},
            {"name": "C1", "role": "C", "host": "q3"},
        ]
    }
    with pytest.raises(NetemPlanError, match=message):
        resolve_bindings(
            farm,
            {"network": {"shaping": shapes}, "workload": {"clients": ["C1"]}},
            topology,
            {"instances": {"F1": 23102, "F2": 23103, "C1": 23104}},
            "run",
        )


def test_netem_refuses_cross_host_client_that_cannot_route_bridge_ip() -> None:
    farm = type(
        "Farm",
        (),
        {"hosts": {"q2": {"netem": False}, "q3": {"netem": True}}},
    )()
    with pytest.raises(NetemPlanError, match="client on the same host"):
        resolve_bindings(
            farm,
            {
                "network": {
                    "shaping": [
                        {"instance": "F1", "rate": "100mbit", "delay_ms": 2}
                    ]
                },
                "workload": {"clients": ["C1"]},
            },
            {
                "instances": [
                    {"name": "F1", "role": "F", "host": "q3"},
                    {"name": "C1", "role": "C", "host": "q2"},
                ]
            },
            {"instances": {"F1": 23102}},
            "run",
        )
