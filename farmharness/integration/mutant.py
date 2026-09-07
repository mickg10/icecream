"""Hash-bound recipes and evidence contract for the H3 scheduler mutant.

The mutant is an integration-only image derived from one sealed P50 source
archive.  The recipe is deliberately pure: it never builds or contacts a
host.  Image construction consumes the returned recipe and must reproduce its
base archive and patch digests before applying the patch.
"""

from __future__ import annotations

import hashlib
import re
import struct
from pathlib import Path
from typing import Any, Mapping

try:
    from .schema_validation import canonical_bytes
except ImportError:  # Direct execution from this directory.
    from schema_validation import canonical_bytes


MUTANT_KIND = "scheduler-mutant"
MUTANT_RECIPE_SCHEMA = "icefarm-scheduler-mutant-recipe-v1"
DAEMON_MUTANT_KIND = "daemon-mutant"
DAEMON_MUTANT_RECIPE_SCHEMA = "icefarm-daemon-mutant-recipe-v1"
MUTANT_TRACE_SCHEMA = "icefarm-scheduler-mutant-trace-v1"
MUTANT_TRACE_PATH = "/results/h3-mutant.jsonl"
MUTANT_PATCH = Path(__file__).with_name("mutants") / "scheduler-tail.patch"
DAEMON_MUTANT_PATCH = (
    Path(__file__).with_name("mutants") / "daemon-session-refuse.patch"
)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
LABEL_RE = re.compile(r"^p50s[0-9]+-h3-[a-z0-9][a-z0-9._-]*$")


class MutantError(ValueError):
    """A scheduler-mutant recipe or trace is not authenticated."""


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            for block in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as exc:
        raise MutantError(f"cannot read mutant patch {path}: {exc}") from exc
    return digest.hexdigest()


def _derive_mutant(
    base_label: str,
    base: Mapping[str, Any],
    patch_path: Path,
    *,
    label: str,
    kind: str,
    recipe_schema: str,
    label_re: re.Pattern[str],
) -> dict[str, Any]:
    """Derive an immutable image authority entry from base archive + patch."""

    if not isinstance(base_label, str) or not base_label:
        raise MutantError("base image label is required")
    if label_re.fullmatch(label) is None:
        raise MutantError(f"invalid {kind} label {label!r}")
    commit = base.get("commit")
    archive_sha256 = base.get("archive_sha256")
    if not isinstance(commit, str) or COMMIT_RE.fullmatch(commit) is None:
        raise MutantError("base image commit is not an immutable commit")
    if not isinstance(archive_sha256, str) or SHA256_RE.fullmatch(archive_sha256) is None:
        raise MutantError("base image archive hash is not immutable")
    patch_sha256 = file_sha256(patch_path)
    recipe_body = {
        "base_archive_sha256": archive_sha256,
        "base_commit": commit,
        "base_label": base_label,
        "patch_sha256": patch_sha256,
        "schema": recipe_schema,
    }
    recipe_sha256 = hashlib.sha256(canonical_bytes(recipe_body)).hexdigest()
    # These are recipe identities, not a claim that an image already exists.
    # The image builder must replace closure/id with observed values in its
    # distribution receipt while retaining this exact recipe body.
    return {
        "archive_sha256": recipe_sha256,
        "base_image": base_label,
        "base_archive_sha256": archive_sha256,
        "base_commit": commit,
        "commit": commit,
        "kind": kind,
        "patch_path": str(patch_path),
        "patch_sha256": patch_sha256,
        "recipe_sha256": recipe_sha256,
        "recipe_schema": recipe_schema,
    }


def derive_scheduler_mutant(
    base_label: str,
    base: Mapping[str, Any],
    patch_path: Path = MUTANT_PATCH,
    *,
    label: str = "p50s4-h3-tail-mutant",
) -> dict[str, Any]:
    return _derive_mutant(
        base_label,
        base,
        patch_path,
        label=label,
        kind=MUTANT_KIND,
        recipe_schema=MUTANT_RECIPE_SCHEMA,
        label_re=LABEL_RE,
    )


