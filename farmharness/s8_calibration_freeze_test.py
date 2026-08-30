from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from s8_calibration_freeze import (
    BUNDLE_SCHEMA,
    CALIBRATION_CELLS,
    CalibrationError,
    REQUEST_SCHEMA,
    SEMANTICS,
    canonical_bytes,
    freeze,
)
from s8_first_triple_driver import run
from s8_first_triple_driver_test import _prepared
from s8_predictive_engine import (
    MANIFEST_SCHEMA as ENGINE_MANIFEST_SCHEMA,
    SEMANTICS as ENGINE_SEMANTICS,
    TOPOLOGY_SCHEMA,
    canonical_bytes as engine_canonical_bytes,
    load_calibration_bundle, load_inputs,
    predict, _predict_curve, PredictionError,
)
from s8_predictive_live_normalizer import RECORD_SCHEMA


PREDICTOR = {
    "source_commit": "a" * 40,
    "source_tree": "b" * 40,
    "model_id": "s8-causal-performance-v2",
}
UNITS = {
    "point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns",
    "throughput_bytes_per_s": "bytes_per_s",
}


def _records(root: Path, cell: dict[str, str], channel_scale: float = 1.25,
             elapsed_scale: float = 0.8) -> tuple[Path, bytes]:
    root.mkdir(parents=True, exist_ok=True)
    identity = {
        **cell,
        "split": "calibration",
        "run_id": "run-001",
        "source_commit": "c" * 40,
        "source_tree": "d" * 40,
        "input_digest": "e" * 64,
        "topology_digest": "f" * 64,
        "model_id": PREDICTOR["model_id"],
    }
    predicted = []
    observed = []
    for step, (channel, elapsed) in enumerate(((100, 100), (200, 250))):
        predicted.append({
            "step": step, "tu_id": f"tu-{step}",
            "cumulative": {
                "channel_bytes": channel, "elapsed_ns": elapsed,
                "throughput_bytes_per_s": channel * 1_000_000_000 / elapsed,
            },
        })
        live_channel = channel * channel_scale
        live_elapsed = elapsed * elapsed_scale
        observed.append({
            "step": step, "tu_id": f"tu-{step}",
            "cumulative": {
                "channel_bytes": live_channel, "elapsed_ns": live_elapsed,
                "throughput_bytes_per_s": live_channel * 1_000_000_000 / live_elapsed,
            },
        })
    records = [
        {"schema": RECORD_SCHEMA, "semantics": SEMANTICS,
         "record_type": "predictive_sim", "cell": cell, "split": "calibration",
         "identity": identity, "units": UNITS, "model_id": identity["model_id"],
         "raw_cumulative_curve": predicted},
        {"schema": RECORD_SCHEMA, "semantics": SEMANTICS,
         "record_type": "live", "cell": cell, "split": "calibration",
         "identity": {**identity, "model_id": "s7-live-observed"}, "units": UNITS,
         "model_id": "s7-live-observed",
         "raw_cumulative_curve": observed},
        {"schema": RECORD_SCHEMA, "semantics": SEMANTICS,
         "record_type": "comparison", "cell": cell, "split": "calibration",
         "identity": identity, "units": UNITS, "model_id": identity["model_id"],
         "point_errors": [{"step": row["step"], "tu_id": row["tu_id"], "errors": {}}
                          for row in predicted],
         "loss_curve": [{"step": row["step"], "tu_id": row["tu_id"],
                         "squared_error": 1.0, "cumulative_loss": float(index + 1)}
                        for index, row in enumerate(predicted)],
        },
    ]
    path = root / "records.jsonl"
    raw = b"".join(canonical_bytes(record) + b"\n" for record in records)
    path.write_bytes(raw)
    return path, raw


def _bucket_scales(cell: dict[str, str]) -> tuple[float, float]:
    profile_offset = {"ZSTD_TU": 0.0, "ZSTD_ROUTE": 0.05,
                      "P29": 0.10, "GRZ_RESIDUAL": 0.15}[cell["profile"]]
    regime_offset = 0.03 if cell["regime"] == "warm" else 0.0
    return 1.25 + profile_offset + regime_offset, 0.8 - profile_offset / 2 - regime_offset


