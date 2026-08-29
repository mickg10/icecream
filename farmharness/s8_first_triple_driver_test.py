from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from s8_first_triple_driver import DriverError, run
from s8_predictive_engine import (MANIFEST_SCHEMA, SEMANTICS, TOPOLOGY_SCHEMA,
                                  canonical_bytes, predict)
from s8_predictive_live_normalizer import MANIFEST_SCHEMA as CURVE_MANIFEST_SCHEMA


CELL = {"corpus": "fmt", "profile": "ZSTD_TU", "regime": "cold"}
IDENTITY_BASE = {
    "corpus": "fmt", "profile": "ZSTD_TU", "regime": "cold",
    "split": "calibration", "run_id": "run-001", "source_commit": "a" * 40,
    "source_tree": "b" * 40, "model_id": "fmt-zstd-tu-cold-v1",
}
UNITS = {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns"}


def _descriptor(path: Path, raw: bytes) -> dict[str, object]:
    return {"path": path.name, "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)}


def _inputs(root: Path, payload: bytes = b"fmt source\n" * 20) -> tuple[Path, Path, bytes]:
    input_path = root / "input.ii"
    input_path.write_bytes(payload)
    topology = {
        "schema": TOPOLOGY_SCHEMA, "semantics": SEMANTICS,
        "cell": CELL,
        "topology": {"c_store_guid": "1".zfill(32), "f_store_guid": "2".zfill(32),
                     "history_nonce": 1, "c_workers": 1, "f_workers": 1,
                     "cache_channel": "direct"},
        "state": {"c_cache": "cold", "f_cache": "cold", "generation": 0},
    }
    topology_raw = canonical_bytes(topology) + b"\n"
    topology_path = root / "topology.json"
    topology_path.write_bytes(topology_raw)
    manifest = {
        "schema": MANIFEST_SCHEMA, "semantics": SEMANTICS, "cell": CELL,
        "split": "calibration", "predictive_mode": True,
        "input": _descriptor(input_path, payload),
        "topology_state": _descriptor(topology_path, topology_raw),
    }
    manifest_path = root / "predictive-manifest.json"
    manifest_path.write_bytes(canonical_bytes(manifest) + b"\n")
    return manifest_path, input_path, topology_raw


def _live_package(root: Path, payload: bytes) -> tuple[Path, bytes]:
    package = root / "live-package"
    package.mkdir()
    record = {
        "schema": "icecream-s7-live-cell-v1", "cell": "fmt/ZSTD_TU/cold",
        "corpus": "fmt", "profile": "ZSTD_TU", "regime": "cold",
        "status": "PASS", "live_status": "PASS", "acceptance_status": "PASS",
        "conformance_status": "PASS",
        "measured": {"input_sha256": hashlib.sha256(payload).hexdigest(),
                     "source_transfer_bytes": 1234, "remote_compile_ms": 56},
    }
    raw = (json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n").encode()
    (package / "results.jsonl").write_bytes(raw)
    (package / "route_trace.json").write_bytes(b"must not be opened")
    return package, raw


def _live_curve(root: Path, predictive_curve: Path, topology_raw: bytes,
                payload: bytes, *, producer: str = "s7_live_observation",
                unscored: bool = False) -> Path:
    rows = [json.loads(line) for line in predictive_curve.read_bytes().splitlines()]
    live_rows = []
    for row in rows:
        row = {"step": row["step"], "tu_id": row["tu_id"],
               "cumulative": dict(row["cumulative"])}
        cumulative = dict(row["cumulative"])
        cumulative["channel_bytes"] += 5
        cumulative["elapsed_ns"] += 7
        if unscored:
            row["status"] = "UNSCORED"
        row["cumulative"] = cumulative
        live_rows.append(row)
    curve = root / "live-curve.jsonl"
    curve_raw = b"".join(canonical_bytes(row) + b"\n" for row in live_rows)
    curve.write_bytes(curve_raw)
    identity = dict(IDENTITY_BASE)
    identity["input_digest"] = hashlib.sha256(payload).hexdigest()
    identity["topology_digest"] = hashlib.sha256(topology_raw).hexdigest()
    manifest = {
        "schema": CURVE_MANIFEST_SCHEMA, "identity": identity, "units": UNITS,
        "curve": _descriptor(curve, curve_raw),
        "provenance": {"mode": "live", "producer": producer, "trace_free": False},
    }
    manifest_path = root / "live-curve-manifest.json"
    manifest_path.write_bytes(canonical_bytes(manifest) + b"\n")
    return manifest_path


def _prepared(tmp_path: Path, *, producer: str = "s7_live_observation",
              unscored: bool = False):
    manifest, input_path, topology_raw = _inputs(tmp_path)
    package, live_raw = _live_package(tmp_path, input_path.read_bytes())
    predictor_curve = tmp_path / "seed-predictive.jsonl"
    predict(manifest, predictor_curve)
    live_manifest = _live_curve(tmp_path, predictor_curve, topology_raw,
                                input_path.read_bytes(), producer=producer,
                                unscored=unscored)
    return manifest, package, live_manifest, live_raw


def test_driver_uses_shared_normalizer_for_scored_records(tmp_path: Path) -> None:
    manifest, package, live_manifest, live_raw = _prepared(tmp_path)
    experiment = run(manifest, package, live_manifest, tmp_path / "experiments")
    records = [json.loads(line) for line in (experiment / "records.jsonl").read_bytes().splitlines()]
    assert [record["record_type"] for record in records] == ["predictive_sim", "live", "comparison"]
    assert records[2]["loss_curve"]
    assert records[2]["point_errors"]
    assert (experiment / "live_summary.jsonl").read_bytes() == live_raw
    assert json.loads((experiment / "experiment_manifest.json").read_bytes())["comparison_scored"] is True


def test_missing_live_curve_fails_with_exact_metric_error(tmp_path: Path) -> None:
    manifest, package, _live_manifest, _raw = _prepared(tmp_path)
    with pytest.raises(DriverError, match="^missing_live_metric_curve$"):
        run(manifest, package, None, tmp_path / "experiments")
    assert not (tmp_path / "experiments").exists()


def test_fabricated_live_curve_producer_cannot_score(tmp_path: Path) -> None:
    manifest, package, _live_manifest, _raw = _prepared(tmp_path, producer="fabricated")
    with pytest.raises(DriverError, match="live_metric_curve_producer_not_authenticated"):
        run(manifest, package, _live_manifest, tmp_path / "experiments")
    assert not (tmp_path / "experiments").exists()


def test_unscored_live_curve_cannot_emit_comparison(tmp_path: Path) -> None:
    manifest, package, _live_manifest, _raw = _prepared(tmp_path, unscored=True)
    with pytest.raises(DriverError, match="live_metric_curve_unscored"):
        run(manifest, package, _live_manifest, tmp_path / "experiments")
    assert not (tmp_path / "experiments").exists()
