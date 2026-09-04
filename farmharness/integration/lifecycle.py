"""Fail-closed preflight, readiness, diagnostics, and labelled teardown."""

from __future__ import annotations

import hashlib
import json
import os
import re
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Iterable, Mapping, Protocol

try:
    from .farm_spec import FarmSpec
    from .images import CommandFactory, ImageError, RecordingTransport, _image_identity
    from .remote import CommandResult, PlannedCommand, RemoteError, docker_argv, execute, ssh_argv
    from .scenario_spec import ScenarioSpec
    from .schema_validation import canonical_bytes
except ImportError:  # Direct execution from this directory.
    from farm_spec import FarmSpec
    from images import CommandFactory, ImageError, RecordingTransport, _image_identity
    from remote import CommandResult, PlannedCommand, RemoteError, docker_argv, execute, ssh_argv
    from scenario_spec import ScenarioSpec
    from schema_validation import canonical_bytes


PREFLIGHT_SCHEMA = "icefarm-preflight-v1"
LIFECYCLE_SCHEMA = "icefarm-lifecycle-v1"
MIN_FREE_BYTES = 25_000_000_000
DISK_PROBE_BYTES = 1 << 30
MIN_WRITE_BPS = 200_000_000
POLL_INTERVAL_S = 1.0
ROLE_BINARY_PATHS = {
    "S": "/opt/icecream/sbin/icecc-scheduler",
    "C": "/opt/icecream/bin/icecc",
    "F": "/opt/icecream/sbin/iceccd",
}


HOST_PREFLIGHT_SCRIPT = r"""
import json, os, pathlib, subprocess, sys, time

root = pathlib.Path(sys.argv[1])
run_id = sys.argv[2]
patterns = json.loads(sys.argv[3])
probe_bytes = int(sys.argv[4])
if not root.is_dir() or root.is_symlink():
    raise SystemExit("scratch root is absent, not a directory, or a symlink")
usage = os.statvfs(root)
free_bytes = usage.f_bavail * usage.f_frsize
source = subprocess.run(
    ["findmnt", "-T", str(root), "-no", "SOURCE"],
    check=True, capture_output=True, text=True,
).stdout.strip()
if not source.startswith("/dev/"):
    raise SystemExit("scratch root has no named block device")
rotations = subprocess.run(
    ["lsblk", "-srndo", "ROTA", source],
    check=True, capture_output=True, text=True,
).stdout.split()
if not rotations or any(item not in ("0", "1") for item in rotations):
    raise SystemExit("scratch device rotation state is unavailable")

protected = {pattern: 0 for pattern in patterns}
skip = {os.getpid(), os.getppid()}
for item in pathlib.Path("/proc").iterdir():
    if not item.name.isdigit() or int(item.name) in skip:
        continue
    try:
        command = (item / "cmdline").read_bytes().replace(b"\0", b" ").decode(
            "utf-8", "replace"
        )
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        continue
    for pattern in patterns:
        if pattern in command:
            protected[pattern] += 1

write_bps = None
if probe_bytes:
    target = root / (".icefarm-fdatasync-" + run_id + "-" + str(os.getpid()))
    descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    started = time.monotonic()
    try:
        block = bytes(1024 * 1024)
        remaining = probe_bytes
        while remaining:
            amount = min(len(block), remaining)
            view = memoryview(block)[:amount]
            while view:
                written = os.write(descriptor, view)
                view = view[written:]
            remaining -= amount
        os.fdatasync(descriptor)
    finally:
        os.close(descriptor)
        target.unlink(missing_ok=True)
    elapsed = max(time.monotonic() - started, 0.000001)
    write_bps = probe_bytes / elapsed

print(json.dumps({
    "device": source,
    "free_bytes": free_bytes,
    "protected": protected,
    "rotational": any(item == "1" for item in rotations),
    "scratch_root": str(root),
    "write_bps": write_bps,
}, sort_keys=True))
""".strip()


SOCKET_PROBE_SCRIPT = r"""
import socket, sys

address = sys.argv[1]
port = int(sys.argv[2])
with socket.create_connection((address, port), timeout=2.0) as stream:
    stream.settimeout(2.0)
    stream.sendall(b"listcs\nquit\n")
    chunks = []
    while True:
        try:
            block = stream.recv(65536)
        except socket.timeout:
            break
        if not block:
            break
        chunks.append(block)
sys.stdout.buffer.write(b"".join(chunks))
""".strip()


ENVIRONMENT_READY_SCRIPT = r"""
import json, pathlib, sys

path = pathlib.Path(sys.argv[1])
workers = json.loads(sys.argv[2])
try:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
except FileNotFoundError:
    lines = []
matched = {}
for worker in workers:
    found = [
        line for line in lines
        if ("RELOGIN " + worker) in line and "[" in line and "]" in line
    ]
    matched[worker] = found[-1] if found else None
print(json.dumps({"matched": matched, "ready": all(matched.values())}, sort_keys=True))
""".strip()


CORPUS_VERIFY_SCRIPT = r"""
import hashlib, json, pathlib, sys

root = pathlib.Path(sys.argv[1])
authority = sys.argv[2]
marker = root / "AUTHORITY.sha256"
manifest = root / "MANIFEST.sha256"
if not marker.is_file() or not manifest.is_file():
    print(json.dumps({"ready": False, "reason": "marker-absent"}, sort_keys=True))
    raise SystemExit(0)
if marker.read_text(encoding="ascii").strip() != authority:
    print(json.dumps({"ready": False, "reason": "authority-mismatch"}, sort_keys=True))
    raise SystemExit(0)
checked = 0
for line in manifest.read_text(encoding="utf-8").splitlines():
    digest, separator, relative = line.partition("  ")
    candidate = pathlib.PurePosixPath(relative)
    if (
        len(digest) != 64
        or any(ch not in "0123456789abcdef" for ch in digest)
        or separator != "  "
        or candidate.is_absolute()
        or ".." in candidate.parts
        or relative in ("", ".")
    ):
        print(json.dumps({"ready": False, "reason": "manifest-invalid"}, sort_keys=True))
        raise SystemExit(0)
    path = root.joinpath(*candidate.parts)
    if not path.is_file() or path.is_symlink():
        print(json.dumps({"ready": False, "reason": "file-absent"}, sort_keys=True))
        raise SystemExit(0)
    observed = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            observed.update(block)
    if observed.hexdigest() != digest:
        print(json.dumps({"ready": False, "reason": "file-mismatch"}, sort_keys=True))
        raise SystemExit(0)
    checked += 1
print(json.dumps({"checked": checked, "ready": True}, sort_keys=True))
""".strip()