def _request(root: Path, *, mutate: dict[str, object] | None = None) -> tuple[Path, list[dict[str, object]]]:
    comparisons: list[dict[str, object]] = []
    for cell in CALIBRATION_CELLS:
        channel_scale, elapsed_scale = _bucket_scales(cell)
        path, raw = _records(
            root / "records" / f"{cell['corpus']}-{cell['profile']}-{cell['regime']}",
            cell, channel_scale, elapsed_scale)
        comparisons.append({
            "cell": dict(cell),
            "records": {"path": str(path.relative_to(root)),
                        "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)},
        })
    if mutate:
        comparisons[0].update(mutate)
    request = {
        "schema": REQUEST_SCHEMA, "semantics": SEMANTICS,
        "predictor": PREDICTOR, "comparisons": comparisons,
    }
    path = root / "calibration-request.json"
    path.write_bytes(canonical_bytes(request) + b"\n")
    return path, comparisons


def test_freeze_binds_exactly_16_inputs_and_source_identity(tmp_path: Path) -> None:
    request, comparisons = _request(tmp_path)
    manifest = freeze(request, tmp_path / "bundle.json", tmp_path / "manifest.json")
    bundle = json.loads((tmp_path / "bundle.json").read_bytes())
    assert bundle["schema"] == BUNDLE_SCHEMA
    assert len(comparisons) == 16
    assert len(bundle["inputs"]) == 16
    assert bundle["predictor"] == PREDICTOR
    assert manifest["predictor"] == PREDICTOR
    assert [item["cell"] for item in bundle["inputs"]] == sorted(
        (item["cell"] for item in bundle["inputs"]),
        key=lambda cell: f"{cell['corpus']}/{cell['profile']}/{cell['regime']}",
    )


def test_held_out_cell_is_rejected(tmp_path: Path) -> None:
    request, _ = _request(tmp_path)
    value = json.loads(request.read_bytes())
    value["comparisons"][0]["cell"] = {"corpus": "DuckDB", "profile": "ZSTD_TU", "regime": "cold"}
    request.write_bytes(canonical_bytes(value) + b"\n")
    with pytest.raises(CalibrationError, match="cell_not_calibration"):
        freeze(request, tmp_path / "bundle.json")


def test_missing_and_duplicate_cells_fail_closed(tmp_path: Path) -> None:
    request, _ = _request(tmp_path)
    value = json.loads(request.read_bytes())
    value["comparisons"] = value["comparisons"][:-1]
    request.write_bytes(canonical_bytes(value) + b"\n")
    with pytest.raises(CalibrationError, match="missing_cells"):
        freeze(request, tmp_path / "missing.json")

    duplicate_root = tmp_path / "duplicate"
    duplicate_root.mkdir()
    duplicate_request, _ = _request(duplicate_root)
    duplicate_value = json.loads(duplicate_request.read_bytes())
    duplicate_value["comparisons"][-1] = duplicate_value["comparisons"][0]
    duplicate_request.write_bytes(canonical_bytes(duplicate_value) + b"\n")
    with pytest.raises(CalibrationError, match="duplicate_cell"):
        freeze(duplicate_request, duplicate_root / "duplicate.json")


def test_records_tampering_is_rejected_by_bound_sha256(tmp_path: Path) -> None:
    request, comparisons = _request(tmp_path)
    path = tmp_path / comparisons[0]["records"]["path"]
    path.write_bytes(path.read_bytes() + b"tampered")
    with pytest.raises(CalibrationError, match="sha256_mismatch"):
        freeze(request, tmp_path / "bundle.json")


def test_bundle_is_deterministic_and_calibration_improves_synthetic_error(tmp_path: Path) -> None:
    request, _ = _request(tmp_path)
    first_bundle = tmp_path / "first" / "bundle.json"
    first_manifest = tmp_path / "first" / "manifest.json"
    second_bundle = tmp_path / "second" / "bundle.json"
    second_manifest = tmp_path / "second" / "manifest.json"
    first = freeze(request, first_bundle, first_manifest)
    second = freeze(request, second_bundle, second_manifest)
    assert first == second
    assert first_bundle.read_bytes() == second_bundle.read_bytes()
    bundle = json.loads(first_bundle.read_bytes())
    scales = bundle["calibration"]["scales"]
    assert len(scales) == 8
    assert scales["ZSTD_TU/cold"]["channel_bytes"] == 1.25
    assert scales["ZSTD_TU/cold"]["elapsed_ns"] == 0.8
    assert scales["ZSTD_ROUTE/warm"] != scales["ZSTD_TU/cold"]
    assert "throughput_bytes_per_s" not in scales["ZSTD_TU/cold"]
    raw_error = abs(1.25 - 1.0) + abs(0.8 - 1.0)
    calibrated_error = abs(1.25 / scales["ZSTD_TU/cold"]["channel_bytes"] - 1.0) + abs(0.8 / scales["ZSTD_TU/cold"]["elapsed_ns"] - 1.0)
    assert calibrated_error < raw_error


def test_metric_shape_and_split_identity_are_rejected(tmp_path: Path) -> None:
    request, comparisons = _request(tmp_path)
    path = tmp_path / comparisons[0]["records"]["path"]
    records = [json.loads(line) for line in path.read_bytes().splitlines()]
    del records[1]["raw_cumulative_curve"][0]["cumulative"]["throughput_bytes_per_s"]
    raw = b"".join(canonical_bytes(record) + b"\n" for record in records)
    path.write_bytes(raw)
    comparisons[0]["records"]["sha256"] = hashlib.sha256(raw).hexdigest()
    comparisons[0]["records"]["bytes"] = len(raw)
    value = json.loads(request.read_bytes())
    value["comparisons"] = comparisons
    request.write_bytes(canonical_bytes(value) + b"\n")
    with pytest.raises(CalibrationError, match="metric_shape_invalid"):
        freeze(request, tmp_path / "bundle.json")


def test_predictive_record_model_id_must_match_request(tmp_path: Path) -> None:
    request, comparisons = _request(tmp_path)
    path = tmp_path / comparisons[0]["records"]["path"]
    records = [json.loads(line) for line in path.read_bytes().splitlines()]
    records[0]["identity"]["model_id"] = "different-predictor"
    records[0]["model_id"] = "different-predictor"
    raw = b"".join(canonical_bytes(record) + b"\n" for record in records)
    path.write_bytes(raw)
    comparisons[0]["records"]["sha256"] = hashlib.sha256(raw).hexdigest()
    comparisons[0]["records"]["bytes"] = len(raw)
    value = json.loads(request.read_bytes())
    value["comparisons"] = comparisons
    request.write_bytes(canonical_bytes(value) + b"\n")
    with pytest.raises(CalibrationError, match="predictor_model_id_mismatch"):
        freeze(request, tmp_path / "bundle.json")


def test_engine_rejects_bundle_tamper_wrong_base_and_bucket(tmp_path: Path) -> None:
    request, _ = _request(tmp_path)
    bundle_path = tmp_path / "bundle.json"
    manifest_path = tmp_path / "manifest.json"
    freeze(request, bundle_path, manifest_path)

    bundle_path.write_bytes(bundle_path.read_bytes() + b"tampered")
    with pytest.raises(ValueError, match="calibration_bundle:sha256_mismatch"):
        load_calibration_bundle(manifest_path)

    root = tmp_path / "wrong-base"
    root.mkdir()
    request2, _ = _request(root)
    bundle2 = root / "bundle.json"
    manifest2 = root / "manifest.json"
    freeze(request2, bundle2, manifest2)
    bundle_value = json.loads(bundle2.read_bytes())
    bundle_value["predictor"]["model_id"] = "wrong-base-model"
    bundle_raw = canonical_bytes(bundle_value) + b"\n"
    bundle2.write_bytes(bundle_raw)
    manifest_value = json.loads(manifest2.read_bytes())
    manifest_value["predictor"]["model_id"] = "wrong-base-model"
    manifest_value["bundle"] = {"path": bundle2.name,
                                 "sha256": hashlib.sha256(bundle_raw).hexdigest(),
                                 "bytes": len(bundle_raw)}
    manifest2.write_bytes(canonical_bytes(manifest_value) + b"\n")
    with pytest.raises(ValueError, match="calibration_manifest:base_model_mismatch"):
        load_calibration_bundle(manifest2)

    root = tmp_path / "wrong-bucket"
    root.mkdir()
    request3, _ = _request(root)
    bundle3 = root / "bundle.json"
    manifest3 = root / "manifest.json"
    freeze(request3, bundle3, manifest3)
    bundle_value = json.loads(bundle3.read_bytes())
    del bundle_value["calibration"]["scales"]["ZSTD_TU/cold"]
    bundle_raw = canonical_bytes(bundle_value) + b"\n"
    bundle3.write_bytes(bundle_raw)
    manifest_value = json.loads(manifest3.read_bytes())
    manifest_value["bundle"] = {"path": bundle3.name,
                                 "sha256": hashlib.sha256(bundle_raw).hexdigest(),
                                 "bytes": len(bundle_raw)}
    manifest3.write_bytes(canonical_bytes(manifest_value) + b"\n")
    with pytest.raises(ValueError, match="calibration_bundle:bucket_set_invalid"):
        load_calibration_bundle(manifest3)

    root = tmp_path / "manifest-bindings"
    root.mkdir()
    request4, _ = _request(root)
    bundle4 = root / "bundle.json"
    manifest4 = root / "manifest.json"
    freeze(request4, bundle4, manifest4)
    original_manifest = json.loads(manifest4.read_bytes())
    manifest_value = json.loads(manifest4.read_bytes())
    manifest_value["request"] = {"sha256": "0" * 64, "bytes": 1}
    manifest4.write_bytes(canonical_bytes(manifest_value) + b"\n")
    with pytest.raises(ValueError, match="calibration_bundle:manifest_binding_mismatch"):
        load_calibration_bundle(manifest4)
    manifest_value = original_manifest
    manifest_value["inputs"] = list(reversed(manifest_value["inputs"]))
    manifest4.write_bytes(canonical_bytes(manifest_value) + b"\n")
    with pytest.raises(ValueError, match="calibration_bundle:manifest_binding_mismatch"):
        load_calibration_bundle(manifest4)


def _engine_inputs(root: Path) -> Path:
    cell = {"corpus": "fmt", "profile": "ZSTD_ROUTE", "regime": "warm"}
    payload = b"predictive calibration input\n"
    root.mkdir()
    input_path = root / "input.ii"
    input_path.write_bytes(payload)
    topology = {
        "schema": TOPOLOGY_SCHEMA, "semantics": ENGINE_SEMANTICS, "cell": cell,
        "topology": {"c_store_guid": "1".zfill(32), "f_store_guid": "2".zfill(32),
                     "history_nonce": 1, "c_workers": 1, "f_workers": 1,
                     "cache_channel": "direct"},
        "state": {"c_cache": "warm", "f_cache": "warm", "generation": 0},
    }
    topology_raw = engine_canonical_bytes(topology) + b"\n"
    topology_path = root / "topology.json"
    topology_path.write_bytes(topology_raw)
    descriptor = lambda path, raw: {"path": path.name, "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)}
    manifest = {
        "schema": ENGINE_MANIFEST_SCHEMA, "semantics": ENGINE_SEMANTICS,
        "cell": cell, "split": "calibration", "predictive_mode": True,
        "input": descriptor(input_path, payload),
        "topology_state": descriptor(topology_path, topology_raw),
    }
    path = root / "predictive-manifest.json"
    path.write_bytes(engine_canonical_bytes(manifest) + b"\n")
    return path


def test_calibrated_engine_and_driver_expose_stable_model_id(tmp_path: Path) -> None:
    request, _ = _request(tmp_path)
    bundle = tmp_path / "bundle.json"
    calibration_manifest = tmp_path / "manifest.json"
    freeze(request, bundle, calibration_manifest)
    predictive_manifest = _engine_inputs(tmp_path / "engine")
    uncalibrated_path = tmp_path / "uncalibrated.jsonl"
    calibrated_path = tmp_path / "calibrated.jsonl"
    predict(predictive_manifest, uncalibrated_path)
    calibrated_sidecar = predict(predictive_manifest, calibrated_path,
                                 calibration_bundle=calibration_manifest)
    uncalibrated_row = json.loads(uncalibrated_path.read_bytes().splitlines()[0])
    calibrated_row = json.loads(calibrated_path.read_bytes().splitlines()[0])
    assert calibrated_row["model_id"].startswith("s8-causal-performance-v2-cal-")
    assert calibrated_sidecar["provenance"]["calibration_bundle_sha256"]
    assert calibrated_row["channel_bytes"] != uncalibrated_row["channel_bytes"]
    scales = json.loads(bundle.read_bytes())["calibration"]["scales"]["ZSTD_ROUTE/warm"]
    base_window = (uncalibrated_row["elapsed_ns"]["compile"] +
                   uncalibrated_row["elapsed_ns"]["result_return"])
    calibrated_window = (calibrated_row["elapsed_ns"]["compile"] +
                         calibrated_row["elapsed_ns"]["result_return"])
    assert calibrated_window == round(base_window * scales["elapsed_ns"])
    for direction in ("C_TO_F", "F_TO_C"):
        assert calibrated_row["channel_bytes"][direction] == round(
            uncalibrated_row["channel_bytes"][direction] * scales["channel_bytes"])
    driver_root = tmp_path / "driver"
    driver_root.mkdir()
    driver_manifest, package, live_manifest, _ = _prepared(driver_root)
    experiment = run(driver_manifest, package, live_manifest, driver_root / "experiments",
                     calibration_bundle=calibration_manifest)
    records = [json.loads(line) for line in (experiment / "records.jsonl").read_bytes().splitlines()]
    assert records[0]["model_id"].startswith("s8-causal-performance-v2-cal-")
    assert records[1]["model_id"] != records[0]["model_id"]


def test_v2_bundle_scopes_factors_to_explicit_topology_and_depth(tmp_path: Path) -> None:
    request, _ = _request(tmp_path)
    value = json.loads(request.read_bytes())
    for comparison in value["comparisons"]:
        comparison["topology"] = "C1F1"
        comparison["depth_class"] = "full"
    request.write_bytes(canonical_bytes(value) + b"\n")
    bundle = tmp_path / "bundle.json"
    manifest = tmp_path / "manifest.json"
    freeze(request, bundle, manifest)
    loaded = load_calibration_bundle(manifest)
    assert loaded["contexts"] == ["C1F1/full"]
    assert set(loaded["scales"]) == {
        f"C1F1/full/{profile}/{regime}"
        for profile in ("ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL")
        for regime in ("cold", "warm")
    }
    engine_manifest = _engine_inputs(tmp_path / "engine")
    _value, _raw, _input, _facts, _sha, topology, cell = load_inputs(engine_manifest)
    with pytest.raises(PredictionError, match="unsupported_context:C1F20/full"):
        _predict_curve(b"payload", topology, cell, loaded, "full", "C1F20")


def test_v2_freeze_preserves_directional_factors(tmp_path: Path) -> None:
    request, comparisons = _request(tmp_path)
    for comparison in comparisons:
        comparison["topology"] = "C1F1"
        comparison["depth_class"] = "100"
        path = tmp_path / comparison["records"]["path"]
        records = [json.loads(line) for line in path.read_bytes().splitlines()]
        for record in records[:2]:
            for row in record["raw_cumulative_curve"]:
                total = row["cumulative"]["channel_bytes"]
                if record["record_type"] == "predictive_sim":
                    row["cumulative"].update({"C_TO_F_bytes": total * 0.6,
                                               "F_TO_C_bytes": total * 0.4})
                else:
                    row["cumulative"].update({"C_TO_F_bytes": total * 0.8,
                                               "F_TO_C_bytes": total * 0.2})
        raw = b"".join(canonical_bytes(record) + b"\n" for record in records)
        path.write_bytes(raw)
        comparison["records"].update({"sha256": hashlib.sha256(raw).hexdigest(),
                                       "bytes": len(raw)})
    request.write_bytes(canonical_bytes({
        "schema": REQUEST_SCHEMA, "semantics": SEMANTICS,
        "predictor": PREDICTOR, "comparisons": comparisons,
    }) + b"\n")
    bundle = tmp_path / "bundle.json"
    freeze(request, bundle)
    scales = json.loads(bundle.read_bytes())["calibration"]["scales"]
    assert scales["C1F1/100/ZSTD_TU/cold"]["C_TO_F_bytes"] == pytest.approx(5 / 3)
    assert scales["C1F1/100/ZSTD_TU/cold"]["F_TO_C_bytes"] == pytest.approx(5 / 8)


def test_v2_freeze_allows_complete_matrices_in_multiple_contexts(tmp_path: Path) -> None:
    request, comparisons = _request(tmp_path)
    # Duplicate the authenticated 16-cell matrix for a second measured
    # topology/depth context.  A context is part of identity, so this is not
    # a duplicate cell and should produce one authenticated bundle.
    second = []
    for comparison in comparisons:
        item = dict(comparison)
        item["topology"] = "C1F20"
        item["depth_class"] = "200"
        second.append(item)
    value = json.loads(request.read_bytes())
    value["comparisons"] = [*comparisons, *second]
    request.write_bytes(canonical_bytes(value) + b"\n")
    bundle = tmp_path / "bundle.json"
    freeze(request, bundle)
    scales = json.loads(bundle.read_bytes())["calibration"]["scales"]
    assert set(scales) == {
        f"{context}/{profile}/{regime}"
        for context in ("C1F1/legacy", "C1F20/200")
        for profile in ("ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL")
        for regime in ("cold", "warm")
    }
    loaded = load_calibration_bundle(tmp_path / "calibration-model-manifest.json")
    assert set(loaded["contexts"]) == {"C1F1/legacy", "C1F20/200"}


def test_v2_freeze_rejects_missing_cell_within_one_context(tmp_path: Path) -> None:
    request, comparisons = _request(tmp_path)
    second = []
    for comparison in comparisons[:-1]:
        item = dict(comparison)
        item["topology"] = "C1F20"
        item["depth_class"] = "200"
        second.append(item)
    value = json.loads(request.read_bytes())
    value["comparisons"] = [*comparisons, *second]
    request.write_bytes(canonical_bytes(value) + b"\n")
    with pytest.raises(CalibrationError, match="missing_cells:C1F20/200"):
        freeze(request, tmp_path / "bundle.json")
