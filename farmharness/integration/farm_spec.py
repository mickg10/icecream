"""Load and semantically validate ``icefarm-farm-v1`` documents."""

from __future__ import annotations

import copy
import hashlib
import ipaddress
import re
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any

try:
    from .schema_validation import ValidationError, canonical_bytes, load_json, validate
except ImportError:  # Direct execution from this directory.
    from schema_validation import ValidationError, canonical_bytes, load_json, validate


SCHEMA_PATH = Path(__file__).with_name("schemas") / "farm-v1.json"
SHA256_RE = re.compile(r"[0-9a-f]{64}")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")
IMAGE_ID_RE = re.compile(r"sha256:[0-9a-f]{64}")
IMAGE_LABEL_RE = re.compile(
    r"p(43|44|50)(s[0-9]+)?-[A-Za-z0-9][A-Za-z0-9._-]*", re.IGNORECASE
)


class FarmSpecError(ValidationError):
    """Farm data is invalid or unsafe to execute."""


def _absolute_safe_path(value: str, field: str) -> None:
    path = PurePosixPath(value)
    if (
        not path.is_absolute()
        or not any(part not in ("/", "//") for part in path.parts)
        or ".." in path.parts
        or any(ord(character) < 32 or ord(character) == 127 for character in value)
    ):
        raise FarmSpecError(
            f"{field}: must be a control-free, non-root absolute path without '..'"
        )


def _relative_safe_path(value: str, field: str) -> None:
    path = PurePosixPath(value)
    if (
        path.is_absolute()
        or ".." in path.parts
        or value in ("", ".")
        or any(ord(character) < 32 or ord(character) == 127 for character in value)
    ):
        raise FarmSpecError(f"{field}: must be a safe relative path")


def _safe_name(value: str, field: str) -> None:
    if value in (".", "..") or any(
        ord(character) < 32 or ord(character) == 127 for character in value
    ):
        raise FarmSpecError(
            f"{field}: control characters and dot path components are forbidden"
        )


def _exact_digest(value: str, field: str, pattern: re.Pattern[str]) -> None:
    if pattern.fullmatch(value) is None:
        raise FarmSpecError(f"{field}: invalid immutable digest")


@dataclass(frozen=True)
class FarmSpec:
    path: Path
    data: dict[str, Any]

    @property
    def hosts(self) -> dict[str, dict[str, Any]]:
        return {host["name"]: host for host in self.data["hosts"]}

    @property
    def digest(self) -> str:
        return hashlib.sha256(canonical_bytes(self.data)).hexdigest()


