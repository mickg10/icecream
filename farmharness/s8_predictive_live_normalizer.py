#!/usr/bin/env python3
"""Normalize authenticated S8 predictive and live cumulative curves.

This module is a comparison boundary, not a producer.  It reads two exact
curve manifests and their authenticated JSONL curves: one ``predictive_sim``
artifact produced without live traces and one independent ``live``
observation artifact.  It emits three records (predictive_sim, live, and
comparison) in one JSONL file.  The first two records retain their raw
cumulative rows; only the comparison record contains point errors and a
cumulative loss curve.

No model, simulator, Docker process, route trace, or action trace is opened
or executed here.  The input manifests deliberately describe only curves
and their run identity.
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

try:  # Works both as a module and as a directly invoked harness script.
    from .s8_schema import (CORPORA, CURRENT_SEMANTICS, DEPTH_CLASSES, PROFILES,
                            REGIMES, SPLITS, TOPOLOGIES)
except ImportError:  # pragma: no cover - exercised by direct script runners.
    from s8_schema import (CORPORA, CURRENT_SEMANTICS, DEPTH_CLASSES, PROFILES,
                           REGIMES, SPLITS, TOPOLOGIES)


MANIFEST_SCHEMA = "icecream-s8-curve-manifest-v1"
RECORD_SCHEMA = "icecream-s8-predictive-live-record-v1"
SEMANTICS = CURRENT_SEMANTICS
MANIFEST_KEYS = {"schema", "identity", "units", "curve", "provenance"}
OPTIONAL_MANIFEST_KEYS = {"evidence", "comparison", "topology", "depth_class", "pass_id",
                          "execution_scope", "role_placement"}
CALIBRATION_METADATA_KEYS = {
    "product_image_digest", "toolchain_digest", "output_contract_digest",
    "host_digest", "ordered_input_class",
}
OPTIONAL_MANIFEST_KEYS |= CALIBRATION_METADATA_KEYS
IDENTITY_KEYS = {
    "corpus", "profile", "regime", "split", "run_id", "source_commit",
    "source_tree", "input_digest", "topology_digest", "model_id",
}
JOIN_IDENTITY_KEYS = IDENTITY_KEYS - {"model_id"}
REQUIRED_UNITS_KEYS = {"point", "channel_bytes", "elapsed_ns"}
THROUGHPUT_UNITS_KEY = "throughput_bytes_per_s"
DIRECTIONAL_UNITS_KEYS = {"C_TO_F_bytes", "F_TO_C_bytes"}
UNIT_KEY_OPTIONS = {
    frozenset(REQUIRED_UNITS_KEYS),
    frozenset((*REQUIRED_UNITS_KEYS, THROUGHPUT_UNITS_KEY)),
    frozenset((*REQUIRED_UNITS_KEYS, THROUGHPUT_UNITS_KEY, *DIRECTIONAL_UNITS_KEYS)),
}
CANONICAL_UNITS = {
    "point": "step",
    "channel_bytes": "bytes",
    "elapsed_ns": "ns",
    "throughput_bytes_per_s": "bytes_per_s",
    "C_TO_F_bytes": "bytes",
    "F_TO_C_bytes": "bytes",
}
DESCRIPTOR_KEYS = {"path", "sha256", "bytes"}
PROVENANCE_KEYS = {"mode", "producer", "trace_free"}
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")
HEX40 = re.compile(r"^[0-9a-fA-F]{40}$")
SAFE_ID = re.compile(r"^[A-Za-z0-9_.-]+$")
MAX_MANIFEST_BYTES = 1 * 1024 * 1024
MAX_CURVE_BYTES = 64 * 1024 * 1024
LIVE_EXPERIMENT_SCHEMA = "icecream-s8-real-c1f1-live-runner-v2"
COMPARISON_SCHEMA = "icecream-s8-plan-capture-join-v1"
PASS_ID = re.compile(r"^[A-Za-z0-9_.:-]+$")
ORDERED_INPUT_CLASSES = frozenset(("ordered",))
EXECUTION_SCOPES = frozenset(("loopback_correctness_only", "external_farm_timing"))
ROLE_PLACEMENT_SCHEMA = "icecream-s8-role-placement-v1"
ROLE_PLACEMENT_MODES = frozenset(("co_resident_loopback", "external_farm"))


class NormalizationError(ValueError):
    """Raised for any unauthenticated, ambiguous, or mismatched input."""


def _validate_calibration_metadata(value: object,
                                   label: str = "calibration_metadata") -> dict[str, str]:
    if not isinstance(value, dict) or set(value) != CALIBRATION_METADATA_KEYS:
        raise NormalizationError(f"{label}:fields_invalid")
    result: dict[str, str] = {}
    for field in CALIBRATION_METADATA_KEYS - {"ordered_input_class"}:
        result[field] = _sha(value[field], f"{label}.{field}")
    if value["ordered_input_class"] not in ORDERED_INPUT_CLASSES:
        raise NormalizationError(f"{label}.ordered_input_class:invalid")
    result["ordered_input_class"] = value["ordered_input_class"]
    return result


def load_calibration_metadata_manifest(path: Path) -> dict[str, str]:
    """Load the measured host/toolchain authority emitted by the live runner."""
    raw, _facts = _snapshot(path, "calibration_metadata_manifest", MAX_MANIFEST_BYTES)
    value = parse_json(raw, "calibration_metadata_manifest")
    if not isinstance(value, dict) or value.get("schema") != LIVE_EXPERIMENT_SCHEMA:
        raise NormalizationError("calibration_metadata_manifest:schema_invalid")
    return _validate_calibration_metadata(
        value.get("calibration_metadata"), "calibration_metadata_manifest.calibration_metadata")


def canonical_bytes(value: object) -> bytes:
    try:
        return json.dumps(
            value,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
            allow_nan=False,
        ).encode("ascii")
    except (TypeError, ValueError, OverflowError, UnicodeError) as exc:
        raise NormalizationError("canonical_json:invalid_value") from exc


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise NormalizationError(f"duplicate_json_key:{key}")
        result[key] = value
    return result


def parse_json(raw: bytes, label: str) -> object:
    try:
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=lambda value: (_ for _ in ()).throw(
                NormalizationError(f"{label}:non_finite_json:{value}")
            ),
        )
    except NormalizationError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise NormalizationError(f"{label}:invalid_json") from exc


def _snapshot(path: Path, label: str, limit: int) -> tuple[bytes, dict[str, object]]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise NormalizationError(f"{label}:unavailable:{path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise NormalizationError(f"{label}:not_private_regular_file:{path}")
    if info.st_nlink != 1:
        raise NormalizationError(f"{label}:hard_link_alias:{path}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise NormalizationError(f"{label}:cannot_open:{path}") from exc
    try:
        before = os.fstat(fd)
        digest = hashlib.sha256()
        chunks: list[bytes] = []
        total = 0
        while True:
            try:
                block = os.read(fd, 1 << 20)
            except OSError as exc:
                raise NormalizationError(f"{label}:read_failed:{path}") from exc
            if not block:
                break
            total += len(block)
            if total > limit:
                raise NormalizationError(f"{label}:too_large")
            chunks.append(block)
            digest.update(block)
        after = os.fstat(fd)
        if any(
            getattr(before, field) != getattr(after, field)
            for field in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")
        ):
            raise NormalizationError(f"{label}:changed_while_reading:{path}")
        return b"".join(chunks), {"sha256": digest.hexdigest(), "bytes": total}
    finally:
        os.close(fd)


def _sha(value: object, label: str, pattern: re.Pattern[str] = HEX64) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        raise NormalizationError(f"{label}:invalid_digest")
    normalized = value.lower()
    if int(normalized, 16) == 0:
        raise NormalizationError(f"{label}:zero_digest")
    return normalized


def _descriptor_path(base: Path, value: object, label: str) -> Path:
    if not isinstance(value, dict) or set(value) != DESCRIPTOR_KEYS:
        raise NormalizationError(f"{label}:descriptor_invalid")
    relative = value["path"]
    if not isinstance(relative, str) or not relative:
        raise NormalizationError(f"{label}:path_missing")
    candidate = Path(relative)
    if candidate.is_absolute() or any(part in ("", ".", "..") for part in candidate.parts):
        raise NormalizationError(f"{label}:path_not_private_relative")
    path = base / candidate
    try:
        path.resolve().relative_to(base.resolve())
    except ValueError as exc:
        raise NormalizationError(f"{label}:path_escapes_manifest_root") from exc
    _sha(value["sha256"], f"{label}.sha256")
    if type(value["bytes"]) is not int or value["bytes"] <= 0:
        raise NormalizationError(f"{label}.bytes:invalid")
    return path


def _authenticate(path: Path, descriptor: dict[str, object], label: str) -> tuple[bytes, str]:
    raw, facts = _snapshot(path, label, MAX_CURVE_BYTES)
    expected_sha = _sha(descriptor["sha256"], f"{label}.sha256")
    if facts["sha256"] != expected_sha:
        raise NormalizationError(f"{label}:sha256_mismatch")
    if facts["bytes"] != descriptor["bytes"]:
        raise NormalizationError(f"{label}:byte_count_mismatch")
    return raw, expected_sha


def _safe_id(value: object, label: str) -> str:
    if not isinstance(value, str) or not value or SAFE_ID.fullmatch(value) is None:
        raise NormalizationError(f"{label}:invalid_identifier")
    return value


def _validate_identity(value: object) -> dict[str, str]:
    if not isinstance(value, dict) or set(value) != IDENTITY_KEYS:
        raise NormalizationError("identity:fields_invalid")
    result = {
        field: _safe_id(value[field], f"identity.{field}")
        for field in ("corpus", "profile", "regime", "run_id", "model_id")
    }
    split = value["split"]
    if split not in {"calibration", "held_out_validation"}:
        raise NormalizationError("identity.split:must_be_calibration_or_held_out_validation")
    if result["corpus"] not in CORPORA or result["profile"] not in PROFILES or result["regime"] not in REGIMES:
        raise NormalizationError("identity:cell_not_declared")
    if split != SPLITS[result["corpus"]]:
        raise NormalizationError("identity.split:cell_policy_mismatch")
    result["split"] = split
    result["source_commit"] = _sha(value["source_commit"], "identity.source_commit", HEX40)
    result["source_tree"] = _sha(value["source_tree"], "identity.source_tree", HEX40)
    result["input_digest"] = _sha(value["input_digest"], "identity.input_digest")
    result["topology_digest"] = _sha(value["topology_digest"], "identity.topology_digest")
    return result


def _validate_units(value: object) -> dict[str, str]:
    if not isinstance(value, dict):
        raise NormalizationError("units:fields_invalid")
    directional = DIRECTIONAL_UNITS_KEYS & set(value)
    if directional and directional != DIRECTIONAL_UNITS_KEYS:
        raise NormalizationError("units:directional_fields_must_be_paired")
    if frozenset(value) not in UNIT_KEY_OPTIONS:
        raise NormalizationError("units:fields_invalid")
    result = {field: _safe_id(value[field], f"units.{field}") for field in value}
    for field, expected in CANONICAL_UNITS.items():
        if field in result and result[field] != expected:
            raise NormalizationError(f"units.{field}:unexpected_unit")
    return result


def _validate_provenance(value: object, mode: str) -> dict[str, object]:
    if not isinstance(value, dict) or set(value) != PROVENANCE_KEYS:
        raise NormalizationError("provenance:fields_invalid")
    if value["mode"] != mode or value["trace_free"] is not (mode == "predictive_sim"):
        raise NormalizationError(f"provenance:{mode}:mode_or_trace_free_invalid")
    producer = _safe_id(value["producer"], "provenance.producer")
    return {"mode": mode, "producer": producer, "trace_free": value["trace_free"]}


def comparison_descriptor(plan_sha256: str, scheduling: object) -> dict[str, str]:
    """Derive the only join key from the authenticated predictive plan.

    Producer run IDs and source identities remain local to each producer.  The
    plan digest is paired with a normalized scheduling projection so an
    input-only plan cannot be reused to join an unrelated topology capture.
    """
    plan_sha256 = _sha(plan_sha256, "comparison.plan_sha256")
    if not isinstance(scheduling, dict):
        raise NormalizationError("comparison:scheduling_missing")
    assignments = scheduling.get("assignments")
    topology = scheduling.get("topology")
    if (not isinstance(topology, str) or not topology or
            not isinstance(assignments, list) or not assignments):
        raise NormalizationError("comparison:scheduling_projection_invalid")
    projection: dict[str, object] = {"topology": topology, "assignments": assignments}
    # New depth plans bind their calibration class into the comparison key.
    # Omit it for legacy scheduling objects so historical artifacts retain
    # their original join identity.
    if "depth_class" in scheduling:
        depth_class = scheduling["depth_class"]
        if not isinstance(depth_class, str) or not depth_class:
            raise NormalizationError("comparison:scheduling_depth_class_invalid")
        projection["depth_class"] = depth_class
    scheduling_sha256 = hashlib.sha256(canonical_bytes(projection)).hexdigest()
    comparison_id = hashlib.sha256(canonical_bytes(
        {"schema": COMPARISON_SCHEMA, "plan_sha256": plan_sha256,
         "scheduling_sha256": scheduling_sha256}
    )).hexdigest()
    return {"schema": COMPARISON_SCHEMA, "plan_sha256": plan_sha256,
            "scheduling_sha256": scheduling_sha256, "comparison_id": comparison_id}


def _validate_comparison(value: object) -> dict[str, str]:
    if not isinstance(value, dict) or set(value) != {
            "schema", "plan_sha256", "scheduling_sha256", "comparison_id"}:
        raise NormalizationError("comparison:fields_invalid")
    if value["schema"] != COMPARISON_SCHEMA:
        raise NormalizationError("comparison:schema_invalid")
    plan_sha = _sha(value["plan_sha256"], "comparison.plan_sha256")
    scheduling_sha = _sha(value["scheduling_sha256"], "comparison.scheduling_sha256")
    comparison_id = _sha(value["comparison_id"], "comparison.comparison_id")
    # The scheduling projection is checked against the authenticated plan by
    # the producer/runner before this descriptor is emitted.  Here we still
    # bind the capture ID to both digests and reject self-inconsistent data.
    expected = hashlib.sha256(canonical_bytes(
        {"schema": COMPARISON_SCHEMA, "plan_sha256": plan_sha,
         "scheduling_sha256": scheduling_sha}
    )).hexdigest()
    if comparison_id != expected or int(scheduling_sha, 16) == 0:
        raise NormalizationError("comparison:plan_binding_mismatch")
    return {"schema": COMPARISON_SCHEMA, "plan_sha256": plan_sha,
            "scheduling_sha256": scheduling_sha, "comparison_id": comparison_id}


def _validate_evidence(value: object) -> dict[str, object]:
    """Validate optional producer evidence without treating it as a curve.

    Older S8 manifests intentionally have no evidence envelope.  New live
    producers may attach this small, content-addressed envelope; its hashes
    are retained in normalized provenance and are never inferred here.
    """
    if not isinstance(value, dict) or set(value) != {
            "results_sha256", "evidence_manifest_sha256", "binary_sha256",
            "evidence_sha256"}:
        raise NormalizationError("evidence:fields_invalid")
    for field in ("results_sha256", "evidence_manifest_sha256", "evidence_sha256"):
        _sha(value[field], f"evidence.{field}")
    binaries = value["binary_sha256"]
    if not isinstance(binaries, dict) or not binaries:
        raise NormalizationError("evidence.binary_sha256:invalid")
    for name, digest in binaries.items():
        if not isinstance(name, str) or not name:
            raise NormalizationError("evidence.binary_sha256:name_invalid")
        _sha(digest, f"evidence.binary_sha256.{name}")
    return {
        "results_sha256": value["results_sha256"].lower(),
        "evidence_manifest_sha256": value["evidence_manifest_sha256"].lower(),
        "binary_sha256": {name: str(digest).lower() for name, digest in binaries.items()},
        "evidence_sha256": value["evidence_sha256"].lower(),
    }


def _validate_manifest_metadata(value: dict[str, object]) -> dict[str, str]:
    """Validate producer-declared campaign identity without inferring it.

    Curve manifests from the original S8 producers predate these fields, so
    they remain optional.  When present they are copied verbatim into all
    three records and joined between predictive and live inputs.  This keeps
    topology/depth/pass labels authenticated instead of silently replacing
    them with a driver's defaults.
    """
    result: dict[str, str] = {}
    if "topology" in value:
        topology = value["topology"]
        if not isinstance(topology, str) or topology not in TOPOLOGIES:
            raise NormalizationError("manifest.topology:invalid")
        result["topology"] = topology
    if "depth_class" in value:
        depth = value["depth_class"]
        if not isinstance(depth, str) or depth not in set(DEPTH_CLASSES) - {"legacy"}:
            raise NormalizationError("manifest.depth_class:invalid")
        result["depth_class"] = depth
    if "pass_id" in value:
        pass_id = value["pass_id"]
        if not isinstance(pass_id, str) or not pass_id or PASS_ID.fullmatch(pass_id) is None:
            raise NormalizationError("manifest.pass_id:invalid")
        result["pass_id"] = pass_id
    calibration_fields = CALIBRATION_METADATA_KEYS & set(value)
    if calibration_fields and calibration_fields != CALIBRATION_METADATA_KEYS:
        missing = ",".join(sorted(CALIBRATION_METADATA_KEYS - calibration_fields))
        raise NormalizationError(f"manifest.calibration_metadata_missing:{missing}")
    if calibration_fields:
        result.update(_validate_calibration_metadata(
            {field: value[field] for field in CALIBRATION_METADATA_KEYS}, "manifest"))
    return result


def _validate_execution_scope(value: object, mode: str) -> str | None:
    """Validate the producer's timing authority without inferring it.

    A local C/F process pair can prove protocol bytes and output identity, but
    its elapsed time includes co-resident scheduler, F, cache, and compiler
    work.  Such a curve is retained as a loopback correctness observation and
    is never eligible for calibration.  A calibration live curve must name an
    independently supplied farm explicitly.
    """
    if value is None:
        return None
    if not isinstance(value, str) or value not in EXECUTION_SCOPES:
        raise NormalizationError("execution_scope:invalid")
    if mode == "predictive_sim":
        raise NormalizationError("execution_scope:predictive_sim_invalid")
    return value


def _validate_role_placement(value: object, mode: str) -> dict[str, object] | None:
    """Validate the authenticated placement of submission and compile roles."""
    if value is None:
        return None
    if not isinstance(value, dict) or set(value) != {
            "schema", "mode", "c_host_digest", "scheduler_host_digest",
            "f_host_digests", "roles_disjoint", "timing_eligible"}:
        raise NormalizationError("role_placement:fields_invalid")
    if value.get("schema") != ROLE_PLACEMENT_SCHEMA or \
            value.get("mode") not in ROLE_PLACEMENT_MODES:
        raise NormalizationError("role_placement:identity_invalid")
    c_digest = _sha(value.get("c_host_digest"), "role_placement.c_host_digest")
    scheduler_digest = _sha(value.get("scheduler_host_digest"),
                             "role_placement.scheduler_host_digest")
    f_digests = value.get("f_host_digests")
    if (not isinstance(f_digests, list) or not f_digests or
            any(not isinstance(item, str) for item in f_digests)):
        raise NormalizationError("role_placement.f_host_digests:invalid")
    f_digests = [_sha(item, "role_placement.f_host_digest") for item in f_digests]
    roles_disjoint = value.get("roles_disjoint")
    timing_eligible = value.get("timing_eligible")
    if type(roles_disjoint) is not bool or type(timing_eligible) is not bool:
        raise NormalizationError("role_placement:flags_invalid")
    placement_mode = value["mode"]
    if placement_mode == "co_resident_loopback":
        if (roles_disjoint or timing_eligible or
                scheduler_digest != c_digest or any(item != c_digest for item in f_digests)):
            raise NormalizationError("role_placement:loopback_identity_invalid")
    elif (not roles_disjoint or not timing_eligible or
          any(item in {c_digest, scheduler_digest} for item in f_digests) or
          len(set(f_digests)) != len(f_digests)):
        raise NormalizationError("role_placement:external_identity_invalid")
    if mode == "predictive_sim":
        raise NormalizationError("role_placement:predictive_sim_invalid")
    return {"schema": ROLE_PLACEMENT_SCHEMA, "mode": placement_mode,
            "c_host_digest": c_digest, "scheduler_host_digest": scheduler_digest,
            "f_host_digests": f_digests, "roles_disjoint": roles_disjoint,
            "timing_eligible": timing_eligible}


def _forbidden_curve_key(key: object) -> bool:
    if not isinstance(key, str):
        return True
    lowered = key.lower()
    return any(token in lowered for token in ("route", "action", "trace"))


def _reject_trace_fields(value: object, label: str) -> None:
    if isinstance(value, dict):
        for key, nested in value.items():
            if _forbidden_curve_key(key):
                raise NormalizationError(f"{label}:trace_or_action_field")
            _reject_trace_fields(nested, label)
    elif isinstance(value, list):
        for nested in value:
            _reject_trace_fields(nested, label)


def _flatten_numeric(value: object, prefix: str) -> dict[str, int | float]:
    if isinstance(value, dict):
        if not value:
            raise NormalizationError(f"curve.{prefix}:empty_object")
        result: dict[str, int | float] = {}
        for key, nested in value.items():
            if _forbidden_curve_key(key):
                raise NormalizationError(f"curve.{prefix}:trace_or_action_field")
            if not isinstance(key, str) or not key:
                raise NormalizationError(f"curve.{prefix}:invalid_metric_key")
            result.update(_flatten_numeric(nested, f"{prefix}.{key}"))
        return result
    if type(value) not in (int, float):
        raise NormalizationError(f"curve.{prefix}:metric_not_finite_number")
    try:
        finite = math.isfinite(float(value))
    except (OverflowError, ValueError):
        finite = False
    if not finite:
        raise NormalizationError(f"curve.{prefix}:metric_not_finite_number")
    if value < 0:
        raise NormalizationError(f"curve.{prefix}:metric_negative")
    return {prefix: value}


def _parse_curve(raw: bytes, label: str, identity: dict[str, str]) -> tuple[list[dict[str, object]], list[dict[str, int | float]], list[str]]:
    rows: list[dict[str, object]] = []
    metrics: list[dict[str, int | float]] = []
    previous_step: int | None = None
    seen_steps: set[int] = set()
    seen_tus: set[str] = set()
    for line_number, line in enumerate(raw.splitlines(), 1):
        if not line.strip():
            raise NormalizationError(f"{label}:blank_line:{line_number}")
        value = parse_json(line, f"{label}:{line_number}")
        if not isinstance(value, dict) or {"step", "tu_id", "cumulative"} - set(value):
            raise NormalizationError(f"{label}:{line_number}:point_fields_invalid")
        _reject_trace_fields(value, f"{label}:{line_number}")
        expected_cell = {field: identity[field] for field in ("corpus", "profile", "regime")}
        if "cell" in value and value["cell"] != expected_cell:
            raise NormalizationError(f"{label}:{line_number}:cell_mismatch")
        if "model_id" in value and value["model_id"] != identity["model_id"]:
            raise NormalizationError(f"{label}:{line_number}:model_id_mismatch")
        step = value["step"]
        tu_id = value["tu_id"]
        if type(step) is not int or step < 0:
            raise NormalizationError(f"{label}:{line_number}:step_invalid")
        if not isinstance(tu_id, str) or not tu_id:
            raise NormalizationError(f"{label}:{line_number}:tu_id_invalid")
        if step in seen_steps or tu_id in seen_tus:
            raise NormalizationError(f"{label}:{line_number}:duplicate_point")
        if previous_step is not None and step != previous_step + 1:
            raise NormalizationError(f"{label}:{line_number}:reordered_or_missing_point")
        cumulative = value["cumulative"]
        if not isinstance(cumulative, dict):
            raise NormalizationError(f"{label}:{line_number}:cumulative_curve_invalid")
        flat = _flatten_numeric(cumulative, "cumulative")
        if metrics and set(flat) != set(metrics[0]):
            raise NormalizationError(f"{label}:{line_number}:metric_shape_changed")
        if metrics:
            for key, current in flat.items():
                # Throughput is a derived instantaneous/aggregate rate;
                # unlike elapsed time and byte counters it may decrease.
                if ("throughput" not in key and
                        current < metrics[-1][key]):
                    raise NormalizationError(f"{label}:{line_number}:cumulative_value_decreased")
        rows.append(value)
        metrics.append(flat)
        seen_steps.add(step)
        seen_tus.add(tu_id)
        previous_step = step
    if not rows:
        raise NormalizationError(f"{label}:empty_curve")
    if rows[0]["step"] != 0:
        raise NormalizationError(f"{label}:reordered_or_missing_point")
    return rows, metrics, sorted(metrics[0])


def _load_manifest(path: Path, mode: str) -> dict[str, object]:
    raw, facts = _snapshot(path, "manifest", MAX_MANIFEST_BYTES)
    value = parse_json(raw, "manifest")
    if not isinstance(value, dict) or not MANIFEST_KEYS.issubset(value) or \
            set(value) - MANIFEST_KEYS - OPTIONAL_MANIFEST_KEYS:
        raise NormalizationError("manifest:fields_invalid")
    if value["schema"] != MANIFEST_SCHEMA:
        raise NormalizationError("manifest:schema_invalid")
    identity = _validate_identity(value["identity"])
    comparison = (_validate_comparison(value["comparison"])
                  if "comparison" in value else None)
    units = _validate_units(value["units"])
    provenance = _validate_provenance(value["provenance"], mode)
    metadata = _validate_manifest_metadata(value)
    execution_scope = _validate_execution_scope(value.get("execution_scope"), mode)
    role_placement = _validate_role_placement(value.get("role_placement"), mode)
    evidence = (_validate_evidence(value["evidence"])
                if "evidence" in value else None)
    curve_path = _descriptor_path(path.parent, value["curve"], "curve")
    if curve_path.resolve() == path.resolve():
        raise NormalizationError("curve:manifest_alias")
    curve_raw, curve_sha = _authenticate(curve_path, value["curve"], f"{mode}_curve")
    rows, metrics, metric_keys = _parse_curve(curve_raw, f"{mode}_curve", identity)
    return {
        "manifest_path": path.resolve(),
        "curve_path": curve_path.resolve(),
        "identity": identity,
        "comparison": comparison,
        "units": units,
        "provenance": provenance,
        "evidence": evidence,
        "metadata": metadata,
        "execution_scope": execution_scope,
        "role_placement": role_placement,
        "manifest_sha256": facts["sha256"],
        "curve_sha256": curve_sha,
        "rows": rows,
        "metrics": metrics,
        "metric_keys": metric_keys,
    }


def _same_identity(left: dict[str, str], right: dict[str, str],
                   left_comparison: dict[str, str] | None = None,
                   right_comparison: dict[str, str] | None = None) -> None:
    # Independent producers necessarily have different run/source/topology
    # identities.  The exact authenticated plan is the shared capture key;
    # cell and input identity remain explicit defense-in-depth checks.
    fields = ("corpus", "profile", "regime", "split", "input_digest")
    if left_comparison is None or right_comparison is None:
        # Legacy artifacts remain fail-closed: without an authenticated plan
        # key, require the complete historical producer identity.
        fields = tuple(JOIN_IDENTITY_KEYS)
    for field in fields:
        if left[field] != right[field]:
            raise NormalizationError(f"identity_mismatch:{field}")
    if left_comparison is not None and right_comparison is not None and left_comparison != right_comparison:
        raise NormalizationError("comparison_mismatch:plan_or_capture")


def _same_units(left: dict[str, str], right: dict[str, str]) -> None:
    for field in set(left) | set(right):
        if left.get(field) != right.get(field):
            raise NormalizationError(f"units_mismatch:{field}")


def _same_metadata(left: dict[str, str], right: dict[str, str]) -> None:
    if left != right:
        for field in sorted(set(left) | set(right)):
            if left.get(field) != right.get(field):
                raise NormalizationError(f"manifest_metadata_mismatch:{field}")


def _comparison(predicted: dict[str, object], observed: dict[str, object], identity: dict[str, str], units: dict[str, str]) -> dict[str, object]:
    p_rows = predicted["rows"]
    o_rows = observed["rows"]
    p_metrics = predicted["metrics"]
    o_metrics = observed["metrics"]
    assert isinstance(p_rows, list) and isinstance(o_rows, list)
    assert isinstance(p_metrics, list) and isinstance(o_metrics, list)
    if len(p_rows) != len(o_rows):
        raise NormalizationError("curve_alignment:missing_point")
    if predicted["metric_keys"] != observed["metric_keys"]:
        raise NormalizationError("curve_alignment:metric_shape_mismatch")
    point_errors: list[dict[str, object]] = []
    loss_curve: list[dict[str, object]] = []
    cumulative_loss = 0.0
    for index, (p_row, o_row, p_values, o_values) in enumerate(
        zip(p_rows, o_rows, p_metrics, o_metrics, strict=True)
    ):
        assert isinstance(p_row, dict) and isinstance(o_row, dict)
        if p_row["step"] != o_row["step"] or p_row["tu_id"] != o_row["tu_id"]:
            raise NormalizationError(f"curve_alignment:point_identity_mismatch:{index}")
        assert isinstance(p_values, dict) and isinstance(o_values, dict)
        errors: dict[str, object] = {}
        point_squared = 0.0
        for key in predicted["metric_keys"]:
            assert isinstance(key, str)
            p_value = p_values[key]
            o_value = o_values[key]
            signed = p_value - o_value
            absolute = abs(signed)
            relative: float | None
            if o_value == 0:
                relative = 0.0 if signed == 0 else None
            else:
                relative = signed / abs(o_value)
            squared = signed * signed
            point_squared += float(squared)
            errors[key] = {
                "signed": signed,
                "absolute": absolute,
                "relative": relative,
                "squared": squared,
            }
        cumulative_loss += point_squared
        point_errors.append({"step": p_row["step"], "tu_id": p_row["tu_id"], "errors": errors})
        loss_curve.append({
            "step": p_row["step"],
            "tu_id": p_row["tu_id"],
            "squared_error": point_squared,
            "cumulative_loss": cumulative_loss,
        })
    return {
        "schema": RECORD_SCHEMA,
        "semantics": SEMANTICS,
        "record_type": "comparison",
        "cell": {field: identity[field] for field in ("corpus", "profile", "regime")},
        "split": identity["split"],
        "identity": identity,
        **({"execution_scope": observed["execution_scope"]}
           if observed.get("execution_scope") is not None else {}),
        **({"role_placement": observed["role_placement"]}
           if observed.get("role_placement") is not None else {}),
        **predicted["metadata"],
        **({"comparison": predicted["comparison"]}
           if predicted.get("comparison") is not None else {}),
        "units": units,
        "model_id": identity["model_id"],
        "provenance": {
            "predictive_manifest_sha256": predicted["manifest_sha256"],
            "live_manifest_sha256": observed["manifest_sha256"],
            "predictive_curve_sha256": predicted["curve_sha256"],
            "live_curve_sha256": observed["curve_sha256"],
        },
        "point_errors": point_errors,
        "loss_curve": loss_curve,
    }


def _normalized_record(mode: str, artifact: dict[str, object]) -> dict[str, object]:
    identity = artifact["identity"]
    units = artifact["units"]
    assert isinstance(identity, dict) and isinstance(units, dict)
    return {
        "schema": RECORD_SCHEMA,
        "semantics": SEMANTICS,
        "record_type": mode,
        "cell": {field: identity[field] for field in ("corpus", "profile", "regime")},
        "split": identity["split"],
        "identity": identity,
        **({"execution_scope": artifact["execution_scope"]}
           if artifact.get("execution_scope") is not None else {}),
        **({"role_placement": artifact["role_placement"]}
           if artifact.get("role_placement") is not None else {}),
        **artifact["metadata"],
        **({"comparison": artifact["comparison"]}
           if artifact.get("comparison") is not None else {}),
        "units": units,
        "model_id": identity["model_id"],
        "provenance": {
            **artifact["provenance"],
            "manifest_sha256": artifact["manifest_sha256"],
            "curve_sha256": artifact["curve_sha256"],
            **({"evidence": artifact["evidence"]}
               if artifact.get("evidence") is not None else {}),
        },
        "raw_cumulative_curve": artifact["rows"],
    }


def _write_new(path: Path, records: list[dict[str, object]]) -> None:
    if path.exists() or path.is_symlink():
        raise NormalizationError(f"output_already_exists:{path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = b"".join(canonical_bytes(record) + b"\n" for record in records)
    try:
        with path.open("xb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as exc:
        raise NormalizationError(f"output_write_failed:{path}") from exc


def normalize(predictive_manifest: Path, live_manifest: Path, out: Path,
              authenticated_metadata: dict[str, str] | None = None) -> list[dict[str, object]]:
    """Authenticate, align, and write exactly three normalized records."""
    predictive = _load_manifest(predictive_manifest, "predictive_sim")
    live = _load_manifest(live_manifest, "live")
    p_identity = predictive["identity"]
    l_identity = live["identity"]
    p_units = predictive["units"]
    l_units = live["units"]
    assert isinstance(p_identity, dict) and isinstance(l_identity, dict)
    assert isinstance(p_units, dict) and isinstance(l_units, dict)
    if predictive["manifest_path"] == live["manifest_path"]:
        raise NormalizationError("artifacts_must_use_separate_manifests")
    if predictive["curve_path"] == live["curve_path"]:
        raise NormalizationError("artifacts_must_use_separate_curves")
    _same_identity(p_identity, l_identity, predictive["comparison"], live["comparison"])
    _same_units(p_units, l_units)
    if authenticated_metadata is not None:
        if (live.get("execution_scope") != "external_farm_timing" or
                not isinstance(live.get("role_placement"), dict) or
                live["role_placement"].get("timing_eligible") is not True):
            raise NormalizationError(
                "role_placement:live_calibration_requires_external_farm")
        authority_metadata = _validate_calibration_metadata(authenticated_metadata)
        for artifact in (predictive, live):
            metadata = artifact["metadata"]
            assert isinstance(metadata, dict)
            declared = {field: metadata[field] for field in CALIBRATION_METADATA_KEYS
                        if field in metadata}
            if declared and declared != authority_metadata:
                raise NormalizationError("authority_metadata:mismatch")
            artifact["metadata"] = {**metadata, **authority_metadata}
    _same_metadata(predictive["metadata"], live["metadata"])
    predictive_record = _normalized_record("predictive_sim", predictive)
    live_record = _normalized_record("live", live)
    comparison_record = _comparison(predictive, live, p_identity, p_units)
    records = [predictive_record, live_record, comparison_record]
    _write_new(out, records)
    return records


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--predictive-manifest", type=Path, required=True)
    parser.add_argument("--live-manifest", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--calibration-metadata-manifest", type=Path)
    args = parser.parse_args(argv)
    try:
        metadata = (load_calibration_metadata_manifest(
            args.calibration_metadata_manifest.absolute())
                    if args.calibration_metadata_manifest is not None else None)
        normalize(args.predictive_manifest.absolute(), args.live_manifest.absolute(),
                  args.out.absolute(), authenticated_metadata=metadata)
    except NormalizationError as exc:
        print(f"s8_predictive_live_normalizer: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
