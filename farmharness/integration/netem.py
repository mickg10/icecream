"""Pure planning and evidence helpers for isolated container netem.

The farm runner normally uses host networking.  A shaped worker must never
use that mode: a per-run Docker bridge is created on the worker's host and
the qdisc is installed through ``docker exec`` in the worker namespace.  This
module contains no transport or subprocess calls; callers retain the exact
argv in their immutable plan/receipts.
"""

from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass
from typing import Any, Mapping

try:
    from .remote import decode_ssh_payload
except ImportError:  # Direct execution from this directory.
    from remote import decode_ssh_payload


NETEM_PLAN_SCHEMA = "icefarm-netem-plan-v1"
NETEM_RECEIPT_SCHEMA = "icefarm-netem-receipt-v1"
SUPPORTED_RATE = "100mbit"
SHAPING_DIRECTION = "F-egress"
_RATE_RE = re.compile(r"^(?P<amount>[1-9][0-9]*)(?P<unit>kbit|mbit|gbit)$")
_QDISC_RE = re.compile(r"\bqdisc\s+netem\b", re.IGNORECASE)


class NetemPlanError(ValueError):
    """A shaping request cannot be safely represented by the bridge seam."""


@dataclass(frozen=True)
class NetemBinding:
    instance: str
    role: str
    host: str
    bridge: str
    rate: str
    delay_ms: int
    host_port: int
    container_port: int
    container: str
    direction: str = SHAPING_DIRECTION

    def as_dict(self) -> dict[str, Any]:
        return {
            "bridge": self.bridge,
            "container": self.container,
            "container_port": self.container_port,
            "delay_ms": self.delay_ms,
            "direction": self.direction,
            "host": self.host,
            "host_port": self.host_port,
            "instance": self.instance,
            "rate": self.rate,
            "role": self.role,
        }


def _safe_run_id(run_id: str) -> None:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,79}", run_id):
        raise NetemPlanError("run id is unsafe for a bridge name")


def resolve_bindings(
    farm: Any,
    scenario: Mapping[str, Any],
    topology: Mapping[str, Any],
    ports: Mapping[str, Any],
    run_id: str,
) -> tuple[NetemBinding, ...]:
    """Resolve the intentionally narrow 100-Mbit shaped-worker contract.

    The first production seam is deliberately one shaped worker per run.  A
    later multi-link implementation can widen this only with a new evidence
    schema; silently applying multiple qdiscs would make S80 comparisons
    ambiguous.
    """

    requests = scenario.get("network", {}).get("shaping", [])
    if not requests:
        return ()
    if not isinstance(requests, list) or len(requests) != 1:
        raise NetemPlanError("exactly one isolated 100mbit shaping request is supported")
    request = requests[0]
    if not isinstance(request, Mapping):
        raise NetemPlanError("shaping request is not an object")
    instance_name = request.get("instance")
    rate = request.get("rate")
    delay_ms = request.get("delay_ms")
    if not isinstance(instance_name, str):
        raise NetemPlanError("shaping instance is not a name")
    if rate != SUPPORTED_RATE:
        raise NetemPlanError("only the isolated 100mbit shaping seam is supported")
    if not isinstance(delay_ms, int) or isinstance(delay_ms, bool) or delay_ms < 0:
        raise NetemPlanError("shaping delay is not a bounded integer")
    if _RATE_RE.fullmatch(rate) is None:
        raise NetemPlanError("shaping rate is malformed")
    instances = topology.get("instances")
    if not isinstance(instances, list):
        raise NetemPlanError("topology instances are absent")
    instance = next((item for item in instances if item.get("name") == instance_name), None)
    if not isinstance(instance, Mapping):
        raise NetemPlanError(f"shaping names unknown instance {instance_name!r}")
    if instance.get("role") != "F":
        raise NetemPlanError("isolated shaping may target only an F instance")
    host_name = instance.get("host")
    if not isinstance(host_name, str):
        raise NetemPlanError("shaped worker has no host")
    try:
        host = farm.hosts[host_name]
    except (AttributeError, KeyError) as exc:
        raise NetemPlanError(f"shaped worker host {host_name!r} is absent") from exc
    if host.get("netem") is not True:
        raise NetemPlanError(f"shaped worker host {host_name!r} lacks netem authority")
    client_names = scenario.get("workload", {}).get("clients")
    clients = [
        item
        for item in instances
        if isinstance(item, Mapping)
        and item.get("role") == "C"
        and isinstance(client_names, list)
        and item.get("name") in client_names
    ]
    if (
        not isinstance(client_names, list)
        or len(client_names) != 1
        or len(clients) != 1
        or clients[0].get("host") != host_name
    ):
        raise NetemPlanError(
            "a shaped bridge worker requires its one workload client on the same host"
        )
    instance_ports = ports.get("instances")
    port = instance_ports.get(instance_name) if isinstance(instance_ports, Mapping) else None
    if type(port) is not int or not (1 <= port <= 65535):
        raise NetemPlanError("shaped worker has no authenticated published port")
    _safe_run_id(run_id)
    bridge = f"icefarm-{run_id}-{instance_name}-netem"
    container = f"icefarm-{run_id}-{instance_name}"
    return (
        NetemBinding(
            instance=instance_name,
            role="F",
            host=host_name,
            bridge=bridge,
            rate=rate,
            delay_ms=delay_ms,
            host_port=port,
            container_port=port,
            container=container,
        ),
    )


