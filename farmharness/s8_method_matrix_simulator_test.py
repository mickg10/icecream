from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest
import s8_method_matrix_simulator as simulator_module

from s8_method_matrix_simulator import (
    CORE_METHODS,
    DEFAULT_CLI_METHODS,
    MatrixError,
    MatrixTopology,
    MethodMatrixSimulator,
    Occurrence,
    assign_relationships,
    repeat_full_state_contract,
    _authenticated_assignment,
    firefox_occurrences,
    verify_experiment,
    _libbsc_authority,
    write_not_ready_canary,
)


def test_capacity_is_not_relationship_cardinality() -> None:
    one = MatrixTopology.from_id("C1F1/100000")
    many = MatrixTopology.from_id("C1F20/40")
    assert one.relationship_count == 1
    assert one.global_slots == 100000
    assert many.relationship_count == 20
    assert many.slots_per_f == 2
    assert many.global_slots == 40
    # Both concurrent slots on F0 resolve to the same relationship state.
    assert many.relationship_for("F0", 0) == many.relationship_for("F0", 1)
    assert len({tuple(row["relationship_key"])
                 for row in assign_relationships(
                     many, [Occurrence(i, b"x", f_store_guid="F0", slot=i % 2)
                            for i in range(10)])}) == 1


def test_deterministic_assignment_has_exactly_twenty_relationships() -> None:
    topology = MatrixTopology.from_id("C1F20/40", c_store_guid="C-guid")
    occurrences = [Occurrence(i, b"payload", slot=i % 2) for i in range(100)]
    authority = _authenticated_assignment("C1F20/40", 100)
    first = assign_relationships(topology, occurrences, authority)
    second = assign_relationships(topology, occurrences, authority)
    assert first == second
    assert len({tuple(item["relationship_key"]) for item in first}) == 20
    assert {item["slot"] for item in first} == {0, 1}


def test_authenticated_assignment_preserves_build_boundary_and_formula() -> None:
    authority = _authenticated_assignment("C1F20/40", 2499)
    assert authority["rows"][2497]["authority_logical"] == 2497
    assert authority["rows"][2498]["authority_build"] == 1
    assert authority["rows"][2498]["authority_logical"] == 0
    for item in authority["rows"][:100]:
        assert item["f_relationship"] == item["global_slot"] // 2
        assert item["per_f_slot"] == item["global_slot"] % 2


def test_real_c1f20_authority_covers_all_five_builds() -> None:
    authority = _authenticated_assignment("C1F20/40", 12490)
    assert authority["selected_count"] == 12490
    assert authority["rows"][2498]["authority_build"] == 1
    assert authority["rows"][12489]["authority_logical"] == 2497


def test_firefox_occurrence_keeps_global_dispatch_at_build_boundary() -> None:
    trace = Path("/tanksmall/scratch/ictmp/lo-s4-e50.G5KsGG/capability/distribution/firefox-corrected.compile-trace.tsv")
    occurrence = firefox_occurrences(trace, topology_id="C1F20/40", count=1,
                                     dispatch_start=2498)[0]
    assert occurrence.ordinal == 2498
    assert occurrence.source_build == 1
    assert occurrence.source_logical == 0
    assert occurrence.raw is None


