#!/usr/bin/env python3
"""Freeze a deterministic S8 calibration model from the 16 calibration cells.

The input is an authenticated request naming one normalized ``records.jsonl``
for every ``fmt``/``RocksDB`` profile and regime cell.  The request never
names a live trace or a held-out corpus.  Each records file is authenticated,
checked as one predictive/live/comparison triple, and reduced to robust
observed-over-predicted scale factors for channel bytes and elapsed
nanoseconds.  Throughput is deliberately derived from those two factors and
is never fitted independently.
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

try:  # Works as a package module and as a direct harness script.
    from .s8_schema import CORPORA, CURRENT_SEMANTICS, DECLARED_CELLS, PROFILES, REGIMES, SPLITS
    from .s8_predictive_live_normalizer import RECORD_SCHEMA
except ImportError:  # pragma: no cover - direct invocation.
    from s8_schema import CORPORA, CURRENT_SEMANTICS, DECLARED_CELLS, PROFILES, REGIMES, SPLITS
    from s8_predictive_live_normalizer import RECORD_SCHEMA


REQUEST_SCHEMA = "icecream-s8-calibration-request-v1"
BUNDLE_SCHEMA = "icecream-s8-calibration-model-bundle-v1"
MANIFEST_SCHEMA = "icecream-s8-calibration-model-manifest-v1"
SEMANTICS = CURRENT_SEMANTICS
REQUEST_KEYS = {"schema", "semantics", "predictor", "comparisons"}
PREDICTOR_KEYS = {"source_commit", "source_tree", "model_id"}
COMPARISON_KEYS = {"cell", "records"}
DESCRIPTOR_KEYS = {"path", "sha256", "bytes"}
CELL_KEYS = {"corpus", "profile", "regime"}
COMMON_METRICS = ("channel_bytes", "elapsed_ns", "throughput_bytes_per_s")
RECORD_TYPES = ("predictive_sim", "live", "comparison")
CALIBRATION_CELLS = tuple(
    dict(cell) for cell in DECLARED_CELLS if SPLITS[cell["corpus"]] == "calibration"
)
CALIBRATION_CELL_IDS = frozenset(
    f"{cell['corpus']}/{cell['profile']}/{cell['regime']}"
    for cell in CALIBRATION_CELLS
)
CALIBRATION_BUCKETS = tuple(
    {"profile": profile, "regime": regime}
    for profile in PROFILES
    for regime in REGIMES
)
CALIBRATION_BUCKET_IDS = frozenset(
    f"{bucket['profile']}/{bucket['regime']}" for bucket in CALIBRATION_BUCKETS
)
HEX40 = re.compile(r"^[0-9a-fA-F]{40}$")
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")
SAFE_ID = re.compile(r"^[A-Za-z0-9_.:-]+$")
MAX_REQUEST_BYTES = 1 * 1024 * 1024
MAX_RECORDS_BYTES = 64 * 1024 * 1024


class CalibrationError(ValueError):
    """Raised when calibration evidence is absent, unauthenticated, or invalid."""


def canonical_bytes(value: object) -> bytes:
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"),
                          ensure_ascii=True, allow_nan=False).encode("ascii")
    except (TypeError, ValueError, OverflowError, UnicodeError) as exc:
        raise CalibrationError("canonical_json:invalid_value") from exc


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise CalibrationError(f"duplicate_json_key:{key}")
        result[key] = value
    return result


def parse_json(raw: bytes, label: str) -> object:
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys,
                          parse_constant=lambda value: (_ for _ in ()).throw(
                              CalibrationError(f"{label}:non_finite_json:{value}")))
    except CalibrationError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise CalibrationError(f"{label}:invalid_json") from exc


def _snapshot(path: Path, label: str, limit: int) -> tuple[bytes, str, int]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise CalibrationError(f"{label}:unavailable:{path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise CalibrationError(f"{label}:not_private_regular_file:{path}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise CalibrationError(f"{label}:cannot_open:{path}") from exc
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
                raise CalibrationError(f"{label}:too_large")
            chunks.append(block)
            digest.update(block)
        after = os.fstat(fd)
        if any(getattr(before, field) != getattr(after, field)
               for field in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
            raise CalibrationError(f"{label}:changed_while_reading:{path}")
        return b"".join(chunks), digest.hexdigest(), total
    finally:
        os.close(fd)


def _digest(value: object, label: str, pattern: re.Pattern[str] = HEX64) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        raise CalibrationError(f"{label}:invalid_digest")
    normalized = value.lower()
    if int(normalized, 16) == 0:
        raise CalibrationError(f"{label}:zero_digest")
    return normalized


def _safe_id(value: object, label: str) -> str:
    if not isinstance(value, str) or not value or SAFE_ID.fullmatch(value) is None:
        raise CalibrationError(f"{label}:invalid_identifier")
    return value


def _cell(value: object, label: str) -> dict[str, str]:
    if not isinstance(value, dict) or set(value) != CELL_KEYS:
        raise CalibrationError(f"{label}:fields_invalid")
    if (not all(isinstance(value[field], str) for field in CELL_KEYS) or
            value["corpus"] not in CORPORA or value["profile"] not in PROFILES or
            value["regime"] not in REGIMES):
        raise CalibrationError(f"{label}:not_declared")
    return {field: value[field] for field in ("corpus", "profile", "regime")}


def _cell_id(cell: dict[str, str]) -> str:
    return f"{cell['corpus']}/{cell['profile']}/{cell['regime']}"


def _descriptor(value: object, base: Path, label: str) -> tuple[Path, str, int]:
    if not isinstance(value, dict) or set(value) != DESCRIPTOR_KEYS:
        raise CalibrationError(f"{label}:descriptor_invalid")
    relative = value["path"]
    if not isinstance(relative, str) or not relative:
        raise CalibrationError(f"{label}:path_missing")
    candidate = Path(relative)
    if candidate.is_absolute() or any(part in ("", ".", "..") for part in candidate.parts):
        raise CalibrationError(f"{label}:path_not_private_relative")
    path = base / candidate
    try:
        path.resolve().relative_to(base.resolve())
    except ValueError as exc:
        raise CalibrationError(f"{label}:path_escapes_request_root") from exc
    digest = _digest(value["sha256"], f"{label}.sha256")
    if type(value["bytes"]) is not int or value["bytes"] <= 0:
        raise CalibrationError(f"{label}.bytes:invalid")
    raw, actual_digest, actual_bytes = _snapshot(path, label, MAX_RECORDS_BYTES)
    del raw
    if actual_digest != digest:
        raise CalibrationError(f"{label}:sha256_mismatch")
    if actual_bytes != value["bytes"]:
        raise CalibrationError(f"{label}:byte_count_mismatch")
    return path, digest, actual_bytes


def _finite_nonnegative(value: object, label: str) -> int | float:
    if type(value) not in (int, float):
        raise CalibrationError(f"{label}:metric_not_finite_number")
    try:
        finite = math.isfinite(float(value))
    except (OverflowError, ValueError):
        finite = False
    if not finite:
        raise CalibrationError(f"{label}:metric_not_finite_number")
    if value < 0:
        raise CalibrationError(f"{label}:metric_negative")
    return value


def _identity(value: object, cell: dict[str, str], label: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise CalibrationError(f"{label}:identity_missing")
    for field in ("corpus", "profile", "regime", "run_id", "model_id"):
        _safe_id(value.get(field), f"{label}.{field}")
    actual_cell = {field: value[field] for field in ("corpus", "profile", "regime")}
    if actual_cell != cell:
        raise CalibrationError(f"{label}:cell_mismatch")
    if value.get("split") != "calibration":
        raise CalibrationError(f"{label}:split_mismatch")
    if value.get("split") != SPLITS[cell["corpus"]]:
        raise CalibrationError(f"{label}:split_policy_mismatch")
    _digest(value.get("source_commit"), f"{label}.source_commit", HEX40)
    _digest(value.get("source_tree"), f"{label}.source_tree", HEX40)
    _digest(value.get("input_digest"), f"{label}.input_digest")
    _digest(value.get("topology_digest"), f"{label}.topology_digest")
    return value


def _curve(value: object, cell: dict[str, str], label: str) -> list[dict[str, object]]:
    if not isinstance(value, list) or not value:
        raise CalibrationError(f"{label}:curve_missing")
    previous: dict[str, int | float] | None = None
    rows: list[dict[str, object]] = []
    seen_steps: set[int] = set()
    seen_tus: set[str] = set()
    for number, row in enumerate(value, 1):
        if not isinstance(row, dict) or {"step", "tu_id", "cumulative"} - set(row):
            raise CalibrationError(f"{label}:{number}:point_fields_invalid")
        step, tu_id, cumulative = row["step"], row["tu_id"], row["cumulative"]
        if type(step) is not int or step < 0 or step in seen_steps:
            raise CalibrationError(f"{label}:{number}:step_invalid_or_duplicate")
        if not isinstance(tu_id, str) or not tu_id or tu_id in seen_tus:
            raise CalibrationError(f"{label}:{number}:tu_id_invalid_or_duplicate")
        if "cell" in row and row["cell"] != cell:
            raise CalibrationError(f"{label}:{number}:cell_mismatch")
        if not isinstance(cumulative, dict) or set(cumulative) != set(COMMON_METRICS):
            raise CalibrationError(f"{label}:{number}:metric_shape_invalid")
        metrics = {
            metric: _finite_nonnegative(cumulative[metric], f"{label}:{number}:{metric}")
            for metric in COMMON_METRICS
        }
        elapsed = metrics["elapsed_ns"]
        channel = metrics["channel_bytes"]
        if elapsed <= 0:
            raise CalibrationError(f"{label}:{number}:elapsed_ns_must_be_positive")
        expected_throughput = float(channel) * 1_000_000_000 / float(elapsed)
        if not math.isclose(float(metrics["throughput_bytes_per_s"]), expected_throughput,
                            rel_tol=1e-12, abs_tol=1e-12):
            raise CalibrationError(f"{label}:{number}:throughput_not_derived")
        if previous is not None:
            if step != len(rows):
                raise CalibrationError(f"{label}:{number}:reordered_or_missing_point")
            for metric in ("channel_bytes", "elapsed_ns"):
                if metrics[metric] < previous[metric]:
                    raise CalibrationError(f"{label}:{number}:{metric}_decreased")
        previous = metrics
        rows.append({"step": step, "tu_id": tu_id, "metrics": metrics})
        seen_steps.add(step)
        seen_tus.add(tu_id)
    if rows[0]["step"] != 0:
        raise CalibrationError(f"{label}:first_step_not_zero")
    return rows


def _records(raw: bytes, cell: dict[str, str], predictor_model_id: str,
             label: str) -> tuple[dict[str, object], dict[str, object], dict[str, object], int]:
    lines = raw.splitlines()
    if len(lines) != 3 or any(not line.strip() for line in lines):
        raise CalibrationError(f"{label}:expected_three_records")
    by_type: dict[str, dict[str, object]] = {}
    shared_identity: dict[str, object] | None = None
    shared_units: dict[str, str] | None = None
    curves: dict[str, list[dict[str, object]]] = {}
    for number, line in enumerate(lines, 1):
        value = parse_json(line, f"{label}:{number}")
        if not isinstance(value, dict) or value.get("schema") != RECORD_SCHEMA or \
                value.get("semantics") != SEMANTICS:
            raise CalibrationError(f"{label}:{number}:record_schema_invalid")
        record_type = value.get("record_type")
        if record_type not in RECORD_TYPES or record_type in by_type:
            raise CalibrationError(f"{label}:{number}:record_type_duplicate_or_invalid")
        if value.get("cell") != cell or value.get("split") != "calibration":
            raise CalibrationError(f"{label}:{number}:cell_or_split_mismatch")
        identity = _identity(value.get("identity"), cell, f"{label}:{number}")
        if value.get("model_id") != identity["model_id"]:
            raise CalibrationError(f"{label}:{number}:model_id_identity_mismatch")
        if record_type in ("predictive_sim", "comparison") and identity["model_id"] != predictor_model_id:
            raise CalibrationError(f"{label}:{number}:predictor_model_id_mismatch")
        if record_type == "live" and identity["model_id"] == predictor_model_id:
            raise CalibrationError(f"{label}:{number}:live_model_id_not_distinct")
        units = value.get("units")
        if not isinstance(units, dict) or units.get("point") != "step" or \
                units.get("channel_bytes") != "bytes" or units.get("elapsed_ns") != "ns":
            raise CalibrationError(f"{label}:{number}:units_invalid")
        if "throughput_bytes_per_s" in units and units["throughput_bytes_per_s"] != "bytes_per_s":
            raise CalibrationError(f"{label}:{number}:throughput_unit_invalid")
        if shared_identity is None:
            shared_identity = identity
            shared_units = units
        else:
            # The predictor/live model identifiers may differ in future, but
            # source, input, topology, run, and cell identity must not drift.
            for field in ("corpus", "profile", "regime", "split", "run_id",
                          "source_commit", "source_tree", "input_digest", "topology_digest"):
                if identity[field] != shared_identity[field]:
                    raise CalibrationError(f"{label}:{number}:identity_mismatch:{field}")
            if units != shared_units:
                raise CalibrationError(f"{label}:{number}:units_mismatch")
        if record_type in ("predictive_sim", "live"):
            curves[record_type] = _curve(value.get("raw_cumulative_curve"), cell,
                                          f"{label}:{record_type}")
        else:
            if not isinstance(value.get("point_errors"), list) or not isinstance(value.get("loss_curve"), list):
                raise CalibrationError(f"{label}:comparison:shape_invalid")
        by_type[record_type] = value
    if set(by_type) != set(RECORD_TYPES):
        raise CalibrationError(f"{label}:record_types_missing")
    predicted, observed = curves["predictive_sim"], curves["live"]
    if len(predicted) != len(observed):
        raise CalibrationError(f"{label}:curve_length_mismatch")
    for index, (left, right) in enumerate(zip(predicted, observed, strict=True)):
        if left["step"] != right["step"] or left["tu_id"] != right["tu_id"]:
            raise CalibrationError(f"{label}:point_identity_mismatch:{index}")
    return by_type["predictive_sim"], by_type["live"], by_type["comparison"], len(predicted)


def _median(values: list[float], label: str) -> float:
    if not values:
        raise CalibrationError(f"{label}:no_samples")
    values = sorted(values)
    middle = len(values) // 2
    result = values[middle] if len(values) % 2 else (values[middle - 1] + values[middle]) / 2
    if not math.isfinite(result) or result <= 0:
        raise CalibrationError(f"{label}:invalid_estimate")
    return result


def _write_new(path: Path, raw: bytes, label: str) -> None:
    if path.exists() or path.is_symlink():
        raise CalibrationError(f"{label}:output_already_exists:{path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open("xb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as exc:
        raise CalibrationError(f"{label}:output_write_failed:{path}") from exc


def freeze(request_path: Path, bundle_path: Path, manifest_path: Path | None = None) -> dict[str, object]:
    """Validate exactly 16 calibration triples and write a deterministic bundle."""
    request_raw, request_sha, request_bytes = _snapshot(request_path, "request", MAX_REQUEST_BYTES)
    request = parse_json(request_raw, "request")
    if not isinstance(request, dict) or set(request) != REQUEST_KEYS:
        raise CalibrationError("request:fields_invalid")
    if request["schema"] != REQUEST_SCHEMA or request["semantics"] != SEMANTICS:
        raise CalibrationError("request:schema_or_semantics_invalid")
    predictor = request["predictor"]
    if not isinstance(predictor, dict) or set(predictor) != PREDICTOR_KEYS:
        raise CalibrationError("predictor:fields_invalid")
    predictor_identity = {
        "source_commit": _digest(predictor["source_commit"], "predictor.source_commit", HEX40),
        "source_tree": _digest(predictor["source_tree"], "predictor.source_tree", HEX40),
        "model_id": _safe_id(predictor["model_id"], "predictor.model_id"),
    }
    comparisons = request["comparisons"]
    if not isinstance(comparisons, list):
        raise CalibrationError("comparisons:not_a_list")
    seen: set[str] = set()
    input_bindings: list[dict[str, object]] = []
    bucket_ratios: dict[str, dict[str, list[float]]] = {
        f"{bucket['profile']}/{bucket['regime']}": {
            "channel_bytes": [], "elapsed_ns": []
        }
        for bucket in CALIBRATION_BUCKETS
    }
    bucket_corpora: dict[str, set[str]] = {bucket_id: set() for bucket_id in CALIBRATION_BUCKET_IDS}
    total_points = 0
    request_root = request_path.parent.resolve()
    for index, item in enumerate(comparisons):
        if not isinstance(item, dict) or set(item) != COMPARISON_KEYS:
            raise CalibrationError(f"comparison:{index}:fields_invalid")
        cell = _cell(item["cell"], f"comparison:{index}.cell")
        cell_id = _cell_id(cell)
        if SPLITS[cell["corpus"]] != "calibration":
            raise CalibrationError(f"comparison:{index}:cell_not_calibration")
        if cell_id not in CALIBRATION_CELL_IDS:
            raise CalibrationError(f"comparison:{index}:cell_not_calibration")
        if cell_id in seen:
            raise CalibrationError(f"comparison:{index}:duplicate_cell:{cell_id}")
        seen.add(cell_id)
        records_path, records_sha, records_bytes = _descriptor(
            item["records"], request_root, f"comparison:{index}.records")
        records_raw, actual_sha, actual_bytes = _snapshot(records_path, "records", MAX_RECORDS_BYTES)
        if actual_sha != records_sha or actual_bytes != records_bytes:
            raise CalibrationError(f"comparison:{index}:records_changed_after_authentication")
        predictive, live, _comparison, point_count = _records(
            records_raw, cell, predictor_identity["model_id"], f"comparison:{index}.records")
        p_curve = _curve(predictive["raw_cumulative_curve"], cell, f"comparison:{index}.predictive")
        l_curve = _curve(live["raw_cumulative_curve"], cell, f"comparison:{index}.live")
        for p_row, l_row in zip(p_curve, l_curve, strict=True):
            p_metrics, l_metrics = p_row["metrics"], l_row["metrics"]
            if p_metrics["channel_bytes"] <= 0 or p_metrics["elapsed_ns"] <= 0:
                raise CalibrationError(f"comparison:{index}:zero_predictive_metric")
            if l_metrics["channel_bytes"] <= 0 or l_metrics["elapsed_ns"] <= 0:
                raise CalibrationError(f"comparison:{index}:zero_live_metric")
            bucket = f"{cell['profile']}/{cell['regime']}"
            bucket_corpora[bucket].add(cell["corpus"])
            bucket_ratios[bucket]["channel_bytes"].append(
                float(l_metrics["channel_bytes"]) / float(p_metrics["channel_bytes"]))
            bucket_ratios[bucket]["elapsed_ns"].append(
                float(l_metrics["elapsed_ns"]) / float(p_metrics["elapsed_ns"]))
        total_points += point_count
        input_bindings.append({
            "cell": cell,
            "records_path": str(records_path.relative_to(request_root)),
            "records_sha256": records_sha,
            "records_bytes": records_bytes,
        })
    missing = sorted(CALIBRATION_CELL_IDS - seen)
    if missing:
        raise CalibrationError(f"comparisons:missing_cells:{','.join(missing)}")
    if len(comparisons) != len(CALIBRATION_CELLS) or len(seen) != len(CALIBRATION_CELLS):
        raise CalibrationError("comparisons:expected_exactly_16_unique_calibration_cells")
    input_bindings.sort(key=lambda item: _cell_id(item["cell"]))
    scales: dict[str, dict[str, float]] = {}
    for bucket_id in sorted(CALIBRATION_BUCKET_IDS):
        ratios = bucket_ratios[bucket_id]
        if (bucket_corpora[bucket_id] != {"fmt", "RocksDB"} or
                len(ratios["channel_bytes"]) < 2 or len(ratios["elapsed_ns"]) < 2):
            raise CalibrationError(f"bucket:{bucket_id}:expected_two_corpora")
        scales[bucket_id] = {
            "channel_bytes": _median(ratios["channel_bytes"], f"{bucket_id}:channel_bytes"),
            "elapsed_ns": _median(ratios["elapsed_ns"], f"{bucket_id}:elapsed_ns"),
        }
    bundle = {
        "schema": BUNDLE_SCHEMA,
        "semantics": SEMANTICS,
        "request": {"sha256": request_sha, "bytes": request_bytes},
        "predictor": predictor_identity,
        "calibration": {
            "method": "median_live_over_predictive_v1",
            "cells": len(input_bindings),
            "points": total_points,
            "scales": scales,
            "derived_metrics": {
                "throughput_bytes_per_s": "channel_bytes * 1000000000 / elapsed_ns",
            },
        },
        "inputs": input_bindings,
    }
    bundle_raw = canonical_bytes(bundle) + b"\n"
    _write_new(bundle_path, bundle_raw, "bundle")
    bundle_sha = hashlib.sha256(bundle_raw).hexdigest()
    if manifest_path is None:
        manifest_path = bundle_path.with_name("calibration-model-manifest.json")
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "semantics": SEMANTICS,
        "bundle": {"path": bundle_path.name, "sha256": bundle_sha, "bytes": len(bundle_raw)},
        "request": {"sha256": request_sha, "bytes": request_bytes},
        "predictor": predictor_identity,
        "inputs": input_bindings,
    }
    _write_new(manifest_path, canonical_bytes(manifest) + b"\n", "manifest")
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", type=Path, required=True)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args(argv)
    try:
        freeze(args.request.absolute(), args.bundle.absolute(),
               args.manifest.absolute() if args.manifest else None)
    except CalibrationError as exc:
        print(f"s8_calibration_freeze: {exc}", file=sys.stderr)
        return 77
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
