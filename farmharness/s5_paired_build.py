#!/usr/bin/env python3
"""Run paired real all-P50 ZSTD_TU/legacy builds over a retained TU manifest.

Preparation (source/archive transfer, daemon startup, and environment creation)
is outside the measured compile interval.  Each invocation of the existing S4
remote cell script owns a fresh private namespace; only the all-P50 cache build
may start the cache sidecar.  This is a runner, not a statistical decision
engine: the S5 verifier remains the authority for complete block evidence.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tarfile
import tempfile
import time
from datetime import datetime, timezone
from io import BytesIO
from pathlib import Path
from typing import Any

from farmharness import s4_real_cells
from capability.distribution import s5_statistics


SCHEMA = "icecream-s5-zstd-tu-paired-run-v1"
EXPECTED_ROOT = "9a1653e600fbb28a7edeef02c10c5ccc5fe8fcad"
# This field is a historical name in the S5 schema; it carries the pinned
# source commit identity, not a digest of the external workload checkout.
SOURCE_SHA = EXPECTED_ROOT
P50_ROOT = "/tanksmall/scratch/ictmp/wt-s2-root-current-20260828"
DEFAULT_WORKLOAD = "/tanksmall/scratch/ictmp/src2/fmt/build/compile_commands.json"
DEFAULT_SOURCE_ROOT = "/tanksmall/scratch/ictmp/src2/fmt"
PROCESS_MARKERS = ("icecc-scheduler", "iceccd", "icecc-cache-service", "icecc")
# Hashes of the role binaries built from EXPECTED_ROOT with the pinned build
# environment.  Keep this S5 identity private to the runner: changing S4's
# historical matrix constants would silently alter its prior evidence.
P50_ROLE_HASHES = {
    "S": "8857b14179559d0efc9b588c29a6f0fe6c922c9f40d9fc2de498887257931212",
    "F": "56f6667da25d99660a335eb2349578946f5c1d096726dffa94df5f9fd8c62136",
    "C": "60004a5cd00ce8f54bc667217b87c02236c0d8d350260f81d0cfb5805ab4c985",
    "E": "ee7d30b240c38bccf66d4afcdd45993f115a01d4a2fb4e9143d38596609d2ba4",
    "X": "dcc6278720c409eeab1efb1491d61232f693b80159b22d4a5659853c0a43ae70",
}
SSH = ["-o", "HostName=10.0.27.101", "-o", "HostKeyAlias=tt-quietbox3",
       "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode() + b"\n"


def immutable_json(path: Path, value: Any) -> str:
    raw = canonical_bytes(value)
    # A manifest is an append-once evidence input.  Exclusive creation avoids
    # silently replacing a prior run if a timestamp or output directory is
    # accidentally reused.
    with path.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    path.chmod(0o444)
    return hashlib.sha256(raw).hexdigest()


def source_identity(repo: Path) -> dict[str, str]:
    """Return the immutable source identity required by the experiment."""
    try:
        completed = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "HEAD"],
            check=False, capture_output=True, text=True, timeout=10,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return {"status": "unavailable", "error": type(exc).__name__}
    commit = completed.stdout.strip()
    if completed.returncode or len(commit) != 40:
        return {"status": "unavailable", "error": completed.stderr.strip()[-200:]}
    # The runner itself is intentionally added on top of the pinned source
    # root.  Authenticate that all product paths still equal the root; a
    # change outside farmharness is an exact-source mismatch.
    exact = subprocess.run(
        ["git", "-C", str(repo), "diff", "--quiet", EXPECTED_ROOT, "--",
         ":!farmharness"], check=False, timeout=10,
    ).returncode == 0
    return {"status": "ok", "commit": commit, "root_commit": EXPECTED_ROOT,
            "product_tree_matches_root": str(exact).lower()}


def validate_source(repo: Path, expected: str = EXPECTED_ROOT) -> tuple[bool, dict[str, str]]:
    identity = source_identity(repo)
    return (identity.get("status") == "ok" and
            (identity.get("commit") == expected or identity.get("product_tree_matches_root") == "true")), identity


def _local_process_facts() -> list[dict[str, str]]:
    """List pre-existing icecream processes without modifying them."""
    try:
        completed = subprocess.run(
            ["ps", "-eo", "pid=,ppid=,stat=,etime=,comm=,args="], check=False,
            capture_output=True, text=True, timeout=10,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return [{"error": type(exc).__name__}]
    facts = []
    own_pid = str(os.getpid())
    for line in completed.stdout.splitlines():
        fields = line.strip().split(None, 5)
        if len(fields) < 6 or fields[0] == own_pid:
            continue
        command_name = fields[4].strip("[]")
        if command_name in PROCESS_MARKERS:
            facts.append({"pid": fields[0], "ppid": fields[1], "stat": fields[2],
                          "etime": fields[3], "command": fields[5]})
    return facts


def _target_preflight(args: argparse.Namespace) -> dict[str, Any]:
    """Probe target state before any worker/daemon is started.

    The remote command only reads process/listener state.  Any inability to
    establish a complete observation is a HOLD, never an empty/green result.
    """
    probe = (
        "set -u -o pipefail; "
        "command -v ps >/dev/null && command -v awk >/dev/null && command -v ss >/dev/null || "
        "{ echo PREFLIGHT_ERROR=required-observer-missing; exit 42; }; "
        "printf 'HOST=%s\\n' \"$(hostname)\"; "
        "ps -eo pid=,ppid=,stat=,etime=,comm=,args= | "
        "awk '$5==\"icecc-scheduler\" || $5==\"iceccd\" || $5==\"icecc-cache-service\" {print}' || true; "
        "ss -ltnp 2>/dev/null | awk '/:(22000|23000|24000|25000|26000|27000|28000|29000)/ {print}' || true"
    )
    command = ["ssh", *SSH, "mickg10@tt-quietbox3", "bash", "-c", probe]
    try:
        completed = subprocess.run(command, check=False, capture_output=True,
                                   text=True, timeout=min(args.timeout, 30))
    except (OSError, subprocess.SubprocessError) as exc:
        return {"status": "HOLD", "reason": "target-preflight-unavailable",
                "error": type(exc).__name__, "command": command, "stdout": "", "stderr": ""}
    lines = completed.stdout.splitlines()
    if not any(line.startswith("HOST=") for line in lines):
        return {"status": "HOLD", "reason": "target-preflight-incomplete", "command": command,
                "stdout": completed.stdout, "stderr": completed.stderr[-2000:]}
    processes = []
    for line in lines:
        fields = line.strip().split(None, 5)
        if (len(fields) >= 6 and fields[0].isdigit() and
                fields[4].strip("[]") in PROCESS_MARKERS):
            processes.append(line.strip())
    listeners = [line.strip() for line in lines if "LISTEN" in line or "users:(" in line]
    if completed.returncode != 0:
        return {"status": "HOLD", "reason": "target-preflight-failed", "returncode": completed.returncode,
                "command": command, "stdout": completed.stdout, "stderr": completed.stderr[-2000:]}
    if processes or listeners:
        return {"status": "HOLD", "reason": "target-process-contention", "processes": processes,
                "listeners": listeners, "command": command, "stdout": completed.stdout,
                "stderr": completed.stderr[-2000:]}
    return {"status": "READY", "reason": "target-clean", "processes": [], "listeners": [],
            "command": command, "stdout": completed.stdout, "stderr": completed.stderr[-2000:]}


def preflight(args: argparse.Namespace) -> dict[str, Any]:
    local = _local_process_facts()
    # The orchestrator does not run a scheduler, daemon, compiler, or cache
    # process locally.  Record local Icecream processes for context, but scope
    # the execution gate to the remote host and private experiment resources
    # that can affect the measured build.
    target = _target_preflight(args)
    if target.get("status") != "READY":
        return {"status": "HOLD", "reason": target.get("reason", "target-contention"),
                "local_processes": local, "target": target}
    return {"status": "READY", "reason": "target-uncontaminated",
            "local_processes": local, "target": target}


def role_manifest(root: Path) -> dict[str, Any]:
    """Validate and describe all P50 role artifacts before staging them."""
    entries: dict[str, Any] = {}
    required = {
        "S": root / "scheduler/icecc-scheduler",
        "F": root / "daemon/iceccd",
        "C": root / "client/icecc",
        "E": root / "client/icecc-create-env",
        "X": root / "cache/icecc-cache-service",
    }
    expected = P50_ROLE_HASHES
    errors: list[str] = []
    for role, path in required.items():
        item: dict[str, Any] = {"role": role, "path": str(path), "exists": path.is_file(),
                                "symlink": path.is_symlink()}
        if path.is_file() and not path.is_symlink():
            item["bytes"] = path.stat().st_size
            item["sha256"] = sha256_file(path)
            item["executable"] = os.access(path, os.X_OK) if role != "E" else True
            if item["sha256"] != expected.get(role):
                errors.append(f"{role}:hash-mismatch:{item['sha256']}")
            if not item["executable"]:
                errors.append(f"{role}:not-executable")
        else:
            errors.append(f"{role}:missing-or-symlink")
        entries[role] = item
    if errors:
        raise ValueError("exact-role-binary-mismatch:" + ";".join(errors))
    return {"root": str(root), "source_sha256": SOURCE_SHA, "roles": entries}


def _stage_p50(args: argparse.Namespace, roles: dict[str, Any]) -> str:
    """Stage only the already-validated role files into a private target root."""
    source = Path(roles["root"]).absolute()
    relatives = ["scheduler/icecc-scheduler", "daemon/iceccd", "client/icecc",
                 "client/icecc-create-env", "cache/icecc-cache-service"]
    tar_proc = subprocess.Popen(["tar", "-C", str(source), "-cf", "-", *relatives],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    remote = ("set -eu; root=$(mktemp -d /tmp/s5-p50-build.XXXXXX); "
              "mkdir -p \"$root/scheduler\" \"$root/daemon\" \"$root/client\" \"$root/cache\"; "
              "tar -xf - -C \"$root\"; printf 'S5_P50_STAGE_ROOT=%s\\n' \"$root\"")
    command = ["ssh", *SSH, "mickg10@tt-quietbox3", "bash", "-c", remote]
    assert tar_proc.stdout is not None
    extract = subprocess.Popen(command, stdin=tar_proc.stdout,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    tar_proc.stdout.close()
    output, error = extract.communicate(timeout=args.timeout)
    tar_rc = tar_proc.wait(timeout=10)
    if extract.returncode != 0 or tar_rc != 0:
        raise RuntimeError("exact P50 staging failed: " + error.decode(errors="replace")[-500:])
    line = next((line for line in output.decode(errors="replace").splitlines()
                 if line.startswith("S5_P50_STAGE_ROOT=")), "")
    if not line:
        raise RuntimeError("exact P50 staging returned no private root")
    return line.split("=", 1)[1]


def _archive(source_root: Path, relatives: list[str]) -> bytes:
    output = BytesIO()
    with tarfile.open(fileobj=output, mode="w:gz") as archive:
        for relative in relatives:
            path = source_root / relative
            if path.is_dir():
                archive.add(path, arcname=relative, recursive=True)
            else:
                archive.add(path, arcname=relative, recursive=False)
    return output.getvalue()


def _compile_tokens(row: dict[str, Any], source_root: Path) -> tuple[str, list[str]]:
    command = row.get("command") or row.get("arguments")
    tokens = list(command) if isinstance(command, list) else shlex.split(str(command))
    if not tokens:
        raise ValueError("workload row has no compiler command")
    source = Path(str(row["file"])).resolve()
    root = source_root.resolve()
    relative = source.relative_to(root).as_posix()
    flags: list[str] = []
    index = 1
    while index < len(tokens):
        token = tokens[index]
        if token in {"-o", "-c"}:
            index += 2
            continue
        if Path(token).resolve() == source:
            index += 1
            continue
        # The retained compile database points into the retained checkout.
        # Relocate only those paths; all other flags remain byte-for-byte.
        if str(root) in token:
            token = token.replace(str(root), "$WORK/source")
        flags.append(token)
        index += 1
    return relative, flags


def load_workload(path: Path, source_root: Path, selected: list[str] | None) -> list[dict[str, Any]]:
    entries = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(entries, list):
        raise ValueError("compile database must be an array")
    wanted = set(selected or [])
    result: list[dict[str, Any]] = []
    seen_sources: set[str] = set()
    for ordinal, row in enumerate(entries, 1):
        if not isinstance(row, dict) or "file" not in row:
            continue
        source = Path(str(row["file"])).resolve()
        try:
            relative, flags = _compile_tokens(row, source_root)
        except (ValueError, KeyError):
            continue
        if wanted and relative not in wanted and Path(relative).name not in wanted:
            continue
        if relative in seen_sources:
            continue
        seen_sources.add(relative)
        source_path = source_root / relative
        result.append({
            "tu_id": f"fmt-{ordinal:04d}-{Path(relative).stem}",
            "source": relative,
            "source_sha256": sha256_file(source_path),
            "flags": flags,
        })
    if not result:
        raise ValueError("no selected TUs in retained workload manifest")
    return result


def schedule(blocks: int, orders: tuple[str, ...] = ("AB", "BA"), *,
             regime: str = "cold") -> list[dict[str, Any]]:
    if blocks < 1 or not orders or regime not in s5_statistics.REGIMES or any(order not in s5_statistics.ORDERS for order in orders):
        raise ValueError("invalid block schedule")
    return [{"block_id": f"{regime}-{index + 1:02d}", "regime": regime,
             "order": orders[index % len(orders)], "sequence": index}
            for index in range(blocks)]


def schedule_matrix(cold_blocks: int = 4, warm_blocks: int = 4) -> list[dict[str, Any]]:
    """Return the preregistered balanced 4-cold + 4-warm block plan."""
    if cold_blocks < 0 or warm_blocks < 0 or not cold_blocks and not warm_blocks:
        raise ValueError("invalid block counts")
    return schedule(cold_blocks, regime="cold") + schedule(warm_blocks, regime="warm")


def _remote_script(source_archive_b64: str, tus: list[dict[str, Any]], mode: str,
                   source_root: Path) -> str:
    # The existing S4 script supplies the authenticated all-P50 Docker path,
    # cleanup, worker selection, cache engagement checks, and byte comparison.
    script = s4_real_cells.REMOTE_SCRIPT
    extract = (
        "printf '%s' '" + source_archive_b64 + "' | base64 -d >\"$WORK/source.tar.gz\"\n"
        "mkdir -p \"$WORK/source\"\n"
        "tar -xzf \"$WORK/source.tar.gz\" -C \"$WORK/source\"\n"
    )
    start = "S5_MEASURE_START_NS=$(date +%s%N)\n"
    finish = "S5_MEASURE_END_NS=$(date +%s%N)\n"
    # The smoke has one TU.  Full runs may pass more, but one remote cell is
    # intentionally one measured build/TU record so failures remain localized.
    tu = tus[0]
    def remote_quote(flag: str) -> str:
        # `$WORK` is deliberately expanded by the remote shell.  Quoting it
        # with shlex.quote would turn the relocation into a literal path.
        return flag if "$WORK" in flag else shlex.quote(flag)

    flags = " ".join(remote_quote(str(flag)) for flag in tu["flags"])
    source = "$WORK/source/" + tu["source"]
    compile_line = f'env "${{CLIENT_ENV[@]}}" timeout 150 "$C" g++ {flags} -c "{source}" -o "$REMOTE_OBJ"'
    # The reference is compiled on q3 from the same extracted retained source
    # and flags, just as the existing S4 byte-identity check does.
    local_line = f'g++ {flags} -c "{source}" -o "$LOCAL_OBJ"'
    old = "printf '%s\\n' '#include <cstdint>' \\\n  'extern \"C\" int s4_real_cell() { return 43 + 7; }' >\"$WORK/main.cpp\""
    if old not in script:
        raise RuntimeError("S4 source generation seam changed")
    script = script.replace(old, extract.rstrip("\n"), 1)
    old_remote = ('env "${CLIENT_ENV[@]}" timeout 150 "$C" g++ -std=c++17 -O2 '
                  '-c "$WORK/main.cpp" -o "$REMOTE_OBJ" \\\n'
                  "  >\"$WORK/client.log\" 2>&1 || { echo 'S4_STATUS=FAIL reason=remote-compile'; exit 1; }")
    old_local = ('g++ -std=c++17 -O2 -c "$WORK/main.cpp" -o "$LOCAL_OBJ" '
                 '2>"$WORK/local.log" || {\n'
                 "    echo 'S4_STATUS=FAIL reason=local-reference'; exit 1;\n"
                 "}")
    if old_remote not in script or old_local not in script:
        raise RuntimeError("S4 compile seam changed")
    measured_remote = (start + compile_line + ' \\\n'
                       "  >\"$WORK/client.log\" 2>&1 || { echo 'S4_STATUS=FAIL reason=remote-compile'; exit 1; }\n"
                       + finish.rstrip("\n"))
    script = script.replace(old_remote, measured_remote, 1)
    script = script.replace(old_local, local_line + ' 2>"$WORK/local.log" || {\n'
                            "    echo 'S4_STATUS=FAIL reason=local-reference'; exit 1;\n"
                            "}", 1)
    marker = 'echo "S4_CACHE_OBSERVED=$cache_seen"'
    if marker not in script:
        raise RuntimeError("S4 result marker changed")
    script = script.replace(marker,
                            'echo "S5_MEASURE_START_NS=$S5_MEASURE_START_NS"\n'
                            'echo "S5_MEASURE_END_NS=$S5_MEASURE_END_NS"\n' + marker)
    if mode == "legacy":
        strict_scheduler = ('case "$CELL" in\n'
                            '  s50-c50-f50|s50-c50-f50-c1f2) '
                            'S_EXTRA="--assignment-fence-mode strict-nonce";;\n'
                            'esac')
        if strict_scheduler not in script:
            raise RuntimeError("S4 strict scheduler seam changed")
        script = script.replace(strict_scheduler, 'S_EXTRA=""', 1)
    hashes = P50_ROLE_HASHES
    script = script.replace("$S_HASH", hashes["S"])
    script = script.replace("$C_HASH", hashes["C"])
    script = script.replace("$D_HASH", hashes["F"])
    script = script.replace("$F_HASH", hashes["F"])
    script = script.replace("$E_HASH", hashes["E"])
    # This runner passes the mode as the existing expectation bit; the shell
    # path itself remains the S4 proven implementation.
    return script


def _parse(stdout: str) -> tuple[dict[str, str], dict[str, str]]:
    fields, logs = s4_real_cells._parse_remote(stdout)
    for line in stdout.splitlines():
        if line.startswith("S5_") and "=" in line:
            key, value = line.split("=", 1)
            fields[key] = value
    return fields, logs


def run_build(args: argparse.Namespace, run_root: Path, block: dict[str, Any],
              mode: str, tu: dict[str, Any], archive_b64: str) -> dict[str, Any]:
    build_id = f"{block['block_id']}-{mode}-{tu['tu_id']}"
    evidence = run_root / "builds" / build_id
    evidence.mkdir(parents=True)
    expected_cache = mode == "cache"
    # A warm experiment needs a retained prewarm snapshot and namespace
    # continuity.  The S4 one-shot cell intentionally cannot provide that
    # contract, so record a non-timing HOLD without starting any process.
    if block["regime"] == "warm":
        row = {
            "kind": "build", "schema": SCHEMA, "build_id": build_id,
            "block_id": block["block_id"], "regime": block["regime"],
            "order": block["order"], "mode": mode, "tu_id": tu["tu_id"],
            "source": tu["source"], "source_sha256": tu["source_sha256"],
            "measurement_boundary": "measured-compile-only", "start_ns": None,
            "end_ns": None, "wall_seconds": None, "cache_expected": expected_cache,
            "cache_observed": False, "legacy_observed": False,
            "remote_compile": False, "byte_identical": False,
            "cleanup": "not-started", "status": "HOLD",
            "reason": "warm-state-cell-not-available",
            "namespace": {"namespace_id": f"s5/warm/{mode}/{block['block_id']}",
                           "mode_private": True, "reset": "not-run"},
            "returncode": None, "command": None, "evidence": str(evidence),
            "runner_elapsed_ns": 0,
        }
        (evidence / "record.json").write_text(json.dumps(row, sort_keys=True) + "\n",
                                                encoding="utf-8")
        return row
    remote_args = [str(args.p50_remote_root), str(args.p50_remote_root), str(args.p50_remote_root),
                   "s50-c50-f50", "1" if expected_cache else "0"]
    command = ["ssh", *SSH, "mickg10@tt-quietbox3", "bash", "-s", "--", *remote_args]
    script = _remote_script(archive_b64, [tu], mode, args.source_root)
    started = time.monotonic_ns()
    try:
        completed = subprocess.run(command, input=script, text=True, capture_output=True,
                                   timeout=args.timeout, check=False)
        stdout, stderr, rc = completed.stdout, completed.stderr, completed.returncode
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
        rc = 124
    (evidence / "ssh.stdout").write_text(stdout, encoding="utf-8", errors="replace")
    (evidence / "ssh.stderr").write_text(stderr, encoding="utf-8", errors="replace")
    fields, logs = _parse(stdout)
    for name, content in logs.items():
        (evidence / f"{name}.log").write_text(content, encoding="utf-8")
    status = fields.get("S4_STATUS", "FAIL").split(" ", 1)[0]
    if status not in {"PASS", "HOLD", "FAIL"}:
        status = "FAIL"
    measure_start = int(fields["S5_MEASURE_START_NS"]) if fields.get("S5_MEASURE_START_NS", "").isdigit() else None
    measure_end = int(fields["S5_MEASURE_END_NS"]) if fields.get("S5_MEASURE_END_NS", "").isdigit() else None
    reason = fields.get("S4_STATUS", "").split("reason=", 1)[1] \
        if "reason=" in fields.get("S4_STATUS", "") else ""
    row = {
        "kind": "build", "schema": SCHEMA, "build_id": build_id,
        "block_id": block["block_id"], "regime": block["regime"],
        "order": block["order"], "mode": mode, "tu_id": tu["tu_id"],
        "source": tu["source"], "source_sha256": tu["source_sha256"],
        "measurement_boundary": "measured-compile-only",
        "start_ns": measure_start, "end_ns": measure_end,
        "wall_seconds": ((measure_end - measure_start) / 1e9
                          if measure_start is not None and measure_end is not None else None),
        "cache_expected": expected_cache,
        "cache_observed": fields.get("S4_CACHE_OBSERVED") == "1",
        "legacy_observed": fields.get("S4_LEGACY_OBSERVED") == "1",
        "remote_compile": fields.get("S4_REMOTE_COMPILE") == "1",
        "byte_identical": fields.get("S4_BYTE_IDENTICAL") == "1",
        "cleanup": fields.get("S4_CLEANUP", "missing"), "status": status,
        "reason": reason,
        "namespace": {"namespace_id": f"s5/{block['regime']}/{mode}/{block['block_id']}",
                       "mode_private": True, "reset": "fresh-per-build" if block["regime"] == "cold" else "not-run"},
        "returncode": rc, "command": command, "evidence": str(evidence),
        "runner_elapsed_ns": time.monotonic_ns() - started,
    }
    (evidence / "record.json").write_text(json.dumps(row, sort_keys=True) + "\n", encoding="utf-8")
    return row


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workload-manifest", type=Path, default=Path(DEFAULT_WORKLOAD))
    parser.add_argument("--source-root", type=Path, default=Path(DEFAULT_SOURCE_ROOT))
    parser.add_argument("--p50-root", type=Path, default=Path(P50_ROOT))
    parser.add_argument("--out-root", type=Path, default=Path("/tanksmall/scratch/ictmp/experiments/icecream"))
    parser.add_argument("--tu", action="append", help="retained relative TU path or basename (repeatable)")
    parser.add_argument("--blocks", type=int, default=1,
                        help="cold blocks for a smoke (default: one clean paired cold block)")
    parser.add_argument("--warm-blocks", type=int, default=0,
                        help="warm blocks; opt-in only after the cold smoke is clean")
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1],
                        help="icecream checkout whose exact current commit is authenticated")
    parser.add_argument("--order", choices=("AB", "BA", "balanced"), default="balanced",
                        help="one fixed order for a smoke, or counterbalance AB/BA (default)")
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args(argv)
    if args.blocks < 1 or args.warm_blocks < 0 or not args.p50_root.is_dir():
        parser.error("positive cold --blocks, nonnegative --warm-blocks, and an exact P50 build root are required")
    source_ok, source = validate_source(args.repo_root)
    if not source_ok:
        parser.error(f"source identity mismatch: expected {EXPECTED_ROOT}, got {source}")
    tus = load_workload(args.workload_manifest, args.source_root, args.tu)
    # An S5 smoke is intentionally one TU/build.  Full corpus runs are opt-in
    # by omitting --tu; no statistic is emitted by this producer.
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    run_root = args.out_root / f"s5-zstd-tu-paired-{stamp}"
    run_root.mkdir(parents=True, exist_ok=False)
    archive = _archive(args.source_root, sorted({"include", *[tu["source"] for tu in tus]}))
    archive_b64 = base64.b64encode(archive).decode("ascii")
    workload = {"schema": "icecream-retained-fmt-workload-v1", "source_root": str(args.source_root),
                "manifest": str(args.workload_manifest), "manifest_sha256": sha256_file(args.workload_manifest),
                "tus": tus}
    workload_digest = immutable_json(run_root / "workload_manifest.json", workload)
    orders = (args.order,) if args.order != "balanced" else s5_statistics.ORDERS
    plan = schedule(args.blocks, orders)
    if args.warm_blocks:
        plan += schedule(args.warm_blocks, orders, regime="warm")
    role_error = ""
    try:
        roles = role_manifest(args.p50_root)
    except ValueError as exc:
        role_error = str(exc)
        roles = {"status": "HOLD", "root": str(args.p50_root),
                 "source_sha256": SOURCE_SHA, "expected_role_hashes": P50_ROLE_HASHES,
                 "error": role_error}
    check = preflight(args)
    if role_error:
        check = {"status": "HOLD", "reason": "exact-role-binary-mismatch",
                 "detail": role_error, "role_root": str(args.p50_root),
                 "expected_role_hashes": P50_ROLE_HASHES,
                 "local_processes": check.get("local_processes", []),
                 "target": check.get("target")}
    immutable_json(run_root / "preflight.json", check)
    experiment = {
        "schema": SCHEMA, "immutable": True, "root_commit": EXPECTED_ROOT,
        "source_sha256": SOURCE_SHA, "source_identity": source, "p50_root": str(args.p50_root),
        "role_manifest": roles,
        "p50_binary_hashes": P50_ROLE_HASHES,
        "s4_runner_sha256": sha256_file(Path(s4_real_cells.__file__)),
        "statistics_schema": s5_statistics.SCHEMA,
        "workload_manifest_digest": workload_digest, "modes": list(s5_statistics.MODES),
        "regimes": list(s5_statistics.REGIMES), "bootstrap_unit": "whole_block",
        "block_plan": plan, "cold_reset": "fresh remote mktemp WORK, fresh C/F env dirs, no prior cache namespace",
        "warm_reset": "mode-private namespace and prewarm snapshot must be retained; unsupported cells HOLD",
        "measurement_boundary": "preparation and reset outside; only client compile through local byte comparison interval",
        "smoke_scope": {"blocks": args.blocks, "tu_count": len(tus), "statistic_claim": False},
    }
    immutable_json(run_root / "experiment_manifest.json", experiment)
    if check.get("status") != "READY":
        # Emit only a typed HOLD row with the exact preflight facts.  In
        # particular, no wall time and no ratio are manufactured.
        result_path = run_root / "results.jsonl"
        with result_path.open("x", encoding="utf-8") as stream:
            stream.write(json.dumps({"kind": "hold", "schema": SCHEMA, "status": "HOLD",
                                     "reason": check["reason"], "preflight": check,
                                     "block_plan": plan}, sort_keys=True) + "\n")
            stream.flush(); os.fsync(stream.fileno())
        summary = {"run_root": str(run_root), "result": str(result_path), "status": "HOLD",
                   "reason": check["reason"], "blocks": 0, "paired_blocks": 0,
                   "ratios": [], "preflight": str(run_root / "preflight.json")}
        print("S5_STATUS=HOLD reason=" + check["reason"], file=sys.stderr)
        print(json.dumps(summary, sort_keys=True))
        return 77
    stage_args = argparse.Namespace(
        p50_local_root=str(args.p50_root), ssh_option=SSH, host="mickg10@tt-quietbox3",
        timeout=args.timeout,
    )
    staged_root = _stage_p50(stage_args, roles)
    args.p50_remote_root = staged_root
    rows: list[dict[str, Any]] = []
    try:
        for block in plan:
            ordered = ("cache", "legacy") if block["order"] == "AB" else ("legacy", "cache")
            for mode in ordered:
                for tu in tus:
                    rows.append(run_build(args, run_root, block, mode, tu, archive_b64))
    finally:
        subprocess.run(["ssh", *SSH, "mickg10@tt-quietbox3", "rm", "-rf", "--", staged_root],
                       timeout=min(args.timeout, 30), check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    result_path = run_root / "results.jsonl"
    with result_path.open("x", encoding="utf-8") as stream:
        for row in rows:
            stream.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    summary = {"run_root": str(run_root), "result": str(result_path),
               "pass": sum(row["status"] == "PASS" for row in rows),
               "hold": sum(row["status"] == "HOLD" for row in rows),
               "fail": sum(row["status"] == "FAIL" for row in rows),
               "blocks": len(plan), "tu_count": len(tus), "statistic_claim": False}
    print(json.dumps(summary, sort_keys=True))
    return 0 if summary["fail"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
