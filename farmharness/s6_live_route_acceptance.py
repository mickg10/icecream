#!/usr/bin/env python3
"""Live S6 cross-wrapper route acceptance harness.

This runner is intentionally an acceptance harness, not a route simulator.  It
starts the built scheduler, two real daemons, and the cache service supervised
by F.  Every compile is a new, short-lived ``icecc`` compiler-wrapper process.
The only identities accepted into the evidence document are emitted by the
product's READY/action/compile-identity trace sinks; missing or malformed
identity evidence is a HOLD.

The current tree may not yet have the complete cross-wrapper wiring.  In that
case the command exits 77 and leaves a useful, schema-valid HOLD record under
``experiments/icecream/<experiment>/<UTC-second>/``.  ``--self-test`` performs
the cheap manifest/evidence checks without starting a service.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


SCHEMA = "icecream-s6-live-cross-wrapper-route-v1"
EVENT_SCHEMA = SCHEMA + "-jsonl"
PROFILES = ("ZSTD_ROUTE", "P29", "GRZ")
DEFAULT_PROFILE = "ZSTD_ROUTE"
SCENARIOS = (
    "same_relationship_tu0_tu1",
    "different_relationship_isolation",
    "abort_retry_no_route_advance",
    "explicit_f_reset_clean_recovery",
)
HEX128 = re.compile(r"^[0-9a-fA-F]{32}$")
READY_RE = re.compile(
    r"^READY v2 generation=(\d+) attempt=(\d+) F_STORE_GENERATION=(\d+) "
    r"DERIVATION_VERSION=(\d+) pid=(\d+) C_STORE_GUID=([0-9a-fA-F]{32}) "
    r"F_STORE_GUID=([0-9a-fA-F]{32}) PATH=(\S+) DIGEST=([0-9a-fA-F]{32}) "
    r"DEV=(\d+) INO=(\d+)$"
)
ACTION_FIELDS = {
    "action", "actor", "c_store_guid", "f_store_guid", "previous_f_store_guid",
    "session_serial", "history_nonce", "rel_seq", "tu_seq", "transaction_digest",
    "raw_digest", "state_digest", "content_digest", "key64", "need_keys",
    "remaining_need", "duplicate", "stage_bytes",
}
IDENTITY_FIELDS = {
    "record", "job_id", "assignment_epoch", "assignment_nonce", "c_guid", "tu_seq",
}


def canonical_json(value: Mapping[str, Any]) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_bytes(index: int) -> bytes:
    """Return deterministic small C++ translation-unit bytes."""
    return (
        "#include <cstdint>\n"
        "extern \"C\" std::uint32_t s6_route_tu_%d() { return UINT32_C(%d); }\n"
        % (index, 6100 + index)
    ).encode("ascii")


def _run_id(experiment: str, output_root: Path) -> tuple[str, Path]:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    base = output_root / "icecream" / experiment
    base.mkdir(parents=True, exist_ok=True)
    candidate = base / stamp
    # A second-level path is the contract.  A suffix only handles concurrent
    # invocations without overwriting one run's evidence.
    if candidate.exists():
        candidate = base / (stamp + "-" + str(os.getpid()))
    candidate.mkdir()
    return candidate.name, candidate


class JsonlWriter:
    def __init__(self, path: Path, run_id: str) -> None:
        self.path = path
        self.run_id = run_id
        self.stream = path.open("x", encoding="utf-8")

    def emit(self, event: str, status: str, **fields: Any) -> None:
        row: dict[str, Any] = {
            "schema": EVENT_SCHEMA,
            "run_id": self.run_id,
            "event": event,
            "status": status,
            "utc": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
        }
        row.update(fields)
        self.stream.write(canonical_json(row) + "\n")
        self.stream.flush()
        os.fsync(self.stream.fileno())

    def close(self) -> None:
        self.stream.close()


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def parse_ready(text: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for line in text.splitlines():
        match = READY_RE.fullmatch(line.strip())
        if not match:
            continue
        generation, attempt, f_generation, derivation, pid, c_guid, f_guid, path, digest, dev, ino = match.groups()
        numbers = [int(value) for value in (generation, attempt, f_generation, derivation, pid, dev, ino)]
        if (any(value <= 0 for value in numbers) or int(derivation) != 1 or
                c_guid == "0" * 32 or f_guid == "0" * 32):
            continue
        rows.append({
            "record": "f-ready",
            "generation": int(generation),
            "attempt": int(attempt),
            "f_generation": int(f_generation),
            "derivation_version": int(derivation),
            "pid": int(pid),
            "C_GUID": c_guid.lower(),
            "F_GUID": f_guid.lower(),
            "socket": path,
            "socket_digest": digest.lower(),
            "device": int(dev),
            "inode": int(ino),
        })
    return rows


def parse_action(text: str) -> list[dict[str, Any]]:
    """Parse only complete product action records; reject malformed rows."""
    rows: list[dict[str, Any]] = []
    for line in text.splitlines():
        try:
            row = json.loads(line)
        except (json.JSONDecodeError, RecursionError):
            continue
        if not isinstance(row, dict) or set(row) != ACTION_FIELDS:
            continue
        if row.get("actor") not in ("C", "F") or not isinstance(row.get("action"), str):
            continue
        if not all(isinstance(row.get(key), int) and row[key] >= 0
                   for key in ("session_serial", "history_nonce", "rel_seq", "tu_seq",
                               "remaining_need", "stage_bytes")):
            continue
        if not isinstance(row.get("duplicate"), bool) or not isinstance(row.get("need_keys"), list):
            continue
        if row.get("key64") is not None and (not isinstance(row["key64"], int) or row["key64"] < 0):
            continue
        if not all(isinstance(value, int) and value >= 0 for value in row["need_keys"]):
            continue
        if not all(isinstance(row.get(key), str) and HEX128.fullmatch(row[key])
                   for key in ("c_store_guid", "f_store_guid", "previous_f_store_guid",
                               "transaction_digest", "raw_digest", "state_digest",
                               "content_digest")):
            continue
        if row["c_store_guid"] == "0" * 32 or row["f_store_guid"] == "0" * 32:
            continue
        rows.append(row)
    return rows


def parse_identity(text: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for line in text.splitlines():
        try:
            row = json.loads(line)
        except (json.JSONDecodeError, RecursionError):
            continue
        if not isinstance(row, dict) or set(row) != IDENTITY_FIELDS:
            continue
        if row.get("record") != "compile-result-identity":
            continue
        if not all(type(row.get(key)) is int and row[key] > 0
                   for key in ("job_id", "assignment_epoch", "assignment_nonce", "c_guid")):
            continue
        if type(row.get("tu_seq")) is not int or row["tu_seq"] < 0:
            continue
        rows.append(row)
    return rows


def _profile_observed(profile: str, logs: Iterable[str]) -> bool:
    markers = {
        "ZSTD_ROUTE": ("ZSTD_ROUTE", "CACHE_SESSION"),
        "P29": ("P29", "CACHE_SESSION"),
        "GRZ": ("GRZ", "GRZ_RESIDUAL", "CACHE_SESSION"),
    }[profile]
    combined = "\n".join(logs)
    return all(marker in combined for marker in markers)


def evidence_document(profile: str, runtime: Path, scenario: str) -> dict[str, Any]:
    """Build evidence from files; no identity is generated by this function."""
    ready = parse_ready(_read(runtime / "ready.trace"))
    actions = parse_action(_read(runtime / "action.trace"))
    identities = parse_identity(_read(runtime / "compile-identity.jsonl"))
    logs = [_read(path) for path in runtime.glob("*.log")]
    issues: list[str] = []
    if not ready:
        issues.append("F_GUID_or_generation_missing")
    if not actions:
        issues.append("REL_SEQ_or_action_trace_missing")
    if not identities:
        issues.append("C_GUID_or_TU_SEQ_missing")
    if not _profile_observed(profile, logs):
        issues.append("profile_wire_evidence_missing")
    if len({row["F_GUID"] for row in ready}) != len(ready):
        # Repeated exact READY rows do not prove a reset and are ambiguous.
        issues.append("duplicate_F_GUID_ready_evidence")
    c_guids = sorted({row["c_store_guid"] for row in actions})
    f_guids = sorted({row["f_store_guid"] for row in actions})
    rel_seq = sorted({row["rel_seq"] for row in actions})
    tu_seq = sorted({row["tu_seq"] for row in identities})
    relationships: dict[str, dict[str, Any]] = {}
    for row in actions:
        key = row["c_store_guid"] + "/" + row["f_store_guid"]
        relationship = relationships.setdefault(key, {"C_GUID": row["c_store_guid"],
                                                       "F_GUID": row["f_store_guid"],
                                                       "TU_SEQ": [], "REL_SEQ": []})
        relationship["TU_SEQ"].append(row["tu_seq"])
        relationship["REL_SEQ"].append(row["rel_seq"])
    for relationship in relationships.values():
        relationship["TU_SEQ"] = sorted(set(relationship["TU_SEQ"]))
        relationship["REL_SEQ"] = sorted(set(relationship["REL_SEQ"]))
    observed_profiles = [profile] if not issues or "profile_wire_evidence_missing" not in issues else []
    return {
        "schema": SCHEMA + "-evidence-v1",
        "scenario": scenario,
        "status": "PASS" if not issues else "HOLD",
        "issues": sorted(set(issues)),
        "PROFILE": observed_profiles,
        "C_GUID": c_guids,
        "F_GUID": f_guids,
        "F_GENERATION": sorted({row["f_generation"] for row in ready}),
        "TU_SEQ": tu_seq,
        "REL_SEQ": rel_seq,
        "relationships": sorted(relationships.values(), key=lambda value: (value["C_GUID"], value["F_GUID"])),
        "ready": ready,
        "actions": actions,
        "compile_identity": identities,
    }


def validate_evidence(document: Mapping[str, Any]) -> tuple[bool, list[str]]:
    required = {"schema", "scenario", "status", "issues", "PROFILE", "C_GUID", "F_GUID",
                "F_GENERATION", "TU_SEQ", "REL_SEQ", "relationships", "ready", "actions",
                "compile_identity"}
    issues: list[str] = []
    if set(document) != required or document.get("schema") != SCHEMA + "-evidence-v1":
        issues.append("evidence_schema_invalid")
    if document.get("status") not in ("PASS", "HOLD"):
        issues.append("evidence_status_invalid")
    for key in ("C_GUID", "F_GUID"):
        values = document.get(key)
        if not isinstance(values, list) or any(not isinstance(value, str) or not HEX128.fullmatch(value)
                                               for value in values):
            issues.append(key + "_invalid")
    for key in ("F_GENERATION", "TU_SEQ", "REL_SEQ"):
        values = document.get(key)
        if not isinstance(values, list) or any(type(value) is not int or value < 0 for value in values):
            issues.append(key + "_invalid")
    if not isinstance(document.get("PROFILE"), list) or any(value not in PROFILES for value in document["PROFILE"]):
        issues.append("PROFILE_invalid")
    if not isinstance(document.get("issues"), list) or any(not isinstance(value, str) for value in document["issues"]):
        issues.append("issues_invalid")
    relationships = document.get("relationships")
    if not isinstance(relationships, list):
        issues.append("relationships_invalid")
    if (not isinstance(document.get("ready"), list) or not document.get("ready") or any(
            not isinstance(row, dict) or row.get("record") != "f-ready"
            for row in document.get("ready", []))):
        issues.append("ready_evidence_invalid")
    if (not isinstance(document.get("actions"), list) or not document.get("actions") or any(
            not isinstance(row, dict) or set(row) != ACTION_FIELDS
            for row in document.get("actions", []))):
        issues.append("action_evidence_invalid")
    if (not isinstance(document.get("compile_identity"), list) or not document.get("compile_identity") or any(
            not isinstance(row, dict) or set(row) != IDENTITY_FIELDS
            for row in document.get("compile_identity", []))):
        issues.append("compile_identity_evidence_invalid")
    if document.get("status") == "PASS" and (issues or document.get("issues") or
                                               not document.get("C_GUID") or not document.get("F_GUID") or
                                               not document.get("F_GENERATION") or not document.get("TU_SEQ") or
                                               not document.get("REL_SEQ") or not document.get("PROFILE")):
        issues.append("PASS_without_complete_identity")
    if document.get("status") == "HOLD" and not document.get("issues") and not issues:
        issues.append("HOLD_without_reason")
    return not issues, sorted(set(issues))


def source_check(source: Path) -> tuple[bool, list[str]]:
    requirements = {
        "client/remote.cpp": ("ICECC_P50_COMPILE_IDENTITY_TRACE", "ZSTD_ROUTE", "P50ZstdSourceSender"),
        "daemon/main.cpp": ("P50SourceArmMsg", "p50_source_arm_fields", "cache_adapter"),
        "cache/p50_cache_service.cpp": ("P50CRouteOwner", "READY v2", "transfer_source_on_owner"),
        "cache/p50_actions.cpp": ("rel_seq", "tu_seq", "action_jsonl"),
    }
    missing: list[str] = []
    for relative, needles in requirements.items():
        text = _read(source / relative)
        for needle in needles:
            if needle not in text:
                missing.append(relative + ":" + needle)
    # Generated executables are intentionally not part of this source check;
    # run() checks their exact build-root paths separately.
    for relative in ("unittests/p50compilee2e-source.sh", "unittests/p50compilee2e-run.sh",
                     "farmharness/s4_real_cells.py"):
        if not (source / relative).is_file():
            missing.append(relative + ":missing")
    return not missing, missing


def self_test() -> int:
    assert DEFAULT_PROFILE in PROFILES
    assert set(SCENARIOS) == {
        "same_relationship_tu0_tu1", "different_relationship_isolation",
        "abort_retry_no_route_advance", "explicit_f_reset_clean_recovery",
    }
    assert source_bytes(0) != source_bytes(1)
    with tempfile.TemporaryDirectory(prefix="s6-harness-selftest-") as directory:
        root = Path(directory)
        runtime = root / "runtime"
        runtime.mkdir()
        (runtime / "ready.trace").write_text("", encoding="utf-8")
        (runtime / "action.trace").write_text("not-json\n", encoding="utf-8")
        (runtime / "compile-identity.jsonl").write_text("not-json\n", encoding="utf-8")
        document = evidence_document(DEFAULT_PROFILE, runtime, SCENARIOS[0])
        good, issues = validate_evidence(document)
        assert not good
        assert "ready_evidence_invalid" in issues
        assert document["status"] == "HOLD"
        assert "C_GUID_or_TU_SEQ_missing" in document["issues"]
        # A complete fixture exercises the same strict shape used by live
        # output.  Its identities are test inputs to the parser, never used by
        # the acceptance run as evidence.
        (runtime / "ready.trace").write_text(
            "READY v2 generation=1 attempt=1 F_STORE_GENERATION=1 DERIVATION_VERSION=1 "
            "pid=123 C_STORE_GUID=00000000000000000000000000000001 "
            "F_STORE_GUID=00000000000000000000000000000002 PATH=/tmp/cache.sock "
            "DIGEST=00000000000000000000000000000003 DEV=1 INO=2\n", encoding="utf-8")
        action: dict[str, Any] = {
            key: ("TX_BEGIN" if key == "action" else "C" if key == "actor" else
                  "00000000000000000000000000000001" if key == "c_store_guid" else
                  "00000000000000000000000000000002" if key in ("f_store_guid", "previous_f_store_guid") else
                  "00000000000000000000000000000004" if key in ("transaction_digest", "raw_digest", "state_digest", "content_digest") else
                  [] if key in ("need_keys",) else False if key == "duplicate" else 0)
            for key in ACTION_FIELDS}
        (runtime / "action.trace").write_text(json.dumps(action) + "\n", encoding="utf-8")
        (runtime / "compile-identity.jsonl").write_text(
            '{"record":"compile-result-identity","job_id":1,"assignment_epoch":1,'
            '"assignment_nonce":1,"c_guid":1,"tu_seq":0}\n', encoding="utf-8")
        (runtime / "wrapper.log").write_text("ZSTD_ROUTE CACHE_SESSION\n", encoding="utf-8")
        complete = evidence_document(DEFAULT_PROFILE, runtime, SCENARIOS[0])
        valid, issues = validate_evidence(complete)
        assert valid, issues
        assert complete["status"] == "PASS"
        path = root / "events.jsonl"
        writer = JsonlWriter(path, "self-test")
        writer.emit("manifest", "HOLD", profile=DEFAULT_PROFILE, scenarios=list(SCENARIOS))
        writer.close()
        row = json.loads(path.read_text(encoding="utf-8"))
        assert row["schema"] == EVENT_SCHEMA and row["event"] == "manifest"
    print("ok - S6 live harness manifest/output/fail-closed self-tests")
    return 0


def _find_port_block() -> tuple[int, int, int]:
    for _ in range(100):
        sockets: list[socket.socket] = []
        try:
            first = 40000 + int.from_bytes(os.urandom(2), "big") % 12000
            first -= first % 4
            for port in (first, first + 1, first + 2, first + 3):
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                sock.bind(("127.0.0.1", port))
                sockets.append(sock)
            return first, first + 2, first + 3
        except OSError:
            pass
        finally:
            for sock in sockets:
                sock.close()
    raise RuntimeError("no available local scheduler/daemon ports")


def _alive(process: subprocess.Popen[Any] | None) -> bool:
    return process is not None and process.poll() is None


def _child_with_executable(parent: int, executable: Path) -> bool:
    """Observe a cache child without accepting an unrelated process."""
    try:
        rows = subprocess.check_output(
            ["ps", "-eo", "pid=,ppid=,args="], text=True, stderr=subprocess.DEVNULL,
        ).splitlines()
    except (OSError, subprocess.SubprocessError):
        return False
    needle = str(executable)
    return any(fields[1] == str(parent) and needle in line
               for line in rows if len(fields := line.strip().split(None, 2)) == 3)


def _terminate(process: subprocess.Popen[Any] | None) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        if process.pid > 1:
            os.killpg(process.pid, signal.SIGTERM)
    except (OSError, ProcessLookupError):
        try:
            process.terminate()
        except OSError:
            pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except (OSError, ProcessLookupError):
            pass
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            pass


@dataclass
class Runtime:
    root: Path
    source: Path
    build: Path
    profile: str
    timeout: float
    writer: JsonlWriter
    docker: bool = False
    docker_image: str = "icecream/farm-node:ubuntu22-gcc11-boost174"
    scheduler: subprocess.Popen[Any] | None = None
    worker: subprocess.Popen[Any] | None = None
    client: subprocess.Popen[Any] | None = None
    client2: subprocess.Popen[Any] | None = None
    sched_port: int = 0
    worker_port: int = 0
    client_socket: Path | None = None
    client2_socket: Path | None = None
    envtar: Path | None = None

    def _worker_command(self, runtime_dir: Path, env_dir: Path, log_name: str,
                        socket_name: str) -> tuple[list[str], dict[str, str]]:
        args = ["-p", str(self.worker_port), "-m", "1", "-s", "127.0.0.1:%d" % self.sched_port,
                "-n", "s6-live-%d" % os.getpid(), "-N", "s6-f", "-b", "/work/" + env_dir.name,
                "-l", "/work/" + log_name, "-vvv", "--cache-service", "/role/cache/icecc-cache-service",
                "--cache-runtime-dir", "/work/" + runtime_dir.name]
        values = self.env(ICECC_TEST_SOCKET="/work/" + socket_name,
                          ICECC_P50_TEST_READY_TRACE="/work/ready.trace",
                          ICECC_P50_TEST_LIFECYCLE_TRACE="/work/lifecycle.trace",
                          ICECC_P50_ACTION_TRACE="/work/action.trace", HOME="/work/home")
        if not self.docker:
            args[args.index("-b") + 1] = str(env_dir)
            args[args.index("-l") + 1] = str(self.root / log_name)
            args[args.index("--cache-service") + 1] = str(self.build / "cache/icecc-cache-service")
            args[args.index("--cache-runtime-dir") + 1] = str(runtime_dir)
            values = self.env(ICECC_TEST_SOCKET=str(self.root / socket_name),
                              ICECC_P50_TEST_READY_TRACE=str(self.root / "ready.trace"),
                              ICECC_P50_TEST_LIFECYCLE_TRACE=str(self.root / "lifecycle.trace"),
                              ICECC_P50_ACTION_TRACE=str(self.root / "action.trace"),
                              HOME=str(self.root / "home"))
            return [str(self.build / "daemon/iceccd"), *args], values
        command = ["docker", "run", "--rm", "--name", self._docker_name,
                   "--network", "host", "--user", "0", "--cap-add", "SYS_CHROOT",
                   "-v", str(self.build) + ":/role:ro", "-v", str(self.root) + ":/work"]
        for key in ("ICECC_TEST_SOCKET", "ICECC_P50_TEST_READY_TRACE", "ICECC_P50_TEST_LIFECYCLE_TRACE",
                    "ICECC_P50_ACTION_TRACE", "ICECC_P50_PROFILE", "ICECC_P50_C1F1_REQUIRED", "HOME", "TMPDIR"):
            command.extend(["-e", key + "=" + values[key]])
        command.extend(["--entrypoint", "/bin/sh", self.docker_image, "-c",
                         "exec /role/daemon/iceccd " + shlex.join(args)])
        return command, os.environ.copy()

    @property
    def _docker_name(self) -> str:
        return "s6-live-f-%d" % os.getpid()

    def _cache_ready(self) -> bool:
        if self.worker is None:
            return False
        if not self.docker:
            return _child_with_executable(self.worker.pid, self.build / "cache/icecc-cache-service")
        try:
            probe = subprocess.run(["docker", "exec", self._docker_name, "sh", "-c",
                                    "ps -eo args= | grep '[i]cecc-cache-service'"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   timeout=5, check=False)
            return probe.returncode == 0
        except (OSError, subprocess.SubprocessError):
            return False

    def env(self, **extra: str) -> dict[str, str]:
        result = os.environ.copy()
        result.update({"TMPDIR": "/tmp", "ICECC_P50_PROFILE": self.profile,
                       "ICECC_P50_C1F1_REQUIRED": "1"})
        result.update(extra)
        return result

    def launch(self) -> tuple[bool, str]:
        self.root.mkdir(parents=True, exist_ok=True)
        for name in ("envs-f", "envs-c", "toolchain", "out", "home", "cache-runtime-f"):
            (self.root / name).mkdir()
        for name in ("envs-f", "envs-c"):
            os.chmod(self.root / name, 0o1777)
        os.chmod(self.root / "home", 0o700)
        os.chmod(self.root / "cache-runtime-f", 0o700)
        self.client_socket = self.root / "client.sock"
        self.client2_socket = self.root / "client2.sock"
        try:
            self.sched_port, self.worker_port, _ = _find_port_block()
            create_env = self.build / "client/icecc-create-env"
            # The generated helper is intentionally invoked through bash; its
            # executable bit is not part of the product identity.
            if not create_env.is_file():
                return False, "missing-icecc-create-env"
            command = ["bash", str(create_env), shutil.which("g++") or "g++"]
            with (self.root / "create-env.log").open("w", encoding="utf-8") as stream:
                result = subprocess.run(command, cwd=self.root / "toolchain", env=self.env(HOME=str(self.root / "home")),
                                        stdout=stream, stderr=subprocess.STDOUT, timeout=self.timeout, check=False)
            if result.returncode != 0:
                return False, "create-env-failed"
            candidates = sorted((self.root / "toolchain").glob("*.tar.gz"))
            if not candidates:
                return False, "create-env-produced-no-tarball"
            self.envtar = candidates[0]
            (self.root / "scheduler.log").touch(mode=0o666)
            for name in ("f.log", "c.log"):
                path = self.root / name
                path.touch(mode=0o666)
                os.chmod(path, 0o666)
            self.scheduler = subprocess.Popen(
                [str(self.build / "scheduler/icecc-scheduler"), "-p", str(self.sched_port),
                 "-n", "s6-live-%d" % os.getpid(), "--assignment-fence-mode", "strict-nonce",
                 "-l", str(self.root / "scheduler.log"), "-vvv"],
                env=self.env(HOME=str(self.root / "home")), cwd=self.source,
                stdout=(self.root / "scheduler.stdout").open("w"), stderr=subprocess.STDOUT,
                start_new_session=True)
            time.sleep(1)
            if not _alive(self.scheduler):
                return False, "scheduler-exited"
            base_daemon, worker_env = self._worker_command(
                self.root / "cache-runtime-f", self.root / "envs-f", "f.log", "worker.sock")
            self.worker = subprocess.Popen(
                base_daemon, env=worker_env, cwd=self.source,
                stdout=(self.root / "worker.stdout").open("w"), stderr=subprocess.STDOUT,
                start_new_session=True)
            client_daemon = [str(self.build / "daemon/iceccd"), "--no-remote", "-m", "0",
                             "-s", "127.0.0.1:%d" % self.sched_port, "-n", "s6-live-%d" % os.getpid(),
                             "-N", "s6-c", "-b", str(self.root / "envs-c"), "-l", str(self.root / "c.log"), "-vvv"]
            self.client = subprocess.Popen(
                client_daemon, env=self.env(ICECC_TEST_SOCKET=str(self.root / "client.sock"),
                    ICECC_P50_ACTION_TRACE=str(self.root / "action.trace"), HOME=str(self.root / "home")),
                cwd=self.source, stdout=(self.root / "client.stdout").open("w"), stderr=subprocess.STDOUT,
                start_new_session=True)
            for _ in range(40):
                if not _alive(self.worker) or not _alive(self.client):
                    return False, "daemon-exited"
                if _read(self.root / "scheduler.log").count("login") >= 2:
                    break
                time.sleep(0.25)
            if _read(self.root / "scheduler.log").count("login") < 2:
                return False, "daemons-did-not-register"
            for _ in range(40):
                if self._cache_ready():
                    break
                time.sleep(0.25)
            if not self._cache_ready():
                return False, "cache-service-not-started-by-worker"
            return True, "ready"
        except (OSError, subprocess.SubprocessError, RuntimeError) as error:
            return False, type(error).__name__

    def compile(self, source: Path, label: str, socket_path: Path | None = None,
                disposition: str | None = None) -> dict[str, Any]:
        if self.envtar is None or socket_path is None:
            return {"status": "HOLD", "reason": "runtime-not-ready", "label": label}
        remote = self.root / "out" / (label + "-remote.o")
        local = self.root / "out" / (label + "-local.o")
        wrapper_log = self.root / (label + ".log")
        trace = self.root / "compile-identity.jsonl"
        env = self.env(ICECC_TEST_SOCKET=str(socket_path), ICECC_TEST_REMOTEBUILD="1",
                       ICECC_VERSION=str(self.envtar), ICECC_PREFERRED_HOST="s6-f",
                       ICECC_P50_COMPILE_IDENTITY_TRACE=str(trace), ICECC_DEBUG="debug",
                       ICECC_LOGFILE=str(wrapper_log), ICECC_CARET_WORKAROUND="0")
        if disposition:
            env["ICECC_P50_TEST_DISPOSITION"] = disposition
        source.write_bytes(source_bytes(int(re.sub(r"\D", "", label) or "0")))
        command = [str(self.build / "client/icecc"), "g++", "-std=c++17", "-O2", "-c", str(source), "-o", str(remote)]
        started = time.monotonic()
        output_stream = (self.root / (label + ".stdout")).open("w", encoding="utf-8")
        wrapper: subprocess.Popen[Any] | None = None
        try:
            wrapper = subprocess.Popen(command, env=env, cwd=self.source, stdout=output_stream,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            try:
                wrapper.communicate(timeout=self.timeout)
            except subprocess.TimeoutExpired:
                _terminate(wrapper)
        finally:
            output_stream.close()
        try:
            if wrapper is None:
                return {"status": "HOLD", "label": label, "reason": "compiler-wrapper-not-started"}
            result: dict[str, Any] = {"status": "PASS" if wrapper.returncode == 0 else "HOLD",
                                      "label": label, "compiler_wrapper": "icecc",
                                      "compiler_wrapper_pid": wrapper.pid,
                                      "compiler_wrapper_command": command,
                                      "returncode": wrapper.returncode, "duration_ms": int((time.monotonic() - started) * 1000),
                                      "remote_object": str(remote), "local_object": str(local)}
            local_result = subprocess.run(["g++", "-std=c++17", "-O2", "-c", str(source), "-o", str(local)],
                                          env=self.env(HOME=str(self.root / "home")), cwd=self.source,
                                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=self.timeout, check=False)
            result["local_returncode"] = local_result.returncode
            if remote.is_file() and local.is_file():
                result["remote_sha256"] = sha256(remote)
                result["local_sha256"] = sha256(local)
                result["byte_identical"] = remote.read_bytes() == local.read_bytes()
            else:
                result["byte_identical"] = False
            if not result["byte_identical"]:
                result["status"] = "HOLD"
                result["reason"] = "remote-object-not-byte-identical"
            return result
        except (OSError, subprocess.SubprocessError) as error:
            return {"status": "HOLD", "label": label, "reason": type(error).__name__}

    def stop_worker(self) -> None:
        _terminate(self.worker)
        self.worker = None

    def start_worker_replacement(self) -> bool:
        if self.worker_port == 0:
            return False
        previous_ready = parse_ready(_read(self.root / "ready.trace"))
        previous_guids = {row["F_GUID"] for row in previous_ready}
        runtime2 = self.root / "cache-runtime-f-reset"
        envs2 = self.root / "envs-f-reset"
        runtime2.mkdir(exist_ok=True)
        envs2.mkdir(exist_ok=True)
        command, worker_env = self._worker_command(runtime2, envs2, "f-reset.log", "worker-reset.sock")
        try:
            self.worker = subprocess.Popen(command, env=worker_env,
                cwd=self.source, stdout=(self.root / "worker-reset.stdout").open("w"), stderr=subprocess.STDOUT,
                start_new_session=True)
        except OSError:
            return False
        for _ in range(40):
            if not _alive(self.worker):
                return False
            current_ready = parse_ready(_read(self.root / "ready.trace"))
            if (len(current_ready) > len(previous_ready) and
                    {row["F_GUID"] for row in current_ready} - previous_guids and
                    self._cache_ready()):
                return True
            time.sleep(0.25)
        return False

    def cleanup(self) -> None:
        for process in (self.client2, self.client, self.worker, self.scheduler):
            _terminate(process)
        if self.docker:
            subprocess.run(["docker", "rm", "-f", self._docker_name],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        self.client2 = self.client = self.worker = self.scheduler = None


def required_binaries(build: Path) -> list[str]:
    paths = [build / "scheduler/icecc-scheduler", build / "daemon/iceccd", build / "client/icecc",
             build / "cache/icecc-cache-service"]
    return [str(path) for path in paths if not path.is_file() or not os.access(path, os.X_OK)]


def run_acceptance(args: argparse.Namespace) -> int:
    source = Path(args.source_root).resolve()
    build = Path(args.build_root or args.source_root).resolve()
    run_id, output = _run_id(args.experiment, Path(args.output_root).resolve())
    writer = JsonlWriter(output / "events.jsonl", run_id)
    manifest: dict[str, Any] = {
        "schema": SCHEMA + "-manifest-v1", "run_id": run_id, "experiment": args.experiment,
        "profile": args.profile, "scenarios": list(SCENARIOS), "source_root": str(source),
        "build_root": str(build), "tmpdir": "/tmp", "test_months_locked": True,
        "reused_assets": ["unittests/p50compilee2e-source.sh",
                          "unittests/p50compilee2e-run.sh",
                          "farmharness/s4_real_cells.py"],
        "created_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
    }
    try:
        try:
            manifest["source_commit"] = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
            manifest["source_tree"] = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD^{tree}"], text=True).strip()
        except (OSError, subprocess.CalledProcessError):
            manifest["source_commit"] = "unavailable"
            manifest["source_tree"] = "unavailable"
        writer.emit("manifest", "READY", manifest=manifest)
        source_ok, source_issues = source_check(source)
        missing = required_binaries(build)
        commands_ok = all(shutil.which(command) for command in ("bash", "g++", "python3"))
        if args.docker and not shutil.which("docker"):
            missing.append("docker (requested by --docker)")
        existing_gate = source / "unittests/p50compilee2e-source.sh"
        try:
            gate_result = subprocess.run(["bash", str(existing_gate)], cwd=source,
                                         env={**os.environ, "TMPDIR": "/tmp"},
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         timeout=args.timeout, check=False) if source_ok and commands_ok else None
        except (OSError, subprocess.SubprocessError) as error:
            gate_result = None
            source_issues.append("existing-p50-source-gate:" + type(error).__name__)
            source_ok = False
        if not source_ok or missing or not commands_ok or gate_result is not None and gate_result.returncode != 0:
            reason = ("source-contract" if not source_ok else
                      "missing-product-binary" if missing else
                      "missing-command" if not commands_ok else "existing-p50-source-gate")
            writer.emit("preflight", "HOLD", reason=reason, source_issues=source_issues,
                        missing_binaries=missing,
                        existing_gate_output=(gate_result.stdout.decode("utf-8", "replace")[-2000:]
                                              if gate_result is not None else ""))
            writer.emit("summary", "HOLD", reason=reason, output=str(output))
            return 77
        runtime = Runtime(output / "runtime", source, build, args.profile, args.timeout, writer,
                          docker=args.docker)
        ready, reason = runtime.launch()
        if not ready:
            writer.emit("launch", "HOLD", reason=reason)
            writer.emit("summary", "HOLD", reason=reason, output=str(output))
            runtime.cleanup()
            return 77
        writer.emit("launch", "READY", scheduler_port=runtime.sched_port, worker_port=runtime.worker_port)
        results: dict[str, dict[str, Any]] = {}
        # Two files are enough to exercise the two source transfers while each
        # invocation has a distinct compiler-wrapper PID.  Identity checks below
        # decide whether product wiring really retained one route.
        src0 = output / "runtime" / "tu0.cpp"
        src1 = output / "runtime" / "tu1.cpp"
        results["tu0"] = runtime.compile(src0, "wrapper0-tu0", runtime.client_socket)
        results["tu1"] = runtime.compile(src1, "wrapper1-tu1", runtime.client_socket)
        evidence = evidence_document(args.profile, runtime.root, SCENARIOS[0])
        same_relationships = [relationship for relationship in evidence["relationships"]
                             if 0 in relationship["TU_SEQ"] and 1 in relationship["TU_SEQ"]]
        same_pass = all(results[key].get("status") == "PASS" for key in ("tu0", "tu1")) and \
            results["tu0"].get("compiler_wrapper_pid") != results["tu1"].get("compiler_wrapper_pid") and \
            evidence["status"] == "PASS" and bool(same_relationships)
        writer.emit("scenario", "PASS" if same_pass else "HOLD", scenario=SCENARIOS[0], evidence=evidence, result=results)
        # Isolation is deliberately a second C daemon/connection.  Failure to
        # create it is a HOLD, never a synthetic alternate C_GUID.
        isolated: dict[str, Any] = {"status": "HOLD", "reason": "second-client-not-started"}
        client2_socket = runtime.client2_socket
        if client2_socket is not None:
            client2_cmd = [str(build / "daemon/iceccd"), "--no-remote", "-m", "0", "-s",
                           "127.0.0.1:%d" % runtime.sched_port, "-n", "s6-live-%d" % os.getpid(), "-N", "s6-c2",
                           "-b", str(runtime.root / "envs-c"), "-l", str(runtime.root / "c2.log"), "-vvv"]
            runtime.client2 = subprocess.Popen(client2_cmd, env=runtime.env(
                ICECC_TEST_SOCKET=str(client2_socket), ICECC_P50_ACTION_TRACE=str(runtime.root / "action.trace"),
                HOME=str(runtime.root / "home")), cwd=source, stdout=(runtime.root / "client2.stdout").open("w"),
                stderr=subprocess.STDOUT, start_new_session=True)
            time.sleep(1)
            isolated = runtime.compile(output / "runtime" / "tu2.cpp", "wrapper2-isolated", client2_socket)
            isolated["distinct_from_previous_wrapper"] = isolated.get("compiler_wrapper_pid") not in {
                results["tu0"].get("compiler_wrapper_pid"), results["tu1"].get("compiler_wrapper_pid")}
            identities = parse_identity(_read(runtime.root / "compile-identity.jsonl"))
            isolated["distinct_C_GUID"] = len({row["c_guid"] for row in identities[-3:]}) >= 2
            isolated["status"] = "PASS" if isolated.get("status") == "PASS" and isolated["distinct_from_previous_wrapper"] and isolated["distinct_C_GUID"] else "HOLD"
        writer.emit("scenario", isolated.get("status", "HOLD"), scenario=SCENARIOS[1], result=isolated)
        retry_source = output / "runtime" / "tu3.cpp"
        before_retry = parse_action(_read(runtime.root / "action.trace"))
        first_retry = runtime.compile(retry_source, "wrapper3-abort", runtime.client_socket, disposition="disconnect")
        retry = runtime.compile(retry_source, "wrapper4-retry", runtime.client_socket)
        retry_evidence = evidence_document(args.profile, runtime.root, SCENARIOS[2])
        after_retry = parse_action(_read(runtime.root / "action.trace"))
        retry_actions = after_retry[len(before_retry):]
        retry["first_attempt"] = first_retry
        retry["route_advanced"] = len({row["rel_seq"] for row in retry_actions}) > 1
        retry["abort_observed"] = any(row["action"] == "TX_ABORTED" for row in retry_actions)
        retry["retry_same_rel_seq"] = len({row["rel_seq"] for row in retry_actions}) == 1 and bool(retry_actions)
        retry["status"] = "PASS" if retry.get("status") == "PASS" and retry["abort_observed"] and retry["retry_same_rel_seq"] and retry_evidence["status"] == "PASS" else "HOLD"
        writer.emit("scenario", retry["status"], scenario=SCENARIOS[2], evidence=retry_evidence, result=retry)
        runtime.stop_worker()
        reset_ready = runtime.start_worker_replacement()
        reset = runtime.compile(output / "runtime" / "tu4.cpp", "wrapper4-after-f-reset", runtime.client_socket)
        reset_evidence = evidence_document(args.profile, runtime.root, SCENARIOS[3])
        generations = reset_evidence["F_GENERATION"]
        reset["status"] = "PASS" if reset.get("status") == "PASS" and reset_ready and len(generations) >= 2 else "HOLD"
        writer.emit("scenario", reset["status"], scenario=SCENARIOS[3], evidence=reset_evidence, result=reset)
        statuses = [same_pass, isolated.get("status") == "PASS", retry.get("status") == "PASS", reset.get("status") == "PASS"]
        final = "PASS" if all(statuses) else "HOLD"
        writer.emit("summary", final, scenarios=dict(zip(SCENARIOS, statuses)), output=str(output))
        return 0 if final == "PASS" else 77
    finally:
        # Runtime is local to the try body; this cleanup also handles launch
        # failures after the object has been created.
        runtime_obj = locals().get("runtime")
        if isinstance(runtime_obj, Runtime):
            runtime_obj.cleanup()
        writer.close()


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--profile", choices=PROFILES, default=DEFAULT_PROFILE)
    result.add_argument("--source-root", default=str(Path(__file__).resolve().parents[1]))
    result.add_argument("--build-root", default=None)
    result.add_argument("--experiment", default="s6-live-cross-wrapper-route")
    result.add_argument("--output-root", default=None,
                        help="artifact root (default: <source-root>/experiments)")
    result.add_argument("--timeout", type=float, default=180.0)
    result.add_argument("--docker", action="store_true",
                        help="run F and its cache service in the real farm Docker image")
    result.add_argument("--self-test", action="store_true")
    result.add_argument("--source-check", action="store_true")
    return result


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    if args.output_root is None:
        args.output_root = str(Path(args.source_root).resolve() / "experiments")
    if args.self_test:
        return self_test()
    if args.source_check:
        ok, issues = source_check(Path(args.source_root).resolve())
        print(json.dumps({"schema": SCHEMA + "-source-check-v1", "status": "PASS" if ok else "HOLD", "issues": issues}, sort_keys=True))
        return 0 if ok else 77
    return run_acceptance(args)


if __name__ == "__main__":
    raise SystemExit(main())
