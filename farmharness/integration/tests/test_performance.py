from __future__ import annotations

from copy import deepcopy

import pytest

from farmharness.integration.performance import (
    S80_ORDER,
    S80EvidenceError,
    s80_cell_from_bundle,
    score_s80_cells,
)


WALL_MS = {"P29V1": 800, "ZSTD_TU": 1000, "ZSTD_ROUTE": 1100, "legacy": 1300}
WIRE_BYTES = {
    "P29V1": 10_000_000,
    "ZSTD_TU": 20_000_000,
    "ZSTD_ROUTE": 18_000_000,
    "legacy": 30_000_000,
}


def _cells() -> list[dict[str, object]]:
    cells = []
    for sequence, (arm, repetition) in enumerate(S80_ORDER, start=1):
        turns = {}
        for turn in ("A", "B"):
            jobs = 1000
            wall = WALL_MS[arm] + (100 if turn == "B" else 0) + repetition
            wire = WIRE_BYTES[arm] + repetition
            p50 = arm != "legacy"
            turns[turn] = {
                "c_to_f_bytes": wire,
                "exact_objects": jobs,
                "f_to_c_bytes": 1000,
                "jobs": jobs,
                "source_mutex_records": jobs if p50 else 0,
                "source_mutex_service_ns": 500_000_000 if p50 else 0,
                "source_mutex_wait_ns": 2_000_000 if p50 else 0,
                "wall_ms": wall,
            }
        cells.append(
            {
                "arm": arm,
                "repetition": repetition,
                "run_id": f"run-{sequence}",
                "schema": "icecream-s80-cell-v1",
                "sequence": sequence,
                "status": "PASS",
                "turns": turns,
            }
        )
    return cells


def test_s80_requires_p29_to_win_every_headline_turn() -> None:
    report = score_s80_cells(_cells())

    assert report["status"] == "PASS"
    assert len(report["clauses"]) == 4
    assert {clause["status"] for clause in report["clauses"]} == {"PASS"}
    assert report["arms"]["P29V1"]["turns"]["A"]["median_wall_ms"] == 802
    assert (
        report["arms"]["P29V1"]["turns"]["A"]["median_source_mutex_service_ns"]
        == 500_000_000
    )
    assert set(report["arms"]["P29V1"]["turns"]["A"]["scores"]) == {
        "10000000000",
        "1000000000",
        "100000000",
        "25000000",
    }


def test_s80_fails_when_p29_loses_only_turn_b() -> None:
    cells = _cells()
    for cell in cells:
        if cell["arm"] == "P29V1":
            cell["turns"]["B"]["wall_ms"] = 10_000

    report = score_s80_cells(cells)

    assert report["status"] == "FAIL"
    failed = [item for item in report["clauses"] if item["status"] == "FAIL"]
    assert {(item["turn"], item["link_bps"]) for item in failed} == {
        ("B", 1_000_000_000),
        ("B", 100_000_000),
    }


def test_s80_tie_is_not_a_strict_win() -> None:
    cells = _cells()
    p29 = next(
        cell for cell in cells if cell["arm"] == "P29V1" and cell["repetition"] == 2
    )
    zstd = next(
        cell for cell in cells if cell["arm"] == "ZSTD_TU" and cell["repetition"] == 2
    )
    for cell in cells:
        if cell["arm"] == "ZSTD_TU":
            cell["turns"] = deepcopy(p29["turns"])
    zstd["turns"] = deepcopy(p29["turns"])

    assert score_s80_cells(cells)["status"] == "FAIL"


def test_s80_cell_is_reduced_from_verified_bundle() -> None:
    source = _cells()[0]
    turns = {
        turn: {
            **values,
            "first_dispatch_ms": 1,
            "last_terminal_ms": 2,
            "source_mutex_wait_max_ns": 100,
            "wrapper_wall_ms": values["wall_ms"] + 1,
        }
        for turn, values in source["turns"].items()
    }
    rows = [
        {
            "exact": True,
            "job_id": f"job-{index}",
            "session_outcome": "committed",
            "tail_profile": "P29V1",
        }
        for index in range(2000)
    ]

    cell = s80_cell_from_bundle(
        "P29V1",
        1,
        1,
        {"observations": {"turns": turns}, "rows": rows, "run_id": "farm-run"},
        {"status": "PASS"},
    )

    assert cell == {**source, "run_id": "farm-run"}


def test_s80_cell_refuses_non_mapping_acceptance_row() -> None:
    source = _cells()[0]
    rows: list[object] = [
        {
            "exact": True,
            "job_id": "job-0",
            "session_outcome": "committed",
            "tail_profile": "P29V1",
        },
        "not-a-row",
    ]
    bundle = {
        "observations": {"turns": source["turns"]},
        "rows": rows,
        "run_id": "malformed-run",
    }

    with pytest.raises(S80EvidenceError, match=r"@row:1"):
        s80_cell_from_bundle("P29V1", 1, 1, bundle, {"status": "PASS"})


def test_s80_legacy_cell_requires_conserved_wire_records() -> None:
    source = next(cell for cell in _cells() if cell["arm"] == "legacy")
    rows = [
        {
            "exact": True,
            "job_id": f"job-{index}",
            "session_outcome": "none",
            "tail_profile": None,
        }
        for index in range(2000)
    ]
    bundle = {
        "observations": {
            "legacy_wire": {"record_count": len(rows)},
            "turns": source["turns"],
        },
        "rows": rows,
        "run_id": "legacy-run",
    }

    cell = s80_cell_from_bundle("legacy", 1, 1, bundle, {"status": "PASS"})
    assert cell["arm"] == "legacy"
    bundle["observations"]["legacy_wire"]["record_count"] -= 1
    with pytest.raises(S80EvidenceError, match="conserved legacy-wire"):
        s80_cell_from_bundle("legacy", 1, 1, bundle, {"status": "PASS"})


@pytest.mark.parametrize(
    ("mutation", "message"),
    (
        (lambda cells: cells.pop(), "exactly 12 cells"),
        (lambda cells: cells[1].__setitem__("run_id", "run-1"), "unique fresh run IDs"),
        (
            lambda cells: cells[1].__setitem__("arm", "P29V1"),
            "arm order is not alternating",
        ),
        (
            lambda cells: cells[0]["turns"]["A"].__setitem__("exact_objects", 999),
            "nonexact or missing objects",
        ),
        (
            lambda cells: cells[0]["turns"]["A"].__setitem__(
                "source_mutex_service_ns", 0
            ),
            "lacks complete source-mutex evidence",
        ),
        (
            lambda cells: (
                cells[3]["turns"]["A"].__setitem__("c_to_f_bytes", 0),
                cells[3]["turns"]["A"].__setitem__("f_to_c_bytes", 0),
            ),
            "has no wire-byte evidence",
        ),
    ),
)
def test_s80_refuses_incomplete_or_unfair_evidence(mutation, message: str) -> None:
    cells = _cells()
    mutation(cells)

    with pytest.raises(S80EvidenceError, match=message):
        score_s80_cells(cells)
