#!/usr/bin/env python3
"""
Pinned, fail-closed TLC/TLAPS acceptance runner for the assignment-fence work.

The runner downloads nothing.  Every executable/jar is supplied explicitly,
hashed locally, and compared with an expected SHA-256.  It rejects parser or
runtime failures, zero-state/depth-truncated runs, wrong-property mutants,
parallel liveness, hidden constraints, missing deadlock policy, missing traces,
and counterexamples whose essential event sequence is not validated by
trace_to_harness.py.

This script records evidence; it does not weaken a failed model or proof into a
passing result.  The generated summary is suitable for retention under an
immutable run directory and for quoting in issue #4.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


SCHEMA_VERSION = 1


class FormalRunError(RuntimeError):
    """A run failed an acceptance precondition or expected result."""


@dataclass(frozen=True)
class Toolchain:
    name: str
    jar: Path
    sha256: str


@dataclass
class CommandResult:
    command: list[str]
    returncode: int
    elapsed_seconds: float
    timed_out: bool
    log_path: Path
    metrics_path: Path
    peak_rss_kib: int | None


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as exc:
        raise FormalRunError(f"cannot hash {path}: {exc}") from exc
    return digest.hexdigest()


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise FormalRunError(f"cannot read {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise FormalRunError(
            f"{path}: invalid JSON at line {exc.lineno}, column {exc.colno}: {exc.msg}"
        ) from exc


def write_json(path: Path, value: Any) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except OSError as exc:
        raise FormalRunError(f"cannot write {path}: {exc}") from exc


def exact_sha(path: Path, expected: str, label: str) -> str:
    actual = sha256_file(path)
    if not re.fullmatch(r"[0-9a-fA-F]{64}", expected or ""):
        raise FormalRunError(f"{label}: expected SHA-256 must be exactly 64 hex digits")
    if actual.lower() != expected.lower():
        raise FormalRunError(
            f"{label}: SHA-256 mismatch: expected {expected.lower()}, got {actual.lower()}"
        )
    return actual.lower()


def run_capture(command: Sequence[str], cwd: Path, timeout: int = 30) -> dict[str, Any]:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            list(command),
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise FormalRunError(f"cannot execute {shlex.join(command)}: {exc}") from exc
    return {
        "command": list(command),
        "returncode": completed.returncode,
        "elapsed_seconds": time.monotonic() - started,
        "output": completed.stdout,
    }


def parse_time_metrics(path: Path) -> int | None:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    match = re.search(r"Maximum resident set size \(kbytes\):\s*([0-9]+)", text)
    return int(match.group(1)) if match else None


def run_logged(
    command: Sequence[str],
    *,
    cwd: Path,
    log_path: Path,
    metrics_path: Path,
    time_bin: Path,
    timeout_seconds: int,
) -> CommandResult:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    metrics_path.parent.mkdir(parents=True, exist_ok=True)
    wrapped = [str(time_bin), "-v", "-o", str(metrics_path), *map(str, command)]
    started = time.monotonic()
    timed_out = False
    try:
        with log_path.open("wb") as log:
            proc = subprocess.Popen(
                wrapped,
                cwd=str(cwd),
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                returncode = proc.wait(timeout=timeout_seconds)
            except subprocess.TimeoutExpired:
                timed_out = True
                os.killpg(proc.pid, signal.SIGTERM)
                try:
                    returncode = proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    returncode = proc.wait()
    except OSError as exc:
        raise FormalRunError(f"cannot run {shlex.join(wrapped)}: {exc}") from exc
    return CommandResult(
        command=wrapped,
        returncode=returncode,
        elapsed_seconds=time.monotonic() - started,
        timed_out=timed_out,
        log_path=log_path,
        metrics_path=metrics_path,
        peak_rss_kib=parse_time_metrics(metrics_path),
    )


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise FormalRunError(f"cannot read {path}: {exc}") from exc


def parse_tlc_stats(log: str) -> dict[str, Any]:
    generated = distinct = left = depth = None
    for match in re.finditer(
        r"([0-9][0-9,]*) states generated,\s*"
        r"([0-9][0-9,]*) distinct states found"
        r"(?:,\s*([0-9][0-9,]*) states left on queue)?",
        log,
        re.IGNORECASE,
    ):
        generated = int(match.group(1).replace(",", ""))
        distinct = int(match.group(2).replace(",", ""))
        left = int(match.group(3).replace(",", "")) if match.group(3) else None
    for match in re.finditer(
        r"depth of the complete state graph search is\s+([0-9][0-9,]*)",
        log,
        re.IGNORECASE,
    ):
        depth = int(match.group(1).replace(",", ""))
    fingerprint = None
    fp_match = re.search(
        r"(?:fingerprint|FP)\s*(?:seed|index|polynomial)?\s*[:=]\s*([^\r\n]+)",
        log,
        re.IGNORECASE,
    )
    if fp_match:
        fingerprint = fp_match.group(1).strip()
    return {
        "generated_states": generated,
        "distinct_states": distinct,
        "states_left_on_queue": left,
        "depth": depth,
        "fingerprint": fingerprint,
    }


_FATAL_LOG_PATTERNS = [
    r"Parsing or semantic analysis failed",
    r"SANY Error",
    r"TLC threw an unexpected exception",
    r"Exception in thread",
    r"java\.lang\.[A-Za-z]+(?:Exception|Error)",
    r"The error occurred while TLC was evaluating",
    r"Error: TLC was unable",
    r"OutOfMemoryError",
    r"StackOverflowError",
]


def reject_runtime_or_parser_failure(log: str) -> None:
    for pattern in _FATAL_LOG_PATTERNS:
        if re.search(pattern, log, re.IGNORECASE):
            raise FormalRunError(f"TLC parser/runtime failure matched {pattern!r}")


def config_contract(
    config_path: Path,
    *,
    expected_property: str | None,
    expected: str,
    kind: str,
    allow_symmetry: bool,
    declared_assumptions: Sequence[str],
) -> dict[str, Any]:
    text = read_text(config_path)
    if not re.search(r"^\s*CHECK_DEADLOCK\s+(?:TRUE|FALSE)\s*$", text, re.MULTILINE):
        raise FormalRunError(f"{config_path}: CHECK_DEADLOCK must be explicit")
    if re.search(r"^\s*(?:CONSTRAINT|ACTION_CONSTRAINT)\b", text, re.MULTILINE):
        if "state_or_action_constraint" not in declared_assumptions:
            raise FormalRunError(
                f"{config_path}: hidden state/action constraint is not an accepted theorem assumption"
            )
    symmetry = bool(re.search(r"^\s*SYMMETRY\b", text, re.MULTILINE))
    if symmetry and (kind == "liveness" or not allow_symmetry):
        raise FormalRunError(f"{config_path}: symmetry is disallowed for this acceptance run")

    invariants = re.findall(r"^\s*INVARIANT\s+([A-Za-z_][A-Za-z0-9_]*)", text, re.MULTILINE)
    properties = re.findall(r"^\s*PROPERTY\s+([A-Za-z_][A-Za-z0-9_]*)", text, re.MULTILINE)
    directly_checked = invariants + properties
    if expected == "counterexample":
        if not expected_property:
            raise FormalRunError(f"{config_path}: counterexample check requires a named property")
        if directly_checked != [expected_property]:
            raise FormalRunError(
                f"{config_path}: expected-counterexample config must check exactly "
                f"{expected_property!r}, found {directly_checked!r}"
            )
    return {
        "sha256": sha256_file(config_path),
        "invariants": invariants,
        "properties": properties,
        "check_deadlock": re.search(
            r"^\s*CHECK_DEADLOCK\s+(TRUE|FALSE)\s*$", text, re.MULTILINE
        ).group(1),
        "symmetry": symmetry,
        "declared_assumptions": list(declared_assumptions),
    }


def expected_counterexample_seen(log: str, property_name: str) -> bool:
    patterns = [
        rf"Invariant\s+{re.escape(property_name)}\s+is violated",
        rf"Property\s+{re.escape(property_name)}\s+is violated",
        rf"{re.escape(property_name)}[^\r\n]*(?:violat|fail)",
    ]
    return any(re.search(pattern, log, re.IGNORECASE) for pattern in patterns)


def import_trace_adapter(formal_dir: Path):
    import importlib.util

    path = formal_dir / "trace_to_harness.py"
    spec = importlib.util.spec_from_file_location("trace_to_harness", path)
    if spec is None or spec.loader is None:
        raise FormalRunError(f"cannot import trace adapter from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def validate_trace_json_exists(path: Path) -> int:
    document = load_json(path)
    states: Any = document
    if isinstance(document, dict):
        for key in ("states", "trace", "counterexample"):
            if key in document:
                states = document[key]
                break
        if isinstance(states, dict):
            states = states.get("states", states)
    if not isinstance(states, list) or len(states) < 2:
        raise FormalRunError(f"{path}: counterexample JSON must contain at least two states")
    return len(states)


def validate_tlc_result(
    *,
    check: Mapping[str, Any],
    command_result: CommandResult,
    trace_path: Path,
    formal_dir: Path,
    run_dir: Path,
) -> dict[str, Any]:
    log = read_text(command_result.log_path)
    if command_result.timed_out:
        raise FormalRunError(f"{check['id']}: TLC exceeded its whole-run timeout")
    reject_runtime_or_parser_failure(log)
    stats = parse_tlc_stats(log)
    if not stats["generated_states"] or not stats["distinct_states"]:
        raise FormalRunError(f"{check['id']}: zero or missing generated/distinct state count")
    if stats["states_left_on_queue"] not in (None, 0):
        raise FormalRunError(f"{check['id']}: state-space run ended with states left on queue")

    expected = check["expected"]
    property_name = check.get("property")
    adapter_result = None
    if expected == "pass":
        if command_result.returncode != 0:
            raise FormalRunError(
                f"{check['id']}: fixed check exited {command_result.returncode}, expected 0"
            )
        if not re.search(r"No error has been found", log, re.IGNORECASE):
            raise FormalRunError(f"{check['id']}: TLC did not report a completed no-error run")
        if re.search(r"violated|counterexample", log, re.IGNORECASE):
            raise FormalRunError(f"{check['id']}: fixed run contains a violation/counterexample")
    elif expected == "counterexample":
        if command_result.returncode == 0:
            raise FormalRunError(f"{check['id']}: mutant unexpectedly exited 0")
        if not property_name or not expected_counterexample_seen(log, property_name):
            raise FormalRunError(
                f"{check['id']}: expected direct violation of {property_name!r} was not reported"
            )
        state_count = validate_trace_json_exists(trace_path)
        manifest_rel = check.get("trace_manifest")
        if not manifest_rel:
            raise FormalRunError(
                f"{check['id']}: counterexample lacks a trace_to_harness manifest"
            )
        manifest_path = formal_dir / manifest_rel
        adapter = import_trace_adapter(formal_dir)
        try:
            adapter_result = adapter.validate_trace(
                trace_path,
                manifest_path,
                tlc_log_path=command_result.log_path,
            )
        except Exception as exc:  # adapter raises its own fail-closed type
            raise FormalRunError(f"{check['id']}: trace adapter rejected the trace: {exc}") from exc
        if adapter_result["trace"]["state_count"] != state_count:
            raise FormalRunError(f"{check['id']}: adapter state count disagrees with trace")
        write_json(run_dir / "harness.json", adapter_result)
    else:
        raise FormalRunError(f"{check['id']}: unknown expected result {expected!r}")

    return {
        "returncode": command_result.returncode,
        "timed_out": command_result.timed_out,
        "elapsed_seconds": command_result.elapsed_seconds,
        "peak_rss_kib": command_result.peak_rss_kib,
        "stats": stats,
        "log_sha256": sha256_file(command_result.log_path),
        "metrics_sha256": sha256_file(command_result.metrics_path),
        "trace_sha256": sha256_file(trace_path) if trace_path.exists() else None,
        "trace_adapter": adapter_result,
    }


def git_identity(repo: Path, expected_sha: str) -> dict[str, Any]:
    rev = run_capture(["git", "rev-parse", "HEAD"], repo)
    if rev["returncode"] != 0:
        raise FormalRunError(f"git rev-parse failed: {rev['output']}")
    actual = rev["output"].strip()
    if actual != expected_sha:
        raise FormalRunError(f"git head mismatch: expected {expected_sha}, got {actual}")
    status = run_capture(["git", "status", "--porcelain"], repo)
    if status["returncode"] != 0:
        raise FormalRunError(f"git status failed: {status['output']}")
    if status["output"].strip():
        raise FormalRunError("formal acceptance checkout is not clean")
    return {"head": actual, "clean": True}


def validate_manifest(document: Any) -> dict[str, Any]:
    if not isinstance(document, dict) or document.get("schema") != SCHEMA_VERSION:
        raise FormalRunError(f"formal manifest schema must be {SCHEMA_VERSION}")
    checks = document.get("checks")
    proofs = document.get("proofs", [])
    if not isinstance(checks, list) or not checks:
        raise FormalRunError("formal manifest requires a non-empty checks array")
    if not isinstance(proofs, list):
        raise FormalRunError("proofs must be an array")
    ids: list[str] = []
    for item in [*checks, *proofs]:
        if not isinstance(item, dict) or not isinstance(item.get("id"), str):
            raise FormalRunError("every check/proof requires a string id")
        ids.append(item["id"])
    if len(ids) != len(set(ids)):
        raise FormalRunError("formal check/proof ids must be unique")
    return document


def run_tlc_check(
    *,
    check: Mapping[str, Any],
    toolchain: Toolchain,
    java: str,
    formal_dir: Path,
    artifacts: Path,
    time_bin: Path,
) -> dict[str, Any]:
    kind = check.get("kind", "safety")
    workers = int(check.get("workers", 1))
    if kind == "liveness" and workers != 1:
        raise FormalRunError(f"{check['id']}: authoritative liveness must use one worker")
    if workers < 1:
        raise FormalRunError(f"{check['id']}: workers must be >= 1")

    config_path = formal_dir / check["config"]
    module_path = formal_dir / f"{check['module']}.tla"
    if not config_path.is_file() or not module_path.is_file():
        raise FormalRunError(
            f"{check['id']}: missing module/config {module_path} / {config_path}"
        )
    contract = config_contract(
        config_path,
        expected_property=check.get("property"),
        expected=check["expected"],
        kind=kind,
        allow_symmetry=bool(check.get("allow_symmetry", False)),
        declared_assumptions=check.get("assumptions", []),
    )

    run_dir = artifacts / check["id"] / toolchain.name
    run_dir.mkdir(parents=True, exist_ok=True)
    trace_path = run_dir / "trace.json"
    log_path = run_dir / "tlc.log"
    metrics_path = run_dir / "time.txt"
    metadir = run_dir / "states"
    metadir.mkdir(parents=True, exist_ok=True)
    command = [
        java,
        "-jar",
        str(toolchain.jar),
        "-workers",
        str(workers),
        "-metadir",
        str(metadir),
        "-config",
        check["config"],
        "-dumpTrace",
        "json",
        str(trace_path),
        check["module"],
    ]
    result = run_logged(
        command,
        cwd=formal_dir,
        log_path=log_path,
        metrics_path=metrics_path,
        time_bin=time_bin,
        timeout_seconds=int(check.get("timeout_seconds", 1800)),
    )
    validated = validate_tlc_result(
        check=check,
        command_result=result,
        trace_path=trace_path,
        formal_dir=formal_dir,
        run_dir=run_dir,
    )
    record = {
        "id": check["id"],
        "toolchain": toolchain.name,
        "tool_sha256": toolchain.sha256,
        "module": check["module"],
        "module_sha256": sha256_file(module_path),
        "config": check["config"],
        "config_contract": contract,
        "kind": kind,
        "workers": workers,
        "expected": check["expected"],
        "property": check.get("property"),
        "command": result.command,
        **validated,
    }
    write_json(run_dir / "result.json", record)
    return record


def run_tlaps_proof(
    *,
    proof: Mapping[str, Any],
    tlapm: Path,
    formal_dir: Path,
    artifacts: Path,
    time_bin: Path,
) -> dict[str, Any]:
    proof_path = formal_dir / proof["file"]
    if not proof_path.is_file():
        raise FormalRunError(f"{proof['id']}: missing proof file {proof_path}")
    run_dir = artifacts / proof["id"] / "tlaps"
    log_path = run_dir / "tlapm.log"
    metrics_path = run_dir / "time.txt"
    command = [str(tlapm), str(proof_path)]
    result = run_logged(
        command,
        cwd=formal_dir,
        log_path=log_path,
        metrics_path=metrics_path,
        time_bin=time_bin,
        timeout_seconds=int(proof.get("timeout_seconds", 1800)),
    )
    log = read_text(log_path)
    if result.timed_out or result.returncode != 0:
        raise FormalRunError(
            f"{proof['id']}: tlapm failed/timeout, exit {result.returncode}"
        )
    if re.search(r"\b(?:error|failed|unproved|omitted|sorry)\b", log, re.IGNORECASE):
        raise FormalRunError(f"{proof['id']}: proof log contains an error/unproved marker")
    if not re.search(r"(?:All obligations proved|obligations? proved)", log, re.IGNORECASE):
        raise FormalRunError(
            f"{proof['id']}: tlapm did not report that all obligations were proved"
        )
    record = {
        "id": proof["id"],
        "file": proof["file"],
        "file_sha256": sha256_file(proof_path),
        "command": result.command,
        "returncode": result.returncode,
        "elapsed_seconds": result.elapsed_seconds,
        "peak_rss_kib": result.peak_rss_kib,
        "log_sha256": sha256_file(log_path),
        "metrics_sha256": sha256_file(metrics_path),
    }
    write_json(run_dir / "result.json", record)
    return record


def select(items: Iterable[Mapping[str, Any]], patterns: Sequence[str]) -> list[Mapping[str, Any]]:
    values = list(items)
    if not patterns:
        return values
    selected = [item for item in values if any(re.fullmatch(p, item["id"]) for p in patterns)]
    if not selected:
        raise FormalRunError(f"--only patterns selected no checks: {patterns}")
    return selected


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--formal-dir", type=Path)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--expected-git-sha", required=True)
    parser.add_argument("--java", default="java")
    parser.add_argument("--stable-jar", type=Path, required=True)
    parser.add_argument("--stable-sha256", required=True)
    parser.add_argument("--differential-jar", type=Path, required=True)
    parser.add_argument("--differential-sha256", required=True)
    parser.add_argument("--tlapm", type=Path, required=True)
    parser.add_argument("--tlapm-sha256", required=True)
    parser.add_argument("--time-bin", type=Path, default=Path("/usr/bin/time"))
    parser.add_argument(
        "--only",
        action="append",
        default=[],
        metavar="REGEX",
        help="run only check/proof ids matching a full regular expression",
    )
    parser.add_argument("--skip-proofs", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        repo = args.repo.resolve()
        formal_dir = (args.formal_dir or repo / "formal").resolve()
        artifacts = args.artifacts.resolve()
        if artifacts.exists() and any(artifacts.iterdir()):
            raise FormalRunError(f"artifact directory must be absent or empty: {artifacts}")
        artifacts.mkdir(parents=True, exist_ok=True)
        if not args.time_bin.is_file():
            raise FormalRunError(
                f"GNU time is required for peak RSS evidence: {args.time_bin}"
            )
        manifest = validate_manifest(load_json(args.manifest))
        git = git_identity(repo, args.expected_git_sha)

        stable = Toolchain(
            "stable",
            args.stable_jar.resolve(),
            exact_sha(args.stable_jar.resolve(), args.stable_sha256, "stable TLC"),
        )
        differential = Toolchain(
            "differential",
            args.differential_jar.resolve(),
            exact_sha(
                args.differential_jar.resolve(),
                args.differential_sha256,
                "differential TLC",
            ),
        )
        tlapm_sha = exact_sha(args.tlapm.resolve(), args.tlapm_sha256, "tlapm")
        java_version = run_capture([args.java, "-version"], repo)
        tlapm_version = run_capture([str(args.tlapm.resolve()), "--version"], repo)
        if java_version["returncode"] != 0 or tlapm_version["returncode"] != 0:
            raise FormalRunError("Java or tlapm version command failed")

        run_metadata = {
            "schema": SCHEMA_VERSION,
            "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "git": git,
            "manifest": {
                "path": str(args.manifest.resolve()),
                "sha256": sha256_file(args.manifest.resolve()),
            },
            "java": java_version,
            "toolchains": {
                "stable": {"path": str(stable.jar), "sha256": stable.sha256},
                "differential": {
                    "path": str(differential.jar),
                    "sha256": differential.sha256,
                },
                "tlapm": {
                    "path": str(args.tlapm.resolve()),
                    "sha256": tlapm_sha,
                    "version": tlapm_version,
                },
            },
        }
        write_json(artifacts / "run-metadata.json", run_metadata)

        checks = select(manifest["checks"], args.only)
        proofs = [] if args.skip_proofs else select(manifest.get("proofs", []), args.only)
        toolchains = {"stable": stable, "differential": differential}
        results: list[dict[str, Any]] = []
        for check in checks:
            names = check.get("toolchains", ["stable", "differential"])
            if not isinstance(names, list) or not names:
                raise FormalRunError(f"{check['id']}: toolchains must be a non-empty array")
            for name in names:
                if name not in toolchains:
                    raise FormalRunError(f"{check['id']}: unknown toolchain {name!r}")
                results.append(
                    run_tlc_check(
                        check=check,
                        toolchain=toolchains[name],
                        java=args.java,
                        formal_dir=formal_dir,
                        artifacts=artifacts,
                        time_bin=args.time_bin.resolve(),
                    )
                )

        proof_results: list[dict[str, Any]] = []
        for proof in proofs:
            proof_results.append(
                run_tlaps_proof(
                    proof=proof,
                    tlapm=args.tlapm.resolve(),
                    formal_dir=formal_dir,
                    artifacts=artifacts,
                    time_bin=args.time_bin.resolve(),
                )
            )

        summary = {
            "schema": SCHEMA_VERSION,
            "status": "PASS",
            "finished_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "git": git,
            "tlc_results": results,
            "tlaps_results": proof_results,
        }
        write_json(artifacts / "summary.json", summary)
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except FormalRunError as exc:
        print(f"formal acceptance failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
