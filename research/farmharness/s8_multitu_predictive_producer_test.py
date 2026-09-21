from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
from pathlib import Path

import pytest

import s8_depth_runner as depth_runner
import s8_predictive_engine as engine
import s8_predictive_live_normalizer as normalizer
from s8_predictive_engine import (
    MANIFEST_SCHEMA, SEMANTICS, TOPOLOGY_SCHEMA, canonical_bytes, load_inputs,
    new_relationship_state, predict_sequential, PredictionError,
)
from s8_multitu_predictive_producer import MultiTUPredictiveError, produce, produce_pair
from s8_schema import ALL_SPLITS, CORPORA, PROFILES, REGIMES, SPLITS


def _write(path: Path, raw: bytes | str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw.encode() if isinstance(raw, str) else raw)


def _matrix(path: Path) -> None:
    cells = [{"cell": f"{corpus}/{profile}/{regime}", "split": SPLITS[corpus],
              "status": "PASS"}
             for corpus in CORPORA for profile in PROFILES for regime in REGIMES]
    value = {
        "schema": depth_runner.MATRIX_AUDIT_SCHEMA, "status": "PASS",
        "matrix": {"expected_cells": 32, "completed_cells": 32, "missing_cells": [],
                   "invalid_candidates": [], "calibration_cells": 16,
                   "held_out_validation_cells": 16},
        "cells": cells,
    }
    _write(path, canonical_bytes(value) + b"\n")


def _template(path: Path, cell: dict[str, str]) -> Path:
    input_path = path / "template.ii"
    _write(input_path, b"template input\n")
    topology = {
        "schema": TOPOLOGY_SCHEMA, "semantics": SEMANTICS, "cell": cell,
        "topology": {"c_store_guid": "1".zfill(32), "f_store_guid": "2".zfill(32),
                     "history_nonce": 1, "c_workers": 1, "f_workers": 1,
                     "cache_channel": {"ZSTD_TU": "direct", "ZSTD_ROUTE": "route",
                                       "P29": "route", "GRZ_RESIDUAL": "residual"}[cell["profile"]]},
        "state": {"c_cache": cell["regime"], "f_cache": cell["regime"], "generation": 0},
    }
    topology_path = path / "topology.json"
    topology_raw = canonical_bytes(topology) + b"\n"
    _write(topology_path, topology_raw)
    descriptor = lambda p: {"path": p.name, "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
                            "bytes": p.stat().st_size}
    manifest = {
        "schema": MANIFEST_SCHEMA, "semantics": SEMANTICS, "cell": cell,
        "split": ALL_SPLITS[cell["corpus"]], "predictive_mode": True,
        "input": descriptor(input_path), "topology_state": descriptor(topology_path),
    }
    manifest_path = path / "engine-manifest.json"
    _write(manifest_path, canonical_bytes(manifest) + b"\n")
    return manifest_path


def _source(tmp_path: Path, count: int) -> tuple[Path, Path]:
    root = tmp_path / "corpus"
    paths: list[str] = []
    for index in range(count):
        path = root / "units" / f"unit-{index:04d}.ii"
        _write(path, f"int unit_{index}();\n")
        paths.append(str(path))
    manifest = root / "manifest.txt"
    _write(manifest, "\n".join(paths) + "\n")
    return root, manifest


def _plan(tmp_path: Path, count: int, depth: int | str, suffix: str,
          regime: str = "cold", profile: str = "P29", topology: str = "C1F1",
          corpus: str = "DuckDB") -> Path:
    root, manifest = _source(tmp_path, count)
    matrix = tmp_path / "matrix.json"
    _matrix(matrix)
    result_dir = tmp_path / f"s8-{corpus}-{profile}-{regime}-20260829T000000Z-{suffix}"
    value = depth_runner.build_plan(manifest, root, matrix, result_dir,
                                    corpus, profile, regime, depth, topology=topology)
    plan_path = tmp_path / f"{suffix}-plan.json"
    _write(plan_path, canonical_bytes(value) + b"\n")
    return plan_path


