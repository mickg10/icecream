#!/usr/bin/env python3
"""Run and authenticate an ordered, real C1F1 S8 live batch.

The shell lifecycle is the existing ``p50compilee2e-run.sh`` product gate:
one scheduler, C/F daemons, cache services, and one client/F relationship
remain alive while the batch is compiled.  This module only authenticates the
manifest before launch and turns the product's captured rows into the current
S8 live-curve schema.  It never accepts caller-supplied measurements.
"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import os
import platform
import re
import shlex
import signal
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Mapping

try:
    from . import s6_live_route_acceptance as action_parser
    from . import s8_depth_runner as depth_runner
    from . import s8_predictive_live_normalizer as normalizer
    from .s8_schema import CORPORA, PROFILES, REGIMES, SPLITS
except ImportError:  # pragma: no cover
    import s6_live_route_acceptance as action_parser
    import s8_depth_runner as depth_runner
    import s8_predictive_live_normalizer as normalizer
    from s8_schema import CORPORA, PROFILES, REGIMES, SPLITS


SCHEMA = "icecream-s8-real-c1f1-live-runner-v2"
RAW_II_PROFILE = "RAW_II"
PRODUCT_PROFILES = (*PROFILES, "GRZ", RAW_II_PROFILE)
TOPOLOGY = "C1F1/100000"
PARALLEL_TOPOLOGY = "C1F20/40"
TOPOLOGIES = frozenset((TOPOLOGY, PARALLEL_TOPOLOGY))


def _transfer_accounting(product_profile: str) -> dict[str, object]:
    """Name the byte basis without changing either live measurement path."""
    if product_profile == RAW_II_PROFILE:
        return {
            "basis": "framed_application_wire_bytes",
            "c_to_f_frames": ["COMPILE_FILE", "FILE_CHUNK", "END"],
            "f_to_c_frames": ["COMPILE_RESULT", "FILE_CHUNK", "END"],
        }
    return {
        "basis": "source_stage_plus_returned_object_payload",
        "c_to_f_frames": ["P50_SOURCE_STAGE"],
        "f_to_c_frames": ["RETURNED_OBJECT"],
    }


def _measurement_descriptor(harness_profile: str,
                            product_profile: str | None) -> tuple[str, str, bool]:
    """Keep the method arm distinct from the S8 input/profile label."""
    effective = product_profile or (
        "GRZ" if harness_profile == "GRZ_RESIDUAL" else harness_profile)
    if effective not in PRODUCT_PROFILES:
        _fail("product_profile:undeclared")
    raw_ii = effective == RAW_II_PROFILE
    return (RAW_II_PROFILE if raw_ii else harness_profile, effective, not raw_ii)


def _plan_identity_profile(harness_profile: str,
                           product_profile: str | None) -> str:
    """Return the profile carried by plans and retained experiment records."""
    _method, effective, _eligible = _measurement_descriptor(harness_profile,
                                                             product_profile)
    return effective if effective == RAW_II_PROFILE else harness_profile
RELATIONSHIP_COUNT = {TOPOLOGY: 1, PARALLEL_TOPOLOGY: 20}
SLOTS_PER_F = {TOPOLOGY: 1, PARALLEL_TOPOLOGY: 2}
SCRIPT = Path(__file__).resolve().parents[1] / "unittests/p50compilee2e-run.sh"
HEX64 = re.compile(r"^[0-9a-f]{64}$")
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX32 = re.compile(r"^[0-9a-f]{32}$")
SAFE = re.compile(r"^[A-Za-z0-9_.-]+$")
TIMESTAMP = re.compile(r"^\d{8}T\d{6}Z$")
ISO_UTC = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
EXTERNAL_EXECUTION_ID = re.compile(r"^[A-Za-z0-9_.:-]{8,128}$")
EXTERNAL_CONTAINER_ID = re.compile(r"^[0-9a-f]{12,128}$")
IMAGE_ID = re.compile(r"^sha256:[0-9a-f]{64}$")
PINNED_IMAGE = "icecream/farm-node:ubuntu22-gcc11-boost174"
DEFAULT_CONTAINER_BIND_ROOT = Path("/tanksmall")
DEFAULT_CONTAINER_TEMP_ROOT = Path("/tmp")
DEFAULT_CONTAINER_WORK_ROOT = Path("/p5")
DEFAULT_REPORTED_WORKDIR = DEFAULT_CONTAINER_WORK_ROOT / "p50compilee2e.run"
CONTAINER_DAEMON_USER = "icecc"
CONTAINER_DAEMON_GROUP = "icecc"
SCORED_CARET_WORKAROUND = "0"
LOOPBACK_EXECUTION_SCOPE = "loopback_correctness_only"
EXTERNAL_FARM_EXECUTION_SCOPE = "external_farm_timing"
EXTERNAL_FARM_SCHEMA = "icecream-s8-external-farm-v1"
EXTERNAL_FARM_AUTHORITY_SCHEMA = "icecream-s8-external-farm-authority-v1"
EXTERNAL_FARM_RECEIPT_SCHEMA = "icecream-s8-external-farm-receipt-v1"
EXTERNAL_FARM_HOSTS = ("q3", "q2", "research6", "research7")
EXTERNAL_FARM_ROLE_PATHS = (
    "scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc",
    "client/icecc-create-env", "cache/icecc-cache-service")
EXTERNAL_FARM_PINNED_IMAGE = "icecream/p50-farm-node:ubuntu22-gcc11-boost174-bdb55d"
EXTERNAL_FARM_IMAGE_INDEX = "sha256:bdb55d4287a473e3ebfbaa7715a50ee670659777278b8d84c350724e6fa8de58"
EXTERNAL_FARM_IMAGE_CONFIG = "sha256:fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b"
ROLE_PLACEMENT_SCHEMA = "icecream-s8-role-placement-v1"
# Keep the runner's emitted identity aligned with the current intake schema.
CALIBRATION_METADATA_FIELDS = frozenset(normalizer.CALIBRATION_METADATA_KEYS)
HOST_DESCRIPTOR_SCHEMA = "icecream-s8-measurement-host-descriptor-v1"
OUTPUT_CONTRACT_SCHEMA = "icecream-s8-output-compile-contract-v1"
HOST_MACHINE_ID = Path("/etc/machine-id")
HOST_DMI_PRODUCT_UUID = Path("/sys/class/dmi/id/product_uuid")
HOST_DMI_BOARD_SERIAL = Path("/sys/class/dmi/id/board_serial")
HOST_CPUINFO = Path("/proc/cpuinfo")


class LiveRunnerError(ValueError):
    """A live run is missing product evidence or has mismatched identity."""


def _fail(reason: str) -> None:
    raise LiveRunnerError(reason)


@dataclass(frozen=True)
class ExternalFarmFinalization:
    """Authenticated output supplied by an external-farm transport.

    The transport owns execution and retention.  It must hand the mature
    runner the exact stdout/workdir pair it captured, plus immutable file
    descriptors for the placement manifest and host/image authority.  No
    remote command or transport behavior is part of this boundary.
    """

    manifest_path: Path
    manifest_sha256: str
    authority_path: Path
    authority_sha256: str
    workdir: Path
    stdout_path: Path
    stdout_sha256: str
    stdout_bytes: int
    receipt_path: Path
    receipt_sha256: str
    receipt_bytes: int


def _external_input(value: object) -> ExternalFarmFinalization:
    """Normalize the small public adapter contract without accepting extras."""
    if isinstance(value, ExternalFarmFinalization):
        return value
    if isinstance(value, Mapping):
        required = {"manifest_path", "manifest_sha256", "authority_path",
                    "authority_sha256", "workdir", "stdout_path",
                    "stdout_sha256", "stdout_bytes", "receipt_path",
                    "receipt_sha256", "receipt_bytes"}
        if set(value) != required:
            _fail("external_farm.input:fields_invalid")
        try:
            return ExternalFarmFinalization(
                manifest_path=Path(value["manifest_path"]),
                manifest_sha256=value["manifest_sha256"],
                authority_path=Path(value["authority_path"]),
                authority_sha256=value["authority_sha256"],
                workdir=Path(value["workdir"]),
                stdout_path=Path(value["stdout_path"]),
                stdout_sha256=value["stdout_sha256"],
                stdout_bytes=value["stdout_bytes"],
                receipt_path=Path(value["receipt_path"]),
                receipt_sha256=value["receipt_sha256"],
                receipt_bytes=value["receipt_bytes"])
        except (TypeError, ValueError) as exc:
            raise LiveRunnerError("external_farm.input:fields_invalid") from exc
    _fail("external_farm.input:fields_invalid")
    raise AssertionError("unreachable")


def _canonical(value: object) -> bytes:
    return normalizer.canonical_bytes(value)


def _sha(path: Path) -> tuple[str, int]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise LiveRunnerError(f"file_unavailable:{path}") from exc
    if path.is_symlink() or not path.is_file() or info.st_nlink != 1:
        _fail(f"file_not_private:{path}")
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
            size += len(block)
    return digest.hexdigest(), size


def _private_location(path: Path, label: str) -> None:
    """Reject descriptor paths reached through a symlinked directory."""
    if not path.is_absolute():
        _fail(f"{label}:descriptor_invalid")
    for ancestor in (path, *path.parents):
        try:
            if ancestor.is_symlink():
                _fail(f"{label}:descriptor_invalid")
        except OSError as exc:
            raise LiveRunnerError(f"{label}:descriptor_invalid") from exc


def _hex(value: object, label: str) -> str:
    if not isinstance(value, str) or HEX64.fullmatch(value) is None or int(value, 16) == 0:
        _fail(f"{label}:invalid_digest")
    return value.lower()


def _external_scheduler_host(value: object) -> str:
    """Accept a routable scheduler name, never an accidental local endpoint."""
    if not isinstance(value, str) or not value or any(char.isspace() for char in value):
        _fail("external_farm.scheduler.host:invalid")
    if value.lower() in {"localhost", "localhost.localdomain"}:
        _fail("external_farm.scheduler.host:loopback")
    try:
        address = ipaddress.ip_address(value)
    except ValueError:
        address = None
    if address is not None and (address.is_loopback or address.is_unspecified or
                                address.is_multicast or address.is_link_local):
        _fail("external_farm.scheduler.host:loopback")
    return value


def load_external_farm_manifest(path: Path, expected_sha256: str,
                                suite: str) -> tuple[dict[str, object], str, int]:
    """Load an authenticated C/F placement and scheduler authority.

    This is intentionally a manifest-only boundary.  The current product
    shell starts a co-resident scheduler/F farm and therefore cannot consume
    this authority yet; accepting the manifest here makes that missing launch
    input explicit instead of allowing local elapsed time into calibration.
    """
    _private_location(path, "external_farm.manifest")
    if (not path.is_absolute() or path.is_symlink() or not path.is_file() or
            path.stat().st_nlink != 1):
        _fail("external_farm.manifest:unavailable")
    expected = _hex(expected_sha256, "external_farm.manifest_sha256")
    digest, size = _sha(path)
    if digest != expected:
        _fail("external_farm.manifest:sha256_mismatch")
    try:
        value = normalizer.parse_json(path.read_bytes(), "external_farm.manifest")
    except (normalizer.NormalizationError, OSError) as exc:
        raise LiveRunnerError("external_farm.manifest:invalid_json") from exc
    if not isinstance(value, dict) or set(value) != {
            "schema", "suite", "scheduler", "client", "workers", "role_placement"}:
        _fail("external_farm.manifest:fields_invalid")
    if value.get("schema") != EXTERNAL_FARM_SCHEMA or value.get("suite") != suite:
        _fail("external_farm.manifest:identity_invalid")
    scheduler = value.get("scheduler")
    if (not isinstance(scheduler, dict) or set(scheduler) != {"host", "port"}):
        _fail("external_farm.scheduler:fields_invalid")
    host = _external_scheduler_host(scheduler.get("host"))
    port = scheduler.get("port")
    if type(port) is not int or not 1 <= port <= 65535:
        _fail("external_farm.scheduler.port:invalid")
    client = value.get("client")
    if not isinstance(client, dict) or set(client) != {"host_digest"}:
        _fail("external_farm.client:fields_invalid")
    client_digest = _hex(client.get("host_digest"), "external_farm.client.host_digest")
    try:
        placement = normalizer._validate_role_placement(value.get("role_placement"), "live")
    except normalizer.NormalizationError as exc:
        raise LiveRunnerError(str(exc)) from exc
    if (placement is None or placement.get("mode") != "external_farm" or
            placement.get("c_host_digest") != client_digest or
            placement.get("roles_disjoint") is not True or
            placement.get("timing_eligible") is not True):
        _fail("external_farm.role_placement:invalid")
    workers = value.get("workers")
    expected_relationships = RELATIONSHIP_COUNT.get(suite)
    if not isinstance(workers, list) or expected_relationships is None or \
            len(workers) != expected_relationships:
        _fail("external_farm.workers:count_invalid")
    seen_relationships: set[int] = set()
    seen_services: set[str] = set()
    normalized_workers: list[dict[str, object]] = []
    for index, worker in enumerate(workers):
        if not isinstance(worker, dict) or set(worker) != {
                "relationship", "service", "host_digest"}:
            _fail(f"external_farm.workers:{index}:fields_invalid")
        relationship = worker.get("relationship")
        service = worker.get("service")
        worker_digest = _hex(worker.get("host_digest"),
                             f"external_farm.workers:{index}.host_digest")
        expected_service = ("p50-f" if suite == TOPOLOGY
                            else f"p50-f-{relationship}")
        if (type(relationship) is not int or relationship in seen_relationships or
                not 0 <= relationship < expected_relationships or
                not isinstance(service, str) or service != expected_service or
                service in seen_services or worker_digest == client_digest):
            _fail(f"external_farm.workers:{index}:identity_invalid")
        seen_relationships.add(relationship)
        seen_services.add(service)
        normalized_workers.append({"relationship": relationship, "service": service,
                                   "host_digest": worker_digest})
    if seen_relationships != set(range(expected_relationships)):
        _fail("external_farm.workers:relationship_set_incomplete")
    physical_hosts = placement.get("f_host_digests")
    worker_hosts = [item["host_digest"] for item in normalized_workers]
    if (not isinstance(physical_hosts, list) or
            any(host not in physical_hosts for host in worker_hosts) or
            set(physical_hosts) != set(worker_hosts)):
        _fail("external_farm.role_placement:worker_binding_mismatch")
    normalized = {"schema": EXTERNAL_FARM_SCHEMA, "suite": suite,
                  "scheduler": {"host": host, "port": port},
                  "client": {"host_digest": client_digest},
                  "workers": normalized_workers, "role_placement": placement}
    return normalized, digest, size


def _private_descriptor(value: object, label: str) -> tuple[Path, str, int]:
    """Authenticate a transport-owned absolute regular-file descriptor."""
    if not isinstance(value, dict) or set(value) != {"path", "sha256", "bytes"}:
        _fail(f"{label}:descriptor_invalid")
    path_value = value.get("path")
    if not isinstance(path_value, str):
        _fail(f"{label}:descriptor_invalid")
    path = Path(path_value)
    _private_location(path, label)
    if (not path.is_absolute() or type(value.get("bytes")) is not int or
            value["bytes"] <= 0):
        _fail(f"{label}:descriptor_invalid")
    expected = _hex(value.get("sha256"), f"{label}.sha256")
    observed, size = _sha(path)
    if observed != expected or size != value["bytes"]:
        _fail(f"{label}:hash_mismatch")
    return path, observed, size


def _load_external_farm_authority(path: Path, expected_sha256: str,
                                  manifest: dict[str, object],
                                  suite: str) -> tuple[dict[str, object], str, int]:
    """Load the canonical four-host authority emitted by S8 capture."""
    _private_location(path, "external_farm.authority")
    if (not path.is_absolute() or path.is_symlink() or not path.is_file() or
            path.stat().st_nlink != 1):
        _fail("external_farm.authority:unavailable")
    expected = _hex(expected_sha256, "external_farm.authority_sha256")
    digest, size = _sha(path)
    if digest != expected:
        _fail("external_farm.authority:sha256_mismatch")
    try:
        value = normalizer.parse_json(path.read_bytes(), "external_farm.authority")
    except (normalizer.NormalizationError, OSError) as exc:
        raise LiveRunnerError("external_farm.authority:invalid_json") from exc
    if (not isinstance(value, dict) or
            set(value) != {"schema", "hosts", "placements"} or
            value.get("schema") != EXTERNAL_FARM_AUTHORITY_SCHEMA):
        _fail("external_farm.authority:fields_invalid")
    hosts = value.get("hosts")
    if not isinstance(hosts, dict) or set(hosts) != set(EXTERNAL_FARM_HOSTS):
        _fail("external_farm.authority:host_set_invalid")
    normalized_hosts: dict[str, object] = {}
    for host in EXTERNAL_FARM_HOSTS:
        item = hosts[host]
        required = {"target", "lan", "hostname", "descriptor", "physical_host_digest",
                    "boot_id_digest", "image", "binaries", "cpu_count", "idle",
                    "cpu_sample", "cpu_sample_digest"}
        if not isinstance(item, dict) or set(item) != required:
            _fail(f"external_farm.authority:{host}:fields_invalid")
        if (not isinstance(item["target"], str) or not item["target"] or
                not isinstance(item["lan"], str) or not item["lan"] or
                not isinstance(item["hostname"], str) or not item["hostname"]):
            _fail(f"external_farm.authority:{host}:identity_invalid")
        physical = _hex(item["physical_host_digest"],
                        f"external_farm.authority:{host}.physical_host_digest")
        boot = _hex(item["boot_id_digest"],
                    f"external_farm.authority:{host}.boot_id_digest")
        descriptor_path, descriptor_sha, descriptor_bytes = _private_descriptor(
            item["descriptor"], f"external_farm.authority:{host}.descriptor")
        try:
            descriptor_value = normalizer.parse_json(
                descriptor_path.read_bytes(), f"external_farm.authority:{host}.descriptor")
        except (normalizer.NormalizationError, OSError) as exc:
            raise LiveRunnerError(
                f"external_farm.authority:{host}.descriptor:invalid_json") from exc
        descriptor_facts = (descriptor_value.get("facts")
                           if isinstance(descriptor_value, dict) else None)
        if (not isinstance(descriptor_value, dict) or
                set(descriptor_value) != {"schema", "facts"} or
                descriptor_value.get("schema") != "icecream-s8-external-host-descriptor-v1" or
                not isinstance(descriptor_facts, dict) or
                set(descriptor_facts) != {"machine_id_sha256", "boot_id_sha256",
                                          "nic_identity_sha256", "cpu_vendor_sha256",
                                          "cpu_model_sha256", "cpu_count",
                                          "physical_host_digest"}):
            _fail(f"external_farm.authority:{host}.descriptor:fields_invalid")
        for field in ("machine_id_sha256", "boot_id_sha256", "nic_identity_sha256",
                      "cpu_vendor_sha256", "cpu_model_sha256", "physical_host_digest"):
            _hex(descriptor_facts[field], f"external_farm.authority:{host}.descriptor.{field}")
        physical_input = {"machine_id_sha256": descriptor_facts["machine_id_sha256"],
                          "nic_identity_sha256": descriptor_facts["nic_identity_sha256"]}
        expected_physical = hashlib.sha256(_canonical(physical_input)).hexdigest()
        if (descriptor_facts["physical_host_digest"] != expected_physical or
                descriptor_facts["physical_host_digest"] != physical or
                descriptor_facts["boot_id_sha256"] != boot or
                type(descriptor_facts["cpu_count"]) is not int or
                descriptor_facts["cpu_count"] != item["cpu_count"]):
            _fail(f"external_farm.authority:{host}:host_identity_mismatch")
        image = item["image"]
        expected_image = (EXTERNAL_FARM_IMAGE_CONFIG if host == "research7"
                          else EXTERNAL_FARM_IMAGE_INDEX)
        if (not isinstance(image, dict) or
                set(image) != {"reference", "image_id", "architecture", "os", "created"} or
                image.get("reference") != EXTERNAL_FARM_PINNED_IMAGE or
                image.get("image_id") != expected_image or
                image.get("architecture") != "amd64" or image.get("os") != "linux" or
                not isinstance(image.get("created"), str) or not image["created"]):
            _fail(f"external_farm.authority:{host}:image_invalid")
        binaries = item["binaries"]
        if not isinstance(binaries, dict) or set(binaries) != set(EXTERNAL_FARM_ROLE_PATHS):
            _fail(f"external_farm.authority:{host}:binary_identity_incomplete")
        normalized_binaries = {role: _hex(binaries[role],
                                           f"external_farm.authority:{host}.binaries.{role}")
                              for role in EXTERNAL_FARM_ROLE_PATHS}
        idle = item["idle"]
        sample = item["cpu_sample"]
        baseline_digest = (_hex(idle.get("baseline_digest"),
                                f"external_farm.authority:{host}.baseline_digest")
                           if isinstance(idle, dict) else "")
        sample_digest = (_hex(item["cpu_sample_digest"],
                              f"external_farm.authority:{host}.cpu_sample_digest")
                         if isinstance(item["cpu_sample_digest"], str) else "")
        if (not isinstance(idle, dict) or set(idle) !=
                {"status", "load_1m", "captured_at", "baseline_digest"} or
                idle.get("status") not in {"PASS", "HOLD"} or
                type(idle.get("load_1m")) not in (int, float) or idle["load_1m"] < 0 or
                not isinstance(idle.get("captured_at"), str) or
                ISO_UTC.fullmatch(idle.get("captured_at", "")) is None or
                not isinstance(sample, dict) or set(sample) !=
                {"before", "after", "duration_seconds", "idle_percent"} or
                any(not isinstance(sample[field], str) or not sample[field]
                    for field in ("before", "after")) or
                type(sample["duration_seconds"]) not in (int, float) or
                not .5 <= float(sample["duration_seconds"]) <= 5 or
                type(sample["idle_percent"]) not in (int, float) or
                not 0 <= float(sample["idle_percent"]) <= 100 or
                idle.get("status") != ("PASS" if float(idle.get("load_1m", 1)) <= 0.50 and
                                        float(sample.get("idle_percent", 0)) >= 95.0 else "HOLD") or
                sample_digest !=
                hashlib.sha256((_canonical(sample) + b"\n")).hexdigest()):
            _fail(f"external_farm.authority:{host}:idle_invalid")
        normalized_hosts[host] = {
            **item, "physical_host_digest": physical, "boot_id_digest": boot,
            "descriptor": {"path": str(descriptor_path), "sha256": descriptor_sha,
                            "bytes": descriptor_bytes},
            "image": {key: image[key] for key in
                       ("reference", "image_id", "architecture", "os", "created")},
            "binaries": normalized_binaries,
        }
    reference_binaries = normalized_hosts["q3"]["binaries"]
    if len({normalized_hosts[host]["physical_host_digest"]
            for host in EXTERNAL_FARM_HOSTS}) != len(EXTERNAL_FARM_HOSTS):
        _fail("external_farm.authority:physical_hosts_not_unique")
    if len({normalized_hosts[host]["boot_id_digest"]
            for host in EXTERNAL_FARM_HOSTS}) != len(EXTERNAL_FARM_HOSTS):
        _fail("external_farm.authority:boot_ids_not_unique")
    if any(normalized_hosts[host]["binaries"] != reference_binaries
           for host in EXTERNAL_FARM_HOSTS):
        _fail("external_farm.authority:per_host_binary_mismatch")
    placements = value.get("placements")
    if not isinstance(placements, dict) or set(placements) != set(TOPOLOGIES):
        _fail("external_farm.authority:placements_invalid")
    mappings: dict[str, list[str]] = {}
    for placement_suite, count in ((TOPOLOGY, 1), (PARALLEL_TOPOLOGY, 20)):
        entry = placements[placement_suite]
        mapping = entry.get("relationship_hosts") if isinstance(entry, dict) else None
        if (not isinstance(entry, dict) or set(entry) != {"relationship_hosts"} or
                not isinstance(mapping, list) or len(mapping) != count or
                any(host not in EXTERNAL_FARM_HOSTS[1:] for host in mapping)):
            _fail(f"external_farm.authority:{placement_suite}:mapping_invalid")
        if any(normalized_hosts[host]["idle"]["status"] != "PASS" for host in set(mapping)):
            _fail(f"external_farm.authority:{placement_suite}:host_not_idle")
        mappings[placement_suite] = list(mapping)
    if normalized_hosts["q3"]["idle"]["status"] != "PASS":
        _fail("external_farm.authority:q3:not_idle")
    mapping = mappings[suite]
    manifest_workers = manifest["workers"]
    if (len(manifest_workers) != len(mapping) or
            manifest["scheduler"]["host"] != normalized_hosts["q3"]["lan"]):
        _fail("external_farm.authority:manifest_mapping_mismatch")
    expected_client = normalized_hosts["q3"]["physical_host_digest"]
    if manifest["client"]["host_digest"] != expected_client:
        _fail("external_farm.authority:client_identity_mismatch")
    worker_digests = []
    for index, (worker, host) in enumerate(zip(manifest_workers, mapping, strict=True)):
        expected_service = "p50-f" if suite == TOPOLOGY else f"p50-f-{index}"
        if (worker["relationship"] != index or worker["service"] != expected_service or
                worker["host_digest"] != normalized_hosts[host]["physical_host_digest"]):
            _fail(f"external_farm.authority:{suite}:worker_mapping_mismatch:{index}")
        worker_digests.append(worker["host_digest"])
    placement = manifest["role_placement"]
    expected_f = list(dict.fromkeys(worker_digests))
    if (placement["c_host_digest"] != expected_client or
            placement["scheduler_host_digest"] != normalized_hosts["q3"]["physical_host_digest"] or
            placement["f_host_digests"] != expected_f):
        _fail("external_farm.authority:role_placement_mismatch")
    normalized = {"schema": EXTERNAL_FARM_AUTHORITY_SCHEMA,
                  "hosts": normalized_hosts,
                  "placements": {name: {"relationship_hosts": mappings[name]}
                                 for name in mappings}}
    return normalized, digest, size


def _external_farm_binding(external: ExternalFarmFinalization, suite: str) -> dict[str, object]:
    """Authenticate all external inputs and return immutable source facts."""
    if not all(isinstance(path, Path) for path in
               (external.manifest_path, external.authority_path, external.workdir,
                external.stdout_path, external.receipt_path)):
        _fail("external_farm.input:fields_invalid")
    manifest, manifest_sha, manifest_bytes = load_external_farm_manifest(
        external.manifest_path, external.manifest_sha256, suite)
    authority, authority_sha, authority_bytes = _load_external_farm_authority(
        external.authority_path, external.authority_sha256, manifest, suite)
    if external.manifest_path.resolve() == external.authority_path.resolve():
        _fail("external_farm:manifest_authority_must_differ")
    stdout_path, stdout_sha, stdout_bytes = _private_descriptor(
        {"path": str(external.stdout_path), "sha256": external.stdout_sha256,
         "bytes": external.stdout_bytes}, "external_farm.stdout")
    try:
        stdout = stdout_path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise LiveRunnerError("external_farm.stdout:invalid") from exc
    receipt_path, receipt_sha, receipt_bytes = _private_descriptor(
        {"path": str(external.receipt_path), "sha256": external.receipt_sha256,
         "bytes": external.receipt_bytes}, "external_farm.receipt")
    try:
        receipt = normalizer.parse_json(receipt_path.read_bytes(), "external_farm.receipt")
    except (normalizer.NormalizationError, OSError) as exc:
        raise LiveRunnerError("external_farm.receipt:invalid_json") from exc
    if (not isinstance(receipt, dict) or set(receipt) != {
            "schema", "mode", "suite", "manifest_sha256", "authority_sha256",
            "stdout", "workdir", "execution"} or receipt.get("schema") != EXTERNAL_FARM_RECEIPT_SCHEMA or
            receipt.get("mode") != "external_farm" or receipt.get("suite") != suite or
            receipt.get("manifest_sha256") != manifest_sha or
            receipt.get("authority_sha256") != authority_sha or
            receipt.get("stdout") != {"path": str(stdout_path), "sha256": stdout_sha,
                                       "bytes": stdout_bytes}):
        _fail("external_farm.receipt:identity_mismatch")
    execution = receipt.get("execution")
    identity_fields = {"host", "service", "physical_host_digest", "boot_id_digest",
                       "pid", "container_id"}
    def execution_identity(value: object, label: str, host: str, service: str) -> None:
        expected_fields = identity_fields | ({"relationship"} if label.startswith("workers:") else set())
        if (not isinstance(value, dict) or set(value) != expected_fields or
                value.get("host") != host or value.get("service") != service or
                value.get("physical_host_digest") != authority["hosts"][host]["physical_host_digest"] or
                value.get("boot_id_digest") != authority["hosts"][host]["boot_id_digest"] or
                type(value.get("pid")) is not int or not 1 <= value["pid"] <= 4_194_304 or
                not isinstance(value.get("container_id"), str) or
                EXTERNAL_CONTAINER_ID.fullmatch(value["container_id"]) is None):
            _fail(f"external_farm.receipt:{label}:identity_invalid")
    if not isinstance(execution, dict) or set(execution) != {
            "execution_id", "scheduler", "client", "workers", "started_at",
            "finished_at", "start_marker", "end_marker", "artifacts"}:
        _fail("external_farm.receipt:execution_invalid")
    execution_id = execution.get("execution_id")
    if (not isinstance(execution_id, str) or
            EXTERNAL_EXECUTION_ID.fullmatch(execution_id) is None):
        _fail("external_farm.receipt:execution_id_invalid")
    execution_identity(execution.get("scheduler"), "scheduler", "q3", "p50-scheduler")
    execution_identity(execution.get("client"), "client", "q3", "p50-c")
    mapping = authority["placements"][suite]["relationship_hosts"]
    workers = execution.get("workers")
    if not isinstance(workers, list) or len(workers) != len(mapping):
        _fail("external_farm.receipt:workers_invalid")
    for index, (worker, host) in enumerate(zip(workers, mapping, strict=True)):
        service = "p50-f" if suite == TOPOLOGY else f"p50-f-{index}"
        execution_identity(worker, f"workers:{index}", host, service)
        if not isinstance(worker, dict) or worker.get("relationship") != index:
            _fail(f"external_farm.receipt:workers:{index}:mapping_invalid")
    if (not isinstance(execution["started_at"], str) or
            ISO_UTC.fullmatch(execution["started_at"]) is None or
            not isinstance(execution["finished_at"], str) or
            ISO_UTC.fullmatch(execution["finished_at"]) is None):
        _fail("external_farm.receipt:execution_time_invalid")
    try:
        started_at = datetime.strptime(execution["started_at"], "%Y-%m-%dT%H:%M:%SZ").replace(
            tzinfo=timezone.utc)
        finished_at = datetime.strptime(execution["finished_at"], "%Y-%m-%dT%H:%M:%SZ").replace(
            tzinfo=timezone.utc)
    except ValueError as exc:
        raise LiveRunnerError("external_farm.receipt:execution_time_invalid") from exc
    now = datetime.now(timezone.utc)
    if (finished_at < started_at or started_at > now + timedelta(seconds=30) or
            finished_at > now + timedelta(seconds=30) or
            (finished_at - started_at).total_seconds() > MAX_TIMEOUT_SECONDS):
        _fail("external_farm.receipt:execution_time_invalid")
    used_hosts = ["q3", *dict.fromkeys(mapping)]
    for host in used_hosts:
        try:
            captured_at = datetime.strptime(
                authority["hosts"][host]["idle"]["captured_at"],
                "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
        except (TypeError, ValueError) as exc:
            raise LiveRunnerError(f"external_farm.authority:{host}:idle_invalid") from exc
        age = (started_at - captured_at).total_seconds()
        if age < -30 or age > 300:
            _fail(f"external_farm.authority:{host}:capture_stale")
    artifacts = execution.get("artifacts")
    if artifacts != {"collection": "complete", "cleanup": "complete"}:
        _fail("external_farm.receipt:artifacts_incomplete")
    marker_pattern = re.compile(
        r"^S8_EXTERNAL_FARM_EXECUTION execution_id=([^ ]+) manifest_sha256=([0-9a-f]{64}) "
        r"authority_sha256=([0-9a-f]{64}) phase=(start|end) timestamp=(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)$")
    marker_rows = []
    for line in stdout.splitlines():
        match = marker_pattern.fullmatch(line)
        if match is not None:
            marker_rows.append((match, line))
    if len(marker_rows) != 2:
        _fail("external_farm.receipt:lifecycle_markers_missing")
    phases = [match.group(4) for match, _line in marker_rows]
    if phases != ["start", "end"]:
        _fail("external_farm.receipt:lifecycle_markers_invalid")
    for marker_index, (match, line) in enumerate(marker_rows):
        if (match.group(1) != execution_id or match.group(2) != manifest_sha or
                match.group(3) != authority_sha or
                match.group(5) != (execution["started_at"] if marker_index == 0
                                   else execution["finished_at"])):
            _fail("external_farm.receipt:lifecycle_marker_identity_mismatch")
    if (execution.get("start_marker") != marker_rows[0][1] or
            execution.get("end_marker") != marker_rows[1][1]):
        _fail("external_farm.receipt:lifecycle_marker_receipt_mismatch")
    if (not external.workdir.is_absolute() or external.workdir.is_symlink() or
            not external.workdir.is_dir()):
        _fail("external_farm.workdir:invalid")
    try:
        if external.workdir.resolve(strict=True) != external.workdir:
            _fail("external_farm.workdir:invalid")
        if receipt.get("workdir") != str(external.workdir):
            _fail("external_farm.receipt:workdir_mismatch")
        marker = [line.split("=", 1)[1] for line in stdout.splitlines()
                  if line.startswith("S7_WORKDIR=")]
        observed_workdir = Path(marker[0]) if len(marker) == 1 else None
        if observed_workdir is None or observed_workdir.resolve() != external.workdir.resolve():
            _fail("external_farm.workdir:stdout_mismatch")
    except OSError as exc:
        raise LiveRunnerError("external_farm.workdir:unavailable") from exc
    result = {
        "manifest": {"path": "product-evidence/external-farm-manifest.json",
                      "sha256": manifest_sha, "bytes": manifest_bytes},
        "authority": {"path": "product-evidence/external-farm-authority.json",
                       "sha256": authority_sha, "bytes": authority_bytes},
        "stdout": {"path": "product-evidence/product-output.log",
                    "sha256": stdout_sha, "bytes": stdout_bytes},
        "receipt": {"path": "product-evidence/external-farm-receipt.json",
                     "sha256": receipt_sha, "bytes": receipt_bytes},
        "role_placement": manifest["role_placement"],
        "hosts": authority["hosts"],
        "manifest_value": manifest,
        "authority_value": authority,
    }
    return result


def _external_calibration_metadata(binding: dict[str, object], work: Path,
                                   preparation: dict[str, Any],
                                   rows: list[dict[str, Any]],
                                   observations: list[dict[str, Any]],
                                   suite: str) -> dict[str, str]:
    """Derive calibration identity from every selected physical host/image."""
    mapping = binding["authority_value"]["placements"][suite]["relationship_hosts"]
    used = ["q3", *dict.fromkeys(mapping)]
    hosts = binding["hosts"]
    closure = [{"host": host, "physical_host_digest": hosts[host]["physical_host_digest"],
                "boot_id_digest": hosts[host]["boot_id_digest"],
                "descriptor": {"sha256": hosts[host]["descriptor"]["sha256"],
                               "bytes": hosts[host]["descriptor"]["bytes"]},
                "image": hosts[host]["image"],
                "binaries": hosts[host]["binaries"]} for host in sorted(used)]
    host_closure = hashlib.sha256(_canonical({"schema": "icecream-s8-host-closure-v1",
                                              "hosts": closure})).hexdigest()
    image_closure = hashlib.sha256(_canonical({"schema": "icecream-s8-image-closure-v1",
                                               "hosts": [{"host": item["host"],
                                                          "image": item["image"]}
                                                         for item in closure]})).hexdigest()
    return {
        "product_image_digest": image_closure,
        "toolchain_digest": _authenticated_toolchain_digest(work, preparation),
        "output_contract_digest": _output_contract_digest(rows, observations, suite),
        "host_digest": host_closure,
        "ordered_input_class": "ordered",
    }


def _co_resident_role_placement(host_digest: str, suite: str) -> dict[str, object]:
    """Describe the local runner's wire-only co-resident placement."""
    return {
        "schema": ROLE_PLACEMENT_SCHEMA,
        "mode": "co_resident_loopback",
        "c_host_digest": host_digest,
        "scheduler_host_digest": host_digest,
        "f_host_digests": [host_digest],
        "roles_disjoint": False,
        "timing_eligible": False,
    }


