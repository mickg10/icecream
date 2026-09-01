from __future__ import annotations

import hashlib
import json
import subprocess
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
    plan = json.loads(plan_path.read_text())
    occurrences = plan["inputs"]
    witness = tmp_path / "witness.json"
    witness.write_text(json.dumps({
        "schema": WITNESS_SCHEMA, "semantics": normalizer.SEMANTICS,
        "cell": cell, "split": "held_out_validation", "formula": FORMULA,
        "rows": [{"ordinal": item["ordinal"], "source_relative": item["source_relative"],
                  "source_sha256": item["sha256"], "source_bytes": item["bytes"],
                  "c_to_f": {"compile_file_bytes": 10, "file_chunk_bytes": 20,
                             "end_bytes": 3, "total_bytes": 33}}
                 for item in occurrences],
    }, sort_keys=True) + "\n")
    engine = tmp_path / "engine.json"
    engine.write_text(json.dumps({
        "schema": ENGINE_SCHEMA, "semantics": normalizer.SEMANTICS,
        "cell": cell, "split": "held_out_validation",
        "control_baseline": normalizer.CONTROL_BASELINE,
        "engine_scope": "raw_ii_control_engine", "model_id": "raw-control-v1",
        "rows": [{"ordinal": item["ordinal"], "source_relative": item["source_relative"],
                  "source_sha256": item["sha256"], "source_bytes": item["bytes"],
                  "f_to_c_bytes": 17, "elapsed_ns": 101}
                 for item in occurrences],
    }, sort_keys=True) + "\n")
    return witness, engine


def _product_root(tmp_path: Path) -> Path:
    root = tmp_path / "product"
    if not (root / ".git").is_dir():
        root.mkdir(exist_ok=True)
        (root / "tracked.txt").write_text("product\n")
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        subprocess.run(["git", "-C", str(root), "config", "user.email",
                        "test@example.invalid"], check=True)
        subprocess.run(["git", "-C", str(root), "config", "user.name", "RAW test"], check=True)
        subprocess.run(["git", "-C", str(root), "add", "tracked.txt"], check=True)
        subprocess.run(["git", "-C", str(root), "commit", "-qm", "product"], check=True)
    return root


def test_raw_producer_binds_legacy_formula_and_normalizer(tmp_path: Path) -> None:
    plan, _source, digest, size = _inputs(tmp_path)
    witness, engine = _control_inputs(tmp_path, plan, digest, size)
    output = Path(json.loads(plan.read_text())["result"]["directory"])
    produced = produce(plan, witness, engine, output, "100", _product_root(tmp_path))
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
        produce(plan, witness, engine,
                Path(json.loads(plan.read_text())["result"]["directory"]), "100",
                _product_root(tmp_path))

    # The F-to-C/elapsed source is a distinct control-engine scope; a generic
    # compressed engine manifest must never be accepted as a fallback.
    witness, engine = _control_inputs(tmp_path, plan, digest, size)
    engine_value = json.loads(engine.read_text())
    engine_value["engine_scope"] = "p29_predictor"
    engine.write_text(json.dumps(engine_value) + "\n")
    with pytest.raises(RawIIError, match="engine_manifest:scope_or_formula_invalid|engine_manifest:scope_invalid"):
        produce(plan, witness, engine, tmp_path / "result-engine", "100", _product_root(tmp_path))


