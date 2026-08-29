from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

import s8_live_batch_prep as prep
import s8_real_c1f1_live_runner as live_runner


def _fixture(tmp_path: Path, topology: str = "C1F1") -> tuple[Path, Path, Path, Path]:
    corpus = tmp_path / "corpus"
    source_root = tmp_path / "source"
    output_root = source_root / "build"
    corpus.mkdir()
    output_root.mkdir(parents=True)
    source = source_root / "same.cpp"
    source.write_text("int value() { return 1; }\n")

    inputs = []
    entries = []
    manifest_lines = []
    for ordinal, target in enumerate(("target-a", "target-b")):
        relative = Path(target) / "same.cpp.ii"
        predictive = corpus / relative
        predictive.parent.mkdir(parents=True)
        raw = f"predictive-{ordinal}\n".encode()
        predictive.write_bytes(raw)
        manifest_lines.append(str(predictive) + "\n")
        inputs.append({"ordinal": ordinal, "path": str(predictive),
                       "source_relative": relative.as_posix(),
                       "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)})
        object_path = output_root / relative.with_suffix(".o")
        entries.append({"directory": str(output_root), "file": str(source),
                        "output": str(object_path),
                        "command": (f"/usr/bin/c++ -DORDINAL={ordinal} -o {object_path} "
                                    f"-c {source}")})

    source_manifest = tmp_path / "source-manifest.txt"
    source_manifest.write_text("".join(manifest_lines))
    source_raw = source_manifest.read_bytes()
    plan_value = {
        "schema": "icecream-s8-depth-run-plan-v1",
        "semantics": "s8-current-semantics-v1",
        "cell": {"corpus": "DuckDB", "profile": "ZSTD_TU", "regime": "cold"},
        "split": "held_out_validation",
        "request": {"depth": 100, "requested_curve_points": 100},
        "source_manifest": {"path": str(source_manifest),
                            "sha256": hashlib.sha256(source_raw).hexdigest(),
                            "bytes": len(source_raw), "entries": 2},
        "source_root": str(corpus), "inputs": inputs,
    }
    # The production plan validator requires depth=100. Replicate the two
    # distinct target rows to make a compact 100-row fixture.
    plan_inputs = []
    manifest_lines = []
    entries = []
    for ordinal in range(100):
        target = f"target-{ordinal:03d}"
        relative = Path(target) / "same.cpp.ii"
        predictive = corpus / relative
        predictive.parent.mkdir(parents=True)
        raw = f"predictive-{ordinal}\n".encode()
        predictive.write_bytes(raw)
        manifest_lines.append(str(predictive) + "\n")
        plan_inputs.append({"ordinal": ordinal, "path": str(predictive),
                            "source_relative": relative.as_posix(),
                            "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)})
        object_path = output_root / relative.with_suffix(".o")
        entries.append({"directory": str(output_root), "file": str(source),
                        "output": str(object_path),
                        "command": (f"/usr/bin/c++ -DORDINAL={ordinal} -o {object_path} "
                                    f"-c {source}")})
    source_manifest.write_text("".join(manifest_lines))
    source_raw = source_manifest.read_bytes()
    plan_value["source_manifest"] = {"path": str(source_manifest),
                                     "sha256": hashlib.sha256(source_raw).hexdigest(),
                                     "bytes": len(source_raw), "entries": 100}
    plan_value["inputs"] = plan_inputs
    slots = 1 if topology == "C1F1" else 40
    plan_value["scheduling"] = {
        "schema": "icecream-s8-scheduling-topology-v1",
        "topology": topology,
        "assignments": [
            {"ordinal": ordinal, "global_slot": ordinal % slots,
             "f_relationship": 0 if topology == "C1F1" else (ordinal % slots) // 2,
             "per_f_slot": 0 if topology == "C1F1" else ordinal % 2}
            for ordinal in range(100)
        ],
    }
    plan = tmp_path / "predictive-plan.json"
    plan.write_text(json.dumps(plan_value, sort_keys=True))
    compile_db = tmp_path / "compile_commands.json"
    compile_db.write_text(json.dumps(entries, sort_keys=True))
    return plan, compile_db, output_root, source_root


def test_prepare_binds_duplicate_sources_by_exact_compile_output(tmp_path: Path) -> None:
    plan, compile_db, output_root, source_root = _fixture(tmp_path)
    target = prep.prepare(predictive_plan=plan, compile_db=compile_db,
                          compile_output_root=output_root,
                          compile_source_root=source_root,
                          output=tmp_path / "prepared")
    rows = live_runner.load_batch_manifest(target / "batch-manifest.jsonl")
    assert len(rows) == 100
    assert len({row["compile_output"] for row in rows}) == 100
    assert len({row["compile_source"] for row in rows}) == 1
    assert rows[0]["tu_id"].startswith("DuckDB-cold-tu-000000-")
    assert json.loads((target / "prep-manifest.json").read_text())["status"] == "READY"


def test_compile_output_cannot_be_rebound_to_another_predictive_tu(tmp_path: Path) -> None:
    plan, compile_db, output_root, source_root = _fixture(tmp_path)
    target = prep.prepare(predictive_plan=plan, compile_db=compile_db,
                          compile_output_root=output_root,
                          compile_source_root=source_root,
                          output=tmp_path / "prepared")
    batch = target / "batch-manifest.jsonl"
    rows = [json.loads(line) for line in batch.read_text().splitlines()]
    rows[1]["compile_output"] = rows[0]["compile_output"]
    batch.write_text("".join(json.dumps(row, sort_keys=True) + "\n" for row in rows))
    with pytest.raises(live_runner.LiveRunnerError,
                       match="compile_output_predictive_mismatch"):
        live_runner.load_batch_manifest(batch)


def test_prepare_c1f20_uses_exact_predictive_assignments(tmp_path: Path) -> None:
    plan, compile_db, output_root, source_root = _fixture(tmp_path, "C1F20")
    target = prep.prepare(predictive_plan=plan, compile_db=compile_db,
                          compile_output_root=output_root,
                          compile_source_root=source_root,
                          output=tmp_path / "prepared")
    plan_value = json.loads(plan.read_text())
    topology = json.loads((target / "topology.json").read_text())
    assert topology["suite"] == live_runner.PARALLEL_TOPOLOGY
    assert [(item["relationship"], item["f_slot"])
            for item in topology["assignments"]] == [
        (item["f_relationship"], item["per_f_slot"])
        for item in plan_value["scheduling"]["assignments"]
    ]
    manifest = json.loads((target / "prep-manifest.json").read_text())
    assert manifest["topology"] == live_runner.PARALLEL_TOPOLOGY


def test_missing_compile_output_mapping_fails_closed(tmp_path: Path) -> None:
    plan, compile_db, output_root, source_root = _fixture(tmp_path)
    entries = json.loads(compile_db.read_text())
    compile_db.write_text(json.dumps(entries[:-1], sort_keys=True))
    with pytest.raises(prep.BatchPrepError, match="input_unmatched:99"):
        prep.prepare(predictive_plan=plan, compile_db=compile_db,
                     compile_output_root=output_root,
                     compile_source_root=source_root,
                     output=tmp_path / "prepared")