def _product_build_root(anchor: Path) -> Path:
    """Create a real clean Git product root containing the built simulator."""
    root = anchor.parent / "product-build"
    root.mkdir(parents=True, exist_ok=True)
    binary = Path(__file__).resolve().parents[2] / "cache" / "sim" / ".p50sim.bin"
    target = root / "cache" / "sim" / ".p50sim.bin"
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists():
        shutil.copy2(binary, target)
    source = Path(__file__).resolve().parents[2] / "cache" / "sim" / "p50sim.cpp"
    source_target = root / "cache" / "sim" / "p50sim.cpp"
    if not source_target.exists():
        shutil.copy2(source, source_target)
    source_root = Path(__file__).resolve().parents[2]
    build_script_target = root / "cache" / "sim" / "build_p50sim.sh"
    if not build_script_target.exists():
        shutil.copy2(source_root / "cache/sim/build_p50sim.sh", build_script_target)
    for name in ("s8_multitu_predictive_producer.py", "s8_depth_runner.py",
                 "s8_predictive_engine.py"):
        target_tool = root / "research" / "farmharness" / name
        target_tool.parent.mkdir(parents=True, exist_ok=True)
        if not target_tool.exists():
            shutil.copy2(source_root / "research" / "farmharness" / name, target_tool)
    marker = root / "product-source.txt"
    if not marker.exists():
        _write(marker, "authenticated product build fixture\n")
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        subprocess.run(["git", "-C", str(root), "config", "user.email", "tests@example.invalid"],
                       check=True)
        subprocess.run(["git", "-C", str(root), "config", "user.name", "S8 tests"], check=True)
        subprocess.run(["git", "-C", str(root), "add", "product-source.txt", "cache/sim/p50sim.cpp",
                        "cache/sim/build_p50sim.sh", "research/farmharness/s8_multitu_predictive_producer.py",
                        "research/farmharness/s8_depth_runner.py", "research/farmharness/s8_predictive_engine.py"],
                       check=True)
        subprocess.run(["git", "-C", str(root), "commit", "-qm", "product build fixture"],
                       check=True)
    def artifact(path: Path) -> dict[str, object]:
        return {"path": str(path.resolve()), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "bytes": path.stat().st_size}
    receipt = {
        "schema": "icecream-p50sim-build-v1",
        "source": {"root": str(root.resolve()),
                   "head": subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip(),
                   "tree": subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD^{tree}"], text=True).strip(),
                   "tracked_clean": True},
        "binary": artifact(target),
        "inputs": {"build_script": artifact(root / "cache/sim/build_p50sim.sh"),
                   "p50sim_source": artifact(source_target), "config_h": None,
                   "cache_makefile": None, "services_makefile": None},
        "configuration": {"with_libbsc": 0, "make_mode": "direct_sources",
                           "dependency_root": "test", "compiler_path": "g++",
                           "compiler_version": "test compiler"},
    }
    _write(root / "cache/sim/.p50sim-build.json", canonical_bytes(receipt) + b"\n")
    return root


def _produce(plan_path: Path, engine_manifest: Path, **kwargs: object) -> dict[str, object]:
    return produce(plan_path, engine_manifest, _product_build_root(plan_path), **kwargs)


def _produce_pair(first_plan: Path, repeat_plan: Path, engine_manifest: Path,
                  **kwargs: object) -> tuple[dict[str, object], dict[str, object]]:
    return produce_pair(first_plan, repeat_plan, engine_manifest,
                        _product_build_root(first_plan), **kwargs)


