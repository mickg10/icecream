"""Fail-closed preflight, readiness, diagnostics, and labelled teardown."""

from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Iterable, Mapping, Protocol

try:
    from .farm_spec import FarmSpec
    from .firefox_corpus_promotion import (
        FirefoxCorpusPromotionError,
        validate_corpus_promotion,
    )
    from .images import CommandFactory, ImageError, RecordingTransport, _image_identity
    from .layout import instance_root, runtime_root, toolchain_root
    from .mutant import MUTANT_ARM_PATH, MUTANT_ARM_CONTRACT
    from .netem import (
        NETEM_RECEIPT_SCHEMA,
        NetemBinding,
        NetemPlanError,
        validate_plan as validate_netem_plan,
        validate_qdisc,
        network_list_args,
        network_inspect_args,
        network_remove_args,
        validate_network_inspect,
    )
    from .remote import (
        CommandResult,
        PlannedCommand,
        RemoteError,
        docker_argv,
        execute,
        ssh_argv,
    )
    from .scenario_spec import ScenarioSpec
    from .schema_validation import canonical_bytes
    from .system_source_snapshot import (
        SystemSourceSnapshotError,
        verify_local_archive,
        verified_materialization,
        private_root,
    )
except ImportError:  # Direct execution from this directory.
    from farm_spec import FarmSpec
    from firefox_corpus_promotion import (
        FirefoxCorpusPromotionError,
        validate_corpus_promotion,
    )
    from images import CommandFactory, ImageError, RecordingTransport, _image_identity
    from layout import instance_root, runtime_root, toolchain_root
    from mutant import MUTANT_ARM_PATH, MUTANT_ARM_CONTRACT
    from netem import (
        NETEM_RECEIPT_SCHEMA,
        NetemBinding,
        NetemPlanError,
        validate_plan as validate_netem_plan,
        validate_qdisc,
        network_list_args,
        network_inspect_args,
        network_remove_args,
        validate_network_inspect,
    )
    from remote import (
        CommandResult,
        PlannedCommand,
        RemoteError,
        docker_argv,
        execute,
        ssh_argv,
    )
    from scenario_spec import ScenarioSpec
    from schema_validation import canonical_bytes
    from system_source_snapshot import (
        SystemSourceSnapshotError,
        verify_local_archive,
        verified_materialization,
        private_root,
    )


PREFLIGHT_SCHEMA = "icefarm-preflight-v1"
LIFECYCLE_SCHEMA = "icefarm-lifecycle-v1"
MIN_FREE_BYTES = 25_000_000_000
DISK_PROBE_BYTES = 1 << 30
MIN_WRITE_BPS = 200_000_000
POLL_INTERVAL_S = 1.0
F_NOFILE_SOFT = 65_536
F_NOFILE_HARD = 65_536
ROLE_BINARY_PATHS = {
    "S": "/opt/icecream/sbin/icecc-scheduler",
    "C": "/opt/icecream/bin/icecc",
    "F": "/opt/icecream/sbin/iceccd",
}


def f_runtime_host_config_valid(host_config: object) -> bool:
    """Return whether an F inspect proves init and the fixed nofile contract."""

    if not isinstance(host_config, Mapping) or host_config.get("Init") is not True:
        return False
    ulimits = host_config.get("Ulimits")
    if not isinstance(ulimits, list):
        return False
    nofile = [
        item
        for item in ulimits
        if isinstance(item, Mapping) and item.get("Name") == "nofile"
    ]
    return (
        len(nofile) == 1
        and nofile[0].get("Soft") == F_NOFILE_SOFT
        and nofile[0].get("Hard") == F_NOFILE_HARD
    )


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


CLIENT_CACHE_READY_SCRIPT = r"""
import json, pathlib, re, sys

path = pathlib.Path(sys.argv[1])
pattern = re.compile(r"cache sidecar adapter state=([0-9]+) lifecycle=([0-9]+)")
try:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
except FileNotFoundError:
    lines = []
observations = []
for line in lines:
    match = pattern.search(line)
    if match is not None:
        observations.append((int(match.group(1)), int(match.group(2)), line))
last = observations[-1] if observations else None
ready = last is not None and last[:2] == (2, 3)
print(json.dumps({
    "line": last[2] if last is not None else None,
    "ready": ready,
    "reason": "ready" if ready else ("no-observation" if last is None else "not-ready"),
    "state": last[0] if last is not None else None,
    "lifecycle": last[1] if last is not None else None,
}, sort_keys=True))
""".strip()


CORPUS_ARCHIVE_VERIFY_SCRIPT = r"""
import hashlib, json, pathlib, sys

archive = pathlib.Path(sys.argv[1])
expected_sha256 = sys.argv[2]
expected_bytes = int(sys.argv[3])
if not archive.exists():
    print(json.dumps({"ready": False, "reason": "absent"}, sort_keys=True))
    raise SystemExit(0)
if not archive.is_file() or archive.is_symlink():
    print(json.dumps({"ready": False, "reason": "unsafe-type"}, sort_keys=True))
    raise SystemExit(0)
observed_bytes = archive.stat().st_size
if observed_bytes != expected_bytes:
    print(json.dumps({"bytes": observed_bytes, "ready": False, "reason": "size-mismatch"}, sort_keys=True))
    raise SystemExit(0)
digest = hashlib.sha256()
with archive.open("rb") as handle:
    for block in iter(lambda: handle.read(1024 * 1024), b""):
        digest.update(block)
observed_sha256 = digest.hexdigest()
print(json.dumps({
    "bytes": observed_bytes,
    "ready": observed_sha256 == expected_sha256,
    "reason": "ready" if observed_sha256 == expected_sha256 else "digest-mismatch",
    "sha256": observed_sha256,
}, sort_keys=True))
""".strip()


CORPUS_MATERIALIZE_SCRIPT = r"""
set -euo pipefail
key=$1
authority=$2
manifest_sha256=$3
group=$4
expected_files=$5
expected_unpacked_bytes=$6
case "$key$authority$manifest_sha256" in
    *[!0-9a-f]*|'') echo "invalid corpus authority" >&2; exit 65;;
esac
test "${#key}" -eq 64
test "${#authority}" -eq 64
test "${#manifest_sha256}" -eq 64
case "$group" in A|B|files) ;; *) echo "invalid corpus group" >&2; exit 65;; esac
test "$expected_files" -ge 1
test "$expected_unpacked_bytes" -ge 1
archive="/icefarm-corpus-archives/$key.tar.zst"
base=/icefarm-corpus-cache
target="$base/active"
test "$(sha256sum "$archive" | awk '{print $1}')" = "$key"
verify_at() {
    candidate=$1
    test -d "$candidate/root" -a ! -L "$candidate/root" \
        -a -f "$candidate/ARCHIVE.sha256" -a ! -L "$candidate/ARCHIVE.sha256" \
        -a "$(cat "$candidate/ARCHIVE.sha256")" = "$key" \
        -a -f "$candidate/root/AUTHORITY.sha256" \
        -a ! -L "$candidate/root/AUTHORITY.sha256" \
        -a "$(cat "$candidate/root/AUTHORITY.sha256")" = "$authority" \
        -a -f "$candidate/root/MANIFEST.sha256" \
        -a ! -L "$candidate/root/MANIFEST.sha256" \
        -a "$(sha256sum "$candidate/root/MANIFEST.sha256" | awk '{print $1}')" = "$manifest_sha256" \
        -a -d "$candidate/root/$group" -a ! -L "$candidate/root/$group" \
        -a "$(wc -l < "$candidate/root/MANIFEST.sha256")" -eq "$expected_files" \
        -a "$(find "$candidate/root" -type f | wc -l)" -eq "$((expected_files + 2))" \
        -a "$(find "$candidate/root/$group" -type f -printf '%s\n' \
            | awk '{total += $1} END {print total + 0}')" -eq "$expected_unpacked_bytes" \
        -a -z "$(find "$candidate/root" -type l -print -quit)" \
        && (cd "$candidate/root" \
            && sha256sum --check --strict MANIFEST.sha256 >/dev/null)
}
if test -e "$target"
then
    if test -f "$target/ARCHIVE.sha256" \
        && test "$(cat "$target/ARCHIVE.sha256")" = "$key"
    then
        verify_at "$target" \
            || { echo "existing corpus cache failed authentication" >&2; exit 65; }
        printf 'verified-existing %s\n' "$target"
        exit 0
    fi
    test -d "$target" -a ! -L "$target" -a -f "$target/ARCHIVE.sha256" \
        || { echo "unowned corpus cache state" >&2; exit 65; }
    old_key=$(cat "$target/ARCHIVE.sha256")
    case "$old_key" in *[!0-9a-f]*|'') exit 65;; esac
    test "${#old_key}" -eq 64
    rm -rf -- "$target"
fi
temporary="$base/.materialize-$key-$$"
test ! -e "$temporary"
trap 'rm -rf -- "$temporary"' EXIT
mkdir -p "$temporary/root"
zstd -q -d --long=31 -c "$archive" | tar -xf - -C "$temporary/root"
printf '%s\n' "$key" >"$temporary/ARCHIVE.sha256"
chmod -R a+rX "$temporary/root"
verify_at "$temporary"
chmod -R a-w "$temporary/root"
sync -f "$temporary/root"
mv "$temporary" "$target"
trap - EXIT
printf 'materialized %s\n' "$target"
""".strip()


CLEAR_CORPUS_INPUT_SCRIPT = r"""
set -euo pipefail
destination=$1
case "$destination" in /icefarm-host/*/input) ;; *) exit 65;; esac
if test -e "$destination"
then
    test -d "$destination" -a ! -L "$destination"
else
    mkdir -p -- "$destination"
fi
find "$destination" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
chmod 0755 "$destination"
printf 'cleared %s\n' "$destination"
""".strip()


RUNTIME_MATERIALIZE_SCRIPT = r"""
set -eu
key=$1
case "$key" in
    *[!0-9a-f]*|'') echo "invalid runtime key" >&2; exit 65;;
esac
test "${#key}" -eq 64
base=/icefarm-runtimes
target="$base/$key"
verify() {
    test -d "$target/root" -a ! -L "$target/root" \
        -a -f "$target/AUTHORITY.sha256" -a ! -L "$target/AUTHORITY.sha256" \
        -a -f "$target/MANIFEST.sha256" -a ! -L "$target/MANIFEST.sha256" \
        -a "$(cat "$target/AUTHORITY.sha256")" = "$key" \
        -a -x "$target/root/bin/icecc" \
        -a -x "$target/root/sbin/iceccd" \
        -a -x "$target/root/sbin/icecc-scheduler" \
        -a -x "$target/root/entry-client.sh" \
        -a -x "$target/root/entry-daemon.sh" \
        -a -x "$target/root/entry-scheduler.sh" \
        && (cd "$target/root" && sha256sum --check --strict ../MANIFEST.sha256 >/dev/null)
}
if test -e "$target"
then
    verify || { echo "existing runtime cache failed authentication" >&2; exit 65; }
    printf 'verified-existing %s\n' "$target"
    exit 0
fi
temporary="$base/.materialize-$key-$$"
test ! -e "$temporary"
mkdir -p "$temporary/root"
cp -a /opt/icecream/. "$temporary/root/"
(cd "$temporary/root" && find . -type f -print0 | sort -z | xargs -0 sha256sum) \
    >"$temporary/MANIFEST.sha256"
printf '%s\n' "$key" >"$temporary/AUTHORITY.sha256"
chmod -R a-w "$temporary/root"
mv "$temporary" "$target"
verify
printf 'materialized %s\n' "$target"
""".strip()


