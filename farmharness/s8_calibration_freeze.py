#!/usr/bin/env python3
"""Freeze a deterministic S8 calibration model from complete evidence slices.

The input is an authenticated request naming normalized ``records.jsonl``
triples for each included profile/regime and every complete topology/depth
slice.  The request never
names a live trace or a held-out corpus.  Each records file is authenticated,
checked as one predictive/live/comparison triple, and reduced to robust
observed-over-predicted scale factors for C-to-F bytes, F-to-C bytes, and
elapsed nanoseconds.  Factors are scoped by topology and depth class.
Throughput is deliberately derived from those factors and is never fitted
independently.
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
    from .s8_schema import (CORPORA, CURRENT_SEMANTICS, DECLARED_CELLS, DEPTH_CLASSES,
                            PROFILES, REGIMES, SPLITS, TOPOLOGIES)
    from . import s8_predictive_live_normalizer as normalizer
    from .s8_predictive_live_normalizer import RECORD_SCHEMA
except ImportError:  # pragma: no cover - direct invocation.
    from s8_schema import (CORPORA, CURRENT_SEMANTICS, DECLARED_CELLS, DEPTH_CLASSES,
                           PROFILES, REGIMES, SPLITS, TOPOLOGIES)
    import s8_predictive_live_normalizer as normalizer
    from s8_predictive_live_normalizer import RECORD_SCHEMA


REQUEST_SCHEMA = "icecream-s8-calibration-request-v2"
BUNDLE_SCHEMA = "icecream-s8-calibration-model-bundle-v2"
MANIFEST_SCHEMA = "icecream-s8-calibration-model-manifest-v2"
LEGACY_REQUEST_SCHEMA = "icecream-s8-calibration-request-v1"
SEMANTICS = CURRENT_SEMANTICS
REQUEST_KEYS = {"schema", "semantics", "predictor", "comparisons"}
PREDICTOR_KEYS = {"source_commit", "source_tree", "model_id"}
COMPARISON_KEYS = {"cell", "records"}
CONTEXT_COMPARISON_KEYS = {"cell", "records", "topology", "depth_class"}
DESCRIPTOR_KEYS = {"path", "sha256", "bytes"}
CELL_KEYS = {"corpus", "profile", "regime"}
COMMON_METRICS = ("channel_bytes", "elapsed_ns", "throughput_bytes_per_s")
FACTOR_FIELDS = ("C_TO_F_bytes", "F_TO_C_bytes", "elapsed_ns")
CALIBRATION_METADATA_FIELDS = (
    "product_image_digest", "toolchain_digest", "output_contract_digest",
    "host_digest", "ordered_input_class",
)
EXPLICIT_DEPTH_CLASSES = ("100", "200", "full", "repeat-full")
FTOC_DEPTH_CLASSES = ("100", "200", "full")
EXPLICIT_CONTEXTS = tuple(
    f"{topology}/{depth_class}"
    for topology in TOPOLOGIES for depth_class in EXPLICIT_DEPTH_CLASSES
)
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


def _depth_class(value: object, label: str) -> str:
    # Depth is serialized as a class, rather than inferred from the number of
    # rows.  ``repeat-full`` is normalized by the depth planner to the
    # continuing ``full`` class; codec state itself remains in the live join.
    if type(value) is int:
        value = str(value)
    if not isinstance(value, str) or value not in set(DEPTH_CLASSES) - {"legacy"}:
        raise CalibrationError(f"{label}:invalid_depth_class")
    return value


def _calibration_context(item: dict[str, object], label: str) -> tuple[str, str, bool]:
    """Return topology/depth and whether this is a legacy C1F1 fixture.

    v1 requests had no context.  They are accepted only as a migration aid
    and are permanently scoped to C1F1/legacy; they cannot calibrate a depth
    plan or the C1F20 topology.
    """
    keys = set(item)
    if keys == COMPARISON_KEYS:
        return "C1F1", "legacy", True
    if keys != CONTEXT_COMPARISON_KEYS:
        raise CalibrationError(f"{label}:fields_invalid")
    topology = item["topology"]
    if topology not in TOPOLOGIES:
        raise CalibrationError(f"{label}:invalid_topology")
    return str(topology), _depth_class(item["depth_class"], f"{label}.depth_class"), False


def _context_id(topology: str, depth_class: str) -> str:
    return f"{topology}/{depth_class}"


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


def _explicit_experiment_authority(records_path: Path, cell: dict[str, str],
                                       topology: str, depth_class: str,
                                       label: str) -> tuple[Path, str, dict[str, object],
                                                             dict[str, str]]:
    """Authenticate the PASS experiment that produced one explicit context.

    Context fields in a request are hints until the adjacent experiment
    manifest proves them.  In particular, never let one records file be
    relabeled as C1F20 (or a different depth) by changing request JSON alone.
    """
    manifest_path = records_path.parent / "experiment_manifest.json"
    raw, digest, size = _snapshot(manifest_path, f"{label}.experiment_manifest", MAX_RECORDS_BYTES)
    authority = parse_json(raw, f"{label}.experiment_manifest")
    if not isinstance(authority, dict) or authority.get("status") != "PASS":
        raise CalibrationError(f"{label}:experiment_manifest_not_pass")
    schema = authority.get("schema")
    if schema != "icecream-s8-derived-experiment-v1":
        raise CalibrationError(f"{label}:experiment_manifest_schema_invalid")
    manifest_cell = authority.get("cell")
    if not isinstance(manifest_cell, dict):
        raise CalibrationError(f"{label}:experiment_manifest_cell_invalid")
    if manifest_cell != cell:
        raise CalibrationError(f"{label}:experiment_manifest_cell_mismatch")
    if authority.get("split") != SPLITS[cell["corpus"]]:
        raise CalibrationError(f"{label}:experiment_manifest_split_mismatch")
    expected_suite = "C1F1/100000" if topology == "C1F1" else "C1F20/40"
    if (authority.get("topology") != expected_suite or
            authority.get("suite") != expected_suite):
        raise CalibrationError(f"{label}:experiment_manifest_topology_mismatch")
    # Derived packages distinguish source depth (``full``) from the
    # state-carrying repeat class (``repeat-full``); the latter is authoritative
    # for context binding.
    if (authority.get("depth") != ("full" if depth_class == "repeat-full" else depth_class) or
            authority.get("depth_class") != depth_class):
        raise CalibrationError(f"{label}:experiment_manifest_depth_mismatch")
    runs = authority.get("runs")
    if not isinstance(runs, list) or not runs or any(not isinstance(run, str) for run in runs):
        raise CalibrationError(f"{label}:experiment_manifest_runs_invalid")
    pass_id = authority.get("pass_id")
    expected_pass_id = "full-2" if depth_class == "repeat-full" else "full-1"
    if pass_id != expected_pass_id or pass_id not in runs or (
            depth_class == "repeat-full" and "full-1" not in runs):
        raise CalibrationError(f"{label}:experiment_manifest_pass_mismatch")
    records_descriptor = authority.get("records")
    if not isinstance(records_descriptor, dict) or set(records_descriptor) != DESCRIPTOR_KEYS:
        raise CalibrationError(f"{label}:experiment_manifest_records_missing")
    descriptor_path = records_descriptor.get("path")
    if not isinstance(descriptor_path, str) or Path(descriptor_path).is_absolute() or \
            any(part in ("", ".", "..") for part in Path(descriptor_path).parts) or \
            (records_path.parent / descriptor_path).resolve() != records_path.resolve():
        raise CalibrationError(f"{label}:experiment_manifest_records_mismatch")
    expected_sha = _digest(records_descriptor.get("sha256"), f"{label}.experiment_manifest.records.sha256")
    if type(records_descriptor.get("bytes")) is not int or records_descriptor["bytes"] <= 0:
        raise CalibrationError(f"{label}:experiment_manifest_records_bytes_invalid")
    actual_raw, actual_sha, actual_bytes = _snapshot(records_path, f"{label}.records", MAX_RECORDS_BYTES)
    del actual_raw
    if actual_sha != expected_sha or actual_bytes != records_descriptor["bytes"]:
        raise CalibrationError(f"{label}:experiment_manifest_records_authentication_failed")
    try:
        metadata = _calibration_metadata(authority.get("calibration_metadata"),
                                         f"{label}.experiment_manifest.calibration_metadata")
    except CalibrationError as exc:
        raise CalibrationError(f"{label}:authority_calibration_metadata_missing") from exc
    _validate_timing_placement(
        authority.get("role_placement"), f"{label}.experiment_manifest",
        execution_scope=authority.get("execution_scope"), required=True)
    return manifest_path, pass_id, {"sha256": digest, "bytes": size}, metadata


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


def _calibration_metadata(value: object, label: str) -> dict[str, str]:
    if not isinstance(value, dict):
        raise CalibrationError(f"{label}:calibration_metadata_missing")
    if set(value) != set(CALIBRATION_METADATA_FIELDS):
        raise CalibrationError(f"{label}:calibration_metadata_missing")
    result: dict[str, str] = {}
    for field in CALIBRATION_METADATA_FIELDS:
        if field == "ordered_input_class":
            if value[field] != "ordered":
                raise CalibrationError(f"{label}.{field}:invalid")
            result[field] = value[field]
        else:
            result[field] = _digest(value[field], f"{label}.{field}")
    return result


def _validate_timing_placement(value: object, label: str, *, execution_scope: object = None,
                               required: bool = False) -> None:
    """Require external-farm placement for any calibration timing input."""
    if execution_scope != "external_farm_timing":
        raise CalibrationError(f"{label}:execution_scope_not_timing_eligible")
    if value is None:
        if required:
            raise CalibrationError(f"{label}:role_placement_missing")
        return
    try:
        placement = normalizer._validate_role_placement(value, "live")
    except normalizer.NormalizationError as exc:
        raise CalibrationError(f"{label}:role_placement_invalid") from exc
    if (placement is None or placement.get("mode") != "external_farm" or
            placement.get("timing_eligible") is not True):
        raise CalibrationError(f"{label}:role_placement_not_timing_eligible")


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


def _curve(value: object, cell: dict[str, str], label: str,
           require_directional: bool = False) -> list[dict[str, object]]:
    if not isinstance(value, list) or not value:
        raise CalibrationError(f"{label}:curve_missing")
    previous: dict[str, int | float] | None = None
    rows: list[dict[str, object]] = []
    seen_steps: set[int] = set()
    seen_tus: set[str] = set()
    directional_shape: bool | None = None
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
        if not isinstance(cumulative, dict) or set(cumulative) not in (
                set(COMMON_METRICS),
                set((*COMMON_METRICS, "C_TO_F_bytes", "F_TO_C_bytes"))):
            raise CalibrationError(f"{label}:{number}:metric_shape_invalid")
        current_directional = "C_TO_F_bytes" in cumulative
        if require_directional and not current_directional:
            raise CalibrationError(f"{label}:{number}:directional_metrics_required")
        if directional_shape is not None and current_directional != directional_shape:
            raise CalibrationError(f"{label}:{number}:metric_shape_changed")
        directional_shape = current_directional
        metrics = {
            metric: _finite_nonnegative(cumulative[metric], f"{label}:{number}:{metric}")
            for metric in COMMON_METRICS
        }
        elapsed = metrics["elapsed_ns"]
        channel = metrics["channel_bytes"]
        if "C_TO_F_bytes" in cumulative:
            for direction in ("C_TO_F_bytes", "F_TO_C_bytes"):
                metrics[direction] = _finite_nonnegative(
                    cumulative[direction], f"{label}:{number}:{direction}")
            if not math.isclose(float(metrics["C_TO_F_bytes"]) +
                                float(metrics["F_TO_C_bytes"]), float(channel),
                                rel_tol=1e-12, abs_tol=1e-12):
                raise CalibrationError(f"{label}:{number}:directional_channel_not_conserved")
        if elapsed <= 0:
            raise CalibrationError(f"{label}:{number}:elapsed_ns_must_be_positive")
        expected_throughput = float(channel) * 1_000_000_000 / float(elapsed)
        if not math.isclose(float(metrics["throughput_bytes_per_s"]), expected_throughput,
                            rel_tol=1e-12, abs_tol=1e-12):
            raise CalibrationError(f"{label}:{number}:throughput_not_derived")
        if previous is not None:
            if step != len(rows):
                raise CalibrationError(f"{label}:{number}:reordered_or_missing_point")
            for metric in ("channel_bytes", "elapsed_ns", "C_TO_F_bytes", "F_TO_C_bytes"):
                if metric not in metrics or metric not in previous:
                    continue
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
             label: str, require_directional: bool = False,
             require_metadata: bool = False
             ) -> tuple[dict[str, object], dict[str, object], dict[str, object], int,
                        dict[str, str]]:
    lines = raw.splitlines()
    if len(lines) != 3 or any(not line.strip() for line in lines):
        raise CalibrationError(f"{label}:expected_three_records")
    by_type: dict[str, dict[str, object]] = {}
    shared_identity: dict[str, object] | None = None
    shared_units: dict[str, str] | None = None
    shared_metadata: dict[str, str] | None = None
    metadata_records = 0
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
        metadata = {field: value[field] for field in CALIBRATION_METADATA_FIELDS
                    if field in value}
        if metadata:
            metadata_records += 1
            metadata = _calibration_metadata(metadata, f"{label}:{number}")
            if shared_metadata is None:
                shared_metadata = metadata
            elif metadata != shared_metadata:
                raise CalibrationError(f"{label}:{number}:calibration_metadata_mismatch")
        elif require_metadata:
            raise CalibrationError(f"{label}:{number}:calibration_metadata_missing")
        if record_type in ("live", "comparison"):
            _validate_timing_placement(value.get("role_placement"),
                                       f"{label}:{number}",
                                       execution_scope=value.get("execution_scope"),
                                       required=True)
        if record_type in ("predictive_sim", "live"):
            curves[record_type] = _curve(value.get("raw_cumulative_curve"), cell,
                                          f"{label}:{record_type}", require_directional)
        else:
            if not isinstance(value.get("point_errors"), list) or not isinstance(value.get("loss_curve"), list):
                raise CalibrationError(f"{label}:comparison:shape_invalid")
        by_type[record_type] = value
    if set(by_type) != set(RECORD_TYPES):
        raise CalibrationError(f"{label}:record_types_missing")
    if require_metadata and metadata_records != len(RECORD_TYPES):
        raise CalibrationError(f"{label}:calibration_metadata_missing")
    predicted, observed = curves["predictive_sim"], curves["live"]
    if len(predicted) != len(observed):
        raise CalibrationError(f"{label}:curve_length_mismatch")
    for index, (left, right) in enumerate(zip(predicted, observed, strict=True)):
        if left["step"] != right["step"] or left["tu_id"] != right["tu_id"]:
            raise CalibrationError(f"{label}:point_identity_mismatch:{index}")
    if shared_metadata is None:
        shared_metadata = {}
    return (by_type["predictive_sim"], by_type["live"], by_type["comparison"],
            len(predicted), shared_metadata)


def _median(values: list[float], label: str) -> float:
    if not values:
        raise CalibrationError(f"{label}:no_samples")
    values = sorted(values)
    middle = len(values) // 2
    result = values[middle] if len(values) % 2 else (values[middle - 1] + values[middle]) / 2
    if not math.isfinite(result) or result <= 0:
        raise CalibrationError(f"{label}:invalid_estimate")
    return result


def _geometric_median(log_values: list[float], label: str) -> float:
    """Robust multiplicative estimate: median(log(observed/predicted))."""
    if not log_values:
        raise CalibrationError(f"{label}:no_samples")
    values = sorted(log_values)
    middle = len(values) // 2
    log_estimate = (values[middle] if len(values) % 2 else
                    (values[middle - 1] + values[middle]) / 2)
    if not math.isfinite(log_estimate):
        raise CalibrationError(f"{label}:invalid_estimate")
    estimate = math.exp(log_estimate)
    if not math.isfinite(estimate) or estimate <= 0:
        raise CalibrationError(f"{label}:invalid_estimate")
    return estimate


def _log_residual_summary(values: list[float], label: str) -> dict[str, object]:
    if not values:
        raise CalibrationError(f"{label}:no_samples")
    ordered = sorted(values)
    position = max(0, min(len(ordered) - 1, math.ceil(.95 * len(ordered)) - 1))
    middle = len(ordered) // 2
    median = (ordered[middle] if len(ordered) % 2 else
              (ordered[middle - 1] + ordered[middle]) / 2)
    if not all(math.isfinite(value) and value >= 0 for value in ordered):
        raise CalibrationError(f"{label}:invalid_residual")
    return {
        "count": len(ordered),
        "median_abs_log_ratio_error": median,
        "p95_abs_log_ratio_error": ordered[position],
        "max_abs_log_ratio_error": ordered[-1],
    }


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
    """Validate complete calibration slices and write a deterministic bundle."""
    request_root = request_path.parent.resolve()
    if bundle_path.parent.resolve() != request_root:
        raise CalibrationError("outputs:must_share_request_root")
    if manifest_path is not None and manifest_path.parent.resolve() != request_root:
        raise CalibrationError("outputs:must_share_request_root")
    request_raw, request_sha, request_bytes = _snapshot(request_path, "request", MAX_REQUEST_BYTES)
    request = parse_json(request_raw, "request")
    if not isinstance(request, dict) or set(request) != REQUEST_KEYS:
        raise CalibrationError("request:fields_invalid")
    if request["schema"] not in {REQUEST_SCHEMA, LEGACY_REQUEST_SCHEMA} or request["semantics"] != SEMANTICS:
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
    # A cell may legitimately be measured in more than one scheduling
    # context.  Context is part of the calibration identity; keying only by
    # cell would make a multi-context bundle impossible to construct.
    seen: set[tuple[str, str]] = set()
    input_bindings: list[dict[str, object]] = []
    # Context is part of the key.  Legacy rows are deliberately isolated in
    # C1F1/legacy so their aggregate factor can never leak into C1F20.
    context_bucket_ratios: dict[str, dict[str, dict[str, list[float]]]] = {}
    context_bucket_corpora: dict[str, dict[str, set[str]]] = {}
    f_log_ratios: dict[str, list[float]] = {}
    f_log_samples: list[tuple[str, str, float]] = []
    elapsed_groups: dict[str, list[float]] = {}
    metadata_identity: dict[str, str] | None = None
    metadata_by_record: list[tuple[str, str, str]] = []
    context_ids: set[str] = set()
    total_points = 0
    for index, item in enumerate(comparisons):
        if not isinstance(item, dict):
            raise CalibrationError(f"comparison:{index}:fields_invalid")
        topology, depth_class, legacy = _calibration_context(item, f"comparison:{index}")
        context = _context_id(topology, depth_class)
        context_ids.add(context)
        context_bucket_ratios.setdefault(context, {
            f"{bucket['profile']}/{bucket['regime']}": {
                "channel_bytes": [], "elapsed_ns": []
            } for bucket in CALIBRATION_BUCKETS
        })
        context_bucket_corpora.setdefault(context, {
            bucket_id: set() for bucket_id in CALIBRATION_BUCKET_IDS
        })
        cell = _cell(item["cell"], f"comparison:{index}.cell")
        cell_id = _cell_id(cell)
        if SPLITS[cell["corpus"]] != "calibration":
            raise CalibrationError(f"comparison:{index}:cell_not_calibration")
        if cell_id not in CALIBRATION_CELL_IDS:
            raise CalibrationError(f"comparison:{index}:cell_not_calibration")
        identity = (context, cell_id)
        if identity in seen:
            raise CalibrationError(
                f"comparison:{index}:duplicate_cell_context:{context}/{cell_id}")
        seen.add(identity)
        records_path, records_sha, records_bytes = _descriptor(
            item["records"], request_root, f"comparison:{index}.records")
        experiment_manifest_path = pass_id = authority_facts = authority_metadata = None
        if not legacy:
            experiment_manifest_path, pass_id, authority_facts, authority_metadata = _explicit_experiment_authority(
                records_path, cell, topology, depth_class, f"comparison:{index}")
        records_raw, actual_sha, actual_bytes = _snapshot(records_path, "records", MAX_RECORDS_BYTES)
        if actual_sha != records_sha or actual_bytes != records_bytes:
            raise CalibrationError(f"comparison:{index}:records_changed_after_authentication")
        predictive, live, _comparison, point_count, record_metadata = _records(
            records_raw, cell, predictor_identity["model_id"], f"comparison:{index}.records",
            require_directional=not legacy, require_metadata=not legacy)
        if not legacy:
            if metadata_identity is None:
                metadata_identity = authority_metadata
            if record_metadata != authority_metadata or record_metadata != metadata_identity:
                raise CalibrationError(f"comparison:{index}:calibration_metadata_mismatch")
            metadata_by_record.append((cell["corpus"], topology,
                                       "full" if depth_class == "repeat-full" else depth_class))
        p_curve = _curve(predictive["raw_cumulative_curve"], cell,
                         f"comparison:{index}.predictive", require_directional=not legacy)
        l_curve = _curve(live["raw_cumulative_curve"], cell,
                         f"comparison:{index}.live", require_directional=not legacy)
        for p_row, l_row in zip(p_curve, l_curve, strict=True):
            p_metrics, l_metrics = p_row["metrics"], l_row["metrics"]
            if p_metrics["channel_bytes"] <= 0 or p_metrics["elapsed_ns"] <= 0:
                raise CalibrationError(f"comparison:{index}:zero_predictive_metric")
            if l_metrics["channel_bytes"] <= 0 or l_metrics["elapsed_ns"] <= 0:
                raise CalibrationError(f"comparison:{index}:zero_live_metric")
            bucket = f"{cell['profile']}/{cell['regime']}"
            context_bucket_corpora[context][bucket].add(cell["corpus"])
            if not legacy:
                # Product batches expose the encoded source byte count as an
                # authoritative boundary value.  It is not a calibratable
                # quantity: predictive and live C-to-F bytes must agree
                # exactly at every joined point.
                if (p_metrics.get("C_TO_F_bytes", p_metrics["channel_bytes"]) !=
                        l_metrics.get("C_TO_F_bytes", l_metrics["channel_bytes"])):
                    raise CalibrationError(
                        f"comparison:{index}:C_TO_F_bytes_boundary_mismatch")
            for direction in ("C_TO_F_bytes", "F_TO_C_bytes"):
                p_direction = (p_metrics[direction] if not legacy
                               else p_metrics.get(direction, p_metrics["channel_bytes"]))
                l_direction = (l_metrics[direction] if not legacy
                               else l_metrics.get(direction, l_metrics["channel_bytes"]))
                if p_direction <= 0 or l_direction <= 0:
                    if direction == "F_TO_C_bytes" and not legacy:
                        raise CalibrationError(
                            f"comparison:{index}:F_TO_C_bytes_nonpositive")
                    if p_direction <= 0:
                        raise CalibrationError(f"comparison:{index}:ratio_nonpositive")
                ratio = float(l_direction) / float(p_direction)
                if direction == "F_TO_C_bytes" and not legacy:
                    fit_depth = "full" if depth_class == "repeat-full" else depth_class
                    log_ratio = math.log(ratio)
                    f_log_ratios.setdefault(fit_depth, []).append(log_ratio)
                    f_log_samples.append((cell["corpus"], fit_depth, log_ratio))
                else:
                    context_bucket_ratios[context][bucket].setdefault(direction, []).append(ratio)
            elapsed_ratio = float(l_metrics["elapsed_ns"]) / float(p_metrics["elapsed_ns"])
            context_bucket_ratios[context][bucket]["elapsed_ns"].append(elapsed_ratio)
            if not legacy:
                elapsed_groups.setdefault(f"{topology}/{bucket}", []).append(elapsed_ratio)
        total_points += point_count
        binding = {
            "cell": cell,
            "topology": topology,
            "depth_class": depth_class,
            "compatibility": "legacy_c1f1" if legacy else "explicit",
            "records_path": str(records_path.relative_to(request_root)),
            "records_sha256": records_sha,
            "records_bytes": records_bytes,
        }
        if not legacy:
            assert experiment_manifest_path is not None and pass_id is not None
            assert authority_facts is not None and authority_metadata is not None
            binding.update({
                "experiment_manifest": {
                    "path": str(experiment_manifest_path.relative_to(request_root)),
                    "sha256": authority_facts["sha256"],
                    "bytes": authority_facts["bytes"],
                },
                "pass_id": pass_id,
            })
        input_bindings.append(binding)
    legacy_request = context_ids == {"C1F1/legacy"}
    if legacy_request:
        for context in sorted(context_ids):
            missing = sorted(CALIBRATION_CELL_IDS - {
                cell_id for context_id, cell_id in seen if context_id == context
            })
            if missing:
                raise CalibrationError(
                    f"comparisons:missing_cells:{context}:{','.join(missing)}")
        expected_points = len(CALIBRATION_CELLS)
        if len(comparisons) != expected_points or len(seen) != expected_points:
            raise CalibrationError("comparisons:expected_complete_16_cell_matrix")
    else:
        # A scoring bundle admits only complete preregistered slices: both
        # calibration corpora x both topologies x all four depth classes for
        # every included profile/regime.  Independent buckets may be frozen
        # incrementally, but no partial context slice is accepted.
        if context_ids != set(EXPLICIT_CONTEXTS):
            missing = sorted(set(EXPLICIT_CONTEXTS) - context_ids)
            raise CalibrationError(
                f"comparisons:missing_complete_context_slice:{','.join(missing)}")
        included_buckets = {
            cell_id.split("/", 1)[1]
            for _context, cell_id in seen
        }
        expected = len(included_buckets) * len(EXPLICIT_CONTEXTS) * 2
        for bucket in sorted(included_buckets):
            present = {
                (context, cell_id.split("/", 1)[0])
                for context, cell_id in seen if cell_id.endswith(f"/{bucket}")
            }
            required = {(context, corpus) for context in EXPLICIT_CONTEXTS
                        for corpus in ("fmt", "RocksDB")}
            if present != required:
                raise CalibrationError(f"comparisons:partial_complete_slice:{bucket}")
        if len(comparisons) != expected or len(seen) != expected:
            raise CalibrationError("comparisons:expected_complete_slice_per_bucket")
    input_bindings.sort(key=lambda item: (
        _context_id(str(item["topology"]), str(item["depth_class"])),
        _cell_id(item["cell"])))
    scales: dict[str, dict[str, float]] = {}
    f_factors: dict[str, float] = {}
    if not legacy_request:
        for depth_class in FTOC_DEPTH_CLASSES:
            f_factors[depth_class] = _geometric_median(
                f_log_ratios.get(depth_class, []),
                f"F_TO_C_bytes/{depth_class}")
    scale_bucket_ids = (sorted(CALIBRATION_BUCKET_IDS) if legacy_request else
                        sorted({cell_id.split("/", 1)[1] for _context, cell_id in seen}))
    for context in sorted(context_ids):
        for bucket_id in scale_bucket_ids:
            ratios = context_bucket_ratios[context][bucket_id]
            if (context_bucket_corpora[context][bucket_id] != {"fmt", "RocksDB"} or
                    len(ratios.get("C_TO_F_bytes", [])) < 2 or
                    (legacy_request and len(ratios.get("F_TO_C_bytes", [])) < 2) or
                    len(ratios["elapsed_ns"]) < 2):
                raise CalibrationError(f"bucket:{context}/{bucket_id}:expected_two_corpora")
            key = f"{context}/{bucket_id}"
            # Existing records expose only aggregate channel bytes.  A
            # migrated legacy fixture therefore intentionally uses the same
            # factor for both directions; explicit v2 evidence must provide
            # directional factors (see FACTOR_FIELDS below).
            c_factor = (1.0 if context != "C1F1/legacy" else
                        _median(ratios.get("C_TO_F_bytes", ratios["channel_bytes"]),
                                f"{key}:C_TO_F_bytes"))
            f_factor = (_geometric_median(ratios.get("F_TO_C_bytes", ratios["channel_bytes"]),
                                          f"{key}:F_TO_C_bytes")
                        if legacy_request else f_factors[
                            "full" if context.split("/", 1)[1] == "repeat-full"
                            else context.split("/", 1)[1]])
            elapsed_samples = (ratios["elapsed_ns"] if legacy_request else
                               elapsed_groups.get(f"{context.split('/', 1)[0]}/{bucket_id}", []))
            elapsed_factor = _median(elapsed_samples, f"{key}:elapsed_ns")
            if context == "C1F1/legacy" and context_ids == {"C1F1/legacy"}:
                # Read-only aliases keep older C1F1 callers source-compatible
                # while the canonical v2 key remains topology/depth scoped.
                scales[bucket_id] = {
                    "channel_bytes": c_factor,
                    "elapsed_ns": elapsed_factor,
                }
            else:
                scales[key] = {"C_TO_F_bytes": c_factor, "F_TO_C_bytes": f_factor,
                               "elapsed_ns": elapsed_factor}
    if legacy_request:
        diagnostics = {
            "leave_one_corpus_out": {corpus: {"excluded": corpus, "samples": 0,
                                                "non_vacuous": False}
                                     for corpus in CORPORA},
            "leave_one_depth_out": {"legacy": {"excluded": "legacy", "samples": 0,
                                                  "non_vacuous": False}},
        }
    else:
        corpus_diagnostics: dict[str, object] = {}
        for excluded in ("fmt", "RocksDB"):
            train_by_depth = {
                depth: [log_ratio for corpus, depth_value, log_ratio in f_log_samples
                        if corpus != excluded and depth_value == depth]
                for depth in FTOC_DEPTH_CLASSES
            }
            heldout = [(depth, log_ratio) for corpus, depth, log_ratio in f_log_samples
                       if corpus == excluded]
            residuals = [abs(log_ratio - math.log(_geometric_median(
                train_by_depth[depth], f"diagnostic.corpus.{excluded}.{depth}")))
                         for depth, log_ratio in heldout]
            summary = _log_residual_summary(residuals, f"diagnostic.corpus.{excluded}")
            summary.update({"excluded": excluded, "non_vacuous": True})
            corpus_diagnostics[excluded] = summary
        depth_diagnostics: dict[str, object] = {}
        for excluded in FTOC_DEPTH_CLASSES:
            train = [log_ratio for _corpus, depth, log_ratio in f_log_samples
                     if depth != excluded]
            heldout = [log_ratio for _corpus, depth, log_ratio in f_log_samples
                       if depth == excluded]
            fit = math.log(_geometric_median(train, f"diagnostic.depth.{excluded}"))
            summary = _log_residual_summary([abs(log_ratio - fit) for log_ratio in heldout],
                                            f"diagnostic.depth.{excluded}")
            summary.update({"excluded": excluded, "non_vacuous": True})
            depth_diagnostics[excluded] = summary
        diagnostics = {"leave_one_corpus_out": corpus_diagnostics,
                       "leave_one_depth_out": depth_diagnostics}
    bundle = {
        "schema": BUNDLE_SCHEMA,
        "semantics": SEMANTICS,
        "request": {"sha256": request_sha, "bytes": request_bytes},
        "predictor": predictor_identity,
        "calibration": {
            "schema": "icecream-s8-calibration-factors-v2",
            "method": "median_live_over_predictive_v1",
            "cells": len(input_bindings),
            "points": total_points,
            "scales": scales,
            "factor_fields": list(FACTOR_FIELDS),
            "contexts": sorted(context_ids),
            "identity": metadata_identity,
            "factor_scopes": {
                "C_TO_F_bytes": "exact_identity_1.0",
                "F_TO_C_bytes": "product_image_digest/toolchain_digest/output_contract_digest/ordered_input_class/depth_class",
                "elapsed_ns": "host_digest/product_image_digest/topology/profile/regime",
            },
            "diagnostics": diagnostics,
            "legacy_contexts": sorted(context for context in context_ids
                                       if context.endswith("/legacy")),
            "derived_metrics": {
                "throughput_bytes_per_s": "(C_TO_F_bytes + F_TO_C_bytes) * 1000000000 / elapsed_ns",
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
