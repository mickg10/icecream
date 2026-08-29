from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from s8_predictive_engine import (
    CORPORA,
    CORPUS_MODELS,
    DECLARED_CELLS,
    MANIFEST_SCHEMA,
    OBSERVATIONS_SCHEMA,
    PROFILES,
    REGIMES,
    SEMANTICS,
    SPLITS,
    TOPOLOGY_SCHEMA,
    PredictionError,
    canonical_bytes,
    predict,
)


def _descriptor(path: Path, raw: bytes) -> dict[str, object]:
    return {"path": path.name, "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)}


def _scenario(tmp_path: Path, payload: bytes = b"fmt source\nfmt source\n",
              corpus: str = "fmt", profile: str = "ZSTD_TU", regime: str = "cold",
              split: str | None = None, cache_channel: str = "direct") -> tuple[Path, Path, Path]:
    input_path = tmp_path / "input.ii"
    input_path.write_bytes(payload)
    topology = {
        "schema": TOPOLOGY_SCHEMA,
        "semantics": SEMANTICS,
        "cell": {"corpus": corpus, "profile": profile, "regime": regime},
        "topology": {
            "c_store_guid": "00000000000000000000000000000001",
            "f_store_guid": "00000000000000000000000000000002",
            "history_nonce": 1,
            "c_workers": 1,
            "f_workers": 1,
            "cache_channel": cache_channel,
        },
        "state": {"c_cache": regime, "f_cache": regime, "generation": 0},
    }
    topology_raw = canonical_bytes(topology) + b"\n"
    topology_path = tmp_path / "topology.json"
    topology_path.write_bytes(topology_raw)
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "semantics": SEMANTICS,
        "cell": {"corpus": corpus, "profile": profile, "regime": regime},
        "split": SPLITS[corpus] if split is None else split,
        "predictive_mode": True,
        "input": _descriptor(input_path, payload),
        "topology_state": _descriptor(topology_path, topology_raw),
    }
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_bytes(canonical_bytes(manifest) + b"\n")
    return manifest_path, input_path, topology_path


def _curve(path: Path) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8").splitlines()[0])


def _refresh_manifest(manifest: Path, input_path: Path, topology_path: Path) -> None:
    value = json.loads(manifest.read_bytes())
    input_raw = input_path.read_bytes()
    topology_raw = topology_path.read_bytes()
    value["input"] = _descriptor(input_path, input_raw)
    value["topology_state"] = _descriptor(topology_path, topology_raw)
    manifest.write_bytes(canonical_bytes(value) + b"\n")


def test_trace_free_producer_emits_raw_cumulative_curve(tmp_path: Path) -> None:
    manifest, _input, _topology = _scenario(tmp_path)
    output = tmp_path / "curve.jsonl"
    sidecar = predict(manifest, output, Path("/does/not/exist"))
    row = _curve(output)
    assert row["schema"] == OBSERVATIONS_SCHEMA
    assert row["provenance"] == "modeled"
    assert row["channel_bytes"]["C_TO_F"] > 0
    assert row["channel_bytes"]["F_TO_C"] > 0
    assert row["elapsed_ns"]["total"] == (
        row["elapsed_ns"]["input_ready"] + row["elapsed_ns"]["compile"] +
        row["elapsed_ns"]["result_return"] + row["elapsed_ns"]["transaction_commit"])
    assert row["cumulative"]["channel_bytes"] == sum(row["channel_bytes"][direction]
                                                        for direction in ("C_TO_F", "F_TO_C"))
    assert sidecar["predictor"]["route_trace_consumed"] is False
    assert sidecar["predictor"]["action_trace_input"] is False


@pytest.mark.parametrize("cell", DECLARED_CELLS,
                         ids=lambda value: f"{value['corpus']}-{value['profile']}-{value['regime']}")
def test_every_declared_cell_produces_modeled_curve(tmp_path: Path,
                                                    cell: dict[str, str]) -> None:
    manifest, _input, _topology = _scenario(
        tmp_path, corpus=cell["corpus"], profile=cell["profile"], regime=cell["regime"])
    output = tmp_path / "curve.jsonl"
    sidecar = predict(manifest, output)
    row = _curve(output)
    assert row["cell"] == cell
    assert sidecar["cell"] == cell
    assert sidecar["split"] == SPLITS[cell["corpus"]]
    assert row["channel_bytes"]["total"] > 0
    assert row["elapsed_ns"]["total"] > 0
    elapsed = row["elapsed_ns"]
    assert elapsed["total"] == sum(elapsed[name] for name in (
        "startup", "input_ready", "compile", "result_return", "transaction_commit"))


@pytest.mark.parametrize("corpus", CORPORA)
def test_split_policy_is_enforced(tmp_path: Path, corpus: str) -> None:
    wrong = "held_out_validation" if SPLITS[corpus] == "calibration" else "calibration"
    manifest, _input, _topology = _scenario(tmp_path, corpus=corpus, split=wrong)
    with pytest.raises(PredictionError, match="cell_split_or_predictive_mode_invalid"):
        predict(manifest, tmp_path / "curve.jsonl")