CANARY_SCRIPT = r"""
set -eu
result_root=$1
preferred=$2
mkdir -p "$result_root/env" "$result_root/canary"
environment=$(find "$result_root/env" -maxdepth 1 -type f -name '*.tar.gz' -print -quit)
if test -z "$environment"
then
    (cd "$result_root/env" && /bin/bash /opt/icecream/bin/icecc-create-env /usr/bin/g++-11) \
        >"$result_root/canary/create-env.log" 2>&1
    environment=$(find "$result_root/env" -maxdepth 1 -type f -name '*.tar.gz' -print -quit)
fi
test -n "$environment"
source_file="$result_root/canary/$preferred.cpp"
remote_object="$result_root/canary/$preferred.remote.o"
local_object="$result_root/canary/$preferred.local.o"
printf '%s\n' '#include <cstdint>' 'extern "C" int icefarm_canary() { return 73; }' >"$source_file"
/usr/bin/g++-11 -std=c++17 -O2 -c "$source_file" -o "$local_object"
ICECC_VERSION="$environment" \
ICECC_PREFERRED_HOST="$preferred" \
ICECC_TEST_REMOTEBUILD=1 \
ICECC_DEBUG=debug \
ICECC_LOGFILE="$result_root/canary/$preferred.client.log" \
    /opt/icecream/bin/icecc /usr/bin/g++-11 -std=c++17 -O2 \
        -c "$source_file" -o "$remote_object" \
        >"$result_root/canary/$preferred.stdout.log" 2>&1
cmp "$local_object" "$remote_object"
sha256sum "$local_object" "$remote_object"
""".strip()


class LifecycleError(RuntimeError):
    """The controlled lifecycle failed after validation."""


class PreflightRefusal(LifecycleError):
    """The farm cannot safely start this scenario."""


class Recorder(Protocol):
    def invoke(self, command: PlannedCommand) -> CommandResult:
        ...


@dataclass(frozen=True)
class CorpusGroup:
    name: str
    source_root: Path
    files: tuple[tuple[Path, PurePosixPath], ...]


@dataclass(frozen=True)
class CorpusLayout:
    authority_sha256: str
    bytes: int
    groups: tuple[CorpusGroup, ...]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _atomic_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_bytes(canonical_bytes(value))
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def bundle_root(farm: FarmSpec, run_id: str) -> Path:
    return Path(farm.data["hub"]["results_root"]) / "results" / run_id


def _used_hosts(farm: FarmSpec, topology: dict[str, Any]) -> list[str]:
    selected = {item["host"] for item in topology["instances"]}
    return [name for name in farm.hosts if name in selected]


def _command(
    factory: CommandFactory,
    *,
    phase: str,
    host: str,
    transport: str,
    timeout_s: int,
    argv: Iterable[str],
) -> PlannedCommand:
    return factory.make(
        phase=phase,
        host=host,
        transport=transport,
        timeout_s=max(1, timeout_s),
        argv=argv,
    )


def _json_result(result: CommandResult, subject: str) -> dict[str, Any]:
    try:
        value = json.loads(result.stdout.strip())
    except json.JSONDecodeError as exc:
        raise LifecycleError(f"{subject} returned invalid JSON") from exc
    if not isinstance(value, dict):
        raise LifecycleError(f"{subject} returned a non-object")
    return value


def _manifest_lines(path: Path) -> list[Path]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise PreflightRefusal(f"cannot read corpus manifest {path}: {exc}") from exc
    result: list[Path] = []
    for index, line in enumerate(lines):
        candidate = Path(line)
        if (
            not candidate.is_absolute()
            or any(ord(character) < 32 or ord(character) == 127 for character in line)
            or not candidate.is_file()
        ):
            raise PreflightRefusal(
                f"corpus manifest {path} line {index + 1} is not an existing absolute file"
            )
        result.append(candidate)
    if not result:
        raise PreflightRefusal(f"corpus manifest {path} is empty")
    return result


def _relative_under(source: Path, root: Path, subject: str) -> PurePosixPath:
    marker = "/" + root.as_posix().lstrip("/").rstrip("/") + "/"
    text = source.as_posix()
    if marker not in text:
        raise PreflightRefusal(f"{subject} path {source} does not contain authority root {root}")
    relative = PurePosixPath(text.split(marker, 1)[1])
    if relative.is_absolute() or ".." in relative.parts or str(relative) in ("", "."):
        raise PreflightRefusal(f"{subject} path {source} has no safe normalized suffix")
    return relative


def corpus_layout(farm: FarmSpec, corpus_name: str) -> CorpusLayout:
    corpus = farm.data["corpora"][corpus_name]
    root = Path(corpus["root"])
    groups: list[CorpusGroup] = []
    if "manifest" in corpus:
        manifest = Path(corpus["manifest"])
        observed = _sha256(manifest)
        if observed != corpus["manifest_sha256"]:
            raise PreflightRefusal(
                f"corpus {corpus_name} manifest mismatch: "
                f"expected {corpus['manifest_sha256']}, got {observed}"
            )
        files = tuple((path, _relative_under(path, root, corpus_name)) for path in _manifest_lines(manifest))
        groups.append(CorpusGroup("files", root, files))
    else:
        normalized: list[list[PurePosixPath]] = []
        for turn, field in (("A", "turn_a_manifest"), ("B", "turn_b_manifest")):
            paths = _manifest_lines(Path(corpus[field]))
            relatives = [_relative_under(path, root, f"{corpus_name}/{turn}") for path in paths]
            normalized.append(relatives)
            groups.append(
                CorpusGroup(turn, Path(os.path.commonpath(paths)), tuple(zip(paths, relatives)))
            )
        if normalized[0] != normalized[1]:
            raise PreflightRefusal(f"corpus {corpus_name} A/B normalized paths differ")
        digest = hashlib.sha256(
            ("\n".join(str(path) for path in normalized[0]) + "\n").encode()
        ).hexdigest()
        if digest != corpus["normalized_manifest_sha256"]:
            raise PreflightRefusal(
                f"corpus {corpus_name} normalized manifest mismatch: "
                f"expected {corpus['normalized_manifest_sha256']}, got {digest}"
            )
    flat = [entry for group in groups for entry in group.files]
    return CorpusLayout(
        authority_sha256=hashlib.sha256(canonical_bytes(corpus)).hexdigest(),
        bytes=sum(source.stat().st_size for source, _relative in flat),
        groups=tuple(groups),
    )