def test_producer_emits_100_ordered_points_and_continuous_relationship(tmp_path: Path) -> None:
    plan_path = _plan(tmp_path / "run", 100, 100, "100", "warm")
    cell = {"corpus": "DuckDB", "profile": "P29", "regime": "warm"}
    manifest = _template(tmp_path / "template", cell)
    plan = json.loads(plan_path.read_bytes())
    output = Path(plan["result"]["directory"])
    result = _produce(plan_path, manifest)
    rows = [json.loads(line) for line in (output / "predictive_sim.jsonl").read_text().splitlines()]
    assert len(rows) == 100
    assert [row["step"] for row in rows] == list(range(100))
    assert len({row["tu_id"] for row in rows}) == 100
    assert len({row["topology_digest"] for row in rows}) == 1
    assert rows[0]["startup_ns"] == 0
    assert all(row["startup_ns"] == 0 for row in rows)
    assert len(result["excluded_prewarms"]) == 1
    assert result["excluded_prewarms"][0]["points"] == 100
    assert result["excluded_prewarms"][0]["raw_bytes"] == sum(item["bytes"] for item in plan["inputs"])
    assert result["relationship"]["mode"] == "route"
    assert result["relationship"]["reset_points"] == []
    assert len(result["relationship"]["state_after_digests"]) == 100
    assert rows[-1]["cumulative"]["channel_bytes"] == sum(
        row["channel_bytes"]["total"] for row in rows)
    assert result["live_observation"].startswith("not_emitted")
    assert not (output / "live_summary.jsonl").exists()


def test_expanded_compressed_curve_is_descriptive_and_canonical_consumer_rejects(
        tmp_path: Path) -> None:
    plan_path = _plan(tmp_path / "opencv", 3, 100, "expanded", profile="ZSTD_TU",
                      corpus="OpenCV")
    manifest = _template(tmp_path / "template-opencv",
                         {"corpus": "OpenCV", "profile": "ZSTD_TU", "regime": "cold"})
    result = _produce(plan_path, manifest)
    output = Path(json.loads(plan_path.read_bytes())["result"]["directory"])
    curve_manifest = output / "predictive_curve_manifest.json"
    value = json.loads(curve_manifest.read_bytes())
    assert result["identity"]["split"] == "expanded_descriptive"
    assert value["identity"]["split"] == "expanded_descriptive"
    with pytest.raises(normalizer.NormalizationError, match="identity.split"):
        normalizer._load_manifest(curve_manifest, "predictive_sim")


def test_short_corpus_repeated_build_occurrences_produce_distinct_curve_points(
        tmp_path: Path) -> None:
    plan_path = _plan(tmp_path / "short", 50, 100, "100", profile="ZSTD_TU")
    manifest = _template(tmp_path / "template-short",
                         {"corpus": "DuckDB", "profile": "ZSTD_TU", "regime": "cold"})
    plan = json.loads(plan_path.read_text())
    output = Path(plan["result"]["directory"])
    _produce(plan_path, manifest)
    rows = [json.loads(line) for line in
            (output / "predictive_sim.jsonl").read_text().splitlines()]
    assert len(rows) == 100
    assert len({row["tu_id"] for row in rows}) == 100
    assert plan["inputs"][0]["path"] == plan["inputs"][50]["path"]
    assert plan["inputs"][0]["sha256"] == plan["inputs"][50]["sha256"]
    assert rows[0]["input_sha256"] == rows[50]["input_sha256"]
    assert rows[0]["tu_id"] != rows[50]["tu_id"]


def test_warm_curve_excludes_authenticated_prewarm_and_carries_boundary(tmp_path: Path) -> None:
    plan_path = _plan(tmp_path / "warm", 100, 100, "warm", "warm", "P29")
    plan = json.loads(plan_path.read_bytes())
    manifest = _template(tmp_path / "template-warm",
                         {"corpus": "DuckDB", "profile": "P29", "regime": "warm"})
    result = _produce(plan_path, manifest)
    output = Path(plan["result"]["directory"])
    rows = [json.loads(line) for line in (output / "predictive_sim.jsonl").read_text().splitlines()]
    assert [row["step"] for row in rows] == list(range(100))
    assert rows[0]["product_completion"]["tu_seq"] == 100
    assert rows[0]["product_completion"]["state_before_digest"] == \
        result["excluded_prewarms"][0]["state_boundaries"][0]["last_state_after_digest"]
    prewarm = result["excluded_prewarms"][0]
    assert prewarm["excluded"] is True
    assert prewarm["segment"] == "prewarm"
    assert prewarm["input_digest"] == hashlib.sha256(canonical_bytes([
        {"ordinal": item["ordinal"], "source_relative": item["source_relative"],
         "sha256": item["sha256"], "bytes": item["bytes"]} for item in plan["inputs"]
    ])).hexdigest()
    assert rows[0]["cumulative"]["C_TO_F_bytes"] == rows[0]["channel_bytes"]["C_TO_F"]


