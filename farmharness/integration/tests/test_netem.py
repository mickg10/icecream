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
    network_inspect_args,
    network_list_args,
    network_remove_args,
    observe_args,
    resolve_bindings,
    validate_receipt,
    validate_qdisc,
    validate_network_inspect,
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
        plan["run_id"],
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
        "direction": "F-egress",
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
    assert network_remove_args("a" * 64)[-1] == "a" * 64


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
                    "direction": "F-egress",
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
        "run",
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
    assert record["removal"]["network_list_argv"] == list(
        network_list_args(binding, "run")
    )
    assert record["removal"]["network_inspect_argv"] == list(
        network_inspect_args("a" * 64)
    )
    assert record["removal"]["network_remove_argv"] == list(
        network_remove_args("a" * 64)
    )
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
        lambda value: value["bindings"][0]["removal"]["network_remove_argv"].append(
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


class _NetworkRecorder:
    def __init__(self, network_ids: str, inspect: dict | None = None) -> None:
        self.commands: list[PlannedCommand] = []
        self.network_ids = network_ids
        self.inspect = inspect

    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        if command.phase == "down.network-list":
            return CommandResult(0, self.network_ids, "")
        if command.phase == "down.network-inspect":
            return CommandResult(0, json.dumps(self.inspect), "")
        return CommandResult(0, "", "")


def _write_creation_receipt(tmp_path: Path, farm, plan, network_id: str) -> None:
    root = Path(farm.data["hub"]["results_root"]) / "results" / plan["run_id"]
    root.mkdir(parents=True)
    (root / "lifecycle.json").write_text(
        json.dumps(
            {
                "network_shaping": {
                    "bindings": [
                        {
                            "instance": "F1",
                            "bridge": {"network_id": network_id},
                        }
                    ],
                    "schema": "icefarm-netem-receipt-v1",
                    "status": "APPLIED",
                }
            }
        ),
        encoding="utf-8",
    )


def _network_document(binding, run_id: str, network_id: str) -> dict:
    return {
        "Id": network_id,
        "Name": binding.bridge,
        "Driver": "bridge",
        "Labels": {
            "icefarm.run": run_id,
            "icefarm.instance": binding.instance,
            "icefarm.netem": "icefarm-netem-plan-v1",
        },
    }


def test_netem_teardown_refuses_unlabelled_bridge_name_collision(tmp_path: Path) -> None:
    farm, _scenario, plan = _shaped_plan(tmp_path)
    binding = lifecycle._netem_bindings(plan)[0]
    _write_creation_receipt(tmp_path, farm, plan, "a" * 64)
    recorder = _NetworkRecorder("")
    receipts, problems = lifecycle._remove_netem_bridges(
        farm, plan, (binding,), recorder, CommandFactory(), 30
    )
    assert not problems
    assert receipts[0]["status"] == "SKIPPED_NO_AUTHENTICATED_NETWORK"
    assert not any(command.phase == "down.network-remove" for command in recorder.commands)


def test_netem_teardown_refuses_foreign_container_name_collision(tmp_path: Path) -> None:
    _farm, _scenario, plan = _shaped_plan(tmp_path)
    binding = lifecycle._netem_bindings(plan)[0]
    recorder = _NetworkRecorder("")
    receipts, problems = lifecycle._remove_netem_bridges(
        _farm, plan, (binding,), recorder, CommandFactory(), 30
    )
    assert not problems
    assert receipts[0]["status"] == "SKIPPED_NO_AUTHENTICATED_NETWORK"
    assert not any("tc" in argument for command in recorder.commands for argument in command.argv)


def test_netem_teardown_refuses_owned_name_replaced_by_different_id(tmp_path: Path) -> None:
    farm, _scenario, plan = _shaped_plan(tmp_path)
    binding = lifecycle._netem_bindings(plan)[0]
    old_id = "a" * 64
    new_id = "b" * 64
    _write_creation_receipt(tmp_path, farm, plan, old_id)
    recorder = _NetworkRecorder(new_id, _network_document(binding, plan["run_id"], new_id))
    receipts, problems = lifecycle._remove_netem_bridges(
        farm, plan, (binding,), recorder, CommandFactory(), 30
    )
    assert not receipts
    assert any("replacement-id" in problem for problem in problems)
    assert not any(command.phase == "down.network-remove" for command in recorder.commands)


def test_netem_teardown_removes_exact_owned_network_id(tmp_path: Path) -> None:
    farm, _scenario, plan = _shaped_plan(tmp_path)
    binding = lifecycle._netem_bindings(plan)[0]
    network_id = "a" * 64
    _write_creation_receipt(tmp_path, farm, plan, network_id)
    recorder = _NetworkRecorder(
        network_id, _network_document(binding, plan["run_id"], network_id)
    )
    receipts, problems = lifecycle._remove_netem_bridges(
        farm, plan, (binding,), recorder, CommandFactory(), 30
    )
    assert not problems
    assert receipts[0]["status"] == "REMOVED"
    remove = next(command for command in recorder.commands if command.phase == "down.network-remove")
    assert network_id in remove.argv
    assert binding.bridge not in remove.argv


def test_network_inspect_requires_exact_authority() -> None:
    binding = type(
        "Binding",
        (),
        {"bridge": "icefarm-run-F1-netem", "instance": "F1"},
    )()
    with pytest.raises(NetemPlanError, match="labels"):
        validate_network_inspect(
            binding,
            "run",
            "a" * 64,
            {"Id": "a" * 64, "Name": binding.bridge, "Driver": "bridge"},
        )


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
