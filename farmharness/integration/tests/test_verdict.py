from __future__ import annotations

import copy

import pytest

from farmharness.integration.verdict import (
    BUNDLE_SCHEMA,
    CONTROL_VERDICT_SCHEMA,
    ROW_SCHEMA,
    VERDICT_SCHEMA,
    evaluate_bundle,
    evaluate_control,
)


SHA_A = "a" * 64
SHA_B = "b" * 64


def _expect() -> dict[str, object]:
    return {
        "engagement": "expected(c,f)",
        "error106_max": 0,
        "exact": "all",
        "reuse": "all-true-when-p29v1",
        "tail_to_incapable": 0,
        "wall_s_max": None,
        "wedges": 0,
    }


def _instance(name: str, role: str, version: int) -> dict[str, object]:
    value: dict[str, object] = {
        "env": {},
        "image": "new" if version == 50 else "old",
        "name": name,
        "role": role,
    }
    if role == "S":
        value["env"] = {"ICECC_P50_PROFILE": "P29V1"}
    return value


def _scenario(
    shape: str,
    *,
    client_versions: tuple[int, ...] = (50,),
    worker_versions: tuple[int, ...] = (50,),
) -> dict[str, object]:
    return {
        "expect": _expect(),
        "id": "fixture-" + str(abs(hash((shape, client_versions, worker_versions)))),
        "instances": [
            _instance("S1", "S", 43 if shape == "SCF" else 50),
            *(
                _instance(f"F{index + 1}", "F", version)
                for index, version in enumerate(worker_versions)
            ),
            *(
                _instance(f"C{index + 1}", "C", version)
                for index, version in enumerate(client_versions)
            ),
        ],
        "shape": shape,
        "timeline": [],
    }


def _row(
    job: int,
    *,
    client: str = "C1",
    client_version: int = 50,
    worker: str = "F1",
    worker_version: int = 50,
    tail: bool | None = None,
    profile: str | None = None,
    outcome: str | None = None,
) -> dict[str, object]:
    if tail is None:
        tail = client_version == worker_version == 50
    if profile is None and tail:
        profile = "P29V1"
    if outcome is None:
        outcome = "committed" if tail else "none"
    return {
        "c_to_f_bytes": 1024,
        "client_instance": client,
        "client_version": client_version,
        "cs": worker,
        "cs_version": worker_version,
        "event_epoch": 0,
        "exact": True,
        "f_to_c_bytes": 512,
        "job_id": str(job),
        "object_sha_local": SHA_A,
        "object_sha_remote": SHA_A,
        "retries": 0,
        "reuse": True if profile == "P29V1" else None,
        "schema": ROW_SCHEMA,
        "session_outcome": outcome,
        "tail_present": tail,
        "tail_profile": profile,
        "tu": f"files/tu-{job}.ii",
        "wall_ms": 25,
    }


def _observations(
    rows: list[dict[str, object]],
    *,
    old_workers: set[str] = frozenset(),
    revisions: dict[str, int] | None = None,
) -> dict[str, object]:
    workers = sorted({str(row["cs"]) for row in rows})
    clients = sorted({str(row["client_instance"]) for row in rows})
    return {
        "cell_wall_ms": 100,
        "compile_failure_job_ids": [],
        "error106_job_ids": [],
        "incomplete_turns": [],
        "job_lifecycle": [
            {
                "deadline_ms": 10_000,
                "dispatch_ms": index * 100,
                "job_id": row["job_id"],
                "terminal": "completion",
                "terminal_ms": index * 100 + 25,
                "turn": "A",
            }
            for index, row in enumerate(rows)
        ],
        "logins": [
            {
                "cache_profiles": [] if worker in old_workers else ["P29V1", "ZSTD_TU", "ZSTD_ROUTE"],
                "instance": worker,
                "protocol": 43 if worker in old_workers else 50,
            }
            for worker in workers
        ],
        "local_fallback_job_ids": [],
        "oracle": {"sample_mismatch_job_ids": [], "sample_total": 1},
        "sidecars": {
            worker: {
                "cache_ports": [] if worker in old_workers else [24000],
                "process_count": 0 if worker in old_workers else 1,
                "sessions": 0 if worker in old_workers else sum(
                    row["cs"] == worker and row["tail_present"] is True for row in rows
                ),
            }
            for worker in workers
        }
        | {client: {"sessions": 0} for client in clients},
        "wire_revisions": revisions
        if revisions is not None
        else {
            name: 1
            for name in workers + clients
            if name not in old_workers
            and any(
                row["tail_present"] is True
                and (
                    (row["cs"] == name and row["cs_version"] == 50)
                    or (row["client_instance"] == name and row["client_version"] == 50)
                )
                for row in rows
            )
        },
    }