def _remote_corpus_root(farm: FarmSpec, host_name: str, corpus_name: str) -> PurePosixPath:
    return (
        PurePosixPath(farm.hosts[host_name]["scratch_root"])
        / "icefarm"
        / "corpora"
        / corpus_name
    )


def _verify_remote_corpus(
    farm: FarmSpec,
    host_name: str,
    corpus_name: str,
    layout: CorpusLayout,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, Any]:
    root = _remote_corpus_root(farm, host_name, corpus_name)
    result = recorder.invoke(
        _command(
            factory,
            phase="preflight.corpus-verify",
            host=host_name,
            transport="ssh",
            timeout_s=timeout_s,
            argv=ssh_argv(
                farm,
                host_name,
                ("python3", "-c", CORPUS_VERIFY_SCRIPT, str(root), layout.authority_sha256),
            ),
        )
    )
    return _json_result(result, f"corpus verification on {host_name}")


def _sync_corpus(
    farm: FarmSpec,
    host_name: str,
    corpus_name: str,
    layout: CorpusLayout,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, Any]:
    first = _verify_remote_corpus(
        farm, host_name, corpus_name, layout, recorder, factory, timeout_s
    )
    if first.get("ready") is True:
        return {"mode": "verified-existing", **first}
    root = _remote_corpus_root(farm, host_name, corpus_name)
    recorder.invoke(
        _command(
            factory,
            phase="preflight.corpus-mkdir",
            host=host_name,
            transport="ssh",
            timeout_s=timeout_s,
            argv=ssh_argv(farm, host_name, ("install", "-d", "-m", "0755", "--", str(root))),
        )
    )
    with tempfile.TemporaryDirectory(prefix="icefarm-corpus-") as raw:
        staging = Path(raw)
        manifest_rows: list[tuple[str, str]] = []
        for group in layout.groups:
            list_path = staging / f"{group.name}.files"
            for source, relative in group.files:
                manifest_rows.append((_sha256(source), str(PurePosixPath(group.name) / relative)))
            relative_rows = [
                source.relative_to(group.source_root).as_posix()
                for source, _relative in group.files
            ]
            list_path.write_text("\n".join(relative_rows) + "\n", encoding="utf-8")
            destination = f"{farm.hosts[host_name]['ssh']}:{root}/{group.name}/"
            recorder.invoke(
                _command(
                    factory,
                    phase="preflight.corpus-sync",
                    host=host_name,
                    transport="rsync-ssh",
                    timeout_s=timeout_s,
                    argv=(
                        "rsync",
                        "--archive",
                        "--checksum",
                        "--protect-args",
                        f"--files-from={list_path}",
                        str(group.source_root) + "/",
                        destination,
                    ),
                )
            )
        manifest_rows.sort(key=lambda item: item[1])
        (staging / "MANIFEST.sha256").write_text(
            "".join(f"{digest}  {relative}\n" for digest, relative in manifest_rows),
            encoding="utf-8",
        )
        (staging / "AUTHORITY.sha256").write_text(
            layout.authority_sha256 + "\n", encoding="ascii"
        )
        recorder.invoke(
            _command(
                factory,
                phase="preflight.corpus-metadata",
                host=host_name,
                transport="rsync-ssh",
                timeout_s=timeout_s,
                argv=(
                    "rsync",
                    "--archive",
                    "--checksum",
                    "--protect-args",
                    str(staging / "MANIFEST.sha256"),
                    str(staging / "AUTHORITY.sha256"),
                    f"{farm.hosts[host_name]['ssh']}:{root}/",
                ),
            )
        )
    final = _verify_remote_corpus(
        farm, host_name, corpus_name, layout, recorder, factory, timeout_s
    )
    if final.get("ready") is not True:
        raise PreflightRefusal(
            f"corpus {corpus_name} failed verification on {host_name}: {final.get('reason')}"
        )
    return {"mode": "synced", **final}


def _host_facts(
    farm: FarmSpec,
    host_name: str,
    run_id: str,
    recorder: Recorder,
    factory: CommandFactory,
    *,
    timeout_s: int,
    probe_bytes: int,
) -> dict[str, Any]:
    patterns = farm.data["protected"]["process_patterns"]
    result = recorder.invoke(
        _command(
            factory,
            phase="preflight.host",
            host=host_name,
            transport="ssh",
            timeout_s=timeout_s,
            argv=ssh_argv(
                farm,
                host_name,
                (
                    "python3",
                    "-c",
                    HOST_PREFLIGHT_SCRIPT,
                    farm.hosts[host_name]["scratch_root"],
                    run_id,
                    json.dumps(patterns, separators=(",", ":")),
                    str(probe_bytes),
                ),
            ),
        )
    )
    value = _json_result(result, f"host preflight on {host_name}")
    required = {
        "device": str,
        "free_bytes": int,
        "protected": dict,
        "rotational": bool,
        "scratch_root": str,
    }
    for key, kind in required.items():
        if not isinstance(value.get(key), kind):
            raise PreflightRefusal(f"host {host_name} preflight lacks typed field {key}")
    if value["scratch_root"] != farm.hosts[host_name]["scratch_root"]:
        raise PreflightRefusal(f"host {host_name} reported a different scratch root")
    if value["rotational"]:
        raise PreflightRefusal(
            f"host {host_name} scratch device {value['device']} is rotational"
        )
    if probe_bytes:
        speed = value.get("write_bps")
        if not isinstance(speed, (int, float)) or speed < MIN_WRITE_BPS:
            raise PreflightRefusal(
                f"host {host_name} fdatasync probe below {MIN_WRITE_BPS} B/s: {speed!r}"
            )
    if value["protected"] != {
        pattern: value["protected"].get(pattern) for pattern in patterns
    } or any(
        not isinstance(value["protected"].get(pattern), int)
        or value["protected"][pattern] < 0
        for pattern in patterns
    ):
        raise PreflightRefusal(f"host {host_name} returned invalid protected counts")
    return value


def _docker_transport(farm: FarmSpec, host_name: str) -> str:
    return "docker-context" if farm.hosts[host_name].get("docker_context") else "ssh-docker"


