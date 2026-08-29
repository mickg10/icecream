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
        HELD_OUT_CORPORA, PROFILES, REGIMES, SPLITS,
    )
except ImportError:  # pragma: no cover - exercised by direct script runners.
    from s8_schema import (
        CALIBRATION_CORPORA, CORPORA, CURRENT_SEMANTICS, DECLARED_CELLS,
        HELD_OUT_CORPORA, PROFILES, REGIMES, SPLITS,
    )

# Compatibility name for callers constructing the default fmt calibration
# fixture.  The held-out policy is always performed against SPLITS[cell["corpus"]].
SPLIT = "calibration"
SEMANTICS = CURRENT_SEMANTICS
MANIFEST_SCHEMA = "icecream-s8-predictive-engine-manifest-v3"
TOPOLOGY_SCHEMA = "icecream-c1f1-topology-state-v1"
OBSERVATIONS_SCHEMA = "icecream-s8-predictive-performance-v2"
ARTIFACT_SCHEMA = "icecream-s8-predictive-artifact-v3"
HEX64 = set("0123456789abcdef")
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
    if not isinstance(value, str) or len(value) != 64 or set(value.lower()) - HEX64:
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


def _predict_curve(raw: bytes, topology: dict[str, object], cell: dict[str, str]) -> list[dict[str, object]]:
    t = topology["topology"]
    assert isinstance(t, dict)
    c_workers, f_workers = int(t["c_workers"]), int(t["f_workers"])
    corpus_model = CORPUS_MODELS[cell["corpus"]]
    profile_model = PROFILE_MODELS[cell["profile"]]
    regime_model = REGIME_MODELS[cell["regime"]]
    channel_model = CHANNEL_MODELS[t["cache_channel"]]
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
        result_bytes = (max(BASE_MODEL["minimum_frame_bytes"],
                            round(raw_size * profile_model["result_fraction"] * regime_model["channel_factor"])) +
                        channel_model["overhead"]) if raw_size else 0
        compress_ns = (BASE_MODEL["base_compress_ns"] + raw_size * BASE_MODEL["compress_ns_per_byte"] + c_workers - 1) // c_workers
        uplink_rate = (BASE_MODEL["uplink_bytes_per_ns"] * c_workers /
                       (profile_model["uplink_factor"] * channel_model["uplink_factor"]))
        input_ready_ns = compress_ns + _ceil_ratio(source_bytes, uplink_rate) + BASE_MODEL["network_rtt_ns"]
        compile_work_ns = (BASE_MODEL["base_compile_ns"] + raw_size * BASE_MODEL["compile_ns_per_byte"])
        compile_work_ns = round(compile_work_ns * corpus_model["compile_factor"] * regime_model["compile_factor"])
        compile_ns = (compile_work_ns + f_workers - 1) // f_workers
        downlink_rate = (BASE_MODEL["downlink_bytes_per_ns"] * f_workers /
                         (profile_model["downlink_factor"] * channel_model["downlink_factor"]))
        return_ns = _ceil_ratio(result_bytes, downlink_rate)
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
            "model_id": f"{BASE_MODEL['id']}:{cell['corpus']}:{cell['profile']}:{cell['regime']}",
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


def predict(manifest_path: Path, out_path: Path, sim_binary: Path | None = None) -> dict[str, object]:
    """Produce a raw cumulative prediction curve and authenticated sidecar.

    ``sim_binary`` remains an ignored compatibility argument for callers of
    the rejected scaffold.  It is never opened or executed.
    """
    del sim_binary
    manifest, input_raw, input_facts, topology_facts, manifest_sha, topology, cell = load_inputs(manifest_path)
    rows = _predict_curve(input_raw, topology, cell)
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
            "name": "icecream-s8-causal-performance-model", "version": BASE_MODEL["id"],
            "route_trace_consumed": False, "action_trace_input": False,
            "model_assumptions": {"base": BASE_MODEL, "corpus": CORPUS_MODELS[cell["corpus"]],
                                  "profile": PROFILE_MODELS[cell["profile"]],
                                  "regime": REGIME_MODELS[cell["regime"]]},
            "topology_effects": {"c_workers": "producer compression and uplink share",
                                  "f_workers": "compile, return and commit share",
                                  "cache_channel": "fixed C_TO_F source and F_TO_C result"},
        },
        "provenance": {"manifest_sha256": manifest_sha, "curve_points": len(rows)},
    }
    _write_new(sidecar_path, canonical_bytes(sidecar) + b"\n", "artifact_manifest")
    return sidecar


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    # Accepted for source compatibility; the predictor never invokes it.
    parser.add_argument("--sim", type=Path)
    args = parser.parse_args(argv)
    try:
        predict(args.manifest.absolute(), args.out.absolute(), args.sim)
    except PredictionError as exc:
        print(str(exc), file=sys.stderr)
        return 77
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