def test_warm_repeat_pair_retains_prewarm_and_two_scored_segments(tmp_path: Path) -> None:
    root, source_manifest = _source(tmp_path / "source", 3)
    matrix = tmp_path / "matrix.json"
    _matrix(matrix)
    cell_args = ("DuckDB", "ZSTD_ROUTE", "warm")
    full_dir = tmp_path / "s8-DuckDB-ZSTD_ROUTE-warm-20260829T000000Z-full"
    full = depth_runner.build_plan(source_manifest, root, matrix, full_dir, *cell_args, "full")
    full_path = tmp_path / "full-plan.json"
    _write(full_path, canonical_bytes(full) + b"\n")
    repeat_dir = tmp_path / "s8-DuckDB-ZSTD_ROUTE-warm-20260829T010000Z-repeat-full"
    repeat = depth_runner.build_plan(source_manifest, root, matrix, repeat_dir,
                                     *cell_args, "repeat-full", full_path)
    repeat_path = tmp_path / "repeat-plan.json"
    _write(repeat_path, canonical_bytes(repeat) + b"\n")
    manifest = _template(tmp_path / "template-warm-pair",
                         {"corpus": "DuckDB", "profile": "ZSTD_ROUTE", "regime": "warm"})
    full_result, repeat_result = _produce_pair(full_path, repeat_path, manifest)
    assert full_result["excluded_prewarms"]
    assert repeat_result["excluded_prewarms"]
    full_rows = [json.loads(line) for line in
                 (full_dir / "predictive_sim.jsonl").read_text().splitlines()]
    repeat_rows = [json.loads(line) for line in
                   (repeat_dir / "predictive_sim.jsonl").read_text().splitlines()]
    assert len(full_rows) == len(repeat_rows) == 3
    assert full_rows[0]["step"] == repeat_rows[0]["step"] == 0
    assert full_rows[0]["cumulative"]["C_TO_F_bytes"] == full_rows[0]["channel_bytes"]["C_TO_F"]
    assert repeat_rows[0]["product_completion"]["state_before_digest"] == \
        full_rows[-1]["product_completion"]["state_after_digest"]
    assert full_result["paired_continuation"] is False
    assert repeat_result["paired_continuation"] is True


def test_relationship_state_route_reuses_identical_tu_but_tu_profile_resets(tmp_path: Path) -> None:
    raw = b"identical translation unit\n" * 8
    for profile in ("ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"):
        cell = {"corpus": "DuckDB", "profile": profile, "regime": "cold"}
        manifest = _template(tmp_path / profile, cell)
        _value, _template_raw, _input, topology_facts, _sha, topology, _cell = load_inputs(manifest)
        state = new_relationship_state(topology, cell, topology_facts["sha256"])
        first, state = predict_sequential(raw, topology, cell, state)
        second, state = predict_sequential(raw, topology, cell, state)
        assert first["relationship_state"]["after_digest"] != second["relationship_state"]["after_digest"]
        if profile == "ZSTD_TU":
            assert second["channel_bytes"] == first["channel_bytes"]
            assert second["relationship_state"]["transition"] == "tu_reset"
            assert second["relationship_state"]["reset_point"] is True
        else:
            assert second["relationship_state"]["transition"] == "c_cache_reuse"
            assert second["relationship_state"]["reset_point"] is False


