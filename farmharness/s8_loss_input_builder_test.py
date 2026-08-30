from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from s8_loss_input_builder import LossInputError, _canonical, build


def _file(path: Path, data: bytes) -> dict[str, object]:
    path.write_bytes(data)
    return {"path": str(path), "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}


def _derived(root: Path, *, corpus="fmt", profile="ZSTD_TU", regime="cold",
             topology="C1F1/100000", depth_class="100", pass_id="full-1",
             parent="accepted") -> Path:
    out = root / parent
    out.mkdir(parents=True)
    records = _file(out / "records.jsonl", b"records\n")
    records["path"] = "records.jsonl"
    predictive = _file(out / "predictive.json", b"predictive\n")
    live = _file(out / "live.json", b"live\n")
    source = _file(out / "source.json", b"source\n")
    manifest = {
        "schema": "icecream-s8-derived-experiment-v1", "status": "PASS",
        "cell": {"corpus": corpus, "profile": profile, "regime": regime},
        "split": "calibration", "topology": topology, "suite": topology,
        "depth": "full" if depth_class == "repeat-full" else depth_class,
        "depth_class": depth_class, "pass_id": pass_id,
        "runs": ["full-1", "full-2"] if pass_id == "full-2" else ["full-1"],
        "requested_curve_points": 1, "comparison_scored": True,
        "records": records, "predictive_curve_manifest": predictive,
        "live_curve_manifest": live, "source_experiment_manifest": source,
        "predictive_plan_sha256": "a" * 64,
    }
    path = out / "experiment_manifest.json"
    path.write_bytes(_canonical(manifest))
    return path


def test_builds_full_grid_and_explicit_missing(tmp_path: Path) -> None:
    _derived(tmp_path)
    output = build(tmp_path, tmp_path / "out.json")
    value = json.loads(output.read_text())
    assert value["schema"] == "icecream-s8-loss-input-v1"
    assert len(value["entries"]) == 128
    assert sum(row["status"] == "PASS" for row in value["entries"]) == 1
    assert sum(row["status"] == "MISSING" for row in value["entries"]) == 127
    assert next(row for row in value["entries"] if row["status"] == "PASS")["records"]["path"].endswith("accepted/records.jsonl")


def test_full_two_uses_repeat_pass_and_rejects_duplicate_context(tmp_path: Path) -> None:
    _derived(tmp_path, depth_class="full", pass_id="full-1", parent="full1")
    _derived(tmp_path, depth_class="repeat-full", pass_id="full-2", parent="full2")
    output = build(tmp_path, tmp_path / "out.json")
    entries = json.loads(output.read_text())["entries"]
    assert {row["pass_id"] for row in entries if row["status"] == "PASS"} == {"full-1", "full-2"}
    duplicate = tmp_path / "duplicate"
    _derived(duplicate, parent="one")
    _derived(duplicate, parent="two")
    with pytest.raises(LossInputError, match="duplicate_context"):
        build(duplicate, tmp_path / "duplicate-out.json")


def test_rejects_quarantined_candidate_and_existing_output(tmp_path: Path) -> None:
    _derived(tmp_path / "quarantined", parent="candidate")
    with pytest.raises(LossInputError, match="excluded_or_quarantined"):
        build(tmp_path, tmp_path / "out.json")
    _derived(tmp_path, parent="accepted")
    output = tmp_path / "out.json"
    output.write_text("sentinel")
    with pytest.raises(LossInputError, match="already_exists"):
        build(tmp_path, output)