def create_args(
    binding: NetemBinding,
    run_id: str,
    *,
    scenario_digest: str | None = None,
    topology_digest: str | None = None,
) -> tuple[str, ...]:
    _safe_run_id(run_id)
    args = [
        "network",
        "create",
        "--driver",
        "bridge",
        "--label",
        f"icefarm.run={run_id}",
        "--label",
        f"icefarm.instance={binding.instance}",
        "--label",
        f"icefarm.netem={NETEM_PLAN_SCHEMA}",
    ]
    if (scenario_digest is None) != (topology_digest is None):
        raise NetemPlanError("netem authority labels must include both digests")
    if scenario_digest is not None and topology_digest is not None:
        for name, digest in (
            ("scenario", scenario_digest),
            ("topology", topology_digest),
        ):
            if re.fullmatch(r"[0-9a-f]{64}", digest) is None:
                raise NetemPlanError(f"netem {name} digest is not authenticated")
            args.extend(("--label", f"icefarm.{name}={digest}"))
    args.append(binding.bridge)
    return tuple(args)


def apply_args(binding: NetemBinding) -> tuple[str, ...]:
    delay = f"{binding.delay_ms}ms"
    return (
        "exec",
        "--user",
        "0",
        binding.container,
        "tc",
        "qdisc",
        "replace",
        "dev",
        "eth0",
        "root",
        "netem",
        "rate",
        binding.rate,
        "delay",
        delay,
    )


def observe_args(binding: NetemBinding) -> tuple[str, ...]:
    return (
        "exec",
        "--user",
        "0",
        binding.container,
        "tc",
        "-s",
        "qdisc",
        "show",
        "dev",
        "eth0",
    )


def network_list_args(binding: NetemBinding, run_id: str) -> tuple[str, ...]:
    _safe_run_id(run_id)
    return (
        "network",
        "ls",
        "--no-trunc",
        "--filter",
        f"label=icefarm.run={run_id}",
        "--filter",
        f"label=icefarm.instance={binding.instance}",
        "--filter",
        f"label=icefarm.netem={NETEM_PLAN_SCHEMA}",
        "--format",
        "{{.ID}}",
    )


def network_inspect_args(network_id: str) -> tuple[str, ...]:
    if re.fullmatch(r"[0-9a-f]{64}", network_id) is None:
        raise NetemPlanError("netem network id is not authenticated")
    return ("network", "inspect", "--format", "{{json .}}", network_id)


def network_remove_args(network_id: str) -> tuple[str, ...]:
    if re.fullmatch(r"[0-9a-f]{64}", network_id) is None:
        raise NetemPlanError("netem network id is not authenticated")
    return ("network", "rm", network_id)


