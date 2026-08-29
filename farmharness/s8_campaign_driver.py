#!/usr/bin/env python3
"""Execute and retain the S8 profile/regime/topology campaign.

The depth planner and multi-TU producer remain the authorities for planning and
simulation.  This module only supplies campaign orchestration: it creates one
immutable, second-granularity campaign directory, records the exact argv for
each cell, runs the predictive commands, and leaves authenticated commands for
the live and comparison stages.  A failed cell is never presented as a pass;
resume creates a new attempt directory and keeps every earlier attempt.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import stat
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

try:  # package invocation
    from .s8_schema import CORPORA, PROFILES, REGIMES, SPLITS
except ImportError:  # direct invocation
    from s8_schema import CORPORA, PROFILES, REGIMES, SPLITS


SCHEMA = "icecream-s8-campaign-driver-v2"
CELL_SCHEMA = "icecream-s8-campaign-cell-v2"
SUMMARY_SCHEMA = "icecream-s8-campaign-summary-v2"
DEPTHS = ("100", "200", "full")
TOPOLOGIES = ("C1F1/100000", "C1F20/40")
TOPOLOGY_ARGS = {"C1F1/100000": "C1F1", "C1F20/40": "C1F20"}
CAMPAIGN_STAMP = "%Y%m%dT%H%M%SZ"
STAMP_RE = re.compile(r"^\d{8}T\d{6}Z$")


class CampaignError(ValueError):
    """A campaign declaration, execution, or retained artifact is invalid."""


def canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def _stamp() -> str:
    return datetime.now(timezone.utc).strftime(CAMPAIGN_STAMP)


def _safe(value: str) -> str:
    return "".join(ch if ch.isalnum() or ch in "_.-" else "-" for ch in value)


def _cell_id(cell: dict[str, str]) -> str:
    return "/".join((cell["corpus"], cell["profile"], cell["regime"], cell["topology"]))


def _slug(cell: dict[str, str]) -> str:
    return _safe("-".join((cell["corpus"], cell["profile"], cell["regime"], cell["topology"])))


def _validate_cell(cell: dict[str, str]) -> None:
    if (set(cell) != {"corpus", "profile", "regime", "topology"} or
            cell["corpus"] not in CORPORA or cell["profile"] not in PROFILES or
            cell["regime"] not in REGIMES or cell["topology"] not in TOPOLOGIES):
        raise CampaignError("cell:undeclared")


def cells(corpus: str) -> list[dict[str, str]]:
    if corpus not in CORPORA:
        raise CampaignError("corpus:undeclared")
    return [{"corpus": corpus, "profile": profile, "regime": regime,
             "topology": topology}
            for profile in PROFILES for regime in REGIMES for topology in TOPOLOGIES]


def _format_path(spec: str | Path, cell: dict[str, str]) -> Path:
    values = {**cell, "topology_arg": TOPOLOGY_ARGS[cell["topology"]],
              "cell": _cell_id(cell)}
    try:
        return Path(str(spec).format(**values)).expanduser().absolute()
    except (KeyError, ValueError) as exc:
        raise CampaignError(f"path_template:invalid:{spec}") from exc


def _write_new(path: Path, raw: bytes) -> None:
    if path.exists() or path.is_symlink():
        raise CampaignError(f"output:already_exists:{path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open("xb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as exc:
        raise CampaignError(f"output:write_failed:{path}") from exc


def _replace_json(path: Path, value: object) -> None:
    """Atomically update mutable campaign state without touching old attempts."""
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    try:
        with temporary.open("xb") as stream:
            stream.write(canonical(value))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except OSError as exc:
        try:
            temporary.unlink()
        except OSError:
            pass
        raise CampaignError(f"state:write_failed:{path}") from exc


def _sha(path: Path) -> dict[str, object]:
    try:
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise CampaignError(f"artifact:not_private_regular_file:{path}")
        raw = path.read_bytes()
    except OSError as exc:
        raise CampaignError(f"artifact:unavailable:{path}") from exc
    return {"path": str(path), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def _artifact_tree(root: Path) -> list[dict[str, object]]:
    result = []
    if not root.is_dir() or root.is_symlink():
        raise CampaignError(f"artifact:result_directory_invalid:{root}")
    for path in sorted(root.rglob("*")):
        if path.is_file() and not path.is_symlink():
            item = _sha(path)
            item["path"] = str(path.relative_to(root))
            result.append(item)
    return result


def _git_identity(repo: Path) -> dict[str, str]:
    try:
        head = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"],
                                       text=True, stderr=subprocess.DEVNULL).strip()
        tree = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD^{tree}"],
                                       text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.SubprocessError) as exc:
        raise CampaignError("git:identity_unavailable") from exc
    return {"root": str(repo), "head": head, "tree": tree}


def _command_record(argv: list[str], cwd: Path, *, stage: str, executable: bool = True,
                    reason: str | None = None) -> dict[str, object]:
    value: dict[str, object] = {
        "stage": stage, "argv": [str(item) for item in argv],
        "shell": shlex.join(str(item) for item in argv), "cwd": str(cwd),
        "executable": executable,
    }
    if reason is not None:
        value["reason"] = reason
    value["sha256"] = hashlib.sha256(canonical(value)).hexdigest()
    return value


def _source_commands(cell_dir: Path, cell: dict[str, str], *, depth: str,
                     source_manifest: Path, source_root: Path, matrix_audit: Path,
                     engine_manifest: Path, product_root: Path, python: str,
                     campaign_stamp: str, compile_db: Path | None,
                     compile_source_root: Path | None, compile_output_root: Path | None,
                     repo: Path) -> tuple[list[dict[str, object]], dict[str, object], dict[str, object], dict[str, object], list[Path]]:
    attempt = cell_dir / "attempt-001"
    result_dir = attempt / f"s8-{_slug(cell)}-{campaign_stamp}"
    plan = attempt / ("depth-plan-full-1.json" if depth == "full" else "depth-plan.json")
    depth_argv = [python, str((repo / "farmharness/s8_depth_runner.py").absolute()),
                  "--source-manifest", str(source_manifest), "--source-root", str(source_root),
                  "--matrix-audit", str(matrix_audit), "--result-dir", str(result_dir),
                  "--corpus", cell["corpus"], "--profile", cell["profile"],
                  "--regime", cell["regime"], "--depth", depth,
                  "--topology", TOPOLOGY_ARGS[cell["topology"]], "--out", str(plan)]
    producer_argv = [python, str((repo / "farmharness/s8_multitu_predictive_producer.py").absolute()),
                     "--plan", str(plan), "--engine-manifest", str(engine_manifest),
                     "--product-build-root", str(product_root)]
    plan_commands: list[dict[str, object]] = []
    result_dirs = [result_dir]
    paired_full = depth == "full" and cell["regime"] == "cold"
    if depth == "full":
        repeat_plan = attempt / "depth-plan-full-2.json"
        repeat_result = attempt / f"s8-{_slug(cell)}-{campaign_stamp}-full-2"
        repeat_argv = [python, str((repo / "farmharness/s8_depth_runner.py").absolute()),
                       "--source-manifest", str(source_manifest), "--source-root", str(source_root),
                       "--matrix-audit", str(matrix_audit), "--result-dir", str(repeat_result),
                       "--corpus", cell["corpus"], "--profile", cell["profile"],
                       "--regime", cell["regime"], "--depth", "repeat-full",
                       "--repeat-of", str(plan), "--topology", TOPOLOGY_ARGS[cell["topology"]],
                       "--out", str(repeat_plan)]
        if paired_full:
            producer_argv.extend(["--repeat-plan", str(repeat_plan)])
            result_dirs.append(repeat_result)
        plan_commands.append(_command_record(repeat_argv, repo, stage="predictive_plan_full_2"))
    if depth != "full" or not paired_full:
        producer_argv.extend(("--output-dir", str(result_dir)))
    plan_commands.insert(0, _command_record(depth_argv, repo, stage="predictive_plan" if depth != "full" else "predictive_plan_full_1"))
    producer_command = _command_record(producer_argv, repo, stage="predictive_producer")

    # These are deliberately staged, not guessed.  A live run requires a
    # compile database and a retained source checkout that are not predictive
    # inputs.  Keeping their exact argv makes the later handoff auditable.
    prep_dir = attempt / "live-prep"
    compile_db_arg = str(compile_db) if compile_db else "<compile-db-required>"
    compile_src_arg = str(compile_source_root) if compile_source_root else "<compile-source-root-required>"
    compile_out_arg = str(compile_output_root) if compile_output_root else str(attempt / "compile-output")
    prep_argv = [python, str((repo / "farmharness/s8_live_batch_prep.py").absolute(),),
                 "--predictive-plan", str(plan), "--compile-db", compile_db_arg,
                 "--compile-source-root", compile_src_arg,
                 "--compile-output-root", compile_out_arg, "--output", str(prep_dir)]
    live_out = attempt / "live-output"
    live_argv = [python, str((repo / "farmharness/s8_real_c1f1_live_runner.py").absolute()),
                 "--batch-manifest", str(prep_dir / "batch-manifest.jsonl"),
                 "--predictive-plan", str(plan), "--topology", str(prep_dir / "topology.json"),
                 "--suite", cell["topology"], "--profile", cell["profile"],
                 "--product-root", str(product_root), "--corpus", cell["corpus"],
                 "--regime", cell["regime"], "--depth", depth, "--passes", "2" if depth == "full" else "1",
                 "--output", str(live_out),
                 "--execute"]
    comparison_argv = [python, str((repo / "farmharness/s8_predictive_live_normalizer.py").absolute()),
                       "--predictive-manifest", str(result_dir / "predictive_curve_manifest.json"),
                       "--live-manifest", str(live_out / "live-curve-manifest.json"),
                       "--out", str(attempt / "records.jsonl")]
    missing = []
    if compile_db is None:
        missing.append("compile-db")
    if compile_source_root is None:
        missing.append("compile-source-root")
    reason = "live inputs not configured: " + ", ".join(missing) if missing else "live execution staged by request"
    return (plan_commands, producer_command,
            {"prepare": _command_record(prep_argv, repo, stage="live_prepare", executable=False, reason=reason),
             "run": _command_record(live_argv, repo, stage="live_run", executable=False, reason=reason)},
            _command_record(comparison_argv, repo, stage="comparison", executable=False,
                            reason="requires authenticated live curve"), result_dirs)


def _run_command(command: dict[str, object], cwd: Path, stdout: Path, stderr: Path) -> int:
    argv = [str(item) for item in command["argv"]]  # type: ignore[index]
    stdout.parent.mkdir(parents=True, exist_ok=True)
    with stdout.open("ab") as out, stderr.open("ab") as err:
        try:
            completed = subprocess.run(argv, cwd=str(cwd), stdout=out, stderr=err,
                                       check=False, timeout=6 * 60 * 60)
        except subprocess.TimeoutExpired:
            err.write(b"campaign driver: command timed out after 21600 seconds\n")
            return 124
        except OSError as exc:
            err.write((f"campaign driver: unable to execute: {exc}\n").encode())
            return 127
    return int(completed.returncode)


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CampaignError(f"state:invalid_json:{path}") from exc
    if not isinstance(value, dict):
        raise CampaignError(f"state:object_required:{path}")
    return value


def _summary(campaign: Path, cell_records: list[dict[str, Any]], config: dict[str, object]) -> dict[str, object]:
    by_cell = {str(row.get("cell_id", row.get("cell"))): row for row in cell_records}
    complete_records: list[dict[str, Any]] = []
    for cell in cells(str(config["corpus"])):
        complete_records.append(by_cell.get(_cell_id(cell),
                                            {"cell": cell, "cell_id": _cell_id(cell),
                                             "status": "PENDING"}))
    statuses = {status: sum(1 for row in complete_records if row.get("status") == status)
                for status in ("PENDING", "RUNNING", "PASS", "FAIL", "INTERRUPTED", "STAGED")}
    if statuses["FAIL"] or statuses["INTERRUPTED"]:
        status = "PARTIAL_FAILURE"
    elif statuses["PENDING"] or statuses["RUNNING"]:
        status = "IN_PROGRESS"
    elif statuses["STAGED"]:
        status = "PARTIAL_STAGED"
    else:
        status = "PASS"
    return {"schema": SUMMARY_SCHEMA, "status": status, "campaign_root": str(campaign),
            "expected_cells": len(complete_records), "counts": statuses,
            "cells": [{"cell": row["cell"], "status": row["status"],
                       "attempt": row.get("attempt"), "result": row.get("result"),
                       "error": row.get("error")} for row in complete_records],
            "config_sha256": hashlib.sha256(canonical(config)).hexdigest()}


def _new_campaign_root(output_root: Path, stamp: str | None = None) -> Path:
    output_root = output_root.absolute()
    if output_root.exists() and (output_root.is_symlink() or not output_root.is_dir()):
        raise CampaignError("campaign_root:not_directory")
    output_root.mkdir(parents=True, exist_ok=True)
    stamp = stamp or _stamp()
    if STAMP_RE.fullmatch(stamp) is None:
        raise CampaignError("timestamp:must_be_second_granularity")
    for suffix in range(1000):
        name = f"s8-campaign-{stamp}" + (f"-{suffix:02d}" if suffix else "")
        candidate = output_root / name
        try:
            candidate.mkdir()
            (candidate / "cells").mkdir()
            return candidate
        except FileExistsError:
            continue
    raise CampaignError("campaign_root:timestamp_collisions_exhausted")


def _attempt(cell_dir: Path) -> int:
    numbers = []
    for path in cell_dir.glob("attempt-*"):
        try:
            numbers.append(int(path.name.split("-", 1)[1]))
        except (ValueError, IndexError):
            continue
    return max(numbers, default=0) + 1


def _cell_state(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"status": "PENDING", "history": []}
    value = _load_json(path)
    if value.get("schema") != CELL_SCHEMA:
        raise CampaignError(f"cell_state:schema_invalid:{path}")
    return value


def run_campaign(*, output_root: Path, repo: Path, corpus: str, depth: str,
                 source_manifest: str | Path, source_root: str | Path,
                 matrix_audit: Path, engine_manifest_template: str | Path,
                 product_build_root: Path, python: str | None = None,
                 resume: Path | None = None, retry_failed: bool = False,
                 compile_db: str | Path | None = None,
                 compile_source_root: str | Path | None = None,
                 compile_output_root: str | Path | None = None,
                 execute: bool = True,
                 mode: str = "predictive-only",
                 command_runner: Callable[[dict[str, object], Path, Path, Path], int] | None = None,
                 timestamp: str | None = None) -> Path:
    """Run a campaign, or resume it without rewriting prior attempts."""
    if depth not in DEPTHS:
        raise CampaignError("depth:undeclared")
    if mode not in ("predictive-only", "all"):
        raise CampaignError("mode:undeclared")
    if corpus not in CORPORA:
        raise CampaignError("corpus:undeclared")
    repo = repo.absolute()
    python = python or sys.executable
    source_manifest_spec, source_root_spec = str(source_manifest), str(source_root)
    matrix_audit = matrix_audit.absolute()
    product_build_root = product_build_root.absolute()
    compile_db_path = Path(compile_db).absolute() if compile_db else None
    compile_source_path = Path(compile_source_root).absolute() if compile_source_root else None
    compile_output_path = Path(compile_output_root).absolute() if compile_output_root else None
    config: dict[str, object] = {"corpus": corpus, "depth": depth,
        "mode": mode,
        "source_manifest": source_manifest_spec, "source_root": source_root_spec,
        "matrix_audit": str(matrix_audit), "engine_manifest_template": str(engine_manifest_template),
        "product_build_root": str(product_build_root), "python": python,
        "compile_db": str(compile_db_path) if compile_db_path else None,
        "compile_source_root": str(compile_source_path) if compile_source_path else None,
        "compile_output_root": str(compile_output_path) if compile_output_path else None,
        "dimensions": {"profiles": list(PROFILES), "regimes": list(REGIMES),
                       "topologies": list(TOPOLOGIES)}}
    if resume is None:
        requested_stamp = timestamp or _stamp()
        campaign = _new_campaign_root(output_root, requested_stamp)
        identity = _git_identity(repo)
        metadata = {"schema": SCHEMA, "status": "IN_PROGRESS", "created_utc": requested_stamp,
                    "campaign_root": str(campaign), "git": identity, "config": config,
                    "mode": mode if execute else "plan-only", "staged_stages": ["live", "comparison"],
                    "config_sha256": hashlib.sha256(canonical(config)).hexdigest(),
                    "matrix": {"corpus": corpus, "profiles": list(PROFILES),
                               "regimes": list(REGIMES), "topologies": list(TOPOLOGIES),
                               "depth": depth, "expected_cells": len(cells(corpus))}}
        _write_new(campaign / "campaign.json", canonical(metadata))
    else:
        campaign = resume.absolute()
        metadata = _load_json(campaign / "campaign.json")
        if metadata.get("schema") != SCHEMA or metadata.get("config") != config:
            raise CampaignError("resume:campaign_configuration_mismatch")
    runner = command_runner or _run_command
    records: list[dict[str, Any]] = []
    stamp = str(metadata["created_utc"])
    for cell in cells(corpus):
        _validate_cell(cell)
        cell_label = _cell_id(cell)
        cell_dir = campaign / "cells" / _slug(cell)
        cell_dir.mkdir(parents=True, exist_ok=True)
        state_path = cell_dir / "status.json"
        old = _cell_state(state_path)
        if old.get("status") == "PASS":
            records.append(old)
            continue
        if old.get("status") == "FAIL" and not retry_failed:
            records.append(old)
            continue
        if old.get("status") in {"FAIL", "STAGED", "INTERRUPTED"}:
            old.setdefault("history", []).append({
                "attempt": old.get("attempt"), "status": old.get("status"),
                "error": old.get("error"), "result": old.get("result"),
                "ended_utc": old.get("ended_utc"),
            })
        if old.get("status") == "RUNNING":
            old.setdefault("history", []).append({"status": "RUNNING", "ended": "interrupted_on_resume"})
            old["status"] = "INTERRUPTED"
            _replace_json(state_path, old)
        attempt_no = _attempt(cell_dir)
        attempt_dir = cell_dir / f"attempt-{attempt_no:03d}"
        attempt_dir.mkdir()
        source_manifest = _format_path(source_manifest_spec, cell)
        source_root = _format_path(source_root_spec, cell)
        engine_manifest = _format_path(engine_manifest_template, cell)
        plan_cmds, producer_cmd, live_cmds, comparison_cmd, result_dirs = _source_commands(
            cell_dir, cell, depth=depth, source_manifest=source_manifest,
            source_root=source_root, matrix_audit=matrix_audit, engine_manifest=engine_manifest,
            product_root=product_build_root, python=python, campaign_stamp=stamp,
            compile_db=compile_db_path, compile_source_root=compile_source_path,
            compile_output_root=compile_output_path, repo=repo)
        # _source_commands uses attempt-001 as a stable template. Rebase every
        # generated path to this attempt so retries cannot overwrite outputs.
        def rebase(value: object) -> object:
            if isinstance(value, Path):
                return Path(str(value).replace(str(cell_dir / "attempt-001"), str(attempt_dir)))
            if isinstance(value, str):
                return value.replace(str(cell_dir / "attempt-001"), str(attempt_dir))
            if isinstance(value, list):
                return [rebase(item) for item in value]
            if isinstance(value, dict):
                return {key: rebase(item) for key, item in value.items()}
            return value
        def rehash(value: object) -> object:
            if isinstance(value, list):
                return [rehash(item) for item in value]
            if isinstance(value, dict):
                mapped = {key: rehash(item) for key, item in value.items() if key != "sha256"}
                if {"stage", "argv", "shell", "cwd", "executable"}.issubset(mapped):
                    mapped["sha256"] = hashlib.sha256(canonical(mapped)).hexdigest()
                return mapped
            return value
        plan_cmds, producer_cmd, live_cmds, comparison_cmd, result_dirs = tuple(
            rehash(rebase(item)) for item in (plan_cmds, producer_cmd, live_cmds, comparison_cmd, result_dirs)
        )  # type: ignore[assignment]
        commands = {"predictive_plan": plan_cmds, "predictive_producer": producer_cmd,
                    "live": live_cmds, "comparison": comparison_cmd}
        _write_new(attempt_dir / "commands.json", canonical(commands))
        status: dict[str, Any] = {"schema": CELL_SCHEMA, "cell": cell, "cell_id": cell_label,
                                  "split": SPLITS[corpus], "status": "RUNNING", "attempt": attempt_no,
                                  "started_utc": datetime.now(timezone.utc).isoformat(),
                                  "commands": {"path": str((attempt_dir / "commands.json").relative_to(campaign)),
                                               "sha256": hashlib.sha256((attempt_dir / "commands.json").read_bytes()).hexdigest()},
                                  "history": old.get("history", [])}
        _replace_json(state_path, status)
        if not execute:
            status["status"] = "STAGED"
            status["ended_utc"] = datetime.now(timezone.utc).isoformat()
            status["reason"] = "predictive execution disabled"
            result_path = attempt_dir / "result.json"
            _write_new(result_path, canonical({"schema": "icecream-s8-campaign-result-v2",
                                               "cell": cell, "attempt": attempt_no,
                                               "status": "STAGED", "result": None,
                                               "error": status["reason"]}))
            status["result_record"] = {"path": str(result_path.relative_to(campaign)),
                                        "bytes": result_path.stat().st_size,
                                        "sha256": hashlib.sha256(result_path.read_bytes()).hexdigest()}
            _replace_json(state_path, status)
            records.append(status)
            continue
        stdout, stderr = attempt_dir / "stdout.log", attempt_dir / "stderr.log"
        result: dict[str, Any] | None = None
        error: str | None = None
        for command in plan_cmds + [producer_cmd]:
            stage = str(command["stage"])
            rc = runner(command, repo, stdout, stderr)
            if rc != 0:
                error = f"{stage}:returncode:{rc}"
                break
        if error is None:
            try:
                segments: list[dict[str, Any]] = []
                for result_dir in result_dirs:
                    result_dir = Path(result_dir)
                    artifacts = _artifact_tree(result_dir)
                    names = {str(item["path"]) for item in artifacts}
                    if "predictive_sim.jsonl" not in names or "producer_manifest.json" not in names:
                        raise CampaignError("predictive_result:required_artifact_missing")
                    curve_lines = (result_dir / "predictive_sim.jsonl").read_text().splitlines()
                    if not curve_lines:
                        raise CampaignError("predictive_result:empty_curve")
                    curve = json.loads(curve_lines[-1])
                    segments.append({"directory": str(result_dir), "artifacts": artifacts,
                                     "points": len(curve_lines),
                                     "final": curve.get("cumulative", {}) if isinstance(curve, dict) else {}})
                result = {"segments": segments,
                          "directories": [item["directory"] for item in segments],
                          "points": sum(int(item["points"]) for item in segments),
                          "final": segments[-1]["final"] if segments else {},
                          "plans": []}
                for plan_command in plan_cmds:
                    plan_argv = [str(item) for item in plan_command["argv"]]  # type: ignore[index]
                    plan_path = Path(plan_argv[plan_argv.index("--out") + 1])
                    plan_record = _sha(plan_path)
                    plan_record["path"] = str(plan_path.relative_to(campaign))
                    result["plans"].append(plan_record)
                if len(segments) == 1:
                    result.update(segments[0])
            except (OSError, ValueError, IndexError, CampaignError) as exc:
                error = f"predictive_result:{exc}"
        if error is None:
            status.update(status="PASS", result=result)
        else:
            status.update(status="FAIL", error=error)
        status["ended_utc"] = datetime.now(timezone.utc).isoformat()
        result_record = {"schema": "icecream-s8-campaign-result-v2", "cell": cell,
                         "attempt": attempt_no, "status": status["status"],
                         "result": result, "error": error}
        result_path = attempt_dir / "result.json"
        _write_new(result_path, canonical(result_record))
        status["result_record"] = {"path": str(result_path.relative_to(campaign)),
                                    "bytes": result_path.stat().st_size,
                                    "sha256": hashlib.sha256(result_path.read_bytes()).hexdigest()}
        if stdout.exists() and stderr.exists():
            status["logs"] = {
                "stdout": {"path": str(stdout.relative_to(campaign)),
                            "bytes": stdout.stat().st_size,
                            "sha256": hashlib.sha256(stdout.read_bytes()).hexdigest()},
                "stderr": {"path": str(stderr.relative_to(campaign)),
                            "bytes": stderr.stat().st_size,
                            "sha256": hashlib.sha256(stderr.read_bytes()).hexdigest()},
            }
        _replace_json(state_path, status)
        records.append(status)
        # Update summary after each cell so a killed process can resume with a
        # truthful partial view.
        _replace_json(campaign / "summary.json", _summary(campaign, records, config))
    # Include untouched prior states when an early skip left records sparse.
    all_records = []
    for cell in cells(corpus):
        all_records.append(_cell_state(campaign / "cells" / _slug(cell) / "status.json"))
    summary = _summary(campaign, all_records, config)
    _replace_json(campaign / "summary.json", summary)
    metadata["status"] = summary["status"]
    summary_raw = (campaign / "summary.json").read_bytes()
    metadata["summary"] = {"path": "summary.json", "bytes": len(summary_raw),
                            "sha256": hashlib.sha256(summary_raw).hexdigest()}
    _replace_json(campaign / "campaign-status.json", metadata)
    return campaign


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", choices=CORPORA, default="DuckDB")
    parser.add_argument("--depth", choices=DEPTHS, required=True)
    parser.add_argument("--source-manifest", required=True,
                        help="path or template using {corpus},{profile},{regime},{topology}")
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--matrix-audit", type=Path, required=True)
    parser.add_argument("--engine-manifest-template", required=True)
    parser.add_argument("--product-build-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, default=Path("experiments"))
    parser.add_argument("--resume", type=Path)
    parser.add_argument("--retry-failed", action="store_true")
    parser.add_argument("--compile-db")
    parser.add_argument("--compile-source-root")
    parser.add_argument("--compile-output-root")
    parser.add_argument("--plan-only", action="store_true",
                        help="retain the full matrix and staged commands without executing predictive cells")
    parser.add_argument("--mode", choices=("predictive-only", "all"), default="predictive-only",
                        help="run predictive cells now; live/comparison commands remain explicitly staged")
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--python", default=sys.executable)
    args = parser.parse_args(argv)
    try:
        campaign = run_campaign(output_root=args.output_root, repo=args.repo,
                                corpus=args.corpus, depth=args.depth,
                                source_manifest=args.source_manifest, source_root=args.source_root,
                                matrix_audit=args.matrix_audit,
                                engine_manifest_template=args.engine_manifest_template,
                                product_build_root=args.product_build_root, python=args.python,
                                resume=args.resume, retry_failed=args.retry_failed,
                                compile_db=args.compile_db, compile_source_root=args.compile_source_root,
                                compile_output_root=args.compile_output_root, execute=not args.plan_only,
                                mode=args.mode)
    except CampaignError as exc:
        print(f"s8_campaign_driver: {exc}", file=sys.stderr)
        return 2
    print(campaign)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
