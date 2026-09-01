from __future__ import annotations

import json
from pathlib import Path

import pytest

import s8_method_matrix_report as report
import s8_method_matrix_simulator as simulator


def _experiment(tmp_path: Path, *, timestamp: str = "20260901T120000Z",
                depth: str = "100", pass_id: str = "p1", methods=("RAW_II",)) -> Path:
    return simulator.MethodMatrixSimulator(
        simulator.MatrixTopology.from_id("C1F1/100000"), methods=methods
    ).run([simulator.Occurrence(0, b"payload")], output_root=tmp_path,
          timestamp=timestamp, depth=depth, pass_id=pass_id)


def test_report_has_one_row_per_method_and_raw_is_descriptor_only(tmp_path: Path) -> None:
    experiment = _experiment(tmp_path / "source")
    output = report.build_report([experiment], tmp_path / "reports")
    rows = [json.loads(line) for line in (output / "results.jsonl").read_text().splitlines()]
    assert len(rows) == 7
    raw = next(row for row in rows if row["method"] == "RAW_II")
    assert raw["raw_bytes"] == len(b"payload")
    assert raw["encoded_bytes"] is None
    assert raw["c_to_f_bytes"] is None
    assert raw["f_to_c_bytes"] is None
    assert raw["execution_ns"] is None
    assert raw["wire_witnessed"] is False
    assert raw["ratio_reason"] == "encoded_bytes_unavailable"
    cohort = next(row for row in rows if row["method"] == "ZSTD_COHORT")
    global_row = next(row for row in rows if row["method"] == "ZSTD_GLOBAL")
    assert cohort["classification"] == "optional"
    assert global_row["classification"] == "optional"
    assert "optional_unavailable_is_not_core_failure" in json.loads(
        (output / "summary.json").read_text())
    assert json.loads((output / "summary.json").read_text())["status"] == \
        "INCOMPLETE_REQUESTED_MATRIX"
    assert {name for name in ("results.jsonl", "summary.json", "matrix.csv", "table.md")
            if (output / name).is_file()} == {"results.jsonl", "summary.json", "matrix.csv", "table.md"}


def test_verified_not_ready_canary_is_reported_without_core_failure(tmp_path: Path) -> None:
    experiment = simulator.write_not_ready_canary(
        tmp_path / "source", tmp_path / "missing-trace.tsv",
        topology=simulator.MatrixTopology.from_id("C1F20/40"), count=100,
        depth="100", pass_id="hold", reason="missing authenticated input")
    output = report.build_report([experiment], tmp_path / "reports")
    rows = [json.loads(line) for line in (output / "results.jsonl").read_text().splitlines()]
    assert len(rows) == 7
    assert {row["status"] for row in rows} == {"UNAVAILABLE"}
    assert all(row["raw_bytes"] is None for row in rows)


def test_duplicate_key_is_rejected(tmp_path: Path) -> None:
    first = _experiment(tmp_path / "one")
    second = _experiment(tmp_path / "two")
    with pytest.raises(report.ReportError, match="duplicate_experiment_key"):
        report.build_report([first, second], tmp_path / "reports")


def test_report_output_directory_is_collision_safe(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    experiment = _experiment(tmp_path / "source")
    monkeypatch.setattr(report, "_stable_stamp", lambda: "20260901T120000Z")
    first = report.build_report([experiment], tmp_path / "reports")
    second = report.build_report([experiment], tmp_path / "reports")
    assert first != second
    assert second.name.endswith("-r01")


def test_mutated_verified_artifact_is_rejected(tmp_path: Path) -> None:
    experiment = _experiment(tmp_path / "source")
    occurrence_file = experiment / "occurrences.jsonl"
    occurrence_file.write_bytes(occurrence_file.read_bytes() + b" ")
    with pytest.raises(report.ReportError, match="experiment_verification_failed"):
        report.build_report([experiment], tmp_path / "reports")


def test_full2_requires_continuity_marker(tmp_path: Path) -> None:
    experiment = _experiment(tmp_path / "source", depth="state-carrying-full-2")
    with pytest.raises(report.ReportError, match="full2:predecessor_continuity_marker_missing"):
        report.build_report([experiment], tmp_path / "reports")


def test_ratio_zero_denominator_and_wire_witness_control() -> None:
    assert report._ratio(0, 0) == (None, None, "zero_raw_denominator")
    assert report._ratio(100, 75) == (0.75, 0.25, None)
    row = {"schema": simulator.OCCURRENCE_SCHEMA, "method": "ZSTD_TU",
           "topology": "C1F1/100000", "ordinal": 0, "raw_bytes": 4,
           "encoded_bytes": 2, "codec_cpu_ns": 1, "codec_wall_ns": 1,
           "wire_witnessed": True, "status": "READY"}
    with pytest.raises(report.ReportError, match="without_product_transaction"):
        report._validate_rows([row], {}, "C1F1/100000", Path("."))


def test_native_transaction_is_the_only_wire_witness(tmp_path: Path) -> None:
    base = {"schema": simulator.OCCURRENCE_SCHEMA, "method": "ZSTD_TU",
            "topology": "C1F1/100000", "ordinal": 0, "raw_bytes": 10,
            "encoded_bytes": 5, "codec_cpu_ns": 1, "codec_wall_ns": 1,
            "wire_witnessed": True, "status": "READY",
            "product_transaction": {"c_to_f_bytes": 5, "f_to_c_bytes": 7,
                                     "simulator_execution_ns": 11}}
    grouped = report._validate_rows([base], {}, "C1F1/100000", Path("."))
    manifest = {"methods": ["ZSTD_TU"], "authority": {}}
    summary = {"method_status": {"ZSTD_TU": "READY"}}
    (tmp_path / "manifest.json").write_bytes(b"{}")
    result = report._method_result(tmp_path, manifest, summary, grouped, "ZSTD_TU",
                                   "C1F1/100000", "100", "p", "t",
                                   {"status": "NOT_APPLICABLE"})
    assert result["wire_witnessed"] is True
    assert result["c_to_f_bytes"] == 5
    assert result["f_to_c_bytes"] == 7
    assert result["execution_ns"] == 11