def validate_network_inspect(
    binding: NetemBinding,
    run_id: str,
    scenario_digest: str,
    topology_digest: str,
    network_id: str,
    value: object,
) -> dict[str, Any]:
    """Authenticate a freshly inspected private bridge before removing it."""

    _safe_run_id(run_id)
    if any(
        re.fullmatch(r"[0-9a-f]{64}", digest) is None
        for digest in (scenario_digest, topology_digest)
    ):
        raise NetemPlanError("netem authority digest is not authenticated")
    if re.fullmatch(r"[0-9a-f]{64}", network_id) is None:
        raise NetemPlanError("netem network id is not authenticated")
    if not isinstance(value, Mapping):
        raise NetemPlanError("netem network inspect is not an object")
    if value.get("Id") != network_id or value.get("Name") != binding.bridge:
        raise NetemPlanError("netem network identity differs from the plan")
    if value.get("Driver") != "bridge":
        raise NetemPlanError("netem network driver is not bridge")
    labels = value.get("Labels")
    expected = {
        "icefarm.run": run_id,
        "icefarm.instance": binding.instance,
        "icefarm.netem": NETEM_PLAN_SCHEMA,
        "icefarm.scenario": scenario_digest,
        "icefarm.topology": topology_digest,
    }
    if not isinstance(labels, Mapping) or any(labels.get(k) != v for k, v in expected.items()):
        raise NetemPlanError("netem network labels are not authenticated")
    return {
        "driver": "bridge",
        "id": network_id,
        "instance": binding.instance,
        "labels": expected,
        "name": binding.bridge,
    }


def validate_qdisc(binding: NetemBinding, output: str) -> dict[str, Any]:
    """Return an authenticated qdisc witness or fail closed."""

    if not isinstance(output, str) or not output.strip():
        raise NetemPlanError("tc qdisc witness is empty")
    if _QDISC_RE.search(output) is None:
        raise NetemPlanError("tc qdisc witness is not netem")
    rate = re.search(r"\brate\s+([0-9]+)(kbit|Mbit|Gbit)\b", output, re.IGNORECASE)
    if rate is None or rate.group(1) != "100" or rate.group(2).lower() != "mbit":
        raise NetemPlanError("tc qdisc witness has the wrong rate")
    delay = re.search(r"\bdelay\s+([0-9]+)(?:\.[0-9]+)?ms\b", output, re.IGNORECASE)
    if delay is None or int(delay.group(1)) != binding.delay_ms:
        raise NetemPlanError("tc qdisc witness has the wrong delay")
    return {
        "delay_ms": binding.delay_ms,
        "rate": binding.rate,
        "text": output,
    }


def validate_plan(value: object) -> tuple[dict[str, Any], ...]:
    if not isinstance(value, Mapping) or value.get("schema") != NETEM_PLAN_SCHEMA:
        raise NetemPlanError("netem plan schema is invalid")
    bindings = value.get("bindings")
    if not isinstance(bindings, list):
        raise NetemPlanError("netem plan bindings are invalid")
    result: list[dict[str, Any]] = []
    for binding in bindings:
        if not isinstance(binding, Mapping) or set(binding) != {
            "bridge", "container", "container_port", "delay_ms", "direction",
            "host", "host_port", "instance", "rate", "role",
        }:
            raise NetemPlanError("netem plan binding fields are invalid")
        if (
            binding["role"] != "F"
            or binding["rate"] != SUPPORTED_RATE
            or binding["direction"] != SHAPING_DIRECTION
        ):
            raise NetemPlanError("netem plan binding is outside the supported seam")
        for field in ("instance", "host", "bridge", "container"):
            if not isinstance(binding[field], str) or not binding[field]:
                raise NetemPlanError(f"netem plan {field} is invalid")
        for field in ("host_port", "container_port", "delay_ms"):
            value = binding[field]
            if type(value) is not int or value < 0:
                raise NetemPlanError(f"netem plan {field} is invalid")
        if not 1 <= binding["host_port"] <= 65535 or not 1 <= binding["container_port"] <= 65535:
            raise NetemPlanError("netem plan port is out of range")
        if binding["delay_ms"] > 60000:
            raise NetemPlanError("netem plan delay is out of range")
        result.append(dict(binding))
    if len(result) > 1:
        raise NetemPlanError("netem plan has multiple bindings")
    return tuple(result)


