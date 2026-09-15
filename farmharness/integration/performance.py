"""Pure, fail-closed scoring for the S80 two-turn performance gate."""

from __future__ import annotations

from collections import Counter
from collections.abc import Mapping, Sequence
from copy import deepcopy
from fractions import Fraction
from statistics import median
from typing import Any


CELL_SCHEMA = "icecream-s80-cell-v1"
REPORT_SCHEMA = "icecream-s80-performance-v1"
ARMS = ("P29V1", "ZSTD_TU", "ZSTD_ROUTE", "legacy")
TURNS = ("A", "B")
CURRENT_S80_CORPUS = "firefox-root-header-1000"
REPLAY_COMPATIBLE_S80_CORPORA = frozenset(("firefox-1000", CURRENT_S80_CORPUS))
HEADLINE_LINK_BPS = (1_000_000_000, 100_000_000)
CONTEXT_LINK_BPS = (10_000_000_000, 25_000_000)
REPETITIONS = 3
S80_ORDER = (
    ("P29V1", 1),
    ("ZSTD_TU", 1),
    ("ZSTD_ROUTE", 1),
    ("legacy", 1),
    ("ZSTD_TU", 2),
    ("legacy", 2),
    ("P29V1", 2),
    ("ZSTD_ROUTE", 2),
    ("legacy", 3),
    ("P29V1", 3),
    ("ZSTD_ROUTE", 3),
    ("ZSTD_TU", 3),
)
CELL_FIELDS = frozenset(
    ("schema", "arm", "repetition", "sequence", "run_id", "status", "turns")
)
TURN_FIELDS = frozenset(
    (
        "jobs",
        "exact_objects",
        "wall_ms",
        "c_to_f_bytes",
        "f_to_c_bytes",
        "job_wall_p95_ms",
        "job_wall_p99_ms",
        "source_mutex_records",
        "source_mutex_wait_ns",
        "source_mutex_service_ns",
    )
)


class S80EvidenceError(ValueError):
    """The performance evidence is incomplete, ambiguous, or not comparable."""


def validate_s80_arm_scenario(
    arm: str,
    scenario: Mapping[str, Any],
    *,
    historical_replay: bool = False,
) -> None:
    """Authenticate one fair, fresh S80 arm before any farm mutation."""

    if arm not in ARMS:
        raise S80EvidenceError(f"invalid S80 arm {arm!r}")
    instances = scenario.get("instances")
    images = scenario.get("images")
    workload = scenario.get("workload")
    expect = scenario.get("expect")
    if not isinstance(instances, list) or not isinstance(images, Mapping):
        raise S80EvidenceError(f"{arm} scenario has invalid instances or images")
    if not isinstance(workload, Mapping) or not isinstance(expect, Mapping):
        raise S80EvidenceError(f"{arm} scenario has invalid workload or expectations")
    schedulers = [item for item in instances if item.get("role") == "S"]
    clients = [item for item in instances if item.get("role") == "C"]
    workers = [item for item in instances if item.get("role") == "F"]
    expected_profile = "OFF" if arm == "legacy" else arm
    selected_profile = (
        schedulers[0].get("env", {}).get("ICECC_P50_PROFILE")
        if len(schedulers) == 1
        else None
    )
    if selected_profile != expected_profile:
        raise S80EvidenceError(
            f"{arm} scenario does not select profile {expected_profile!r}"
        )
    if len(clients) != 1 or len(workers) != 1:
        raise S80EvidenceError(f"{arm} scenario needs exactly one C and one stable F")
    references = set(images.values())
    if len(references) != 1 or not all(
        isinstance(reference, str)
        and reference.rsplit(":", 1)[-1].lower().startswith("p50")
        for reference in references
    ):
        raise S80EvidenceError(f"{arm} scenario must use one sealed P50 product image")
    if scenario.get("shape") != "S'C'F'":
        raise S80EvidenceError(f"{arm} scenario must use the full-newgen shape")
    accepted_corpora = (
        REPLAY_COMPATIBLE_S80_CORPORA
        if historical_replay
        else frozenset((CURRENT_S80_CORPUS,))
    )
    if (
        workload.get("corpus") not in accepted_corpora
        or workload.get("turns") != ["A", "B"]
        or workload.get("repeat") != 1
    ):
        raise S80EvidenceError(f"{arm} scenario must run one Firefox A/B pair")
    if scenario.get("timeline") != [] or scenario.get("controls") != []:
        raise S80EvidenceError(f"{arm} scenario cannot contain events or controls")
    required_expect = {
        "engagement": "expected(c,f)",
        "error106_max": 0,
        "exact": "all",
        "tail_to_incapable": 0,
        "wall_s_max": None,
        "wedges": 0,
    }
    if any(expect.get(key) != value for key, value in required_expect.items()):
        raise S80EvidenceError(f"{arm} scenario expectations are not fail-closed")
    expected_reuse = "all-true-when-p29v1" if arm == "P29V1" else "none-when-legacy"
    if expect.get("reuse") != expected_reuse:
        raise S80EvidenceError(f"{arm} scenario has the wrong reuse expectation")


