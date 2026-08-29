#!/usr/bin/env python3
"""Build an authenticated, side-effect-free S8 depth-run plan.

The plan is consumed by ``s8_multitu_predictive_producer``.  Live observation
collection remains a separate authenticated input to the normalizer; this
planner never treats a collection of single-TU records as a completed live
curve.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
from pathlib import Path
from typing import Any

try:
    from .s8_schema import CORPORA, CURRENT_SEMANTICS, PROFILES, REGIMES, SPLITS
except ImportError:  # pragma: no cover
    from s8_schema import CORPORA, CURRENT_SEMANTICS, PROFILES, REGIMES, SPLITS


SCHEMA = "icecream-s8-depth-run-plan-v1"
MATRIX_AUDIT_SCHEMA = "icecream-s8-matrix-audit-v1"
DEPTHS = (100, 200, "full", "repeat-full")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
TIMESTAMPED_DIR = re.compile(
    r"^s8-[A-Za-z0-9_.-]+-\d{8}T\d{6}Z(?:-[A-Za-z0-9_.-]+)?$"
)
MAX_MANIFEST_BYTES = 64 * 1024 * 1024
MAX_INPUT_BYTES = 512 * 1024 * 1024


class DepthPlanError(ValueError):
    """Raised when a depth request cannot be authenticated without guessing."""


def _digest(path: Path, label: str, limit: int = MAX_INPUT_BYTES) -> dict[str, Any]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise DepthPlanError(f"{label}:unavailable:{path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise DepthPlanError(f"{label}:not_private_regular_file:{path}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise DepthPlanError(f"{label}:cannot_open:{path}") from exc
    try:
        before = os.fstat(fd)
        digest = hashlib.sha256()
        size = 0
        while True:
            block = os.read(fd, 1 << 20)
            if not block:
                break
            size += len(block)
            if size > limit:
                raise DepthPlanError(f"{label}:too_large:{path}")
            digest.update(block)
        after = os.fstat(fd)
        if any(getattr(before, field) != getattr(after, field) for field in
               ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
            raise DepthPlanError(f"{label}:changed_while_reading:{path}")
        return {"path": str(path.resolve()), "bytes": size, "sha256": digest.hexdigest()}
    except OSError as exc:
        raise DepthPlanError(f"{label}:read_failed:{path}") from exc
    finally:
        os.close(fd)


def _json(path: Path, label: str) -> tuple[dict[str, Any], dict[str, Any]]:
    facts = _digest(path, label, MAX_MANIFEST_BYTES)
    try:
        value = json.loads(path.read_bytes().decode("utf-8"),
                          object_pairs_hook=_unique_keys,
                          parse_constant=lambda token: (_ for _ in ()).throw(
                              DepthPlanError(f"{label}:non_finite")))
    except DepthPlanError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise DepthPlanError(f"{label}:invalid_json:{path}") from exc
    if not isinstance(value, dict):
        raise DepthPlanError(f"{label}:object_required:{path}")
    return value, facts


def _unique_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise DepthPlanError(f"duplicate_json_key:{key}")
        result[key] = value
    return result


def _cell(corpus: str, profile: str, regime: str) -> dict[str, str]:
    if corpus not in CORPORA or profile not in PROFILES or regime not in REGIMES:
        raise DepthPlanError("cell:undeclared")
    return {"corpus": corpus, "profile": profile, "regime": regime}


def _manifest_inputs(path: Path, source_root: Path, label: str) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    try:
        root_info = source_root.lstat()
    except OSError as exc:
        raise DepthPlanError(f"{label}:source_root_unavailable:{source_root}") from exc
    if stat.S_ISLNK(root_info.st_mode) or not stat.S_ISDIR(root_info.st_mode):
        raise DepthPlanError(f"{label}:source_root_not_private_directory:{source_root}")
    try:
        facts = _digest(path, label, MAX_MANIFEST_BYTES)
        lines = path.read_bytes().decode("utf-8").splitlines()
    except (UnicodeError, OSError) as exc:
        raise DepthPlanError(f"{label}:unreadable:{path}") from exc
    if not lines or any(not line or line != line.strip() for line in lines):
        raise DepthPlanError(f"{label}:line_format_invalid")
    root = source_root.resolve()
    paths: list[dict[str, Any]] = []
    for ordinal, line in enumerate(lines):
        candidate = Path(line)
        if not candidate.is_absolute():
            candidate = root / candidate
        try:
            resolved = candidate.resolve()
            resolved.relative_to(root)
        except (OSError, ValueError) as exc:
            raise DepthPlanError(f"{label}:path_outside_root:{line}") from exc
        file_facts = _digest(resolved, f"{label}.input[{ordinal}]")
        paths.append({"ordinal": ordinal, "path": str(resolved),
                      "source_relative": str(resolved.relative_to(root)),
                      "sha256": file_facts["sha256"], "bytes": file_facts["bytes"]})
    if len({item["path"] for item in paths}) != len(paths):
        raise DepthPlanError(f"{label}:duplicate_input")
    return paths, facts


def _matrix_precondition(path: Path, cell: dict[str, str]) -> dict[str, Any]:
    value, facts = _json(path, "matrix_audit")
    if value.get("schema") != MATRIX_AUDIT_SCHEMA or value.get("status") != "PASS":
        raise DepthPlanError("matrix_audit:not_complete_pass")
    matrix = value.get("matrix")
    if (not isinstance(matrix, dict) or matrix.get("expected_cells") != 32 or
            matrix.get("completed_cells") != 32 or matrix.get("missing_cells") != [] or
            matrix.get("invalid_candidates") != [] or
            matrix.get("calibration_cells") != 16 or
            matrix.get("held_out_validation_cells") != 16):
        raise DepthPlanError("matrix_audit:32_cell_precondition_failed")
    cells = value.get("cells")
    cell_id = "/".join(cell[field] for field in ("corpus", "profile", "regime"))
    if (not isinstance(cells, list) or not any(
            isinstance(row, dict) and row.get("cell") == cell_id and
            row.get("split") == SPLITS[cell["corpus"]] for row in cells)):
        raise DepthPlanError(f"matrix_audit:cell_missing:{cell_id}")
    return {"path": facts["path"], "sha256": facts["sha256"], "bytes": facts["bytes"],
            "schema": value["schema"], "status": value["status"]}


def _repeat_precondition(path: Path, cell: dict[str, str], source_manifest: dict[str, Any],
                         selected: list[dict[str, Any]]) -> dict[str, Any]:
    value, facts = _json(path, "repeat_of")
    if value.get("schema") != SCHEMA or value.get("request", {}).get("depth") != "full":
        raise DepthPlanError("repeat_of:not_full_depth_plan")
    if value.get("cell") != cell:
        raise DepthPlanError("repeat_of:cell_mismatch")
    prior_source = value.get("source_manifest")
    if (not isinstance(prior_source, dict) or
            prior_source.get("sha256") != source_manifest["sha256"]):
        raise DepthPlanError("repeat_of:source_manifest_mismatch")
    prior_inputs = value.get("inputs")
    if prior_inputs != selected:
        raise DepthPlanError("repeat_of:input_sequence_mismatch")
    return {"path": facts["path"], "sha256": facts["sha256"], "bytes": facts["bytes"]}


def build_plan(source_manifest: Path, source_root: Path, matrix_audit: Path,
               result_dir: Path, corpus: str, profile: str, regime: str,
               depth: int | str, repeat_of: Path | None = None) -> dict[str, Any]:
    cell = _cell(corpus, profile, regime)
    if depth not in DEPTHS:
        raise DepthPlanError("request:depth_invalid")
    if not result_dir.is_absolute() or TIMESTAMPED_DIR.fullmatch(result_dir.name) is None:
        raise DepthPlanError("result_dir:timestamped_absolute_directory_required")
    if result_dir.exists() or result_dir.is_symlink():
        raise DepthPlanError(f"result_dir:already_exists:{result_dir}")
    matrix_facts = _matrix_precondition(matrix_audit, cell)
    all_inputs, manifest_facts = _manifest_inputs(source_manifest, source_root, "source_manifest")
    if isinstance(depth, int):
        selected = all_inputs[:depth]
        if len(selected) != depth:
            raise DepthPlanError(f"request:insufficient_inputs:{len(all_inputs)}<{depth}")
        requested_points: int | str = depth
    else:
        selected = all_inputs
        requested_points = "repeat-full" if depth == "repeat-full" else "full"
    if depth == "repeat-full":
        if repeat_of is None:
            raise DepthPlanError("repeat_of:required_for_repeat_full")
        repeat_facts = _repeat_precondition(repeat_of, cell,
                                            {"sha256": manifest_facts["sha256"]}, selected)
    elif repeat_of is not None:
        raise DepthPlanError("repeat_of:only_valid_for_repeat_full")
    source = {"root": str(source_root.resolve()),
              "manifest": {"path": manifest_facts["path"], "sha256": manifest_facts["sha256"],
                           "bytes": manifest_facts["bytes"], "entries": len(all_inputs)}}
    plan: dict[str, Any] = {
        "schema": SCHEMA, "semantics": CURRENT_SEMANTICS,
        "cell": cell, "split": SPLITS[corpus],
        "request": {"depth": depth, "requested_curve_points": requested_points,
                    "base_matrix_cells": 32},
        "source_manifest": source["manifest"], "source_root": source["root"],
        "matrix_precondition": matrix_facts,
        "inputs": selected,
        "result": {"directory": str(result_dir),
                    "raw_jsonl": ["predictive_sim.jsonl", "live_summary.jsonl", "records.jsonl"],
                    "records": "records.jsonl", "experiment_manifest": "experiment_manifest.json"},
        "execution_contract": {
            "status": "READY_MULTI_TU_PREDICTOR",
            "producer": "farmharness.s8_multitu_predictive_producer",
            "producer_points": requested_points,
            "live_observation": "separate_authenticated_curve_required",
            "required": "authenticated predictive curve over the ordered TU sequence",
            "note": "The producer invokes the current simulator per TU and binds the aggregate input digest; live observations are never synthesized.",
        },
    }
    if repeat_of is not None:
        plan["repeat_of"] = repeat_facts
    return plan


def _write_new(path: Path, raw: bytes) -> None:
    if path.exists() or path.is_symlink():
        raise DepthPlanError(f"output_already_exists:{path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-manifest", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--matrix-audit", type=Path, required=True)
    parser.add_argument("--result-dir", type=Path, required=True)
    parser.add_argument("--corpus", choices=CORPORA, required=True)
    parser.add_argument("--profile", choices=PROFILES, required=True)
    parser.add_argument("--regime", choices=REGIMES, required=True)
    parser.add_argument("--depth", choices=("100", "200", "full", "repeat-full"), required=True)
    parser.add_argument("--repeat-of", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    depth: int | str = int(args.depth) if args.depth.isdigit() else args.depth
    try:
        plan = build_plan(args.source_manifest.absolute(), args.source_root.absolute(),
                          args.matrix_audit.absolute(), args.result_dir.absolute(),
                          args.corpus, args.profile, args.regime, depth,
                          args.repeat_of.absolute() if args.repeat_of else None)
        raw = (json.dumps(plan, sort_keys=True, separators=(",", ":")) + "\n").encode()
        _write_new(args.out.absolute(), raw)
    except DepthPlanError as exc:
        print(f"s8_depth_runner: {exc}", file=sys.stderr)
        return 2
    print(raw.decode(), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
