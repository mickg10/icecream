from __future__ import annotations

import hashlib
import json
import copy
import os
import stat
import subprocess
import sys
from types import SimpleNamespace
from pathlib import Path

import pytest
import s8_method_matrix_simulator as simulator_module

from s8_method_matrix_simulator import (
    CORE_METHODS,
    DEFAULT_CLI_METHODS,
    MatrixError,
    MatrixTopology,
    MethodMatrixSimulator,
    NotReady,
    Occurrence,
    assign_relationships,
    repeat_full_state_contract,
    _authenticated_assignment,
    firefox_occurrences,
    verify_experiment,
    _libbsc_authority,
    _native_producer_identity,
    _native_batch_timeout_seconds,
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


def test_native_producer_identity_is_compact_and_fail_closed() -> None:
    receipt = {"schema": "icecream-p50sim-build-v1",
               "source": {"head": "a" * 40, "tree": "b" * 40},
               "binary": {"path": "/product/.p50sim.bin", "bytes": 12,
                           "sha256": "c" * 64}}
    facts = {"path": "/product/.p50sim-build.json", "bytes": 34,
             "sha256": "d" * 64}
    identity = _native_producer_identity(receipt, facts)
    assert identity == {
        "schema": simulator_module.PRODUCER_IDENTITY_SCHEMA,
        "source": {"head": "a" * 40, "tree": "b" * 40},
        "p50sim_binary": {"path": "/product/.p50sim.bin", "bytes": 12,
                           "sha256": "c" * 64},
        "build_receipt": {"schema": "icecream-p50sim-build-v1",
                           "path": "/product/.p50sim-build.json", "bytes": 34,
                           "sha256": "d" * 64}}
    malformed = dict(receipt)
    malformed["source"] = {"head": "not-a-head", "tree": "b" * 40}
    assert _native_producer_identity(malformed, facts) is None


def _native_receipt_fixture(root: Path) -> dict[str, object]:
    return {
        "schema": "icecream-p50sim-build-v1",
        "source": {"root": str(root), "head": "a" * 40,
                   "tree": "b" * 40, "tracked_clean": True},
        "binary": {"path": str(root / "cache/sim/.p50sim.bin"),
                   "sha256": "e" * 64, "bytes": 1},
        "inputs": {name: {"path": str(root / name), "sha256": "e" * 64,
                          "bytes": 1}
                   for name in ("p50sim_source", "config_h", "cache_makefile",
                                "services_makefile")},
        "libbsc": None,
        "configuration": {"with_libbsc": 0, "make_mode": "direct_sources",
                           "dependency_root": "/deps", "compiler_path": "/bin/c++",
                           "compiler_version": "compiler"},
    }


@pytest.mark.parametrize(("field", "replacement"), (
    pytest.param("source", "bad", id="source-string"),
    pytest.param("source", [], id="source-list"),
    pytest.param("source", None, id="source-null"),
    pytest.param("binary", "bad", id="binary-string"),
    pytest.param("binary", [], id="binary-list"),
    pytest.param("binary", None, id="binary-null"),
    pytest.param("inputs", "bad", id="inputs-string"),
    pytest.param("inputs", [], id="inputs-list"),
    pytest.param("inputs", None, id="inputs-null"),
    pytest.param("configuration", "bad", id="configuration-string"),
    pytest.param("configuration", [], id="configuration-list"),
    pytest.param("configuration", None, id="configuration-null"),
    pytest.param("configuration", {}, id="configuration-empty-map"),
    pytest.param("libbsc", "bad", id="libbsc-string"),
    pytest.param("libbsc", [], id="libbsc-list"),
    pytest.param("libbsc", {}, id="libbsc-empty-map"),
))
def test_native_receipt_nested_mutations_fail_closed_without_exception(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch, field: str,
        replacement: object) -> None:
    root = tmp_path / "root"
    receipt_path = root / "cache/sim/.p50sim-build.json"
    valid = _native_receipt_fixture(root)
    mutated = copy.deepcopy(valid)
    mutated[field] = replacement
    raw = simulator_module._canonical(mutated)
    facts = {"path": str(receipt_path), "bytes": len(raw),
             "sha256": simulator_module._sha256(raw)}

    def fake_private_bytes(path: Path, _label: str):
        assert path == receipt_path
        return raw, facts

    monkeypatch.setattr(simulator_module, "_private_bytes", fake_private_bytes)
    monkeypatch.setattr(simulator_module, "_private_digest",
                        lambda path, _label: {"path": str(path), "bytes": 1,
                                              "sha256": "e" * 64})
    monkeypatch.setattr(simulator_module.subprocess, "check_output",
                        lambda command, **_kwargs: ("a" * 40 if command[-1] == "HEAD"
                                                     else "b" * 40).encode())
    receipt, returned_facts = simulator_module._native_receipt(root)
    assert receipt is None
    assert returned_facts == facts


@pytest.mark.parametrize("field", ("source", "binary", "inputs", "configuration", "libbsc"))
def test_native_receipt_nested_deletion_fails_closed(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch, field: str) -> None:
    root = tmp_path / "root"
    receipt_path = root / "cache/sim/.p50sim-build.json"
    mutated = _native_receipt_fixture(root)
    del mutated[field]
    raw = simulator_module._canonical(mutated)
    facts = {"path": str(receipt_path), "bytes": len(raw),
             "sha256": simulator_module._sha256(raw)}
    monkeypatch.setattr(simulator_module, "_private_bytes",
                        lambda path, _label: (raw, facts) if path == receipt_path
                        else pytest.fail("unexpected authority read"))
    receipt, returned_facts = simulator_module._native_receipt(root)
    assert receipt is None
    assert returned_facts == facts


@pytest.mark.parametrize("method", ("ZSTD_TU", "P29", "GRZ_RESIDUAL"))
def test_method_authority_malformed_receipt_is_not_ready(
        monkeypatch: pytest.MonkeyPatch, method: str) -> None:
    monkeypatch.setattr(simulator_module, "_native_receipt",
                        lambda _root: (None, {"available": False, "sha256": None}))
    authority = simulator_module.method_authority(method)
    assert authority["status"] == "NOT_READY"
    assert authority.get("producer_identity") is None


def test_method_authority_consumes_fail_closed_receipt_parser(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    fake_root = tmp_path / "root"
    receipt_path = fake_root / "cache/sim/.p50sim-build.json"
    receipt = _native_receipt_fixture(fake_root)
    receipt["source"] = "malformed-source"
    raw = simulator_module._canonical(receipt)
    facts = {"path": str(receipt_path), "bytes": len(raw),
             "sha256": simulator_module._sha256(raw)}
    original_private_bytes = simulator_module._private_bytes

    def fake_private_bytes(path: Path, label: str):
        if path == receipt_path:
            return raw, facts
        return original_private_bytes(path, label)

    monkeypatch.setattr(simulator_module, "_private_bytes", fake_private_bytes)
    original_native_receipt = simulator_module._native_receipt
    monkeypatch.setattr(simulator_module, "_native_receipt",
                        lambda _root: original_native_receipt(fake_root))
    authority = simulator_module.method_authority("P29")
    assert authority["status"] == "NOT_READY"
    assert authority.get("producer_identity") is None


def test_persisted_experiment_retains_native_producer_identity(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    identity = {"schema": simulator_module.PRODUCER_IDENTITY_SCHEMA,
                "source": {"head": "a" * 40, "tree": "b" * 40},
                "p50sim_binary": {"path": "/product/.p50sim.bin", "bytes": 12,
                                   "sha256": "c" * 64},
                "build_receipt": {"schema": "icecream-p50sim-build-v1",
                                   "path": "/product/.p50sim-build.json", "bytes": 34,
                                   "sha256": "d" * 64}}
    monkeypatch.setattr(simulator_module, "method_authority",
                        lambda _method: {"status": "NOT_READY",
                                         "reason": "fixture",
                                         "producer_identity": identity})
    experiment = MethodMatrixSimulator(
        MatrixTopology.from_id("C1F1/100000"), methods=("ZSTD_ROUTE",)).run(
            [Occurrence(0, b"payload")], output_root=tmp_path,
            timestamp="20260902T120000Z")
    manifest = json.loads((experiment / "manifest.json").read_text())
    assert manifest["producer_identity"] == {"ZSTD_ROUTE": identity}


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
            "tu_seq": ordinal, "rel_seq": ordinal, "state_before_digest": "a" * 32,
            "state_digest": "b" * 32, "transaction_digest": "d" * 32,
            "encoded_source_bytes": 1 << 20, "simulator_execution_ns": 1}}
        rows.append(matrix._run_occurrence(
            None, Occurrence(ordinal, b"x" * (1 << 20)),
            {"relationship_key": ["C0", "F0"], "relationship_index": 0,
             "slot": 0, "global_slot": 0, "authority_tu_seq": ordinal}, "ZSTD_ROUTE", state,
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
        "tu_seq": 0, "rel_seq": 0, "state_before_digest": "a" * 32,
        "state_digest": "b" * 32, "transaction_digest": "d" * 32,
        "encoded_source_bytes": 1, "simulator_execution_ns": 1}}
    matrix._run_occurrence(
        None, Occurrence(0, b"candidate", commit=False),
        {"relationship_key": ["C0", "F0"], "relationship_index": 0,
         "slot": 0, "global_slot": 0, "authority_tu_seq": 0}, "ZSTD_ROUTE", state,
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
        "tu_seq": ordinal, "rel_seq": ordinal, "state_before_digest": "a" * 32,
        "state_digest": "b" * 32, "transaction_digest": "d" * 32,
        "encoded_source_bytes": 1, "simulator_execution_ns": 1}}
    rows.append(matrix._run_occurrence(
        None, s.Occurrence(ordinal, b"x" * (1 << 20)),
        {"relationship_key": ["C0", "F0"], "relationship_index": 0,
         "slot": 0, "global_slot": 0, "authority_tu_seq": ordinal}, "ZSTD_ROUTE", state,
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
                      "authority_dispatch_order": 0, "authority_tu_seq": 0}]}
    _fake_native_batch_runner(monkeypatch)
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


