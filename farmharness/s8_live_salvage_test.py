"""Focused immutable-input gates for the S8 salvage producer."""

from __future__ import annotations

import hashlib
import json
import shutil
from pathlib import Path

import pytest

from s8_live_salvage import (SalvageError, _load_timing, _product_log,
                              _validate_predictive_producer, _validate_trace, salvage)


RAW = Path(
    "/tanksmall/scratch/ictmp/experiments/icecream/"
    "s8-rocksdb-zstd-tu-cold-full-6c72c2da-20260830T125215Z/"
    "live-output-full2/C1F1/icecream/C1F1-100000/20260830T125215Z/ZSTD_TU"
)
PRODUCER_FULL = Path(
    "/tanksmall/scratch/ictmp/experiments/icecream/"
    "s8-rocksdb-zstd-tu-cold-full-6c72c2da-20260830T125215Z/results/"
    "s8-RocksDB-ZSTD_TU-cold-C1F1-20260830T125215Z-full"
)


def _copy_evidence(tmp_path: Path, *names: str) -> Path:
    root = tmp_path / "raw"
    evidence = root / "product-evidence"
    evidence.mkdir(parents=True)
    for name in names:
        source = RAW / name if name.startswith("timing_") else (RAW / name if "/" in name else RAW / "product-evidence" / name)
        target = root / Path(name).name if name.startswith("timing_") else evidence / Path(name).name
        target.write_bytes(source.read_bytes())
    return root


@pytest.mark.skipif(not RAW.is_dir(), reason="terminal raw package is not mounted")
def test_deletion_of_timing_is_fail_closed(tmp_path: Path) -> None:
    root = _copy_evidence(tmp_path, "timing_full-2.jsonl")
    (root / "timing_full-2.jsonl").unlink()
    with pytest.raises(SalvageError, match="unavailable"):
        _load_timing(root, "full-2")


@pytest.mark.skipif(not RAW.is_dir(), reason="terminal raw package is not mounted")
def test_mutated_product_log_is_rejected_and_source_hash_is_unchanged(tmp_path: Path) -> None:
    source = RAW / "product-evidence" / "product-output.log"
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    root = _copy_evidence(tmp_path, "product-output.log")
    changed = bytearray((root / "product-evidence" / "product-output.log").read_bytes())
    marker = changed.index(b"S8_BATCH_TU")
    changed[marker] = ord("X")
    (root / "product-evidence" / "product-output.log").write_bytes(changed)
    with pytest.raises(SalvageError, match="product_log:"):
        _product_log(root)
    assert hashlib.sha256(source.read_bytes()).hexdigest() == before


@pytest.mark.skipif(not RAW.is_dir(), reason="terminal raw package is not mounted")
def test_predictive_manifest_without_adjacent_producer_is_rejected(tmp_path: Path) -> None:
    source = PRODUCER_FULL / "predictive_curve_manifest.json"
    isolated = tmp_path / "predictive_curve_manifest.json"
    isolated.write_bytes(source.read_bytes())
    evidence = json.loads((RAW / "evidence.json").read_text())
    with pytest.raises(SalvageError, match="producer_manifest.full-1:unavailable"):
        _validate_predictive_producer(isolated, RAW / "product-evidence/predictive-plan.json", "full-1", evidence)


@pytest.mark.skipif(not RAW.is_dir(), reason="terminal raw package is not mounted")
def test_mutated_predictive_curve_is_rejected_by_adjacent_producer(tmp_path: Path) -> None:
    source_dir = PRODUCER_FULL
    isolated_dir = tmp_path / "producer"
    shutil.copytree(source_dir, isolated_dir)
    manifest = isolated_dir / "predictive_curve_manifest.json"
    producer = isolated_dir / "producer_manifest.json"
    producer_value = json.loads(producer.read_text())
    producer_value["outputs"]["predictive_sim"]["sha256"] = "1" * 64
    producer.write_text(json.dumps(producer_value, sort_keys=True, separators=(",", ":")))
    evidence = json.loads((RAW / "evidence.json").read_text())
    with pytest.raises(SalvageError, match="producer.full-1.curve:descriptor_mismatch"):
        _validate_predictive_producer(manifest, RAW / "product-evidence/predictive-plan.json", "full-1", evidence)


