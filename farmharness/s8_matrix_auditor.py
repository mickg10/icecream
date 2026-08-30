#!/usr/bin/env python3
"""Audit canonical S8 three-record experiments and write JSON/Markdown reports.

Only a ``records.jsonl`` directly bound by its sibling PASS
``experiment_manifest.json`` is a canonical experiment.  Files below
supplemental, legacy, control, mutation, or old trees are deliberately
ignored.  This report is read-only with respect to the experiment root and
never promotes an incomplete or malformed cell to PASS.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import stat
import sys
from pathlib import Path
from typing import Any

try:
    from . import s8_predictive_live_normalizer as schema
    from .s8_schema import CORPORA, PROFILES, REGIMES, SPLITS
except ImportError:  # pragma: no cover
    import s8_predictive_live_normalizer as schema
    from s8_schema import CORPORA, PROFILES, REGIMES, SPLITS


RECORD_SCHEMA = "icecream-s8-predictive-live-record-v1"
RECORD_TYPES = ("predictive_sim", "live", "comparison")
CELLS = tuple((corpus, profile, regime)
              for corpus in CORPORA for profile in PROFILES for regime in REGIMES)
EXCLUDED_PARTS = frozenset({"supplemental", "legacy", "old", "control", "mutation"})
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")


class AuditError(ValueError):
    """Raised when a canonical candidate is invalid or ambiguous."""


def _duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise AuditError(f"duplicate_json_key:{key}")
        value[key] = item
    return value


def _parse(raw: bytes, label: str) -> object:
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_duplicate_keys,
                          parse_constant=lambda token: (_ for _ in ()).throw(
                              AuditError(f"{label}:non_finite")))
    except AuditError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise AuditError(f"{label}:invalid_json") from exc


def _snapshot(path: Path, label: str, limit: int = 128 * 1024 * 1024) -> tuple[bytes, dict[str, Any]]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise AuditError(f"{label}:unavailable") from exc
    if not stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
        raise AuditError(f"{label}:not_private_regular_file")
    if info.st_nlink != 1:
        raise AuditError(f"{label}:hard_link_alias")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise AuditError(f"{label}:cannot_open") from exc
    try:
        before = os.fstat(fd)
        chunks: list[bytes] = []
        digest = hashlib.sha256()
        total = 0
        while True:
            block = os.read(fd, 1 << 20)
            if not block:
                break
            total += len(block)
            if total > limit:
                raise AuditError(f"{label}:too_large")
            chunks.append(block)
            digest.update(block)
        after = os.fstat(fd)
        if any(getattr(before, field) != getattr(after, field) for field in
               ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
            raise AuditError(f"{label}:changed_while_reading")
        return b"".join(chunks), {"sha256": digest.hexdigest(), "bytes": total}
    except OSError as exc:
        raise AuditError(f"{label}:read_failed") from exc
    finally:
        os.close(fd)


def _sha(value: object, label: str) -> str:
    if not isinstance(value, str) or HEX64.fullmatch(value) is None:
        raise AuditError(f"{label}:invalid_digest")
    normalized = value.lower()
    if int(normalized, 16) == 0:
        raise AuditError(f"{label}:zero_digest")
    return normalized


def _cell(value: object, label: str) -> tuple[str, str, str]:
    if isinstance(value, str) and value.count("/") == 2:
        parts = tuple(value.split("/"))
    elif isinstance(value, dict) and set(value) == {"corpus", "profile", "regime"}:
        parts = (value["corpus"], value["profile"], value["regime"])
    else:
        raise AuditError(f"{label}:invalid_cell")
    if not all(isinstance(part, str) for part in parts):
        raise AuditError(f"{label}:invalid_cell")
    if parts not in CELLS:
        raise AuditError(f"{label}:undeclared_cell")
    return parts  # type: ignore[return-value]


def _descriptor_matches(manifest: dict[str, Any], records: Path) -> dict[str, Any]:
    descriptor = manifest.get("records")
    if not isinstance(descriptor, dict) or set(descriptor) != {"path", "sha256", "bytes"}:
        raise AuditError("experiment_manifest:records_descriptor_missing")
    if descriptor["path"] != "records.jsonl":
        raise AuditError("experiment_manifest:records_path_not_canonical")
    _sha(descriptor["sha256"], "experiment_manifest.records.sha256")
    if type(descriptor["bytes"]) is not int or descriptor["bytes"] <= 0:
        raise AuditError("experiment_manifest.records.bytes_invalid")
    raw, facts = _snapshot(records, "records")
    if facts["sha256"] != descriptor["sha256"].lower() or facts["bytes"] != descriptor["bytes"]:
        raise AuditError("experiment_manifest:records_digest_mismatch")
    return {"raw": raw, "facts": facts}


def _jsonl(raw: bytes, label: str) -> list[dict[str, Any]]:
    lines = raw.splitlines()
    if len(lines) != 3 or any(not line.strip() for line in lines):
        raise AuditError(f"{label}:exactly_three_nonblank_records_required")
    result: list[dict[str, Any]] = []
    for number, line in enumerate(lines, 1):
        value = _parse(line, f"{label}:{number}")
        if not isinstance(value, dict):
            raise AuditError(f"{label}:{number}:object_required")
        result.append(value)
    if tuple(row.get("record_type") for row in result) != RECORD_TYPES:
        raise AuditError(f"{label}:record_order_invalid")
    return result


def _record_identity(record: dict[str, Any], expected: tuple[str, str, str], label: str) -> dict[str, str]:
    if record.get("schema") != RECORD_SCHEMA or record.get("semantics") != schema.SEMANTICS:
        raise AuditError(f"{label}:schema_invalid")
    if _cell(record.get("cell"), f"{label}.cell") != expected:
        raise AuditError(f"{label}:cell_mismatch")
    if record.get("split") != SPLITS[expected[0]]:
        raise AuditError(f"{label}:split_mismatch")
    try:
        identity = schema._validate_identity(record.get("identity"))
        schema._validate_units(record.get("units"))
    except schema.NormalizationError as exc:
        raise AuditError(f"{label}:identity_invalid:{exc}") from exc
    if record.get("model_id") != identity["model_id"]:
        raise AuditError(f"{label}:model_id_mismatch")
    return identity


def _record_provenance(value: object, mode: str, label: str) -> dict[str, str]:
    if not isinstance(value, dict):
        raise AuditError(f"{label}:provenance_invalid")
    required = {"mode", "producer", "trace_free"}
    allowed = required | {"manifest_sha256", "curve_sha256", "evidence"}
    if not required.issubset(value) or not set(value).issubset(allowed):
        raise AuditError(f"{label}:provenance_invalid")
    try:
        schema._validate_provenance({key: value[key] for key in required}, mode)
    except schema.NormalizationError as exc:
        raise AuditError(f"{label}:provenance_invalid:{exc}") from exc
    digests: dict[str, str] = {}
    for key in ("manifest_sha256", "curve_sha256"):
        if key not in value:
            raise AuditError(f"{label}.{key}:missing")
        digests[key] = _sha(value[key], f"{label}.{key}")
    if "evidence" in value:
        try:
            schema._validate_evidence(value["evidence"])
        except schema.NormalizationError as exc:
            raise AuditError(f"{label}.evidence:invalid:{exc}") from exc
    return digests


def _record_units(record: dict[str, Any], label: str) -> dict[str, str]:
    try:
        return schema._validate_units(record.get("units"))
    except schema.NormalizationError as exc:
        raise AuditError(f"{label}:units_invalid:{exc}") from exc


def _finite(value: object, label: str) -> float:
    if type(value) not in (int, float) or not math.isfinite(float(value)):
        raise AuditError(f"{label}:number_invalid")
    return float(value)


def _curves(records: list[dict[str, Any]], identities: list[dict[str, str]], label: str) -> dict[str, Any]:
    curves: list[tuple[list[dict[str, object]], list[dict[str, int | float]], list[str]]] = []
    for index, (record, identity) in enumerate(zip(records[:2], identities[:2])):
        raw = record.get("raw_cumulative_curve")
        if not isinstance(raw, list) or not raw:
            raise AuditError(f"{label}:raw_curve_missing:{index}")
        if any(not isinstance(row, dict) for row in raw):
            raise AuditError(f"{label}:curve_invalid:{index}:point_object_required")
        try:
            encoded = b"".join(schema.canonical_bytes(row) + b"\n" for row in raw)
            curves.append(schema._parse_curve(encoded, f"{label}.curve.{index}", identity))
        except schema.NormalizationError as exc:
            raise AuditError(f"{label}:curve_invalid:{exc}") from exc
    predicted, observed = curves
    if predicted[2] != observed[2] or len(predicted[0]) != len(observed[0]):
        raise AuditError(f"{label}:curve_shape_mismatch")
    for left, right in zip(predicted[0], observed[0]):
        if left["step"] != right["step"] or left["tu_id"] != right["tu_id"]:
            raise AuditError(f"{label}:curve_point_alignment_mismatch")
    comparison = records[2]
    if "raw_cumulative_curve" in comparison:
        raise AuditError(f"{label}:comparison_must_not_retain_raw_curve")
    errors = comparison.get("point_errors")
    losses = comparison.get("loss_curve")
    if not isinstance(errors, list) or not isinstance(losses, list) or \
            len(errors) != len(predicted[0]) or len(losses) != len(predicted[0]):
        raise AuditError(f"{label}:comparison_curve_count_mismatch")
    cumulative_loss = 0.0
    for index, (error_row, loss_row, pvals, ovals, p_row) in enumerate(
            zip(errors, losses, predicted[1], observed[1], predicted[0])):
        if not isinstance(error_row, dict) or not isinstance(loss_row, dict):
            raise AuditError(f"{label}:comparison_point_invalid:{index}")
        if (error_row.get("step"), error_row.get("tu_id")) != (p_row["step"], p_row["tu_id"]):
            raise AuditError(f"{label}:comparison_point_identity_mismatch:{index}")
        if (loss_row.get("step"), loss_row.get("tu_id")) != (p_row["step"], p_row["tu_id"]):
            raise AuditError(f"{label}:loss_point_identity_mismatch:{index}")
        point_errors = error_row.get("errors")
        if not isinstance(point_errors, dict) or set(point_errors) != set(pvals):
            raise AuditError(f"{label}:prediction_error_shape_mismatch:{index}")
        point_squared = 0.0
        for key in pvals:
            expected_signed = pvals[key] - ovals[key]
            expected_absolute = abs(expected_signed)
            expected_relative = (0.0 if expected_signed == 0 else None) if ovals[key] == 0 else expected_signed / abs(ovals[key])
            expected_squared = expected_signed * expected_signed
            item = point_errors[key]
            if not isinstance(item, dict) or set(item) != {"signed", "absolute", "relative", "squared"}:
                raise AuditError(f"{label}:prediction_error_invalid:{index}")
            if not math.isclose(_finite(item["signed"], "signed"), float(expected_signed), rel_tol=0, abs_tol=1e-12) or \
               not math.isclose(_finite(item["absolute"], "absolute"), float(expected_absolute), rel_tol=0, abs_tol=1e-12) or \
               (item["relative"] is None and expected_relative is not None) or \
               (item["relative"] is not None and (expected_relative is None or not math.isclose(_finite(item["relative"], "relative"), float(expected_relative), rel_tol=0, abs_tol=1e-12))) or \
               not math.isclose(_finite(item["squared"], "squared"), float(expected_squared), rel_tol=0, abs_tol=1e-12):
                raise AuditError(f"{label}:prediction_error_value_mismatch:{index}")
            point_squared += float(expected_squared)
        cumulative_loss += point_squared
        if not math.isclose(_finite(loss_row.get("squared_error"), "squared_error"), point_squared, rel_tol=0, abs_tol=1e-12) or \
           not math.isclose(_finite(loss_row.get("cumulative_loss"), "cumulative_loss"), cumulative_loss, rel_tol=0, abs_tol=1e-12):
            raise AuditError(f"{label}:loss_value_mismatch:{index}")
    return {"predicted": predicted, "observed": observed, "errors": errors, "losses": losses}


def _requested_depth(manifest: dict[str, Any], observed: int) -> dict[str, Any]:
    keys = [key for key in ("requested_curve_points", "curve_points_requested") if key in manifest]
    if len(keys) > 1 and manifest[keys[0]] != manifest[keys[1]]:
        raise AuditError("experiment_manifest:requested_curve_points_conflict")
    requested = manifest[keys[0]] if keys else "unspecified"
    if type(requested) is int and requested > 0:
        classification = "complete" if observed >= requested else ("one-point-underfilled" if observed == 1 else "underfilled")
        return {"requested": requested, "observed": observed, "classification": classification}
    if isinstance(requested, str) and requested in {"full", "repeat-full"}:
        return {"requested": requested, "observed": observed,
                "classification": "one-point-underfilled" if observed == 1 else "observed"}
    if requested != "unspecified":
        raise AuditError("experiment_manifest:requested_curve_points_invalid")
    return {"requested": "unspecified", "observed": observed,
            "classification": "one-point" if observed == 1 else "observed"}


def _candidate(records_path: Path) -> dict[str, Any]:
    parent = records_path.parent
    manifest_path = parent / "experiment_manifest.json"
    manifest_raw, _manifest_facts = _snapshot(manifest_path, "experiment_manifest", 4 * 1024 * 1024)
    manifest = _parse(manifest_raw, "experiment_manifest")
    if not isinstance(manifest, dict) or manifest.get("status") != "PASS":
        raise AuditError("experiment_manifest:not_pass")
    descriptor = _descriptor_matches(manifest, records_path)
    expected = _cell(manifest.get("cell"), "experiment_manifest.cell")
    if manifest.get("split") != SPLITS[expected[0]]:
        raise AuditError("experiment_manifest:split_mismatch")
    records = _jsonl(descriptor["raw"], str(records_path))
    identities = [_record_identity(record, expected, f"record.{i}") for i, record in enumerate(records)]
    # Predictive and live producers are independent: source commit/tree,
    # topology digest, and run ID may differ.  The normalizer authenticates
    # their shared cell/input and exact plan capture key instead of relabeling
    # either producer's identity.
    shared = ("corpus", "profile", "regime", "split", "input_digest")
    if identities[0] != identities[2] or any(identities[0][key] != identities[1][key] for key in shared):
        raise AuditError("records:identity_join_mismatch")
    if records[0].get("comparison") != records[1].get("comparison"):
        raise AuditError("records:comparison_join_mismatch")
    predictive_provenance = _record_provenance(
        records[0].get("provenance"), "predictive_sim", "records.predictive_sim")
    live_provenance = _record_provenance(records[1].get("provenance"), "live", "records.live")
    units = [_record_units(record, f"record.{index}") for index, record in enumerate(records)]
    if units[0] != units[1] or units[0] != units[2]:
        raise AuditError("records:units_join_mismatch")
    comparison_provenance = records[2].get("provenance")
    if (not isinstance(comparison_provenance, dict) or
            set(comparison_provenance) != {"predictive_manifest_sha256", "live_manifest_sha256",
                                           "predictive_curve_sha256", "live_curve_sha256"}):
        raise AuditError("records:comparison_provenance_invalid")
    comparison_digests = {
        key: _sha(value, f"records.comparison.provenance.{key}")
        for key, value in comparison_provenance.items()
    }
    if (comparison_digests["predictive_manifest_sha256"] != predictive_provenance["manifest_sha256"] or
            comparison_digests["predictive_curve_sha256"] != predictive_provenance["curve_sha256"] or
            comparison_digests["live_manifest_sha256"] != live_provenance["manifest_sha256"] or
            comparison_digests["live_curve_sha256"] != live_provenance["curve_sha256"]):
        raise AuditError("records:comparison_provenance_mismatch")
    curves = _curves(records, identities, str(records_path))
    observed = len(curves["predicted"][0])
    live_values = curves["observed"][0][-1]["cumulative"]
    final_loss = curves["losses"][-1]["cumulative_loss"]
    return {
        "cell": "/".join(expected), "split": SPLITS[expected[0]], "status": "PASS",
        "experiment": str(parent), "record_sha256": descriptor["facts"]["sha256"],
        "predictive_model_id": identities[0]["model_id"], "live_model_id": identities[1]["model_id"],
        "live_values": live_values,
        "prediction_errors_final": curves["errors"][-1]["errors"],
        "prediction_error_final_cumulative_loss": final_loss,
        "curve_depth": _requested_depth(manifest, observed),
        "curve_points": {"predictive_sim": observed, "live": len(curves["observed"][0]),
                         "point_errors": len(curves["errors"]), "loss_curve": len(curves["losses"])},
    }


def audit(root: Path) -> dict[str, Any]:
    """Recursively audit canonical experiments below ``root``."""
    try:
        info = root.lstat()
    except OSError as exc:
        raise AuditError("root:unavailable") from exc
    if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
        raise AuditError("root:not_private_directory")
    candidates: list[Path] = []
    ignored = 0
    for path in sorted(root.rglob("records.jsonl")):
        if any(part in EXCLUDED_PARTS for part in path.relative_to(root).parts):
            ignored += 1
            continue
        if (path.parent / "experiment_manifest.json").is_file():
            candidates.append(path)
        else:
            ignored += 1
    cells: dict[str, dict[str, Any]] = {}
    invalid: list[dict[str, str]] = []
    for path in candidates:
        try:
            row = _candidate(path)
        except AuditError as exc:
            invalid.append({"path": str(path), "reason": str(exc)})
            continue
        except (KeyError, TypeError, OverflowError) as exc:
            invalid.append({"path": str(path),
                            "reason": f"malformed_candidate:{type(exc).__name__}"})
            continue
        if row["cell"] in cells:
            invalid.append({"path": str(path), "reason": "duplicate_canonical_cell"})
            continue
        cells[row["cell"]] = row
    expected_cells = {"/".join(cell) for cell in CELLS}
    missing = sorted(expected_cells - set(cells))
    calibration = sum(row["split"] == "calibration" for row in cells.values())
    held_out = sum(row["split"] == "held_out_validation" for row in cells.values())
    rows = [cells[key] for key in sorted(cells)]
    complete_matrix = (len(rows) == 32 and not missing and not invalid and
                       calibration == 16 and held_out == 16)
    return {
        "schema": "icecream-s8-matrix-audit-v1", "status": "PASS" if complete_matrix else "INCOMPLETE",
        "matrix": {"expected_cells": 32, "completed_cells": len(rows), "missing_cells": missing,
                    "invalid_candidates": invalid, "ignored_noncanonical_records": ignored,
                    "calibration_cells": calibration, "held_out_validation_cells": held_out,
                    "calibration_expected": 16, "held_out_validation_expected": 16},
        "cells": rows,
    }


def markdown(summary: dict[str, Any]) -> str:
    lines = ["# S8 matrix audit", "", f"Status: **{summary['status']}**", "",
             "| Cell | Split | Completion | Live values (final) | Prediction errors (final; loss) | Curve points (P/L/E/L) |",
             "|---|---|---|---|---:|---:|"]
    by_cell = {row["cell"]: row for row in summary["cells"]}
    for cell in sorted("/".join(item) for item in CELLS):
        row = by_cell.get(cell)
        if row is None:
            corpus = cell.split("/")[0]
            lines.append(f"| {cell} | {SPLITS[corpus]} | MISSING | — | — | — |")
            continue
        depth = row["curve_depth"]
        points = row["curve_points"]
        lines.append(f"| {cell} | {row['split']} | {depth['classification']} ({depth['observed']}/{depth['requested']}) | "
                     f"`{json.dumps(row['live_values'], sort_keys=True, separators=(',', ':'))}` | "
                     f"`{json.dumps(row['prediction_errors_final'], sort_keys=True, separators=(',', ':'))}`; "
                     f"loss={row['prediction_error_final_cumulative_loss']:.12g} | "
                     f"{points['predictive_sim']}/{points['live']}/{points['point_errors']}/{points['loss_curve']} |")
    return "\n".join(lines) + "\n"


def _write(path: Path, raw: bytes) -> None:
    if path.exists() or path.is_symlink():
        raise AuditError(f"output_already_exists:{path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--json-out", type=Path, required=True)
    parser.add_argument("--markdown-out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        summary = audit(args.root.absolute())
        _write(args.json_out.absolute(), (json.dumps(summary, sort_keys=True, separators=(",", ":")) + "\n").encode())
        _write(args.markdown_out.absolute(), markdown(summary).encode())
    except AuditError as exc:
        print(f"s8_matrix_auditor: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(summary, sort_keys=True, separators=(",", ":")))
    return 0 if summary["status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