def _read_host_fact(path: Path, label: str) -> str:
    """Read one stable host fact without retaining the raw identifier."""
    try:
        info = path.lstat()
        if path.is_symlink() or not path.is_file() or info.st_nlink != 1:
            _fail(f"host_descriptor:{label}:not_private_file")
        raw = path.read_text(encoding="utf-8").strip()
    except (OSError, UnicodeError) as exc:
        raise LiveRunnerError(f"host_descriptor:{label}:unavailable") from exc
    if not raw or "\x00" in raw:
        _fail(f"host_descriptor:{label}:empty_or_binary")
    return raw


def _hash_host_fact(label: str, raw: str) -> str:
    return hashlib.sha256(("icecream-s8-host-fact-v1\0" + label + "\0" + raw).encode(
        "utf-8")).hexdigest()


def _stable_host_facts() -> dict[str, object]:
    """Capture stable physical-host facts before any container is launched."""
    machine_id = _read_host_fact(HOST_MACHINE_ID, "machine_id")
    dmi_path: Path | None = None
    dmi_value = ""
    for candidate, label in ((HOST_DMI_PRODUCT_UUID, "dmi_product_uuid"),
                             (HOST_DMI_BOARD_SERIAL, "dmi_board_serial")):
        try:
            dmi_value = _read_host_fact(candidate, label)
        except LiveRunnerError:
            continue
        dmi_path = candidate
        break
    try:
        cpuinfo = _read_host_fact(HOST_CPUINFO, "cpuinfo")
    except LiveRunnerError:
        cpuinfo = ""
    cpu_vendor = next((line.split(":", 1)[1].strip() for line in cpuinfo.splitlines()
                       if line.startswith("vendor_id") and ":" in line), "")
    cpu_model = next((line.split(":", 1)[1].strip() for line in cpuinfo.splitlines()
                      if line.startswith("model name") and ":" in line), "")
    cpu_vendor = cpu_vendor or platform.machine()
    cpu_model = cpu_model or platform.processor() or platform.machine()
    if not cpu_vendor or not cpu_model:
        _fail("host_descriptor:cpu_identity:unavailable")
    cpu_count = os.cpu_count()
    if type(cpu_count) is not int or cpu_count <= 0:
        _fail("host_descriptor:cpu_count:unavailable")
    return {
        "machine_id_sha256": _hash_host_fact("machine_id", machine_id),
        "dmi_source": dmi_path.name if dmi_path is not None else "unavailable",
        "dmi_identity_sha256": (_hash_host_fact(dmi_path.name, dmi_value)
                                if dmi_path is not None else None),
        "cpu_vendor_sha256": _hash_host_fact("cpu_vendor", cpu_vendor),
        "cpu_model_sha256": _hash_host_fact("cpu_model", cpu_model),
        "cpu_count": cpu_count,
    }


def capture_host_descriptor(path: Path) -> dict[str, object]:
    """Persist an exact, redacted measurement-host descriptor once."""
    if not path.is_absolute() or path.exists() or path.is_symlink():
        _fail("host_descriptor:output_invalid")
    value = {"schema": HOST_DESCRIPTOR_SCHEMA, "facts": _stable_host_facts()}
    _write_new(path, _canonical(value) + b"\n")
    digest, size = _sha(path)
    return {"path": str(path), "sha256": digest, "bytes": size}


def _load_host_descriptor(path: Path) -> tuple[dict[str, object], str, int]:
    """Authenticate a previously captured descriptor and return its facts."""
    try:
        private = (path.is_absolute() and not path.is_symlink() and path.is_file() and
                   path.stat().st_nlink == 1)
    except OSError:
        private = False
    if not private:
        _fail("host_descriptor:unavailable")
    digest, size = _sha(path)
    try:
        value = normalizer.parse_json(path.read_bytes(), "host_descriptor")
    except (normalizer.NormalizationError, OSError) as exc:
        raise LiveRunnerError("host_descriptor:invalid_json") from exc
    if not isinstance(value, dict) or set(value) != {"schema", "facts"} or \
            value.get("schema") != HOST_DESCRIPTOR_SCHEMA:
        _fail("host_descriptor:fields_invalid")
    facts = value.get("facts")
    required = {"machine_id_sha256", "dmi_source", "dmi_identity_sha256",
                "cpu_vendor_sha256", "cpu_model_sha256", "cpu_count"}
    if not isinstance(facts, dict) or set(facts) != required:
        _fail("host_descriptor:facts_invalid")
    for field in required - {"dmi_source", "cpu_count", "dmi_identity_sha256"}:
        _hex(facts.get(field), f"host_descriptor.facts.{field}")
    if (facts.get("dmi_source") not in {HOST_DMI_PRODUCT_UUID.name,
                                         HOST_DMI_BOARD_SERIAL.name, "unavailable"} or
            (facts.get("dmi_source") != "unavailable" and
             not HEX64.fullmatch(facts.get("dmi_identity_sha256", ""))) or
            type(facts.get("cpu_count")) is not int or facts["cpu_count"] <= 0 or
            (facts.get("dmi_source") == "unavailable" and
             facts.get("dmi_identity_sha256") is not None)):
        _fail("host_descriptor:facts_invalid")
    return value, digest, size


def _authenticated_toolchain_digest(work: Path,
                                    preparation: dict[str, Any]) -> str:
    """Bind the product environment archive bytes to calibration identity."""
    expected_sha = _hex(preparation.get("archive_sha256"),
                        "toolchain.archive_sha256")
    expected_bytes = preparation.get("archive_bytes")
    if type(expected_bytes) is not int or expected_bytes <= 0:
        _fail("toolchain.archive_bytes:invalid")
    try:
        candidates = sorted(path for path in (work / "toolchain").iterdir()
                            if path.is_file() and not path.is_symlink() and
                            path.suffix in {".gz", ".xz", ".zst", ".bz2", ".tgz", ".tar"})
    except OSError as exc:
        raise LiveRunnerError("toolchain:archive_unavailable") from exc
    if len(candidates) != 1:
        _fail("toolchain:archive_ambiguous_or_missing")
    observed_sha, observed_bytes = _sha(candidates[0])
    if observed_sha != expected_sha or observed_bytes != expected_bytes:
        _fail("toolchain:archive_binding_mismatch")
    return observed_sha


