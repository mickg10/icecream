"""Offline, fail-closed authority generation for the stable runtime source snapshot."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
from pathlib import Path
from typing import Any

try:
    from .farm_spec import FarmSpec
    from .images import ImageError, _image_identity
    from .remote import CommandResult, PlannedCommand, SubprocessTransport
    from .schema_validation import canonical_bytes
    from .system_source_snapshot import COMPRESSION, ENUMERATION_SCHEMA, ROOTS, SNAPSHOT_SCHEMA, SYMLINK_POLICY
except ImportError:
    from farm_spec import FarmSpec
    from images import ImageError, _image_identity
    from remote import CommandResult, PlannedCommand, SubprocessTransport
    from schema_validation import canonical_bytes
    from system_source_snapshot import COMPRESSION, ENUMERATION_SCHEMA, ROOTS, SNAPSHOT_SCHEMA, SYMLINK_POLICY


AUTHORITY_SCHEMA = "icefarm-system-source-authority-v1"
IDENTITY_SCHEMA = "icefarm-runtime-identity-v1"
SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
IMAGE_ID_RE = re.compile(r"sha256:[0-9a-f]{64}\Z")


class SystemSourceAuthorityError(ValueError):
    """The source snapshot cannot be authenticated or published safely."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _safe_new(path: Path, subject: str) -> None:
    if path.exists() or path.is_symlink():
        raise SystemSourceAuthorityError(f"{subject} already exists")
    for ancestor in (path, *path.parents):
        if ancestor.is_symlink():
            raise SystemSourceAuthorityError(f"{subject} has a symlink ancestor")


def _runtime_identity(farm: FarmSpec, source_root: Path, observed: dict[str, str] | None = None) -> dict[str, str]:
    runtime = farm.data["runtime_image"]
    if observed is not None:
        if observed != {
            "reference": runtime["reference"],
            "id": runtime["id"],
            "closure_sha256": runtime["closure_sha256"],
        }:
            raise SystemSourceAuthorityError("observed runtime identity differs from pinned farm runtime")
        return observed
    identity_path = source_root / ".icefarm-runtime-identity.json"
    if identity_path.is_symlink() or not identity_path.is_file():
        raise SystemSourceAuthorityError("source root lacks authenticated runtime identity")
    try:
        identity = json.loads(identity_path.read_text(encoding="utf-8"))
    except (OSError, ValueError, UnicodeError) as exc:
        raise SystemSourceAuthorityError("runtime identity is malformed") from exc
    if not isinstance(identity, dict) or set(identity) != {"schema", "reference", "id", "closure_sha256"}:
        raise SystemSourceAuthorityError("runtime identity has an invalid schema")
    if identity != {
        "schema": IDENTITY_SCHEMA,
        "reference": runtime["reference"],
        "id": runtime["id"],
        "closure_sha256": runtime["closure_sha256"],
    }:
        raise SystemSourceAuthorityError("source runtime identity differs from pinned farm runtime")
    if IMAGE_ID_RE.fullmatch(identity["id"]) is None or SHA256_RE.fullmatch(identity["closure_sha256"]) is None:
        raise SystemSourceAuthorityError("runtime identity digest is malformed")
    return {key: identity[key] for key in ("reference", "id", "closure_sha256")}