def load_farm_spec(path: str | Path) -> FarmSpec:
    resolved = Path(path).resolve()
    try:
        value = load_json(resolved)
        validate(value, SCHEMA_PATH)
    except ValidationError as exc:
        raise FarmSpecError(str(exc)) from exc
    if not isinstance(value, dict):
        raise FarmSpecError("$: farm root must be an object")

    start, end = value["port_range"]
    if start > end:
        raise FarmSpecError("$.port_range: first port must not exceed the second")

    _absolute_safe_path(value["hub"]["results_root"], "$.hub.results_root")
    _safe_name(value["netname_prefix"], "$.netname_prefix")
    names: set[str] = set()
    ssh_targets: set[str] = set()
    contexts: set[str] = set()
    for index, host in enumerate(value["hosts"]):
        name = host["name"]
        _safe_name(name, f"$.hosts[{index}].name")
        if name in names:
            raise FarmSpecError(f"$.hosts[{index}].name: duplicate host {name!r}")
        names.add(name)
        if host["ssh"] in ssh_targets:
            raise FarmSpecError(f"$.hosts[{index}].ssh: duplicate target {host['ssh']!r}")
        if any(ord(character) < 32 or ord(character) == 127 for character in host["ssh"]):
            raise FarmSpecError(f"$.hosts[{index}].ssh: control characters are forbidden")
        ssh_targets.add(host["ssh"])
        context = host.get("docker_context")
        if context:
            _safe_name(context, f"$.hosts[{index}].docker_context")
            if context in contexts:
                raise FarmSpecError(f"$.hosts[{index}].docker_context: duplicate {context!r}")
            contexts.add(context)
        _absolute_safe_path(host["scratch_root"], f"$.hosts[{index}].scratch_root")
        try:
            ipaddress.ip_address(host["lan_ip"])
            network = ipaddress.ip_network(host["shared_lan"], strict=False)
        except ValueError as exc:
            raise FarmSpecError(f"$.hosts[{index}]: invalid lan_ip/shared_lan: {exc}") from exc
        if ipaddress.ip_address(host["lan_ip"]) not in network:
            raise FarmSpecError(f"$.hosts[{index}]: lan_ip is outside shared_lan")

    if value["registry"]["host"] not in names:
        raise FarmSpecError("$.registry.host: host is not declared")
    if not value["protected"]["process_patterns"] or any(
        not item.strip() for item in value["protected"]["process_patterns"]
    ):
        raise FarmSpecError("$.protected.process_patterns: empty pattern is forbidden")
    if any(
        any(ord(character) < 32 or ord(character) == 127 for character in item)
        for item in value["protected"]["process_patterns"]
    ):
        raise FarmSpecError("$.protected.process_patterns: control characters are forbidden")

    for name, corpus in value["corpora"].items():
        _safe_name(name, f"$.corpora.{name}")
        for field in (
            "hub_path",
            "root",
            "manifest",
            "turn_a_manifest",
            "turn_b_manifest",
        ):
            if field in corpus:
                _absolute_safe_path(corpus[field], f"$.corpora.{name}.{field}")
        for field in ("turn_a", "turn_b"):
            if field in corpus:
                _relative_safe_path(corpus[field], f"$.corpora.{name}.{field}")
        if corpus["kind"] == "tu-manifest":
            single = "manifest" in corpus
            paired = "turn_a_manifest" in corpus or "turn_b_manifest" in corpus
            if single == paired:
                raise FarmSpecError(
                    f"$.corpora.{name}: tu-manifest requires exactly one of manifest "
                    "or the turn_a_manifest/turn_b_manifest pair"
                )
            if single:
                if "manifest_sha256" not in corpus:
                    raise FarmSpecError(
                        f"$.corpora.{name}.manifest_sha256: required for manifest"
                    )
            elif not {"turn_a_manifest", "turn_b_manifest", "normalized_manifest_sha256", "pair_index_sha256"} <= set(corpus):
                raise FarmSpecError(
                    f"$.corpora.{name}: paired tu-manifest requires both manifests and "
                    "their normalized/pair authority hashes"
                )
        for field, item in corpus.items():
            if field.endswith("_sha256"):
                _exact_digest(item, f"$.corpora.{name}.{field}", SHA256_RE)

    authority = value["authority"]
    if authority["schema"] != "icecream-newgen-farm-authority-v1":
        raise FarmSpecError("$.authority.schema: newgen authority v1 is required")
    authority_hosts = authority["hosts"]
    for name, entry in authority_hosts.items():
        _safe_name(name, f"$.authority.hosts.<key:{name!r}>")
        if any(
            ord(character) < 32 or ord(character) == 127
            for character in entry["address"]
        ):
            raise FarmSpecError(
                f"$.authority.hosts.{name}.address: control characters are forbidden"
            )
    for index, host in enumerate(value["hosts"]):
        entry = authority_hosts.get(host["name"])
        if not isinstance(entry, dict):
            raise FarmSpecError(
                f"$.hosts[{index}].name: host is absent from the newgen authority"
            )
        if entry.get("address") != host["lan_ip"]:
            raise FarmSpecError(
                f"$.hosts[{index}].lan_ip: does not match authority address"
            )
    for version, store in authority["role_stores"].items():
        if version not in ("43", "44", "50"):
            raise FarmSpecError(
                f"$.authority.role_stores.<key:{version!r}>: unsupported version"
            )
        for role in ("scheduler", "client", "daemon"):
            _exact_digest(
                store[role]["sha256"],
                f"$.authority.role_stores.{version}.{role}.sha256",
                SHA256_RE,
            )
    for label, image in authority["images"].items():
        if IMAGE_LABEL_RE.fullmatch(label) is None:
            raise FarmSpecError(
                f"$.authority.images.<key:{label!r}>: invalid display label"
            )
        _exact_digest(image["commit"], f"$.authority.images.{label}.commit", COMMIT_RE)
        _exact_digest(
            image["archive_sha256"],
            f"$.authority.images.{label}.archive_sha256",
            SHA256_RE,
        )
        if "id" in image:
            _exact_digest(image["id"], f"$.authority.images.{label}.id", IMAGE_ID_RE)
    for name in authority["topologies"]:
        _safe_name(name, f"$.authority.topologies.<key:{name!r}>")

    return FarmSpec(path=resolved, data=copy.deepcopy(value))