def _binding_from_plan(
    scenario: Mapping[str, Any], plan: Mapping[str, Any]
) -> NetemBinding:
    bindings = validate_plan(plan.get("network_shaping"))
    requests = scenario.get("network", {}).get("shaping")
    if (
        not isinstance(requests, list)
        or len(requests) != 1
        or not isinstance(requests[0], Mapping)
        or set(requests[0]) != {"delay_ms", "instance", "rate"}
    ):
        raise NetemPlanError("shaped evidence requires one exact scenario request")
    if len(bindings) != 1:
        raise NetemPlanError("shaped evidence requires one exact plan binding")
    value = bindings[0]
    request = requests[0]
    if any(request.get(field) != value[field] for field in request):
        raise NetemPlanError("scenario shaping request differs from the plan binding")

    topology = plan.get("topology", {}).get("instances")
    if not isinstance(topology, list):
        raise NetemPlanError("shaped plan has no topology instances")
    targets = [
        item
        for item in topology
        if isinstance(item, Mapping) and item.get("name") == value["instance"]
    ]
    if (
        len(targets) != 1
        or targets[0].get("role") != "F"
        or targets[0].get("host") != value["host"]
    ):
        raise NetemPlanError("shaped plan target is not the bound worker")
    client_names = scenario.get("workload", {}).get("clients")
    clients = [
        item
        for item in topology
        if isinstance(item, Mapping)
        and item.get("role") == "C"
        and isinstance(client_names, list)
        and item.get("name") in client_names
    ]
    if (
        not isinstance(client_names, list)
        or len(client_names) != 1
        or len(clients) != 1
        or clients[0].get("host") != value["host"]
    ):
        raise NetemPlanError("shaped plan client cannot route the private bridge worker")
    ports = plan.get("ports", {}).get("instances")
    if (
        not isinstance(ports, Mapping)
        or ports.get(value["instance"]) != value["container_port"]
        or value["host_port"] != value["container_port"]
    ):
        raise NetemPlanError("shaped plan port differs from the binding")
    run_id = plan.get("run_id")
    if not isinstance(run_id, str):
        raise NetemPlanError("shaped plan has no run id")
    _safe_run_id(run_id)
    if (
        value["container"] != f"icefarm-{run_id}-{value['instance']}"
        or value["bridge"] != f"icefarm-{run_id}-{value['instance']}-netem"
    ):
        raise NetemPlanError("shaped plan object identity differs from the run")
    return NetemBinding(**value)


def _planned_command(
    plan: Mapping[str, Any], binding: NetemBinding, phase: str
) -> Mapping[str, Any]:
    commands = plan.get("commands")
    if not isinstance(commands, list):
        raise NetemPlanError("shaped plan commands are absent")
    matches = [
        item
        for item in commands
        if isinstance(item, Mapping)
        and item.get("phase") == phase
        and item.get("instance") == binding.instance
    ]
    if len(matches) != 1 or matches[0].get("host") != binding.host:
        raise NetemPlanError(f"shaped plan has no unique {phase} command")
    command = matches[0]
    argv = command.get("argv")
    if not isinstance(argv, list) or not argv or any(not isinstance(v, str) for v in argv):
        raise NetemPlanError(f"shaped plan {phase} argv is invalid")
    return command


def _planned_argv(
    plan: Mapping[str, Any], binding: NetemBinding, phase: str
) -> list[str]:
    return list(_planned_command(plan, binding, phase)["argv"])


def _planned_docker_args(
    plan: Mapping[str, Any], binding: NetemBinding, phase: str
) -> tuple[str, ...]:
    command = _planned_command(plan, binding, phase)
    argv = tuple(command["argv"])
    transport = command.get("transport")
    if transport == "docker-context":
        if len(argv) < 4 or argv[:2] != ("docker", "--context") or not argv[2]:
            raise NetemPlanError(f"shaped plan {phase} docker wrapper is invalid")
        return argv[3:]
    if transport == "ssh-docker":
        try:
            decoded = decode_ssh_payload(argv)
        except ValueError as exc:
            raise NetemPlanError(
                f"shaped plan {phase} SSH wrapper is invalid"
            ) from exc
        if len(decoded) < 2 or decoded[0] != "docker":
            raise NetemPlanError(f"shaped plan {phase} is not a Docker command")
        return decoded[1:]
    raise NetemPlanError(f"shaped plan {phase} transport is invalid")