def _bundle(
    scenario: dict[str, object],
    rows: list[dict[str, object]],
    observations: dict[str, object],
) -> dict[str, object]:
    return {
        "observations": observations,
        "rows": rows,
        "scenario": scenario,
        "schema": BUNDLE_SCHEMA,
    }


def _shape_fixtures() -> dict[str, dict[str, object]]:
    scf_rows = [_row(1, client_version=43, worker_version=43)]
    scheduler_first_rows = [_row(1, client_version=43, worker_version=43)]
    old_worker_rows = [_row(1, client_version=50, worker_version=43)]
    full_rows = [_row(1)]
    mixed_rows = [
        _row(1, client="C1", client_version=50, worker="F1", worker_version=50),
        _row(2, client="C1", client_version=50, worker="F2", worker_version=43),
        _row(3, client="C2", client_version=43, worker="F1", worker_version=50),
        _row(4, client="C2", client_version=43, worker="F2", worker_version=43),
    ]
    skew_rows = [
        _row(1, worker="F1"),
        _row(2, worker="F2", tail=False, outcome="fallback"),
    ]
    skew_observations = _observations(
        skew_rows,
        revisions={"C1": 1, "F1": 1, "F2": 2},
    )
    skew_observations["wire_revision_mismatches"] = [
        {
            "error": "WIRE_REVISION_MISMATCH",
            "fallback": "legacy",
            "job_id": "2",
        }
    ]
    return {
        "SCF": _bundle(
            _scenario("SCF", client_versions=(43,), worker_versions=(43,)),
            scf_rows,
            _observations(scf_rows, old_workers={"F1"}),
        ),
        "S'CF": _bundle(
            _scenario("S'CF", client_versions=(43,), worker_versions=(43,)),
            scheduler_first_rows,
            _observations(scheduler_first_rows, old_workers={"F1"}),
        ),
        "S'FC'": _bundle(
            _scenario("S'FC'", client_versions=(50,), worker_versions=(43,)),
            old_worker_rows,
            _observations(old_worker_rows, old_workers={"F1"}),
        ),
        "S'C'F'": _bundle(
            _scenario("S'C'F'"),
            full_rows,
            _observations(full_rows),
        ),
        "S'[FF'][CC']": _bundle(
            _scenario(
                "S'[FF'][CC']",
                client_versions=(50, 43),
                worker_versions=(50, 43),
            ),
            mixed_rows,
            _observations(mixed_rows, old_workers={"F2"}),
        ),
        "S'[F'F''][C']": _bundle(
            _scenario(
                "S'[F'F''][C']",
                client_versions=(50,),
                worker_versions=(50, 50),
            ),
            skew_rows,
            skew_observations,
        ),
    }


SHAPE_FIXTURES = _shape_fixtures()


@pytest.mark.parametrize("shape", sorted(SHAPE_FIXTURES))
def test_one_fixture_bundle_per_shape_passes(shape: str) -> None:
    verdict = evaluate_bundle(SHAPE_FIXTURES[shape])
    assert verdict["schema"] == VERDICT_SCHEMA
    assert verdict["status"] == "PASS", verdict
    assert verdict["offending_job_ids"] == []


