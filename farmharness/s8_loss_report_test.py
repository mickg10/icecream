from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from s8_loss_report import LossReportError, build_report
from s8_schema import SPLITS


def _canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def _experiment(root: Path, corpus: str = "fmt", manifest_extra: dict[str, object] | None = None) -> Path:
    cell = {"corpus": corpus, "profile": "ZSTD_TU", "regime": "cold"}
    identity = {**cell, "split": SPLITS[corpus], "run_id": "run-1",
                "source_commit": "a" * 40, "source_tree": "b" * 40,
                "input_digest": "c" * 64, "topology_digest": "d" * 64,
                "model_id": "predictor-v1"}
    units = {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns",
             "C_TO_F_bytes": "bytes", "F_TO_C_bytes": "bytes",
             "throughput_bytes_per_s": "bytes_per_s"}
    predicted, observed, errors, losses = [], [], [], []
    cumulative_loss = 0.0
    for step in range(2):
        p = {"C_TO_F_bytes": 10 + step, "F_TO_C_bytes": 5 + step,
             "channel_bytes": 15 + 2 * step, "elapsed_ns": 100 + 10 * step,
             "throughput_bytes_per_s": (15 + 2 * step) * 1_000_000_000 / (100 + 10 * step)}
        o = {"C_TO_F_bytes": 12 + step, "F_TO_C_bytes": 6 + step,
             "channel_bytes": 18 + 2 * step, "elapsed_ns": 120 + 10 * step,
             "throughput_bytes_per_s": (18 + 2 * step) * 1_000_000_000 / (120 + 10 * step)}
        predicted.append({"step": step, "tu_id": f"tu-{step}", "cumulative": p})
        observed.append({"step": step, "tu_id": f"tu-{step}", "cumulative": o})
        point = {}
        squared = 0.0
        for key in p:
            signed = p[key] - o[key]
            squared += signed * signed
            point[f"cumulative.{key}"] = {"signed": signed, "absolute": abs(signed),
                                            "relative": signed / abs(o[key]),
                                            "squared": signed * signed}
        cumulative_loss += squared
        errors.append({"step": step, "tu_id": f"tu-{step}", "errors": point})
        losses.append({"step": step, "tu_id": f"tu-{step}",
                       "squared_error": squared, "cumulative_loss": cumulative_loss})
    common = {"schema": "icecream-s8-predictive-live-record-v1",
              "semantics": "s8-current-semantics-v1", "cell": cell,
              "split": SPLITS[corpus], "identity": identity, "units": units}
    pred = {**common, "record_type": "predictive_sim", "model_id": "predictor-v1",
            "provenance": {"mode": "predictive_sim", "producer": "fixture", "trace_free": True,
                           "manifest_sha256": "1" * 64, "curve_sha256": "2" * 64},
            "raw_cumulative_curve": predicted}
    live_identity = {**identity, "model_id": "live-v1"}
    live = {**common, "record_type": "live", "model_id": "live-v1", "identity": live_identity,
            "provenance": {"mode": "live", "producer": "fixture", "trace_free": False,
                           "manifest_sha256": "3" * 64, "curve_sha256": "4" * 64},
            "raw_cumulative_curve": observed}
    comparison = {**common, "record_type": "comparison", "model_id": "predictor-v1",
                  "provenance": {"predictive_manifest_sha256": "1" * 64,
                                  "live_manifest_sha256": "3" * 64,
                                  "predictive_curve_sha256": "2" * 64,
                                  "live_curve_sha256": "4" * 64},
                  "point_errors": errors, "loss_curve": losses}
    records = b"".join(_canonical(row) for row in (pred, live, comparison))
    experiment = root / "accepted"
    experiment.mkdir()
    records_path = experiment / "records.jsonl"
    records_path.write_bytes(records)
    manifest = {
        "schema": "icecream-s8-first-triple-driver-v2", "status": "PASS", "cell": cell,
        "split": SPLITS[corpus], "records": {"path": "records.jsonl",
        "sha256": hashlib.sha256(records).hexdigest(), "bytes": len(records)}}
    if manifest_extra:
        manifest.update(manifest_extra)
    (experiment / "experiment_manifest.json").write_bytes(_canonical(manifest))
    return records_path


def _input_manifest(root: Path, records: Path, corpus: str = "fmt", status: str = "PASS") -> Path:
    cell = {"corpus": corpus, "profile": "ZSTD_TU", "regime": "cold"}
    entries = [{"cell": cell, "split": SPLITS[corpus], "topology": "C1F1",
                "depth_class": "200", "pass_id": "full-1", "status": status,
                "records": ({"path": str(records.relative_to(root)),
                             "sha256": hashlib.sha256(records.read_bytes()).hexdigest(),
                             "bytes": records.stat().st_size} if status == "PASS" else None),
                **({"reason": "not measured"} if status != "PASS" else {})}]
    path = root / "input.json"
    path.write_bytes(_canonical({"schema": "icecream-s8-loss-input-v1",
                                 "semantics": "s8-current-semantics-v1", "entries": entries}))
    return path