def test_firefox_loader_uses_requested_topology_at_repeat_boundary(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    source = tmp_path / "unit.ii"
    source.write_bytes(b"x")
    trace = tmp_path / "trace.tsv"
    trace.write_text("logical\tjob_id\tii_relative\tactual_input\traw_bytes\n"
                     "0\tjob\tunit.ii\tactual\t1\n")
    manifest = tmp_path / "manifest.txt"
    manifest.write_text("unit.ii\n")
    monkeypatch.setattr(simulator_module, "AUTH_TRACE_SHA256",
                        hashlib.sha256(trace.read_bytes()).hexdigest())
    monkeypatch.setattr(simulator_module, "AUTH_CORPUS_MANIFEST_SHA256",
                        hashlib.sha256(manifest.read_bytes()).hexdigest())
    calls: list[tuple[str, int, int]] = []

    def assignment(topology_id: str, count: int, *, start: int = 0) -> dict[str, object]:
        calls.append((topology_id, count, start))
        return {"rows": [{"authority_logical": 0, "dispatch_order": 2498,
                           "authority_build": 1}], "selected_count": count,
                "topology": topology_id}

    monkeypatch.setattr(simulator_module, "_authenticated_assignment", assignment)
    rows = firefox_occurrences(trace, topology_id="C1F20/40", count=1,
                               dispatch_start=2498, corpus_root=tmp_path,
                               corpus_manifest=manifest)
    assert calls == [("C1F20/40", 1, 2498)]
    assert rows[0].ordinal == 2498
    assert rows[0].source_build == 1
    assert rows[0].source_logical == 0


def test_cli_method_selection_is_stable_duplicate_free_and_core_default() -> None:
    assert DEFAULT_CLI_METHODS == (
        "RAW_II", "ZSTD_TU", "P29", "GRZ_RESIDUAL", "ZSTD_ROUTE")
    assert simulator_module._validate_method_selection(DEFAULT_CLI_METHODS) == DEFAULT_CLI_METHODS
    with pytest.raises(MatrixError, match="duplicates"):
        simulator_module._validate_method_selection(("RAW_II", "RAW_II"))
    with pytest.raises(MatrixError, match="one or more"):
        simulator_module._validate_method_selection(())


def test_not_ready_canary_retains_only_selected_methods(tmp_path: Path) -> None:
    experiment = write_not_ready_canary(
        tmp_path, tmp_path / "missing.tsv", topology=MatrixTopology.from_id("C1F1/100000"),
        count=100, reason="missing", methods=DEFAULT_CLI_METHODS)
    manifest = json.loads((experiment / "manifest.json").read_text())
    assert manifest["methods"] == list(DEFAULT_CLI_METHODS)


def test_cli_threads_topology_and_methods_into_not_ready_path(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]) -> None:
    loader_calls: list[tuple[str, int, int]] = []
    canary_methods: list[tuple[str, ...]] = []

    def no_inputs(trace: Path, *, topology_id: str, count: int,
                  dispatch_start: int, **kwargs: object) -> list[Occurrence]:
        loader_calls.append((topology_id, count, dispatch_start))
        return []

    def canary(*args: object, methods: tuple[str, ...], **kwargs: object) -> Path:
        canary_methods.append(methods)
        return tmp_path / f"canary-{len(canary_methods)}"

    monkeypatch.setattr(simulator_module, "firefox_occurrences", no_inputs)
    monkeypatch.setattr(simulator_module, "write_not_ready_canary", canary)
    assert simulator_module.main([
        "--firefox-trace", str(tmp_path / "trace.tsv"), "--output-root", str(tmp_path),
        "--depth", "100", "--methods", "RAW_II", "ZSTD_TU",
    ]) == 0
    assert loader_calls == [("C1F1/100000", 100, 0), ("C1F20/40", 100, 0)]
    assert canary_methods == [("RAW_II", "ZSTD_TU"), ("RAW_II", "ZSTD_TU")]
    capsys.readouterr()


def test_cli_rejects_duplicate_methods(tmp_path: Path) -> None:
    with pytest.raises(SystemExit, match="2"):
        simulator_module.main([
            "--firefox-trace", str(tmp_path / "trace.tsv"),
            "--methods", "RAW_II", "RAW_II",
        ])


def test_route_uses_fresh_frames_and_only_commit_advances_prefix(tmp_path: Path) -> None:
    topology = MatrixTopology.from_id("C1F1/100000")
    simulator = MethodMatrixSimulator(topology, methods=("ZSTD_ROUTE",), max_history_bytes=8)
    rows = simulator.run([
        Occurrence(0, b"A" * 32),
        Occurrence(1, b"B" * 32, commit=False, release=True),
        Occurrence(2, b"C" * 32),
    ], output_root=tmp_path, timestamp="20260901T120000Z")
    data = [json.loads(line) for line in (rows / "occurrences.jsonl").read_text().splitlines()]
    assert [row["transition"] for row in data] == [
        "committed_relationship_advance", "tentative_discarded_explicit_release",
        "committed_relationship_advance"]
    assert data[1]["pre_state_digest"] == data[1]["post_state_digest"]
    summary = json.loads((rows / "summary.json").read_text())
    assert summary["relationships"]["ZSTD_ROUTE"]["C0|F0"]["next_rel_seq"] == 2
    descriptor = summary["relationships"]["ZSTD_ROUTE"]["C0|F0"]["committed_raw_prefix_descriptor"]
    assert descriptor["schema"] == simulator_module.PREFIX_DESCRIPTOR_SCHEMA
    assert descriptor["bytes"] == 8
    # A fresh encoded frame is retained for every occurrence, including the
    # rejected candidate; no endless stream is emitted by this contract.
    assert len(list((rows / "bytes" / "ZSTD_ROUTE").glob("encoded-*.bin"))) == 3