ACTIVATE_CORPUS_SCRIPT = r"""
set -euo pipefail
source=$1
destination=$2
archive_sha256=$3
authority=$4
manifest_sha256=$5
expected_files=$6
case "$source" in /icefarm-host/corpus-cache/active/root) ;; *) exit 65;; esac
case "$destination" in /icefarm-host/*/input) ;; *) exit 65;; esac
test -d "$source" -a ! -L "$source"
test -f "$source/../ARCHIVE.sha256" -a ! -L "$source/../ARCHIVE.sha256"
test "$(cat "$source/../ARCHIVE.sha256")" = "$archive_sha256"
test -f "$source/AUTHORITY.sha256" -a ! -L "$source/AUTHORITY.sha256"
test "$(cat "$source/AUTHORITY.sha256")" = "$authority"
test -f "$source/MANIFEST.sha256" -a ! -L "$source/MANIFEST.sha256"
test "$(sha256sum "$source/MANIFEST.sha256" | awk '{print $1}')" = "$manifest_sha256"
test "$(wc -l < "$source/MANIFEST.sha256")" -eq "$expected_files"
test -d "$destination" -a ! -L "$destination"
test -z "$(find "$destination" -mindepth 1 -maxdepth 1 -print -quit)"
cp -al -- "$source/." "$destination/"
chmod 0755 "$destination"
test "$(find "$destination" -type f | wc -l)" -eq "$((expected_files + 2))"
test -z "$(find "$destination" -type l -print -quit)"
printf 'activated %s\n' "$destination"
""".strip()


TOOLCHAIN_MATERIALIZE_SCRIPT = r"""
set -euo pipefail
key=$1
case "$key" in
    *[!0-9a-f]*|'') echo "invalid toolchain key" >&2; exit 65;;
esac
test "${#key}" -eq 64
archive="/icefarm-toolchain-archives/$key.tar.zst"
base=/icefarm-toolchains
target="$base/$key"
test "$(sha256sum "$archive" | awk '{print $1}')" = "$key"
verify() {
    test -d "$target/root" -a ! -L "$target/root" \
        -a -f "$target/AUTHORITY.sha256" -a ! -L "$target/AUTHORITY.sha256" \
        -a -f "$target/MANIFEST.sha256" -a ! -L "$target/MANIFEST.sha256" \
        -a "$(cat "$target/AUTHORITY.sha256")" = "$key" \
        && (cd "$target/root" && sha256sum --check --strict ../MANIFEST.sha256 >/dev/null)
}
if test -e "$target"
then
    verify || { echo "existing toolchain cache failed authentication" >&2; exit 65; }
    printf 'verified-existing %s\n' "$target"
    exit 0
fi
temporary="$base/.materialize-$key-$$"
test ! -e "$temporary"
mkdir -p "$temporary/root"
zstd -q -d --long=31 -c "$archive" | tar -xf - -C "$temporary/root"
(cd "$temporary/root" && find . -type f -print0 | sort -z | xargs -0 sha256sum) \
    >"$temporary/MANIFEST.sha256"
printf '%s\n' "$key" >"$temporary/AUTHORITY.sha256"
chmod -R a+rX "$temporary/root"
chmod -R a-w "$temporary/root"
mv "$temporary" "$target"
verify
printf 'materialized %s\n' "$target"
""".strip()


COMPILER_PROBE_SCRIPT = r"""
set -eu
compiler=$1
expected_binary=$2
expected_configuration=$3
expected_version=$4
test -x "$compiler"
observed_binary=$(sha256sum "$compiler" | awk '{print $1}')
test "$observed_binary" = "$expected_binary"
observed_version=$("$compiler" --version | head -n 1)
test "$observed_version" = "$expected_version"
observed_configuration=$(
    {
        printf 'dumpmachine\0'
        "$compiler" -dumpmachine
        printf 'dumpversion\0'
        "$compiler" -dumpfullversion -dumpversion
        printf 'version\0'
        "$compiler" --version
        printf 'search-dirs\0'
        "$compiler" -print-search-dirs
    } | sha256sum | awk '{print $1}'
)
test "$observed_configuration" = "$expected_configuration"
printf 'compiler-ok %s %s\n' "$observed_binary" "$observed_configuration"
""".strip()


CANARY_SCRIPT = r"""
set -euo pipefail
result_root=$1
preferred=$2
compiler=$3
shift 3
compiler_args=("$@")
mkdir -p "$result_root/env" "$result_root/canary"
environment=$(find "$result_root/env" -maxdepth 1 -type f -name '*.tar.gz' -print -quit)
if test -z "$environment"
then
    (cd "$result_root/env" && /bin/bash /opt/icecream/bin/icecc-create-env "$compiler") \
        >"$result_root/canary/create-env.log" 2>&1
    environment=$(find "$result_root/env" -maxdepth 1 -type f -name '*.tar.gz' -print -quit)
fi
test -n "$environment"
source_file="$result_root/canary/$preferred.cpp"
remote_object="$result_root/canary/$preferred.remote.o"
local_object="$result_root/canary/$preferred.local.o"
printf '%s\n' '#include <cstdint>' 'extern "C" int icefarm_canary() { return 73; }' >"$source_file"
"$compiler" "${compiler_args[@]}" -std=c++17 -c "$source_file" -o "$local_object"
ICECC_VERSION="$environment" \
ICECC_PREFERRED_HOST="$preferred" \
ICECC_TEST_REMOTEBUILD=1 \
ICECC_DEBUG=debug \
ICECC_LOGFILE="$result_root/canary/$preferred.client.log" \
    /opt/icecream/bin/icecc "$compiler" "${compiler_args[@]}" -std=c++17 \
        -c "$source_file" -o "$remote_object" \
        >"$result_root/canary/$preferred.stdout.log" 2>&1
if grep -qF '<building_local>' \
        "$result_root/canary/$preferred.client.log" \
        "$result_root/canary/$preferred.stdout.log" 2>/dev/null \
    || grep -qF 'building myself, but telling localhost' \
        "$result_root/canary/$preferred.client.log" \
        "$result_root/canary/$preferred.stdout.log" 2>/dev/null
then
    echo "readiness canary compiled locally for $preferred" >&2
    exit 70
fi
cmp "$local_object" "$remote_object"
sha256sum "$local_object" "$remote_object"
""".strip()


S30_ROTATE_CANARY_TRACE_SCRIPT = r"""
set -eu
source_path=/results/s30-mutant-f.jsonl
canary_path=/results/s30-mutant-f-canary.jsonl
expected=$1
test "$expected" -ge 1
test -f "$source_path" -a ! -L "$source_path"
test ! -e "$canary_path"
observed=$(wc -l <"$source_path")
test "$observed" -eq "$expected"
mv -- "$source_path" "$canary_path"
printf 'rotated-canary-refusals %s\n' "$observed"
""".strip()


class LifecycleError(RuntimeError):
    """The controlled lifecycle failed after validation."""


