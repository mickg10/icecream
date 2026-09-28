"""Load and semantically validate ``icefarm-scenario-v1`` documents."""

from __future__ import annotations

import copy
import hashlib
import ipaddress
import re
import stat
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any

try:
    from .farm_spec import FarmSpec
    from .schema_validation import ValidationError, canonical_bytes, load_json, validate
except ImportError:  # Direct execution from this directory.
    from farm_spec import FarmSpec
    from schema_validation import ValidationError, canonical_bytes, load_json, validate


SCHEMA_PATH = Path(__file__).with_name("schemas") / "scenario-v1.json"
PROFILES = frozenset(("P29V1", "ZSTD_TU", "ZSTD_ROUTE"))
SCHEDULER_PROFILES = PROFILES | {"OFF"}
P29_FAULT_ENV = "ICECC_P50_FAULT_INJECTION"
P29_FAULT_VALUE = "P29_INTERNER_FAIL_ONCE"
RUNNER_ENV = frozenset(
    (
        "ICECC_NETNAME",
        "ICECC_PREFERRED_HOST",
        "ICECC_REMOTE_REQUIRED",
        "ICECC_P50_COMPILE_IDENTITY_TRACE",
        "ICECC_P50_C_ACTION_TRACE",
        "ICECC_P50_C_LEGACY_WIRE_TRACE",
        "ICECC_P50_F_ACTION_TRACE",
        "ICECC_P50_F_LEGACY_WIRE_TRACE",
        "ICECC_P50_H3_MUTANT",
        "ICECC_P50_H3_ARM_FILE",
        "ICECC_P50_H3_SCHEDULER_INSTANCE",
        "ICECC_P50_H3_TRACE",
        "ICECC_P50_S30_MUTANT_TRACE",
        "ICECC_P50_SOURCE_RESULT_TRACE",
        "ICECC_P50_TEST_LIFECYCLE_TRACE",
        "ICECC_P50_TEST_READY_TRACE",
        "ICECC_P50_PIPELINE_WINDOW",
        "ICECC_SCHEDULER",
        "ICECC_TEST_SOCKET",
        "ICECC_VERSION",
        "ICEFARM_D18_BARRIER",
        "TEMP",
        "TEMPDIR",
        "TMP",
        "TMPDIR",
    )
)
IMAGE_GENERATION_RE = re.compile(r"^p(43|44|50)(?:s[0-9]+)?(?:-|$)", re.IGNORECASE)
ENVIRONMENT_KEY_RE = re.compile(r"[A-Z_][A-Z0-9_]*")
SAFE_NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
RATE_RE = re.compile(r"[1-9][0-9]*(?:kbit|mbit|gbit)")


class ScenarioSpecError(ValidationError):
    """Scenario data is invalid or unsafe to execute."""


def _validate_environment(environment: dict[str, str], field: str) -> None:
    for key, value in environment.items():
        if ENVIRONMENT_KEY_RE.fullmatch(key) is None:
            raise ScenarioSpecError(f"{field}.<key>: unsafe environment name {key!r}")
        if key in RUNNER_ENV:
            raise ScenarioSpecError(
                f"{field}.{key}: runner-owned environment is forbidden"
            )
        if "\0" in value:
            raise ScenarioSpecError(f"{field}.{key}: NUL is forbidden")


def _validate_safe_name(value: str, field: str) -> None:
    if SAFE_NAME_RE.fullmatch(value) is None or value in (".", ".."):
        raise ScenarioSpecError(f"{field}: must be a safe non-dot name")


def _validate_container_path(value: str, field: str) -> None:
    path = PurePosixPath(value)
    if (
        not path.is_absolute()
        or value in ("/", "//", "/.")
        or ".." in path.parts
        or any(ord(character) < 32 or ord(character) == 127 for character in value)
    ):
        raise ScenarioSpecError(
            f"{field}: must be a control-free, non-root absolute path without '..'"
        )


def _image_generation(reference: str, field: str) -> int:
    label = reference.rsplit(":", 1)[-1]
    match = IMAGE_GENERATION_RE.match(label)
    if match is None:
        raise ScenarioSpecError(f"{field}: image label must encode p43, p44, or p50")
    return int(match.group(1))