def test_relationship_state_reset_or_step_mutation_is_not_silent(tmp_path: Path) -> None:
    cell = {"corpus": "DuckDB", "profile": "ZSTD_ROUTE", "regime": "cold"}
    manifest = _template(tmp_path / "route", cell)
    _value, _raw, _input, topology_facts, _sha, topology, _cell = load_inputs(manifest)
    state = new_relationship_state(topology, cell, topology_facts["sha256"])
    _first, state = predict_sequential(b"same\n", topology, cell, state)
    _first, continued_state = predict_sequential(b"same\n", topology, cell,
                                                  new_relationship_state(topology, cell,
                                                                          topology_facts["sha256"]))
    _continued, _continued_state = predict_sequential(b"same\n", topology, cell, continued_state)
    reset_state = new_relationship_state(topology, cell, topology_facts["sha256"])
    assert reset_state["next_step"] == 0
    state["next_step"] = 0
    with pytest.raises(PredictionError, match="relationship_state"):
        predict_sequential(b"same\n", topology, cell, state)


def test_reorder_and_mutation_fail_before_producer_output(tmp_path: Path) -> None:
    plan_path = _plan(tmp_path / "reorder", 100, 100, "reorder")
    value = json.loads(plan_path.read_bytes())
    value["inputs"][0], value["inputs"][1] = value["inputs"][1], value["inputs"][0]
    _write(plan_path, canonical_bytes(value) + b"\n")
    manifest = _template(tmp_path / "template", {"corpus": "DuckDB", "profile": "P29", "regime": "cold"})
    with pytest.raises(MultiTUPredictiveError, match="input_sequence"):
        _produce(plan_path, manifest)
    assert not Path(value["result"]["directory"]).exists()

    plan_path = _plan(tmp_path / "mutation", 100, 100, "mutation")
    plan = json.loads(plan_path.read_bytes())
    Path(plan["inputs"][0]["path"]).write_bytes(b"mutated\n")
    with pytest.raises(MultiTUPredictiveError):
        _produce(plan_path, manifest)
    assert not Path(plan["result"]["directory"]).exists()

    plan_path = _plan(tmp_path / "duplicate", 100, 100, "duplicate")
    value = json.loads(plan_path.read_bytes())
    value["inputs"][1] = value["inputs"][0]
    _write(plan_path, canonical_bytes(value) + b"\n")
    with pytest.raises(MultiTUPredictiveError, match="input_sequence"):
        _produce(plan_path, manifest)


def test_repeat_full_has_same_input_identity_but_distinct_run_identity(tmp_path: Path) -> None:
    root, source_manifest = _source(tmp_path / "source", 3)
    matrix = tmp_path / "matrix.json"
    _matrix(matrix)
    full_dir = tmp_path / "s8-DuckDB-P29-cold-20260829T000000Z-full"
    full = depth_runner.build_plan(source_manifest, root, matrix, full_dir,
                                   "DuckDB", "P29", "cold", "full")
    full_path = tmp_path / "full-plan.json"
    _write(full_path, canonical_bytes(full) + b"\n")
    repeat_dir = tmp_path / "s8-DuckDB-P29-cold-20260829T010000Z-repeat-full"
    repeat = depth_runner.build_plan(source_manifest, root, matrix, repeat_dir,
                                     "DuckDB", "P29", "cold", "repeat-full", full_path)
    repeat_path = tmp_path / "repeat-plan.json"
    _write(repeat_path, canonical_bytes(repeat) + b"\n")
    manifest = _template(tmp_path / "template", {"corpus": "DuckDB", "profile": "P29", "regime": "cold"})
    full_result, repeat_result = _produce_pair(full_path, repeat_path, manifest)
    assert full_result["identity"]["input_digest"] == repeat_result["identity"]["input_digest"]
    assert full_result["identity"]["run_id"] != repeat_result["identity"]["run_id"]
    assert full_result["outputs"]["predictive_sim"]["points"] == 3
    assert repeat_result["outputs"]["predictive_sim"]["points"] == 3