def validate_receipt(
    scenario: Mapping[str, Any], plan: Mapping[str, Any], value: object
) -> dict[str, Any]:
    """Validate and normalize the immutable applied-qdisc evidence."""

    binding = _binding_from_plan(scenario, plan)
    if (
        not isinstance(value, Mapping)
        or set(value) != {"bindings", "schema", "status"}
        or value.get("schema") != NETEM_RECEIPT_SCHEMA
        or value.get("status") != "APPLIED"
    ):
        raise NetemPlanError("netem receipt envelope is invalid")
    records = value.get("bindings")
    if not isinstance(records, list) or len(records) != 1:
        raise NetemPlanError("netem receipt requires one exact binding")
    record = records[0]
    if not isinstance(record, Mapping) or set(record) != {
        "application",
        "bridge",
        "container",
        "instance",
        "observation",
        "removal",
        "request",
    }:
        raise NetemPlanError("netem receipt binding fields are invalid")
    if (
        record.get("instance") != binding.instance
        or record.get("container") != binding.container
        or record.get("request") != binding.as_dict()
    ):
        raise NetemPlanError("netem receipt identity differs from the plan")

    bridge = record.get("bridge")
    application = record.get("application")
    observation = record.get("observation")
    removal = record.get("removal")
    if not isinstance(bridge, Mapping) or set(bridge) != {
        "argv",
        "network_id",
        "returncode",
    }:
        raise NetemPlanError("netem bridge receipt is invalid")
    if not isinstance(application, Mapping) or set(application) != {
        "argv",
        "returncode",
    }:
        raise NetemPlanError("netem application receipt is invalid")
    if not isinstance(observation, Mapping) or set(observation) != {
        "argv",
        "returncode",
        "sha256",
        "witness",
    }:
        raise NetemPlanError("netem observation receipt is invalid")
    if not isinstance(removal, Mapping) or set(removal) != {
        "network_inspect_argv",
        "network_list_argv",
        "network_remove_argv",
    }:
        raise NetemPlanError("netem removal receipt is invalid")
    network_id = bridge.get("network_id")
    returncodes = (
        bridge.get("returncode"),
        application.get("returncode"),
        observation.get("returncode"),
    )
    if (
        any(type(returncode) is not int or returncode != 0 for returncode in returncodes)
        or not isinstance(network_id, str)
        or re.fullmatch(r"[0-9a-f]{64}", network_id) is None
    ):
        raise NetemPlanError("netem receipt does not prove successful application")
    if bridge.get("argv") != _planned_argv(plan, binding, "up.network-create"):
        raise NetemPlanError("netem bridge argv differs from the immutable plan")
    if application.get("argv") != _planned_argv(plan, binding, "up.netem-apply"):
        raise NetemPlanError("netem application argv differs from the immutable plan")
    if observation.get("argv") != _planned_argv(plan, binding, "up.netem-observe"):
        raise NetemPlanError("netem observation argv differs from the immutable plan")
    run_id = plan["run_id"]
    semantic_commands = (
        (
            "up.network-create",
            create_args(
                binding,
                run_id,
                scenario_digest=plan["scenario_digest"],
                topology_digest=plan["topology_digest"],
            ),
        ),
        ("up.netem-apply", apply_args(binding)),
        ("up.netem-observe", observe_args(binding)),
    )
    for phase, expected in semantic_commands:
        if _planned_docker_args(plan, binding, phase) != expected:
            raise NetemPlanError(f"netem {phase} command differs from the safe seam")
    if (
        removal.get("network_list_argv")
        != list(network_list_args(binding, run_id))
        or removal.get("network_inspect_argv") != list(network_inspect_args(network_id))
        or removal.get("network_remove_argv") != list(network_remove_args(network_id))
    ):
        raise NetemPlanError("netem removal argv differs from the plan binding")
    witness = observation.get("witness")
    if not isinstance(witness, Mapping) or set(witness) != {"delay_ms", "rate", "text"}:
        raise NetemPlanError("netem qdisc witness fields are invalid")
    text = witness.get("text")
    if not isinstance(text, str):
        raise NetemPlanError("netem qdisc witness text is invalid")
    expected_witness = validate_qdisc(binding, text)
    if dict(witness) != expected_witness or observation.get("sha256") != hashlib.sha256(
        text.encode()
    ).hexdigest():
        raise NetemPlanError("netem qdisc witness digest or values are invalid")
    return {
        "bindings": [
            {
                "application": dict(application),
                "bridge": dict(bridge),
                "container": binding.container,
                "instance": binding.instance,
                "observation": {
                    **dict(observation),
                    "witness": expected_witness,
                },
                "removal": dict(removal),
                "request": binding.as_dict(),
            }
        ],
        "schema": NETEM_RECEIPT_SCHEMA,
        "status": "APPLIED",
    }