def _validate_shape(
    value: dict[str, Any],
    generations: dict[str, int],
    wire_revisions: dict[str, int | None],
) -> None:
    shape = value["shape"]
    if shape in ("-", "all", "mixed"):
        return

    by_role = {
        role: [item for item in value["instances"] if item["role"] == role]
        for role in ("S", "C", "F")
    }

    def is_new(instance: dict[str, Any]) -> bool:
        return generations[instance["image"]] == 50

    def all_new(role: str) -> bool:
        return all(is_new(item) for item in by_role[role])

    def all_old(role: str) -> bool:
        return all(not is_new(item) for item in by_role[role])

    matches = False
    if shape == "SCF":
        matches = all_old("S") and all_old("C") and all_old("F")
    elif shape == "S'CF":
        matches = all_new("S") and all_old("C") and all_old("F")
    elif shape == "S'FC'":
        matches = all_new("S") and all_new("C") and all_old("F")
    elif shape == "S'C'F'":
        matches = all_new("S") and all_new("C") and all_new("F")
    elif shape == "S'[FF'][CC']":
        f_states = {is_new(item) for item in by_role["F"]}
        c_states = {is_new(item) for item in by_role["C"]}
        matches = (
            all_new("S") and f_states == {False, True} and c_states == {False, True}
        )
    elif shape == "S'[F'F''][C']":
        worker_revisions = {wire_revisions[item["image"]] for item in by_role["F"]}
        client_revisions = {wire_revisions[item["image"]] for item in by_role["C"]}
        matches = (
            all_new("S")
            and all_new("C")
            and all_new("F")
            and None not in worker_revisions
            and None not in client_revisions
            and len(worker_revisions) >= 2
            and len(client_revisions) == 1
            and bool(worker_revisions & client_revisions)
            and bool(worker_revisions - client_revisions)
        )
    elif shape == "S'[F~][C']":
        matches = (
            len(by_role["S"]) == 1
            and len(by_role["C"]) == 1
            and len(by_role["F"]) == 1
            and all_new("S")
            and all_new("C")
            and all_new("F")
        )

    if not matches:
        raise ScenarioSpecError(
            f"$.shape: {shape!r} does not match the declared instance image generations"
        )


@dataclass(frozen=True)
class ScenarioSpec:
    path: Path
    data: dict[str, Any]

    @property
    def instances(self) -> dict[str, dict[str, Any]]:
        return {instance["name"]: instance for instance in self.data["instances"]}

    @property
    def digest(self) -> str:
        return hashlib.sha256(canonical_bytes(self.data)).hexdigest()


def _validate_d18_role_mix(
    value: dict[str, Any],
    workload: dict[str, Any],
    role_instances: dict[str, list[dict[str, Any]]],
    generations: dict[str, int],
    wire_revisions: dict[str, int | None],
) -> None:
    """Fail closed on the specific three-client P43/R1/R2 external gate."""
    if (
        workload["jobs"] != 1
        or workload["repeat"] != 1
        or workload["turns"] != ["A"]
        or value["timeline"]
        or value["controls"]
    ):
        raise ScenarioSpecError(
            "$.workload: D18 uses one bounded, single-turn client workload without transitions"
        )
    mapping = workload["d18_roles"]
    clients = mapping["clients"]
    workers = mapping["workers"]
    if len(set(clients.values())) != 3 or set(clients.values()) != set(workload["clients"]):
        raise ScenarioSpecError(
            "$.workload.d18_roles.clients: must name exactly the three selected clients"
        )
    if set(workers) != {"R1", "R2"} or workers["R1"] == workers["R2"]:
        raise ScenarioSpecError(
            "$.workload.d18_roles.workers: R1 and R2 require distinct F instances"
        )
    by_name = {item["name"]: item for role in ("S", "F", "C") for item in role_instances[role]}
    if any(name not in by_name or by_name[name]["role"] != "C" for name in clients.values()):
        raise ScenarioSpecError("$.workload.d18_roles.clients: every target must be a C instance")
    if any(name not in by_name or by_name[name]["role"] != "F" for name in workers.values()):
        raise ScenarioSpecError("$.workload.d18_roles.workers: every target must be an F instance")
    scheduler = role_instances["S"][0]
    if (
        generations[scheduler["image"]] != 50
        or scheduler.get("env", {}).get("ICECC_P50_PROFILE") not in PROFILES
        or scheduler.get("env", {}).get("ICECC_P51_MODE") != "on"
    ):
        raise ScenarioSpecError(
            "$.workload: D18 requires a P50/R2 scheduler with a selected profile"
        )

    p43 = by_name[clients["P43"]]
    r1 = by_name[clients["R1"]]
    r2 = by_name[clients["R2"]]
    f1 = by_name[workers["R1"]]
    f2 = by_name[workers["R2"]]
    if generations[p43["image"]] != 43:
        raise ScenarioSpecError("$.workload.d18_roles.clients.P43: requires a pinned P43 image")
    # This authority field describes the image's CacheWire envelope schema
    # (the current P50 codec is revision 1). R2 selection is a separate
    # runtime capability: ICECC_P51_MODE=on selects CACHE_WIRE_REVISION_R2.
    # Do not confuse the two revisions when validating the image metadata.
    for label, instance in (
        ("R1 client", r1), ("R1 worker", f1),
        ("R2 client", r2), ("R2 worker", f2),
    ):
        if generations[instance["image"]] != 50 or wire_revisions[instance["image"]] != 1:
            raise ScenarioSpecError(
                f"$.workload.d18_roles: {label} must use a pinned P50 image "
                "with CacheWire revision 1"
            )
    if r1.get("env", {}).get("ICECC_P50_MODE") != "on" or r1.get("env", {}).get("ICECC_P51_MODE") != "off":
        raise ScenarioSpecError("$.workload.d18_roles.clients.R1: requires P50 on and R2 off")
    if r2.get("env", {}).get("ICECC_P50_MODE") != "on" or r2.get("env", {}).get("ICECC_P51_MODE") != "on":
        raise ScenarioSpecError("$.workload.d18_roles.clients.R2: requires P50 and persistent R2 on")
    if f1.get("env", {}).get("ICECC_P51_MODE") != "off" or f2.get("env", {}).get("ICECC_P51_MODE") != "on":
        raise ScenarioSpecError("$.workload.d18_roles.workers: worker protocol modes do not match R1/R2")


