#!/usr/bin/env python3
"""Foreground SSH/Docker orchestrator for the explicit local-LAN Icecream farm."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import shlex
import socket
import subprocess
import sys
import time
from typing import Any, Mapping, Sequence


HERE = Path(__file__).resolve().parent
DEFAULT_MANIFEST = HERE / "manifests" / "local-lan.json"
RUN_LABEL_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
SAFE_NAME_RE = re.compile(r"^[A-Za-z0-9_.@-]+$")
PATH_KEYS = {
    "runtime_prefix",
    "source_root",
    "corpus_root",
    "build_root",
    "results_root",
    "state_root",
}


class FarmError(RuntimeError):
    """A configuration, readiness, or exact-operation failure."""


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def image_fingerprint(details: Mapping[str, Any]) -> str:
    content = {
        "architecture": details.get("Architecture"),
        "config": details.get("Config"),
        "os": details.get("Os"),
        "rootfs": details.get("RootFS"),
    }
    return "sha256:" + hashlib.sha256(canonical_json(content)).hexdigest()


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise FarmError(f"cannot read JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise FarmError(f"top-level JSON value in {path} must be an object")
    return value


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def positive_int(value: str) -> int:
    result = int(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return result


def absolute_path(value: str | None, description: str) -> str:
    if not value:
        raise FarmError(f"{description} is not configured")
    path = Path(value)
    if not path.is_absolute() or ".." in path.parts or str(path) == "/":
        raise FarmError(f"{description} must be a specific absolute path: {value!r}")
    return str(path)


def load_manifest(path: Path, mounts_path: Path | None = None,
                  node_image_ref: str | None = None,
                  node_image_fingerprint: str | None = None,
                  scheduler_port: int | None = None) -> dict[str, Any]:
    manifest = read_json(path)
    if mounts_path is not None:
        overlay = read_json(mounts_path)
        unknown_top = set(overlay) - {"hosts"}
        if unknown_top:
            raise FarmError(f"unknown mount override keys: {sorted(unknown_top)}")
        for host_name, paths in overlay.get("hosts", {}).items():
            if host_name not in manifest.get("hosts", {}):
                raise FarmError(f"mount override names unknown host {host_name!r}")
            if not isinstance(paths, dict):
                raise FarmError(f"mount override for {host_name} must be an object")
            unknown_paths = set(paths) - PATH_KEYS
            if unknown_paths:
                raise FarmError(
                    f"unknown mount keys for {host_name}: {sorted(unknown_paths)}"
                )
            manifest["hosts"][host_name]["paths"].update(paths)
    if node_image_ref:
        if not node_image_fingerprint:
            raise FarmError("--node-image-ref requires --node-image-fingerprint")
        manifest["runtime_image"]["ref"] = node_image_ref
        manifest["runtime_image"]["expected_id"] = None
    if node_image_fingerprint:
        if not re.fullmatch(r"sha256:[0-9a-f]{64}", node_image_fingerprint):
            raise FarmError("--node-image-fingerprint must be an exact sha256 value")
        manifest["runtime_image"]["expected_fingerprint"] = node_image_fingerprint
    if scheduler_port is not None:
        if scheduler_port < 1024 or scheduler_port >= 65535:
            raise FarmError("scheduler port must be in the range 1024..65534")
        manifest["network"]["scheduler_port"] = scheduler_port
        manifest["network"]["scheduler_control_port"] = scheduler_port + 1
    validate_manifest(manifest)
    return manifest


def validate_manifest(manifest: Mapping[str, Any]) -> None:
    errors: list[str] = []
    if manifest.get("schema") != 2:
        errors.append("schema must be 2")

    network = manifest.get("network", {})
    if network.get("mode") != "local-lan":
        errors.append("default manifest network.mode must be local-lan")
    try:
        lan = ipaddress.ip_network(network.get("cidr", ""), strict=True)
    except ValueError as exc:
        errors.append(f"invalid LAN CIDR: {exc}")
        lan = ipaddress.ip_network("192.0.2.0/24")
    forbidden: list[ipaddress._BaseNetwork] = []
    for value in network.get("forbidden_cidrs", []):
        try:
            forbidden.append(ipaddress.ip_network(value, strict=True))
        except ValueError as exc:
            errors.append(f"invalid forbidden CIDR {value!r}: {exc}")
    if manifest.get("wan_hosts") != []:
        errors.append("default local-LAN manifest must have wan_hosts=[]")
    ports = {
        key: network.get(key)
        for key in (
            "scheduler_port",
            "scheduler_control_port",
            "worker_port_base",
        )
    }
    if any(not isinstance(value, int) or value < 1024 or value > 65535
           for value in ports.values()):
        errors.append("network ports must be integers in the range 1024..65535")
    elif network["scheduler_control_port"] != network["scheduler_port"] + 1:
        errors.append("scheduler_control_port must be scheduler_port + 1")

    hosts = manifest.get("hosts", {})
    if not isinstance(hosts, dict) or not hosts:
        errors.append("hosts must be a non-empty object")
        hosts = {}
    for name, host in hosts.items():
        try:
            address = ipaddress.ip_address(host.get("address", ""))
            if address not in lan:
                errors.append(f"host {name} address {address} is outside {lan}")
            for blocked in forbidden:
                if address in blocked:
                    errors.append(f"host {name} address {address} is in forbidden {blocked}")
        except ValueError as exc:
            errors.append(f"host {name} has invalid address: {exc}")
        interface = host.get("interface")
        if interface is not None and (
            not SAFE_NAME_RE.fullmatch(interface) or "tail" in interface.lower()
        ):
            errors.append(f"host {name} has unsafe or overlay interface {interface!r}")
        transport = host.get("transport", {})
        mode = transport.get("mode")
        if mode not in {"local", "ssh"}:
            errors.append(f"host {name} has invalid transport mode {mode!r}")
        if mode == "ssh":
            alias = transport.get("alias")
            if alias is not None and not SAFE_NAME_RE.fullmatch(alias):
                errors.append(f"host {name} has unsafe SSH alias {alias!r}")
            if transport.get("host_override") != host.get("address"):
                errors.append(f"host {name} SSH host_override must equal its LAN address")
        paths = host.get("paths", {})
        if set(paths) != PATH_KEYS:
            errors.append(f"host {name} paths must contain exactly {sorted(PATH_KEYS)}")
        for key, value in paths.items():
            if value is not None:
                try:
                    absolute_path(value, f"{name}.{key}")
                except FarmError as exc:
                    errors.append(str(exc))
        launch_enabled = host.get("launch_enabled", True)
        launch_block = host.get("launch_block")
        if not isinstance(launch_enabled, bool):
            errors.append(f"host {name} launch_enabled must be boolean")
        if launch_enabled and launch_block is not None:
            errors.append(f"launch-enabled host {name} must have launch_block=null")
        if launch_enabled is False and (
            not isinstance(launch_block, str) or not launch_block.strip()
        ):
            errors.append(f"launch-disabled host {name} must record launch_block")

    scheduler_host = manifest.get("scheduler_host")
    if scheduler_host != "nas642":
        errors.append("default scheduler_host must be nas642")
    if scheduler_host not in hosts:
        errors.append("scheduler_host is not declared in hosts")

    groups = manifest.get("resource_groups", {})
    for group_name, group in groups.items():
        members = group.get("members", [])
        for host_name in members:
            if host_name not in hosts:
                errors.append(f"resource group {group_name} names unknown host {host_name}")
            elif hosts[host_name].get("resource_group") != group_name:
                errors.append(f"host {host_name} does not point back to {group_name}")
        if int(group.get("aggregate_worker_cap", -1)) < 0:
            errors.append(f"resource group {group_name} has invalid aggregate_worker_cap")
    for host_name, host in hosts.items():
        if host.get("resource_group") not in groups:
            errors.append(f"host {host_name} references an unknown resource group")

    profiles = manifest.get("profiles", {})
    for profile_name, profile in profiles.items():
        slots = profile.get("worker_slots", {})
        if set(slots) - set(hosts):
            errors.append(f"profile {profile_name} names unknown worker hosts")
        if any(not isinstance(value, int) or value < 0 or value > 32
               for value in slots.values()):
            errors.append(
                f"profile {profile_name} worker slots must be integers in the range 0..32"
            )
        total = sum(slots.values())
        if total != profile.get("advertised_worker_slots"):
            errors.append(
                f"profile {profile_name} advertises {profile.get('advertised_worker_slots')} "
                f"but declares {total}"
            )
        order = profile.get("worker_order", [])
        if len(order) != len(set(order)) or set(order) != {
            name for name, count in slots.items() if count > 0
        }:
            errors.append(f"profile {profile_name} worker_order must name each positive host once")
        for submitter in profile.get("submitters", []):
            if submitter not in hosts:
                errors.append(f"profile {profile_name} names unknown submitter {submitter}")
            elif slots.get(submitter, 0) and not profile.get("allow_submitter_worker_overlap"):
                errors.append(
                    f"profile {profile_name} hides submitter work behind worker slots on {submitter}"
                )
        for group_name, group in groups.items():
            group_slots = sum(slots.get(member, 0) for member in group.get("members", []))
            if group_slots > group.get("aggregate_worker_cap", 0):
                errors.append(
                    f"profile {profile_name} allocates {group_slots} workers to {group_name}, "
                    f"above cap {group.get('aggregate_worker_cap')}"
                )

    scenarios = manifest.get("scenarios", {})
    for scenario_name, scenario in scenarios.items():
        if not SAFE_NAME_RE.fullmatch(scenario_name):
            errors.append(f"unsafe scenario name {scenario_name!r}")
        for key in ("submitter_count", "f_count"):
            if not isinstance(scenario.get(key), int) or scenario[key] <= 0:
                errors.append(f"scenario {scenario_name} {key} must be positive")
        if isinstance(scenario.get("f_count"), int) and scenario["f_count"] > 4:
            errors.append(f"scenario {scenario_name} cannot select more than four physical Fs")

    dimensions = manifest.get("scenario_dimensions", {})
    expected_dimensions = {
        "component_versions", "workload", "routing_policy", "codec_policy",
        "event_schedule",
    }
    if set(dimensions) != expected_dimensions:
        errors.append(
            "scenario_dimensions must contain exactly " + str(sorted(expected_dimensions))
        )
    binary_sets = manifest.get("component_binary_sets", {})
    versions = dimensions.get("component_versions", {})
    if set(versions) != {"scheduler", "submitter", "worker"}:
        errors.append("component_versions must select scheduler, submitter, and worker")
    for role, version_name in versions.items():
        if version_name not in binary_sets:
            errors.append(f"{role} selects unknown component binary set {version_name!r}")
    for name, item in binary_sets.items():
        if not SAFE_NAME_RE.fullmatch(name):
            errors.append(f"unsafe component binary set name {name!r}")
        if not isinstance(item.get("available"), bool):
            errors.append(f"component binary set {name} must declare available")
        if item.get("available") and item.get("runtime_path_key") not in PATH_KEYS:
            errors.append(f"available component binary set {name} lacks a runtime path key")
        if not item.get("available") and (
            item.get("runtime_path_key") is not None
            or item.get("runtime_image_class") is not None
        ):
            errors.append(f"unavailable component binary set {name} must not name artifacts")

    environments = manifest.get("environment_classes", {})
    if len(environments) != 4:
        errors.append("the accepted corpus matrix must expose exactly four environment classes")
    if sum(item.get("corpus_cells", 0) for item in environments.values()) != 44:
        errors.append("environment class corpus_cells must total 44")
    for name, environment in environments.items():
        expected_id = environment.get("expected_id", "")
        if not re.fullmatch(r"sha256:[0-9a-f]{64}", expected_id):
            errors.append(f"environment {name} lacks an exact sha256 image ID")
        if "@sha256:" not in environment.get("image", ""):
            errors.append(f"environment {name} image is not content-addressed")
        if not re.fullmatch(
            r"sha256:[0-9a-f]{64}", environment.get("expected_fingerprint", "")
        ):
            errors.append(f"environment {name} lacks an exact content fingerprint")
    if manifest.get("runtime_image", {}).get("class") in environments:
        errors.append("daemon runtime image must remain distinct from build environment classes")
    runtime = manifest.get("runtime_image", {})
    runtime_id = runtime.get("expected_id")
    if runtime_id is not None and not re.fullmatch(r"sha256:[0-9a-f]{64}", runtime_id):
        errors.append("daemon runtime inventoried image ID is invalid")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", runtime.get("expected_fingerprint", "")):
        errors.append("daemon runtime image lacks an exact content fingerprint")

    if errors:
        raise FarmError("manifest validation failed:\n- " + "\n- ".join(errors))


def binary_set_run_label(manifest: Mapping[str, Any],
                         dimensions: Mapping[str, Any]) -> str:
    versions = dimensions["component_versions"]
    selected = {
        role: manifest["component_binary_sets"][versions[role]]
        for role in ("scheduler", "submitter", "worker")
    }
    protocols = [selected[role]["protocol"] for role in selected]
    names = [versions[role] for role in selected]
    if len(set(protocols)) == 1 and isinstance(protocols[0], int):
        return f"p{protocols[0]}"
    if len(set(names)) == 1:
        name = re.sub(r"[^a-z0-9_.-]+", "-", names[0].lower()).strip("-.")
        return f"binary-{name or 'unnamed'}"
    role_tokens = []
    role_codes = {"scheduler": "s", "submitter": "c", "worker": "f"}
    for role in ("scheduler", "submitter", "worker"):
        protocol = selected[role]["protocol"]
        token = f"p{protocol}" if isinstance(protocol, int) else versions[role]
        token = re.sub(r"[^a-z0-9_.-]+", "-", token.lower()).strip("-.")
        role_tokens.append(f"{role_codes[role]}-{token or 'unnamed'}")
    label = "mixed-" + "-".join(role_tokens)
    if len(label) > 56:
        digest = hashlib.sha256(canonical_json(versions)).hexdigest()[:10]
        label = f"mixed-binary-sets-{digest}"
    return label


def derive_run_id(manifest: Mapping[str, Any], profile: str, scenario: str,
                  environment: str, run_label: str,
                  submitters: Sequence[str],
                  dimensions: Mapping[str, Any] | None = None) -> str:
    if not RUN_LABEL_RE.fullmatch(run_label):
        raise FarmError(
            "run label must use 1-64 lowercase letters, digits, '.', '_' or '-'"
        )
    selected_dimensions = dimensions or manifest.get("scenario_dimensions", {})
    identity = {
        "manifest": manifest,
        "profile": profile,
        "scenario": scenario,
        "environment": environment,
        "run_label": run_label,
        "submitters": list(submitters),
        "dimensions": selected_dimensions,
    }
    digest = hashlib.sha256(canonical_json(identity)).hexdigest()[:12]
    return f"{binary_set_run_label(manifest, selected_dimensions)}-{scenario}-{digest}"


def select_f_hosts(profile: Mapping[str, Any], count: int) -> dict[str, int]:
    """Select physical F endpoints; the returned values are slots, not daemon counts."""
    order = list(profile["worker_order"])
    if count > len(order):
        raise FarmError(
            f"scenario requests {count} F endpoints but profile has {len(order)} physical Fs"
        )
    selected = order[:count]
    slots = profile["worker_slots"]
    return {host_name: slots[host_name] for host_name in selected}


def build_plan(manifest: Mapping[str, Any], profile_name: str, scenario_name: str,
               environment_name: str, run_label: str,
               submitter_overrides: Sequence[str] | None = None,
               component_version_overrides: Mapping[str, str] | None = None) -> dict[str, Any]:
    try:
        profile = manifest["profiles"][profile_name]
    except KeyError as exc:
        raise FarmError(f"unknown profile {profile_name!r}") from exc
    try:
        scenario = manifest["scenarios"][scenario_name]
    except KeyError as exc:
        raise FarmError(f"unknown scenario {scenario_name!r}") from exc
    if environment_name not in manifest["environment_classes"]:
        raise FarmError(f"unknown environment class {environment_name!r}")

    submitters = list(submitter_overrides or profile["submitters"])
    if not submitters:
        raise FarmError("at least one submitter is required")
    if submitter_overrides is None and len(submitters) != scenario["submitter_count"]:
        raise FarmError("profile submitter count does not match scenario")
    for host_name in submitters:
        if host_name not in manifest["hosts"]:
            raise FarmError(f"unknown submitter host {host_name!r}")
        if profile["worker_slots"].get(host_name, 0) and not profile[
            "allow_submitter_worker_overlap"
        ]:
            raise FarmError(
                f"submitter {host_name} retains worker slots; select a profile that excludes them"
            )

    slots_by_f_host = select_f_hosts(profile, scenario["f_count"])
    dimensions = dict(manifest["scenario_dimensions"])
    versions = dict(dimensions["component_versions"])
    versions.update(component_version_overrides or {})
    if set(versions) != {"scheduler", "submitter", "worker"}:
        raise FarmError("component version overrides may name only scheduler, submitter, and worker")
    for role, version_name in versions.items():
        try:
            binary_set = manifest["component_binary_sets"][version_name]
        except KeyError as exc:
            raise FarmError(f"{role} selects unknown component binary set {version_name!r}") from exc
        if not binary_set["available"]:
            raise FarmError(
                f"{role} component binary set {version_name!r} is declared but unavailable: "
                f"{binary_set['provenance']}"
            )
    dimensions["component_versions"] = versions
    selected_binary_sets = {
        role: {
            "name": version_name,
            **manifest["component_binary_sets"][version_name],
        }
        for role, version_name in versions.items()
    }
    submitter_capacity = [
        manifest["hosts"][host_name].get(
            "client_capacity",
            {
                "class": "not-a-client-profile",
                "provenance": "no client capacity class declared",
                "measured_rate": None,
                "measurement_status": "not applicable",
            },
        )
        for host_name in submitters
    ]
    artifact_identities = {
        "controller_source_git_head": git_head(HERE),
        "runtime_image_fingerprint": manifest["runtime_image"]["expected_fingerprint"],
        "environment_image_fingerprint": manifest["environment_classes"][environment_name][
            "expected_fingerprint"
        ],
    }
    identity_dimensions = {**dimensions, "artifact_identities": artifact_identities}
    run_id = derive_run_id(
        manifest, profile_name, scenario_name, environment_name, run_label, submitters,
        identity_dimensions,
    )
    network = manifest["network"]

    workers: list[dict[str, Any]] = []
    for ordinal, (host_name, slots) in enumerate(slots_by_f_host.items()):
        address = manifest["hosts"][host_name]["address"]
        port = network["worker_port_base"]
        workers.append(
            {
                "role": "worker",
                "host": host_name,
                "ordinal": ordinal,
                "host_index": 0,
                "slots": slots,
                "port": port,
                "route_identity": f"{address}:{port}",
                "cache_identity_owner": host_name,
                "node_name": f"f-{host_name}",
                "service": f"f{ordinal:02d}",
                "container": f"icefarm-{run_id}-f{ordinal:02d}",
                "log_file": f"f{ordinal:02d}-{host_name}.log",
            }
        )

    submitter_records: list[dict[str, Any]] = []
    per_host_submitter_count: dict[str, int] = {}
    for ordinal, host_name in enumerate(submitters):
        host_index = per_host_submitter_count.get(host_name, 0)
        per_host_submitter_count[host_name] = host_index + 1
        submitter_records.append(
            {
                "role": "submitter",
                "host": host_name,
                "ordinal": ordinal,
                "host_index": host_index,
                "accepts_remote_jobs": False,
                "registration_port": 0,
                "node_name": f"c-{host_name}-{host_index:02d}",
                "service": f"c{ordinal:02d}",
                "container": f"icefarm-{run_id}-c{ordinal:02d}",
                "log_file": f"c{ordinal:02d}-{host_name}.log",
            }
        )

    scheduler_host = manifest["scheduler_host"]
    scheduler = {
        "role": "scheduler",
        "host": scheduler_host,
        "service": "scheduler",
        "container": f"icefarm-{run_id}-scheduler",
        "node_name": "scheduler",
        "port": network["scheduler_port"],
        "log_file": "scheduler.log",
    }
    group_slots: dict[str, int] = {}
    group_f_counts: dict[str, int] = {}
    for host_name, slots in slots_by_f_host.items():
        group = manifest["hosts"][host_name]["resource_group"]
        group_slots[group] = group_slots.get(group, 0) + slots
        group_f_counts[group] = group_f_counts.get(group, 0) + 1

    return {
        "schema": 2,
        "run_id": run_id,
        "run_label": run_label,
        "profile": profile_name,
        "scenario": scenario_name,
        "environment": environment_name,
        "dimensions": dimensions,
        "selected_binary_sets": selected_binary_sets,
        "artifact_identities": artifact_identities,
        "client_capacity": submitter_capacity,
        "network_mode": "local-lan",
        "netname": run_id,
        "scheduler": scheduler,
        "submitters": submitter_records,
        "workers": workers,
        "accounting": {
            "advertised_profile_worker_slots": profile["advertised_worker_slots"],
            "selected_compile_slots": sum(slots_by_f_host.values()),
            "f_daemon_containers": len(workers),
            "f_host_identities": len(slots_by_f_host),
            "route_identity_count": len({worker["route_identity"] for worker in workers}),
            "cache_identity_owner_count": len(
                {worker["cache_identity_owner"] for worker in workers}
            ),
            "independent_resource_groups": len(group_slots),
            "slots_by_f_host": slots_by_f_host,
            "f_daemons_by_host": {host_name: 1 for host_name in slots_by_f_host},
            "slots_by_resource_group": group_slots,
            "f_daemons_by_resource_group": group_f_counts,
            "scheduler_resource_group": manifest["hosts"][scheduler_host]["resource_group"],
            "scheduler_and_submitter_load_is_additional": True,
        },
    }


def plan_hosts(plan: Mapping[str, Any]) -> dict[str, set[str]]:
    result: dict[str, set[str]] = {}
    for record in [plan["scheduler"], *plan["submitters"], *plan["workers"]]:
        result.setdefault(record["host"], set()).add(record["role"])
    return result


def records_for_host(plan: Mapping[str, Any], host_name: str) -> list[dict[str, Any]]:
    records = [plan["scheduler"], *plan["submitters"], *plan["workers"]]
    return [record for record in records if record["host"] == host_name]


def expected_registrations(manifest: Mapping[str, Any],
                           plan: Mapping[str, Any]) -> list[str]:
    result = []
    for record in [*plan["submitters"], *plan["workers"]]:
        address = manifest["hosts"][record["host"]]["address"]
        port = (
            record["registration_port"]
            if record["role"] == "submitter"
            else record["port"]
        )
        result.append(f"{record['node_name']} ({address}:{port})")
    return result


def registration_matches(manifest: Mapping[str, Any], record: Mapping[str, Any],
                         snapshot: str) -> bool:
    address = manifest["hosts"][record["host"]]["address"]
    port = record.get("registration_port", record.get("port"))
    identity = re.escape(f"{record['node_name']} ({address}:{port})")
    if record["role"] == "worker":
        # listcs reports current/max jobs. Requiring the configured maximum proves
        # that one daemon advertised its slot budget, rather than N one-slot daemons.
        suffix = rf"[^\n]*\bjobs=[0-9]+/{record['slots']}\b"
    else:
        suffix = r"[^\n]*\bjobs=[0-9]+/0\b"
    return re.search(identity + suffix, snapshot) is not None


def required_listener_ports(manifest: Mapping[str, Any],
                            plan: Mapping[str, Any], host_name: str) -> list[int]:
    """Return only ports opened by services in this plan on one host."""
    ports = {
        record["port"]
        for record in records_for_host(plan, host_name)
        if record["role"] in {"scheduler", "worker"}
    }
    if plan["scheduler"]["host"] == host_name:
        ports.add(manifest["network"]["scheduler_control_port"])
    return sorted(ports)


def missing_registrations(manifest: Mapping[str, Any], plan: Mapping[str, Any],
                          snapshot: str) -> list[str]:
    missing = []
    for record in [*plan["submitters"], *plan["workers"]]:
        if registration_matches(manifest, record, snapshot):
            continue
        expected = expected_registrations(manifest, {**plan, "submitters": [], "workers": [record]})[0]
        if record["role"] == "worker":
            expected += f" jobs=*/{record['slots']}"
        else:
            expected += " jobs=*/0"
        missing.append(expected)
    return missing


def planned_container_status(runner: "HostRunner",
                             records: Sequence[Mapping[str, Any]]) -> tuple[dict[str, Any],
                                                                              list[str]]:
    report: dict[str, Any] = {}
    errors: list[str] = []
    for record in records:
        container = record["container"]
        existing = runner.run(
            [
                "docker", "container", "ls", "--all",
                "--filter", f"name=^/{container}$",
                "--format", "{{.Names}}",
            ],
            timeout=15,
        )
        names = existing.stdout.decode("utf-8", "replace").splitlines()
        item = {"container": container, "exists": container in names}
        if existing.returncode != 0:
            item["error"] = existing.stderr.decode("utf-8", "replace").strip()
            errors.append(f"cannot inspect planned container name {container}")
        elif item["exists"]:
            errors.append(f"planned container already exists: {container}")
        report[container] = item
    return report, errors


def acceptance_source_status(runner: "HostRunner",
                             source_root: str) -> tuple[dict[str, Any], str | None]:
    source_file = f"{source_root}/docker/farm/acceptance/tiny.cpp"
    expected_sha256 = hashlib.sha256(
        (HERE / "acceptance" / "tiny.cpp").read_bytes()
    ).hexdigest()
    source_digest = runner.run(["sha256sum", "--", source_file], timeout=30)
    actual_sha256 = None
    if source_digest.returncode == 0:
        fields = source_digest.stdout.decode("ascii", "replace").split(maxsplit=1)
        if fields and re.fullmatch(r"[0-9a-f]{64}", fields[0]):
            actual_sha256 = fields[0]
    item = {
        "path": source_file,
        "expected_sha256": expected_sha256,
        "actual_sha256": actual_sha256,
        "ok": actual_sha256 == expected_sha256,
    }
    if item["ok"]:
        return item, None
    return item, (
        "mounted acceptance source differs from the controller harness: "
        f"{source_file}"
    )


def expected_paths_for_roles(roles: set[str]) -> tuple[set[str], set[str]]:
    must_exist = {"runtime_prefix"}
    creatable = {"results_root"}
    if "worker" in roles or "submitter" in roles:
        creatable.add("state_root")
    if "submitter" in roles:
        must_exist.update({"source_root", "corpus_root"})
        creatable.add("build_root")
    return must_exist, creatable


class HostRunner:
    def __init__(self, name: str, config: Mapping[str, Any]):
        self.name = name
        self.config = config

    def available(self) -> tuple[bool, str]:
        transport = self.config["transport"]
        if transport["mode"] == "local":
            return True, ""
        alias = transport.get("alias")
        if not alias:
            return False, "SSH alias/account is not configured"
        return True, ""

    def prefix(self) -> list[str]:
        transport = self.config["transport"]
        if transport["mode"] == "local":
            return []
        alias = transport.get("alias")
        if not alias or not SAFE_NAME_RE.fullmatch(alias):
            raise FarmError(f"host {self.name} has no safe SSH alias")
        address = self.config["address"]
        return [
            "ssh",
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=8",
            "-o", "StrictHostKeyChecking=yes",
            "-o", f"HostName={address}",
            "-o", f"HostKeyAlias={alias}",
            alias,
        ]

    def run(self, argv: Sequence[str], *, input_bytes: bytes | None = None,
            timeout: int = 30, check: bool = False) -> subprocess.CompletedProcess[bytes]:
        if not argv:
            raise FarmError("empty host command")
        if self.config["transport"]["mode"] == "local":
            command = list(argv)
            controller_timeout = timeout
        else:
            remote_command = [
                "timeout", "--signal=TERM", "--kill-after=5s", f"{timeout}s", *argv
            ]
            command = self.prefix() + [shlex.join(remote_command)]
            controller_timeout = timeout + 15
        try:
            result = subprocess.run(
                command,
                input=input_bytes,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=controller_timeout,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise FarmError(f"{self.name}: command failed to run: {exc}") from exc
        if check and result.returncode != 0:
            stderr = result.stderr.decode("utf-8", "replace").strip()
            raise FarmError(
                f"{self.name}: {shlex.join(argv)} exited {result.returncode}: {stderr}"
            )
        return result


def interface_counter_snapshot(manifest: Mapping[str, Any],
                               plan: Mapping[str, Any]) -> dict[str, Any]:
    """Capture read-only host-interface byte counters for the exact LAN interfaces."""
    captured: dict[str, Any] = {
        "captured_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "monotonic_ns": time.monotonic_ns(),
        "hosts": {},
    }
    for host_name in sorted(plan_hosts(plan)):
        host = manifest["hosts"][host_name]
        interface = host["interface"]
        runner = HostRunner(host_name, host)
        values: dict[str, int] = {}
        for direction in ("rx", "tx"):
            path = f"/sys/class/net/{interface}/statistics/{direction}_bytes"
            result = runner.run(["cat", "--", path], timeout=10)
            text_value = result.stdout.decode("ascii", "replace").strip()
            if result.returncode != 0 or not text_value.isdigit():
                raise FarmError(
                    f"{host_name}: cannot read exact LAN {direction} counter from {path}"
                )
            values[f"{direction}_bytes"] = int(text_value)
        captured["hosts"][host_name] = {
            "address": host["address"],
            "interface": interface,
            **values,
        }
    return captured


def directional_network_ledger(manifest: Mapping[str, Any], plan: Mapping[str, Any],
                               before: Mapping[str, Any],
                               after: Mapping[str, Any],
                               routes_before: Mapping[str, Any],
                               routes_after: Mapping[str, Any]) -> dict[str, Any]:
    if not routes_before.get("routes") or not routes_after.get("routes"):
        raise FarmError("network ledger requires verified C/F routes before and after acceptance")
    if any(
        not record.get("ok")
        for snapshot in (routes_before, routes_after)
        for record in snapshot["routes"]
    ):
        raise FarmError("network ledger refuses an unverified C/F route")
    elapsed_ns = int(after["monotonic_ns"]) - int(before["monotonic_ns"])
    if elapsed_ns <= 0:
        raise FarmError("network observation elapsed time must be positive")
    elapsed_seconds = elapsed_ns / 1_000_000_000
    host_deltas: dict[str, Any] = {}
    for host_name in sorted(plan_hosts(plan)):
        first = before["hosts"][host_name]
        last = after["hosts"][host_name]
        rx_delta = int(last["rx_bytes"]) - int(first["rx_bytes"])
        tx_delta = int(last["tx_bytes"]) - int(first["tx_bytes"])
        if rx_delta < 0 or tx_delta < 0:
            raise FarmError(f"{host_name}: LAN interface counters moved backwards")
        host_deltas[host_name] = {
            "address": manifest["hosts"][host_name]["address"],
            "interface": manifest["hosts"][host_name]["interface"],
            "rx_bytes": rx_delta,
            "tx_bytes": tx_delta,
            "rx_bits_per_second": round(rx_delta * 8 / elapsed_seconds, 3),
            "tx_bits_per_second": round(tx_delta * 8 / elapsed_seconds, 3),
        }
    routes = []
    for submitter in plan["submitters"]:
        c_host = submitter["host"]
        for worker in plan["workers"]:
            f_host = worker["host"]
            routes.append(
                {
                    "c_host": c_host,
                    "c_address": manifest["hosts"][c_host]["address"],
                    "f_host": f_host,
                    "f_address": manifest["hosts"][f_host]["address"],
                    "f_route_identity": worker["route_identity"],
                    "c_to_f": {
                        "c_interface_tx": host_deltas[c_host]["tx_bytes"],
                        "f_interface_rx": host_deltas[f_host]["rx_bytes"],
                    },
                    "f_to_c": {
                        "f_interface_tx": host_deltas[f_host]["tx_bytes"],
                        "c_interface_rx": host_deltas[c_host]["rx_bytes"],
                    },
                }
            )
    return {
        "schema": 2,
        "measurement": "host LAN-interface aggregate counters during acceptance",
        "attribution": (
            "directional provenance, not per-flow byte attribution; shared-interface traffic "
            "may contribute to the observed deltas"
        ),
        "elapsed_ns": elapsed_ns,
        "client_capacity": plan["client_capacity"],
        "host_deltas": host_deltas,
        "routes": routes,
        "route_checks": {
            "before": routes_before,
            "after": routes_after,
        },
        "before": before,
        "after": after,
    }


def parse_key_values(output: bytes) -> dict[str, str]:
    result: dict[str, str] = {}
    for raw_line in output.decode("utf-8", "replace").splitlines():
        if "=" in raw_line:
            key, value = raw_line.split("=", 1)
            result[key] = value
    return result


def basic_inventory(runner: HostRunner) -> dict[str, Any]:
    available, reason = runner.available()
    if not available:
        return {"ok": False, "error": reason}
    script = r'''set -u
printf 'hostname=%s\n' "$(hostname -f 2>/dev/null || hostname)"
printf 'architecture=%s\n' "$(uname -m)"
printf 'nproc_effective=%s\n' "$(nproc)"
printf 'nproc_all=%s\n' "$(nproc --all)"
printf 'online_cpus=%s\n' "$(getconf _NPROCESSORS_ONLN)"
printf 'omp_num_threads=%s\n' "${OMP_NUM_THREADS:-}"
printf 'cpuset=%s\n' "$(cat /sys/fs/cgroup/cpuset.cpus.effective 2>/dev/null || true)"
printf 'cpu_max=%s\n' "$(cat /sys/fs/cgroup/cpu.max 2>/dev/null || true)"
printf 'cpu_cfs_quota_us=%s\n' "$(cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us 2>/dev/null || true)"
printf 'cpu_cfs_period_us=%s\n' "$(cat /sys/fs/cgroup/cpu/cpu.cfs_period_us 2>/dev/null || true)"
printf 'load=%s\n' "$(cut -d' ' -f1-3 /proc/loadavg)"
printf 'memory_kib=%s\n' "$(grep '^MemTotal:' /proc/meminfo | tr -s ' ' | cut -d' ' -f2)"
printf 'virtualization=%s\n' "$(systemd-detect-virt 2>/dev/null || true)"
printf 'os=%s\n' "$(. /etc/os-release && printf '%s %s' "$NAME" "$VERSION_ID")"
printf 'kernel=%s\n' "$(uname -r)"
printf 'docker_client=%s\n' "$(docker version --format '{{.Client.Version}}' 2>/dev/null || printf unavailable)"
printf 'docker_server=%s\n' "$(docker version --format '{{.Server.Version}}' 2>/dev/null || printf unavailable)"
printf 'compose=%s\n' "$(docker compose version --short 2>/dev/null || printf unavailable)"
printf 'docker_permission=%s\n' "$(docker info >/dev/null 2>&1 && printf yes || printf no)"
printf 'cpu_pressure=%s\n' "$(tr '\n' ';' </proc/pressure/cpu 2>/dev/null || true)"
'''
    result = runner.run(["bash", "-c", script], timeout=15)
    if result.returncode != 0:
        return {
            "ok": False,
            "error": result.stderr.decode("utf-8", "replace").strip(),
        }
    facts = parse_key_values(result.stdout)
    facts["ok"] = True
    return facts


def verify_transport_identity(manifest: Mapping[str, Any], host_name: str) -> dict[str, Any]:
    host = manifest["hosts"][host_name]
    runner = HostRunner(host_name, host)
    available, reason = runner.available()
    if not available:
        raise FarmError(f"{host_name}: {reason}")
    facts = basic_inventory(runner)
    if not facts.get("ok"):
        raise FarmError(f"{host_name}: {facts.get('error', 'inventory failed')}")
    expected_hostname = host["transport"].get("expected_hostname")
    if expected_hostname and facts.get("hostname") != expected_hostname:
        raise FarmError(
            f"{host_name}: expected hostname {expected_hostname!r}, got {facts.get('hostname')!r}"
        )
    interface = host.get("interface")
    if not interface:
        raise FarmError(f"{host_name}: LAN interface is not inventoried")
    interface_result = runner.run(
        ["ip", "-o", "-4", "addr", "show", "dev", interface, "scope", "global"],
        timeout=10,
    )
    interface_text = interface_result.stdout.decode("utf-8", "replace")
    if interface_result.returncode or f" {host['address']}/" not in interface_text:
        raise FarmError(
            f"{host_name}: {interface} does not expose required LAN address {host['address']}"
        )
    if facts.get("docker_permission") != "yes":
        raise FarmError(f"{host_name}: Docker server permission is unavailable")
    return facts


def scheduler_route_error(route_text: str, interface: str, source_address: str,
                          *, scheduler_is_local: bool) -> str | None:
    tokens = route_text.split()
    target_address = ""
    if tokens:
        target_address = tokens[1] if tokens[0] == "local" and len(tokens) > 1 else tokens[0]
    return lan_route_error(
        route_text,
        interface,
        source_address,
        target_address,
        target_is_local=scheduler_is_local,
    )


def lan_route_error(route_text: str, interface: str, source_address: str,
                    target_address: str, *, target_is_local: bool) -> str | None:
    if not route_text:
        return f"route to {target_address} is unavailable"
    tokens = route_text.split()
    if any("tailscale" in token.lower() for token in tokens):
        return f"route to {target_address} uses an overlay interface"
    overlay = ipaddress.ip_network("100.64.0.0/10")
    for token in tokens:
        try:
            address = ipaddress.ip_address(token)
        except ValueError:
            continue
        if address in overlay:
            return f"route to {target_address} uses an overlay address"
    if target_address not in tokens:
        return f"route output does not name exact target {target_address}"
    try:
        route_interface = tokens[tokens.index("dev") + 1]
        route_source = tokens[tokens.index("src") + 1]
    except (ValueError, IndexError):
        return f"route to {target_address} lacks an exact device or source address"
    allowed_interfaces = {interface, "lo"} if target_is_local else {interface}
    if route_interface not in allowed_interfaces:
        return (
            f"route to {target_address} uses device {route_interface!r}, expected "
            f"{sorted(allowed_interfaces)}"
        )
    if route_source != source_address:
        return (
            f"route to {target_address} uses source {route_source!r}, "
            f"expected {source_address!r}"
        )
    return None


def peer_route_snapshot(manifest: Mapping[str, Any],
                        plan: Mapping[str, Any]) -> dict[str, Any]:
    """Verify both directions of every selected C/F pair on explicit LAN identities."""
    records = []
    seen: set[tuple[str, str]] = set()
    for submitter in plan["submitters"]:
        c_host = submitter["host"]
        for worker in plan["workers"]:
            f_host = worker["host"]
            for direction, source_name, target_name in (
                ("c-to-f", c_host, f_host),
                ("f-to-c", f_host, c_host),
            ):
                key = (source_name, target_name)
                if key in seen:
                    continue
                seen.add(key)
                source = manifest["hosts"][source_name]
                target = manifest["hosts"][target_name]
                runner = HostRunner(source_name, source)
                result = runner.run(
                    ["ip", "-o", "route", "get", target["address"]], timeout=10
                )
                route_text = result.stdout.decode("utf-8", "replace").strip()
                error = None
                if result.returncode != 0:
                    error = f"route command exited {result.returncode}"
                else:
                    error = lan_route_error(
                        route_text,
                        source["interface"],
                        source["address"],
                        target["address"],
                        target_is_local=source_name == target_name,
                    )
                record = {
                    "direction": direction,
                    "source_host": source_name,
                    "source_address": source["address"],
                    "source_interface": source["interface"],
                    "target_host": target_name,
                    "target_address": target["address"],
                    "route": route_text,
                    "ok": error is None,
                }
                if error:
                    record["error"] = error
                    raise FarmError(
                        f"{direction} {source_name}->{target_name} LAN route failed: {error}; "
                        f"observed {route_text!r}"
                    )
                records.append(record)
    return {
        "checked_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "network_mode": "local-lan",
        "routes": records,
    }


def inspect_host(manifest: Mapping[str, Any], plan: Mapping[str, Any],
                 host_name: str, roles: set[str]) -> dict[str, Any]:
    host = manifest["hosts"][host_name]
    runner = HostRunner(host_name, host)
    report: dict[str, Any] = {
        "host": host_name,
        "address": host["address"],
        "interface": host.get("interface"),
        "resource_group": host["resource_group"],
        "roles": sorted(roles),
        "errors": [],
        "warnings": [],
    }
    if not host.get("launch_enabled", True):
        report["errors"].append(f"launch disabled: {host['launch_block']}")
        report["ok"] = False
        return report
    available, reason = runner.available()
    if not available:
        report["errors"].append(reason)
        report["ok"] = False
        return report

    facts = basic_inventory(runner)
    report["facts"] = facts
    if not facts.get("ok"):
        report["errors"].append(facts.get("error", "inventory command failed"))
        report["ok"] = False
        return report
    if facts.get("docker_permission") != "yes":
        report["errors"].append("Docker server permission is unavailable")
    if facts.get("compose") == "unavailable":
        report["errors"].append("Docker Compose is unavailable")
    if facts.get("architecture") != "x86_64":
        report["errors"].append(
            f"build environments require x86_64, found {facts.get('architecture')!r}"
        )
    expected_hostname = host["transport"].get("expected_hostname")
    if expected_hostname and facts.get("hostname") != expected_hostname:
        report["errors"].append(
            f"hostname mismatch: expected {expected_hostname}, got {facts.get('hostname')}"
        )

    interface = host.get("interface")
    if not interface:
        report["errors"].append("LAN interface is not inventoried")
    else:
        ip_result = runner.run(
            ["ip", "-o", "-4", "addr", "show", "dev", interface, "scope", "global"],
            timeout=10,
        )
        ip_text = ip_result.stdout.decode("utf-8", "replace")
        report["interface_record"] = ip_text.strip()
        if ip_result.returncode != 0 or f" {host['address']}/" not in ip_text:
            report["errors"].append(
                f"{interface} does not expose required LAN address {host['address']}"
            )
        if "tailscale" in ip_text.lower():
            report["errors"].append("overlay interface appeared in the selected LAN route")

    scheduler_address = manifest["hosts"][manifest["scheduler_host"]]["address"]
    route = runner.run(["ip", "-o", "route", "get", scheduler_address], timeout=10)
    route_text = route.stdout.decode("utf-8", "replace").strip()
    report["scheduler_route"] = route_text
    route_error = None
    if route.returncode != 0:
        route_error = "scheduler route command failed"
    elif interface:
        route_error = scheduler_route_error(
            route_text,
            interface,
            host["address"],
            scheduler_is_local=host_name == manifest["scheduler_host"],
        )
    if route_error:
        report["errors"].append(route_error)

    selected_ports = required_listener_ports(manifest, plan, host_name)
    listeners = runner.run(["ss", "-H", "-ltn"], timeout=10)
    listener_lines = listeners.stdout.decode("utf-8", "replace").splitlines()
    occupied: dict[int, list[str]] = {}
    if listeners.returncode != 0:
        report["errors"].append("cannot inspect existing TCP listeners")
    else:
        for port in selected_ports:
            matching = [
                line for line in listener_lines
                if any(token.rsplit(":", 1)[-1] == str(port) for token in line.split())
            ]
            if matching:
                occupied[port] = matching
                report["errors"].append(f"required TCP port {port} is already listening")
    report["tcp_ports"] = {
        "required": selected_ports,
        "occupied": occupied,
    }

    container_report, container_errors = planned_container_status(
        runner, records_for_host(plan, host_name)
    )
    report["errors"].extend(container_errors)
    report["planned_containers"] = container_report

    must_exist, creatable = expected_paths_for_roles(roles)
    paths = host["paths"]
    path_report: dict[str, Any] = {}
    for key in sorted(must_exist | creatable):
        value = paths.get(key)
        if not value:
            report["errors"].append(f"{key} is not configured")
            path_report[key] = {"path": None, "ok": False}
            continue
        try:
            value = absolute_path(value, f"{host_name}.{key}")
        except FarmError as exc:
            report["errors"].append(str(exc))
            path_report[key] = {"path": value, "ok": False}
            continue
        if key in must_exist:
            result = runner.run(
                ["test", "-d", value, "-a", "-r", value, "-a", "-x", value],
                timeout=10,
            )
            ok = result.returncode == 0
            if not ok:
                report["errors"].append(
                    f"required directory is absent or unreadable: {value}"
                )
            path_report[key] = {"path": value, "must_exist": True, "ok": ok}
        else:
            parent = str(Path(value).parent)
            result = runner.run(["test", "-d", parent, "-a", "-w", parent], timeout=10)
            ok = result.returncode == 0
            if not ok:
                report["errors"].append(
                    f"parent directory is absent or not writable for {value}: {parent}"
                )
            path_report[key] = {
                "path": value,
                "will_create": True,
                "parent": parent,
                "ok": ok,
            }
    report["paths"] = path_report

    if "submitter" in roles and paths.get("source_root"):
        acceptance_source, source_error = acceptance_source_status(
            runner, paths["source_root"]
        )
        report["acceptance_source"] = acceptance_source
        if source_error:
            report["errors"].append(source_error)

    runtime_prefix = paths.get("runtime_prefix")
    binary_report: dict[str, Any] = {}
    if runtime_prefix:
        required_bins: dict[str, str] = {}
        if "scheduler" in roles:
            required_bins["icecc-scheduler"] = f"{runtime_prefix}/sbin/icecc-scheduler"
        if roles & {"worker", "submitter"}:
            required_bins["iceccd"] = f"{runtime_prefix}/sbin/iceccd"
        if "submitter" in roles:
            required_bins["icecc"] = f"{runtime_prefix}/bin/icecc"
            required_bins["icecc-create-env"] = f"{runtime_prefix}/bin/icecc-create-env"
            required_bins["compilerwrapper"] = (
                f"{runtime_prefix}/libexec/icecc/compilerwrapper"
            )
        for name, binary in required_bins.items():
            result = runner.run(["test", "-x", binary], timeout=10)
            item = {"path": binary, "executable": result.returncode == 0, "sha256": None}
            if result.returncode != 0:
                report["errors"].append(f"required installed binary is not executable: {binary}")
            else:
                digest = runner.run(["sha256sum", "--", binary], timeout=30)
                if digest.returncode != 0:
                    report["errors"].append(f"cannot hash required installed binary: {binary}")
                else:
                    value = digest.stdout.decode("ascii", "replace").split(maxsplit=1)[0]
                    if not re.fullmatch(r"[0-9a-f]{64}", value):
                        report["errors"].append(
                            f"invalid SHA-256 output for required installed binary: {binary}"
                        )
                    else:
                        item["sha256"] = value
            binary_report[name] = item
    report["runtime_binaries"] = binary_report

    images = [("runtime", manifest["runtime_image"])]
    if "submitter" in roles:
        images.append(("environment", manifest["environment_classes"][plan["environment"]]))
    image_report: list[dict[str, Any]] = []
    for kind, image in images:
        ref = image.get("ref") or image.get("tag") or image.get("image")
        result = runner.run(
            ["docker", "image", "inspect", "--format", "{{json .}}", ref], timeout=15
        )
        details: dict[str, Any] = {}
        if result.returncode == 0:
            try:
                details = json.loads(result.stdout)
            except json.JSONDecodeError:
                report["errors"].append(f"Docker returned invalid image metadata for {ref}")
        actual_id = details.get("Id")
        actual_fingerprint = image_fingerprint(details) if details else None
        platform = f"{details.get('Os')}/{details.get('Architecture')}"
        labels = details.get("Config", {}).get("Labels") or {}
        item = {
            "kind": kind,
            "ref": ref,
            "expected_id": image.get("expected_id"),
            "actual_id": actual_id or None,
            "engine_id_matches_inventory": (
                actual_id == image.get("expected_id") if image.get("expected_id") else None
            ),
            "expected_fingerprint": image.get("expected_fingerprint"),
            "actual_fingerprint": actual_fingerprint,
            "platform": platform,
            "labels": labels,
            "ok": result.returncode == 0 and bool(details),
        }
        if result.returncode != 0:
            report["errors"].append(f"required image is unavailable: {ref}")
        elif platform != "linux/amd64":
            item["ok"] = False
            report["errors"].append(f"image {ref} has unsupported platform {platform}")
        elif actual_fingerprint != image.get("expected_fingerprint"):
            item["ok"] = False
            report["errors"].append(
                f"image content mismatch for {ref}: expected "
                f"{image.get('expected_fingerprint')}, got {actual_fingerprint}"
            )
        if kind == "runtime" and result.returncode == 0:
            if labels.get("org.icecream.farm.image-class") != "daemon-runtime":
                item["ok"] = False
                report["errors"].append(f"runtime image {ref} lacks daemon-runtime class label")
            if labels.get("org.icecream.farm.boost") != "1.74":
                item["ok"] = False
                report["errors"].append(f"runtime image {ref} lacks Boost 1.74 label")
        image_report.append(item)
    report["images"] = image_report

    visible = facts.get("online_cpus")
    declared = plan["accounting"]["slots_by_f_host"].get(host_name, 0)
    if declared and visible and int(visible) < declared:
        report["warnings"].append(
            f"declared compile-slot allocation {declared} exceeds current visible CPU count {visible}; "
            "owner allocation is retained and the ratio is recorded"
        )

    report["ok"] = not report["errors"]
    return report


def preflight(manifest: Mapping[str, Any], plan: Mapping[str, Any]) -> dict[str, Any]:
    host_roles = plan_hosts(plan)
    hosts = {
        name: inspect_host(manifest, plan, name, roles)
        for name, roles in sorted(host_roles.items())
    }
    runtime_fingerprints = {
        item.get("actual_fingerprint")
        for host in hosts.values()
        for item in host.get("images", [])
        if item.get("kind") == "runtime" and item.get("actual_fingerprint")
    }
    if len(runtime_fingerprints) > 1:
        message = (
            "selected hosts have different daemon runtime content fingerprints: "
            f"{sorted(runtime_fingerprints)}"
        )
        for host in hosts.values():
            host["errors"].append(message)
            host["ok"] = False
    binary_hashes: dict[str, dict[str, str]] = {}
    for host_name, host in hosts.items():
        for name, item in host.get("runtime_binaries", {}).items():
            if item.get("sha256"):
                binary_hashes.setdefault(name, {})[host_name] = item["sha256"]
    for name, host_hashes in binary_hashes.items():
        if len(set(host_hashes.values())) > 1:
            message = f"selected hosts have different {name} binary hashes: {host_hashes}"
            for host_name in host_hashes:
                hosts[host_name]["errors"].append(message)
                hosts[host_name]["ok"] = False
    peer_routes: dict[str, Any] = {
        "checked_at": None,
        "network_mode": "local-lan",
        "routes": [],
        "status": "skipped-due-to-host-preflight-errors",
    }
    peer_route_errors: list[str] = []
    if all(host.get("ok") for host in hosts.values()):
        try:
            peer_routes = peer_route_snapshot(manifest, plan)
            peer_routes["status"] = "verified"
        except FarmError as exc:
            peer_route_errors.append(str(exc))
            peer_routes["status"] = "failed"
            peer_routes["error"] = str(exc)
    group_measurements: dict[str, Any] = {}
    for group_name, group in manifest["resource_groups"].items():
        selected = [name for name in group["members"] if name in hosts]
        if not selected:
            continue
        group_measurements[group_name] = {
            "selected_hosts": selected,
            "aggregate_worker_cap": group["aggregate_worker_cap"],
            "planned_f_daemons": plan["accounting"]["f_daemons_by_resource_group"].get(
                group_name, 0
            ),
            "planned_compile_slots": plan["accounting"]["slots_by_resource_group"].get(
                group_name, 0
            ),
            "visible_cpu_views": {
                name: hosts[name].get("facts", {}).get("online_cpus") for name in selected
            },
            "loads": {
                name: hosts[name].get("facts", {}).get("load") for name in selected
            },
            "do_not_interpret_visible_cpu_sum_as_independent_capacity": len(group["members"]) > 1,
        }
    return {
        "schema": 2,
        "checked_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "run_id": plan["run_id"],
        "network_mode": "local-lan",
        "wan_hosts": [],
        "hosts": hosts,
        "peer_routes": peer_routes,
        "errors": peer_route_errors,
        "runtime_image_engine_ids": sorted({
            item.get("actual_id")
            for host in hosts.values()
            for item in host.get("images", [])
            if item.get("kind") == "runtime" and item.get("actual_id")
        }),
        "runtime_image_fingerprints": sorted(runtime_fingerprints),
        "runtime_binary_hashes": binary_hashes,
        "resource_groups": group_measurements,
        "ok": all(item["ok"] for item in hosts.values()) and not peer_route_errors,
    }


def bind(source: str, target: str, read_only: bool = False) -> dict[str, Any]:
    return {
        "type": "bind",
        "source": source,
        "target": target,
        "read_only": read_only,
    }


def service_base(plan: Mapping[str, Any], record: Mapping[str, Any], image: str) -> dict[str, Any]:
    return {
        "image": image,
        "container_name": record["container"],
        "network_mode": "host",
        "pull_policy": "never",
        "init": True,
        "restart": "no",
        "labels": {
            "org.icecream.farm.run_id": plan["run_id"],
            "org.icecream.farm.role": record["role"],
            "org.icecream.farm.host": record["host"],
        },
        "stop_grace_period": "20s",
    }


def daemon_process(binary: str, arguments: Sequence[str],
                   writable_paths: Sequence[str]) -> dict[str, Any]:
    """Prepare only run-scoped mounts, then replace the shell with the daemon."""
    if not writable_paths:
        raise FarmError("daemon process requires at least one writable run path")
    quoted_paths = " ".join(shlex.quote(path) for path in writable_paths)
    script = (
        f"chown 65534:65534 {quoted_paths}\n"
        f"exec {shlex.quote(binary)} \"$@\""
    )
    return {
        "entrypoint": ["/bin/sh", "-ec"],
        "command": [script, Path(binary).name, *arguments],
    }


def render_compose(manifest: Mapping[str, Any], plan: Mapping[str, Any],
                   host_name: str) -> dict[str, Any]:
    host = manifest["hosts"][host_name]
    paths = host["paths"]
    scheduler_address = manifest["hosts"][manifest["scheduler_host"]]["address"]
    scheduler_endpoint = f"{scheduler_address}:{manifest['network']['scheduler_port']}"
    run_id = plan["run_id"]
    result_path = f"{absolute_path(paths['results_root'], host_name + '.results_root')}/{run_id}"
    runtime = absolute_path(paths["runtime_prefix"], host_name + ".runtime_prefix")
    services: dict[str, Any] = {}

    for record in records_for_host(plan, host_name):
        if record["role"] == "scheduler":
            service = service_base(plan, record, manifest["runtime_image"]["ref"])
            service.update(
                {
                    **daemon_process(
                        "/opt/icecream/sbin/icecc-scheduler",
                        [
                        "-n", plan["netname"],
                        "-p", str(record["port"]),
                        "-i", host["interface"],
                        "-l", f"/farm/results/{record['log_file']}",
                        "-u", "nobody",
                        "-r", "-v", "-v", "-v",
                        ],
                        ["/farm/results"],
                    ),
                    "volumes": [
                        bind(runtime, "/opt/icecream", True),
                        bind(result_path, "/farm/results"),
                    ],
                    "healthcheck": {
                        "test": [
                            "CMD-SHELL",
                            "pgrep -x icecc-scheduler >/dev/null && "
                            "printf 'listcs\\nquit\\n' | "
                            f"nc -w 2 {host['address']} "
                            f"{manifest['network']['scheduler_control_port']} | "
                            "grep -q '200 done'",
                        ],
                        "interval": "2s",
                        "timeout": "2s",
                        "retries": 30,
                    },
                }
            )
        elif record["role"] == "worker":
            state_path = (
                f"{absolute_path(paths['state_root'], host_name + '.state_root')}/"
                f"{run_id}/{record['service']}"
            )
            service = service_base(plan, record, manifest["runtime_image"]["ref"])
            service.update(
                {
                    **daemon_process(
                        "/opt/icecream/sbin/iceccd",
                        [
                        "-n", plan["netname"],
                        "-N", record["node_name"],
                        "-m", str(record["slots"]),
                        "-p", str(record["port"]),
                        "-s", scheduler_endpoint,
                        "-i", host["interface"],
                        "-b", "/var/lib/icecc",
                        "-l", f"/farm/results/{record['log_file']}",
                        "-u", "nobody",
                        "-v", "-v", "-v",
                        ],
                        ["/farm/results", "/var/lib/icecc"],
                    ),
                    "cap_add": ["SYS_CHROOT", "SETUID", "SETGID"],
                    "volumes": [
                        bind(runtime, "/opt/icecream", True),
                        bind(result_path, "/farm/results"),
                        bind(state_path, "/var/lib/icecc"),
                    ],
                    "healthcheck": {
                        "test": [
                            "CMD-SHELL",
                            "pgrep -x iceccd >/dev/null && "
                            "printf 'listcs\\nquit\\n' | "
                            f"nc -w 2 {scheduler_address} "
                            f"{manifest['network']['scheduler_control_port']} | "
                            "grep -q '200 done'",
                        ],
                        "interval": "2s",
                        "timeout": "2s",
                        "retries": 30,
                    },
                }
            )
        else:
            state_path = (
                f"{absolute_path(paths['state_root'], host_name + '.state_root')}/"
                f"{run_id}/{record['service']}"
            )
            build_path = (
                f"{absolute_path(paths['build_root'], host_name + '.build_root')}/{run_id}/"
                f"{record['service']}"
            )
            environment = manifest["environment_classes"][plan["environment"]]
            # Compose uses the retained local tag; preflight binds that tag to the
            # exact expected image ID before any container is created.
            service = service_base(plan, record, environment["tag"])
            service.update(
                {
                    **daemon_process(
                        "/opt/icecream/sbin/iceccd",
                        [
                        "-n", plan["netname"],
                        "-N", record["node_name"],
                        "--no-remote",
                        "-m", "0",
                        "-s", scheduler_endpoint,
                        "-i", host["interface"],
                        "-b", "/var/lib/icecc",
                        "-l", f"/farm/results/{record['log_file']}",
                        "-u", "nobody",
                        "-v", "-v", "-v",
                        ],
                        ["/farm/build", "/farm/results", "/var/lib/icecc"],
                    ),
                    "cap_add": ["SYS_CHROOT", "SETUID", "SETGID"],
                    "environment": {
                        "ICECC_SCHEDULER": scheduler_endpoint,
                        "ICECREAM_FARM_RUN_ID": run_id,
                    },
                    "volumes": [
                        bind(runtime, "/opt/icecream", True),
                        bind(
                            absolute_path(paths["source_root"], host_name + ".source_root"),
                            "/workspace", True,
                        ),
                        bind(
                            absolute_path(paths["corpus_root"], host_name + ".corpus_root"),
                            "/corpus", True,
                        ),
                        bind(build_path, "/farm/build"),
                        bind(result_path, "/farm/results"),
                        bind(state_path, "/var/lib/icecc"),
                    ],
                }
            )
        services[record["service"]] = service
    return {"name": run_id, "services": services}


def docker_command(runner: HostRunner, args: Sequence[str], *, timeout: int = 30,
                   input_bytes: bytes | None = None,
                   check: bool = False) -> subprocess.CompletedProcess[bytes]:
    return runner.run(["docker", *args], input_bytes=input_bytes, timeout=timeout, check=check)


def ensure_run_directories(manifest: Mapping[str, Any], plan: Mapping[str, Any],
                           host_name: str) -> None:
    host = manifest["hosts"][host_name]
    roles = plan_hosts(plan)[host_name]
    paths = host["paths"]
    directories = [
        f"{absolute_path(paths['results_root'], host_name + '.results_root')}/{plan['run_id']}"
    ]
    if roles & {"worker", "submitter"}:
        state_root = absolute_path(paths["state_root"], host_name + ".state_root")
        for record in records_for_host(plan, host_name):
            if record["role"] in {"worker", "submitter"}:
                directories.append(f"{state_root}/{plan['run_id']}/{record['service']}")
    if "submitter" in roles:
        build_root = absolute_path(paths["build_root"], host_name + ".build_root")
        for record in records_for_host(plan, host_name):
            if record["role"] == "submitter":
                directories.append(f"{build_root}/{plan['run_id']}/{record['service']}")
    runner = HostRunner(host_name, host)
    runner.run(["install", "-d", "-m", "0755", "--", *directories], timeout=30, check=True)


def compose_up(runner: HostRunner, run_id: str, compose: Mapping[str, Any],
               services: Sequence[str]) -> None:
    payload = json.dumps(compose, sort_keys=True).encode("utf-8")
    docker_command(
        runner,
        ["compose", "--project-name", run_id, "-f", "-", "up", "-d", *services],
        input_bytes=payload,
        timeout=600,
        check=True,
    )


def receive_control_reply(connection: socket.socket, marker: bytes) -> bytes:
    result = bytearray()
    while marker not in result:
        chunk = connection.recv(65536)
        if not chunk:
            raise FarmError("scheduler control connection closed before its terminator")
        result.extend(chunk)
        if len(result) > 1024 * 1024:
            raise FarmError("scheduler control reply exceeds 1 MiB")
    return bytes(result)


def scheduler_snapshot(manifest: Mapping[str, Any], plan: Mapping[str, Any]) -> str:
    scheduler = plan["scheduler"]
    address = manifest["hosts"][scheduler["host"]]["address"]
    port = manifest["network"]["scheduler_control_port"]
    try:
        with socket.create_connection((address, port), timeout=3) as connection:
            connection.settimeout(3)
            receive_control_reply(connection, b"200 Use 'help' for help and 'quit' to quit.\n")
            connection.sendall(b"listcs\n")
            response = receive_control_reply(connection, b"200 done\n")
            connection.sendall(b"quit\n")
    except OSError as exc:
        raise FarmError(f"scheduler control probe failed: {exc}") from exc
    return response.decode("utf-8", "replace")


def wait_for_scheduler(manifest: Mapping[str, Any], plan: Mapping[str, Any],
                       timeout_seconds: int) -> None:
    scheduler = plan["scheduler"]
    runner = HostRunner(scheduler["host"], manifest["hosts"][scheduler["host"]])
    deadline = time.monotonic() + timeout_seconds
    last_error = ""
    while time.monotonic() < deadline:
        inspect = docker_command(
            runner,
            ["inspect", "--format", "{{.State.Status}}|{{if .State.Health}}{{.State.Health.Status}}{{end}}", scheduler["container"]],
            timeout=10,
        )
        value = inspect.stdout.decode("utf-8", "replace").strip()
        if inspect.returncode == 0 and value in {"running|healthy", "running|"}:
            try:
                scheduler_snapshot(manifest, plan)
                return
            except FarmError as exc:
                last_error = str(exc)
        else:
            last_error = value or inspect.stderr.decode("utf-8", "replace").strip()
        time.sleep(1)
    raise FarmError(f"scheduler readiness timed out: {last_error}")


def wait_for_cluster(manifest: Mapping[str, Any], plan: Mapping[str, Any],
                     timeout_seconds: int) -> str:
    deadline = time.monotonic() + timeout_seconds
    last_snapshot = ""
    while time.monotonic() < deadline:
        try:
            last_snapshot = scheduler_snapshot(manifest, plan)
        except FarmError:
            time.sleep(1)
            continue
        if not missing_registrations(manifest, plan, last_snapshot):
            return last_snapshot
        time.sleep(1)
    missing = missing_registrations(manifest, plan, last_snapshot)
    raise FarmError(f"cluster readiness timed out; scheduler is missing {missing}")


def exact_remove_container(runner: HostRunner, run_id: str, container: str) -> dict[str, Any]:
    inspect = docker_command(
        runner,
        ["inspect", "--format", "{{index .Config.Labels \"org.icecream.farm.run_id\"}}", container],
        timeout=15,
    )
    if inspect.returncode != 0:
        probe = docker_command(
            runner,
            [
                "container", "ls", "--all",
                "--filter", f"name=^/{container}$",
                "--format", "{{.Names}}",
            ],
            timeout=15,
        )
        if probe.returncode != 0:
            details = (probe.stderr or inspect.stderr).decode("utf-8", "replace").strip()
            raise FarmError(
                f"cannot verify whether exact container {container} exists: {details}"
            )
        names = probe.stdout.decode("utf-8", "replace").splitlines()
        if container in names:
            details = inspect.stderr.decode("utf-8", "replace").strip()
            raise FarmError(f"cannot inspect existing container {container}: {details}")
        return {"container": container, "result": "absent"}
    label = inspect.stdout.decode("utf-8", "replace").strip()
    if label != run_id:
        raise FarmError(
            f"refusing to remove {container}: run label is {label!r}, expected {run_id!r}"
        )
    removed = docker_command(runner, ["rm", "--force", container], timeout=30)
    if removed.returncode != 0:
        raise FarmError(
            f"failed to remove exact container {container}: "
            + removed.stderr.decode("utf-8", "replace").strip()
        )
    return {"container": container, "result": "removed"}


def load_run(run_dir: Path) -> dict[str, Any]:
    run = read_json(run_dir / "run.json")
    expected = run.get("plan", {}).get("run_id")
    if run_dir.name != expected:
        raise FarmError(
            f"run directory leaf {run_dir.name!r} does not match recorded run ID {expected!r}"
        )
    validate_manifest(run["manifest"])
    return run


def collect_run(run_dir: Path, run: dict[str, Any]) -> dict[str, Any]:
    manifest = run["manifest"]
    plan = run["plan"]
    log_root = run_dir / "collected"
    log_root.mkdir(parents=True, exist_ok=True)
    collection: dict[str, Any] = {
        "collected_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "hosts": {},
    }
    for host_name, roles in sorted(plan_hosts(plan).items()):
        runner = HostRunner(host_name, manifest["hosts"][host_name])
        host_record: dict[str, Any] = {"roles": sorted(roles), "containers": {}}
        host_inventory = basic_inventory(runner)
        host_record["inventory"] = host_inventory
        write_json(log_root / f"{host_name}.inventory.json", host_inventory)
        for record in records_for_host(plan, host_name):
            container = record["container"]
            item: dict[str, Any] = {}
            for kind, docker_args in (
                ("inspect", ["inspect", container]),
                ("stats", ["stats", "--no-stream", "--format", "{{json .}}", container]),
                ("container-log", ["logs", "--timestamps", container]),
                ("service-log", ["exec", container, "cat", f"/farm/results/{record['log_file']}"]),
            ):
                result = docker_command(runner, docker_args, timeout=30)
                suffix = "json" if kind in {"inspect", "stats"} else "log"
                output_path = log_root / f"{host_name}.{record['service']}.{kind}.{suffix}"
                output_path.write_bytes(result.stdout + result.stderr)
                item[kind] = {
                    "returncode": result.returncode,
                    "path": str(output_path),
                    "sha256": hashlib.sha256(output_path.read_bytes()).hexdigest(),
                }
            host_record["containers"][container] = item
        collection["hosts"][host_name] = host_record
    try:
        snapshot = scheduler_snapshot(manifest, plan)
    except FarmError as exc:
        snapshot = f"ERROR: {exc}\n"
    snapshot_path = log_root / "scheduler-listcs.txt"
    snapshot_path.write_text(snapshot, encoding="utf-8")
    collection["scheduler_snapshot"] = {
        "path": str(snapshot_path),
        "sha256": hashlib.sha256(snapshot_path.read_bytes()).hexdigest(),
    }
    write_json(log_root / "collection.json", collection)
    ledger_path = run_dir / "evidence-hashes.sha256"
    ledger_rows = []
    for path in sorted(run_dir.rglob("*")):
        if not path.is_file() or path in {run_dir / "run.json", ledger_path}:
            continue
        ledger_rows.append(
            f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(run_dir)}"
        )
    ledger_path.write_text("\n".join(ledger_rows) + "\n", encoding="ascii")
    collection["hash_ledger"] = {
        "path": str(ledger_path),
        "sha256": hashlib.sha256(ledger_path.read_bytes()).hexdigest(),
        "entries": len(ledger_rows),
    }
    run["last_collection"] = collection
    write_json(run_dir / "run.json", run)
    return collection


def inspect_run_state(run: Mapping[str, Any]) -> dict[str, Any]:
    """Read exact run-labelled container and registration state without mutation."""
    manifest = run["manifest"]
    plan = run["plan"]
    containers: list[dict[str, Any]] = []
    all_running = True
    any_present = False
    for record in [plan["scheduler"], *plan["submitters"], *plan["workers"]]:
        runner = HostRunner(record["host"], manifest["hosts"][record["host"]])
        result = docker_command(
            runner,
            [
                "inspect", "--format",
                "{{index .Config.Labels \"org.icecream.farm.run_id\"}}|{{.State.Status}}",
                record["container"],
            ],
            timeout=15,
        )
        value = result.stdout.decode("utf-8", "replace").strip()
        if result.returncode != 0:
            probe = docker_command(
                runner,
                [
                    "container", "ls", "--all",
                    "--filter", f"name=^/{record['container']}$",
                    "--format", "{{.Names}}",
                ],
                timeout=15,
            )
            if probe.returncode != 0:
                item = {
                    **record,
                    "present": None,
                    "state": "unavailable",
                    "error": (probe.stderr or result.stderr).decode(
                        "utf-8", "replace"
                    ).strip(),
                }
                any_present = True
            else:
                names = probe.stdout.decode("utf-8", "replace").splitlines()
                present = record["container"] in names
                item = {
                    **record,
                    "present": present,
                    "state": "uninspectable" if present else "absent",
                }
                any_present = any_present or present
            all_running = False
        else:
            any_present = True
            label, separator, state = value.partition("|")
            item = {
                **record,
                "present": True,
                "state": state if separator else "unknown",
                "label_matches": label == plan["run_id"],
            }
            if not separator or label != plan["run_id"] or state != "running":
                all_running = False
        containers.append(item)
    snapshot = None
    missing: list[str] = []
    if all_running:
        try:
            snapshot = scheduler_snapshot(manifest, plan)
            missing = missing_registrations(manifest, plan, snapshot)
        except FarmError as exc:
            missing = [str(exc)]
    if all_running and not missing:
        state = "ready"
    elif any_present:
        state = "partial"
    else:
        state = "absent"
    return {
        "checked_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "run_id": plan["run_id"],
        "state": state,
        "containers": containers,
        "scheduler_snapshot": snapshot,
        "missing_registrations": missing,
    }


def teardown_run(run_dir: Path, run: dict[str, Any], *, collect_first: bool = True) -> list[dict[str, Any]]:
    errors: list[str] = []
    if collect_first:
        try:
            collect_run(run_dir, run)
        except Exception as exc:
            errors.append(f"collection failed: {exc}")
    manifest = run["manifest"]
    plan = run["plan"]
    outcomes: list[dict[str, Any]] = []
    records = [*plan["submitters"], *plan["workers"], plan["scheduler"]]
    for record in records:
        runner = HostRunner(record["host"], manifest["hosts"][record["host"]])
        try:
            outcome = exact_remove_container(runner, plan["run_id"], record["container"])
        except Exception as exc:
            outcome = {
                "container": record["container"],
                "result": "error",
                "error": str(exc),
            }
            errors.append(f"{record['host']}/{record['container']}: {exc}")
        outcome["host"] = record["host"]
        outcomes.append(outcome)
    run["teardown"] = {
        "completed_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "containers": outcomes,
        "bind_mounted_results_and_state_retained": True,
        "errors": errors,
    }
    run["status"] = "teardown-incomplete" if errors else "down"
    write_json(run_dir / "run.json", run)
    if errors:
        raise FarmError("teardown incomplete: " + " | ".join(errors))
    return outcomes


def new_run_directory(controller_output: Path, run_id: str) -> Path:
    root = controller_output.resolve()
    if str(root) == "/":
        raise FarmError("controller output root cannot be /")
    root.mkdir(parents=True, exist_ok=True)
    run_dir = root / run_id
    try:
        run_dir.mkdir()
    except FileExistsError as exc:
        raise FarmError(
            f"deterministic run directory already exists: {run_dir}; choose a new run label"
        ) from exc
    return run_dir


def launch_run(manifest: dict[str, Any], plan: dict[str, Any], controller_output: Path,
               readiness_timeout: int) -> tuple[Path, dict[str, Any]]:
    existing_dir = controller_output.resolve() / plan["run_id"]
    if existing_dir.is_dir():
        existing = load_run(existing_dir)
        if existing["manifest"] != manifest or existing["plan"] != plan:
            raise FarmError(
                f"existing deterministic run {existing_dir} does not match the requested plan"
            )
        status = inspect_run_state(existing)
        if status["state"] == "ready":
            existing["last_status"] = status
            write_json(existing_dir / "run.json", existing)
            return existing_dir, existing
        raise FarmError(
            f"existing deterministic run {existing_dir} is {status['state']}; "
            "use status/down to reconcile it or choose a new run label"
        )
    report = preflight(manifest, plan)
    if not report["ok"]:
        details = list(report.get("errors", []))
        for host_name, host in report["hosts"].items():
            if host["errors"]:
                details.append(f"{host_name}: " + "; ".join(host["errors"]))
        raise FarmError(
            "preflight failed; no run directories or containers were created: "
            + " | ".join(details)
        )
    run_dir = new_run_directory(controller_output, plan["run_id"])
    run: dict[str, Any] = {
        "schema": 2,
        "status": "preparing",
        "created_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "controller_hostname": socket.getfqdn(),
        "manifest": manifest,
        "manifest_sha256": hashlib.sha256(canonical_json(manifest)).hexdigest(),
        "plan": plan,
        "preflight": report,
        "source_git_head": git_head(HERE),
        "compose": {},
    }
    write_json(run_dir / "run.json", run)

    try:
        composes: dict[str, dict[str, Any]] = {}
        for host_name in sorted(plan_hosts(plan)):
            ensure_run_directories(manifest, plan, host_name)
            compose = render_compose(manifest, plan, host_name)
            composes[host_name] = compose
            compose_path = run_dir / f"compose.{host_name}.json"
            write_json(compose_path, compose)
            run["compose"][host_name] = {
                "path": str(compose_path),
                "sha256": hashlib.sha256(compose_path.read_bytes()).hexdigest(),
            }
        write_json(run_dir / "run.json", run)

        scheduler = plan["scheduler"]
        scheduler_runner = HostRunner(
            scheduler["host"], manifest["hosts"][scheduler["host"]]
        )
        compose_up(
            scheduler_runner,
            plan["run_id"],
            composes[scheduler["host"]],
            [scheduler["service"]],
        )
        wait_for_scheduler(manifest, plan, readiness_timeout)

        for host_name in sorted(plan_hosts(plan)):
            services = [
                record["service"]
                for record in records_for_host(plan, host_name)
                if record["role"] != "scheduler"
            ]
            if services:
                runner = HostRunner(host_name, manifest["hosts"][host_name])
                compose_up(runner, plan["run_id"], composes[host_name], services)
        snapshot = wait_for_cluster(manifest, plan, readiness_timeout)
        snapshot_path = run_dir / "readiness-listcs.txt"
        snapshot_path.write_text(snapshot, encoding="utf-8")
        run["readiness"] = {
            "completed_at": dt.datetime.now(dt.timezone.utc).isoformat(),
            "scheduler_snapshot": str(snapshot_path),
            "expected_nodes": [
                record["node_name"] for record in [*plan["submitters"], *plan["workers"]]
            ],
            "expected_registrations": expected_registrations(manifest, plan),
            "expected_f_slots": {
                record["node_name"]: record["slots"] for record in plan["workers"]
            },
            "route_identities": [record["route_identity"] for record in plan["workers"]],
            "cache_identity_owners": [
                record["cache_identity_owner"] for record in plan["workers"]
            ],
        }
        run["status"] = "ready"
        write_json(run_dir / "run.json", run)
        return run_dir, run
    except BaseException as exc:
        run["launch_error"] = str(exc)
        run["status"] = "launch-failed"
        write_json(run_dir / "run.json", run)
        try:
            teardown_run(run_dir, run, collect_first=True)
        except Exception as cleanup_exc:
            run["cleanup_error"] = str(cleanup_exc)
            write_json(run_dir / "run.json", run)
        raise


def run_acceptance(run_dir: Path, run: dict[str, Any], submitter_index: int = 0) -> dict[str, Any]:
    plan = run["plan"]
    if run.get("status") not in {"ready", "accepted"}:
        raise FarmError(f"run is not ready for acceptance: status={run.get('status')!r}")
    if submitter_index < 0 or submitter_index >= len(plan["submitters"]):
        raise FarmError("submitter index is out of range")
    submitter = plan["submitters"][submitter_index]
    manifest = run["manifest"]
    host_name = submitter["host"]
    runner = HostRunner(host_name, manifest["hosts"][host_name])
    scheduler_address = manifest["hosts"][manifest["scheduler_host"]]["address"]
    scheduler = f"{scheduler_address}:{manifest['network']['scheduler_port']}"
    worker_endpoints = ",".join(
        f"{manifest['hosts'][worker['host']]['address']}:{worker['port']}"
        for worker in plan["workers"]
    )
    source = "/workspace/docker/farm/acceptance/tiny.cpp"
    build = f"/farm/build/acceptance-{plan['environment']}"
    results = "/farm/results"
    script = (HERE / "acceptance.sh").read_bytes()
    stdout_path = run_dir / "acceptance.stdout"
    stderr_path = run_dir / "acceptance.stderr"
    routes_before = peer_route_snapshot(manifest, plan)
    counters_before = interface_counter_snapshot(manifest, plan)
    result = docker_command(
        runner,
        [
            "exec", "-i", submitter["container"], "sh", "-s", "--",
            source, build, results, scheduler, plan["environment"], plan["run_id"],
            worker_endpoints,
        ],
        input_bytes=script,
        timeout=300,
    )
    stdout_path.write_bytes(result.stdout)
    stderr_path.write_bytes(result.stderr)
    routes_after = peer_route_snapshot(manifest, plan)
    counters_after = interface_counter_snapshot(manifest, plan)
    network_ledger = directional_network_ledger(
        manifest, plan, counters_before, counters_after, routes_before, routes_after
    )
    network_ledger_path = run_dir / "network-ledger.json"
    write_json(network_ledger_path, network_ledger)
    if result.returncode != 0:
        raise FarmError(
            f"acceptance compile exited {result.returncode}; retained {stdout_path} and {stderr_path}"
        )
    provenance = docker_command(
        runner,
        ["exec", submitter["container"], "cat", "/farm/results/acceptance-provenance.tsv"],
        timeout=30,
        check=True,
    )
    provenance_path = run_dir / "acceptance-provenance.tsv"
    provenance_path.write_bytes(provenance.stdout)
    if b"program_output\ticecream-farm-ok 42\n" not in provenance.stdout:
        raise FarmError("acceptance provenance lacks the exact program output")
    if b"completion_identity\t" not in provenance.stdout or b"Job ID:" not in provenance.stdout:
        raise FarmError("acceptance provenance lacks the remote completion identity")
    if not any(
        f"selected_worker_endpoint\t{endpoint}\n".encode() in provenance.stdout
        for endpoint in worker_endpoints.split(",")
    ):
        raise FarmError("acceptance provenance does not identify a planned F endpoint")
    acceptance = {
        "completed_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "submitter": submitter,
        "environment": plan["environment"],
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "provenance": str(provenance_path),
        "provenance_sha256": hashlib.sha256(provenance.stdout).hexdigest(),
        "network_ledger": str(network_ledger_path),
        "network_ledger_sha256": hashlib.sha256(network_ledger_path.read_bytes()).hexdigest(),
        "verified_program_output": "icecream-farm-ok 42",
        "remote_assignment_required": True,
    }
    run["acceptance"] = acceptance
    run["status"] = "accepted"
    write_json(run_dir / "run.json", run)
    collect_run(run_dir, run)
    return acceptance


def git_head(path: Path) -> str | None:
    result = subprocess.run(
        ["git", "-C", str(path), "rev-parse", "HEAD"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode:
        return None
    return result.stdout.decode("ascii", "replace").strip()


def prepare(args: argparse.Namespace) -> tuple[dict[str, Any], dict[str, Any]]:
    manifest = load_manifest(
        Path(args.manifest),
        Path(args.mounts) if getattr(args, "mounts", None) else None,
        getattr(args, "node_image_ref", None),
        getattr(args, "node_image_fingerprint", None),
        getattr(args, "scheduler_port", None),
    )
    version_overrides = {
        role: value
        for role, value in {
            "scheduler": getattr(args, "scheduler_version", None),
            "submitter": getattr(args, "submitter_version", None),
            "worker": getattr(args, "worker_version", None),
        }.items()
        if value is not None
    }
    plan = build_plan(
        manifest,
        args.profile,
        args.scenario,
        args.environment,
        args.run_label,
        getattr(args, "submitter", None),
        version_overrides,
    )
    return manifest, plan


def command_validate(args: argparse.Namespace) -> int:
    manifest = load_manifest(
        Path(args.manifest), Path(args.mounts) if args.mounts else None
    )
    print(
        json.dumps(
            {
                "ok": True,
                "manifest": str(Path(args.manifest).resolve()),
                "sha256": hashlib.sha256(canonical_json(manifest)).hexdigest(),
                "wan_hosts": manifest["wan_hosts"],
                "environment_classes": sorted(manifest["environment_classes"]),
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0


def command_plan(args: argparse.Namespace) -> int:
    manifest, plan = prepare(args)
    output = {"manifest_sha256": hashlib.sha256(canonical_json(manifest)).hexdigest(), "plan": plan}
    if args.output:
        write_json(Path(args.output), output)
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0


def command_inventory(args: argparse.Namespace) -> int:
    manifest = load_manifest(Path(args.manifest))
    hosts: dict[str, Any] = {}
    for name, config in manifest["hosts"].items():
        runner = HostRunner(name, config)
        item = basic_inventory(runner)
        item["address"] = config["address"]
        item["configured_interface"] = config.get("interface")
        item["resource_group"] = config["resource_group"]
        hosts[name] = item
    report = {
        "schema": 1,
        "collected_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "network_mode": "local-lan",
        "wan_hosts": [],
        "hosts": hosts,
    }
    if args.output:
        write_json(Path(args.output), report)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if all(item.get("ok") for item in hosts.values()) else 1


def command_preflight(args: argparse.Namespace) -> int:
    manifest, plan = prepare(args)
    report = preflight(manifest, plan)
    if args.output:
        write_json(Path(args.output), report)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["ok"] else 1


def command_up(args: argparse.Namespace) -> int:
    manifest, plan = prepare(args)
    run_dir, _ = launch_run(
        manifest, plan, Path(args.controller_output), args.readiness_timeout
    )
    print(run_dir)
    return 0


def command_accept(args: argparse.Namespace) -> int:
    run_dir = Path(args.run_dir).resolve()
    run = load_run(run_dir)
    acceptance = run_acceptance(run_dir, run, args.submitter_index)
    print(json.dumps(acceptance, indent=2, sort_keys=True))
    return 0


def command_collect(args: argparse.Namespace) -> int:
    run_dir = Path(args.run_dir).resolve()
    run = load_run(run_dir)
    collection = collect_run(run_dir, run)
    print(json.dumps(collection, indent=2, sort_keys=True))
    return 0


def record_absent_run_down(run_dir: Path, run: dict[str, Any],
                           status: Mapping[str, Any], source: str) -> list[dict[str, Any]]:
    if status.get("state") != "absent":
        raise FarmError("only an exactly absent run may be recorded down without teardown")
    outcomes = [
        {"host": record["host"], "container": record["container"], "result": "absent"}
        for record in [*run["plan"]["submitters"], *run["plan"]["workers"], run["plan"]["scheduler"]]
    ]
    run["status"] = "down"
    run["last_status"] = dict(status)
    run["reconciliation"] = {
        "completed_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "source": source,
        "desired": "down",
        "observed": "absent",
        "containers": outcomes,
        "unrelated_resources_touched": False,
    }
    write_json(run_dir / "run.json", run)
    return outcomes


def command_down(args: argparse.Namespace) -> int:
    run_dir = Path(args.run_dir).resolve()
    run = load_run(run_dir)
    status = inspect_run_state(run)
    if status["state"] == "absent":
        outcomes = record_absent_run_down(run_dir, run, status, "down")
    else:
        outcomes = teardown_run(run_dir, run, collect_first=not args.no_collect)
    print(json.dumps(outcomes, indent=2, sort_keys=True))
    return 0


def command_status(args: argparse.Namespace) -> int:
    run_dir = Path(args.run_dir).resolve()
    run = load_run(run_dir)
    status = inspect_run_state(run)
    print(json.dumps(status, indent=2, sort_keys=True))
    return 0 if status["state"] in {"ready", "absent"} else 1


def command_reconcile(args: argparse.Namespace) -> int:
    run_dir = Path(args.run_dir).resolve()
    run = load_run(run_dir)
    status = inspect_run_state(run)
    if args.desired == "down":
        if status["state"] == "absent":
            record_absent_run_down(run_dir, run, status, "reconcile")
        else:
            teardown_run(run_dir, run, collect_first=not args.no_collect)
        print(json.dumps({"desired": "down", "state": "absent"}, indent=2))
        return 0
    if status["state"] != "ready":
        raise FarmError(
            f"cannot reconcile {run['plan']['run_id']} to up from {status['state']} "
            "without replacing retained evidence; run exact down, then choose a new run label"
        )
    print(json.dumps({"desired": "up", "state": "ready"}, indent=2))
    return 0


def command_run(args: argparse.Namespace) -> int:
    manifest, plan = prepare(args)
    existing_dir = Path(args.controller_output).resolve() / plan["run_id"]
    if existing_dir.is_dir():
        existing = load_run(existing_dir)
        if existing["manifest"] == manifest and existing["plan"] == plan:
            if existing.get("status") == "down" and existing.get("acceptance"):
                print(existing_dir)
                return 0
    run_dir: Path | None = None
    run: dict[str, Any] | None = None
    failure: BaseException | None = None
    try:
        run_dir, run = launch_run(
            manifest, plan, Path(args.controller_output), args.readiness_timeout
        )
        run_acceptance(run_dir, run, args.submitter_index)
    except BaseException as exc:
        failure = exc
    finally:
        if run_dir is not None:
            try:
                latest = load_run(run_dir)
                if latest.get("status") != "down":
                    teardown_run(run_dir, latest, collect_first=True)
            except Exception as cleanup_exc:
                if failure is None:
                    failure = cleanup_exc
                else:
                    print(f"cleanup also failed: {cleanup_exc}", file=sys.stderr)
    if failure is not None:
        raise failure
    assert run_dir is not None
    print(run_dir)
    return 0


def host_process_argv(runner: HostRunner, command: Sequence[str],
                      timeout_seconds: int = 1800) -> list[str]:
    if runner.config["transport"]["mode"] == "local":
        return ["timeout", "--signal=TERM", "--kill-after=5s", f"{timeout_seconds}s", *command]
    bounded = [
        "timeout", "--signal=TERM", "--kill-after=5s", f"{timeout_seconds}s", *command
    ]
    return runner.prefix() + [shlex.join(bounded)]


def command_sync_image(args: argparse.Namespace) -> int:
    manifest = load_manifest(Path(args.manifest))
    if args.environment:
        image = manifest["environment_classes"][args.environment]
        image_ref = image["tag"]
    else:
        image = manifest["runtime_image"]
        image_ref = manifest["runtime_image"]["ref"]
    expected_fingerprint = image["expected_fingerprint"]
    if args.source not in manifest["hosts"]:
        raise FarmError(f"unknown source host {args.source!r}")
    source = HostRunner(args.source, manifest["hosts"][args.source])
    verify_transport_identity(manifest, args.source)
    source_inspect = docker_command(
        source, ["image", "inspect", "--format", "{{json .}}", image_ref], check=True
    )
    try:
        source_details = json.loads(source_inspect.stdout)
    except json.JSONDecodeError as exc:
        raise FarmError(f"{args.source}: Docker returned invalid image metadata") from exc
    source_id = source_details.get("Id")
    source_fingerprint = image_fingerprint(source_details)
    if source_fingerprint != expected_fingerprint:
        raise FarmError(
            f"source image content mismatch: expected {expected_fingerprint}, "
            f"found {source_fingerprint}"
        )
    outcomes = []
    for destination_name in args.destination:
        if destination_name == args.source:
            raise FarmError("source and destination hosts must differ")
        if destination_name not in manifest["hosts"]:
            raise FarmError(f"unknown destination host {destination_name!r}")
        destination = HostRunner(destination_name, manifest["hosts"][destination_name])
        verify_transport_identity(manifest, destination_name)
        existing = docker_command(
            destination, ["image", "inspect", "--format", "{{json .}}", image_ref]
        )
        if existing.returncode == 0:
            try:
                existing_details = json.loads(existing.stdout)
            except json.JSONDecodeError as exc:
                raise FarmError(
                    f"{destination_name}: Docker returned invalid image metadata"
                ) from exc
            existing_fingerprint = image_fingerprint(existing_details)
            if existing_fingerprint == source_fingerprint:
                outcomes.append(
                    {
                        "source": args.source,
                        "destination": destination_name,
                        "image": image_ref,
                        "source_engine_id": source_id,
                        "destination_engine_id": existing_details.get("Id"),
                        "content_fingerprint": existing_fingerprint,
                        "result": "already-present",
                    }
                )
                continue
        source_process = subprocess.Popen(
            host_process_argv(source, ["docker", "image", "save", image_ref]),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert source_process.stdout is not None
        destination_process = subprocess.Popen(
            host_process_argv(destination, ["docker", "image", "load"]),
            stdin=source_process.stdout,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        source_process.stdout.close()
        destination_stdout, destination_stderr = destination_process.communicate()
        source_stderr = source_process.stderr.read() if source_process.stderr else b""
        source_returncode = source_process.wait()
        if source_returncode or destination_process.returncode:
            raise FarmError(
                f"image stream {args.source}->{destination_name} failed: "
                f"save={source_returncode} load={destination_process.returncode}; "
                f"{source_stderr.decode('utf-8', 'replace')} "
                f"{destination_stderr.decode('utf-8', 'replace')}"
            )
        verify = docker_command(
            destination, ["image", "inspect", "--format", "{{json .}}", image_ref], check=True
        )
        try:
            destination_details = json.loads(verify.stdout)
        except json.JSONDecodeError as exc:
            raise FarmError(
                f"{destination_name}: Docker returned invalid image metadata after load"
            ) from exc
        destination_id = destination_details.get("Id")
        destination_fingerprint = image_fingerprint(destination_details)
        if destination_fingerprint != source_fingerprint:
            raise FarmError(
                f"destination {destination_name} image fingerprint "
                f"{destination_fingerprint} != source {source_fingerprint}"
            )
        outcomes.append(
            {
                "source": args.source,
                "destination": destination_name,
                "image": image_ref,
                "source_engine_id": source_id,
                "destination_engine_id": destination_id,
                "content_fingerprint": destination_fingerprint,
                "result": "loaded",
                "load_output": destination_stdout.decode("utf-8", "replace").strip(),
            }
        )
    print(json.dumps(outcomes, indent=2, sort_keys=True))
    return 0


def add_manifest_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    parser.add_argument(
        "--mounts",
        help="JSON file containing only per-host runtime/source/corpus/build/results/state overrides",
    )


def add_plan_args(parser: argparse.ArgumentParser) -> None:
    add_manifest_args(parser)
    parser.add_argument("--profile", default="conservative_nas_submitter")
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--environment", default="debian-gcc")
    parser.add_argument("--run-label", required=True)
    parser.add_argument(
        "--submitter",
        action="append",
        help="override submitter host; repeat for multiple C containers",
    )
    parser.add_argument("--node-image-ref")
    parser.add_argument("--node-image-fingerprint")
    parser.add_argument("--scheduler-version")
    parser.add_argument("--submitter-version")
    parser.add_argument("--worker-version")
    parser.add_argument(
        "--scheduler-port",
        type=int,
        help="explicit scheduler port; the control port is the following port",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate = subparsers.add_parser("validate", help="validate the static LAN manifest")
    add_manifest_args(validate)
    validate.set_defaults(handler=command_validate)

    plan = subparsers.add_parser("plan", help="render a deterministic scenario plan without SSH")
    add_plan_args(plan)
    plan.add_argument("--output")
    plan.set_defaults(handler=command_plan)

    inventory = subparsers.add_parser("inventory", help="read current facts from all LAN hosts")
    inventory.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    inventory.add_argument("--output")
    inventory.set_defaults(handler=command_inventory)

    preflight_parser = subparsers.add_parser(
        "preflight", help="verify selected hosts, paths, images and binaries without mutation"
    )
    add_plan_args(preflight_parser)
    preflight_parser.add_argument("--output")
    preflight_parser.set_defaults(handler=command_preflight)

    up = subparsers.add_parser("up", help="preflight and launch an exact run")
    add_plan_args(up)
    up.add_argument("--controller-output", required=True)
    up.add_argument("--readiness-timeout", type=positive_int, default=90)
    up.set_defaults(handler=command_up)

    accept = subparsers.add_parser("accept", help="submit and verify the tiny real compile")
    accept.add_argument("--run-dir", required=True)
    accept.add_argument("--submitter-index", type=int, default=0)
    accept.set_defaults(handler=command_accept)

    collect = subparsers.add_parser("collect", help="retain per-node logs and current resource facts")
    collect.add_argument("--run-dir", required=True)
    collect.set_defaults(handler=command_collect)

    down = subparsers.add_parser("down", help="remove only exact run-labeled containers")
    down.add_argument("--run-dir", required=True)
    down.add_argument("--no-collect", action="store_true")
    down.set_defaults(handler=command_down)

    status = subparsers.add_parser(
        "status", help="read exact run-labelled container and scheduler state"
    )
    status.add_argument("--run-dir", required=True)
    status.set_defaults(handler=command_status)

    reconcile = subparsers.add_parser(
        "reconcile", help="idempotently verify up or converge exact run containers to down"
    )
    reconcile.add_argument("--run-dir", required=True)
    reconcile.add_argument("--desired", choices=["up", "down"], required=True)
    reconcile.add_argument("--no-collect", action="store_true")
    reconcile.set_defaults(handler=command_reconcile)

    run = subparsers.add_parser(
        "run", help="launch, accept, collect and always perform exact container teardown"
    )
    add_plan_args(run)
    run.add_argument("--controller-output", required=True)
    run.add_argument("--readiness-timeout", type=positive_int, default=90)
    run.add_argument("--submitter-index", type=int, default=0)
    run.set_defaults(handler=command_run)

    sync = subparsers.add_parser(
        "sync-image", help="explicitly stream one exact image between physical hosts"
    )
    sync.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    group = sync.add_mutually_exclusive_group(required=True)
    group.add_argument("--environment", choices=[
        "debian-gcc", "fedora-clang-libcxx", "linuxbrew", "conan-gcc"
    ])
    group.add_argument("--runtime", action="store_true")
    sync.add_argument("--source", required=True)
    sync.add_argument("--destination", action="append", required=True)
    sync.set_defaults(handler=command_sync_image)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.handler(args)
    except FarmError as exc:
        print(f"farm: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
