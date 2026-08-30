#!/usr/bin/env python3
"""Emit an authenticated, flattened S8 loss report.

The normalizer is the authority for a single predictive/live join.  This
small boundary consumes a manifest of accepted joins, reuses the matrix
auditor to authenticate each one, and writes a timestamped report directory.
Only entries explicitly marked ``PASS`` contribute measurements; missing,
excluded, held, and dry-run entries remain visible in the report but are never
filled in or counted as observations.

Held-out entries require an authenticated frozen calibration-model manifest.
The report does not fit factors and does not execute a simulator, runner,
container, or product workload.
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
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from . import s8_matrix_auditor as auditor
    from .s8_predictive_engine import load_calibration_bundle
    from .s8_schema import CURRENT_SEMANTICS, DEPTH_CLASSES, SPLITS, TOPOLOGIES
except ImportError:  # pragma: no cover
    import s8_matrix_auditor as auditor
    from s8_predictive_engine import load_calibration_bundle
    from s8_schema import CURRENT_SEMANTICS, DEPTH_CLASSES, SPLITS, TOPOLOGIES


INPUT_SCHEMA = "icecream-s8-loss-input-v1"
POINT_SCHEMA = "icecream-s8-loss-point-v1"
CURVE_SCHEMA = "icecream-s8-loss-curve-v1"
REPORT_SCHEMA = "icecream-s8-loss-report-v1"
AUTHORITY_SCHEMA = "icecream-s8-loss-authority-chain-v1"
SEMANTICS = CURRENT_SEMANTICS
STATUSES = frozenset(("PASS", "MISSING", "EXCLUDED", "HOLD", "DRY_RUN"))
LEGACY_EXPERIMENT_SCHEMA = "icecream-s8-first-triple-driver-v2"
REAL_EXPERIMENT_SCHEMA = "icecream-s8-real-c1f1-live-runner-v2"
PASS_IDS = re.compile(r"^[A-Za-z0-9_.:-]+$")
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")
METRICS = {
    "C_TO_F": "cumulative.C_TO_F_bytes",
    "F_TO_C": "cumulative.F_TO_C_bytes",
    "total": "cumulative.channel_bytes",
    "elapsed": "cumulative.elapsed_ns",
    "throughput": "cumulative.throughput_bytes_per_s",
}


class LossReportError(ValueError):
    """Raised when report inputs are missing, unauthenticated, or ambiguous."""


def _canonical(value: object) -> bytes:
    try:
        return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")
    except (TypeError, ValueError, OverflowError, UnicodeError) as exc:
        raise LossReportError("canonical_json:invalid_value") from exc


def _parse(raw: bytes, label: str) -> object:
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_duplicate,
                          parse_constant=lambda value: (_ for _ in ()).throw(
                              LossReportError(f"{label}:non_finite")))
    except LossReportError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise LossReportError(f"{label}:invalid_json") from exc


def _duplicate(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise LossReportError(f"duplicate_json_key:{key}")
        result[key] = value
    return result


def _snapshot(path: Path, label: str, limit: int = 128 * 1024 * 1024) -> tuple[bytes, dict[str, object]]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise LossReportError(f"{label}:unavailable:{path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise LossReportError(f"{label}:not_private_regular_file:{path}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise LossReportError(f"{label}:cannot_open:{path}") from exc
    try:
        before = os.fstat(fd)
        chunks: list[bytes] = []
        digest = hashlib.sha256()
        size = 0
        while True:
            block = os.read(fd, 1 << 20)
            if not block:
                break
            size += len(block)
            if size > limit:
                raise LossReportError(f"{label}:too_large")
            chunks.append(block)
            digest.update(block)
        after = os.fstat(fd)
        if any(getattr(before, field) != getattr(after, field)
               for field in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
            raise LossReportError(f"{label}:changed_while_reading:{path}")
        return b"".join(chunks), {"sha256": digest.hexdigest(), "bytes": size}
    except OSError as exc:
        raise LossReportError(f"{label}:read_failed:{path}") from exc
    finally:
        os.close(fd)


def _digest(value: object, label: str) -> str:
    if not isinstance(value, str) or HEX64.fullmatch(value) is None or int(value, 16) == 0:
        raise LossReportError(f"{label}:invalid_digest")
    return value.lower()


def _relative(base: Path, value: object, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise LossReportError(f"{label}:path_missing")
    candidate = Path(value)
    if candidate.is_absolute() or any(part in ("", ".", "..") for part in candidate.parts):
        raise LossReportError(f"{label}:path_not_private_relative")
    path = base / candidate
    try:
        path.resolve().relative_to(base.resolve())
    except ValueError as exc:
        raise LossReportError(f"{label}:path_escapes_root") from exc
    return path


def _descriptor(base: Path, value: object, label: str) -> tuple[Path, dict[str, object]]:
    if not isinstance(value, dict) or set(value) != {"path", "sha256", "bytes"}:
        raise LossReportError(f"{label}:descriptor_invalid")
    path = _relative(base, value["path"], f"{label}.path")
    digest = _digest(value["sha256"], f"{label}.sha256")
    if type(value["bytes"]) is not int or value["bytes"] <= 0:
        raise LossReportError(f"{label}.bytes:invalid")
    raw, facts = _snapshot(path, label)
    del raw
    if facts["sha256"] != digest or facts["bytes"] != value["bytes"]:
        raise LossReportError(f"{label}:digest_mismatch")
    return path, {"path": str(path), "sha256": digest, "bytes": facts["bytes"]}


def _cell(value: object, label: str) -> dict[str, str]:
    if not isinstance(value, dict) or set(value) != {"corpus", "profile", "regime"}:
        raise LossReportError(f"{label}:invalid_cell")
    if not all(isinstance(value[key], str) for key in value):
        raise LossReportError(f"{label}:invalid_cell")
    expected = {"fmt", "RocksDB", "DuckDB", "LLVM-1238"}
    profiles = {"ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"}
    regimes = {"cold", "warm"}
    if value["corpus"] not in expected or value["profile"] not in profiles or value["regime"] not in regimes:
        raise LossReportError(f"{label}:undeclared_cell")
    return {key: value[key] for key in ("corpus", "profile", "regime")}


def _load_input(path: Path) -> tuple[dict[str, Any], bytes, str]:
    raw, facts = _snapshot(path, "input_manifest", 16 * 1024 * 1024)
    value = _parse(raw, "input_manifest")
    if not isinstance(value, dict) or set(value) != {"schema", "semantics", "entries"}:
        raise LossReportError("input_manifest:fields_invalid")
    if value["schema"] != INPUT_SCHEMA or value["semantics"] != SEMANTICS:
        raise LossReportError("input_manifest:schema_or_semantics_invalid")
    entries = value["entries"]
    if not isinstance(entries, list) or not entries:
        raise LossReportError("input_manifest:entries_empty")
    return value, raw, str(facts["sha256"])


def _entry(value: object, index: int) -> dict[str, Any]:
    label = f"entry:{index}"
    if not isinstance(value, dict):
        raise LossReportError(f"{label}:object_required")
    allowed = {"cell", "split", "topology", "depth_class", "pass_id", "status", "records", "reason"}
    required = allowed - {"reason"}
    if set(value) - allowed or not required.issubset(value):
        raise LossReportError(f"{label}:fields_invalid")
    cell = _cell(value["cell"], f"{label}.cell")
    if value["split"] != SPLITS[cell["corpus"]]:
        raise LossReportError(f"{label}:split_mismatch")
    if value["topology"] not in TOPOLOGIES:
        raise LossReportError(f"{label}:topology_invalid")
    depth = str(value["depth_class"])
    if depth not in set(DEPTH_CLASSES) - {"legacy"}:
        raise LossReportError(f"{label}:depth_class_invalid")
    if not isinstance(value["pass_id"], str) or not value["pass_id"] or PASS_IDS.fullmatch(value["pass_id"]) is None:
        raise LossReportError(f"{label}:pass_id_invalid")
    status = value["status"]
    if status not in STATUSES:
        raise LossReportError(f"{label}:status_invalid")
    if status == "PASS" and not isinstance(value["records"], dict):
        raise LossReportError(f"{label}:records_required_for_pass")
    if status != "PASS" and value["records"] is not None:
        raise LossReportError(f"{label}:non_pass_records_forbidden")
    if status != "PASS" and (not isinstance(value.get("reason"), str) or not value["reason"]):
        raise LossReportError(f"{label}:non_pass_reason_required")
    return {"cell": cell, "split": value["split"], "topology": value["topology"],
            "depth_class": depth, "pass_id": value["pass_id"], "status": status,
            "records": value["records"], **({"reason": value["reason"]} if "reason" in value else {})}


def _number(value: object, label: str) -> float:
    if type(value) not in (int, float) or not math.isfinite(float(value)):
        raise LossReportError(f"{label}:not_finite")
    return float(value)


def _validate_experiment_identity(manifest: dict[str, Any], entry: dict[str, Any]) -> None:
    """Bind caller labels to real-run metadata when that metadata exists.

    The original first-triple driver manifests predate topology/depth/pass
    fields and are the only compatibility case.  A real runner manifest is
    self-describing; accepting a caller-supplied relabel for any of its
    declared fields would make a valid curve appear to belong to another
    topology or pass.
    """
    fields = {key for key in ("topology", "suite", "depth", "runs") if key in manifest}
    if not fields:
        if manifest.get("schema") == LEGACY_EXPERIMENT_SCHEMA:
            return
        raise LossReportError(f"{entry['pass_id']}:experiment_identity_metadata_missing")
    if (manifest.get("schema") == REAL_EXPERIMENT_SCHEMA and
            fields != {"topology", "suite", "depth", "runs"}):
        raise LossReportError(f"{entry['pass_id']}:experiment_identity_metadata_incomplete")
    expected_suite = {"C1F1": "C1F1/100000", "C1F20": "C1F20/40"}[entry["topology"]]
    if "topology" in manifest and manifest["topology"] != expected_suite:
        raise LossReportError(f"{entry['pass_id']}:experiment_topology_mismatch")
    if "suite" in manifest and manifest["suite"] != expected_suite:
        raise LossReportError(f"{entry['pass_id']}:experiment_suite_mismatch")
    expected_depth = "full" if entry["depth_class"] == "repeat-full" else entry["depth_class"]
    if "depth" in manifest and str(manifest["depth"]) != expected_depth:
        raise LossReportError(f"{entry['pass_id']}:experiment_depth_mismatch")
    if "runs" in manifest:
        runs = manifest["runs"]
        if not isinstance(runs, list) or any(not isinstance(run, str) for run in runs):
            raise LossReportError(f"{entry['pass_id']}:experiment_runs_invalid")
        if entry["pass_id"] not in runs:
            raise LossReportError(f"{entry['pass_id']}:experiment_pass_mismatch")
        if entry["pass_id"] == "full-2" and "full-1" not in runs:
            raise LossReportError(f"{entry['pass_id']}:experiment_repeat_without_full_1")


def _metrics(values: dict[str, int | float], label: str) -> dict[str, float]:
    result: dict[str, float] = {}
    for output, source in METRICS.items():
        if source not in values:
            raise LossReportError(f"{label}:missing_directional_metric:{source}")
        result[output] = _number(values[source], f"{label}.{output}")
    if result["C_TO_F"] < 0 or result["F_TO_C"] < 0 or result["total"] < 0 or result["elapsed"] <= 0 or result["throughput"] < 0:
        raise LossReportError(f"{label}:metric_range_invalid")
    if not math.isclose(result["C_TO_F"] + result["F_TO_C"], result["total"], rel_tol=0, abs_tol=1e-9):
        raise LossReportError(f"{label}:directional_total_mismatch")
    expected_throughput = result["total"] * 1_000_000_000 / result["elapsed"]
    if not math.isclose(result["throughput"], expected_throughput, rel_tol=1e-12, abs_tol=1e-9):
        raise LossReportError(f"{label}:throughput_not_derived")
    return result


def _point(entry: dict[str, Any], record_sha: str, step: int, tu_id: str,
           predicted: dict[str, int | float], observed: dict[str, int | float],
           calibration_sha256: str | None) -> dict[str, Any]:
    p = _metrics(predicted, f"{entry['pass_id']}:{step}:prediction")
    o = _metrics(observed, f"{entry['pass_id']}:{step}:observation")
    metrics: dict[str, Any] = {}
    squared = 0.0
    for name in METRICS:
        signed = p[name] - o[name]
        absolute = abs(signed)
        relative = (0.0 if signed == 0 else None) if o[name] == 0 else signed / abs(o[name])
        item = {"prediction": p[name], "observation": o[name],
                "error": {"signed": signed, "absolute": absolute,
                          "relative": relative, "squared": signed * signed}}
        metrics[name] = item
        squared += signed * signed
    return {"schema": POINT_SCHEMA, "semantics": SEMANTICS,
            "cell": entry["cell"], "split": entry["split"],
            "topology": entry["topology"], "depth_class": entry["depth_class"],
            "pass_id": entry["pass_id"], "step": step, "tu_id": tu_id,
            "record_sha256": record_sha, "calibration_manifest_sha256": calibration_sha256,
            "metrics": metrics, "unit_mixed_squared_error": squared}


def _accepted(entry: dict[str, Any], base: Path,
              calibration_manifest_sha256: str | None,
              calibration_bundle_sha256: str | None) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    records_path, descriptor = _descriptor(base, entry["records"], f"{entry['pass_id']}.records")
    manifest_path = records_path.parent / "experiment_manifest.json"
    manifest_raw, manifest_facts = _snapshot(manifest_path, f"{entry['pass_id']}.experiment_manifest", 4 * 1024 * 1024)
    manifest_value = _parse(manifest_raw, f"{entry['pass_id']}.experiment_manifest")
    if not isinstance(manifest_value, dict) or manifest_value.get("status") != "PASS":
        raise LossReportError(f"{entry['pass_id']}:experiment_manifest_not_pass")
    _validate_experiment_identity(manifest_value, entry)
    if entry["split"] == "held_out_validation":
        frozen_sha = calibration_bundle_sha256
        attached = manifest_value.get("calibration_bundle")
        if frozen_sha is not None:
            if (not isinstance(attached, dict) or
                    not isinstance(attached.get("sha256"), str) or
                    attached["sha256"].lower() != frozen_sha):
                raise LossReportError(f"{entry['pass_id']}:experiment_calibration_bundle_mismatch")
    try:
        candidate = auditor._candidate(records_path)
    except (auditor.AuditError, KeyError, TypeError, OverflowError) as exc:
        raise LossReportError(f"{entry['pass_id']}:canonical_join_invalid:{exc}") from exc
    expected_cell = "/".join(entry["cell"][key] for key in ("corpus", "profile", "regime"))
    if candidate["cell"] != expected_cell or candidate["split"] != entry["split"]:
        raise LossReportError(f"{entry['pass_id']}:canonical_identity_mismatch")
    descriptor_result = auditor._descriptor_matches(manifest_value, records_path)
    raw = descriptor_result["raw"]
    facts = descriptor_result["facts"]
    if facts["sha256"] != descriptor["sha256"] or facts["bytes"] != descriptor["bytes"]:
        raise LossReportError(f"{entry['pass_id']}:records_changed_after_authentication")
    records = auditor._jsonl(raw, str(records_path))
    identities = [auditor._record_identity(record, tuple(entry["cell"][key] for key in ("corpus", "profile", "regime")),
                                          f"{entry['pass_id']}.record.{index}")
                  for index, record in enumerate(records)]
    # Producer/source identities remain truthful and independent; the
    # normalizer binds the join through shared cell/input and plan capture.
    shared_identity = ("corpus", "profile", "regime", "split", "input_digest")
    if identities[0] != identities[2] or any(identities[0][key] != identities[1][key]
                                              for key in shared_identity):
        raise LossReportError(f"{entry['pass_id']}:identity_join_mismatch")
    if records[0].get("comparison") != records[1].get("comparison"):
        raise LossReportError(f"{entry['pass_id']}:comparison_join_mismatch")
    curves = auditor._curves(records, identities, str(records_path))
    predicted = curves["predicted"][1]
    observed = curves["observed"][1]
    pred_rows = curves["predicted"][0]
    obs_rows = curves["observed"][0]
    points = [_point(entry, str(descriptor["sha256"]), int(p_row["step"]), str(p_row["tu_id"]), p_values, o_values,
                     calibration_manifest_sha256)
              for p_row, o_row, p_values, o_values in zip(pred_rows, obs_rows, predicted, observed, strict=True)
              if p_row["step"] == o_row["step"] and p_row["tu_id"] == o_row["tu_id"]]
    if len(points) != len(pred_rows):
        raise LossReportError(f"{entry['pass_id']}:point_alignment_mismatch")
    return points, {"records": descriptor, "points": len(points), "cell": entry["cell"],
                    "split": entry["split"], "topology": entry["topology"],
                    "depth_class": entry["depth_class"], "pass_id": entry["pass_id"],
                    "experiment_manifest": {"path": str(manifest_path),
                                             "sha256": manifest_facts["sha256"],
                                             "bytes": manifest_facts["bytes"]}}


def _new_dir(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    for suffix in range(1000):
        name = stamp if suffix == 0 else f"{stamp}-{suffix:03d}"
        path = root / name
        try:
            path.mkdir()
            return path
        except FileExistsError:
            continue
    raise LossReportError("output:timestamp_collision")


def _write_new(path: Path, raw: bytes) -> dict[str, object]:
    if path.exists() or path.is_symlink():
        raise LossReportError(f"output:already_exists:{path}")
    with path.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    return {"path": path.name, "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)}


def _frozen(path: Path) -> dict[str, object]:
    _raw, facts = _snapshot(path, "calibration_manifest", 4 * 1024 * 1024)
    try:
        loaded = load_calibration_bundle(path)
    except Exception as exc:  # engine has a deliberately broad input boundary
        raise LossReportError(f"calibration_manifest:not_frozen:{exc}") from exc
    bundle_sha = loaded.get("bundle_sha256") if isinstance(loaded, dict) else None
    if not isinstance(bundle_sha, str) or HEX64.fullmatch(bundle_sha) is None or int(bundle_sha, 16) == 0:
        raise LossReportError("calibration_manifest:bundle_digest_missing")
    return {"path": str(path.resolve()), "sha256": facts["sha256"], "bytes": facts["bytes"],
            "bundle_sha256": bundle_sha.lower()}


def build_report(input_manifest: Path, output_root: Path,
                 calibration_manifest: Path | None = None) -> Path:
    """Build one timestamped report directory without executing workloads."""
    manifest, input_raw, input_sha = _load_input(input_manifest.resolve())
    entries = [_entry(value, index) for index, value in enumerate(manifest["entries"])]
    held_out = any(entry["status"] == "PASS" and entry["split"] == "held_out_validation" for entry in entries)
    calibration_binding = _frozen(calibration_manifest.resolve()) if calibration_manifest else None
    if held_out and calibration_binding is None:
        raise LossReportError("held_out:authenticated_frozen_calibration_required")
    out = _new_dir(output_root.resolve())
    base = input_manifest.resolve().parent
    status_counts = {status: sum(entry["status"] == status for entry in entries) for status in sorted(STATUSES)}
    point_rows: list[dict[str, Any]] = []
    accepted: list[dict[str, Any]] = []
    for entry in entries:
        if entry["status"] != "PASS":
            continue
        points, binding = _accepted(entry, base,
                                    str(calibration_binding["sha256"]) if calibration_binding else None,
                                    str(calibration_binding["bundle_sha256"]) if calibration_binding else None)
        point_rows.extend(points)
        accepted.append(binding)
    point_raw = b"".join(_canonical(row) for row in point_rows)
    point_descriptor = _write_new(out / "point-loss.jsonl", point_raw or b"")
    by_step: dict[int, list[dict[str, Any]]] = {}
    for row in point_rows:
        by_step.setdefault(int(row["step"]), []).append(row)
    expected = len(accepted)
    cumulative: dict[str, dict[str, float | int]] = {
        name: {"sse": 0.0, "absolute": 0.0, "count": 0,
               "relative": 0.0, "relative_count": 0}
        for name in METRICS
    }
    curve: list[dict[str, Any]] = []
    for step in sorted(by_step):
        rows = by_step[step]
        per_metric: dict[str, Any] = {}
        for name in METRICS:
            errors = [row["metrics"][name]["error"] for row in rows]
            step_sse = sum(float(item["squared"]) for item in errors)
            step_absolute = sum(float(item["absolute"]) for item in errors)
            relative = [abs(float(item["relative"])) for item in errors
                        if item["relative"] is not None]
            state = cumulative[name]
            state["sse"] = float(state["sse"]) + step_sse
            state["absolute"] = float(state["absolute"]) + step_absolute
            state["count"] = int(state["count"]) + len(errors)
            state["relative"] = float(state["relative"]) + sum(relative)
            state["relative_count"] = int(state["relative_count"]) + len(relative)
            count = len(errors)
            total_count = int(state["count"])
            relative_count = int(state["relative_count"])
            per_metric[name] = {
                "point_count": count,
                "sse": step_sse,
                "mse": step_sse / count if count else None,
                "mae": step_absolute / count if count else None,
                "mape": sum(relative) / len(relative) if relative else None,
                "mape_count": len(relative),
                "cumulative_sse": float(state["sse"]),
                "cumulative_mse": float(state["sse"]) / total_count if total_count else None,
                "cumulative_mae": float(state["absolute"]) / total_count if total_count else None,
                "cumulative_mape": (float(state["relative"]) / relative_count
                                     if relative_count else None),
                "cumulative_mape_count": relative_count,
            }
        curve.append({"step": step, "point_count": len(rows),
                      "missing_measurements": expected - len(rows),
                      "metrics": per_metric})
    curve_value = {"schema": CURVE_SCHEMA, "semantics": SEMANTICS,
                   "status": "PASS" if not any(status_counts[s] for s in STATUSES - {"PASS"}) else "INCOMPLETE",
                   "accepted_entries": expected, "accepted_points": len(point_rows),
                   "excluded_entries": {key: status_counts[key] for key in sorted(STATUSES - {"PASS"})},
                   "mape_definition": "mean(abs(prediction-observation)/abs(observation)); zero observations excluded",
                   "loss_curve": curve}
    curve_raw = _canonical(curve_value)
    curve_descriptor = _write_new(out / "loss-curve.json", curve_raw)
    report_status = "PASS" if not any(status_counts[s] for s in STATUSES - {"PASS"}) else "INCOMPLETE"
    report_value = {"schema": REPORT_SCHEMA, "semantics": SEMANTICS, "status": report_status,
                    "input_manifest": {"path": str(input_manifest.resolve()), "sha256": input_sha, "bytes": len(input_raw)},
                    "entries": [{key: entry[key] for key in ("cell", "split", "topology", "depth_class", "pass_id", "status")}
                                for entry in entries],
                    "accepted": accepted, "status_counts": status_counts,
                    "calibration_manifest": calibration_binding,
                    "outputs": {"point_loss": point_descriptor, "loss_curve": curve_descriptor}}
    report_raw = _canonical(report_value)
    report_descriptor = _write_new(out / "report-manifest.json", report_raw)
    authority = {"schema": AUTHORITY_SCHEMA, "semantics": SEMANTICS, "status": report_status,
                 "checks": {"input_manifest_sha256": input_sha, "accepted_records_revalidated": True,
                            "predictive_live_join_revalidated": True,
                            "missing_excluded_dry_run_not_measured": True,
                            "held_out_frozen_calibration": calibration_binding is not None or not held_out,
                            "calibration_manifest_sha256": (calibration_binding["sha256"]
                                                              if calibration_binding else None)}}
    authority_descriptor = _write_new(out / "authority-chain-validation.json", _canonical(authority))
    sha_entries = {name: descriptor for name, descriptor in (
        ("point-loss.jsonl", point_descriptor), ("loss-curve.json", curve_descriptor),
        ("report-manifest.json", report_descriptor), ("authority-chain-validation.json", authority_descriptor))}
    _write_new(out / "sha256-manifest.json", _canonical({"schema": "icecream-s8-sha256-manifest-v1",
                                                           "files": sha_entries,
                                                           "self_excluded": True}))
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-manifest", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True,
                        help="directory under which one UTC timestamped report directory is created")
    parser.add_argument("--calibration-manifest", type=Path)
    args = parser.parse_args(argv)
    try:
        out = build_report(args.input_manifest, args.output_root, args.calibration_manifest)
    except LossReportError as exc:
        print(f"s8_loss_report: {exc}", file=sys.stderr)
        return 77
    print(str(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
