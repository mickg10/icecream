#!/usr/bin/env python3
"""Materialize the executable boundary for the S4 homogeneous P50 matrix.

The S4 planner intentionally stops at descriptors.  This small adapter binds
those descriptors to the existing S8 depth, predictive, live, and normalizer
commands.  It is dry-run only by default and never reads corpus inputs while
materializing a campaign.  RAW_II uses the explicit whole-legacy live arm
while retaining a separate S8 input profile for source-plan paths.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from .s4_version_transition_planner import audit_plan, build_plan
    from . import s8_depth_runner
    from .s8_campaign_driver import TOPOLOGY_ARGS, _command_record, _safe
    from .s8_schema import CALIBRATION_CORPORA, SPLITS
except ImportError:  # pragma: no cover
    from s4_version_transition_planner import audit_plan, build_plan
    import s8_depth_runner
    from s8_campaign_driver import TOPOLOGY_ARGS, _command_record, _safe
    from s8_schema import CALIBRATION_CORPORA, SPLITS


SCHEMA = "icecream-s4-method-matrix-executor-v1"
SUMMARY_SCHEMA = "icecream-s4-method-matrix-summary-v1"
RAW_II_GAP = (
    "RAW_II uses the P50 whole-legacy/no-cache path and is measured by the "
    "role-labelled legacy wire witness."
)


def _transfer_accounting(method: str) -> dict[str, object]:
    if method == "RAW_II":
        return {
            "basis": "framed_application_wire_bytes",
            "c_to_f_frames": ["COMPILE_FILE", "FILE_CHUNK", "END"],
            "f_to_c_frames": ["COMPILE_RESULT", "FILE_CHUNK", "END"],
        }
    return {
        "basis": "source_stage_plus_returned_object_payload",
        "c_to_f_frames": ["P50_SOURCE_STAGE"],
        "f_to_c_frames": ["RETURNED_OBJECT"],
    }


def _stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def _canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def _jsonl_bytes(rows: list[dict[str, Any]]) -> bytes:
    """Encode deterministic JSONL without executing or reading workloads."""
    return b"".join(_canonical(row) for row in rows)


def _repo_head(repo: Path) -> str:
    try:
        return subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"],
                                       text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.SubprocessError) as exc:
        raise ValueError("git:head_unavailable") from exc


def _authenticated_regular_file(path: str | Path | None) -> bool:
    """Return true only for an existing non-aliased regular input file."""
    if path is None:
        return False
    candidate = Path(path)
    try:
        info = candidate.lstat()
    except OSError:
        return False
    return candidate.is_file() and not candidate.is_symlink() and info.st_nlink == 1


def _authenticated_directory(path: str | Path | None) -> bool:
    if path is None:
        return False
    candidate = Path(path)
    try:
        info = candidate.lstat()
    except OSError:
        return False
    # Directory link counts include ``.``/``..`` and child directories, so
    # unlike files they are not an identity/authenticity signal.
    return candidate.is_dir() and not candidate.is_symlink()


def _matrix_audit_gate(path: Path) -> tuple[bool, str | None]:
    """Validate the immutable S8 matrix precondition before staging arms."""
    if not _authenticated_regular_file(path):
        return False, "matrix audit is not an authenticated regular file"
    try:
        value, _facts = s8_depth_runner._json(path, "matrix_audit")
        s8_depth_runner._validate_matrix_audit(value)
    except s8_depth_runner.DepthPlanError as exc:
        if str(exc) == "matrix_audit:not_complete_pass":
            return False, "matrix audit status is not PASS"
        return False, str(exc)
    return True, None


def _repeat_predecessor(root: Path, method: str) -> Path:
    """Resolve repeat-full to its sibling full arm in this campaign."""
    regime_dir, topology_dir, depth_dir, method_dir = root.parents[:4]
    return (method_dir / "full" / topology_dir.name / regime_dir.name / root.name /
            "arms" / method / "attempt-001" / "depth-plan.json")


def _arm(method: str, block: dict[str, Any], root: Path, *, python: str,
         repo: Path, source_manifest: str, source_root: str,
         matrix_audit: Path, engine_manifest: str, product_root: Path,
         compile_db: Path | None, compile_source_root: Path | None,
         compile_output_root: Path | None,
         matrix_audit_ready: bool, matrix_audit_reason: str | None) -> dict[str, Any]:
    """Return one arm record without touching any source or product input."""
    product_method = method
    harness_profile = ("RAW_II" if method == "RAW_II" else
                       ("GRZ_RESIDUAL" if method == "GRZ" else method))
    arm_dir = root / "arms" / method
    arm_dir.mkdir(parents=True, exist_ok=True)
    common = {
        "method": method, "product_profile": product_method,
        "harness_profile": harness_profile, "state": "s50-c50-f50",
        "artifact": "P50", "artifact_version": 50,
        "transfer_accounting": _transfer_accounting(method),
        "mode": "whole-legacy" if method == "RAW_II" else "current",
        "cache_expected": method != "RAW_II",
        "cache_disabled": method == "RAW_II",
    }

    depth = str(block["depth"])
    corpus = str(block["corpus"])
    regime = str(block["regime"])
    topology = str(block["topology"])
    topology_arg = TOPOLOGY_ARGS[topology]
    attempt = arm_dir / "attempt-001"
    result = attempt / "predictive"
    plan = attempt / "depth-plan.json"
    repeat_of = root / "references" / "depth-plan-full.json"
    first_plan = plan
    first_result = result
    predecessor: dict[str, Any] | None = None
    # repeat-full is a distinct contract depth.  It references the same arm's
    # full plan, but remains a separate materialized experiment directory.
    if depth == "repeat-full":
        repeat_of = _repeat_predecessor(root, method)
        first_plan = repeat_of
        first_result = repeat_of.parent / "predictive"
        predecessor = {"depth": "full", "path": str(repeat_of),
                       "corpus": corpus, "method": method,
                       "topology": topology, "regime": regime,
                       "order": str(block["order"])}
    depth_args = [python, str(repo / "research/farmharness/s8_depth_runner.py"),
                  "--source-manifest", source_manifest, "--source-root", source_root,
                  "--matrix-audit", str(matrix_audit), "--result-dir", str(result),
                  "--corpus", corpus, "--profile", harness_profile,
                  "--regime", regime, "--depth", depth, "--topology", topology_arg,
                  "--out", str(plan)]
    if depth == "repeat-full":
        depth_args.extend(("--repeat-of", str(repeat_of)))
    if method == "RAW_II":
        # RAW_II has a dedicated producer and two mandatory authenticated
        # inputs.  Placeholders deliberately keep this arm NOT_READY until a
        # caller supplies real regular files; no compressed profile is used.
        producer_args = [python, str(repo / "research/farmharness/s8_raw_ii_predictive_producer.py"),
                         "--plan", str(first_plan),
                         "--raw-ii-witness", "<raw-ii-witness-required>",
                         "--engine-manifest", "<raw-ii-control-engine-required>",
                         "--depth",
                         "full" if depth == "repeat-full" else depth,
                         "--product-root", str(product_root)]
    else:
        producer_args = [python, str(repo / "research/farmharness/s8_multitu_predictive_producer.py"),
                         "--plan", str(first_plan), "--engine-manifest", engine_manifest,
                         "--product-build-root", str(product_root)]
    if depth == "repeat-full":
        producer_args.extend(("--repeat-plan", str(plan)))
    else:
        producer_args.extend(("--output-dir", str(result)))
    prep = attempt / "live-prep"
    prep_args = [python, str(repo / "research/farmharness/s8_live_batch_prep.py"),
                 "--predictive-plan", str(first_plan), "--compile-db",
                 str(compile_db or "<compile-db-required>"),
                 "--compile-source-root", str(compile_source_root or "<compile-source-root-required>"),
                 "--compile-output-root", str(compile_output_root or attempt / "compile-output"),
                 "--output", str(prep)]
    live_args = [python, str(repo / "research/farmharness/s8_real_c1f1_live_runner.py"),
                 "--batch-manifest", str(prep / "batch-manifest.jsonl"),
                 "--predictive-plan", str(first_plan), "--topology", str(prep / "topology.json"),
                 "--suite", topology, "--profile", harness_profile,
                 "--product-profile", product_method,
                 "--product-root", str(product_root), "--corpus", corpus,
                 "--regime", regime, "--depth", "full" if depth == "repeat-full" else depth,
                 "--passes", "2" if depth == "repeat-full" else "1", "--output", str(attempt / "live-output"),
                 "--timestamp", _stamp(), "--execute"]
    if depth == "repeat-full":
        live_args.extend(("--repeat-predictive-plan", str(plan)))
    compare_args = []
    compare_segments = [(first_result, "full-1" if depth == "repeat-full" else None)]
    if depth == "repeat-full":
        compare_segments.append((result, "full-2"))
    for compare_result, segment in compare_segments:
        suffix = f"-{segment}" if segment else ""
        compare_args.append([python, str(repo / "research/farmharness/s8_predictive_live_normalizer.py"),
                             "--predictive-manifest", str(compare_result / "predictive_curve_manifest.json"),
                             "--live-manifest", str(attempt / "live-output" /
                                                      (f"live_curve_manifest_{segment}.json"
                                                       if segment else "live_curve_manifest.json")),
                             "--out", str(attempt / f"records{suffix}.jsonl")])
    live_ready = (
        method != "RAW_II" and
        matrix_audit_ready and
        _authenticated_regular_file(source_manifest) and
        _authenticated_directory(source_root) and
        _authenticated_regular_file(engine_manifest) and
        _authenticated_directory(product_root) and
        _authenticated_regular_file(compile_db) and
        _authenticated_directory(compile_source_root)
    )
    if method == "RAW_II":
        reason = "RAW_II witness and control-engine inputs are not bound"
    elif not matrix_audit_ready:
        reason = matrix_audit_reason or "matrix audit is not a complete PASS record"
    elif not live_ready:
        reason = "authenticated regular source/engine/compile inputs are not bound"
    else:
        reason = None
    command_reason = None if live_ready else reason
    command_executable = live_ready
    commands = [
        _command_record(depth_args, repo, stage="predictive_plan", executable=command_executable, reason=command_reason),
        _command_record(producer_args, repo, stage="predictive_producer", executable=command_executable, reason=command_reason),
        _command_record(prep_args, repo, stage="live_prepare", executable=command_executable, reason=command_reason),
        _command_record(live_args, repo, stage="live_run", executable=command_executable, reason=command_reason),
    ]
    commands.extend(_command_record(command, repo, stage="comparison" if len(compare_args) == 1
                                    else f"comparison_{segment or 'full-1'}", executable=command_executable,
                                    reason=command_reason)
                    for command, (_result, segment) in zip(compare_args, compare_segments))
    record_status = "STAGED" if command_executable else ("NOT_READY" if method == "RAW_II" else "BLOCKED")
    record = {**common, "status": record_status, "executable": command_executable,
              **({"execution_blocker": command_reason} if command_reason else {}),
              "commands": commands,
              "measurement": _measurement()}
    if predecessor is not None:
        record["repeat_predecessor"] = predecessor
    (arm_dir / "manifest.json").write_bytes(_canonical(record))
    return record


def _measurement() -> dict[str, Any]:
    return {"status": "PENDING", "channel_bytes": None, "elapsed_ns": None,
            "throughput_bytes_per_s": None, "simulator_comparison": None}


def _format_template(spec: str, *, corpus: str, profile: str, regime: str,
                     topology: str) -> str:
    """Apply the same cell placeholders as the S8 campaign driver."""
    values = {"corpus": corpus, "profile": profile, "regime": regime,
              "topology": topology, "topology_arg": TOPOLOGY_ARGS[topology],
              "cell": f"{corpus}/{profile}/{regime}/{topology}"}
    try:
        return str(spec).format(**values)
    except (KeyError, ValueError) as exc:
        raise ValueError(f"path_template:invalid:{spec}") from exc


def materialize_matrix(*, output_root: Path, repo: Path, corpus: str,
                       source_manifest: str,
                       source_root: str, matrix_audit: Path,
                       engine_manifest: str, product_root: Path,
                       compile_db: Path | None = None,
                       compile_source_root: Path | None = None,
                       compile_output_root: Path | None = None,
                       python: str = sys.executable, timestamp: str | None = None,
                       execute: bool = False) -> Path:
    if execute:
        raise ValueError("execution is intentionally unavailable in this materializer")
    if corpus not in CALIBRATION_CORPORA:
        raise ValueError("held-out corpora are forbidden")
    repo = repo.absolute()
    plan = build_plan()
    if audit_plan(plan).get("status") != "PASS":
        raise ValueError("s4_plan:audit_failed")
    contract = plan["execution_measurement_contract"]
    matrix_audit_ready, matrix_audit_reason = _matrix_audit_gate(matrix_audit.absolute())
    stamp = timestamp or _stamp()
    campaign = output_root.absolute() / f"s4-method-matrix-{stamp}"
    campaign.mkdir(parents=True)
    # Keep compatibility and ordered-transition planning records beside the
    # timestamped experiment tree.  These records are declarative; a later
    # runner must bind every row to an audited artifact receipt.
    compatibility_rows = [
        {"schema": "icecream-s4-compatibility-experiment-v1",
         "timestamp": stamp, "campaign": campaign.name, "kind": "state",
         **row}
        for row in plan["compatibility_matrix"]
    ]
    transition_rows = [
        {"schema": "icecream-s4-transition-experiment-v1",
         "timestamp": stamp, "campaign": campaign.name, "kind": "transition",
         **row}
        for row in plan["transitions"]
    ]
    (campaign / "compatibility-plan.jsonl").write_bytes(_jsonl_bytes(compatibility_rows))
    (campaign / "transition-plan.jsonl").write_bytes(_jsonl_bytes(transition_rows))
    blocks = 0
    blocked = 0
    staged = 0
    for template in contract["measurement_cells"]:
        block = {**template, "corpus": corpus}
        harness_profile = ("RAW_II"
                           if template["method"] == "RAW_II" else
                           ("GRZ_RESIDUAL" if template["method"] == "GRZ"
                            else str(template["method"])))
        block_source_manifest = _format_template(
            source_manifest, corpus=corpus, profile=harness_profile,
            regime=str(template["regime"]), topology=str(template["topology"]))
        block_source_root = _format_template(
            source_root, corpus=corpus, profile=harness_profile,
            regime=str(template["regime"]), topology=str(template["topology"]))
        block_engine_manifest = _format_template(
            engine_manifest, corpus=corpus, profile=harness_profile,
            regime=str(template["regime"]), topology=str(template["topology"]))
        ident = f"{corpus}/{template['method']}/{template['depth']}/{_safe(template['topology'])}/{template['regime']}/{template['order']}"
        block_dir = campaign / "experiments" / ident
        block_dir.mkdir(parents=True, exist_ok=True)
        arms = []
        for arm_template in template["sequence"]:
            method = str(arm_template["method"])
            arms.append(_arm(method, block, block_dir, python=python, repo=repo,
                             source_manifest=block_source_manifest, source_root=block_source_root,
                             matrix_audit=matrix_audit.absolute(),
                             engine_manifest=block_engine_manifest, product_root=product_root.absolute(),
                             compile_db=compile_db, compile_source_root=compile_source_root,
                             compile_output_root=compile_output_root,
                             matrix_audit_ready=matrix_audit_ready,
                             matrix_audit_reason=matrix_audit_reason))
        block_record = {"schema": "icecream-s4-method-comparison-block-v1",
                        "id": template["id"], "corpus": corpus,
                        "split": SPLITS[corpus], "method": template["method"],
                        "depth": template["depth"], "topology": template["topology"],
                        "regime": template["regime"], "order": template["order"],
                        "counterbalanced": True,
                        "sequence": [row["method"] for row in arms],
                        "arms": [{"method": row["method"], "status": row["status"],
                                  "manifest": f"arms/{row['method']}/manifest.json",
                                  "product_profile": row["product_profile"],
                                  "harness_profile": row["harness_profile"]} for row in arms]}
        (block_dir / "manifest.json").write_bytes(_canonical(block_record))
        blocks += 1
        blocked += sum(row["status"] in {"BLOCKED", "NOT_READY"} for row in arms)
        staged += sum(row["status"] == "STAGED" for row in arms)
    summary = {"schema": SUMMARY_SCHEMA,
               "status": "STAGED" if blocked == 0 else "NOT_READY",
               "campaign_root": str(campaign), "dry_run": True,
               "execution_ready": False,
               "source_plan_schema": plan["schema"], "split_policy": {
                   "campaign_corpus": corpus,
                   "allowed_corpora": list(sorted(CALIBRATION_CORPORA)),
                   "held_out_corpora": ["DuckDB", "LLVM-1238"]},
               "comparison_blocks": blocks, "arm_runs": blocks * 2,
               "compatibility_states": len(compatibility_rows),
               "transition_rows": len(transition_rows),
               "compatibility_plan": "compatibility-plan.jsonl",
               "transition_plan": "transition-plan.jsonl",
               "staged_arms": staged, "blocked_arms": blocked,
               "raw_ii_gap": (None if blocked == 0 else
                              "RAW_II witness and control-engine inputs are not bound"),
               "metrics": {"channel_bytes": "pending", "elapsed_ns": "pending",
                            "simulator_comparison": "pending"}}
    (campaign / "summary.json").write_bytes(_canonical(summary))
    config = {"source_manifest": source_manifest, "source_root": source_root,
              "matrix_audit": str(matrix_audit), "engine_manifest": engine_manifest,
              "product_root": str(product_root), "corpus": corpus,
              "python": python, "execute_requested": execute}
    (campaign / "matrix.json").write_bytes(_canonical({"schema": SCHEMA,
                                                        "repo_head": _repo_head(repo),
                                                        "config": config,
                                                        "config_sha256": hashlib.sha256(_canonical(config)).hexdigest(),
                                                        "plan": plan}))
    return campaign


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", choices=tuple(sorted(CALIBRATION_CORPORA)), required=True,
                        help="one calibration corpus per campaign; run separately for the other corpus")
    parser.add_argument("--source-manifest", required=True)
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--matrix-audit", type=Path, required=True)
    parser.add_argument("--engine-manifest", required=True)
    parser.add_argument("--product-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, default=Path("experiments"))
    parser.add_argument("--compile-db", type=Path)
    parser.add_argument("--compile-source-root", type=Path)
    parser.add_argument("--compile-output-root", type=Path)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--execute", action="store_true",
                        help="fail closed: a RAW_II bridge is not yet available")
    args = parser.parse_args(argv)
    try:
        print(materialize_matrix(output_root=args.output_root, repo=args.repo, corpus=args.corpus,
                                 source_manifest=args.source_manifest, source_root=args.source_root,
                                 matrix_audit=args.matrix_audit, engine_manifest=args.engine_manifest,
                                 product_root=args.product_root, compile_db=args.compile_db,
                                 compile_source_root=args.compile_source_root,
                                 compile_output_root=args.compile_output_root,
                                 python=args.python, execute=args.execute))
    except (OSError, ValueError, KeyError) as exc:
        print(f"s4_method_matrix_executor: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