def test_unreleased_preparation_blocks_until_explicit_release() -> None:
    simulator = MethodMatrixSimulator(MatrixTopology.from_id("C1F1/100000"),
                                       methods=("RAW_II",))
    with pytest.raises(MatrixError, match="pending preparation"):
        simulator.run([Occurrence(0, b"a", commit=False), Occurrence(1, b"b")])


def test_route_reset_rebind_requires_new_nonce_and_preserves_order() -> None:
    topology = MatrixTopology.from_id("C1F1/100000")
    simulator = MethodMatrixSimulator(topology, methods=("ZSTD_ROUTE",))
    with pytest.raises(MatrixError, match="rebind"):
        simulator.run([Occurrence(0, b"a", route_id="route-a"),
                       Occurrence(1, b"b", route_id="route-b")])

    simulator = MethodMatrixSimulator(topology, methods=("ZSTD_ROUTE",))
    result = simulator.run([
        Occurrence(0, b"a", route_id="route-a"),
        Occurrence(1, b"b", route_id="route-a"),
        Occurrence(2, b"c", route_id="route-b", history_nonce=2, reset_before=True),
    ])
    assert result["rows"][2]["transition"] == "committed_relationship_advance"
    assert result["rows"][2]["post_state_digest"] != result["rows"][1]["post_state_digest"]


def test_large_route_rows_keep_fixed_size_prefix_evidence() -> None:
    topology = MatrixTopology.from_id("C1F1/100000")
    matrix = MethodMatrixSimulator(topology, methods=("ZSTD_ROUTE",), max_history_bytes=1024)
    matrix.authority["ZSTD_ROUTE"]["status"] = "READY"
    state = simulator_module._RelationshipState(("C0", "F0"), history=b"",
                                                  last_route_id="C0->F0")
    rows = []
    for ordinal in range(100):
        matrix._native_rows["ZSTD_ROUTE"] = {ordinal: {
            "tu_seq": ordinal, "state_before_digest": "a" * 32,
            "state_digest": "b" * 32, "transaction_digest": "d" * 32,
            "encoded_source_bytes": 1 << 20, "simulator_execution_ns": 1}}
        rows.append(matrix._run_occurrence(
            None, Occurrence(ordinal, b"x" * (1 << 20)),
            {"relationship_key": ["C0", "F0"], "relationship_index": 0,
             "slot": 0, "global_slot": 0}, "ZSTD_ROUTE", state,
            raw_override=b"x" * (1 << 20)))
    raw = ("\n".join(json.dumps(row, separators=(",", ":")) for row in rows)).encode()
    assert len(raw) < 400_000
    half = ("\n".join(json.dumps(row, separators=(",", ":")) for row in rows[:50])).encode()
    assert len(raw) < 2 * len(half) + 1_000

    def contains_body(value: object) -> bool:
        if isinstance(value, dict):
            return any(key == "committed_raw_prefix" or contains_body(item)
                       for key, item in value.items())
        if isinstance(value, list):
            return any(contains_body(item) for item in value)
        return False

    assert not contains_body(rows)
    assert all(row["product_transaction"]["committed_raw_prefix_descriptor"]["bytes"] == 1024
               for row in rows)


def test_route_bytearray_runtime_state_does_not_advance_on_tentative_row() -> None:
    matrix = MethodMatrixSimulator(MatrixTopology.from_id("C1F1/100000"),
                                   methods=("ZSTD_ROUTE",), max_history_bytes=8)
    matrix.authority["ZSTD_ROUTE"]["status"] = "READY"
    state = simulator_module._RelationshipState(("C0", "F0"), history=bytearray(b"seed"))
    matrix._native_rows["ZSTD_ROUTE"] = {0: {
        "tu_seq": 0, "state_before_digest": "a" * 32,
        "state_digest": "b" * 32, "transaction_digest": "d" * 32,
        "encoded_source_bytes": 1, "simulator_execution_ns": 1}}
    matrix._run_occurrence(
        None, Occurrence(0, b"candidate", commit=False),
        {"relationship_key": ["C0", "F0"], "relationship_index": 0,
         "slot": 0, "global_slot": 0}, "ZSTD_ROUTE", state,
        raw_override=b"candidate")
    assert state.history == bytearray(b"seed")


