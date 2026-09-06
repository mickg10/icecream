"""Pure promotion of an observed daemon-mutant image receipt."""

from __future__ import annotations

import copy
import hashlib
import re
from collections.abc import Mapping
from typing import Any

from .farm_spec import FarmSpec
from .images import (
    DAEMON_ROLE_PATH,
    DAEMON_ROLE_PROBE_LABEL,
    IMAGE_RECEIPT_SCHEMA,
    _runtime_reference,
)
from .mutant import DAEMON_MUTANT_KIND, MutantError, validate_mutant_authority
from .schema_validation import canonical_bytes


SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
IMAGE_ID_RE = re.compile(r"^sha256:[0-9a-f]{64}$")


class DaemonMutantPromotionError(ValueError):
    """The candidate or observed image receipt is not promotable."""


def _digest(value: Any, field: str) -> str:
    if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
        raise DaemonMutantPromotionError(f"{field} is not a SHA-256 digest")
    return value


def _image_id(value: Any, field: str) -> str:
    if not isinstance(value, str) or IMAGE_ID_RE.fullmatch(value) is None:
        raise DaemonMutantPromotionError(f"{field} is not a Docker image ID")
    return value


def promote_daemon_mutant(
    farm: FarmSpec, label: str, receipt: Mapping[str, Any]
) -> dict[str, Any]:
    """Return a promoted copy without writing the farm.

    A promotion invalidates any prior ``authority_capture`` hash.  The returned
    document therefore removes that capture rather than forging a new hash;
    callers must run a fresh ``farmtest authority capture`` to produce the
    executable farm file.
    """

    if not isinstance(receipt, Mapping):
        raise DaemonMutantPromotionError("image receipt must be a JSON object")
    document = farm.data
    images = document.get("authority", {}).get("images", {})
    candidate = images.get(label)
    if not isinstance(candidate, Mapping) or candidate.get("kind") != DAEMON_MUTANT_KIND:
        raise DaemonMutantPromotionError(f"{label}: daemon-mutant candidate is absent")
    if any(key in candidate for key in ("closure_sha256", "id", "role_overrides")):
        raise DaemonMutantPromotionError(f"{label}: candidate is already promoted or overwritten")
    try:
        validate_mutant_authority(label, candidate, images)
    except MutantError as exc:
        raise DaemonMutantPromotionError(str(exc)) from exc
    if receipt.get("schema") != IMAGE_RECEIPT_SCHEMA:
        raise DaemonMutantPromotionError("image receipt schema is unsupported")
    if receipt.get("farm_digest") != farm.digest:
        raise DaemonMutantPromotionError("image receipt farm digest differs")
    receipt_images = receipt.get("images")
    if not isinstance(receipt_images, Mapping) or set(receipt_images) != {label}:
        raise DaemonMutantPromotionError(
            "image receipt must contain exactly the promoted candidate"
        )
    observed = receipt_images.get(label)
    if not isinstance(observed, Mapping):
        raise DaemonMutantPromotionError(f"image receipt lacks {label}")
    if observed.get("commit") != candidate.get("commit"):
        raise DaemonMutantPromotionError("receipt commit differs from candidate")
    if observed.get("archive_sha256") != candidate.get("archive_sha256"):
        raise DaemonMutantPromotionError("receipt recipe identity differs from candidate")
    expected_reference = _runtime_reference(farm, label)
    if observed.get("reference") != expected_reference:
        raise DaemonMutantPromotionError("receipt reference differs from farm authority")
    commands = receipt.get("commands")
    if not isinstance(commands, list):
        raise DaemonMutantPromotionError("image receipt commands are absent")
    command_fields = {
        "argv", "host", "instance", "phase", "sequence", "timeout_s", "transport"
    }
    if any(
        not isinstance(command, Mapping) or set(command) != command_fields
        for command in commands
    ):
        raise DaemonMutantPromotionError("image receipt contains malformed commands")
    sequences = [command["sequence"] for command in commands]
    if (
        any(type(sequence) is not int for sequence in sequences)
        or sorted(sequences) != list(range(len(commands)))
    ):
        raise DaemonMutantPromotionError("image receipt command sequence is not contiguous")
    probe_commands = [
        command for command in commands
        if isinstance(command, Mapping)
        and command.get("phase") == "images.probe-daemon-role"
    ]
    if len(probe_commands) != 1:
        raise DaemonMutantPromotionError(
            "image receipt must contain exactly one daemon role probe"
        )
    probe = probe_commands[0]
    if (
        probe.get("host") != "hub"
        or probe.get("transport") != "local-docker"
        or probe.get("instance") is not None
        or type(probe.get("timeout_s")) is not int
        or probe["timeout_s"] <= 0
        or type(probe.get("sequence")) is not int
        or probe["sequence"] < 0
    ):
        raise DaemonMutantPromotionError("daemon role probe command identity is unsafe")
    expected_argv = (
        "docker", "run", "--rm", "--pull=never", "--network", "none",
        "--cap-drop=ALL", "--security-opt=no-new-privileges", "--read-only",
        "--label", DAEMON_ROLE_PROBE_LABEL, "--entrypoint", "/usr/bin/sha256sum",
        expected_reference, DAEMON_ROLE_PATH,
    )
    argv = probe.get("argv")
    if not isinstance(argv, (list, tuple)) or not all(
        isinstance(item, str) for item in argv
    ) or tuple(argv) != expected_argv:
        raise DaemonMutantPromotionError("daemon role probe argv is unsafe")
    hub_id = _image_id(observed.get("hub_id"), f"{label}.hub_id")
    closure = _digest(observed.get("closure_sha256"), f"{label}.closure_sha256")
    daemon_sha = _digest(observed.get("daemon_role_sha256"), f"{label}.daemon_role_sha256")
    if observed.get("closure_schema") != "docker-inspect-runtime-closure-v1":
        raise DaemonMutantPromotionError("receipt closure schema is unsupported")
    hosts = observed.get("hosts")
    if not isinstance(hosts, Mapping) or set(hosts) != set(farm.hosts):
        raise DaemonMutantPromotionError("receipt host set differs from farm hosts")
    for host_name, host_observed in hosts.items():
        if not isinstance(host_observed, Mapping):
            raise DaemonMutantPromotionError(f"receipt host {host_name} is malformed")
        if _digest(host_observed.get("closure_sha256"), f"{label}.{host_name}.closure_sha256") != closure:
            raise DaemonMutantPromotionError(f"receipt host {host_name} closure differs")
        _image_id(host_observed.get("id"), f"{label}.{host_name}.id")
    promoted = copy.deepcopy(document)
    promoted.pop("authority_capture", None)
    promoted_candidate = promoted["authority"]["images"][label]
    promoted_candidate["closure_sha256"] = closure
    promoted_candidate["id"] = hub_id
    promoted_candidate["role_overrides"] = {"daemon": {"sha256": daemon_sha}}
    try:
        validate_mutant_authority(
            label, promoted_candidate, promoted["authority"]["images"]
        )
    except MutantError as exc:
        raise DaemonMutantPromotionError(
            f"promoted candidate failed mutant validation: {exc}"
        ) from exc
    # Revalidate the returned authority object through the same immutable digest law.
    expected_authority = promoted["authority"]
    if hashlib.sha256(canonical_bytes(expected_authority)).hexdigest() == hashlib.sha256(
        canonical_bytes(document["authority"])
    ).hexdigest():
        raise DaemonMutantPromotionError("promotion did not change authority")
    return promoted
