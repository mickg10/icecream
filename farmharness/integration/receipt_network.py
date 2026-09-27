"""Per-client private bridges for P51 receipt-window gate isolation.

The receipt driver installs UID-scoped OUTPUT rules inside each C container.
When multiple host-network containers run as the same uid those rules overlap;
putting each selected C in its own bridge network gives each rule a private
network namespace without changing S/F addressing or adding host interception rules.
"""

from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass
from typing import Any, Mapping


RECEIPT_NETWORK_SCHEMA = "icefarm-p51-client-network-v1"


class ReceiptNetworkError(ValueError):
    """A receipt-window client network cannot be verified."""


def _safe_run_id(run_id: str) -> None:
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,79}", run_id) is None:
        raise ReceiptNetworkError("run id is not valid for a client bridge")


@dataclass(frozen=True)
class ReceiptClientNetwork:
    instance: str
    host: str
    bridge: str
    container: str

    def as_dict(self) -> dict[str, str]:
        return {
            "bridge": self.bridge,
            "container": self.container,
            "host": self.host,
            "instance": self.instance,
            "role": "C",
        }


def resolve_client_networks(
    scenario: Mapping[str, Any], topology: Mapping[str, Any], run_id: str
) -> tuple[ReceiptClientNetwork, ...]:
    """Create exactly one deterministic isolated bridge per receipt C."""

    workload = scenario.get("workload")
    if not isinstance(workload, Mapping) or workload.get("driver") != "p51-receipt-window":
        return ()
    _safe_run_id(run_id)
    names = workload.get("clients")
    instances = topology.get("instances")
    if not isinstance(names, list) or not names or not isinstance(instances, list):
        raise ReceiptNetworkError("receipt-window clients/topology are absent")
    if any(
        not isinstance(name, str)
        or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,79}", name) is None
        for name in names
    ):
        raise ReceiptNetworkError("receipt-window client names are malformed")
    by_name = {
        value.get("name"): value
        for value in instances
        if isinstance(value, Mapping) and isinstance(value.get("name"), str)
    }
    if len(set(names)) != len(names):
        raise ReceiptNetworkError("receipt-window client list has duplicates")
    bindings = []
    for name in names:
        item = by_name.get(name)
        if not isinstance(item, Mapping) or item.get("role") != "C":
            raise ReceiptNetworkError(f"receipt-window client {name!r} is not a C instance")
        host = item.get("host")
        if not isinstance(host, str) or not host:
            raise ReceiptNetworkError(f"receipt-window client {name!r} has no host")
        # Docker limits network names to 63 characters.  The run/name are
        # also recorded in labels, so this stable short suffix is sufficient.
        suffix = hashlib.sha256(f"{run_id}\0{name}".encode()).hexdigest()[:20]
        bridge = f"ifc-{suffix}"
        bindings.append(
            ReceiptClientNetwork(
                instance=name,
                host=host,
                bridge=bridge,
                container=f"icefarm-{run_id}-{name}",
            )
        )
    if len({item.bridge for item in bindings}) != len(bindings):
        raise ReceiptNetworkError("receipt-window bridge identity collision")
    return tuple(bindings)


def create_args(
    binding: ReceiptClientNetwork,
    run_id: str,
    scenario_digest: str,
    topology_digest: str,
) -> tuple[str, ...]:
    _safe_run_id(run_id)
    if any(re.fullmatch(r"[0-9a-f]{64}", value) is None for value in (scenario_digest, topology_digest)):
        raise ReceiptNetworkError("receipt network plan digests are not verified")
    return (
        "network", "create", "--driver", "bridge",
        "--label", f"icefarm.run={run_id}",
        "--label", f"icefarm.instance={binding.instance}",
        "--label", f"icefarm.receipt-network={RECEIPT_NETWORK_SCHEMA}",
        "--label", f"icefarm.scenario={scenario_digest}",
        "--label", f"icefarm.topology={topology_digest}",
        binding.bridge,
    )