def test_route_verifier_binds_global_dispatch_ordinal_at_full2_boundary(tmp_path: Path) -> None:
    source = tmp_path / "boundary.ii"
    source.write_bytes(b"boundary")
    authority = _authenticated_assignment("C1F1/100000", 1, start=2498)
    item = {"ordinal": 2498, "source_relative": source.name,
            "bytes": source.stat().st_size,
            "sha256": hashlib.sha256(source.read_bytes()).hexdigest()}
    observations, _final, _facts = simulator_module._stream_route_prefixes(
        [item], authority, tmp_path, label="boundary")
    assert set(observations) == {2498}
    item["ordinal"] = 0
    with pytest.raises(MatrixError, match="identity_invalid"):
        simulator_module._stream_route_prefixes([item], authority, tmp_path, label="boundary")


@pytest.mark.parametrize("count", (32, 64, 128))
def test_subprocess_route_prefix_evidence_is_bounded_and_linear(count: int) -> None:
    code = r'''
import json, resource
import s8_method_matrix_simulator as s

topology = s.MatrixTopology.from_id("C1F1/100000")
matrix = s.MethodMatrixSimulator(topology, methods=("ZSTD_ROUTE",), max_history_bytes=1024)
matrix.authority["ZSTD_ROUTE"]["status"] = "READY"
state = s._RelationshipState(("C0", "F0"))
rows = []
for ordinal in range(COUNT):
    matrix._native_rows["ZSTD_ROUTE"] = {ordinal: {
        "tu_seq": ordinal, "state_before_digest": "a" * 32,
        "state_digest": "b" * 32, "transaction_digest": "d" * 32,
        "encoded_source_bytes": 1, "simulator_execution_ns": 1}}
    rows.append(matrix._run_occurrence(
        None, s.Occurrence(ordinal, b"x" * (1 << 20)),
        {"relationship_key": ["C0", "F0"], "relationship_index": 0,
         "slot": 0, "global_slot": 0}, "ZSTD_ROUTE", state,
        raw_override=b"x" * (1 << 20)))
wire = ("\n".join(json.dumps(row, separators=(",", ":")) for row in rows)).encode()
assert b'"committed_raw_prefix":' not in wire
print(json.dumps({"bytes": len(wire), "rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}))
'''.replace("COUNT", str(count))
    completed = subprocess.run([sys.executable, "-c", code], check=True,
                               capture_output=True, text=True,
                               env={**os.environ, "PYTHONPATH": str(Path(__file__).parent)})
    result = json.loads(completed.stdout)
    assert result["bytes"] < count * 5_000
    assert result["rss_kib"] < 256 * 1024


def test_methods_do_not_alias_and_missing_authority_is_explicit() -> None:
    topology = MatrixTopology.from_id("C1F1/100000")
    simulator = MethodMatrixSimulator(topology)
    result = simulator.run([Occurrence(0, b"payload")])
    assert result["core_completion"]["status"] == "NOT_READY"
    statuses = result["method_status"]
    assert statuses == {
        "RAW_II": "READY", "ZSTD_TU": "READY", "P29": "READY",
        "GRZ_RESIDUAL": "READY", "ZSTD_ROUTE": "READY",
        "ZSTD_COHORT": "NOT_READY", "ZSTD_GLOBAL": "NOT_IMPLEMENTED",
    }
    rows = {row["method"]: row for row in result["rows"]}
    assert rows["ZSTD_COHORT"]["encoded_bytes"] is None
    assert rows["ZSTD_GLOBAL"]["encoded_bytes"] is None
    assert rows["RAW_II"]["encoded_sha256"] is None
    assert rows["RAW_II"]["measurement_scope"] == "raw_bytes_only_no_wire_witness"


