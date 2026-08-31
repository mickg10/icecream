from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from s8_calibration_freeze import (
    CALIBRATION_CELLS, CalibrationError, REQUEST_SCHEMA, SEMANTICS,
    canonical_bytes, freeze,
)
from s8_predictive_engine import _predict_curve, load_calibration_bundle
from s8_predictive_live_normalizer import RECORD_SCHEMA


META = {
    "product_image_digest": "a" * 64,
    "toolchain_digest": "b" * 64,
    "output_contract_digest": "c" * 64,
    "host_digest": "d" * 64,
    "ordered_input_class": "ordered",
}
PREDICTOR = {"source_commit": "1" * 40, "source_tree": "2" * 40,
             "model_id": "s8-causal-performance-v2"}
UNITS = {"point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns",
         "throughput_bytes_per_s": "bytes_per_s"}
EXTERNAL_PLACEMENT = {
    "schema": "icecream-s8-role-placement-v1",
    "mode": "external_farm",
    "c_host_digest": "d" * 64,
    "scheduler_host_digest": "d" * 64,
    "f_host_digests": ["e" * 64],
    "roles_disjoint": True,
    "timing_eligible": True,
}


def _triple(root: Path, cell: dict[str, str], context: str) -> tuple[str, dict[str, object]]:
    root.mkdir(parents=True, exist_ok=True)
    identity = {**cell, "split": "calibration", "run_id": "run-1",
                "source_commit": "3" * 40, "source_tree": "4" * 40,
                "input_digest": "5" * 64, "topology_digest": "6" * 64,
                "model_id": PREDICTOR["model_id"]}
    predicted, observed = [], []
    for step, (total, elapsed) in enumerate(((100, 100), (200, 250))):
        p_c, p_f = total * .6, total * .4
        live_total = total * 1.25
        live_c, live_f = p_c, live_total - p_c
        predicted.append({"step": step, "tu_id": f"tu-{step}", "cumulative": {
            "channel_bytes": total, "C_TO_F_bytes": p_c, "F_TO_C_bytes": p_f,
            "elapsed_ns": elapsed, "throughput_bytes_per_s": total * 1e9 / elapsed}})
        observed.append({"step": step, "tu_id": f"tu-{step}", "cumulative": {
            "channel_bytes": live_total, "C_TO_F_bytes": live_c, "F_TO_C_bytes": live_f,
            "elapsed_ns": elapsed * .8, "throughput_bytes_per_s": live_total * 1e9 / (elapsed * .8)}})
    records = []
    for kind, curve, model_id in (("predictive_sim", predicted, PREDICTOR["model_id"]),
                                   ("live", observed, "s7-live")):
        records.append({"schema": RECORD_SCHEMA, "semantics": SEMANTICS,
                        "record_type": kind, "cell": cell, "split": "calibration",
                        "identity": {**identity, "model_id": model_id}, **META,
                        "units": UNITS, "model_id": model_id,
                        **({"execution_scope": "external_farm_timing",
                            "role_placement": EXTERNAL_PLACEMENT}
                           if kind == "live" else {}),
                        "raw_cumulative_curve": curve})
    records.append({"schema": RECORD_SCHEMA, "semantics": SEMANTICS,
                    "record_type": "comparison", "cell": cell, "split": "calibration",
                    "identity": identity, **META, "units": UNITS,
                    "model_id": PREDICTOR["model_id"], "point_errors": [], "loss_curve": []})
    records[-1].update({"execution_scope": "external_farm_timing",
                        "role_placement": EXTERNAL_PLACEMENT})
    raw = b"".join(canonical_bytes(record) + b"\n" for record in records)
    records_path = root / "records.jsonl"
    records_path.write_bytes(raw)
    topology, depth = context.split("/")
    suite = "C1F1/100000" if topology == "C1F1" else "C1F20/40"
    pass_id = "full-2" if depth == "repeat-full" else "full-1"
    authority = {"schema": "icecream-s8-derived-experiment-v1", "status": "PASS",
                 "cell": cell, "split": "calibration", "topology": suite, "suite": suite,
                 "depth": "full" if depth == "repeat-full" else depth,
                 "depth_class": depth, "pass_id": pass_id,
                 "runs": ["full-1", "full-2"] if depth == "repeat-full" else ["full-1"],
                 "calibration_metadata": dict(META),
                 "execution_scope": "external_farm_timing",
                 "role_placement": EXTERNAL_PLACEMENT,
                 "records": {"path": "records.jsonl", "sha256": hashlib.sha256(raw).hexdigest(),
                             "bytes": len(raw)}}
    (root / "experiment_manifest.json").write_bytes(canonical_bytes(authority) + b"\n")
    return str(records_path), {"path": str(records_path),
                               "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)}


def _explicit_request(root: Path, contexts: set[str] | None = None) -> Path:
    contexts = contexts or {f"{topology}/{depth}"
                            for topology in ("C1F1", "C1F20")
                            for depth in ("100", "200", "full", "repeat-full")}
    comparisons = []
    for context in sorted(contexts):
        for cell in CALIBRATION_CELLS:
            cell_root = root / context.replace("/", "-") / (
                f"{cell['corpus']}-{cell['profile']}-{cell['regime']}")
            path, descriptor = _triple(cell_root, dict(cell), context)
            descriptor["path"] = str(Path(path).relative_to(root))
            comparisons.append({"cell": dict(cell), "topology": context.split("/")[0],
                                "depth_class": context.split("/")[1], "records": descriptor})
    request = {"schema": REQUEST_SCHEMA, "semantics": SEMANTICS,
               "predictor": PREDICTOR, "comparisons": comparisons}
    path = root / "request.json"
    path.write_bytes(canonical_bytes(request) + b"\n")
    return path


def test_complete_slices_pool_ftoc_and_retain_loo_diagnostics(tmp_path: Path) -> None:
    request = _explicit_request(tmp_path)
    manifest = freeze(request, tmp_path / "bundle.json", tmp_path / "manifest.json")
    bundle = json.loads((tmp_path / "bundle.json").read_bytes())
    scales = bundle["calibration"]["scales"]
    assert all(scales[f"{context}/ZSTD_TU/cold"]["C_TO_F_bytes"] == 1.0
               for context in bundle["calibration"]["contexts"])
    assert scales["C1F1/100/ZSTD_TU/cold"]["F_TO_C_bytes"] == pytest.approx(
        scales["C1F20/100/GRZ_RESIDUAL/warm"]["F_TO_C_bytes"])
    assert bundle["calibration"]["diagnostics"]["leave_one_corpus_out"]["fmt"]["non_vacuous"]
    assert bundle["calibration"]["diagnostics"]["leave_one_corpus_out"]["fmt"][
        "p95_abs_log_ratio_error"] >= 0
    assert bundle["calibration"]["diagnostics"]["leave_one_depth_out"]["full"]["count"] > 0
    assert manifest["inputs"]

    subset_root = tmp_path / "subset"
    subset_request = _explicit_request(subset_root)
    subset_value = json.loads(subset_request.read_bytes())
    subset_value["comparisons"] = [item for item in subset_value["comparisons"]
                                    if item["cell"]["profile"] == "ZSTD_TU" and
                                    item["cell"]["regime"] == "cold"]
    subset_request.write_bytes(canonical_bytes(subset_value) + b"\n")
    freeze(subset_request, subset_root / "bundle.json", subset_root / "manifest.json")
    loaded = load_calibration_bundle(subset_root / "manifest.json")
    assert set(loaded["scales"]) == {
        f"{context}/ZSTD_TU/cold" for context in bundle["calibration"]["contexts"]
    }


def test_partial_context_slice_and_metadata_mutation_fail_closed(tmp_path: Path) -> None:
    request = _explicit_request(tmp_path)
    value = json.loads(request.read_bytes())
    value["comparisons"] = [item for item in value["comparisons"]
                             if not (item["topology"] == "C1F20" and
                                     item["depth_class"] == "repeat-full" and
                                     item["cell"]["profile"] == "ZSTD_TU" and
                                     item["cell"]["regime"] == "cold")]
    request.write_bytes(canonical_bytes(value) + b"\n")
    with pytest.raises(CalibrationError, match="partial_complete_slice"):
        freeze(request, tmp_path / "bundle.json")

    request = _explicit_request(tmp_path / "mutated")
    value = json.loads(request.read_bytes())
    descriptor = value["comparisons"][0]["records"]
    record_path = request.parent / descriptor["path"]
    records = [json.loads(line) for line in record_path.read_bytes().splitlines()]
    records[0]["output_contract_digest"] = "e" * 64
    raw = b"".join(canonical_bytes(record) + b"\n" for record in records)
    record_path.write_bytes(raw)
    descriptor.update({"sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)})
    authority_path = record_path.parent / "experiment_manifest.json"
    authority = json.loads(authority_path.read_bytes())
    authority["records"] = {"path": "records.jsonl", "sha256": descriptor["sha256"],
                             "bytes": descriptor["bytes"]}
    authority_path.write_bytes(canonical_bytes(authority) + b"\n")
    request.write_bytes(canonical_bytes(value) + b"\n")
    with pytest.raises(CalibrationError, match="calibration_metadata_mismatch"):
        freeze(request, request.parent / "mutated-bundle.json")


def test_uncalibrated_ftoc_base_is_profile_and_regime_invariant() -> None:
    topology = {"topology": {"c_workers": 1, "f_workers": 1,
                              "cache_channel": "direct"}}
    raw = b"same ordered input" * 100
    rows = []
    for profile in ("ZSTD_TU", "ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"):
        for regime in ("cold", "warm"):
            rows.append(_predict_curve(raw, topology,
                                       {"corpus": "fmt", "profile": profile, "regime": regime})[0]
                        ["cumulative"]["F_TO_C_bytes"])
    assert len(set(rows)) == 1