@pytest.mark.skipif(not RAW.is_dir(), reason="terminal raw package is not mounted")
@pytest.mark.parametrize(
    ("field", "replacement"),
    (
        ("run_id", "rebound-run"),
        ("model_id", "rebound-model"),
        ("split", "held_out_validation"),
        ("source_commit", "a" * 40),
        ("source_tree", "b" * 40),
        ("input_digest", "a" * 64),
        ("topology_digest", "b" * 64),
        ("corpus", "DuckDB"),
        ("profile", "ZSTD_ROUTE"),
        ("regime", "warm"),
    ),
)
def test_each_producer_identity_field_is_bound_to_predictive_authority(
    tmp_path: Path, field: str, replacement: str
) -> None:
    isolated_dir = tmp_path / "producer"
    shutil.copytree(PRODUCER_FULL, isolated_dir)
    producer = isolated_dir / "producer_manifest.json"
    value = json.loads(producer.read_text())
    value["identity"][field] = replacement
    producer.write_text(json.dumps(value, sort_keys=True, separators=(",", ":")))
    evidence = json.loads((RAW / "evidence.json").read_text())
    with pytest.raises(SalvageError):
        _validate_predictive_producer(
            isolated_dir / "predictive_curve_manifest.json",
            RAW / "product-evidence/predictive-plan.json",
            "full-1",
            evidence,
        )


@pytest.mark.skipif(not RAW.is_dir(), reason="terminal raw package is not mounted")
@pytest.mark.parametrize("field", ("input_manifest_sha256", "assignment_sha256"))
def test_each_producer_batch_authority_is_bound_to_plan_authority(
    tmp_path: Path, field: str
) -> None:
    isolated_dir = tmp_path / "producer"
    shutil.copytree(PRODUCER_FULL, isolated_dir)
    producer = isolated_dir / "producer_manifest.json"
    value = json.loads(producer.read_text())
    value["batch_binding"][field] = "a" * 64
    producer.write_text(json.dumps(value, sort_keys=True, separators=(",", ":")))
    evidence = json.loads((RAW / "evidence.json").read_text())
    with pytest.raises(SalvageError):
        _validate_predictive_producer(
            isolated_dir / "predictive_curve_manifest.json",
            RAW / "product-evidence/predictive-plan.json",
            "full-1",
            evidence,
        )


def _trace_fixture(tmp_path: Path) -> tuple[Path, dict[str, list[dict[str, object]]]]:
    root = tmp_path / "trace-raw"
    evidence_dir = root / "product-evidence"
    evidence_dir.mkdir(parents=True)
    for name in ("s7-measured-c-action-trace.jsonl", "s7-measured-f-action-trace.jsonl"):
        (evidence_dir / name).write_bytes((RAW / "product-evidence" / name).read_bytes())
    timings = {run: _load_timing(RAW, run)[0] for run in ("full-1", "full-2")}
    return root, timings


@pytest.mark.skipif(not RAW.is_dir(), reason="terminal raw package is not mounted")
def test_swapped_c_begin_commit_is_rejected(tmp_path: Path) -> None:
    root, timings = _trace_fixture(tmp_path)
    path = root / "product-evidence/s7-measured-c-action-trace.jsonl"
    lines = path.read_text().splitlines()
    lines[0], lines[1] = lines[1], lines[0]
    path.write_text("\n".join(lines) + "\n")
    with pytest.raises(SalvageError, match="c_action:ordered_sequence"):
        _validate_trace(root, timings)


@pytest.mark.skipif(not RAW.is_dir(), reason="terminal raw package is not mounted")
def test_swapped_complete_f_transaction_groups_are_rejected(tmp_path: Path) -> None:
    root, timings = _trace_fixture(tmp_path)
    path = root / "product-evidence/s7-measured-f-action-trace.jsonl"
    lines = path.read_text().splitlines()
    # Each transaction's six action rows are bracketed by SESSION_DISCONNECTED
    # and SESSION_OPENED.  Exchange the first two complete groups while
    # retaining all lifecycle rows and their copied witness bytes.
    lines[2:8], lines[10:16] = lines[10:16], lines[2:8]
    path.write_text("\n".join(lines) + "\n")
    with pytest.raises(SalvageError, match="f_action:ordered_sequence"):
        _validate_trace(root, timings)


@pytest.mark.skipif(not RAW.is_dir(), reason="terminal raw package is not mounted")
def test_c_trace_actor_mutation_is_rejected(tmp_path: Path) -> None:
    root, timings = _trace_fixture(tmp_path)
    path = root / "product-evidence/s7-measured-c-action-trace.jsonl"
    first = path.read_text().splitlines()
    first[0] = first[0].replace('"actor":"C"', '"actor":"F"', 1)
    path.write_text("\n".join(first) + "\n")
    with pytest.raises(SalvageError, match="c_action:actor:0"):
        _validate_trace(root, timings)


@pytest.mark.skipif(not RAW.is_dir(), reason="terminal raw package is not mounted")
def test_symlink_live_root_is_rejected(tmp_path: Path) -> None:
    link = tmp_path / "live-link"
    link.symlink_to(RAW, target_is_directory=True)
    with pytest.raises(SalvageError, match="output:must_be_new_sibling"):
        salvage(link, Path("missing-predictive-1"), Path("missing-predictive-2"), tmp_path / "sibling")