class PreflightRefusal(LifecycleError):
    """The farm cannot safely start this scenario."""

    def __init__(
        self,
        message: str,
        *,
        reason_code: str = "unsafe-preflight",
        details: dict[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.reason_code = reason_code
        self.details = dict(details or {})


class Recorder(Protocol):
    def invoke(self, command: PlannedCommand) -> CommandResult: ...


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
    peak_bytes: int


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
    instance: str | None = None,
    transport: str,
    timeout_s: int,
    argv: Iterable[str],
) -> PlannedCommand:
    return factory.make(
        phase=phase,
        host=host,
        instance=instance,
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
        raise PreflightRefusal(
            f"{subject} path {source} does not contain authority root {root}"
        )
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
        files = tuple(
            (path, _relative_under(path, root, corpus_name))
            for path in _manifest_lines(manifest)
        )
        groups.append(CorpusGroup("files", root, files))
    else:
        try:
            promotion = validate_corpus_promotion(corpus)
        except FirefoxCorpusPromotionError as exc:
            raise PreflightRefusal(
                f"corpus {corpus_name} authority receipt failed: {exc}"
            ) from exc
        relatives = tuple(PurePosixPath(row["path"]) for row in promotion.pair_rows)
        for offset, turn in enumerate(("A", "B")):
            paths = tuple(pair[offset] for pair in promotion.physical_paths)
            groups.append(
                CorpusGroup(
                    turn,
                    Path(os.path.commonpath(paths)),
                    tuple(zip(paths, relatives, strict=True)),
                )
            )
    flat = [entry for group in groups for entry in group.files]
    group_bytes = [
        sum(source.stat().st_size for source, _relative in group.files)
        for group in groups
    ]
    source_authority = {
        key: value
        for key, value in corpus.items()
        if key not in ("archives", "compression")
    }
    return CorpusLayout(
        authority_sha256=hashlib.sha256(canonical_bytes(source_authority)).hexdigest(),
        bytes=sum(source.stat().st_size for source, _relative in flat),
        groups=tuple(groups),
        peak_bytes=max(group_bytes),
    )


def _remote_corpus_archive_root(
    farm: FarmSpec, host_name: str, corpus_name: str
) -> PurePosixPath:
    return (
        PurePosixPath(farm.hosts[host_name]["scratch_root"])
        / "icefarm"
        / "corpus-archives"
        / corpus_name
    )


def _remote_corpus_cache_root(farm: FarmSpec, host_name: str) -> PurePosixPath:
    return (
        PurePosixPath(farm.hosts[host_name]["scratch_root"])
        / "icefarm"
        / "corpus-cache"
    )


def _corpus_group_for_turn(corpus: Mapping[str, Any], turn: str) -> str:
    if "manifest" in corpus:
        if turn != "A":
            raise PreflightRefusal("single-manifest corpora only define turn A")
        return "files"
    if turn not in ("A", "B"):
        raise PreflightRefusal(f"paired corpus has no turn {turn!r}")
    return turn


def _selected_corpus_archives(
    farm: FarmSpec, corpus_name: str, turns: Iterable[str]
) -> dict[str, dict[str, Any]]:
    corpus = farm.data["corpora"][corpus_name]
    result: dict[str, dict[str, Any]] = {}
    for turn in turns:
        group = _corpus_group_for_turn(corpus, turn)
        result.setdefault(group, corpus["archives"][group])
    return result


def _verify_remote_corpus_archive(
    farm: FarmSpec,
    host_name: str,
    corpus_name: str,
    archive: Mapping[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, Any]:
    path = (
        _remote_corpus_archive_root(farm, host_name, corpus_name)
        / f"{archive['archive_sha256']}.tar.zst"
    )
    result = recorder.invoke(
        _command(
            factory,
            phase="preflight.corpus-archive-verify",
            host=host_name,
            transport="ssh",
            timeout_s=timeout_s,
            argv=ssh_argv(
                farm,
                host_name,
                (
                    "python3",
                    "-c",
                    CORPUS_ARCHIVE_VERIFY_SCRIPT,
                    str(path),
                    archive["archive_sha256"],
                    str(archive["archive_bytes"]),
                ),
            ),
        )
    )
    return _json_result(result, f"corpus archive verification on {host_name}")


def _sync_corpus_archives(
    farm: FarmSpec,
    host_name: str,
    corpus_name: str,
    archives: Mapping[str, Mapping[str, Any]],
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, Any]:
    archive_root = _remote_corpus_archive_root(farm, host_name, corpus_name)
    cache_root = _remote_corpus_cache_root(farm, host_name)
    recorder.invoke(
        _command(
            factory,
            phase="preflight.corpus-archive-mkdir",
            host=host_name,
            transport="ssh",
            timeout_s=timeout_s,
            argv=ssh_argv(
                farm,
                host_name,
                (
                    "install",
                    "-d",
                    "-m",
                    "0755",
                    "--",
                    str(archive_root),
                    str(cache_root),
                ),
            ),
        )
    )
    receipts: dict[str, Any] = {}
    for group, archive in archives.items():
        source = Path(archive["archive"])
        expected = archive["archive_sha256"]
        if not source.is_file() or source.is_symlink():
            raise PreflightRefusal(
                f"corpus {corpus_name}/{group} archive is absent or a symlink: {source}"
            )
        if source.stat().st_size != archive["archive_bytes"]:
            raise PreflightRefusal(
                f"corpus {corpus_name}/{group} archive size does not match authority"
            )
        observed = _sha256(source)
        if observed != expected:
            raise PreflightRefusal(
                f"corpus {corpus_name}/{group} archive mismatch: "
                f"expected {expected}, got {observed}"
            )
        first = _verify_remote_corpus_archive(
            farm, host_name, corpus_name, archive, recorder, factory, timeout_s
        )
        mode = "verified-existing"
        if first.get("ready") is not True:
            if first.get("reason") != "absent":
                raise PreflightRefusal(
                    f"corpus {corpus_name}/{group} has corrupt content-addressed "
                    f"archive on {host_name}: {first.get('reason')}"
                )
            remote = archive_root / f"{expected}.tar.zst"
            recorder.invoke(
                _command(
                    factory,
                    phase="preflight.corpus-archive-sync",
                    host=host_name,
                    transport="rsync-ssh",
                    timeout_s=timeout_s,
                    argv=(
                        "rsync",
                        "--archive",
                        "--checksum",
                        "--protect-args",
                        "--chmod=F0444",
                        "--partial-dir=.rsync-partial",
                        str(source),
                        f"{farm.hosts[host_name]['ssh']}:{remote}",
                    ),
                )
            )
            first = _verify_remote_corpus_archive(
                farm, host_name, corpus_name, archive, recorder, factory, timeout_s
            )
            if first.get("ready") is not True:
                raise PreflightRefusal(
                    f"corpus {corpus_name}/{group} failed archive verification "
                    f"on {host_name}: {first.get('reason')}"
                )
            mode = "synced"
        receipts[group] = {"mode": mode, **first}
    return receipts


def _corpus_container_path(farm: FarmSpec, instance: Mapping[str, Any]) -> str:
    host_root = PurePosixPath(farm.hosts[instance["host"]]["scratch_root"]) / "icefarm"
    destination = (
        instance_root(farm, instance["host"], instance["run_id"], instance["name"])
        / "input"
    )
    return str(PurePosixPath("/icefarm-host") / destination.relative_to(host_root))


def _clear_corpus_input(
    farm: FarmSpec,
    instance: dict[str, Any],
    run_id: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> None:
    instance_with_run = {**instance, "run_id": run_id}
    destination = _corpus_container_path(farm, instance_with_run)
    host_destination = (
        instance_root(farm, instance["host"], run_id, instance["name"]) / "input"
    )
    scratch = PurePosixPath(farm.hosts[instance["host"]]["scratch_root"]) / "icefarm"
    recorder.invoke(
        _command(
            factory,
            phase="run.corpus-input-mkdir",
            host=instance["host"],
            transport="ssh",
            timeout_s=timeout_s,
            argv=ssh_argv(
                farm,
                instance["host"],
                ("install", "-d", "-m", "0755", "--", str(host_destination)),
            ),
        )
    )
    result = recorder.invoke(
        _command(
            factory,
            phase="run.corpus-clear",
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
                    f"icefarm-{run_id}-corpus-clear-{instance['name']}",
                    "--label",
                    f"icefarm.run={run_id}",
                    "--user",
                    "0",
                    "--mount",
                    f"type=bind,src={scratch},dst=/icefarm-host",
                    "--entrypoint",
                    "/bin/bash",
                    instance["container_image"]["reference"],
                    "-c",
                    CLEAR_CORPUS_INPUT_SCRIPT,
                    "icefarm-corpus-clear",
                    destination,
                ),
            ),
        )
    )
    if result.stdout.strip() != f"cleared {destination}":
        raise PreflightRefusal(
            f"client {instance['name']} returned malformed corpus clear receipt"
        )


def _materialize_corpus_group(
    farm: FarmSpec,
    instance: dict[str, Any],
    run_id: str,
    corpus_name: str,
    group: str,
    archive: Mapping[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, str]:
    host_name = instance["host"]
    archive_root = _remote_corpus_archive_root(farm, host_name, corpus_name)
    cache_root = _remote_corpus_cache_root(farm, host_name)
    result = recorder.invoke(
        _command(
            factory,
            phase="run.corpus-materialize",
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
                    f"icefarm-{run_id}-corpus-{archive['archive_sha256'][:12]}",
                    "--label",
                    f"icefarm.run={run_id}",
                    "--user",
                    "0",
                    "--mount",
                    f"type=bind,src={archive_root},dst=/icefarm-corpus-archives,readonly",
                    "--mount",
                    f"type=bind,src={cache_root},dst=/icefarm-corpus-cache",
                    "--entrypoint",
                    "/bin/bash",
                    instance["container_image"]["reference"],
                    "-c",
                    CORPUS_MATERIALIZE_SCRIPT,
                    "icefarm-corpus-materialize",
                    archive["archive_sha256"],
                    archive["authority_sha256"],
                    archive["manifest_sha256"],
                    group,
                    str(archive["files"]),
                    str(archive["unpacked_bytes"]),
                ),
            ),
        )
    )
    mode, separator, observed = result.stdout.strip().partition(" ")
    expected = "/icefarm-corpus-cache/active"
    if (
        mode not in ("materialized", "verified-existing")
        or separator != " "
        or observed != expected
    ):
        raise PreflightRefusal(
            f"host {host_name} returned malformed corpus materialization receipt"
        )
    return {
        "group": group,
        "mode": mode,
        "path": str(cache_root / "active"),
    }


def _activate_corpus_input(
    farm: FarmSpec,
    instance: dict[str, Any],
    run_id: str,
    archive: Mapping[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, str]:
    instance_with_run = {**instance, "run_id": run_id}
    destination = _corpus_container_path(farm, instance_with_run)
    scratch = PurePosixPath(farm.hosts[instance["host"]]["scratch_root"]) / "icefarm"
    source = "/icefarm-host/corpus-cache/active/root"
    result = recorder.invoke(
        _command(
            factory,
            phase="run.corpus-activate",
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
                    f"icefarm-{run_id}-corpus-activate-{instance['name']}",
                    "--label",
                    f"icefarm.run={run_id}",
                    "--user",
                    "0",
                    "--mount",
                    f"type=bind,src={scratch},dst=/icefarm-host",
                    "--entrypoint",
                    "/bin/bash",
                    instance["container_image"]["reference"],
                    "-c",
                    ACTIVATE_CORPUS_SCRIPT,
                    "icefarm-corpus-activate",
                    source,
                    destination,
                    archive["archive_sha256"],
                    archive["authority_sha256"],
                    archive["manifest_sha256"],
                    str(archive["files"]),
                ),
            ),
        )
    )
    if result.stdout.strip() != f"activated {destination}":
        raise PreflightRefusal(
            f"client {instance['name']} returned malformed corpus activation receipt"
        )
    return {"mode": "hardlink-view", "path": str(destination)}


def activate_corpus_turn(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    turn: str,
    recorder: Recorder,
    factory: CommandFactory,
    *,
    timeout_s: int,
) -> dict[str, Any]:
    """Materialize and expose exactly one authenticated turn on each C host."""

    corpus_name = scenario.data["workload"]["corpus"]
    corpus = farm.data["corpora"][corpus_name]
    group = _corpus_group_for_turn(corpus, turn)
    archive = corpus["archives"][group]
    clients = [
        instance
        for instance in plan["topology"]["instances"]
        if instance["role"] == "C"
        and instance["name"] in scenario.data["workload"]["clients"]
    ]
    by_host: dict[str, list[dict[str, Any]]] = {}
    for instance in clients:
        by_host.setdefault(instance["host"], []).append(instance)
    receipt: dict[str, Any] = {"group": group, "hosts": {}, "turn": turn}
    for host_name, host_clients in sorted(by_host.items()):
        for instance in host_clients:
            _clear_corpus_input(
                farm, instance, plan["run_id"], recorder, factory, timeout_s
            )
        materialized = _materialize_corpus_group(
            farm,
            host_clients[0],
            plan["run_id"],
            corpus_name,
            group,
            archive,
            recorder,
            factory,
            timeout_s,
        )
        active = {
            instance["name"]: _activate_corpus_input(
                farm,
                instance,
                plan["run_id"],
                archive,
                recorder,
                factory,
                timeout_s,
            )
            for instance in host_clients
        }
        receipt["hosts"][host_name] = {
            "active_inputs": active,
            "archive_sha256": archive["archive_sha256"],
            "materialized": materialized,
        }
    return receipt


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
            raise PreflightRefusal(
                f"host {host_name} preflight lacks typed field {key}"
            )
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
    return (
        "docker-context"
        if farm.hosts[host_name].get("docker_context")
        else "ssh-docker"
    )


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
        value = _json_result(
            inspected, f"labelled container {container_id} on {host_name}"
        )
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
            {
                "created": created,
                "id": container_id,
                "name": name[1:],
                "run_id": run_label,
            }
        )
    return containers


def _created_time(value: str) -> datetime:
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    try:
        parsed = datetime.fromisoformat(normalized)
    except ValueError as exc:
        raise PreflightRefusal(
            f"Docker returned invalid creation time {value!r}"
        ) from exc
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
        raise PreflightRefusal(
            f"stale icefarm containers require --reap-stale: {names}"
        )
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
        raise PreflightRefusal(
            f"labelled containers are newer than reap threshold: {names}"
        )
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
            raise PreflightRefusal(
                f"host {host_name} retained labelled objects after reap"
            )
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
    expected_labels = _expected_image_labels(authority)
    if not isinstance(labels, dict) or any(
        labels.get(key) != value for key, value in expected_labels.items()
    ):
        raise PreflightRefusal(
            f"host {host_name} image {label} has wrong source labels"
        )
    return {
        "bytes": size,
        "closure_sha256": identity.closure_sha256,
        "id": identity.native_id,
        "reference": reference,
    }


def _expected_image_labels(authority: Mapping[str, Any]) -> dict[str, str]:
    """Bind ordinary images to source and mutants to source plus recipe."""

    if authority.get("kind") in ("scheduler-mutant", "daemon-mutant"):
        return {
            "icefarm.source.commit": authority["base_commit"],
            "icefarm.source.archive_sha256": authority["base_archive_sha256"],
            "icefarm.mutant.patch_sha256": authority["patch_sha256"],
            "icefarm.mutant.recipe_sha256": authority["recipe_sha256"],
        }
    return {
        "icefarm.source.commit": authority["commit"],
        "icefarm.source.archive_sha256": authority["archive_sha256"],
    }


def _inspect_container_image(
    farm: FarmSpec,
    host_name: str,
    reference: str,
    expected_closure: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, Any]:
    result = recorder.invoke(
        _command(
            factory,
            phase="preflight.container-image",
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
        identity = _image_identity(
            result, f"container image {reference} on {host_name}"
        )
    except (json.JSONDecodeError, ImageError) as exc:
        raise PreflightRefusal(str(exc)) from exc
    if identity.closure_sha256 != expected_closure:
        raise PreflightRefusal(
            f"host {host_name} container image closure mismatch for {reference}: "
            f"expected {expected_closure}, got {identity.closure_sha256}"
        )
    size = raw.get("Size") if isinstance(raw, dict) else None
    if isinstance(size, bool) or not isinstance(size, int) or size < 0:
        raise PreflightRefusal(
            f"host {host_name} container image {reference} has no valid byte size"
        )
    return {
        "bytes": size,
        "closure_sha256": identity.closure_sha256,
        "id": identity.native_id,
        "reference": reference,
    }


def _materialize_runtime(
    farm: FarmSpec,
    instance: dict[str, Any],
    run_id: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, str]:
    host_name = instance["host"]
    closure = instance["image"]["closure_sha256"]
    root = runtime_root(farm, instance)
    parent = root.parent.parent
    recorder.invoke(
        _command(
            factory,
            phase="preflight.runtime-mkdir",
            host=host_name,
            transport="ssh",
            timeout_s=timeout_s,
            argv=ssh_argv(
                farm,
                host_name,
                ("install", "-d", "-m", "0755", "--", str(parent)),
            ),
        )
    )
    registry = farm.data["registry"]
    product_reference = (
        f"{registry['host']}:{registry['port']}/icefarm/icecream:"
        f"{instance['image']['label']}"
    )
    result = recorder.invoke(
        _command(
            factory,
            phase="preflight.runtime-materialize",
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
                    f"icefarm-{run_id}-runtime-{closure[:12]}",
                    "--label",
                    f"icefarm.run={run_id}",
                    "--user",
                    "0",
                    "--mount",
                    f"type=bind,src={parent},dst=/icefarm-runtimes",
                    "--entrypoint",
                    "/bin/sh",
                    product_reference,
                    "-c",
                    RUNTIME_MATERIALIZE_SCRIPT,
                    "icefarm-runtime-materialize",
                    closure,
                ),
            ),
        )
    )
    mode, separator, observed = result.stdout.strip().partition(" ")
    expected = f"/icefarm-runtimes/{closure}"
    if (
        mode not in ("materialized", "verified-existing")
        or separator != " "
        or observed != expected
    ):
        raise PreflightRefusal(
            f"host {host_name} returned malformed runtime materialization receipt"
        )
    return {"mode": mode, "path": str(root), "product_closure_sha256": closure}


