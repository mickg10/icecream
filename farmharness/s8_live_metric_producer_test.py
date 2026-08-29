from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest

from s8_live_metric_producer import produce
from s8_predictive_live_normalizer import _load_manifest, canonical_bytes


def _digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _fixture(root: Path, regime: str = "cold", corpus: str = "fmt") -> Path:
    package = root / f"s7-{regime}"
    package.mkdir()
    payload = b"fmt source bytes\n"
    summary = {
        "schema": "icecream-s7-live-cell-v1", "cell": f"{corpus}/ZSTD_TU/{regime}",
        "status": "PASS", "live_status": "PASS", "acceptance_status": "PASS",
        "conformance_status": "PASS",
        "binary_sha256": {"client": "d" * 64, "daemon": "e" * 64},
        "measured": {"input_sha256": _digest(payload)},
    }
    results = (json.dumps(summary, sort_keys=True, separators=(",", ":")) + "\n").encode()
    (package / "results.jsonl").write_bytes(results)
    (package / "input.ii").write_bytes(payload)
    rows = [
        {"schema": "icecream-s7-live-timing-v1", "cell": f"{corpus}/ZSTD_TU/{regime}",
         "phase": "measured", "tu_id": "tu-0", "elapsed_ns": 100, "channel_bytes": 50},
        {"schema": "icecream-s7-live-timing-v1", "cell": f"{corpus}/ZSTD_TU/{regime}",
         "phase": "measured", "tu_id": "tu-1", "elapsed_ns": 200, "channel_bytes": 150},
    ]
    timing = b"".join(canonical_bytes(row) + b"\n" for row in rows)
    (package / "timing.jsonl").write_bytes(timing)
    evidence = {
        "schema": "icecream-s7-live-evidence-v1", "cell": f"{corpus}/ZSTD_TU/{regime}",
        "run_id": f"fixture-{regime}", "source_commit": "a" * 40,
        "source_tree": "b" * 40, "topology_sha256": "c" * 64,
        "binary_sha256": {"client": "d" * 64, "daemon": "e" * 64},
        "evidence": {
            "results": {"path": "results.jsonl", "sha256": _digest(results), "bytes": len(results)},
            "timing": {"path": "timing.jsonl", "sha256": _digest(timing), "bytes": len(timing)},
        },
    }
    evidence["evidence_sha256"] = _digest(canonical_bytes(evidence))
    (package / "evidence.json").write_bytes(canonical_bytes(evidence) + b"\n")
    return package


def test_cold_and_warm_emit_authenticated_curve(tmp_path: Path) -> None:
    for regime in ("cold", "warm"):
        out = tmp_path / f"out-{regime}"
        result = json.loads(produce(_fixture(tmp_path, regime), out).read_text())
        assert result["status"] == "PASS"
        assert result["scored"] is True
        rows = [json.loads(line) for line in (out / "live-curve.jsonl").read_bytes().splitlines()]
        assert rows[0]["cumulative"] == {
            "channel_bytes": 50, "elapsed_ns": 100,
            "throughput_bytes_per_s": 500000000.0,
        }
        assert rows[1]["cumulative"]["channel_bytes"] == 200
        artifact = _load_manifest(out / "live-curve-manifest.json", "live")
        assert artifact["evidence"]["binary_sha256"]["client"] == "d" * 64
        assert artifact["units"] == {
            "point": "step", "channel_bytes": "bytes", "elapsed_ns": "ns",
            "throughput_bytes_per_s": "bytes_per_s",
        }


def test_missing_timing_is_hold_and_never_scored(tmp_path: Path) -> None:
    package = _fixture(tmp_path)
    (package / "timing.jsonl").unlink()
    result = json.loads(produce(package, tmp_path / "hold").read_text())
    assert result == {
        "schema": "icecream-s8-live-metric-producer-v1",
        "status": "HOLD", "scored": False,
        "reason": "evidence.timing:unavailable:" + str(package / "timing.jsonl"),
    }
    assert not (tmp_path / "hold" / "live-curve-manifest.json").exists()


@pytest.mark.parametrize("corpus", ("DuckDB", "LLVM-1238"))
def test_held_out_cells_are_scored_only_from_authenticated_fixture(tmp_path: Path,
                                                                    corpus: str) -> None:
    result = json.loads(produce(_fixture(tmp_path, corpus=corpus),
                                tmp_path / "live").read_text())
    assert result["status"] == "PASS"
    assert result["scored"] is True
    manifest = json.loads((tmp_path / "live" / "live-curve-manifest.json").read_text())
    assert manifest["identity"]["split"] == "held_out_validation"


def test_undeclared_cell_is_hold_before_evidence_is_trusted(tmp_path: Path) -> None:
    package = _fixture(tmp_path)
    summary_path = package / "results.jsonl"
    summary = json.loads(summary_path.read_text())
    summary["cell"] = "not-a-corpus/ZSTD_TU/cold"
    summary_path.write_bytes(canonical_bytes(summary) + b"\n")
    result = json.loads(produce(package, tmp_path / "hold").read_text())
    assert result["status"] == "HOLD"
    assert result["scored"] is False
    assert result["reason"] == "results:cell_invalid"


def test_mismatched_evidence_digest_is_hold(tmp_path: Path) -> None:
    package = _fixture(tmp_path)
    evidence = json.loads((package / "evidence.json").read_text())
    evidence["binary_sha256"]["client"] = "f" * 64
    (package / "evidence.json").write_bytes(canonical_bytes(evidence) + b"\n")
    result = json.loads(produce(package, tmp_path / "hold").read_text())
    assert result["status"] == "HOLD"
    assert result["reason"] == "evidence_sha256:mismatch"


def test_binary_hash_mismatch_with_s7_summary_is_hold(tmp_path: Path) -> None:
    package = _fixture(tmp_path)
    evidence = json.loads((package / "evidence.json").read_text())
    evidence["binary_sha256"]["client"] = "f" * 64
    evidence.pop("evidence_sha256")
    evidence["evidence_sha256"] = _digest(canonical_bytes(evidence))
    (package / "evidence.json").write_bytes(canonical_bytes(evidence) + b"\n")
    result = json.loads(produce(package, tmp_path / "hold").read_text())
    assert result["status"] == "HOLD"
    assert result["reason"] == "binary_sha256:results_mismatch"
