"""Bounded, authenticated timeline events for one controlled-farm turn.

The event worker is deliberately small.  It consumes the already resolved
plan, never constructs shell source, and authenticates a container's
``icefarm.run``/``icefarm.instance`` labels before issuing a mutating Docker
operation.  Actions which need a new image or bind mount are resolved through
the sealed farm authority and materialized before the workload starts;
unsupported host mutations are rejected during construction.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
import threading
import time
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Iterable, Mapping

try:
    from .farm_spec import FarmSpec
    from .images import CommandFactory, RecordingTransport
    from .lifecycle import (
        _inspect_required_image,
        _materialize_runtime,
        _probe_role_hashes,
        _wait_scheduler,
        _wait_workers,
        bundle_root,
    )
    from .layout import instance_root, runtime_root
    from .remote import docker_argv, ssh_argv
    from .scenario_spec import ScenarioSpec
    from .schema_validation import canonical_bytes
except ImportError:  # Direct execution from this directory.
    from farm_spec import FarmSpec
    from images import CommandFactory, RecordingTransport
    from lifecycle import (
        _inspect_required_image,
        _materialize_runtime,
        _probe_role_hashes,
        _wait_scheduler,
        _wait_workers,
        bundle_root,
    )
    from layout import instance_root, runtime_root
    from remote import docker_argv, ssh_argv
    from scenario_spec import ScenarioSpec
    from schema_validation import canonical_bytes


TRIGGER_TIME = re.compile(r"^t\+([0-9]+(?:\.[0-9]+)?)$")
TRIGGER_JOB = re.compile(r"^job ([1-9][0-9]*)$")
TRIGGER_TURN = re.compile(r"^after turn ([A-Za-z0-9][A-Za-z0-9._-]*)$")
DISPATCH_RE = re.compile(r"\bput\s+([0-9]+)\s+in joblist of\b", re.IGNORECASE)
JOB_PATTERNS = (
    DISPATCH_RE,
    re.compile(r"\bJob ID:\s*([0-9]+)\b", re.IGNORECASE),
    re.compile(r"\bjob(?:_id)?[=: ]+([0-9]+)\b", re.IGNORECASE),
)
SUPPORTED_ACTIONS = frozenset(
    (
        "restart",
        "kill -9",
        "upgrade",
        "downgrade",
        "env_set",
        "header_edit",
        "disk_fill",
    )
)
UNSUPPORTED_ACTIONS = frozenset(("netem_set",))
TRANSITION_ACTIONS = frozenset(("upgrade", "downgrade", "env_set"))
TRANSITION_SCHEMA = "icefarm-transition-v2"
P29_FAULT_ENV = "ICECC_P50_FAULT_INJECTION"
P29_FAULT_VALUE = "P29_INTERNER_FAIL_ONCE"
IMAGE_LABEL_RE = re.compile(r"^p(43|44|50)(?:s[0-9]+)?(?:-|$)", re.IGNORECASE)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
READINESS_SCRIPT = r'''
import json, pathlib, re, sys

path = pathlib.Path(sys.argv[1])
offset = int(sys.argv[2])
role = sys.argv[3]
name = sys.argv[4]
try:
    text = path.read_bytes()[offset:].decode("utf-8", "replace")
except FileNotFoundError:
    text = ""
lines = text.splitlines()
if role == "S":
    pattern = re.compile(r"ICECREAM scheduler .* starting up, port [0-9]+")
else:
    pattern = re.compile(r"ICECREAM daemon .* starting up")
matches = [line for line in lines if pattern.search(line)]
print(json.dumps({
    "bytes": len(text.encode("utf-8")),
    "line": matches[-1] if matches else None,
    "ready": bool(matches),
}, sort_keys=True))
'''.strip()
GATE_SCHEMA = "icefarm-event-gate-v1"
SCHEDULER_RESTART_SCHEMA = "icefarm-scheduler-restart-v1"
CLIENT_ROUTE_RESTART_SCHEMA = "icefarm-client-route-restart-v1"
WORKER_RESTART_SCHEMA = "icefarm-worker-restart-v1"
CLIENT_ROUTE_SIGNAL_SCHEMA = "icefarm-client-route-signal-v1"
HEADER_EDIT_SCHEMA = "icefarm-header-edit-v1"
DISK_FILL_SCHEMA = "icefarm-disk-fill-v1"
CLIENT_TRANSITION_SCHEMA = "icefarm-client-transition-v1"
CACHE_DISK_FAULT_PATH = "/var/cache/icecream"
CACHE_DISK_FAULT_FILE = "/var/cache/icecream/.icefarm-disk-fill"
CACHE_DISK_FAULT_BYTES = 128 * 1024 * 1024
CACHE_DISK_FAULT_MIN_HEADROOM_BYTES = 8 * 1024 * 1024
DISK_FILL_WATCHDOG_S = 30
DISK_FILL_SCRIPT = r'''
import errno, json, os, pathlib, signal, stat, sys, time

root = pathlib.Path(sys.argv[1])
filler = pathlib.Path(sys.argv[2])
limit = int(sys.argv[3])
watchdog_s = int(sys.argv[4])
minimum_headroom = int(sys.argv[5])
if str(root) != "/var/cache/icecream" or str(filler) != "/var/cache/icecream/.icefarm-disk-fill":
    raise SystemExit("disk-fill path contract mismatch")
if limit != 128 * 1024 * 1024 or watchdog_s != 30 or minimum_headroom != 8 * 1024 * 1024:
    raise SystemExit("disk-fill bound contract mismatch")

def watchdog(_signum, _frame):
    raise TimeoutError("disk-fill in-container watchdog expired")

signal.signal(signal.SIGALRM, watchdog)
signal.alarm(watchdog_s)
started_ns = time.monotonic_ns()
info = root.lstat()
if not stat.S_ISDIR(info.st_mode) or root.is_symlink():
    raise SystemExit("disk-fill root is unsafe")
if filler.exists() or filler.is_symlink():
    raise SystemExit("disk-fill target already exists")
before = os.statvfs(root)
available_before = before.f_bavail * before.f_frsize
if available_before < minimum_headroom:
    raise SystemExit("disk-fill target lacks required pre-fault headroom")
block = bytes(1024 * 1024)
written = 0
failure_errno = None
fd = os.open(
    filler,
    os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
    0o600,
)
try:
    while written <= limit:
        try:
            count = os.write(fd, block)
        except OSError as exc:
            failure_errno = exc.errno
            break
        if count <= 0:
            raise SystemExit("disk-fill write made no progress")
        written += count
        if written > limit:
            raise SystemExit("disk-fill exceeded its byte bound without ENOSPC")
finally:
    os.close(fd)
if failure_errno != errno.ENOSPC:
    raise SystemExit("disk-fill did not authenticate ENOSPC")
after = os.statvfs(root)
available_after = after.f_bavail * after.f_frsize
filler_info = filler.lstat()
if not stat.S_ISREG(filler_info.st_mode) or filler.is_symlink():
    raise SystemExit("disk-fill output is unsafe")
if filler_info.st_size != written:
    raise SystemExit("disk-fill byte count mismatch")
elapsed_ms = (time.monotonic_ns() - started_ns + 999999) // 1000000
signal.alarm(0)
print(json.dumps({
    "available_after": available_after,
    "available_before": available_before,
    "directory_gid": info.st_gid,
    "directory_mode": stat.S_IMODE(info.st_mode),
    "directory_uid": info.st_uid,
    "elapsed_ms": elapsed_ms,
    "errno": failure_errno,
    "filler_bytes": written,
    "filler_path": str(filler),
    "limit_bytes": limit,
    "minimum_headroom_bytes": minimum_headroom,
    "schema": "icefarm-disk-fill-operation-v1",
    "watchdog_s": watchdog_s,
}, sort_keys=True))
'''.strip()
SCHEDULER_HEADER_RELOGIN_SCRIPT = r'''
import hashlib, json, pathlib, re, sys

path = pathlib.Path(sys.argv[1])
offset = int(sys.argv[2])
target = sys.argv[3]
profile = sys.argv[4].lower()
include_loss = len(sys.argv) == 6 and sys.argv[5] == "include-loss"
try:
    payload = path.read_bytes()[offset:]
except FileNotFoundError:
    payload = b""
lines = payload.decode("utf-8", "replace").splitlines()
role_pattern = re.compile(
    r"\blogin\s+" + re.escape(target)
    + r"\s+protocol\s+version:\s*50\b"
)
cache_pattern = re.compile(
    r"\bRELOGIN " + re.escape(target)
    + r"\([^)]*\):.*\bcache=([^ ]+) cache_wire=v1 "
      r"cache_protocol=1 cache_profiles=([a-z0-9_ ]+)\s*$"
)
role_lines = [line for line in lines if role_pattern.search(line)]
cache_matches = []
for line in lines:
    match = cache_pattern.search(line)
    if match is None:
        continue
    profiles = match.group(2).split()
    if profile in profiles:
        cache_matches.append((line, profiles))
role_line = role_lines[-1] if role_lines else None
cache_line = cache_matches[-1][0] if cache_matches else None
loss_pattern = re.compile(r"\bSTOP \((?:DAEMON|DAEMON2)\) FOR ([0-9]+)\b")
loss_job_ids = sorted({
    int(match.group(1))
    for line in lines
    if (match := loss_pattern.search(line)) is not None
})
document = {
    "bytes": len(payload),
    "cache_line": cache_line,
    "cache_protocol": 1 if cache_line is not None else None,
    "login_line": role_line,
    "profile": profile,
    "ready": role_line is not None and cache_line is not None,
    "role_protocol": 50 if role_line is not None else None,
    "sha256": hashlib.sha256(payload).hexdigest(),
    "target": target,
}
if include_loss:
    document["loss_job_ids"] = loss_job_ids
print(json.dumps(document, sort_keys=True))
'''.strip()
GATE_CONTROL_SCRIPT = r'''
import fcntl, json, os, pathlib, re, stat, sys, time

action, root_arg, client, turn, epoch_arg, timeout_arg = sys.argv[1:]
root = pathlib.Path(root_arg)
epoch = int(epoch_arg)
timeout_s = float(timeout_arg)
if action not in {"pause", "quiesce", "resume", "abort"} or epoch < 1 or timeout_s <= 0:
    raise SystemExit("invalid event-gate arguments")
if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", client):
    raise SystemExit("invalid event-gate client")
if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", turn):
    raise SystemExit("invalid event-gate turn")
gate = root / "event-gate"
state_path = gate / "state.tsv"
lock_path = gate / "state.lock"
active_path = gate / "active"

def require_regular(path):
    value = path.lstat()
    if not stat.S_ISREG(value.st_mode) or path.is_symlink():
        raise SystemExit(f"unsafe event-gate file: {path}")

require_regular(state_path)
require_regular(lock_path)
value = active_path.lstat()
if not stat.S_ISDIR(value.st_mode) or active_path.is_symlink():
    raise SystemExit("unsafe event-gate active directory")

def read_state():
    fields = state_path.read_text(encoding="ascii").strip().split("\t")
    if len(fields) != 2 or fields[0] not in {"OPEN", "PAUSE", "QUIESCE", "ABORT"}:
        raise SystemExit("malformed event-gate state")
    try:
        observed_epoch = int(fields[1])
    except ValueError as exc:
        raise SystemExit("malformed event-gate epoch") from exc
    if observed_epoch < 0:
        raise SystemExit("negative event-gate epoch")
    return fields[0], observed_epoch

def write_state(mode):
    temporary = gate / f".state-{os.getpid()}"
    temporary.write_text(f"{mode}\t{epoch}\n", encoding="ascii")
    os.chmod(temporary, 0o666)
    os.replace(temporary, state_path)

def markers():
    result = []
    for path in sorted(active_path.glob("job-*.tsv")):
        require_regular(path)
        if re.fullmatch(r"job-[1-9][0-9]*-[1-9][0-9]*[.]tsv", path.name) is None:
            raise SystemExit(f"malformed event-gate marker: {path.name}")
        result.append(path.name)
    return result

started_ms = time.time_ns() // 1_000_000
with lock_path.open("r+") as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    before_mode, before_epoch = read_state()
    # Workload admission takes this same lock before creating an active
    # marker.  Snapshot the pre-transition set while holding it, before a
    # fast completion can observe PAUSE and remove the last marker.
    initial = markers()
    if action in {"pause", "quiesce"}:
        if before_mode == "OPEN" and before_epoch < epoch:
            write_state("PAUSE" if action == "pause" else "QUIESCE")
        elif (before_mode, before_epoch) not in {
            ("PAUSE", epoch),
            ("QUIESCE", epoch),
        }:
            raise SystemExit(
                f"event-gate {action} expected an earlier OPEN epoch, got {before_mode}/{before_epoch}"
            )
        target_mode = "PAUSE" if action == "pause" else "QUIESCE"
    else:
        target_mode = "OPEN" if action == "resume" else "ABORT"
        if (before_mode, before_epoch) in {
            ("PAUSE", epoch),
            ("QUIESCE", epoch),
        }:
            write_state(target_mode)
        elif before_mode == "OPEN" and before_epoch < epoch:
            write_state(target_mode)
        elif action == "abort" and (before_mode, before_epoch) == ("OPEN", epoch):
            write_state(target_mode)
        elif (before_mode, before_epoch) != (target_mode, epoch):
            raise SystemExit(
                f"event-gate {action} expected PAUSE/{epoch}, got {before_mode}/{before_epoch}"
            )
deadline = time.monotonic() + timeout_s
if action in {"pause", "quiesce"}:
    current = initial
    while current:
        if time.monotonic() >= deadline:
            raise SystemExit(
                "event-gate drain timeout with active markers: " + ",".join(current)
            )
        time.sleep(min(0.05, max(0.001, deadline - time.monotonic())))
        current = markers()
    status = "PAUSED" if action == "pause" else "QUIESCED"
else:
    current = markers()
    status = target_mode
finished_ms = time.time_ns() // 1_000_000
print(json.dumps({
    "action": action,
    "active_after": len(current),
    "active_before": len(initial),
    "client": client,
    "epoch": epoch,
    "finished_ms": finished_ms,
    "schema": "icefarm-event-gate-v1",
    "started_ms": started_ms,
    "status": status,
    "turn": turn,
}, sort_keys=True, separators=(",", ":")))
'''.strip()

CLIENT_ROUTE_SIGNAL_SCRIPT = r'''
import json, os, pathlib, signal, sys, time

daemon_exe, route_exe = sys.argv[1:]

def snapshot(pid):
    root = pathlib.Path("/proc") / str(pid)
    raw = (root / "stat").read_text(encoding="ascii")
    fields = raw.rsplit(") ", 1)[1].split()
    argv = [
        value.decode("utf-8", "surrogateescape")
        for value in (root / "cmdline").read_bytes().split(b"\0")
        if value
    ]
    status = (root / "status").read_text(encoding="ascii")
    uid_line = next(line for line in status.splitlines() if line.startswith("Uid:"))
    try:
        executable = os.readlink(root / "exe")
        executable_evidence = "proc-exe"
    except PermissionError as exc:
        if exc.errno != 13 or not argv or not argv[0].startswith("/"):
            raise
        # iceccd drops to nobody and the default Docker capability set cannot
        # read /proc/<pid>/exe across that transition.  The container/runtime
        # and role binary are already authenticated by preflight; retain the
        # denial explicitly and bind the process to its absolute argv[0].
        executable = argv[0]
        executable_evidence = "argv0-after-proc-exe-eacces"
    return {
        "argv": argv,
        "exe": executable,
        "exe_evidence": executable_evidence,
        "pid": pid,
        "ppid": int(fields[1]),
        "start_ticks": int(fields[19]),
        "uid": int(uid_line.split()[1]),
    }

def exact_process(executable):
    matches = []
    for entry in pathlib.Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            value = snapshot(int(entry.name))
        except (FileNotFoundError, PermissionError, ProcessLookupError, StopIteration, ValueError):
            continue
        if value["exe"] == executable:
            matches.append(value)
    if len(matches) != 1:
        raise SystemExit(f"expected exactly one {executable}, found {len(matches)}")
    return matches[0]

daemon = exact_process(daemon_exe)
route_owner = exact_process(route_exe)
if (
    route_owner["pid"] <= 1
    or route_owner["ppid"] != daemon["pid"]
    or route_owner["uid"] != daemon["uid"]
):
    raise SystemExit("cache route owner is not the daemon's direct child")
if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
    raise SystemExit("pidfd signalling is unavailable")
pidfd = os.pidfd_open(route_owner["pid"], 0)
try:
    if snapshot(route_owner["pid"]) != route_owner:
        raise SystemExit("cache route owner changed before pidfd signal")
    signal.pidfd_send_signal(pidfd, signal.SIGKILL)
finally:
    os.close(pidfd)
print(json.dumps({
    "daemon": daemon,
    "mechanism": "pidfd_send_signal",
    "route_owner": route_owner,
    "schema": "icefarm-client-route-signal-v1",
    "sent_ms": time.time_ns() // 1_000_000,
    "signal": 9,
}, sort_keys=True))
'''.strip()

CLIENT_ROUTE_SNAPSHOT_SCRIPT = r'''
import json, os, pathlib, sys

daemon_exe, route_exe = sys.argv[1:]

def snapshot(pid):
    root = pathlib.Path("/proc") / str(pid)
    raw = (root / "stat").read_text(encoding="ascii")
    fields = raw.rsplit(") ", 1)[1].split()
    argv = [
        value.decode("utf-8", "surrogateescape")
        for value in (root / "cmdline").read_bytes().split(b"\0")
        if value
    ]
    status = (root / "status").read_text(encoding="ascii")
    uid_line = next(line for line in status.splitlines() if line.startswith("Uid:"))
    try:
        executable = os.readlink(root / "exe")
        executable_evidence = "proc-exe"
    except PermissionError as exc:
        if exc.errno != 13 or not argv or not argv[0].startswith("/"):
            raise
        executable = argv[0]
        executable_evidence = "argv0-after-proc-exe-eacces"
    return {
        "argv": argv,
        "exe": executable,
        "exe_evidence": executable_evidence,
        "pid": pid,
        "ppid": int(fields[1]),
        "start_ticks": int(fields[19]),
        "uid": int(uid_line.split()[1]),
    }

def matches(executable):
    result = []
    for entry in pathlib.Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            value = snapshot(int(entry.name))
        except (FileNotFoundError, PermissionError, ProcessLookupError, StopIteration, ValueError):
            continue
        if value["exe"] == executable:
            result.append(value)
    return result

daemons = matches(daemon_exe)
owners = matches(route_exe)
ready = (
    len(daemons) == 1
    and len(owners) == 1
    and owners[0]["ppid"] == daemons[0]["pid"]
    and owners[0]["uid"] == daemons[0]["uid"]
)
print(json.dumps({
    "daemon": daemons[0] if len(daemons) == 1 else None,
    "daemon_count": len(daemons),
    "ready": ready,
    "route_owner": owners[0] if len(owners) == 1 else None,
    "route_owner_count": len(owners),
    "schema": "icefarm-client-route-snapshot-v1",
}, sort_keys=True))
'''.strip()

CLIENT_ROUTE_READINESS_SCRIPT = r'''
import json, pathlib, re, sys

path = pathlib.Path(sys.argv[1])
offset = int(sys.argv[2])
try:
    payload = path.read_bytes()[offset:]
except FileNotFoundError:
    payload = b""
text = payload.decode("utf-8", "replace")
pattern = re.compile(r"cache sidecar adapter state=([0-9]+) lifecycle=([0-9]+)")
matches = [
    (int(match.group(1)), int(match.group(2)), line)
    for line in text.splitlines()
    if (match := pattern.search(line)) is not None
]
last = matches[-1] if matches else None
print(json.dumps({
    "bytes": len(payload),
    "lifecycle": last[1] if last is not None else None,
    "line": last[2] if last is not None else None,
    "ready": last is not None and last[:2] == (2, 3),
    "state": last[0] if last is not None else None,
}, sort_keys=True))
'''.strip()

CLIENT_SCHEDULER_READINESS_SCRIPT = r'''
import json, pathlib, re, sys

path = pathlib.Path(sys.argv[1])
offset = int(sys.argv[2])
cache_required = sys.argv[3] == "1"
try:
    payload = path.read_bytes()[offset:]
except FileNotFoundError:
    payload = b""
lines = payload.decode("utf-8", "replace").splitlines()
connected = [line for line in lines if "Connected to scheduler (I am known as " in line]
cache = [
    line
    for line in lines
    if re.search(r"cache sidecar adapter state=2 lifecycle=3", line)
]
print(json.dumps({
    "bytes": len(payload),
    "cache_line": cache[-1] if cache else None,
    "cache_required": cache_required,
    "connected_line": connected[-1] if connected else None,
    "ready": bool(connected) and (not cache_required or bool(cache)),
}, sort_keys=True))
'''.strip()

SCHEDULER_WORKER_REJOIN_SCRIPT = r'''
import json, pathlib, re, sys

path = pathlib.Path(sys.argv[1])
offset = int(sys.argv[2])
target = sys.argv[3]
protocol = int(sys.argv[4])
try:
    payload = path.read_bytes()[offset:]
except FileNotFoundError:
    payload = b""
lines = payload.decode("utf-8", "replace").splitlines()
pattern = re.compile(
    r"\blogin\s+" + re.escape(target)
    + r"\s+protocol\s+version:\s*" + str(protocol) + r"\b"
)
matches = [line for line in lines if pattern.search(line)]
print(json.dumps({
    "bytes": len(payload),
    "login_line": matches[-1] if matches else None,
    "ready": bool(matches),
    "role_protocol": protocol if matches else None,
    "target": target,
}, sort_keys=True))
'''.strip()

HEADER_EDIT_SCRIPT = r'''
import hashlib, json, os, pathlib, stat, sys

path = pathlib.PurePosixPath(sys.argv[1])
marker = sys.argv[2]
cache_dir = pathlib.Path("/var/cache/icecream/p50-runtime")
cache_names = (
    "p29-system-source-fingerprint-v1.cache",
    "p29-system-source-fingerprint-v1.lock",
)
if ".." in path.parts or not marker or "/" in marker:
    raise SystemExit("unsafe header edit arguments")
if not path.is_absolute() or not (
    str(path).startswith("/usr/include/")
    or str(path).startswith("/usr/local/include/")
):
    raise SystemExit("header path must be below /usr/include or /usr/local/include")
header = pathlib.Path(path)
value = header.lstat()
if not stat.S_ISREG(value.st_mode) or header.is_symlink():
    raise SystemExit("header is not an absolute regular non-symlink file")
before = header.read_bytes()
before_sha = hashlib.sha256(before).hexdigest()
addition = ("\n/* icefarm S40 header_edit " + marker + " */\n").encode("ascii")
if addition in before:
    raise SystemExit("header edit marker already exists")
with header.open("ab") as handle:
    handle.write(addition)
    handle.flush()
    os.fsync(handle.fileno())
after = header.read_bytes()
after_sha = hashlib.sha256(after).hexdigest()
if before_sha == after_sha:
    raise SystemExit("header fingerprint did not change")
if cache_dir.is_symlink() or not cache_dir.is_dir():
    raise SystemExit("managed P29 runtime cache directory is unsafe")
before_cache = []
for name in cache_names:
    item = cache_dir / name
    if not item.exists() or item.is_symlink():
        raise SystemExit("managed P29 cache entry is absent")
    value = item.lstat()
    if not stat.S_ISREG(value.st_mode):
        raise SystemExit("managed P29 cache entry is unsafe")
    before_cache.append(name)
if before_cache != list(cache_names):
    raise SystemExit("managed P29 cache set is incomplete")
for name in before_cache:
    (cache_dir / name).unlink()
after_cache = [
    name for name in cache_names
    if (cache_dir / name).exists() or (cache_dir / name).is_symlink()
]
if after_cache:
    raise SystemExit("managed P29 cache invalidation did not complete")
print(json.dumps({
    "after_cache": after_cache,
    "after_sha256": after_sha,
    "before_cache": before_cache,
    "before_sha256": before_sha,
    "cache_directory": str(cache_dir),
    "header_path": str(path),
    "removed_cache": before_cache,
}, sort_keys=True, separators=(",", ":")))
'''.strip()

HEADER_READINESS_SCRIPT = r'''
import json, pathlib, re, sys

path = pathlib.Path(sys.argv[1])
offset = int(sys.argv[2])
try:
    payload = path.read_bytes()[offset:]
except FileNotFoundError:
    payload = b""
text = payload.decode("utf-8", "replace")
lines = text.splitlines()
starts = [line for line in lines if re.search(r"ICECREAM daemon .* starting up", line)]
ready = [line for line in lines if re.search(r"cache sidecar adapter state=2 lifecycle=3", line)]
print(json.dumps({
    "bytes": len(payload),
    "cache_line": ready[-1] if ready else None,
    "line": starts[-1] if starts else None,
    "ready": bool(starts) and bool(ready),
}, sort_keys=True, separators=(",", ":")))
'''.strip()


class EventError(RuntimeError):
    """A timeline event could not be validated, dispatched, or recorded."""


class EventTimeout(EventError):
    """A trigger did not become eligible before its bounded deadline."""


class UnsupportedEvent(EventError):
    """The current runtime cannot safely perform an event action."""


@dataclass(frozen=True)
class Trigger:
    kind: str
    value: float | int | str

    @classmethod
    def parse(cls, text: str) -> "Trigger":
        if not isinstance(text, str):
            raise EventError("event trigger must be a string")
        match = TRIGGER_TIME.fullmatch(text)
        if match:
            return cls("time", float(match.group(1)))
        match = TRIGGER_JOB.fullmatch(text)
        if match:
            return cls("job", int(match.group(1)))
        match = TRIGGER_TURN.fullmatch(text)
        if match:
            return cls("turn", match.group(1))
        raise EventError(
            f"invalid event trigger {text!r}; expected t+<seconds>, job <n>, or after turn A"
        )


@dataclass(frozen=True)
class TimelineEvent:
    index: int
    trigger_text: str
    trigger: Trigger
    action: str
    instance: str
    fields: dict[str, Any]

    @classmethod
    def from_dict(cls, index: int, value: dict[str, Any]) -> "TimelineEvent":
        if not isinstance(value, dict):
            raise EventError(f"timeline event {index} is not an object")
        try:
            trigger_text = value["trigger"]
            action = value["action"]
            instance = value["instance"]
        except KeyError as exc:
            raise EventError(f"timeline event {index} lacks {exc.args[0]!r}") from exc
        if not all(isinstance(item, str) and item for item in (action, instance)):
            raise EventError(f"timeline event {index} has invalid action or instance")
        return cls(
            index=index,
            trigger_text=trigger_text,
            trigger=Trigger.parse(trigger_text),
            action=action,
            instance=instance,
            fields={key: value for key, value in value.items() if key not in {"trigger", "action", "instance"}},
        )


@dataclass(frozen=True)
class EventRecord:
    """The immutable evidence row written to ``events/events.json``."""

    event_epoch: int
    event_index: int
    action: str
    instance: str
    trigger: str
    fired_ms: int
    last_dispatched_job: int | None
    workload_dispatch_count: int
    receipt: dict[str, Any] | None = None

    def as_dict(self) -> dict[str, Any]:
        result = {
            "action": self.action,
            "event_epoch": self.event_epoch,
            "event_index": self.event_index,
            "fired_ms": self.fired_ms,
            "instance": self.instance,
            "last_dispatched_job": self.last_dispatched_job,
            "trigger": self.trigger,
            "workload_dispatch_count": self.workload_dispatch_count,
        }
        if self.receipt is not None:
            result["receipt"] = self.receipt
        return result


JobReader = Callable[[], int | str | Iterable[str] | None]
Clock = Callable[[], float]
WallClock = Callable[[], int]


def parse_last_dispatched_job(value: int | str | Iterable[str] | None) -> int | None:
    """Return the largest scheduler job id observed in a log sample."""

    if value is None:
        return None
    if isinstance(value, int) and not isinstance(value, bool):
        return value if value >= 0 else None
    if isinstance(value, str):
        text = value
    else:
        text = "\n".join(str(item) for item in value)
    values = [int(match.group(1)) for pattern in JOB_PATTERNS for match in pattern.finditer(text)]
    return max(values) if values else None


def parse_scheduler_dispatches(value: int | str | Iterable[str] | None) -> tuple[int, ...]:
    """Return scheduler dispatch ids in log order.

    An integer reader is a deterministic-test convenience representing that
    many sequential dispatches.  Live readers return the cumulative scheduler
    log and are matched only against its ``put ... in joblist`` records.
    """

    if value is None:
        return ()
    if isinstance(value, int) and not isinstance(value, bool):
        return tuple(range(1, value + 1)) if value >= 0 else ()
    text = value if isinstance(value, str) else "\n".join(str(item) for item in value)
    result: list[int] = []
    for line in text.splitlines():
        match = DISPATCH_RE.search(line)
        if match is not None and "(will install now)" not in line:
            result.append(int(match.group(1)))
    return tuple(result)


def _atomic_write(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{threading.get_ident()}")
    try:
        temporary.write_bytes(value)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


class EventProducer:
    """Watch a scenario timeline and dispatch bounded event operations.

    ``start``/``stop`` are intentionally explicit so the workload owns the
    lifetime.  ``stop`` always joins the worker; a live worker is an error and
    cannot silently outlive a turn.
    """

    def __init__(
        self,
        farm: FarmSpec,
        scenario: ScenarioSpec,
        plan: dict[str, Any],
        *,
        recorder: RecordingTransport,
        factory: CommandFactory | None = None,
        job_reader: JobReader | None = None,
        event_path: Path | None = None,
        deadline_s: float | None = None,
        poll_interval_s: float = 0.10,
        monotonic: Clock = time.monotonic,
        wall_ms: WallClock | None = None,
        quiesce_workload: Callable[[str, tuple[Mapping[str, Any], ...]], Mapping[str, Any]] | None = None,
        relaunch_workload: Callable[[str, Mapping[str, Any]], Mapping[str, Any]] | None = None,
    ) -> None:
        self.farm = farm
        self.scenario = scenario
        self.plan = plan
        self.recorder = recorder
        self.factory = factory or CommandFactory()
        self.job_reader = job_reader
        self.event_path = event_path or (bundle_root(farm, plan["run_id"]) / "events" / "events.json")
        self.failure_path = self.event_path.with_name("failure.json")
        self.deadline_s = float(
            deadline_s
            if deadline_s is not None
            else scenario.data["timeouts"]["turn_s"]
        )
        self.poll_interval_s = max(0.01, float(poll_interval_s))
        self.monotonic = monotonic
        self.wall_ms = wall_ms or (lambda: time.time_ns() // 1_000_000)
        self.quiesce_workload = quiesce_workload
        self.relaunch_workload = relaunch_workload
        self.events = [
            TimelineEvent.from_dict(index, value)
            for index, value in enumerate(scenario.data.get("timeline", []))
        ]
        self._validate_events()
        self._pending = list(self.events)
        self._records: list[EventRecord] = []
        self._turns: set[str] = set()
        self._stop = threading.Event()
        self._wake = threading.Event()
        self._thread: threading.Thread | None = None
        self._exception: BaseException | None = None
        self._start = 0.0
        self._last_job: int | None = None
        self._baseline_dispatches: tuple[int, ...] = ()
        self._dispatch_count = 0
        self._job_trigger_floor = 0
        self._lock = threading.Lock()
        self._active_turn: str | None = None
        self._current_event: TimelineEvent | None = None
        self._current_phase: str | None = None
        self._failure_evidence: dict[str, Any] = {
            "gate_receipts": {},
            "gate_attempts": {},
            "route": {
                "signal": {"state": "NOT_STARTED", "receipt": None},
                "process_ready": {"state": "NOT_STARTED", "receipt": None},
                "log_ready": {"state": "NOT_STARTED", "receipt": None},
            },
        }
        self._current_gate_key: str | None = None
        self._state = {
            item["name"]: {
                "image": dict(item["image"]),
                "env": dict(item.get("env", {})),
                "sha256": item["sha256"],
            }
            for item in self.plan["topology"]["instances"]
        }
        self._start_commands = {
            item["instance"]: item
            for item in self.plan["commands"]
            if item["phase"].startswith("up.start-")
        }
        self._start_argvs = {
            name: tuple(item["argv"]) for name, item in self._start_commands.items()
        }
        self._target_preflight: dict[tuple[str, str, str], dict[str, Any]] = {}
        self._preflight_transition_targets()

    @property
    def records(self) -> tuple[EventRecord, ...]:
        with self._lock:
            return tuple(self._records)

    @property
    def last_dispatched_job(self) -> int | None:
        return self._last_job

    @property
    def exception(self) -> BaseException | None:
        return self._exception

    def _validate_events(self) -> None:
        instances = {item["name"]: item for item in self.plan["topology"]["instances"]}
        simulated_labels = {
            name: item["image"]["label"] for name, item in instances.items()
        }
        if sum(
            event.action in {"upgrade", "downgrade", "env_set"}
            and instances.get(event.instance, {}).get("role") == "C"
            and event.trigger.kind == "job"
            for event in self.events
        ) > 1:
            raise UnsupportedEvent(
                "multiple checkpointed C events in one workload timeline are refused"
            )
        if sum(event.action == "disk_fill" for event in self.events) > 1:
            raise UnsupportedEvent(
                "multiple disk_fill events exceed the fixed fault bound; refusing before workload"
            )
        for event in self.events:
            if event.instance not in instances:
                raise EventError(f"timeline event targets unknown resolved instance {event.instance!r}")
            if event.action in UNSUPPORTED_ACTIONS:
                raise UnsupportedEvent(
                    f"timeline action {event.action!r} is not implemented safely; refusing before workload"
                )
            if event.action not in SUPPORTED_ACTIONS:
                raise UnsupportedEvent(f"timeline action {event.action!r} is unsupported")
            if (
                event.action in {"upgrade", "downgrade", "env_set"}
                and instances[event.instance]["role"] == "C"
                and event.trigger.kind == "job"
                and (self.quiesce_workload is None or self.relaunch_workload is None)
            ):
                raise UnsupportedEvent(
                    "C transitions require a checkpointed workload driver; refusing before workload"
                )
            if event.action in {"upgrade", "downgrade"}:
                image_alias = event.fields.get("image")
                if not isinstance(image_alias, str) or image_alias not in self.scenario.data["images"]:
                    raise EventError(
                        f"timeline {event.action} requires an image alias declared by the scenario"
                    )
                target_label = self.scenario.data["images"][image_alias]
                self._authority_image(target_label)
                current_version = self._image_version(simulated_labels[event.instance])
                target_version = self._image_version(target_label)
                if event.action == "upgrade" and target_version <= current_version:
                    raise EventError(
                        f"upgrade of {event.instance!r} must increase protocol generation"
                    )
                if event.action == "downgrade" and target_version >= current_version:
                    raise EventError(
                        f"downgrade of {event.instance!r} must decrease protocol generation"
                    )
                simulated_labels[event.instance] = target_label
            elif event.action == "env_set":
                self._validate_env_update(event)
            elif event.action == "header_edit":
                if instances[event.instance]["role"] != "F":
                    raise EventError("header_edit is only safe for an F instance")
                if set(event.fields) != {"path"}:
                    raise EventError(
                        "header_edit accepts exactly the path field; extra controls are refused"
                    )
                path = event.fields.get("path")
                if (
                    not isinstance(path, str)
                    or not PurePosixPath(path).is_absolute()
                    or ".." in PurePosixPath(path).parts
                    or not (
                        path.startswith("/usr/include/")
                        or path.startswith("/usr/local/include/")
                    )
                ):
                    raise EventError(
                        "header_edit path must be an absolute system header below /usr/include"
                    )
            elif event.action == "disk_fill":
                if instances[event.instance]["role"] != "F" or event.fields:
                    raise EventError("disk_fill requires exactly one F instance and no controls")
                start = next(
                    (
                        item
                        for item in self.plan["commands"]
                        if item.get("instance") == event.instance
                        and str(item.get("phase", "")).startswith("up.start-")
                    ),
                    None,
                )
                expected_mount = (
                    "type=tmpfs,"
                    f"dst={CACHE_DISK_FAULT_PATH},"
                    f"tmpfs-size={CACHE_DISK_FAULT_BYTES},tmpfs-mode=0700"
                )
                if not isinstance(start, Mapping) or expected_mount not in start.get("argv", []):
                    raise UnsupportedEvent(
                        "disk_fill requires the fixed bounded cache tmpfs; refusing before workload"
                    )
            if event.trigger.kind == "time" and float(event.trigger.value) > self.deadline_s:
                raise EventTimeout(
                    f"timeline event {event.index} trigger exceeds turn deadline: {event.trigger_text!r}"
                )

    def _authority_image(self, label: str) -> dict[str, Any]:
        authority = self.farm.data["authority"]["images"].get(label)
        if not isinstance(authority, dict):
            raise EventError(f"timeline image {label!r} is absent from the image authority")
        closure = authority.get("closure_sha256")
        if not isinstance(closure, str) or SHA256_RE.fullmatch(closure) is None:
            raise EventError(f"timeline image {label!r} has no authenticated runtime closure")
        commit = authority.get("commit")
        archive = authority.get("archive_sha256")
        if not isinstance(commit, str) or not isinstance(archive, str):
            raise EventError(f"timeline image {label!r} has incomplete image authority")
        return authority

    def _role_hash(self, label: str, role: str) -> str:
        authority = self._authority_image(label)
        match = IMAGE_LABEL_RE.match(label.rsplit(":", 1)[-1])
        if match is None:
            raise EventError(f"timeline image {label!r} has no protocol generation")
        version = match.group(1)
        role_key = {"S": "scheduler", "C": "client", "F": "daemon"}[role]
        if authority.get("kind") == "scheduler-mutant" and role == "S":
            override = authority.get("role_overrides", {}).get("scheduler")
            digest = override.get("sha256") if isinstance(override, dict) else None
        elif authority.get("kind") == "daemon-mutant" and role == "F":
            override = authority.get("role_overrides", {}).get("daemon")
            digest = override.get("sha256") if isinstance(override, dict) else None
        else:
            store = self.farm.data["authority"]["role_stores"].get(version, {})
            entry = store.get(role_key) if isinstance(store, dict) else None
            digest = entry.get("sha256") if isinstance(entry, dict) else None
        if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
            raise EventError(
                f"timeline image {label!r} has no authenticated {role_key} role hash"
            )
        return digest

    def _validate_env_update(self, event: TimelineEvent) -> None:
        instance = next(item for item in self.plan["topology"]["instances"] if item["name"] == event.instance)
        update = event.fields.get("env")
        if instance["role"] == "S":
            authorized = {
                "ICECC_P50_PROFILE": {"P29V1", "ZSTD_TU", "ZSTD_ROUTE", "OFF"}
            }
        elif instance["role"] == "C":
            authorized = {
                "ICECC_P50_MODE": {"on", "off"},
                P29_FAULT_ENV: {P29_FAULT_VALUE},
            }
        else:
            raise EventError("env_set is only safe for S or C instances")
        if not isinstance(update, dict) or len(update) != 1:
            raise EventError(
                f"env_set on {instance['role']} must set exactly one authorized product setting"
            )
        key, value = next(iter(update.items()))
        if key not in authorized or not isinstance(value, str) or value not in authorized[key]:
            raise EventError("env_set value is not an authorized product setting")

    def _target_instance(
        self, event: TimelineEvent, *, enforce_direction: bool = True
    ) -> dict[str, Any]:
        current = next(item for item in self.plan["topology"]["instances"] if item["name"] == event.instance)
        state = self._state[event.instance]
        target = dict(current)
        target["image"] = dict(state["image"])
        target["env"] = dict(state["env"])
        target["sha256"] = state["sha256"]
        if event.action in {"upgrade", "downgrade"}:
            alias = event.fields["image"]
            label = self.scenario.data["images"][alias]
            authority = self._authority_image(label)
            target["image"] = {
                key: authority[key]
                for key in ("archive_sha256", "commit", "id", "label", "closure_sha256")
                if key in authority
            }
            target["image"]["label"] = label
            target["sha256"] = self._role_hash(label, current["role"])
            current_version = self._image_version(state["image"]["label"])
            target_version = self._image_version(label)
            if enforce_direction and event.action == "upgrade" and target_version <= current_version:
                raise EventError(
                    f"upgrade of {event.instance!r} must increase protocol generation"
                )
            if enforce_direction and event.action == "downgrade" and target_version >= current_version:
                raise EventError(
                    f"downgrade of {event.instance!r} must decrease protocol generation"
                )
            if current["role"] == "S" and self._image_version(label) == 50:
                profile = target["env"].get("ICECC_P50_PROFILE")
                if profile is None:
                    target["env"]["ICECC_P50_PROFILE"] = "P29V1"
            elif current["role"] == "S":
                target["env"].pop("ICECC_P50_PROFILE", None)
            if current["role"] == "C":
                if self._image_version(label) == 50:
                    target["env"].setdefault("ICECC_P50_MODE", "on")
                else:
                    target["env"].pop("ICECC_P50_MODE", None)
                    target["env"].pop(P29_FAULT_ENV, None)
        elif event.action == "env_set":
            target["env"].update(event.fields["env"])
        return target

    @staticmethod
    def _image_version(label: str) -> int:
        match = IMAGE_LABEL_RE.match(label.rsplit(":", 1)[-1])
        if match is None:
            raise EventError(f"image {label!r} has no protocol generation")
        return int(match.group(1))

    def _preflight_transition_targets(self) -> None:
        for event in self.events:
            if event.action not in TRANSITION_ACTIONS:
                continue
            target = self._target_instance(event, enforce_direction=False)
            key = (target["host"], target["image"]["label"], target["role"])
            if key in self._target_preflight:
                continue
            timeout = self._command_timeout() if self._start else int(self.scenario.data["timeouts"]["up_s"])
            image_receipt = _inspect_required_image(
                self.farm,
                target["host"],
                target["image"]["label"],
                self.recorder,
                self.factory,
                timeout,
            )
            runtime_receipt = _materialize_runtime(
                self.farm, target, self.plan["run_id"], self.recorder, self.factory, timeout
            )
            role_receipt = _probe_role_hashes(
                self.farm,
                target["host"],
                target["image"]["label"],
                target["container_image"]["reference"],
                Path(runtime_receipt["path"]),
                {target["role"]: target["sha256"]},
                self.plan["run_id"],
                self.recorder,
                self.factory,
                timeout,
            )
            if role_receipt.get(target["role"]) != target["sha256"]:
                raise EventError(
                    f"target {target['image']['label']} returned an unauthenticated "
                    f"{target['role']} role hash"
                )
            self._target_preflight[key] = {
                "image": image_receipt,
                "runtime": runtime_receipt,
                "role_hashes": role_receipt,
            }

    def _container(self, name: str) -> tuple[str, dict[str, Any]]:
        instance = next(item for item in self.plan["topology"]["instances"] if item["name"] == name)
        return f"icefarm-{self.plan['run_id']}-{name}", instance

    def _invoke(self, command: Any) -> Any:
        self._current_phase = command.phase
        route_phase = {
            "event.client-route-signal": "signal",
            "event.client-route-process-ready": "process_ready",
            "event.client-route-log-ready": "log_ready",
        }.get(command.phase)
        if route_phase is not None:
            self._failure_evidence["route"][route_phase] = {
                "state": "ATTEMPTED",
                "receipt": None,
            }
        result = self.recorder.invoke(command)
        if result.returncode != 0:
            self._mark_failure(command.phase, EventError(
                f"timeline command {command.phase!r} failed with rc={result.returncode}: "
                f"{result.stderr.strip()}"
            ))
            if route_phase is not None:
                self._failure_evidence["route"][route_phase] = {
                    "state": "FAILED",
                    "error": f"rc={result.returncode}: {result.stderr.strip()}",
                    "receipt": None,
                }
            raise EventError(
                f"timeline command {command.phase!r} failed with rc={result.returncode}: "
                f"{result.stderr.strip()}"
            )
        return result

    def _mark_failure(self, phase: str | None, exc: BaseException) -> None:
        if phase:
            self._failure_evidence.setdefault("root_phase", phase)
        route_phase = {
            "event.client-route-signal": "signal",
            "event.client-route-process-ready": "process_ready",
            "event.client-route-log-ready": "log_ready",
        }.get(phase)
        if route_phase is not None:
            self._failure_evidence["route"][route_phase] = {
                "state": "FAILED",
                "error": f"{type(exc).__name__}: {exc}",
                "receipt": None,
            }
        if self._current_gate_key is not None:
            self._failure_evidence["gate_attempts"][self._current_gate_key] = {
                "state": "FAILED",
                "error": f"{type(exc).__name__}: {exc}",
            }

    def _reset_failure_evidence(self) -> None:
        self._failure_evidence = {
            "gate_receipts": {},
            "gate_attempts": {},
            "route": {
                "signal": {"state": "NOT_STARTED", "receipt": None},
                "process_ready": {"state": "NOT_STARTED", "receipt": None},
                "log_ready": {"state": "NOT_STARTED", "receipt": None},
            },
        }
        self._current_gate_key = None
        self._current_phase = None

    def _persist_failure(self, exc: BaseException) -> None:
        event = self._current_event
        payload = {
            "action": event.action if event is not None else None,
            "event_epoch": len(self._records) + 1,
            "event_index": event.index if event is not None else None,
            "exception": f"{type(exc).__name__}: {exc}",
            "evidence": self._failure_evidence,
            "last_completed_epoch": len(self._records),
            "last_phase": self._current_phase,
            "phase": self._failure_evidence.get("root_phase", self._current_phase),
            "schema": "icefarm-event-failure-v1",
            "trigger": event.trigger_text if event is not None else None,
        }
        try:
            if self.failure_path.exists():
                previous = json.loads(self.failure_path.read_text(encoding="utf-8"))
                if previous != payload:
                    raise EventError("event failure descriptor is immutable and differs")
                return
            _atomic_write(self.failure_path, canonical_bytes(payload))
        except EventError:
            raise
        except (OSError, UnicodeError, TypeError, ValueError) as persist_exc:
            raise EventError(f"cannot persist event failure descriptor: {persist_exc}") from persist_exc

    def _inspect(
        self,
        name: str,
        *,
        expected_runtime: str | None = None,
        expected_env: Mapping[str, str] | None = None,
        previous_id: str | None = None,
    ) -> dict[str, Any]:
        container, instance = self._container(name)
        result = self._invoke(
            self.factory.make(
                phase="event.authenticate",
                host=instance["host"],
                instance=name,
                transport=_docker_transport(self.farm, instance["host"]),
                timeout_s=self._command_timeout(),
                argv=docker_argv(
                    self.farm,
                    instance["host"],
                    ("container", "inspect", "--format", "{{json .}}", container),
                ),
            )
        )
        try:
            document = json.loads(result.stdout.strip())
            labels = document["Config"]["Labels"]
            identifier = document["Id"]
            image_id = document.get("Image")
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
            raise EventError(f"container {name!r} authentication output is malformed") from exc
        if (
            not isinstance(labels, dict)
            or labels.get("icefarm.run") != self.plan["run_id"]
            or labels.get("icefarm.instance") != name
            or not isinstance(identifier, str)
            or SHA256_RE.fullmatch(identifier) is None
            or document.get("Name") != f"/{container}"
        ):
            raise EventError(f"container {name!r} is not labelled for this exact run")
        state = document.get("State")
        if previous_id is not None and identifier == previous_id:
            raise EventError(f"container {name!r} did not receive a fresh container ID")
        mounts = document.get("Mounts")
        runtime_mount = next(
            (
                mount.get("Source")
                for mount in mounts
                if isinstance(mount, dict)
                and mount.get("Destination") == "/opt/icecream"
                and isinstance(mount.get("Source"), str)
            ),
            None,
        ) if isinstance(mounts, list) else None
        if expected_runtime is not None and runtime_mount != expected_runtime:
            raise EventError(f"container {name!r} has no authenticated target runtime mount")
        runtime_cache_mount = next(
            (
                mount
                for mount in mounts
                if isinstance(mount, dict)
                and mount.get("Type") == "tmpfs"
                and mount.get("Destination") == CACHE_DISK_FAULT_PATH
                and mount.get("RW") is True
            ),
            None,
        ) if isinstance(mounts, list) else None
        configured_mounts = document.get("HostConfig", {}).get("Mounts")
        configured_cache_mount = next(
            (
                mount
                for mount in configured_mounts
                if isinstance(mount, dict)
                and mount.get("Type") == "tmpfs"
                and mount.get("Target") == CACHE_DISK_FAULT_PATH
            ),
            None,
        ) if isinstance(configured_mounts, list) else None
        tmpfs_options = (
            configured_cache_mount.get("TmpfsOptions")
            if isinstance(configured_cache_mount, dict)
            else None
        )
        cache_fault_mount = (
            {
                "destination": CACHE_DISK_FAULT_PATH,
                "size_bytes": tmpfs_options.get("SizeBytes"),
                "type": "tmpfs",
            }
            if isinstance(runtime_cache_mount, dict)
            and isinstance(tmpfs_options, dict)
            else None
        )
        config_env = document.get("Config", {}).get("Env")
        if expected_env is not None and not isinstance(config_env, list):
            raise EventError(f"container {name!r} has no inspectable environment")
        if not isinstance(config_env, list):
            config_env = []
        observed_env = {
            item.partition("=")[0]: item.partition("=")[2]
            for item in config_env
            if isinstance(item, str) and "=" in item
        }
        managed = {"ICECC_P50_PROFILE", "ICECC_P50_MODE", P29_FAULT_ENV}
        managed_env = {key: observed_env[key] for key in managed if key in observed_env}
        if expected_env is not None:
            for key in managed:
                if managed_env.get(key) != expected_env.get(key):
                    raise EventError(f"container {name!r} has unauthenticated {key} after transition")
        return {
            "id": identifier,
            "image_id": (
                image_id.removeprefix("sha256:")
                if isinstance(image_id, str)
                else None
            ),
            "name": document.get("Name", f"/{container}"),
            "labels": dict(labels),
            "running": state.get("Running") if isinstance(state, dict) else None,
            "started_at": state.get("StartedAt") if isinstance(state, dict) else None,
            "runtime_path": runtime_mount,
            "cache_fault_mount": cache_fault_mount,
            "env": managed_env,
        }

    def _command_timeout(self, maximum: int = 30) -> int:
        remaining = max(0.001, self.deadline_s - (self.monotonic() - self._start))
        return max(1, min(maximum, int(math.ceil(remaining))))

    def _readiness_log(self, instance: Mapping[str, Any]) -> tuple[str, str]:
        leaf = {"S": "scheduler.log", "C": "client-daemon.log", "F": "iceccd.log"}[instance["role"]]
        path = instance_root(self.farm, instance["host"], self.plan["run_id"], instance["name"]) / "log" / leaf
        return instance["host"], str(path)

    def _readiness_baseline(self, instance: Mapping[str, Any]) -> dict[str, Any]:
        host, path = self._readiness_log(instance)
        result = self._invoke(
            self.factory.make(
                phase="event.readiness-baseline",
                host=host,
                instance=instance["name"],
                transport="ssh",
                timeout_s=self._command_timeout(),
                argv=ssh_argv(self.farm, host, ("wc", "-c", "--", path)),
            )
        )
        try:
            offset = int(result.stdout.split()[0]) if result.stdout.split() else 0
        except (TypeError, ValueError) as exc:
            raise EventError("readiness baseline returned a malformed log offset") from exc
        if offset < 0:
            raise EventError("readiness baseline returned a negative log offset")
        return {"host": host, "path": path, "offset": offset}

    def _readiness_witness(
        self, instance: Mapping[str, Any], baseline: Mapping[str, Any]
    ) -> dict[str, Any]:
        deadline = self._start + self.deadline_s
        while self.monotonic() < deadline:
            result = self._invoke(
                self.factory.make(
                    phase="event.readiness",
                    host=baseline["host"],
                    instance=instance["name"],
                    transport="ssh",
                    timeout_s=self._command_timeout(),
                    argv=ssh_argv(
                        self.farm,
                        baseline["host"],
                        (
                            "python3",
                            "-c",
                            READINESS_SCRIPT,
                            baseline["path"],
                            str(baseline["offset"]),
                            instance["role"],
                            instance["name"],
                        ),
                    ),
                )
            )
            try:
                witness = json.loads(result.stdout.strip())
            except (TypeError, ValueError, json.JSONDecodeError) as exc:
                raise EventError("readiness witness returned malformed JSON") from exc
            if isinstance(witness, dict) and witness.get("ready") is True and isinstance(witness.get("line"), str):
                return {
                    "host": baseline["host"],
                    "line": witness["line"],
                    "log_path": baseline["path"],
                    "offset": baseline["offset"],
                    "role": instance["role"],
                }
            self._wake.wait(timeout=min(self.poll_interval_s, max(0.001, deadline - self.monotonic())))
            self._wake.clear()
        raise EventTimeout(f"transition readiness expired for {instance['name']!r}")

    def _header_readiness_witness(
        self, instance: Mapping[str, Any], baseline: Mapping[str, Any]
    ) -> dict[str, Any]:
        deadline = min(
            self._start + self.deadline_s,
            self.monotonic() + float(self.scenario.data["timeouts"]["up_s"]),
        )
        while self.monotonic() < deadline:
            result = self._invoke(
                self.factory.make(
                    phase="event.header-readiness",
                    host=baseline["host"],
                    instance=instance["name"],
                    transport="ssh",
                    timeout_s=self._command_timeout(),
                    argv=ssh_argv(
                        self.farm,
                        baseline["host"],
                        (
                            "python3",
                            "-c",
                            HEADER_READINESS_SCRIPT,
                            baseline["path"],
                            str(baseline["offset"]),
                        ),
                    ),
                )
            )
            try:
                witness = json.loads(result.stdout.strip())
            except (TypeError, ValueError, json.JSONDecodeError) as exc:
                raise EventError("header readiness returned malformed JSON") from exc
            if (
                isinstance(witness, dict)
                and set(witness) == {"bytes", "cache_line", "line", "ready"}
                and witness.get("ready") is True
                and type(witness.get("bytes")) is int
                and witness["bytes"] > 0
                and isinstance(witness.get("line"), str)
                and "ICECREAM daemon " in witness["line"]
                and isinstance(witness.get("cache_line"), str)
                and "cache sidecar adapter state=2 lifecycle=3" in witness["cache_line"]
            ):
                return {
                    "cache_line": witness["cache_line"],
                    "host": baseline["host"],
                    "line": witness["line"],
                    "log_path": baseline["path"],
                    "offset": baseline["offset"],
                    "role": instance["role"],
                }
            self._wake.wait(
                timeout=min(
                    self.poll_interval_s,
                    max(0.001, deadline - self.monotonic()),
                )
            )
            self._wake.clear()
        raise EventTimeout(f"header edit readiness expired for {instance['name']!r}")

    def _header_scheduler_rejoin_witness(
        self,
        scheduler: Mapping[str, Any],
        target: Mapping[str, Any],
        baseline: Mapping[str, Any],
        *,
        include_loss: bool = False,
    ) -> dict[str, Any]:
        profile = scheduler.get("env", {}).get("ICECC_P50_PROFILE")
        if profile not in {"P29V1", "ZSTD_TU", "ZSTD_ROUTE"}:
            raise EventError("header_edit requires a selected P50 scheduler profile")
        deadline = min(
            self._start + self.deadline_s,
            self.monotonic() + float(self.scenario.data["timeouts"]["up_s"]),
        )
        while self.monotonic() < deadline:
            result = self._invoke(
                self.factory.make(
                    phase="event.header-scheduler-rejoin",
                    host=baseline["host"],
                    instance=scheduler["name"],
                    transport="ssh",
                    timeout_s=self._command_timeout(),
                    argv=ssh_argv(
                        self.farm,
                        baseline["host"],
                        (
                            "python3",
                            "-c",
                            SCHEDULER_HEADER_RELOGIN_SCRIPT,
                            baseline["path"],
                            str(baseline["offset"]),
                            target["name"],
                            profile,
                            *(("include-loss",) if include_loss else ()),
                        ),
                    ),
                )
            )
            try:
                witness = json.loads(result.stdout.strip())
            except (TypeError, ValueError, json.JSONDecodeError) as exc:
                raise EventError("scheduler rejoin returned malformed JSON") from exc
            if (
                isinstance(witness, dict)
                and set(witness)
                == {
                    "bytes",
                    "cache_line",
                    "cache_protocol",
                    "login_line",
                    "profile",
                    "ready",
                    "role_protocol",
                    "sha256",
                    "target",
                }
                | ({"loss_job_ids"} if include_loss else set())
                and witness.get("ready") is True
                and type(witness.get("bytes")) is int
                and witness["bytes"] > 0
                and witness.get("target") == target["name"]
                and witness.get("profile") == profile.lower()
                and witness.get("role_protocol") == 50
                and isinstance(witness.get("sha256"), str)
                and re.fullmatch(r"[0-9a-f]{64}", witness["sha256"]) is not None
                and witness.get("cache_protocol") == 1
                and isinstance(witness.get("login_line"), str)
                and re.search(
                    rf"\blogin\s+{re.escape(target['name'])}\s+protocol\s+version:\s*50\b",
                    witness["login_line"],
                )
                and isinstance(witness.get("cache_line"), str)
                and f"RELOGIN {target['name']}" in witness["cache_line"]
                and "cache_wire=v1" in witness["cache_line"]
                and "cache_protocol=1" in witness["cache_line"]
                and f"{profile.lower()}" in witness["cache_line"]
                and (
                    not include_loss
                    or (
                        isinstance(witness.get("loss_job_ids"), list)
                        and len(witness["loss_job_ids"])
                        == len(set(witness["loss_job_ids"]))
                        and all(
                            type(item) is int and item > 0
                            for item in witness["loss_job_ids"]
                        )
                    )
                )
            ):
                result = {
                    "cache_line": witness["cache_line"],
                    "cache_protocol": 1,
                    "host": baseline["host"],
                    "login_line": witness["login_line"],
                    "log_path": baseline["path"],
                    "offset": baseline["offset"],
                    "profile": profile,
                    "role_protocol": 50,
                    "scheduler": scheduler["name"],
                    "target": target["name"],
                }
                if include_loss:
                    result["bytes"] = witness["bytes"]
                    result["loss_job_ids"] = witness["loss_job_ids"]
                    result["sha256"] = witness["sha256"]
                return result
            self._wake.wait(
                timeout=min(
                    self.poll_interval_s,
                    max(0.001, deadline - self.monotonic()),
                )
            )
            self._wake.clear()
        raise EventTimeout(f"scheduler rejoin expired for {target['name']!r}")

    def _gate_control(
        self,
        client: Mapping[str, Any],
        *,
        action: str,
        turn: str,
        epoch: int,
        timeout_s: int,
    ) -> dict[str, Any]:
        key = f"{turn}/{client['name']}/{action}"
        self._current_gate_key = key
        try:
            return self._gate_control_impl(
                client, action=action, turn=turn, epoch=epoch, timeout_s=timeout_s
            )
        except BaseException as exc:
            self._mark_failure(self._current_phase, exc)
            raise
        finally:
            self._current_gate_key = None

    def _gate_control_impl(
        self,
        client: Mapping[str, Any],
        *,
        action: str,
        turn: str,
        epoch: int,
        timeout_s: int,
    ) -> dict[str, Any]:
        container = f"icefarm-{self.plan['run_id']}-{client['name']}"
        key = f"{turn}/{client['name']}/{action}"
        self._failure_evidence["gate_attempts"][key] = {"state": "ATTEMPTED"}
        try:
            result = self._invoke(
                self.factory.make(
                phase={
                    "pause": "event.pause-drain",
                    "quiesce": "event.quiesce-drain",
                    "resume": "event.resume",
                    "abort": "event.abort-resume",
                }[action],
                host=client["host"],
                instance=client["name"],
                transport=_docker_transport(self.farm, client["host"]),
                timeout_s=timeout_s,
                argv=docker_argv(
                    self.farm,
                    client["host"],
                    (
                        "exec",
                        "--user",
                        "0",
                        container,
                        "python3",
                        "-c",
                        GATE_CONTROL_SCRIPT,
                        action,
                        f"/results/workload/{turn}",
                        client["name"],
                        turn,
                        str(epoch),
                        str(max(1, timeout_s - 1)),
                    ),
                ),
                )
            )
        except BaseException as exc:
            self._failure_evidence["gate_attempts"][key] = {
                "state": "FAILED",
                "error": f"{type(exc).__name__}: {exc}",
            }
            raise
        try:
            receipt = json.loads(result.stdout.strip())
        except (TypeError, ValueError, json.JSONDecodeError) as exc:
            raise EventError(f"event gate {action} returned malformed JSON") from exc
        fields = {
            "action",
            "active_after",
            "active_before",
            "client",
            "epoch",
            "finished_ms",
            "schema",
            "started_ms",
            "status",
            "turn",
        }
        expected_status = {
            "pause": "PAUSED",
            "quiesce": "QUIESCED",
            "resume": "OPEN",
            "abort": "ABORT",
        }[action]
        if (
            not isinstance(receipt, dict)
            or set(receipt) != fields
            or receipt.get("schema") != GATE_SCHEMA
            or receipt.get("action") != action
            or receipt.get("status") != expected_status
            or receipt.get("client") != client["name"]
            or receipt.get("turn") != turn
            or receipt.get("epoch") != epoch
            or any(
                type(receipt.get(field)) is not int or receipt[field] < 0
                for field in ("active_after", "active_before", "finished_ms", "started_ms")
            )
            or receipt["finished_ms"] < receipt["started_ms"]
            or (
                action in {"pause", "quiesce"}
                and (
                    receipt["active_before"] < 1
                    or receipt["active_after"] != 0
                )
            )
        ):
            raise EventError(f"event gate {action} returned an invalid receipt")
        self._failure_evidence["gate_receipts"][f"{turn}/{client['name']}/{action}"] = receipt
        self._failure_evidence["gate_attempts"][key] = {
            "state": "SUCCEEDED",
            "receipt": receipt,
        }
        return receipt

    def _wait_scheduler_client_readiness(
        self,
        client: Mapping[str, Any],
        baseline: Mapping[str, Any],
    ) -> dict[str, Any]:
        deadline = min(
            self._start + self.deadline_s,
            self.monotonic() + float(self.scenario.data["timeouts"]["up_s"]),
        )
        cache_required = (
            client.get("version") == 50
            and client.get("env", {}).get("ICECC_P50_MODE") == "on"
        )
        while self.monotonic() < deadline:
            result = self._invoke(
                self.factory.make(
                    phase="event.client-scheduler-ready",
                    host=baseline["host"],
                    instance=client["name"],
                    transport="ssh",
                    timeout_s=self._command_timeout(),
                    argv=ssh_argv(
                        self.farm,
                        baseline["host"],
                        (
                            "python3",
                            "-c",
                            CLIENT_SCHEDULER_READINESS_SCRIPT,
                            baseline["path"],
                            str(baseline["offset"]),
                            "1" if cache_required else "0",
                        ),
                    ),
                )
            )
            try:
                witness = json.loads(result.stdout.strip())
            except (TypeError, ValueError, json.JSONDecodeError) as exc:
                raise EventError("client scheduler readiness returned malformed JSON") from exc
            if not isinstance(witness, dict) or set(witness) != {
                "bytes",
                "cache_line",
                "cache_required",
                "connected_line",
                "ready",
            }:
                raise EventError("client scheduler readiness returned an invalid witness")
            if witness.get("ready") is True:
                connected = witness.get("connected_line")
                cache_line = witness.get("cache_line")
                if (
                    witness.get("cache_required") is not cache_required
                    or type(witness.get("bytes")) is not int
                    or witness["bytes"] < 1
                    or not isinstance(connected, str)
                    or "Connected to scheduler (I am known as " not in connected
                    or (
                        cache_required
                        and (
                            not isinstance(cache_line, str)
                            or re.search(
                                r"cache sidecar adapter state=2 lifecycle=3",
                                cache_line,
                            )
                            is None
                        )
                    )
                    or (not cache_required and cache_line is not None)
                ):
                    raise EventError("client scheduler readiness is inconsistent")
                return {
                    "cache_line": cache_line,
                    "cache_required": cache_required,
                    "connected_line": connected,
                    "host": baseline["host"],
                    "log_path": baseline["path"],
                    "offset": baseline["offset"],
                }
            self._wake.wait(
                timeout=min(
                    self.poll_interval_s,
                    max(0.001, deadline - self.monotonic()),
                )
            )
            self._wake.clear()
        raise EventTimeout(
            f"client did not rejoin the scheduler after restart: {client['name']!r}"
        )

    def _wait_scheduler_worker_rejoin(
        self,
        scheduler: Mapping[str, Any],
        target: Mapping[str, Any],
        baseline: Mapping[str, Any],
    ) -> dict[str, Any]:
        """Require a fresh, target-bound worker login after an F transition."""

        protocol = self._image_version(target["image"]["label"])
        deadline = min(
            self._start + self.deadline_s,
            self.monotonic() + float(self.scenario.data["timeouts"]["up_s"]),
        )
        while self.monotonic() < deadline:
            result = self._invoke(
                self.factory.make(
                    phase="event.scheduler-worker-rejoin",
                    host=baseline["host"],
                    instance=scheduler["name"],
                    transport="ssh",
                    timeout_s=self._command_timeout(),
                    argv=ssh_argv(
                        self.farm,
                        baseline["host"],
                        (
                            "python3",
                            "-c",
                            SCHEDULER_WORKER_REJOIN_SCRIPT,
                            baseline["path"],
                            str(baseline["offset"]),
                            target["name"],
                            str(protocol),
                        ),
                    ),
                )
            )
            try:
                witness = json.loads(result.stdout.strip())
            except (TypeError, ValueError, json.JSONDecodeError) as exc:
                raise EventError("scheduler worker rejoin returned malformed JSON") from exc
            if (
                isinstance(witness, dict)
                and set(witness) == {"bytes", "login_line", "ready", "role_protocol", "target"}
                and witness.get("ready") is True
                and type(witness.get("bytes")) is int
                and witness["bytes"] > 0
                and witness.get("target") == target["name"]
                and witness.get("role_protocol") == protocol
                and isinstance(witness.get("login_line"), str)
                and re.search(
                    rf"\blogin\s+{re.escape(target['name'])}\s+protocol\s+version:\s*{protocol}\b",
                    witness["login_line"],
                )
            ):
                return {
                    "bytes": witness["bytes"],
                    "host": baseline["host"],
                    "line": witness["login_line"],
                    "log_path": baseline["path"],
                    "offset": baseline["offset"],
                    "role_protocol": protocol,
                    "target": target["name"],
                }
            self._wake.wait(
                timeout=min(
                    self.poll_interval_s,
                    max(0.001, deadline - self.monotonic()),
                )
            )
            self._wake.clear()
        raise EventTimeout(f"worker {target['name']!r} did not rejoin the scheduler")

    def _coordinated_transition(
        self, event: TimelineEvent, instance: Mapping[str, Any]
    ) -> dict[str, Any]:
        """Replace an S/F image only across an authenticated readiness boundary."""

        if instance["role"] == "C":
            raise UnsupportedEvent(
                "C image transitions require a checkpointed workload driver; refusing before workload"
            )
        with self._lock:
            turn = self._active_turn
            epoch = len(self._records) + 1
        clients = []
        if turn is not None:
            client_names = set(self.scenario.data["workload"]["clients"])
            clients = sorted(
                (
                    item
                    for item in self.plan["topology"]["instances"]
                    if item["role"] == "C" and item["name"] in client_names
                ),
                key=lambda item: item["name"],
            )
            if {item["name"] for item in clients} != client_names or not clients:
                raise EventError("transition cannot resolve every workload client")

        before_state = {
            "image": dict(self._state[event.instance]["image"]),
            "env": dict(self._state[event.instance]["env"]),
            "sha256": self._state[event.instance]["sha256"],
        }
        before = self._inspect(
            event.instance,
            expected_runtime=str(runtime_root(self.farm, instance | before_state)),
            expected_env=before_state["env"],
        )
        before_started = before.get("started_at")
        if (
            before.get("running") is not True
            or not isinstance(before_started, str)
            or not before_started
        ):
            raise EventError("transition has no authenticated running pre-state")
        target = self._target_instance(event)
        before_id = before["id"]
        start_argv = self._start_argv(event, target)
        pauses: dict[str, dict[str, Any]] = {}
        resumes: dict[str, dict[str, Any]] = {}
        paused: list[Mapping[str, Any]] = []
        primary: BaseException | None = None
        after: dict[str, Any] | None = None
        readiness: dict[str, Any] | None = None
        scheduler_snapshot: str | None = None
        worker_snapshot: str | None = None
        scheduler_startup: dict[str, Any] | None = None
        scheduler_worker_rejoin: dict[str, Any] | None = None
        client_readiness: dict[str, dict[str, Any]] = {}
        ready_ms: int | None = None
        drain_timeout = self._command_timeout(int(self.scenario.data["timeouts"]["turn_s"]))
        scheduler = next(
            item for item in self.plan["topology"]["instances"] if item["role"] == "S"
        )
        try:
            for client in clients:
                pauses[client["name"]] = self._gate_control(
                    client,
                    action="pause",
                    turn=turn,
                    epoch=epoch,
                    timeout_s=drain_timeout,
                )
                paused.append(client)
            client_baselines = {
                client["name"]: self._readiness_baseline(client) for client in clients
            }
            target_baseline = self._readiness_baseline(instance)
            scheduler_baseline = (
                self._readiness_baseline(scheduler) if instance["role"] == "F" else None
            )
            for operation, args in (
                (
                    "stop",
                    ("container", "stop", "--time", "10", before_id),
                ),
                ("remove", ("container", "rm", "--force", before_id)),
            ):
                self._invoke(
                    self.factory.make(
                        phase=f"event.{event.action}.{operation}",
                        host=instance["host"],
                        instance=event.instance,
                        transport=_docker_transport(self.farm, instance["host"]),
                        timeout_s=self._command_timeout(),
                        argv=docker_argv(self.farm, instance["host"], args),
                    )
                )
            self._invoke(
                self.factory.make(
                    phase=f"event.{event.action}.start",
                    host=instance["host"],
                    instance=event.instance,
                    transport=_docker_transport(self.farm, instance["host"]),
                    timeout_s=self._command_timeout(),
                    argv=start_argv,
                )
            )
            after = self._inspect(
                event.instance,
                expected_runtime=str(runtime_root(self.farm, target)),
                expected_env=target["env"],
                previous_id=before_id,
            )
            after_started = after.get("started_at")
            if (
                after.get("running") is not True
                or not isinstance(after_started, str)
                or not after_started
                or after_started == before_started
            ):
                raise EventError("transition has no authenticated fresh running post-state")
            readiness = self._readiness_witness(instance, target_baseline)
            readiness_deadline = min(
                self._start + self.deadline_s,
                self.monotonic() + float(self.scenario.data["timeouts"]["up_s"]),
            )
            if instance["role"] == "S":
                scheduler_startup = readiness
                scheduler_snapshot = _wait_scheduler(
                    self.farm,
                    self.plan,
                    self.recorder,
                    self.factory,
                    deadline=readiness_deadline,
                    monotonic=self.monotonic,
                    sleeper=time.sleep,
                )
                worker_snapshot = _wait_workers(
                    self.farm,
                    self.plan,
                    self.recorder,
                    self.factory,
                    deadline=readiness_deadline,
                    monotonic=self.monotonic,
                    sleeper=time.sleep,
                )
                client_readiness = {
                    client["name"]: self._wait_scheduler_client_readiness(
                        client, client_baselines[client["name"]]
                    )
                    for client in clients
                }
            else:
                assert scheduler_baseline is not None
                scheduler_worker_rejoin = self._wait_scheduler_worker_rejoin(
                    scheduler, target, scheduler_baseline
                )
                worker_snapshot = _wait_workers(
                    self.farm,
                    self.plan,
                    self.recorder,
                    self.factory,
                    deadline=readiness_deadline,
                    monotonic=self.monotonic,
                    sleeper=time.sleep,
                )
                scheduler_snapshot = worker_snapshot
            ready_ms = int(self.wall_ms())
        except BaseException as exc:
            # Preserve the first failing phase before the abort/release path
            # advances ``_current_phase``.  This also covers rc=0
            # parse/identity failures after a command returned.
            self._mark_failure(self._current_phase, exc)
            primary = exc
        finally:
            release = "abort" if primary is not None else "resume"
            release_errors: list[str] = []
            for client in paused:
                try:
                    resumes[client["name"]] = self._gate_control(
                        client,
                        action=release,
                        turn=turn,
                        epoch=epoch,
                        timeout_s=self._command_timeout(),
                    )
                except BaseException as exc:
                    release_errors.append(f"{client['name']}: {exc}")
            if primary is not None:
                detail = f"; resume errors: {'; '.join(release_errors)}" if release_errors else ""
                raise EventError(f"coordinated transition failed: {primary}{detail}") from primary
            if release_errors:
                raise EventError(
                    "coordinated transition could not release every client: "
                    + "; ".join(release_errors)
                )

        assert after is not None and readiness is not None and ready_ms is not None
        assert worker_snapshot is not None and scheduler_snapshot is not None
        preflight = self._target_preflight[
            (target["host"], target["image"]["label"], target["role"])
        ]
        self._state[event.instance] = {
            "image": dict(target["image"]),
            "env": dict(target["env"]),
            "sha256": target["sha256"],
        }
        self._start_argvs[event.instance] = start_argv
        coordination: dict[str, Any] = {
            "clients": pauses,
            "ready_ms": ready_ms,
            "resume": resumes,
            "scheduler_snapshot": scheduler_snapshot,
            "worker_snapshot": worker_snapshot,
            "workers": sorted(
                item["name"]
                for item in self.plan["topology"]["instances"]
                if item["role"] == "F"
            ),
        }
        if instance["role"] == "S":
            coordination.update(
                {
                    "client_readiness": client_readiness,
                    "scheduler_startup": scheduler_startup,
                }
            )
        else:
            assert scheduler_worker_rejoin is not None
            coordination["scheduler_worker_rejoin"] = scheduler_worker_rejoin
        before_state["env"] = dict(before["env"])
        after_state = dict(self._state[event.instance])
        after_state["env"] = dict(after["env"])
        return {
            "action": event.action,
            "after": self._snapshot(state=after_state, container_id=after["id"]),
            "before": self._snapshot(state=before_state, container_id=before_id),
            "coordination": coordination,
            "event_epoch": epoch,
            "instance": event.instance,
            "preflight": {
                "image_closure_sha256": preflight["image"]["closure_sha256"],
                "role_sha256": preflight["role_hashes"][target["role"]],
                "runtime_path": preflight["runtime"]["path"],
            },
            "readiness": readiness,
            "schema": TRANSITION_SCHEMA,
            "turn": turn,
        }

    @staticmethod
    def _checkpoint_receipt(
        value: Any,
        *,
        client: str,
        turn: str,
    ) -> dict[str, Any]:
        fields = {
            "checkpoint_sha256",
            "client",
            "completed_rows",
            "completed_rows_sha256",
            "expected_jobs",
            "schema",
            "status",
            "turn",
            "worklist_sha256",
        }
        if not isinstance(value, dict) or set(value) != fields:
            raise EventError(f"client {client!r} returned an invalid checkpoint receipt")
        if (
            value["schema"] != "icefarm-workload-checkpoint-v1"
            or value["status"] != "QUIESCED"
            or value["client"] != client
            or value["turn"] != turn
            or type(value["expected_jobs"]) is not int
            or value["expected_jobs"] < 1
            or not SHA256_RE.fullmatch(value["checkpoint_sha256"])
            or not SHA256_RE.fullmatch(value["completed_rows_sha256"])
            or not SHA256_RE.fullmatch(value["worklist_sha256"])
            or not isinstance(value["completed_rows"], list)
            or not value["completed_rows"]
        ):
            raise EventError(f"client {client!r} returned an unauthenticated checkpoint")
        rows_bytes = json.dumps(
            value["completed_rows"], sort_keys=True, separators=(",", ":")
        ).encode()
        if value["completed_rows_sha256"] != hashlib.sha256(rows_bytes).hexdigest():
            raise EventError(f"client {client!r} returned a tampered checkpoint row digest")
        body = dict(value)
        digest = body.pop("checkpoint_sha256")
        if digest != hashlib.sha256(
            json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest():
            raise EventError(f"client {client!r} returned a tampered checkpoint receipt")
        seen: set[int] = set()
        for row in value["completed_rows"]:
            if not isinstance(row, dict) or set(row) != {"index", "path", "sha256"}:
                raise EventError(f"client {client!r} returned malformed checkpoint rows")
            if (
                type(row["index"]) is not int
                or not 1 <= row["index"] <= value["expected_jobs"]
                or row["index"] in seen
                or not isinstance(row["path"], str)
                or row["path"] != f"jobs/{row['index']:06d}/result.tsv"
                or not isinstance(row["sha256"], str)
                or SHA256_RE.fullmatch(row["sha256"]) is None
            ):
                raise EventError(f"client {client!r} returned duplicate or invalid checkpoint rows")
            seen.add(row["index"])
        return value

    def _coordinated_client_transition(
        self,
        event: TimelineEvent,
        instance: Mapping[str, Any],
        identifier: str | None,
    ) -> dict[str, Any]:
        """Checkpoint every workload client before replacing one C container."""

        if event.trigger.kind != "job":
            raise UnsupportedEvent("C transitions require a job-triggered quiesce boundary")
        if self.quiesce_workload is None or self.relaunch_workload is None:
            raise UnsupportedEvent("C transitions require a checkpointed workload driver")
        with self._lock:
            turn = self._active_turn
            epoch = len(self._records) + 1
        if turn is None:
            raise EventError("C transition fired outside an active workload turn")
        client_names = set(self.scenario.data["workload"]["clients"])
        clients = tuple(
            sorted(
                (
                    item
                    for item in self.plan["topology"]["instances"]
                    if item["role"] == "C" and item["name"] in client_names
                ),
                key=lambda item: item["name"],
            )
        )
        if {item["name"] for item in clients} != client_names or not clients:
            raise EventError("C transition cannot resolve every workload client")
        before_state = {
            "image": dict(self._state[event.instance]["image"]),
            "env": dict(self._state[event.instance]["env"]),
            "sha256": self._state[event.instance]["sha256"],
        }
        before = self._inspect(
            event.instance,
            expected_runtime=str(runtime_root(self.farm, instance | before_state)),
            expected_env=before_state["env"],
        )
        if before.get("running") is not True:
            raise EventError("C transition has no authenticated running pre-state")
        before_id = before["id"] if identifier is None else identifier
        if before_id != before["id"]:
            raise EventError("C transition identifier is not the authenticated container")
        target = dict(instance)
        target["image"] = dict(before_state["image"])
        target["env"] = dict(before_state["env"])
        target["sha256"] = before_state["sha256"]
        if event.action in {"upgrade", "downgrade", "env_set"}:
            target = self._target_instance(event)
        start_argv = self._start_argv(event, target)
        client_baselines = {
            item["name"]: self._readiness_baseline(item) for item in clients
        }
        pauses: dict[str, dict[str, Any]] = {}
        resumes: dict[str, dict[str, Any]] = {}
        checkpoints: Mapping[str, Any] | None = None
        relaunch: Mapping[str, Any] | None = None
        readiness: dict[str, Any] | None = None
        client_readiness: dict[str, Any] | None = None
        after: dict[str, Any] | None = None
        primary: BaseException | None = None
        drain_timeout = self._command_timeout(int(self.scenario.data["timeouts"]["turn_s"]))
        try:
            for client in clients:
                pauses[client["name"]] = self._gate_control(
                    client,
                    action="quiesce",
                    turn=turn,
                    epoch=epoch,
                    timeout_s=drain_timeout,
                )
            raw_checkpoints = self.quiesce_workload(turn, clients)
            if not isinstance(raw_checkpoints, Mapping) or set(raw_checkpoints) != {
                item["name"] for item in clients
            }:
                raise EventError("C transition checkpoint set does not match workload clients")
            checkpoints = {
                name: self._checkpoint_receipt(raw_checkpoints[name], client=name, turn=turn)
                for name in sorted(raw_checkpoints)
            }
            self._failure_evidence["checkpoint_clients"] = sorted(checkpoints)
            if event.instance not in checkpoints:
                raise EventError("target C has no authenticated checkpoint")
            for operation, args in (
                ("stop", ("container", "stop", "--time", "10", before_id)),
                ("remove", ("container", "rm", "--force", before_id)),
            ):
                self._invoke(
                    self.factory.make(
                        phase=f"event.{event.action}.{operation}",
                        host=instance["host"],
                        instance=event.instance,
                        transport=_docker_transport(self.farm, instance["host"]),
                        timeout_s=self._command_timeout(),
                        argv=docker_argv(self.farm, instance["host"], args),
                    )
                )
            self._invoke(
                self.factory.make(
                    phase=f"event.{event.action}.start",
                    host=instance["host"],
                    instance=event.instance,
                    transport=_docker_transport(self.farm, instance["host"]),
                    timeout_s=self._command_timeout(),
                    argv=start_argv,
                )
            )
            after = self._inspect(
                event.instance,
                expected_runtime=str(runtime_root(self.farm, target)),
                expected_env=target["env"],
                previous_id=before_id,
            )
            if after.get("running") is not True:
                raise EventError("C transition did not produce a running container")
            readiness = self._readiness_witness(instance, client_baselines[event.instance])
            client_readiness = self._wait_scheduler_client_readiness(
                target, client_baselines[event.instance]
            )
            for client in clients:
                resumes[client["name"]] = self._gate_control(
                    client,
                    action="resume",
                    turn=turn,
                    epoch=epoch,
                    timeout_s=self._command_timeout(),
                )
            raw_relaunch = self.relaunch_workload(turn, checkpoints)
            if not isinstance(raw_relaunch, Mapping) or set(raw_relaunch) != {
                item["name"] for item in clients
            }:
                raise EventError("C transition relaunch set does not match workload clients")
            relaunch = dict(raw_relaunch)
            self._failure_evidence["relaunch_clients"] = sorted(relaunch)
            for name, evidence in relaunch.items():
                if (
                    not isinstance(evidence, Mapping)
                    or set(evidence) != {"client", "expected_jobs", "jobs", "failures", "status"}
                    or evidence["client"] != name
                    or evidence["status"] != "COMPLETE"
                    or type(evidence["expected_jobs"]) is not int
                    or type(evidence["jobs"]) is not int
                    or type(evidence["failures"]) is not int
                    or evidence["jobs"] != evidence["expected_jobs"]
                    or evidence["failures"] != 0
                ):
                    raise EventError(f"client {name!r} returned incomplete relaunch evidence")
        except BaseException as exc:
            self._mark_failure(self._current_phase, exc)
            primary = exc
        finally:
            if primary is not None or relaunch is None:
                for client in clients:
                    try:
                        resumes[client["name"]] = self._gate_control(
                            client,
                            action="abort",
                            turn=turn,
                            epoch=epoch,
                            timeout_s=self._command_timeout(),
                        )
                    except BaseException:
                        pass
            if primary is not None:
                raise EventError(f"checkpointed C transition failed: {primary}") from primary
        assert after is not None and readiness is not None and client_readiness is not None
        assert checkpoints is not None and relaunch is not None
        self._state[event.instance] = {
            "image": dict(target["image"]),
            "env": dict(target["env"]),
            "sha256": target["sha256"],
        }
        self._start_argvs[event.instance] = start_argv
        preflight = self._target_preflight.get(
            (target["host"], target["image"]["label"], target["role"]),
            {
                "image": {"closure_sha256": target["image"]["closure_sha256"]},
                "role_hashes": {target["role"]: target["sha256"]},
                "runtime": {"path": str(runtime_root(self.farm, target))},
            },
        )
        return {
            "action": event.action,
            "after": self._snapshot(state={**target, "env": after["env"]}, container_id=after["id"]),
            "before": self._snapshot(state={**before_state, "env": before["env"]}, container_id=before_id),
            "checkpoints": checkpoints,
            "client_readiness": client_readiness,
            "coordination": {
                "clients": pauses,
                "ready_ms": int(self.wall_ms()),
                "relaunch": relaunch,
                "resume": resumes,
            },
            "event_epoch": epoch,
            "instance": event.instance,
            "preflight": {
                "image_closure_sha256": preflight["image"]["closure_sha256"],
                "role_sha256": preflight["role_hashes"][target["role"]],
                "runtime_path": preflight["runtime"]["path"],
            },
            "readiness": readiness,
            "schema": CLIENT_TRANSITION_SCHEMA,
            "turn": turn,
        }

    def _coordinated_scheduler_restart(
        self, event: TimelineEvent, instance: Mapping[str, Any], identifier: str
    ) -> dict[str, Any]:
        with self._lock:
            turn = self._active_turn
            epoch = len(self._records) + 1
        if event.trigger.kind != "job" or turn is None:
            raise EventError(
                "a scheduler restart must be job-triggered during an authenticated workload turn"
            )
        client_names = set(self.scenario.data["workload"]["clients"])
        clients = sorted(
            (
                item
                for item in self.plan["topology"]["instances"]
                if item["role"] == "C" and item["name"] in client_names
            ),
            key=lambda item: item["name"],
        )
        if {item["name"] for item in clients} != client_names or not clients:
            raise EventError("scheduler restart cannot resolve every workload client")

        before = self._inspect(event.instance)
        before_started = before.get("started_at")
        if before.get("running") is not True or not isinstance(before_started, str) or not before_started:
            raise EventError("scheduler restart has no authenticated running pre-state")

        pause_receipts: dict[str, dict[str, Any]] = {}
        resume_receipts: dict[str, dict[str, Any]] = {}
        primary: BaseException | None = None
        startup: dict[str, Any] | None = None
        scheduler_snapshot: str | None = None
        worker_snapshot: str | None = None
        client_readiness: dict[str, dict[str, Any]] = {}
        after: dict[str, Any] | None = None
        ready_ms: int | None = None
        drain_timeout = self._command_timeout(
            int(self.scenario.data["timeouts"]["turn_s"])
        )
        try:
            for client in clients:
                pause_receipts[client["name"]] = self._gate_control(
                    client,
                    action="pause",
                    turn=turn,
                    epoch=epoch,
                    timeout_s=drain_timeout,
                )
            client_baselines = {
                client["name"]: self._readiness_baseline(client)
                for client in clients
            }
            readiness_baseline = self._readiness_baseline(instance)
            self._invoke(
                self.factory.make(
                    phase="event.restart",
                    host=instance["host"],
                    instance=event.instance,
                    transport=_docker_transport(self.farm, instance["host"]),
                    timeout_s=self._command_timeout(),
                    argv=docker_argv(
                        self.farm,
                        instance["host"],
                        ("container", "restart", "--time", "10", identifier),
                    ),
                )
            )
            after = self._inspect(event.instance)
            after_started = after.get("started_at")
            if (
                after.get("running") is not True
                or after.get("id") != identifier
                or not isinstance(after_started, str)
                or not after_started
                or after_started == before_started
            ):
                raise EventError("scheduler restart has no authenticated fresh running post-state")
            startup = self._readiness_witness(instance, readiness_baseline)
            readiness_deadline = min(
                self._start + self.deadline_s,
                self.monotonic() + float(self.scenario.data["timeouts"]["up_s"]),
            )
            scheduler_snapshot = _wait_scheduler(
                self.farm,
                self.plan,
                self.recorder,
                self.factory,
                deadline=readiness_deadline,
                monotonic=self.monotonic,
                sleeper=time.sleep,
            )
            worker_snapshot = _wait_workers(
                self.farm,
                self.plan,
                self.recorder,
                self.factory,
                deadline=readiness_deadline,
                monotonic=self.monotonic,
                sleeper=time.sleep,
            )
            client_readiness = {
                client["name"]: self._wait_scheduler_client_readiness(
                    client, client_baselines[client["name"]]
                )
                for client in clients
            }
            ready_ms = int(self.wall_ms())
        except BaseException as exc:
            self._mark_failure(self._current_phase, exc)
            primary = exc
        finally:
            resume_action = "abort" if primary is not None else "resume"
            resume_errors: list[str] = []
            for client in clients:
                try:
                    resume_receipts[client["name"]] = self._gate_control(
                        client,
                        action=resume_action,
                        turn=turn,
                        epoch=epoch,
                        timeout_s=self._command_timeout(),
                    )
                except BaseException as exc:
                    resume_errors.append(f"{client['name']}: {exc}")
            if resume_errors and primary is None:
                for client in clients:
                    try:
                        self._gate_control(
                            client,
                            action="abort",
                            turn=turn,
                            epoch=epoch,
                            timeout_s=self._command_timeout(),
                        )
                    except BaseException as exc:
                        resume_errors.append(f"abort {client['name']}: {exc}")
            if primary is not None:
                detail = f"; resume errors: {'; '.join(resume_errors)}" if resume_errors else ""
                raise EventError(f"coordinated scheduler restart failed: {primary}{detail}") from primary
            if resume_errors:
                raise EventError(
                    "coordinated scheduler restart could not release every client: "
                    + "; ".join(resume_errors)
                )

        assert after is not None
        assert startup is not None
        assert scheduler_snapshot is not None
        assert worker_snapshot is not None
        assert ready_ms is not None
        workers = sorted(
            item["name"]
            for item in self.plan["topology"]["instances"]
            if item["role"] == "F"
        )
        return {
            "action": event.action,
            "after": {"container_id": after["id"], "started_at": after["started_at"]},
            "before": {"container_id": before["id"], "started_at": before_started},
            "coordination": {
                "clients": pause_receipts,
                "client_readiness": client_readiness,
                "ready_ms": ready_ms,
                "resume": resume_receipts,
                "scheduler_snapshot": scheduler_snapshot,
                "scheduler_startup": startup,
                "workers": workers,
                "worker_snapshot": worker_snapshot,
            },
            "event_epoch": epoch,
            "instance": event.instance,
            "schema": SCHEDULER_RESTART_SCHEMA,
            "turn": turn,
        }

    @staticmethod
    def _valid_process_snapshot(value: Any, executable: str) -> bool:
        return (
            isinstance(value, dict)
            and set(value)
            == {"argv", "exe", "exe_evidence", "pid", "ppid", "start_ticks", "uid"}
            and value.get("exe") == executable
            and value.get("exe_evidence")
            in {"proc-exe", "argv0-after-proc-exe-eacces"}
            and isinstance(value.get("argv"), list)
            and bool(value["argv"])
            and value["argv"][0] == executable
            and all(isinstance(item, str) and "\0" not in item for item in value["argv"])
            and all(
                type(value.get(field)) is int and value[field] >= minimum
                for field, minimum in (
                    ("pid", 1),
                    ("ppid", 0),
                    ("start_ticks", 1),
                    ("uid", 0),
                )
            )
        )

    def _signal_client_route_owner(self, instance: Mapping[str, Any]) -> dict[str, Any]:
        try:
            return self._signal_client_route_owner_impl(instance)
        except BaseException as exc:
            self._mark_failure("event.client-route-signal", exc)
            raise

    def _signal_client_route_owner_impl(self, instance: Mapping[str, Any]) -> dict[str, Any]:
        container = f"icefarm-{self.plan['run_id']}-{instance['name']}"
        result = self._invoke(
            self.factory.make(
                phase="event.client-route-signal",
                host=instance["host"],
                instance=instance["name"],
                transport=_docker_transport(self.farm, instance["host"]),
                timeout_s=self._command_timeout(),
                argv=docker_argv(
                    self.farm,
                    instance["host"],
                    (
                        "exec",
                        "--user",
                        "0",
                        container,
                        "python3",
                        "-c",
                        CLIENT_ROUTE_SIGNAL_SCRIPT,
                        "/opt/icecream/sbin/iceccd",
                        "/opt/icecream/sbin/icecc-cache-service",
                    ),
                ),
            )
        )
        try:
            receipt = json.loads(result.stdout.strip())
        except (TypeError, ValueError, json.JSONDecodeError) as exc:
            raise EventError("client route-owner signal returned malformed JSON") from exc
        if (
            not isinstance(receipt, dict)
            or set(receipt)
            != {"daemon", "mechanism", "route_owner", "schema", "sent_ms", "signal"}
            or receipt.get("schema") != CLIENT_ROUTE_SIGNAL_SCHEMA
            or receipt.get("mechanism") != "pidfd_send_signal"
            or receipt.get("signal") != 9
            or type(receipt.get("sent_ms")) is not int
            or receipt["sent_ms"] < 0
            or not self._valid_process_snapshot(
                receipt.get("daemon"), "/opt/icecream/sbin/iceccd"
            )
            or not self._valid_process_snapshot(
                receipt.get("route_owner"),
                "/opt/icecream/sbin/icecc-cache-service",
            )
            or receipt["route_owner"]["pid"] <= 1
            or receipt["route_owner"]["ppid"] != receipt["daemon"]["pid"]
            or receipt["route_owner"]["uid"] != receipt["daemon"]["uid"]
        ):
            raise EventError("client route-owner signal returned an invalid receipt")
        self._failure_evidence["route"]["signal"] = {
            "state": "SUCCEEDED",
            "receipt": receipt,
        }
        return receipt

    def _wait_client_route_owner(
        self,
        instance: Mapping[str, Any],
        before: Mapping[str, Any],
    ) -> dict[str, Any]:
        try:
            return self._wait_client_route_owner_impl(instance, before)
        except BaseException as exc:
            self._mark_failure("event.client-route-process-ready", exc)
            raise

    def _wait_client_route_owner_impl(
        self,
        instance: Mapping[str, Any],
        before: Mapping[str, Any],
    ) -> dict[str, Any]:
        container = f"icefarm-{self.plan['run_id']}-{instance['name']}"
        deadline = min(
            self._start + self.deadline_s,
            self.monotonic() + float(self.scenario.data["timeouts"]["up_s"]),
        )
        while self.monotonic() < deadline:
            result = self._invoke(
                self.factory.make(
                    phase="event.client-route-process-ready",
                    host=instance["host"],
                    instance=instance["name"],
                    transport=_docker_transport(self.farm, instance["host"]),
                    timeout_s=self._command_timeout(),
                    argv=docker_argv(
                        self.farm,
                        instance["host"],
                        (
                            "exec",
                            "--user",
                            "0",
                            container,
                            "python3",
                            "-c",
                            CLIENT_ROUTE_SNAPSHOT_SCRIPT,
                            "/opt/icecream/sbin/iceccd",
                            "/opt/icecream/sbin/icecc-cache-service",
                        ),
                    ),
                )
            )
            try:
                snapshot = json.loads(result.stdout.strip())
            except (TypeError, ValueError, json.JSONDecodeError) as exc:
                raise EventError("client route-owner readiness returned malformed JSON") from exc
            if not isinstance(snapshot, dict) or set(snapshot) != {
                "daemon",
                "daemon_count",
                "ready",
                "route_owner",
                "route_owner_count",
                "schema",
            }:
                raise EventError("client route-owner readiness returned an invalid snapshot")
            if snapshot.get("schema") != "icefarm-client-route-snapshot-v1":
                raise EventError("client route-owner readiness returned the wrong schema")
            if snapshot.get("ready") is True:
                daemon = snapshot.get("daemon")
                route_owner = snapshot.get("route_owner")
                if (
                    snapshot.get("daemon_count") != 1
                    or snapshot.get("route_owner_count") != 1
                    or not self._valid_process_snapshot(
                        daemon, "/opt/icecream/sbin/iceccd"
                    )
                    or not self._valid_process_snapshot(
                        route_owner,
                        "/opt/icecream/sbin/icecc-cache-service",
                    )
                    or route_owner["ppid"] != daemon["pid"]
                    or route_owner["uid"] != daemon["uid"]
                ):
                    raise EventError("client route-owner readiness is internally inconsistent")
                if daemon != before["daemon"]:
                    raise EventError("client daemon changed during route-owner restart")
                old_identity = (
                    before["route_owner"]["pid"],
                    before["route_owner"]["start_ticks"],
                )
                new_identity = (route_owner["pid"], route_owner["start_ticks"])
                if new_identity != old_identity:
                    self._failure_evidence["route"]["process_ready"] = {
                        "state": "SUCCEEDED",
                        "receipt": snapshot,
                    }
                    return {"daemon": daemon, "route_owner": route_owner}
            self._wake.wait(
                timeout=min(
                    self.poll_interval_s,
                    max(0.001, deadline - self.monotonic()),
                )
            )
            self._wake.clear()
        raise EventTimeout(
            f"client route owner did not obtain a fresh process identity for {instance['name']!r}"
        )

    def _wait_client_route_readiness(
        self,
        instance: Mapping[str, Any],
        baseline: Mapping[str, Any],
    ) -> dict[str, Any]:
        try:
            return self._wait_client_route_readiness_impl(instance, baseline)
        except BaseException as exc:
            self._mark_failure("event.client-route-log-ready", exc)
            raise

    def _wait_client_route_readiness_impl(
        self,
        instance: Mapping[str, Any],
        baseline: Mapping[str, Any],
    ) -> dict[str, Any]:
        deadline = min(
            self._start + self.deadline_s,
            self.monotonic() + float(self.scenario.data["timeouts"]["up_s"]),
        )
        while self.monotonic() < deadline:
            result = self._invoke(
                self.factory.make(
                    phase="event.client-route-log-ready",
                    host=baseline["host"],
                    instance=instance["name"],
                    transport="ssh",
                    timeout_s=self._command_timeout(),
                    argv=ssh_argv(
                        self.farm,
                        baseline["host"],
                        (
                            "python3",
                            "-c",
                            CLIENT_ROUTE_READINESS_SCRIPT,
                            baseline["path"],
                            str(baseline["offset"]),
                        ),
                    ),
                )
            )
            try:
                witness = json.loads(result.stdout.strip())
            except (TypeError, ValueError, json.JSONDecodeError) as exc:
                raise EventError("client route-owner log readiness returned malformed JSON") from exc
            if not isinstance(witness, dict) or set(witness) != {
                "bytes",
                "lifecycle",
                "line",
                "ready",
                "state",
            }:
                raise EventError("client route-owner log readiness returned an invalid witness")
            if witness.get("ready") is True:
                if (
                    witness.get("state") != 2
                    or witness.get("lifecycle") != 3
                    or type(witness.get("bytes")) is not int
                    or witness["bytes"] < 1
                    or not isinstance(witness.get("line"), str)
                    or re.search(
                        r"cache sidecar adapter state=2 lifecycle=3",
                        witness["line"],
                    )
                    is None
                ):
                    raise EventError("client route-owner log readiness is inconsistent")
                self._failure_evidence["route"]["log_ready"] = {
                    "state": "SUCCEEDED",
                    "receipt": witness,
                }
                return {
                    "host": baseline["host"],
                    "lifecycle": 3,
                    "line": witness["line"],
                    "log_path": baseline["path"],
                    "offset": baseline["offset"],
                    "state": 2,
                }
            self._wake.wait(
                timeout=min(
                    self.poll_interval_s,
                    max(0.001, deadline - self.monotonic()),
                )
            )
            self._wake.clear()
        raise EventTimeout(f"client route owner did not become READY for {instance['name']!r}")

    def _coordinated_client_route_restart(
        self, event: TimelineEvent, instance: Mapping[str, Any], identifier: str
    ) -> dict[str, Any]:
        with self._lock:
            turn = self._active_turn
            epoch = len(self._records) + 1
        if event.trigger.kind != "job" or turn is None:
            raise EventError(
                "a client route-owner restart must be job-triggered during an authenticated workload turn"
            )
        client_names = set(self.scenario.data["workload"]["clients"])
        clients = sorted(
            (
                item
                for item in self.plan["topology"]["instances"]
                if item["role"] == "C" and item["name"] in client_names
            ),
            key=lambda item: item["name"],
        )
        if (
            {item["name"] for item in clients} != client_names
            or event.instance not in client_names
            or not clients
        ):
            raise EventError("client route-owner restart cannot resolve every workload client")
        before_container = self._inspect(event.instance)
        if before_container.get("running") is not True or before_container.get("id") != identifier:
            raise EventError("client route-owner restart has no authenticated running container")

        pauses: dict[str, dict[str, Any]] = {}
        resumes: dict[str, dict[str, Any]] = {}
        primary: BaseException | None = None
        signal_receipt: dict[str, Any] | None = None
        after_processes: dict[str, Any] | None = None
        after_container: dict[str, Any] | None = None
        readiness: dict[str, Any] | None = None
        ready_ms: int | None = None
        drain_timeout = self._command_timeout(
            int(self.scenario.data["timeouts"]["turn_s"])
        )
        try:
            for client in clients:
                pauses[client["name"]] = self._gate_control(
                    client,
                    action="pause",
                    turn=turn,
                    epoch=epoch,
                    timeout_s=drain_timeout,
                )
            baseline = self._readiness_baseline(instance)
            signal_receipt = self._signal_client_route_owner(instance)
            after_processes = self._wait_client_route_owner(instance, signal_receipt)
            readiness = self._wait_client_route_readiness(instance, baseline)
            after_container = self._inspect(event.instance)
            if (
                after_container.get("running") is not True
                or after_container.get("id") != before_container.get("id")
                or after_container.get("started_at") != before_container.get("started_at")
            ):
                raise EventError("client container changed during route-owner restart")
            ready_ms = int(self.wall_ms())
        except BaseException as exc:
            self._mark_failure(self._current_phase, exc)
            primary = exc
        finally:
            release = "abort" if primary is not None else "resume"
            release_errors: list[str] = []
            for client in clients:
                try:
                    resumes[client["name"]] = self._gate_control(
                        client,
                        action=release,
                        turn=turn,
                        epoch=epoch,
                        timeout_s=self._command_timeout(),
                    )
                except BaseException as exc:
                    release_errors.append(f"{client['name']}: {exc}")
            if primary is not None:
                detail = f"; release errors: {'; '.join(release_errors)}" if release_errors else ""
                raise EventError(
                    f"coordinated client route-owner restart failed: {primary}{detail}"
                ) from primary
            if release_errors:
                raise EventError(
                    "coordinated client route-owner restart could not release every client: "
                    + "; ".join(release_errors)
                )

        assert signal_receipt is not None
        assert after_processes is not None
        assert after_container is not None
        assert readiness is not None
        assert ready_ms is not None
        return {
            "action": event.action,
            "after": {
                "container_id": after_container["id"],
                "container_started_at": after_container["started_at"],
                **after_processes,
            },
            "before": {
                "container_id": before_container["id"],
                "container_started_at": before_container["started_at"],
                "daemon": signal_receipt["daemon"],
                "route_owner": signal_receipt["route_owner"],
            },
            "coordination": {
                "clients": pauses,
                "ready_ms": ready_ms,
                "readiness": readiness,
                "resume": resumes,
                "signal": signal_receipt,
            },
            "event_epoch": epoch,
            "instance": event.instance,
            "schema": CLIENT_ROUTE_RESTART_SCHEMA,
            "turn": turn,
        }

    def _authenticated_worker_restart(
        self,
        event: TimelineEvent,
        instance: Mapping[str, Any],
        before: Mapping[str, Any],
    ) -> dict[str, Any]:
        """Restart one F without pausing clients and prove its fresh P50 rejoin."""

        with self._lock:
            turn = self._active_turn
            epoch = len(self._records) + 1
        if turn is None:
            raise EventError(
                "an authenticated worker restart must fire during a workload turn"
            )
        state = self._state[event.instance]
        identifier = before.get("id")
        before_started = before.get("started_at")
        if (
            before.get("running") is not True
            or before.get("id") != identifier
            or not isinstance(before_started, str)
            or not before_started
        ):
            raise EventError("worker restart has no authenticated running pre-state")
        scheduler = next(
            item
            for item in self.plan["topology"]["instances"]
            if item["role"] == "S"
        )
        readiness_baseline = self._readiness_baseline(instance)
        scheduler_baseline = self._readiness_baseline(scheduler)
        self._invoke(
            self.factory.make(
                phase="event.restart",
                host=instance["host"],
                instance=event.instance,
                transport=_docker_transport(self.farm, instance["host"]),
                timeout_s=self._command_timeout(),
                argv=docker_argv(
                    self.farm,
                    instance["host"],
                    ("container", "restart", "--time", "10", identifier),
                ),
            )
        )
        after = self._inspect(
            event.instance,
            expected_runtime=str(
                runtime_root(self.farm, instance | {"image": state["image"]})
            ),
            expected_env=state["env"],
        )
        after_started = after.get("started_at")
        if (
            after.get("running") is not True
            or after.get("id") != identifier
            or not isinstance(after_started, str)
            or not after_started
            or after_started == before_started
        ):
            raise EventError("worker restart has no authenticated fresh post-state")
        readiness = self._header_readiness_witness(instance, readiness_baseline)
        scheduler_rejoin = self._header_scheduler_rejoin_witness(
            scheduler, instance, scheduler_baseline, include_loss=True
        )
        deadline = min(
            self._start + self.deadline_s,
            self.monotonic() + float(self.scenario.data["timeouts"]["up_s"]),
        )
        worker_snapshot = _wait_workers(
            self.farm,
            self.plan,
            self.recorder,
            self.factory,
            deadline=deadline,
            monotonic=self.monotonic,
            sleeper=time.sleep,
        )
        ready_ms = int(self.wall_ms())
        return {
            "action": "restart",
            "after": {
                "container_id": identifier,
                "started_at": after_started,
            },
            "before": {
                "container_id": identifier,
                "started_at": before_started,
            },
            "coordination": {
                "ready_ms": ready_ms,
                "readiness": readiness,
                "scheduler_rejoin": scheduler_rejoin,
                "worker_snapshot": worker_snapshot,
                "workers": sorted(
                    item["name"]
                    for item in self.plan["topology"]["instances"]
                    if item["role"] == "F"
                ),
            },
            "event_epoch": epoch,
            "instance": event.instance,
            "schema": WORKER_RESTART_SCHEMA,
            "turn": turn,
        }

    def _coordinated_header_edit(
        self, event: TimelineEvent, instance: Mapping[str, Any], identifier: str
    ) -> dict[str, Any]:
        with self._lock:
            turn = self._active_turn
            epoch = len(self._records) + 1
        if event.trigger.kind != "job" or turn is None:
            raise EventError(
                "a header_edit must be job-triggered during an authenticated workload turn"
            )
        client_names = set(self.scenario.data["workload"]["clients"])
        clients = sorted(
            (
                item
                for item in self.plan["topology"]["instances"]
                if item["role"] == "C" and item["name"] in client_names
            ),
            key=lambda item: item["name"],
        )
        if {item["name"] for item in clients} != client_names or not clients:
            raise EventError("header_edit cannot resolve every workload client")
        scheduler = next(
            (
                item
                for item in self.plan["topology"]["instances"]
                if item["role"] == "S"
            ),
            None,
        )
        if not isinstance(scheduler, Mapping):
            raise EventError("header_edit cannot resolve the planned scheduler")

        before = self._inspect(event.instance)
        if before.get("running") is not True:
            raise EventError("header_edit found the target F without a running container")
        before_started = before.get("started_at")
        if not isinstance(before_started, str) or not before_started:
            raise EventError("header_edit found no authenticated F start identity")
        pauses: dict[str, dict[str, Any]] = {}
        resumes: dict[str, dict[str, Any]] = {}
        primary: BaseException | None = None
        mutation: dict[str, Any] | None = None
        after: dict[str, Any] | None = None
        readiness: dict[str, Any] | None = None
        scheduler_rejoin: dict[str, Any] | None = None
        ready_ms: int | None = None
        drain_timeout = self._command_timeout(
            int(self.scenario.data["timeouts"]["turn_s"])
        )
        try:
            for client in clients:
                pauses[client["name"]] = self._gate_control(
                    client,
                    action="pause",
                    turn=turn,
                    epoch=epoch,
                    timeout_s=drain_timeout,
                )
            baseline = self._readiness_baseline(instance)
            result = self._invoke(
                self.factory.make(
                    phase="event.header-edit",
                    host=instance["host"],
                    instance=event.instance,
                    transport=_docker_transport(self.farm, instance["host"]),
                    timeout_s=self._command_timeout(),
                    argv=docker_argv(
                        self.farm,
                        instance["host"],
                        (
                            "exec",
                            "--user",
                            "0",
                            identifier,
                            "python3",
                            "-c",
                            HEADER_EDIT_SCRIPT,
                            event.fields["path"],
                            f"event-{epoch}",
                        ),
                    ),
                )
            )
            try:
                mutation = json.loads(result.stdout.strip())
            except (TypeError, ValueError, json.JSONDecodeError) as exc:
                raise EventError("header_edit returned malformed JSON") from exc
            if (
                not isinstance(mutation, dict)
                or set(mutation)
                != {
                    "after_cache",
                    "after_sha256",
                    "before_cache",
                    "before_sha256",
                    "cache_directory",
                    "header_path",
                    "removed_cache",
                }
                or mutation["header_path"] != event.fields["path"]
                or mutation["cache_directory"] != "/var/cache/icecream/p50-runtime"
                or mutation["removed_cache"] != mutation["before_cache"]
                or mutation["after_cache"] != []
                or not isinstance(mutation["before_cache"], list)
                or not isinstance(mutation["removed_cache"], list)
                or mutation["before_cache"]
                != [
                    "p29-system-source-fingerprint-v1.cache",
                    "p29-system-source-fingerprint-v1.lock",
                ]
                or mutation["removed_cache"]
                != [
                    "p29-system-source-fingerprint-v1.cache",
                    "p29-system-source-fingerprint-v1.lock",
                ]
                or any(
                    item
                    not in {
                        "p29-system-source-fingerprint-v1.cache",
                        "p29-system-source-fingerprint-v1.lock",
                    }
                    for item in mutation["before_cache"]
                )
                or len(set(mutation["before_cache"])) != len(mutation["before_cache"])
                or not all(
                    isinstance(mutation[key], str)
                    and SHA256_RE.fullmatch(mutation[key]) is not None
                    for key in ("before_sha256", "after_sha256")
                )
                or mutation["before_sha256"] == mutation["after_sha256"]
            ):
                raise EventError("header_edit returned an invalid mutation receipt")
            scheduler_baseline = self._readiness_baseline(scheduler)
            self._invoke(
                self.factory.make(
                    phase="event.header-edit.restart",
                    host=instance["host"],
                    instance=event.instance,
                    transport=_docker_transport(self.farm, instance["host"]),
                    timeout_s=self._command_timeout(),
                    argv=docker_argv(
                        self.farm,
                        instance["host"],
                        ("container", "restart", "--time", "10", identifier),
                    ),
                )
            )
            after = self._inspect(event.instance)
            after_started = after.get("started_at")
            if (
                after.get("running") is not True
                or after.get("id") != identifier
                or not isinstance(after_started, str)
                or not after_started
                or after_started == before_started
            ):
                raise EventError("header_edit has no fresh running F post-state")
            readiness = self._header_readiness_witness(instance, baseline)
            scheduler_rejoin = self._header_scheduler_rejoin_witness(
                scheduler, instance, scheduler_baseline
            )
            ready_ms = int(self.wall_ms())
        except BaseException as exc:
            self._mark_failure(self._current_phase, exc)
            primary = exc
        finally:
            release = "abort" if primary is not None else "resume"
            release_errors: list[str] = []
            for client in clients:
                try:
                    resumes[client["name"]] = self._gate_control(
                        client,
                        action=release,
                        turn=turn,
                        epoch=epoch,
                        timeout_s=self._command_timeout(),
                    )
                except BaseException as exc:
                    release_errors.append(f"{client['name']}: {exc}")
            if primary is not None:
                detail = f"; resume errors: {'; '.join(release_errors)}" if release_errors else ""
                raise EventError(f"coordinated header_edit failed: {primary}{detail}") from primary
            if release_errors:
                raise EventError(
                    "coordinated header_edit could not release every client: "
                    + "; ".join(release_errors)
                )

        assert mutation is not None
        assert after is not None
        assert readiness is not None
        assert scheduler_rejoin is not None
        assert ready_ms is not None
        return {
            "action": event.action,
            "after": {
                "container_id": after["id"],
                "running": after["running"],
                "started_at": after["started_at"],
                "header_path": mutation["header_path"],
                "header_sha256": mutation["after_sha256"],
                "p29_cache_files": mutation["after_cache"],
            },
            "before": {
                "container_id": before["id"],
                "running": before["running"],
                "started_at": before_started,
                "header_path": mutation["header_path"],
                "header_sha256": mutation["before_sha256"],
                "p29_cache_files": mutation["before_cache"],
            },
            "cache_invalidation": {
                "directory": mutation["cache_directory"],
                "files": [
                    "p29-system-source-fingerprint-v1.cache",
                    "p29-system-source-fingerprint-v1.lock",
                ],
                "removed": mutation["removed_cache"],
            },
            "coordination": {
                "clients": pauses,
                "ready_ms": ready_ms,
                "readiness": readiness,
                "resume": resumes,
                "scheduler_rejoin": scheduler_rejoin,
            },
            "event_epoch": epoch,
            "instance": event.instance,
            "schema": HEADER_EDIT_SCHEMA,
            "turn": turn,
        }

    def _authenticated_disk_fill(
        self,
        event: TimelineEvent,
        instance: Mapping[str, Any],
        before: Mapping[str, Any],
    ) -> dict[str, Any]:
        expected_mount = {
            "destination": CACHE_DISK_FAULT_PATH,
            "size_bytes": CACHE_DISK_FAULT_BYTES,
            "type": "tmpfs",
        }
        if (
            before.get("running") is not True
            or not isinstance(before.get("started_at"), str)
            or not before["started_at"]
            or before.get("cache_fault_mount") != expected_mount
        ):
            raise EventError("disk_fill target lacks the authenticated bounded cache tmpfs")
        result = self._invoke(
            self.factory.make(
                phase="event.disk-fill",
                host=instance["host"],
                instance=event.instance,
                transport=_docker_transport(self.farm, instance["host"]),
                timeout_s=self._command_timeout(),
                argv=docker_argv(
                    self.farm,
                    instance["host"],
                    (
                        "container",
                        "exec",
                        before["id"],
                        "python3",
                        "-c",
                        DISK_FILL_SCRIPT,
                        CACHE_DISK_FAULT_PATH,
                        CACHE_DISK_FAULT_FILE,
                        str(CACHE_DISK_FAULT_BYTES),
                        str(DISK_FILL_WATCHDOG_S),
                        str(CACHE_DISK_FAULT_MIN_HEADROOM_BYTES),
                    ),
                ),
            )
        )
        try:
            operation = json.loads(result.stdout.strip())
        except (TypeError, ValueError, json.JSONDecodeError) as exc:
            raise EventError("disk_fill operation returned malformed JSON") from exc
        operation_fields = {
            "available_after",
            "available_before",
            "directory_gid",
            "directory_mode",
            "directory_uid",
            "elapsed_ms",
            "errno",
            "filler_bytes",
            "filler_path",
            "limit_bytes",
            "minimum_headroom_bytes",
            "schema",
            "watchdog_s",
        }
        if (
            not isinstance(operation, dict)
            or set(operation) != operation_fields
            or operation.get("schema") != "icefarm-disk-fill-operation-v1"
            or operation.get("errno") != 28
            or operation.get("filler_path") != CACHE_DISK_FAULT_FILE
            or operation.get("limit_bytes") != CACHE_DISK_FAULT_BYTES
            or type(operation.get("filler_bytes")) is not int
            or not 0 < operation["filler_bytes"] <= CACHE_DISK_FAULT_BYTES
            or type(operation.get("available_before")) is not int
            or operation["available_before"] < CACHE_DISK_FAULT_MIN_HEADROOM_BYTES
            or operation["available_before"] > CACHE_DISK_FAULT_BYTES
            or operation["filler_bytes"] > operation["available_before"]
            or type(operation.get("available_after")) is not int
            or not 0 <= operation["available_after"] < 1024 * 1024
            or operation["available_after"] >= operation["available_before"]
            or operation.get("directory_uid") != 65534
            or operation.get("directory_gid") != 65534
            or operation.get("directory_mode") != 0o700
            or type(operation.get("elapsed_ms")) is not int
            or not 0 <= operation["elapsed_ms"] <= DISK_FILL_WATCHDOG_S * 1000
            or operation.get("minimum_headroom_bytes")
            != CACHE_DISK_FAULT_MIN_HEADROOM_BYTES
            or operation.get("watchdog_s") != DISK_FILL_WATCHDOG_S
        ):
            raise EventError("disk_fill operation did not prove the fixed bounded ENOSPC contract")
        after = self._inspect(event.instance)
        if (
            after.get("id") != before.get("id")
            or after.get("started_at") != before.get("started_at")
            or after.get("running") is not True
            or after.get("cache_fault_mount") != expected_mount
        ):
            raise EventError("disk_fill target identity or bounded mount changed")

        def snapshot(value: Mapping[str, Any]) -> dict[str, Any]:
            return {
                "container_id": value["id"],
                "container_name": value["name"],
                "host": instance["host"],
                "image_closure_sha256": instance["image"]["closure_sha256"],
                "image_id": value["image_id"],
                "labels": value["labels"],
                "mount": value["cache_fault_mount"],
                "running": value["running"],
                "runtime_path": value["runtime_path"],
                "started_at": value["started_at"],
            }

        return {
            "action": "disk_fill",
            "after": snapshot(after),
            "before": snapshot(before),
            "event_epoch": len(self._records) + 1,
            "fill": operation,
            "instance": event.instance,
            "schema": DISK_FILL_SCHEMA,
        }

    def _dispatch(self, event: TimelineEvent) -> dict[str, Any] | None:
        container, instance = self._container(event.instance)
        if (
            event.action == "restart"
            and instance["role"] == "F"
            and self.scenario.data.get("expect", {}).get("engagement")
            == "s70-b4-worker-bounces"
        ):
            state = self._state[event.instance]
            before = self._inspect(
                event.instance,
                expected_runtime=str(
                    runtime_root(self.farm, instance | {"image": state["image"]})
                ),
                expected_env=state["env"],
            )
            return self._authenticated_worker_restart(event, instance, before)
        before = self._inspect(event.instance)
        identifier = before["id"]
        if event.action == "kill -9":
            operation = ("container", "kill", "--signal", "KILL", identifier)
        elif event.action == "disk_fill":
            if instance["role"] != "F":
                raise UnsupportedEvent("disk_fill is only safe for an F instance")
            return self._authenticated_disk_fill(event, instance, before)
        elif event.action == "header_edit":
            if instance["role"] != "F":
                raise UnsupportedEvent("header_edit is only safe for an F instance")
            return self._coordinated_header_edit(event, instance, identifier)
        elif event.action == "restart":
            if instance["role"] == "S":
                return self._coordinated_scheduler_restart(event, instance, identifier)
            if instance["role"] == "C":
                return self._coordinated_client_route_restart(event, instance, identifier)
            operation = ("container", "restart", "--time", "10", identifier)
        else:  # guarded by _validate_events
            raise UnsupportedEvent(f"timeline action {event.action!r} is unsupported")
        self._invoke(
            self.factory.make(
                phase=f"event.{event.action.replace(' ', '-')}",
                host=instance["host"],
                instance=event.instance,
                transport=_docker_transport(self.farm, instance["host"]),
                timeout_s=self._command_timeout(),
                argv=docker_argv(self.farm, instance["host"], operation),
            )
        )
        return None

    def _start_argv(self, event: TimelineEvent, target: dict[str, Any]) -> tuple[str, ...]:
        if event.instance not in self._start_argvs:
            raise EventError(f"run plan has no start command for {event.instance!r}")
        argv = list(self._start_argvs[event.instance])
        current = self._state[event.instance]
        current_closure = current["image"].get("closure_sha256")
        target_closure = target["image"].get("closure_sha256")
        if not isinstance(current_closure, str) or not isinstance(target_closure, str):
            raise EventError("transition image closure is unavailable")
        current_runtime = str(
            runtime_root(
                self.farm,
                {"host": target["host"], "image": {"closure_sha256": current_closure}},
            )
        )
        target_runtime = str(runtime_root(self.farm, target))
        mount_index = next(
            (
                index
                for index, value in enumerate(argv)
                if value.startswith("type=bind,")
                and value.endswith(",dst=/opt/icecream,readonly")
            ),
            None,
        )
        if mount_index is None or f"src={current_runtime}," not in argv[mount_index]:
            raise EventError("run plan runtime bind does not match the authenticated current image")
        argv[mount_index] = argv[mount_index].replace(
            f"src={current_runtime},", f"src={target_runtime},", 1
        )

        managed = {"ICECC_P50_PROFILE", "ICECC_P50_MODE", P29_FAULT_ENV}
        rewritten: list[str] = []
        index = 0
        while index < len(argv):
            if argv[index] == "--env" and index + 1 < len(argv):
                key = argv[index + 1].partition("=")[0]
                if key in managed:
                    index += 2
                    continue
                rewritten.extend((argv[index], argv[index + 1]))
                index += 2
                continue
            rewritten.append(argv[index])
            index += 1
        insertion = next(
            (index for index, value in enumerate(rewritten) if value == "--entrypoint"),
            len(rewritten),
        )
        managed_args: list[str] = []
        for key in sorted(managed.intersection(target["env"])):
            managed_args.extend(("--env", f"{key}={target['env'][key]}"))
        rewritten[insertion:insertion] = managed_args
        return tuple(rewritten)

    @staticmethod
    def _snapshot(*, state: Mapping[str, Any], container_id: str) -> dict[str, Any]:
        image = state["image"]
        return {
            "container_id": container_id,
            "closure_sha256": image["closure_sha256"],
            "env": dict(sorted(state["env"].items())),
            "image": image["label"],
            "role_sha256": state["sha256"],
        }

    def _transition(self, event: TimelineEvent) -> dict[str, Any]:
        _container, instance = self._container(event.instance)
        if instance["role"] in {"S", "F"} and event.trigger.kind == "job":
            return self._coordinated_transition(event, instance)
        if (
            instance["role"] == "C"
            and event.trigger.kind == "job"
            and self.quiesce_workload is not None
            and self.relaunch_workload is not None
        ):
            return self._coordinated_client_transition(event, instance, None)
        if event.action == "env_set" and instance["role"] == "C":
            with self._lock:
                active_turn = self._active_turn
            if active_turn is not None:
                raise UnsupportedEvent(
                    "active C environment transitions require a job-triggered "
                    "checkpointed workload driver"
                )
        before_state = {
            "image": dict(self._state[event.instance]["image"]),
            "env": dict(self._state[event.instance]["env"]),
            "sha256": self._state[event.instance]["sha256"],
        }
        before_identity = self._inspect(
            event.instance,
            expected_runtime=str(runtime_root(self.farm, instance | before_state)),
            expected_env=before_state["env"],
        )
        if before_identity["running"] is not True:
            raise EventError(
                f"transition {event.action} found {event.instance!r} without a running container"
            )
        target = self._target_instance(event)
        before_id = before_identity["id"]
        readiness_baseline = self._readiness_baseline(instance)
        start_argv = self._start_argv(event, target)
        self._invoke(
            self.factory.make(
                phase=f"event.{event.action}.stop",
                host=instance["host"],
                instance=event.instance,
                transport=_docker_transport(self.farm, instance["host"]),
                timeout_s=self._command_timeout(),
                argv=docker_argv(
                    self.farm,
                    instance["host"],
                    ("container", "stop", "--time", "10", before_id),
                ),
            )
        )
        self._invoke(
            self.factory.make(
                phase=f"event.{event.action}.remove",
                host=instance["host"],
                instance=event.instance,
                transport=_docker_transport(self.farm, instance["host"]),
                timeout_s=self._command_timeout(),
                argv=docker_argv(
                    self.farm,
                    instance["host"],
                    ("container", "rm", "--force", before_id),
                ),
            )
        )
        self._invoke(
            self.factory.make(
                phase=f"event.{event.action}.start",
                host=instance["host"],
                instance=event.instance,
                transport=_docker_transport(self.farm, instance["host"]),
                timeout_s=self._command_timeout(),
                argv=start_argv,
            )
        )
        after_identity = self._inspect(
            event.instance,
            expected_runtime=str(runtime_root(self.farm, target)),
            expected_env=target["env"],
            previous_id=before_id,
        )
        if after_identity["running"] is not True:
            raise EventError(
                f"transition {event.action} restarted {event.instance!r} without a running container"
            )
        readiness = self._readiness_witness(instance, readiness_baseline)
        self._state[event.instance] = {
            "image": dict(target["image"]),
            "env": dict(target["env"]),
            "sha256": target["sha256"],
        }
        self._start_argvs[event.instance] = start_argv
        preflight = self._target_preflight[
            (target["host"], target["image"]["label"], target["role"])
        ]
        before_state["env"] = dict(before_identity["env"])
        after_state = dict(self._state[event.instance])
        after_state["env"] = dict(after_identity["env"])
        return {
            "action": event.action,
            "instance": event.instance,
            "preflight": {
                "image_closure_sha256": preflight["image"]["closure_sha256"],
                "role_sha256": preflight["role_hashes"][target["role"]],
                "runtime_path": preflight["runtime"]["path"],
            },
            "readiness": readiness,
            "before": self._snapshot(state=before_state, container_id=before_id),
            "after": self._snapshot(state=after_state, container_id=after_identity["id"]),
        }

    def _persist(self) -> None:
        payload = canonical_bytes({"events": [record.as_dict() for record in self._records]})
        if self.event_path.exists():
            try:
                previous = json.loads(self.event_path.read_text(encoding="utf-8"))
                previous_events = previous["events"]
            except (OSError, UnicodeError, TypeError, ValueError, KeyError) as exc:
                raise EventError("existing events.json is not valid immutable evidence") from exc
            current_events = [record.as_dict() for record in self._records]
            if not isinstance(previous_events, list) or previous_events != current_events[: len(previous_events)]:
                raise EventError("events.json is immutable and does not match this timeline prefix")
            if previous_events == current_events:
                return
        _atomic_write(self.event_path, payload)

    def _remote_job_reader(self) -> str:
        scheduler = next(
            item for item in self.plan["topology"]["instances"] if item["role"] == "S"
        )
        path = (
            PurePosixPath(self.farm.hosts[scheduler["host"]]["scratch_root"])
            / "icefarm"
            / self.plan["run_id"]
            / scheduler["name"]
            / "log"
            / "scheduler.log"
        )
        result = self._invoke(
            self.factory.make(
                phase="event.poll",
                host=scheduler["host"],
                instance=scheduler["name"],
                transport="ssh",
                timeout_s=self._command_timeout(),
                argv=ssh_argv(
                    self.farm,
                    scheduler["host"],
                    ("cat", str(path)),
                ),
            )
        )
        return result.stdout + "\n" + result.stderr

    def _eligible(self, event: TimelineEvent, now: float) -> bool:
        if event.trigger.kind == "time":
            return now - self._start >= float(event.trigger.value)
        if event.trigger.kind == "job":
            reader = self.job_reader or self._remote_job_reader
            observed = parse_scheduler_dispatches(reader())
            baseline_size = len(self._baseline_dispatches)
            if observed[:baseline_size] != self._baseline_dispatches:
                raise EventError("scheduler dispatch log changed beneath the timeline watcher")
            workload_dispatches = observed[baseline_size:]
            self._dispatch_count = len(workload_dispatches)
            if workload_dispatches:
                self._last_job = workload_dispatches[-1]
            required = max(
                int(event.trigger.value),
                self._job_trigger_floor + 1,
            )
            return self._dispatch_count >= required
        return str(event.trigger.value) in self._turns

    def _run(self) -> None:
        try:
            deadline = self._start + self.deadline_s
            while self._pending and not self._stop.is_set():
                now = self.monotonic()
                if now >= deadline:
                    raise EventTimeout("timeline deadline expired before all events fired")
                fired = False
                for event in tuple(self._pending):
                    self._current_event = event
                    # Polling/eligibility is part of this event attempt too.  Do
                    # not let a reader or parser failure inherit evidence from
                    # an earlier completed event.
                    self._reset_failure_evidence()
                    if self._eligible(event, now):
                        receipt = (
                            self._transition(event)
                            if event.action in TRANSITION_ACTIONS
                            else self._dispatch(event)
                        )
                        fired_ms = (
                            int(receipt["coordination"]["ready_ms"])
                            if isinstance(receipt, dict)
                            and receipt.get("schema")
                            in {
                                SCHEDULER_RESTART_SCHEMA,
                                CLIENT_ROUTE_RESTART_SCHEMA,
                                WORKER_RESTART_SCHEMA,
                                HEADER_EDIT_SCHEMA,
                                TRANSITION_SCHEMA,
                                CLIENT_TRANSITION_SCHEMA,
                            }
                            else int(self.wall_ms())
                        )
                        if self._records:
                            fired_ms = max(fired_ms, self._records[-1].fired_ms)
                        record = EventRecord(
                            event_epoch=len(self._records) + 1,
                            event_index=event.index,
                            action=event.action,
                            instance=event.instance,
                            trigger=event.trigger_text,
                            fired_ms=fired_ms,
                            last_dispatched_job=self._last_job,
                            workload_dispatch_count=self._dispatch_count,
                            receipt=receipt,
                        )
                        with self._lock:
                            self._records.append(record)
                            self._pending.remove(event)
                            if event.trigger.kind == "job":
                                self._job_trigger_floor = self._dispatch_count
                        self._persist()
                        fired = True
                        now = self.monotonic()
                if not fired:
                    self._wake.wait(timeout=min(self.poll_interval_s, max(0.001, deadline - now)))
                    self._wake.clear()
            if self._pending and not self._stop.is_set():
                raise EventTimeout("timeline worker stopped with pending events")
        except BaseException as exc:  # stored and re-raised by the owner thread
            try:
                self._persist_failure(exc)
            except BaseException as persist_exc:
                exc = EventError(f"{exc}; failure descriptor: {persist_exc}")
            if not self._stop.is_set() or isinstance(exc, EventError):
                self._exception = exc
            self._stop.set()

    def start(self) -> None:
        if self._thread is not None:
            raise EventError("timeline worker already started")
        self._start = self.monotonic()
        if not self._pending:
            return
        if any(event.trigger.kind == "job" for event in self._pending):
            reader = self.job_reader or self._remote_job_reader
            self._baseline_dispatches = parse_scheduler_dispatches(reader())
        self._thread = threading.Thread(target=self._run, name="icefarm-events")
        self._thread.start()

    def signal_turn_complete(self, turn: str) -> None:
        with self._lock:
            self._turns.add(turn)
            if self._active_turn == turn:
                self._active_turn = None
        self._wake.set()

    def signal_turn_start(self, turn: str) -> None:
        if turn not in self.scenario.data["workload"]["turns"]:
            raise EventError(f"cannot start undeclared workload turn {turn!r}")
        with self._lock:
            if self._active_turn is not None:
                raise EventError(
                    f"workload turn {self._active_turn!r} is already active"
                )
            self._active_turn = turn
        self._wake.set()

    def raise_if_failed(self) -> None:
        if self._exception is not None:
            if isinstance(self._exception, EventError):
                raise self._exception
            raise EventError(str(self._exception)) from self._exception

    def stop(self) -> None:
        self._stop.set()
        self._wake.set()
        if self._thread is not None:
            remaining = max(0.1, self.deadline_s - (self.monotonic() - self._start) + 1.0)
            self._thread.join(timeout=remaining)
            if self._thread.is_alive():
                raise EventTimeout("timeline worker did not stop before its deadline")
        self.raise_if_failed()

    def wait(self) -> None:
        """Wait for all pending triggers, subject to the configured deadline."""

        if self._thread is None:
            return
        remaining = max(0.1, self.deadline_s - (self.monotonic() - self._start) + 1.0)
        self._thread.join(timeout=remaining)
        if self._thread.is_alive():
            raise EventTimeout("timeline worker did not finish before its deadline")
        self.raise_if_failed()


def _docker_transport(farm: FarmSpec, host_name: str) -> str:
    return "docker-context" if farm.hosts[host_name].get("docker_context") else "ssh-docker"


def run_events(
    farm: FarmSpec,
    scenario: ScenarioSpec,
    plan: dict[str, Any],
    *,
    recorder: RecordingTransport,
    job_reader: JobReader | None = None,
    event_path: Path | None = None,
    deadline_s: float | None = None,
) -> tuple[EventRecord, ...]:
    """Synchronous convenience wrapper used by deterministic callers/tests."""

    producer = EventProducer(
        farm,
        scenario,
        plan,
        recorder=recorder,
        job_reader=job_reader,
        event_path=event_path,
        deadline_s=deadline_s,
    )
    producer.start()
    try:
        producer.wait()
    finally:
        producer.stop()
    return producer.records