def validate_s80_matrix_scenarios(
    scenarios: Mapping[str, Mapping[str, Any]],
    *,
    historical_replay: bool = False,
) -> None:
    """Require one comparable scenario, varying only arm-owned fields."""

    if set(scenarios) != set(ARMS):
        raise S80EvidenceError("S80 matrix must contain exactly the four arms")
    normalized: dict[str, dict[str, Any]] = {}
    for arm in ARMS:
        validate_s80_arm_scenario(
            arm, scenarios[arm], historical_replay=historical_replay
        )
        document = deepcopy(dict(scenarios[arm]))
        document["id"] = "S80-arm"
        scheduler = next(
            item for item in document["instances"] if item.get("role") == "S"
        )
        scheduler["env"]["ICECC_P50_PROFILE"] = "S80_PROFILE"
        document["expect"]["reuse"] = "S80_REUSE"
        normalized[arm] = document
    baseline = normalized[ARMS[0]]
    differing = [arm for arm in ARMS[1:] if normalized[arm] != baseline]
    if differing:
        raise S80EvidenceError(
            "S80 arm scenarios differ outside profile/id/reuse: "
            f"{differing!r}"
        )


def render_s80_report(report: Mapping[str, Any]) -> str:
    """Render a compact deterministic report from a validated S80 score."""

    lines = [
        "# S80 two-build performance",
        "",
        f"Verdict: **{report['status']}**",
        "",
        "| Arm | Turn | Median wall (ms) | Job p95/p99 (ms) | Median wire (bytes) | Mutex wait (ns) | Mutex service (ns) | 1 Gbit/s | 100 Mbit/s | 10 Gbit/s | 25 Mbit/s |",
        "|---|:---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    rates = (1_000_000_000, 100_000_000, 10_000_000_000, 25_000_000)
    for arm in ARMS:
        for turn in TURNS:
            values = report["arms"][arm]["turns"][turn]
            score_values = [
                values["scores"][str(rate)]["median_effective_seconds"]
                for rate in rates
            ]
            lines.append(
                f"| {arm} | {turn} | {values['median_wall_ms']} | "
                f"{values['median_job_wall_p95_ms']}/"
                f"{values['median_job_wall_p99_ms']} | "
                f"{values['median_wire_bytes']} | "
                f"{values['median_source_mutex_wait_ns']} | "
                f"{values['median_source_mutex_service_ns']} | "
                + " | ".join(str(value) for value in score_values)
                + " |"
            )
    lines.extend(("", "## Headline clauses", ""))
    for clause in report["clauses"]:
        lines.append(
            f"- **{clause['status']}** turn {clause['turn']} at "
            f"{clause['link_bps']} bit/s: P29V1 "
            f"{clause['p29_effective_seconds']} s"
        )
    return "\n".join(lines) + "\n"


def s80_cell_from_bundle(
    arm: str,
    repetition: int,
    sequence: int,
    bundle: Mapping[str, Any],
    verdict: Mapping[str, Any],
) -> dict[str, Any]:
    """Reduce one verified fresh farm bundle to the pure S80 cell contract."""

    if arm not in ARMS:
        raise S80EvidenceError(f"invalid S80 arm {arm!r}")
    if verdict.get("status") != "PASS":
        raise S80EvidenceError(f"{arm} repetition {repetition} did not pass")
    run_id = bundle.get("run_id")
    rows = bundle.get("rows")
    observations = bundle.get("observations")
    if not isinstance(run_id, str) or not run_id:
        raise S80EvidenceError("bundle has no run ID")
    if not isinstance(rows, list) or not rows:
        raise S80EvidenceError(f"bundle {run_id} has no acceptance rows")
    if not isinstance(observations, Mapping):
        raise S80EvidenceError(f"bundle {run_id} has no observations")
    expected_profile = None if arm == "legacy" else arm
    bad_rows: list[object] = []
    expected_outcome = "none" if arm == "legacy" else "committed"
    for index, row in enumerate(rows):
        if not isinstance(row, Mapping):
            bad_rows.append(f"@row:{index}")
        elif (
            row.get("exact") is not True
            or row.get("tail_profile") != expected_profile
            or row.get("session_outcome") != expected_outcome
        ):
            bad_rows.append(row.get("job_id", f"@row:{index}"))
    if bad_rows:
        raise S80EvidenceError(
            f"bundle {run_id} has rows outside the {arm} arm: {bad_rows!r}"
        )
    turns = observations.get("turns")
    if not isinstance(turns, Mapping) or frozenset(turns) != frozenset(TURNS):
        raise S80EvidenceError(f"bundle {run_id} has incomplete turn observations")
    selected_turns = {
        turn: {field: turns[turn].get(field) for field in TURN_FIELDS}
        if isinstance(turns[turn], Mapping)
        else {}
        for turn in TURNS
    }
    if arm == "legacy":
        legacy = observations.get("legacy_wire")
        if not isinstance(legacy, Mapping) or legacy.get("record_count") != len(rows):
            raise S80EvidenceError(
                f"bundle {run_id} lacks complete conserved legacy-wire evidence"
            )
    return {
        "arm": arm,
        "repetition": repetition,
        "run_id": run_id,
        "schema": CELL_SCHEMA,
        "sequence": sequence,
        "status": "PASS",
        "turns": selected_turns,
    }


def _integer(value: object, field: str, *, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise S80EvidenceError(f"{field} must be an integer >= {minimum}")
    return value


def _effective_ns(turn: Mapping[str, Any], link_bps: int) -> Fraction:
    wire_bytes = turn["c_to_f_bytes"] + turn["f_to_c_bytes"]
    return Fraction(turn["wall_ms"] * 1_000_000) + Fraction(
        wire_bytes * 8 * 1_000_000_000, link_bps
    )


def _seconds(value: Fraction) -> float:
    return round(float(value / 1_000_000_000), 9)


def _validated_turn(
    value: object, *, arm: str, repetition: int, turn: str
) -> dict[str, int]:
    label = f"{arm} repetition {repetition} turn {turn}"
    if not isinstance(value, Mapping) or frozenset(value) != TURN_FIELDS:
        raise S80EvidenceError(f"{label} fields do not match the S80 contract")
    result = {
        field: _integer(value.get(field), f"{label} {field}") for field in TURN_FIELDS
    }
    if result["jobs"] == 0 or result["wall_ms"] == 0:
        raise S80EvidenceError(f"{label} has no measured work")
    if result["exact_objects"] != result["jobs"]:
        raise S80EvidenceError(f"{label} contains nonexact or missing objects")
    if result["c_to_f_bytes"] + result["f_to_c_bytes"] == 0:
        raise S80EvidenceError(f"{label} has no wire-byte evidence")
    if (
        result["job_wall_p95_ms"] == 0
        or result["job_wall_p99_ms"] < result["job_wall_p95_ms"]
    ):
        raise S80EvidenceError(f"{label} has invalid tail-latency evidence")
    if arm == "legacy":
        if any(
            result[field] != 0
            for field in (
                "source_mutex_records",
                "source_mutex_wait_ns",
                "source_mutex_service_ns",
            )
        ):
            raise S80EvidenceError(f"{label} has impossible P50 mutex evidence")
    elif (
        result["source_mutex_records"] != result["jobs"]
        or result["source_mutex_service_ns"] == 0
    ):
        raise S80EvidenceError(f"{label} lacks complete source-mutex evidence")
    return result


def score_s80_cells(cells: Sequence[Mapping[str, Any]]) -> dict[str, Any]:
    """Validate twelve fresh A/B cells and apply the strict P29 headline gate."""

    expected_count = len(ARMS) * REPETITIONS
    if len(cells) != expected_count:
        raise S80EvidenceError(
            f"S80 needs exactly {expected_count} cells, observed {len(cells)}"
        )
    validated: list[dict[str, Any]] = []
    for index, cell in enumerate(cells, start=1):
        if not isinstance(cell, Mapping) or frozenset(cell) != CELL_FIELDS:
            raise S80EvidenceError(f"cell {index} fields do not match the S80 contract")
        if cell.get("schema") != CELL_SCHEMA:
            raise S80EvidenceError(f"cell {index} schema is invalid")
        arm = cell.get("arm")
        if arm not in ARMS:
            raise S80EvidenceError(f"cell {index} arm is invalid")
        repetition = _integer(
            cell.get("repetition"), f"cell {index} repetition", minimum=1
        )
        sequence = _integer(cell.get("sequence"), f"cell {index} sequence", minimum=1)
        run_id = cell.get("run_id")
        if not isinstance(run_id, str) or not run_id:
            raise S80EvidenceError(f"cell {index} run_id is invalid")
        if cell.get("status") != "PASS":
            raise S80EvidenceError(f"cell {index} did not pass its scenario verdict")
        turns = cell.get("turns")
        if not isinstance(turns, Mapping) or frozenset(turns) != frozenset(TURNS):
            raise S80EvidenceError(
                f"cell {index} does not contain exactly turns A and B"
            )
        validated.append(
            {
                "arm": arm,
                "repetition": repetition,
                "run_id": run_id,
                "sequence": sequence,
                "turns": {
                    turn: _validated_turn(
                        turns[turn], arm=arm, repetition=repetition, turn=turn
                    )
                    for turn in TURNS
                },
            }
        )

    run_ids = [cell["run_id"] for cell in validated]
    if len(set(run_ids)) != expected_count:
        raise S80EvidenceError("S80 cells do not have unique fresh run IDs")
    sequences = [cell["sequence"] for cell in validated]
    if sorted(sequences) != list(range(1, expected_count + 1)):
        raise S80EvidenceError("S80 sequence is not exactly 1 through 12")
    ordered = sorted(validated, key=lambda cell: cell["sequence"])
    if any(left["arm"] == right["arm"] for left, right in zip(ordered, ordered[1:])):
        raise S80EvidenceError("S80 arm order is not alternating")
    counts = Counter((cell["arm"], cell["repetition"]) for cell in validated)
    expected_keys = {
        (arm, repetition) for arm in ARMS for repetition in range(1, REPETITIONS + 1)
    }
    if set(counts) != expected_keys or any(count != 1 for count in counts.values()):
        raise S80EvidenceError("S80 does not contain one cell per arm and repetition")

    rates = (*HEADLINE_LINK_BPS, *CONTEXT_LINK_BPS)
    scores: dict[tuple[str, str, int], Fraction] = {}
    arm_report: dict[str, Any] = {}
    for arm in ARMS:
        arm_cells = sorted(
            (cell for cell in validated if cell["arm"] == arm),
            key=lambda cell: cell["repetition"],
        )
        turns_report: dict[str, Any] = {}
        for turn in TURNS:
            turn_rows = [cell["turns"][turn] for cell in arm_cells]
            effective = {
                link_bps: [_effective_ns(row, link_bps) for row in turn_rows]
                for link_bps in rates
            }
            for link_bps, values in effective.items():
                scores[(arm, turn, link_bps)] = median(values)
            turns_report[turn] = {
                "median_job_wall_p95_ms": median(
                    row["job_wall_p95_ms"] for row in turn_rows
                ),
                "median_job_wall_p99_ms": median(
                    row["job_wall_p99_ms"] for row in turn_rows
                ),
                "median_source_mutex_service_ns": median(
                    row["source_mutex_service_ns"] for row in turn_rows
                ),
                "median_source_mutex_wait_ns": median(
                    row["source_mutex_wait_ns"] for row in turn_rows
                ),
                "median_wall_ms": median(row["wall_ms"] for row in turn_rows),
                "median_wire_bytes": median(
                    row["c_to_f_bytes"] + row["f_to_c_bytes"] for row in turn_rows
                ),
                "runs": [
                    {
                        "c_to_f_bytes": row["c_to_f_bytes"],
                        "f_to_c_bytes": row["f_to_c_bytes"],
                        "jobs": row["jobs"],
                        "job_wall_p95_ms": row["job_wall_p95_ms"],
                        "job_wall_p99_ms": row["job_wall_p99_ms"],
                        "repetition": cell["repetition"],
                        "run_id": cell["run_id"],
                        "source_mutex_records": row["source_mutex_records"],
                        "source_mutex_service_ns": row["source_mutex_service_ns"],
                        "source_mutex_wait_ns": row["source_mutex_wait_ns"],
                        "wall_ms": row["wall_ms"],
                    }
                    for cell, row in zip(arm_cells, turn_rows)
                ],
                "scores": {
                    str(link_bps): {
                        "median_effective_seconds": _seconds(
                            scores[(arm, turn, link_bps)]
                        ),
                        "run_effective_seconds": [
                            _seconds(value) for value in effective[link_bps]
                        ],
                    }
                    for link_bps in rates
                },
            }
        arm_report[arm] = {"turns": turns_report}

    clauses: list[dict[str, Any]] = []
    for turn in TURNS:
        for link_bps in HEADLINE_LINK_BPS:
            p29 = scores[("P29V1", turn, link_bps)]
            comparisons = {
                arm: _seconds(scores[(arm, turn, link_bps)])
                for arm in ARMS
                if arm != "P29V1"
            }
            passing = all(
                p29 < scores[(arm, turn, link_bps)] for arm in ARMS if arm != "P29V1"
            )
            clauses.append(
                {
                    "comparisons": comparisons,
                    "link_bps": link_bps,
                    "p29_effective_seconds": _seconds(p29),
                    "status": "PASS" if passing else "FAIL",
                    "turn": turn,
                }
            )

    return {
        "arms": arm_report,
        "cell_order": [cell["run_id"] for cell in ordered],
        "clauses": clauses,
        "context_link_bps": list(CONTEXT_LINK_BPS),
        "headline_link_bps": list(HEADLINE_LINK_BPS),
        "schema": REPORT_SCHEMA,
        "status": "PASS"
        if all(item["status"] == "PASS" for item in clauses)
        else "FAIL",
    }