def test_raw_producer_keys_duplicate_content_by_occurrence_and_parallel_makespan(
        tmp_path: Path) -> None:
    source_root = tmp_path / "sources"
    source_root.mkdir()
    source_paths = []
    for name in ("a.cc", "b.cc", "c.cc", "d.cc"):
        path = source_root / name
        path.write_bytes(b"identical translation unit\n")
        source_paths.append(path)
    source_manifest = tmp_path / "sources.txt"
    source_manifest.write_text("".join(str(path) + "\n" for path in source_paths))
    matrix = tmp_path / "matrix.json"
    matrix.write_text(json.dumps({
        "schema": "icecream-s8-matrix-audit-v1", "status": "PASS",
        "matrix": {"expected_cells": 32, "completed_cells": 32,
                    "missing_cells": [], "invalid_candidates": [],
                    "calibration_cells": 16, "held_out_validation_cells": 16},
        "cells": [],
    }) + "\n")
    result = tmp_path / "s8-DuckDB-RAW_II-cold-20260901T010000Z"
    plan_value = depth.build_plan(source_manifest, source_root, matrix, result,
                                  "DuckDB", "RAW_II", "cold", 100, topology="C1F20")
    plan_path = tmp_path / "depth-plan.json"
    plan_path.write_text(json.dumps(plan_value, sort_keys=True) + "\n")
    cell = plan_value["cell"]
    witness_rows = []
    engine_rows = []
    for item in plan_value["inputs"]:
        witness_rows.append({
            "ordinal": item["ordinal"], "source_relative": item["source_relative"],
            "source_sha256": item["sha256"], "source_bytes": item["bytes"],
            "c_to_f": {"compile_file_bytes": 10, "file_chunk_bytes": 20,
                       "end_bytes": 3, "total_bytes": 33}})
        engine_rows.append({
            "ordinal": item["ordinal"], "source_relative": item["source_relative"],
            "source_sha256": item["sha256"], "source_bytes": item["bytes"],
            "f_to_c_bytes": 17, "elapsed_ns": 100})
    witness = tmp_path / "witness.json"
    witness.write_text(json.dumps({"schema": WITNESS_SCHEMA,
                                   "semantics": normalizer.SEMANTICS,
                                   "cell": cell, "split": "held_out_validation",
                                   "formula": FORMULA, "rows": witness_rows}, sort_keys=True) + "\n")
    engine = tmp_path / "engine.json"
    engine.write_text(json.dumps({"schema": ENGINE_SCHEMA,
                                  "semantics": normalizer.SEMANTICS,
                                  "cell": cell, "split": "held_out_validation",
                                  "control_baseline": normalizer.CONTROL_BASELINE,
                                  "engine_scope": "raw_ii_control_engine",
                                  "model_id": "raw-control-v1", "rows": engine_rows},
                                 sort_keys=True) + "\n")
    output = result
    produce(plan_path, witness, engine, output, "100", _product_root(tmp_path))
    rows = [json.loads(line) for line in
            (output / "predictive_sim.jsonl").read_text().splitlines()]
    assert len({row["occurrence"]["sha256"] for row in rows}) == 1
    assert len({row["occurrence"]["source_relative"] for row in rows}) == 4
    assert [row["scheduling"]["global_slot"] for row in rows[:4]] == [0, 1, 2, 3]
    assert [row["cumulative"]["elapsed_ns"] for row in rows[:4]] == [100, 100, 100, 100]
    assert rows[-1]["cumulative"]["elapsed_ns"] < 100 * 100
    producer = json.loads((output / "producer_manifest.json").read_text())
    assert producer["scheduling"]["makespan_ns"] == rows[-1]["cumulative"]["elapsed_ns"]
    assert producer["scheduling"]["serial_service_ns"] == 100 * 100


def test_raw_producer_engine_v2_rejects_split_service_fields(tmp_path: Path) -> None:
    plan, _source, digest, size = _inputs(tmp_path)
    witness, engine = _control_inputs(tmp_path, plan, digest, size)
    value = json.loads(engine.read_text())
    value["rows"][0]["source_service_ns"] = 40
    engine.write_text(json.dumps(value) + "\n")
    with pytest.raises(RawIIError, match="engine_manifest:row_invalid"):
        produce(plan, witness, engine,
                Path(json.loads(plan.read_text())["result"]["directory"]), "100",
                _product_root(tmp_path))


def test_raw_producer_retains_plan_snapshot_and_generated_inventory(tmp_path: Path) -> None:
    plan, _source, digest, size = _inputs(tmp_path)
    witness, engine = _control_inputs(tmp_path, plan, digest, size)
    product = _product_root(tmp_path)
    generated = product / "generated-object.bin"
    generated.write_bytes(b"generated product output\n")
    output = Path(json.loads(plan.read_text())["result"]["directory"])
    produce(plan, witness, engine, output, "100", product)
    value = json.loads((output / "producer_manifest.json").read_text())
    plan_facts = value["plan"]
    assert plan_facts == {
        "path": str(plan.resolve()),
        "bytes": plan.stat().st_size,
        "sha256": hashlib.sha256(plan.read_bytes()).hexdigest(),
    }
    inventory = value["product_git"]["untracked"]
    assert inventory["count"] == 1
    assert inventory["entries"][0]["path"] == generated.name
    assert inventory["entries"][0]["sha256"] == hashlib.sha256(generated.read_bytes()).hexdigest()


def test_raw_producer_rejects_duplicate_json_keys(tmp_path: Path) -> None:
    plan, _source, digest, size = _inputs(tmp_path)
    _witness, engine = _control_inputs(tmp_path, plan, digest, size)
    witness = tmp_path / "witness.json"
    witness.write_text('{"schema":"' + WITNESS_SCHEMA + '","schema":"' +
                       WITNESS_SCHEMA + '"}\n')
    with pytest.raises(RawIIError, match="duplicate_json_key:schema"):
        produce(plan, witness, engine,
                Path(json.loads(plan.read_text())["result"]["directory"]), "100",
                _product_root(tmp_path))


def test_raw_producer_resnapshots_plan_inputs_before_use(tmp_path: Path) -> None:
    plan, source, digest, size = _inputs(tmp_path)
    witness, engine = _control_inputs(tmp_path, plan, digest, size)
    source.write_bytes(source.read_bytes() + b"changed\n")
    with pytest.raises(RawIIError, match="plan:(input_sequence_not_declared_selection|input_changed)"):
        produce(plan, witness, engine,
                Path(json.loads(plan.read_text())["result"]["directory"]), "100",
                _product_root(tmp_path))