SYSTEM_SOURCE_VERIFY_SCRIPT = r'''
import hashlib, json, pathlib, sys
path = pathlib.Path(sys.argv[1])
expected_bytes = int(sys.argv[2])
expected_sha256 = sys.argv[3]
if path.is_symlink():
    print(json.dumps({"status": "unsafe"}, sort_keys=True))
    raise SystemExit(0)
if not path.exists():
    print(json.dumps({"status": "absent"}, sort_keys=True))
    raise SystemExit(0)
if not path.is_file():
    print(json.dumps({"status": "unsafe"}, sort_keys=True))
    raise SystemExit(0)
digest_builder = hashlib.sha256()
with path.open("rb") as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        digest_builder.update(block)
digest = digest_builder.hexdigest()
observed = {"bytes": path.stat().st_size, "sha256": digest}
if observed["bytes"] != expected_bytes or digest != expected_sha256:
    print(json.dumps({"status": "mismatch", **observed}, sort_keys=True))
else:
    print(json.dumps({"status": "ready", **observed}, sort_keys=True))
'''.strip()


SYSTEM_SOURCE_TAR_AUDIT_SCRIPT = r'''
import pathlib, sys, tarfile

allowed = ("usr/include", "usr/lib/gcc", "usr/local/include")
for member in tarfile.open(fileobj=sys.stdin.buffer, mode="r|"):
    if "\\" in member.name:
        raise SystemExit("archive backslash path is forbidden")
    name = member.name
    parts = pathlib.PurePosixPath(name).parts
    if not name or pathlib.PurePosixPath(name).is_absolute() or ".." in parts:
        raise SystemExit("unsafe archive path")
    if not any(name == root or name.startswith(root + "/") for root in allowed):
        raise SystemExit("archive member outside authorized roots")
    if not (member.isdir() or member.isfile()):
        raise SystemExit("archive links and special files are forbidden")
'''.strip()
SYSTEM_SOURCE_TAR_AUDIT_B64 = base64.b64encode(SYSTEM_SOURCE_TAR_AUDIT_SCRIPT.encode()).decode()


SYSTEM_SOURCE_MATERIALIZE_SCRIPT = r'''#!/bin/bash
set -euo pipefail
archive_key=$1
key=$2
expected_files=$3
expected_manifest=$2
case "$archive_key" in *[!0-9a-f]*|'') exit 65;; esac
case "$key" in *[!0-9a-f]*|'') exit 65;; esac
archive="/icefarm-system-source-archives/$archive_key.tar.zst"
base=/icefarm-system-source
target="$base/$key"
test "$(sha256sum -- "$archive" | cut -d ' ' -f1)" = "$archive_key"
if test -e "$target" || test -L "$target"; then
    test -d "$target" && test ! -L "$target" || { echo 'unsafe preexisting snapshot target' >&2; exit 65; }
fi
verify_existing() {
python3 - "$1" "$expected_manifest" "$expected_files" <<'PY'
import hashlib, os, pathlib, sys
root = pathlib.Path(sys.argv[1])
expected, expected_count = sys.argv[2], int(sys.argv[3])
rows = []
def digest_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()
for top_name in ('/usr/include', '/usr/lib/gcc', '/usr/local/include'):
    top = root / top_name.lstrip('/')
    if not top.is_dir() or top.is_symlink(): raise SystemExit('unsafe snapshot root')
    for directory, dirs, files in os.walk(top, topdown=True, followlinks=False):
        if any((pathlib.Path(directory) / name).is_symlink() for name in dirs): raise SystemExit('directory symlink is unsafe')
        for name in files:
            path = pathlib.Path(directory) / name
            if path.is_symlink() or not path.is_file(): raise SystemExit('non-regular existing snapshot file')
            rows.append(('/' + path.relative_to(root).as_posix(), digest_file(path)))
rows.sort()
payload = ''.join(path + chr(9) + digest + chr(10) for path, digest in rows).encode()
if hashlib.sha256(payload).hexdigest() != expected or len(rows) != expected_count: raise SystemExit('existing snapshot manifest mismatch')
PY
}
if test -d "$target" && test ! -L "$target"; then
    verify_existing "$target"
    echo "{\"file_count\":$expected_files,\"manifest_sha256\":\"$expected_manifest\"}"
    exit 0
fi
temporary="$base/.materialize-$key-$$"
test ! -e "$temporary"
cleanup() { test -z "${temporary:-}" || rm -rf -- "$temporary"; }
trap cleanup EXIT HUP INT TERM
mkdir -p "$temporary"
zstd -q -d --long=31 -c "$archive" \
    | python3 -c "import base64,sys; exec(base64.b64decode(sys.argv[1]))" \
        __ICEFARM_AUDIT_B64__
zstd -q -d --long=31 -c "$archive" | tar -xf - -C "$temporary"
python3 - "$temporary" "$expected_manifest" "$expected_files" <<'PY'
import hashlib, os, pathlib, sys
root = pathlib.Path(sys.argv[1])
expected, expected_count = sys.argv[2], int(sys.argv[3])
roots = tuple(root / item.lstrip('/') for item in ('/usr/include', '/usr/lib/gcc', '/usr/local/include'))
rows = []
def digest_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()
for top in roots:
    if not top.is_dir() or top.is_symlink(): raise SystemExit('unsafe snapshot root')
    for directory, dirs, files in os.walk(top, topdown=True, followlinks=False):
        if any((pathlib.Path(directory) / name).is_symlink() for name in dirs): raise SystemExit('directory symlink is unsafe')
        for name in files:
            path = pathlib.Path(directory) / name
            if path.is_symlink():
                target = path.resolve()
                if not target.is_file() or not target.is_relative_to(root): raise SystemExit('unsafe file symlink')
                path.unlink()
                with target.open('rb') as source, path.open('wb') as destination:
                    for block in iter(lambda: source.read(1024 * 1024), b''):
                        destination.write(block)
            if not path.is_file() or path.is_symlink(): raise SystemExit('non-regular snapshot file')
            rows.append(('/' + path.relative_to(root).as_posix(), digest_file(path)))
rows.sort()
payload = ''.join(path + chr(9) + digest + chr(10) for path, digest in rows).encode()
if hashlib.sha256(payload).hexdigest() != expected or len(rows) != expected_count: raise SystemExit('snapshot manifest mismatch')
(root / 'MANIFEST.sha256').write_bytes(payload)
PY
# The archive is opened independently for audit and extraction.  Refuse a
# concurrent replacement before publishing the materialized tree.
test "$(sha256sum -- "$archive" | cut -d ' ' -f1)" = "$archive_key"
chmod -R a-w "$temporary"
mv "$temporary" "$target"
temporary=
trap - EXIT HUP INT TERM
echo "{\"file_count\":$expected_files,\"manifest_sha256\":\"$expected_manifest\"}"
'''.strip().replace("__ICEFARM_AUDIT_B64__", SYSTEM_SOURCE_TAR_AUDIT_B64)


def _materialize_system_source_snapshot(
    farm: FarmSpec,
    instance: dict[str, Any],
    snapshot: Mapping[str, Any],
    run_id: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, Any]:
    try:
        local = verify_local_archive(snapshot)
    except SystemSourceSnapshotError as exc:
        raise PreflightRefusal(str(exc), reason_code="system-source-archive") from exc
    host_name = instance["host"]
    key = snapshot["manifest_sha256"]
    archive_key = snapshot["archive"]["sha256"]
    scratch = PurePosixPath(farm.hosts[host_name]["scratch_root"]) / "icefarm"
    archive_parent = scratch / "system-source-archives"
    materialized_parent = scratch / "system-source-snapshots"
    private = instance["role"] == "F"
    if private:
        materialized_parent = private_root(farm.hosts[host_name]["scratch_root"], key, run_id, instance["name"]).parent
    remote_archive = archive_parent / f"{archive_key}.tar.zst"
    recorder.invoke(_command(factory, phase="preflight.system-source-mkdir", host=host_name, transport="ssh", timeout_s=timeout_s, argv=ssh_argv(farm, host_name, ("install", "-d", "-m", "0755", "--", str(archive_parent), str(materialized_parent)))))
    first = recorder.invoke(_command(factory, phase="preflight.system-source-verify", host=host_name, transport="ssh", timeout_s=timeout_s, argv=ssh_argv(farm, host_name, ("python3", "-c", SYSTEM_SOURCE_VERIFY_SCRIPT, str(remote_archive), str(snapshot["archive"]["archive_bytes"]), archive_key))))
    def parse_archive_status(result: CommandResult) -> dict[str, Any]:
        if result.returncode != 0:
            raise PreflightRefusal("remote system-source verifier failed", reason_code="system-source-archive")
        try:
            value = json.loads(result.stdout.strip())
        except (TypeError, ValueError) as exc:
            raise PreflightRefusal("remote system-source verifier returned malformed JSON", reason_code="system-source-archive") from exc
        if not isinstance(value, dict) or not isinstance(value.get("status"), str):
            raise PreflightRefusal("remote system-source verifier returned malformed status", reason_code="system-source-archive")
        status = value["status"]
        if status in {"absent", "unsafe"}:
            if set(value) != {"status"}:
                raise PreflightRefusal("remote system-source verifier returned extra status fields", reason_code="system-source-archive")
        elif status in {"ready", "mismatch"}:
            if set(value) != {"status", "bytes", "sha256"}:
                raise PreflightRefusal("remote system-source verifier returned incomplete status fields", reason_code="system-source-archive")
            if isinstance(value["bytes"], bool) or not isinstance(value["bytes"], int) or value["bytes"] < 0:
                raise PreflightRefusal("remote system-source verifier returned invalid byte count", reason_code="system-source-archive")
            if not isinstance(value["sha256"], str) or len(value["sha256"]) != 64 or any(character not in "0123456789abcdef" for character in value["sha256"]):
                raise PreflightRefusal("remote system-source verifier returned invalid digest", reason_code="system-source-archive")
        else:
            raise PreflightRefusal("remote system-source verifier returned unknown status", reason_code="system-source-archive")
        return value

    first_status = parse_archive_status(first)
    if first_status["status"] == "ready":
        if first_status["bytes"] != snapshot["archive"]["archive_bytes"] or first_status["sha256"] != archive_key:
            raise PreflightRefusal("remote system-source archive ready receipt differs", reason_code="system-source-archive")
    elif first_status["status"] != "absent":
        raise PreflightRefusal("remote system-source archive is unsafe or mismatched", reason_code="system-source-archive")
    if first_status["status"] == "absent":
        recorder.invoke(_command(factory, phase="preflight.system-source-sync", host=host_name, transport="rsync-ssh", timeout_s=timeout_s, argv=("rsync", "--archive", "--checksum", "--protect-args", str(local["path"]), f"{farm.hosts[host_name]['ssh']}:{remote_archive}")))
        verified = recorder.invoke(_command(factory, phase="preflight.system-source-verify", host=host_name, transport="ssh", timeout_s=timeout_s, argv=ssh_argv(farm, host_name, ("python3", "-c", SYSTEM_SOURCE_VERIFY_SCRIPT, str(remote_archive), str(snapshot["archive"]["archive_bytes"]), archive_key))))
        verified_status = parse_archive_status(verified)
        if verified_status["status"] != "ready" or verified_status["bytes"] != snapshot["archive"]["archive_bytes"] or verified_status["sha256"] != archive_key:
            raise PreflightRefusal("remote system-source archive did not verify after sync", reason_code="system-source-archive")
    argv = docker_argv(
        farm,
        host_name,
        (
            "run", "--rm", "--pull=never", "--name",
            f"icefarm-{run_id}-system-source-{instance['name']}-{key[:12]}",
            "--label", f"icefarm.run={run_id}", "--user", "0",
            "--mount", f"type=bind,src={archive_parent},dst=/icefarm-system-source-archives,readonly",
            "--mount", f"type=bind,src={materialized_parent},dst=/icefarm-system-source",
            "--entrypoint", "/bin/bash", instance["container_image"]["reference"],
            "-c", SYSTEM_SOURCE_MATERIALIZE_SCRIPT,
            "icefarm-system-source-materialize", archive_key,
            snapshot["manifest_sha256"], str(snapshot["file_count"]),
        ),
    )
    result = recorder.invoke(_command(
        factory,
        phase="preflight.system-source-materialize",
        host=host_name,
        transport=_docker_transport(farm, host_name),
        timeout_s=timeout_s,
        argv=argv,
    ))
    try:
        if result.returncode != 0:
            raise ValueError(f"materializer exited with rc={result.returncode}")
        observed = json.loads(result.stdout.strip())
        if (
            not isinstance(observed, dict)
            or set(observed) != {"file_count", "manifest_sha256"}
            or isinstance(observed["file_count"], bool)
            or not isinstance(observed["file_count"], int)
            or observed["file_count"] < 1
            or not isinstance(observed["manifest_sha256"], str)
            or len(observed["manifest_sha256"]) != 64
            or any(character not in "0123456789abcdef" for character in observed["manifest_sha256"])
        ):
            raise ValueError("malformed materialization receipt")
        verified = verified_materialization(snapshot, farm.hosts[host_name]["scratch_root"], observed["manifest_sha256"], observed["file_count"])
        if private:
            root = materialized_parent / key
            verified["root"] = str(root)
            verified["mounts"] = {destination: str(root / destination.lstrip("/")) for destination in verified["mounts"]}
            verified["instance"] = instance["name"]
            verified["scope"] = "run-instance"
    except (ValueError, KeyError, TypeError, SystemSourceSnapshotError) as exc:
        raise PreflightRefusal(f"system-source materialization failed: {exc}", reason_code="system-source-manifest") from exc
    return {"archive_sha256": archive_key, "archive_bytes": snapshot["archive"]["archive_bytes"], "host": host_name, **verified}


