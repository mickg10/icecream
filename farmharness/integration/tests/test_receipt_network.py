from __future__ import annotations

import hashlib
import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from farmharness.integration import farmtest, lifecycle
from farmharness.integration import workload
from farmharness.integration.images import CommandFactory, RecordingTransport
from farmharness.integration.receipt_network import (
    RECEIPT_NETWORK_SCHEMA,
    ReceiptNetworkError,
    resolve_client_networks,
    validate_client_inspect,
    validate_network_inspect,
)
from farmharness.integration.remote import CommandResult
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.tests import farm_fixture
from farmharness.integration.tests.test_lifecycle import ScriptedLifecycle, _farm_scenario_plan


INTEGRATION = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize(
    "clients,topology",
    [
        (["C1"], [{"name": "C1", "role": "C", "host": "h"}]),
        (
            ["C1", "C2"],
            [
                {"name": "C1", "role": "C", "host": "h"},
                {"name": "C2", "role": "C", "host": "h"},
            ],
        ),
    ],
)
def test_selected_receipt_clients_get_distinct_bridges(clients, topology) -> None:
    bindings = resolve_client_networks(
        {"workload": {"driver": "p51-receipt-window", "clients": clients}},
        {"instances": topology},
        "run-123",
    )
    assert [binding.instance for binding in bindings] == clients
    assert len({binding.bridge for binding in bindings}) == len(clients)
    assert len({binding.container for binding in bindings}) == len(clients)


@pytest.mark.parametrize("clients", [["C1", "C1"], ["C1", {}], ["missing"]])
def test_receipt_network_selection_rejects_malformed_or_unknown_clients(clients) -> None:
    with pytest.raises(ReceiptNetworkError):
        resolve_client_networks(
            {"workload": {"driver": "p51-receipt-window", "clients": clients}},
            {"instances": [{"name": "C1", "role": "C", "host": "h"}]},
            "run-123",
        )


def test_receipt_network_and_client_inspects_reject_host_or_shared_network() -> None:
    binding = resolve_client_networks(
        {"workload": {"driver": "p51-receipt-window", "clients": ["C1"]}},
        {"instances": [{"name": "C1", "role": "C", "host": "h"}]},
        "run-123",
    )[0]
    run, scenario, topology, network_id = "run-123", "a" * 64, "b" * 64, "c" * 64
    labels = {
        "icefarm.run": run,
        "icefarm.instance": "C1",
        "icefarm.receipt-network": RECEIPT_NETWORK_SCHEMA,
        "icefarm.scenario": scenario,
        "icefarm.topology": topology,
    }
    network = {"Id": network_id, "Name": binding.bridge, "Driver": "bridge", "Labels": labels}
    assert validate_network_inspect(binding, run, scenario, topology, network_id, network)["id"] == network_id

    valid_client = {
        "Name": "/" + binding.container,
        "Config": {"Labels": {"icefarm.run": run, "icefarm.instance": "C1"}},
        "State": {"Running": True},
        "HostConfig": {"NetworkMode": binding.bridge},
        "NetworkSettings": {"Networks": {
            binding.bridge: {"NetworkID": network_id, "IPAddress": "172.28.0.2"}
        }},
    }
    assert validate_client_inspect(binding, run, network_id, valid_client)["bridge"] == binding.bridge
    for mutate in (
        lambda value: value.update(Name=None),
        lambda value: value["HostConfig"].update(NetworkMode="host"),
        lambda value: value["NetworkSettings"]["Networks"].update(host={}),
        lambda value: value["NetworkSettings"]["Networks"][binding.bridge].update(NetworkID="d" * 64),
    ):
        import copy

        changed = copy.deepcopy(valid_client)
        mutate(changed)
        with pytest.raises(ReceiptNetworkError):
            validate_client_inspect(binding, run, network_id, changed)


def _p51_plan(tmp_path: Path):
    farm, scenario, _ = _farm_scenario_plan(tmp_path)
    scenario.data["workload"]["driver"] = "p51-receipt-window"
    scenario.data["workload"]["receipt_gate"] = {
        "binary": "/bin/true",
        "binary_sha256": hashlib.sha256(Path("/bin/true").read_bytes()).hexdigest(),
        "expected_commits": 1,
        "negotiated_window": 1,
        "expect_observed": True,
    }
    scenario.data["workload"]["clients"] = ["C1"]
    return farm, scenario, farmtest.build_plan(farm, scenario, run_id="receipt-net-test")