def test_output_manifest_is_consumable_by_existing_normalizer(tmp_path: Path) -> None:
    plan_path = _plan(tmp_path / "run", 100, 100, "normalize")
    manifest = _template(tmp_path / "template", {"corpus": "DuckDB", "profile": "P29", "regime": "cold"})
    result = _produce(plan_path, manifest)
    output = Path(json.loads(plan_path.read_bytes())["result"]["directory"])
    predictive_manifest = output / "predictive_curve_manifest.json"
    predicted = (output / "predictive_sim.jsonl").read_bytes()
    live_curve = output / "live.jsonl"
    live_curve.write_bytes(predicted)
    live_value = json.loads(predictive_manifest.read_bytes())
    live_value["provenance"] = {"mode": "live", "producer": "authenticated-live-v1", "trace_free": False}
    live_value["curve"] = {"path": live_curve.name, "sha256": hashlib.sha256(predicted).hexdigest(),
                            "bytes": len(predicted)}
    live_manifest = output / "live-manifest.json"
    _write(live_manifest, canonical_bytes(live_value) + b"\n")
    records = normalizer.normalize(predictive_manifest, live_manifest, output / "records.jsonl")
    assert [record["record_type"] for record in records] == ["predictive_sim", "live", "comparison"]
    assert len(records[0]["raw_cumulative_curve"]) == 100


def test_non_zstd_p29_keeps_product_endpoint_bytes(tmp_path: Path) -> None:
    profile = "P29"
    plan_path = _plan(tmp_path / profile, 100, 100, profile, profile=profile)
    cell = {"corpus": "DuckDB", "profile": profile, "regime": "cold"}
    manifest = _template(tmp_path / f"template-{profile}", cell)
    result = _produce(plan_path, manifest)
    output = Path(json.loads(plan_path.read_bytes())["result"]["directory"])
    rows = [json.loads(line) for line in (output / "predictive_sim.jsonl").read_text().splitlines()]
    _value, _raw, _input, _topology_facts, _sha, topology, _cell = load_inputs(manifest)
    assert rows[0]["channel_bytes"]["C_TO_F"] == rows[0]["product_completion"]["encoded_source_bytes"]
    assert result["codec"]["mode"] == "product_endpoint_batch"
    assert result["codec"]["native_product"] is True


def test_grz_without_libbsc_fails_closed(tmp_path: Path) -> None:
    plan_path = _plan(tmp_path / "grz", 100, 100, "GRZ_RESIDUAL", profile="GRZ_RESIDUAL")
    manifest = _template(tmp_path / "template-grz",
                         {"corpus": "DuckDB", "profile": "GRZ_RESIDUAL", "regime": "cold"})
    with pytest.raises(MultiTUPredictiveError, match="requires a simulator built with --with-libbsc"):
        _produce(plan_path, manifest)


def test_twenty_relationships_have_authenticated_warm_boundaries(tmp_path: Path) -> None:
    plan_path = _plan(tmp_path / "twenty", 100, 100, "twenty", "warm", "ZSTD_ROUTE", "C1F20")
    cell = {"corpus": "DuckDB", "profile": "ZSTD_ROUTE", "regime": "warm"}
    manifest = _template(tmp_path / "template-twenty", cell)
    result = _produce(plan_path, manifest)
    output = Path(json.loads(plan_path.read_bytes())["result"]["directory"])
    rows = [json.loads(line) for line in (output / "predictive_sim.jsonl").read_text().splitlines()]
    assert all(row["startup_ns"] == 0 for row in rows)
    assert len({row["relationship_id"] for row in rows}) == 20
    assert {row["scheduling"]["global_slot"] for row in rows} == set(range(40))
    assert len(result["excluded_prewarms"][0]["state_boundaries"]) == 20