def _materialize_toolchain(
    farm: FarmSpec,
    instance: dict[str, Any],
    run_id: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, Any] | None:
    recipe = instance.get("compiler_recipe")
    toolchain = recipe.get("toolchain") if isinstance(recipe, dict) else None
    root = toolchain_root(farm, instance)
    if not isinstance(toolchain, dict) or root is None:
        return None
    archive = Path(toolchain["archive"])
    if not archive.is_file() or archive.is_symlink():
        raise PreflightRefusal(
            f"client {instance['name']} toolchain archive is absent or a symlink: {archive}"
        )
    expected_bytes = toolchain["archive_bytes"]
    observed_bytes = archive.stat().st_size
    if observed_bytes != expected_bytes:
        raise PreflightRefusal(
            f"client {instance['name']} toolchain archive size mismatch: "
            f"expected {expected_bytes}, got {observed_bytes}"
        )
    observed_local = _sha256(archive)
    expected = toolchain["archive_sha256"]
    if observed_local != expected:
        raise PreflightRefusal(
            f"client {instance['name']} toolchain archive mismatch: "
            f"expected {expected}, got {observed_local}"
        )
    host_name = instance["host"]
    scratch = PurePosixPath(farm.hosts[host_name]["scratch_root"]) / "icefarm"
    archive_parent = scratch / "toolchain-archives"
    remote_archive = archive_parent / f"{expected}.tar.zst"
    materialized_parent = root.parent.parent
    recorder.invoke(
        _command(
            factory,
            phase="preflight.toolchain-mkdir",
            host=host_name,
            transport="ssh",
            timeout_s=timeout_s,
            argv=ssh_argv(
                farm,
                host_name,
                (
                    "install",
                    "-d",
                    "-m",
                    "0755",
                    "--",
                    str(archive_parent),
                    str(materialized_parent),
                ),
            ),
        )
    )
    recorder.invoke(
        _command(
            factory,
            phase="preflight.toolchain-sync",
            host=host_name,
            transport="rsync-ssh",
            timeout_s=timeout_s,
            argv=(
                "rsync",
                "--archive",
                "--checksum",
                "--protect-args",
                str(archive),
                f"{farm.hosts[host_name]['ssh']}:{remote_archive}",
            ),
        )
    )
    remote_digest = recorder.invoke(
        _command(
            factory,
            phase="preflight.toolchain-verify",
            host=host_name,
            transport="ssh",
            timeout_s=timeout_s,
            argv=ssh_argv(farm, host_name, ("sha256sum", "--", str(remote_archive))),
        )
    ).stdout.split()
    if not remote_digest or remote_digest[0] != expected:
        raise PreflightRefusal(
            f"client {instance['name']} remote toolchain archive mismatch"
        )
    result = recorder.invoke(
        _command(
            factory,
            phase="preflight.toolchain-materialize",
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
                    f"icefarm-{run_id}-toolchain-{expected[:12]}",
                    "--label",
                    f"icefarm.run={run_id}",
                    "--user",
                    "0",
                    "--mount",
                    f"type=bind,src={archive_parent},"
                    "dst=/icefarm-toolchain-archives,readonly",
                    "--mount",
                    f"type=bind,src={materialized_parent},dst=/icefarm-toolchains",
                    "--entrypoint",
                    "/bin/bash",
                    instance["container_image"]["reference"],
                    "-c",
                    TOOLCHAIN_MATERIALIZE_SCRIPT,
                    "icefarm-toolchain-materialize",
                    expected,
                ),
            ),
        )
    )
    mode, separator, observed = result.stdout.strip().partition(" ")
    if (
        mode not in ("materialized", "verified-existing")
        or separator != " "
        or observed != f"/icefarm-toolchains/{expected}"
    ):
        raise PreflightRefusal(
            f"client {instance['name']} returned malformed toolchain receipt"
        )
    return {
        "archive": str(archive),
        "archive_sha256": expected,
        "bytes": expected_bytes,
        "compression": dict(toolchain["compression"]),
        "mode": mode,
        "path": str(root),
    }


def _probe_compiler(
    farm: FarmSpec,
    instance: dict[str, Any],
    run_id: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, str]:
    recipe = instance["compiler_recipe"]
    mounts: list[str] = []
    root = toolchain_root(farm, instance)
    if root is not None:
        mounts.extend(
            (
                "--mount",
                f"type=bind,src={root},dst={recipe['toolchain']['mount']},readonly",
            )
        )
    probe_id = hashlib.sha256(
        canonical_bytes(
            {
                "container": instance["container_image"],
                "recipe": recipe,
            }
        )
    ).hexdigest()[:12]
    result = recorder.invoke(
        _command(
            factory,
            phase="preflight.compiler",
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
                    f"icefarm-{run_id}-compiler-{probe_id}",
                    "--label",
                    f"icefarm.run={run_id}",
                    "--user",
                    "0",
                    *mounts,
                    "--entrypoint",
                    "/bin/bash",
                    instance["container_image"]["reference"],
                    "-c",
                    COMPILER_PROBE_SCRIPT,
                    "icefarm-compiler-probe",
                    recipe["executable"],
                    recipe["binary_sha256"],
                    recipe["configuration_sha256"],
                    recipe["version"],
                ),
            ),
        )
    )
    fields = result.stdout.strip().split()
    if fields != [
        "compiler-ok",
        recipe["binary_sha256"],
        recipe["configuration_sha256"],
    ]:
        raise PreflightRefusal(
            f"client {instance['name']} returned malformed compiler receipt"
        )
    return {
        "binary_sha256": recipe["binary_sha256"],
        "configuration_sha256": recipe["configuration_sha256"],
        "executable": recipe["executable"],
        "version": recipe["version"],
    }