def derive_daemon_mutant(
    base_label: str,
    base: Mapping[str, Any],
    patch_path: Path = DAEMON_MUTANT_PATCH,
    *,
    label: str = "p50s30-f-refusal-mutant",
) -> dict[str, Any]:
    return _derive_mutant(
        base_label,
        base,
        patch_path,
        label=label,
        kind=DAEMON_MUTANT_KIND,
        recipe_schema=DAEMON_MUTANT_RECIPE_SCHEMA,
        label_re=re.compile(r"^p50s[0-9]+-f-[a-z0-9][a-z0-9._-]*$"),
    )


def validate_mutant_authority(
    label: str, image: Mapping[str, Any], base_images: Mapping[str, Any]
) -> None:
    """Reject a mutant entry unless it binds to an existing sealed base."""

    kind = image.get("kind")
    if kind not in (MUTANT_KIND, DAEMON_MUTANT_KIND):
        raise MutantError(f"{label}: authority entry is not a supported mutant")
    if image.get("base_image") not in base_images:
        raise MutantError(f"{label}: base image is absent from authority")
    base = base_images[image["base_image"]]
    if image.get("base_commit") != base.get("commit"):
        raise MutantError(f"{label}: base commit is not bound to authority")
    if image.get("base_archive_sha256") != base.get("archive_sha256"):
        raise MutantError(f"{label}: base archive is not bound to authority")
    if image.get("commit") != base.get("commit"):
        raise MutantError(f"{label}: commit is not bound to base image")
    patch = image.get("patch_sha256")
    recipe = image.get("recipe_sha256")
    if not isinstance(patch, str) or SHA256_RE.fullmatch(patch) is None:
        raise MutantError(f"{label}: patch hash is invalid")
    if not isinstance(recipe, str) or SHA256_RE.fullmatch(recipe) is None:
        raise MutantError(f"{label}: recipe hash is invalid")
    expected_schema = (
        MUTANT_RECIPE_SCHEMA
        if kind == MUTANT_KIND
        else DAEMON_MUTANT_RECIPE_SCHEMA
    )
    if image.get("recipe_schema") != expected_schema:
        raise MutantError(f"{label}: recipe schema does not match mutant kind")
    expected = hashlib.sha256(
        canonical_bytes(
            {
                "base_archive_sha256": image["base_archive_sha256"],
                "base_commit": image["base_commit"],
                "base_label": image["base_image"],
                "patch_sha256": patch,
                "schema": expected_schema,
            }
        )
    ).hexdigest()
    if recipe != expected or image.get("archive_sha256") != expected:
        raise MutantError(f"{label}: recipe digest does not bind base and patch")
    overrides = image.get("role_overrides")
    if overrides is not None:
        role = "scheduler" if kind == MUTANT_KIND else "daemon"
        if not isinstance(overrides, Mapping) or set(overrides) != {role}:
            raise MutantError(f"{label}: role overrides do not match mutant kind")
        override = overrides[role]
        if (
            not isinstance(override, Mapping)
            or set(override) != {"sha256"}
            or not isinstance(override.get("sha256"), str)
            or SHA256_RE.fullmatch(override["sha256"]) is None
        ):
            raise MutantError(f"{label}: {role} role override hash is invalid")