def _output_contract_digest(rows: list[dict[str, Any]],
                            observations: list[dict[str, Any]], suite: str) -> str:
    """Hash the authenticated compile/output contract used by measured rows."""
    if not rows or len(rows) != len(observations):
        _fail("output_contract:row_count_mismatch")
    for row, observed in zip(rows, observations, strict=True):
        if not all(field in row for field in ("compile_db", "compile_db_sha256",
                                               "compile_source", "compile_output")):
            _fail("output_contract:compile_binding_missing")
        _hex(row["compile_db_sha256"], "output_contract.compile_db")
        if observed.get("remote_compile", True) is not True:
            _fail("output_contract:remote_compile_required")
    contract = {
        "schema": OUTPUT_CONTRACT_SCHEMA,
        "compile": {"authority": "authenticated_compile_database",
                     "output_operand": "single_-o",
                     "source_binding": "compile_entry_file", "required": True},
        "measurement_window": "compile+result_return",
        "output": {"remote_compile_required": True,
                    "identity": "returned_remote_object_sha256",
                    "bytes": "returned_remote_object_bytes"},
    }
    return hashlib.sha256(_canonical(contract)).hexdigest()


def _calibration_metadata(*, runtime_image: dict[str, str],
                          preparation: dict[str, Any], work: Path,
                          rows: list[dict[str, Any]],
                          observations: list[dict[str, Any]], suite: str,
                          host_descriptor: Path) -> dict[str, str]:
    """Derive the five identity fields from authenticated run authorities."""
    if (set(runtime_image) != {"reference", "image_id", "architecture", "os", "created"} or
            IMAGE_ID.fullmatch(runtime_image.get("image_id", "")) is None or
            runtime_image.get("architecture") != "amd64" or
            runtime_image.get("os") != "linux"):
        _fail("calibration_metadata:product_image_invalid")
    _value, host_digest, _host_bytes = _load_host_descriptor(host_descriptor)
    return {
        "product_image_digest": runtime_image["image_id"].removeprefix("sha256:"),
        "toolchain_digest": _authenticated_toolchain_digest(work, preparation),
        "output_contract_digest": _output_contract_digest(rows, observations, suite),
        "host_digest": host_digest,
        "ordered_input_class": "ordered",
    }


def _authenticated_host_binding(path: Path,
                                launch_identity: dict[str, Any]) -> tuple[str, int]:
    """Require the descriptor captured before launch to survive the run."""
    _value, digest, size = _load_host_descriptor(path)
    launch_host = launch_identity.get("host_descriptor")
    if (not isinstance(launch_host, dict) or
            set(launch_host) != {"sha256", "bytes"} or
            launch_host.get("sha256") != digest or
            launch_host.get("bytes") != size):
        _fail("calibration_metadata:host_descriptor_changed_during_run")
    return digest, size


def _compile_entry_output_operand(entry: object) -> tuple[Path, str] | None:
    """Return the command working directory and its single ``-o`` operand."""
    if not isinstance(entry, dict):
        return None
    directory, command = entry.get("directory"), entry.get("command")
    if not isinstance(directory, str) or not os.path.isabs(directory) or not isinstance(command, str):
        return None
    try:
        tokens = shlex.split(command)
    except ValueError:
        return None
    outputs: list[str] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-o":
            if index + 1 >= len(tokens):
                return None
            outputs.append(tokens[index + 1])
            index += 2
            continue
        if token.startswith("-o") and len(token) > 2:
            outputs.append(token[2:])
        index += 1
    if len(outputs) != 1 or not outputs[0]:
        return None
    return Path(directory), outputs[0]


def compile_entry_output(entry: object) -> Path | None:
    """Resolve the object actually named by the compile command's ``-o``.

    CMake's optional compile-database ``output`` field can be relative to the
    top-level build tree even when ``directory`` is a subdirectory.  The
    command itself runs in ``directory`` and is the execution authority used
    to create the retained corpus, so its single ``-o`` operand is the only
    reliable live binding.
    """
    parsed = _compile_entry_output_operand(entry)
    if parsed is None:
        return None
    directory, operand = parsed
    output = Path(operand)
    return (output if output.is_absolute() else directory / output).resolve()


def compile_entry_predictive_relative(entry: object) -> str | None:
    """Project a command output to the corpus path used by preprocess_corpus.py."""
    parsed = _compile_entry_output_operand(entry)
    if parsed is None:
        return None
    directory, operand = parsed
    output = Path(operand)
    relative = os.path.relpath(output, directory) if output.is_absolute() else operand
    relative = relative.lstrip("./")
    path = Path(relative)
    if (not relative or path.is_absolute() or any(part in ("", ".", "..") for part in path.parts)):
        return None
    return (path.with_suffix(".ii") if path.suffix == ".o" else
            Path(f"{path.as_posix()}.ii")).as_posix()


DEPTH_COUNTS = {"100": 100, "200": 200}
MIN_TIMEOUT_SECONDS = 180
MAX_TIMEOUT_SECONDS = 4 * 60 * 60


def selected_count(depth: str, full_count: int | None = None) -> int:
    if depth in DEPTH_COUNTS:
        return DEPTH_COUNTS[depth]
    if depth == "full" and isinstance(full_count, int) and full_count > 0:
        return full_count
    _fail("depth:full_count_required" if depth == "full" else "depth:undeclared")
    raise AssertionError("unreachable")


def derive_timeout(tu_count: int, passes: int = 2, warm: bool = False,
                   timeout_seconds: int | None = None) -> int:
    """Bound a real run by selected work, including warm prewarm work."""
    if type(tu_count) is not int or tu_count <= 0 or passes not in (1, 2):
        _fail("timeout:arguments_invalid")
    work_passes = passes + (1 if warm else 0)
    if timeout_seconds is not None:
        if (type(timeout_seconds) is not int or
                not MIN_TIMEOUT_SECONDS <= timeout_seconds <= MAX_TIMEOUT_SECONDS):
            _fail("timeout:override_invalid")
        return timeout_seconds
    # 12 seconds/TU/pass is deliberately conservative for a remote compile;
    # cap runaway manifests while allowing a multi-hour full-corpus run.
    return min(MAX_TIMEOUT_SECONDS, max(MIN_TIMEOUT_SECONDS, 30 + tu_count * 12 * work_passes))


def load_batch_manifest(path: Path, expected_count: int = 100) -> list[dict[str, Any]]:
    """Authenticate the exact source snapshot list used by the shell gate."""
    _sha(path)
    try:
        values = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise LiveRunnerError("batch_manifest:invalid_jsonl") from exc
    if len(values) != expected_count:
        _fail(f"batch_manifest:expected_{expected_count}_rows")
    rows: list[dict[str, Any]] = []
    seen: set[str] = set()
    compile_databases: dict[str, tuple[str, list[object]]] = {}
    compile_match_indexes: dict[str, dict[tuple[Path, Path], list[dict[str, Any]]]] = {}
    for ordinal, value in enumerate(values):
        if not isinstance(value, dict):
            _fail(f"batch_manifest:{ordinal}:object_required")
        required = {"tu_id", "source", "source_relative", "sha256", "predictive_input"}
        compile_fields = {"compile_db", "compile_db_sha256", "compile_source", "compile_output"}
        if not required.issubset(value) or set(value) - required - compile_fields:
            _fail(f"batch_manifest:{ordinal}:fields_invalid")
        present_compile_fields = compile_fields.intersection(value)
        if present_compile_fields and present_compile_fields != compile_fields:
            _fail(f"batch_manifest:{ordinal}:compile_binding_incomplete")
        tu_id, source = value["tu_id"], value["source"]
        source_relative = value["source_relative"]
        if (not isinstance(tu_id, str) or SAFE.fullmatch(tu_id) is None or tu_id in seen
                or not isinstance(source, str) or not os.path.isabs(source)
                or not isinstance(source_relative, str) or not source_relative or
                os.path.isabs(source_relative) or any(part in ("", ".", "..")
                                                       for part in Path(source_relative).parts)):
            _fail(f"batch_manifest:{ordinal}:identity_invalid")
        digest, size = _sha(Path(source))
        if value.get("sha256") != digest:
            _fail(f"batch_manifest:{ordinal}:source_digest_mismatch")
        predictive_input = value.get("predictive_input")
        if (not isinstance(predictive_input, dict) or
                set(predictive_input) != {"ordinal", "path", "source_relative", "sha256", "bytes"} or
                predictive_input["ordinal"] != ordinal or
                not isinstance(predictive_input["path"], str) or not os.path.isabs(predictive_input["path"]) or
                not isinstance(predictive_input["source_relative"], str) or
                not predictive_input["source_relative"] or os.path.isabs(predictive_input["source_relative"]) or
                any(part in ("", ".", "..") for part in Path(predictive_input["source_relative"]).parts)):
            _fail(f"batch_manifest:{ordinal}:predictive_input_invalid")
        payload_sha = _hex(predictive_input["sha256"], f"batch_manifest:{ordinal}.predictive_input.sha256")
        payload_bytes = predictive_input["bytes"]
        if type(payload_bytes) is not int or payload_bytes <= 0:
            _fail(f"batch_manifest:{ordinal}:predictive_input.bytes_invalid")
        observed_payload_sha, observed_payload_bytes = _sha(Path(predictive_input["path"]))
        if observed_payload_sha != payload_sha or observed_payload_bytes != payload_bytes:
            _fail(f"batch_manifest:{ordinal}:predictive_input_digest_mismatch")
        predictive_input = {"ordinal": ordinal, "path": predictive_input["path"],
                            "source_relative": predictive_input["source_relative"],
                            "sha256": payload_sha, "bytes": payload_bytes}
        row = {"ordinal": ordinal, "tu_id": tu_id, "source": source,
               "source_relative": source_relative,
               "sha256": digest, "bytes": size, "predictive_input": predictive_input}
        if present_compile_fields:
            db_value = value["compile_db"]
            db_sha = _hex(value["compile_db_sha256"],
                          f"batch_manifest:{ordinal}.compile_db_sha256")
            compile_source = value["compile_source"]
            compile_output = value["compile_output"]
            if (not isinstance(db_value, str) or not os.path.isabs(db_value) or
                    not isinstance(compile_source, str) or not os.path.isabs(compile_source) or
                    not isinstance(compile_output, str) or not os.path.isabs(compile_output) or
                    compile_source != source):
                _fail(f"batch_manifest:{ordinal}:compile_binding_invalid")
            expected_output_relative = Path(predictive_input["source_relative"]).with_suffix(".o")
            output_parts = Path(compile_output).parts
            expected_parts = expected_output_relative.parts
            if (len(output_parts) < len(expected_parts) or
                    output_parts[-len(expected_parts):] != expected_parts):
                _fail(f"batch_manifest:{ordinal}:compile_output_predictive_mismatch")
            db_path = Path(db_value)
            cached = compile_databases.get(str(db_path))
            if cached is None:
                observed_db_sha, _ = _sha(db_path)
                try:
                    entries = json.loads(db_path.read_text())
                except (OSError, UnicodeError, json.JSONDecodeError) as exc:
                    raise LiveRunnerError(
                        f"batch_manifest:{ordinal}:compile_db_invalid") from exc
                if not isinstance(entries, list):
                    _fail(f"batch_manifest:{ordinal}:compile_db_invalid")
                cached = (observed_db_sha, entries)
                compile_databases[str(db_path)] = cached
            if cached[0] != db_sha:
                _fail(f"batch_manifest:{ordinal}:compile_db_digest_mismatch")
            source_path = Path(compile_source).resolve()
            output_path = Path(compile_output).resolve()
            match_index = compile_match_indexes.get(str(db_path))
            if match_index is None:
                match_index = {}
                for entry in cached[1]:
                    if (not isinstance(entry, dict) or
                            not isinstance(entry.get("directory"), str)):
                        continue
                    entry_source = entry.get("file")
                    if not isinstance(entry_source, str):
                        continue
                    resolved_output = compile_entry_output(entry)
                    if (resolved_output is None or
                            not isinstance(entry.get("command"), str)):
                        continue
                    key = (Path(entry_source).resolve(), resolved_output)
                    match_index.setdefault(key, []).append(entry)
                compile_match_indexes[str(db_path)] = match_index
            matches = match_index.get((source_path, output_path), [])
            if len(matches) != 1:
                _fail(f"batch_manifest:{ordinal}:compile_entry_not_unique")
            row.update({"compile_db": str(db_path), "compile_db_sha256": db_sha,
                        "compile_source": str(source_path),
                        "compile_output": str(output_path)})
        rows.append(row)
        seen.add(tu_id)
    return rows


