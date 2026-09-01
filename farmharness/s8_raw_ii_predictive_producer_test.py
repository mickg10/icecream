from __future__ import annotations

import hashlib
import io
import json
import os
import socket
import subprocess
from pathlib import Path

import pytest

import s8_depth_runner as depth
import s8_raw_ii_predictive_producer as producer
import s8_real_c1f1_live_runner as live_runner
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


def test_raw_paired_repeat_cli_binds_each_plan_output_and_rejects_reuse(
        tmp_path: Path) -> None:
    first_100, _source, _digest, _size = _inputs(tmp_path)
    first_value = json.loads(first_100.read_text())
    full_output = tmp_path / "s8-DuckDB-RAW_II-cold-20260901T030000Z"
    full = depth.build_plan(
        Path(first_value["source_manifest"]["path"]),
        Path(first_value["source_root"]),
        Path(first_value["matrix_precondition"]["path"]),
        full_output, "DuckDB", "RAW_II", "cold", "full", topology="C1F1")
    full_plan = tmp_path / "depth-plan-full.json"
    full_plan.write_text(json.dumps(full, sort_keys=True) + "\n")

    repeat_output = tmp_path / "s8-DuckDB-RAW_II-cold-20260901T030001Z"
    repeat = depth.build_plan(
        Path(json.loads(first_100.read_text())["source_manifest"]["path"]),
        Path(json.loads(first_100.read_text())["source_root"]),
        Path(json.loads(first_100.read_text())["matrix_precondition"]["path"]),
        repeat_output, "DuckDB", "RAW_II", "cold", "repeat-full",
        repeat_of=full_plan, topology="C1F1")
    repeat_plan = tmp_path / "depth-plan-repeat.json"
    repeat_plan.write_text(json.dumps(repeat, sort_keys=True) + "\n")
    digest = full["inputs"][0]["sha256"]
    size = full["inputs"][0]["bytes"]
    witness, engine = _control_inputs(tmp_path, full_plan, digest, size)
    product = _product_root(tmp_path)
    args = ["--plan", str(full_plan), "--repeat-plan", str(repeat_plan),
            "--raw-ii-witness", str(witness), "--engine-manifest", str(engine),
            "--depth", "full", "--product-root", str(product)]
    assert producer.main(args) == 0
    assert (full_output / "predictive_curve_manifest.json").is_file()
    assert (repeat_output / "predictive_curve_manifest.json").is_file()

    # A second invocation must fail at the producer's new-output boundary;
    # paired mode must never reinterpret --repeat-plan as an output override.
    assert producer.main(args) == 2


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


def test_raw_product_identity_rejects_deterministic_toctou_mutation(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    product = _product_root(tmp_path)
    tracked = product / "tracked.txt"
    original = producer._untracked_inventory

    def mutate_after_snapshot(root: Path) -> dict[str, object]:
        tracked.write_text("changed after pre-snapshot\n")
        return original(root)

    monkeypatch.setattr(producer, "_untracked_inventory", mutate_after_snapshot)
    with pytest.raises(RawIIError, match="product_root:changed_during_inventory"):
        producer._product_identity(product)


def test_raw_product_identity_rejects_generated_symlink(
        tmp_path: Path) -> None:
    product = _product_root(tmp_path)
    target = tmp_path / "outside-generated"
    target.write_bytes(b"generated\n")
    (product / "generated-link").symlink_to(target)
    with pytest.raises(RawIIError, match="product_root:untracked_artifact_invalid"):
        producer._product_identity(product)


@pytest.mark.parametrize("node_kind", ["fifo", "socket"])
def test_raw_product_identity_rejects_generated_nonregular(
        tmp_path: Path, node_kind: str) -> None:
    product = _product_root(tmp_path)
    path = product / f"generated-{node_kind}"
    listener: socket.socket | None = None
    if node_kind == "fifo":
        os.mkfifo(path)
    else:
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(path))
    try:
        with pytest.raises(RawIIError, match="product_root:untracked_artifact_invalid"):
            producer._product_identity(product)
    finally:
        if listener is not None:
            listener.close()