def test_authenticated_topology_schedule_declares_exact_capacity_and_makespan(tmp_path: Path) -> None:
    one_path = _plan(tmp_path / "one", 100, 100, "one", topology="C1F1")
    one = json.loads(one_path.read_bytes())
    assert isinstance(one["scheduling"], dict)
    assert one["scheduling"]["f_relationships"] == 1
    assert one["scheduling"]["global_slots"] == 1
    assert one["scheduling"]["execution_slots"] == 1
    assert one["scheduling"]["stream_capacity_tus"] == 100000
    assert one["scheduling"]["stream_capacity_status"] == "DECLARED"
    assert {item["global_slot"] for item in one["scheduling"]["assignments"]} == {0}

    plan_path = _plan(tmp_path / "twenty", 100, 100, "twenty-schedule", topology="C1F20")
    value = json.loads(plan_path.read_bytes())
    scheduling = value["scheduling"]
    assert (scheduling["f_relationships"], scheduling["slots_per_f"],
            scheduling["global_slots"], scheduling["execution_slots"],
            scheduling["stream_capacity_tus"], scheduling["stream_capacity_status"]) == (20, 2, 40, 40, None, "NOT_DECLARED")
    assert {item["global_slot"] for item in scheduling["assignments"]} == set(range(40))
    assert {item["f_relationship"] for item in scheduling["assignments"]} == set(range(20))
    assert {item["per_f_slot"] for item in scheduling["assignments"]} == {0, 1}
    manifest = _template(tmp_path / "template-schedule",
                         {"corpus": "DuckDB", "profile": "P29", "regime": "cold"})
    _produce(plan_path, manifest)
    rows = [json.loads(line) for line in
            (Path(value["result"]["directory"]) / "predictive_sim.jsonl").read_text().splitlines()]
    serial = sum(row["scheduling"]["service_ns"] for row in rows)
    assert rows[-1]["cumulative"]["elapsed_ns"] < serial
    assert rows[-1]["schedule_summary"]["makespan_ns"] == rows[-1]["cumulative"]["elapsed_ns"]
    assert rows[-1]["schedule_summary"]["serial_service_ns"] == serial
    assert all(row["scheduling"]["finish_ns"] ==
               row["scheduling"]["source_start_ns"] + row["scheduling"]["service_ns"] for row in rows)
    first_f = [row for row in rows if row["scheduling"]["f_relationship"] == 0]
    assert first_f[1]["scheduling"]["source_start_ns"] == first_f[0]["scheduling"]["source_finish_ns"]
    assert first_f[1]["scheduling"]["source_start_ns"] < first_f[0]["scheduling"]["finish_ns"]


def test_planning_policy_is_explicit_for_variable_input_sizes(tmp_path: Path) -> None:
    inputs = [{"bytes": 1}, {"bytes": 101}, {"bytes": 7}]
    schedule = depth_runner.build_schedule(inputs, "C1F20")
    assert schedule["assignment_policy"] == "least_planned_load_then_lowest_slot"
    assert schedule["assignment_policy_version"] == "s8-planned-load-v1"
    assert schedule["service_duration_model"] == "base_compile_ns_plus_24_ns_per_input_byte"
    assert [item["planning_service_ns"] for item in schedule["assignments"]] == [1500024, 1502424, 1500168]
    assert [item["global_slot"] for item in schedule["assignments"]] == [0, 1, 2]


def test_c1f1_schedule_is_serial_and_topology_mutation_fails_closed(tmp_path: Path) -> None:
    plan_path = _plan(tmp_path / "serial", 100, 100, "serial", topology="C1F1")
    manifest = _template(tmp_path / "template-serial",
                         {"corpus": "DuckDB", "profile": "P29", "regime": "cold"})
    _produce(plan_path, manifest)
    value = json.loads(plan_path.read_bytes())
    rows = [json.loads(line) for line in
            (Path(value["result"]["directory"]) / "predictive_sim.jsonl").read_text().splitlines()]
    assert all(row["scheduling"]["global_slot"] == 0 for row in rows)
    assert rows[-1]["cumulative"]["elapsed_ns"] == sum(
        row["scheduling"]["service_ns"] for row in rows)

    mutated = _plan(tmp_path / "mutated", 100, 100, "mutated", topology="C1F20")
    changed = json.loads(mutated.read_bytes())
    changed["scheduling"]["global_slots"] = 39
    _write(mutated, canonical_bytes(changed) + b"\n")
    with pytest.raises(MultiTUPredictiveError, match="scheduling_authenticated_mismatch"):
        _produce(mutated, manifest)