def test_raw_control_retains_no_copy_and_runs_are_collision_safe(tmp_path: Path,
                                                                monkeypatch: pytest.MonkeyPatch) -> None:
    source = tmp_path / "external.ii"
    source.write_bytes(b"payload")
    occurrence = Occurrence(0, None, source_path=str(source),
                            source_sha256=hashlib.sha256(b"payload").hexdigest())
    monkeypatch.setattr(Occurrence, "read_raw",
                        lambda _self: pytest.fail("RAW_II loaded an external payload"))
    simulator = MethodMatrixSimulator(MatrixTopology.from_id("C1F1/100000"),
                                       methods=("RAW_II",))
    first = simulator.run([occurrence], output_root=tmp_path,
                          timestamp="20260901T120000Z", depth="100")
    second = simulator.run([occurrence], output_root=tmp_path,
                           timestamp="20260901T120000Z", depth="100")
    assert first != second
    for experiment in (first, second):
        assert not list((experiment / "bytes").rglob("*"))
        row = json.loads((experiment / "occurrences.jsonl").read_text())
        assert row["method"] == "RAW_II"
        assert row["encoded_bytes"] is None
        assert row["codec_cpu_ns"] is None
        assert row["codec_wall_ns"] is None


def test_experiment_verifier_rejects_deletion_and_mutation(tmp_path: Path) -> None:
    experiment = MethodMatrixSimulator(
        MatrixTopology.from_id("C1F1/100000"), methods=("ZSTD_TU",)).run(
            [Occurrence(0, b"payload")], output_root=tmp_path,
            timestamp="20260901T120000Z", depth="100")
    assert verify_experiment(experiment)["status"] == "PASS"
    occurrences = experiment / "occurrences.jsonl"
    original = occurrences.read_bytes()
    occurrences.unlink()
    with pytest.raises(MatrixError, match="artifact_added_or_deleted"):
        verify_experiment(experiment)
    occurrences.write_bytes(original)
    summary = experiment / "summary.json"
    summary.write_bytes(summary.read_bytes() + b"\n")
    with pytest.raises(MatrixError, match="artifact_mutated:summary.json"):
        verify_experiment(experiment)


def test_experiment_evidence_rejects_duplicate_nonfinite_and_symlink(tmp_path: Path) -> None:
    experiment = MethodMatrixSimulator(
        MatrixTopology.from_id("C1F1/100000"), methods=("RAW_II",)).run(
            [Occurrence(0, b"payload")], output_root=tmp_path,
            timestamp="20260901T120000Z", depth="100")
    manifest = experiment / "manifest.json"
    original = manifest.read_bytes()
    duplicate = original.replace(
        b'"schema":"icecream-s8-method-matrix-simulator-v1"',
        b'"schema":"forged","schema":"icecream-s8-method-matrix-simulator-v1"', 1)
    manifest.write_bytes(duplicate)
    with pytest.raises(MatrixError, match="json:duplicate_key:schema"):
        verify_experiment(experiment)
    manifest.write_bytes(original)

    occurrences = experiment / "occurrences.jsonl"
    original_occurrences = occurrences.read_bytes()
    changed = occurrences.read_bytes().replace(b'"authority":', b'"x":NaN,"authority":', 1)
    occurrences.write_bytes(changed)
    value = json.loads(original)
    value["artifacts"]["occurrences.jsonl"] = {
        "bytes": len(changed), "sha256": hashlib.sha256(changed).hexdigest()}
    manifest.write_bytes(json.dumps(value, sort_keys=True, separators=(",", ":")).encode() + b"\n")
    with pytest.raises(MatrixError, match="json:nonfinite:NaN"):
        verify_experiment(experiment)

    occurrences.write_bytes(original_occurrences)
    manifest.write_bytes(original)
    value = json.loads(original)
    value["input_authority"]["selected_inputs"] = [{
        "ordinal": 0, "build": 0, "logical": 0, "source_relative": "../outside",
        "bytes": 7, "sha256": "f" * 64}]
    manifest.write_bytes(json.dumps(value, sort_keys=True, separators=(",", ":")).encode() + b"\n")
    with pytest.raises(MatrixError, match="input_descriptor_invalid"):
        verify_experiment(experiment)

    manifest.write_bytes(original)
    for legacy_key in (b'"committed_raw_prefix"',
                       b'"committed_raw_prefix_before"',
                       b'"committed_raw_prefix_bytes"',
                       b'"committed_raw_prefix_digest"'):
        body_row = original_occurrences.replace(
            b'"authority":', legacy_key + b':"forbidden","authority":', 1)
        occurrences.write_bytes(body_row)
        value = json.loads(original)
        value["artifacts"]["occurrences.jsonl"] = {
            "bytes": len(body_row), "sha256": hashlib.sha256(body_row).hexdigest()}
        manifest.write_bytes(json.dumps(value, sort_keys=True, separators=(",", ":")).encode() + b"\n")
        with pytest.raises(MatrixError, match="route_prefix_body_forbidden"):
            verify_experiment(experiment)

    occurrences.write_bytes(original_occurrences)
    manifest.write_bytes(original)
    escape = tmp_path / "outside-artifact"
    escape.write_bytes(b"outside")
    (experiment / "bytes" / "escape.bin").symlink_to(escape)
    with pytest.raises(MatrixError, match="artifact_symlink"):
        verify_experiment(experiment)