@pytest.mark.parametrize("field, first, second", [
    ("corpus", "fmt", "LLVM-1238"),
    ("profile", "ZSTD_TU", "P29"),
    ("regime", "cold", "warm"),
])
def test_declared_cell_mutation_changes_numeric_prediction(tmp_path: Path, field: str,
                                                            first: str, second: str) -> None:
    first_dir, second_dir = tmp_path / "first", tmp_path / "second"
    first_dir.mkdir()
    second_dir.mkdir()
    first_kwargs = {"corpus": "fmt", "profile": "ZSTD_TU", "regime": "cold"}
    assert first_kwargs[field] == first
    second_kwargs = dict(first_kwargs)
    second_kwargs[field] = second
    first_manifest, *_ = _scenario(first_dir, **first_kwargs)
    second_manifest, *_ = _scenario(second_dir, **second_kwargs)
    predict(first_manifest, first_dir / "curve.jsonl")
    predict(second_manifest, second_dir / "curve.jsonl")
    first_row = _curve(first_dir / "curve.jsonl")
    second_row = _curve(second_dir / "curve.jsonl")
    assert first_row["channel_bytes"] != second_row["channel_bytes"] or first_row["elapsed_ns"] != second_row["elapsed_ns"]


def test_authenticated_input_size_is_not_corpus_scaled(tmp_path: Path) -> None:
    fmt_dir, rocks_dir = tmp_path / "fmt", tmp_path / "rocks"
    fmt_dir.mkdir()
    rocks_dir.mkdir()
    fmt_manifest, *_ = _scenario(fmt_dir, corpus="fmt")
    rocks_manifest, *_ = _scenario(rocks_dir, corpus="RocksDB")
    predict(fmt_manifest, fmt_dir / "curve.jsonl")
    predict(rocks_manifest, rocks_dir / "curve.jsonl")
    assert all("size_factor" not in model for model in CORPUS_MODELS.values())
    assert _curve(fmt_dir / "curve.jsonl")["channel_bytes"] == _curve(rocks_dir / "curve.jsonl")["channel_bytes"]


def test_input_mutation_changes_predicted_values_not_only_identity(tmp_path: Path) -> None:
    manifest, input_path, topology = _scenario(tmp_path, b"A" * 128)
    first = tmp_path / "first.jsonl"
    predict(manifest, first)
    first_row = _curve(first)

    # Same byte count, deliberately different entropy/run structure, so a
    # digest-only implementation cannot satisfy this control.
    input_path.write_bytes(bytes(range(128)))
    _refresh_manifest(manifest, input_path, topology)
    second = tmp_path / "second.jsonl"
    predict(manifest, second)
    second_row = _curve(second)
    assert first_row["features"]["raw_input_bytes"] == second_row["features"]["raw_input_bytes"]
    assert first_row["channel_bytes"] != second_row["channel_bytes"]
    assert first_row["elapsed_ns"] != second_row["elapsed_ns"] or first_row["cumulative"] != second_row["cumulative"]


def test_topology_mutation_changes_predicted_values_not_only_identity(tmp_path: Path) -> None:
    manifest, _input, topology_path = _scenario(tmp_path)
    first = tmp_path / "first.jsonl"
    predict(manifest, first)
    first_row = _curve(first)
    topology = json.loads(topology_path.read_bytes())
    topology["topology"]["c_workers"] = 2
    topology["topology"]["f_workers"] = 2
    topology_path.write_bytes(canonical_bytes(topology) + b"\n")
    _refresh_manifest(manifest, tmp_path / "input.ii", topology_path)
    second = tmp_path / "second.jsonl"
    predict(manifest, second)
    second_row = _curve(second)
    assert first_row["channel_bytes"] == second_row["channel_bytes"]
    assert first_row["elapsed_ns"] != second_row["elapsed_ns"]
    assert first_row["cumulative"] != second_row["cumulative"]


def test_topology_channel_mutation_changes_numeric_prediction(tmp_path: Path) -> None:
    manifest, _input, topology_path = _scenario(tmp_path)
    first = tmp_path / "first.jsonl"
    predict(manifest, first)
    topology = json.loads(topology_path.read_bytes())
    topology["topology"]["cache_channel"] = "route"
    changed = canonical_bytes(topology) + b"\n"
    topology_path.write_bytes(changed)
    _refresh_manifest(manifest, tmp_path / "input.ii", topology_path)
    second = tmp_path / "second.jsonl"
    predict(manifest, second)
    assert _curve(first)["channel_bytes"] != _curve(second)["channel_bytes"]


def test_deleting_authenticated_input_fails_before_output(tmp_path: Path) -> None:
    manifest, input_path, _topology = _scenario(tmp_path)
    input_path.unlink()
    with pytest.raises(PredictionError):
        predict(manifest, tmp_path / "curve.jsonl")
    assert not (tmp_path / "curve.jsonl").exists()


def test_route_trace_manifest_field_is_rejected(tmp_path: Path) -> None:
    manifest, _input, _topology = _scenario(tmp_path)
    value = json.loads(manifest.read_bytes())
    value["route_trace"] = str(tmp_path / "must-not-be-read")
    manifest.write_bytes(canonical_bytes(value) + b"\n")
    with pytest.raises(PredictionError, match="current_engine_manifest"):
        predict(manifest, tmp_path / "out.jsonl")
