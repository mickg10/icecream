"""Pure planning and evidence helpers for isolated container netem.

The farm runner normally uses host networking.  A shaped worker must never
use that mode: a per-run Docker bridge is created on the worker's host and
the qdisc is installed through ``docker exec`` in the worker namespace.  This
module contains no transport or subprocess calls; callers retain the exact
argv in their immutable plan/receipts.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Any, Mapping


NETEM_PLAN_SCHEMA = "icefarm-netem-plan-v1"
NETEM_RECEIPT_SCHEMA = "icefarm-netem-receipt-v1"
SUPPORTED_RATE = "100mbit"
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

    def as_dict(self) -> dict[str, Any]:
        return {
            "bridge": self.bridge,
            "container": self.container,
            "container_port": self.container_port,
            "delay_ms": self.delay_ms,
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


def create_args(binding: NetemBinding, run_id: str) -> tuple[str, ...]:
    return (
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
        binding.bridge,
    )


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


def remove_args(binding: NetemBinding) -> tuple[str, ...]:
    return (
        "exec",
        "--user",
        "0",
        binding.container,
        "tc",
        "qdisc",
        "del",
        "dev",
        "eth0",
        "root",
    )


def network_remove_args(binding: NetemBinding) -> tuple[str, ...]:
    return ("network", "rm", binding.bridge)


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
            "bridge", "container", "container_port", "delay_ms", "host",
            "host_port", "instance", "rate", "role",
        }:
            raise NetemPlanError("netem plan binding fields are invalid")
        if binding["role"] != "F" or binding["rate"] != SUPPORTED_RATE:
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