def test_product_identity_requires_clean_build_root_and_timeout_contract(tmp_path: Path) -> None:
    plan_path = _plan(tmp_path / "identity", 100, 100, "identity")
    manifest = _template(tmp_path / "template-identity",
                         {"corpus": "DuckDB", "profile": "P29", "regime": "cold"})
    build_root = _product_build_root(plan_path)
    result = produce(plan_path, manifest, build_root)
    expected_commit = subprocess.check_output(
        ["git", "-C", str(build_root), "rev-parse", "HEAD"], text=True).strip()
    expected_tree = subprocess.check_output(
        ["git", "-C", str(build_root), "rev-parse", "HEAD^{tree}"], text=True).strip()
    assert result["identity"]["source_commit"] == expected_commit
    assert result["identity"]["source_tree"] == expected_tree
    assert result["product_build_root"] == str(build_root.resolve())
    assert result["execution"]["batch_timeout_seconds"] == 260

    changed_source = build_root / "cache" / "sim" / "p50sim.cpp"
    changed_source.write_bytes(changed_source.read_bytes() + b"\n")
    dirty_plan = _plan(tmp_path / "dirty", 100, 100, "dirty")
    dirty_manifest = _template(tmp_path / "template-dirty",
                               {"corpus": "DuckDB", "profile": "P29", "regime": "cold"})
    with pytest.raises(MultiTUPredictiveError, match="tracked_worktree_dirty"):
        produce(dirty_plan, dirty_manifest, build_root)

    timeout_plan = _plan(tmp_path / "timeout", 100, 100, "timeout")
    timeout_value = json.loads(timeout_plan.read_bytes())
    timeout_value["execution_contract"]["timeout_policy"] = "180"
    _write(timeout_plan, canonical_bytes(timeout_value) + b"\n")
    with pytest.raises(MultiTUPredictiveError, match="multi_tu_producer_contract_invalid"):
        _produce(timeout_plan, dirty_manifest)


def test_standalone_repeat_fails_and_pair_carries_terminal_codec_state(tmp_path: Path) -> None:
    root, source_manifest = _source(tmp_path / "source", 3)
    matrix = tmp_path / "matrix.json"
    _matrix(matrix)
    cell_args = ("DuckDB", "ZSTD_ROUTE", "cold")
    full_dir = tmp_path / "s8-DuckDB-ZSTD_ROUTE-warm-20260829T000000Z-full"
    full = depth_runner.build_plan(source_manifest, root, matrix, full_dir, *cell_args, "full")
    full_path = tmp_path / "full-plan.json"
    _write(full_path, canonical_bytes(full) + b"\n")
    repeat_dir = tmp_path / "s8-DuckDB-ZSTD_ROUTE-warm-20260829T010000Z-repeat-full"
    repeat = depth_runner.build_plan(source_manifest, root, matrix, repeat_dir, *cell_args, "repeat-full", full_path)
    repeat_path = tmp_path / "repeat-plan.json"
    _write(repeat_path, canonical_bytes(repeat) + b"\n")
    manifest = _template(tmp_path / "template-pair", {"corpus": "DuckDB", "profile": "ZSTD_ROUTE", "regime": "cold"})
    with pytest.raises(MultiTUPredictiveError, match="paired_producer"):
        _produce(repeat_path, manifest)
    first_result, second_result = _produce_pair(full_path, repeat_path, manifest)
    second_output = Path(repeat["result"]["directory"])
    second_rows = [json.loads(line) for line in (second_output / "predictive_sim.jsonl").read_text().splitlines()]
    assert second_rows[0]["relationship_state"]["transition"] == "relationship_continue"
    first_output = Path(full["result"]["directory"])
    first_rows = [json.loads(line) for line in (first_output / "predictive_sim.jsonl").read_text().splitlines()]
    assert first_rows[0]["scheduling"]["source_start_ns"] == 0
    assert second_rows[0]["scheduling"]["source_start_ns"] == 0
    assert second_result["codec"]["paired_stream_scope"] == "full-1+full-2"
    assert first_result["codec"]["mode"] == "paired_continuation"
