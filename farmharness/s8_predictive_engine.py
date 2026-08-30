#!/usr/bin/env python3
"""Trace-free predictive producer for the declared S8 cell cross-product.

The producer is intentionally causal.  It reads only an authenticated input
payload and a predeclared C1F1 topology declaration.  It does not read a live
route, assignment, action, or result trace, and it does not invoke Docker or
the live product.  The small, frozen model below predicts source/result
channel bytes and elapsed-time components from payload statistics and the
declared worker counts.  Each output row is a raw cumulative point suitable
for a later comparison adapter.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import stat
import sys
from pathlib import Path
from typing import Any

try:  # Works both as a module and as a directly invoked harness script.
    from .s8_schema import (
        CALIBRATION_CORPORA, CORPORA, CURRENT_SEMANTICS, DECLARED_CELLS,
        HELD_OUT_CORPORA, PROFILES, REGIMES, SPLITS, TOPOLOGIES, DEPTH_CLASSES,
    )
except ImportError:  # pragma: no cover - exercised by direct script runners.
    from s8_schema import (
        CALIBRATION_CORPORA, CORPORA, CURRENT_SEMANTICS, DECLARED_CELLS,
        HELD_OUT_CORPORA, PROFILES, REGIMES, SPLITS, TOPOLOGIES, DEPTH_CLASSES,
    )

# Compatibility name for callers constructing the default fmt calibration
# fixture.  The held-out policy is always performed against SPLITS[cell["corpus"]].
SPLIT = "calibration"
SEMANTICS = CURRENT_SEMANTICS
MANIFEST_SCHEMA = "icecream-s8-predictive-engine-manifest-v3"
TOPOLOGY_SCHEMA = "icecream-c1f1-topology-state-v1"
OBSERVATIONS_SCHEMA = "icecream-s8-predictive-performance-v2"
ARTIFACT_SCHEMA = "icecream-s8-predictive-artifact-v3"
CALIBRATION_MANIFEST_SCHEMA = "icecream-s8-calibration-model-manifest-v2"
CALIBRATION_BUNDLE_SCHEMA = "icecream-s8-calibration-model-bundle-v2"
LEGACY_CALIBRATION_MANIFEST_SCHEMA = "icecream-s8-calibration-model-manifest-v1"
LEGACY_CALIBRATION_BUNDLE_SCHEMA = "icecream-s8-calibration-model-bundle-v1"
HEX64 = set("0123456789abcdef")
HEX40 = set("0123456789abcdef")
MAX_INPUT_BYTES = 64 * 1024 * 1024

# This is the predeclared model.  Keeping coefficients in source makes the
# model immutable and reviewable; the topology declaration is the per-run
# input.  Units are explicit so the output can be compared without inference.
BASE_MODEL = {
    "id": "s8-causal-performance-v2",
    "compression_level": 3,
    "base_compress_ns": 18_000,
    "compress_ns_per_byte": 10,
    "base_compile_ns": 1_500_000,
    "compile_ns_per_byte": 24,
    "base_commit_ns": 35_000,
    "network_rtt_ns": 80_000,
    "uplink_bytes_per_ns": 0.0125,
    "downlink_bytes_per_ns": 0.025,
    "result_fraction": 0.12,
    "minimum_frame_bytes": 64,
}

# These immutable parameters are the complete model declaration.  They are
# intentionally small and explicit: no fitted/live result is loaded while
# producing a prediction.  Factors are dimensionless except for the base and
# per-byte terms in BASE_MODEL.
CORPUS_MODELS = {
    "fmt": {"compile_factor": 0.90},
    "RocksDB": {"compile_factor": 1.12},
    "DuckDB": {"compile_factor": 1.25},
    "LLVM-1238": {"compile_factor": 1.38},
}
PROFILE_MODELS = {
    "ZSTD_TU": {"compression_factor": 1.00, "result_fraction": 0.12,
                "uplink_factor": 1.00, "downlink_factor": 1.00,
                "channel_overhead": 0},
    "ZSTD_ROUTE": {"compression_factor": 0.92, "result_fraction": 0.12,
                   "uplink_factor": 1.08, "downlink_factor": 0.96,
                   "channel_overhead": 96},
    "P29": {"compression_factor": 0.86, "result_fraction": 0.10,
            "uplink_factor": 1.16, "downlink_factor": 0.92,
            "channel_overhead": 128},
    "GRZ_RESIDUAL": {"compression_factor": 0.74, "result_fraction": 0.08,
                     "uplink_factor": 1.28, "downlink_factor": 0.88,
                     "channel_overhead": 160},
}
REGIME_MODELS = {
    "cold": {"compression_factor": 1.00, "compile_factor": 1.00,
             "channel_factor": 1.00, "startup_ns": 0},
    "warm": {"compression_factor": 0.72, "compile_factor": 0.82,
             "channel_factor": 0.84, "startup_ns": 25_000},
}
CHANNEL_MODELS = {
    "direct": {"uplink_factor": 1.00, "downlink_factor": 1.00, "overhead": 0},
    "route": {"uplink_factor": 1.12, "downlink_factor": 1.08, "overhead": 80},
    "residual": {"uplink_factor": 1.25, "downlink_factor": 1.16, "overhead": 120},
}

# The profile relationship is part of the declared product contract.  It is
# deliberately separate from the topology's physical channel so a sequential
# producer cannot accidentally turn a direct TU run into a routed run merely
# because a fixture used a route-shaped topology.
RELATIONSHIP_MODES = {
    "ZSTD_TU": "tu",
    "ZSTD_ROUTE": "route",
    "P29": "route",
    "GRZ_RESIDUAL": "residual",
}
RELATIONSHIP_STATE_SCHEMA = "icecream-s8-predictive-relationship-state-v1"
RELATIONSHIP_CODEC_SCHEMA = "icecream-s8-predictive-zstd-relationship-v1"

MANIFEST_KEYS = {"schema", "semantics", "cell", "split", "predictive_mode",
                 "input", "topology_state"}
ARTIFACT_KEYS = {"path", "sha256", "bytes"}
TOPOLOGY_KEYS = {"schema", "semantics", "cell", "topology", "state"}
TOPOLOGY_FIELDS = {"c_store_guid", "f_store_guid", "history_nonce", "c_workers",
                   "f_workers", "cache_channel"}
STATE_FIELDS = {"c_cache", "f_cache", "generation"}


class PredictionError(ValueError):
    """An authenticated prediction input is absent or invalid."""


def canonical_bytes(value: object) -> bytes:
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"),
                          ensure_ascii=True, allow_nan=False).encode("ascii")
    except (TypeError, ValueError, OverflowError, UnicodeError) as exc:
        raise PredictionError("canonical_json:invalid_value") from exc


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise PredictionError(f"duplicate_json_key:{key}")
        result[key] = value
    return result


def parse_json(raw: bytes, label: str) -> object:
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys,
                          parse_constant=lambda value: (_ for _ in ()).throw(
                              PredictionError(f"{label}:non_finite_json:{value}")))
    except PredictionError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PredictionError(f"{label}:invalid_json") from exc


def regular_snapshot(path: Path, label: str, limit: int | None = None) -> tuple[bytes, dict[str, Any]]:
    """Read and hash one private regular file through one descriptor."""
    try:
        info = path.lstat()
    except OSError as exc:
        raise PredictionError(f"{label}:unavailable:{path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise PredictionError(f"{label}:not_private_regular_file:{path}")
    if info.st_nlink != 1:
        raise PredictionError(f"{label}:hard_link_alias:{path}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise PredictionError(f"{label}:cannot_open:{path}") from exc
    try:
        before = os.fstat(fd)
        chunks: list[bytes] = []
        digest = hashlib.sha256()
        total = 0
        while True:
            try:
                block = os.read(fd, 1 << 20)
            except OSError as exc:
                raise PredictionError(f"{label}:read_failed:{path}") from exc
            if not block:
                break
            total += len(block)
            if limit is not None and total > limit:
                raise PredictionError(f"{label}:too_large")
            chunks.append(block)
            digest.update(block)
        after = os.fstat(fd)
        if any(getattr(before, field) != getattr(after, field)
               for field in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")):
            raise PredictionError(f"{label}:changed_while_reading:{path}")
        return b"".join(chunks), {"sha256": digest.hexdigest(), "bytes": total}
    finally:
        os.close(fd)


def _sha256(value: object, label: str) -> str:
    if (not isinstance(value, str) or len(value) != 64 or
            set(value.lower()) - HEX64 or int(value, 16) == 0):
        raise PredictionError(f"{label}:invalid_sha256")
    return value.lower()


def _artifact_path(base: Path, value: object, label: str) -> Path:
    if not isinstance(value, dict) or set(value) != ARTIFACT_KEYS:
        raise PredictionError(f"{label}:descriptor_invalid")
    relative = value["path"]
    if not isinstance(relative, str) or not relative:
        raise PredictionError(f"{label}:path_missing")
    candidate = Path(relative)
    if candidate.is_absolute() or any(part in ("", ".", "..") for part in candidate.parts):
        raise PredictionError(f"{label}:path_not_private_relative")
    path = base / candidate
    try:
        path.resolve().relative_to(base.resolve())
    except ValueError as exc:
        raise PredictionError(f"{label}:path_escapes_manifest_root") from exc
    _sha256(value["sha256"], f"{label}.sha256")
    if type(value["bytes"]) is not int or value["bytes"] < 0:
        raise PredictionError(f"{label}.bytes:invalid")
    return path


def _authenticate(path: Path, descriptor: dict[str, object], label: str,
                  limit: int | None = None) -> tuple[bytes, dict[str, Any]]:
    raw, facts = regular_snapshot(path, label, limit)
    if facts["sha256"] != str(descriptor["sha256"]).lower():
        raise PredictionError(f"{label}:sha256_mismatch")
    if facts["bytes"] != descriptor["bytes"]:
        raise PredictionError(f"{label}:byte_count_mismatch")
    return raw, facts


def load_calibration_bundle(manifest_path: Path) -> dict[str, object]:
    """Authenticate a frozen calibration bundle for this base model."""
    manifest_raw, manifest_facts = regular_snapshot(manifest_path, "calibration_manifest")
    value = parse_json(manifest_raw, "calibration_manifest")
    expected_keys = {"schema", "semantics", "bundle", "request", "predictor", "inputs"}
    if not isinstance(value, dict) or set(value) != expected_keys:
        raise PredictionError("calibration_manifest:fields_invalid")
    if value["schema"] not in {CALIBRATION_MANIFEST_SCHEMA, LEGACY_CALIBRATION_MANIFEST_SCHEMA} or value["semantics"] != SEMANTICS:
        raise PredictionError("calibration_manifest:schema_or_semantics_invalid")
    migrated = value["schema"] == LEGACY_CALIBRATION_MANIFEST_SCHEMA
    predictor = value["predictor"]
    if not isinstance(predictor, dict) or set(predictor) != {"source_commit", "source_tree", "model_id"}:
        raise PredictionError("calibration_manifest:predictor_invalid")
    if predictor["model_id"] != BASE_MODEL["id"]:
        raise PredictionError("calibration_manifest:base_model_mismatch")
    for field in ("source_commit", "source_tree"):
        digest = predictor[field]
        if (not isinstance(digest, str) or len(digest) != 40 or
                set(digest.lower()) - HEX40 or int(digest, 16) == 0):
            raise PredictionError(f"calibration_manifest:predictor_{field}_invalid")
    bundle_descriptor = _artifact_path(manifest_path.parent, value["bundle"], "calibration_bundle")
    bundle_raw, bundle_facts = _authenticate(bundle_descriptor, value["bundle"], "calibration_bundle")
    bundle = parse_json(bundle_raw, "calibration_bundle")
    if not isinstance(bundle, dict) or set(bundle) != {
            "schema", "semantics", "request", "predictor", "calibration", "inputs"}:
        raise PredictionError("calibration_bundle:fields_invalid")
    expected_bundle_schema = (LEGACY_CALIBRATION_BUNDLE_SCHEMA if migrated else CALIBRATION_BUNDLE_SCHEMA)
    if bundle["schema"] != expected_bundle_schema or bundle["semantics"] != SEMANTICS:
        raise PredictionError("calibration_bundle:schema_or_semantics_invalid")
    if bundle["predictor"] != predictor:
        raise PredictionError("calibration_bundle:predictor_identity_mismatch")
    if value["request"] != bundle["request"] or value["inputs"] != bundle["inputs"]:
        raise PredictionError("calibration_bundle:manifest_binding_mismatch")
    expected_cells = {
        f"{cell['corpus']}/{cell['profile']}/{cell['regime']}": cell
        for cell in DECLARED_CELLS if SPLITS[cell["corpus"]] == "calibration"
    }
    if (not isinstance(bundle["request"], dict) or
            set(bundle["request"]) != {"sha256", "bytes"} or
            not isinstance(bundle["inputs"], list) or
            len(bundle["inputs"]) < len(expected_cells)):
        raise PredictionError("calibration_bundle:input_bindings_invalid")
    # Bind each evidence cell together with its topology/depth context.  The
    # same corpus/profile/regime is expected to recur across contexts.
    seen_cells: set[tuple[str, str]] = set()
    binding_contexts: set[str] = set()
    bucket_corpora: dict[str, set[str]] = {
        f"{profile}/{regime}": set()
        for profile in PROFILE_MODELS for regime in REGIME_MODELS
    }
    for binding in bundle["inputs"]:
        expected_binding_keys = {"cell", "records_path", "records_sha256", "records_bytes"}
        if not migrated:
            expected_binding_keys |= {"topology", "depth_class", "compatibility"}
        if not isinstance(binding, dict) or set(binding) != expected_binding_keys:
            raise PredictionError("calibration_bundle:input_binding_invalid")
        cell = binding["cell"]
        if not isinstance(cell, dict) or set(cell) != {"corpus", "profile", "regime"}:
            raise PredictionError("calibration_bundle:input_cell_invalid")
        cell_id = f"{cell.get('corpus')}/{cell.get('profile')}/{cell.get('regime')}"
        if cell_id not in expected_cells:
            raise PredictionError(f"calibration_bundle:input_cell_duplicate_or_invalid:{cell_id}")
        if migrated:
            topology, depth_class = "C1F1", "legacy"
        else:
            topology, depth_class = binding["topology"], binding["depth_class"]
            if (topology not in TOPOLOGIES or
                    depth_class not in (set(DEPTH_CLASSES) - {"legacy"} | {"legacy"}) or
                    (depth_class == "legacy" and topology != "C1F1") or
                    binding["compatibility"] != ("legacy_c1f1" if depth_class == "legacy" else "explicit")):
                raise PredictionError("calibration_bundle:context_invalid")
        binding_contexts.add(f"{topology}/{depth_class}")
        context_cell = (f"{topology}/{depth_class}", cell_id)
        if context_cell in seen_cells:
            raise PredictionError(
                f"calibration_bundle:input_cell_duplicate_or_invalid:{topology}/{depth_class}/{cell_id}")
        seen_cells.add(context_cell)
        bucket_corpora[f"{cell['profile']}/{cell['regime']}"].add(cell["corpus"])
        path = binding["records_path"]
        if (not isinstance(path, str) or not path or Path(path).is_absolute() or
                any(part in ("", ".", "..") for part in Path(path).parts)):
            raise PredictionError("calibration_bundle:input_path_invalid")
        digest = binding["records_sha256"]
        if (not isinstance(digest, str) or len(digest) != 64 or
                set(digest.lower()) - HEX64 or int(digest, 16) == 0):
            raise PredictionError("calibration_bundle:input_digest_invalid")
        if type(binding["records_bytes"]) is not int or binding["records_bytes"] <= 0:
            raise PredictionError("calibration_bundle:input_bytes_invalid")
    expected_context_cells = {
        (context, cell_id)
        for context in binding_contexts
        for cell_id in expected_cells
    }
    if seen_cells != expected_context_cells:
        raise PredictionError("calibration_bundle:input_cell_set_invalid")
    if any(corpora != {"fmt", "RocksDB"} for corpora in bucket_corpora.values()):
        raise PredictionError("calibration_bundle:bucket_sources_invalid")
    calibration = bundle["calibration"]
    if not isinstance(calibration, dict) or not isinstance(calibration.get("scales"), dict):
        raise PredictionError("calibration_bundle:calibration_invalid")
    expected_buckets = {f"{profile}/{regime}"
                        for profile in PROFILE_MODELS for regime in REGIME_MODELS}
    scales = calibration["scales"]
    canonical_scales: dict[str, dict[str, object]] = {}
    if migrated:
        if set(scales) != expected_buckets:
            raise PredictionError("calibration_bundle:bucket_set_invalid")
        for bucket, bucket_scales in scales.items():
            if not isinstance(bucket_scales, dict) or set(bucket_scales) != {"channel_bytes", "elapsed_ns"}:
                raise PredictionError(f"calibration_bundle:bucket_invalid:{bucket}")
            canonical_scales[f"C1F1/legacy/{bucket}"] = {
                "C_TO_F_bytes": bucket_scales["channel_bytes"],
                "F_TO_C_bytes": bucket_scales["channel_bytes"],
                "elapsed_ns": bucket_scales["elapsed_ns"],
            }
    else:
        contexts = calibration.get("contexts")
        if (not isinstance(contexts, list) or not contexts or
                any(not isinstance(context, str) or "/" not in context for context in contexts)):
            raise PredictionError("calibration_bundle:contexts_invalid")
        if binding_contexts != set(contexts):
            raise PredictionError("calibration_bundle:context_binding_mismatch")
        expected_keys = {f"{context}/{bucket}" for context in contexts for bucket in expected_buckets}
        legacy_aliases = (expected_buckets if set(contexts) == {"C1F1/legacy"} else set())
        # Aliases are permitted only for migrated legacy fixtures; a v2 bundle
        # must have no unscoped aggregate bucket capable of leaking to C1F20.
        accepted_keys = expected_keys | legacy_aliases
        # v2 bundles emitted from a legacy request intentionally contain only
        # the old aliases; those aliases are still normalized to a scoped
        # C1F1/legacy context below.
        if set(scales) not in (expected_keys, accepted_keys, legacy_aliases):
            raise PredictionError("calibration_bundle:bucket_set_invalid")
        for bucket, bucket_scales in scales.items():
            expected_fields = ({"channel_bytes", "elapsed_ns"} if bucket in legacy_aliases else
                               {"C_TO_F_bytes", "F_TO_C_bytes", "elapsed_ns"})
            if not isinstance(bucket_scales, dict) or set(bucket_scales) != expected_fields:
                raise PredictionError(f"calibration_bundle:bucket_invalid:{bucket}")
            canonical_scales[bucket if bucket not in legacy_aliases else f"C1F1/legacy/{bucket}"] = (
                {"C_TO_F_bytes": bucket_scales["channel_bytes"],
                 "F_TO_C_bytes": bucket_scales["channel_bytes"],
                 "elapsed_ns": bucket_scales["elapsed_ns"]}
                if bucket in legacy_aliases else bucket_scales)
    for bucket, bucket_scales in canonical_scales.items():
        for metric, scale in bucket_scales.items():
            if type(scale) not in (int, float) or not math.isfinite(float(scale)) or scale <= 0:
                raise PredictionError(f"calibration_bundle:scale_invalid:{bucket}:{metric}")
    return {
        "bundle": bundle,
        "bundle_sha256": bundle_facts["sha256"],
        "manifest_sha256": manifest_facts["sha256"],
        "scales": canonical_scales,
        "contexts": sorted({key.rsplit("/", 2)[0] for key in canonical_scales}),
        "model_id": f"{BASE_MODEL['id']}-cal-{bundle_facts['sha256']}",
    }


def _guid(value: object, label: str) -> str:
    if not isinstance(value, str) or len(value) != 32 or set(value.lower()) - set("0123456789abcdef"):
        raise PredictionError(f"topology:{label}:invalid_guid")
    if int(value, 16) == 0:
        raise PredictionError(f"topology:{label}:zero_guid")
    return value.lower()


def validate_topology(value: object, cell: dict[str, str]) -> None:
    if not isinstance(value, dict) or set(value) != TOPOLOGY_KEYS:
        raise PredictionError("topology:fields_invalid")
    if value["schema"] != TOPOLOGY_SCHEMA or value["semantics"] != SEMANTICS or value["cell"] != cell:
        raise PredictionError("topology:schema_or_cell_invalid")
    topology, state = value["topology"], value["state"]
    if not isinstance(topology, dict) or set(topology) != TOPOLOGY_FIELDS:
        raise PredictionError("topology:topology_fields_invalid")
    if not isinstance(state, dict) or set(state) != STATE_FIELDS:
        raise PredictionError("topology:state_fields_invalid")
    _guid(topology["c_store_guid"], "c_store_guid")
    _guid(topology["f_store_guid"], "f_store_guid")
    if type(topology["history_nonce"]) is not int or topology["history_nonce"] <= 0:
        raise PredictionError("topology:history_nonce_invalid")
    for field in ("c_workers", "f_workers"):
        if type(topology[field]) is not int or topology[field] <= 0:
            raise PredictionError(f"topology:{field}_invalid")
    if topology["cache_channel"] not in CHANNEL_MODELS:
        raise PredictionError("topology:cache_channel_invalid")
    if state["c_cache"] != cell["regime"] or state["f_cache"] != cell["regime"]:
        raise PredictionError("topology:state_regime_mismatch")
    if type(state["generation"]) is not int or state["generation"] < 0:
        raise PredictionError("topology:generation_invalid")


def load_inputs(manifest_path: Path) -> tuple[dict[str, object], bytes, dict[str, Any], dict[str, Any], str, dict[str, object], dict[str, str]]:
    manifest_raw, manifest_facts = regular_snapshot(manifest_path, "manifest")
    value = parse_json(manifest_raw, "manifest")
    if not isinstance(value, dict) or set(value) != MANIFEST_KEYS:
        raise PredictionError("manifest:fields_are_not_current_engine_manifest")
    if value["schema"] != MANIFEST_SCHEMA:
        raise PredictionError("manifest:schema_invalid")
    if value["semantics"] != SEMANTICS:
        raise PredictionError("manifest:semantics_invalid")
    cell = value["cell"]
    if not isinstance(cell, dict) or set(cell) != {"corpus", "profile", "regime"}:
        raise PredictionError("manifest:cell_invalid")
    if (cell["corpus"] not in CORPORA or cell["profile"] not in PROFILES or
            cell["regime"] not in REGIMES):
        raise PredictionError("manifest:cell_not_declared")
    if value["split"] != SPLITS[cell["corpus"]] or value["predictive_mode"] is not True:
        raise PredictionError("manifest:cell_split_or_predictive_mode_invalid")
    base = manifest_path.parent
    input_path = _artifact_path(base, value["input"], "input")
    topology_path = _artifact_path(base, value["topology_state"], "topology_state")
    if input_path.resolve() == topology_path.resolve():
        raise PredictionError("input_and_topology_state_must_be_distinct")
    input_raw, input_facts = _authenticate(input_path, value["input"], "input", MAX_INPUT_BYTES)
    topology_raw, topology_facts = _authenticate(topology_path, value["topology_state"], "topology_state")
    topology = parse_json(topology_raw, "topology_state")
    validate_topology(topology, cell)
    assert isinstance(topology, dict)
    return value, input_raw, input_facts, topology_facts, manifest_facts["sha256"], topology, cell


def _ceil_ratio(numerator: int, denominator: float) -> int:
    return max(1, math.ceil(numerator / denominator))


def _payload_stats(raw: bytes) -> tuple[int, float, float]:
    """Return raw bytes, entropy bits/byte, and adjacent-transition rate."""
    if not raw:
        return 0, 0.0, 0.0
    counts = [0] * 256
    transitions = 0
    for index, byte in enumerate(raw):
        counts[byte] += 1
        if index and raw[index - 1] != byte:
            transitions += 1
    size = len(raw)
    entropy = -sum((count / size) * math.log2(count / size)
                   for count in counts if count)
    return size, entropy, transitions / max(1, size - 1)


def _calibration_factors(calibration: dict[str, object], topology: dict[str, object],
                         cell: dict[str, str], depth_class: str,
                         topology_name: str | None = None) -> dict[str, float]:
    topology_values = topology.get("topology")
    if not isinstance(topology_values, dict):
        raise PredictionError("calibration:topology_missing")
    # The calibration topology key is the scheduling topology, not the
    # physical cache-channel name.  C1F20 therefore cannot fall back to the
    # C1F1 aggregate bundle.
    topology_name = topology_name or topology_values.get("topology", topology_values.get("suite"))
    if topology_name is None:
        # Engine topology artifacts predate the scheduling name.  Worker
        # cardinality is an unambiguous local derivation for this compatibility
        # path and never broadens a C1F1 calibration to C1F20.
        topology_name = "C1F1" if topology_values.get("f_workers") == 1 else "C1F20"
    if topology_name not in TOPOLOGIES:
        raise PredictionError("calibration:topology_unsupported")
    if depth_class not in DEPTH_CLASSES:
        raise PredictionError("calibration:depth_class_unsupported")
    context = f"{topology_name}/{depth_class}"
    scales = calibration.get("scales")
    if not isinstance(scales, dict):
        raise PredictionError("calibration:scales_invalid")
    bucket = scales.get(f"{context}/{cell['profile']}/{cell['regime']}")
    if bucket is None:
        raise PredictionError(f"calibration:unsupported_context:{context}")
    if not isinstance(bucket, dict) or set(bucket) != {"C_TO_F_bytes", "F_TO_C_bytes", "elapsed_ns"}:
        raise PredictionError("calibration:factor_shape_invalid")
    return {field: float(bucket[field]) for field in ("C_TO_F_bytes", "F_TO_C_bytes", "elapsed_ns")}


def _predict_curve(raw: bytes, topology: dict[str, object], cell: dict[str, str],
                   calibration: dict[str, object] | None = None,
                   depth_class: str = "legacy",
                   calibration_topology: str | None = None) -> list[dict[str, object]]:
    t = topology["topology"]
    assert isinstance(t, dict)
    c_workers, f_workers = int(t["c_workers"]), int(t["f_workers"])
    corpus_model = CORPUS_MODELS[cell["corpus"]]
    profile_model = PROFILE_MODELS[cell["profile"]]
    regime_model = REGIME_MODELS[cell["regime"]]
    channel_model = CHANNEL_MODELS[t["cache_channel"]]
    calibrated_model_id = BASE_MODEL["id"]
    c_to_f_scale = f_to_c_scale = elapsed_scale = 1.0
    if calibration is not None:
        factors = _calibration_factors(calibration, topology, cell, depth_class,
                                       calibration_topology)
        c_to_f_scale = factors["C_TO_F_bytes"]
        f_to_c_scale = factors["F_TO_C_bytes"]
        elapsed_scale = factors["elapsed_ns"]
        calibrated_model_id = str(calibration["model_id"])
    cumulative_c_to_f = cumulative_f_to_c = cumulative_elapsed = 0
    rows: list[dict[str, object]] = []
    # Live evidence is observed at the translation-unit boundary.  Preserve
    # that measurement unit here: subdividing one authenticated .ii into
    # artificial byte ranges would create predictive points for which no
    # independent live observation exists and would make the loss curve
    # impossible to score honestly.
    chunks = [raw]
    for step, chunk in enumerate(chunks):
        raw_size, entropy, transitions = _payload_stats(chunk)
        # The authenticated input byte count is already the corpus size.  Do
        # not multiply it by a corpus-specific factor or charge it twice.
        # Entropy and transitions are content features, not identity metadata.
        ratio = min(0.98, max(0.20, 0.20 + 0.065 * entropy + 0.13 * transitions))
        ratio *= profile_model["compression_factor"] * regime_model["compression_factor"]
        source_bytes = (max(BASE_MODEL["minimum_frame_bytes"],
                            round(raw_size * ratio)) + profile_model["channel_overhead"] +
                        channel_model["overhead"]) if raw_size else 0
        base_result_bytes = (max(BASE_MODEL["minimum_frame_bytes"],
                                 round(raw_size * profile_model["result_fraction"] * regime_model["channel_factor"])) +
                             channel_model["overhead"]) if raw_size else 0
        result_bytes = base_result_bytes
        if raw_size:
            # Standalone causal predictions use direction-specific factors.
            # Product batch callers replace C_TO_F with authenticated product
            # bytes at the boundary below.
            source_bytes = max(1, round(source_bytes * c_to_f_scale))
            result_bytes = max(1, round(base_result_bytes * f_to_c_scale))
        compress_ns = (BASE_MODEL["base_compress_ns"] + raw_size * BASE_MODEL["compress_ns_per_byte"] + c_workers - 1) // c_workers
        uplink_rate = (BASE_MODEL["uplink_bytes_per_ns"] * c_workers /
                       (profile_model["uplink_factor"] * channel_model["uplink_factor"]))
        input_ready_ns = compress_ns + _ceil_ratio(source_bytes, uplink_rate) + BASE_MODEL["network_rtt_ns"]
        compile_work_ns = (BASE_MODEL["base_compile_ns"] + raw_size * BASE_MODEL["compile_ns_per_byte"])
        compile_work_ns = round(compile_work_ns * corpus_model["compile_factor"] * regime_model["compile_factor"])
        compile_ns = (compile_work_ns + f_workers - 1) // f_workers
        downlink_rate = (BASE_MODEL["downlink_bytes_per_ns"] * f_workers /
                         (profile_model["downlink_factor"] * channel_model["downlink_factor"]))
        # The live wait-for-cs comparison window is compile + result return.
        # Compute that base window before calibration channel scaling leaks into
        # return time, then apportion one rounded scaled total deterministically.
        base_return_ns = _ceil_ratio(base_result_bytes, downlink_rate)
        base_window_ns = compile_ns + base_return_ns
        scaled_window_ns = max(2, round(base_window_ns * elapsed_scale))
        scaled_compile_ns = max(1, round(compile_ns * elapsed_scale))
        if scaled_compile_ns >= scaled_window_ns:
            scaled_compile_ns = scaled_window_ns - 1
        compile_ns = scaled_compile_ns
        return_ns = scaled_window_ns - scaled_compile_ns
        commit_ns = (BASE_MODEL["base_commit_ns"] + f_workers - 1) // f_workers
        startup_ns = regime_model["startup_ns"] if step == 0 else 0
        total_ns = input_ready_ns + compile_ns + return_ns + commit_ns + startup_ns
        cumulative_c_to_f += source_bytes
        cumulative_f_to_c += result_bytes
        cumulative_elapsed += total_ns
        # Cumulative keys are deliberately numeric and raw, with no digest
        # substituted for a predictive value.
        rows.append({
            "schema": OBSERVATIONS_SCHEMA,
            "step": step,
            "tu_id": f"{cell['corpus']}-{cell['regime']}-input-{step:06d}",
            "cell": cell,
            "model_id": calibrated_model_id,
            "channel_bytes": {"C_TO_F": source_bytes, "F_TO_C": result_bytes,
                               "total": source_bytes + result_bytes},
            "elapsed_ns": {"startup": startup_ns, "compression": compress_ns,
                            "input_ready": input_ready_ns,
                            "compile": compile_ns, "result_return": return_ns,
                            "transaction_commit": commit_ns, "total": total_ns},
            "cumulative": {"C_TO_F_bytes": cumulative_c_to_f,
                            "F_TO_C_bytes": cumulative_f_to_c,
                            "channel_bytes": cumulative_c_to_f + cumulative_f_to_c,
                            "elapsed_ns": cumulative_elapsed},
            "features": {"raw_input_bytes": raw_size,
                          "entropy_bits_per_byte": entropy,
                          "transition_rate": transitions},
            "provenance": "modeled",
        })
    return rows


def relationship_mode(cell: dict[str, str]) -> str:
    """Return the declared sequential relationship for one S8 profile."""
    try:
        return RELATIONSHIP_MODES[cell["profile"]]
    except (KeyError, TypeError) as exc:
        raise PredictionError("relationship:profile_not_declared") from exc


def apply_encoded_source_bytes(row: dict[str, object], encoded_bytes: int,
                               topology: dict[str, object], cell: dict[str, str],
                               startup: int | None = None) -> None:
    """Recompute the modeled input-link window from actual encoded bytes."""
    if type(encoded_bytes) is not int or encoded_bytes < 0:
        raise PredictionError("relationship_codec:encoded_bytes_invalid")
    elapsed = row.get("elapsed_ns")
    if not isinstance(elapsed, dict):
        raise PredictionError("relationship_codec:elapsed_shape_invalid")
    topology_values = topology["topology"]
    assert isinstance(topology_values, dict)
    c_workers = int(topology_values["c_workers"])
    profile_model = PROFILE_MODELS[cell["profile"]]
    channel_model = CHANNEL_MODELS[topology_values["cache_channel"]]
    uplink_rate = (BASE_MODEL["uplink_bytes_per_ns"] * c_workers /
                   (profile_model["uplink_factor"] * channel_model["uplink_factor"]))
    compression = int(elapsed["compression"])
    old_ready = int(elapsed["input_ready"])
    new_ready = compression + _ceil_ratio(encoded_bytes, uplink_rate) + BASE_MODEL["network_rtt_ns"]
    elapsed["input_ready"] = new_ready
    elapsed["total"] = int(elapsed["total"]) + new_ready - old_ready
    if startup is not None:
        if type(startup) is not int or startup < 0:
            raise PredictionError("relationship_codec:startup_invalid")
        previous_startup = int(elapsed["startup"])
        elapsed["startup"] = startup
        elapsed["total"] = int(elapsed["total"]) + startup - previous_startup


def new_relationship_state(topology: dict[str, object], cell: dict[str, str],
                           topology_digest: str | None = None) -> dict[str, object]:
    """Create the explicit state carried between ordered simulator calls.

    The default one-TU ``predict`` path never uses this interface and remains
    byte-for-byte/numerically frozen.  ``topology_digest`` should be the hash
    of the authenticated topology artifact when one is available.
    """
    validate_topology(topology, cell)
    digest = topology_digest or hashlib.sha256(canonical_bytes(topology)).hexdigest()
    _sha256(digest, "relationship.topology_digest")
    return {
        "schema": RELATIONSHIP_STATE_SCHEMA, "semantics": SEMANTICS,
        "cell": dict(cell), "mode": relationship_mode(cell),
        "topology_digest": digest.lower(), "next_step": 0,
        "last_input_sha256": None, "c_cache_bytes": 0,
        "f_cache_digest": None, "reset_points": [],
    }


def _validate_relationship_state(state: object, topology: dict[str, object],
                                 cell: dict[str, str]) -> dict[str, object]:
    if not isinstance(state, dict) or set(state) != {
            "schema", "semantics", "cell", "mode", "topology_digest", "next_step",
            "last_input_sha256", "c_cache_bytes", "f_cache_digest", "reset_points"}:
        raise PredictionError("relationship_state:fields_invalid")
    if (state["schema"] != RELATIONSHIP_STATE_SCHEMA or
            state["semantics"] != SEMANTICS or state["cell"] != cell or
            state["mode"] != relationship_mode(cell)):
        raise PredictionError("relationship_state:identity_invalid")
    _sha256(state["topology_digest"], "relationship.topology_digest")
    if type(state["next_step"]) is not int or state["next_step"] < 0:
        raise PredictionError("relationship_state:next_step_invalid")
    last = state["last_input_sha256"]
    if last is not None:
        _sha256(last, "relationship.last_input_sha256")
    if type(state["c_cache_bytes"]) is not int or state["c_cache_bytes"] < 0:
        raise PredictionError("relationship_state:c_cache_bytes_invalid")
    if state["f_cache_digest"] is not None:
        _sha256(state["f_cache_digest"], "relationship.f_cache_digest")
    if state["next_step"] == 0 and (last is not None or state["c_cache_bytes"] != 0 or
                                     state["f_cache_digest"] is not None):
        raise PredictionError("relationship_state:reset_sequence_mismatch")
    if state["mode"] == "tu" and (last is not None or state["c_cache_bytes"] != 0 or
                                    state["f_cache_digest"] is not None):
        raise PredictionError("relationship_state:tu_cache_must_reset")
    resets = state["reset_points"]
    if (not isinstance(resets, list) or any(type(point) is not int or point < 0
                                             for point in resets) or
            resets != list(range(len(resets))) or len(resets) > state["next_step"]):
        raise PredictionError("relationship_state:reset_points_invalid")
    validate_topology(topology, cell)
    return state


def predict_sequential(raw: bytes, topology: dict[str, object], cell: dict[str, str],
                       state: dict[str, object],
                       calibration: dict[str, object] | None = None,
                       depth_class: str = "legacy",
                       calibration_topology: str | None = None) -> tuple[dict[str, object], dict[str, object]]:
    """Predict one TU and advance a declared relationship state.

    ``ZSTD_TU`` clears relationship caches at every boundary.  The route and
    residual modes retain a rolling C/F relationship; a repeated payload is a
    cache reuse and therefore has a smaller C-to-F transfer and input-ready
    window.  No route/action/trace evidence is read or generated.
    """
    state = _validate_relationship_state(state, topology, cell)
    before_digest = hashlib.sha256(canonical_bytes(state)).hexdigest()
    mode = str(state["mode"])
    payload_digest = hashlib.sha256(raw).hexdigest()
    step = int(state["next_step"])
    reset_point = mode == "tu"
    if reset_point:
        state["last_input_sha256"] = None
        state["c_cache_bytes"] = 0
        state["f_cache_digest"] = None
        state["reset_points"].append(step)
    rows = _predict_curve(raw, topology, cell, calibration, depth_class,
                          calibration_topology)
    if len(rows) != 1:
        raise PredictionError("relationship:one_tu_prediction_required")
    row = rows[0]
    channel = row["channel_bytes"]
    elapsed = row["elapsed_ns"]
    assert isinstance(channel, dict) and isinstance(elapsed, dict)
    previous = state["last_input_sha256"]
    cache_reuse = mode != "tu" and previous == payload_digest
    # Numeric wire bytes are supplied by the authenticated product boundary.
    # This state API only records the transition; it does not use a
    # metadata-derived byte multiplier.
    transition = "c_cache_reuse" if cache_reuse else ("tu_reset" if reset_point else "relationship_advance")
    if mode != "tu" and step > 0:
        startup = int(elapsed["startup"])
        elapsed["startup"] = 0
        elapsed["total"] = int(elapsed["total"]) - startup
    if mode == "tu":
        state["last_input_sha256"] = None
        state["c_cache_bytes"] = 0
        state["f_cache_digest"] = None
    else:
        state["last_input_sha256"] = payload_digest
        state["c_cache_bytes"] = int(channel["C_TO_F"])
        state["f_cache_digest"] = hashlib.sha256(canonical_bytes({
            "previous_f": state["f_cache_digest"], "previous_c": payload_digest,
            "F_TO_C_bytes": channel["F_TO_C"]})).hexdigest()
    state["next_step"] = step + 1
    after_digest = hashlib.sha256(canonical_bytes(state)).hexdigest()
    row["relationship_mode"] = mode
    row["relationship_state"] = {
        "topology_digest": state["topology_digest"],
        "before_digest": before_digest, "after_digest": after_digest,
        "transition": transition, "reset_point": reset_point,
    }
    return row, state


def _write_new(path: Path, raw: bytes, label: str) -> None:
    if path.exists() or path.is_symlink():
        raise PredictionError(f"{label}:output_already_exists:{path}")
    try:
        with path.open("xb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as exc:
        raise PredictionError(f"{label}:output_write_failed:{path}") from exc


def predict(manifest_path: Path, out_path: Path, sim_binary: Path | None = None,
            calibration_bundle: Path | None = None, *, depth_class: str = "legacy",
            calibration_topology: str | None = None) -> dict[str, object]:
    """Produce a raw cumulative prediction curve and authenticated sidecar.

    ``sim_binary`` remains an ignored compatibility argument for callers of
    the rejected scaffold.  It is never opened or executed.
    """
    del sim_binary
    manifest, input_raw, input_facts, topology_facts, manifest_sha, topology, cell = load_inputs(manifest_path)
    calibration = load_calibration_bundle(calibration_bundle) if calibration_bundle is not None else None
    rows = _predict_curve(input_raw, topology, cell, calibration, depth_class,
                          calibration_topology)
    payload = b"".join(canonical_bytes(row) + b"\n" for row in rows)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    sidecar_path = out_path.with_name(out_path.name + ".manifest.json")
    if sidecar_path.exists() or sidecar_path.is_symlink():
        raise PredictionError(f"artifact_manifest:output_already_exists:{sidecar_path}")
    _write_new(out_path, payload, "prediction_curve")
    output_facts = {"sha256": hashlib.sha256(payload).hexdigest(), "bytes": len(payload)}
    sidecar = {
        "schema": ARTIFACT_SCHEMA, "semantics": SEMANTICS, "cell": cell,
        "split": manifest["split"], "observations": {"path": out_path.name, **output_facts},
        "input": {"path": manifest["input"]["path"], **input_facts},
        "topology_state": {"path": manifest["topology_state"]["path"], **topology_facts},
        "predictor": {
            "name": "icecream-s8-causal-performance-model",
            "version": calibration["model_id"] if calibration is not None else BASE_MODEL["id"],
            "base_model_id": BASE_MODEL["id"],
            "route_trace_consumed": False, "action_trace_input": False,
            "model_assumptions": {"base": BASE_MODEL, "corpus": CORPUS_MODELS[cell["corpus"]],
                                  "profile": PROFILE_MODELS[cell["profile"]],
                                  "regime": REGIME_MODELS[cell["regime"]]},
            **({"calibration": {
                "bundle_sha256": calibration["bundle_sha256"],
                "context": f"{calibration_topology or 'C1F1'}/{depth_class}",
                "factor_fields": ["C_TO_F_bytes", "F_TO_C_bytes", "elapsed_ns"],
                "factors": _calibration_factors(calibration, topology, cell, depth_class,
                                                  calibration_topology),
                "throughput": "derived_from_calibrated_channel_bytes_and_elapsed_ns",
            }} if calibration is not None else {}),
            "topology_effects": {"c_workers": "producer compression and uplink share",
                                  "f_workers": "compile, return and commit share",
                                  "cache_channel": "fixed C_TO_F source and F_TO_C result"},
        },
        "provenance": {"manifest_sha256": manifest_sha, "curve_points": len(rows),
                       **({"calibration_bundle_sha256": calibration["bundle_sha256"]}
                          if calibration is not None else {})},
    }
    _write_new(sidecar_path, canonical_bytes(sidecar) + b"\n", "artifact_manifest")
    return sidecar


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    # Accepted for source compatibility; the predictor never invokes it.
    parser.add_argument("--sim", type=Path)
    parser.add_argument("--calibration-manifest", type=Path,
                        help="authenticated frozen calibration model manifest")
    parser.add_argument("--depth-class", choices=tuple(DEPTH_CLASSES), default="legacy")
    parser.add_argument("--calibration-topology", choices=TOPOLOGIES)
    args = parser.parse_args(argv)
    try:
        predict(args.manifest.absolute(), args.out.absolute(), args.sim,
                args.calibration_manifest.absolute() if args.calibration_manifest else None,
                depth_class=args.depth_class, calibration_topology=args.calibration_topology)
    except PredictionError as exc:
        print(str(exc), file=sys.stderr)
        return 77
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
