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
    from .firefox_corpus_promotion import (
        FirefoxCorpusPromotionError,
        validate_corpus_promotion,
    )
    from .mutant import MutantError, file_sha256, validate_mutant_authority
    from .schema_validation import ValidationError, canonical_bytes, load_json, validate
except ImportError:  # Direct execution from this directory.
    from firefox_corpus_promotion import (
        FirefoxCorpusPromotionError,
        validate_corpus_promotion,
    )
    from mutant import MutantError, file_sha256, validate_mutant_authority
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
            elif not {
                "turn_a_manifest",
                "turn_b_manifest",
                "normalized_manifest_sha256",
                "pair_index_sha256",
                "authority_receipt",
            } <= set(corpus):
                raise FarmSpecError(
                    f"$.corpora.{name}: paired tu-manifest requires both manifests and "
                    "their normalized/pair hashes and compile-valid authority receipt"
                )
            else:
                receipt = corpus["authority_receipt"]
                _absolute_safe_path(
                    receipt["path"], f"$.corpora.{name}.authority_receipt.path"
                )
                _exact_digest(
                    receipt["sha256"],
                    f"$.corpora.{name}.authority_receipt.sha256",
                    SHA256_RE,
                )
                try:
                    validate_corpus_promotion(corpus)
                except FirefoxCorpusPromotionError as exc:
                    raise FarmSpecError(
                        f"$.corpora.{name}.authority_receipt: {exc}"
                    ) from exc
        for field, item in corpus.items():
            if field.endswith("_sha256"):
                _exact_digest(item, f"$.corpora.{name}.{field}", SHA256_RE)
        archives = corpus.get("archives")
        expected_groups = {"files"} if "manifest" in corpus else {"A", "B"}
        if not isinstance(archives, dict) or set(archives) != expected_groups:
            raise FarmSpecError(
                f"$.corpora.{name}.archives: expected exactly "
                + ", ".join(sorted(expected_groups))
            )
        source_authority = {
            key: item
            for key, item in corpus.items()
            if key not in ("archives", "compression")
        }
        source_authority_sha256 = hashlib.sha256(
            canonical_bytes(source_authority)
        ).hexdigest()
        for group, archive in archives.items():
            _safe_name(group, f"$.corpora.{name}.archives.<key:{group!r}>")
            _absolute_safe_path(
                archive["archive"], f"$.corpora.{name}.archives.{group}.archive"
            )
            for field in ("archive_sha256", "authority_sha256", "manifest_sha256"):
                _exact_digest(
                    archive[field],
                    f"$.corpora.{name}.archives.{group}.{field}",
                    SHA256_RE,
                )
            if archive["files"] != corpus["tus"]:
                raise FarmSpecError(
                    f"$.corpora.{name}.archives.{group}.files: expected {corpus['tus']}"
                )
            expected_authority = hashlib.sha256(
                canonical_bytes(
                    {
                        "corpus_authority_sha256": source_authority_sha256,
                        "group": group,
                        "manifest_sha256": archive["manifest_sha256"],
                    }
                )
            ).hexdigest()
            if archive["authority_sha256"] != expected_authority:
                raise FarmSpecError(
                    f"$.corpora.{name}.archives.{group}.authority_sha256: "
                    "does not bind the corpus source authority and manifest"
                )
        for environment_name, recipe in corpus.get("compiler_recipes", {}).items():
            _safe_name(
                environment_name,
                f"$.corpora.{name}.compiler_recipes.<key:{environment_name!r}>",
            )
            for index, argument in enumerate(recipe["arguments"]):
                if any(ord(character) < 32 or ord(character) == 127 for character in argument):
                    raise FarmSpecError(
                        f"$.corpora.{name}.compiler_recipes.{environment_name}."
                        f"arguments[{index}]: control characters are forbidden"
                    )

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
    runtime_image = value["runtime_image"]
    _exact_digest(runtime_image["id"], "$.runtime_image.id", IMAGE_ID_RE)
    _exact_digest(
        runtime_image["closure_sha256"],
        "$.runtime_image.closure_sha256",
        SHA256_RE,
    )
    _relative_safe_path(runtime_image["dockerfile"], "$.runtime_image.dockerfile")
    if any(
        ord(character) < 32 or ord(character) == 127
        for character in runtime_image["reference"]
    ):
        raise FarmSpecError("$.runtime_image.reference: control characters are forbidden")

    client_environments = value["client_environments"]
    expected_client_environments = {
        "conan-gcc": "ice-ii/conan-gcc:v2-p50",
        "debian-gcc": "ice-ii/debian-gcc:v2-p50",
        "fedora-clang-libcxx": "ice-ii/fedora-clang-libcxx:v2-p50",
        "linuxbrew": "ice-ii/linuxbrew:v2-p50",
    }
    if set(client_environments) != set(expected_client_environments):
        raise FarmSpecError(
            "$.client_environments: exactly conan-gcc, debian-gcc, "
            "fedora-clang-libcxx, and linuxbrew are required"
        )
    client_references: set[str] = set()
    for name, environment in client_environments.items():
        _safe_name(name, f"$.client_environments.<key:{name!r}>")
        _exact_digest(
            environment["id"],
            f"$.client_environments.{name}.id",
            IMAGE_ID_RE,
        )
        _exact_digest(
            environment["closure_sha256"],
            f"$.client_environments.{name}.closure_sha256",
            SHA256_RE,
        )
        _relative_safe_path(
            environment["dockerfile"],
            f"$.client_environments.{name}.dockerfile",
        )
        if any(
            ord(character) < 32 or ord(character) == 127
            for character in environment["reference"]
        ):
            raise FarmSpecError(
                f"$.client_environments.{name}.reference: "
                "control characters are forbidden"
            )
        if environment["reference"] != expected_client_environments[name]:
            raise FarmSpecError(
                f"$.client_environments.{name}.reference: expected "
                f"{expected_client_environments[name]!r}"
            )
        if environment["reference"] == runtime_image["reference"]:
            raise FarmSpecError(
                f"$.client_environments.{name}.reference: "
                "C and S/F runtime images must be distinct"
            )
        if environment["reference"] in client_references:
            raise FarmSpecError(
                f"$.client_environments.{name}.reference: "
                "duplicate C image reference"
            )
        client_references.add(environment["reference"])
    snapshots = value.get("system_source_snapshots", {})
    for name, snapshot in snapshots.items():
        _safe_name(name, f"$.system_source_snapshots.<key:{name!r}>")
        if snapshot["source_runtime_reference"] != runtime_image["reference"]:
            raise FarmSpecError(
                f"$.system_source_snapshots.{name}.source_runtime_reference: differs from runtime image"
            )
        if snapshot["source_runtime_id"] != runtime_image["id"]:
            raise FarmSpecError(
                f"$.system_source_snapshots.{name}.source_runtime_id: differs from runtime image"
            )
        if snapshot["source_runtime_closure_sha256"] != runtime_image["closure_sha256"]:
            raise FarmSpecError(
                f"$.system_source_snapshots.{name}.source_runtime_closure_sha256: differs from runtime image"
            )
        archive = snapshot["archive"]
        _absolute_safe_path(
            archive["path"], f"$.system_source_snapshots.{name}.archive.path"
        )
        if snapshot["enumeration"]["roots"] != [
            "/usr/include", "/usr/lib/gcc", "/usr/local/include"
        ]:
            raise FarmSpecError(
                f"$.system_source_snapshots.{name}.enumeration.roots: unsupported roots"
            )
    for name, corpus in value["corpora"].items():
        for environment_name, recipe in corpus.get("compiler_recipes", {}).items():
            if environment_name not in client_environments:
                raise FarmSpecError(
                    f"$.corpora.{name}.compiler_recipes.{environment_name}: "
                    "absent from client environments"
                )
            _absolute_safe_path(
                recipe["executable"],
                f"$.corpora.{name}.compiler_recipes.{environment_name}.executable",
            )
            _exact_digest(
                recipe["binary_sha256"],
                f"$.corpora.{name}.compiler_recipes.{environment_name}.binary_sha256",
                SHA256_RE,
            )
            _exact_digest(
                recipe["configuration_sha256"],
                f"$.corpora.{name}.compiler_recipes.{environment_name}.configuration_sha256",
                SHA256_RE,
            )
            if any(
                ord(character) < 32 or ord(character) == 127
                for character in recipe["version"]
            ):
                raise FarmSpecError(
                    f"$.corpora.{name}.compiler_recipes.{environment_name}.version: "
                    "control characters are forbidden"
                )
            toolchain = recipe.get("toolchain")
            if toolchain is not None:
                _absolute_safe_path(
                    toolchain["archive"],
                    f"$.corpora.{name}.compiler_recipes.{environment_name}."
                    "toolchain.archive",
                )
                _exact_digest(
                    toolchain["archive_sha256"],
                    f"$.corpora.{name}.compiler_recipes.{environment_name}."
                    "toolchain.archive_sha256",
                    SHA256_RE,
                )
                _absolute_safe_path(
                    toolchain["mount"],
                    f"$.corpora.{name}.compiler_recipes.{environment_name}."
                    "toolchain.mount",
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
        revision = image.get("cache_wire_revision")
        if revision is not None and (
            type(revision) is not int or not (1 <= revision <= 0xFFFF)
        ):
            raise FarmSpecError(
                f"$.authority.images.{label}.cache_wire_revision: "
                "must be an integer in 1..65535"
            )
        if "closure_sha256" in image:
            _exact_digest(
                image["closure_sha256"],
                f"$.authority.images.{label}.closure_sha256",
                SHA256_RE,
            )
        if "id" in image:
            _exact_digest(image["id"], f"$.authority.images.{label}.id", IMAGE_ID_RE)
        if image.get("kind") in ("scheduler-mutant", "daemon-mutant"):
            try:
                validate_mutant_authority(label, image, authority["images"])
                patch_reference = image["patch_path"]
                patch_path = Path(resolved.parent, patch_reference).resolve()
                if not patch_path.is_file() or patch_path.is_symlink():
                    # Test and staging copies of farm.example.json retain the
                    # checked-in recipe path; authority still binds the bytes
                    # to this harness support file, never to the copied farm.
                    patch_path = (Path(__file__).parent / patch_reference).resolve()
                if not patch_path.is_file() or patch_path.is_symlink():
                    raise MutantError(f"{label}: mutant patch is absent or unsafe")
                if file_sha256(patch_path) != image["patch_sha256"]:
                    raise MutantError(f"{label}: mutant patch hash does not match authority")
                overrides = image.get("role_overrides")
                if overrides is not None:
                    role_key = "scheduler" if image.get("kind") == "scheduler-mutant" else "daemon"
                    role_override = overrides.get(role_key) if isinstance(overrides, dict) else None
                    digest = role_override.get("sha256") if isinstance(role_override, dict) else None
                    if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
                        raise MutantError(
                            f"{label}: {role_key} role override hash is invalid"
                        )
            except (KeyError, MutantError) as exc:
                raise FarmSpecError(f"$.authority.images.{label}: {exc}") from exc
    for name in authority["topologies"]:
        _safe_name(name, f"$.authority.topologies.<key:{name!r}>")

    capture = value.get("authority_capture")
    if capture is not None:
        expected_authority_sha256 = hashlib.sha256(canonical_bytes(authority)).hexdigest()
        if capture["authority_sha256"] != expected_authority_sha256:
            raise FarmSpecError(
                "$.authority_capture.authority_sha256: does not bind $.authority"
            )
        if set(capture["hosts"]) != names:
            raise FarmSpecError(
                "$.authority_capture.hosts: must exactly match the declared farm hosts"
            )
        physical_hosts: set[str] = set()
        for index, host in enumerate(value["hosts"]):
            descriptor = capture["hosts"][host["name"]]
            if descriptor["address"] != host["lan_ip"]:
                raise FarmSpecError(
                    f"$.authority_capture.hosts.{host['name']}.address: "
                    "does not match the declared LAN address"
                )
            if descriptor["arch"] != host["arch"] or descriptor["cpu_count"] != host["cores"]:
                raise FarmSpecError(
                    f"$.authority_capture.hosts.{host['name']}: CPU identity differs "
                    f"from $.hosts[{index}]"
                )
            physical = descriptor["physical_host_sha256"]
            expected_physical = hashlib.sha256(
                canonical_bytes(
                    {
                        "machine_id_sha256": descriptor["machine_id_sha256"],
                        "nic_identity_sha256": descriptor["nic_identity_sha256"],
                    }
                )
            ).hexdigest()
            if physical != expected_physical:
                raise FarmSpecError(
                    f"$.authority_capture.hosts.{host['name']}.physical_host_sha256: "
                    "does not bind machine and NIC identity"
                )
            if physical in physical_hosts:
                raise FarmSpecError(
                    "$.authority_capture.hosts: duplicate physical host identity"
                )
            physical_hosts.add(physical)
            docker = descriptor["docker"]
            if docker["os"] != "linux":
                raise FarmSpecError(
                    f"$.authority_capture.hosts.{host['name']}.docker.os: "
                    "a Linux Docker daemon is required"
                )
            _absolute_safe_path(
                docker["root_dir"],
                f"$.authority_capture.hosts.{host['name']}.docker.root_dir",
            )

    return FarmSpec(path=resolved, data=copy.deepcopy(value))