def test_receipt_plan_uses_private_client_network_before_start_and_canary(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _p51_plan(tmp_path)
    binding = plan["receipt_client_networks"]["bindings"][0]
    assert plan["receipt_client_networks"]["client_mode"] == "no-remote"
    assert plan["receipt_client_networks"]["schema"] == RECEIPT_NETWORK_SCHEMA
    start_c = next(item for item in plan["commands"] if item["phase"] == "up.start-c")
    assert start_c["argv"][start_c["argv"].index("--network") + 1] == binding["bridge"]
    assert not any(
        "iptables" in item["argv"] and item["host"] != binding["host"]
        for item in plan["commands"]
    )
    worker_start = next(
        item for item in plan["commands"]
        if item["phase"] == "up.start-f" and item["instance"] == "F1"
    )
    assert worker_start["argv"][worker_start["argv"].index("--network") + 1] == "host"
    create = [item for item in plan["commands"] if item["phase"] == "up.receipt-network-create"]
    assert len(create) == 1
    assert plan["commands"].index(create[0]) < next(
        index for index, item in enumerate(plan["commands"])
        if item["phase"] == "up.start-s"
    )


def test_bring_up_persists_bridge_intent_and_each_id_before_role_start(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _p51_plan(tmp_path)
    scripted = ScriptedLifecycle(farm)
    receipt = lifecycle.bring_up(
        farm,
        scenario,
        plan,
        recorder=RecordingTransport(scripted),
        probe_bytes=0,
        sync_corpora=False,
    )
    assert receipt["status"] == "UP"
    network_receipt = receipt["receipt_client_networks"]
    assert network_receipt["status"] == "VERIFIED"
    assert network_receipt["complete"] is True
    assert len(network_receipt["bindings"]) == 1
    assert network_receipt["bindings"][0]["bridge"]["network_id"] in scripted.receipt_networks


@pytest.mark.parametrize(
    "topology,clients",
    [("C1F2", ["C1"]), ("C2F1", ["C1", "C2"])],
)
def test_multilink_client_network_isolation_is_per_c_not_per_f(topology, clients) -> None:
    instances = [
        {"name": name, "role": "C", "host": "h"} for name in clients
    ]
    worker_names = ["F1", "F2"] if topology == "C1F2" else ["F1"]
    instances.extend(
        {"name": name, "role": "F", "host": "h"} for name in worker_names
    )
    bindings = resolve_client_networks(
        {"workload": {"driver": "p51-receipt-window", "clients": clients}},
        {"instances": instances},
        "run-topology",
    )
    assert {binding.instance for binding in bindings} == set(clients)
    assert len({binding.bridge for binding in bindings}) == len(clients)
    assert all("iptables" not in " ".join(binding.as_dict().values()) for binding in bindings)


def test_partial_network_creation_preserves_success_and_failed_cleanup_is_narrow(
    tmp_path: Path,
) -> None:
    farm, _scenario, plan = _p51_plan(tmp_path)
    first = lifecycle._receipt_client_networks(plan)[0]
    second = type(first)("C2", first.host, "ifc-other", "icefarm-receipt-net-test-C2")
    bindings = (first, second)
    first_id = "c" * 64
    commands = [
        SimpleNamespace(argv=("docker", "network", "create", first.bridge), instance="C1"),
        SimpleNamespace(argv=("docker", "network", "create", second.bridge), instance="C2"),
    ]
    receipt = lifecycle._receipt_network_created_receipt(
        plan,
        bindings,
        [CommandResult(0, first_id + "\n", ""), CommandResult(1, "", "failed")],
        commands,
    )
    assert receipt["complete"] is False
    assert [item["bridge"]["network_id"] for item in receipt["bindings"]] == [first_id]

    lifecycle_path = lifecycle.bundle_root(farm, plan["run_id"]) / "lifecycle.json"
    lifecycle_path.parent.mkdir(parents=True, exist_ok=True)
    lifecycle_path.write_text(
        json.dumps({"receipt_client_networks": {
            **receipt,
            "bindings": [{
                **receipt["bindings"][0],
                "bridge": {"network_id": first_id},
            }],
        }}),
        encoding="utf-8",
    )

    class Recorder:
        commands = []
        attached = True

        def invoke(self, command):
            self.commands.append(command)
            if command.phase == "down.receipt-network-list":
                return CommandResult(
                    0,
                    first_id + "\n"
                    if self.attached and command.instance == "C1"
                    else "",
                    "",
                )
            if command.phase == "down.receipt-network-inspect":
                return CommandResult(0, json.dumps({
                    "Id": first_id,
                    "Name": first.bridge,
                    "Driver": "bridge",
                    "Labels": {
                        "icefarm.run": plan["run_id"],
                        "icefarm.instance": first.instance,
                        "icefarm.receipt-network": RECEIPT_NETWORK_SCHEMA,
                        "icefarm.scenario": plan["scenario_digest"],
                        "icefarm.topology": plan["topology_digest"],
                    },
                }), "")
            if command.phase == "down.receipt-network-remove":
                return CommandResult(1, "", "network has active endpoint")
            raise AssertionError(command.phase)

    recorder = Recorder()
    removed, problems = lifecycle._remove_receipt_client_bridges(
        farm, plan, bindings, recorder, CommandFactory(), 5
    )
    assert problems == ["C1:network-remove:rc=1"]
    assert removed[0]["network_id"] == first_id
    remove = next(item for item in recorder.commands if item.phase == "down.receipt-network-remove")
    assert remove.argv[-1] == first_id
    assert not any(item.argv[:2] == ("container", "rm") for item in recorder.commands)


def test_bad_create_output_does_not_discard_other_created_network_ids(tmp_path: Path) -> None:
    farm, _scenario, plan = _p51_plan(tmp_path)
    first = lifecycle._receipt_client_networks(plan)[0]
    second = type(first)("C2", first.host, "ifc-other", "icefarm-receipt-net-test-C2")
    bindings = (first, second)
    commands = [
        SimpleNamespace(argv=("network", "create", first.bridge), instance="C1"),
        SimpleNamespace(argv=("network", "create", second.bridge), instance="C2"),
    ]
    receipt = lifecycle._receipt_network_created_receipt(
        plan,
        bindings,
        [CommandResult(0, "c" * 64 + "\n", ""), CommandResult(0, "not-an-id\n", "")],
        commands,
    )
    assert receipt["complete"] is False
    assert receipt["creation_errors"] == ["C2:create returned no exact network id"]
    assert [row["instance"] for row in receipt["bindings"]] == ["C1"]


def test_unreceipted_live_bridge_is_reported_but_never_removed(tmp_path: Path) -> None:
    farm, _scenario, plan = _p51_plan(tmp_path)
    binding = lifecycle._receipt_client_networks(plan)[0]
    lifecycle_path = lifecycle.bundle_root(farm, plan["run_id"]) / "lifecycle.json"
    lifecycle_path.parent.mkdir(parents=True, exist_ok=True)
    lifecycle_path.write_text(
        json.dumps({"receipt_client_networks": {
            "schema": RECEIPT_NETWORK_SCHEMA,
            "status": "CREATING",
            "scenario_digest": plan["scenario_digest"],
            "topology_digest": plan["topology_digest"],
            "bindings": [{
                "instance": binding.instance,
                "bridge": {"network_id": None, "returncode": None},
                "request": binding.as_dict(),
            }],
        }}),
        encoding="utf-8",
    )

    class Recorder:
        commands = []

        def invoke(self, command):
            self.commands.append(command)
            return CommandResult(0, "e" * 64 + "\n", "")

    recorder = Recorder()
    _removed, problems = lifecycle._remove_receipt_client_bridges(
        farm, plan, (binding,), recorder, CommandFactory(), 5
    )
    assert problems == ["C1:network-list:unreceipted-network-present"]
    assert not any(command.phase == "down.receipt-network-remove" for command in recorder.commands)


def test_repeated_down_recognizes_previously_removed_bridge(tmp_path: Path) -> None:
    farm, _scenario, plan = _p51_plan(tmp_path)
    binding = lifecycle._receipt_client_networks(plan)[0]
    network_id = "f" * 64
    lifecycle_path = lifecycle.bundle_root(farm, plan["run_id"]) / "lifecycle.json"
    lifecycle_path.parent.mkdir(parents=True, exist_ok=True)
    lifecycle_path.write_text(json.dumps({"receipt_client_networks": {
        "schema": RECEIPT_NETWORK_SCHEMA,
        "status": "CREATED",
        "scenario_digest": plan["scenario_digest"],
        "topology_digest": plan["topology_digest"],
        "bindings": [{"instance": binding.instance, "bridge": {"network_id": network_id}}],
    }}), encoding="utf-8")

    class Recorder:
        commands = []

        def invoke(self, command):
            self.commands.append(command)
            assert command.phase == "down.receipt-network-list"
            return CommandResult(0, "", "")

    receipts, problems = lifecycle._remove_receipt_client_bridges(
        farm, plan, (binding,), Recorder(), CommandFactory(), 5
    )
    assert problems == []
    assert receipts == [{"instance": "C1", "status": "ALREADY_REMOVED"}]


def test_legacy_host_network_p51_plan_is_refused_before_preflight(tmp_path: Path) -> None:
    farm, scenario, plan = _p51_plan(tmp_path)
    plan.pop("receipt_client_networks")
    transport = RecordingTransport()
    with pytest.raises(lifecycle.LifecycleError, match="plan is absent"):
        lifecycle.bring_up(
            farm, scenario, plan, recorder=transport, probe_bytes=0, sync_corpora=False
        )
    assert transport.commands == []
    farm, scenario, plan = _p51_plan(tmp_path / "wrong-mode")
    start = next(
        item for item in plan["commands"]
        if item["phase"] == "up.start-c" and item["instance"] == "C1"
    )
    start["argv"][start["argv"].index("--network") + 1] = "host"
    transport = RecordingTransport()
    with pytest.raises(lifecycle.LifecycleError, match="private bridge"):
        lifecycle.bring_up(
            farm, scenario, plan, recorder=transport, probe_bytes=0, sync_corpora=False
        )
    assert transport.commands == []


def test_p51_plan_rejects_changed_no_remote_entrypoint_before_preflight(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _p51_plan(tmp_path)
    plan["receipt_client_networks"]["entrypoint_sha256"] = "0" * 64
    transport = RecordingTransport()
    with pytest.raises(lifecycle.LifecycleError, match="entrypoint differs"):
        lifecycle.bring_up(
            farm, scenario, plan, recorder=transport, probe_bytes=0, sync_corpora=False
        )
    assert transport.commands == []


def test_workload_rechecks_live_container_network_before_any_gate_exec(
    tmp_path: Path,
) -> None:
    farm, scenario, plan = _p51_plan(tmp_path)
    binding = lifecycle._receipt_client_networks(plan)[0]
    network_id = "d" * 64
    labels = {
        "icefarm.run": plan["run_id"],
        "icefarm.instance": binding.instance,
        "icefarm.receipt-network": RECEIPT_NETWORK_SCHEMA,
        "icefarm.scenario": plan["scenario_digest"],
        "icefarm.topology": plan["topology_digest"],
    }
    lifecycle_path = lifecycle.bundle_root(farm, plan["run_id"]) / "lifecycle.json"
    lifecycle_path.parent.mkdir(parents=True, exist_ok=True)
    lifecycle_path.write_text(json.dumps({
        "status": "UP",
        "run_id": plan["run_id"],
        "scenario_digest": scenario.digest,
        "topology_digest": plan["topology_digest"],
        "receipt_client_networks": {
            "schema": RECEIPT_NETWORK_SCHEMA,
            "status": "VERIFIED",
            "complete": True,
            "scenario_digest": plan["scenario_digest"],
            "topology_digest": plan["topology_digest"],
            "bindings": [{
                "instance": binding.instance,
                "bridge": {"network_id": network_id},
            }],
        }
    }), encoding="utf-8")

    class Recorder:
        commands = []

        def invoke(self, command):
            self.commands.append(command)
            if command.phase == "readiness.receipt-network-inspect":
                return CommandResult(0, json.dumps({
                    "Id": network_id,
                    "Name": binding.bridge,
                    "Driver": "bridge",
                    "Labels": labels,
                }), "")
            if command.phase == "readiness.receipt-client-network":
                # Simulate the C container having been switched back to host
                # mode after UP but before the receipt gate starts.
                return CommandResult(0, json.dumps({
                    "Name": "/" + binding.container,
                    "Config": {"Labels": {
                        "icefarm.run": plan["run_id"],
                        "icefarm.instance": binding.instance,
                    }},
                    "State": {"Running": True},
                    "HostConfig": {"NetworkMode": "host"},
                    "NetworkSettings": {"Networks": {"host": {}}},
                }), "")
            raise AssertionError(f"unexpected pre-gate command {command.phase}")

    recorder = Recorder()
    transport = RecordingTransport(recorder)
    with pytest.raises(workload.WorkloadError, match="changed since UP"):
        workload.run_workload(
            farm, scenario, plan, recorder=transport, require_up=True
        )
    assert [command.phase for command in recorder.commands] == [
        "readiness.receipt-network-inspect",
        "readiness.receipt-client-network",
    ]
    assert not any(command.phase.startswith("run.p51-receipt-window")
                   for command in recorder.commands)


def test_p51_gate_iptables_command_is_executed_inside_exact_c_container(
    tmp_path: Path,
) -> None:
    farm, _scenario, plan = _p51_plan(tmp_path)
    client = next(item for item in plan["topology"]["instances"] if item["role"] == "C")
    class Delegate:
        def invoke(self, _command):
            return CommandResult(0, "", "")

    transport = RecordingTransport(Delegate())
    workload._p51_gate_call(
        farm,
        plan,
        client,
        CommandFactory(),
        transport,
        "test-isolated-rule",
        ("iptables", "-t", "nat", "-A", "OUTPUT", "-p", "tcp"),
    )
    command = transport.commands[-1]
    assert command.argv[command.argv.index("exec") + 3] == "icefarm-receipt-net-test-C1"
    assert command.argv[-7:] == ("iptables", "-t", "nat", "-A", "OUTPUT", "-p", "tcp")
    assert command.phase.startswith("run.p51-receipt-window.")
