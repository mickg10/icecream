from __future__ import annotations

import json
import copy
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


def test_verified_bundle_is_single_read_under_post_verify_mutation(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    experiment = _experiment(tmp_path / "source", pass_id="original")
    original_verify = simulator.verify_experiment

    def verify_then_mutate(path: Path) -> dict[str, object]:
        bundle = original_verify(path)
        manifest = json.loads((path / "manifest.json").read_text())
        manifest["run_identity"]["pass"] = "forged"
        (path / "manifest.json").write_text(json.dumps(manifest) + "\n")
        return bundle

    monkeypatch.setattr(simulator, "verify_experiment", verify_then_mutate)
    output = report.build_report([experiment], tmp_path / "reports")
    rows = [json.loads(line) for line in (output / "results.jsonl").read_text().splitlines()]
    assert {row["pass"] for row in rows} == {"original"}


@pytest.mark.parametrize("timestamp", [None, "2026-09-01T12:00:00Z"])
def test_missing_or_malformed_timestamp_is_clean_cli_reject(
        tmp_path: Path, capsys: pytest.CaptureFixture[str], timestamp: str | None) -> None:
    experiment = _experiment(tmp_path / "source")
    manifest = experiment / "manifest.json"
    value = json.loads(manifest.read_text())
    if timestamp is None:
        del value["run_identity"]["timestamp"]
    else:
        value["run_identity"]["timestamp"] = timestamp
    manifest.write_text(json.dumps(value) + "\n")
    rc = report.main([str(experiment), "--output-root", str(tmp_path / "reports")])
    assert rc == 2
    assert "run_identity" in capsys.readouterr().err


def test_full2_requires_continuity_marker(tmp_path: Path) -> None:
    experiment = _experiment(tmp_path / "source", depth="state-carrying-full-2")
    output = report.build_report([experiment], tmp_path / "reports")
    rows = [json.loads(line) for line in (output / "results.jsonl").read_text().splitlines()]
    assert {row["full2_continuity"]["status"] for row in rows} == {"NOT_PROVEN"}
    assert json.loads((output / "summary.json").read_text())["status"] == \
        "INCOMPLETE_REQUESTED_MATRIX"


def test_full2_mismatched_predecessor_stays_not_proven(tmp_path: Path) -> None:
    predecessor = _experiment(tmp_path / "predecessor", depth="full-1", pass_id="full1")
    experiment = _experiment(tmp_path / "current", depth="state-carrying-full-2")
    manifest_path = experiment / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["repeat_full"] = True
    manifest["predecessor_input_authority"] = {
        "experiment": str(predecessor), "manifest_sha256": "0" * 64,
        "run_identity": {"timestamp": "20260901T120000Z", "topology": "C1F1/100000",
                          "depth": "full-1", "pass": "full1"},
        "selected_inputs": [], "assignment": {"topology": "C1F1/100000"}}
    manifest_path.write_text(json.dumps(manifest) + "\n")
    output = report.build_report([experiment], tmp_path / "reports")
    rows = [json.loads(line) for line in (output / "results.jsonl").read_text().splitlines()]
    assert {row["full2_continuity"]["status"] for row in rows} == {"NOT_PROVEN"}


@pytest.mark.parametrize("mutation", ("omit_next", "zero_next", "wrong_last_type",
                                       "unrelated_prefix", "final_digest", "missing_last_tx"))
def test_full2_current_state_marker_omission_partial_and_type_stay_not_proven(
        monkeypatch: pytest.MonkeyPatch, mutation: str) -> None:
    topology = "C1F1/100000"
    key = "C0|F0"
    prefix = b"prior"
    prefix_hex = prefix.hex()
    prefix_digest = simulator._digest128(prefix)
    prior_state = {"native_last_tu_seq": 4, "native_next_rel_seq": 5,
                   "native_state_digest": "a" * 32, "history_nonce": 1,
                   "route_identity": "C0->F0", "committed_raw_prefix_descriptor":
                   {"schema": simulator.PREFIX_DESCRIPTOR_SCHEMA, "bytes": len(prefix),
                    "digest128": prefix_digest}}
    after_state = {"native_last_tu_seq": 5, "native_next_rel_seq": 6,
                   "native_state_digest": "b" * 32, "history_nonce": 1,
                   "route_identity": "C0->F0", "committed_raw_prefix_descriptor":
                   {"schema": simulator.PREFIX_DESCRIPTOR_SCHEMA, "bytes": len(prefix) + 4,
                    "digest128": simulator._digest128(prefix + b"next")}}
    predecessor_path = "/tmp/s8-full1-fixture"
    predecessor_identity = {"timestamp": "20260901T120000Z", "topology": topology,
                            "depth": "full-1", "pass": "full-1"}
    assignment = {"topology": topology, "rows": []}
    predecessor_manifest = {"schema": simulator.SCHEMA, "experiment": "s8-full1-fixture",
                            "repeat_full": False, "topology": {"id": topology},
                            "run_identity": predecessor_identity,
                            "input_authority": {"selected_inputs": []},
                            "assignment_authority": assignment}
    predecessor_summary = {"schema": simulator.SUMMARY_SCHEMA,
                           "topology": {"id": topology},
                           "relationships": {method: {key: copy.deepcopy(prior_state)}
                                             for method in ("ZSTD_ROUTE", "P29", "GRZ_RESIDUAL")}}
    monkeypatch.setattr(simulator, "verify_experiment", lambda path: {
        "experiment": predecessor_path, "manifest": predecessor_manifest,
        "summary": predecessor_summary, "manifest_facts": {"sha256": "c" * 64}})
    manifest = {"repeat_full": True, "predecessor_input_authority": {
        "experiment": predecessor_path, "manifest_sha256": "c" * 64,
        "run_identity": predecessor_identity, "selected_inputs": [],
        "assignment": assignment}, "assignment_authority": {"topology": topology}}
    summary = {"relationships": {method: {key: copy.deepcopy(after_state)}
                                  for method in ("ZSTD_ROUTE", "P29", "GRZ_RESIDUAL")}}
    rows = [{"method": method, "relationship_key": ["C0", "F0"],
             "ordinal": 0, "native_tu_seq": 5, "native_next_rel_seq": 6,
             "native_state_before_digest": "a" * 32, "native_state_digest": "b" * 32,
             "committed": True, "product_transaction": {"committed": True,
                                      "state_before_digest": "a" * 32, "state_digest": "b" * 32,
                                      "history_nonce": 1, "route_identity": "C0->F0",
                                      "committed_raw_prefix_descriptor":
                                      {"schema": simulator.PREFIX_DESCRIPTOR_SCHEMA,
                                       "bytes": len(prefix) + 4,
                                       "digest128": simulator._digest128(prefix + b"next")},
                                      "committed_raw_prefix_before_descriptor":
                                      {"schema": simulator.PREFIX_DESCRIPTOR_SCHEMA,
                                       "bytes": len(prefix), "digest128": prefix_digest}}}
            for method in ("ZSTD_ROUTE", "P29", "GRZ_RESIDUAL")]
    valid_marker = report._full2_marker(manifest, summary, rows, topology,
                                        "state-carrying-full-2")
    assert valid_marker["status"] == "CONTINUOUS"
    if mutation == "omit_next":
        del summary["relationships"]["P29"][key]["native_next_rel_seq"]
    elif mutation == "zero_next":
        summary["relationships"]["GRZ_RESIDUAL"][key]["native_next_rel_seq"] = 0
    elif mutation == "wrong_last_type":
        summary["relationships"]["ZSTD_ROUTE"][key]["native_last_tu_seq"] = "5"
    elif mutation == "unrelated_prefix":
        other = b"unrelated"
        route = summary["relationships"]["ZSTD_ROUTE"][key]
        route["committed_raw_prefix_descriptor"] = {
            "schema": simulator.PREFIX_DESCRIPTOR_SCHEMA, "bytes": len(other),
            "digest128": simulator._digest128(other)}
    elif mutation == "final_digest":
        summary["relationships"]["P29"][key]["native_state_digest"] = "c" * 32
    else:
        del rows[0]["product_transaction"]["state_digest"]
    marker = report._full2_marker(manifest, summary, rows, topology,
                                  "state-carrying-full-2")
    assert marker["status"] == "NOT_PROVEN"


def test_simulator_route_transaction_preserves_pre_prefix() -> None:
    topology = "C1F1/100000"
    matrix = simulator.MethodMatrixSimulator(
        simulator.MatrixTopology.from_id(topology), methods=("ZSTD_ROUTE",))
    matrix.authority["ZSTD_ROUTE"]["status"] = "READY"
    state = simulator._RelationshipState(
        ("C0", "F0"), history=b"prior", next_rel_seq=5,
        last_route_id="C0->F0", history_nonce=1,
        native_last_tu_seq=4, native_state_digest="a" * 32)
    product = {"tu_seq": 5, "state_before_digest": "a" * 32,
               "state_digest": "b" * 32, "transaction_digest": "d" * 32,
               "encoded_source_bytes": 4, "simulator_execution_ns": 1}
    matrix._native_rows["ZSTD_ROUTE"] = {0: product}
    row = matrix._run_occurrence(
        None, simulator.Occurrence(0, b"next"),
        {"relationship_key": ["C0", "F0"], "relationship_index": 0,
         "slot": 0, "global_slot": 0}, "ZSTD_ROUTE", state,
        raw_override=b"next")
    transaction = row["product_transaction"]
    assert isinstance(transaction, dict)
    assert report._transaction_prefix(transaction, before=True) == (
        5, simulator._digest128(b"prior"))
    assert report._transaction_prefix(transaction, before=False) == (
        9, simulator._digest128(b"priornext"))
    assert transaction["committed_raw_prefix_before_descriptor"] == {
        "schema": simulator.PREFIX_DESCRIPTOR_SCHEMA, "bytes": 5,
        "digest128": simulator._digest128(b"prior")}
    assert transaction["committed_raw_prefix_descriptor"] == {
        "schema": simulator.PREFIX_DESCRIPTOR_SCHEMA, "bytes": 9,
        "digest128": simulator._digest128(b"priornext")}


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