def _validate_p51_receipt_window(
    value: dict[str, Any], workload: dict[str, Any],
    role_instances: dict[str, list[dict[str, Any]]],
    *, manifest_jobs: int,
) -> None:
    links = workload["receipt_gate"].get("links")
    multilink = links is not None
    restart_extension = workload["receipt_gate"].get("restart_extension")
    if restart_extension is not None and not multilink:
        raise ScenarioSpecError(
            "$.workload.receipt_gate.restart_extension: requires multi-link receipt mode"
        )
    if restart_extension is not None and workload["receipt_gate"].get("negotiated_window") != 30:
        raise ScenarioSpecError(
            "$.workload.receipt_gate.restart_extension: held worker restart requires W30"
        )
    selected_clients = {item["name"] for item in role_instances["C"]}
    selected_workers = {item["name"] for item in role_instances["F"]}
    if (
        len(role_instances["S"]) != 1
        or (not multilink and (len(role_instances["C"]) != 1 or len(role_instances["F"]) != 1))
        or set(workload["clients"]) != selected_clients
        or workload["turns"] != ["A"]
        or (not multilink and workload["repeat"] != 1)
        or value["timeline"]
        or value["controls"]
        or "fault" in value
    ):
        raise ScenarioSpecError(
            "$.workload: p51-receipt-window requires one S, selected C clients, "
            "one A turn, and no controls"
        )
    scheduler = role_instances["S"][0]
    if not multilink and int(role_instances["F"][0].get("slots", 0)) < 31:
        raise ScenarioSpecError(
            "$.instances: single-link receipt-window requires at least 31 F slots"
        )
    if (
        scheduler.get("env", {}).get("ICECC_P51_MODE") != "on"
        or scheduler.get("env", {}).get("ICECC_P50_PROFILE") not in PROFILES
        or any(item.get("env", {}).get("ICECC_P50_MODE") != "on" for item in role_instances["C"])
        or any(item.get("env", {}).get("ICECC_P51_MODE") != "on" for item in role_instances["C"])
        or any(item.get("env", {}).get("ICECC_P51_MODE") != "on" for item in role_instances["F"])
    ):
        raise ScenarioSpecError(
            "$.instances: receipt-window requires selected-profile R2 scheduler and P50/R2 C+F"
        )
    gate = workload["receipt_gate"]
    if "command_timeout_s" in gate:
        command_timeout_s = gate["command_timeout_s"]
        if command_timeout_s <= 30:
            raise ScenarioSpecError(
                "$.workload.receipt_gate.command_timeout_s: must leave a 30s cleanup margin"
            )
        if command_timeout_s > value["timeouts"]["turn_s"]:
            raise ScenarioSpecError(
                "$.workload.receipt_gate.command_timeout_s: may not exceed the turn budget"
            )
    binary = Path(gate["binary"])
    if not binary.is_absolute() or ".." in binary.parts:
        raise ScenarioSpecError("$.workload.receipt_gate.binary: must be an absolute safe path")
    try:
        metadata = binary.lstat()
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    except OSError as exc:
        raise ScenarioSpecError(f"$.workload.receipt_gate.binary: unavailable: {exc}") from exc
    if (
        not stat.S_ISREG(metadata.st_mode)
        or not metadata.st_mode & 0o111
        or digest != gate["binary_sha256"]
    ):
        raise ScenarioSpecError(
            "$.workload.receipt_gate: helper must be an executable regular file matching binary_sha256"
        )
    expected = gate["expected_commits"]
    negotiated = gate["negotiated_window"]
    if workload["jobs"] < expected:
        raise ScenarioSpecError(
            "$.workload.jobs: must offer enough jobs to test the configured receipt window"
        )
    if gate["expect_observed"]:
        if expected != negotiated:
            raise ScenarioSpecError(
                "$.workload.receipt_gate: positive gate count must equal negotiated window"
            )
    elif expected != 30 or negotiated != 1:
        raise ScenarioSpecError(
            "$.workload.receipt_gate: W1 negative control must offer and assert 30"
        )

    if multilink:
        restart = gate.get("restart_extension")
        if not gate["expect_observed"]:
            raise ScenarioSpecError("$.workload.receipt_gate.links: multi-link gates are positive only")
        if (len(selected_clients) != 1 and len(selected_workers) != 1) or (
            restart is not None and (len(selected_clients) != 1 or len(selected_workers) != 2)
        ):
            raise ScenarioSpecError(
                "$.workload.receipt_gate.links: only one-to-many or many-to-one topologies are supported"
            )
        if len(links) < 2 or (restart is not None and len(links) != 2):
            raise ScenarioSpecError("$.workload.receipt_gate.links: requires multiple links")
        by_name = {item["name"]: item for item in role_instances["C"] + role_instances["F"]}
        seen_pairs: set[tuple[str, str]] = set()
        ranges_by_client: dict[str, list[tuple[int, int]]] = {name: [] for name in selected_clients}
        windows_by_worker: dict[str, int] = {name: 0 for name in selected_workers}
        windows_by_client: dict[str, int] = {name: 0 for name in selected_clients}
        for index, link in enumerate(links):
            client_name, worker_name = link["client"], link["worker"]
            pair = (client_name, worker_name)
            if client_name not in selected_clients or worker_name not in selected_workers:
                raise ScenarioSpecError(
                    f"$.workload.receipt_gate.links[{index}]: names must resolve to declared C/F instances"
                )
            if pair in seen_pairs:
                raise ScenarioSpecError(
                    f"$.workload.receipt_gate.links[{index}]: duplicate C/F relationship"
                )
            seen_pairs.add(pair)
            first, last = link["first_job"], link["last_job"]
            if first > last or last > manifest_jobs or last - first + 1 < negotiated:
                raise ScenarioSpecError(
                    f"$.workload.receipt_gate.links[{index}]: range must contain at least "
                    "one negotiated window within the manifest"
                )
            ranges_by_client[client_name].append((first, last))
            windows_by_client[client_name] += negotiated
            windows_by_worker[worker_name] += negotiated

        expected_pairs = {
            (client_name, worker_name)
            for client_name in selected_clients
            for worker_name in selected_workers
        }
        if seen_pairs != expected_pairs:
            raise ScenarioSpecError(
                "$.workload.receipt_gate.links: must cover every selected C/F relationship exactly once"
            )

        initial_end_by_client: dict[str, int] = {}
        for client_name, ranges in ranges_by_client.items():
            ordered = sorted(ranges)
            cursor = 1
            for first, last in ordered:
                if first != cursor:
                    raise ScenarioSpecError(
                        f"$.workload.receipt_gate.links: {client_name} job ranges must be "
                        "contiguous, disjoint, and cover the full manifest"
                    )
                cursor = last + 1
            initial_end_by_client[client_name] = cursor - 1
            if restart is None and cursor - 1 != manifest_jobs:
                raise ScenarioSpecError(
                    f"$.workload.receipt_gate.links: {client_name} ranges do not cover "
                    "the full manifest"
                )
        if restart is not None:
            client_name = next(iter(selected_clients))
            initial_end = initial_end_by_client[client_name]
            affected = restart["affected_link"]
            healthy = restart["healthy_link"]
            affected_pair = (affected["client"], affected["worker"])
            healthy_pair = (healthy["client"], healthy["worker"])
            affected_range = next(
                (
                    (item["first_job"], item["last_job"])
                    for item in links
                    if (item["client"], item["worker"]) == affected_pair
                ),
                None,
            )
            if (
                restart.get("kind") not in {
                    "held-f-restart-v1", "held-f-cache-store-restart-v1"
                }
                or affected_pair not in seen_pairs
                or affected_range is None
                or healthy_pair not in seen_pairs
                or affected_pair == healthy_pair
                or affected["client"] != client_name
                or healthy["client"] != client_name
                or selected_workers != {affected["worker"], healthy["worker"]}
                or manifest_jobs != initial_end + negotiated + 1
                or affected_range[1] != initial_end
                or affected_range[1] - affected_range[0] + 1 != negotiated
            ):
                raise ScenarioSpecError(
                    "$.workload.receipt_gate.restart_extension: requires C1F2, two initial links, "
                    "an exact affected W30 cohort, and one post-restart window plus trailing transfer"
                )
        for client_name, total in windows_by_client.items():
            if workload["jobs"] < total:
                raise ScenarioSpecError(
                    f"$.workload.jobs: concurrency must cover all receipt windows for {client_name}"
                )
        for worker_name, total in windows_by_worker.items():
            if int(by_name[worker_name].get("slots", 0)) < total:
                raise ScenarioSpecError(
                    f"$.instances.{worker_name}.slots: must cover the aggregate receipt windows "
                    "assigned to this F worker"
                )
        max_client_window = max(windows_by_client.values(), default=0)
        aggregate_worker_slots = sum(
            int(by_name[worker_name].get("slots", 0))
            for worker_name in selected_workers
        )
        if aggregate_worker_slots <= max_client_window:
            raise ScenarioSpecError(
                "$.instances: aggregate F slots must exceed the largest per-C "
                "receipt window so scheduler dispatch credit is not clamped below it"
            )


