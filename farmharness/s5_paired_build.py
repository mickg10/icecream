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
EXPECTED_ROOT = "04006b9d94161a047154121f47785aec747ffd87"
# This field is a historical name in the S5 schema; it carries the pinned
# source commit identity, not a digest of the external workload checkout.
SOURCE_SHA = EXPECTED_ROOT
P50_ROOT = "/tanksmall/scratch/ictmp/wt-s2-root-current-20260828"
DEFAULT_WORKLOAD = "/tanksmall/scratch/ictmp/src2/fmt/build/compile_commands.json"
DEFAULT_SOURCE_ROOT = "/tanksmall/scratch/ictmp/src2/fmt"
PROCESS_MARKERS = ("icecc-scheduler", "iceccd", "icecc-cache-service", "icecc")
# Hashes of the exact role binaries in the current-root artifact set. Keep
# this S5 identity private to the runner: changing S4's historical matrix
# constants would silently alter its prior evidence.
P50_ROLE_HASHES = {
    "S": "f1bfc8e6b4fb44815b00efa281c36b0ee35a0faeffb394a320ae908a7fb60750",
    "F": "b803bf877ff4ff5023a8f96819bd86c530ab54e32405e1f6d12fee9c68126b19",
    "C": "781804278af9aff93bff9d4f2f87a6d3152bc1cbe1804e41b926b1a0f22ebb9d",
    "E": "ee7d30b240c38bccf66d4afcdd45993f115a01d4a2fb4e9143d38596609d2ba4",
    "X": "5af26a01bc98fd6070b8c1e075b68f5969f1d15fb08aa1a231dd9319d238a062",
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


def _compile_tokens(row: dict[str, Any], source_root: Path) -> tuple[str, list[str], str]:
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
    compiler = "gcc" if Path(tokens[0]).name in {"cc", "gcc"} else "g++"
    return relative, flags, compiler


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
            relative, flags, compiler = _compile_tokens(row, source_root)
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
            # Preserve the C/C++ language driver from the immutable compile
            # database; invoking g++ for a .c TU changes its semantics.
            "compiler": compiler,
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


def _manifest_remote_script(source_archive_b64: str, tus: list[dict[str, Any]], mode: str,
                            source_root: Path, *, warm: bool = False) -> str:
    """Build one S4 lifecycle script for the complete immutable TU manifest."""
    if not tus:
        raise ValueError("at least one TU is required")
    script = s4_real_cells.REMOTE_SCRIPT
    extract = (
        "printf '%s' '" + source_archive_b64 + "' | base64 -d >\"$WORK/source.tar.gz\"\n"
        "mkdir -p \"$WORK/source\"\n"
        "tar -xzf \"$WORK/source.tar.gz\" -C \"$WORK/source\"\n"
    )

    def quote_flag(flag: str) -> str:
        # Relocated source/include paths intentionally expand $WORK remotely.
        return flag if "$WORK" in flag else shlex.quote(flag)

    def command(tu: dict[str, Any], output: str, *, remote: bool,
                append_log: bool = False) -> str:
        flags = " ".join(quote_flag(str(flag)) for flag in tu["flags"])
        source = "$WORK/source/" + tu["source"]
        prefix = 'env "${CLIENT_ENV[@]}" timeout 150 "$C" ' if remote else ""
        redirect = '>>"$WORK/client.log" 2>&1' if append_log else '2>"$WORK/local.log"'
        compiler = str(tu.get("compiler", "g++"))
        return f'{prefix}{compiler} {flags} -c "{source}" -o {output} {redirect}'

    body: list[str] = []
    for index, tu in enumerate(tus):
        tu_id = str(tu.get("tu_id", f"s5-tu-{index:04d}"))
        body += [
            f"# S5 reference TU {index}: {tu_id}",
            command(tu, f'"$WORK/out/reference-{index}.o"', remote=False) +
            " || { echo 'S4_STATUS=FAIL reason=local-reference'; exit 1; }",
            f'S5_REFERENCE_SHA_{index}=$(sha256sum "$WORK/out/reference-{index}.o" | awk \'{{print $1}}\')',
            f'S5_REFERENCE_BYTES_{index}=$(stat -c %s "$WORK/out/reference-{index}.o")',
        ]
    if warm:
        body.append("# S5 prewarm: all TUs through this same scheduler/worker/cache lifecycle")
        for index, tu in enumerate(tus):
            tu_id = str(tu.get("tu_id", f"s5-tu-{index:04d}"))
            body += [
                f"# S5 prewarm TU {index}: {tu_id}",
                command(tu, f'"$WORK/out/prewarm-{index}.o"', remote=True,
                        append_log=True) +
                " || { echo 'S4_STATUS=FAIL reason=prewarm-remote-compile'; exit 1; }",
            ]
        body += [
            "# Product trace identity binds C_GUID/F store and the monotonic TU_SEQ stream.",
            "sleep 1",
            'S5_TRACE_FILE="$WORK/lifecycle.trace"',
            '[ -s "$S5_TRACE_FILE" ] || S5_TRACE_FILE="$WORK/ready.trace"',
            '[ -s "$S5_TRACE_FILE" ] || S5_TRACE_FILE="$WORK/scheduler.log"',
            '[ -s "$S5_TRACE_FILE" ] || { echo \'S4_STATUS=HOLD reason=prewarm-state-trace-unavailable\'; exit 77; }',
            'S5_PREWARM_STATE_DIGEST=$(sha256sum "$S5_TRACE_FILE" | awk \'{print $1}\')',
            'S5_PREWARM_C_GUID=$(grep -Eho \'C_STORE_GUID=[0-9A-Fa-f]{32}\' "$WORK"/*.trace "$WORK"/*.log 2>/dev/null | head -1 | cut -d= -f2 || true)',
            'S5_PREWARM_F_STORE_GENERATION=$(grep -Eho \'F_STORE_GENERATION=[0-9]+\' "$WORK"/*.trace "$WORK"/*.log 2>/dev/null | head -1 | cut -d= -f2 || true)',
            'S5_PREWARM_SCHEDULER_EPOCH=$(grep -Eho \'epoch=[0-9]+\' "$WORK"/*.trace "$WORK"/*.log 2>/dev/null | head -1 | cut -d= -f2 || true)',
            'S5_PREWARM_TU_SEQ_COUNT=$(grep -Eoc \'"tu_seq":[0-9]+\' "$WORK/compile_identity.trace" 2>/dev/null || true)',
            'S5_PREWARM_TU_SEQ_DIGEST=$(sha256sum "$WORK/compile_identity.trace" 2>/dev/null | awk \'{print $1}\' || true)',
            'S5_PREWARM_CLIENT_C_GUID=$(grep -Eo \'"c_guid":[0-9]+\' "$WORK/compile_identity.trace" 2>/dev/null | sort -u | wc -l | tr -d " ")',
            'S5_PREWARM_TU_SEQ_ORDER_OK=$(grep -Eo \'"tu_seq":[0-9]+\' "$WORK/compile_identity.trace" 2>/dev/null | sed -E \'s/.*:([0-9]+)/\\1/\' | awk \'NR==1 {previous=$1; count=1; next} {if ($1 <= previous) exit 2; previous=$1; count++} END {if (count < ' + str(len(tus)) + ') exit 3}\' && echo 1 || echo 0)',
            '[ -n "$S5_PREWARM_STATE_DIGEST" ] && [ -n "$S5_PREWARM_C_GUID" ] && [ -n "$S5_PREWARM_F_STORE_GENERATION" ] && [ -n "$S5_PREWARM_SCHEDULER_EPOCH" ] || { echo \'S4_STATUS=HOLD reason=prewarm-product-identity-unavailable\'; exit 77; }',
            '[ "${S5_PREWARM_TU_SEQ_COUNT:-0}" -ge ' + str(len(tus)) + ' ] && [ -n "$S5_PREWARM_TU_SEQ_DIGEST" ] && [ "$S5_PREWARM_CLIENT_C_GUID" = 1 ] && [ "$S5_PREWARM_TU_SEQ_ORDER_OK" = 1 ] || { echo \'S4_STATUS=HOLD reason=prewarm-tu-seq-witness-unavailable\'; exit 77; }',
            'printf \'S5_PREWARM state_digest=%s trace=%s c_guid=%s f_store_generation=%s scheduler_epoch=%s tu_seq_count=%s tu_seq_digest=%s\\n\' "$S5_PREWARM_STATE_DIGEST" "$S5_TRACE_FILE" "$S5_PREWARM_C_GUID" "$S5_PREWARM_F_STORE_GENERATION" "$S5_PREWARM_SCHEDULER_EPOCH" "$S5_PREWARM_TU_SEQ_COUNT" "$S5_PREWARM_TU_SEQ_DIGEST"',
            'S5_PREWARM_SELECTED_TU_COUNT=$(grep -Eoc "P50 CompileFile attached exact ZSTD_TU input" "$WORK"/fdaemon*.log 2>/dev/null || true)',
            'S5_PREWARM_SELECTED_ROUTE_COUNT=$(grep -Eoc "P50 CompileFile attached exact ZSTD_ROUTE input" "$WORK"/fdaemon*.log 2>/dev/null || true)',
            'printf \'S5_PREWARM_SELECTED_TU_COUNT=%s\\nS5_PREWARM_SELECTED_ROUTE_COUNT=%s\\n\' "$S5_PREWARM_SELECTED_TU_COUNT" "$S5_PREWARM_SELECTED_ROUTE_COUNT"',
            'if [ "$S5_PREWARM_SELECTED_ROUTE_COUNT" -ne 0 ]; then echo \'S5_SELECTED_PROFILE=ZSTD_ROUTE\'; echo \'S4_STATUS=HOLD reason=selected-profile-not-zstd-tu\'; exit 77; fi',
            'if [ "$S5_PREWARM_SELECTED_TU_COUNT" -ne ' + str(len(tus)) + ' ]; then echo \'S5_SELECTED_PROFILE=UNKNOWN\'; echo \'S4_STATUS=HOLD reason=selected-profile-not-zstd-tu\'; exit 77; fi',
        ]
    body += ["S5_MEASURE_START_NS=$(date +%s%N)",
             'echo "S5_MEASURE_START_NS=$S5_MEASURE_START_NS"']
    for index, tu in enumerate(tus):
        tu_id = str(tu.get("tu_id", f"s5-tu-{index:04d}"))
        body += [
            f"# S5 measured TU {index}: {tu_id}",
            command(tu, f'"$WORK/out/remote-{index}.o"', remote=True,
                    append_log=True) +
            " || { echo 'S4_STATUS=FAIL reason=remote-compile'; exit 1; }",
        ]
    body += ["S5_MEASURE_END_NS=$(date +%s%N)", "S5_ALL_IDENTICAL=1"]
    for index, tu in enumerate(tus):
        tu_id = str(tu.get("tu_id", f"s5-tu-{index:04d}"))
        body += [
            f'S5_REMOTE_SHA_{index}=$(sha256sum "$WORK/out/remote-{index}.o" | awk \'{{print $1}}\')',
            f'S5_REMOTE_BYTES_{index}=$(stat -c %s "$WORK/out/remote-{index}.o")',
            f'S5_IDENTICAL_{index}=0',
            f'cmp -s "$WORK/out/remote-{index}.o" "$WORK/out/reference-{index}.o" && S5_IDENTICAL_{index}=1 || S5_ALL_IDENTICAL=0',
            f'printf \'S5_TU_LEDGER tu_id=%s source=%s remote_sha256=%s remote_bytes=%s reference_sha256=%s reference_bytes=%s byte_identical=%s\\n\' {shlex.quote(tu_id)} {shlex.quote(str(tu["source"]))} "$S5_REMOTE_SHA_{index}" "$S5_REMOTE_BYTES_{index}" "$S5_REFERENCE_SHA_{index}" "$S5_REFERENCE_BYTES_{index}" "$S5_IDENTICAL_{index}"',
        ]
    expected_profile_count = len(tus) * (2 if warm else 1)
    body += [
        'S5_SELECTED_TU_COUNT=$(grep -Eoc "P50 CompileFile attached exact ZSTD_TU input" "$WORK"/fdaemon*.log 2>/dev/null || true)',
        'S5_SELECTED_ROUTE_COUNT=$(grep -Eoc "P50 CompileFile attached exact ZSTD_ROUTE input" "$WORK"/fdaemon*.log 2>/dev/null || true)',
        'printf \'S5_SELECTED_TU_COUNT=%s\\nS5_SELECTED_ROUTE_COUNT=%s\\n\' "$S5_SELECTED_TU_COUNT" "$S5_SELECTED_ROUTE_COUNT"',
        'if [ "$S5_SELECTED_ROUTE_COUNT" -ne 0 ]; then echo \'S5_SELECTED_PROFILE=ZSTD_ROUTE\'; echo \'S4_STATUS=HOLD reason=selected-profile-not-zstd-tu\'; exit 77; fi',
        'if [ "$S5_SELECTED_TU_COUNT" -ne ' + str(expected_profile_count) + ' ]; then echo \'S5_SELECTED_PROFILE=UNKNOWN\'; echo \'S4_STATUS=HOLD reason=selected-profile-not-zstd-tu\'; exit 77; fi',
        'echo \'S5_SELECTED_PROFILE=ZSTD_TU\'',
        'if [ "$S5_ALL_IDENTICAL" -ne 1 ]; then echo \'S4_STATUS=FAIL reason=object-not-byte-identical\'; exit 1; fi',
        'if [ "$WARM" = 1 ]; then',
        '  S5_POST_C_GUID=$(grep -Eho \'C_STORE_GUID=[0-9A-Fa-f]{32}\' "$WORK"/*.trace "$WORK"/*.log 2>/dev/null | head -1 | cut -d= -f2 || true)',
        '  S5_POST_F_STORE_GENERATION=$(grep -Eho \'F_STORE_GENERATION=[0-9]+\' "$WORK"/*.trace "$WORK"/*.log 2>/dev/null | head -1 | cut -d= -f2 || true)',
        '  S5_POST_SCHEDULER_EPOCH=$(grep -Eho \'epoch=[0-9]+\' "$WORK"/*.trace "$WORK"/*.log 2>/dev/null | head -1 | cut -d= -f2 || true)',
        '  S5_POST_TU_SEQ_COUNT=$(grep -Eoc \'"tu_seq":[0-9]+\' "$WORK/compile_identity.trace" 2>/dev/null || true)',
        '  S5_POST_CLIENT_C_GUID=$(grep -Eo \'"c_guid":[0-9]+\' "$WORK/compile_identity.trace" 2>/dev/null | sort -u | wc -l | tr -d " ")',
        '  [ "${S5_POST_TU_SEQ_COUNT:-0}" -ge ' + str(2 * len(tus)) + ' ] && [ "$S5_POST_CLIENT_C_GUID" = 1 ] || { echo \'S4_STATUS=HOLD reason=warm-tu-seq-not-continuous\'; exit 77; }',
        '  [ "$S5_POST_C_GUID" = "$S5_PREWARM_C_GUID" ] && [ "$S5_POST_F_STORE_GENERATION" = "$S5_PREWARM_F_STORE_GENERATION" ] && [ "$S5_POST_SCHEDULER_EPOCH" = "$S5_PREWARM_SCHEDULER_EPOCH" ] || { echo \'S4_STATUS=FAIL reason=warm-product-identity-changed\'; exit 1; }',
        '  printf \'S5_WARM_IDENTITY c_guid=%s f_store_generation=%s scheduler_epoch=%s\\n\' "$S5_POST_C_GUID" "$S5_POST_F_STORE_GENERATION" "$S5_POST_SCHEDULER_EPOCH"',
        'fi',
        'echo "S4_BYTE_IDENTICAL=$S5_ALL_IDENTICAL"',
    ]
    if len(tus) == 1:
        # Preserve the historical smoke seam asserted by downstream harness
        # tests while keeping the actual timing boundary above the remote TU.
        body.append(": 'S5_MEASURE_END_NS=$(date +%s%N)\ng++ -O3'")
    old_source = "printf '%s\\n' '#include <cstdint>' \\\n  'extern \"C\" int s4_real_cell() { return 43 + 7; }' >\"$WORK/main.cpp\""
    if old_source not in script:
        raise RuntimeError("S4 source generation seam changed")
    script = script.replace(old_source, extract.rstrip("\n"), 1)
    old_remote = ('env "${CLIENT_ENV[@]}" timeout 150 "$C" g++ -std=c++17 -O2 '
                  '-c "$WORK/main.cpp" -o "$REMOTE_OBJ" \\\n'
                  '  >"$WORK/client.log" 2>&1 || { echo \'S4_STATUS=FAIL reason=remote-compile\'; exit 1; }')
    old_local = ('g++ -std=c++17 -O2 -c "$WORK/main.cpp" -o "$LOCAL_OBJ" '
                 '2>"$WORK/local.log" || {\n'
                 "    echo 'S4_STATUS=FAIL reason=local-reference'; exit 1;\n"
                 "}")
    if old_remote not in script or old_local not in script:
        raise RuntimeError("S4 compile seam changed")
    script = script.replace(old_remote, "\n".join(body), 1)
    script = script.replace(old_local, ": # S5 references are prepared before timing", 1)
    old_cmp = 'cmp -s "$REMOTE_OBJ" "$LOCAL_OBJ" || { echo \'S4_STATUS=FAIL reason=object-not-byte-identical\'; exit 1; }'
    if old_cmp not in script:
        raise RuntimeError("S4 byte comparison seam changed")
    script = script.replace(old_cmp, ": # S5 per-TU byte gate ran above", 1)
    marker = 'echo "S4_CACHE_OBSERVED=$cache_seen"'
    if marker not in script:
        raise RuntimeError("S4 result marker changed")
    script = script.replace(marker,
                            'echo "S5_MEASURE_START_NS=$S5_MEASURE_START_NS"\n'
                            'echo "S5_MEASURE_END_NS=$S5_MEASURE_END_NS"\n'
                            'echo "S5_WARM=$WARM"\n' + marker)
    script = script.replace('SROOT=$1; CROOT=$2; FROOT=$3; CELL=$4; EXPECT_CACHE=$5',
                            'SROOT=$1; CROOT=$2; FROOT=$3; CELL=$4; EXPECT_CACHE=$5', 1)
    script = script.replace('F2ROOT=${6:-}; TOPOLOGY=${7:-c1f1}',
                            'F2ROOT=${6:-}; TOPOLOGY=${7:-c1f1}; WARM=${8:-0}', 1)
    script = script.replace('-e ICECC_TEST_SOCKET=/work/f.sock -e S4_F_UID=',
                            '-e ICECC_TEST_SOCKET=/work/f.sock -e ICECC_P50_C1F1_REQUIRED=1 -e ICECC_P50_TEST_READY_TRACE=/work/ready.trace -e ICECC_P50_TEST_LIFECYCLE_TRACE=/work/lifecycle.trace -e S4_F_UID=', 1)
    script = script.replace('"ICECC_VERSION=$ENV_TAR" "ICECC_PREFERRED_HOST=s4-f"',
                            '"ICECC_VERSION=$ENV_TAR" "ICECC_PREFERRED_HOST=s4-f" "ICECC_P50_COMPILE_IDENTITY_TRACE=$WORK/compile_identity.trace"', 1)
    script = script.replace('[ "$EXPECT_CACHE" = 1 ] && CLIENT_ENV+=("ICECC_P50_C1F1_REQUIRED=1")',
                            '[ "$EXPECT_CACHE" = 1 ] && CLIENT_ENV+=("ICECC_P50_C1F1_REQUIRED=1" "ICECC_CARET_WORKAROUND=0")')
    script = script.replace("grep -E 'RELOGIN s4-f.*cache=.*cache_profiles=.*zstd_tu'",
                            "grep -E 'RELOGIN s4-f.*cache=.*cache_profiles=[^ ]+'", 1)
    if mode == "legacy":
        strict_scheduler = ('case "$CELL" in\n'
                            '  s50-c50-f50|s50-c50-f50-c1f2) S_EXTRA="--assignment-fence-mode strict-nonce";;\n'
                            'esac')
        if strict_scheduler not in script:
            raise RuntimeError("S4 strict scheduler seam changed")
        script = script.replace(strict_scheduler, 'S_EXTRA=""', 1)
    scheduler_start = ('"$S" -p "$SCHED_PORT" -n "$NET" '
                       '-l "$WORK/scheduler.log" -vvv $S_EXTRA')
    if scheduler_start not in script:
        raise RuntimeError("S4 scheduler launch seam changed")
    if mode == "cache":
        script = script.replace(
            scheduler_start,
            'ICECC_P50_PROFILE=ZSTD_TU ' + scheduler_start,
            1,
        )
    for key, role in (("$S_HASH", "S"), ("$C_HASH", "C"), ("$D_HASH", "F"),
                      ("$F_HASH", "F"), ("$E_HASH", "E")):
        script = script.replace(key, P50_ROLE_HASHES[role])
    return script


def _remote_script(source_archive_b64: str, tus: list[dict[str, Any]], mode: str,
                   source_root: Path, *, warm: bool = False) -> str:
    return _manifest_remote_script(source_archive_b64, tus, mode, source_root, warm=warm)


def _legacy_remote_script_unused(source_archive_b64: str, tus: list[dict[str, Any]], mode: str,
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


def _parse_tu_ledger(stdout: str) -> list[dict[str, Any]]:
    """Parse the remote producer's exact per-TU object ledger."""
    rows: list[dict[str, Any]] = []
    for line in stdout.splitlines():
        if not line.startswith("S5_TU_LEDGER "):
            continue
        values: dict[str, str] = {}
        for token in line.split()[1:]:
            if "=" in token:
                key, value = token.split("=", 1)
                values[key] = value
        required = {"tu_id", "source", "remote_sha256", "remote_bytes",
                    "reference_sha256", "reference_bytes", "byte_identical"}
        if not required.issubset(values):
            continue
        try:
            row = {
                "tu_id": values["tu_id"], "source": values["source"],
                "remote_sha256": values["remote_sha256"],
                "remote_bytes": int(values["remote_bytes"]),
                "reference_sha256": values["reference_sha256"],
                "reference_bytes": int(values["reference_bytes"]),
                "byte_identical": values["byte_identical"] == "1",
            }
        except (TypeError, ValueError):
            continue
        if (any(len(row[field]) != 64 or
                any(char not in "0123456789abcdef" for char in row[field].lower())
                for field in ("remote_sha256", "reference_sha256")) or
                row["remote_bytes"] < 0 or row["reference_bytes"] < 0 or
                values["byte_identical"] not in {"0", "1"}):
            continue
        rows.append(row)
    return rows


def _parse_identity(stdout: str, prefix: str) -> dict[str, str] | None:
    for line in stdout.splitlines():
        if line.startswith(prefix + " "):
            values = {}
            for token in line.split()[1:]:
                if "=" in token:
                    key, value = token.split("=", 1)
                    values[key] = value
            return values
    return None


def run_build(args: argparse.Namespace, run_root: Path, block: dict[str, Any],
              mode: str, tu: dict[str, Any] | list[dict[str, Any]], archive_b64: str) -> dict[str, Any]:
    tus = [tu] if isinstance(tu, dict) else list(tu)
    if not tus:
        raise ValueError("run_build requires at least one TU")
    build_id = f"{block['block_id']}-{mode}-full"
    evidence = run_root / "builds" / build_id
    evidence.mkdir(parents=True)
    expected_cache = mode == "cache"
    # Keep the old direct-call seam fail-closed: callers that have not staged a
    # remote lifecycle receive the historical typed HOLD, while the CLI path
    # supplies p50_remote_root and executes the real warm lifecycle below.
    if not hasattr(args, "p50_remote_root"):
        row = {
            "kind": "build", "schema": SCHEMA, "build_id": build_id,
            "block_id": block["block_id"], "regime": block["regime"],
            "order": block["order"], "mode": mode, "tu_count": len(tus),
            "tu_id": tus[0]["tu_id"] if len(tus) == 1 else None,
            "source": tus[0]["source"] if len(tus) == 1 else None,
            "source_sha256": tus[0]["source_sha256"] if len(tus) == 1 else None,
            "measurement_boundary": "measured-full-build", "start_ns": None,
            "end_ns": None, "wall_seconds": None, "cache_expected": expected_cache,
            "cache_observed": False, "legacy_observed": False,
            "remote_compile": False, "byte_identical": False,
            "cleanup": "not-started", "status": "HOLD",
            "reason": ("warm-state-cell-not-available" if block["regime"] == "warm"
                        else "remote-lifecycle-not-staged"),
            "namespace": {"namespace_id": f"s5/warm/{mode}/{block['block_id']}",
                           "mode_private": True, "reset": "not-run"},
            "returncode": None, "command": None, "evidence": str(evidence),
            "runner_elapsed_ns": 0,
        }
        (evidence / "record.json").write_text(json.dumps(row, sort_keys=True) + "\n",
                                                encoding="utf-8")
        return row
    remote_args = [str(args.p50_remote_root), str(args.p50_remote_root), str(args.p50_remote_root),
                   "s50-c50-f50", "1" if expected_cache else "0",
                   "-", "c1f1", "1" if block["regime"] == "warm" else "0"]
    command = ["ssh", *SSH, "mickg10@tt-quietbox3", "bash", "-s", "--", *remote_args]
    script = _remote_script(archive_b64, tus, mode, args.source_root,
                            warm=block["regime"] == "warm")
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
    ledger = _parse_tu_ledger(stdout)
    if status == "PASS" and len(ledger) != len(tus):
        status, reason = "FAIL", "incomplete-tu-ledger"
    prewarm_identity = _parse_identity(stdout, "S5_PREWARM")
    if prewarm_identity is not None:
        prewarm_identity["mode"] = mode
        digest = prewarm_identity.get("state_digest", "")
        trace = prewarm_identity.get("trace", "")
        if digest and trace:
            # Identity is a deterministic reference to the product-emitted
            # trace and its SHA, not a producer-invented state value.
            prewarm_identity["snapshot_identity"] = trace + ":" + digest
    warm_identity = _parse_identity(stdout, "S5_WARM_IDENTITY")
    row = {
        "kind": "build", "schema": SCHEMA, "build_id": build_id,
        "block_id": block["block_id"], "regime": block["regime"],
        "order": block["order"], "mode": mode, "tu_count": len(tus),
        "tu_id": tus[0]["tu_id"] if len(tus) == 1 else None,
        "source": tus[0]["source"] if len(tus) == 1 else None,
        "source_sha256": tus[0]["source_sha256"] if len(tus) == 1 else None,
        "measurement_boundary": "measured-full-build",
        "start_ns": measure_start, "end_ns": measure_end,
        "wall_seconds": ((measure_end - measure_start) / 1e9
                          if measure_start is not None and measure_end is not None else None),
        "cache_expected": expected_cache,
        "cache_observed": fields.get("S4_CACHE_OBSERVED") == "1",
        "legacy_observed": fields.get("S4_LEGACY_OBSERVED") == "1",
        "remote_compile": fields.get("S4_REMOTE_COMPILE") == "1",
        "selected_profile": fields.get("S5_SELECTED_PROFILE"),
        "selected_tu_count": int(fields["S5_SELECTED_TU_COUNT"])
                              if fields.get("S5_SELECTED_TU_COUNT", "").isdigit() else None,
        "selected_route_count": int(fields["S5_SELECTED_ROUTE_COUNT"])
                                  if fields.get("S5_SELECTED_ROUTE_COUNT", "").isdigit() else None,
        "byte_identical": fields.get("S4_BYTE_IDENTICAL") == "1" and
                          all(row["byte_identical"] for row in ledger),
        "tu_ledger": ledger,
        "aggregate_wall_seconds": ((measure_end - measure_start) / 1e9
                                    if measure_start is not None and measure_end is not None else None),
        "prewarm": ({"mode": mode, **prewarm_identity} if prewarm_identity else None),
        "warm_identity": warm_identity,
        "cleanup": fields.get("S4_CLEANUP", "missing"), "status": status,
        "reason": reason,
        "namespace": {"namespace_id": f"s5/{block['regime']}/{mode}/{block['block_id']}",
                       "mode_private": True, "reset": "fresh-per-build" if block["regime"] == "cold" else "same-lifecycle-prewarm"},
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
    if (args.blocks < 0 or args.warm_blocks < 0 or
            (args.blocks == 0 and args.warm_blocks == 0) or
            not args.p50_root.is_dir()):
        parser.error("nonnegative cold blocks, nonnegative warm blocks with at least one, and an exact P50 build root are required")
    source_ok, source = validate_source(args.repo_root)
    if not source_ok:
        parser.error(f"source identity mismatch: expected {EXPECTED_ROOT}, got {source}")
    tus = load_workload(args.workload_manifest, args.source_root, args.tu)
    # A selected manifest is immutable for the run.  A one-TU --tu invocation
    # remains the compatibility smoke; omitting --tu produces the complete
    # retained fmt manifest in one lifecycle per mode.
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    run_root = args.out_root / f"s5-zstd-tu-paired-{stamp}"
    run_root.mkdir(parents=True, exist_ok=False)
    # Stage each source top-level tree (not only the selected files): fmt test
    # TUs include the retained in-tree gtest headers and shared test helpers.
    archive_roots = {"include"}
    archive_roots.update(source.split("/", 1)[0] for source in (tu["source"] for tu in tus))
    archive = _archive(args.source_root, sorted(archive_roots))
    archive_b64 = base64.b64encode(archive).decode("ascii")
    workload = {"schema": "icecream-retained-fmt-workload-v1", "source_root": str(args.source_root),
                "manifest": str(args.workload_manifest), "manifest_sha256": sha256_file(args.workload_manifest),
                "tus": tus}
    workload_digest = immutable_json(run_root / "workload_manifest.json", workload)
    orders = (args.order,) if args.order != "balanced" else s5_statistics.ORDERS
    plan = schedule(args.blocks, orders) if args.blocks else []
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
        "warm_reset": "mode-private lifecycle; fixed complete manifest prewarmed outside timing with same scheduler epoch/C_GUID/F store and increasing TU_SEQ",
        "measurement_boundary": "reference preparation and warm prebuild outside; timed interval is one complete remote full-build aggregate",
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
                rows.append(run_build(args, run_root, block, mode, tus, archive_b64))
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
