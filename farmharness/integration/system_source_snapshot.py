"""Authenticated system-source snapshot contracts and derived paths."""

from __future__ import annotations

import hashlib
from pathlib import Path, PurePosixPath
from typing import Any, Mapping


ROOTS = ("/usr/include", "/usr/lib/gcc", "/usr/local/include")
ENUMERATION_SCHEMA = "icefarm-system-source-manifest-v1"
SNAPSHOT_SCHEMA = "icefarm-system-source-snapshot-v1"
SYMLINK_POLICY = "no-directory-follow;regular-file-symlinks-regularized"
COMPRESSION = {"checksum": True, "codec": "zstd", "level": 19, "long": 31, "threads": 8}


class SystemSourceSnapshotError(ValueError):
    """Snapshot authority, archive, or materialization is invalid."""


def derived_root(scratch_root: str | Path, manifest_sha256: str) -> PurePosixPath:
    if not isinstance(manifest_sha256, str) or len(manifest_sha256) != 64:
        raise SystemSourceSnapshotError("snapshot manifest digest is invalid")
    if any(character not in "0123456789abcdef" for character in manifest_sha256):
        raise SystemSourceSnapshotError("snapshot manifest digest is invalid")
    root = PurePosixPath(str(scratch_root)) / "icefarm" / "system-source-snapshots" / manifest_sha256
    if not root.is_absolute():
        raise SystemSourceSnapshotError("snapshot scratch root must be absolute")
    return root


def derived_mounts(scratch_root: str | Path, manifest_sha256: str) -> dict[str, str]:
    root = derived_root(scratch_root, manifest_sha256)
    return {destination: str(root / destination.lstrip("/")) for destination in ROOTS}


def validate_snapshot_authority(snapshot: Mapping[str, Any]) -> None:
    if snapshot.get("schema") != SNAPSHOT_SCHEMA or snapshot.get("source") != "runtime":
        raise SystemSourceSnapshotError("unsupported system-source snapshot schema/source")
    reference = snapshot.get("source_runtime_reference")
    if not isinstance(reference, str) or not reference or any(ord(character) < 32 for character in reference):
        raise SystemSourceSnapshotError("system-source runtime reference is invalid")
    enumeration = snapshot.get("enumeration")
    if (
        not isinstance(enumeration, Mapping)
        or enumeration.get("schema") != ENUMERATION_SCHEMA
        or enumeration.get("roots") != list(ROOTS)
        or enumeration.get("symlink_policy") != SYMLINK_POLICY
    ):
        raise SystemSourceSnapshotError("system-source enumeration authority is invalid")
    archive = snapshot.get("archive")
    if not isinstance(archive, Mapping) or archive.get("compression") != COMPRESSION:
        raise SystemSourceSnapshotError("system-source archive compression is not pinned")
    path = archive.get("path")
    if (
        not isinstance(path, str)
        or not path.startswith("/")
        or ".." in PurePosixPath(path).parts
        or any(ord(character) < 32 or ord(character) == 127 for character in path)
    ):
        raise SystemSourceSnapshotError("system-source archive path is unsafe")
    for key in ("sha256",):
        value = archive.get(key)
        if not isinstance(value, str) or len(value) != 64 or any(
            character not in "0123456789abcdef" for character in value
        ):
            raise SystemSourceSnapshotError(f"system-source archive {key} is invalid")
    for key in ("archive_bytes", "unpacked_bytes"):
        value = archive.get(key)
        if isinstance(value, bool) or not isinstance(value, int) or value < 1:
            raise SystemSourceSnapshotError(f"system-source archive {key} is invalid")
    digest = snapshot.get("manifest_sha256")
    if not isinstance(digest, str) or len(digest) != 64 or any(
        character not in "0123456789abcdef" for character in digest
    ):
        raise SystemSourceSnapshotError("system-source manifest digest is invalid")
    count = snapshot.get("file_count")
    if isinstance(count, bool) or not isinstance(count, int) or count < 1:
        raise SystemSourceSnapshotError("system-source manifest file count is invalid")


def verify_local_archive(snapshot: Mapping[str, Any]) -> dict[str, Any]:
    """Verify the pinned local archive before any sync or container start."""

    validate_snapshot_authority(snapshot)
    archive = snapshot["archive"]
    path = Path(archive["path"])
    if not path.is_file() or path.is_symlink():
        raise SystemSourceSnapshotError(f"system-source archive is absent or symlink: {path}")
    if path.stat().st_size != archive["archive_bytes"]:
        raise SystemSourceSnapshotError("system-source archive byte count differs")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    observed = digest.hexdigest()
    if observed != archive["sha256"]:
        raise SystemSourceSnapshotError(
            f"system-source archive digest differs: expected {archive['sha256']}, got {observed}"
        )
    return {"archive_sha256": observed, "archive_bytes": archive["archive_bytes"], "path": str(path)}


def verified_materialization(
    snapshot: Mapping[str, Any],
    scratch_root: str | Path,
    observed_manifest_sha256: str,
    observed_file_count: int,
) -> dict[str, Any]:
    """Accept only a remote receipt matching the immutable snapshot authority."""

    validate_snapshot_authority(snapshot)
    if observed_manifest_sha256 != snapshot["manifest_sha256"]:
        raise SystemSourceSnapshotError("materialized system-source manifest differs")
    if observed_file_count != snapshot["file_count"]:
        raise SystemSourceSnapshotError("materialized system-source file count differs")
    return {
        "file_count": observed_file_count,
        "manifest_sha256": observed_manifest_sha256,
        "mounts": derived_mounts(scratch_root, observed_manifest_sha256),
        "root": str(derived_root(scratch_root, observed_manifest_sha256)),
    }