def load_scenario_spec(path: str | Path, farm: FarmSpec) -> ScenarioSpec:
    resolved = Path(path).resolve()
    try:
        value = load_json(resolved)
        validate(value, SCHEMA_PATH)
    except ValidationError as exc:
        raise ScenarioSpecError(str(exc)) from exc
    if not isinstance(value, dict):
        raise ScenarioSpecError("$: scenario root must be an object")

    _validate_safe_name(value["id"], "$.id")
    farm_hosts = farm.hosts
    generations: dict[str, int] = {}
    wire_revisions: dict[str, int | None] = {}
    for alias, reference in value["images"].items():
        _validate_safe_name(alias, f"$.images.<key:{alias!r}>")
        if any(ord(character) < 32 or ord(character) == 127 for character in reference):
            raise ScenarioSpecError(
                f"$.images.{alias}: control characters are forbidden"
            )
        generations[alias] = _image_generation(reference, f"$.images.{alias}")
        if reference not in farm.data["authority"]["images"]:
            raise ScenarioSpecError(
                f"$.images.{alias}: image label is absent from the immutable authority map"
            )
        wire_revisions[alias] = farm.data["authority"]["images"][reference].get(
            "cache_wire_revision",
            1 if generations[alias] == 50 else None,
        )

    names: set[str] = set()
    role_instances: dict[str, list[dict[str, Any]]] = {"S": [], "F": [], "C": []}
    for index, instance in enumerate(value["instances"]):
        name = instance["name"]
        _validate_safe_name(name, f"$.instances[{index}].name")
        if name in names:
            raise ScenarioSpecError(
                f"$.instances[{index}].name: duplicate instance {name!r}"
            )
        names.add(name)
        role_instances[instance["role"]].append(instance)
        if instance["host"] not in farm_hosts:
            raise ScenarioSpecError(
                f"$.instances[{index}].host: undeclared host {instance['host']!r}"
            )
        if farm_hosts[instance["host"]].get("execution", "ssh") == "local":
            if "cpus" not in instance or "memory_mb" not in instance:
                raise ScenarioSpecError(
                    f"$.instances[{index}]: local execution requires explicit cpus and memory_mb limits"
                )
        if instance["image"] not in value["images"]:
            raise ScenarioSpecError(
                f"$.instances[{index}].image: undeclared alias {instance['image']!r}"
            )
        allowed = farm_hosts[instance["host"]]["roles_allowed"]
        if instance["role"] not in allowed:
            detail = (
                "lacks SYS_CHROOT/F authority"
                if instance["role"] == "F"
                else "role is not allowed"
            )
            raise ScenarioSpecError(
                f"$.instances[{index}]: host {instance['host']!r} {detail}"
            )
        if instance["role"] != "F" and "slots" in instance:
            raise ScenarioSpecError(
                f"$.instances[{index}].slots: only F instances have slots"
            )
        client_environment = instance.get("client_environment")
        if instance["role"] == "C":
            if client_environment not in farm.data["client_environments"]:
                raise ScenarioSpecError(
                    f"$.instances[{index}].client_environment: absent from the "
                    "farm client environments"
                )
        elif client_environment is not None:
            raise ScenarioSpecError(
                f"$.instances[{index}].client_environment: valid only for C instances"
            )
        snapshot_name = instance.get("system_source_snapshot")
        if snapshot_name is not None:
            if instance["role"] not in ("C", "F"):
                raise ScenarioSpecError(
                    f"$.instances[{index}].system_source_snapshot: valid only for C or F instances"
                )
            snapshots = farm.data.get("system_source_snapshots", {})
            snapshot = snapshots.get(snapshot_name)
            if not isinstance(snapshot, dict):
                raise ScenarioSpecError(
                    f"$.instances[{index}].system_source_snapshot: absent from farm authority"
                )
            if instance["role"] == "C" and client_environment != "fedora-clang-libcxx":
                raise ScenarioSpecError(
                    f"$.instances[{index}].system_source_snapshot: requires fedora-clang-libcxx C foundation"
                )
        environment = instance.get("env", {})
        profile = environment.get("ICECC_P50_PROFILE")
        if profile is not None and profile not in SCHEDULER_PROFILES:
            raise ScenarioSpecError(
                f"$.instances[{index}].env.ICECC_P50_PROFILE: must be one of "
                f"{sorted(SCHEDULER_PROFILES)!r}"
            )
        if profile is not None and instance["role"] != "S":
            raise ScenarioSpecError(
                f"$.instances[{index}].env.ICECC_P50_PROFILE: only S owns profile selection"
            )
        mode = environment.get("ICECC_P50_MODE")
        if mode is not None and instance["role"] != "C":
            raise ScenarioSpecError(
                f"$.instances[{index}].env.ICECC_P50_MODE: only C owns the client mode"
            )
        if mode is not None and mode not in ("on", "off"):
            raise ScenarioSpecError(
                f"$.instances[{index}].env.ICECC_P50_MODE: must be 'on' or 'off'"
            )
        p29_fault = environment.get(P29_FAULT_ENV)
        if p29_fault is not None and (
            instance["role"] != "C" or generations[instance["image"]] != 50
        ):
            raise ScenarioSpecError(
                f"$.instances[{index}].env.{P29_FAULT_ENV}: only a p50 C owns fault injection"
            )
        if p29_fault is not None and p29_fault != P29_FAULT_VALUE:
            raise ScenarioSpecError(
                f"$.instances[{index}].env.{P29_FAULT_ENV}: must be {P29_FAULT_VALUE!r}"
            )
        _validate_environment(environment, f"$.instances[{index}].env")

    local_usage: dict[str, tuple[float, int]] = {}
    for instance in value["instances"]:
        host_name = instance["host"]
        if farm_hosts[host_name].get("execution", "ssh") != "local":
            continue
        cpus, memory_mb = local_usage.get(host_name, (0.0, 0))
        local_usage[host_name] = (
            cpus + float(instance["cpus"]), memory_mb + int(instance["memory_mb"])
        )
    for host_name, (cpus, memory_mb) in local_usage.items():
        host = farm_hosts[host_name]
        if cpus > host["cores"] or memory_mb > host["mem_gb"] * 1024:
            raise ScenarioSpecError(
                f"local instance limits exceed host {host_name!r} capacity"
            )

    if len(role_instances["S"]) != 1:
        raise ScenarioSpecError(
            "$.instances: scenario must declare exactly one S instance"
        )
    if not role_instances["F"] or not role_instances["C"]:
        raise ScenarioSpecError(
            "$.instances: scenario must declare at least one F and one C instance"
        )
    _validate_shape(value, generations, wire_revisions)

    if "H3" in value["controls"]:
        scheduler = role_instances["S"][0]
        scheduler_label = value["images"][scheduler["image"]]
        scheduler_authority = farm.data["authority"]["images"].get(scheduler_label)
        if not isinstance(scheduler_authority, dict) or scheduler_authority.get(
            "kind"
        ) != "scheduler-mutant":
            raise ScenarioSpecError(
                "$.controls: H3 requires an authority-bound scheduler-mutant image"
            )
        selected_clients = [
            item for item in role_instances["C"] if item["name"] in value["workload"]["clients"]
        ]
        if any(generations[item["image"]] == 50 for item in selected_clients):
            raise ScenarioSpecError("$.controls: H3 workload clients must be old")

    scheduler_host = farm_hosts[role_instances["S"][0]["host"]]
    scheduler_lan = ipaddress.ip_network(scheduler_host["shared_lan"], strict=False)
    for instance in role_instances["F"] + role_instances["C"]:
        peer_ip = ipaddress.ip_address(farm_hosts[instance["host"]]["lan_ip"])
        if peer_ip not in scheduler_lan:
            raise ScenarioSpecError(
                f"$.instances: S host shared_lan does not contain {instance['name']} at {peer_ip}"
            )

    workload = value["workload"]
    corpus = farm.data["corpora"].get(workload["corpus"])
    if corpus is None:
        raise ScenarioSpecError(
            f"$.workload.corpus: undeclared corpus {workload['corpus']!r}"
        )
    d18_roles = workload.get("d18_roles")
    is_d18 = workload["driver"] == "d18-role-mix"
    is_receipt_window = workload["driver"] == "p51-receipt-window"
    if is_receipt_window:
        scheduler_host_name = role_instances["S"][0]["host"]
        scheduler_host = farm_hosts[scheduler_host_name]
        scheduler_lan_ip = ipaddress.ip_address(scheduler_host["lan_ip"])
        scheduler_authority = farm.data["authority"]["hosts"].get(
            scheduler_host_name
        )
        authority_ip = (
            ipaddress.ip_address(scheduler_authority["address"])
            if isinstance(scheduler_authority, dict)
            else None
        )
        loopback_addresses = sorted(
            {
                str(address)
                for address in (scheduler_lan_ip, authority_ip)
                if address is not None and address.is_loopback
            }
        )
        if loopback_addresses:
            raise ScenarioSpecError(
                "$.instances: P51 receipt-window isolated clients require the "
                f"scheduler host {scheduler_host_name!r} to advertise a "
                f"non-loopback LAN address (got loopback address(es) "
                f"{', '.join(loopback_addresses)})"
            )
    if is_d18 != (d18_roles is not None):
        raise ScenarioSpecError(
            "$.workload.d18_roles: required only for d18-role-mix workloads"
        )
    expected_driver_corpus = (
        "tu-manifest" if is_d18 or is_receipt_window else workload["driver"]
    )
    if expected_driver_corpus != corpus["kind"]:
        raise ScenarioSpecError(
            "$.workload.driver: does not match the declared corpus kind"
        )
    if is_d18:
        _validate_d18_role_mix(
            value, workload, role_instances, generations, wire_revisions
        )
    if is_receipt_window:
        if "receipt_gate" not in workload:
            raise ScenarioSpecError("$.workload.receipt_gate: required for receipt-window driver")
        manifest_jobs = (
            int(corpus["tus"])
            * int(corpus.get("repeat", 1))
            * int(workload["repeat"])
        )
        _validate_p51_receipt_window(
            value, workload, role_instances, manifest_jobs=manifest_jobs
        )
    elif "receipt_gate" in workload:
        raise ScenarioSpecError("$.workload.receipt_gate: valid only for receipt-window driver")
    allowed_turns = {"A"} if "manifest" in corpus else {"A", "B"}
    unknown_turns = sorted(set(workload["turns"]) - allowed_turns)
    if unknown_turns:
        raise ScenarioSpecError(
            f"$.workload.turns: corpus does not define turns {unknown_turns!r}"
        )
    for client in workload["clients"]:
        instance = next(
            (item for item in role_instances["C"] if item["name"] == client), None
        )
        if instance is None:
            raise ScenarioSpecError(
                f"$.workload.clients: {client!r} is not a C instance"
            )
        selected_environment = instance["client_environment"]
        if selected_environment not in corpus.get("compiler_recipes", {}):
            raise ScenarioSpecError(
                f"$.workload.clients: corpus {workload['corpus']!r} has no compiler "
                f"recipe for C environment {selected_environment!r}"
            )
    if value["shape"] == "S'[FF'][CC']":
        selected_clients = [
            item for item in role_instances["C"] if item["name"] in workload["clients"]
        ]
        selected_generations = {
            generations[item["image"]] == 50 for item in selected_clients
        }
        if selected_generations != {False, True}:
            raise ScenarioSpecError(
                "$.workload.clients: mixed-pool shape must run both old and new clients"
            )

    fault = value.get("fault")
    if fault is not None:
        if value["controls"] != ["H4"]:
            raise ScenarioSpecError("$.fault: corrupt-object is restricted to exact H4")
        if fault["client"] not in workload["clients"]:
            raise ScenarioSpecError(
                "$.fault.client: must name a selected workload client"
            )
        if len(workload["turns"]) != 1:
            raise ScenarioSpecError("$.fault: H4 requires exactly one workload turn")
        total_jobs = corpus["tus"] * corpus.get("repeat", 1) * workload["repeat"]
        if fault["job"] > total_jobs:
            raise ScenarioSpecError(
                f"$.fault.job: {fault['job']} exceeds client job count {total_jobs}"
            )
    elif "H4" in value["controls"]:
        raise ScenarioSpecError("$.fault: H4 requires one corrupt-object descriptor")

    shaped: set[str] = set()
    for index, shaping in enumerate(value["network"]["shaping"]):
        name = shaping["instance"]
        if RATE_RE.fullmatch(shaping["rate"]) is None:
            raise ScenarioSpecError(f"$.network.shaping[{index}].rate: invalid rate")
        if name in shaped:
            raise ScenarioSpecError(
                f"$.network.shaping[{index}]: duplicate instance {name!r}"
            )
        shaped.add(name)
        instance = next(
            (item for item in value["instances"] if item["name"] == name), None
        )
        if instance is None:
            raise ScenarioSpecError(
                f"$.network.shaping[{index}]: unknown instance {name!r}"
            )
        if not farm_hosts[instance["host"]]["netem"]:
            raise ScenarioSpecError(
                f"$.network.shaping[{index}]: host {instance['host']!r} has no netem authority"
            )

    disk_fill_count = sum(
        event.get("action") == "disk_fill" for event in value["timeline"]
    )
    if disk_fill_count > 1:
        raise ScenarioSpecError(
            "$.timeline: at most one bounded disk_fill event is permitted"
        )

    for index, event in enumerate(value["timeline"]):
        field = f"$.timeline[{index}]"
        action = event["action"]
        instance_name = event.get("instance")
        if instance_name is None:
            raise ScenarioSpecError(f"{field}.instance: required for action {action!r}")
        if instance_name not in names:
            raise ScenarioSpecError(
                f"{field}.instance: unknown instance {instance_name!r}"
            )
        instance = next(
            item for item in value["instances"] if item["name"] == instance_name
        )

        action_fields = {
            "upgrade": frozenset(("image",)),
            "downgrade": frozenset(("image",)),
            "restart": frozenset(),
            "scheduler-loss-active": frozenset(),
            "kill -9": frozenset(),
            "env_set": frozenset(("env",)),
            "netem_set": frozenset(("rate", "delay_ms")),
            "disk_fill": frozenset(),
            "header_edit": frozenset(("path",)),
        }
        required = action_fields[action]
        missing = sorted(required - event.keys())
        if missing:
            raise ScenarioSpecError(
                f"{field}: action {action!r} requires fields {missing!r}"
            )
        allowed = {"trigger", "action", "instance", *required}
        unused = sorted(event.keys() - allowed)
        if unused:
            raise ScenarioSpecError(
                f"{field}: action {action!r} does not use fields {unused!r}"
            )

        image = event.get("image")
        if image is not None and image not in value["images"]:
            raise ScenarioSpecError(f"{field}.image: unknown image alias {image!r}")
        if action == "env_set":
            if instance["role"] not in ("S", "C"):
                raise ScenarioSpecError(
                    f"{field}: env_set may target only S or C instances"
                )
            _validate_environment(event["env"], f"{field}.env")
            if instance["role"] == "S":
                if set(event["env"]) != {"ICECC_P50_PROFILE"}:
                    raise ScenarioSpecError(
                        f"{field}.env: S env_set must set only ICECC_P50_PROFILE"
                    )
                if event["env"]["ICECC_P50_PROFILE"] not in PROFILES | {"OFF"}:
                    raise ScenarioSpecError(
                        f"{field}.env.ICECC_P50_PROFILE: must be a product profile or 'OFF'"
                    )
            else:
                if set(event["env"]) == {"ICECC_P50_MODE"}:
                    if event["env"]["ICECC_P50_MODE"] not in ("on", "off"):
                        raise ScenarioSpecError(
                            f"{field}.env.ICECC_P50_MODE: must be 'on' or 'off'"
                        )
                elif set(event["env"]) == {P29_FAULT_ENV}:
                    if generations[instance["image"]] != 50:
                        raise ScenarioSpecError(
                            f"{field}.env.{P29_FAULT_ENV}: requires a p50 C"
                        )
                    if event["env"][P29_FAULT_ENV] != P29_FAULT_VALUE:
                        raise ScenarioSpecError(
                            f"{field}.env.{P29_FAULT_ENV}: must be {P29_FAULT_VALUE!r}"
                        )
                else:
                    raise ScenarioSpecError(
                        f"{field}.env: C env_set must set exactly one authorized product setting"
                    )
        if action == "netem_set":
            if RATE_RE.fullmatch(event["rate"]) is None:
                raise ScenarioSpecError(f"{field}.rate: invalid rate")
            if not farm_hosts[instance["host"]]["netem"]:
                raise ScenarioSpecError(f"{field}: netem host lacks authority")
        if action in ("disk_fill", "header_edit") and instance["role"] != "F":
            raise ScenarioSpecError(f"{field}: {action} may target only F instances")
        if action == "scheduler-loss-active":
            if instance["role"] != "S" or not re.fullmatch(r"job [1-9][0-9]*", event["trigger"]):
                raise ScenarioSpecError(
                    f"{field}: scheduler-loss-active requires a job-triggered S instance"
                )
            if sum(item["role"] == "F" for item in value["instances"]) != 1:
                raise ScenarioSpecError(
                    f"{field}: scheduler-loss-active requires exactly one F instance"
                )
        if action == "header_edit":
            _validate_container_path(event["path"], f"{field}.path")

    return ScenarioSpec(path=resolved, data=copy.deepcopy(value))