def list_args(binding: ReceiptClientNetwork, run_id: str) -> tuple[str, ...]:
    _safe_run_id(run_id)
    return (
        "network", "ls", "--no-trunc",
        "--filter", f"label=icefarm.run={run_id}",
        "--filter", f"label=icefarm.instance={binding.instance}",
        "--filter", f"label=icefarm.receipt-network={RECEIPT_NETWORK_SCHEMA}",
        "--format", "{{.ID}}",
    )


def inspect_args(network_id: str) -> tuple[str, ...]:
    if re.fullmatch(r"[0-9a-f]{64}", network_id) is None:
        raise ReceiptNetworkError("receipt network id is not verified")
    return ("network", "inspect", "--format", "{{json .}}", network_id)


def remove_args(network_id: str) -> tuple[str, ...]:
    if re.fullmatch(r"[0-9a-f]{64}", network_id) is None:
        raise ReceiptNetworkError("receipt network id is not verified")
    return ("network", "rm", network_id)


def validate_network_inspect(
    binding: ReceiptClientNetwork,
    run_id: str,
    scenario_digest: str,
    topology_digest: str,
    network_id: str,
    value: object,
) -> dict[str, Any]:
    _safe_run_id(run_id)
    if any(re.fullmatch(r"[0-9a-f]{64}", item) is None for item in (scenario_digest, topology_digest, network_id)):
        raise ReceiptNetworkError("receipt network identity digest is malformed")
    if not isinstance(value, Mapping):
        raise ReceiptNetworkError("receipt network inspect is not an object")
    expected_labels = {
        "icefarm.run": run_id,
        "icefarm.instance": binding.instance,
        "icefarm.receipt-network": RECEIPT_NETWORK_SCHEMA,
        "icefarm.scenario": scenario_digest,
        "icefarm.topology": topology_digest,
    }
    labels = value.get("Labels")
    if (
        value.get("Id") != network_id
        or value.get("Name") != binding.bridge
        or value.get("Driver") != "bridge"
        or not isinstance(labels, Mapping)
        or any(labels.get(key) != expected for key, expected in expected_labels.items())
    ):
        raise ReceiptNetworkError("receipt network inspect differs from verified plan")
    return {"driver": "bridge", "id": network_id, "instance": binding.instance,
            "labels": expected_labels, "name": binding.bridge}


def validate_client_inspect(
    binding: ReceiptClientNetwork, run_id: str, network_id: str, value: object
) -> dict[str, str]:
    if not isinstance(value, Mapping):
        raise ReceiptNetworkError("receipt client inspect is not an object")
    name = value.get("Name")
    if not isinstance(name, str):
        raise ReceiptNetworkError("receipt client inspect has no valid container name")
    labels = value.get("Config", {}).get("Labels") if isinstance(value.get("Config"), Mapping) else None
    state = value.get("State")
    host_config = value.get("HostConfig")
    settings = value.get("NetworkSettings")
    networks = settings.get("Networks") if isinstance(settings, Mapping) else None
    attached = networks.get(binding.bridge) if isinstance(networks, Mapping) else None
    if (
        name.lstrip("/") != binding.container
        or not isinstance(labels, Mapping)
        or labels.get("icefarm.run") != run_id
        or labels.get("icefarm.instance") != binding.instance
        or not isinstance(state, Mapping)
        or state.get("Running") is not True
        or not isinstance(host_config, Mapping)
        or host_config.get("NetworkMode") != binding.bridge
        or not isinstance(networks, Mapping)
        or set(networks) != {binding.bridge}
        or not isinstance(attached, Mapping)
        or attached.get("NetworkID") != network_id
        or not isinstance(attached.get("IPAddress"), str)
        or not attached.get("IPAddress")
    ):
        raise ReceiptNetworkError(
            f"receipt client {binding.instance} is not exclusively attached to its planned bridge"
        )
    return {"bridge": binding.bridge, "container": binding.container,
            "ip_address": attached["IPAddress"], "network_id": network_id}