def test_native_batch_rejects_duplicate_output_ordinal(tmp_path: Path,
                                                       monkeypatch: pytest.MonkeyPatch) -> None:
    source = tmp_path / "unit.ii"
    source.write_bytes(b"native payload")
    raw = source.read_bytes()
    occurrence = Occurrence(0, None, source_path=str(source), source_relative=source.name,
                            source_sha256=hashlib.sha256(raw).hexdigest(),
                            source_digest128=simulator_module._digest128(raw))
    topology = MatrixTopology.from_id("C1F1/100000")
    assignment = {"status": "READY", "topology": topology.topology_id,
                  "selected_count": 1, "rows": [{
                      "ordinal": 0, "global_slot": 0, "f_relationship": 0,
                      "per_f_slot": 0, "dispatch_order": 0,
                      "authority_dispatch_order": 0}]}
    original_run = simulator_module.subprocess.run

    def duplicate_output(*args: object, **kwargs: object) -> object:
        result = original_run(*args, **kwargs)
        command = args[0] if args else kwargs["args"]
        output = Path(str(command[command.index("--batch-output") + 1]))
        output.write_bytes(output.read_bytes() + output.read_bytes())
        return result

    monkeypatch.setattr(simulator_module.subprocess, "run", duplicate_output)
    with pytest.raises(MatrixError, match="duplicate ordinal"):
        simulator_module._native_batch([occurrence], topology, assignment, "ZSTD_TU")


@pytest.mark.parametrize("field", ("profile", "relationship_id", "tu_seq",
                                    "raw_digest", "state_digest"))
def test_native_batch_rejects_mutated_identity_fields(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch, field: str) -> None:
    source = tmp_path / "unit.ii"
    source.write_bytes(b"native payload")
    raw = source.read_bytes()
    occurrence = Occurrence(0, None, source_path=str(source), source_relative=source.name,
                            source_sha256=hashlib.sha256(raw).hexdigest(),
                            source_digest128=simulator_module._digest128(raw))
    topology = MatrixTopology.from_id("C1F1/100000")
    assignment = {"status": "READY", "topology": topology.topology_id,
                  "selected_count": 1, "rows": [{"ordinal": 0, "f_relationship": 0,
                  "per_f_slot": 0, "dispatch_order": 0, "authority_dispatch_order": 0}]}
    original_run = simulator_module.subprocess.run

    def mutate_output(*args: object, **kwargs: object) -> object:
        result = original_run(*args, **kwargs)
        command = args[0] if args else kwargs["args"]
        output = Path(str(command[command.index("--batch-output") + 1]))
        value = json.loads(output.read_text())
        value[field] = ("P29" if field == "profile" else
                        "bad" if field == "relationship_id" else
                        1 if field == "tu_seq" else "0" * 32)
        output.write_text(json.dumps(value) + "\n")
        return result

    monkeypatch.setattr(simulator_module.subprocess, "run", mutate_output)
    with pytest.raises(MatrixError):
        simulator_module._native_batch([occurrence], topology, assignment, "ZSTD_TU")


