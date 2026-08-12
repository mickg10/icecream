#!/usr/bin/env python3
"""Local, digest-pinned SANY/TLC runner for formal/G4Lifecycle.tla.

The runner never downloads tools and never uses a hosted workflow.  It retains
exact configs, commands, complete logs, state counts, depth, queue-at-end,
elapsed time, and source/tool hashes.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

PINNED = {
    "1.7.4": "936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88",
    "1.8.0": "ab323b79802aedc3203b3f9af37c6aca3ed43f4e0225b36f2aa77b26de46c05f",
}
MUTANTS = (
    "MutantLegacyNeedsConf", "MutantStartBeforeBegin",
    "MutantPartialCleanup", "MutantUnboundedBatch",
    "MutantIdFirstPriority", "MutantAllowSecondBound",
    "MutantStaleDecision", "MutantCompleteWhileDisconnected",
    "MutantCapacityLE", "MutantDuplicateTerminal",
)


@dataclass(frozen=True)
class Row:
    name: str
    spec: str
    constants: dict[str, Any]
    invariants: tuple[str, ...]
    properties: tuple[str, ...] = ()
    violation: str | None = None
    default: bool = True


def constants(protocol: int = 48, clients: int = 1, decisions: int = 1,
              capacity: int = 1, max_batch: int | None = None,
              **enabled: bool) -> dict[str, Any]:
    result: dict[str, Any] = {
        "Protocol": protocol,
        "ClientCount": clients,
        "DecisionCount": decisions,
        "MaxBatch": decisions if max_batch is None else max_batch,
        "Capacity": capacity,
        "MaxGeneration": 2,
        "MaxRequestGeneration": 2,
    }
    result.update({name: bool(enabled.get(name, False)) for name in MUTANTS})
    unknown = set(enabled) - set(MUTANTS)
    if unknown:
        raise ValueError(f"unknown mutants: {sorted(unknown)}")
    return result


ROWS = (
    Row("fixed-legacy-c1d1-cap1", "FairSpec", constants(protocol=23),
        ("CoreSafety",), ("LegacyActivationProgress", "LossCleanupProgress")),
    Row("fixed-modern-c1d1-cap1", "FairSpec", constants(),
        ("CoreSafety",), ("ConfActivationProgress", "LossCleanupProgress")),
    Row("fixed-modern-c2d2-cap1", "FairSpec", constants(clients=2, decisions=2),
        ("CoreSafety",), ("ConfActivationProgress", "LossCleanupProgress")),
    Row("fixed-modern-c3d3-cap2", "SafetySpec",
        constants(clients=3, decisions=3, capacity=2), ("CoreSafety",), default=False),
    Row("witness-reconnect-generation2", "SafetySpec", constants(),
        ("NeverSecondGeneration",), violation="NeverSecondGeneration"),
    Row("mutant-legacy-needs-conf", "FairSpec",
        constants(protocol=23, MutantLegacyNeedsConf=True), ("CoreSafety",),
        ("LegacyActivationProgress",), "LegacyActivationProgress"),
    Row("mutant-start-before-begin", "SafetySpec",
        constants(MutantStartBeforeBegin=True), ("StartedAfterBegin",),
        violation="StartedAfterBegin"),
    Row("mutant-partial-cleanup", "SafetySpec",
        constants(decisions=2, MutantPartialCleanup=True),
        ("DisconnectedClean", "RequestClosedShape"),
        violation="DisconnectedClean|RequestClosedShape"),
    Row("mutant-unbounded-batch", "SafetySpec",
        constants(max_batch=1, MutantUnboundedBatch=True), ("BatchBound",),
        violation="BatchBound"),
    Row("mutant-id-first-priority", "SafetySpec",
        constants(clients=2, MutantIdFirstPriority=True), ("PriorityMinimal",),
        violation="PriorityMinimal"),
    Row("mutant-second-bound", "SafetySpec",
        constants(decisions=2, capacity=2, MutantAllowSecondBound=True),
        ("OneBoundLocal",), violation="OneBoundLocal"),
    Row("mutant-stale-decision", "SafetySpec",
        constants(MutantStaleDecision=True), ("OwnerEvidence", "LiveOwnedCurrent"),
        violation="OwnerEvidence|LiveOwnedCurrent"),
    Row("mutant-complete-disconnected", "SafetySpec",
        constants(MutantCompleteWhileDisconnected=True), ("CompletionAuthority",),
        violation="CompletionAuthority"),
    Row("mutant-capacity-le", "SafetySpec",
        constants(decisions=2, capacity=1, MutantCapacityLE=True),
        ("CapacityBound",), violation="CapacityBound"),
    Row("mutant-duplicate-terminal", "SafetySpec",
        constants(MutantDuplicateTerminal=True), ("TerminalUniqueness",),
        violation="TerminalUniqueness"),
)


class Failure(RuntimeError):
    pass


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def git(repo: Path, *args: str) -> str:
    proc = subprocess.run(["git", *args], cwd=repo, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode:
        raise Failure(proc.stderr.strip() or f"git {' '.join(args)} failed")
    return proc.stdout.strip()


def static_check(model: Path) -> dict[str, Any]:
    text = model.read_text(encoding="utf-8")
    required = ("MODULE G4Lifecycle", "SafetySpec ==", "FairSpec ==",
                "ConfArrives ==", "HandleConf ==", "CleanupLoss ==",
                "OneBoundLocal ==", "NeverSecondGeneration ==") + MUTANTS
    missing = [item for item in required if item not in text]
    if missing:
        raise Failure(f"model preflight missing: {missing}")
    if not text.rstrip().endswith("============================================================================="):
        raise Failure("model terminator missing")
    return {"lines": len(text.splitlines()), "sha256": digest(model)}


def tla(value: Any) -> str:
    if isinstance(value, bool):
        return "TRUE" if value else "FALSE"
    if isinstance(value, int):
        return str(value)
    raise TypeError(value)


def config(row: Row) -> str:
    lines = [f"SPECIFICATION {row.spec}", "", "CONSTANTS"]
    lines += [f"  {name} = {tla(value)}" for name, value in row.constants.items()]
    lines += ["", "CHECK_DEADLOCK TRUE"]
    lines += [f"INVARIANT {name}" for name in row.invariants]
    lines += [f"PROPERTY {name}" for name in row.properties]
    return "\n".join(lines) + "\n"


def parse_jar(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("--jar requires LABEL=/absolute/path")
    label, path = value.split("=", 1)
    return label, Path(path).expanduser().resolve()


def jars(values: list[tuple[str, Path]]) -> dict[str, Path]:
    found = dict(values)
    for label, env in (("1.7.4", "TLA2TOOLS_174"), ("1.8.0", "TLA2TOOLS_180")):
        if env in os.environ and label not in found:
            found[label] = Path(os.environ[env]).expanduser().resolve()
    if not found:
        raise Failure("no pinned tla2tools jar supplied; set TLA2TOOLS_174 and/or "
                      "TLA2TOOLS_180, or pass --jar LABEL=/path; no downloads occur")
    for label, path in found.items():
        if label not in PINNED:
            raise Failure(f"unrecognized tool label {label}")
        if not path.is_file():
            raise Failure(f"{label}: jar not found: {path}")
        actual = digest(path)
        if actual != PINNED[label]:
            raise Failure(f"{label}: SHA-256 {actual} != pinned {PINNED[label]}")
    return found


def parse(log: str) -> dict[str, Any]:
    def number(pattern: str) -> int | None:
        match = re.search(pattern, log)
        return int(match.group(1).replace(",", "")) if match else None
    return {
        "generated": number(r"([0-9][0-9,]*) states generated"),
        "distinct": number(r"([0-9][0-9,]*) distinct states found"),
        "queue_at_end": number(r"([0-9][0-9,]*) states left on queue"),
        "depth": number(r"depth of the complete state graph search is ([0-9]+)"),
        "clean": "Model checking completed. No error has been found." in log,
    }


def command(cmd: list[str], cwd: Path, log: Path, timeout: int,
            timing: Path | None = None) -> tuple[int, float]:
    full = cmd
    if timing and Path("/usr/bin/time").is_file():
        full = ["/usr/bin/time", "-v", "-o", str(timing), *cmd]
    start = time.monotonic()
    try:
        with log.open("w", encoding="utf-8") as output:
            proc = subprocess.run(full, cwd=cwd, stdout=output,
                                  stderr=subprocess.STDOUT, timeout=timeout)
        return proc.returncode, time.monotonic() - start
    except subprocess.TimeoutExpired:
        with log.open("a", encoding="utf-8") as output:
            output.write(f"\nRUNNER_TIMEOUT={timeout}\n")
        return 124, time.monotonic() - start


def selected(names: list[str], include_large: bool) -> list[Row]:
    mapping = {row.name: row for row in ROWS}
    if names:
        unknown = set(names) - set(mapping)
        if unknown:
            raise Failure(f"unknown rows: {sorted(unknown)}")
        return [mapping[name] for name in names]
    return [row for row in ROWS if row.default or include_large]


def run(repo: Path, tools: dict[str, Path], rows: list[Row], root: Path,
        timeout: int) -> dict[str, Any]:
    status = git(repo, "status", "--porcelain", "--untracked-files=all")
    if status:
        raise Failure("working tree is not clean:\n" + status)
    root.mkdir(parents=True, exist_ok=False)
    configs = root / "configs"
    configs.mkdir()
    model = repo / "formal" / "G4Lifecycle.tla"
    shutil.copy2(model, root / model.name)
    source = {"commit": git(repo, "rev-parse", "HEAD"),
              "model_sha256": digest(model),
              "runner_sha256": digest(Path(__file__).resolve())}
    (root / "source.json").write_text(json.dumps(source, indent=2) + "\n")
    tool_info = {label: {"path": str(path), "sha256": digest(path)}
                 for label, path in tools.items()}
    (root / "tools.json").write_text(json.dumps(tool_info, indent=2) + "\n")
    results: list[dict[str, Any]] = []
    ok = True
    for label, jar in tools.items():
        sany = root / f"SANY-{label}.log"
        rc, elapsed = command(["java", "-cp", str(jar), "tla2sany.SANY", str(model)],
                              repo, sany, timeout)
        if rc:
            raise Failure(f"SANY {label} failed; see {sany}")
        for row in rows:
            cfg = configs / f"{row.name}.cfg"
            cfg.write_text(config(row), encoding="utf-8")
            row_root = root / label / row.name
            row_root.mkdir(parents=True)
            log = row_root / "TLC.log"
            timing = row_root / "time.txt"
            cmd = ["java", "-jar", str(jar), "-workers", "1",
                   "-metadir", str(row_root / "states"),
                   "-config", str(cfg), "G4Lifecycle"]
            rc, elapsed = command(cmd, repo / "formal", log, timeout, timing)
            text = log.read_text(encoding="utf-8", errors="replace")
            stats = parse(text)
            if row.violation is None:
                semantic = rc == 0 and stats["clean"] and (stats["generated"] or 0) > 0 \
                           and stats["queue_at_end"] == 0
            else:
                semantic = rc != 0 and (stats["generated"] or 0) > 0 \
                           and re.search(row.violation, text) is not None \
                           and "Exception in thread" not in text
            result = {"tool": label, "row": row.name, "command": cmd,
                      "exit": rc, "elapsed_seconds": elapsed,
                      "expected_violation": row.violation,
                      "semantic_pass": semantic, "log": str(log),
                      "log_sha256": digest(log), "config_sha256": digest(cfg), **stats}
            (row_root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            results.append(result)
            ok = ok and semantic
    summary = {"schema": "icecream.g4.formal-run.v1", "source": source,
               "tools": tool_info, "result": "PASS" if ok else "FAIL",
               "results": results}
    (root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--jar", action="append", type=parse_jar, default=[])
    parser.add_argument("--row", action="append", default=[])
    parser.add_argument("--include-large", action="store_true")
    parser.add_argument("--artifact-root", type=Path)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    repo = args.repo.resolve()
    try:
        preflight = static_check(repo / "formal" / "G4Lifecycle.tla")
        rows = selected(args.row, args.include_large)
        if args.list or args.check_only:
            print(json.dumps({"result": "PASS", "model": preflight,
                              "rows": [row.name for row in rows],
                              "all_rows": [row.name for row in ROWS],
                              "pinned": PINNED}, indent=2))
            return 0
        tools = jars(args.jar)
        if args.artifact_root:
            root = args.artifact_root.resolve()
        else:
            stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            short = git(repo, "rev-parse", "--short=12", "HEAD")
            base = Path(os.environ.get("G4_FORMAL_ARTIFACT_ROOT", os.environ.get("TMPDIR", "/tmp")))
            root = base / "icecream-g4-formal" / f"{stamp}-{short}"
        summary = run(repo, tools, rows, root, args.timeout)
    except (Failure, OSError, ValueError) as exc:
        print(f"G4 FORMAL RUN FAILED: {exc}", file=sys.stderr)
        return 2
    print(json.dumps({"result": summary["result"], "artifact_root": str(root),
                      "row_results": len(summary["results"])}, sort_keys=True))
    return 0 if summary["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
