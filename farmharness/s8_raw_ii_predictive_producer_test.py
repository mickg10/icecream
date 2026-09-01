from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

import s8_depth_runner as depth
import s8_predictive_live_normalizer as normalizer
from s8_raw_ii_predictive_producer import (
    ENGINE_SCHEMA,
    FORMULA,
    RawIIError,
    SCHEMA,
    WITNESS_SCHEMA,
    produce,
)


def _inputs(tmp_path: Path) -> tuple[Path, Path, str, int]:
    source = tmp_path / "tu.cc"
    source.write_bytes(b"int main() { return 0; }\n")
    source_manifest = tmp_path / "sources.txt"
    source_manifest.write_text(str(source) + "\n")
    matrix = tmp_path / "matrix.json"
    matrix.write_text(json.dumps({
        "schema": "icecream-s8-matrix-audit-v1", "status": "PASS",
        "matrix": {"expected_cells": 32, "completed_cells": 32,
                    "missing_cells": [], "invalid_candidates": [],
                    "calibration_cells": 16, "held_out_validation_cells": 16},
        "cells": [],
    }) + "\n")
    result = tmp_path / "s8-DuckDB-RAW_II-cold-20260901T000000Z"
    plan = depth.build_plan(source_manifest, tmp_path, matrix, result,
                            "DuckDB", "RAW_II", "cold", 100, topology="C1F1")
    plan_path = tmp_path / "depth-plan.json"
    plan_path.write_text(json.dumps(plan, sort_keys=True) + "\n")
    digest = plan["inputs"][0]["sha256"]
    size = plan["inputs"][0]["bytes"]
    return plan_path, source, digest, size


def _control_inputs(tmp_path: Path, plan_path: Path, digest: str, size: int) -> tuple[Path, Path]:
    cell = {"corpus": "DuckDB", "profile": "RAW_II", "regime": "cold"}
    witness = tmp_path / "witness.json"
    witness.write_text(json.dumps({
        "schema": WITNESS_SCHEMA, "semantics": normalizer.SEMANTICS,
        "cell": cell, "split": "held_out_validation", "formula": FORMULA,
        "rows": [{"source_sha256": digest, "source_bytes": size,
                  "c_to_f": {"compile_file_bytes": 10, "file_chunk_bytes": 20,
                             "end_bytes": 3, "total_bytes": 33}}],
    }, sort_keys=True) + "\n")
    engine = tmp_path / "engine.json"
    engine.write_text(json.dumps({
        "schema": ENGINE_SCHEMA, "semantics": normalizer.SEMANTICS,
        "cell": cell, "split": "held_out_validation",
        "control_baseline": normalizer.CONTROL_BASELINE,
        "engine_scope": "raw_ii_control_engine", "model_id": "raw-control-v1",
        "rows": [{"source_sha256": digest, "f_to_c_bytes": 17, "elapsed_ns": 101}],
    }, sort_keys=True) + "\n")
    return witness, engine


def test_raw_producer_binds_legacy_formula_and_normalizer(tmp_path: Path) -> None:
    plan, _source, digest, size = _inputs(tmp_path)
    witness, engine = _control_inputs(tmp_path, plan, digest, size)
    output = tmp_path / "result"
    produced = produce(plan, witness, engine, output, "100")
    assert produced["schema"] == SCHEMA
    manifest = output / "predictive_curve_manifest.json"
    value = json.loads(manifest.read_text())
    assert value["identity"]["profile"] == "RAW_II"
    assert value["control_baseline"] == normalizer.CONTROL_BASELINE
    row = json.loads((output / "predictive_sim.jsonl").read_text().splitlines()[0])
    assert row["cumulative"]["C_TO_F_bytes"] == 33
    assert row["cumulative"]["F_TO_C_bytes"] == 17
    assert row["cumulative"]["channel_bytes"] == 50

    # Exercise the actual comparison boundary with an independently named
    # live curve; RAW_II's control declaration must survive all three records.
    live_curve = tmp_path / "live.jsonl"
    live_curve.write_bytes((output / "predictive_sim.jsonl").read_bytes())
    live_manifest = tmp_path / "live_curve_manifest.json"
    live_value = dict(value)
    live_value["curve"] = {"path": live_curve.name,
                            "sha256": hashlib.sha256(live_curve.read_bytes()).hexdigest(),
                            "bytes": live_curve.stat().st_size}
    live_value["provenance"] = {"mode": "live", "producer": "raw-ii-live-control",
                                 "trace_free": False}
    live_manifest.write_text(json.dumps(live_value, sort_keys=True) + "\n")
    records = normalizer.normalize(manifest, live_manifest, tmp_path / "records.jsonl")
    assert [record["record_type"] for record in records] == [
        "predictive_sim", "live", "comparison"]
    assert all(record["control_baseline"] == normalizer.CONTROL_BASELINE
               for record in records)


def test_raw_producer_rejects_formula_or_engine_scope_mutation(tmp_path: Path) -> None:
    plan, _source, digest, size = _inputs(tmp_path)
    witness, engine = _control_inputs(tmp_path, plan, digest, size)
    value = json.loads(witness.read_text())
    value["rows"][0]["c_to_f"]["total_bytes"] = 34
    witness.write_text(json.dumps(value) + "\n")
    with pytest.raises(RawIIError, match="formula_mismatch"):
        produce(plan, witness, engine, tmp_path / "result", "100")

    # The F-to-C/elapsed source is a distinct control-engine scope; a generic
    # compressed engine manifest must never be accepted as a fallback.
    witness, engine = _control_inputs(tmp_path, plan, digest, size)
    engine_value = json.loads(engine.read_text())
    engine_value["engine_scope"] = "p29_predictor"
    engine.write_text(json.dumps(engine_value) + "\n")
    with pytest.raises(RawIIError, match="engine_manifest:scope_or_formula_invalid|engine_manifest:scope_invalid"):
        produce(plan, witness, engine, tmp_path / "result-engine", "100")