def payload_descriptors(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [dict(row["predictive_input"]) for row in rows]


def payload_descriptor_digest(rows: list[dict[str, Any]], source_manifest_sha256: str) -> str:
    """Use the same source-manifest-plus-ordered-input contract as S8."""
    aggregate = {"source_manifest_sha256": source_manifest_sha256,
                 "inputs": payload_descriptors(rows)}
    return hashlib.sha256(_canonical(aggregate)).hexdigest()


def load_predictive_plan(path: Path, *, corpus: str, profile: str, regime: str,
                         depth: str) -> tuple[dict[str, Any], list[dict[str, Any]], str]:
    """Authenticate the depth plan that owns the predictive `.ii` sequence."""
    plan_sha, _ = _sha(path)
    try:
        value = json.loads(path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise LiveRunnerError("predictive_plan:invalid_json") from exc
    if not isinstance(value, dict) or value.get("schema") != "icecream-s8-depth-run-plan-v1":
        _fail("predictive_plan:schema_invalid")
    if (value.get("cell") != {"corpus": corpus, "profile": profile, "regime": regime} or
            value.get("split") != SPLITS[corpus]):
        _fail("predictive_plan:cell_mismatch")
    request = value.get("request")
    if not isinstance(request, dict):
        _fail("predictive_plan:request_missing")
    requested_depth = request.get("depth")
    if ((depth in {"full", "repeat-full"} and requested_depth != depth) or
            (depth not in {"full", "repeat-full"} and requested_depth != int(depth))):
        _fail("predictive_plan:depth_mismatch")
    source = value.get("source_manifest")
    inputs = value.get("inputs")
    if (not isinstance(source, dict) or set(source) != {"path", "sha256", "bytes", "entries"} or
            not isinstance(inputs, list) or not inputs):
        _fail("predictive_plan:source_or_inputs_invalid")
    if (not isinstance(source["path"], str) or not os.path.isabs(source["path"]) or
            _hex(source.get("sha256"), "predictive_plan.source_manifest.sha256") != source["sha256"] or
            type(source.get("bytes")) is not int or source["bytes"] <= 0 or
            type(source.get("entries")) is not int or source["entries"] <= 0):
        _fail("predictive_plan:source_descriptor_invalid")
    source_path = Path(source["path"])
    source_sha, source_bytes = _sha(source_path)
    if source_sha != source["sha256"] or source_bytes != source["bytes"]:
        _fail("predictive_plan:source_manifest_mismatch")
    source_root = value.get("source_root")
    if (not isinstance(source_root, str) or not os.path.isabs(source_root) or
            not Path(source_root).is_dir() or Path(source_root).is_symlink()):
        _fail("predictive_plan:source_root_invalid")
    try:
        source_lines = [line.strip() for line in source_path.read_text().splitlines()]
    except (OSError, UnicodeError) as exc:
        raise LiveRunnerError("predictive_plan:source_manifest_unreadable") from exc
    if source["entries"] != len(source_lines):
        _fail("predictive_plan:source_manifest_entries_mismatch")
    if depth not in {"full", "repeat-full"} and len(inputs) != int(depth):
        _fail("predictive_plan:input_count_mismatch")
    try:
        inventory, _manifest_facts = depth_runner._manifest_inputs(
            source_path, Path(source_root), "source_manifest")
        expected_inputs, expected_selection = depth_runner.select_inputs(
            inventory, int(depth) if depth.isdigit() else depth)
    except depth_runner.DepthPlanError as exc:
        raise LiveRunnerError(str(exc)) from exc
    request = value.get("request")
    if not isinstance(request, dict):
        _fail("predictive_plan:request_invalid")
    declared_selection = request.get("source_selection")
    if declared_selection is None:
        if len(expected_inputs) > len(inventory):
            _fail("predictive_plan:source_selection_required_for_repetition")
    elif declared_selection != expected_selection:
        _fail("predictive_plan:source_selection_mismatch")
    for ordinal, item in enumerate(inputs):
        if (not isinstance(item, dict) or set(item) != {"ordinal", "path", "source_relative", "sha256", "bytes"} or
                item["ordinal"] != ordinal or not isinstance(item["path"], str) or
                not os.path.isabs(item["path"]) or not isinstance(item["source_relative"], str) or
                not item["source_relative"] or os.path.isabs(item["source_relative"]) or
                any(part in ("", ".", "..") for part in Path(item["source_relative"]).parts) or
                _hex(item.get("sha256"), f"predictive_plan.input[{ordinal}].sha256") != item["sha256"] or
                type(item.get("bytes")) is not int or item["bytes"] <= 0):
            _fail(f"predictive_plan:input_descriptor_invalid:{ordinal}")
        item_path = Path(item["path"])
        root = Path(source_root).resolve()
        if not _inside(item_path, root) or str(item_path.resolve().relative_to(root)) != item["source_relative"]:
            _fail(f"predictive_plan:input_root_mismatch:{ordinal}")
        digest, size = _sha(item_path)
        if digest != item["sha256"] or size != item["bytes"]:
            _fail(f"predictive_plan:input_mismatch:{ordinal}")
        listed = Path(source_lines[ordinal % len(source_lines)])
        if not listed.is_absolute():
            listed = Path(source_root) / listed
        if listed.resolve() != item_path.resolve():
            _fail(f"predictive_plan:input_sequence_mismatch:{ordinal}")
    if inputs != expected_inputs:
        _fail("predictive_plan:source_selection_mismatch")
    return value, [dict(item) for item in inputs], plan_sha


def load_repeat_predictive_plan(path: Path, *, first_path: Path,
                                first_plan: dict[str, Any],
                                first_inputs: list[dict[str, Any]], first_sha: str,
                                corpus: str, profile: str,
                                regime: str) -> tuple[dict[str, Any],
                                                      list[dict[str, Any]], str]:
    """Authenticate full-2 as the declared repeat of the exact full-1 plan."""
    repeat, repeat_inputs, repeat_sha = load_predictive_plan(
        path, corpus=corpus, profile=profile, regime=regime, depth="repeat-full")
    current_first_sha, current_first_bytes = _sha(first_path)
    if current_first_sha != first_sha:
        _fail("repeat_predictive_plan:first_plan_changed")
    repeat_of = repeat.get("repeat_of")
    if (not isinstance(repeat_of, dict) or
            set(repeat_of) != {"path", "sha256", "bytes"} or
            not isinstance(repeat_of.get("path"), str) or
            not os.path.isabs(repeat_of["path"]) or
            Path(repeat_of["path"]).resolve() != first_path.resolve() or
            repeat_of.get("sha256") != first_sha or
            repeat_of.get("bytes") != current_first_bytes):
        _fail("repeat_predictive_plan:repeat_of_mismatch")
    for field in ("semantics", "source_manifest", "source_root",
                  "matrix_precondition", "inputs", "scheduling"):
        if repeat.get(field) != first_plan.get(field):
            _fail(f"repeat_predictive_plan:{field}_mismatch")
    if repeat_inputs != first_inputs:
        _fail("repeat_predictive_plan:input_sequence_mismatch")
    return repeat, repeat_inputs, repeat_sha


def bind_batch_to_plan(rows: list[dict[str, Any]], plan_inputs: list[dict[str, Any]]) -> None:
    if len(rows) != len(plan_inputs):
        _fail("predictive_plan:batch_input_count_mismatch")
    for ordinal, row in enumerate(rows):
        if row["predictive_input"] != plan_inputs[ordinal]:
            _fail(f"predictive_plan:batch_input_mismatch:{ordinal}")


def load_topology(path: Path, rows: list[dict[str, Any]],
                  suite: str = TOPOLOGY,
                  scheduling: dict[str, Any] | None = None) -> str:
    """Require an explicit assignment map bound to ordered product inputs.

    C1F20/40 keeps one ordered source-stage queue per F relationship.  The
    map is therefore part of the authenticated input, rather than a caller
    hint that the shell runner may replace after launch.
    """
    if suite not in TOPOLOGIES:
        _fail("topology:unsupported_suite")
    digest, _ = _sha(path)
    try:
        value = json.loads(path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise LiveRunnerError("topology:invalid_json") from exc
    if (not isinstance(value, dict) or
            value.get("schema") != "icecream-s8-topology-assignment-v1" or
            value.get("suite") != suite):
        _fail("topology:unsupported_suite")
    assignments = value.get("assignments", value.get("inputs"))
    if not isinstance(assignments, list) or len(assignments) != len(rows):
        _fail("topology:assignment_count_mismatch")
    for ordinal, (item, row) in enumerate(zip(assignments, rows, strict=True)):
        if (not isinstance(item, dict) or item.get("ordinal") != ordinal or
                item.get("tu_id") != row["tu_id"]):
            _fail(f"topology:{ordinal}:identity_mismatch")
        relationship, f_slot = item.get("relationship"), item.get("f_slot")
        if (type(relationship) is not int or type(f_slot) is not int or
                relationship < 0 or relationship >= RELATIONSHIP_COUNT[suite] or
                f_slot < 0 or f_slot >= SLOTS_PER_F[suite]):
            _fail(f"topology:{ordinal}:assignment_invalid")
    if suite == PARALLEL_TOPOLOGY:
        relationships = [item["relationship"] for item in assignments]
        if set(relationships) != set(range(RELATIONSHIP_COUNT[suite])):
            _fail("topology:relationship_set_invalid")
        slots = {(item["relationship"], item["f_slot"]) for item in assignments}
        expected_slots = {(relationship, slot)
                          for relationship in range(RELATIONSHIP_COUNT[suite])
                          for slot in range(SLOTS_PER_F[suite])}
        if slots != expected_slots:
            _fail("topology:slot_set_invalid")
        # Each relationship is an independent ordered source-stage stream.
        # Interleaving between relationships is allowed, but a relationship's
        # source order must never move backwards or overlap itself.
        for relationship in range(RELATIONSHIP_COUNT[suite]):
            positions = [i for i, value in enumerate(relationships) if value == relationship]
            if positions != sorted(positions):  # defensive, documents contract
                _fail("topology:relationship_order_invalid")
    if scheduling is not None:
        if not isinstance(scheduling, dict):
            _fail("topology:predictive_schedule_invalid")
        planned = scheduling.get("assignments")
        expected_plan_topology = ("C1F20" if suite == PARALLEL_TOPOLOGY else "C1F1")
        if (scheduling.get("topology") != expected_plan_topology or
                not isinstance(planned, list) or len(planned) != len(rows)):
            _fail("topology:predictive_schedule_invalid")
        for ordinal, (planned_item, live_item) in enumerate(
                zip(planned, assignments, strict=True)):
            relationship = live_item["relationship"]
            f_slot = live_item["f_slot"]
            expected_global_slot = relationship * SLOTS_PER_F[suite] + f_slot
            if (not isinstance(planned_item, dict) or
                    planned_item.get("ordinal") != ordinal or
                    planned_item.get("global_slot") != expected_global_slot or
                    planned_item.get("f_relationship") != relationship or
                    planned_item.get("per_f_slot") != f_slot):
                _fail(f"topology:{ordinal}:predictive_schedule_mismatch")
    return digest


def _inside(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def product_identity(product_root: Path, script: Path = SCRIPT) -> tuple[str, str, dict[str, str], str]:
    """Bind source and all runtime executables to the selected product tree."""
    if (not product_root.is_absolute() or product_root.is_symlink() or
            not product_root.is_dir()):
        _fail("product_root:unavailable")
    root = product_root.resolve()
    script_resolved = script.resolve()
    if not _inside(script_resolved, root) or script.is_symlink() or not script.is_file():
        _fail("runner_script:outside_product_root")
    roles = ("scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc",
             "cache/icecc-cache-service")
    binaries: dict[str, str] = {}
    for role in roles:
        path = root / role
        if not _inside(path, root):
            _fail(f"binary_identity:{role}:outside_product_root")
        digest, _ = _sha(path)
        binaries[role] = digest
    try:
        status = subprocess.check_output(
            ["git", "-C", str(root), "status", "--porcelain", "--untracked-files=no"],
            text=True, stderr=subprocess.STDOUT)
        commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
        tree = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD^{tree}"], text=True).strip()
    except (OSError, subprocess.SubprocessError) as exc:
        raise LiveRunnerError("source_identity:unavailable") from exc
    if status:
        _fail("source_identity:tracked_or_index_dirty")
    if HEX40.fullmatch(commit) is None or HEX40.fullmatch(tree) is None:
        _fail("source_identity:invalid")
    script_sha, _ = _sha(script)
    return commit, tree, binaries, script_sha


def build_command(batch_manifest: Path, profile: str,
                  *, product_root: Path, corpus: str = "DuckDB",
                  regime: str = "cold", depth: str = "100", full_count: int | None = None,
                  passes: int = 2, workdir: Path | None = None,
                  predictive_plan: Path | None = None, script: Path = SCRIPT,
                  suite: str = TOPOLOGY, topology: Path | None = None,
                  timeout_seconds: int | None = None,
                  product_profile: str | None = None) -> list[str]:
    """Build the exact launch argv; this function never executes it."""
    if profile not in PROFILES:
        _fail("profile:undeclared")
    _measurement_method, effective_product_profile, _calibration_eligible = \
        _measurement_descriptor(profile, product_profile)
    if corpus not in CORPORA:
        _fail("corpus:undeclared")
    if suite not in TOPOLOGIES:
        _fail("topology:unsupported_suite")
    if regime not in REGIMES:
        _fail("regime:undeclared")
    count = selected_count(depth, full_count)
    if passes not in (1, 2):
        _fail("passes:undeclared")
    effective_timeout = derive_timeout(count, passes, regime == "warm", timeout_seconds)
    if predictive_plan is None or not predictive_plan.is_absolute() or not predictive_plan.is_file() or predictive_plan.is_symlink():
        _fail("predictive_plan:unavailable")
    plan_profile = _plan_identity_profile(profile, product_profile)
    load_predictive_plan(predictive_plan, corpus=corpus, profile=plan_profile,
                         regime=regime, depth=depth)
    if not batch_manifest.is_absolute() or not batch_manifest.is_file() or batch_manifest.is_symlink():
        _fail("batch_manifest:unavailable")
    rows = load_batch_manifest(batch_manifest, count)
    if topology is not None:
        if not topology.is_absolute() or not topology.is_file() or topology.is_symlink():
            _fail("topology:unavailable")
        load_topology(topology, rows, suite)
    elif suite != TOPOLOGY:
        _fail("topology:required_for_parallel_suite")
    product_identity(product_root, script)
    command = ["env", f"ICECC_TEST_TOP_SRCDIR={product_root}",
            f"ICECC_TEST_TOP_BUILDDIR={product_root}",
            f"ICECC_CARET_WORKAROUND={SCORED_CARET_WORKAROUND}",
            f"ICECC_P50_PROFILE={effective_product_profile}", f"ICECC_P50_CORPUS={corpus}",
            f"ICECC_P50_C1F1_WARM={int(regime == 'warm')}",
            f"ICECC_P50_C1F1_TIMEOUT={effective_timeout}",
            f"ICECC_P50_C1F1_PASSES={passes}",
            f"ICECC_P50_C1F1_EXPECTED_COUNT={count}",
            f"ICECC_P50_SUITE={suite}",
            "ICECC_P50_C1F1_KEEP_WORK=1",
            f"ICECC_P50_C1F1_BATCH_MANIFEST={batch_manifest}",
            f"ICECC_P50_PREDICTIVE_PLAN={predictive_plan}"]
    if topology is not None:
        command.append(f"ICECC_P50_TOPOLOGY={topology}")
    if workdir is not None:
        if (not workdir.is_absolute() or workdir.name.startswith("p50compilee2e.") is False):
            _fail("workdir:invalid")
        command.append(f"ICECC_P50_C1F1_WORKDIR={workdir}")
    command.append(str(script))
    return command


def container_image_identity(image: str = PINNED_IMAGE,
                             expected_image_id: str | None = None) -> dict[str, str]:
    """Resolve a mutable image reference once and return its immutable identity."""
    if not isinstance(image, str) or not image:
        _fail("container_image:invalid_reference")
    try:
        completed = subprocess.run(
            ["docker", "image", "inspect", image], check=True, capture_output=True,
            text=True, timeout=30)
        values = json.loads(completed.stdout)
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError) as exc:
        raise LiveRunnerError("container_image:inspect_failed") from exc
    if not isinstance(values, list) or len(values) != 1 or not isinstance(values[0], dict):
        _fail("container_image:inspect_ambiguous")
    value = values[0]
    image_id = value.get("Id")
    architecture = value.get("Architecture")
    operating_system = value.get("Os")
    created = value.get("Created")
    if (not isinstance(image_id, str) or IMAGE_ID.fullmatch(image_id.lower()) is None or
            architecture != "amd64" or operating_system != "linux" or
            not isinstance(created, str) or not created):
        _fail("container_image:identity_invalid")
    if (expected_image_id is not None and
            (IMAGE_ID.fullmatch(expected_image_id.lower()) is None or
             image_id.lower() != expected_image_id.lower())):
        _fail("container_image:content_id_mismatch")
    return {"reference": image, "image_id": image_id.lower(),
            "architecture": architecture, "os": operating_system, "created": created}


def validated_container_temp_root(path: Path) -> Path:
    root = path.absolute()
    if not root.is_dir() or root.is_symlink():
        _fail("container_temp_root:invalid")
    return root


def _path_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except (OSError, ValueError):
        return False
    return True


def container_name(work_parent: Path) -> str:
    """Return the unique, inspectable Docker name for one private run root."""
    name = f"p50-s8-{work_parent.name}"
    if (not work_parent.name.startswith("p5.") or SAFE.fullmatch(name) is None or
            len(name) > 63):
        _fail("container_name:invalid")
    return name


def build_container_command(inner: list[str], *, image_identity: dict[str, str],
                            bind_root: Path, work_parent: Path,
                            required_paths: list[Path],
                            temp_root: Path = DEFAULT_CONTAINER_TEMP_ROOT,
                            container_work_root: Path = DEFAULT_CONTAINER_WORK_ROOT) -> list[str]:
    """Wrap one product lifecycle in the pinned root-capable build image."""
    if (set(image_identity) != {"reference", "image_id", "architecture", "os", "created"} or
            IMAGE_ID.fullmatch(image_identity.get("image_id", "")) is None or
            image_identity.get("architecture") != "amd64" or
            image_identity.get("os") != "linux"):
        _fail("container_image:identity_invalid")
    bind_root = bind_root.resolve()
    work_parent = work_parent.resolve()
    temp_root = validated_container_temp_root(temp_root).resolve()
    if (not container_work_root.is_absolute() or
            container_work_root.parent != Path("/") or
            SAFE.fullmatch(container_work_root.name) is None):
        _fail("container_mount:container_work_root_invalid")
    if (not bind_root.is_absolute() or bind_root.is_symlink() or not bind_root.is_dir() or
            not work_parent.is_absolute() or work_parent.is_symlink() or
            not work_parent.is_dir() or work_parent.parent != temp_root or
            not work_parent.name.startswith("p5.")):
        _fail("container_mount:invalid_root")
    if not required_paths or any(not _path_within(path, bind_root)
                                 for path in required_paths):
        _fail("container_mount:required_path_outside_read_only_root")
    if not inner or inner[0] != "env":
        _fail("container_command:inner_invalid")
    uid, gid = os.geteuid(), os.getegid()
    inner_shell = shlex.join(inner)
    # The pinned farm-node image is deliberately a build image and does not
    # install the icecc package account.  Provision the normal service
    # account while the container is still root so every daemon (including
    # the scheduler's constructor-time lookup) sees the same account before
    # product startup.  The commands are idempotent for a future image that
    # already carries the account.
    account_setup_shell = (
        f"if ! getent group {CONTAINER_DAEMON_GROUP} >/dev/null 2>&1; then\n"
        f"    groupadd --system {CONTAINER_DAEMON_GROUP}\n"
        "fi\n"
        f"if ! getent passwd {CONTAINER_DAEMON_USER} >/dev/null 2>&1; then\n"
        f"    useradd --system --gid {CONTAINER_DAEMON_GROUP} --no-create-home "
        f"--home-dir /nonexistent --shell /usr/sbin/nologin {CONTAINER_DAEMON_USER}\n"
        "fi\n"
        f"getent passwd {CONTAINER_DAEMON_USER} >/dev/null 2>&1 || exit 71\n"
        f"getent group {CONTAINER_DAEMON_GROUP} >/dev/null 2>&1 || exit 71\n"
    )
    cleanup_shell = (account_setup_shell + "set +e\n" + inner_shell + "\n"
                     "product_status=$?\n"
                     f"chown -R {uid}:{gid} {shlex.quote(str(container_work_root))} || exit 70\n"
                     "exit \"$product_status\"\n")
    return ["docker", "run", "--rm", "--user", "0", "--network", "host",
            "--name", container_name(work_parent), "--oom-score-adj=-1000",
            "--env", f"ICECC_TEST_DAEMON_UID={CONTAINER_DAEMON_USER}",
            "--env", f"ICECC_TEST_DAEMON_GID={CONTAINER_DAEMON_GROUP}",
            "-v", f"{bind_root}:{bind_root}:ro",
            "-v", f"{work_parent}:{container_work_root}:rw",
            image_identity["image_id"], "/bin/sh", "-lc", cleanup_shell]


def stop_container(name: str) -> None:
    """Remove the exact private run container after client loss or timeout."""
    if SAFE.fullmatch(name) is None or not name.startswith("p50-s8-p5."):
        _fail("container_cleanup:name_invalid")
    try:
        completed = subprocess.run(
            ["docker", "container", "rm", "--force", name], capture_output=True,
            text=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as exc:
        raise LiveRunnerError("container_cleanup:command_failed") from exc
    if completed.returncode != 0 and "No such container" not in completed.stderr:
        _fail("container_cleanup:remove_failed")


def _fields(stdout: str, prefix: str) -> list[dict[str, str]]:
    result = []
    for line in stdout.splitlines():
        if not line.startswith(prefix + " "):
            continue
        row: dict[str, str] = {}
        for token in line.split()[1:]:
            if "=" in token:
                key, val = token.split("=", 1)
                row[key] = val
        result.append(row)
    return result


def _environment_preparation(stdout: str, work: Path, suite: str) -> dict[str, Any]:
    """Authenticate real environment warmup and post-warmup sidecar rotation."""
    expected_relationships = RELATIONSHIP_COUNT[suite]
    disabled = _fields(stdout, "S8_ENV_PREPARATION")
    if len(disabled) == 1 and disabled[0].get("cache_state") == "disabled":
        try:
            start_ns = int(disabled[0]["start_ns"])
            end_ns = int(disabled[0]["end_ns"])
            relationships = int(disabled[0]["relationships"])
            readiness_compiles = int(disabled[0]["readiness_compiles"])
            archive_bytes = int(disabled[0]["archive_bytes"])
            archive_sha = _hex(disabled[0].get("archive_sha256"),
                               "environment_preparation.archive_sha256")
        except (KeyError, TypeError, ValueError) as exc:
            raise LiveRunnerError("environment_preparation:disabled_invalid") from exc
        if (start_ns <= 0 or end_ns < start_ns or archive_bytes <= 0 or
                relationships != expected_relationships or
                readiness_compiles != expected_relationships):
            _fail("environment_preparation:disabled_invalid")
        readiness = _fields(stdout, "S8_RAW_ENV_READY")
        if len(readiness) != expected_relationships:
            _fail("environment_preparation:raw_readiness_count_mismatch")
        seen: set[int] = set()
        readiness_rows: list[dict[str, Any]] = []
        for index, row in enumerate(readiness):
            try:
                relationship = int(row["relationship"])
            except (KeyError, TypeError, ValueError) as exc:
                raise LiveRunnerError(
                    f"environment_preparation:raw_readiness_invalid:{index}") from exc
            label = row.get("label", "")
            if (relationship in seen or not 0 <= relationship < expected_relationships or
                    row.get("measured") != "0" or row.get("cache_state") != "disabled" or
                    not SAFE.fullmatch(label) or
                    label != (f"raw-env-ready-{relationship}" if suite == PARALLEL_TOPOLOGY
                              else "raw-env-ready")):
                _fail(f"environment_preparation:raw_readiness_invalid:{index}")
            log = work / f"client-compile-{label}.log"
            try:
                raw = log.read_text(encoding="utf-8", errors="replace")
            except OSError as exc:
                raise LiveRunnerError(
                    f"environment_preparation:raw_readiness_log_missing:{index}") from exc
            if "has env: false" not in raw:
                _fail(f"environment_preparation:raw_readiness_evidence_missing:{index}")
            seen.add(relationship)
            readiness_rows.append({"relationship": relationship, "label": label,
                                   "measured": False, "cache_state": "disabled"})
        if seen != set(range(expected_relationships)):
            _fail("environment_preparation:raw_readiness_relationship_set_incomplete")
        return {"relationships": relationships, "readiness_compiles": readiness_compiles,
                "archive_sha256": archive_sha,
                "archive_bytes": archive_bytes, "preparation_start_ns": start_ns,
                "preparation_end_ns": end_ns, "measured": False,
                "cache_state": "disabled", "warmups": readiness_rows,
                "sidecar_rotations": [], "post_rotation_ready": {"relationships": 0}}
    rows = _fields(stdout, "S8_ENV_WARMUP")
    if len(rows) != expected_relationships:
        _fail("environment_preparation:warmup_count_mismatch")
    seen: set[int] = set()
    warmups: list[dict[str, Any]] = []
    for index, row in enumerate(rows):
        try:
            relationship = int(row["relationship"])
        except (KeyError, TypeError, ValueError) as exc:
            raise LiveRunnerError(f"environment_preparation:{index}:field_invalid") from exc
        if (relationship in seen or not 0 <= relationship < expected_relationships or
                row.get("warmup_has_env") != "false" or
                row.get("ready_has_env") != "true" or
                row.get("preparation_measured") != "0" or
                row.get("cache_state") != "pre_rotation" or
                not SAFE.fullmatch(row.get("warmup_label", "")) or
                not SAFE.fullmatch(row.get("ready_label", ""))):
            _fail(f"environment_preparation:{index}:identity_invalid")
        seen.add(relationship)
        for label in (row["warmup_label"], row["ready_label"]):
            log = work / f"client-compile-{label}.log"
            try:
                raw = log.read_text(encoding="utf-8", errors="replace")
            except OSError as exc:
                raise LiveRunnerError(f"environment_preparation:{index}:warmup_log_missing") from exc
            expected = "has env: false" if label == row["warmup_label"] else "has env: true"
            if expected not in raw:
                _fail(f"environment_preparation:{index}:has_env_evidence_missing")
        warmups.append({"relationship": relationship, "warmup_label": row["warmup_label"],
                        "ready_label": row["ready_label"], "measured": False,
                        "cache_state": "pre_rotation"})
    if seen != set(range(expected_relationships)):
        _fail("environment_preparation:relationship_set_incomplete")
    expected_rotations = expected_relationships + 1
    rotations = _fields(stdout, "S8_SIDECAR_ROTATION")
    if len(rotations) != expected_rotations:
        _fail("environment_preparation:rotation_count_mismatch")
    rotation_keys: set[tuple[str, int]] = set()
    rotation_evidence: list[dict[str, Any]] = []
    for index, row in enumerate(rotations):
        role = row.get("role", "")
        try:
            relationship = int(row["relationship"])
            before_pid = int(row["before_pid"])
            after_pid = int(row["after_pid"])
        except (KeyError, TypeError, ValueError) as exc:
            raise LiveRunnerError(f"environment_preparation:rotation_{index}:field_invalid") from exc
        key = (role, relationship)
        if (role not in {"C", "F"} or key in rotation_keys or
                before_pid <= 0 or after_pid <= 0 or before_pid == after_pid or
                (role == "C" and relationship != 0) or
                (role == "F" and not 0 <= relationship < expected_relationships)):
            _fail(f"environment_preparation:rotation_{index}:identity_invalid")
        guid_values = {}
        for field in ("before_c_store_guid", "after_c_store_guid",
                      "before_f_store_guid", "after_f_store_guid"):
            value = row.get(field)
            if not isinstance(value, str) or HEX32.fullmatch(value) is None or int(value, 16) == 0:
                _fail(f"environment_preparation:rotation_{index}.{field}:invalid_guid")
            guid_values[field] = value.lower()
        before_c = guid_values["before_c_store_guid"]
        after_c = guid_values["after_c_store_guid"]
        before_f = guid_values["before_f_store_guid"]
        after_f = guid_values["after_f_store_guid"]
        if before_c == after_c or before_f == after_f:
            _fail(f"environment_preparation:rotation_{index}:guid_did_not_rotate")
        ready_path = work / ("ready-c.trace" if role == "C" else
                             f"ready-f-{relationship}.trace" if expected_relationships > 1 else
                             "ready-f.trace")
        if not ready_path.is_file() or ready_path.is_symlink():
            _fail(f"environment_preparation:rotation_{index}:ready_trace_missing")
        raw = ready_path.read_text(encoding="ascii")
        if (f"pid={before_pid}" not in raw or f"pid={after_pid}" not in raw or
                f"C_STORE_GUID={before_c}" not in raw or f"C_STORE_GUID={after_c}" not in raw or
                f"F_STORE_GUID={before_f}" not in raw or f"F_STORE_GUID={after_f}" not in raw):
            _fail(f"environment_preparation:rotation_{index}:ready_trace_binding_invalid")
        rotation_keys.add(key)
        rotation_evidence.append({"role": role, "relationship": relationship,
                                  "before_pid": before_pid, "after_pid": after_pid,
                                  "before_c_store_guid": before_c, "after_c_store_guid": after_c,
                                  "before_f_store_guid": before_f, "after_f_store_guid": after_f,
                                  "ready_trace": str(ready_path.relative_to(work))})
    expected_keys = ({("C", 0)} | {("F", relationship) for relationship in range(expected_relationships)})
    if rotation_keys != expected_keys:
        _fail("environment_preparation:rotation_set_incomplete")
    post_ready = _fields(stdout, "S8_ENV_POST_ROTATION_READY")
    if len(post_ready) != 1:
        _fail("environment_preparation:post_rotation_ready_missing_or_duplicate")
    try:
        post_count = int(post_ready[0]["relationships"])
        scheduler_offset = int(post_ready[0]["log_offset"])
    except (KeyError, TypeError, ValueError) as exc:
        raise LiveRunnerError("environment_preparation:post_rotation_ready_invalid") from exc
    scheduler_log = work / "scheduler.log"
    if (post_count != expected_relationships or scheduler_offset < 0 or
            not scheduler_log.is_file() or scheduler_log.is_symlink()):
        _fail("environment_preparation:post_rotation_ready_identity_invalid")
    scheduler_tail = scheduler_log.read_bytes()[scheduler_offset:]
    if expected_relationships == 1:
        # Real scheduler service names include the platform suffix before the
        # colon (``p50-f(x86_64):``).  Keep accepting the compact fixture form
        # while requiring a service-name boundary so p50-f-* cannot satisfy
        # the single-relationship witness.
        ready_matches = re.findall(
            rb"RELOGIN p50-f(?:\([^\r\n()]+\))?:?(?:\s|$)[^\r\n]*cache=[^\r\n]*cache_profiles=",
            scheduler_tail,
        )
        if not ready_matches:
            _fail("environment_preparation:post_rotation_ready_absent")
    else:
        ready_matches = set(re.findall(rb"RELOGIN (p50-f-[0-9]+).*cache=.*cache_profiles=", scheduler_tail))
        expected_services = {f"p50-f-{relationship}".encode() for relationship in range(expected_relationships)}
        if ready_matches != expected_services:
            _fail("environment_preparation:post_rotation_ready_incomplete")
    preparation = _fields(stdout, "S8_ENV_PREPARATION")
    if len(preparation) != 1:
        _fail("environment_preparation:summary_missing_or_duplicate")
    summary = preparation[0]
    try:
        count = int(summary["relationships"])
        archive_bytes = int(summary["archive_bytes"])
        start_ns = int(summary["start_ns"])
        end_ns = int(summary["end_ns"])
        archive_sha = _hex(summary.get("archive_sha256"), "environment_preparation.archive_sha256")
    except (KeyError, TypeError, ValueError) as exc:
        raise LiveRunnerError("environment_preparation:summary_invalid") from exc
    if (count != expected_relationships or start_ns <= 0 or end_ns < start_ns or
            archive_bytes <= 0 or summary.get("measured") != "0" or
            summary.get("cache_state") != "rotated"):
        _fail("environment_preparation:summary_identity_invalid")
    if len(_fields(stdout, "S8_ENV_MEASURED_NO_INSTALL")) != 1:
        _fail("environment_preparation:measured_install_guard_missing")
    rotation_evidence.sort(key=lambda item: (item["role"], item["relationship"]))
    warmups.sort(key=lambda item: item["relationship"])
    return {"relationships": count, "archive_sha256": archive_sha,
            "archive_bytes": archive_bytes, "preparation_start_ns": start_ns,
            "preparation_end_ns": end_ns, "measured": False,
            "cache_state": "rotated", "warmups": warmups,
            "sidecar_rotations": rotation_evidence,
            "post_rotation_ready": {"relationships": post_count,
                                     "scheduler_log_offset": scheduler_offset}}


def _binary_identity(stdout: str, *, allow_raw: bool = False) -> dict[str, str]:
    allow_raw = allow_raw or "S8_RAW_II mode=whole-legacy" in stdout
    binaries: dict[str, str] = {}
    for row in _fields(stdout, "S8_BINARY"):
        role, digest = row.get("role"), row.get("sha256")
        if not isinstance(role, str) or role in binaries or HEX64.fullmatch(digest or "") is None:
            _fail("binary_identity:invalid")
        binaries[role] = digest  # type: ignore[assignment]
    expected = {"scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc"}
    if not allow_raw:
        expected.add("cache/icecc-cache-service")
    if set(binaries) != expected:
        _fail("binary_identity:incomplete")
    return binaries


def _validate_product_log_evidence(work: Path, observations: list[dict[str, Any]],
                                   profile: str) -> None:
    """Require per-TU profile/session evidence from the product log.

    The shell gate may only enforce that a command returned successfully; it
    must not turn that assertion into a trusted metric.  The selected profile
    is checked in every measured client invocation, while CACHE_SESSION is
    checked in the F daemon log for that planned relationship.  The action
    trace below separately authenticates the resulting F store identity.
    """
    if profile == RAW_II_PROFILE:
        for path in sorted(work.glob("*.log")):
            raw = path.read_text(encoding="utf-8", errors="replace")
            if "CACHE_SESSION" in raw or re.search(
                    r"P29|ZSTD_TU|ZSTD_ROUTE|GRZ_RESIDUAL", raw):
                _fail("raw_ii:cache_or_profile_evidence_observed")
        for index, observed in enumerate(observations):
            log = work / f"client-compile-{observed['run']}-{observed['ordinal']}.log"
            try:
                raw = log.read_text(encoding="utf-8", errors="replace")
            except OSError as exc:
                raise LiveRunnerError(f"batch:{index}:client_log_missing") from exc
            if "write_fd_to_server" not in raw:
                _fail(f"batch:{index}:raw_ii_legacy_transfer_evidence_missing")
            if re.search(r"building myself|building_local|local build forced|fallback_local|client_exception", raw):
                _fail(f"batch:{index}:product_local_fallback")
        return
    profile_markers = {
        "P29": ("P29", "CACHE_SESSION"),
        "ZSTD_TU": ("ZSTD_TU", "CACHE_SESSION"),
        "ZSTD_ROUTE": ("ZSTD_ROUTE", "CACHE_SESSION"),
        "GRZ_RESIDUAL": ("GRZ_RESIDUAL", "CACHE_SESSION"),
    }
    try:
        markers = profile_markers[profile]
    except KeyError as exc:  # pragma: no cover - argparse/schema guards this
        raise LiveRunnerError("profile:undeclared") from exc
    for index, observed in enumerate(observations):
        log = work / f"client-compile-{observed['run']}-{observed['ordinal']}.log"
        try:
            raw = log.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            raise LiveRunnerError(f"batch:{index}:client_log_missing") from exc
        if markers[0] not in raw:
            _fail(f"batch:{index}:product_profile_evidence_missing")
        service_identity = observed.get("observed_f_service_identity")
        f_log = (work / "f.log" if service_identity == "p50-f" else
                 work / f"f-{observed['planned_relationship']}.log")
        try:
            f_raw = f_log.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            raise LiveRunnerError(f"batch:{index}:f_log_missing") from exc
        if markers[1] not in f_raw:
            _fail(f"batch:{index}:cache_session_evidence_missing")
        if re.search(r"building myself|building_local|local build forced|fallback_local|client_exception", raw):
            _fail(f"batch:{index}:product_local_fallback")


def _reported_artifact_path(value: object, *, work: Path,
                            reported_work: Path | None,
                            reason: str) -> Path:
    if not isinstance(value, str) or not value:
        _fail(reason)
    observed = Path(value)
    if not observed.is_absolute() or ".." in observed.parts:
        _fail(reason)
    if reported_work is None:
        return observed
    if reported_work != DEFAULT_REPORTED_WORKDIR:
        _fail(reason)
    try:
        relative = observed.relative_to(reported_work)
    except ValueError:
        _fail(reason)
    if not relative.parts:
        _fail(reason)
    return work.joinpath(*relative.parts)


def _timing_rows(stdout: str, rows: list[dict[str, Any]], work: Path,
                 passes: int, assignments: list[dict[str, Any]] | None = None,
                 suite: str = TOPOLOGY,
                 reported_work: Path | None = None) -> list[dict[str, Any]]:
    if suite not in TOPOLOGIES:
        _fail("topology:unsupported_suite")
    if assignments is None:
        assignments = [{"relationship": 0, "f_slot": 0} for _ in rows]
    if len(assignments) != len(rows):
        _fail("batch:assignment_count_mismatch")
    observations = _fields(stdout, "S8_BATCH_TU")
    if len(observations) != passes * len(rows):
        _fail("batch:incomplete_product_rows")
    result: list[dict[str, Any]] = []
    for index, observed in enumerate(observations):
        run = observed.get("run")
        ordinal = int(observed.get("ordinal", "-1")) if observed.get("ordinal", "").isdigit() else -1
        expected = rows[ordinal] if 0 <= ordinal < len(rows) else None
        expected_run = "full-1" if index < len(rows) else "full-2"
        if (run != expected_run or expected is None or
                observed.get("tu_id") != expected["tu_id"]):
            _fail(f"batch:{index}:identity_invalid")
        if ordinal != index % len(rows):
            _fail(f"batch:{index}:order_invalid")
        for key in ("source_sha256", "preprocessed_sha256", "remote_sha256", "local_sha256"):
            _hex(observed.get(key), f"batch:{index}.{key}")
        if observed["source_sha256"] != expected["sha256"]:
            _fail(f"batch:{index}:source_digest_mismatch")
        integer_fields = (
            "preprocessed_bytes", "remote_bytes", "local_bytes", "admission_start_ns",
            "input_ready_ns", "compile_start_ns", "compile_end_ns", "witness_end_ns",
            "wait_for_cs_ns", "planned_assignment_ordinal", "planned_relationship",
            "planned_admission_lane", "observed_scheduler_job_id", "observed_source_tu_seq",
        )
        parsed = {}
        for key in integer_fields:
            try:
                parsed[key] = int(observed[key])
            except (KeyError, TypeError, ValueError) as exc:
                raise LiveRunnerError(f"batch:{index}:{key}_invalid") from exc
        expected_assignment = assignments[ordinal]
        expected_route = ordinal if suite == PARALLEL_TOPOLOGY else 0
        if (parsed["preprocessed_bytes"] <= 0 or parsed["remote_bytes"] <= 0 or
                parsed["local_bytes"] <= 0 or
                not parsed["admission_start_ns"] <= parsed["compile_start_ns"] <=
                    parsed["input_ready_ns"] <= parsed["compile_end_ns"] <=
                    parsed["witness_end_ns"] or
                parsed["wait_for_cs_ns"] <= 0 or
                parsed["planned_assignment_ordinal"] != expected_route or
                parsed["planned_relationship"] != expected_assignment["relationship"] or
                parsed["planned_admission_lane"] != expected_assignment["f_slot"] or
                parsed["observed_scheduler_job_id"] <= 0 or
                parsed["observed_source_tu_seq"] < 0):
            _fail(f"batch:{index}:product_metric_invalid")
        expected_service = (f"p50-f-{parsed['planned_relationship']}"
                            if suite == PARALLEL_TOPOLOGY else "p50-f")
        if observed.get("observed_f_service_identity") != expected_service:
            _fail(f"batch:{index}:service_identity_invalid")
        if (observed["preprocessed_sha256"] != expected["predictive_input"]["sha256"] or
                parsed["preprocessed_bytes"] != expected["predictive_input"]["bytes"]):
            _fail(f"batch:{index}:predictive_payload_mismatch")
        remote_path = _reported_artifact_path(
            observed.get("remote_path"), work=work, reported_work=reported_work,
            reason=f"batch:{index}:remote_object_path_invalid")
        local_path = _reported_artifact_path(
            observed.get("local_path"), work=work, reported_work=reported_work,
            reason=f"batch:{index}:local_object_path_invalid")
        pre_path = _reported_artifact_path(
            observed.get("preprocessed_path"), work=work, reported_work=reported_work,
            reason=f"batch:{index}:preprocessed_path_invalid")
        for path, expected_sha, expected_bytes, label in (
                (remote_path, observed["remote_sha256"], parsed["remote_bytes"], "remote_object"),
                (local_path, observed["local_sha256"], parsed["local_bytes"], "local_object"),
                (pre_path, observed["preprocessed_sha256"], parsed["preprocessed_bytes"], "preprocessed")):
            expected_parent = work if label == "preprocessed" else work / "out"
            if (path.parent != expected_parent or expected_parent.is_symlink() or
                    not expected_parent.is_dir()):
                _fail(f"batch:{index}:{label}_path_invalid")
            digest, size = _sha(path)
            if digest != expected_sha or size != expected_bytes:
                _fail(f"batch:{index}:{label}_digest_mismatch")
        if remote_path.read_bytes() != local_path.read_bytes():
            _fail(f"batch:{index}:object_not_byte_identical")
        result.append({**observed, **parsed, "run": run, "ordinal": ordinal,
                       "reported_preprocessed_path": observed["preprocessed_path"],
                       "reported_remote_path": observed["remote_path"],
                       "reported_local_path": observed["local_path"],
                       "preprocessed_path": str(pre_path),
                       "remote_path": str(remote_path),
                       "local_path": str(local_path),
                       # Compatibility aliases below remain internal to the
                       # current curve builder.  Product evidence uses the
                       # explicit planned/observed names above.
                       "assignment": parsed["planned_assignment_ordinal"],
                       "relationship": parsed["planned_relationship"],
                       "f_slot": parsed["planned_admission_lane"],
                       "elapsed_ns": parsed["compile_end_ns"] - parsed["compile_start_ns"],
                       "client_elapsed_ns": parsed["compile_end_ns"] - parsed["compile_start_ns"],
                       "measured_elapsed_ns": parsed["wait_for_cs_ns"],
                       "channel_bytes": 0,
                       "returned_object_bytes": parsed["remote_bytes"]})
    return result


def _max_concurrency(intervals: list[tuple[int, int]]) -> int:
    """Return the peak for positive half-open intervals.

    End events sort before start events at an equal timestamp, so adjacent
    planned-lane leases are not misreported as overlap.
    """
    if not intervals or any(end <= start for start, end in intervals):
        _fail("batch:concurrency_interval_invalid")
    events = [(start, 1) for start, _ in intervals]
    events.extend((end, -1) for _, end in intervals)
    active = maximum = 0
    for _timestamp, delta in sorted(events, key=lambda item: (item[0], item[1])):
        active += delta
        if active < 0:
            _fail("batch:concurrency_interval_invalid")
        maximum = max(maximum, active)
    if active != 0:
        _fail("batch:concurrency_interval_invalid")
    return maximum


def _observed_concurrency(selected: list[dict[str, Any]], suite: str) -> dict[str, int]:
    """Authenticate relationship ordering/lane caps and derive live peaks."""
    relationships = RELATIONSHIP_COUNT[suite]
    slots_per_f = SLOTS_PER_F[suite]
    per_relationship: list[int] = []
    for relationship in range(relationships):
        related = sorted(
            (row for row in selected if int(row["planned_relationship"]) == relationship),
            key=lambda row: int(row["planned_assignment_ordinal"]),
        )
        if not related:
            _fail(f"batch:relationship_missing:{relationship}")
        for previous, current in zip(related, related[1:]):
            if (int(current["admission_start_ns"]) < int(previous["input_ready_ns"]) or
                    int(current["observed_source_tu_seq"]) !=
                    int(previous["observed_source_tu_seq"]) + 1):
                _fail(f"batch:relationship_admission_order_invalid:{relationship}")
        for lane in range(slots_per_f):
            lane_rows = [row for row in related
                         if int(row["planned_admission_lane"]) == lane]
            for previous, current in zip(lane_rows, lane_rows[1:]):
                if int(current["admission_start_ns"]) < int(previous["compile_end_ns"]):
                    _fail(f"batch:planned_lane_overlap:{relationship}:{lane}")
        relationship_peak = _max_concurrency([
            (int(row["admission_start_ns"]), int(row["compile_end_ns"]))
            for row in related
        ])
        if relationship_peak > slots_per_f:
            _fail(f"batch:relationship_active_cap_exceeded:{relationship}")
        per_relationship.append(relationship_peak)
    return {
        "max_concurrent_source_admissions": _max_concurrency([
            (int(row["admission_start_ns"]), int(row["input_ready_ns"]))
            for row in selected
        ]),
        "max_concurrent_compile_result_jobs": _max_concurrency([
            (int(row["compile_start_ns"]), int(row["compile_end_ns"]))
            for row in selected
        ]),
        "max_concurrent_admitted_or_compiling_jobs": _max_concurrency([
            (int(row["admission_start_ns"]), int(row["compile_end_ns"]))
            for row in selected
        ]),
        "max_concurrent_active_per_relationship": max(per_relationship),
    }


def _batch_windows(stdout: str, observations: list[dict[str, Any]],
                   rows: list[dict[str, Any]], passes: int,
                   suite: str) -> dict[str, dict[str, int]]:
    """Authenticate wall windows and independently recompute emitted peaks."""
    windows = _fields(stdout, "S8_BATCH_WINDOW")
    metric_rows = _fields(stdout, "S8_BATCH_METRICS")
    expected_runs = ["full-1"] + (["full-2"] if passes == 2 else [])
    if len(windows) != len(expected_runs) or len(metric_rows) != len(expected_runs):
        _fail("batch:window_count_mismatch")
    result: dict[str, dict[str, int]] = {}
    metric_fields = (
        "makespan_ns", "harness_completion_ns", "max_concurrent_source_admissions",
        "max_concurrent_compile_result_jobs", "max_concurrent_admitted_or_compiling_jobs",
        "max_concurrent_active_per_relationship",
    )
    for window, emitted_metrics, run in zip(windows, metric_rows, expected_runs, strict=True):
        if (window.get("run") != run or emitted_metrics.get("run") != run or
                run in result):
            _fail("batch:window_identity_invalid")
        try:
            start_ns, end_ns = int(window["start_ns"]), int(window["end_ns"])
            parsed_metrics = {key: int(emitted_metrics[key]) for key in metric_fields}
        except (KeyError, TypeError, ValueError) as exc:
            raise LiveRunnerError("batch:window_timestamp_invalid") from exc
        if start_ns <= 0 or end_ns <= start_ns:
            _fail("batch:window_timestamp_invalid")
        selected = [row for row in observations if row["run"] == run]
        if len(selected) != len(rows):
            _fail("batch:window_observation_count_mismatch")
        starts = [int(row["compile_start_ns"]) for row in selected]
        ends = [int(row["compile_end_ns"]) for row in selected]
        if (min(int(row["admission_start_ns"]) for row in selected) < start_ns or
                max(int(row["witness_end_ns"]) for row in selected) > end_ns or
                min(starts) < start_ns or max(ends) > end_ns):
            _fail(f"batch:{run}:compile_outside_batch_window")
        # Scored wall time ends at the last remote result; the local byte
        # witness remains a separate harness completion boundary and cannot
        # tax throughput.
        computed_metrics = {
            "makespan_ns": max(ends) - start_ns,
            "harness_completion_ns": end_ns - start_ns,
            **_observed_concurrency(selected, suite),
        }
        if parsed_metrics != computed_metrics:
            _fail(f"batch:{run}:concurrency_metrics_mismatch")
        if suite == PARALLEL_TOPOLOGY:
            if computed_metrics["max_concurrent_compile_result_jobs"] <= 1:
                _fail(f"batch:{run}:no_observed_compile_overlap")
            routes = {(int(row["planned_relationship"]),
                       int(row["planned_admission_lane"])) for row in selected}
            if routes != {(relationship, slot)
                          for relationship in range(RELATIONSHIP_COUNT[suite])
                          for slot in range(SLOTS_PER_F[suite])}:
                _fail(f"batch:{run}:slot_evidence_incomplete")
        result[run] = {"start_ns": start_ns, "end_ns": end_ns, **computed_metrics}
    return result


def _live_curve_rows(selected: list[dict[str, Any]], rows: list[dict[str, Any]],
                     cell: dict[str, str], batch_start_ns: int) -> list[dict[str, Any]]:
    """Build a prefix wall-time curve for possibly overlapping live jobs."""
    if len(selected) != len(rows) or batch_start_ns <= 0:
        _fail("curve:live_prefix_inputs_invalid")
    c_to_f_total = f_to_c_total = channel = 0
    prefix_end = batch_start_ns
    curve: list[dict[str, Any]] = []
    for step, (item, source) in enumerate(zip(selected, rows, strict=True)):
        try:
            prefix_end = max(prefix_end, int(item["compile_end_ns"]))
            c_to_f = int(item["c_to_f_bytes"])
            f_to_c = int(item["f_to_c_bytes"])
            item_channel = int(item["channel_bytes"])
        except (KeyError, TypeError, ValueError) as exc:
            raise LiveRunnerError("curve:live_prefix_metric_invalid") from exc
        if c_to_f <= 0 or f_to_c <= 0 or item_channel != c_to_f + f_to_c:
            _fail("curve:live_prefix_metric_invalid")
        c_to_f_total += c_to_f
        f_to_c_total += f_to_c
        channel += item_channel
        elapsed = prefix_end - batch_start_ns
        if elapsed <= 0 or channel <= 0:
            _fail("curve:live_prefix_metric_invalid")
        curve.append({
            "step": step,
            "tu_id": source["tu_id"],
            "cell": cell,
            "cumulative": {
                "C_TO_F_bytes": c_to_f_total,
                "F_TO_C_bytes": f_to_c_total,
                "channel_bytes": channel,
                "elapsed_ns": elapsed,
                "throughput_bytes_per_s": channel * 1_000_000_000 / elapsed,
            },
        })
    return curve


def _f_service_relationships(path: Path) -> dict[str, tuple[int, str]]:
    """Bind observed F store GUIDs to the daemon/service trace that emitted them."""
    if not path.is_file() or path.is_symlink():
        _fail("action_trace:F_service_map_missing")
    result: dict[str, tuple[int, str]] = {}
    seen_relationships: set[int] = set()
    for index, line in enumerate(path.read_text(encoding="ascii").splitlines()):
        fields = line.split("\t")
        if len(fields) != 3:
            _fail(f"action_trace:F_service_map_invalid:{index}")
        try:
            relationship = int(fields[0])
        except ValueError as exc:
            raise LiveRunnerError(f"action_trace:F_service_map_invalid:{index}") from exc
        service, guid = fields[1], fields[2]
        if (not 0 <= relationship < RELATIONSHIP_COUNT[PARALLEL_TOPOLOGY] or
                service != f"p50-f-{relationship}" or HEX32.fullmatch(guid) is None or
                guid == "0" * 32 or guid in result or relationship in seen_relationships):
            _fail(f"action_trace:F_service_map_invalid:{index}")
        result[guid] = (relationship, service)
        seen_relationships.add(relationship)
    if seen_relationships != set(range(RELATIONSHIP_COUNT[PARALLEL_TOPOLOGY])):
        _fail("action_trace:F_service_map_incomplete")
    return result


def _action_stage_paths(c_path: Path, f_path: Path, expected_count: int,
                        assignments: list[dict[str, Any]] | None = None,
                        suite: str = TOPOLOGY) -> list[dict[str, Any]]:
    if not c_path.is_file() or not f_path.is_file():
        _fail("action_trace:missing")
    c_rows = action_parser.parse_action(c_path.read_text())
    f_rows = action_parser.parse_action(f_path.read_text())
    c_begins = [row for row in c_rows if row["action"] == "TX_BEGIN" and row["actor"] == "C"]
    f_begins = [row for row in f_rows if row["action"] == "TX_BEGIN" and row["actor"] == "F"]
    if len(c_begins) != expected_count or len(f_begins) != expected_count:
        _fail("action_trace:TX_BEGIN_count_mismatch")
    if suite not in TOPOLOGIES:
        _fail("action_trace:unsupported_suite")
    if assignments is None:
        assignments = [{"relationship": 0, "f_slot": 0} for _ in range(expected_count)]
    elif len(assignments) != expected_count:
        if len(assignments) == 0 or expected_count % len(assignments):
            _fail("action_trace:assignment_count_mismatch")
        assignments = assignments * (expected_count // len(assignments))
    # Transaction digests are relationship-local: two independent F services
    # can process the same repeated source at the same local sequence and
    # legitimately emit the same digest.  Include both store identities and
    # relationship-local sequence in the join key; only an exact duplicate
    # product transaction is invalid.
    def transaction_key(row: dict[str, Any]) -> tuple[str, str, int, int, str]:
        return (row["c_store_guid"], row["f_store_guid"], int(row["tu_seq"]),
                int(row["rel_seq"]), row["transaction_digest"])

    f_by_transaction: dict[tuple[str, str, int, int, str], dict[str, Any]] = {}
    for row in f_begins:
        key = transaction_key(row)
        if key in f_by_transaction:
            _fail("action_trace:duplicate_transaction")
        f_by_transaction[key] = row
    c_guid = {row["c_store_guid"] for row in c_begins}
    if len(c_guid) != 1:
        _fail("action_trace:C_identity_changed")
    if suite == PARALLEL_TOPOLOGY:
        by_f_guid = _f_service_relationships(c_path.parent / "s8-f-service-map.tsv")
    else:
        f_guids = {row["f_store_guid"] for row in f_begins}
        if len(f_guids) != 1:
            _fail("action_trace:relationship_service_changed")
        by_f_guid = {f_guids.pop(): (0, "p50-f")}
    observed_by_relationship: dict[int, list[tuple[dict[str, Any], dict[str, Any], str]]] = {
        relationship: [] for relationship in range(RELATIONSHIP_COUNT[suite])
    }
    for index, c_row in enumerate(c_begins):
        f_row = f_by_transaction.get(transaction_key(c_row))
        if f_row is None:
            _fail(f"action_trace:{index}:transaction_missing_on_F")
        # The two role traces must describe the same transaction, not merely
        # the same long-lived service GUIDs.  Keep the F-side state digest as
        # evidence below; C's stage bytes are the measured C->F channel.
        for field in ("tu_seq", "rel_seq", "history_nonce", "raw_digest", "transaction_digest"):
            if c_row[field] != f_row[field]:
                _fail(f"action_trace:{index}:{field}_mismatch")
        if c_row["c_store_guid"] != f_row["c_store_guid"] or c_row["f_store_guid"] != f_row["f_store_guid"]:
            _fail(f"action_trace:{index}:service_identity_mismatch")
        try:
            relationship, service = by_f_guid[f_row["f_store_guid"]]
        except KeyError as exc:
            raise LiveRunnerError(f"action_trace:{index}:unbound_F_service") from exc
        observed_by_relationship[relationship].append((c_row, f_row, service))

    result: list[dict[str, Any] | None] = [None] * expected_count
    for relationship, observed in observed_by_relationship.items():
        planned = [(index, assignment) for index, assignment in enumerate(assignments)
                   if int(assignment["relationship"]) == relationship]
        observed.sort(key=lambda item: (int(item[0]["rel_seq"]), int(item[0]["tu_seq"])))
        if len(planned) != len(observed) or not observed:
            _fail(f"action_trace:relationship_count_mismatch:{relationship}")
        for position in range(1, len(observed)):
            previous, current = observed[position - 1][0], observed[position][0]
            if (int(current["rel_seq"]) != int(previous["rel_seq"]) + 1 or
                    int(current["tu_seq"]) != int(previous["tu_seq"]) + 1):
                _fail(f"action_trace:relationship_sequence_not_contiguous:{relationship}")
        for (planned_index, assignment), (c_row, f_row, service) in zip(
                planned, observed, strict=True):
            result[planned_index] = {
                "c_to_f_bytes": int(c_row["stage_bytes"]),
                "c_store_guid": c_row["c_store_guid"], "f_store_guid": c_row["f_store_guid"],
                "tu_seq": int(c_row["tu_seq"]), "rel_seq": int(c_row["rel_seq"]),
                "history_nonce": int(c_row["history_nonce"]),
                "raw_digest": c_row["raw_digest"], "state_digest": c_row["state_digest"],
                "transaction_digest": c_row["transaction_digest"],
                "f_state_digest": f_row["state_digest"], "f_raw_digest": f_row["raw_digest"],
                "planned_relationship": relationship,
                "planned_admission_lane": int(assignment["f_slot"]),
                "observed_f_service_identity": service,
                # Compatibility aliases for the current curve schema.
                "relationship": relationship, "f_slot": int(assignment["f_slot"]),
            }
    if any(item is None for item in result):
        _fail("action_trace:relationship_binding_incomplete")
    return [item for item in result if item is not None]


def _external_measured_f_action_trace(work: Path,
                                      external_farm: bool) -> Path:
    """Select the measured F trace name owned by the execution transport."""
    return work / ("s7-warm-f-action-trace.jsonl" if external_farm else
                   "s7-measured-f-action-trace.jsonl")


def _action_stage(work: Path, expected_count: int,
                  assignments: list[dict[str, Any]] | None = None,
                  suite: str = TOPOLOGY,
                  f_action_trace: Path | None = None) -> list[dict[str, Any]]:
    # These are the post-prewarm slices emitted by the shell gate.  Reading
    # the full warm trace would admit prewarm transactions as measurements.
    return _action_stage_paths(work / "s7-measured-c-action-trace.jsonl",
                               f_action_trace or
                               work / "s7-measured-f-action-trace.jsonl",
                               expected_count, assignments, suite)


def _legacy_wire_rows(path: Path, role: str) -> list[dict[str, int | str]]:
    """Read the product's role-labelled legacy frame ledger."""
    if role not in {"C", "F"} or not path.is_file() or path.is_symlink():
        _fail("legacy_wire:trace_missing")
    required = (
        "job_id", "assignment_epoch", "assignment_nonce", "c_guid", "tu_seq",
        "c_to_f_sent_bytes", "c_to_f_received_bytes", "f_to_c_sent_bytes",
        "f_to_c_received_bytes",
    )
    result: list[dict[str, int | str]] = []
    for index, line in enumerate(path.read_text(encoding="ascii").splitlines()):
        try:
            value = json.loads(line)
        except (ValueError, TypeError) as exc:
            raise LiveRunnerError(f"legacy_wire:row_invalid:{index}") from exc
        if (not isinstance(value, dict) or
                value.get("schema") != "icecream-p50-legacy-wire-v1" or
                value.get("role") != role or set(value) != {"schema", "role", *required}):
            _fail(f"legacy_wire:row_identity_invalid:{index}")
        row: dict[str, int | str] = {"schema": value["schema"], "role": value["role"]}
        for field in required:
            candidate = value[field]
            if type(candidate) is not int or candidate < 0:
                _fail(f"legacy_wire:{field}_invalid:{index}")
            row[field] = candidate
        if (int(row["job_id"]) == 0 or int(row["assignment_epoch"]) == 0 or
                int(row["assignment_nonce"]) == 0 or int(row["c_guid"]) == 0):
            _fail(f"legacy_wire:identity_zero:{index}")
        result.append(row)
    if not result:
        _fail("legacy_wire:trace_empty")
    return result


def _legacy_wire_stage(work: Path, observations: list[dict[str, Any]],
                       assignments: list[dict[str, Any]],
                       suite: str = TOPOLOGY) -> list[dict[str, Any]]:
    """Join C/F whole-legacy byte witnesses to the live TU rows."""
    if suite not in TOPOLOGIES or len(observations) != len(assignments):
        _fail("legacy_wire:inputs_invalid")
    c_rows = _legacy_wire_rows(work / "s7-measured-c-legacy-wire-trace.jsonl", "C")
    f_paths = (sorted(work.glob("s7-measured-f-legacy-wire-trace-*.jsonl"))
               if suite == PARALLEL_TOPOLOGY else
               [work / "s7-measured-f-legacy-wire-trace.jsonl"])
    if not f_paths or any(not path.is_file() or path.is_symlink() for path in f_paths):
        _fail("legacy_wire:F_trace_missing")
    f_rows = [row for path in f_paths for row in _legacy_wire_rows(path, "F")]
    if len(c_rows) != len(observations) or len(f_rows) != len(observations):
        _fail("legacy_wire:row_count_mismatch")

    def key(row: dict[str, int | str]) -> tuple[int, int, int, int, int]:
        return (int(row["job_id"]), int(row["assignment_epoch"]),
                int(row["assignment_nonce"]), int(row["c_guid"]), int(row["tu_seq"]))

    c_by_key: dict[tuple[int, int, int, int, int], dict[str, int | str]] = {}
    f_by_key: dict[tuple[int, int, int, int, int], dict[str, int | str]] = {}
    for row in c_rows:
        if key(row) in c_by_key:
            _fail("legacy_wire:duplicate_C_identity")
        c_by_key[key(row)] = row
    for row in f_rows:
        if key(row) in f_by_key:
            _fail("legacy_wire:duplicate_F_identity")
        f_by_key[key(row)] = row
    result: list[dict[str, Any]] = []
    for index, observation in enumerate(observations):
        try:
            job_id = int(observation["observed_scheduler_job_id"])
            tu_seq = int(observation["observed_source_tu_seq"])
        except (KeyError, TypeError, ValueError) as exc:
            raise LiveRunnerError(f"legacy_wire:observation_identity_invalid:{index}") from exc
        matches = [row for row in c_rows
                   if int(row["job_id"]) == job_id and int(row["tu_seq"]) == tu_seq]
        if len(matches) != 1:
            _fail(f"legacy_wire:C_observation_binding_invalid:{index}")
        c_row = matches[0]
        identity = key(c_row)
        f_row = f_by_key.get(identity)
        if f_row is None:
            _fail(f"legacy_wire:F_identity_missing:{index}")
        c_send = int(c_row["c_to_f_sent_bytes"])
        c_recv = int(c_row["f_to_c_received_bytes"])
        f_recv = int(f_row["c_to_f_received_bytes"])
        f_send = int(f_row["f_to_c_sent_bytes"])
        if (c_send <= 0 or c_recv <= 0 or f_recv <= 0 or f_send <= 0 or
                int(c_row["c_to_f_received_bytes"]) != 0 or
                int(c_row["f_to_c_sent_bytes"]) != 0 or
                int(f_row["c_to_f_sent_bytes"]) != 0 or
                int(f_row["f_to_c_received_bytes"]) != 0 or
                c_send != f_recv or c_recv != f_send):
            _fail(f"legacy_wire:directional_bytes_mismatch:{index}")
        assignment = assignments[index]
        result.append({
            "c_to_f_bytes": c_send,
            "f_to_c_bytes": c_recv,
            "channel_bytes": c_send + c_recv,
            "legacy_wire_identity": {
                "job_id": identity[0], "assignment_epoch": identity[1],
                "assignment_nonce": identity[2], "c_guid": identity[3],
                "tu_seq": identity[4],
            },
            "planned_relationship": int(assignment["relationship"]),
            "planned_admission_lane": int(assignment["f_slot"]),
            "relationship": int(assignment["relationship"]),
            "f_slot": int(assignment["f_slot"]),
        })
    return result


def _validate_warm_continuation(prewarm: list[dict[str, Any]],
                                measured: list[dict[str, Any]], expected_count: int,
                                suite: str = TOPOLOGY) -> None:
    """Bind measured TU zero to the terminal prewarm transaction state."""
    if len(prewarm) != expected_count or len(measured) < expected_count:
        _fail("action_trace:prewarm_count_mismatch")
    if suite == PARALLEL_TOPOLOGY:
        for relationship in range(RELATIONSHIP_COUNT[suite]):
            prior = [item for item in prewarm if item["relationship"] == relationship]
            current = [item for item in measured if item["relationship"] == relationship]
            if not prior or not current:
                _fail("action_trace:relationship_missing_from_warm_continuation")
            terminal, first = prior[-1], current[0]
            if (terminal["f_store_guid"] != first["f_store_guid"] or
                    first["tu_seq"] <= terminal["tu_seq"] or
                    first["rel_seq"] != terminal["rel_seq"] + 1 or
                    first["history_nonce"] != terminal["history_nonce"]):
                _fail("action_trace:relationship_state_does_not_continue_prewarm")
        return
    terminal, first = prewarm[-1], measured[0]
    if (terminal["c_store_guid"] != first["c_store_guid"] or
            terminal["f_store_guid"] != first["f_store_guid"] or
            first["tu_seq"] != terminal["tu_seq"] + 1 or
            first["rel_seq"] != terminal["rel_seq"] + 1 or
            first["history_nonce"] != terminal["history_nonce"]):
        _fail("action_trace:measured_state_does_not_continue_prewarm")


def _write_new(path: Path, raw: bytes) -> None:
    if path.exists() or path.is_symlink():
        _fail(f"output_exists:{path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())


def _retained_workdir(stdout: str, *, host_workdir: Path | None = None,
                      reported_workdir: Path | None = None) -> Path:
    work_lines = [line.split("=", 1)[1] for line in stdout.splitlines()
                  if line.startswith("S7_WORKDIR=")]
    if len(work_lines) != 1:
        _fail("product_run:workdir_missing")
    observed = Path(work_lines[0])
    if host_workdir is None:
        if reported_workdir is not None:
            _fail("product_run:workdir_mapping_incomplete")
        work = observed
    else:
        if (reported_workdir != DEFAULT_REPORTED_WORKDIR or
                observed != DEFAULT_REPORTED_WORKDIR):
            _fail("product_run:workdir_mapping_mismatch")
        work = host_workdir
    try:
        canonical_work = work.resolve(strict=True)
    except OSError:
        _fail("product_run:workdir_unavailable")
    if (not work.is_absolute() or work.is_symlink() or not work.is_dir() or
            canonical_work != work):
        _fail("product_run:workdir_unavailable")
    return work


def finalize(stdout: str, returncode: int, *, batch_manifest: Path, topology: Path,
             predictive_plan: Path, output: Path, profile: str, product_root: Path,
             repeat_predictive_plan: Path | None = None,
             product_profile: str | None = None,
             corpus: str = "DuckDB", regime: str = "cold", depth: str = "100",
             full_count: int | None = None, passes: int = 2,
             timeout_seconds: int | None = None,
             artifact_sample: int = 2, retain_all_artifacts: bool = False,
             launch_identity: dict[str, Any] | None = None,
             execution_environment: str = "host_product_build",
             runtime_image: dict[str, str] | None = None,
             host_descriptor_path: Path | None = None,
             host_workdir: Path | None = None,
             reported_workdir: Path | None = None,
             external_farm: ExternalFarmFinalization | Mapping[str, object] | None = None,
             timestamp: str | None = None, suite: str = TOPOLOGY) -> Path:
    """Turn one completed product invocation into two authenticated live curves."""
    external_binding: dict[str, object] | None = None
    external_input: ExternalFarmFinalization | None = None
    external_stdout = stdout
    if external_farm is not None:
        external_input = _external_input(external_farm)
        # An external result is a replacement for the local launch lifecycle;
        # accepting either authority would make local elapsed time relabelable.
        if launch_identity is not None or host_workdir is not None or reported_workdir is not None:
            _fail("external_farm:local_execution_authority_present")
        if execution_environment != "external_farm_product_build":
            _fail("external_farm:execution_environment_invalid")
        external_binding = _external_farm_binding(external_input, suite)
        external_stdout = Path(external_input.stdout_path).read_text(encoding="utf-8")
        stdout = external_stdout
    measurement_method, effective_product_profile, calibration_eligible = \
        _measurement_descriptor(profile, product_profile)
    raw_ii = effective_product_profile == RAW_II_PROFILE
    transfer_accounting = _transfer_accounting(effective_product_profile)
    if returncode != 0 or "PASS: all-P50 C1F1" not in stdout:
        _fail("product_run:did_not_pass")
    if execution_environment not in {"host_product_build", "pinned_container_product_build",
                                     "external_farm_product_build"}:
        _fail("execution_environment:invalid")
    # This runner co-locates C, F, the scheduler, caches, and compilers.  Its
    # elapsed time remains useful for protocol/output evidence only.
    execution_scope = (EXTERNAL_FARM_EXECUTION_SCOPE if external_binding is not None
                       else LOOPBACK_EXECUTION_SCOPE)
    if (external_binding is None and
            (execution_environment == "pinned_container_product_build") !=
            (runtime_image is not None)):
        _fail("execution_environment:image_binding_invalid")
    if external_binding is not None and not external_binding.get("hosts"):
        _fail("external_farm:authority_hosts_missing")
    if runtime_image is not None and (
            set(runtime_image) != {"reference", "image_id", "architecture", "os", "created"} or
            IMAGE_ID.fullmatch(runtime_image.get("image_id", "")) is None):
        _fail("execution_environment:image_identity_invalid")
    if corpus not in CORPORA or regime not in REGIMES or suite not in TOPOLOGIES:
        _fail("cell:undeclared")
    if passes not in (1, 2) or type(artifact_sample) is not int or artifact_sample < 0:
        _fail("run_options:invalid")
    expected_count = selected_count(depth, full_count)
    effective_timeout = derive_timeout(expected_count, passes, regime == "warm", timeout_seconds)
    plan_profile = _plan_identity_profile(profile, product_profile)
    plan, plan_inputs, plan_sha = load_predictive_plan(
        predictive_plan, corpus=corpus, profile=plan_profile, regime=regime, depth=depth)
    repeat_plan: dict[str, Any] | None = None
    repeat_plan_sha: str | None = None
    if repeat_predictive_plan is not None:
        if depth != "full" or passes != 2:
            _fail("repeat_predictive_plan:only_valid_for_full_two_pass")
        repeat_plan, _repeat_inputs, repeat_plan_sha = load_repeat_predictive_plan(
            repeat_predictive_plan, first_path=predictive_plan, first_plan=plan,
            first_inputs=plan_inputs, first_sha=plan_sha, corpus=corpus,
            profile=plan_profile, regime=regime)
    elif depth == "full" and passes == 2:
        _fail("repeat_predictive_plan:required_for_full_two_pass")
    if full_count is None and depth == "full":
        full_count = len(plan_inputs)
    expected_count = selected_count(depth, full_count)
    rows = load_batch_manifest(batch_manifest, expected_count)
    bind_batch_to_plan(rows, plan_inputs)
    topology_sha = load_topology(topology, rows, suite, plan.get("scheduling"))
    topology_value = json.loads(topology.read_text())
    assignments = topology_value.get("assignments", topology_value.get("inputs"))
    work = _retained_workdir(stdout, host_workdir=host_workdir,
                             reported_workdir=reported_workdir)
    def output_value(prefix: str) -> str:
        values = [line.split("=", 1)[1] for line in stdout.splitlines()
                  if line.startswith(prefix + "=")]
        if len(values) != 1:
            _fail(f"product_run:{prefix}:missing_or_duplicate")
        return values[0]
    if output_value("S8_BATCH_COUNT") != str(expected_count):
        _fail("product_run:batch_count_mismatch")
    if output_value("S8_SUITE") != suite:
        _fail("product_run:suite_mismatch")
    if output_value("S8_BATCH_PASSES") != str(passes):
        _fail("product_run:batch_passes_mismatch")
    if output_value("S8_BATCH_WARM") != str(int(regime == "warm")):
        _fail("product_run:batch_regime_mismatch")
    scheduling_marker = (
        "S8_SCHEDULING mode=relationship-ordered execution_slots=40 relationships=20 "
        "planned_admission_lanes_per_relationship=2 physical_slot_observed=0"
        if suite == PARALLEL_TOPOLOGY else
        "S8_SCHEDULING mode=relationship-ordered execution_slots=1 relationships=1 "
        "planned_admission_lanes_per_relationship=1"
    )
    if scheduling_marker not in stdout.splitlines():
        _fail("product_run:scheduling_identity_missing")
    environment_preparation = _environment_preparation(stdout, work, suite)
    expected_binaries = _binary_identity(stdout)
    commit, tree, binaries, runner_sha = product_identity(product_root)
    if binaries != expected_binaries:
        _fail("binary_identity:stdout_mismatch")
    if external_binding is not None:
        mapping = external_binding["authority_value"]["placements"][suite]["relationship_hosts"]
        used_hosts = ["q3", *dict.fromkeys(mapping)]
        authority_binaries = external_binding["hosts"]["q3"]["binaries"]
        if any(external_binding["hosts"][host]["binaries"] != authority_binaries
               for host in used_hosts):
            _fail("external_farm.authority:per_host_binary_mismatch")
        # The product identity is the local final-head binary inventory.  It
        # is checked directly against the captured authority (the stdout
        # inventory was already checked above), so a caller cannot fabricate
        # an otherwise matching S8_BINARY line.
        try:
            create_env_sha, _create_env_bytes = _sha(
                product_root / "client/icecc-create-env")
        except LiveRunnerError as exc:
            raise LiveRunnerError("external_farm.authority:product_binary_mismatch") from exc
        local_product_binaries = {**binaries, "client/icecc-create-env": create_env_sha}
        if (set(local_product_binaries) != set(authority_binaries) or
                any(authority_binaries.get(role) != digest
                    for role, digest in local_product_binaries.items())):
            _fail("external_farm.authority:product_binary_mismatch")
    if launch_identity is not None:
        if (launch_identity.get("source_commit") != commit or
                launch_identity.get("source_tree") != tree or
                launch_identity.get("binary_sha256") != binaries or
                launch_identity.get("runner_sha256") != runner_sha):
            _fail("product_identity:changed_during_run")
    observations = _timing_rows(stdout, rows, work, passes, assignments, suite,
                                reported_workdir)
    batch_windows = _batch_windows(stdout, observations, rows, passes, suite)
    # Product execution uses the GRZ alias, but evidence is declared by the
    # harness profile.  Keep the original profile here so GRZ_RESIDUAL stays
    # distinct in identity and validation while product accounting remains GRZ.
    _validate_product_log_evidence(work, observations, profile)
    calibration_metadata: dict[str, str] | None = None
    host_descriptor_binding: dict[str, object] | None = None
    role_placement: dict[str, object] | None = None
    if launch_identity is not None or external_binding is not None:
        if external_binding is not None:
            # Re-read both authority files immediately before deriving
            # metadata.  A transport cannot swap a valid authority after the
            # initial gate and still label the retained run with its hash.
            rebound = _external_farm_binding(external_input, suite)  # type: ignore[arg-type]
            if rebound != external_binding:
                _fail("external_farm:authority_changed_during_finalize")
            calibration_metadata = _external_calibration_metadata(
                external_binding, work, environment_preparation, rows, observations, suite)
            role_placement = external_binding["role_placement"]
            host_descriptor_binding = {
                "mode": "external_farm",
                "hosts": {host: external_binding["hosts"][host]["descriptor"]
                          for host in sorted(external_binding["hosts"])
                          if host == "q3" or host in external_binding["authority_value"]["placements"][suite]["relationship_hosts"]},
            }
        else:
            if runtime_image is None or host_descriptor_path is None:
                _fail("calibration_metadata:authority_missing")
            if launch_identity.get("runtime_image") != runtime_image:
                _fail("calibration_metadata:runtime_image_changed_during_run")
            host_digest, host_bytes = _authenticated_host_binding(
                host_descriptor_path, launch_identity)
        if external_binding is None:
            calibration_metadata = _calibration_metadata(
                runtime_image=runtime_image, preparation=environment_preparation,
                work=work, rows=rows, observations=observations, suite=suite,
                host_descriptor=host_descriptor_path)
            host_descriptor_binding = {"path": "product-evidence/host-descriptor.json",
                                       "sha256": host_digest, "bytes": host_bytes}
            role_placement = _co_resident_role_placement(
                calibration_metadata["host_digest"], suite)
            try:
                normalizer._validate_role_placement(role_placement, "live")
            except normalizer.NormalizationError as exc:
                raise LiveRunnerError("role_placement:local_identity_invalid") from exc
    measured_f_action_trace = _external_measured_f_action_trace(
        work, external_binding is not None)
    stages = (_legacy_wire_stage(work, observations, assignments, suite)
              if raw_ii else _action_stage(work, len(observations), assignments, suite,
                                           measured_f_action_trace))
    if len(stages) != len(observations):
        _fail("action_trace:stage_count_mismatch")
    for observation, action in zip(observations, stages, strict=True):
        if (not raw_ii and (observation["observed_source_tu_seq"] != action["tu_seq"] or
                observation["observed_f_service_identity"] !=
                action["observed_f_service_identity"] or
                observation["planned_relationship"] != action["planned_relationship"] or
                observation["planned_admission_lane"] != action["planned_admission_lane"])):
            _fail("action_trace:job_topology_binding_mismatch")
        observation.update(action)
        observation["c_to_f_bytes"] = action["c_to_f_bytes"]
        if not raw_ii:
            observation["f_to_c_bytes"] = observation["returned_object_bytes"]
            observation["channel_bytes"] = (observation["c_to_f_bytes"] +
                                             observation["f_to_c_bytes"])
        observation["elapsed_ns"] = observation["measured_elapsed_ns"]
        if action["c_to_f_bytes"] <= 0:
            _fail("action_trace:zero_stage_bytes")
    prewarm_stages: list[dict[str, Any]] = []
    if regime == "warm" and not raw_ii:
        prewarm_stages = _action_stage_paths(
            work / "s7-prewarm-c-action-trace.jsonl",
            work / "s7-prewarm-f-action-trace.jsonl", expected_count,
            assignments, suite)
        _validate_warm_continuation(prewarm_stages, stages, expected_count, suite)
    batch_manifest_sha, batch_manifest_bytes = _sha(batch_manifest)
    input_sha = hashlib.sha256(_canonical({"source_manifest_sha256": plan["source_manifest"]["sha256"],
                                           "inputs": plan_inputs})).hexdigest()
    try:
        comparison = normalizer.comparison_descriptor(plan_sha, plan.get("scheduling"))
        comparisons = {"full-1": comparison}
        if passes == 2:
            comparisons["full-2"] = normalizer.comparison_descriptor(
                repeat_plan_sha if repeat_plan_sha is not None else plan_sha,
                repeat_plan.get("scheduling") if repeat_plan is not None else plan.get("scheduling"))
    except normalizer.NormalizationError as exc:
        _fail(f"predictive_plan:{exc}")
    if timestamp is None:
        timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    if TIMESTAMP.fullmatch(timestamp) is None:
        _fail("timestamp:invalid")
    target = (output / "icecream" / suite.replace("/", "-") / timestamp /
              (RAW_II_PROFILE if raw_ii else profile))
    target.parent.mkdir(parents=True, exist_ok=True)
    target.mkdir(parents=True, exist_ok=False)
    # Preserve the product-generated files, not just their digests.  The
    # source workdir remains untouched; these are private copies in the new
    # immutable experiment directory.
    retained = target / "product-evidence"
    retained.mkdir()
    shutil.copy2(batch_manifest, retained / "batch-manifest.jsonl")
    shutil.copy2(topology, retained / "topology.json")
    shutil.copy2(predictive_plan, retained / "predictive-plan.json")
    if repeat_predictive_plan is not None:
        shutil.copy2(repeat_predictive_plan, retained / "predictive-plan-full-2.json")
    if host_descriptor_path is not None and calibration_metadata is not None:
        shutil.copy2(host_descriptor_path, retained / "host-descriptor.json")
        retained_host_sha, retained_host_bytes = _sha(retained / "host-descriptor.json")
        if (host_descriptor_binding is None or
                retained_host_sha != host_descriptor_binding["sha256"] or
                retained_host_bytes != host_descriptor_binding["bytes"]):
            _fail("host_descriptor:snapshot_changed_during_copy")
    if external_binding is not None:
        # The source descriptors were re-authenticated immediately before
        # finalization.  Verify the copies too, so every downstream manifest
        # points at bytes retained beside the product evidence.
        manifest_source = external_input.manifest_path  # type: ignore[union-attr]
        authority_source = external_input.authority_path  # type: ignore[union-attr]
        stdout_source = external_input.stdout_path  # type: ignore[union-attr]
        receipt_source = external_input.receipt_path  # type: ignore[union-attr]
        shutil.copy2(manifest_source, retained / "external-farm-manifest.json")
        shutil.copy2(authority_source, retained / "external-farm-authority.json")
        shutil.copy2(stdout_source, retained / "product-output.log")
        shutil.copy2(receipt_source, retained / "external-farm-receipt.json")
        copied_manifest_sha, copied_manifest_bytes = _sha(
            retained / "external-farm-manifest.json")
        copied_authority_sha, copied_authority_bytes = _sha(
            retained / "external-farm-authority.json")
        if (copied_manifest_sha != external_binding["manifest"]["sha256"] or
                copied_manifest_bytes != external_binding["manifest"]["bytes"] or
                copied_authority_sha != external_binding["authority"]["sha256"] or
                copied_authority_bytes != external_binding["authority"]["bytes"]):
            _fail("external_farm:source_changed_during_copy")
        if (_sha(retained / "product-output.log") !=
                (external_binding["stdout"]["sha256"], external_binding["stdout"]["bytes"]) or
                _sha(retained / "external-farm-receipt.json") !=
                (external_binding["receipt"]["sha256"], external_binding["receipt"]["bytes"])):
            _fail("external_farm:stdout_or_receipt_changed_during_copy")
        copied_descriptors: dict[str, dict[str, object]] = {}
        used_hosts = ["q3", *dict.fromkeys(
            external_binding["authority_value"]["placements"][suite]["relationship_hosts"])]
        for host in sorted(used_hosts):
            source = Path(external_binding["hosts"][host]["descriptor"]["path"])
            destination = retained / f"external-host-descriptor-{host}.json"
            shutil.copy2(source, destination)
            copied_sha, copied_bytes = _sha(destination)
            expected = external_binding["hosts"][host]["descriptor"]
            if copied_sha != expected["sha256"] or copied_bytes != expected["bytes"]:
                _fail(f"external_farm:host_descriptor_changed_during_copy:{host}")
            copied_descriptors[host] = {"path": f"product-evidence/{destination.name}",
                                        "sha256": copied_sha, "bytes": copied_bytes}
        host_descriptor_binding = {"mode": "external_farm", "hosts": copied_descriptors}
    payload_raw = _canonical({"source_manifest_sha256": plan["source_manifest"]["sha256"],
                              "inputs": plan_inputs})
    _write_new(retained / "input-descriptors.json", payload_raw)
    retained_input_sha, retained_input_bytes = _sha(retained / "batch-manifest.jsonl")
    retained_payload_sha, retained_payload_bytes = _sha(retained / "input-descriptors.json")
    retained_topology_sha, retained_topology_bytes = _sha(retained / "topology.json")
    retained_plan_sha, retained_plan_bytes = _sha(retained / "predictive-plan.json")
    retained_repeat_plan_sha: str | None = None
    retained_repeat_plan_bytes: int | None = None
    if repeat_predictive_plan is not None:
        retained_repeat_plan_sha, retained_repeat_plan_bytes = _sha(
            retained / "predictive-plan-full-2.json")
    if (retained_input_sha != batch_manifest_sha or retained_topology_sha != topology_sha or
            retained_plan_sha != plan_sha or retained_payload_sha != input_sha):
        _fail("manifest_snapshot:changed_during_copy")
    if (repeat_predictive_plan is not None and
            retained_repeat_plan_sha != repeat_plan_sha):
        _fail("repeat_predictive_plan:snapshot_changed_during_copy")
    trace_paths: list[tuple[Path, str]] = ([] if raw_ii else [
        (work / "s7-measured-c-action-trace.jsonl",
         "s7-measured-c-action-trace.jsonl"),
        (measured_f_action_trace, "s7-measured-f-action-trace.jsonl"),
    ])
    if regime == "warm" and not raw_ii:
        trace_paths.extend(((work / "s7-prewarm-c-action-trace.jsonl",
                             "s7-prewarm-c-action-trace.jsonl"),
                            (work / "s7-prewarm-f-action-trace.jsonl",
                             "s7-prewarm-f-action-trace.jsonl")))
    for path, retained_name in trace_paths:
        if not path.is_file():
            _fail(f"action_trace:retained_file_missing:{path.name}")
        shutil.copy2(path, retained / retained_name)
    wire_paths = [work / "s7-measured-c-legacy-wire-trace.jsonl"]
    wire_paths.extend(sorted(work.glob("s7-measured-f-legacy-wire-trace-*.jsonl"))
                        if suite == PARALLEL_TOPOLOGY else
                        [work / "s7-measured-f-legacy-wire-trace.jsonl"])
    if raw_ii:
        for path in wire_paths:
            if not path.is_file() or path.is_symlink():
                _fail(f"legacy_wire:retained_file_missing:{path.name}")
            shutil.copy2(path, retained / path.name)
    for path in sorted(work.glob("ready-*.trace")) + sorted(work.glob("s8-environment-warmup-*.jsonl")):
        if path.is_file() and not path.is_symlink():
            shutil.copy2(path, retained / path.name)
    service_map_raw = b""
    if suite == PARALLEL_TOPOLOGY and not raw_ii:
        service_map_path = work / "s8-f-service-map.tsv"
        # Parsing above authenticated the exact 20-row GUID/service binding.
        service_map_raw = service_map_path.read_bytes()
        shutil.copy2(service_map_path, retained / service_map_path.name)
    # Logs are product evidence, not caller-provided measurements.  Preserve
    # every lifecycle log so assignment and timing rows can be audited later.
    for path in sorted(work.glob("*.log")):
        if path.is_file() and path.name not in {p.name for p, _ in trace_paths}:
            shutil.copy2(path, retained / path.name)
    assignment_raw = _canonical({"suite": suite, "assignments": assignments}) + b"\n"
    _write_new(retained / "assignment-witness.json", assignment_raw)
    for item in observations:
        if retain_all_artifacts or item["ordinal"] < artifact_sample:
            for field in ("preprocessed_path", "remote_path", "local_path"):
                source = Path(item[field])
                shutil.copy2(source, retained / source.name)
            log = work / f"client-compile-{item['run']}-{item['ordinal']}.log"
            shutil.copy2(log, retained / log.name)
    identity_profile = effective_product_profile if raw_ii else profile
    cell = f"{corpus}/{identity_profile}/{regime}"
    split = SPLITS[corpus]
    summary_value = {"schema": "icecream-s7-live-cell-v1",
                     "cell": cell, "split": split, "status": "PASS",
                     "measurement_method": measurement_method,
                     "product_profile": effective_product_profile,
                     "calibration_eligible": calibration_eligible,
                     "transfer_accounting": transfer_accounting,
                     "live_status": "PASS", "acceptance_status": "PASS",
                     "conformance_status": "PASS", "binary_sha256": binaries,
                     "measured": {"input_sha256": input_sha}}
    if external_binding is not None:
        summary_value["external_farm"] = {
            "manifest": external_binding["manifest"],
            "authority": external_binding["authority"],
            "stdout": external_binding["stdout"],
            "receipt": external_binding["receipt"],
        }
    summary_raw = _canonical(summary_value) + b"\n"
    _write_new(target / "results.jsonl", summary_raw)
    timing_by_run: dict[str, bytes] = {}
    run_names = ["full-1"] + (["full-2"] if passes == 2 else [])
    for run in run_names:
        timing_rows = [{"schema": "icecream-s7-live-timing-v1", "cell": cell,
                        "phase": "measured", "ordinal": item["ordinal"], "tu_id": item["tu_id"],
                        "elapsed_ns": item["elapsed_ns"], "channel_bytes": item["channel_bytes"],
                        "C_TO_F_bytes": item["c_to_f_bytes"], "F_TO_C_bytes": item["f_to_c_bytes"],
                        "object_sha256": item["remote_sha256"], "remote_compile": True,
                        "wait_for_cs_ns": item["wait_for_cs_ns"],
                        "client_elapsed_ns": item["client_elapsed_ns"],
                        "returned_object_bytes": item["returned_object_bytes"], "run": run,
                        "measurement_window": "compile+result_return",
                        "admission_start_ns": item["admission_start_ns"],
                        "input_ready_ns": item["input_ready_ns"],
                        "compile_start_ns": item["compile_start_ns"],
                        "compile_end_ns": item["compile_end_ns"],
                        "witness_end_ns": item["witness_end_ns"],
                        "planned_assignment_ordinal": item["planned_assignment_ordinal"],
                        "planned_relationship": item["planned_relationship"],
                        "planned_admission_lane": item["planned_admission_lane"],
                        "observed_scheduler_job_id": item["observed_scheduler_job_id"],
                        "observed_f_service_identity": item["observed_f_service_identity"],
                        "observed_source_tu_seq": item["observed_source_tu_seq"],
                        **({"tu_seq": item["tu_seq"],
                            "rel_seq": item["rel_seq"], "c_store_guid": item["c_store_guid"],
                            "f_store_guid": item["f_store_guid"], "state_digest": item["state_digest"],
                            "f_state_digest": item["f_state_digest"],
                            "transaction_digest": item["transaction_digest"],
                            "raw_digest": item["raw_digest"], "f_raw_digest": item["f_raw_digest"],
                            "history_nonce": item["history_nonce"]} if not raw_ii else {})}
                       for item in observations if item["run"] == run]
        timing_raw = b"".join(_canonical(row) + b"\n" for row in timing_rows)
        timing_by_run[run] = timing_raw
        _write_new(target / f"timing_{run}.jsonl", timing_raw)
    # The current S8 live normalizer consumes one measured curve and rejects
    # duplicate TU identities.  Keep full-1 as the canonical live package and
    # retain the state-carrying repeat as a separately named witness.
    timing_raw = timing_by_run["full-1"]
    _write_new(target / "timing.jsonl", timing_raw)
    c_action_raw = ((retained / "s7-measured-c-action-trace.jsonl").read_bytes()
                    if not raw_ii else b"")
    f_action_raw = ((retained / "s7-measured-f-action-trace.jsonl").read_bytes()
                    if not raw_ii else b"")
    c_wire_raw = ((retained / "s7-measured-c-legacy-wire-trace.jsonl").read_bytes()
                  if raw_ii else b"")
    f_wire_raw = (b"".join((retained / path.name).read_bytes()
                            for path in wire_paths[1:]) if raw_ii else b"")
    prewarm_descriptor = None
    if regime == "warm" and not raw_ii:
        pre_c_raw = (retained / "s7-prewarm-c-action-trace.jsonl").read_bytes()
        pre_f_raw = (retained / "s7-prewarm-f-action-trace.jsonl").read_bytes()
        prewarm_descriptor = {"count": len(prewarm_stages), "input_digest": input_sha,
                              "c_action_sha256": hashlib.sha256(pre_c_raw).hexdigest(),
                              "f_action_sha256": hashlib.sha256(pre_f_raw).hexdigest(),
                              "terminal_tu_seq": prewarm_stages[-1]["tu_seq"],
                              "terminal_rel_seq": prewarm_stages[-1]["rel_seq"]}
    evidence_sha = hashlib.sha256(_canonical({
        "results": hashlib.sha256(summary_raw).hexdigest(),
        "c_action": hashlib.sha256(c_action_raw).hexdigest(),
        "f_action": hashlib.sha256(f_action_raw).hexdigest(),
        "c_legacy_wire": hashlib.sha256(c_wire_raw).hexdigest() if raw_ii else None,
        "f_legacy_wire": hashlib.sha256(f_wire_raw).hexdigest() if raw_ii else None,
        "f_service_map": hashlib.sha256(service_map_raw).hexdigest() if service_map_raw else None,
    })).hexdigest()
    witness = {"assignment": {"path": "product-evidence/assignment-witness.json",
                               "sha256": hashlib.sha256(assignment_raw).hexdigest(),
                               "bytes": len(assignment_raw)}}
    if not raw_ii:
        witness["c_action"] = {"path": "product-evidence/s7-measured-c-action-trace.jsonl",
                                "sha256": hashlib.sha256(c_action_raw).hexdigest(), "bytes": len(c_action_raw)}
        witness["f_action"] = {"path": "product-evidence/s7-measured-f-action-trace.jsonl",
                                "sha256": hashlib.sha256(f_action_raw).hexdigest(), "bytes": len(f_action_raw)}
    else:
        witness["c_legacy_wire"] = {"path": "product-evidence/s7-measured-c-legacy-wire-trace.jsonl",
                                     "sha256": hashlib.sha256(c_wire_raw).hexdigest(), "bytes": len(c_wire_raw)}
        witness["f_legacy_wire"] = {"paths": [f"product-evidence/{path.name}" for path in wire_paths[1:]],
                                     "sha256": hashlib.sha256(f_wire_raw).hexdigest(), "bytes": len(f_wire_raw)}
    if service_map_raw:
        witness["f_service_map"] = {
            "path": "product-evidence/s8-f-service-map.tsv",
            "sha256": hashlib.sha256(service_map_raw).hexdigest(),
            "bytes": len(service_map_raw),
        }
    if passes == 2:
        witness["timing_full_2"] = {"path": "timing_full-2.jsonl", "sha256": hashlib.sha256(timing_by_run["full-2"]).hexdigest(), "bytes": len(timing_by_run["full-2"])}
    predictive_plans: dict[str, dict[str, object]] = {
        "full-1": {"path": "product-evidence/predictive-plan.json",
                   "sha256": retained_plan_sha, "bytes": retained_plan_bytes},
    }
    if retained_repeat_plan_sha is not None and retained_repeat_plan_bytes is not None:
        predictive_plans["full-2"] = {
            "path": "product-evidence/predictive-plan-full-2.json",
            "sha256": retained_repeat_plan_sha,
            "bytes": retained_repeat_plan_bytes,
        }
    evidence_value = {"schema": "icecream-s7-live-evidence-v2",
                      "cell": cell, "split": split, "run_id": "s8-real-c1f1",
                      "measurement_method": measurement_method,
                      "product_profile": effective_product_profile,
                      "calibration_eligible": calibration_eligible,
                      "transfer_accounting": transfer_accounting,
                      "source_commit": commit, "source_tree": tree,
                      "input_manifest_sha256": batch_manifest_sha, "input_digest": input_sha,
                      "topology_sha256": topology_sha,
                      "input_manifest": {"path": "product-evidence/batch-manifest.jsonl",
                                          "sha256": retained_input_sha,
                                          "bytes": retained_input_bytes},
                      "input_descriptors": {"path": "product-evidence/input-descriptors.json",
                                            "sha256": retained_payload_sha,
                                            "bytes": retained_payload_bytes},
                      "predictive_plan": {"path": "product-evidence/predictive-plan.json",
                                          "sha256": retained_plan_sha, "bytes": retained_plan_bytes},
                      "predictive_plans": predictive_plans,
                      "comparison": comparison,
                      "comparisons": comparisons,
                      "topology": {"path": "product-evidence/topology.json",
                                   "sha256": retained_topology_sha,
                                   "bytes": retained_topology_bytes},
                      "runner": {"name": "p50compilee2e-run.sh", "sha256": runner_sha},
                      "binary_sha256": binaries,
                      "execution_environment": execution_environment,
                      "execution_scope": execution_scope,
                      **({"role_placement": role_placement}
                         if role_placement is not None else {}),
                      "execution_limits": {"timeout_seconds": effective_timeout},
                      "runtime_image": runtime_image,
                      "diagnostic_policy": {
                          "icecc_caret_workaround": SCORED_CARET_WORKAROUND,
                          "default_caret_companion_excluded": True,
                      },
                      "state_carrying_repeat": passes == 2,
                      "measurement_window": "compile+result_return",
                      "environment_preparation": environment_preparation,
                      "prewarm": prewarm_descriptor,
                      **({"calibration_metadata": calibration_metadata,
                          "host_descriptor": host_descriptor_binding}
                         if calibration_metadata is not None else {}),
                      "evidence": {"results": {"path": "results.jsonl", "sha256": hashlib.sha256(summary_raw).hexdigest(), "bytes": len(summary_raw)},
                                   "timing": {"path": "timing.jsonl", "sha256": hashlib.sha256(timing_raw).hexdigest(), "bytes": len(timing_raw)}},
                      "witness": witness}
    if external_binding is not None:
        evidence_value["external_farm"] = {
            "manifest": external_binding["manifest"],
            "authority": external_binding["authority"],
            "stdout": external_binding["stdout"],
            "receipt": external_binding["receipt"],
        }
    evidence_value["evidence_sha256"] = hashlib.sha256(_canonical(evidence_value)).hexdigest()
    evidence_raw = _canonical(evidence_value) + b"\n"
    _write_new(target / "evidence.json", evidence_raw)
    evidence_manifest_sha = hashlib.sha256(evidence_raw).hexdigest()
    records: list[dict[str, Any]] = []
    manifests: dict[str, str] = {}
    for run in run_names:
        selected = [row for row in observations if row["run"] == run]
        curve = _live_curve_rows(
            selected, rows,
            {"corpus": corpus, "profile": identity_profile, "regime": regime},
            batch_windows[run]["start_ns"],
        )
        curve_raw = b"".join(_canonical(row) + b"\n" for row in curve)
        curve_name = f"live_curve_{run}.jsonl"
        manifest_name = f"live_curve_manifest_{run}.json"
        manifest_value = {"schema": normalizer.MANIFEST_SCHEMA,
                          "identity": {"corpus": corpus, "profile": identity_profile, "regime": regime,
                                        "split": split, "run_id": run,
                                        "source_commit": commit, "source_tree": tree,
                                        "input_digest": input_sha, "topology_digest": topology_sha,
                                        "model_id": "s8-real-live"},
                          "comparison": comparisons[run],
                          "units": {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns",
                                    "C_TO_F_bytes": "bytes", "F_TO_C_bytes": "bytes",
                                    "throughput_bytes_per_s": "bytes_per_s"},
                          "curve": {"path": curve_name, "sha256": hashlib.sha256(curve_raw).hexdigest(), "bytes": len(curve_raw)},
                          "execution_scope": execution_scope,
                          **({"role_placement": role_placement}
                             if role_placement is not None else {}),
                          **({"control_baseline": normalizer.CONTROL_BASELINE}
                             if raw_ii else {}),
                          "provenance": {"mode": "live", "producer": "s8_real_c1f1_live_runner", "trace_free": False},
                          "evidence": {"results_sha256": hashlib.sha256(summary_raw).hexdigest(),
                                       "evidence_manifest_sha256": evidence_manifest_sha,
                                       "binary_sha256": binaries, "evidence_sha256": evidence_value["evidence_sha256"]}}
        if calibration_metadata is not None:
            manifest_value.update(calibration_metadata)
        manifest_raw = _canonical(manifest_value) + b"\n"
        _write_new(target / curve_name, curve_raw)
        _write_new(target / manifest_name, manifest_raw)
        artifact = {"identity": manifest_value["identity"],
                    "comparison": manifest_value["comparison"],
                    "units": manifest_value["units"],
                    "provenance": manifest_value["provenance"], "manifest_sha256": hashlib.sha256(manifest_raw).hexdigest(),
                    "curve_sha256": hashlib.sha256(curve_raw).hexdigest(), "rows": curve,
                    "metadata": normalizer._validate_manifest_metadata(manifest_value),
                    "control_baseline": (normalizer.CONTROL_BASELINE
                                          if raw_ii else None),
                    "execution_scope": execution_scope,
                    "role_placement": role_placement,
                    "evidence": manifest_value["evidence"]}
        records.append(normalizer._normalized_record("live", artifact))
        manifests[run] = manifest_name
    _write_new(target / "live_curve.jsonl", (target / "live_curve_full-1.jsonl").read_bytes())
    _write_new(target / "live_curve_manifest.json", (target / "live_curve_manifest_full-1.json").read_bytes())
    _write_new(target / "records.jsonl", b"".join(_canonical(record) + b"\n" for record in records))
    experiment = {"schema": SCHEMA, "cell": {"corpus": corpus, "profile": identity_profile, "regime": regime},
                  "measurement_method": measurement_method,
                  "product_profile": effective_product_profile,
                  "calibration_eligible": calibration_eligible,
                  "transfer_accounting": transfer_accounting,
                  "split": split, "topology": suite, "depth": depth,
                  "declared_count": expected_count, "runs": ["full-1"] + (["full-2"] if passes == 2 else []),
                  "same_service_state": True, "input_manifest_sha256": batch_manifest_sha,
                  "input_digest": input_sha,
                  "topology_sha256": topology_sha, "predictive_plan_sha256": plan_sha,
                  "predictive_plan_sha256_by_run": {
                      run: comparisons[run]["plan_sha256"] for run in run_names
                  },
                  "execution_environment": execution_environment,
                  "execution_scope": execution_scope,
                  **({"role_placement": role_placement}
                     if role_placement is not None else {}),
                  "execution_limits": {"timeout_seconds": effective_timeout},
                  "runtime_image": runtime_image,
                  "diagnostic_policy": {
                      "icecc_caret_workaround": SCORED_CARET_WORKAROUND,
                      "default_caret_companion_excluded": True,
                  },
                  "binary_sha256": binaries, "source_commit": commit, "source_tree": tree,
                  "runner_sha256": runner_sha,
                  "environment_preparation": environment_preparation,
                  "launch_identity": launch_identity,
                  **({"calibration_metadata": calibration_metadata,
                      "host_descriptor": host_descriptor_binding}
                     if calibration_metadata is not None else {}),
                  "curve_manifests": manifests, "remote_compile_required": True,
                  "batch_windows": batch_windows,
                  "scheduling": ({
                      "mode": "relationship_ordered", "execution_slots": 40,
                      "planned_admission_lanes_per_relationship": 2,
                      "admission_lane_field": "planned_admission_lane",
                      "physical_slot_observed": False,
                      "source_admission": "per_relationship_source_commit_gate",
                      "observed_batch_concurrency": batch_windows,
                  } if suite == PARALLEL_TOPOLOGY else {
                      "mode": "relationship_ordered", "execution_slots": 1,
                      "planned_admission_lanes_per_relationship": 1,
                      "admission_lane_field": "planned_admission_lane",
                      "physical_slot_observed": False,
                      "source_admission": "per_relationship_source_commit_gate",
                      "observed_batch_concurrency": batch_windows,
                  }),
                  "artifact_retention": {"mode": "all" if retain_all_artifacts else "sample",
                                         "sample_tus_per_run": artifact_sample},
                  "prewarm": regime == "warm", "prewarm_evidence": prewarm_descriptor,
                  "suite": suite, "relationship_count": RELATIONSHIP_COUNT[suite],
                  "slots_per_f": SLOTS_PER_F[suite],
                  "assignment_witness": "product-evidence/assignment-witness.json"}
    if external_binding is not None:
        experiment["external_farm"] = {
            "manifest": external_binding["manifest"],
            "authority": external_binding["authority"],
            "stdout": external_binding["stdout"],
            "receipt": external_binding["receipt"],
        }
    _write_new(target / "experiment_manifest.json", _canonical(experiment) + b"\n")
    # The lifecycle temporary tree is diagnostic state on failure, but must
    # not survive a fully authenticated successful retention.
    if not work.name.startswith("p50compilee2e."):
        _fail("workdir:unsafe_cleanup_path")
    shutil.rmtree(work)
    return target


def _protect_child_from_oom() -> None:
    """Set and verify the runner child protection before product startup."""
    try:
        path = Path("/proc/self/oom_score_adj")
        path.write_text("-1000")
        if path.read_text().strip() != "-1000":
            raise OSError("oom_score_adj verification failed")
    except (OSError, UnicodeError, ValueError):
        # os._exit is intentional: a child that cannot establish protection
        # must not reach the measurement lifecycle.
        os._exit(125)


def _verify_child_oom(pid: int) -> bool:
    try:
        return Path(f"/proc/{pid}/oom_score_adj").read_text().strip() == "-1000"
    except (OSError, UnicodeError, ValueError):
        return False


def _run_product(command: list[str], timeout: int) -> tuple[str, int]:
    """Run the shell lifecycle as one process group with signal cleanup."""
    process = subprocess.Popen(command, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, start_new_session=True,
                               preexec_fn=_protect_child_from_oom)
    if not _verify_child_oom(process.pid):
        os.killpg(process.pid, signal.SIGKILL)
        raise LiveRunnerError("product_run:oom_protection_unavailable")
    previous: dict[int, Any] = {}

    def interrupt(signum: int, _frame: object) -> None:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
        raise KeyboardInterrupt

    try:
        for signum in (signal.SIGINT, signal.SIGTERM):
            previous[signum] = signal.signal(signum, interrupt)
        try:
            stdout, _ = process.communicate(timeout=timeout)
            return stdout, process.returncode
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                stdout, _ = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                stdout, _ = process.communicate()
            raise LiveRunnerError(f"product_run:timeout:{timeout}")
        except KeyboardInterrupt as exc:
            try:
                stdout, _ = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                stdout, _ = process.communicate()
            raise LiveRunnerError("product_run:interrupted") from exc
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--batch-manifest", type=Path, required=True)
    parser.add_argument("--predictive-plan", type=Path, required=True)
    parser.add_argument("--repeat-predictive-plan", type=Path)
    parser.add_argument("--topology", type=Path, required=True)
    parser.add_argument("--suite", choices=tuple(TOPOLOGIES), default=TOPOLOGY)
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--product-profile", choices=PRODUCT_PROFILES)
    parser.add_argument("--product-root", type=Path, required=True)
    parser.add_argument("--corpus", choices=CORPORA, default="DuckDB")
    parser.add_argument("--regime", choices=REGIMES, default="cold")
    parser.add_argument("--depth", choices=("100", "200", "full"), default="100")
    parser.add_argument("--full-count", type=int)
    parser.add_argument("--passes", type=int, choices=(1, 2), default=2)
    parser.add_argument("--timeout-seconds", type=int,
                        help=f"override timeout ({MIN_TIMEOUT_SECONDS}-{MAX_TIMEOUT_SECONDS} seconds)")
    parser.add_argument("--artifact-sample", type=int, default=2)
    parser.add_argument("--retain-all-artifacts", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timestamp", help="UTC experiment directory, YYYYMMDDTHHMMSSZ")
    parser.add_argument("--execution-mode", choices=("pinned-container", "host"),
                        default="pinned-container")
    parser.add_argument("--container-image", default=PINNED_IMAGE)
    parser.add_argument("--container-image-id",
                        help="exact content ID expected for --container-image")
    parser.add_argument("--container-bind-root", type=Path,
                        default=DEFAULT_CONTAINER_BIND_ROOT)
    parser.add_argument("--container-temp-root", type=Path,
                        default=DEFAULT_CONTAINER_TEMP_ROOT)
    parser.add_argument("--execute", action="store_true", help="execute one real run; intentionally separate from dry-run tests")
    args = parser.parse_args(argv)
    if args.execute and args.execution_mode == "host":
        _fail("calibration_metadata:runtime_image_required")
    batch_manifest = args.batch_manifest.absolute()
    predictive_plan = args.predictive_plan.absolute()
    repeat_predictive_plan = (args.repeat_predictive_plan.absolute()
                              if args.repeat_predictive_plan is not None else None)
    topology = args.topology.absolute()
    plan_profile = _plan_identity_profile(args.profile, args.product_profile)
    _plan, plan_inputs, _plan_sha = load_predictive_plan(
        predictive_plan, corpus=args.corpus, profile=plan_profile,
        regime=args.regime, depth=args.depth)
    if repeat_predictive_plan is not None:
        if args.depth != "full" or args.passes != 2:
            _fail("repeat_predictive_plan:only_valid_for_full_two_pass")
        load_repeat_predictive_plan(
            repeat_predictive_plan, first_path=predictive_plan, first_plan=_plan,
            first_inputs=plan_inputs, first_sha=_plan_sha, corpus=args.corpus,
            profile=plan_profile, regime=args.regime)
    elif args.depth == "full" and args.passes == 2:
        _fail("repeat_predictive_plan:required_for_full_two_pass")
    if args.depth == "full":
        args.full_count = len(plan_inputs)
    count = selected_count(args.depth, args.full_count)
    effective_timeout = derive_timeout(count, args.passes, args.regime == "warm",
                                       args.timeout_seconds)
    rows = load_batch_manifest(batch_manifest, count)
    load_topology(topology, rows, args.suite, _plan.get("scheduling"))
    run_workdir: Path | None = None
    command_workdir: Path | None = None
    run_work_parent: Path | None = None
    host_descriptor_path: Path | None = None
    runtime_image: dict[str, str] | None = None
    container_temp_root = (validated_container_temp_root(args.container_temp_root)
                           if args.execution_mode == "pinned-container" else None)
    execution_environment = "host_product_build"
    if args.execute:
        if args.execution_mode == "pinned-container":
            assert container_temp_root is not None
            run_work_parent = Path(tempfile.mkdtemp(
                prefix="p5.", dir=container_temp_root))
            run_work_parent.chmod(0o711)
            run_workdir = run_work_parent / "p50compilee2e.run"
            command_workdir = DEFAULT_REPORTED_WORKDIR
            runtime_image = container_image_identity(args.container_image,
                                                     args.container_image_id)
            execution_environment = "pinned_container_product_build"
            host_descriptor_path = run_work_parent / "host-descriptor.json"
        else:
            run_workdir = Path(tempfile.mkdtemp(
                prefix="p50compilee2e.", dir=tempfile.gettempdir()))
            run_workdir.rmdir()
            command_workdir = run_workdir
            host_descriptor_path = run_workdir / "host-descriptor.json"
        assert host_descriptor_path is not None
        host_descriptor = capture_host_descriptor(host_descriptor_path)
    launch_identity = None
    if args.execute:
        launch_commit, launch_tree, launch_binaries, launch_runner_sha = product_identity(args.product_root.absolute())
        launch_identity = {"source_commit": launch_commit, "source_tree": launch_tree,
                           "binary_sha256": launch_binaries, "runner_sha256": launch_runner_sha}
        if runtime_image is not None:
            launch_identity["runtime_image"] = runtime_image
        if host_descriptor_path is None:
            _fail("calibration_metadata:host_descriptor_missing")
        host_digest = host_descriptor.get("sha256")
        host_bytes = host_descriptor.get("bytes")
        if (not isinstance(host_digest, str) or type(host_bytes) is not int):
            _fail("calibration_metadata:host_descriptor_invalid")
        launch_identity["host_descriptor"] = {"sha256": host_digest,
                                               "bytes": host_bytes}
    command = build_command(batch_manifest, args.profile,
                            product_root=args.product_root.absolute(), corpus=args.corpus,
                            regime=args.regime, depth=args.depth, full_count=args.full_count,
                            passes=args.passes, workdir=command_workdir,
                            predictive_plan=predictive_plan, suite=args.suite,
                            topology=topology, timeout_seconds=effective_timeout,
                            product_profile=args.product_profile)
    if not args.execute:
        print(json.dumps({"schema": SCHEMA, "status": "DRY_RUN", "command": command,
                          "repeat_predictive_plan": str(repeat_predictive_plan)
                          if repeat_predictive_plan is not None else None,
                          "execution_mode": args.execution_mode,
                          "container_image": args.container_image
                          if args.execution_mode == "pinned-container" else None,
                          "container_temp_root": str(args.container_temp_root.absolute())
                          if args.execution_mode == "pinned-container" else None,
                          "container_work_root": str(DEFAULT_CONTAINER_WORK_ROOT)
                          if args.execution_mode == "pinned-container" else None,
                          "execution_limits": {"timeout_seconds": effective_timeout}},
                         sort_keys=True))
        return 0
    if args.execution_mode == "pinned-container":
        assert run_work_parent is not None and runtime_image is not None
        required_paths = [batch_manifest, predictive_plan, topology,
                          args.product_root.absolute(), SCRIPT]
        if repeat_predictive_plan is not None:
            required_paths.append(repeat_predictive_plan)
        for row in rows:
            required_paths.extend((Path(row["source"]),
                                   Path(row["predictive_input"]["path"])))
            for field in ("compile_db", "compile_source", "compile_output"):
                if field in row:
                    required_paths.append(Path(row[field]))
        command = build_container_command(
            command, image_identity=runtime_image,
            bind_root=args.container_bind_root.absolute(),
            work_parent=run_work_parent, required_paths=required_paths,
            temp_root=container_temp_root,
            container_work_root=DEFAULT_CONTAINER_WORK_ROOT)
    try:
        timeout = effective_timeout
        try:
            stdout, returncode = _run_product(command, timeout)
        except LiveRunnerError as exc:
            cleanup_error: LiveRunnerError | None = None
            if run_work_parent is not None:
                try:
                    stop_container(container_name(run_work_parent))
                except LiveRunnerError as stop_exc:
                    cleanup_error = stop_exc
            if host_descriptor_path is not None and host_descriptor_path.is_file():
                host_descriptor_path.unlink()
            print(f"timeout_seconds={timeout}")
            print(f"workdir={run_workdir}" if run_workdir is not None else "workdir=unknown")
            print(str(exc))
            if cleanup_error is not None:
                print(str(cleanup_error))
            return 77
        if run_workdir is not None:
            _write_new(run_workdir / "product-output.log", stdout.encode("utf-8"))
        path = finalize(stdout, returncode, batch_manifest=batch_manifest,
                        topology=topology, output=args.output.absolute(), profile=args.profile,
                        predictive_plan=predictive_plan, product_root=args.product_root.absolute(),
                        product_profile=args.product_profile,
                        repeat_predictive_plan=repeat_predictive_plan,
                        corpus=args.corpus, regime=args.regime, depth=args.depth,
                        full_count=args.full_count, passes=args.passes,
                        artifact_sample=args.artifact_sample,
                        retain_all_artifacts=args.retain_all_artifacts,
                        timeout_seconds=effective_timeout,
                        launch_identity=launch_identity,
                        execution_environment=execution_environment,
                        runtime_image=runtime_image,
                        host_descriptor_path=host_descriptor_path,
                        host_workdir=run_workdir
                        if args.execution_mode == "pinned-container" else None,
                        reported_workdir=command_workdir
                        if args.execution_mode == "pinned-container" else None,
                        timestamp=args.timestamp, suite=args.suite)
    except LiveRunnerError as exc:
        print(str(exc))
        if run_workdir is not None:
            print(f"workdir={run_workdir}")
        return 77
    if run_work_parent is not None:
        if host_descriptor_path is not None and host_descriptor_path.is_file():
            host_descriptor_path.unlink()
        run_work_parent.rmdir()
    print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
