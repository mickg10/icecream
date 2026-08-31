"""Focused tests for the authenticated S8 predictive/live normalizer."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from s8_predictive_live_normalizer import (
    MANIFEST_SCHEMA,
    NormalizationError,
    canonical_bytes,
    comparison_descriptor,
    normalize,
)
from s8_schema import CURRENT_SEMANTICS, DECLARED_CELLS, SPLITS


IDENTITY = {
    "corpus": "fmt",
    "profile": "ZSTD_TU",
    "regime": "cold",
    "split": "calibration",
    "run_id": "run-001",
    "source_commit": "a" * 40,
    "source_tree": "b" * 40,
    "input_digest": "c" * 64,
    "topology_digest": "d" * 64,
    "model_id": "fmt-zstd-tu-cold-v1",
}
UNITS = {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns"}
SCHEDULING = {"topology": "C1F1", "assignments": [{"ordinal": 0,
                                                     "global_slot": 0}]}
COMPARISON = comparison_descriptor("e" * 64, SCHEDULING)
CALIBRATION_METADATA = {
    "product_image_digest": "1" * 64,
    "toolchain_digest": "2" * 64,
    "output_contract_digest": "3" * 64,
    "host_digest": "4" * 64,
    "ordered_input_class": "ordered",
}
LOOPBACK_PLACEMENT = {
    "schema": "icecream-s8-role-placement-v1",
    "mode": "co_resident_loopback",
    "c_host_digest": "4" * 64,
    "scheduler_host_digest": "4" * 64,
    "f_host_digests": ["4" * 64],
    "roles_disjoint": False,
    "timing_eligible": False,
}
EXTERNAL_PLACEMENT = {
    "schema": "icecream-s8-role-placement-v1",
    "mode": "external_farm",
    "c_host_digest": "4" * 64,
    "scheduler_host_digest": "5" * 64,
    "f_host_digests": ["6" * 64],
    "roles_disjoint": True,
    "timing_eligible": True,
}


def _curve_rows(offset: int = 0) -> list[dict[str, object]]:
    return [
        {"step": 0, "tu_id": "tu-0", "cumulative": {"channel_bytes": 10 + offset, "elapsed_ns": 100 + offset}},
        {"step": 1, "tu_id": "tu-1", "cumulative": {"channel_bytes": 30 + offset, "elapsed_ns": 240 + offset}},
    ]


def _write_curve(path: Path, rows: list[dict[str, object]]) -> bytes:
    raw = b"".join(canonical_bytes(row) + b"\n" for row in rows)
    path.write_bytes(raw)
    return raw


def _write_manifest(root: Path, name: str, mode: str, rows: list[dict[str, object]], *, identity: dict[str, object] | None = None, extra: dict[str, object] | None = None) -> Path:
    curve = root / f"{name}.jsonl"
    raw = _write_curve(curve, rows)
    value: dict[str, object] = {
        "schema": MANIFEST_SCHEMA,
        "identity": identity or IDENTITY,
        "comparison": COMPARISON,
        "units": UNITS,
        "curve": {"path": curve.name, "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)},
        "provenance": {
            "mode": mode,
            "producer": "s8_predictive_engine" if mode == "predictive_sim" else "s7_live_observation",
            "trace_free": mode == "predictive_sim",
        },
    }
    if extra:
        value.update(extra)
    manifest = root / f"{name}-manifest.json"
    manifest.write_bytes(canonical_bytes(value) + b"\n")
    return manifest


def _pair(tmp_path: Path, *, predicted: list[dict[str, object]] | None = None, observed: list[dict[str, object]] | None = None, live_identity: dict[str, object] | None = None):
    p = _write_manifest(tmp_path, "predictive", "predictive_sim", predicted or _curve_rows())
    l = _write_manifest(tmp_path, "live", "live", observed or _curve_rows(2), identity=live_identity)
    return p, l


def test_emits_three_records_and_keeps_raw_curves(tmp_path: Path) -> None:
    predictive, live = _pair(tmp_path)
    output = tmp_path / "normalized.jsonl"
    records = normalize(predictive, live, output)
    assert [record["record_type"] for record in records] == ["predictive_sim", "live", "comparison"]
    assert records[0]["raw_cumulative_curve"] == _curve_rows()
    assert records[1]["raw_cumulative_curve"] == _curve_rows(2)
    assert "point_errors" not in records[0] and "loss_curve" not in records[1]
    assert records[0]["cell"] == {"corpus": "fmt", "profile": "ZSTD_TU", "regime": "cold"}
    assert records[0]["split"] == "calibration"
    comparison = records[2]
    assert comparison["point_errors"][0]["errors"]["cumulative.channel_bytes"] == {
        "signed": -2, "absolute": 2, "relative": -2 / 12, "squared": 4,
    }
    assert comparison["loss_curve"][-1]["cumulative_loss"] == 16.0
    assert len(output.read_text(encoding="utf-8").splitlines()) == 3
    assert all(record["semantics"] == CURRENT_SEMANTICS for record in records)


def test_identity_and_units_are_bound_exactly(tmp_path: Path) -> None:
    predictive, _live = _pair(tmp_path)
    mismatched = dict(IDENTITY)
    mismatched["profile"] = "P29"
    live = _write_manifest(tmp_path, "live2", "live", _curve_rows(), identity=mismatched)
    with pytest.raises(NormalizationError, match="identity_mismatch:profile"):
        normalize(predictive, live, tmp_path / "out.jsonl")


def test_predictor_and_live_producer_ids_remain_distinct(tmp_path: Path) -> None:
    live_identity = dict(IDENTITY)
    live_identity["model_id"] = "s7-live-observed"
    predictive, live = _pair(tmp_path, live_identity=live_identity)
    records = normalize(predictive, live, tmp_path / "out.jsonl")
    assert records[0]["model_id"] == IDENTITY["model_id"]
    assert records[1]["model_id"] == "s7-live-observed"
    assert records[2]["model_id"] == IDENTITY["model_id"]


def test_manifest_campaign_identity_is_preserved_on_all_records(tmp_path: Path) -> None:
    metadata = {"topology": "C1F1", "depth_class": "100", "pass_id": "a90"}
    predictive = _write_manifest(tmp_path, "predictive", "predictive_sim", _curve_rows(), extra=metadata)
    live = _write_manifest(tmp_path, "live", "live", _curve_rows(2), extra=metadata)
    records = normalize(predictive, live, tmp_path / "out.jsonl")
    assert all({key: record[key] for key in metadata} == metadata for record in records)


def test_manifest_campaign_identity_mismatch_is_rejected(tmp_path: Path) -> None:
    predictive = _write_manifest(tmp_path, "predictive", "predictive_sim", _curve_rows(),
                                 extra={"topology": "C1F1", "depth_class": "100", "pass_id": "a90"})
    live = _write_manifest(tmp_path, "live", "live", _curve_rows(2),
                           extra={"topology": "C1F1", "depth_class": "200", "pass_id": "a90"})
    with pytest.raises(NormalizationError, match="manifest_metadata_mismatch:depth_class"):
        normalize(predictive, live, tmp_path / "out.jsonl")


def test_live_calibration_authority_fills_trace_free_predictive_metadata(
        tmp_path: Path) -> None:
    predictive = _write_manifest(tmp_path, "predictive", "predictive_sim", _curve_rows())
    live = _write_manifest(tmp_path, "live", "live", _curve_rows(2),
                           extra={**CALIBRATION_METADATA,
                                  "execution_scope": "external_farm_timing",
                                  "role_placement": EXTERNAL_PLACEMENT})
    with pytest.raises(NormalizationError, match="manifest_metadata_mismatch:host_digest"):
        normalize(predictive, live, tmp_path / "without-authority.jsonl")
    records = normalize(
        predictive, live, tmp_path / "with-authority.jsonl",
        authenticated_metadata=CALIBRATION_METADATA)
    assert all({field: row[field] for field in CALIBRATION_METADATA} ==
               CALIBRATION_METADATA for row in records)


def test_loopback_live_is_rejected_from_calibration(tmp_path: Path) -> None:
    predictive = _write_manifest(tmp_path, "predictive", "predictive_sim", _curve_rows())
    live = _write_manifest(
        tmp_path, "live", "live", _curve_rows(2),
        extra={**CALIBRATION_METADATA, "execution_scope": "loopback_correctness_only",
               "role_placement": LOOPBACK_PLACEMENT})
    with pytest.raises(NormalizationError,
                       match="role_placement:live_calibration_requires_external_farm"):
        normalize(predictive, live, tmp_path / "out.jsonl",
                  authenticated_metadata=CALIBRATION_METADATA)


def test_external_farm_live_is_eligible_when_placement_is_disjoint(tmp_path: Path) -> None:
    predictive = _write_manifest(tmp_path, "predictive", "predictive_sim", _curve_rows())
    live = _write_manifest(
        tmp_path, "live", "live", _curve_rows(2),
        extra={**CALIBRATION_METADATA, "execution_scope": "external_farm_timing",
               "role_placement": EXTERNAL_PLACEMENT})
    records = normalize(predictive, live, tmp_path / "out.jsonl",
                        authenticated_metadata=CALIBRATION_METADATA)
    assert all(record["execution_scope"] == "external_farm_timing" for record in records[1:])
    assert records[2]["role_placement"]["roles_disjoint"] is True


def test_calibration_authority_rejects_live_metadata_mismatch(tmp_path: Path) -> None:
    predictive = _write_manifest(tmp_path, "predictive", "predictive_sim", _curve_rows())
    changed = {**CALIBRATION_METADATA, "host_digest": "5" * 64}
    live = _write_manifest(
        tmp_path, "live", "live", _curve_rows(2),
        extra={**changed, "execution_scope": "external_farm_timing",
               "role_placement": EXTERNAL_PLACEMENT})
    with pytest.raises(NormalizationError, match="authority_metadata:mismatch"):
        normalize(predictive, live, tmp_path / "out.jsonl",
                  authenticated_metadata=CALIBRATION_METADATA)


def test_exact_plan_join_allows_independent_run_and_source_identity(tmp_path: Path) -> None:
    live_identity = dict(IDENTITY)
    live_identity.update(run_id="full-1", source_commit="f" * 40,
                         source_tree="2" * 40, topology_digest="1" * 64,
                         model_id="s8-real-live")
    predictive, live = _pair(tmp_path, live_identity=live_identity)
    records = normalize(predictive, live, tmp_path / "out.jsonl")
    assert records[2]["comparison"]["comparison_id"] == COMPARISON["comparison_id"]


def test_run_id_source_mismatch_without_plan_key_is_rejected(tmp_path: Path) -> None:
    predictive, live = _pair(tmp_path)
    value = json.loads(live.read_text())
    value.pop("comparison")
    value["identity"] = dict(value["identity"], run_id="unrelated-live",
                              source_commit="f" * 40, source_tree="2" * 40)
    live.write_bytes(canonical_bytes(value) + b"\n")
    with pytest.raises(NormalizationError, match="identity_mismatch:(run_id|source_commit|source_tree)"):
        normalize(predictive, live, tmp_path / "out.jsonl")


def test_changed_input_or_plan_topology_cannot_join(tmp_path: Path) -> None:
    predictive, _ = _pair(tmp_path)
    changed_input = dict(IDENTITY, input_digest="1" * 64)
    live = _write_manifest(tmp_path, "live-input", "live", _curve_rows(),
                           identity=changed_input)
    with pytest.raises(NormalizationError, match="identity_mismatch:input_digest"):
        normalize(predictive, live, tmp_path / "input-out.jsonl")

    changed_plan = comparison_descriptor("2" * 64, SCHEDULING)
    live = _write_manifest(tmp_path, "live-topology", "live", _curve_rows(),
                           extra={"comparison": changed_plan})
    with pytest.raises(NormalizationError, match="comparison_mismatch"):
        normalize(predictive, live, tmp_path / "topology-out.jsonl")


def test_directional_units_must_be_declared_as_a_pair(tmp_path: Path) -> None:
    predictive, _ = _pair(tmp_path)
    live = _write_manifest(tmp_path, "live-partial-units", "live", _curve_rows(),
                           extra={"units": {**UNITS, "C_TO_F_bytes": "bytes"}})
    with pytest.raises(NormalizationError, match="directional_fields_must_be_paired"):
        normalize(predictive, live, tmp_path / "partial-units-out.jsonl")


def test_plan_capture_id_changes_with_authenticated_schedule_or_topology() -> None:
    plan = {"scheduling": {"topology": "C1F1", "assignments": [{"ordinal": 0,
                                                                    "global_slot": 0}]}}
    plan_sha = hashlib.sha256(canonical_bytes(plan)).hexdigest()
    changed = {"scheduling": {"topology": "C1F1", "assignments": [{"ordinal": 0,
                                                                       "global_slot": 1}]}}
    changed_sha = hashlib.sha256(canonical_bytes(changed)).hexdigest()
    assert comparison_descriptor(plan_sha, plan["scheduling"])["comparison_id"] != comparison_descriptor(
        changed_sha, changed["scheduling"])["comparison_id"]


def test_plan_capture_id_binds_depth_class_when_declared() -> None:
    scheduling = {"topology": "C1F1", "depth_class": "100",
                  "assignments": [{"ordinal": 0, "global_slot": 0}]}
    changed = {**scheduling, "depth_class": "200"}
    plan_sha = "a" * 64
    assert comparison_descriptor(plan_sha, scheduling)["comparison_id"] != comparison_descriptor(
        plan_sha, changed)["comparison_id"]


@pytest.mark.parametrize(
    "cell", DECLARED_CELLS,
    ids=lambda value: f"{value['corpus']}-{value['profile']}-{value['regime']}",
)
def test_all_declared_cells_join_three_typed_records(tmp_path: Path, cell: dict[str, str]) -> None:
    identity = dict(IDENTITY)
    identity.update(cell, split=SPLITS[cell["corpus"]])
    predictive = _write_manifest(tmp_path, "predictive", "predictive_sim", _curve_rows(), identity=identity)
    live = _write_manifest(tmp_path, "live", "live", _curve_rows(2), identity=identity)
    records = normalize(predictive, live, tmp_path / "out.jsonl")
    assert len(records) == 3
    assert [record["record_type"] for record in records] == ["predictive_sim", "live", "comparison"]
    assert all(record["identity"]["split"] == SPLITS[cell["corpus"]] for record in records)


@pytest.mark.parametrize(
    "rows, message",
    [
        ([_curve_rows()[1], _curve_rows()[0]], "reordered_or_missing_point"),
        ([_curve_rows()[0], {**_curve_rows()[0], "tu_id": "tu-duplicate"}], "duplicate_point"),
        ([_curve_rows()[0], {**_curve_rows()[1], "step": 3}], "reordered_or_missing_point"),
    ],
)
def test_reordered_missing_or_duplicate_points_fail_closed(tmp_path: Path, rows: list[dict[str, object]], message: str) -> None:
    predictive, live = _pair(tmp_path, predicted=rows)
    with pytest.raises(NormalizationError, match=message):
        normalize(predictive, live, tmp_path / "out.jsonl")
    assert not (tmp_path / "out.jsonl").exists()


def test_curve_tampering_fails_authentication(tmp_path: Path) -> None:
    predictive, live = _pair(tmp_path)
    curve = tmp_path / "predictive.jsonl"
    curve.write_bytes(curve.read_bytes() + b"\n")
    with pytest.raises(NormalizationError, match="sha256_mismatch|byte_count_mismatch"):
        normalize(predictive, live, tmp_path / "out.jsonl")


def test_route_action_trace_fields_are_not_consumed(tmp_path: Path) -> None:
    rows = [{**_curve_rows()[0], "action": "TX_BEGIN"}, _curve_rows()[1]]
    predictive, live = _pair(tmp_path, predicted=rows)
    with pytest.raises(NormalizationError, match="trace_or_action_field"):
        normalize(predictive, live, tmp_path / "out.jsonl")


def test_negative_metric_is_rejected(tmp_path: Path) -> None:
    negative = [{**_curve_rows()[0],
                 "cumulative": {"channel_bytes": -1, "elapsed_ns": 100}},
                _curve_rows()[1]]
    predictive, live = _pair(tmp_path, predicted=negative)
    with pytest.raises(NormalizationError, match="metric_negative"):
        normalize(predictive, live, tmp_path / "out.jsonl")


def test_manifest_trace_reference_is_rejected(tmp_path: Path) -> None:
    predictive, live = _pair(tmp_path)
    raw = json.loads(predictive.read_bytes())
    raw["action_trace"] = "must-not-be-read.jsonl"
    predictive.write_bytes(canonical_bytes(raw) + b"\n")
    with pytest.raises(NormalizationError, match="manifest:fields_invalid"):
        normalize(predictive, live, tmp_path / "out.jsonl")


def test_curve_embedded_identity_must_match_manifest(tmp_path: Path) -> None:
    rows = [{**_curve_rows()[0], "cell": {"corpus": "P29", "profile": "ZSTD_TU", "regime": "cold"}}, _curve_rows()[1]]
    predictive, live = _pair(tmp_path, predicted=rows)
    with pytest.raises(NormalizationError, match="cell_mismatch"):
        normalize(predictive, live, tmp_path / "out.jsonl")


def test_existing_output_is_not_overwritten(tmp_path: Path) -> None:
    predictive, live = _pair(tmp_path)
    output = tmp_path / "out.jsonl"
    output.write_bytes(b"sentinel\n")
    with pytest.raises(NormalizationError, match="output_already_exists"):
        normalize(predictive, live, output)
    assert output.read_bytes() == b"sentinel\n"
