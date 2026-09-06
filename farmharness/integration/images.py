"""Build immutable icecream images and distribute an authenticated runtime closure."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Iterable, Protocol

try:
    from .farm_spec import FarmSpec
    from .remote import (
        CommandResult,
        PlannedCommand,
        RemoteError,
        SubprocessTransport,
        ssh_argv,
    )
    from .schema_validation import canonical_bytes
except ImportError:  # Direct execution from this directory.
    from farm_spec import FarmSpec
    from remote import (
        CommandResult,
        PlannedCommand,
        RemoteError,
        SubprocessTransport,
        ssh_argv,
    )
    from schema_validation import canonical_bytes


IMAGE_RECEIPT_SCHEMA = "icefarm-images-v1"
FOUNDATION_IMAGE_RECEIPT_SCHEMA = "icefarm-foundation-images-v1"
IMAGE_TRANSPORT_SCHEMA = "icefarm-docker-save-zstd-v1"
DOCKER_DIR = Path(__file__).with_name("docker")
IMAGE_TRANSPORT_HELPER = Path(__file__).with_name("image_transport.py")
DOCKER_FILES = (
    "Dockerfile.icecream",
    "entry-scheduler.sh",
    "entry-daemon.sh",
    "entry-client.sh",
)

VERIFY_REMOTE_ARCHIVE_SCRIPT = r"""
import hashlib, json, os, pathlib, sys

path = pathlib.Path(sys.argv[1])
expected_bytes = int(sys.argv[2])
expected_sha256 = sys.argv[3]
if path.is_symlink() or not path.is_file():
    raise SystemExit("transport archive is missing or a symlink")
observed_bytes = path.stat().st_size
digest = hashlib.sha256()
with path.open("rb") as handle:
    for block in iter(lambda: handle.read(1024 * 1024), b""):
        digest.update(block)
observed_sha256 = digest.hexdigest()
if observed_bytes != expected_bytes or observed_sha256 != expected_sha256:
    raise SystemExit(
        "transport archive mismatch: "
        + json.dumps(
            {
                "expected_bytes": expected_bytes,
                "expected_sha256": expected_sha256,
                "observed_bytes": observed_bytes,
                "observed_sha256": observed_sha256,
            },
            sort_keys=True,
        )
    )
path.chmod(0o444)
directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
try:
    os.fsync(directory)
finally:
    os.close(directory)
print(
    json.dumps(
        {"bytes": observed_bytes, "path": str(path), "sha256": observed_sha256},
        sort_keys=True,
        separators=(",", ":"),
    )
)
""".strip()

LOAD_REMOTE_ARCHIVE_SCRIPT = r"""
import hashlib, pathlib, subprocess, sys, tempfile

path = pathlib.Path(sys.argv[1])
expected_bytes = int(sys.argv[2])
expected_sha256 = sys.argv[3]
if path.is_symlink() or not path.is_file() or path.stat().st_size != expected_bytes:
    raise SystemExit("transport archive changed before load")
digest = hashlib.sha256()
with path.open("rb") as handle:
    for block in iter(lambda: handle.read(1024 * 1024), b""):
        digest.update(block)
if digest.hexdigest() != expected_sha256:
    raise SystemExit("transport archive hash changed before load")
with tempfile.TemporaryFile() as zstd_error, tempfile.TemporaryFile() as docker_error:
    decoder = subprocess.Popen(
        ["zstd", "-q", "-d", "--long=31", "-c", str(path)],
        stdout=subprocess.PIPE,
        stderr=zstd_error,
    )
    loader = subprocess.Popen(
        ["docker", "image", "load"],
        stdin=decoder.stdout,
        stdout=subprocess.PIPE,
        stderr=docker_error,
    )
    if decoder.stdout is None:
        decoder.kill()
        loader.kill()
        raise SystemExit("cannot connect zstd decoder to docker load")
    decoder.stdout.close()
    docker_stdout, unused = loader.communicate()
    decoder_status = decoder.wait()
    if decoder_status != 0 or loader.returncode != 0:
        zstd_error.seek(0)
        docker_error.seek(0)
        sys.stderr.buffer.write(zstd_error.read()[-2000:] + docker_error.read()[-2000:])
        raise SystemExit(
            f"zstd/docker load pipeline failed: zstd={decoder_status}, "
            f"docker={loader.returncode}"
        )
    sys.stdout.buffer.write(docker_stdout)