def validate_h3_trace(
    records: Any,
    *,
    client_instances: set[str],
    scheduler_instance: str,
    jobs: list[Mapping[str, Any]],
    worker_instances: set[str] | None = None,
) -> dict[str, Any]:
    """Authenticate scheduler emission only, never client rejection."""

    if not isinstance(records, list) or not records:
        raise MutantError("H3 scheduler trace is empty")
    if any(not isinstance(item, Mapping) for item in records):
        raise MutantError("H3 scheduler trace contains a non-object")
    required = {
        "assignment_epoch",
        "assignment_nonce",
        "client_instance",
        "cache_port",
        "cache_profile_mask",
        "cache_protocol",
        "emission",
        "schema",
        "scheduler_instance",
        "scheduler_job",
        "tail_bytes",
        "tail_hex",
        "worker_instance",
    }
    claims = {
        (int(job["scheduler_job"]), str(job["client"]), str(job["worker"]))
        for job in jobs
        if type(job.get("scheduler_job")) is int
        and isinstance(job.get("client"), str)
        and isinstance(job.get("worker"), str)
    }
    seen: set[tuple[int, str, str]] = set()
    result: list[dict[str, Any]] = []
    for index, item in enumerate(records, start=1):
        if set(item) != required or item.get("schema") != MUTANT_TRACE_SCHEMA:
            raise MutantError(f"H3 scheduler trace row {index} has wrong fields")
        if item.get("scheduler_instance") != scheduler_instance:
            raise MutantError(f"H3 scheduler trace row {index} names another scheduler")
        client = item.get("client_instance")
        job = item.get("scheduler_job")
        if client not in client_instances or type(job) is not int or job <= 0:
            raise MutantError(f"H3 scheduler trace row {index} has invalid identity")
        worker = item.get("worker_instance")
        key = (job, client, worker)
        if key in seen or key not in claims:
            raise MutantError(f"H3 scheduler trace row {index} is not an assignment witness")
        seen.add(key)
        if worker_instances is None or worker not in worker_instances:
            raise MutantError(f"H3 scheduler trace row {index} targets an unapproved worker")
        if item.get("emission") != "protocol-50-tail-sent":
            raise MutantError(f"H3 scheduler trace row {index} is not an emission")
        for field in ("cache_port", "cache_protocol", "cache_profile_mask"):
            if type(item.get(field)) is not int or not (1 <= item[field] <= 65535):
                raise MutantError(f"H3 scheduler trace row {index} has invalid {field}")
        if item.get("tail_bytes") != 12:
            raise MutantError(f"H3 scheduler trace row {index} is not a 12-byte tail")
        tail = item.get("tail_hex")
        expected_tail = struct.pack(
            ">III",
            item["cache_port"],
            item["cache_protocol"],
            item["cache_profile_mask"],
        ).hex()
        if tail != expected_tail:
            raise MutantError(f"H3 scheduler trace row {index} has invalid tail bytes")
        for field in ("assignment_epoch", "assignment_nonce"):
            if type(item.get(field)) is not int or item[field] <= 0:
                raise MutantError(f"H3 scheduler trace row {index} has invalid {field}")
        result.append(dict(item))
    return {
        "records": result,
        "record_count": len(result),
        "schema": MUTANT_TRACE_SCHEMA,
        "authenticated": True,
    }


CLIENT_REJECTION_RE = re.compile(
    r"internal error - message(?: \(USE_CS\))? not read correctly, "
    r"message size ([0-9]+) read ([0-9]+)"
)


def parse_h3_client_rejections(text: str, *, client_instance: str) -> list[dict[str, Any]]:
    """Parse the old client's independent unread-payload errors."""

    result: list[dict[str, Any]] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        match = CLIENT_REJECTION_RE.search(line)
        if match is None:
            continue
        message_size, bytes_read = (int(value) for value in match.groups())
        unread = message_size - bytes_read
        if unread != 12:
            raise MutantError(
                f"client rejection line {line_number} has {unread} unread bytes"
            )
        result.append(
            {
                "bytes_read": bytes_read,
                "client_instance": client_instance,
                "line": line_number,
                "message_size": message_size,
                "schema": "icefarm-h3-client-rejection-v1",
                "unread_bytes": unread,
            }
        )
    if not result:
        raise MutantError("old client has no authenticated unread 12-byte rejection")
    return result
