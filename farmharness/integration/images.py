"""Build immutable icecream images and distribute an authenticated runtime closure."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Protocol

try:
    from .farm_spec import FarmSpec
    from .remote import CommandResult, PlannedCommand, RemoteError, SubprocessTransport
    from .schema_validation import canonical_bytes
except ImportError:  # Direct execution from this directory.
    from farm_spec import FarmSpec
    from remote import CommandResult, PlannedCommand, RemoteError, SubprocessTransport
    from schema_validation import canonical_bytes


IMAGE_RECEIPT_SCHEMA = "icefarm-images-v1"
DOCKER_DIR = Path(__file__).with_name("docker")
DOCKER_FILES = (
    "Dockerfile.icecream",
    "entry-scheduler.sh",
    "entry-daemon.sh",
    "entry-client.sh",
)


class ImageError(RuntimeError):
    """An image was not built or distributed with its authenticated identity."""


class Recorder(Protocol):
    def invoke(self, command: PlannedCommand) -> CommandResult:
        ...


@dataclass(frozen=True)
class ImageBinding:
    label: str
    commit: str
    archive_sha256: str
    expected_closure: str | None
    expected_id: str | None
    reference: str


@dataclass(frozen=True)
class ImageIdentity:
    native_id: str
    closure_sha256: str


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
        transport: str,
        timeout_s: int,
        argv: Iterable[str],
    ) -> PlannedCommand:
        command = PlannedCommand(
            sequence=self.sequence,
            phase=phase,
            host=host,
            instance=None,
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
            raise ImageError(f"image label {label!r} is absent from the immutable authority map")
        result.append(
            ImageBinding(
                label=label,
                commit=entry["commit"],
                archive_sha256=entry["archive_sha256"],
                expected_closure=entry.get("closure_sha256"),
                expected_id=entry.get("id"),
                reference=_runtime_reference(farm, label),
            )
        )
    return result


def create_source_archive(repo: Path, binding: ImageBinding, destination: Path) -> None:
    """Write exactly ``git archive <binding.commit>`` and verify its authority hash."""

    repo = repo.resolve()
    destination = destination.resolve()
    try:
        subprocess.run(
            ["git", "cat-file", "-e", f"{binding.commit}^{{commit}}"],
            cwd=repo,
            check=True,
            capture_output=True,
            shell=False,
        )
        with destination.open("wb") as output:
            subprocess.run(
                ["git", "archive", "--format=tar", binding.commit],
                cwd=repo,
                check=True,
                stdout=output,
                stderr=subprocess.PIPE,
                shell=False,
            )
    except (OSError, subprocess.CalledProcessError) as exc:
        destination.unlink(missing_ok=True)
        raise ImageError(f"cannot archive immutable commit {binding.commit}: {exc}") from exc
    observed = _sha256(destination)
    if observed != binding.archive_sha256:
        destination.unlink(missing_ok=True)
        raise ImageError(
            f"source archive mismatch for {binding.label}: "
            f"expected {binding.archive_sha256}, got {observed}"
        )


def prepare_build_context(repo: Path, binding: ImageBinding, context: Path) -> None:
    context.mkdir(parents=True, exist_ok=False)
    create_source_archive(repo, binding, context / "source.tar")
    for name in DOCKER_FILES:
        source = DOCKER_DIR / name
        if not source.is_file():
            raise ImageError(f"image support file is missing: {source}")
        shutil.copy2(source, context / name)


def _image_identity(result: CommandResult, subject: str) -> ImageIdentity:
    try:
        value = json.loads(result.stdout.strip())
    except json.JSONDecodeError as exc:
        raise ImageError(f"{subject} did not return Docker inspect JSON") from exc
    if not isinstance(value, dict):
        raise ImageError(f"{subject} did not return one Docker image object")
    native_id = value.get("Id")
    if not isinstance(native_id, str) or not native_id.startswith("sha256:") or len(native_id) != 71:
        raise ImageError(f"{subject} did not return one Docker native image ID")
    try:
        closure = {
            key: value[key]
            for key in ("Architecture", "Os", "Created", "Config", "RootFS")
        }
    except KeyError as exc:
        raise ImageError(f"{subject} lacks portable closure field {exc.args[0]}") from exc
    return ImageIdentity(
        native_id=native_id,
        closure_sha256=hashlib.sha256(canonical_bytes(closure)).hexdigest(),
    )


def _host_docker_argv(farm: FarmSpec, host_name: str, args: Iterable[str]) -> tuple[str, ...]:
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
                f"SOURCE_ARCHIVE_SHA256={binding.archive_sha256}",
                "--tag",
                binding.reference,
                "--file",
                str(context / "Dockerfile.icecream"),
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
                argv=("docker", "image", "inspect", "--format", "{{json .}}", binding.reference),
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


def _inspect_host(
    farm: FarmSpec,
    binding: ImageBinding,
    host_name: str,
    recorder: Recorder,
    commands: CommandFactory,
    timeout_s: int,
) -> ImageIdentity:
    return _image_identity(
        recorder.invoke(
            commands.make(
                phase="images.inspect-host",
                host=host_name,
                transport="docker-context" if farm.hosts[host_name].get("docker_context") else "docker-ssh",
                timeout_s=timeout_s,
                argv=_host_docker_argv(
                    farm,
                    host_name,
                    ("image", "inspect", "--format", "{{json .}}", binding.reference),
                ),
            )
        ),
        f"host {host_name} image {binding.label}",
    )


def _save_once(
    binding: ImageBinding,
    image_tar: Path,
    recorder: Recorder,
    commands: CommandFactory,
    timeout_s: int,
) -> None:
    if image_tar.exists():
        return
    recorder.invoke(
        commands.make(
            phase="images.save",
            host="hub",
            transport="local-docker",
            timeout_s=timeout_s,
            argv=("docker", "image", "save", "--output", str(image_tar), binding.reference),
        )
    )


def _save_load_host(
    farm: FarmSpec,
    binding: ImageBinding,
    host_name: str,
    image_tar: Path,
    recorder: Recorder,
    commands: CommandFactory,
    timeout_s: int,
) -> ImageIdentity:
    _save_once(binding, image_tar, recorder, commands, timeout_s)
    recorder.invoke(
        commands.make(
            phase="images.load",
            host=host_name,
            transport="docker-context" if farm.hosts[host_name].get("docker_context") else "docker-ssh",
            timeout_s=timeout_s,
            argv=_host_docker_argv(
                farm,
                host_name,
                ("image", "load", "--input", str(image_tar)),
            ),
        )
    )
    return _inspect_host(farm, binding, host_name, recorder, commands, timeout_s)


def _push_digest(
    binding: ImageBinding,
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
        raise ImageError(f"registry push returned invalid RepoDigests for {binding.label}") from exc
    if not isinstance(digests, list):
        raise ImageError(f"registry push returned no RepoDigests for {binding.label}")
    repository = binding.reference.rsplit(":", 1)[0]
    matches = sorted(
        value
        for value in digests
        if isinstance(value, str) and value.startswith(repository + "@sha256:")
    )
    if not matches:
        raise ImageError(f"registry push returned no matching digest for {binding.label}")
    return matches[0]


def _registry_host(
    farm: FarmSpec,
    binding: ImageBinding,
    digest_reference: str,
    host_name: str,
    recorder: Recorder,
    commands: CommandFactory,
    timeout_s: int,
) -> ImageIdentity:
    transport = "docker-context" if farm.hosts[host_name].get("docker_context") else "docker-ssh"
    recorder.invoke(
        commands.make(
            phase="images.pull",
            host=host_name,
            transport=transport,
            timeout_s=timeout_s,
            argv=_host_docker_argv(farm, host_name, ("image", "pull", digest_reference)),
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
    binding: ImageBinding,
    hub_identity: ImageIdentity,
    image_tar: Path,
    recorder: Recorder,
    commands: CommandFactory,
    *,
    timeout_s: int,
) -> dict[str, dict[str, str]]:
    mode = farm.data["registry"]["mode"]
    digest_reference: str | None = None
    if mode in ("registry", "auto"):
        try:
            digest_reference = _push_digest(binding, recorder, commands, timeout_s)
        except (RemoteError, ImageError):
            if mode == "registry":
                raise

    result: dict[str, dict[str, str]] = {}
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
                    host_name,
                    image_tar,
                    recorder,
                    commands,
                    timeout_s,
                )
        else:
            observed = _save_load_host(
                farm,
                binding,
                host_name,
                image_tar,
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
                host_name,
                image_tar,
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
    return result


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
    with tempfile.TemporaryDirectory(prefix="icefarm-images-", dir=output.parent) as raw_work:
        work = Path(raw_work)
        for binding in bindings:
            context = work / f"context-{binding.label}"
            image_tar = work / f"{binding.label}.image.tar"
            prepare_build_context(repo, binding, context)
            hub_identity = build_image(
                farm,
                binding,
                context,
                transport,
                commands,
                timeout_s=timeout_s,
            )
            hosts = distribute_image(
                farm,
                binding,
                hub_identity,
                image_tar,
                transport,
                commands,
                timeout_s=timeout_s,
            )
            results[binding.label] = {
                "archive_sha256": binding.archive_sha256,
                "commit": binding.commit,
                "hosts": hosts,
                "closure_sha256": hub_identity.closure_sha256,
                "closure_schema": "docker-inspect-runtime-closure-v1",
                "hub_id": hub_identity.native_id,
                "reference": binding.reference,
                "transport_archive": (
                    {
                        "bytes": image_tar.stat().st_size,
                        "sha256": _sha256(image_tar),
                    }
                    if image_tar.is_file()
                    else None
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