def test_report_flattens_authenticated_directional_points_and_excludes_statuses(tmp_path: Path) -> None:
    records = _experiment(tmp_path)
    input_path = _input_manifest(tmp_path, records)
    output = build_report(input_path, tmp_path / "experiments")
    point_rows = [json.loads(line) for line in (output / "point-loss.jsonl").read_bytes().splitlines()]
    assert len(point_rows) == 2
    assert point_rows[0]["topology"] == "C1F1"
    assert point_rows[0]["depth_class"] == "200"
    assert point_rows[0]["pass_id"] == "full-1"
    assert set(point_rows[0]["metrics"]) == {"C_TO_F", "F_TO_C", "total", "elapsed", "throughput"}
    assert point_rows[0]["metrics"]["C_TO_F"]["error"]["signed"] == -2.0
    curve = json.loads((output / "loss-curve.json").read_text())
    assert curve["accepted_points"] == 2
    assert curve["loss_curve"][0]["point_count"] == 1
    assert (output / "sha256-manifest.json").exists()


def test_held_out_requires_frozen_calibration(tmp_path: Path) -> None:
    records = _experiment(tmp_path, corpus="DuckDB")
    input_path = _input_manifest(tmp_path, records, corpus="DuckDB")
    with pytest.raises(LossReportError, match="frozen_calibration_required"):
        build_report(input_path, tmp_path / "experiments")


def test_non_pass_entry_is_retained_as_status_but_not_measurement(tmp_path: Path) -> None:
    records = _experiment(tmp_path)
    input_path = _input_manifest(tmp_path, records)
    value = json.loads(input_path.read_text())
    value["entries"].append({
        "cell": {"corpus": "fmt", "profile": "ZSTD_TU", "regime": "cold"},
        "split": "calibration", "topology": "C1F20", "depth_class": "full",
        "pass_id": "full-2", "status": "DRY_RUN", "records": None,
        "reason": "wrapper not executed",
    })
    input_path.write_bytes(_canonical(value))
    output = build_report(input_path, tmp_path / "experiments")
    report = json.loads((output / "report-manifest.json").read_text())
    curve = json.loads((output / "loss-curve.json").read_text())
    assert report["status"] == "INCOMPLETE"
    assert report["status_counts"]["DRY_RUN"] == 1
    assert curve["accepted_entries"] == 1
    assert curve["excluded_entries"]["DRY_RUN"] == 1


@pytest.mark.parametrize(("field", "value"), (("topology", "C1F20/40"),
                                                  ("suite", "C1F20/40"),
                                                  ("depth", "100"),
                                                  ("runs", ["full-2"])))
def test_real_manifest_cannot_be_relabelled(tmp_path: Path, field: str, value: object) -> None:
    records = _experiment(tmp_path, manifest_extra={
        "schema": "icecream-s8-real-c1f1-live-runner-v2", "topology": "C1F1/100000",
        "suite": "C1F1/100000", "depth": "200", "runs": ["full-1"],
    })
    extra = {"schema": "icecream-s8-real-c1f1-live-runner-v2", "topology": "C1F1/100000",
             "suite": "C1F1/100000", "depth": "200", "runs": ["full-1"], field: value}
    manifest_path = records.parent / "experiment_manifest.json"
    manifest = json.loads(manifest_path.read_text())
    manifest.update(extra)
    manifest_path.write_bytes(_canonical(manifest))
    with pytest.raises(LossReportError, match="experiment_(topology|suite|depth|pass)_mismatch"):
        build_report(_input_manifest(tmp_path, records), tmp_path / "experiments")


def test_real_manifest_shape_is_accepted(tmp_path: Path) -> None:
    records = _experiment(tmp_path, manifest_extra={
        "schema": "icecream-s8-real-c1f1-live-runner-v2", "topology": "C1F1/100000",
        "suite": "C1F1/100000", "depth": "200", "runs": ["full-1"],
    })
    output = build_report(_input_manifest(tmp_path, records), tmp_path / "experiments")
    assert json.loads((output / "report-manifest.json").read_text())["status"] == "PASS"


def test_held_out_rejects_unrelated_frozen_bundle(monkeypatch: pytest.MonkeyPatch,
                                                 tmp_path: Path) -> None:
    records = _experiment(tmp_path, corpus="DuckDB", manifest_extra={
        "calibration_bundle": {"path": "frozen.json", "sha256": "b" * 64},
    })
    frozen = tmp_path / "frozen.json"
    frozen.write_bytes(b"fixture\n")
    monkeypatch.setattr("s8_loss_report.load_calibration_bundle",
                        lambda _path: {"bundle_sha256": "a" * 64})
    with pytest.raises(LossReportError, match="experiment_calibration_bundle_mismatch"):
        build_report(_input_manifest(tmp_path, records, corpus="DuckDB"),
                     tmp_path / "experiments", frozen)
