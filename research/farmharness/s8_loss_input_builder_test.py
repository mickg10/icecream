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
             parent="accepted", timing_eligible=True) -> Path:
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
    if timing_eligible:
        manifest.update({
            "execution_scope": "external_farm_timing",
            "calibration_eligible": True,
            "measurement_method": profile,
            "product_profile": "GRZ" if profile == "GRZ_RESIDUAL" else profile,
            "role_placement": {
                "schema": "icecream-s8-role-placement-v1",
                "mode": "external_farm",
                "c_host_digest": "b" * 64,
                "scheduler_host_digest": "b" * 64,
                "f_host_digests": ["c" * 64],
                "roles_disjoint": True,
                "timing_eligible": True,
            },
        })
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


def test_loopback_diagnostic_cannot_shadow_external_timing_context(tmp_path: Path) -> None:
    _derived(tmp_path, parent="old-loopback", timing_eligible=False)
    accepted = _derived(tmp_path, parent="external")
    output = build(tmp_path, tmp_path / "out.json")
    entries = json.loads(output.read_text())["entries"]
    passed = [row for row in entries if row["status"] == "PASS"]
    assert len(passed) == 1
    assert passed[0]["records"]["path"].endswith("external/records.jsonl")
    assert accepted.is_file()


def test_claimed_external_timing_requires_disjoint_placement(tmp_path: Path) -> None:
    manifest_path = _derived(tmp_path)
    manifest = json.loads(manifest_path.read_text())
    manifest["role_placement"]["timing_eligible"] = False
    manifest_path.write_bytes(_canonical(manifest))
    with pytest.raises(LossInputError, match="role_placement_invalid"):
        build(tmp_path, tmp_path / "out.json")


def test_rejects_quarantined_candidate_and_existing_output(tmp_path: Path) -> None:
    _derived(tmp_path / "quarantined", parent="candidate")
    with pytest.raises(LossInputError, match="excluded_or_quarantined"):
        build(tmp_path, tmp_path / "out.json")
    _derived(tmp_path, parent="accepted")
    output = tmp_path / "out.json"
    output.write_text("sentinel")
    with pytest.raises(LossInputError, match="already_exists"):
        build(tmp_path, output)


def test_records_are_relative_to_manifest_parent_not_derived_root(tmp_path: Path) -> None:
    derived = tmp_path / "derived"
    _derived(derived)
    output = build(derived, tmp_path / "loss-input.json")
    entry = next(row for row in json.loads(output.read_text())["entries"]
                 if row["status"] == "PASS")
    assert entry["records"]["path"] == "derived/accepted/records.jsonl"
    assert (output.parent / entry["records"]["path"]).read_bytes() == b"records\n"

    outside = tmp_path / "sibling" / "loss-input.json"
    with pytest.raises(LossInputError, match="accepted_records_not_under_manifest_parent"):
        build(derived, outside)