def _copy_regular(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as source_stream, destination.open("wb") as destination_stream:
        for block in iter(lambda: source_stream.read(1024 * 1024), b""):
            destination_stream.write(block)
    destination.chmod(0o644)


def _stage_source(source_root: Path, stage: Path) -> tuple[list[str], int]:
    rows: list[str] = []
    unpacked_bytes = 0
    for root_name in ROOTS:
        source = source_root / root_name.lstrip("/")
        if source.is_symlink() or not source.is_dir():
            raise SystemSourceAuthorityError(f"source root is not a real directory: {root_name}")
        target = stage / root_name.lstrip("/")
        target.mkdir(parents=True, exist_ok=True, mode=0o755)
        for directory, dirs, files in os.walk(source, topdown=True, followlinks=False):
            directory_path = Path(directory)
            dirs[:] = sorted(dirs)
            files[:] = sorted(files)
            for name in dirs:
                item = directory_path / name
                if item.is_symlink():
                    raise SystemSourceAuthorityError(f"directory symlink is forbidden: {item}")
                if not item.is_dir():
                    raise SystemSourceAuthorityError(f"non-directory source member: {item}")
                (stage / item.relative_to(source_root)).mkdir(parents=True, exist_ok=True, mode=0o755)
            for name in files:
                item = directory_path / name
                mode = item.lstat().st_mode
                relative = item.relative_to(source_root).as_posix()
                output = stage / relative
                if stat.S_ISLNK(mode):
                    resolved = item.resolve()
                    if not resolved.is_file() or not resolved.is_relative_to(source_root):
                        raise SystemSourceAuthorityError(f"file symlink is unsafe: {item}")
                    _copy_regular(resolved, output)
                elif stat.S_ISREG(mode):
                    _copy_regular(item, output)
                else:
                    raise SystemSourceAuthorityError(f"special source member is forbidden: {item}")
                size = output.stat().st_size
                unpacked_bytes += size
                rows.append("/" + relative + "\t" + _sha256(output) + "\n")
    rows.sort()
    return rows, unpacked_bytes


def _publish_bytes(path: Path, payload: bytes, subject: str) -> None:
    _safe_new(path, subject)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    _safe_new(temporary, subject + " temporary")
    try:
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, path)
        temporary.unlink()
        directory_fd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise


def capture_system_source_snapshot(
    farm: FarmSpec,
    *,
    source_root: Path,
    output: Path,
    archive_dir: Path | None = None,
    observed_identity: dict[str, str] | None = None,
) -> dict[str, Any]:
    """Generate one authenticated snapshot from a verified runtime extraction.

    The extraction must carry ``.icefarm-runtime-identity.json`` matching the
    farm's exact pinned runtime reference, native ID, and portable closure.
    """

    if source_root.is_symlink() or not source_root.is_dir():
        raise SystemSourceAuthorityError("source root is not a directory")
    source_root = source_root.resolve()
    identity = _runtime_identity(farm, source_root, observed_identity)
    # ICEFARM_TMPDIR may intentionally be a stable symlink such as /tmp/i.
    # Canonicalize that trusted boundary and every publication target first;
    # descendant symlinks that escape it are still refused by relative_to.
    output = output.resolve(strict=False)
    archive_dir = (archive_dir or output.parent).resolve(strict=False)
    configured_tmp = os.environ.get("ICEFARM_TMPDIR")
    if configured_tmp is not None:
        tmp_boundary = Path(configured_tmp).resolve(strict=False)
        for candidate, subject in ((output, "snapshot receipt"), (archive_dir, "snapshot archive")):
            try:
                candidate.relative_to(tmp_boundary)
            except ValueError as exc:
                raise SystemSourceAuthorityError(
                    f"{subject} must be under ICEFARM_TMPDIR"
                ) from exc
    _safe_new(output, "snapshot receipt")
    archive_dir.mkdir(parents=True, exist_ok=True)
    for ancestor in (archive_dir, *archive_dir.parents):
        if ancestor.is_symlink():
            raise SystemSourceAuthorityError("archive directory has a symlink ancestor")
    temp_root = Path(os.environ.get("ICEFARM_TMPDIR", str(output.parent))).resolve(
        strict=False
    )
    temp_root.mkdir(parents=True, exist_ok=True)
    stage = temp_root / f".system-source-stage-{os.getpid()}"
    if stage.exists() or stage.is_symlink():
        raise SystemSourceAuthorityError("staging path already exists")
    archive_temp: Path | None = None
    archive_path: Path | None = None
    archive_published = False
    receipt_published = False
    try:
        stage.mkdir(mode=0o700)
        rows, unpacked_bytes = _stage_source(source_root, stage)
        manifest_payload = "".join(rows).encode("utf-8")
        manifest_sha256 = hashlib.sha256(manifest_payload).hexdigest()
        archive_name = f"system-source-{manifest_sha256}.tar.zst"
        archive_temp = archive_dir / ("." + archive_name + ".tmp")
        archive_path = archive_dir / archive_name
        _publish_bytes(stage / "MANIFEST.sha256", manifest_payload, "snapshot manifest")
        _safe_new(archive_temp, "snapshot archive temporary")
        tar = subprocess.Popen(
            ["tar", "--sort=name", "--mtime=@0", "--owner=0", "--group=0", "--numeric-owner", "-cf", "-", *[item.lstrip("/") for item in ROOTS]],
            cwd=stage,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if tar.stdout is None:
            raise SystemSourceAuthorityError("cannot capture tar stream")
        with archive_temp.open("wb") as destination:
            zstd = subprocess.Popen(
                ["zstd", "-q", "-19", "--long=31", "--threads=8", "--check", "-c"],
                stdin=tar.stdout,
                stdout=destination,
                stderr=subprocess.PIPE,
            )
            tar.stdout.close()
            zstd_stderr = zstd.communicate()[1]
            tar_stderr = tar.communicate()[1]
            if tar.returncode != 0 or zstd.returncode != 0:
                raise SystemSourceAuthorityError(
                    f"snapshot archive pipeline failed: tar={tar.returncode}, zstd={zstd.returncode}: "
                    f"{(tar_stderr + zstd_stderr)[-1000:].decode(errors='replace')}"
                )
        os.link(archive_temp, archive_path)
        archive_published = True
        archive_temp.unlink()
        archive_bytes = archive_path.stat().st_size
        archive_sha256 = _sha256(archive_path)
        receipt = {
            "schema": SNAPSHOT_SCHEMA,
            "source": "runtime",
            "source_runtime_reference": identity["reference"],
            "source_runtime_id": identity["id"],
            "source_runtime_closure_sha256": identity["closure_sha256"],
            "manifest_sha256": manifest_sha256,
            "file_count": len(rows),
            "enumeration": {"schema": ENUMERATION_SCHEMA, "roots": list(ROOTS), "symlink_policy": SYMLINK_POLICY},
            "archive": {"path": str(archive_path), "sha256": archive_sha256, "archive_bytes": archive_bytes, "unpacked_bytes": unpacked_bytes, "compression": COMPRESSION},
        }
        _publish_bytes(output, canonical_bytes(receipt), "snapshot receipt")
        receipt_published = True
        return receipt
    finally:
        if archive_temp is not None:
            archive_temp.unlink(missing_ok=True)
        if archive_published and not receipt_published and archive_path is not None:
            archive_path.unlink(missing_ok=True)
        shutil.rmtree(stage, ignore_errors=True)


def runtime_identity_command(farm: FarmSpec) -> PlannedCommand:
    """Return the fakeable local Docker identity probe for the pinned runtime."""

    return PlannedCommand(0, "authority.system-source-runtime-inspect", "local", None, "local-docker", 60, ("docker", "image", "inspect", "--format", "{{json .}}", farm.data["runtime_image"]["reference"]))


SYSTEM_SOURCE_EXTRACT_SCRIPT = r'''
import os, pathlib, shutil, stat

destination = pathlib.Path("/out/root")
destination.mkdir(parents=True, exist_ok=False)
for root_name in ("/usr/include", "/usr/lib/gcc", "/usr/local/include"):
    source = pathlib.Path(root_name)
    if source.is_symlink() or not source.is_dir():
        raise SystemExit("runtime source root is not a real directory")
    target = destination / root_name.lstrip("/")
    target.mkdir(parents=True, exist_ok=True)
    for directory, dirs, files in os.walk(source, topdown=True, followlinks=False):
        directory_path = pathlib.Path(directory)
        dirs[:] = sorted(dirs)
        files[:] = sorted(files)
        for name in dirs:
            item = directory_path / name
            if item.is_symlink():
                raise SystemExit("directory symlink is forbidden")
            (destination / item.relative_to("/")).mkdir(parents=True, exist_ok=True)
        for name in files:
            item = directory_path / name
            output = destination / item.relative_to("/")
            output.parent.mkdir(parents=True, exist_ok=True)
            mode = item.lstat().st_mode
            if stat.S_ISLNK(mode):
                resolved = item.resolve()
                if not resolved.is_file() or not resolved.is_relative_to(pathlib.Path("/")):
                    raise SystemExit("unsafe runtime file symlink")
                source_file = resolved
            elif stat.S_ISREG(mode):
                source_file = item
            else:
                raise SystemExit("special runtime source member is forbidden")
            with source_file.open("rb") as source_stream, output.open("wb") as destination_stream:
                shutil.copyfileobj(source_stream, destination_stream, length=1024 * 1024)
            output.chmod(0o644)
'''.strip()


def _runtime_extract_command(farm: FarmSpec, native_id: str, stage: Path, timeout_s: int) -> PlannedCommand:
    return PlannedCommand(
        1,
        "authority.system-source-extract",
        "local",
        None,
        "local-docker",
        timeout_s,
        (
            "docker", "run", "--rm", "--pull=never", "--network", "none",
            "--cap-drop=ALL", "--security-opt=no-new-privileges", "--read-only",
            "--user", f"{os.getuid()}:{os.getgid()}", "--env", "PYTHONDONTWRITEBYTECODE=1",
            "--label", "icefarm.probe=system-source-authority-v1",
            "--name", f"icefarm-system-source-capture-{os.getpid()}",
            "--mount", f"type=bind,src={stage},dst=/out",
            "--entrypoint", "python3", native_id, "-c", SYSTEM_SOURCE_EXTRACT_SCRIPT,
        ),
    )


def _runtime_cleanup_commands(name: str, timeout_s: int) -> tuple[PlannedCommand, PlannedCommand]:
    label = "icefarm.probe=system-source-authority-v1"
    return (
        PlannedCommand(
            2, "authority.system-source-cleanup-list", "local", None, "local-docker", timeout_s,
            ("docker", "ps", "-aq", "--no-trunc", "--filter", f"name=^/{name}$", "--filter", f"label={label}"),
        ),
        PlannedCommand(
            3, "authority.system-source-cleanup-remove", "local", None, "local-docker", timeout_s,
            ("docker", "rm", "--force", "--volumes"),
        ),
    )


def validate_runtime_identity_result(farm: FarmSpec, result: CommandResult) -> dict[str, str]:
    if result.returncode != 0:
        raise SystemSourceAuthorityError("runtime image identity probe failed")
    try:
        observed = _image_identity(result, "runtime image identity probe")
    except (ImageError, ValueError, KeyError, TypeError) as exc:
        raise SystemSourceAuthorityError("runtime image identity probe returned malformed JSON") from exc
    expected = farm.data["runtime_image"]
    if observed.native_id != expected["id"] or observed.closure_sha256 != expected["closure_sha256"]:
        raise SystemSourceAuthorityError("runtime image identity differs from pinned farm runtime")
    return {
        "reference": expected["reference"],
        "id": observed.native_id,
        "closure_sha256": observed.closure_sha256,
    }


def capture_system_source_from_runtime(
    farm: FarmSpec,
    *,
    output: Path,
    archive_dir: Path | None = None,
    recorder=None,
    timeout_s: int = 120,
) -> dict[str, Any]:
    """Inspect and extract only the authenticated native runtime image."""

    if not 1 <= timeout_s <= 600:
        raise SystemSourceAuthorityError("capture timeout must be between 1 and 600 seconds")
    output = output.resolve(strict=False)
    archive_dir = (archive_dir or output.parent).resolve(strict=False)
    _safe_new(output, "snapshot receipt")
    if archive_dir.exists() and not archive_dir.is_dir():
        raise SystemSourceAuthorityError("snapshot archive path is not a directory")
    for ancestor in (archive_dir, *archive_dir.parents):
        if ancestor.is_symlink():
            raise SystemSourceAuthorityError("snapshot archive directory has a symlink ancestor")
    configured_tmp = os.environ.get("ICEFARM_TMPDIR")
    if configured_tmp is not None:
        boundary = Path(configured_tmp).resolve(strict=False)
        for candidate, subject in ((output, "snapshot receipt"), (archive_dir, "snapshot archive")):
            try:
                candidate.relative_to(boundary)
            except ValueError as exc:
                raise SystemSourceAuthorityError(f"{subject} must be under ICEFARM_TMPDIR") from exc
    transport = recorder or SubprocessTransport()
    identity_result = transport.invoke(runtime_identity_command(farm))
    identity = validate_runtime_identity_result(farm, identity_result)
    base = Path(os.environ.get("ICEFARM_TMPDIR", str(output.parent))).resolve(
        strict=False
    )
    base.mkdir(parents=True, exist_ok=True)
    stage = base / f".system-source-capture-{os.getpid()}"
    if stage.exists() or stage.is_symlink():
        raise SystemSourceAuthorityError("runtime extraction staging path already exists")
    stage.mkdir(mode=0o700)
    container_name = f"icefarm-system-source-capture-{os.getpid()}"
    cleanup_error: BaseException | None = None
    primary_error: BaseException | None = None
    try:
        extraction_command = _runtime_extract_command(farm, identity["id"], stage, timeout_s)
        extraction = transport.invoke(extraction_command)
        if extraction.returncode != 0:
            raise SystemSourceAuthorityError(
                f"runtime source extraction failed rc={extraction.returncode}: {extraction.stderr.strip()}"
            )
        source_root = stage / "root"
        receipt = capture_system_source_snapshot(
            farm,
            source_root=source_root,
            output=output,
            archive_dir=archive_dir,
            observed_identity=identity,
        )
        return receipt
    except BaseException as exc:
        primary_error = exc
        raise
    finally:
        cleanup_list, cleanup_remove = _runtime_cleanup_commands(container_name, timeout_s)
        try:
            listed = transport.invoke(cleanup_list)
            if listed.returncode != 0:
                raise SystemSourceAuthorityError(
                    f"runtime extraction cleanup listing failed rc={listed.returncode}: {listed.stderr.strip()}"
                )
            ids = [line.strip() for line in listed.stdout.splitlines() if line.strip()]
            if any(not re.fullmatch(r"[0-9a-f]{64}", item) for item in ids):
                raise SystemSourceAuthorityError("runtime extraction cleanup returned unsafe container IDs")
            if ids:
                removed = transport.invoke(
                    PlannedCommand(
                        cleanup_remove.sequence, cleanup_remove.phase, cleanup_remove.host,
                        cleanup_remove.instance, cleanup_remove.transport, cleanup_remove.timeout_s,
                        cleanup_remove.argv + tuple(ids),
                    )
                )
                if removed.returncode != 0:
                    raise SystemSourceAuthorityError(
                        f"runtime extraction cleanup removal failed rc={removed.returncode}: {removed.stderr.strip()}"
                    )
        except BaseException as exc:
            cleanup_error = exc
        shutil.rmtree(stage, ignore_errors=True)
        if primary_error is None and cleanup_error is not None:
            raise cleanup_error
        if primary_error is not None and cleanup_error is not None:
            primary_error.add_note(f"runtime extraction cleanup failed: {cleanup_error}")
