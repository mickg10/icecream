#!/usr/bin/env python3
"""Materialize the executable boundary for the S4 homogeneous P50 matrix.

The S4 planner intentionally stops at descriptors.  This small adapter binds
those descriptors to the existing S8 depth, predictive, live, and normalizer
commands.  It is dry-run only by default and never reads corpus inputs while
materializing a campaign.  RAW_II is reported as a hard, explicit gap: the
current product has no legacy multi-TU measurement entry point.
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
    from .s8_campaign_driver import TOPOLOGY_ARGS, _command_record, _safe
    from .s8_schema import CALIBRATION_CORPORA, SPLITS
except ImportError:  # pragma: no cover
    from s4_version_transition_planner import audit_plan, build_plan
    from s8_campaign_driver import TOPOLOGY_ARGS, _command_record, _safe
    from s8_schema import CALIBRATION_CORPORA, SPLITS


SCHEMA = "icecream-s4-method-matrix-executor-v1"
SUMMARY_SCHEMA = "icecream-s4-method-matrix-summary-v1"
RAW_II_GAP = (
    "RAW_II is not an S8 product profile and the current live runner is "
    "cache-profile-only; s4_real_cells is a one-TU Docker/SSH harness and "
    "does not provide the required multi-TU transfer curve."
)


def _stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def _canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def _repo_head(repo: Path) -> str:
    try:
        return subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"],
                                       text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.SubprocessError) as exc:
        raise ValueError("git:head_unavailable") from exc


def _repeat_predecessor(root: Path, method: str) -> Path:
    """Resolve repeat-full to its sibling full arm in this campaign."""
    regime_dir, topology_dir, depth_dir, method_dir = root.parents[:4]
    return (method_dir / "full" / topology_dir.name / regime_dir.name / root.name /
            "arms" / method / "attempt-001" / "depth-plan.json")


def _arm(method: str, block: dict[str, Any], root: Path, *, python: str,
         repo: Path, source_manifest: str, source_root: str,
         matrix_audit: Path, engine_manifest: str, product_root: Path,
         compile_db: Path | None, compile_source_root: Path | None,
         compile_output_root: Path | None) -> dict[str, Any]:
    """Return one arm record without touching any source or product input."""
    product_method = method
    harness_profile = "GRZ_RESIDUAL" if method == "GRZ" else method
    arm_dir = root / "arms" / method
    arm_dir.mkdir(parents=True, exist_ok=True)
    common = {
        "method": method, "product_profile": product_method,
        "harness_profile": harness_profile, "state": "s50-c50-f50",
        "artifact": "P50", "artifact_version": 50,
        "mode": "current", "cache_expected": True,
        "cache_disabled": False,
    }
    if method == "RAW_II":
        record = {**common, "mode": "whole-legacy", "cache_expected": False,
                  "cache_disabled": True, "status": "BLOCKED",
                  "executable": False, "gap": RAW_II_GAP,
                  "commands": [], "measurement": _measurement()}
        (arm_dir / "manifest.json").write_bytes(_canonical(record))
        return record

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
    depth_args = [python, str(repo / "farmharness/s8_depth_runner.py"),
                  "--source-manifest", source_manifest, "--source-root", source_root,
                  "--matrix-audit", str(matrix_audit), "--result-dir", str(result),
                  "--corpus", corpus, "--profile", harness_profile,
                  "--regime", regime, "--depth", depth, "--topology", topology_arg,
                  "--out", str(plan)]
    if depth == "repeat-full":
        depth_args.extend(("--repeat-of", str(repeat_of)))
    producer_args = [python, str(repo / "farmharness/s8_multitu_predictive_producer.py"),
                     "--plan", str(first_plan), "--engine-manifest", engine_manifest,
                     "--product-build-root", str(product_root)]
    if depth == "repeat-full":
        producer_args.extend(("--repeat-plan", str(plan)))
    else:
        producer_args.extend(("--output-dir", str(result)))
    prep = attempt / "live-prep"
    prep_args = [python, str(repo / "farmharness/s8_live_batch_prep.py"),
                 "--predictive-plan", str(first_plan), "--compile-db",
                 str(compile_db or "<compile-db-required>"),
                 "--compile-source-root", str(compile_source_root or "<compile-source-root-required>"),
                 "--compile-output-root", str(compile_output_root or attempt / "compile-output"),
                 "--output", str(prep)]
    live_args = [python, str(repo / "farmharness/s8_real_c1f1_live_runner.py"),
                 "--batch-manifest", str(prep / "batch-manifest.jsonl"),
                 "--predictive-plan", str(first_plan), "--topology", str(prep / "topology.json"),
                 "--suite", topology, "--profile", harness_profile,
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
        compare_args.append([python, str(repo / "farmharness/s8_predictive_live_normalizer.py"),
                             "--predictive-manifest", str(compare_result / "predictive_curve_manifest.json"),
                             "--live-manifest", str(attempt / "live-output" /
                                                      (f"live_curve_manifest_{segment}.json"
                                                       if segment else "live_curve_manifest.json")),
                             "--out", str(attempt / f"records{suffix}.jsonl")])
    live_ready = compile_db is not None and compile_source_root is not None
    reason = None if live_ready else "authenticated compile DB/source root not configured"
    # The S8 commands are retained as a handoff, but no individual arm is
    # executable until the comparison's RAW_II half has a valid bridge.
    command_reason = RAW_II_GAP
    if not live_ready:
        command_reason += "; " + reason
    commands = [
        _command_record(depth_args, repo, stage="predictive_plan", executable=False, reason=command_reason),
        _command_record(producer_args, repo, stage="predictive_producer", executable=False, reason=command_reason),
        _command_record(prep_args, repo, stage="live_prepare", executable=False, reason=command_reason),
        _command_record(live_args, repo, stage="live_run", executable=False, reason=command_reason),
    ]
    commands.extend(_command_record(command, repo, stage="comparison" if len(compare_args) == 1
                                    else f"comparison_{segment or 'full-1'}",
                                    executable=False, reason=command_reason)
                    for command, (_result, segment) in zip(compare_args, compare_segments))
    record = {**common, "status": "STAGED", "executable": False,
              "execution_blocker": RAW_II_GAP, "commands": commands,
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
        raise ValueError("execution is intentionally unavailable until RAW_II bridge exists")
    if corpus not in CALIBRATION_CORPORA:
        raise ValueError("held-out corpora are forbidden")
    repo = repo.absolute()
    plan = build_plan()
    if audit_plan(plan).get("status") != "PASS":
        raise ValueError("s4_plan:audit_failed")
    contract = plan["execution_measurement_contract"]
    stamp = timestamp or _stamp()
    campaign = output_root.absolute() / f"s4-method-matrix-{stamp}"
    campaign.mkdir(parents=True)
    blocks = 0
    blocked = 0
    staged = 0
    for template in contract["measurement_cells"]:
        block = {**template, "corpus": corpus}
        harness_profile = ("GRZ_RESIDUAL" if template["method"] == "GRZ"
                           else str(template["method"]))
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
                             compile_output_root=compile_output_root))
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
        blocked += sum(row["status"] == "BLOCKED" for row in arms)
        staged += sum(row["status"] == "STAGED" for row in arms)
    summary = {"schema": SUMMARY_SCHEMA, "status": "STAGED_RAW_II_GAP",
               "campaign_root": str(campaign), "dry_run": True,
               "execution_ready": False,
               "source_plan_schema": plan["schema"], "split_policy": {
                   "campaign_corpus": corpus,
                   "allowed_corpora": list(sorted(CALIBRATION_CORPORA)),
                   "held_out_corpora": ["DuckDB", "LLVM-1238"]},
               "comparison_blocks": blocks, "arm_runs": blocks * 2,
               "staged_arms": staged, "blocked_arms": blocked,
               "raw_ii_gap": RAW_II_GAP,
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
