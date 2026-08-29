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
