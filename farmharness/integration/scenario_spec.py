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
RUNNER_ENV = frozenset(
    (
        "ICECC_NETNAME",
        "ICECC_P50_COMPILE_IDENTITY_TRACE",
        "ICECC_P50_C_ACTION_TRACE",
        "ICECC_P50_C_LEGACY_WIRE_TRACE",
        "ICECC_P50_F_ACTION_TRACE",
        "ICECC_P50_F_LEGACY_WIRE_TRACE",
        "ICECC_P50_SOURCE_RESULT_TRACE",
        "ICECC_P50_TEST_LIFECYCLE_TRACE",
        "ICECC_P50_TEST_READY_TRACE",
        "ICECC_SCHEDULER",
        "ICECC_TEST_SOCKET",
        "ICECC_VERSION",
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
            raise ScenarioSpecError(f"{field}.{key}: runner-owned environment is forbidden")
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


def _validate_shape(value: dict[str, Any], generations: dict[str, int]) -> None:
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
        matches = all_new("S") and f_states == {False, True} and c_states == {False, True}
    elif shape == "S'[F'F''][C']":
        worker_images = {value["images"][item["image"]] for item in by_role["F"]}
        matches = (
            all_new("S")
            and all_new("C")
            and all_new("F")
            and len(worker_images) >= 2
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
    for alias, reference in value["images"].items():
        _validate_safe_name(alias, f"$.images.<key:{alias!r}>")
        if any(ord(character) < 32 or ord(character) == 127 for character in reference):
            raise ScenarioSpecError(f"$.images.{alias}: control characters are forbidden")
        generations[alias] = _image_generation(reference, f"$.images.{alias}")
        if reference not in farm.data["authority"]["images"]:
            raise ScenarioSpecError(
                f"$.images.{alias}: image label is absent from the immutable authority map"
            )

    names: set[str] = set()
    role_instances: dict[str, list[dict[str, Any]]] = {"S": [], "F": [], "C": []}
    for index, instance in enumerate(value["instances"]):
        name = instance["name"]
        _validate_safe_name(name, f"$.instances[{index}].name")
        if name in names:
            raise ScenarioSpecError(f"$.instances[{index}].name: duplicate instance {name!r}")
        names.add(name)
        role_instances[instance["role"]].append(instance)
        if instance["host"] not in farm_hosts:
            raise ScenarioSpecError(f"$.instances[{index}].host: undeclared host {instance['host']!r}")
        if instance["image"] not in value["images"]:
            raise ScenarioSpecError(f"$.instances[{index}].image: undeclared alias {instance['image']!r}")
        allowed = farm_hosts[instance["host"]]["roles_allowed"]
        if instance["role"] not in allowed:
            detail = "lacks SYS_CHROOT/F authority" if instance["role"] == "F" else "role is not allowed"
            raise ScenarioSpecError(f"$.instances[{index}]: host {instance['host']!r} {detail}")
        if instance["role"] != "F" and "slots" in instance:
            raise ScenarioSpecError(f"$.instances[{index}].slots: only F instances have slots")
        environment = instance.get("env", {})
        profile = environment.get("ICECC_P50_PROFILE")
        if profile is not None and profile not in PROFILES:
            raise ScenarioSpecError(
                f"$.instances[{index}].env.ICECC_P50_PROFILE: must be one of {sorted(PROFILES)!r}"
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
        _validate_environment(environment, f"$.instances[{index}].env")

    if len(role_instances["S"]) != 1:
        raise ScenarioSpecError("$.instances: scenario must declare exactly one S instance")
    if not role_instances["F"] or not role_instances["C"]:
        raise ScenarioSpecError("$.instances: scenario must declare at least one F and one C instance")
    _validate_shape(value, generations)

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
        raise ScenarioSpecError(f"$.workload.corpus: undeclared corpus {workload['corpus']!r}")
    if workload["driver"] != corpus["kind"]:
        raise ScenarioSpecError("$.workload.driver: does not match the declared corpus kind")
    allowed_turns = {"A"} if "manifest" in corpus else {"A", "B"}
    unknown_turns = sorted(set(workload["turns"]) - allowed_turns)
    if unknown_turns:
        raise ScenarioSpecError(
            f"$.workload.turns: corpus does not define turns {unknown_turns!r}"
        )
    for client in workload["clients"]:
        instance = next((item for item in role_instances["C"] if item["name"] == client), None)
        if instance is None:
            raise ScenarioSpecError(f"$.workload.clients: {client!r} is not a C instance")
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

    shaped: set[str] = set()
    for index, shaping in enumerate(value["network"]["shaping"]):
        name = shaping["instance"]
        if RATE_RE.fullmatch(shaping["rate"]) is None:
            raise ScenarioSpecError(f"$.network.shaping[{index}].rate: invalid rate")
        if name in shaped:
            raise ScenarioSpecError(f"$.network.shaping[{index}]: duplicate instance {name!r}")
        shaped.add(name)
        instance = next((item for item in value["instances"] if item["name"] == name), None)
        if instance is None:
            raise ScenarioSpecError(f"$.network.shaping[{index}]: unknown instance {name!r}")
        if not farm_hosts[instance["host"]]["netem"]:
            raise ScenarioSpecError(
                f"$.network.shaping[{index}]: host {instance['host']!r} has no netem authority"
            )

    for index, event in enumerate(value["timeline"]):
        field = f"$.timeline[{index}]"
        action = event["action"]
        instance_name = event.get("instance")
        if instance_name is None:
            raise ScenarioSpecError(f"{field}.instance: required for action {action!r}")
        if instance_name not in names:
            raise ScenarioSpecError(f"{field}.instance: unknown instance {instance_name!r}")
        instance = next(item for item in value["instances"] if item["name"] == instance_name)

        action_fields = {
            "upgrade": frozenset(("image",)),
            "downgrade": frozenset(("image",)),
            "restart": frozenset(),
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
                raise ScenarioSpecError(f"{field}: env_set may target only S or C instances")
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
                if set(event["env"]) != {"ICECC_P50_MODE"}:
                    raise ScenarioSpecError(
                        f"{field}.env: C env_set must set only ICECC_P50_MODE"
                    )
                if event["env"]["ICECC_P50_MODE"] not in ("on", "off"):
                    raise ScenarioSpecError(
                        f"{field}.env.ICECC_P50_MODE: must be 'on' or 'off'"
                    )
        if action == "netem_set":
            if RATE_RE.fullmatch(event["rate"]) is None:
                raise ScenarioSpecError(f"{field}.rate: invalid rate")
            if not farm_hosts[instance["host"]]["netem"]:
                raise ScenarioSpecError(f"{field}: netem host lacks authority")
        if action in ("disk_fill", "header_edit") and instance["role"] != "F":
            raise ScenarioSpecError(f"{field}: {action} may target only F instances")
        if action == "header_edit":
            _validate_container_path(event["path"], f"{field}.path")

    return ScenarioSpec(path=resolved, data=copy.deepcopy(value))