""".strip()


class ImageError(RuntimeError):
    """An image was not built or distributed with its authenticated identity."""


class Recorder(Protocol):
    def invoke(self, command: PlannedCommand) -> CommandResult: ...


class TransportBinding(Protocol):
    label: str
    reference: str


@dataclass(frozen=True)
class ImageBinding:
    label: str
    commit: str
    archive_sha256: str
    expected_closure: str | None
    expected_id: str | None
    reference: str
    kind: str | None = None
    base_image: str | None = None
    base_commit: str | None = None
    base_archive_sha256: str | None = None
    patch_path: str | None = None
    patch_sha256: str | None = None
    recipe_sha256: str | None = None


@dataclass(frozen=True)
class ImageIdentity:
    native_id: str
    closure_sha256: str


DAEMON_ROLE_PATH = "/opt/icecream/sbin/iceccd"
DAEMON_ROLE_PROBE_LABEL = "icefarm.probe=daemon-role-sha256-v1"


def _probe_daemon_role_hash(
    binding: ImageBinding,
    recorder: Recorder,
    commands: CommandFactory,
    *,
    timeout_s: int,
) -> str:
    """Measure the mutant daemon binary in one bounded, isolated hub probe."""

    result = recorder.invoke(
        commands.make(
            phase="images.probe-daemon-role",
            host="hub",
            transport="local-docker",
            timeout_s=timeout_s,
            argv=(
                "docker",
                "run",
                "--rm",
                "--pull=never",
                "--network",
                "none",
                "--cap-drop=ALL",
                "--security-opt=no-new-privileges",
                "--read-only",
                "--label",
                DAEMON_ROLE_PROBE_LABEL,
                "--entrypoint",
                "/usr/bin/sha256sum",
                binding.reference,
                DAEMON_ROLE_PATH,
            ),
        )
    )
    lines = result.stdout.splitlines()
    if result.returncode != 0 or len(lines) != 1:
        raise ImageError(
            f"daemon role probe for {binding.label} returned malformed output"
        )
    fields = lines[0].split()
    if (
        len(fields) != 2
        or re.fullmatch(r"[0-9a-f]{64}", fields[0]) is None
        or fields[1] != DAEMON_ROLE_PATH
    ):
        raise ImageError(f"daemon role probe for {binding.label} is not authenticated")
    return fields[0]


@dataclass(frozen=True)
class FoundationImageBinding:
    key: str
    kind: str
    label: str
    reference: str
    dockerfile: Path
    expected_closure: str
    expected_id: str


class RecordingTransport:
    """Keep an evidence-ready command list around the real bounded transport."""

    def __init__(self, delegate: Recorder | None = None) -> None:
        self.delegate = delegate or SubprocessTransport()
        self.commands: list[PlannedCommand] = []

    def invoke(self, command: PlannedCommand) -> CommandResult:
        self.commands.append(command)
        return self.delegate.invoke(command)


class CommandFactory:
    def __init__(self) -> None:
        self.sequence = 0

    def make(
        self,
        *,
        phase: str,
        host: str,
        instance: str | None = None,
        transport: str,
        timeout_s: int,
        argv: Iterable[str],
    ) -> PlannedCommand:
        command = PlannedCommand(
            sequence=self.sequence,
            phase=phase,
            host=host,
            instance=instance,
            transport=transport,
            timeout_s=timeout_s,
            argv=tuple(argv),
        )
        self.sequence += 1
        return command


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _runtime_reference(farm: FarmSpec, label: str) -> str:
    registry = farm.data["registry"]
    return f"{registry['host']}:{registry['port']}/icefarm/icecream:{label}"


def image_bindings(farm: FarmSpec, labels: Iterable[str]) -> list[ImageBinding]:
    requested = list(labels)
    if not requested:
        raise ImageError("at least one image label is required")
    if len(requested) != len(set(requested)):
        raise ImageError("duplicate image labels are forbidden")
    result: list[ImageBinding] = []
    authority = farm.data["authority"]["images"]
    for label in requested:
        entry = authority.get(label)
        if not isinstance(entry, dict):
            raise ImageError(
                f"image label {label!r} is absent from the immutable authority map"
            )
        result.append(
            ImageBinding(
                label=label,
                commit=entry["commit"],
                archive_sha256=entry["archive_sha256"],
                expected_closure=entry.get("closure_sha256"),
                expected_id=entry.get("id"),
                reference=_runtime_reference(farm, label),
                kind=entry.get("kind"),
                base_image=entry.get("base_image"),
                base_commit=entry.get("base_commit"),
                base_archive_sha256=entry.get("base_archive_sha256"),
                patch_path=entry.get("patch_path"),
                patch_sha256=entry.get("patch_sha256"),
                recipe_sha256=entry.get("recipe_sha256"),
            )
        )
    return result


def create_source_archive(repo: Path, binding: ImageBinding, destination: Path) -> None:
    """Write exactly ``git archive <binding.commit>`` and verify its authority hash."""

    repo = repo.resolve()
    destination = destination.resolve()
    source_commit = binding.base_commit or binding.commit
    source_archive_sha256 = binding.base_archive_sha256 or binding.archive_sha256
    try:
        subprocess.run(
            ["git", "cat-file", "-e", f"{source_commit}^{{commit}}"],
            cwd=repo,
            check=True,
            capture_output=True,
            shell=False,
        )
        with destination.open("wb") as output:
            subprocess.run(
                ["git", "archive", "--format=tar", source_commit],
                cwd=repo,
                check=True,
                stdout=output,
                stderr=subprocess.PIPE,
                shell=False,
            )
    except (OSError, subprocess.CalledProcessError) as exc:
        destination.unlink(missing_ok=True)
        raise ImageError(
            f"cannot archive immutable commit {source_commit}: {exc}"
        ) from exc
    observed = _sha256(destination)
    if observed != source_archive_sha256:
        destination.unlink(missing_ok=True)
        raise ImageError(
            f"source archive mismatch for {binding.label}: "
            f"expected {source_archive_sha256}, got {observed}"
        )


def prepare_build_context(repo: Path, binding: ImageBinding, context: Path) -> None:
    context.mkdir(parents=True, exist_ok=False)
    create_source_archive(repo, binding, context / "source.tar")
    for name in DOCKER_FILES:
        source = DOCKER_DIR / name
        if not source.is_file():
            raise ImageError(f"image support file is missing: {source}")
        shutil.copy2(source, context / name)
    if binding.kind in ("scheduler-mutant", "daemon-mutant"):
        if not binding.patch_path or not binding.patch_sha256:
            raise ImageError(f"{binding.label}: mutant patch authority is incomplete")
        patch = (Path(__file__).parent / binding.patch_path).resolve()
        if not patch.is_file() or patch.is_symlink() or _sha256(patch) != binding.patch_sha256:
            raise ImageError(f"{binding.label}: mutant patch is not hash-authenticated")
        patch_name = (
            "scheduler-tail.patch"
            if binding.kind == "scheduler-mutant"
            else "daemon-mutant.patch"
        )
        dockerfile_name = "Dockerfile.scheduler-mutant" if binding.kind == "scheduler-mutant" else "Dockerfile.daemon-mutant"
        shutil.copy2(patch, context / patch_name)
        shutil.copy2(DOCKER_DIR / dockerfile_name, context / dockerfile_name)


def _image_identity(result: CommandResult, subject: str) -> ImageIdentity:
    try:
        value = json.loads(result.stdout.strip())
    except json.JSONDecodeError as exc:
        raise ImageError(f"{subject} did not return Docker inspect JSON") from exc
    if not isinstance(value, dict):
        raise ImageError(f"{subject} did not return one Docker image object")
    native_id = value.get("Id")
    if (
        not isinstance(native_id, str)
        or not native_id.startswith("sha256:")
        or len(native_id) != 71
    ):
        raise ImageError(f"{subject} did not return one Docker native image ID")
    try:
        closure = {
            key: value[key]
            for key in ("Architecture", "Os", "Created", "Config", "RootFS")
        }
    except KeyError as exc:
        raise ImageError(
            f"{subject} lacks portable closure field {exc.args[0]}"
        ) from exc
    return ImageIdentity(
        native_id=native_id,
        closure_sha256=hashlib.sha256(canonical_bytes(closure)).hexdigest(),
    )


def _host_docker_argv(
    farm: FarmSpec, host_name: str, args: Iterable[str]
) -> tuple[str, ...]:
    host = farm.hosts[host_name]
    context = host.get("docker_context")
    if context:
        return ("docker", "--context", context, *tuple(args))
    return ("docker", "--host", f"ssh://{host['ssh']}", *tuple(args))


def build_image(
    farm: FarmSpec,
    binding: ImageBinding,
    context: Path,
    recorder: Recorder,
    commands: CommandFactory,
    *,
    timeout_s: int,
) -> ImageIdentity:
    recorder.invoke(
        commands.make(
            phase="images.build",
            host="hub",
            transport="local-docker",
            timeout_s=timeout_s,
            argv=(
                "docker",
                "build",
                "--pull=false",
                "--progress=plain",
                "--build-arg",
                f"SOURCE_COMMIT={binding.commit}",
                "--build-arg",
                f"SOURCE_ARCHIVE_SHA256={binding.base_archive_sha256 or binding.archive_sha256}",
                "--build-arg",
                f"MUTANT_RECIPE_SHA256={binding.recipe_sha256 or ''}",
                "--build-arg",
                f"MUTANT_PATCH_SHA256={binding.patch_sha256 or ''}",
                "--tag",
                binding.reference,
                "--file",
                str(
                    context
                    / (
                        "Dockerfile.scheduler-mutant"
                        if binding.kind == "scheduler-mutant"
                        else "Dockerfile.daemon-mutant"
                        if binding.kind == "daemon-mutant"
                        else "Dockerfile.icecream"
                    )
                ),
                str(context),
            ),
        )
    )
    observed = _image_identity(
        recorder.invoke(
            commands.make(
                phase="images.inspect-hub",
                host="hub",
                transport="local-docker",
                timeout_s=timeout_s,
                argv=(
                    "docker",
                    "image",
                    "inspect",
                    "--format",
                    "{{json .}}",
                    binding.reference,
                ),
            )
        ),
        f"hub image {binding.label}",
    )
    if binding.expected_id is not None and observed.native_id != binding.expected_id:
        raise ImageError(
            f"hub image id mismatch for {binding.label}: "
            f"expected {binding.expected_id}, got {observed.native_id}"
        )
    if (
        binding.expected_closure is not None
        and observed.closure_sha256 != binding.expected_closure
    ):
        raise ImageError(
            f"hub image closure mismatch for {binding.label}: "
            f"expected {binding.expected_closure}, got {observed.closure_sha256}"
        )
    return observed


def ensure_product_image(
    farm: FarmSpec,
    binding: ImageBinding,
    repo: Path,
    context: Path,
    recorder: Recorder,
    commands: CommandFactory,
    *,
    timeout_s: int,
) -> tuple[ImageIdentity, bool]:
    """Reuse an authority-bound hub image or build it from immutable source."""

    if binding.expected_closure is not None:
        try:
            current = _inspect_local_reference(
                binding.reference,
                phase="images.inspect-hub-cached",
                subject=f"cached hub image {binding.label}",
                recorder=recorder,
                commands=commands,
                timeout_s=timeout_s,
            )
        except RemoteError:
            current = None
        if (
            current is not None
            and current.closure_sha256 == binding.expected_closure
            and (
                binding.expected_id is None
                or current.native_id == binding.expected_id
            )
        ):
            return current, False

    prepare_build_context(repo, binding, context)
    return (
        build_image(
            farm,
            binding,
            context,
            recorder,
            commands,
            timeout_s=timeout_s,
        ),
        True,
    )


def _inspect_host(
    farm: FarmSpec,
    binding: TransportBinding,
    host_name: str,
    recorder: Recorder,
    commands: CommandFactory,
    timeout_s: int,
    *,
    reference: str | None = None,
    phase: str = "images.inspect-host",
) -> ImageIdentity:
    selected_reference = reference or binding.reference
    return _image_identity(
        recorder.invoke(
            commands.make(
                phase=phase,
                host=host_name,
                transport="docker-context"
                if farm.hosts[host_name].get("docker_context")
                else "docker-ssh",
                timeout_s=timeout_s,
                argv=_host_docker_argv(
                    farm,
                    host_name,
                    ("image", "inspect", "--format", "{{json .}}", selected_reference),
                ),
            )
        ),
        f"host {host_name} image {selected_reference}",
    )


def _transport_reference(closure_sha256: str) -> str:
    return f"icefarm-transport:{closure_sha256}"


def _transport_metadata_path(image_archive: Path) -> Path:
    return image_archive.with_name(image_archive.name + ".json")


def _read_transport_metadata(
    image_archive: Path,
    *,
    closure_sha256: str,
    transport_reference: str,
) -> dict[str, Any] | None:
    metadata_path = _transport_metadata_path(image_archive)
    if not image_archive.exists() and not metadata_path.exists():
        return None
    if not image_archive.is_file() or not metadata_path.is_file():
        raise ImageError(f"incomplete Docker transport cache at {image_archive}")
    try:
        metadata = json.loads(metadata_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise ImageError(
            f"invalid Docker transport metadata {metadata_path}: {exc}"
        ) from exc
    expected = {
        "closure_sha256": closure_sha256,
        "format": "docker-save-tar-zstd",
        "reference": transport_reference,
        "schema": IMAGE_TRANSPORT_SCHEMA,
    }
    if not isinstance(metadata, dict) or any(
        metadata.get(key) != value for key, value in expected.items()
    ):
        raise ImageError(
            f"Docker transport metadata does not bind {transport_reference}"
        )
    artifact = metadata.get("artifact")
    if not isinstance(artifact, dict):
        raise ImageError("Docker transport metadata lacks artifact identity")
    if (
        not isinstance(metadata.get("helper_sha256"), str)
        or len(metadata["helper_sha256"]) != 64
    ):
        raise ImageError("Docker transport metadata lacks its helper identity")
    observed_bytes = image_archive.stat().st_size
    observed_sha256 = _sha256(image_archive)
    if (
        artifact.get("bytes") != observed_bytes
        or artifact.get("sha256") != observed_sha256
    ):
        raise ImageError(f"Docker transport archive changed: {image_archive}")
    if (
        metadata.get("zstd", {}).get("level") != 19
        or metadata.get("zstd", {}).get("long") != 31
    ):
        raise ImageError("Docker transport compression is not zstd -19 --long=31")
    if metadata.get("zstd", {}).get("threads") != 8:
        raise ImageError(
            "Docker transport compression does not use the fixed eight threads"
        )
    return metadata


def _save_once(
    binding: TransportBinding,
    hub_identity: ImageIdentity,
    image_archive: Path,
    recorder: Recorder,
    commands: CommandFactory,
    timeout_s: int,
) -> dict[str, Any]:
    transport_reference = _transport_reference(hub_identity.closure_sha256)
    cached = _read_transport_metadata(
        image_archive,
        closure_sha256=hub_identity.closure_sha256,
        transport_reference=transport_reference,
    )
    if cached is not None:
        return cached
    image_archive.parent.mkdir(parents=True, exist_ok=True)
    recorder.invoke(
        commands.make(
            phase="images.transport-tag",
            host="hub",
            transport="local-docker",
            timeout_s=timeout_s,
            argv=("docker", "image", "tag", binding.reference, transport_reference),
        )
    )
    tagged_identity = _image_identity(
        recorder.invoke(
            commands.make(
                phase="images.inspect-transport-hub",
                host="hub",
                transport="local-docker",
                timeout_s=timeout_s,
                argv=(
                    "docker",
                    "image",
                    "inspect",
                    "--format",
                    "{{json .}}",
                    transport_reference,
                ),
            )
        ),
        f"hub transport image {transport_reference}",
    )
    if tagged_identity.closure_sha256 != hub_identity.closure_sha256:
        raise ImageError(
            f"transport tag {transport_reference} changed the image closure"
        )
    result = recorder.invoke(
        commands.make(
            phase="images.save-compressed",
            host="hub",
            transport="local-process",
            timeout_s=timeout_s,
            argv=(
                sys.executable,
                str(IMAGE_TRANSPORT_HELPER),
                "save",
                "--reference",
                transport_reference,
                "--output",
                str(image_archive),
            ),
        )
    )
    try:
        saved = json.loads(result.stdout.strip())
    except json.JSONDecodeError as exc:
        raise ImageError("compressed Docker save returned invalid JSON") from exc
    if not isinstance(saved, dict) or not image_archive.is_file():
        raise ImageError("compressed Docker save did not create its archive")
    zstd = saved.get("zstd")
    if (
        not isinstance(zstd, dict)
        or {key: zstd.get(key) for key in ("check", "level", "long", "threads")}
        != {"check": True, "level": 19, "long": 31, "threads": 8}
        or not isinstance(zstd.get("version"), str)
        or not zstd["version"]
    ):
        raise ImageError("compressed Docker save did not use the pinned zstd policy")
    compressed_bytes = image_archive.stat().st_size
    compressed_sha256 = _sha256(image_archive)
    if (
        saved.get("compressed_bytes") != compressed_bytes
        or saved.get("compressed_sha256") != compressed_sha256
        or not isinstance(saved.get("raw_bytes"), int)
        or saved["raw_bytes"] <= 0
        or not isinstance(saved.get("raw_sha256"), str)
        or len(saved["raw_sha256"]) != 64
    ):
        raise ImageError("compressed Docker save receipt does not match its archive")
    metadata = {
        "artifact": {"bytes": compressed_bytes, "sha256": compressed_sha256},
        "closure_sha256": hub_identity.closure_sha256,
        "format": "docker-save-tar-zstd",
        "helper_sha256": _sha256(IMAGE_TRANSPORT_HELPER),
        "raw_stream": {"bytes": saved["raw_bytes"], "sha256": saved["raw_sha256"]},
        "reference": transport_reference,
        "schema": IMAGE_TRANSPORT_SCHEMA,
        "zstd": zstd,
    }
    metadata_path = _transport_metadata_path(image_archive)
    temporary = metadata_path.with_name(metadata_path.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_bytes(canonical_bytes(metadata))
        os.replace(temporary, metadata_path)
    finally:
        temporary.unlink(missing_ok=True)
    return metadata


def _remote_transport_path(
    farm: FarmSpec, host_name: str, compressed_sha256: str
) -> PurePosixPath:
    return (
        PurePosixPath(farm.hosts[host_name]["scratch_root"])
        / "icefarm"
        / "image-transport"
        / f"{compressed_sha256}.docker.tar.zst"
    )


def _sync_transport_archive(
    farm: FarmSpec,
    host_name: str,
    image_archive: Path,
    metadata: dict[str, Any],
    recorder: Recorder,
    commands: CommandFactory,
    timeout_s: int,
) -> PurePosixPath:
    artifact = metadata["artifact"]
    remote_archive = _remote_transport_path(farm, host_name, artifact["sha256"])
    recorder.invoke(
        commands.make(
            phase="images.transport-mkdir",
            host=host_name,
            transport="ssh",
            timeout_s=timeout_s,
            argv=ssh_argv(
                farm,
                host_name,
                ("install", "-d", "-m", "0755", str(remote_archive.parent)),
            ),
        )
    )
    recorder.invoke(
        commands.make(
            phase="images.sync-compressed",
            host=host_name,
            transport="rsync-ssh",
            timeout_s=timeout_s,
            argv=(
                "rsync",
                "--archive",
                "--checksum",
                "--protect-args",
                "--partial-dir=.rsync-partial",
                "-e",
                "ssh -o BatchMode=yes -o ConnectTimeout=10 -o Compression=no",
                str(image_archive),
                f"{farm.hosts[host_name]['ssh']}:{remote_archive}",
            ),
        )
    )
    verified = recorder.invoke(
        commands.make(
            phase="images.verify-compressed",
            host=host_name,
            transport="ssh",
            timeout_s=timeout_s,
            argv=ssh_argv(
                farm,
                host_name,
                (
                    "python3",
                    "-c",
                    VERIFY_REMOTE_ARCHIVE_SCRIPT,
                    str(remote_archive),
                    str(artifact["bytes"]),
                    artifact["sha256"],
                ),
            ),
        )
    )
    try:
        observed = json.loads(verified.stdout.strip())
    except json.JSONDecodeError as exc:
        raise ImageError(
            f"host {host_name} returned invalid archive verification JSON"
        ) from exc
    if observed != {
        "bytes": artifact["bytes"],
        "path": str(remote_archive),
        "sha256": artifact["sha256"],
    }:
        raise ImageError(
            f"host {host_name} did not verify the compressed image archive"
        )
    return remote_archive


def _save_load_host(
    farm: FarmSpec,
    binding: TransportBinding,
    hub_identity: ImageIdentity,
    host_name: str,
    image_archive: Path,
    recorder: Recorder,
    commands: CommandFactory,
    timeout_s: int,
    *,
    metadata: dict[str, Any] | None = None,
) -> ImageIdentity:
    try:
        cached = _inspect_host(
            farm,
            binding,
            host_name,
            recorder,
            commands,
            timeout_s,
            phase="images.inspect-host-cached",
        )
    except RemoteError:
        cached = None
    if cached is not None and cached.closure_sha256 == hub_identity.closure_sha256:
        return cached
    if metadata is None:
        metadata = _save_once(
            binding,
            hub_identity,
            image_archive,
            recorder,
            commands,
            timeout_s,
        )
    remote_archive = _sync_transport_archive(
        farm,
        host_name,
        image_archive,
        metadata,
        recorder,
        commands,
        timeout_s,
    )
    artifact = metadata["artifact"]
    recorder.invoke(
        commands.make(
            phase="images.load-compressed",
            host=host_name,
            transport="ssh",
            timeout_s=timeout_s,
            argv=ssh_argv(
                farm,
                host_name,
                (
                    "python3",
                    "-c",
                    LOAD_REMOTE_ARCHIVE_SCRIPT,
                    str(remote_archive),
                    str(artifact["bytes"]),
                    artifact["sha256"],
                ),
            ),
        )
    )
    transport_reference = metadata["reference"]
    loaded = _inspect_host(
        farm,
        binding,
        host_name,
        recorder,
        commands,
        timeout_s,
        reference=transport_reference,
        phase="images.inspect-transport-host",
    )
    if loaded.closure_sha256 != hub_identity.closure_sha256:
        raise ImageError(
            f"host {host_name} transport image closure mismatch for {binding.label}: "
            f"expected {hub_identity.closure_sha256}, got {loaded.closure_sha256}"
        )
    recorder.invoke(
        commands.make(
            phase="images.tag-verified",
            host=host_name,
            transport=(
                "docker-context"
                if farm.hosts[host_name].get("docker_context")
                else "docker-ssh"
            ),
            timeout_s=timeout_s,
            argv=_host_docker_argv(
                farm,
                host_name,
                ("image", "tag", transport_reference, binding.reference),
            ),
        )
    )
    return _inspect_host(farm, binding, host_name, recorder, commands, timeout_s)


def _push_digest(
    binding: TransportBinding,
    recorder: Recorder,
    commands: CommandFactory,
    timeout_s: int,
) -> str:
    recorder.invoke(
        commands.make(
            phase="images.push",
            host="hub",
            transport="local-docker",
            timeout_s=timeout_s,
            argv=("docker", "image", "push", binding.reference),
        )
    )
    result = recorder.invoke(
        commands.make(
            phase="images.inspect-repodigests",
            host="hub",
            transport="local-docker",
            timeout_s=timeout_s,
            argv=(
                "docker",
                "image",
                "inspect",
                "--format",
                "{{json .RepoDigests}}",
                binding.reference,
            ),
        )
    )
    try:
        digests = json.loads(result.stdout.strip())
    except json.JSONDecodeError as exc:
        raise ImageError(
            f"registry push returned invalid RepoDigests for {binding.label}"
        ) from exc
    if not isinstance(digests, list):
        raise ImageError(f"registry push returned no RepoDigests for {binding.label}")
    repository = binding.reference.rsplit(":", 1)[0]
    matches = sorted(
        value
        for value in digests
        if isinstance(value, str) and value.startswith(repository + "@sha256:")
    )
    if not matches:
        raise ImageError(
            f"registry push returned no matching digest for {binding.label}"
        )
    return matches[0]


def _registry_host(
    farm: FarmSpec,
    binding: TransportBinding,
    digest_reference: str,
    host_name: str,
    recorder: Recorder,
    commands: CommandFactory,
    timeout_s: int,
) -> ImageIdentity:
    transport = (
        "docker-context"
        if farm.hosts[host_name].get("docker_context")
        else "docker-ssh"
    )
    recorder.invoke(
        commands.make(
            phase="images.pull",
            host=host_name,
            transport=transport,
            timeout_s=timeout_s,
            argv=_host_docker_argv(
                farm, host_name, ("image", "pull", digest_reference)
            ),
        )
    )
    recorder.invoke(
        commands.make(
            phase="images.tag",
            host=host_name,
            transport=transport,
            timeout_s=timeout_s,
            argv=_host_docker_argv(
                farm, host_name, ("image", "tag", digest_reference, binding.reference)
            ),
        )
    )
    return _inspect_host(farm, binding, host_name, recorder, commands, timeout_s)


def distribute_image(
    farm: FarmSpec,
    binding: TransportBinding,
    hub_identity: ImageIdentity,
    image_archive: Path,
    recorder: Recorder,
    commands: CommandFactory,
    *,
    timeout_s: int,
) -> dict[str, dict[str, Any]]:
    mode = farm.data["registry"]["mode"]
    digest_reference: str | None = None
    if mode in ("registry", "auto"):
        try:
            digest_reference = _push_digest(binding, recorder, commands, timeout_s)
        except (RemoteError, ImageError):
            if mode == "registry":
                raise

    result: dict[str, dict[str, Any]] = {}
    for host_name in farm.hosts:
        used = "save-load"
        observed: ImageIdentity
        if digest_reference is not None:
            try:
                observed = _registry_host(
                    farm,
                    binding,
                    digest_reference,
                    host_name,
                    recorder,
                    commands,
                    timeout_s,
                )
                used = "registry"
            except (RemoteError, ImageError):
                if mode == "registry":
                    raise
                observed = _save_load_host(
                    farm,
                    binding,
                    hub_identity,
                    host_name,
                    image_archive,
                    recorder,
                    commands,
                    timeout_s,
                )
        else:
            observed = _save_load_host(
                farm,
                binding,
                hub_identity,
                host_name,
                image_archive,
                recorder,
                commands,
                timeout_s,
            )
        if (
            observed.closure_sha256 != hub_identity.closure_sha256
            and used == "registry"
            and mode == "auto"
        ):
            observed = _save_load_host(
                farm,
                binding,
                hub_identity,
                host_name,
                image_archive,
                recorder,
                commands,
                timeout_s,
            )
            used = "save-load"
        if observed.closure_sha256 != hub_identity.closure_sha256:
            raise ImageError(
                f"host {host_name} image closure mismatch for {binding.label}: "
                f"expected {hub_identity.closure_sha256}, got {observed.closure_sha256}"
            )
        result[host_name] = {
            "closure_sha256": observed.closure_sha256,
            "id": observed.native_id,
            "mode": used,
        }
        if used == "save-load":
            transport_reference = _transport_reference(hub_identity.closure_sha256)
            metadata = _read_transport_metadata(
                image_archive,
                closure_sha256=hub_identity.closure_sha256,
                transport_reference=transport_reference,
            )
            if metadata is None:
                raise ImageError("save-load completed without transport metadata")
            result[host_name]["transport_archive"] = {
                **metadata["artifact"],
                "path": str(
                    _remote_transport_path(
                        farm,
                        host_name,
                        metadata["artifact"]["sha256"],
                    )
                ),
            }
    return result


def foundation_image_bindings(
    farm: FarmSpec,
    repo: Path,
    keys: Iterable[str] | None = None,
) -> list[FoundationImageBinding]:
    """Resolve the stable F/S image and four heterogeneous C images."""

    repo = repo.resolve()
    requested = (
        list(keys)
        if keys is not None
        else [
            "runtime",
            *sorted(farm.data["client_environments"]),
        ]
    )
    if not requested or len(requested) != len(set(requested)):
        raise ImageError("foundation image keys must be non-empty and unique")
    result: list[FoundationImageBinding] = []
    for key in requested:
        if key == "runtime":
            kind = "runtime"
            source = farm.data["runtime_image"]
        else:
            kind = "client-environment"
            source = farm.data["client_environments"].get(key)
            if not isinstance(source, dict):
                raise ImageError(f"unknown client environment {key!r}")
        dockerfile = (repo / source["dockerfile"]).resolve()
        if not dockerfile.is_relative_to(repo) or not dockerfile.is_file():
            raise ImageError(
                f"foundation Dockerfile escapes or is missing: {dockerfile}"
            )
        result.append(
            FoundationImageBinding(
                key=key,
                kind=kind,
                label=f"{kind}-{key}",
                reference=source["reference"],
                dockerfile=dockerfile,
                expected_closure=source["closure_sha256"],
                expected_id=source["id"],
            )
        )
    return result


def _inspect_local_reference(
    reference: str,
    *,
    phase: str,
    subject: str,
    recorder: Recorder,
    commands: CommandFactory,
    timeout_s: int,
) -> ImageIdentity:
    return _image_identity(
        recorder.invoke(
            commands.make(
                phase=phase,
                host="hub",
                transport="local-docker",
                timeout_s=timeout_s,
                argv=(
                    "docker",
                    "image",
                    "inspect",
                    "--format",
                    "{{json .}}",
                    reference,
                ),
            )
        ),
        subject,
    )


def ensure_foundation_image(
    binding: FoundationImageBinding,
    repo: Path,
    recorder: Recorder,
    commands: CommandFactory,
    *,
    timeout_s: int,
) -> tuple[ImageIdentity, bool]:
    """Use a closure-identical image or verify a rebuilt candidate before tagging."""

    try:
        current = _inspect_local_reference(
            binding.reference,
            phase="images.foundation-inspect-cached",
            subject=f"cached foundation image {binding.key}",
            recorder=recorder,
            commands=commands,
            timeout_s=timeout_s,
        )
    except RemoteError:
        current = None
    if current is not None and current.closure_sha256 == binding.expected_closure:
        return current, False

    dockerfile_sha256 = _sha256(binding.dockerfile)
    candidate = f"icefarm-foundation-candidate:{binding.key}-{dockerfile_sha256[:16]}"
    recorder.invoke(
        commands.make(
            phase="images.foundation-build",
            host="hub",
            transport="local-docker",
            timeout_s=timeout_s,
            argv=(
                "docker",
                "build",
                "--pull=false",
                "--progress=plain",
                "--tag",
                candidate,
                "--file",
                str(binding.dockerfile),
                str(repo.resolve()),
            ),
        )
    )
    observed = _inspect_local_reference(
        candidate,
        phase="images.foundation-inspect-candidate",
        subject=f"candidate foundation image {binding.key}",
        recorder=recorder,
        commands=commands,
        timeout_s=timeout_s,
    )
    if observed.closure_sha256 != binding.expected_closure:
        raise ImageError(
            f"foundation image {binding.key} differs from authority: "
            f"closure={observed.closure_sha256}"
        )
    recorder.invoke(
        commands.make(
            phase="images.foundation-tag-verified",
            host="hub",
            transport="local-docker",
            timeout_s=timeout_s,
            argv=("docker", "image", "tag", candidate, binding.reference),
        )
    )
    final = _inspect_local_reference(
        binding.reference,
        phase="images.foundation-inspect-final",
        subject=f"final foundation image {binding.key}",
        recorder=recorder,
        commands=commands,
        timeout_s=timeout_s,
    )
    if final != observed:
        raise ImageError(f"verified foundation tag changed identity for {binding.key}")
    return final, True


def foundation_targets_for_scenario(
    scenario_data: dict[str, Any],
) -> dict[str, list[str]]:
    """Return only the foundation images and hosts selected by one scenario."""

    targets: dict[str, set[str]] = {}
    for instance in scenario_data["instances"]:
        key = (
            "runtime"
            if instance["role"] in ("S", "F")
            else instance["client_environment"]
        )
        targets.setdefault(key, set()).add(instance["host"])
    return {key: sorted(hosts) for key, hosts in sorted(targets.items())}


def foundation_targets_for_farm(farm: FarmSpec) -> dict[str, list[str]]:
    """Place the stable runtime and every C environment on all capable hosts."""

    runtime_hosts = sorted(
        name
        for name, host in farm.hosts.items()
        if set(host["roles_allowed"]).intersection(("S", "F"))
    )
    client_hosts = sorted(
        name for name, host in farm.hosts.items() if "C" in host["roles_allowed"]
    )
    return {
        "runtime": runtime_hosts,
        **{key: client_hosts for key in sorted(farm.data["client_environments"])},
    }


def build_and_distribute_foundations(
    farm: FarmSpec,
    *,
    repo: Path,
    output: Path,
    target_hosts: dict[str, Iterable[str]] | None = None,
    recorder: RecordingTransport | None = None,
) -> dict[str, Any]:
    """Build/package foundations and optionally distribute them to selected hosts."""

    selected_targets = (
        foundation_targets_for_farm(farm) if target_hosts is None else target_hosts
    )
    requested_keys = selected_targets.keys()
    bindings = foundation_image_bindings(farm, repo, requested_keys)
    transport = recorder or RecordingTransport()
    commands = CommandFactory()
    timeout_s = 7200
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    results: dict[str, Any] = {}
    for binding in bindings:
        hub_identity, built = ensure_foundation_image(
            binding,
            repo,
            transport,
            commands,
            timeout_s=timeout_s,
        )
        image_archive = (
            output.parent / "archives" / f"{hub_identity.closure_sha256}.docker.tar.zst"
        )
        transport_metadata = _save_once(
            binding,
            hub_identity,
            image_archive,
            transport,
            commands,
            timeout_s,
        )
        hosts: dict[str, Any] = {}
        selected_hosts = sorted(set(selected_targets.get(binding.key, ())))
        for host_name in selected_hosts:
            if host_name not in farm.hosts:
                raise ImageError(f"unknown foundation image target host {host_name!r}")
            required_role = "C" if binding.kind == "client-environment" else None
            allowed = farm.hosts[host_name]["roles_allowed"]
            if required_role is not None and required_role not in allowed:
                raise ImageError(
                    f"host {host_name} cannot run client environment {binding.key}"
                )
            if required_role is None and not set(allowed).intersection(("S", "F")):
                raise ImageError(f"host {host_name} cannot run the stable S/F image")
            observed = _save_load_host(
                farm,
                binding,
                hub_identity,
                host_name,
                image_archive,
                transport,
                commands,
                timeout_s,
                metadata=transport_metadata,
            )
            if observed.closure_sha256 != hub_identity.closure_sha256:
                raise ImageError(
                    f"host {host_name} foundation closure mismatch for {binding.key}"
                )
            hosts[host_name] = {
                "closure_sha256": observed.closure_sha256,
                "id": observed.native_id,
                "mode": "save-load",
                "transport_archive": {
                    **transport_metadata["artifact"],
                    "path": str(
                        _remote_transport_path(
                            farm,
                            host_name,
                            transport_metadata["artifact"]["sha256"],
                        )
                    ),
                },
            }
        results[binding.key] = {
            "built": built,
            "closure_sha256": hub_identity.closure_sha256,
            "dockerfile": str(binding.dockerfile),
            "dockerfile_sha256": _sha256(binding.dockerfile),
            "hosts": hosts,
            "hub_id": hub_identity.native_id,
            "kind": binding.kind,
            "reference": binding.reference,
            "transport_archive": {**transport_metadata, "path": str(image_archive)},
        }
    receipt = {
        "commands": [command.as_dict() for command in transport.commands],
        "farm_digest": farm.digest,
        "images": results,
        "schema": FOUNDATION_IMAGE_RECEIPT_SCHEMA,
    }
    temporary = output.with_name(output.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_bytes(canonical_bytes(receipt))
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    return receipt


def build_and_distribute(
    farm: FarmSpec,
    labels: Iterable[str],
    *,
    repo: Path,
    output: Path,
    recorder: RecordingTransport | None = None,
) -> dict[str, Any]:
    """Build each authority label and atomically write a distribution receipt."""

    bindings = image_bindings(farm, labels)
    transport = recorder or RecordingTransport()
    commands = CommandFactory()
    timeout_s = 1800
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    results: dict[str, Any] = {}
    with tempfile.TemporaryDirectory(
        prefix="icefarm-images-", dir=output.parent
    ) as raw_work:
        work = Path(raw_work)
        for binding in bindings:
            context = work / f"context-{binding.label}"
            hub_identity, built = ensure_product_image(
                farm,
                binding,
                repo,
                context,
                transport,
                commands,
                timeout_s=timeout_s,
            )
            daemon_role_sha256 = None
            if binding.kind == "daemon-mutant":
                daemon_role_sha256 = _probe_daemon_role_hash(
                    binding, transport, commands, timeout_s=timeout_s
                )
            image_archive = (
                output.parent
                / "archives"
                / f"{hub_identity.closure_sha256}.docker.tar.zst"
            )
            transport_metadata = _save_once(
                binding,
                hub_identity,
                image_archive,
                transport,
                commands,
                timeout_s,
            )
            hosts = distribute_image(
                farm,
                binding,
                hub_identity,
                image_archive,
                transport,
                commands,
                timeout_s=timeout_s,
            )
            results[binding.label] = {
                "archive_sha256": binding.archive_sha256,
                "built": built,
                "commit": binding.commit,
                "hosts": hosts,
                "closure_sha256": hub_identity.closure_sha256,
                "closure_schema": "docker-inspect-runtime-closure-v1",
                "hub_id": hub_identity.native_id,
                "reference": binding.reference,
                "transport_archive": {
                    **transport_metadata,
                    "path": str(image_archive),
                },
                **(
                    {"daemon_role_sha256": daemon_role_sha256}
                    if daemon_role_sha256 is not None
                    else {}
                ),
            }
    receipt = {
        "commands": [command.as_dict() for command in transport.commands],
        "farm_digest": farm.digest,
        "images": results,
        "schema": IMAGE_RECEIPT_SCHEMA,
    }
    temporary = output.with_name(output.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_bytes(canonical_bytes(receipt))
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    return receipt