def _labelled_containers(
    farm: FarmSpec,
    host_name: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> list[dict[str, Any]]:
    result = recorder.invoke(
        _command(
            factory,
            phase="preflight.stale-list",
            host=host_name,
            transport=_docker_transport(farm, host_name),
            timeout_s=timeout_s,
            argv=docker_argv(
                farm,
                host_name,
                ("ps", "-aq", "--filter", "label=icefarm.run"),
            ),
        )
    )
    containers: list[dict[str, Any]] = []
    for container_id in result.stdout.split():
        if re.fullmatch(r"[0-9a-f]{12,64}", container_id) is None:
            raise PreflightRefusal(
                f"host {host_name} returned unsafe labelled container id {container_id!r}"
            )
        inspected = recorder.invoke(
            _command(
                factory,
                phase="preflight.stale-inspect",
                host=host_name,
                transport=_docker_transport(farm, host_name),
                timeout_s=timeout_s,
                argv=docker_argv(
                    farm,
                    host_name,
                    ("container", "inspect", "--format", "{{json .}}", container_id),
                ),
            )
        )
        value = _json_result(inspected, f"labelled container {container_id} on {host_name}")
        labels = value.get("Config", {}).get("Labels", {})
        name = value.get("Name")
        created = value.get("Created")
        run_label = labels.get("icefarm.run") if isinstance(labels, dict) else None
        if (
            not isinstance(name, str)
            or not name.startswith("/icefarm-")
            or not isinstance(created, str)
            or not isinstance(run_label, str)
        ):
            raise PreflightRefusal(
                f"host {host_name} has a labelled object outside the icefarm naming contract"
            )
        containers.append(
            {"created": created, "id": container_id, "name": name[1:], "run_id": run_label}
        )
    return containers


def _created_time(value: str) -> datetime:
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    try:
        parsed = datetime.fromisoformat(normalized)
    except ValueError as exc:
        raise PreflightRefusal(f"Docker returned invalid creation time {value!r}") from exc
    if parsed.tzinfo is None:
        raise PreflightRefusal(f"Docker returned timezone-free creation time {value!r}")
    return parsed.astimezone(timezone.utc)


def _refuse_or_reap_stale(
    farm: FarmSpec,
    hosts: list[str],
    recorder: Recorder,
    factory: CommandFactory,
    *,
    timeout_s: int,
    reap_stale_hours: float | None,
    now: datetime,
) -> list[dict[str, Any]]:
    observed: list[dict[str, Any]] = []
    for host_name in hosts:
        for item in _labelled_containers(farm, host_name, recorder, factory, timeout_s):
            observed.append({"host": host_name, **item})
    if not observed:
        return []
    if reap_stale_hours is None:
        names = ", ".join(f"{item['host']}:{item['name']}" for item in observed)
        raise PreflightRefusal(f"stale icefarm containers require --reap-stale: {names}")
    if reap_stale_hours < 0:
        raise PreflightRefusal("--reap-stale hours must be non-negative")
    cutoff_s = reap_stale_hours * 3600
    too_young = [
        item
        for item in observed
        if (now - _created_time(item["created"])).total_seconds() < cutoff_s
    ]
    if too_young:
        names = ", ".join(f"{item['host']}:{item['name']}" for item in too_young)
        raise PreflightRefusal(f"labelled containers are newer than reap threshold: {names}")
    for item in observed:
        recorder.invoke(
            _command(
                factory,
                phase="preflight.stale-reap",
                host=item["host"],
                transport=_docker_transport(farm, item["host"]),
                timeout_s=timeout_s,
                argv=docker_argv(
                    farm, item["host"], ("container", "rm", "--force", item["id"])
                ),
            )
        )
    for host_name in hosts:
        if _labelled_containers(farm, host_name, recorder, factory, timeout_s):
            raise PreflightRefusal(f"host {host_name} retained labelled objects after reap")
    return observed


def _inspect_required_image(
    farm: FarmSpec,
    host_name: str,
    label: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, Any]:
    registry = farm.data["registry"]
    reference = f"{registry['host']}:{registry['port']}/icefarm/icecream:{label}"
    result = recorder.invoke(
        _command(
            factory,
            phase="preflight.image",
            host=host_name,
            transport=_docker_transport(farm, host_name),
            timeout_s=timeout_s,
            argv=docker_argv(
                farm,
                host_name,
                ("image", "inspect", "--format", "{{json .}}", reference),
            ),
        )
    )
    try:
        raw = json.loads(result.stdout.strip())
        identity = _image_identity(result, f"image {label} on {host_name}")
    except (json.JSONDecodeError, ImageError) as exc:
        raise PreflightRefusal(str(exc)) from exc
    authority = farm.data["authority"]["images"][label]
    expected = authority.get("closure_sha256")
    if not isinstance(expected, str):
        raise PreflightRefusal(
            f"image {label} has no captured runtime closure in the execution authority"
        )
    if identity.closure_sha256 != expected:
        raise PreflightRefusal(
            f"host {host_name} image closure mismatch for {label}: "
            f"expected {expected}, got {identity.closure_sha256}"
        )
    size = raw.get("Size") if isinstance(raw, dict) else None
    if isinstance(size, bool) or not isinstance(size, int) or size < 0:
        raise PreflightRefusal(f"host {host_name} image {label} has no valid byte size")
    labels = raw.get("Config", {}).get("Labels", {}) if isinstance(raw, dict) else {}
    if not isinstance(labels, dict) or (
        labels.get("icefarm.source.commit") != authority["commit"]
        or labels.get("icefarm.source.archive_sha256") != authority["archive_sha256"]
    ):
        raise PreflightRefusal(f"host {host_name} image {label} has wrong source labels")
    return {
        "bytes": size,
        "closure_sha256": identity.closure_sha256,
        "id": identity.native_id,
    }


def _probe_chroot(
    farm: FarmSpec,
    host_name: str,
    label: str,
    run_id: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> None:
    registry = farm.data["registry"]
    reference = f"{registry['host']}:{registry['port']}/icefarm/icecream:{label}"
    name = f"icefarm-{run_id}-preflight-{host_name}"
    recorder.invoke(
        _command(
            factory,
            phase="preflight.chroot",
            host=host_name,
            transport=_docker_transport(farm, host_name),
            timeout_s=timeout_s,
            argv=docker_argv(
                farm,
                host_name,
                (
                    "run",
                    "--rm",
                    "--pull=never",
                    "--name",
                    name,
                    "--label",
                    f"icefarm.run={run_id}",
                    "--user",
                    "0",
                    "--cap-add",
                    "SYS_CHROOT",
                    "--entrypoint",
                    "/usr/sbin/chroot",
                    reference,
                    "/",
                    "/bin/true",
                ),
            ),
        )
    )


def _probe_role_hashes(
    farm: FarmSpec,
    host_name: str,
    label: str,
    expected: dict[str, str],
    run_id: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, str]:
    registry = farm.data["registry"]
    reference = f"{registry['host']}:{registry['port']}/icefarm/icecream:{label}"
    roles = sorted(expected)
    probe_id = hashlib.sha256((host_name + label).encode()).hexdigest()[:12]
    result = recorder.invoke(
        _command(
            factory,
            phase="preflight.role-hashes",
            host=host_name,
            transport=_docker_transport(farm, host_name),
            timeout_s=timeout_s,
            argv=docker_argv(
                farm,
                host_name,
                (
                    "run",
                    "--rm",
                    "--pull=never",
                    "--name",
                    f"icefarm-{run_id}-hashes-{probe_id}",
                    "--label",
                    f"icefarm.run={run_id}",
                    "--entrypoint",
                    "/usr/bin/sha256sum",
                    reference,
                    *(ROLE_BINARY_PATHS[role] for role in roles),
                ),
            ),
        )
    )
    observed: dict[str, str] = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) != 2 or re.fullmatch(r"[0-9a-f]{64}", fields[0]) is None:
            raise PreflightRefusal(
                f"host {host_name} image {label} returned malformed role hashes"
            )
        observed[fields[1]] = fields[0]
    for role in roles:
        path = ROLE_BINARY_PATHS[role]
        if observed.get(path) != expected[role]:
            raise PreflightRefusal(
                f"host {host_name} image {label} {role} hash mismatch: "
                f"expected {expected[role]}, got {observed.get(path)}"
            )
    return {role: observed[ROLE_BINARY_PATHS[role]] for role in roles}


def preflight(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    *,
    probe_bytes: int = DISK_PROBE_BYTES,
    reap_stale_hours: float | None = None,
    sync_corpora: bool = True,
    now: datetime | None = None,
) -> dict[str, Any]:
    """Refuse unsafe state before executing any persistent start command."""

    topology = plan["topology"]
    hosts = _used_hosts(farm, topology)
    timeout_s = scenario.data["timeouts"]["up_s"]
    for instance in topology["instances"]:
        authority = farm.data["authority"]["images"][instance["image"]["label"]]
        if not isinstance(authority.get("closure_sha256"), str):
            raise PreflightRefusal(
                f"image {instance['image']['label']} has no captured runtime closure "
                "in the execution authority"
            )
    reaped = _refuse_or_reap_stale(
        farm,
        hosts,
        recorder,
        factory,
        timeout_s=timeout_s,
        reap_stale_hours=reap_stale_hours,
        now=now or datetime.now(timezone.utc),
    )
    layout = corpus_layout(farm, scenario.data["workload"]["corpus"])
    host_facts: dict[str, Any] = {}
    for host_name in hosts:
        facts = _host_facts(
            farm,
            host_name,
            plan["run_id"],
            recorder,
            factory,
            timeout_s=timeout_s,
            probe_bytes=probe_bytes,
        )
        host_facts[host_name] = facts

    image_receipts: dict[str, Any] = {}
    image_bytes = {host_name: 0 for host_name in hosts}
    role_hashes: dict[tuple[str, str], dict[str, str]] = {}
    chroot_images: set[tuple[str, str]] = set()
    for instance in topology["instances"]:
        key = f"{instance['host']}:{instance['image']['label']}"
        if key not in image_receipts:
            image_receipts[key] = _inspect_required_image(
                farm,
                instance["host"],
                instance["image"]["label"],
                recorder,
                factory,
                timeout_s,
            )
            image_bytes[instance["host"]] += image_receipts[key]["bytes"]
        group_key = (instance["host"], instance["image"]["label"])
        expected = role_hashes.setdefault(group_key, {})
        previous = expected.setdefault(instance["role"], instance["sha256"])
        if previous != instance["sha256"]:
            raise PreflightRefusal(
                f"conflicting {instance['role']} hashes for image {instance['image']['label']}"
            )
        if instance["role"] == "F":
            chroot_images.add(group_key)

    for host_name in hosts:
        required = MIN_FREE_BYTES + layout.bytes + image_bytes[host_name]
        if host_facts[host_name]["free_bytes"] < required:
            raise PreflightRefusal(
                f"host {host_name} has {host_facts[host_name]['free_bytes']} free bytes; "
                f"needs corpus {layout.bytes}, images {image_bytes[host_name]}, "
                f"and {MIN_FREE_BYTES} working bytes"
            )
        host_facts[host_name]["image_bytes"] = image_bytes[host_name]
        host_facts[host_name]["required_free_before_sync"] = required

    for host_name, label in sorted(chroot_images):
        _probe_chroot(
            farm,
            host_name,
            label,
            plan["run_id"],
            recorder,
            factory,
            timeout_s,
        )

    for (host_name, label), expected in sorted(role_hashes.items()):
        image_receipts[f"{host_name}:{label}"]["role_sha256"] = _probe_role_hashes(
            farm,
            host_name,
            label,
            expected,
            plan["run_id"],
            recorder,
            factory,
            timeout_s,
        )

    corpus_receipts: dict[str, Any] = {}
    if sync_corpora:
        for host_name in hosts:
            corpus_receipts[host_name] = _sync_corpus(
                farm,
                host_name,
                scenario.data["workload"]["corpus"],
                layout,
                recorder,
                factory,
                timeout_s,
            )
            after = _host_facts(
                farm,
                host_name,
                plan["run_id"],
                recorder,
                factory,
                timeout_s=timeout_s,
                probe_bytes=0,
            )
            if after["free_bytes"] < MIN_FREE_BYTES:
                raise PreflightRefusal(
                    f"host {host_name} has less than {MIN_FREE_BYTES} working bytes after sync"
                )
            host_facts[host_name]["free_bytes_after_sync"] = after["free_bytes"]

    return {
        "corpus": {
            "authority_sha256": layout.authority_sha256,
            "bytes": layout.bytes,
            "hosts": corpus_receipts,
            "name": scenario.data["workload"]["corpus"],
        },
        "farm_digest": farm.digest,
        "hosts": host_facts,
        "images": image_receipts,
        "reaped": reaped,
        "run_id": plan["run_id"],
        "schema": PREFLIGHT_SCHEMA,
        "scenario_digest": scenario.digest,
        "topology_digest": plan["topology_digest"],
    }


def _plan_commands(plan: dict[str, Any]) -> list[PlannedCommand]:
    return [
        PlannedCommand(
            sequence=item["sequence"],
            phase=item["phase"],
            host=item["host"],
            instance=item["instance"],
            transport=item["transport"],
            timeout_s=item["timeout_s"],
            argv=tuple(item["argv"]),
        )
        for item in plan["commands"]
    ]


def _recorded_commands(recorder: Recorder) -> list[dict[str, Any]]:
    commands = getattr(recorder, "commands", [])
    return [command.as_dict() for command in commands]


def _timeout_left(deadline: float, monotonic: Callable[[], float]) -> int:
    return max(1, int(deadline - monotonic() + 0.999))


def _scheduler_snapshot(
    plan: dict[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> str:
    scheduler = next(item for item in plan["topology"]["instances"] if item["role"] == "S")
    result = recorder.invoke(
        _command(
            factory,
            phase="readiness.listcs",
            host="hub",
            transport="local-socket",
            timeout_s=min(timeout_s, 5),
            argv=(
                "python3",
                "-c",
                SOCKET_PROBE_SCRIPT,
                scheduler["address"],
                str(plan["ports"]["scheduler_control"]),
            ),
        )
    )
    return result.stdout


def _assert_running(
    farm: FarmSpec,
    host_name: str,
    container: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> None:
    result = recorder.invoke(
        _command(
            factory,
            phase="readiness.container",
            host=host_name,
            transport=_docker_transport(farm, host_name),
            timeout_s=min(timeout_s, 5),
            argv=docker_argv(
                farm,
                host_name,
                (
                    "container",
                    "inspect",
                    "--format",
                    "{{json .State}}",
                    container,
                ),
            ),
        )
    )
    value = _json_result(result, f"container state {host_name}:{container}")
    if value.get("Running") is not True:
        raise LifecycleError(
            f"container {host_name}:{container} exited before readiness: "
            f"status={value.get('Status')} exit={value.get('ExitCode')} error={value.get('Error')}"
        )


def _wait_for(
    description: str,
    probe: Callable[[], tuple[bool, str]],
    *,
    deadline: float,
    monotonic: Callable[[], float],
    sleeper: Callable[[float], None],
) -> str:
    last = "no observation"
    while monotonic() < deadline:
        try:
            ready, detail = probe()
            last = detail
            if ready:
                return detail
        except RemoteError as exc:
            last = str(exc)
        remaining = deadline - monotonic()
        if remaining > 0:
            sleeper(min(POLL_INTERVAL_S, remaining))
    raise LifecycleError(f"readiness timeout waiting for {description}: {last}")


def _wait_scheduler(
    farm: FarmSpec,
    plan: dict[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    *,
    deadline: float,
    monotonic: Callable[[], float],
    sleeper: Callable[[float], None],
) -> str:
    scheduler = next(item for item in plan["topology"]["instances"] if item["role"] == "S")

    def probe() -> tuple[bool, str]:
        _assert_running(
            farm,
            scheduler["host"],
            f"icefarm-{plan['run_id']}-{scheduler['name']}",
            recorder,
            factory,
            _timeout_left(deadline, monotonic),
        )
        snapshot = _scheduler_snapshot(
            plan, recorder, factory, _timeout_left(deadline, monotonic)
        )
        return True, snapshot

    return _wait_for(
        "scheduler control port",
        probe,
        deadline=deadline,
        monotonic=monotonic,
        sleeper=sleeper,
    )


def _wait_workers(
    farm: FarmSpec,
    plan: dict[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    *,
    deadline: float,
    monotonic: Callable[[], float],
    sleeper: Callable[[float], None],
) -> str:
    workers = sorted(
        (item for item in plan["topology"]["instances"] if item["role"] == "F"),
        key=lambda item: item["name"],
    )
    names = [item["name"] for item in workers]

    def probe() -> tuple[bool, str]:
        for worker in workers:
            _assert_running(
                farm,
                worker["host"],
                f"icefarm-{plan['run_id']}-{worker['name']}",
                recorder,
                factory,
                _timeout_left(deadline, monotonic),
            )
        snapshot = _scheduler_snapshot(
            plan, recorder, factory, _timeout_left(deadline, monotonic)
        )
        ready = all(
            re.search(rf"(^|\s){re.escape(name)}(\s|$)", snapshot, re.MULTILINE)
            is not None
            for name in names
        )
        return ready, snapshot[-4000:]

    return _wait_for(
        "worker login: " + ",".join(names),
        probe,
        deadline=deadline,
        monotonic=monotonic,
        sleeper=sleeper,
    )


def _run_canaries(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    *,
    deadline: float,
    monotonic: Callable[[], float],
) -> dict[str, str]:
    client_name = scenario.data["workload"]["clients"][0]
    client = next(item for item in plan["topology"]["instances"] if item["name"] == client_name)
    container = f"icefarm-{plan['run_id']}-{client_name}"
    _assert_running(
        farm,
        client["host"],
        container,
        recorder,
        factory,
        _timeout_left(deadline, monotonic),
    )
    results: dict[str, str] = {}
    for worker in sorted(
        (item for item in plan["topology"]["instances"] if item["role"] == "F"),
        key=lambda item: item["name"],
    ):
        result = recorder.invoke(
            _command(
                factory,
                phase="readiness.canary",
                host=client["host"],
                transport=_docker_transport(farm, client["host"]),
                timeout_s=_timeout_left(deadline, monotonic),
                argv=docker_argv(
                    farm,
                    client["host"],
                    (
                        "exec",
                        container,
                        "/bin/sh",
                        "-c",
                        CANARY_SCRIPT,
                        "icefarm-canary",
                        "/results",
                        worker["name"],
                    ),
                ),
            )
        )
        results[worker["name"]] = result.stdout.strip()
    return results


def _wait_environments(
    farm: FarmSpec,
    plan: dict[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    *,
    deadline: float,
    monotonic: Callable[[], float],
    sleeper: Callable[[float], None],
) -> dict[str, Any]:
    scheduler = next(item for item in plan["topology"]["instances"] if item["role"] == "S")
    workers = sorted(
        item["name"] for item in plan["topology"]["instances"] if item["role"] == "F"
    )
    root = (
        PurePosixPath(farm.hosts[scheduler["host"]]["scratch_root"])
        / "icefarm"
        / plan["run_id"]
        / scheduler["name"]
        / "log"
        / "scheduler.log"
    )
    latest: dict[str, Any] = {}

    def probe() -> tuple[bool, str]:
        nonlocal latest
        result = recorder.invoke(
            _command(
                factory,
                phase="readiness.environments",
                host=scheduler["host"],
                transport="ssh",
                timeout_s=min(_timeout_left(deadline, monotonic), 5),
                argv=ssh_argv(
                    farm,
                    scheduler["host"],
                    (
                        "python3",
                        "-c",
                        ENVIRONMENT_READY_SCRIPT,
                        str(root),
                        json.dumps(workers, separators=(",", ":")),
                    ),
                ),
            )
        )
        latest = _json_result(result, "environment readiness")
        return latest.get("ready") is True, json.dumps(latest, sort_keys=True)

    _wait_for(
        "worker environment installation",
        probe,
        deadline=deadline,
        monotonic=monotonic,
        sleeper=sleeper,
    )
    return latest


def collect_diagnostics(
    farm: FarmSpec,
    plan: dict[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    destination: Path,
    *,
    container_ids: Mapping[str, str] | None = None,
) -> list[str]:
    """Best-effort diagnostic capture that never replaces the original error."""

    problems: list[str] = []
    destination.mkdir(parents=True, exist_ok=True)
    for instance in plan["topology"]["instances"]:
        host_name = instance["host"]
        host_dir = destination / host_name
        host_dir.mkdir(parents=True, exist_ok=True)
        container = f"icefarm-{plan['run_id']}-{instance['name']}"
        container_target = (
            container_ids.get(instance["name"], container)
            if container_ids is not None
            else container
        )
        for kind, args in (
            ("inspect", ("container", "inspect", container_target)),
            ("logs", ("container", "logs", container_target)),
        ):
            try:
                result = recorder.invoke(
                    _command(
                        factory,
                        phase=f"diagnostics.{kind}",
                        host=host_name,
                        transport=_docker_transport(farm, host_name),
                        timeout_s=30,
                        argv=docker_argv(farm, host_name, args),
                    )
                )
                (host_dir / f"{instance['name']}.{kind}").write_text(
                    result.stdout + result.stderr, encoding="utf-8"
                )
            except (RemoteError, OSError) as exc:
                problems.append(f"{host_name}:{instance['name']}:{kind}:{exc}")
        remote_log = (
            PurePosixPath(farm.hosts[host_name]["scratch_root"])
            / "icefarm"
            / plan["run_id"]
            / instance["name"]
            / "log"
        )
        local_log = host_dir / f"{instance['name']}.log"
        local_log.mkdir(parents=True, exist_ok=True)
        try:
            recorder.invoke(
                _command(
                    factory,
                    phase="diagnostics.sync-log",
                    host=host_name,
                    transport="rsync-ssh",
                    timeout_s=60,
                    argv=(
                        "rsync",
                        "--archive",
                        "--protect-args",
                        f"{farm.hosts[host_name]['ssh']}:{remote_log}/",
                        str(local_log) + "/",
                    ),
                )
            )
        except RemoteError as exc:
            problems.append(f"{host_name}:{instance['name']}:sync-log:{exc}")
    return problems


def _remove_run_containers(
    farm: FarmSpec,
    plan: dict[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> list[str]:
    problems: list[str] = []
    for host_name in _used_hosts(farm, plan["topology"]):
        try:
            containers = [
                item
                for item in _labelled_containers(
                    farm, host_name, recorder, factory, timeout_s
                )
                if item["run_id"] == plan["run_id"]
            ]
            for item in containers:
                recorder.invoke(
                    _command(
                        factory,
                        phase="down.remove-container",
                        host=host_name,
                        transport=_docker_transport(farm, host_name),
                        timeout_s=timeout_s,
                        argv=docker_argv(
                            farm,
                            host_name,
                            ("container", "rm", "--force", item["id"]),
                        ),
                    )
                )
            leftovers = [
                item
                for item in _labelled_containers(
                    farm, host_name, recorder, factory, timeout_s
                )
                if item["run_id"] == plan["run_id"]
            ]
            if leftovers:
                problems.append(
                    f"{host_name}:leftovers=" + ",".join(item["name"] for item in leftovers)
                )
        except (RemoteError, LifecycleError) as exc:
            problems.append(f"{host_name}:container-cleanup:{exc}")
    return problems


def tear_down(
    farm: FarmSpec,
    plan: dict[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    *,
    protected_before: dict[str, dict[str, int]] | None = None,
) -> dict[str, Any]:
    timeout_s = plan.get("timeouts", {}).get("down_s", 300)
    problems = _remove_run_containers(farm, plan, recorder, factory, timeout_s)
    for instance in plan["topology"]["instances"]:
        root = (
            PurePosixPath(farm.hosts[instance["host"]]["scratch_root"])
            / "icefarm"
            / plan["run_id"]
            / instance["name"]
        )
        registry = farm.data["registry"]
        reference = (
            f"{registry['host']}:{registry['port']}/icefarm/icecream:"
            f"{instance['image']['label']}"
        )
        cleanup_name = f"icefarm-{plan['run_id']}-cleanup-{instance['name']}"
        try:
            recorder.invoke(
                _command(
                    factory,
                    phase="down.remove-scratch",
                    host=instance["host"],
                    transport=_docker_transport(farm, instance["host"]),
                    timeout_s=timeout_s,
                    argv=docker_argv(
                        farm,
                        instance["host"],
                        (
                            "run",
                            "--rm",
                            "--pull=never",
                            "--name",
                            cleanup_name,
                            "--label",
                            f"icefarm.run={plan['run_id']}",
                            "--user",
                            "0",
                            "--mount",
                            f"type=bind,src={root},dst=/cleanup",
                            "--entrypoint",
                            "/bin/sh",
                            reference,
                            "-c",
                            "rm -rf -- /cleanup/cache /cleanup/tmp /cleanup/log",
                        ),
                    ),
                )
            )
        except RemoteError as exc:
            problems.append(f"{instance['host']}:{instance['name']}:scratch-cleanup:{exc}")

    problems.extend(_remove_run_containers(farm, plan, recorder, factory, timeout_s))

    protected_after: dict[str, dict[str, int]] = {}
    if protected_before is not None:
        for host_name in _used_hosts(farm, plan["topology"]):
            try:
                facts = _host_facts(
                    farm,
                    host_name,
                    plan["run_id"],
                    recorder,
                    factory,
                    timeout_s=timeout_s,
                    probe_bytes=0,
                )
                protected_after[host_name] = facts["protected"]
                if protected_after[host_name] != protected_before.get(host_name):
                    problems.append(
                        f"{host_name}:protected-count-changed:"
                        f"{protected_before.get(host_name)}->{protected_after[host_name]}"
                    )
            except (RemoteError, LifecycleError) as exc:
                problems.append(f"{host_name}:protected-check:{exc}")
    receipt = {
        "problems": problems,
        "protected_after": protected_after,
        "run_id": plan["run_id"],
        "schema": LIFECYCLE_SCHEMA,
        "status": "DOWN" if not problems else "DOWN_WITH_ERRORS",
    }
    if problems:
        raise LifecycleError("teardown incomplete: " + "; ".join(problems))
    return receipt


def bring_up(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    *,
    recorder: RecordingTransport | None = None,
    probe_bytes: int = DISK_PROBE_BYTES,
    reap_stale_hours: float | None = None,
    sync_corpora: bool = True,
    monotonic: Callable[[], float] = time.monotonic,
    sleeper: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    """Bring one validated plan up, proving real remote readiness with canaries."""

    transport = recorder or RecordingTransport()
    factory = CommandFactory()
    bundle = bundle_root(farm, plan["run_id"])
    bundle.mkdir(parents=True, exist_ok=True)
    preflight_receipt: dict[str, Any] | None = None
    try:
        preflight_receipt = preflight(
            farm,
            scenario,
            plan,
            transport,
            factory,
            probe_bytes=probe_bytes,
            reap_stale_hours=reap_stale_hours,
            sync_corpora=sync_corpora,
        )
        _atomic_json(bundle / "preflight.json", preflight_receipt)
        deadline = monotonic() + scenario.data["timeouts"]["up_s"]
        planned = _plan_commands(plan)
        phases = {
            phase: [command for command in planned if command.phase == phase]
            for phase in ("up.prepare", "up.start-s", "up.start-f", "up.start-c")
        }
        execute(phases["up.prepare"], transport)
        execute(phases["up.start-s"], transport)
        scheduler = _wait_scheduler(
            farm,
            plan,
            transport,
            factory,
            deadline=deadline,
            monotonic=monotonic,
            sleeper=sleeper,
        )
        execute(phases["up.start-f"], transport)
        workers = _wait_workers(
            farm,
            plan,
            transport,
            factory,
            deadline=deadline,
            monotonic=monotonic,
            sleeper=sleeper,
        )
        execute(phases["up.start-c"], transport)
        canaries = _run_canaries(
            farm,
            scenario,
            plan,
            transport,
            factory,
            deadline=deadline,
            monotonic=monotonic,
        )
        environments = _wait_environments(
            farm,
            plan,
            transport,
            factory,
            deadline=deadline,
            monotonic=monotonic,
            sleeper=sleeper,
        )
        receipt = {
            "canaries": canaries,
            "commands": _recorded_commands(transport),
            "environment_readiness": environments,
            "farm": str(farm.path),
            "farm_digest": farm.digest,
            "plan": plan,
            "run_id": plan["run_id"],
            "scenario": str(scenario.path),
            "scenario_digest": scenario.digest,
            "scheduler_snapshot": scheduler,
            "schema": LIFECYCLE_SCHEMA,
            "status": "UP",
            "topology_digest": plan["topology_digest"],
            "worker_snapshot": workers,
        }
        _atomic_json(bundle / "lifecycle.json", receipt)
        return receipt
    except BaseException as exc:
        observed_commands = getattr(transport, "commands", [])
        prepared_or_started = any(
            command.phase == "up.prepare" or command.phase.startswith("up.start-")
            for command in observed_commands
        )
        preflight_container_possible = any(
            command.phase in ("preflight.chroot", "preflight.role-hashes")
            for command in observed_commands
        )
        diagnostics = (
            collect_diagnostics(farm, plan, transport, factory, bundle / "diagnostics")
            if prepared_or_started
            else []
        )
        protected = (
            {
                host: facts["protected"]
                for host, facts in preflight_receipt["hosts"].items()
            }
            if preflight_receipt is not None
            else None
        )
        cleanup_error: str | None = None
        if prepared_or_started:
            try:
                tear_down(
                    farm,
                    plan,
                    transport,
                    factory,
                    protected_before=protected,
                )
            except LifecycleError as down_exc:
                cleanup_error = str(down_exc)
        elif preflight_container_possible:
            problems = _remove_run_containers(
                farm,
                plan,
                transport,
                factory,
                plan.get("timeouts", {}).get("down_s", 300),
            )
            if problems:
                cleanup_error = "; ".join(problems)
        failure = {
            "cleanup_error": cleanup_error,
            "commands": _recorded_commands(transport),
            "diagnostic_errors": diagnostics,
            "error": str(exc) or type(exc).__name__,
            "run_id": plan["run_id"],
            "schema": LIFECYCLE_SCHEMA,
            "status": "FAILED",
        }
        _atomic_json(bundle / "lifecycle.json", failure)
        if isinstance(exc, PreflightRefusal):
            raise
        if not isinstance(exc, Exception):
            raise
        detail = str(exc)
        if cleanup_error:
            detail += f"; {cleanup_error}"
        raise LifecycleError(detail) from exc


def down_from_state(
    farm: FarmSpec,
    plan: dict[str, Any],
    *,
    recorder: RecordingTransport | None = None,
) -> dict[str, Any]:
    transport = recorder or RecordingTransport()
    factory = CommandFactory()
    bundle = bundle_root(farm, plan["run_id"])
    protected_before: dict[str, dict[str, int]] | None = None
    preflight_path = bundle / "preflight.json"
    if preflight_path.is_file():
        try:
            value = json.loads(preflight_path.read_text(encoding="utf-8"))
            protected_before = {
                host: facts["protected"] for host, facts in value["hosts"].items()
            }
        except (OSError, KeyError, TypeError, json.JSONDecodeError) as exc:
            raise LifecycleError(f"cannot load preflight state for down: {exc}") from exc
    collect_diagnostics(farm, plan, transport, factory, bundle / "diagnostics")
    receipt = tear_down(
        farm,
        plan,
        transport,
        factory,
        protected_before=protected_before,
    )
    receipt["commands"] = _recorded_commands(transport)
    _atomic_json(bundle / "down.json", receipt)
    return receipt
