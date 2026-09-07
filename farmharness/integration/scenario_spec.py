"""Load and semantically validate ``icefarm-scenario-v1`` documents."""

from __future__ import annotations

import copy
import hashlib
import ipaddress
import re
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
        "ICECC_P50_COMPILE_IDENTITY_TRACE",
        "ICECC_P50_C_ACTION_TRACE",
        "ICECC_P50_C_LEGACY_WIRE_TRACE",
        "ICECC_P50_F_ACTION_TRACE",
        "ICECC_P50_F_LEGACY_WIRE_TRACE",
        "ICECC_P50_H3_MUTANT",
        "ICECC_P50_H3_SCHEDULER_INSTANCE",
        "ICECC_P50_H3_TRACE",
        "ICECC_P50_S30_MUTANT_TRACE",
        "ICECC_P50_SOURCE_RESULT_TRACE",
        "ICECC_P50_TEST_LIFECYCLE_TRACE",
        "ICECC_P50_TEST_READY_TRACE",
        "ICECC_SCHEDULER",
        "ICECC_TEST_SOCKET",
        "ICECC_VERSION",
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
            if instance["role"] != "C":
                raise ScenarioSpecError(
                    f"$.instances[{index}].system_source_snapshot: valid only for C instances"
                )
            snapshots = farm.data.get("system_source_snapshots", {})
            snapshot = snapshots.get(snapshot_name)
            if not isinstance(snapshot, dict):
                raise ScenarioSpecError(
                    f"$.instances[{index}].system_source_snapshot: absent from farm authority"
                )
            if client_environment != "fedora-clang-libcxx":
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
    if workload["driver"] != corpus["kind"]:
        raise ScenarioSpecError(
            "$.workload.driver: does not match the declared corpus kind"
        )
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