def test_raw_product_identity_rejects_foreign_generated_owner(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    product = _product_root(tmp_path)
    generated = product / "generated-owner"
    generated.write_bytes(b"generated\n")
    current_uid = os.geteuid()
    monkeypatch.setattr(producer.os, "geteuid", lambda: current_uid + 1)
    with pytest.raises(RawIIError, match="product_root:untracked_artifact_invalid"):
        producer._product_identity(product)


@pytest.mark.parametrize("flag", ["assume-unchanged", "skip-worktree"])
def test_raw_product_identity_rejects_tracked_index_bypass(
        tmp_path: Path, flag: str) -> None:
    product = _product_root(tmp_path)
    subprocess.run(["git", "-C", str(product), "update-index", f"--{flag}", "tracked.txt"],
                   check=True)
    with pytest.raises(RawIIError, match="product_root:tracked_index_flags_set"):
        producer._product_identity(product)


def test_raw_product_identity_rejects_tracked_symlink(
        tmp_path: Path) -> None:
    product = _product_root(tmp_path)
    target = tmp_path / "target"
    target.write_text("target\n")
    tracked_link = product / "tracked-link"
    tracked_link.symlink_to(target)
    subprocess.run(["git", "-C", str(product), "add", "tracked-link"], check=True)
    subprocess.run(["git", "-C", str(product), "commit", "-qm", "link"], check=True)
    with pytest.raises(RawIIError, match="product_root:tracked_artifact_invalid"):
        producer._product_identity(product)


def test_raw_product_identity_rechecks_generated_digest_after_first_inventory(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    product = _product_root(tmp_path)
    generated = product / "generated-race"
    generated.write_bytes(b"first\n")
    original = producer._untracked_inventory
    calls = 0

    def mutate_after_first(root: Path) -> dict[str, object]:
        nonlocal calls
        calls += 1
        result = original(root)
        if calls == 1:
            generated.write_bytes(b"second\n")
        return result

    monkeypatch.setattr(producer, "_untracked_inventory", mutate_after_first)
    with pytest.raises(RawIIError, match="product_root:changed_during_inventory"):
        producer._product_identity(product)
    assert calls == 2


def test_raw_product_walk_rejects_directory_replacement(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    product = _product_root(tmp_path)
    generated_dir = product / "generated-dir"
    generated_dir.mkdir()
    (generated_dir / "file").write_text("generated\n")
    outside = tmp_path / "outside"
    outside.mkdir()
    original_open = producer.os.open
    replaced = False

    def replace_before_child_open(path: object, flags: int, *args: object,
                                  **kwargs: object) -> int:
        nonlocal replaced
        if not replaced and kwargs.get("dir_fd") is not None and path == "generated-dir":
            generated_dir.rename(tmp_path / "moved-generated-dir")
            generated_dir.symlink_to(outside, target_is_directory=True)
            replaced = True
        return original_open(path, flags, *args, **kwargs)

    monkeypatch.setattr(producer.os, "open", replace_before_child_open)
    with pytest.raises(RawIIError, match="product_root:walk_unavailable"):
        producer._product_identity(product)
    assert replaced


def test_raw_product_identity_reconciles_git_omitted_generated_file(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    product = _product_root(tmp_path)
    generated = product / "generated-omitted"
    generated.write_bytes(b"generated\n")
    original = producer._git_untracked_paths

    def omit_generated(root: Path, *, ignored: bool) -> list[str]:
        return [] if root == product else original(root, ignored=ignored)

    monkeypatch.setattr(producer, "_git_untracked_paths", omit_generated)
    with pytest.raises(RawIIError, match="product_root:untracked_inventory_changed"):
        producer._product_identity(product)


def test_raw_product_identity_rejects_walk_depth_cap(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    product = _product_root(tmp_path)
    nested = product
    for index in range(4):
        nested /= f"generated-dir-{index}"
        nested.mkdir()
    (nested / "generated.txt").write_text("generated\n")
    monkeypatch.setattr(producer, "MAX_PRODUCT_WALK_DEPTH", 3)
    with pytest.raises(RawIIError, match="product_root:walk_depth_exceeded"):
        producer._product_identity(product)


def test_raw_untracked_inventory_aborts_and_reaps_at_entry_cap(
        monkeypatch: pytest.MonkeyPatch) -> None:
    class FakeProcess:
        def __init__(self) -> None:
            self.stdout = io.BytesIO(
                b"x\0" * (producer.MAX_UNTRACKED_ENTRIES + 1))
            self.terminated = False
            self.waited = False

        def poll(self) -> int | None:
            return 0 if self.terminated else None

        def terminate(self) -> None:
            self.terminated = True

        def kill(self) -> None:
            self.terminated = True

        def wait(self, timeout: float | None = None) -> int:
            self.waited = True
            return 0

    fake = FakeProcess()
    monkeypatch.setattr(producer.subprocess, "Popen", lambda *args, **kwargs: fake)
    with pytest.raises(RawIIError, match="product_root:untracked_inventory_too_many"):
        producer._git_untracked_paths(Path("/tmp/product"), ignored=False)
    assert fake.terminated and fake.waited


def test_raw_git_status_aborts_and_reaps_at_byte_cap(
        monkeypatch: pytest.MonkeyPatch) -> None:
    class FakeProcess:
        def __init__(self) -> None:
            self.stdout = io.BytesIO(b"12345")
            self.terminated = False
            self.waited = False

        def poll(self) -> int | None:
            return 0 if self.terminated else None

        def terminate(self) -> None:
            self.terminated = True

        def kill(self) -> None:
            self.terminated = True

        def wait(self, timeout: float | None = None) -> int:
            self.waited = True
            return 0

    fake = FakeProcess()
    monkeypatch.setattr(producer, "MAX_GIT_STATUS_BYTES", 4)
    monkeypatch.setattr(producer.subprocess, "Popen", lambda *args, **kwargs: fake)
    with pytest.raises(RawIIError, match="product_root:git_status_too_large"):
        producer._git_status_snapshot(Path("/tmp/product"))
    assert fake.terminated and fake.waited


def test_raw_read_uses_bounded_fd_stream(monkeypatch: pytest.MonkeyPatch,
                                        tmp_path: Path) -> None:
    path = tmp_path / "manifest.json"
    path.write_text('{"ok": true}\n')
    monkeypatch.setattr(Path, "read_bytes",
                        lambda _path: (_ for _ in ()).throw(AssertionError(
                            "unbounded read_bytes used")))
    value, facts = producer._read(path, "manifest")
    assert value == {"ok": True}
    assert facts["bytes"] == path.stat().st_size


def test_raw_local_producer_real_finalizer_then_normalizer(tmp_path: Path,
                                                           monkeypatch: pytest.MonkeyPatch) -> None:
    plan_path, _source, _digest, _size = _inputs(tmp_path)
    plan_value = depth.build_plan(
        tmp_path / "sources.txt", tmp_path, tmp_path / "matrix.json",
        tmp_path / "s8-DuckDB-RAW_II-cold-20260901T000000Z-full",
        "DuckDB", "RAW_II", "cold", "full")
    plan_path.write_text(json.dumps(plan_value, sort_keys=True) + "\n")
    digest = plan_value["inputs"][0]["sha256"]
    size = plan_value["inputs"][0]["bytes"]
    witness, engine = _control_inputs(tmp_path, plan_path, digest, size)
    product = _product_root(tmp_path)
    plan = json.loads(plan_path.read_text())
    predictive_dir = Path(plan["result"]["directory"])
    produce(plan_path, witness, engine, predictive_dir, "full", product)
    live_tu_id = json.loads(
        (predictive_dir / "predictive_sim.jsonl").read_text().splitlines()[0]
    )["tu_id"]

    work = tmp_path / "p50compilee2e.raw"
    (work / "out").mkdir(parents=True)
    (work / "input.ii").write_bytes(b"input\n")
    (work / "out" / "remote.o").write_bytes(b"remote\n")
    (work / "out" / "local.o").write_bytes(b"local\n")
    (work / "client-compile-full-1-0.log").write_text("write_fd_to_server\n")
    (work / "s7-measured-c-legacy-wire-trace.jsonl").write_text("c-wire\n")
    (work / "s7-measured-f-legacy-wire-trace.jsonl").write_text("f-wire\n")
    batch = tmp_path / "batch.jsonl"
    batch.write_bytes(b"batch\n")
    topology = tmp_path / "topology.json"
    topology.write_text(json.dumps({"assignments": [{"relationship": 0, "f_slot": 0}]}) + "\n")
    plan_sha = hashlib.sha256(plan_path.read_bytes()).hexdigest()
    plan_inputs = [dict(plan["inputs"][0])]
    rows = [{"tu_id": live_tu_id, "predictive_input": plan_inputs[0]}]
    observation = {
        "run": "full-1", "ordinal": 0, "tu_id": live_tu_id,
        "preprocessed_path": str(work / "input.ii"),
        "remote_path": str(work / "out" / "remote.o"),
        "local_path": str(work / "out" / "local.o"),
        "remote_sha256": hashlib.sha256((work / "out" / "remote.o").read_bytes()).hexdigest(),
        "local_sha256": hashlib.sha256((work / "out" / "local.o").read_bytes()).hexdigest(),
        "measured_elapsed_ns": 100, "channel_bytes": 50,
        "wait_for_cs_ns": 10, "client_elapsed_ns": 100,
        "returned_object_bytes": 17, "remote_bytes": 7, "local_bytes": 6,
        "admission_start_ns": 100, "input_ready_ns": 110,
        "compile_start_ns": 120, "compile_end_ns": 200, "witness_end_ns": 210,
        "planned_assignment_ordinal": 0, "planned_relationship": 0,
        "planned_admission_lane": 0, "observed_scheduler_job_id": 1,
        "observed_f_service_identity": "p50-f", "observed_source_tu_seq": 0,
    }
    binary_identity = {role: hashlib.sha256(role.encode()).hexdigest()
                       for role in ("scheduler/icecc-scheduler", "daemon/iceccd",
                                    "client/icecc", "cache/icecc-cache-service")}
    monkeypatch.setattr(live_runner, "load_predictive_plan",
                        lambda *_args, **_kwargs: (plan, plan_inputs, plan_sha))
    monkeypatch.setattr(live_runner, "load_batch_manifest", lambda *_args: rows)
    monkeypatch.setattr(live_runner, "bind_batch_to_plan", lambda *_args: None)
    topology_sha = hashlib.sha256(topology.read_bytes()).hexdigest()
    monkeypatch.setattr(live_runner, "load_topology", lambda *_args: topology_sha)
    monkeypatch.setattr(live_runner, "_retained_workdir", lambda *_args, **_kwargs: work)
    monkeypatch.setattr(live_runner, "_environment_preparation", lambda *_args: {})
    monkeypatch.setattr(live_runner, "_binary_identity", lambda *_args, **_kwargs: binary_identity)
    monkeypatch.setattr(live_runner, "product_identity",
                        lambda *_args, **_kwargs: ("a" * 40, "b" * 40,
                                                    binary_identity, "c" * 64))
    monkeypatch.setattr(live_runner, "_timing_rows", lambda *_args, **_kwargs: [dict(observation)])
    monkeypatch.setattr(live_runner, "_batch_windows",
                        lambda *_args, **_kwargs: {"full-1": {"start_ns": 100, "end_ns": 200}})
    monkeypatch.setattr(live_runner, "_validate_product_log_evidence", lambda *_args: None)
    monkeypatch.setattr(live_runner, "_legacy_wire_stage", lambda *_args: [{
        "c_to_f_bytes": 33, "f_to_c_bytes": 17, "channel_bytes": 50,
        "planned_relationship": 0, "planned_admission_lane": 0,
        "relationship": 0, "f_slot": 0,
    }])
    stdout = (f"PASS: all-P50 C1F1\nS7_WORKDIR={work}\nS8_BATCH_COUNT=1\n"
              f"S8_SUITE={live_runner.TOPOLOGY}\nS8_BATCH_PASSES=1\n"
              "S8_BATCH_WARM=0\nS8_SCHEDULING mode=relationship-ordered execution_slots=1 "
              "relationships=1 planned_admission_lanes_per_relationship=1\n")
    output = live_runner.finalize(
        stdout, 0, batch_manifest=batch, topology=topology,
        predictive_plan=plan_path, output=tmp_path / "live-output", profile="P29",
        product_profile="RAW_II", product_root=product, corpus="DuckDB", regime="cold",
        depth="full", full_count=1, passes=1, timestamp="20260901T000000Z")
    live_manifest = output / "live_curve_manifest.json"
    records = normalizer.normalize(predictive_dir / "predictive_curve_manifest.json",
                                   live_manifest, tmp_path / "records.jsonl")
    assert [record["record_type"] for record in records] == [
        "predictive_sim", "live", "comparison"]
    assert all(record["identity"]["profile"] == "RAW_II" for record in records)

    mutant = json.loads(live_manifest.read_text())
    mutant["identity"] = dict(mutant["identity"], input_digest="e" * 64)
    live_manifest.write_text(json.dumps(mutant, sort_keys=True) + "\n")
    with pytest.raises(normalizer.NormalizationError,
                       match="identity_mismatch:input_digest"):
        normalizer.normalize(predictive_dir / "predictive_curve_manifest.json",
                             live_manifest, tmp_path / "mutant-records.jsonl")


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