def _probe_chroot(
    farm: FarmSpec,
    host_name: str,
    reference: str,
    run_id: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> None:
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


def _probe_netem_tool(
    farm: FarmSpec,
    instance: Mapping[str, Any],
    run_id: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> None:
    """Refuse before ``up`` when the sealed worker image lacks ``tc``."""

    reference = instance["container_image"]["reference"]
    try:
        result = recorder.invoke(
            _command(
                factory,
                phase="preflight.netem-tool",
                host=instance["host"],
                instance=instance["name"],
                transport=_docker_transport(farm, instance["host"]),
                timeout_s=timeout_s,
                argv=docker_argv(
                    farm,
                    instance["host"],
                    (
                        "run",
                        "--rm",
                        "--pull=never",
                        "--network",
                        "none",
                        "--cap-drop",
                        "ALL",
                        "--entrypoint",
                        "/usr/sbin/tc",
                        reference,
                        "qdisc",
                        "show",
                        "dev",
                        "lo",
                    ),
                ),
            )
        )
    except RemoteError as exc:
        raise PreflightRefusal(
            f"shaped worker {instance['name']} image lacks a usable /usr/sbin/tc",
            reason_code="netem-tool-missing",
        ) from exc
    if result.returncode != 0:
        raise PreflightRefusal(
            f"shaped worker {instance['name']} image lacks a usable /usr/sbin/tc",
            reason_code="netem-tool-missing",
        )


def _probe_role_hashes(
    farm: FarmSpec,
    host_name: str,
    product_label: str,
    container_reference: str,
    mounted_runtime: PurePosixPath,
    expected: dict[str, str],
    run_id: str,
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> dict[str, str]:
    roles = sorted(expected)
    probe_id = hashlib.sha256((host_name + product_label).encode()).hexdigest()[:12]
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
                    "--mount",
                    f"type=bind,src={mounted_runtime},dst=/opt/icecream,readonly",
                    "--entrypoint",
                    "/usr/bin/sha256sum",
                    container_reference,
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
                f"host {host_name} product {product_label} returned malformed role hashes"
            )
        observed[fields[1]] = fields[0]
    for role in roles:
        path = ROLE_BINARY_PATHS[role]
        if observed.get(path) != expected[role]:
            raise PreflightRefusal(
                f"host {host_name} product {product_label} {role} hash mismatch: "
                f"expected {expected[role]}, got {observed.get(path)}",
                reason_code="role-hash-mismatch",
                details={
                    "expected_sha256": expected[role],
                    "host": host_name,
                    "observed_sha256": observed.get(path),
                    "product": product_label,
                    "role": role,
                },
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
    netem_targets = {binding.instance for binding in _netem_bindings(plan)}
    for instance in topology["instances"]:
        authority = farm.data["authority"]["images"][instance["image"]["label"]]
        if authority.get("kind") in ("scheduler-mutant", "daemon-mutant"):
            overrides = authority.get("role_overrides")
            mutant_role = "S" if authority.get("kind") == "scheduler-mutant" else "F"
            role_key = "scheduler" if mutant_role == "S" else "daemon"
            role_override = overrides.get(role_key) if isinstance(overrides, dict) else None
            digest = role_override.get("sha256") if isinstance(role_override, dict) else None
            if instance["role"] == mutant_role and (
                not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None
            ):
                raise PreflightRefusal(
                    f"mutant {role_key} {instance['image']['label']} lacks an observed {role_key} role hash",
                    reason_code="role-hash-mismatch",
                    details={
                        "expected_sha256": None,
                        "observed_sha256": None,
                        "product": instance["image"]["label"],
                        "role": mutant_role,
                    },
                )
        if not isinstance(authority.get("closure_sha256"), str):
            raise PreflightRefusal(
                f"image {instance['image']['label']} has no captured runtime closure "
                "in the execution authority"
            )
        if instance["name"] in netem_targets:
            _probe_netem_tool(
                farm,
                instance,
                plan["run_id"],
                recorder,
                factory,
                timeout_s,
            )
    verified_snapshots: dict[tuple[str, str, str], dict[str, Any]] = {}
    for instance in topology["instances"]:
        snapshot = instance.get("system_source_snapshot")
        if snapshot is None:
            continue
        snapshot_key = (snapshot["name"], instance["host"], instance["name"] if instance["role"] == "F" else "")
        if snapshot_key in verified_snapshots:
            continue
        authority_snapshot = farm.data.get("system_source_snapshots", {}).get(snapshot["name"])
        if not isinstance(authority_snapshot, Mapping):
            raise PreflightRefusal(
                f"system-source snapshot {snapshot['name']!r} is absent from authority",
                reason_code="system-source-authority",
            )
        verified_snapshots[snapshot_key] = _materialize_system_source_snapshot(
            farm, instance, authority_snapshot, plan["run_id"], recorder, factory, timeout_s
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
    selected_archives = _selected_corpus_archives(
        farm,
        scenario.data["workload"]["corpus"],
        scenario.data["workload"]["turns"],
    )
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
    toolchain_bytes = {host_name: 0 for host_name in hosts}
    counted_toolchains: set[tuple[str, str]] = set()
    counted_images: set[tuple[str, str]] = set()
    role_hashes: dict[tuple[str, str, str], dict[str, str]] = {}
    role_instances: dict[tuple[str, str, str], dict[str, Any]] = {}
    runtime_instances: dict[tuple[str, str], dict[str, Any]] = {}
    chroot_images: set[tuple[str, str]] = set()
    client_hosts = {
        instance["host"]
        for instance in topology["instances"]
        if instance["role"] == "C"
    }
    for instance in topology["instances"]:
        host_name = instance["host"]
        product_label = instance["image"]["label"]
        product_key = f"product:{host_name}:{product_label}"
        if product_key not in image_receipts:
            image_receipts[product_key] = _inspect_required_image(
                farm,
                host_name,
                product_label,
                recorder,
                factory,
                timeout_s,
            )
            product_reference = image_receipts[product_key]["reference"]
            counted_key = (host_name, product_reference)
            if counted_key not in counted_images:
                image_bytes[host_name] += image_receipts[product_key]["bytes"]
                counted_images.add(counted_key)
        container = instance["container_image"]
        container_key = f"container:{host_name}:{container['reference']}"
        if container_key not in image_receipts:
            image_receipts[container_key] = _inspect_container_image(
                farm,
                host_name,
                container["reference"],
                container["closure_sha256"],
                recorder,
                factory,
                timeout_s,
            )
            counted_key = (host_name, container["reference"])
            if counted_key not in counted_images:
                image_bytes[host_name] += image_receipts[container_key]["bytes"]
                counted_images.add(counted_key)
        runtime_instances.setdefault((host_name, product_label), instance)
        group_key = (host_name, product_label, container["reference"])
        role_instances.setdefault(group_key, instance)
        expected = role_hashes.setdefault(group_key, {})
        previous = expected.setdefault(instance["role"], instance["sha256"])
        if previous != instance["sha256"]:
            raise PreflightRefusal(
                f"conflicting {instance['role']} hashes for product {product_label}"
            )
        if instance["role"] == "F":
            chroot_images.add((host_name, container["reference"]))
        if instance["role"] == "C" and "toolchain" in instance["compiler_recipe"]:
            toolchain = instance["compiler_recipe"]["toolchain"]
            counted_toolchain = (host_name, toolchain["archive_sha256"])
            if counted_toolchain not in counted_toolchains:
                toolchain_bytes[host_name] += (
                    toolchain["archive_bytes"] + toolchain["unpacked_bytes"]
                )
                counted_toolchains.add(counted_toolchain)

    for host_name in hosts:
        corpus_bytes = (
            max(archive["unpacked_bytes"] for archive in selected_archives.values())
            + sum(archive["archive_bytes"] for archive in selected_archives.values())
            if host_name in client_hosts
            else 0
        )
        required = (
            MIN_FREE_BYTES
            + corpus_bytes
            + image_bytes[host_name]
            + toolchain_bytes[host_name]
        )
        if host_facts[host_name]["free_bytes"] < required:
            raise PreflightRefusal(
                f"host {host_name} has {host_facts[host_name]['free_bytes']} free bytes; "
                f"needs corpus {corpus_bytes}, images {image_bytes[host_name]}, "
                f"toolchains {toolchain_bytes[host_name]}, "
                f"and {MIN_FREE_BYTES} working bytes"
            )
        host_facts[host_name]["corpus_bytes"] = corpus_bytes
        host_facts[host_name]["image_bytes"] = image_bytes[host_name]
        host_facts[host_name]["toolchain_bytes"] = toolchain_bytes[host_name]
        host_facts[host_name]["required_free_before_sync"] = required

    runtime_receipts = {
        f"{host_name}:{label}": _materialize_runtime(
            farm, instance, plan["run_id"], recorder, factory, timeout_s
        )
        for (host_name, label), instance in sorted(runtime_instances.items())
    }

    for host_name, reference in sorted(chroot_images):
        _probe_chroot(
            farm,
            host_name,
            reference,
            plan["run_id"],
            recorder,
            factory,
            timeout_s,
        )

    role_receipts: dict[str, Any] = {}
    for (host_name, label, reference), expected in sorted(role_hashes.items()):
        representative = role_instances[(host_name, label, reference)]
        role_receipts[f"{host_name}:{label}:{reference}"] = _probe_role_hashes(
            farm,
            host_name,
            label,
            reference,
            runtime_root(farm, representative),
            expected,
            plan["run_id"],
            recorder,
            factory,
            timeout_s,
        )

    toolchain_receipts: dict[str, Any] = {}
    compiler_receipts: dict[str, Any] = {}
    materialized_toolchains: set[tuple[str, str]] = set()
    for instance in topology["instances"]:
        if instance["role"] != "C":
            continue
        toolchain = instance["compiler_recipe"].get("toolchain")
        if isinstance(toolchain, dict):
            key = (instance["host"], toolchain["archive_sha256"])
            if key not in materialized_toolchains:
                receipt = _materialize_toolchain(
                    farm,
                    instance,
                    plan["run_id"],
                    recorder,
                    factory,
                    timeout_s,
                )
                toolchain_receipts[f"{key[0]}:{key[1]}"] = receipt
                materialized_toolchains.add(key)
        compiler_receipts[instance["name"]] = _probe_compiler(
            farm,
            instance,
            plan["run_id"],
            recorder,
            factory,
            timeout_s,
        )

    corpus_receipts: dict[str, Any] = {}
    active_inputs: dict[str, Any] = {}
    active_turn: dict[str, Any] | None = None
    if sync_corpora:
        for host_name in sorted(client_hosts):
            corpus_receipts[host_name] = _sync_corpus_archives(
                farm,
                host_name,
                scenario.data["workload"]["corpus"],
                selected_archives,
                recorder,
                factory,
                timeout_s,
            )
        first_turn = scenario.data["workload"]["turns"][0]
        active_turn = activate_corpus_turn(
            farm,
            scenario,
            plan,
            first_turn,
            recorder,
            factory,
            timeout_s=timeout_s,
        )
        for host_receipt in active_turn["hosts"].values():
            active_inputs.update(host_receipt["active_inputs"])
        for host_name in sorted(client_hosts):
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
            "active_turn": active_turn,
            "authority_sha256": layout.authority_sha256,
            "bytes": layout.bytes,
            "active_inputs": active_inputs,
            "hosts": corpus_receipts,
            "name": scenario.data["workload"]["corpus"],
            "peak_bytes": layout.peak_bytes,
        },
        "farm_digest": farm.digest,
        "hosts": host_facts,
        "images": image_receipts,
        "compilers": compiler_receipts,
        "role_hashes": role_receipts,
        "runtimes": runtime_receipts,
        "system_sources": [
            receipt
            for _key, receipt in sorted(verified_snapshots.items())
        ],
        "toolchains": toolchain_receipts,
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


def _netem_bindings(plan: Mapping[str, Any]) -> tuple[NetemBinding, ...]:
    """Authenticate the immutable plan before using any netem argv."""

    document = plan.get("network_shaping")
    if document is None:
        return ()
    try:
        values = validate_netem_plan(document)
    except NetemPlanError as exc:
        raise LifecycleError(f"netem plan is invalid: {exc}") from exc
    topology = plan.get("topology", {}).get("instances", [])
    by_name = {item.get("name"): item for item in topology if isinstance(item, Mapping)}
    ports = plan.get("ports", {}).get("instances", {})
    result: list[NetemBinding] = []
    for value in values:
        name = value["instance"]
        instance = by_name.get(name)
        if not isinstance(instance, Mapping) or instance.get("role") != "F":
            raise LifecycleError(f"netem plan target {name!r} is not the planned F")
        if instance.get("host") != value["host"]:
            raise LifecycleError(f"netem plan host mismatch for {name!r}")
        if ports.get(name) != value["container_port"] or value["host_port"] != ports.get(name):
            raise LifecycleError(f"netem plan port mismatch for {name!r}")
        expected_container = f"icefarm-{plan['run_id']}-{name}"
        expected_bridge = f"icefarm-{plan['run_id']}-{name}-netem"
        if value["container"] != expected_container or value["bridge"] != expected_bridge:
            raise LifecycleError(f"netem plan identity mismatch for {name!r}")
        result.append(
            NetemBinding(
                instance=name,
                role="F",
                host=value["host"],
                bridge=value["bridge"],
                rate=value["rate"],
                delay_ms=value["delay_ms"],
                host_port=value["host_port"],
                container_port=value["container_port"],
                container=value["container"],
                direction=value["direction"],
            )
        )
    return tuple(result)


def _netem_up_receipt(
    run_id: str,
    bindings: tuple[NetemBinding, ...],
    create_results: list[CommandResult],
    apply_results: list[CommandResult],
    observe_results: list[CommandResult],
    create_commands: list[PlannedCommand],
    apply_commands: list[PlannedCommand],
    observe_commands: list[PlannedCommand],
) -> dict[str, Any]:
    if not (
        len(bindings)
        == len(create_results)
        == len(apply_results)
        == len(observe_results)
        == len(create_commands)
        == len(apply_commands)
        == len(observe_commands)
    ):
        raise LifecycleError("netem command/result cardinality is inconsistent")
    records: list[dict[str, Any]] = []
    for binding, created, applied, observed, create, apply, observe in zip(
        bindings,
        create_results,
        apply_results,
        observe_results,
        create_commands,
        apply_commands,
        observe_commands,
        strict=True,
    ):
        if created.returncode != 0 or applied.returncode != 0 or observed.returncode != 0:
            raise LifecycleError(f"netem command failed for {binding.instance}")
        network_id = created.stdout.strip()
        if re.fullmatch(r"[0-9a-f]{64}", network_id) is None:
            raise LifecycleError(
                f"netem bridge creation returned no exact network id for {binding.instance}"
            )
        witness = validate_qdisc(binding, observed.stdout)
        records.append(
            {
                "application": {
                    "argv": list(apply.argv),
                    "returncode": applied.returncode,
                },
                "bridge": {
                    "argv": list(create.argv),
                    "network_id": network_id,
                    "returncode": created.returncode,
                },
                "container": binding.container,
                "instance": binding.instance,
                "request": binding.as_dict(),
                "removal": {
                    "network_inspect_argv": list(network_inspect_args(network_id)),
                    "network_list_argv": list(
                        network_list_args(binding, run_id)
                    ),
                    "network_remove_argv": list(network_remove_args(network_id)),
                },
                "observation": {
                    "argv": list(observe.argv),
                    "returncode": observed.returncode,
                    "sha256": hashlib.sha256(observed.stdout.encode()).hexdigest(),
                    "witness": witness,
                },
            }
        )
    return {
        "bindings": records,
        "schema": NETEM_RECEIPT_SCHEMA,
        "status": "APPLIED" if records else "DISABLED",
    }


def _netem_created_receipt(
    run_id: str,
    scenario_digest: str,
    topology_digest: str,
    bindings: tuple[NetemBinding, ...],
    create_results: list[CommandResult],
    create_commands: list[PlannedCommand],
) -> dict[str, Any]:
    """Retain successful network creation before any dependent step runs."""

    if len(bindings) != len(create_results) or len(bindings) != len(create_commands):
        raise LifecycleError("netem create command/result cardinality is inconsistent")
    records: list[dict[str, Any]] = []
    for binding, created, create in zip(
        bindings, create_results, create_commands, strict=True
    ):
        if created.returncode != 0:
            continue
        network_id = created.stdout.strip()
        if re.fullmatch(r"[0-9a-f]{64}", network_id) is None:
            raise LifecycleError(
                f"netem bridge creation returned no exact network id for {binding.instance}"
            )
        records.append(
            {
                "bridge": {
                    "argv": list(create.argv),
                    "network_id": network_id,
                    "returncode": created.returncode,
                },
                "container": binding.container,
                "instance": binding.instance,
                "removal": {
                    "network_inspect_argv": list(network_inspect_args(network_id)),
                    "network_list_argv": list(network_list_args(binding, run_id)),
                    "network_remove_argv": list(network_remove_args(network_id)),
                },
                "request": binding.as_dict(),
            }
        )
    return {
        "bindings": records,
        "schema": NETEM_RECEIPT_SCHEMA,
        "status": "CREATED" if records else "NOT_APPLIED",
        "scenario_digest": scenario_digest,
        "topology_digest": topology_digest,
    }


def _timeout_left(deadline: float, monotonic: Callable[[], float]) -> int:
    return max(1, int(deadline - monotonic() + 0.999))


def _scheduler_snapshot(
    plan: dict[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> str:
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
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
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )

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


def _wait_clients(
    farm: FarmSpec,
    plan: dict[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    *,
    deadline: float,
    monotonic: Callable[[], float],
    sleeper: Callable[[float], None],
) -> dict[str, Any]:
    clients = sorted(
        (item for item in plan["topology"]["instances"] if item["role"] == "C"),
        key=lambda item: item["name"],
    )
    cache_clients = [
        item
        for item in clients
        if item["version"] == 50 and item["env"].get("ICECC_P50_MODE") == "on"
    ]
    latest: dict[str, Any] = {}

    def probe() -> tuple[bool, str]:
        nonlocal latest
        for client in clients:
            _assert_running(
                farm,
                client["host"],
                f"icefarm-{plan['run_id']}-{client['name']}",
                recorder,
                factory,
                _timeout_left(deadline, monotonic),
            )
        latest = {
            client["name"]: {
                "ready": True,
                "reason": "legacy-or-cache-disabled",
            }
            for client in clients
            if client not in cache_clients
        }
        for client in cache_clients:
            log_path = (
                instance_root(farm, client["host"], plan["run_id"], client["name"])
                / "log"
                / "client-daemon.log"
            )
            result = recorder.invoke(
                _command(
                    factory,
                    phase="readiness.client-cache",
                    host=client["host"],
                    transport="ssh",
                    timeout_s=min(_timeout_left(deadline, monotonic), 5),
                    argv=ssh_argv(
                        farm,
                        client["host"],
                        ("python3", "-c", CLIENT_CACHE_READY_SCRIPT, str(log_path)),
                    ),
                )
            )
            latest[client["name"]] = _json_result(
                result, f"client cache readiness {client['name']}"
            )
        ready = all(item.get("ready") is True for item in latest.values())
        return ready, json.dumps(latest, sort_keys=True)

    names = ",".join(item["name"] for item in cache_clients) or "none-required"
    _wait_for(
        "client cache READY: " + names,
        probe,
        deadline=deadline,
        monotonic=monotonic,
        sleeper=sleeper,
    )
    return latest


def _run_canaries(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    *,
    deadline: float,
    monotonic: Callable[[], float],
) -> dict[str, dict[str, str]]:
    instances = {item["name"]: item for item in plan["topology"]["instances"]}
    clients = [instances[name] for name in sorted(scenario.data["workload"]["clients"])]
    workers = sorted(
        (item for item in plan["topology"]["instances"] if item["role"] == "F"),
        key=lambda item: item["name"],
    )
    results: dict[str, dict[str, str]] = {}
    for client in clients:
        client_name = client["name"]
        container = f"icefarm-{plan['run_id']}-{client_name}"
        _assert_running(
            farm,
            client["host"],
            container,
            recorder,
            factory,
            _timeout_left(deadline, monotonic),
        )
        client_results: dict[str, str] = {}
        for worker in workers:
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
                            "--user",
                            "65534:65534",
                            container,
                            "/bin/bash",
                            "-c",
                            CANARY_SCRIPT,
                            "icefarm-canary",
                            "/results",
                            worker["name"],
                            client["compiler_recipe"]["executable"],
                            *client["compiler_recipe"]["arguments"],
                        ),
                    ),
                )
            )
            client_results[worker["name"]] = result.stdout.strip()
        results[client_name] = client_results
    return results


H3_ARM_SCRIPT = r"""
import hashlib, json, os, pathlib, sys
marker = pathlib.Path(sys.argv[1])
run_id, scheduler, contract = sys.argv[2:5]
if marker.exists() or marker.is_symlink():
    raise SystemExit("H3 already armed or stale marker")
trace = marker.parent / "h3-mutant.jsonl"
if trace.exists() or trace.is_symlink():
    raise SystemExit("H3 emitted a trace before arming")
log = pathlib.Path("/var/log/icecream/scheduler.log")
if log.is_symlink():
    raise SystemExit("H3 scheduler log is a symlink")
prefix = log.read_bytes()
if not prefix or not prefix.endswith(b"\n"):
    raise SystemExit("H3 boundary requires complete scheduler log lines")
receipt = {"schema": contract, "run_id": run_id, "scheduler": scheduler,
           "scheduler_log_bytes": len(prefix),
           "scheduler_log_lines": prefix.count(b"\n"),
           "scheduler_log_sha256": hashlib.sha256(prefix).hexdigest()}
payload = json.dumps(receipt, sort_keys=True).encode() + b"\n"
temporary = marker.with_suffix(".pending")
with temporary.open("xb") as handle:
    handle.write(payload)
    handle.flush()
    os.fsync(handle.fileno())
os.link(temporary, marker)
temporary.unlink()
print(payload.decode(), end="")
""".strip()


def _arm_h3_mutant(farm, plan, recorder, factory, *, timeout_s):
    contract = plan.get("h3_arm_contract")
    if contract is None:
        return None
    if contract != MUTANT_ARM_CONTRACT:
        raise LifecycleError("unknown H3 arming contract")
    scheduler = next(item for item in plan["topology"]["instances"] if item["role"] == "S")
    result = recorder.invoke(_command(
        factory, phase="readiness.h3-arm", host=scheduler["host"],
        transport=_docker_transport(farm, scheduler["host"]), timeout_s=timeout_s,
        argv=docker_argv(farm, scheduler["host"], (
            "exec", f"icefarm-{plan['run_id']}-{scheduler['name']}",
            "python3", "-c", H3_ARM_SCRIPT, MUTANT_ARM_PATH,
            plan["run_id"], scheduler["name"], contract,
        )),
    ))
    receipt = _json_result(result, "H3 arming")
    if (receipt.get("schema") != contract or receipt.get("run_id") != plan["run_id"]
            or receipt.get("scheduler") != scheduler["name"]):
        raise LifecycleError("H3 arming receipt identity mismatch")
    return receipt


def _rotate_s30_canary_traces(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    recorder: Recorder,
    factory: CommandFactory,
    *,
    timeout_s: int,
) -> dict[str, dict[str, Any]]:
    """Preserve readiness refusals and open a clean workload trace boundary."""

    if scenario.data.get("id") != "S30-mutant-f-refusal":
        return {}
    client_count = len(scenario.data["workload"]["clients"])
    if client_count < 1:
        raise LifecycleError("S30 mutant trace rotation has no workload client")
    receipts: dict[str, dict[str, Any]] = {}
    for instance in plan["topology"]["instances"]:
        if instance["role"] != "F" or instance["image"].get("kind") != "daemon-mutant":
            continue
        container = f"icefarm-{plan['run_id']}-{instance['name']}"
        result = recorder.invoke(
            _command(
                factory,
                phase="readiness.s30-mutant-trace-boundary",
                host=instance["host"],
                transport=_docker_transport(farm, instance["host"]),
                timeout_s=timeout_s,
                argv=docker_argv(
                    farm,
                    instance["host"],
                    (
                        "exec",
                        "--user",
                        "0",
                        container,
                        "/bin/bash",
                        "-c",
                        S30_ROTATE_CANARY_TRACE_SCRIPT,
                        "icefarm-s30-trace-boundary",
                        str(client_count),
                    ),
                ),
            )
        )
        receipts[instance["name"]] = {
            "canary_refusals": client_count,
            "output": result.stdout.strip(),
        }
    if not receipts:
        raise LifecycleError("S30 mutant scenario has no daemon-mutant worker")
    return receipts


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
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
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
    live_only: bool = False,
    include_live: bool = True,
) -> list[str]:
    """Best-effort diagnostic capture that never replaces the original error."""

    problems: list[str] = []
    destination.mkdir(parents=True, exist_ok=True)
    shaped_names = {
        binding.instance for binding in _netem_bindings(plan)
    }
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
        diagnostics = [
            (
                "inspect",
                (
                    "container",
                    "inspect",
                    "--format",
                    "{{json .}}",
                    container_target,
                ),
            ),
            ("logs", ("container", "logs", container_target)),
        ]
        if live_only:
            diagnostics = []
        if include_live and instance["name"] in shaped_names:
            diagnostics.append(
                (
                    "tc",
                    (
                        "exec",
                        "--user",
                        "0",
                        container_target,
                        "tc",
                        "-s",
                        "qdisc",
                        "show",
                        "dev",
                        "eth0",
                    ),
                )
            )
        for kind, args in diagnostics:
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
                if kind == "inspect" and instance["role"] == "F":
                    try:
                        document = _json_result(
                            result,
                            f"container inspect for {host_name}:{instance['name']}",
                        )
                    except LifecycleError as exc:
                        problems.append(
                            f"{host_name}:{instance['name']}:inspect:{exc}"
                        )
                    else:
                        host_config = document.get("HostConfig")
                        if not f_runtime_host_config_valid(host_config):
                            problems.append(
                                f"{host_name}:{instance['name']}:inspect lacks Docker Init=true "
                                "or exact nofile=65536:65536"
                            )
                (host_dir / f"{instance['name']}.{kind}").write_text(
                    result.stdout + result.stderr, encoding="utf-8"
                )
            except (RemoteError, OSError) as exc:
                problems.append(f"{host_name}:{instance['name']}:{kind}:{exc}")
        if live_only:
            continue
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
        if (
            instance.get("role") == "C"
            and plan.get("diagnostic_capture_client_output") is True
        ):
            remote_output = (
                PurePosixPath(farm.hosts[host_name]["scratch_root"])
                / "icefarm"
                / plan["run_id"]
                / instance["name"]
                / "output"
            )
            local_output = host_dir / f"{instance['name']}.output"
            local_output.mkdir(parents=True, exist_ok=True)
            try:
                recorder.invoke(
                    _command(
                        factory,
                        phase="diagnostics.sync-output",
                        host=host_name,
                        transport="rsync-ssh",
                        timeout_s=60,
                        argv=(
                            "rsync",
                            "--archive",
                            "--no-owner",
                            "--no-group",
                            "--omit-dir-times",
                            "--protect-args",
                            f"{farm.hosts[host_name]['ssh']}:{remote_output}/canary/",
                            str(local_output / "canary") + "/",
                        ),
                    )
                )
            except RemoteError as exc:
                problems.append(
                    f"{host_name}:{instance['name']}:sync-output:{exc}"
                )
    return problems