def test_not_ready_runs_are_collision_safe_and_identity_bound(tmp_path: Path,
                                                              monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(simulator_module, "_stamp", lambda: "20260901T120000Z")
    trace = tmp_path / "missing.tsv"
    first = write_not_ready_canary(tmp_path, trace,
        topology=MatrixTopology.from_id("C1F1/100000"), count=100,
        depth="100", reason="missing")
    second = write_not_ready_canary(tmp_path, trace,
        topology=MatrixTopology.from_id("C1F1/100000"), count=100,
        depth="100", reason="missing")
    assert first != second
    assert first.name.endswith("-100-not-ready")
    assert second.name.endswith("-r01")
    identity = json.loads((first / "manifest.json").read_text())["run_identity"]
    assert identity == {"timestamp": "20260901T120000Z", "topology": "C1F1/100000",
                        "depth": "100", "pass": "not-ready"}


def test_cohort_requires_its_own_authenticated_dictionary_authority() -> None:
    topology = MatrixTopology.from_id("C1F1/100000")
    with pytest.raises(Exception, match="dictionary authority"):
        MethodMatrixSimulator(topology, methods=("ZSTD_COHORT",), cohort_dictionary=b"dict")
    dictionary = b"immutable cohort dictionary"
    simulator = MethodMatrixSimulator(
        topology, methods=("ZSTD_COHORT",), cohort_dictionary=dictionary,
        cohort_authority={"status": "READY", "construction": "fixture-authority-v1",
                          "sha256": __import__("hashlib").sha256(dictionary).hexdigest()})
    with pytest.raises(Exception, match="native authority"):
        simulator.run([Occurrence(0, b"payload")])
    assert repeat_full_state_contract("ZSTD_COHORT")["survives"] is True


@pytest.mark.parametrize("methods", [("RAW_II",), ("ZSTD_TU",), ("P29", "GRZ_RESIDUAL")])
def test_subset_core_completion_never_claims_full_matrix(methods: tuple[str, ...]) -> None:
    result = MethodMatrixSimulator(MatrixTopology.from_id("C1F1/100000"),
                                   methods=methods).run([Occurrence(0, b"payload")])
    completion = result["core_completion"]
    assert completion["status"] != "COMPLETED"
    assert set(completion["missing_core_methods"]) == set(CORE_METHODS) - set(methods)


def test_libbsc_archive_provenance_reproduces_pinned_hash() -> None:
    authority = _libbsc_authority(Path.cwd())
    archive = authority["archive"]
    command = str(archive["reproducible_from"]).split()
    result = subprocess.run(command, cwd="/tanksmall/scratch/ictmp/libbsc-issue16",
                            check=True, stdout=subprocess.PIPE)
    assert hashlib.sha256(result.stdout).hexdigest() == archive["sha256"]


def test_repeat_full_only_carries_declared_relationship_state() -> None:
    assert repeat_full_state_contract("RAW_II")["fields"] == []
    assert repeat_full_state_contract("ZSTD_TU")["fields"] == []
    assert "committed_raw_prefix_descriptor" in repeat_full_state_contract("ZSTD_ROUTE")["fields"]


def test_native_repeat_full_carries_state_with_changed_assignment_map(tmp_path: Path) -> None:
    topology = MatrixTopology.from_id("C1F20/40")

    def assignment_row(index: int, relationship: int) -> dict[str, object]:
        return {"ordinal": index, "global_slot": relationship * 2,
                "f_relationship": relationship, "per_f_slot": 0,
                "dispatch_order": index, "authority_dispatch_order": index,
                "authority_worker": relationship, "authority_slot": 0,
                "authority_build": index // 2, "authority_logical": index}

    paths = []
    for index, payload in enumerate((b"a" * 32, b"b" * 32, b"c" * 32, b"d" * 32)):
        path = tmp_path / f"{index}.ii"
        path.write_bytes(payload)
        paths.append(path)

    def occurrence(index: int) -> Occurrence:
        raw = paths[index].read_bytes()
        return Occurrence(index, None, source_path=str(paths[index]),
                          source_sha256=hashlib.sha256(raw).hexdigest(),
                          source_relative=paths[index].name)

    predecessor = [occurrence(0), occurrence(1)]
    measured = [occurrence(2), occurrence(3)]
    predecessor_authority = {"status": "READY", "topology": "C1F20/40",
        "selected_count": 2, "rows": [assignment_row(0, 0), assignment_row(1, 0)]}
    measured_authority = {"status": "READY", "topology": "C1F20/40",
        "selected_count": 2, "rows": [assignment_row(0, 1), assignment_row(1, 0)]}
    empty_descriptor = {"schema": simulator_module.PREFIX_DESCRIPTOR_SCHEMA, "bytes": 0,
                        "digest128": simulator_module._digest128(b"")}
    prior = {"relationships": {"ZSTD_ROUTE": {
        "C0|F0": {"committed_raw_prefix_descriptor": empty_descriptor,
                  "_runtime_history": b"", "next_rel_seq": 0},
        "C0|F1": {"committed_raw_prefix_descriptor": empty_descriptor,
                  "_runtime_history": b"", "next_rel_seq": 0}}}}
    result = MethodMatrixSimulator(
        topology, methods=("ZSTD_ROUTE",), assignment_authority=measured_authority).run(
            measured, repeat_full=True, prior_state=prior,
            predecessor_occurrences=predecessor,
            predecessor_assignment_authority=predecessor_authority)
    assert result["status"] == "PARTIAL_NOT_READY"
    assert result["core_completion"]["status"] == "INCOMPLETE_REQUESTED_SUBSET"
    assert [row["product_transaction"]["tu_seq"] for row in result["rows"]] == [0, 2]
    assert all(row["wire_witnessed"] is True for row in result["rows"])
    assert all(row["product_transaction"]["route_identity"] ==
               f"{row['relationship_key'][0]}->{row['relationship_key'][1]}"
               for row in result["rows"])
    assert all(row["product_transaction"]["history_nonce"] == 1
               for row in result["rows"])


@pytest.mark.parametrize("topology_id", ("C1F1/100000", "C1F20/40"))
@pytest.mark.parametrize("method", ("ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"))
def test_stateful_native_profiles_carry_three_tu_successor(
        tmp_path: Path, topology_id: str, method: str) -> None:
    topology = MatrixTopology.from_id(topology_id)
    paths = []
    for index in range(6):
        path = tmp_path / f"{topology_id.replace('/', '-')}-{index}.ii"
        path.write_bytes(f"state-{index}\n".encode())
        paths.append(path)

    def occurrence(index: int) -> Occurrence:
        raw = paths[index].read_bytes()
        return Occurrence(index, None, source_path=str(paths[index]), source_relative=paths[index].name,
                          source_sha256=hashlib.sha256(raw).hexdigest(),
                          source_digest128=simulator_module._digest128(raw))

    def authority(relations: list[int]) -> dict[str, object]:
        return {"status": "READY", "topology": topology_id, "selected_count": 3,
                "rows": [{"ordinal": index, "global_slot": relation * (2 if topology_id == "C1F20/40" else 100000),
                           "f_relationship": relation, "per_f_slot": 0,
                           "dispatch_order": index, "authority_dispatch_order": index}
                          for index, relation in enumerate(relations)]}

    predecessor = [occurrence(index) for index in range(3)]
    measured = [occurrence(index) for index in range(3, 6)]
    first = authority([0, 0, 0])
    second = authority([1, 0, 1]) if topology_id == "C1F20/40" else authority([0, 0, 0])
    output = simulator_module._native_batch(
        measured, topology, second, method,
        predecessor_occurrences=predecessor, predecessor_assignment=first)
    assert [row["tu_seq"] for row in output.values()] == (
        [3, 4, 5] if topology_id == "C1F1/100000" else [0, 3, 1])
    assert all(row["committed"] is True for row in output.values())


def test_synthetic_codec_is_not_claimed_as_wire_evidence(tmp_path: Path) -> None:
    experiment = MethodMatrixSimulator(
        MatrixTopology.from_id("C1F1/100000"), methods=("ZSTD_TU",)).run(
            [Occurrence(0, b"payload")], output_root=tmp_path,
            timestamp="20260901T120000Z", depth="100")
    row = json.loads((experiment / "occurrences.jsonl").read_text())
    summary = json.loads((experiment / "summary.json").read_text())
    assert row["measurement_scope"] == "native_codec_witnessed"
    assert row["wire_witnessed"] is False
    assert "product_transaction" not in row
    assert summary["totals"]["ZSTD_TU"]["wire_witnessed"] is False