def _control_fixtures() -> dict[str, dict[str, object]]:
    h1 = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    h1["rows"] = []
    h1["observations"]["job_lifecycle"] = []
    h1["observations"]["preflight_refusal"] = {
        "jobs_started": 0,
        "reason_code": "role-hash-mismatch",
    }

    h2 = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    h2["rows"][0].update(
        reuse=None,
        session_outcome="none",
        tail_present=False,
        tail_profile=None,
    )
    h2["observations"]["fault"] = {"client_kill_switch": True}

    h3 = copy.deepcopy(SHAPE_FIXTURES["S'[FF'][CC']"])
    old_client_row = h3["rows"][2]
    old_client_row.update(
        reuse=True,
        session_outcome="committed",
        tail_present=True,
        tail_profile="P29V1",
    )
    h3["observations"]["fault"] = {"mutant_scheduler": True}

    h4 = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    h4["rows"][0].update(exact=False, object_sha_remote=SHA_B)
    h4["observations"]["fault"] = {"corrupt_object": True}

    h5 = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    h5["scenario"]["timeline"] = [
        {"action": "kill -9", "instance": "F1", "trigger": "job 1"}
    ]
    h5["rows"][0]["retries"] = 1
    h5["observations"].update(
        fault={"worker_killed_job_ids": ["1"]},
        process_loss_recovery_job_ids=["1"],
        retry_limit_per_job=1,
    )
    h5["observations"]["job_lifecycle"][0]["terminal"] = "process-loss-recovery"
    return {"H1": h1, "H2": h2, "H3": h3, "H4": h4, "H5": h5}


CONTROL_FIXTURES = _control_fixtures()


@pytest.mark.parametrize("control", sorted(CONTROL_FIXTURES))
def test_harness_control_fixture_produces_its_designed_result(control: str) -> None:
    result = evaluate_control(control, CONTROL_FIXTURES[control])
    assert result["schema"] == CONTROL_VERDICT_SCHEMA
    assert result["status"] == "PASS", result


def test_control_oracle_rejects_a_broken_liveness_control() -> None:
    fixture = copy.deepcopy(CONTROL_FIXTURES["H2"])
    fixture["rows"][0].update(
        reuse=True,
        session_outcome="committed",
        tail_present=True,
        tail_profile="P29V1",
    )
    assert evaluate_bundle(fixture)["status"] == "PASS"
    assert evaluate_control("H2", fixture)["status"] == "FAIL"


def test_oracle_sample_mismatch_names_the_job() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    fixture["observations"]["oracle"]["sample_mismatch_job_ids"] = ["1"]
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    assert "1" in verdict["offending_job_ids"]
    assert next(item for item in verdict["clauses"] if item["id"] == "oracle.sample")["status"] == "FAIL"


def test_wedge_is_recomputed_instead_of_trusting_a_summary() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    fixture["observations"]["wedges"] = []
    fixture["observations"]["job_lifecycle"][0].update(
        deadline_ms=180_000,
        terminal=None,
        terminal_ms=None,
    )
    verdict = evaluate_bundle(fixture)
    clause = next(item for item in verdict["clauses"] if item["id"] == "wedges")
    assert clause["status"] == "FAIL"
    assert clause["offending_job_ids"] == ["1"]


def test_row_schema_is_closed_and_fail_closed() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["S'C'F'"])
    fixture["rows"][0]["unreviewed"] = True
    verdict = evaluate_bundle(fixture)
    assert verdict["status"] == "FAIL"
    assert next(item for item in verdict["clauses"] if item["id"] == "rows.schema")["status"] == "FAIL"


def test_malformed_login_entry_fails_closed_without_crashing() -> None:
    fixture = copy.deepcopy(SHAPE_FIXTURES["SCF"])
    fixture["observations"]["logins"].append("not-an-object")
    verdict = evaluate_bundle(fixture)
    clause = next(
        item for item in verdict["clauses"] if item["id"] == "shape.negotiated-legacy"
    )
    assert clause["status"] == "FAIL"
    assert clause["offending_job_ids"] == ["@login:1"]