def _remove_netem_bridges(
    farm: FarmSpec,
    plan: Mapping[str, Any],
    bindings: tuple[NetemBinding, ...],
    recorder: Recorder,
    factory: CommandFactory,
    timeout_s: int,
) -> tuple[list[dict[str, Any]], list[str]]:
    """Remove only the freshly inspected network created for this run.

    The container namespace owns the qdisc, so removing the exact labelled
    container is sufficient to tear it down.  Network removal is separately
    authenticated by a fresh ID lookup and inspect; a planned name is never a
    sufficient target.
    """

    receipts: list[dict[str, Any]] = []
    problems: list[str] = []
    if not bindings:
        return receipts, problems
    expected: dict[str, str] = {}
    receipt_error: str | None = None
    lifecycle_path = bundle_root(farm, plan["run_id"]) / "lifecycle.json"
    try:
        document = json.loads(lifecycle_path.read_text(encoding="utf-8"))
        network_document = document.get("network_shaping")
        if (
            not isinstance(network_document, Mapping)
            or network_document.get("schema") != NETEM_RECEIPT_SCHEMA
            or network_document.get("status") not in {"CREATED", "APPLIED"}
        ):
            raise LifecycleError("netem creation receipt is not applied")
        if network_document.get("status") == "CREATED" and (
            network_document.get("scenario_digest") != plan["scenario_digest"]
            or network_document.get("topology_digest") != plan["topology_digest"]
        ):
            raise LifecycleError("netem partial receipt is not bound to this plan")
        records = network_document.get("bindings", [])
        if isinstance(records, list):
            for record in records:
                if not isinstance(record, Mapping):
                    continue
                instance = record.get("instance")
                network_id = record.get("bridge", {}).get("network_id")
                if (
                    isinstance(instance, str)
                    and isinstance(network_id, str)
                    and re.fullmatch(r"[0-9a-f]{64}", network_id)
                ):
                    expected[instance] = network_id
    except (OSError, json.JSONDecodeError, AttributeError, LifecycleError) as exc:
        expected = {}
        receipt_error = str(exc) or type(exc).__name__

    for binding in bindings:
        if receipt_error is not None:
            problems.append(f"{binding.instance}:network-receipt:{receipt_error}")
            continue
        try:
            list_command = _command(
                factory,
                phase="down.network-list",
                host=binding.host,
                instance=binding.instance,
                transport=_docker_transport(farm, binding.host),
                timeout_s=timeout_s,
                argv=docker_argv(
                    farm,
                    binding.host,
                    network_list_args(binding, plan["run_id"]),
                ),
            )
            listed = recorder.invoke(list_command)
            if listed.returncode != 0:
                problems.append(f"{binding.instance}:network-list:rc={listed.returncode}")
                continue
            candidates = listed.stdout.split()
            if not candidates:
                receipts.append(
                    {
                        "instance": binding.instance,
                        "status": "SKIPPED_NO_AUTHENTICATED_NETWORK",
                        "list_argv": list(list_command.argv),
                    }
                )
                continue
            if len(candidates) != 1 or any(
                re.fullmatch(r"[0-9a-f]{64}", value) is None for value in candidates
            ):
                problems.append(f"{binding.instance}:network-identity:ambiguous-or-unsafe-list")
                continue
            network_id = candidates[0]
            expected_id = expected.get(binding.instance)
            if expected_id is None:
                problems.append(f"{binding.instance}:network-identity:missing-creation-receipt")
                continue
            inspect_command = _command(
                factory,
                phase="down.network-inspect",
                host=binding.host,
                instance=binding.instance,
                transport=_docker_transport(farm, binding.host),
                timeout_s=timeout_s,
                argv=docker_argv(
                    farm, binding.host, network_inspect_args(network_id)
                ),
            )
            inspected = recorder.invoke(inspect_command)
            validate_network_inspect(
                binding,
                plan["run_id"],
                plan["scenario_digest"],
                plan["topology_digest"],
                network_id,
                _json_result(inspected, f"netem network {network_id} inspect"),
            )
            if network_id != expected_id:
                problems.append(
                    f"{binding.instance}:network-identity:replacement-id:{network_id}"
                )
                continue
            remove_command = _command(
                factory,
                phase="down.network-remove",
                host=binding.host,
                instance=binding.instance,
                transport=_docker_transport(farm, binding.host),
                timeout_s=timeout_s,
                argv=docker_argv(
                    farm, binding.host, network_remove_args(network_id)
                ),
            )
            result = recorder.invoke(remove_command)
            receipts.append(
                {
                    "inspect_argv": list(inspect_command.argv),
                    "instance": binding.instance,
                    "list_argv": list(list_command.argv),
                    "network_id": network_id,
                    "remove_argv": list(remove_command.argv),
                    "returncode": result.returncode,
                    "stderr": result.stderr,
                    "stdout": result.stdout,
                    "status": "REMOVED" if result.returncode == 0 else "REMOVE_FAILED",
                }
            )
            if result.returncode != 0:
                problems.append(f"{binding.instance}:bridge-remove:rc={result.returncode}")
        except (RemoteError, LifecycleError, NetemPlanError) as exc:
            problems.append(f"{binding.instance}:network-identity:{exc}")
    return receipts, problems


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
                    f"{host_name}:leftovers="
                    + ",".join(item["name"] for item in leftovers)
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
    bindings = _netem_bindings(plan)
    problems = _remove_run_containers(farm, plan, recorder, factory, timeout_s)
    network_receipts, network_problems = _remove_netem_bridges(
        farm, plan, bindings, recorder, factory, timeout_s
    )
    problems = [*problems, *network_problems]
    for instance in plan["topology"]["instances"]:
        root = instance_root(farm, instance["host"], plan["run_id"], instance["name"])
        reference = instance["container_image"]["reference"]
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
                            "rm -rf -- /cleanup/cache /cleanup/tmp /cleanup/log "
                            "/cleanup/input /cleanup/output /cleanup/system-source",
                        ),
                    ),
                )
            )
        except RemoteError as exc:
            problems.append(
                f"{instance['host']}:{instance['name']}:scratch-cleanup:{exc}"
            )

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
        "network_shaping": {
            "bindings": network_receipts,
            "schema": NETEM_RECEIPT_SCHEMA,
            "status": "REMOVED" if not network_problems else "REMOVE_FAILED",
        },
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
    network_receipt: dict[str, Any] = {
        "bindings": [],
        "schema": NETEM_RECEIPT_SCHEMA,
        "status": "NOT_APPLIED",
    }
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
        netem_bindings = _netem_bindings(plan)
        phases = {
            phase: [command for command in planned if command.phase == phase]
            for phase in (
                "up.prepare",
                "up.prepare-persistent",
                "up.network-create",
                "up.start-s",
                "up.start-f",
                "up.start-c",
                "up.netem-apply",
                "up.netem-observe",
            )
        }
        execute(phases["up.prepare"], transport)
        execute(phases["up.prepare-persistent"], transport)
        create_results = execute(phases["up.network-create"], transport)
        network_receipt = _netem_created_receipt(
            plan["run_id"],
            scenario.digest,
            plan["topology_digest"],
            netem_bindings,
            create_results,
            phases["up.network-create"],
        )
        if network_receipt["status"] == "CREATED":
            _atomic_json(
                bundle / "lifecycle.json",
                {
                    "farm": str(farm.path),
                    "farm_digest": farm.digest,
                    "network_shaping": network_receipt,
                    "plan": plan,
                    "run_id": plan["run_id"],
                    "scenario": str(scenario.path),
                    "scenario_digest": scenario.digest,
                    "schema": LIFECYCLE_SCHEMA,
                    "status": "STARTING",
                    "topology_digest": plan["topology_digest"],
                },
            )
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
        apply_results = execute(phases["up.netem-apply"], transport)
        observe_results = execute(phases["up.netem-observe"], transport)
        network_receipt = _netem_up_receipt(
            plan["run_id"],
            netem_bindings,
            create_results,
            apply_results,
            observe_results,
            phases["up.network-create"],
            phases["up.netem-apply"],
            phases["up.netem-observe"],
        )
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
        clients = _wait_clients(
            farm,
            plan,
            transport,
            factory,
            deadline=deadline,
            monotonic=monotonic,
            sleeper=sleeper,
        )
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
        s30_trace_boundary = _rotate_s30_canary_traces(
            farm,
            scenario,
            plan,
            transport,
            factory,
            timeout_s=_timeout_left(deadline, monotonic),
        )
        receipt = {
            "canaries": canaries,
            **({"h3_arm": _arm_h3_mutant(
                farm, plan, transport, factory,
                timeout_s=_timeout_left(deadline, monotonic),
            )} if plan.get("h3_arm_contract") else {}),
            "client_cache_readiness": clients,
            "commands": _recorded_commands(transport),
            "environment_readiness": environments,
            "farm": str(farm.path),
            "farm_digest": farm.digest,
            "network_shaping": network_receipt,
            "plan": plan,
            "run_id": plan["run_id"],
            "scenario": str(scenario.path),
            "scenario_digest": scenario.digest,
            "scheduler_snapshot": scheduler,
            "schema": LIFECYCLE_SCHEMA,
            "s30_mutant_trace_boundary": s30_trace_boundary,
            "status": "UP",
            "topology_digest": plan["topology_digest"],
            "worker_snapshot": workers,
        }
        _atomic_json(bundle / "lifecycle.json", receipt)
        return receipt
    except BaseException as exc:
        observed_commands = getattr(transport, "commands", [])
        prepared_or_started = any(
            command.phase.startswith("up.prepare")
            or command.phase.startswith("up.start-")
            or command.phase.startswith("up.network-")
            or command.phase.startswith("up.netem-")
            for command in observed_commands
        )
        preflight_container_possible = any(
            command.phase
            in (
                "preflight.chroot",
                "preflight.compiler",
                "preflight.role-hashes",
                "preflight.runtime-materialize",
                "preflight.toolchain-materialize",
            )
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
            "farm_digest": farm.digest,
            "network_shaping": network_receipt,
            "plan": plan,
            "run_id": plan["run_id"],
            "scenario_digest": scenario.digest,
            "schema": LIFECYCLE_SCHEMA,
            "status": "FAILED",
            "topology_digest": plan["topology_digest"],
        }
        _atomic_json(bundle / "lifecycle.json", failure)
        if isinstance(exc, PreflightRefusal):
            refusal = {
                "commands": _recorded_commands(transport),
                "details": exc.details,
                "error": str(exc) or type(exc).__name__,
                "farm_digest": farm.digest,
                "jobs_started": 0,
                "persistent_start_attempted": prepared_or_started,
                "reason_code": exc.reason_code,
                "run_id": plan["run_id"],
                "scenario_digest": scenario.digest,
                "schema": "icefarm-preflight-refusal-v1",
                "topology_digest": plan["topology_digest"],
            }
            _atomic_json(bundle / "preflight-refusal.json", refusal)
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
            raise LifecycleError(
                f"cannot load preflight state for down: {exc}"
            ) from exc
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
