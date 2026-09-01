from __future__ import annotations

import json
from pathlib import Path

import pytest

from s8_method_matrix_simulator import (
    MatrixError,
    MatrixTopology,
    MethodMatrixSimulator,
    Occurrence,
    assign_relationships,
    repeat_full_state_contract,
    _authenticated_assignment,
    verify_experiment,
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
    assert summary["relationships"]["ZSTD_ROUTE"]["C0|F0"]["committed_raw_prefix_bytes"] == 8
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


def test_methods_do_not_alias_and_missing_authority_is_explicit() -> None:
    topology = MatrixTopology.from_id("C1F1/100000")
    simulator = MethodMatrixSimulator(topology)
    result = simulator.run([Occurrence(0, b"payload")])
    statuses = result["method_status"]
    assert statuses == {
        "RAW_II": "READY", "ZSTD_TU": "READY", "P29": "NOT_READY",
        "GRZ_RESIDUAL": "NOT_READY", "ZSTD_ROUTE": "READY",
        "ZSTD_COHORT": "NOT_READY", "ZSTD_GLOBAL": "NOT_IMPLEMENTED",
    }
    rows = {row["method"]: row for row in result["rows"]}
    assert rows["ZSTD_COHORT"]["encoded_bytes"] is None
    assert rows["ZSTD_GLOBAL"]["encoded_bytes"] is None
    assert rows["RAW_II"]["encoded_sha256"] != rows["ZSTD_TU"]["encoded_sha256"]


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


def test_repeat_full_only_carries_declared_relationship_state() -> None:
    assert repeat_full_state_contract("RAW_II")["fields"] == []
    assert repeat_full_state_contract("ZSTD_TU")["fields"] == []
    assert "committed_raw_prefix" in repeat_full_state_contract("ZSTD_ROUTE")["fields"]
