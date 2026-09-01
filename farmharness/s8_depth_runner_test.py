from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

import s8_depth_runner as runner
from s8_schema import CORPORA, PROFILES, REGIMES, SPLITS


def _write(path: Path, raw: bytes | str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw.encode() if isinstance(raw, str) else raw)


def _matrix(path: Path) -> None:
    cells = [{"cell": f"{corpus}/{profile}/{regime}", "split": SPLITS[corpus]}
             for corpus in CORPORA for profile in PROFILES for regime in REGIMES]
    value = {
        "schema": runner.MATRIX_AUDIT_SCHEMA, "status": "PASS",
        "matrix": {"expected_cells": 32, "completed_cells": 32, "missing_cells": [],
                   "invalid_candidates": [], "calibration_cells": 16,
                   "held_out_validation_cells": 16},
        "cells": cells,
    }
    _write(path, json.dumps(value, sort_keys=True) + "\n")


def _inputs(tmp_path: Path, count: int = 201) -> tuple[Path, Path]:
    root = tmp_path / "corpus"
    manifest = root / "manifest.txt"
    paths = []
    for index in range(count):
        path = root / "build" / f"unit-{index:04d}.ii"
        _write(path, f"# 1 \"unit-{index}\"\nint unit_{index}();\n")
        paths.append(str(path))
    _write(manifest, "\n".join(paths) + "\n")
    return root, manifest


def test_depths_select_authenticated_order_and_preserve_single_result_dir(tmp_path: Path) -> None:
    source_root, manifest = _inputs(tmp_path)
    matrix = tmp_path / "matrix.json"
    _matrix(matrix)
    common = (manifest, source_root, matrix, "DuckDB", "P29", "cold")
    plans = {}
    for depth in (100, 200, "full"):
        result_dir = tmp_path / f"s8-DuckDB-P29-cold-20260829T000000Z-{depth}"
        plan = runner.build_plan(*common[:3], result_dir, *common[3:], depth)
        plans[depth] = plan
        assert len(plan["inputs"]) == (depth if isinstance(depth, int) else 201)
        assert plan["request"]["depth_class"] == ("full" if depth == "full" else str(depth))
        assert plan["scheduling"]["depth_class"] == plan["request"]["depth_class"]
        assert plan["inputs"][0]["ordinal"] == 0
        assert plan["inputs"][-1]["ordinal"] == len(plan["inputs"]) - 1
        assert not result_dir.exists()
        assert plan["result"]["raw_jsonl"] == ["predictive_sim.jsonl", "live_summary.jsonl", "records.jsonl"]
        assert plan["execution_contract"]["status"] == "READY_MULTI_TU_PREDICTOR"
    assert plans[100]["source_manifest"]["sha256"] == plans[200]["source_manifest"]["sha256"]


def test_short_corpus_repeats_explicit_build_occurrences_for_100_and_200(tmp_path: Path) -> None:
    source_root, manifest = _inputs(tmp_path, 50)
    matrix = tmp_path / "matrix.json"
    _matrix(matrix)
    for depth, cycles in ((100, 2), (200, 4)):
        result_dir = tmp_path / f"s8-fmt-ZSTD_TU-cold-20260829T000000Z-{depth}"
        plan = runner.build_plan(manifest, source_root, matrix, result_dir,
                                 "fmt", "ZSTD_TU", "cold", depth)
        assert len(plan["inputs"]) == depth
        assert [item["ordinal"] for item in plan["inputs"]] == list(range(depth))
        assert plan["inputs"][0]["path"] == plan["inputs"][50]["path"]
        assert plan["inputs"][0]["sha256"] == plan["inputs"][50]["sha256"]
        assert plan["request"]["source_selection"] == {
            "policy": "manifest_order_cycles_then_prefix",
            "source_manifest_entries": 50,
            "complete_build_cycles": cycles,
            "tail_entries": 0,
            "occurrence_identity": "ordinal_plus_source_relative_plus_sha256_plus_bytes",
        }


def test_repeat_full_requires_matching_prior_full_plan(tmp_path: Path) -> None:
    source_root, manifest = _inputs(tmp_path, 4)
    matrix = tmp_path / "matrix.json"
    _matrix(matrix)
    full_dir = tmp_path / "s8-DuckDB-P29-cold-20260829T000000Z-full"
    full = runner.build_plan(manifest, source_root, matrix, full_dir,
                             "DuckDB", "P29", "cold", "full")
    full_path = tmp_path / "full-plan.json"
    _write(full_path, json.dumps(full, sort_keys=True) + "\n")
    repeat_dir = tmp_path / "s8-DuckDB-P29-cold-20260829T010000Z-repeat-full"
    repeat = runner.build_plan(manifest, source_root, matrix, repeat_dir,
                               "DuckDB", "P29", "cold", "repeat-full", full_path)
    assert repeat["request"]["requested_curve_points"] == "repeat-full"
    assert full["request"]["depth_class"] == "full"
    assert repeat["request"]["depth_class"] == "full"
    assert repeat["scheduling"]["depth_class"] == "full"
    assert repeat["repeat_of"]["sha256"] == hashlib.sha256(full_path.read_bytes()).hexdigest()
    assert len(repeat["inputs"]) == 4


def test_repeat_full_and_incomplete_matrix_fail_closed(tmp_path: Path) -> None:
    source_root, manifest = _inputs(tmp_path, 2)
    matrix = tmp_path / "matrix.json"
    _matrix(matrix)
    result_dir = tmp_path / "s8-DuckDB-P29-cold-20260829T000000Z-repeat-full"
    with pytest.raises(runner.DepthPlanError, match="repeat_of:required"):
        runner.build_plan(manifest, source_root, matrix, result_dir,
                          "DuckDB", "P29", "cold", "repeat-full")
    value = json.loads(matrix.read_text())
    value["matrix"]["completed_cells"] = 31
    _write(matrix, json.dumps(value) + "\n")
    with pytest.raises(runner.DepthPlanError, match="32_cell_precondition_failed"):
        runner.build_plan(manifest, source_root, matrix,
                          tmp_path / "s8-DuckDB-P29-cold-20260829T020000Z-full",
                          "DuckDB", "P29", "cold", "full")