@pytest.mark.parametrize(("transaction_count", "expected_timeout"), (
    pytest.param(3, 600, id="three"),
    pytest.param(100, 1000, id="depth100"),
    pytest.param(200, 2000, id="depth200"),
    pytest.param(2498, 24980, id="full1"),
    pytest.param(4996, 49960, id="full2"),
))
def test_native_batch_timeout_scales_with_authenticated_transaction_count(
        transaction_count: int, expected_timeout: int) -> None:
    assert _native_batch_timeout_seconds(transaction_count) == expected_timeout


def test_native_batch_timeout_is_fail_closed_and_includes_full2_segments(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    source_paths = []
    for index in range(6):
        source = tmp_path / f"unit-{index}.ii"
        source.write_bytes(f"native payload {index}".encode())
        source_paths.append(source)

    def occurrence(index: int) -> Occurrence:
        raw = source_paths[index].read_bytes()
        return Occurrence(index, None, source_path=str(source_paths[index]),
                          source_relative=source_paths[index].name,
                          source_sha256=hashlib.sha256(raw).hexdigest(),
                          source_digest128=simulator_module._digest128(raw))

    topology = MatrixTopology.from_id("C1F20/40")

    def assignment() -> dict[str, object]:
        return {"status": "READY", "topology": topology.topology_id,
                "selected_count": 3,
                "rows": [{"ordinal": index, "global_slot": 0,
                           "f_relationship": 0, "per_f_slot": 0,
                           "dispatch_order": index,
                           "authority_dispatch_order": index}
                          for index in range(3)]}

    binary = (Path(simulator_module.__file__).resolve().parents[2] /
              "cache" / "sim" / ".p50sim.bin")
    original_is_file = Path.is_file

    def is_file(path: Path) -> bool:
        return path == binary or original_is_file(path)

    def timeout(*args: object, **kwargs: object) -> object:
        assert kwargs["timeout"] == 600
        command = args[0] if args else kwargs["args"]
        raise subprocess.TimeoutExpired(command, kwargs["timeout"])

    monkeypatch.setattr(Path, "is_file", is_file)
    monkeypatch.setattr(simulator_module.subprocess, "run", timeout)
    with pytest.raises(NotReady,
                       match=r"method=P29 transactions=6 timeout=600s"):
        simulator_module._native_batch(
            [occurrence(index) for index in range(3, 6)], topology, assignment(), "P29",
            predecessor_occurrences=[occurrence(index) for index in range(3)],
            predecessor_assignment=assignment())


def _partial_native_inputs(tmp_path: Path) -> tuple[list[Occurrence], MatrixTopology,
                                                     dict[str, object]]:
    topology = MatrixTopology.from_id("C1F20/40")
    occurrences: list[Occurrence] = []
    rows: list[dict[str, object]] = []
    for index, relation in enumerate((0, 1)):
        source = tmp_path / f"partial-{index}.ii"
        source.write_bytes(f"partial payload {index}".encode())
        raw = source.read_bytes()
        occurrences.append(Occurrence(
            index, None, source_path=str(source), source_relative=source.name,
            source_sha256=hashlib.sha256(raw).hexdigest(),
            source_digest128=simulator_module._digest128(raw)))
        rows.append({"ordinal": index, "global_slot": relation * 2,
                     "f_relationship": relation, "per_f_slot": 0,
                     "dispatch_order": index, "authority_dispatch_order": index,
                     "authority_tu_seq": index, "authority_rel_seq": 0})
    return occurrences, topology, {"status": "READY", "topology": topology.topology_id,
                                  "selected_count": len(rows), "rows": rows}


def _failure_experiment(root: Path) -> Path:
    experiments = sorted(root.glob("*-native-failure-*"))
    assert len(experiments) == 1
    return experiments[0]


def test_native_batch_preserves_valid_prefix_when_child_exits_nonzero(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    occurrences, topology, assignment = _partial_native_inputs(tmp_path)
    _fake_native_batch_runner(monkeypatch)
    original_run = simulator_module.subprocess.run

    def failed_after_prefix(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        original = original_run(command, **kwargs)
        output = Path(command[command.index("--batch-output") + 1])
        lines = output.read_text().splitlines()
        output.write_text("\n".join(lines) + "\n")
        return subprocess.CompletedProcess(command, 9, b"", b"worker stopped after prefix" + b"x" * 10000)

    monkeypatch.setattr(simulator_module.subprocess, "run", failed_after_prefix)
    with pytest.raises(NotReady, match=r"exit:returncode=9"):
        simulator_module._native_batch(occurrences, topology, assignment, "P29",
                                       failure_output_root=tmp_path,
                                       failure_authority={"status": "READY",
                                                          "binary": "authenticated"})
    experiment = _failure_experiment(tmp_path)
    preserved = (experiment / "native-output.jsonl").read_text().splitlines()
    assert len(preserved) == 2
    failure = json.loads((experiment / "native-failure.json").read_text())
    assert failure["status"] == "NOT_READY"
    assert failure["outcome"] == "FAILED"
    assert failure["terminal"]["kind"] == "exit"
    assert failure["terminal"]["returncode"] == 9
    assert failure["terminal"]["stderr_classification"] == "stderr"
    assert failure["metrics"]["valid_rows"] == 2
    assert failure["metrics"]["simulator_execution_ns"] == 2
    assert failure["metrics"]["valid_rows_by_segment"] == {"full-1": 2, "full-2": 0}
    assert json.loads((experiment / "manifest.json").read_text())["authority"]["P29"] == {
        "status": "READY", "binary": "authenticated"}
    assert len((experiment / "native-stderr.txt").read_bytes()) == 4096
    assert json.loads((experiment / "manifest.json").read_text())["failure_method_status"] == {
        "P29": "NOT_READY"}
    assert json.loads((experiment / "summary.json").read_text())["status"] == "NOT_READY"
    assert verify_experiment(experiment)["status"] == "PASS"


def test_native_batch_real_child_diagnostics_are_bounded(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    occurrences, topology, assignment = _partial_native_inputs(tmp_path)
    binary = (Path(simulator_module.__file__).resolve().parents[2] /
              "cache" / "sim" / ".p50sim.bin")
    original_is_file = Path.is_file
    monkeypatch.setattr(Path, "is_file", lambda path: (
        True if path == binary else original_is_file(path)))
    original_run = simulator_module.subprocess.run

    def real_diagnostics(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        assert kwargs["stdout"] is subprocess.DEVNULL
        assert kwargs["stderr"] is not subprocess.PIPE
        code = "import sys; sys.stderr.write('d' * 1000000); sys.exit(17)"
        return original_run([sys.executable, "-c", code], env=kwargs["env"], check=False,
                            stdout=kwargs["stdout"], stderr=kwargs["stderr"],
                            timeout=kwargs["timeout"])

    monkeypatch.setattr(simulator_module.subprocess, "run", real_diagnostics)
    with pytest.raises(NotReady, match=r"exit:returncode=17"):
        simulator_module._native_batch(occurrences, topology, assignment, "P29",
                                       failure_output_root=tmp_path)
    experiment = _failure_experiment(tmp_path)
    assert len((experiment / "native-stderr.txt").read_bytes()) == 4096
    failure = json.loads((experiment / "native-failure.json").read_text())
    assert failure["terminal"]["kind"] == "exit"
    assert failure["terminal"]["returncode"] == 17
    assert failure["terminal"]["stderr_bytes"] == 4096
    assert failure["terminal"]["stderr_limit"] == 4096
    assert failure["terminal"]["stderr_truncated"] is True
    assert verify_experiment(experiment)["status"] == "PASS"


def test_native_batch_preserves_prefix_and_records_malformed_last_row(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    occurrences, topology, assignment = _partial_native_inputs(tmp_path)
    _fake_native_batch_runner(monkeypatch)
    original_run = simulator_module.subprocess.run

    def malformed_last(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        original = original_run(command, **kwargs)
        output = Path(command[command.index("--batch-output") + 1])
        first = output.read_text().splitlines()[0]
        output.write_text(first + "\n{\"schema\":\n")
        return subprocess.CompletedProcess(command, 0, b"", b"")

    monkeypatch.setattr(simulator_module.subprocess, "run", malformed_last)
    with pytest.raises(NotReady, match="malformed_output"):
        simulator_module._native_batch(occurrences, topology, assignment, "P29",
                                       failure_output_root=tmp_path)
    experiment = _failure_experiment(tmp_path)
    assert len((experiment / "native-output.jsonl").read_text().splitlines()) == 1
    failure = json.loads((experiment / "native-failure.json").read_text())
    assert failure["terminal"]["kind"] == "output"
    assert failure["terminal"]["output_classification"].startswith("malformed_output:line=2")
    assert failure["metrics"]["valid_rows"] == 1
    assert verify_experiment(experiment)["status"] == "PASS"


def test_native_batch_preserves_prefix_on_timeout_and_bounds_stderr(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    occurrences, topology, assignment = _partial_native_inputs(tmp_path)
    _fake_native_batch_runner(monkeypatch)
    original_run = simulator_module.subprocess.run

    def timed_out(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        original_run(command, **kwargs)
        output = Path(command[command.index("--batch-output") + 1])
        output.write_text(output.read_text().splitlines()[0] + "\n")
        raise subprocess.TimeoutExpired(command, kwargs["timeout"], stderr=b"timeout worker")

    monkeypatch.setattr(simulator_module.subprocess, "run", timed_out)
    with pytest.raises(NotReady, match=r"timeout:method=P29 transactions=2 timeout=600s"):
        simulator_module._native_batch(occurrences, topology, assignment, "P29",
                                       failure_output_root=tmp_path)
    experiment = _failure_experiment(tmp_path)
    assert len((experiment / "native-output.jsonl").read_text().splitlines()) == 1
    failure = json.loads((experiment / "native-failure.json").read_text())
    assert failure["terminal"]["kind"] == "timeout"
    assert failure["terminal"]["stderr_classification"] == "timeout"
    assert verify_experiment(experiment)["status"] == "PASS"


def test_native_batch_failure_binds_full2_identity_and_separate_metrics(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    (tmp_path / "pre").mkdir()
    predecessor, topology, predecessor_assignment = _partial_native_inputs(tmp_path / "pre")
    # Recreate the helper's inputs under the already-created parent and give
    # the measured segment the authenticated continuation TU/REL markers.
    (tmp_path / "measured").mkdir()
    measured, _topology, measured_assignment = _partial_native_inputs(tmp_path / "measured")
    for index, row in enumerate(measured_assignment["rows"]):
        row["authority_tu_seq"] = index + 2
        row["authority_rel_seq"] = 1
    _fake_native_batch_runner(monkeypatch)
    original_run = simulator_module.subprocess.run

    def failed_full2(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        original_run(command, **kwargs)
        return subprocess.CompletedProcess(command, 4, b"", b"full2 stopped")

    monkeypatch.setattr(simulator_module.subprocess, "run", failed_full2)
    with pytest.raises(NotReady, match=r"exit:returncode=4"):
        simulator_module._native_batch(
            measured, topology, measured_assignment, "P29",
            predecessor_occurrences=predecessor,
            predecessor_assignment=predecessor_assignment,
            failure_output_root=tmp_path, failure_depth="state-carrying-full-2",
            failure_pass="pass-2", failure_timestamp="20260902T010203Z",
            failure_authority={"status": "READY", "receipt": "source-bound"})
    experiment = _failure_experiment(tmp_path)
    manifest = json.loads((experiment / "manifest.json").read_text())
    assert manifest["run_identity"] == {
        "timestamp": "20260902T010203Z", "topology": "C1F20/40",
        "depth": "state-carrying-full-2", "pass": "pass-2"}
    failure = json.loads((experiment / "native-failure.json").read_text())
    assert failure["metrics"]["valid_rows_by_segment"] == {"full-1": 2, "full-2": 2}
    assert failure["metrics"]["segments"]["full-1"]["simulator_execution_ns"] == 2
    assert failure["metrics"]["segments"]["full-2"]["simulator_execution_ns"] == 2
    assert manifest["authority"]["P29"] == {"status": "READY", "receipt": "source-bound"}
    assert json.loads((experiment / "summary.json").read_text())["status"] == "NOT_READY"
    assert verify_experiment(experiment)["status"] == "PASS"


def test_native_batch_zero_exit_missing_output_persists_typed_failure(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    occurrences, topology, assignment = _partial_native_inputs(tmp_path)
    _fake_native_batch_runner(monkeypatch)
    original_run = simulator_module.subprocess.run

    def missing_output(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        result = original_run(command, **kwargs)
        Path(command[command.index("--batch-output") + 1]).unlink()
        return result

    monkeypatch.setattr(simulator_module.subprocess, "run", missing_output)
    with pytest.raises(NotReady, match="output:stderr=empty"):
        simulator_module._native_batch(occurrences, topology, assignment, "P29",
                                       failure_output_root=tmp_path)
    experiment = _failure_experiment(tmp_path)
    failure = json.loads((experiment / "native-failure.json").read_text())
    assert failure["terminal"]["kind"] == "output"
    assert failure["terminal"]["output_classification"].startswith("output_unavailable:")
    assert failure["metrics"]["valid_rows"] == 0
    assert verify_experiment(experiment)["status"] == "PASS"


def test_native_batch_success_does_not_create_failure_artifact(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    occurrences, topology, assignment = _partial_native_inputs(tmp_path)
    _fake_native_batch_runner(monkeypatch)
    result = simulator_module._native_batch(occurrences, topology, assignment, "P29",
                                            failure_output_root=tmp_path)
    assert len(result) == 2
    assert not list(tmp_path.glob("*-native-failure-*"))


def test_native_route_prefix_key_set_is_exact_and_nonroute_has_none() -> None:
    route = {"schema": "icecream-p50sim-batch-v1",
             "committed_raw_prefix_before_descriptor": {},
             "committed_raw_prefix_descriptor": {}}
    simulator_module._validate_native_prefix_keys(route, "ZSTD_ROUTE")
    for key in ("committed_raw_prefix_before", "committed_raw_prefix_extra"):
        forged = dict(route)
        forged[key] = {}
        with pytest.raises(MatrixError, match="descriptor keys invalid"):
            simulator_module._validate_native_prefix_keys(forged, "ZSTD_ROUTE")
    missing = dict(route)
    del missing["committed_raw_prefix_descriptor"]
    with pytest.raises(MatrixError, match="descriptor keys invalid"):
        simulator_module._validate_native_prefix_keys(missing, "ZSTD_ROUTE")
    with pytest.raises(MatrixError, match="prefix body forbidden"):
        simulator_module._validate_native_prefix_keys(
            {"committed_raw_prefix_descriptor": {}}, "P29")


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
    assert "c_authorities.native_next_tu_seq" in repeat_full_state_contract("ZSTD_ROUTE")["fields"]


def _fake_native_batch_runner(monkeypatch: pytest.MonkeyPatch) -> None:
    """Provide a deterministic product-output seam without building p50sim."""
    original_is_file = Path.is_file
    monkeypatch.setattr(Path, "is_file", lambda path: (
        True if path.name == ".p50sim.bin" else original_is_file(path)))

    def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        output = Path(command[command.index("--batch-output") + 1])
        segments: list[tuple[str, Path, Path]] = [("full-1",
            Path(command[command.index("--batch-manifest") + 1]),
            Path(command[command.index("--batch-assignment-map") + 1]))]
        if "--batch-manifest-2" in command:
            segments.append(("full-2",
                Path(command[command.index("--batch-manifest-2") + 1]),
                Path(command[command.index("--batch-assignment-map-2") + 1])))
        state: dict[int, str] = {}
        next_rel: dict[int, int] = {}
        global_tu = 0
        lines: list[str] = []
        for segment, manifest, mapping in segments:
            paths = manifest.read_text().splitlines()
            mapping_lines = mapping.read_text().splitlines()
            cardinality = int(mapping_lines[0].split("=", 1)[1])
            assignment_rows = mapping_lines[1:]
            for index, (path_text, relation_text) in enumerate(zip(paths, assignment_rows)):
                raw = Path(path_text).read_bytes()
                relation = int(relation_text)
                rel_seq = next_rel.get(relation, 0)
                next_rel[relation] = rel_seq + 1
                before = state.get(relation, "a" * 32)
                after = ("b" if relation == 0 else "c") * 32
                state[relation] = after
                item = {
                    "schema": "icecream-p50sim-batch-v1", "segment": segment,
                    "profile": kwargs["env"]["ICECC_P50_PROFILE"],
                    "relationship_id": f"c1f{cardinality}-r{relation:02d}", "rel_seq": rel_seq,
                    "tu_index": index, "tu_seq": global_tu,
                    "raw_bytes": len(raw), "raw_digest": simulator_module._digest128(raw),
                    "encoded_source_bytes": 1, "c_to_f_bytes": 1, "f_to_c_bytes": 1,
                    "simulator_execution_ns": 1, "state_before_digest": before,
                    "state_digest": after, "transaction_digest": "d" * 32,
                    "committed": True}
                if kwargs["env"]["ICECC_P50_PROFILE"] == "ZSTD_ROUTE":
                    empty = {"schema": simulator_module.PREFIX_DESCRIPTOR_SCHEMA,
                             "bytes": 0, "digest128": simulator_module._digest128(b"")}
                    item["committed_raw_prefix_before_descriptor"] = empty
                    item["committed_raw_prefix_descriptor"] = empty
                lines.append(json.dumps(item))
                global_tu += 1
        output.write_text("\n".join(lines) + "\n")
        return subprocess.CompletedProcess(command, 0, b"", b"")

    monkeypatch.setattr(simulator_module.subprocess, "run", run)


def test_native_batch_requires_one_global_tu_seq_across_interleaved_relationships(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    topology = MatrixTopology.from_id("C1F20/40")
    paths = []
    for index in range(4):
        path = tmp_path / f"{index}.ii"
        path.write_bytes(f"payload-{index}".encode())
        paths.append(path)
    occurrences = [Occurrence(index, None, source_path=str(path), source_relative=path.name,
                               source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                               source_digest128=simulator_module._digest128(path.read_bytes()))
                   for index, path in enumerate(paths)]
    rows = [{"ordinal": index, "global_slot": relation * 2,
             "f_relationship": relation, "per_f_slot": 0,
             "dispatch_order": index, "authority_dispatch_order": index,
             "authority_tu_seq": index, "authority_rel_seq": rel}
            for index, (relation, rel) in enumerate(((0, 0), (1, 0), (0, 1), (1, 1)))]
    assignment = {"status": "READY", "topology": topology.topology_id,
                  "selected_count": 4, "rows": rows}
    _fake_native_batch_runner(monkeypatch)
    output = simulator_module._native_batch(occurrences, topology, assignment, "P29")
    assert [item["tu_seq"] for item in output.values()] == [0, 1, 2, 3]
    assert [item["_native_rel_seq"] for item in output.values()] == [0, 0, 1, 1]
    assert [item["_native_next_tu_seq"] for item in output.values()] == [1, 2, 3, 4]


def test_native_batch_binds_nonzero_tu_seq_to_assignment_authority(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    topology = MatrixTopology.from_id("C1F20/40")
    paths = []
    for index in range(2):
        path = tmp_path / f"nonzero-{index}.ii"
        path.write_bytes(f"payload-{index}".encode())
        paths.append(path)
    occurrences = [Occurrence(index + 100, None, source_path=str(path), source_relative=path.name,
                               source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                               source_digest128=simulator_module._digest128(path.read_bytes()))
                   for index, path in enumerate(paths)]
    rows = [{"ordinal": index, "global_slot": relation * 2,
             "f_relationship": relation, "per_f_slot": 0,
             "dispatch_order": 100 + index, "authority_dispatch_order": 100 + index,
             "authority_tu_seq": 100 + index, "authority_rel_seq": 0}
            for index, relation in enumerate((0, 1))]
    assignment = {"status": "READY", "topology": topology.topology_id,
                  "selected_count": 2, "rows": rows}
    _fake_native_batch_runner(monkeypatch)
    base_runner = simulator_module.subprocess.run

    def shifted_runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        result = base_runner(command, **kwargs)
        output = Path(command[command.index("--batch-output") + 1])
        lines = []
        for line in output.read_text().splitlines():
            item = json.loads(line)
            item["tu_seq"] += 100
            lines.append(json.dumps(item))
        output.write_text("\n".join(lines) + "\n")
        return result

    monkeypatch.setattr(simulator_module.subprocess, "run", shifted_runner)
    output = simulator_module._native_batch(occurrences, topology, assignment, "P29")
    assert [item["tu_seq"] for item in output.values()] == [100, 101]

    def mismatched_runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        result = shifted_runner(command, **kwargs)
        output = Path(command[command.index("--batch-output") + 1])
        item = json.loads(output.read_text().splitlines()[0])
        item["tu_seq"] += 1
        output.write_text(json.dumps(item) + "\n")
        return result

    monkeypatch.setattr(simulator_module.subprocess, "run", mismatched_runner)
    with pytest.raises(MatrixError, match="TU sequence"):
        simulator_module._native_batch(occurrences, topology, assignment, "P29")


def test_native_full2_keeps_global_next_tu_separate_from_route_rel_seq(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    topology = MatrixTopology.from_id("C1F20/40")
    paths = []
    for index in range(4):
        path = tmp_path / f"{index}.ii"
        path.write_bytes(f"payload-{index}".encode())
        paths.append(path)
    def occurrence(index: int) -> Occurrence:
        path = paths[index]
        raw = path.read_bytes()
        return Occurrence(index, None, source_path=str(path), source_relative=path.name,
                          source_sha256=hashlib.sha256(raw).hexdigest(),
                          source_digest128=simulator_module._digest128(raw))
    def authority(relations: tuple[tuple[int, int], ...], start: int = 0) -> dict[str, object]:
        return {"status": "READY", "topology": topology.topology_id,
                "selected_count": len(relations), "rows": [
                    {"ordinal": index, "global_slot": relation * 2,
                     "f_relationship": relation, "per_f_slot": 0,
                     "dispatch_order": start + index, "authority_dispatch_order": start + index,
                     "authority_tu_seq": start + index,
                     "authority_rel_seq": rel}
                    for index, (relation, rel) in enumerate(relations)]}
    _fake_native_batch_runner(monkeypatch)
    output = simulator_module._native_batch(
        # The current segment starts on F0 although the predecessor's global
        # last TU was on F1; route-local last TU must not seed C-wide TU_SEQ.
        [occurrence(2), occurrence(3)], topology, authority(((0, 1), (1, 1)), 2), "P29",
        predecessor_occurrences=[occurrence(0), occurrence(1)],
        predecessor_assignment=authority(((0, 0), (1, 0)), 0))
    measured = list(output.values())
    assert [item["tu_seq"] for item in measured] == [2, 3]
    assert [item["_native_rel_seq"] for item in measured] == [1, 1]
    assert [item["_native_next_tu_seq"] for item in measured] == [3, 4]


def test_codec_mode_uses_c_wide_tu_allocator_not_relationship_rel_seq(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    matrix = MethodMatrixSimulator(MatrixTopology.from_id("C1F20/40"),
                                   methods=("ZSTD_TU",))
    state = simulator_module._RelationshipState(("C0", "F0"), next_rel_seq=17)
    assignment = {"authority_tu_seq": 41}
    captured: list[list[str]] = []
    original_lstat = Path.lstat
    original_access = os.access
    original_private_bytes = simulator_module._private_bytes

    def fake_lstat(path: Path) -> object:
        if path.name == ".p50sim.bin":
            return SimpleNamespace(st_mode=stat.S_IFREG, st_nlink=1)
        return original_lstat(path)

    def fake_private_bytes(path: Path, label: str) -> tuple[bytes, dict[str, object]]:
        if path.name == ".p50sim-build.json":
            raw = json.dumps({"schema": "icecream-p50sim-build-v1",
                              "binary": {"path": str(path.parent / ".p50sim.bin"),
                                         "sha256": "e" * 64}}).encode()
            return raw, {"path": str(path), "bytes": len(raw), "sha256": simulator_module._sha256(raw)}
        return original_private_bytes(path, label)

    def fake_run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
        captured.append(command)
        output = Path(command[command.index("--codec-output") + 1])
        output.write_bytes(b"encoded")
        return subprocess.CompletedProcess(command, 0, b"", b"")

    monkeypatch.setattr(Path, "lstat", fake_lstat)
    monkeypatch.setattr(os, "access", lambda path, mode: True
                        if Path(path).name == ".p50sim.bin" else original_access(path, mode))
    monkeypatch.setattr(simulator_module, "_private_bytes", fake_private_bytes)
    monkeypatch.setattr(simulator_module, "_private_digest",
                        lambda path, label: {"path": str(path), "bytes": 0, "sha256": "e" * 64})
    monkeypatch.setattr(simulator_module.subprocess, "run", fake_run)
    matrix._native_codec("ZSTD_TU", b"first", state,
                         authority_tu_seq=assignment["authority_tu_seq"])
    state.next_rel_seq = 18
    assignment["authority_tu_seq"] = 42
    matrix._native_codec("ZSTD_TU", b"second", state,
                         authority_tu_seq=assignment["authority_tu_seq"])
    assert [command[command.index("--codec-tu-seq") + 1] for command in captured] == ["41", "42"]
    assert [command[command.index("--codec-rel-seq") + 1] for command in captured] == ["17", "18"]


def test_native_repeat_full_carries_state_with_changed_assignment_map(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    topology = MatrixTopology.from_id("C1F20/40")

    def assignment_row(index: int, relationship: int, tu_seq: int | None = None,
                       rel_seq: int | None = None) -> dict[str, object]:
        return {"ordinal": index, "global_slot": relationship * 2,
                "f_relationship": relationship, "per_f_slot": 0,
                "dispatch_order": index, "authority_dispatch_order": index,
                "authority_tu_seq": index if tu_seq is None else tu_seq,
                "authority_rel_seq": index if rel_seq is None else rel_seq,
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
        "selected_count": 2, "rows": [assignment_row(0, 1, 2), assignment_row(1, 0, 3, 2)]}
    empty_descriptor = {"schema": simulator_module.PREFIX_DESCRIPTOR_SCHEMA, "bytes": 0,
                        "digest128": simulator_module._digest128(b"")}
    prior = {"c_authorities": {"ZSTD_ROUTE": {"native_next_tu_seq": 2}},
             "relationships": {"ZSTD_ROUTE": {
        "C0|F0": {"committed_raw_prefix_descriptor": empty_descriptor,
                  "_runtime_history": b"", "next_rel_seq": 0},
        "C0|F1": {"committed_raw_prefix_descriptor": empty_descriptor,
                  "_runtime_history": b"", "next_rel_seq": 0}}}}
    matrix = MethodMatrixSimulator(
        topology, methods=("ZSTD_ROUTE",), assignment_authority=measured_authority)
    matrix.authority["ZSTD_ROUTE"]["status"] = "READY"
    _fake_native_batch_runner(monkeypatch)
    result = matrix.run(
            measured, repeat_full=True, prior_state=prior,
            predecessor_occurrences=predecessor,
            predecessor_assignment_authority=predecessor_authority)
    assert result["status"] == "PARTIAL_NOT_READY"
    assert result["core_completion"]["status"] == "INCOMPLETE_REQUESTED_SUBSET"
    assert [row["product_transaction"]["tu_seq"] for row in result["rows"]] == [2, 3]
    assert all(row["wire_witnessed"] is True for row in result["rows"])
    assert all(row["product_transaction"]["route_identity"] ==
               f"{row['relationship_key'][0]}->{row['relationship_key'][1]}"
               for row in result["rows"])
    assert all(row["product_transaction"]["history_nonce"] == 1
               for row in result["rows"])


def test_in_memory_full2_keeps_c_wide_tu_and_route_rel_continuity(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    topology = MatrixTopology.from_id("C1F1/100000")
    paths = []
    for index, payload in enumerate((b"previous", b"current")):
        path = tmp_path / f"memory-{index}.ii"
        path.write_bytes(payload)
        paths.append(path)

    def occurrence(index: int) -> Occurrence:
        raw = paths[index].read_bytes()
        return Occurrence(index, None, source_path=str(paths[index]),
                          source_relative=paths[index].name,
                          source_sha256=hashlib.sha256(raw).hexdigest(),
                          source_digest128=simulator_module._digest128(raw))

    def assignment(start: int) -> dict[str, object]:
        return {"status": "READY", "topology": topology.topology_id,
                "selected_count": 1, "rows": [{
                    "ordinal": 0, "global_slot": 0, "f_relationship": 0,
                    "per_f_slot": 0, "dispatch_order": start,
                    "authority_dispatch_order": start,
                    "authority_tu_seq": start}]}

    empty = {"schema": simulator_module.PREFIX_DESCRIPTOR_SCHEMA, "bytes": 0,
             "digest128": simulator_module._digest128(b"")}
    prior = {"c_authorities": {"ZSTD_ROUTE": {"native_next_tu_seq": 1}},
             "relationships": {"ZSTD_ROUTE": {"C0|F0": {
                 "committed_raw_prefix_descriptor": empty,
                 "_runtime_history": b"", "next_rel_seq": 1,
                 "native_last_tu_seq": 0, "native_next_rel_seq": 1,
                 "native_state_digest": "b" * 32,
                 "history_nonce": 1, "route_identity": "C0->F0"}}}}
    matrix = MethodMatrixSimulator(topology, methods=("ZSTD_ROUTE",),
                                   assignment_authority=assignment(1))
    matrix.authority["ZSTD_ROUTE"]["status"] = "READY"
    _fake_native_batch_runner(monkeypatch)
    result = matrix.run([occurrence(1)], repeat_full=True, prior_state=prior,
                        predecessor_occurrences=[occurrence(0)],
                        predecessor_assignment_authority=assignment(0))
    row = result["rows"][0]
    assert row["native_tu_seq"] == 1
    assert row["rel_seq"] == 1
    assert result["c_authorities"]["ZSTD_ROUTE"]["native_next_tu_seq"] == 2


@pytest.mark.parametrize("mutation", ("missing_public", "wrong_public",
                                       "missing_both", "wrong_internal"))
def test_direct_native_product_rel_evidence_fails_closed(
        mutation: str) -> None:
    matrix = MethodMatrixSimulator(MatrixTopology.from_id("C1F1/100000"),
                                   methods=("P29",))
    matrix.authority["P29"]["status"] = "READY"
    product: dict[str, object] = {
        "tu_seq": 0, "rel_seq": 1, "_native_rel_seq": 1,
        "state_before_digest": "a" * 32, "state_digest": "b" * 32,
        "transaction_digest": "d" * 32, "encoded_source_bytes": 1,
        "simulator_execution_ns": 1, "committed": True}
    if mutation == "missing_public":
        del product["rel_seq"]
    elif mutation == "wrong_public":
        product["rel_seq"] = 2
    elif mutation == "missing_both":
        del product["rel_seq"]
        del product["_native_rel_seq"]
    elif mutation == "wrong_internal":
        product["_native_rel_seq"] = 2
    matrix._native_rows["P29"] = {0: product}
    state = simulator_module._RelationshipState(
        ("C0", "F0"), next_rel_seq=1, native_next_rel_seq=1)
    assignment = {"relationship_key": ["C0", "F0"], "relationship_index": 0,
                  "slot": 0, "global_slot": 0, "authority_tu_seq": 0}
    with pytest.raises(MatrixError, match="REL"):
        matrix._run_occurrence(None, Occurrence(0, b"payload"), assignment,
                               "P29", state, raw_override=b"payload")


@pytest.mark.parametrize("mutation", (
    "missing_product", "string_product", "bool_product", "mismatch_product",
    "missing_authority", "string_authority", "bool_authority", "mismatch_authority",
    "valid_nonzero"))
def test_direct_native_product_tu_evidence_binds_assignment(
        mutation: str) -> None:
    matrix = MethodMatrixSimulator(MatrixTopology.from_id("C1F1/100000"),
                                   methods=("P29",))
    matrix.authority["P29"]["status"] = "READY"
    product: dict[str, object] = {
        "tu_seq": 41, "rel_seq": 1, "_native_rel_seq": 1,
        "state_before_digest": "a" * 32, "state_digest": "b" * 32,
        "transaction_digest": "d" * 32, "encoded_source_bytes": 1,
        "simulator_execution_ns": 1, "committed": True}
    assignment: dict[str, object] = {
        "relationship_key": ["C0", "F0"], "relationship_index": 0,
        "slot": 0, "global_slot": 0, "authority_tu_seq": 41}
    if mutation == "missing_product":
        del product["tu_seq"]
    elif mutation == "string_product":
        product["tu_seq"] = "41"
    elif mutation == "bool_product":
        product["tu_seq"] = True
    elif mutation == "mismatch_product":
        product["tu_seq"] = 42
    elif mutation == "missing_authority":
        del assignment["authority_tu_seq"]
    elif mutation == "string_authority":
        assignment["authority_tu_seq"] = "41"
    elif mutation == "bool_authority":
        assignment["authority_tu_seq"] = True
    elif mutation == "mismatch_authority":
        assignment["authority_tu_seq"] = 42
    matrix._native_rows["P29"] = {0: product}
    state = simulator_module._RelationshipState(
        ("C0", "F0"), next_rel_seq=1, native_next_rel_seq=1)
    if mutation == "valid_nonzero":
        row = matrix._run_occurrence(None, Occurrence(0, b"payload"), assignment,
                                     "P29", state, raw_override=b"payload")
        assert row["native_tu_seq"] == 41
    else:
        with pytest.raises(MatrixError, match="TU sequence"):
            matrix._run_occurrence(None, Occurrence(0, b"payload"), assignment,
                                   "P29", state, raw_override=b"payload")


@pytest.mark.parametrize("topology_id", ("C1F1/100000", "C1F20/40"))
@pytest.mark.parametrize("method", ("ZSTD_ROUTE", "P29", "GRZ_RESIDUAL"))
def test_stateful_native_profiles_carry_three_tu_successor(
        tmp_path: Path, topology_id: str, method: str,
        monkeypatch: pytest.MonkeyPatch) -> None:
    # Exercise the batch lifecycle without requiring an untracked native build
    # in this isolated Python-only worktree.
    _fake_native_batch_runner(monkeypatch)
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

    def authority(relations: list[int], start: int = 0,
                  prior_rel: dict[int, int] | None = None) -> dict[str, object]:
        next_rel = dict(prior_rel or {})
        rows = []
        for index, relation in enumerate(relations):
            rel_seq = next_rel.get(relation, 0)
            next_rel[relation] = rel_seq + 1
            rows.append({"ordinal": index,
                         "global_slot": relation * (2 if topology_id == "C1F20/40" else 100000),
                         "f_relationship": relation, "per_f_slot": 0,
                         "dispatch_order": start + index, "authority_dispatch_order": start + index,
                         "authority_tu_seq": start + index,
                         "authority_rel_seq": rel_seq})
        return {"status": "READY", "topology": topology_id, "selected_count": 3,
                "rows": rows}

    predecessor = [occurrence(index) for index in range(3)]
    measured = [occurrence(index) for index in range(3, 6)]
    first = authority([0, 0, 0])
    second = (authority([1, 0, 1], 3, {0: 3, 1: 0}) if topology_id == "C1F20/40"
              else authority([0, 0, 0], 3, {0: 3}))
    output = simulator_module._native_batch(
        measured, topology, second, method,
        predecessor_occurrences=predecessor, predecessor_assignment=first)
    assert [row["tu_seq"] for row in output.values()] == [3, 4, 5]
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
