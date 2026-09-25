from __future__ import annotations

import copy
import hashlib
import json
import shutil
from pathlib import Path

import pytest

from farmharness.integration.tests import farm_fixture

from farmharness.integration import farmtest, report
from farmharness.integration.collect import (
    CollectError,
    _authenticated_rejoin_line,
    _abandoned_p50_retry_request_observation,
    _client_assignments,
    _canary_assignment_claims,
    _checkpoint_result_path,
    _assignment_preference,
    _control_observations,
    _event_log,
    _instance_version_at,
    _legacy_wire_binding_marker,
    _legacy_wire_candidates_for_assignment,
    _legacy_wire_results,
    _missing_compile_result_identity_reason,
    _one_role_log,
    _p29_action_lineages,
    _p29_interner_faults,
    _p50_assignment_identity_marker,
    _p50_assignment_identity_evidence,
    _retained_log_witness,
    _retained_log_witness_exact,
    _result_stream_kill_loss,
    _parse_logins,
    _reconcile_scheduler_dispatches,
    _scheduler_jobs,
    _scheduler_dispatch_epoch,
    _snapshot_live_evidence,
    _source_candidates_for_assignment,
    _source_results,
    _require_collectable_source_accounting,
    _source_transfer_failure_observation,
    _uncommitted_transport_failure_observation,
    _successful_strict_p50_late_result_binding,
    _transition_target_env,
    _unassigned_p50_failure_observation,
    _validate_preexposure_redispatches,
    _validate_orphan_recovery_markers,
    _warm_hint_overrides,
    collect_bundle,
    load_verified_bundle,
)
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.images import RecordingTransport
from farmharness.integration.lifecycle import bundle_root
from farmharness.integration.remote import (
    CommandResult,
    PlannedCommand,
    decode_ssh_payload,
)
from farmharness.integration.report import ReportError, report_bundle, verify_bundle
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness.integration.schema_validation import canonical_bytes
from farmharness.integration.verdict import (
    _assignment_preference_errors,
    evaluate_bundle,
)


INTEGRATION = Path(__file__).resolve().parents[1]
SHA = "a" * 64
C_GUID = "1" * 32


def _p29_action_lineage_record() -> dict[str, object]:
    return {
        "action": "TX_BEGIN",
        "actor": "F",
        "c_store_guid": C_GUID,
        "f_store_guid": "2" * 32,
        "history_nonce": 1,
        "previous_f_store_guid": "0" * 32,
        "profile": "p29_v1",
        "raw_digest": "3" * 32,
        "rel_seq": 0,
        "session_serial": 1,
        "tu_seq": 7,
    }


def test_p29_action_lineage_retains_exact_relationship_identity(
    tmp_path: Path,
) -> None:
    path = tmp_path / "f-action.jsonl"
    record = _p29_action_lineage_record()
    _write_jsonl(path, [record])
    assert _p29_action_lineages(path) == {
        (C_GUID, 7): {
            "c_store_guid": C_GUID,
            "f_store_guid": "2" * 32,
            "history_nonce": 1,
            "previous_f_store_guid": "0" * 32,
            "raw_digest": "3" * 32,
            "rel_seq": 0,
            "session_serial": 1,
            "tu_seq": 7,
        }
    }


@pytest.mark.parametrize(
    ("field", "value"),
    (
        ("actor", "C"),
        ("profile", "zstd_route"),
        ("c_store_guid", "0" * 32),
        ("f_store_guid", "0" * 32),
        ("previous_f_store_guid", "bad"),
        ("raw_digest", "bad"),
        ("tu_seq", -1),
        ("session_serial", 0),
        ("history_nonce", 0),
        ("rel_seq", -1),
    ),
)
def test_p29_action_lineage_rejects_invalid_identity(
    tmp_path: Path, field: str, value: object
) -> None:
    path = tmp_path / "f-action.jsonl"
    record = _p29_action_lineage_record()
    record[field] = value
    _write_jsonl(path, [record])
    with pytest.raises(CollectError, match="P29 TX_BEGIN"):
        _p29_action_lineages(path)


def test_p29_action_lineage_rejects_duplicate_source_identity(
    tmp_path: Path,
) -> None:
    path = tmp_path / "f-action.jsonl"
    record = _p29_action_lineage_record()
    _write_jsonl(path, [record, record])
    with pytest.raises(CollectError, match="duplicate P29 TX_BEGIN"):
        _p29_action_lineages(path)


@pytest.mark.parametrize("current_contract", [False, True])
def test_worker_bounce_collection_emits_verdict_compatible_action_lineage(
    tmp_path: Path, current_contract: bool,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    scenario.data["id"] = "S70-b4-worker-bounces"
    scenario.data["expect"]["engagement"] = "s70-b4-worker-bounces"
    scenario.data["expect"]["worker_cold_witness"] = "p29-action-lineage-v1"
    plan = farmtest.build_plan(farm, scenario, run_id=plan["run_id"])
    assert plan["worker_rejoin_epoch_contract"] == "icefarm-worker-rejoin-log-order-v1"
    if not current_contract:
        # Historical single-row extraction fixture has no restart timeline.
        # Keep its old behavior covered, separately from fail-closed v1 evidence.
        plan.pop("worker_rejoin_epoch_contract")
    for leaf in ("preflight.json", "lifecycle.json", "workload.json"):
        receipt_path = root / leaf
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        for field in (
            "farm_digest",
            "run_id",
            "scenario_digest",
            "topology_digest",
        ):
            receipt[field] = plan[field]
        if leaf == "lifecycle.json":
            receipt["plan"] = plan
        _write_json(receipt_path, receipt)
    lineage = _p29_action_lineage_record()
    lineage["raw_digest"] = "2" * 32
    lineage["tu_seq"] = 1
    _write_jsonl(
        root / "F1.results" / "f-action.jsonl",
        [
            lineage,
            {"action": "INPUT_COMMITTED", "c_store_guid": C_GUID, "tu_seq": 1},
        ],
    )

    if current_contract:
        with pytest.raises(CollectError, match="complete bounce timeline"):
            collect_bundle(farm, scenario, plan, sync_remote=False)
        return
    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)
    assert bundle["observations"]["p29_action_lineage"] == {
        "record_count": 1,
        "records": [
            {
                "c_store_guid": C_GUID,
                "f_store_guid": "2" * 32,
                "history_nonce": 1,
                "job_id": "C1:A:1:2",
                "previous_f_store_guid": "0" * 32,
                "raw_digest": "2" * 32,
                "rel_seq": 0,
                "schema": "icefarm-p29-action-lineage-v1",
                "session_serial": 1,
                "tu_seq": 1,
                "worker_instance": "F1",
            }
        ],
        "schema": "icefarm-p29-action-lineage-v1",
    }
    verdict = evaluate_bundle(bundle)
    worker_clause = next(
        clause
        for clause in verdict["clauses"]
        if clause["id"] == "s70.b4-worker-bounces"
    )
    assert "@observations:s70-b4-worker-action-lineage" not in worker_clause[
        "offending_job_ids"
    ]
    assert "@observations:s70-b4-worker-source-routes" not in worker_clause[
        "offending_job_ids"
    ]


def _source_result_record() -> dict[str, object]:
    return {
        "assignment_epoch": 1,
        "assignment_nonce": 1,
        "attempts": 1,
        "c_store_guid": C_GUID,
        "c_to_f_bytes": 0,
        "f_to_c_bytes": 0,
        "logical_job": 2,
        "profile": "P29V1",
        "raw_bytes": 0,
        "raw_digest": "0" * 32,
        "schema": "icecream-p50-source-result-v2",
        "source_mutex_service_ns": 1,
        "source_mutex_wait_ns": 0,
        "status": 4,
        "system_source_reuse": None,
        "terminal_error_code": 4,
        "terminal_error_name": "WIRE_REVISION_MISMATCH",
        "tu_seq": 0,
        "wire_job_id": 2,
    }


def test_source_result_authenticates_named_wire_revision_mismatch(
    tmp_path: Path,
) -> None:
    path = tmp_path / "source-result.jsonl"
    record = _source_result_record()
    _write_jsonl(path, [record])
    assert _source_results(path) == {(2, 1, 1): record}


def test_source_results_accept_v2_and_v3_rows_in_one_stream(
    tmp_path: Path,
) -> None:
    path = tmp_path / "source-result.jsonl"
    v2 = _source_result_record()
    v3 = _source_result_record()
    v2["wire_job_id"] = 20
    v2["logical_job"] = 20
    v2["schema"] = "icecream-p50-source-result-v2"
    v3["wire_job_id"] = 30
    v3["logical_job"] = 30
    v3["schema"] = "icecream-p50-source-result-v3"
    _write_jsonl(path, [v2, v3])

    assert _source_results(path) == {
        (20, 1, 1): v2,
        (30, 1, 1): v3,
    }


def test_source_result_v4_preserves_unavailable_r2_accounting_as_null(
    tmp_path: Path,
) -> None:
    path = tmp_path / "source-result.jsonl"
    record = _source_result_record()
    record.update(
        {
            "schema": "icecream-p50-source-result-v4",
            "mode": "R2_LINK",
            "stage": "post_read_dispatch_completion",
            "status": 0,
            "attempts": None,
            "attempts_measured": False,
            "wire_bytes_measured": False,
            "source_mutex_wait_ns": None,
            "source_mutex_service_ns": None,
            "source_mutex_timing_measured": False,
            "tu_seq": 9,
            "raw_bytes": 512,
            "raw_digest": "a" * 32,
            "c_to_f_bytes": None,
            "f_to_c_bytes": None,
            "system_source_reuse": False,
            "terminal_error_code": 0,
            "terminal_error_name": None,
        }
    )
    _write_jsonl(path, [record])

    parsed = _source_results(path)
    assert parsed[(2, 1, 1)] == record
    with pytest.raises(CollectError, match="attempts, wire bytes, or source-mutex timing unavailable"):
        _require_collectable_source_accounting(parsed)


def test_source_result_v4_measured_r1_accounting_remains_collectable(
    tmp_path: Path,
) -> None:
    path = tmp_path / "source-result.jsonl"
    record = _source_result_record()
    record.update(
        {
            "schema": "icecream-p50-source-result-v4",
            "mode": "R1_SERIAL",
            "stage": "serialized_transfer_completion",
            "status": 0,
            "attempts": 2,
            "attempts_measured": True,
            "wire_bytes_measured": True,
            "source_mutex_timing_measured": True,
            "tu_seq": 9,
            "raw_bytes": 512,
            "raw_digest": "a" * 32,
            "c_to_f_bytes": 812,
            "f_to_c_bytes": 231,
            "system_source_reuse": False,
            "source_mutex_wait_ns": 23,
            "source_mutex_service_ns": 17,
            "terminal_error_code": 0,
            "terminal_error_name": None,
        }
    )
    _write_jsonl(path, [record])

    parsed = _source_results(path)
    assert parsed[(2, 1, 1)] == record
    _require_collectable_source_accounting(parsed)


@pytest.mark.parametrize(
    ("attempts_measured", "attempts", "wire_bytes_measured", "c_to_f_bytes", "f_to_c_bytes"),
    (
        (True, 2, False, None, None),
        (False, None, True, 812, 231),
    ),
)
def test_source_result_v4_keeps_accounting_availability_independent(
    tmp_path: Path,
    attempts_measured: bool,
    attempts: object,
    wire_bytes_measured: bool,
    c_to_f_bytes: object,
    f_to_c_bytes: object,
) -> None:
    path = tmp_path / "source-result.jsonl"
    record = _source_result_record()
    record.update(
        {
            "schema": "icecream-p50-source-result-v4",
            "mode": "R1_SERIAL",
            "stage": "serialized_transfer_completion",
            "status": 0,
            "attempts": attempts,
            "attempts_measured": attempts_measured,
            "wire_bytes_measured": wire_bytes_measured,
            "source_mutex_timing_measured": True,
            "tu_seq": 9,
            "raw_bytes": 512,
            "raw_digest": "a" * 32,
            "c_to_f_bytes": c_to_f_bytes,
            "f_to_c_bytes": f_to_c_bytes,
            "system_source_reuse": False,
            "source_mutex_wait_ns": 23,
            "source_mutex_service_ns": 17,
            "terminal_error_code": 0,
            "terminal_error_name": None,
        }
    )
    _write_jsonl(path, [record])

    assert _source_results(path)[(2, 1, 1)] == record
    with pytest.raises(CollectError, match="attempts, wire bytes, or source-mutex timing unavailable"):
        _require_collectable_source_accounting({(2, 1, 1): record})


@pytest.mark.parametrize(
    ("attempts_measured", "attempts", "wire_bytes_measured", "c_to_f_bytes"),
    ((False, 1, False, None), (True, None, False, None),
     (False, None, False, 0)),
)
def test_source_result_v4_rejects_availability_value_disagreement(
    tmp_path: Path,
    attempts_measured: bool,
    attempts: object,
    wire_bytes_measured: bool,
    c_to_f_bytes: object,
) -> None:
    path = tmp_path / "source-result.jsonl"
    record = _source_result_record()
    record.update(
        {
            "schema": "icecream-p50-source-result-v4",
            "mode": "R1_SERIAL",
            "stage": "serialized_transfer_completion",
            "attempts_measured": attempts_measured,
            "attempts": attempts,
            "wire_bytes_measured": wire_bytes_measured,
            "source_mutex_timing_measured": True,
            "c_to_f_bytes": c_to_f_bytes,
            "f_to_c_bytes": None if not wire_bytes_measured else 8,
        }
    )
    _write_jsonl(path, [record])
    with pytest.raises(CollectError, match="accounting availability disagrees"):
        _source_results(path)


@pytest.mark.parametrize(
    ("mode", "stage"),
    (("UNKNOWN", "post_read_dispatch_completion"),
     ("R2_LINK", "serialized_transfer_completion")),
)
def test_source_result_v4_rejects_unknown_or_mismatched_mode(
    tmp_path: Path,
    mode: str,
    stage: str,
) -> None:
    path = tmp_path / "source-result.jsonl"
    record = _source_result_record()
    record.update(
        {
            "schema": "icecream-p50-source-result-v4",
            "mode": mode,
            "stage": stage,
            "attempts": None,
            "attempts_measured": False,
            "wire_bytes_measured": False,
            "source_mutex_timing_measured": False,
            "source_mutex_wait_ns": None,
            "source_mutex_service_ns": None,
            "c_to_f_bytes": None,
            "f_to_c_bytes": None,
        }
    )
    _write_jsonl(path, [record])
    with pytest.raises(CollectError, match="accounting availability disagrees"):
        _source_results(path)


@pytest.mark.parametrize(
    "mutation",
    ("unknown-schema", "missing-field", "extra-field", "non-string-schema"),
)
def test_source_result_rejects_unknown_or_changed_schema(
    tmp_path: Path, mutation: str,
) -> None:
    path = tmp_path / "source-result.jsonl"
    record = _source_result_record()
    if mutation == "unknown-schema":
        record["schema"] = "icecream-p50-source-result-v4"
    elif mutation == "missing-field":
        record.pop("source_mutex_wait_ns")
    elif mutation == "extra-field":
        record["unexpected"] = 1
    else:
        record["schema"] = ["icecream-p50-source-result-v3"]
    _write_jsonl(path, [record])

    with pytest.raises(CollectError, match="source-result schema mismatch"):
        _source_results(path)


@pytest.mark.parametrize(
    ("field", "value"),
    (
        ("terminal_error_code", 0),
        ("terminal_error_code", 65536),
        ("terminal_error_name", None),
        ("terminal_error_name", "OTHER"),
        ("status", 0),
    ),
)
def test_source_result_refuses_forged_terminal_error(
    tmp_path: Path, field: str, value: object
) -> None:
    path = tmp_path / "source-result.jsonl"
    record = _source_result_record()
    record[field] = value
    _write_jsonl(path, [record])
    with pytest.raises(CollectError):
        _source_results(path)


def test_p29_interner_fault_witness_is_exact_and_bound_to_client(
    tmp_path: Path,
) -> None:
    topology = [
        {"host": "h1", "name": "C1", "role": "C"},
        {"host": "h1", "name": "F1", "role": "F"},
    ]
    diagnostics = tmp_path / "diagnostics" / "h1"
    diagnostics.mkdir(parents=True)
    marker = (
        '{"schema":"icecream-p50-fault-v1",'
        '"fault":"p29-interner-fail-once","outcome":"fired"}'
    )
    (diagnostics / "C1.logs").write_text("ordinary log\n" + marker + "\n")
    (diagnostics / "F1.logs").write_text(marker + "\n")

    assert _p29_interner_faults(tmp_path, topology) == [
        {
            "client_instance": "C1",
            "schema": "icecream-p50-fault-v1",
            "fault": "p29-interner-fail-once",
            "outcome": "fired",
        }
    ]

    (diagnostics / "C1.logs").write_text(marker + "\n" + marker + "\n")
    with pytest.raises(CollectError, match="duplicate P29 interner fault"):
        _p29_interner_faults(tmp_path, topology)

    (diagnostics / "C1.logs").write_text(marker[:-1] + ',"extra":1}\n')
    with pytest.raises(CollectError, match="invalid P29 interner fault"):
        _p29_interner_faults(tmp_path, topology)

    duplicate_key = (
        '{"schema":"icecream-p50-fault-v1",'
        '"fault":"wrong","fault":"p29-interner-fail-once","outcome":"fired"}\n'
    )
    (diagnostics / "C1.logs").write_text(duplicate_key)
    with pytest.raises(CollectError, match="duplicate JSON key 'fault'"):
        _p29_interner_faults(tmp_path, topology)


def test_retained_readiness_witness_is_bound_to_exact_post_offset_log_line(
    tmp_path: Path,
) -> None:
    scheduler = {"host": "h1", "name": "S1", "role": "S"}
    path = tmp_path / "diagnostics" / "h1" / "S1.log" / "scheduler.log"
    path.parent.mkdir(parents=True)
    stale = "[1] stale startup"
    fresh = "[2] ICECREAM scheduler 1.5.90 starting up, port 23000"
    prefix = (stale + "\n").encode()
    captured = (fresh + "\n").encode()
    path.write_bytes(prefix + captured + b"[3] later scheduler activity\n")

    assert _retained_log_witness(tmp_path, scheduler, len(prefix), fresh)
    assert not _retained_log_witness(tmp_path, scheduler, len(prefix), stale)
    assert not _retained_log_witness(tmp_path, scheduler, path.stat().st_size + 1, fresh)
    assert not _retained_log_witness(
        tmp_path, {"host": "h1", "name": "F1", "role": "F"}, 0, fresh
    )
    assert not _retained_log_witness(tmp_path, scheduler, len(prefix), "absent")
    assert _retained_log_witness_exact(
        tmp_path,
        scheduler,
        len(prefix),
        fresh,
        len(captured),
        hashlib.sha256(captured).hexdigest(),
    )
    assert not _retained_log_witness_exact(
        tmp_path,
        scheduler,
        len(prefix),
        fresh,
        len(captured) - 1,
        hashlib.sha256(captured).hexdigest(),
    )
    assert not _retained_log_witness_exact(
        tmp_path, scheduler, len(prefix), fresh, len(captured), "0" * 64
    )


def _preference_fixture(*, selected: str = "F2", saturated: bool = False):
    scenario = type("Scenario", (), {"data": {"workload": {"clients": ["C1"]}}})()
    plan = {
        "topology": {
            "instances": [
                {"name": "F1", "role": "F", "slots": 1},
                {"name": "F2", "role": "F", "slots": 1},
                {"name": "C1", "role": "C"},
            ],
            "relationships": [
                {"c": "C1", "f": "F1", "cache_expected": True},
                {"c": "C1", "f": "F2", "cache_expected": True},
            ],
        }
    }
    claims = [
        {
            "client": "C1",
            "kind": "canary",
            "row_job_id": None,
            "scheduler_record": {
                "client": "C1",
                "dispatch_line": 1,
                "scheduler_job": 1,
                "terminal_line": 2,
                "worker": "F1",
            },
            "worker": "F1",
        }
    ]
    if saturated:
        claims.extend(
            {
                "client": "C1",
                "kind": "workload",
                "row_job_id": str(job),
                "scheduler_record": {
                    "client": "C1",
                    "dispatch_line": line,
                    "scheduler_job": job,
                    "terminal_line": 100,
                    "worker": worker,
                },
                "worker": worker,
            }
            for job, line, worker in ((1, 10, "F1"), (2, 11, "F2"))
        )
        claims.append(
            {
                "client": "C1",
                "kind": "workload",
                "row_job_id": "3",
                "scheduler_record": {
                    "client": "C1",
                    "dispatch_line": 12,
                    "scheduler_job": 3,
                    "terminal_line": 20,
                    "worker": selected,
                },
                "worker": selected,
            }
        )
    else:
        claims.append(
            {
                "client": "C1",
                "kind": "workload",
                "row_job_id": "1",
                "scheduler_record": {
                    "client": "C1",
                    "dispatch_line": 10,
                    "scheduler_job": 1,
                    "terminal_line": 20,
                    "worker": selected,
                },
                "worker": selected,
            }
        )
    return scenario, plan, claims


def test_assignment_preference_replays_occupancy_and_excludes_canaries() -> None:
    scenario, plan, claims = _preference_fixture()
    result = _assignment_preference(scenario, plan, claims)
    assert result["counts"] == {"checks": 1, "escapes": 0, "preferred": 1, "violations": 0}
    assert result["decisions"][0]["compatible_free_workers"] == ["F1", "F2"]
    assert result["decisions"][0]["dispatch_line"] == 10

    scenario, plan, claims = _preference_fixture(saturated=True)
    result = _assignment_preference(scenario, plan, claims)
    assert result["counts"] == {"checks": 3, "escapes": 1, "preferred": 2, "violations": 0}
    assert result["decisions"][-1]["compatible_free_workers"] == []


def test_assignment_preference_counts_preexposure_reservation_until_loss() -> None:
    scenario, plan, claims = _preference_fixture(selected="F2")
    current = claims[-1]
    current["scheduler_record"]["preexposure_redispatches"] = [
        {
            "lost_dispatch_line": 5,
            "lost_worker": "F1",
            "marker_line": 15,
        }
    ]
    claims.insert(
        -1,
        {
            "client": "C1",
            "kind": "workload",
            "row_job_id": "0",
            "scheduler_record": {
                "client": "C1",
                "dispatch_line": 4,
                "scheduler_job": 2,
                "terminal_line": 20,
                "worker": "F2",
            },
            "worker": "F2",
        },
    )

    result = _assignment_preference(scenario, plan, claims)

    assert result["counts"] == {
        "checks": 2,
        "escapes": 1,
        "preferred": 1,
        "violations": 0,
    }
    assert result["decisions"][-1]["occupancy"] == {"F1": 1, "F2": 1}
    assert result["decisions"][-1]["compatible_free_workers"] == []


def test_assignment_preference_reports_selected_compatible_worker_violation() -> None:
    scenario, plan, claims = _preference_fixture(selected="F1")
    result = _assignment_preference(scenario, plan, claims)
    assert result["counts"]["violations"] == 0
    claims[-1]["scheduler_record"]["terminal_line"] = 30
    claims.insert(
        0,
        {
            "client": "C1",
            "kind": "workload",
            "row_job_id": "0",
            "scheduler_record": {
                "client": "C1",
                "dispatch_line": 5,
                "scheduler_job": 2,
                "terminal_line": 25,
                "worker": "F1",
            },
            "worker": "F1",
        },
    )
    result = _assignment_preference(scenario, plan, claims)
    assert result["violations"] == ["1"]
    verdict_scenario = {
        "images": {"new": "p50s4-fixture"},
        "instances": [
            {"image": "new", "name": "F1", "role": "F", "slots": 1},
            {"image": "new", "name": "F2", "role": "F", "slots": 1},
            {
                "env": {"ICECC_P50_MODE": "on"},
                "image": "new",
                "name": "C1",
                "role": "C",
            },
        ],
    }
    assert _assignment_preference_errors(
        result, verdict_scenario, plan["topology"]
    ) == {"1"}


@pytest.mark.parametrize(
    "missing", ("slots", "scheduler_record", "scheduler_job", "relationship")
)
def test_assignment_preference_refuses_malformed_evidence(missing: str) -> None:
    scenario, plan, claims = _preference_fixture()
    if missing == "slots":
        plan["topology"]["instances"][0]["slots"] = 0
    elif missing == "scheduler_record":
        claims[-1].pop("scheduler_record")
    elif missing == "scheduler_job":
        claims[-1]["scheduler_record"]["scheduler_job"] = True
    else:
        plan["topology"]["relationships"].pop()
    with pytest.raises(CollectError):
        _assignment_preference(scenario, plan, claims)


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(canonical_bytes(value))


def _write_jsonl(path: Path, values: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"".join(canonical_bytes(value) for value in values))


def _raw_collection(tmp_path: Path):
    farm = load_farm_spec(farm_fixture.example_farm_path())
    farm.data["hub"]["results_root"] = str(tmp_path)
    farm.data["corpora"]["fmt-100"]["tus"] = 1
    farm.data["corpora"]["fmt-100"]["repeat"] = 1
    archive = farm.data["corpora"]["fmt-100"]["archives"]["files"]
    archive["files"] = 1
    manifest = f"{SHA}  files/x.ii\n".encode()
    archive["manifest_sha256"] = hashlib.sha256(manifest).hexdigest()
    source_authority = {
        key: value
        for key, value in farm.data["corpora"]["fmt-100"].items()
        if key not in ("archives", "compression")
    }
    source_authority_sha256 = hashlib.sha256(
        canonical_bytes(source_authority)
    ).hexdigest()
    archive["authority_sha256"] = hashlib.sha256(
        canonical_bytes(
            {
                "corpus_authority_sha256": source_authority_sha256,
                "group": "files",
                "manifest_sha256": archive["manifest_sha256"],
            }
        )
    ).hexdigest()
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    plan = farmtest.build_plan(farm, scenario, run_id="collect-fixture")
    root = bundle_root(farm, plan["run_id"])

    bindings = {
        "farm_digest": plan["farm_digest"],
        "run_id": plan["run_id"],
        "scenario_digest": plan["scenario_digest"],
        "topology_digest": plan["topology_digest"],
    }
    used_hosts = {item["host"] for item in plan["topology"]["instances"]}
    _write_json(
        root / "preflight.json",
        {
            **bindings,
            "hosts": {
                host: {"protected": {"bigfarm": 1}} for host in sorted(used_hosts)
            },
            "images": {"authenticated": True},
        },
    )
    _write_json(root / "lifecycle.json", {**bindings, "plan": plan, "status": "UP"})
    _write_json(root / "workload.json", {**bindings, "status": "COMPLETE"})

    instances = {item["name"]: item for item in plan["topology"]["instances"]}
    for name in instances:
        (root / f"{name}.results").mkdir(parents=True)
    diagnostics = root / "diagnostics"
    scheduler = instances["S1"]
    worker = instances["F1"]
    endpoint = f"{worker['address']}:{plan['ports']['instances']['F1']}"
    scheduler_log = diagnostics / scheduler["host"] / "S1.log" / "scheduler.log"
    scheduler_log.parent.mkdir(parents=True)
    scheduler_log.write_text(
        "[1] 2026-09-05 01:00:00: ICECREAM scheduler 1.5.90 starting up, port 23000\n"
        f"[1] 2026-09-05 01:00:01: RELOGIN F1(x86_64): cache={endpoint} "
        "cache_wire=v1 cache_protocol=1 cache_profiles=p29v1 zstd_tu zstd_route\n"
        "[1] 2026-09-05 01:00:02: NEW 1 client=C1 versions=[] /canary.cpp C++ 0\n"
        "[1] 2026-09-05 01:00:02: put 1 in joblist of F1 (will install now)\n"
        "[1] 2026-09-05 01:00:02: BEGIN: 1 client=C1(x86_64) server=F1(x86_64)\n"
        "[1] 2026-09-05 01:00:03: END 1 status=0 server=F1\n"
        "[1] 2026-09-05 01:00:04: NEW 2 client=C1 versions=[] /corpus/files/x.ii C++ 0\n"
        "[1] 2026-09-05 01:00:04: put 2 in joblist of F1\n"
        "[1] 2026-09-05 01:00:04: BEGIN: 2 client=C1(x86_64) server=F1(x86_64)\n"
        "[1] 2026-09-05 01:00:05: END 2 status=0 server=F1\n"
        "[1] 2026-09-05 01:00:06: RELOGIN F1(x86_64): cache=off\n",
        encoding="utf-8",
    )
    worker_log = diagnostics / worker["host"] / "F1.log" / "iceccd.log"
    worker_log.parent.mkdir(parents=True)
    worker_log.write_text(
        "P50 CompileFile attached exact P29V1 input for job 2\n",
        encoding="utf-8",
    )
    _write_json(
        diagnostics / worker["host"] / "F1.inspect",
        {
            "HostConfig": {
                "Init": True,
                "Ulimits": [{"Name": "nofile", "Soft": 65536, "Hard": 65536}],
            }
        },
    )

    client_results = root / "C1.results"
    canary = client_results / "canary"
    canary.mkdir()
    (canary / "F1.client.log").write_text(
        f"ICECC[1] 2026-09-05 01:00:02: Have to use host {endpoint} "
        "- Job ID: 1 - env: x86_64\n",
        encoding="utf-8",
    )
    (canary / "F1.stdout.log").write_text("", encoding="utf-8")
    job = client_results / "workload" / "jobs" / "000001"
    job.mkdir(parents=True)
    (client_results / "workload" / "corpus-manifest.sha256").write_bytes(manifest)
    (job / "result.tsv").write_text(
        "\t".join(
            (
                "1",
                "A",
                "0",
                "files/x.ii",
                "2",
                endpoint,
                "1000",
                "1025",
                "0",
                SHA,
                SHA,
                "1",
                "1",
                "0",
            )
        )
        + "\n",
        encoding="utf-8",
    )
    (job / "client-debug.log").write_text(
        f"ICECC[2] 2026-09-05 01:00:04: Have to use host {endpoint} "
        "- Job ID: 2 - env: x86_64\n"
        "P29V1 source committed for P50 CompileFile: 100 exact bytes, "
        "TU sequence 1\n",
        encoding="utf-8",
    )
    (job / "client-output.log").write_text("", encoding="utf-8")
    workload = client_results / "workload"
    (workload / "oracle-summary.tsv").write_text(
        "sample_total\t1\nsample_mismatches\t0\n", encoding="utf-8"
    )
    (workload / "oracle-samples.tsv").write_text(
        f"files/x.ii\t{SHA}\t{SHA}\t1\n", encoding="utf-8"
    )
    _write_jsonl(
        client_results / "source-result.jsonl",
        [
            {
                "assignment_epoch": 1,
                "assignment_nonce": 1,
                "attempts": 1,
                "c_store_guid": C_GUID,
                "c_to_f_bytes": 321,
                "f_to_c_bytes": 123,
                "logical_job": 2,
                "profile": "P29V1",
                "raw_bytes": 100,
                "raw_digest": "2" * 32,
                "schema": "icecream-p50-source-result-v2",
                "source_mutex_service_ns": 2_000_000,
                "source_mutex_wait_ns": 1_000,
                "status": 0,
                "system_source_reuse": False,
                "terminal_error_code": 0,
                "terminal_error_name": None,
                "tu_seq": 1,
                "wire_job_id": 2,
            }
        ],
    )
    _write_jsonl(
        client_results / "compile-identity.jsonl",
        [
            {
                "assignment_epoch": 1,
                "assignment_nonce": 1,
                "c_guid": 1,
                "job_id": 2,
                "record": "compile-result-identity",
                # Scheduler compile TU sequence and cache-route TU sequence
                # are distinct authenticated domains and need not match.
                "tu_seq": 99,
            }
        ],
    )
    _write_jsonl(
        client_results / "c-action.jsonl",
        [{"action": "COMMIT_ACCEPTED", "c_store_guid": C_GUID, "tu_seq": 1}],
    )
    _write_jsonl(
        root / "F1.results" / "f-action.jsonl",
        [
            {"action": "SESSION_OPENED"},
            {"action": "INPUT_COMMITTED", "c_store_guid": C_GUID, "tu_seq": 1},
        ],
    )
    return farm, scenario, plan, root


@pytest.mark.parametrize("raw_trigger", [False, True])
def test_replay_rejects_rehashed_invented_lost_reply(tmp_path: Path, raw_trigger: bool) -> None:
    from farmharness.integration.collect import _evidence_artifacts, _write_checksums

    farm, scenario, plan, root = _raw_collection(tmp_path)
    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)
    assert plan["source_failure_observation_contract"] == "icefarm-source-failure-observers-v2"
    invented = {"record_count": 1, "records": [{"control_result_received": False}]}
    if raw_trigger:
        # Remove the derived trigger; the retained raw diagnostic must still
        # force comparison, even after the caller recomputes every checksum.
        invented["records"] = [{}]
        debug = next((root / "evidence/instances").glob(
            "*/results/workload/**/client-debug.log"
        ))
        debug.write_text(
            debug.read_text() + "\nP50 cache control operation ended disconnected\n",
            encoding="utf-8",
        )
    bundle["observations"]["failed_p50_source_transfers"] = invented
    (root / "evidence/derived/observations.json").write_text(
        json.dumps(bundle["observations"]), encoding="utf-8"
    )
    bundle["artifacts"] = _evidence_artifacts(root)
    bundle["checksum_policy"]["sha256sums_sha256"] = _write_checksums(
        root, bundle["artifacts"]
    )
    (root / "bundle.json").write_text(json.dumps(bundle), encoding="utf-8")
    with pytest.raises(CollectError, match="lost control reply observations"):
        load_verified_bundle(root)


def test_collection_verdict_report_and_replay_are_reproducible(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)

    assert bundle["rows"] == [
        {
            "c_to_f_bytes": 321,
            "client_instance": "C1",
            "client_version": 50,
            "cs": "F1",
            "cs_version": 50,
            "event_epoch": 0,
            "exact": True,
            "f_to_c_bytes": 123,
            "job_id": "C1:A:1:2",
            "object_sha_local": SHA,
            "object_sha_remote": SHA,
            "retries": 0,
            "reuse": False,
            "schema": "icecream-newgen-farm-acceptance-v1",
            "session_outcome": "committed",
            "tail_present": True,
            "tail_profile": "P29V1",
            "tu": "files/x.ii",
            "wall_ms": 25,
        }
    ]
    assert bundle["observations"]["logins"] == [
        {
            "cache_protocol": 1,
            "cache_profiles": ["P29V1", "ZSTD_TU", "ZSTD_ROUTE"],
            "instance": "F1",
            "protocol": 50,
        }
    ]
    assert bundle["observations"]["sidecars"]["F1"]["process_count"] == 1
    assert bundle["observations"]["source_mutex"] == {
        "record_count": 1,
        "records": [
            {
                "client_instance": "C1",
                "job_id": "C1:A:1:2",
                "outcome": "committed",
                "profile": "P29V1",
                "service_ns": 2_000_000,
                "turn": "A",
                "wait_ns": 1_000,
            }
        ],
        "service_total_ns": 2_000_000,
        "wait_max_ns": 1_000,
        "wait_total_ns": 1_000,
    }
    assert bundle["observations"]["p50_source_routes"] == {
        "record_count": 1,
        "records": [
            {
                "c_store_guid": C_GUID,
                "c_to_f_bytes": 321,
                "client_instance": "C1",
                "f_to_c_bytes": 123,
                "job_id": "C1:A:1:2",
                "profile": "P29V1",
                "raw_bytes": 100,
                "raw_digest": "2" * 32,
                "schema": "icefarm-p50-source-route-v1",
                "tu_seq": 1,
                "worker_instance": "F1",
            }
        ],
    }
    turn = bundle["observations"]["turns"]["A"]
    assert {
        key: turn[key]
        for key in (
            "c_to_f_bytes",
            "exact_objects",
            "f_to_c_bytes",
            "jobs",
            "job_wall_p95_ms",
            "job_wall_p99_ms",
            "source_mutex_records",
            "source_mutex_service_ns",
            "source_mutex_wait_max_ns",
            "source_mutex_wait_ns",
            "wall_ms",
            "wrapper_wall_ms",
        )
    } == {
        "c_to_f_bytes": 321,
        "exact_objects": 1,
        "f_to_c_bytes": 123,
        "jobs": 1,
        "job_wall_p95_ms": 25,
        "job_wall_p99_ms": 25,
        "source_mutex_records": 1,
        "source_mutex_service_ns": 2_000_000,
        "source_mutex_wait_max_ns": 1_000,
        "source_mutex_wait_ns": 1_000,
        "wall_ms": 1_000,
        "wrapper_wall_ms": 25,
    }
    assert bundle["observations"]["warm_hint_overrides"] == {
        "count": 0,
        "events": [],
        "job_ids": [],
    }
    assert bundle["observations"]["scheduler_reconciliation"] == {
        "canary_dispatches": 1,
        "generations": 1,
        "preexposure_redispatches": [],
        "scheduler_dispatches": 2,
        "workload_dispatches": 1,
    }
    lifecycle = bundle["observations"]["job_lifecycle"]
    assert lifecycle[0]["dispatch_ms"] == 1_788_570_004_000
    assert lifecycle[0]["first_dispatch_ms"] == 1_788_570_004_000
    assert lifecycle[0]["final_dispatch_ms"] == 1_788_570_004_000
    assert lifecycle[0]["terminal_ms"] == 1_788_570_005_000
    assert lifecycle[0]["dispatch_ms"] != 1000
    assert lifecycle[0]["terminal_ms"] != 1025
    verdict = verify_bundle(root)
    assert verdict["status"] == "PASS", verdict
    reported, markdown = report_bundle(root)
    assert reported == verdict
    assert "Verdict: **PASS**" in markdown
    assert "Source mutex: records=1; wait=0.001 ms total" in markdown
    assert "Worker distribution: F1=1 (p95=25 ms; p99=25 ms)." in markdown
    assert (
        "Warm affinity overrides of idle compatible workers: 0; jobs: none." in markdown
    )
    assert (root / "witness.json").is_file()
    assert collect_bundle(farm, scenario, plan, sync_remote=False) == bundle
    assert load_verified_bundle(root) == bundle
    replay = farmtest.replay_bundle(root)
    assert replay["status"] == "REPRODUCED"
    assert replay["topology_digest"] == plan["topology_digest"]
    assert replay["resolver_mode"] == "current-v2-resolver"
    assert replay["verdict"] == verdict


@pytest.mark.parametrize("value", (False, None))
def test_collection_refuses_false_or_missing_f_docker_init(
    tmp_path: Path, value: bool | None
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    host = next(
        item["host"]
        for item in plan["topology"]["instances"]
        if item["name"] == "F1"
    )
    inspect = root / "diagnostics" / host / "F1.inspect"
    if value is None:
        inspect.unlink()
    else:
        _write_json(
            inspect,
            {
                "HostConfig": {
                    "Init": value,
                    "Ulimits": [{"Name": "nofile", "Soft": 65536, "Hard": 65536}],
                }
            },
        )
    with pytest.raises(CollectError, match="Init=true|nofile|F Init witnesses|cannot read"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


@pytest.mark.parametrize(
    "ulimits",
    (
        None,
        [],
        [{"Name": "nofile", "Soft": 1024, "Hard": 524288}],
        [{"Name": "nofile", "Soft": 65536, "Hard": 524288}],
        [
            {"Name": "nofile", "Soft": 65536, "Hard": 65536},
            {"Name": "nofile", "Soft": 65536, "Hard": 65536},
        ],
    ),
)
def test_collection_refuses_missing_or_divergent_f_nofile(
    tmp_path: Path, ulimits: object
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    host = next(
        item["host"]
        for item in plan["topology"]["instances"]
        if item["name"] == "F1"
    )
    inspect = root / "diagnostics" / host / "F1.inspect"
    host_config = {"Init": True}
    if ulimits is not None:
        host_config["Ulimits"] = ulimits
    _write_json(inspect, {"HostConfig": host_config})

    with pytest.raises(CollectError, match="nofile=65536:65536"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_verified_bundle_refuses_mutated_evidence(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    collect_bundle(farm, scenario, plan, sync_remote=False)
    result = root / "evidence" / "instances" / "C1" / "results" / "source-result.jsonl"
    result.write_bytes(result.read_bytes() + b"\n")

    with pytest.raises(CollectError, match="checksum mismatch"):
        load_verified_bundle(root)


def test_turn_completeness_binds_the_authenticated_tu_occurrence_multiset(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    result = root / "C1.results" / "workload" / "jobs" / "000001" / "result.tsv"
    fields = result.read_text(encoding="utf-8").rstrip("\n").split("\t")
    fields[3] = "substituted.ii"
    result.write_text("\t".join(fields) + "\n", encoding="utf-8")

    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)
    assert bundle["observations"]["incomplete_turns"] == [
        "C1:A:authenticated-tu-multiset"
    ]
    assert verify_bundle(root)["status"] == "FAIL"


def test_collection_requires_the_authenticated_corpus_manifest(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    (root / "C1.results" / "workload" / "corpus-manifest.sha256").unlink()

    with pytest.raises(CollectError, match="authenticated corpus manifest is absent"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_control_verdict_is_recomputed_from_a_declared_control_bundle(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    bundle = {"scenario": {"controls": ["H2"]}}
    monkeypatch.setattr(report, "load_verified_bundle", lambda _root: bundle)
    monkeypatch.setattr(
        report,
        "evaluate_control",
        lambda control_id, _bundle: {"control": control_id, "status": "PASS"},
    )

    verdict = report.verify_control_bundle(tmp_path)

    assert verdict == {
        "controls": [{"control": "H2", "status": "PASS"}],
        "schema": "icefarm-controls-verdict-v1",
        "status": "PASS",
    }
    assert json.loads((tmp_path / "control-verdict.json").read_text()) == verdict


def test_control_verdict_refuses_an_undeclared_control_bundle(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(
        report,
        "load_verified_bundle",
        lambda _root: {"scenario": {"controls": []}},
    )

    with pytest.raises(ReportError, match="declares no H1-H5 controls"):
        report.verify_control_bundle(tmp_path)


def test_collection_refuses_an_unclaimed_scheduler_dispatch(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    path = root / "diagnostics" / scheduler["host"] / "S1.log" / "scheduler.log"
    path.write_text(
        path.read_text(encoding="utf-8")
        + "[1] 2026-09-05 01:00:07: NEW 3 client=C1 versions=[] /ghost.ii C++ 0\n"
        + "[1] 2026-09-05 01:00:07: put 3 in joblist of F1\n"
        + "[1] 2026-09-05 01:00:08: END 3 status=0 server=F1\n",
        encoding="utf-8",
    )

    with pytest.raises(CollectError, match="keys differ.*extra"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_collection_refuses_a_scheduler_dispatch_without_terminal(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    path = root / "diagnostics" / scheduler["host"] / "S1.log" / "scheduler.log"
    path.write_text(
        path.read_text(encoding="utf-8").replace(
            "[1] 2026-09-05 01:00:05: END 2 status=0 server=F1\n", ""
        ),
        encoding="utf-8",
    )

    with pytest.raises(CollectError, match="dispatch has no terminal"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def _scheduler_loss_log_fixture(tmp_path: Path) -> tuple[Path, dict[str, object]]:
    evidence = tmp_path / "evidence"
    scheduler_log = evidence / "diagnostics" / "h1" / "S1.log" / "scheduler.log"
    scheduler_log.parent.mkdir(parents=True)
    scheduler_log.write_text(
        "[1] 2026-09-10 12:00:00: ICECREAM scheduler 1.5.90 starting up, port 23000\n"
        "[1] 2026-09-10 12:00:01: NEW 3 client=C1 versions=[] /lost.ii C++ 0\n"
        "[1] 2026-09-10 12:00:01: put 3 in joblist of F1\n"
        "[1] 2026-09-10 12:00:01: BEGIN: 3 client=C1(x86_64) server=F1(x86_64)\n"
        "[1] 2026-09-10 12:00:06: ICECREAM scheduler 1.5.90 starting up, port 23000\n"
        "[1] 2026-09-10 12:00:07: NEW 1 client=C1 versions=[] /retry.ii C++ 0\n"
        "[1] 2026-09-10 12:00:07: put 1 in joblist of F1\n"
        "[1] 2026-09-10 12:00:08: END 1 status=0 server=F1\n",
        encoding="utf-8",
    )
    plan: dict[str, object] = {
        "topology": {
            "instances": [
                {"host": "h1", "name": "S1", "role": "S"},
            ]
        }
    }
    return evidence, plan


def test_scheduler_loss_terminal_binds_the_successor_generation_start(
    tmp_path: Path,
) -> None:
    evidence, plan = _scheduler_loss_log_fixture(tmp_path)

    jobs = _scheduler_jobs(
        evidence,
        plan,
        allow_unterminated_job_ids={3},
        allow_unterminated_generations={1},
    )

    lost = jobs[0]
    assert lost["terminal"] == "scheduler-loss"
    assert lost["terminal_line"] == 5
    assert lost["terminal_ms"] == 1_789_041_606_000


def test_scheduler_loss_terminal_requires_a_successor_generation(
    tmp_path: Path,
) -> None:
    evidence, plan = _scheduler_loss_log_fixture(tmp_path)
    scheduler_log = evidence / "diagnostics" / "h1" / "S1.log" / "scheduler.log"
    scheduler_log.write_text(
        "\n".join(scheduler_log.read_text(encoding="utf-8").splitlines()[:4])
        + "\n",
        encoding="utf-8",
    )

    with pytest.raises(CollectError, match="no authenticated successor"):
        _scheduler_jobs(
            evidence,
            plan,
            allow_unterminated_job_ids={3},
            allow_unterminated_generations={1},
        )


def _preexposure_scheduler_fixture(
    tmp_path: Path,
) -> tuple[Path, dict[str, object], list[dict[str, object]], Path]:
    evidence = tmp_path / "evidence"
    scheduler_log = evidence / "diagnostics" / "h1" / "S1.log" / "scheduler.log"
    scheduler_log.parent.mkdir(parents=True)
    scheduler_log.write_text(
        "[1] 2026-09-09 08:01:20: ICECREAM scheduler 1.5.90 starting up, port 23000\n"
        "[1] 2026-09-09 08:01:21: NEW 7 client=C1 versions=[] /x.ii C++ 0\n"
        "[1] 2026-09-09 08:01:22: put 7 in joblist of F1\n"
        "[1] 2026-09-09 08:01:23: redispatch unexposed assignment 7 after worker loss F1\n"
        "[1] 2026-09-09 08:01:24: put 7 in joblist of F2\n"
        "[1] 2026-09-09 08:01:25: BEGIN: 7 client=C1(x86_64) server=F2(x86_64)\n"
        "[1] 2026-09-09 08:01:26: END 7 status=0 server=F2\n",
        encoding="utf-8",
    )
    plan: dict[str, object] = {
        "topology": {
            "instances": [
                {"host": "h1", "name": "S1", "role": "S"},
                {"host": "h1", "name": "F1", "role": "F"},
                {"host": "h2", "name": "F2", "role": "F"},
            ]
        }
    }
    claims: list[dict[str, object]] = [
        {
            "attempt_index": 0,
            "client": "C1",
            "kind": "workload",
            "observed_ms": 1_788_940_884_000,
            "row_job_id": "C1:A:1:7",
            "scheduler_job": 7,
            "worker": "F2",
        }
    ]
    return evidence, plan, claims, scheduler_log


def test_collection_authenticates_preexposure_redispatch_before_usecs(
    tmp_path: Path,
) -> None:
    evidence, plan, claims, _scheduler_log = _preexposure_scheduler_fixture(tmp_path)

    result = _reconcile_scheduler_dispatches(evidence, plan, claims)

    record = claims[0]["scheduler_record"]
    assert isinstance(record, dict)
    assert record["worker"] == "F2"
    assert record["dispatch_line"] == 5
    assert record["terminal"] == "completion"
    assert result == {
        "canary_dispatches": 0,
        "generations": 1,
        "preexposure_redispatches": [
            {
                "attempt_index": 0,
                "client": "C1",
                "generation": 1,
                "lost_dispatch_line": 3,
                "lost_dispatch_ms": 1_788_940_882_000,
                "lost_worker": "F1",
                "marker_line": 4,
                "marker_ms": 1_788_940_883_000,
                "replacement_dispatch_line": 5,
                "replacement_dispatch_ms": 1_788_940_884_000,
                "replacement_worker": "F2",
                "row_job_id": "C1:A:1:7",
                "scheduler_job": 7,
            }
        ],
        "scheduler_dispatches": 1,
        "workload_dispatches": 1,
    }


def test_reconciliation_records_exact_failed_request_dispatch(
    tmp_path: Path,
) -> None:
    evidence, plan, claims, scheduler_log = _preexposure_scheduler_fixture(tmp_path)
    scheduler_log.write_text(
        scheduler_log.read_text(encoding="utf-8")
        + "[1] 2026-09-09 08:01:27: NEW 8 client=C1 versions=[] /ghost.ii C++ 0\n"
        + "[1] 2026-09-09 08:01:27: put 8 in joblist of F1\n"
        + "[1] 2026-09-09 08:01:28: END 8 status=255\n",
        encoding="utf-8",
    )
    requests = [
        {
            "client_instance": "C1",
            "request_finished_ms": 1_788_940_888_000,
            "request_started_ms": 1_788_940_887_000,
            "row_job_id": "C1:A:2:missing-2",
        }
    ]

    result = _reconcile_scheduler_dispatches(
        evidence, plan, claims, unclaimed_requests=requests
    )

    assert result["scheduler_dispatches"] == 2
    assert result["workload_dispatches"] == 2
    assert result["unclaimed_dispatches"] == [
        {
            "client": "C1",
            "dispatch_line": 9,
            "generation": 1,
            "scheduler_job": 8,
            "terminal": "cancellation",
            "worker": "F1",
        }
    ]


def test_reconciliation_allows_authenticated_predispatch_refusal(
    tmp_path: Path,
) -> None:
    evidence, plan, claims, _scheduler_log = _preexposure_scheduler_fixture(tmp_path)
    requests = [
        {
            "client_instance": "C1",
            "request_finished_ms": 1_788_940_888_000,
            "request_started_ms": 1_788_940_887_000,
            "row_job_id": "C1:A:2:missing-2",
        }
    ]

    result = _reconcile_scheduler_dispatches(
        evidence, plan, claims, unclaimed_requests=requests
    )

    assert result.get("unclaimed_dispatches", []) == []
    assert result["scheduler_dispatches"] == result["workload_dispatches"] == 1


@pytest.mark.parametrize("mutation", ("wrong-client", "wrong-time", "duplicate"))
def test_reconciliation_rejects_unbound_failed_request_dispatch(
    tmp_path: Path, mutation: str
) -> None:
    evidence, plan, claims, scheduler_log = _preexposure_scheduler_fixture(tmp_path)
    scheduler_log.write_text(
        scheduler_log.read_text(encoding="utf-8")
        + "[1] 2026-09-09 08:01:27: NEW 8 client=C1 versions=[] /ghost.ii C++ 0\n"
        + "[1] 2026-09-09 08:01:27: put 8 in joblist of F1\n"
        + "[1] 2026-09-09 08:01:28: END 8 status=255\n",
        encoding="utf-8",
    )
    request = {
        "client_instance": "C2" if mutation == "wrong-client" else "C1",
        "request_finished_ms": (
            1_788_940_800_000 if mutation == "wrong-time" else 1_788_940_888_000
        ),
        "request_started_ms": (
            1_788_940_799_000 if mutation == "wrong-time" else 1_788_940_887_000
        ),
        "row_job_id": "C1:A:2:missing-2",
    }
    requests = [request, dict(request)] if mutation == "duplicate" else [request]

    with pytest.raises(CollectError, match="unclaimed scheduler dispatches"):
        _reconcile_scheduler_dispatches(
            evidence, plan, claims, unclaimed_requests=requests
        )


@pytest.mark.parametrize(
    "mutation",
    (
        "missing-marker",
        "wrong-lost-worker",
        "same-replacement-worker",
        "marker-after-begin",
        "dangling-marker",
        "malformed-marker",
    ),
)
def test_collection_refuses_unauthenticated_preexposure_redispatch(
    tmp_path: Path, mutation: str
) -> None:
    evidence, plan, _claims, scheduler_log = _preexposure_scheduler_fixture(tmp_path)
    text = scheduler_log.read_text(encoding="utf-8")
    marker = (
        "[1] 2026-09-09 08:01:23: redispatch unexposed assignment 7 "
        "after worker loss F1\n"
    )
    replacement = "[1] 2026-09-09 08:01:24: put 7 in joblist of F2\n"
    begin = (
        "[1] 2026-09-09 08:01:25: BEGIN: 7 client=C1(x86_64) "
        "server=F2(x86_64)\n"
    )
    terminal = "[1] 2026-09-09 08:01:26: END 7 status=0 server=F2\n"
    if mutation == "missing-marker":
        text = text.replace(marker, "")
    elif mutation == "wrong-lost-worker":
        text = text.replace("worker loss F1", "worker loss F9")
    elif mutation == "same-replacement-worker":
        text = text.replace("joblist of F2", "joblist of F1")
    elif mutation == "marker-after-begin":
        text = text.replace(marker + replacement + begin, replacement + begin + marker)
    elif mutation == "dangling-marker":
        text = text.replace(replacement + begin + terminal, "")
    else:
        text = text.replace("after worker loss F1", "after loss F1")
    scheduler_log.write_text(text, encoding="utf-8")

    with pytest.raises(CollectError):
        _scheduler_jobs(evidence, plan)


def test_collection_refuses_preexposure_redispatch_for_a_canary(
    tmp_path: Path,
) -> None:
    evidence, plan, claims, _scheduler_log = _preexposure_scheduler_fixture(tmp_path)
    claims[0].update(kind="canary", row_job_id=None)

    with pytest.raises(CollectError, match="does not bind a workload assignment"):
        _reconcile_scheduler_dispatches(evidence, plan, claims)


@pytest.mark.parametrize("mutation", ("outside-gate", "missing-event", "late"))
def test_collection_binds_preexposure_redispatch_to_worker_restart(
    tmp_path: Path, mutation: str
) -> None:
    evidence, plan, claims, _scheduler_log = _preexposure_scheduler_fixture(tmp_path)
    result = _reconcile_scheduler_dispatches(evidence, plan, claims)
    scenario = type(
        "Scenario",
        (),
        {
            "data": {
                "expect": {"engagement": "s70-b4-worker-bounces"},
                "workload": {"clients": ["C1"]},
            }
        },
    )()
    events = [
        {
            "action": "restart",
            "fired_ms": 1_788_940_890_000,
            "instance": "F1",
        }
    ]
    if mutation == "outside-gate":
        scenario.data["expect"]["engagement"] = "baseline"
    elif mutation == "missing-event":
        events.clear()
    else:
        events[0]["fired_ms"] = 1_788_940_882_500

    with pytest.raises(CollectError):
        _validate_preexposure_redispatches(
            scenario,
            plan,
            events,
            result["preexposure_redispatches"],
        )


def test_collection_accepts_restart_bound_preexposure_redispatch(
    tmp_path: Path,
) -> None:
    evidence, plan, claims, _scheduler_log = _preexposure_scheduler_fixture(tmp_path)
    result = _reconcile_scheduler_dispatches(evidence, plan, claims)
    scenario = type(
        "Scenario",
        (),
        {
            "data": {
                "expect": {"engagement": "s70-b4-worker-bounces"},
                "workload": {"clients": ["C1"]},
            }
        },
    )()

    _validate_preexposure_redispatches(
        scenario,
        plan,
        [
            {
                "action": "restart",
                "fired_ms": 1_788_940_890_000,
                "instance": "F1",
            }
        ],
        result["preexposure_redispatches"],
    )


def test_collection_refuses_a_wrapper_assignment_without_scheduler_dispatch(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    path = root / "diagnostics" / scheduler["host"] / "S1.log" / "scheduler.log"
    content = path.read_text(encoding="utf-8")
    for line in (
        "[1] 2026-09-05 01:00:04: NEW 2 client=C1 versions=[] /corpus/files/x.ii C++ 0\n",
        "[1] 2026-09-05 01:00:04: put 2 in joblist of F1\n",
        "[1] 2026-09-05 01:00:04: BEGIN: 2 client=C1(x86_64) server=F1(x86_64)\n",
        "[1] 2026-09-05 01:00:05: END 2 status=0 server=F1\n",
    ):
        content = content.replace(line, "")
    path.write_text(content, encoding="utf-8")

    with pytest.raises(CollectError, match="keys differ.*missing"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_collection_refuses_missing_canary_assignment_evidence(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    (root / "C1.results" / "canary" / "F1.client.log").unlink()

    with pytest.raises(CollectError, match="canary assignment log is absent"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_collection_refuses_retained_canary_local_fallback(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    path = root / "C1.results" / "canary" / "F1.client.log"
    path.write_text(
        path.read_text(encoding="utf-8")
        + "ICECC[1] 2026-09-07 10:28:41: got exception Error 15 - write to host failed\n"
        + "ICECC[1] 2026-09-07 10:28:41: <building_local>\n",
        encoding="utf-8",
    )

    with pytest.raises(CollectError, match="readiness canary compiled locally"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_collection_records_authenticated_local_fallback_as_verdict_failure(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    job = root / "C1.results" / "workload" / "jobs" / "000001"
    result = job / "result.tsv"
    fields = result.read_text(encoding="utf-8").rstrip("\n").split("\t")
    fields[-2] = "0"
    result.write_text("\t".join(fields) + "\n", encoding="utf-8")
    (job / "client-debug.log").write_text(
        "ICECC[2] 2026-09-05 01:00:04: Have to use host "
        + fields[5]
        + " - Job ID: 2 - env: x86_64\n"
        + "ICECC[2] 2026-09-05 01:00:04: got exception Error 15 - write to host failed\n"
        + "ICECC[2] 2026-09-05 01:00:04: <building_local>\n",
        encoding="utf-8",
    )
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    scheduler_log = (
        root / "diagnostics" / scheduler["host"] / "S1.log" / "scheduler.log"
    )
    scheduler_text = scheduler_log.read_text(encoding="utf-8")
    scheduler_text = scheduler_text.replace(
        "[1] 2026-09-05 01:00:04: BEGIN: 2 client=C1(x86_64) server=F1(x86_64)\n",
        "",
    ).replace("END 2 status=0 server=F1", "END 2 status=118")
    scheduler_log.write_text(scheduler_text, encoding="utf-8")
    for name in ("source-result.jsonl", "compile-identity.jsonl", "c-action.jsonl"):
        (root / "C1.results" / name).write_text("", encoding="utf-8")
    (root / "F1.results" / "f-action.jsonl").write_text(
        '{"action":"SESSION_OPENED"}\n', encoding="utf-8"
    )
    worker = next(
        item for item in plan["topology"]["instances"] if item["name"] == "F1"
    )
    (
        root / "diagnostics" / worker["host"] / "F1.log" / "iceccd.log"
    ).write_text("", encoding="utf-8")

    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)
    assert bundle["observations"]["local_fallback_job_ids"] == ["C1:A:1:2"]
    verdict = verify_bundle(root)
    assert verdict["status"] == "FAIL"
    assert next(
        clause for clause in verdict["clauses"] if clause["id"] == "jobs.remote-only"
    )["status"] == "FAIL"


def test_collection_refuses_remote_flag_over_local_build_evidence(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    path = (
        root
        / "C1.results"
        / "workload"
        / "jobs"
        / "000001"
        / "client-debug.log"
    )
    path.write_text(
        path.read_text(encoding="utf-8") + "<building_local>\n",
        encoding="utf-8",
    )

    with pytest.raises(CollectError, match="remote flag disagrees"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_canary_evidence_covers_every_workload_client_worker_pair(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    scenario.data["workload"]["clients"].append("C2")
    shutil.copytree(root / "C1.results", root / "instances" / "C1" / "results")
    (root / "instances" / "C2" / "results").mkdir(parents=True)

    with pytest.raises(CollectError, match="C2->F1"):
        _canary_assignment_claims(scenario, plan, root)

    worker = next(
        item for item in plan["topology"]["instances"] if item["name"] == "F1"
    )
    endpoint = f"{worker['address']}:{plan['ports']['instances']['F1']}"
    canary = root / "instances" / "C2" / "results" / "canary"
    canary.mkdir(parents=True)
    (canary / "F1.client.log").write_text(
        f"ICECC[3] 2026-09-05 01:00:06: Have to use host {endpoint} "
        "- Job ID: 3 - env: x86_64\n",
        encoding="utf-8",
    )

    claims = _canary_assignment_claims(scenario, plan, root)
    assert [
        (claim["client"], claim["worker"], claim["scheduler_job"]) for claim in claims
    ] == [("C1", "F1", 1), ("C2", "F1", 3)]


def test_collection_refuses_wrapper_retry_count_without_each_assignment(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    path = root / "C1.results" / "workload" / "jobs" / "000001" / "result.tsv"
    fields = path.read_text(encoding="utf-8").rstrip("\n").split("\t")
    fields[-1] = "1"
    path.write_text("\t".join(fields) + "\n", encoding="utf-8")

    with pytest.raises(CollectError, match="retry count does not match"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_collection_reconciles_every_scheduler_retry_attempt(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    scheduler_log = (
        root / "diagnostics" / scheduler["host"] / "S1.log" / "scheduler.log"
    )
    content = scheduler_log.read_text(encoding="utf-8").replace(
        "[1] 2026-09-05 01:00:05: END 2 status=0 server=F1\n",
        "[1] 2026-09-05 01:00:05: END 2 status=107\n"
        "[1] 2026-09-05 01:00:06: NEW 3 client=C1 versions=[] /corpus/files/x.ii C++ 0\n"
        "[1] 2026-09-05 01:00:06: put 3 in joblist of F1\n"
        "[1] 2026-09-05 01:00:06: BEGIN: 3 client=C1(x86_64) server=F1(x86_64)\n"
        "[1] 2026-09-05 01:00:07: END 3 status=0 server=F1\n",
    )
    scheduler_log.write_text(content, encoding="utf-8")
    results = root / "C1.results"
    (results / "source-result.jsonl").unlink()
    (results / "compile-identity.jsonl").unlink()
    (results / "c-action.jsonl").unlink()
    _write_jsonl(
        results.parent / "F1.results" / "f-action.jsonl", [{"action": "SESSION_OPENED"}]
    )
    job = results / "workload" / "jobs" / "000001"
    endpoint = next(
        f"{item['address']}:{plan['ports']['instances'][item['name']]}"
        for item in plan["topology"]["instances"]
        if item["name"] == "F1"
    )
    (job / "client-debug.log").write_text(
        f"ICECC[2] 2026-09-05 01:00:04: Have to use host {endpoint} "
        "- Job ID: 2 - env: x86_64\n"
        f"ICECC[2] 2026-09-05 01:00:06: Have to use host {endpoint} "
        "- Job ID: 3 - env: x86_64\n",
        encoding="utf-8",
    )
    result_path = job / "result.tsv"
    fields = result_path.read_text(encoding="utf-8").rstrip("\n").split("\t")
    fields[4] = "3"
    fields[-1] = "1"
    result_path.write_text("\t".join(fields) + "\n", encoding="utf-8")

    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)
    assert bundle["rows"][0]["job_id"] == "C1:A:1:3"
    assert bundle["rows"][0]["retries"] == 1
    assert bundle["observations"]["scheduler_reconciliation"] == {
        "canary_dispatches": 1,
        "generations": 1,
        "preexposure_redispatches": [],
        "scheduler_dispatches": 3,
        "workload_dispatches": 2,
    }
    lifecycle = bundle["observations"]["job_lifecycle"][0]
    assert lifecycle["dispatch_ms"] == 1_788_570_004_000
    assert lifecycle["first_dispatch_ms"] == 1_788_570_004_000
    assert lifecycle["final_dispatch_ms"] == 1_788_570_006_000
    assert lifecycle["terminal_ms"] == 1_788_570_007_000


def test_scheduler_stop_is_a_real_process_loss_terminal(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    scheduler_log = (
        root / "diagnostics" / scheduler["host"] / "S1.log" / "scheduler.log"
    )
    scheduler_log.write_text(
        scheduler_log.read_text(encoding="utf-8").replace(
            "[1] 2026-09-05 01:00:05: END 2 status=0 server=F1\n",
            "[1] 2026-09-05 01:00:05: STOP (DAEMON2) FOR 2\n",
        ),
        encoding="utf-8",
    )
    result = root / "C1.results" / "workload" / "jobs" / "000001" / "result.tsv"
    fields = result.read_text(encoding="utf-8").rstrip("\n").split("\t")
    fields[8] = "1"
    fields[9] = "0" * 64
    fields[11] = "0"
    result.write_text("\t".join(fields) + "\n", encoding="utf-8")

    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)
    lifecycle = bundle["observations"]["job_lifecycle"][0]
    assert lifecycle["terminal"] == "process-loss-recovery"
    assert lifecycle["terminal_ms"] == 1_788_570_005_000


def _late_result_binding_fixture(tmp_path: Path):
    _farm, scenario, _plan, _root = _raw_collection(tmp_path)
    scenario.data["id"] = "S70-b4-worker-bounces"
    scenario.data["expect"]["engagement"] = "s70-b4-worker-bounces"
    row = {
        "cs": "F1",
        "exact": True,
        "job_id": "C1:A:1:2",
        "retries": 0,
        "session_outcome": "committed",
        "tail_present": True,
        "tail_profile": "P29V1",
    }
    raw = {
        "compile_rc": 0,
        "exact": 1,
        "local_build": False,
        "remote": 1,
    }
    records = [
        {
            "dispatch_ms": 9_000,
            "generation": 1,
            "scheduler_job": 2,
            "terminal": "process-loss-recovery",
            "terminal_ms": 9_500,
            "worker": "F1",
        }
    ]
    events = [
        {
            "action": "restart",
            "event_index": 0,
            "fired_ms": 10_000,
            "instance": "F1",
            "receipt": {
                "schema": "icefarm-worker-restart-v1",
                "coordination": {
                    "scheduler_rejoin": {
                        "loss_job_ids": [2],
                        "target": "F1",
                    }
                },
            },
        }
    ]
    return scenario, row, raw, records, events


def test_strict_p50_late_result_binds_exact_declared_worker_loss(
    tmp_path: Path,
) -> None:
    scenario, row, raw, records, events = _late_result_binding_fixture(tmp_path)
    assert _successful_strict_p50_late_result_binding(
        scenario, row, raw, records, events
    ) == {
        "attempt_index": 0,
        "dispatch_ms": 9_000,
        "generation": 1,
        "job_id": "C1:A:1:2",
        "restart_event_index": 0,
        "restart_fired_ms": 10_000,
        "scheduler_job": 2,
        "terminal_ms": 9_500,
        "worker": "F1",
    }


@pytest.mark.parametrize(
    "mutation",
    (
        "other-scenario",
        "not-exact",
        "retry",
        "not-p29",
        "not-committed",
        "local-build",
        "completion-terminal",
        "wrong-worker",
        "missing-loss-job",
        "outside-boundary",
    ),
)
def test_strict_p50_late_result_binding_fails_closed(
    tmp_path: Path, mutation: str
) -> None:
    scenario, row, raw, records, events = _late_result_binding_fixture(tmp_path)
    if mutation == "other-scenario":
        scenario.data["id"] = "S00-smoke"
    elif mutation == "not-exact":
        row["exact"] = False
    elif mutation == "retry":
        row["retries"] = 1
    elif mutation == "not-p29":
        row["tail_profile"] = "ZSTD_TU"
    elif mutation == "not-committed":
        row["session_outcome"] = "fallback"
    elif mutation == "local-build":
        raw["local_build"] = True
    elif mutation == "completion-terminal":
        records[0]["terminal"] = "completion"
    elif mutation == "wrong-worker":
        events[0]["instance"] = "F2"
    elif mutation == "missing-loss-job":
        events[0]["receipt"]["coordination"]["scheduler_rejoin"][
            "loss_job_ids"
        ] = []
    else:
        records[0]["terminal_ms"] = 10_001

    assert (
        _successful_strict_p50_late_result_binding(
            scenario, row, raw, records, events
        )
        is None
    )


def test_collection_refuses_unbound_successful_wrapper_process_loss(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    scheduler_log = (
        root / "diagnostics" / scheduler["host"] / "S1.log" / "scheduler.log"
    )
    scheduler_log.write_text(
        scheduler_log.read_text(encoding="utf-8").replace(
            "[1] 2026-09-05 01:00:05: END 2 status=0 server=F1\n",
            "[1] 2026-09-05 01:00:05: STOP (DAEMON2) FOR 2\n",
        ),
        encoding="utf-8",
    )

    with pytest.raises(CollectError, match="wrapper status disagrees"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_control_observations_require_actual_kill_switch_and_process_loss(
    tmp_path: Path,
) -> None:
    farm, scenario, _plan, _root = _raw_collection(tmp_path)
    del farm
    scenario.data["controls"] = ["H2", "H5"]
    client = next(item for item in scenario.data["instances"] if item["name"] == "C1")
    client.setdefault("env", {})["ICECC_P50_MODE"] = "off"
    raw_jobs = [
        {
            "assignment_claims": [
                {
                    "scheduler_record": {
                        "scheduler_job": 7,
                        "terminal": "process-loss-recovery",
                        "worker": "F1",
                    }
                }
            ],
            "row_job_id": "C1:A:1:2",
        }
    ]

    observations = _control_observations(
        scenario,
        tmp_path / "unused-evidence",
        raw_jobs,
        [{"action": "kill -9", "instance": "F1"}],
    )

    assert observations == {
        "fault": {
            "client_kill_switch": True,
            "worker_killed_job_ids": ["C1:A:1:2"],
        },
        "object_corruption": None,
        "process_loss_recovery_bindings": [
            {
                "attempt_index": 0,
                "job_id": "C1:A:1:2",
                "scheduler_job": 7,
                "worker": "F1",
            }
        ],
        "process_loss_recovery_job_ids": ["C1:A:1:2"],
    }


def test_control_observations_do_not_self_attest_named_controls(
    tmp_path: Path,
) -> None:
    _farm, scenario, _plan, _root = _raw_collection(tmp_path)
    scenario.data["controls"] = ["H2", "H5"]
    raw_jobs = [
        {
            "assignment_claims": [
                {
                    "scheduler_record": {
                        "scheduler_job": 8,
                        "terminal": "process-loss-recovery",
                        "worker": "F2",
                    }
                }
            ],
            "row_job_id": "C1:A:1:2",
        }
    ]

    observations = _control_observations(
        scenario,
        tmp_path / "unused-evidence",
        raw_jobs,
        [{"action": "kill -9", "instance": "F1"}],
    )

    assert observations["fault"] == {
        "client_kill_switch": False,
        "worker_killed_job_ids": [],
    }
    assert observations["process_loss_recovery_job_ids"] == ["C1:A:1:2"]
    assert observations["process_loss_recovery_bindings"] == [
        {
            "attempt_index": 0,
            "job_id": "C1:A:1:2",
            "scheduler_job": 8,
            "worker": "F2",
        }
    ]


def test_h4_observation_authenticates_before_and_after_job_digests(
    tmp_path: Path,
) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "H4-corrupt-object.json", farm
    )
    evidence = tmp_path / "evidence"
    receipt = (
        evidence
        / "instances"
        / "C1"
        / "results"
        / "workload"
        / "A"
        / "fault"
        / "h4.tsv"
    )
    receipt.parent.mkdir(parents=True)
    before = "a" * 64
    after = "b" * 64
    receipt.write_text(
        f"icefarm-h4-object-fault-v1\tC1\t1\t{before}\t{after}\tjobs/000001/remote.o\n",
        encoding="ascii",
    )
    raw_jobs = [
        {
            "assignment_claims": [],
            "client": "C1",
            "compile_rc": 0,
            "exact": 0,
            "index": 1,
            "local_sha": before,
            "remote_sha": after,
            "row_job_id": "C1:A:1:7",
            "turn": "A",
        }
    ]

    observations = _control_observations(scenario, evidence, raw_jobs, [])

    assert observations["fault"]["corrupt_object"] is True
    assert observations["object_corruption"] == {
        "after_sha256": after,
        "before_sha256": before,
        "client": "C1",
        "job": 1,
        "row_job_id": "C1:A:1:7",
    }
    receipt.write_text(
        receipt.read_text(encoding="ascii").replace(after, "c" * 64),
        encoding="ascii",
    )
    with pytest.raises(CollectError, match="digests differ"):
        _control_observations(scenario, evidence, raw_jobs, [])


def test_collection_reconciles_reused_job_id_after_scheduler_restart(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    scheduler_log = (
        root / "diagnostics" / scheduler["host"] / "S1.log" / "scheduler.log"
    )
    scheduler_log.write_text(
        scheduler_log.read_text(encoding="utf-8").replace(
            "[1] 2026-09-05 01:00:06: RELOGIN F1(x86_64): cache=off\n",
            "[1] 2026-09-05 01:00:06: ICECREAM scheduler 1.5.90 starting up, "
            "port 23000\n"
            "[1] 2026-09-05 01:00:07: NEW 2 client=C1 versions=[] "
            "/corpus/files/x.ii C++ 0\n"
            "[1] 2026-09-05 01:00:07: put 2 in joblist of F1\n"
            "[1] 2026-09-05 01:00:07: BEGIN: 2 client=C1(x86_64) "
            "server=F1(x86_64)\n"
            "[1] 2026-09-05 01:00:08: END 2 status=0 server=F1\n"
            "[1] 2026-09-05 01:00:09: RELOGIN F1(x86_64): cache=off\n",
        ),
        encoding="utf-8",
    )
    job = root / "C1.results" / "workload" / "jobs" / "000001"
    endpoint = next(
        f"{item['address']}:{plan['ports']['instances'][item['name']]}"
        for item in plan["topology"]["instances"]
        if item["name"] == "F1"
    )
    debug = job / "client-debug.log"
    debug.write_text(
        debug.read_text(encoding="utf-8").replace(
            "P29V1 source committed",
            f"ICECC[2] 2026-09-05 01:00:07: Have to use host {endpoint} "
            "- Job ID: 2 - env: x86_64\nP29V1 source committed",
        ),
        encoding="utf-8",
    )
    result = job / "result.tsv"
    fields = result.read_text(encoding="utf-8").rstrip("\n").split("\t")
    fields[-1] = "1"
    result.write_text("\t".join(fields) + "\n", encoding="utf-8")

    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)
    assert bundle["observations"]["scheduler_reconciliation"] == {
        "canary_dispatches": 1,
        "generations": 2,
        "preexposure_redispatches": [],
        "scheduler_dispatches": 3,
        "workload_dispatches": 2,
    }
    lifecycle = bundle["observations"]["job_lifecycle"][0]
    # The logical lifecycle starts at the first attempt and terminates at the
    # final attempt, even when the scheduler reuses its numeric ID.
    assert lifecycle["dispatch_ms"] == 1_788_570_004_000
    assert lifecycle["first_dispatch_ms"] == 1_788_570_004_000
    assert lifecycle["final_dispatch_ms"] == 1_788_570_007_000
    assert lifecycle["terminal_ms"] == 1_788_570_008_000


@pytest.mark.parametrize(
    ("field", "replacement", "message"),
    (
        ("images", {}, "bundle images differ"),
        ("instances", [], "bundle instances differ"),
        ("run_id", "another-run", "bundle run_id differs"),
    ),
)
def test_verified_bundle_refuses_unbound_duplicate_fields(
    tmp_path: Path, field: str, replacement: object, message: str
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    collect_bundle(farm, scenario, plan, sync_remote=False)
    path = root / "bundle.json"
    bundle = json.loads(path.read_text(encoding="utf-8"))
    bundle[field] = replacement
    _write_json(path, bundle)

    with pytest.raises(CollectError, match=message):
        load_verified_bundle(root)


def test_collection_refuses_a_marker_without_exact_source_result(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    (root / "C1.results" / "source-result.jsonl").unlink()

    with pytest.raises(
        CollectError, match="has no full-identity source-result witness"
    ):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def _make_failed_p50_transport_fixture(root: Path) -> None:
    results = root / "C1.results"
    identity = results / "compile-identity.jsonl"
    identity.write_text("", encoding="utf-8")
    job = results / "workload" / "jobs" / "000001"
    debug = job / "client-debug.log"
    debug.write_text(
        "P50 assignment identity bound for job 2 epoch 1 nonce 1 "
        "c_guid 1 tu_seq 99\n"
        + debug.read_text(encoding="utf-8")
        + "normalizing P50 client error 14 to Error 106 for a fresh assignment\n",
        encoding="utf-8",
    )
    result = job / "result.tsv"
    fields = result.read_text(encoding="utf-8").rstrip("\n").split("\t")
    fields[8] = "100"
    fields[9] = "b" * 64
    fields[11] = "0"
    result.write_text("\t".join(fields) + "\n", encoding="utf-8")
    workload = results / "workload"
    (workload / "oracle-summary.tsv").write_text(
        "sample_total\t1\nsample_mismatches\t1\n", encoding="utf-8"
    )
    (workload / "oracle-samples.tsv").write_text(
        f"files/x.ii\t{SHA}\t{'b' * 64}\t0\n", encoding="utf-8"
    )


def test_failed_transport_without_result_identity_collects_as_fail(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    _make_failed_p50_transport_fixture(root)
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    scheduler_log = (
        root / "diagnostics" / scheduler["host"] / "S1.log" / "scheduler.log"
    )
    scheduler_log.write_text(
        scheduler_log.read_text(encoding="utf-8").replace(
            "END 2 status=0 server=F1", "END 2 status=151"
        ),
        encoding="utf-8",
    )

    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)

    assert bundle["rows"][0]["exact"] is False
    assert bundle["rows"][0]["session_outcome"] == "failed"
    assert bundle["observations"]["compile_failure_job_ids"] == ["C1:A:1:2"]
    assert bundle["observations"]["failed_p50_result_identities"] == {
        "record_count": 1,
        "records": [
            {
                "assignment_epoch": 1,
                "assignment_nonce": 1,
                "attempt_index": 0,
                "reason": "result-stream-loss",
                "result_identity_present": False,
                "row_job_id": "C1:A:1:2",
                "scheduler_job": 2,
                "worker": "F1",
            }
        ],
    }
    assert verify_bundle(root)["status"] == "FAIL"


@pytest.mark.parametrize("interval", (
    False, True, "missing", "wrong-id", "wrong-host", "wrong-instance",
    "wrong-name", "wrong-signal", "wrong-schema", "reversed", "boolean-time",
    "after-event", "no-overlap", "retained-id", "retained-label",
))
def test_killed_worker_result_stream_loss_collects_without_inventing_success(
    tmp_path: Path, interval: bool | str,
) -> None:
    """A real scheduler STOP and client EOF are compatible loss witnesses."""
    farm, scenario, old_plan, root = _raw_collection(tmp_path)
    _make_failed_p50_transport_fixture(root)
    scenario.data["timeline"] = [
        {"action": "kill -9", "instance": "F1", "trigger": "job 2"}
    ]
    plan = farmtest.build_plan(farm, scenario, run_id=old_plan["run_id"])
    if not interval:
        plan.pop("kill_timing_contract")  # Retained pre-interval plan semantics.
    worker = next(i for i in plan["topology"]["instances"] if i["name"] == "F1")
    inspect = json.loads((root / "diagnostics" / worker["host"] / "F1.inspect").read_text())
    if interval:
        inspect.update(Id="3" * 64, Name=f"/icefarm-{plan['run_id']}-F1")
        inspect.setdefault("Config", {})["Labels"] = {
            "icefarm.run": plan["run_id"], "icefarm.instance": "F1",
        }
        _write_json(root / "diagnostics" / worker["host"] / "F1.inspect", inspect)
    for name in ("preflight", "lifecycle", "workload"):
        path = root / f"{name}.json"
        receipt = json.loads(path.read_text(encoding="utf-8"))
        for key in ("farm_digest", "scenario_digest", "topology_digest"):
            receipt[key] = plan[key]
        if name == "lifecycle":
            receipt["plan"] = plan
        _write_json(path, receipt)
    _write_json(
        root / "events" / "events.json",
        {"events": [{
            "action": "kill -9", "instance": "F1", "trigger": "job 2",
            "event_epoch": 1, "event_index": 0,
            "fired_ms": 1_788_570_006_500 if interval else 1_788_570_005_500,
            "last_dispatched_job": 2,
            "workload_dispatch_count": 1,
            **({"receipt": {
                "schema": "icefarm-kill-interval-v1", "instance": "F1",
                "host": worker["host"], "container_id": inspect["Id"],
                "container_name": inspect["Name"], "signal": "KILL",
                "started_ms": 1_788_570_005_500,
                "completed_ms": 1_788_570_006_500,
            }} if interval else {}),
        }]},
    )
    scheduler = next(i for i in plan["topology"]["instances"] if i["role"] == "S")
    log = root / "diagnostics" / scheduler["host"] / "S1.log" / "scheduler.log"
    log.write_text(log.read_text(encoding="utf-8").replace(
        "END 2 status=0 server=F1", "STOP (DAEMON2) FOR 2"
    ), encoding="utf-8")
    result = root / "C1.results" / "workload" / "jobs" / "000001" / "result.tsv"
    fields = result.read_text(encoding="utf-8").rstrip("\n").split("\t")
    fields[7] = "1788570007000"
    result.write_text("\t".join(fields) + "\n", encoding="utf-8")

    if isinstance(interval, str):
        event_path = root / "events" / "events.json"
        document = json.loads(event_path.read_text())
        event = document["events"][0]
        changes = {
            "wrong-id": ("container_id", "4" * 64),
            "wrong-host": ("host", "unplanned-host"),
            "wrong-instance": ("instance", "F2"),
            "wrong-name": ("container_name", "/wrong-container"),
            "wrong-signal": ("signal", "TERM"),
            "wrong-schema": ("schema", "unknown"),
            "reversed": ("started_ms", 1_788_570_006_501),
            "boolean-time": ("started_ms", True),
            "after-event": ("completed_ms", 1_788_570_006_501),
            "no-overlap": ("started_ms", 1_788_570_006_000),
        }
        if interval == "missing":
            del event["receipt"]
        elif interval in changes:
            key, value = changes[interval]
            event["receipt"][key] = value
        else:
            if interval == "retained-id":
                inspect["Id"] = "4" * 64
            else:
                inspect["Config"]["Labels"]["icefarm.run"] = "another-run"
            _write_json(root / "diagnostics" / worker["host"] / "F1.inspect", inspect)
        _write_json(event_path, document)
        with pytest.raises(CollectError):
            collect_bundle(farm, scenario, plan, sync_remote=False)
        assert not (root / "bundle.json").exists()
        return

    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)

    assert bundle["observations"]["failed_p50_result_identities"]["records"][0][
        "reason"
    ] == "result-stream-loss"
    assert bundle["observations"]["assignment_lifecycle"][0]["attempts"][0][
        "terminal"
    ] == "process-loss-recovery"
    assert bundle["rows"][0]["exact"] is False
    assert verify_bundle(root)["status"] == "FAIL"


@pytest.mark.parametrize(
    "mutation",
    (
        None, "last-millisecond", "wrong-row", "wrong-job", "wrong-worker",
        "wrong-terminal", "wrong-reason", "result-present", "no-kill",
        "other-action", "other-worker", "before-start", "after-finish",
        "before-dispatch", "after-terminal-second", "duplicate-kill",
    ),
)
def test_result_stream_kill_loss_requires_exact_assignment_and_interval(
    mutation: str | None,
) -> None:
    missing = {
        "reason": "result-stream-loss", "result_identity_present": False,
        "row_job_id": "C1:A:24:28", "scheduler_job": 25, "worker": "F1",
    }
    raw = {"row_job_id": "C1:A:24:28", "started": 1000, "finished": 10000}
    record = {
        "scheduler_job": 25, "worker": "F1", "terminal": "process-loss-recovery",
        "dispatch_ms": 2000, "terminal_ms": 5000,
    }
    events = [{"action": "kill -9", "instance": "F1", "fired_ms": 5500}]
    if mutation == "last-millisecond":
        events[0]["fired_ms"] = 5999
    elif mutation == "wrong-row":
        missing["row_job_id"] = "C1:A:25:28"
    elif mutation == "wrong-job":
        missing["scheduler_job"] = 26
    elif mutation == "wrong-worker":
        missing["worker"] = "F2"
    elif mutation == "wrong-terminal":
        record["terminal"] = "cancellation"
    elif mutation == "wrong-reason":
        missing["reason"] = "worker-restart-loss"
    elif mutation == "result-present":
        missing["result_identity_present"] = True
    elif mutation == "no-kill":
        events = []
    elif mutation == "other-action":
        events[0]["action"] = "restart"
    elif mutation == "other-worker":
        events[0]["instance"] = "F2"
    elif mutation == "before-start":
        raw["started"] = 6000
    elif mutation == "after-finish":
        raw["finished"] = 5000
    elif mutation == "before-dispatch":
        record["dispatch_ms"] = 6000
    elif mutation == "after-terminal-second":
        events[0]["fired_ms"] = 6000
    elif mutation == "duplicate-kill":
        events.append(dict(events[0]))

    assert _result_stream_kill_loss(missing, raw, record, events) is (
        mutation in (None, "last-millisecond")
    )


@pytest.mark.parametrize(
    "mutation", ("successful", "wrong-nonce", "wrong-digest", "no-error106")
)
def test_missing_result_identity_never_authenticates_ambiguous_or_successful_row(
    tmp_path: Path, mutation: str
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    _make_failed_p50_transport_fixture(root)
    job = root / "C1.results" / "workload" / "jobs" / "000001"
    if mutation == "successful":
        result = job / "result.tsv"
        fields = result.read_text(encoding="utf-8").rstrip("\n").split("\t")
        fields[8] = "0"
        fields[9] = SHA
        fields[11] = "1"
        result.write_text("\t".join(fields) + "\n", encoding="utf-8")
    elif mutation == "wrong-nonce":
        debug = job / "client-debug.log"
        debug.write_text(
            debug.read_text(encoding="utf-8").replace("nonce 1", "nonce 2"),
            encoding="utf-8",
        )
    elif mutation == "wrong-digest":
        source_path = root / "C1.results" / "source-result.jsonl"
        source = json.loads(source_path.read_text(encoding="utf-8"))
        source["raw_bytes"] = 101
        _write_jsonl(source_path, [source])
    else:
        debug = job / "client-debug.log"
        debug.write_text(
            debug.read_text(encoding="utf-8").replace(
                "normalizing P50 client error 14 to Error 106 for a fresh assignment\n",
                "",
            ),
            encoding="utf-8",
        )

    with pytest.raises(CollectError, match="full-identity source-result witness"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_worker_restart_loss_is_the_only_restart_identity_exception() -> None:
    source = {
        **_source_result_record(),
        "logical_job": 17,
        "wire_job_id": 17,
        "raw_bytes": 100,
        "raw_digest": "2" * 32,
        "status": 0,
        "terminal_error_code": 0,
        "terminal_error_name": None,
        "c_to_f_bytes": 321,
        "f_to_c_bytes": 123,
        "source_mutex_service_ns": 2_000_000,
        "system_source_reuse": False,
        "tu_seq": 1,
    }
    marker = {"line": 3, "profile": "P29V1", "raw_bytes": 100, "tu_seq": 1}
    kwargs = {
        "raw": {"compile_rc": 105, "remote": 1},
        "final_attempt": True,
        "local_build": False,
        "log_text": "",
        "events": [_worker_restart_loss_event(scheduler_job=17)],
        "assignment": {"scheduler_job": 17, "worker": "F1"},
        "assignment_identity": (17, 1, 1),
        "source_key": (17, 1, 1),
        "source": source,
        "marker": marker,
        "c_commits": {(C_GUID, 1)},
        "f_commits": {"F1": {(C_GUID, 1)}},
    }
    assert _missing_compile_result_identity_reason(**kwargs) == "worker-restart-loss"
    kwargs["events"] = [_worker_restart_loss_event(scheduler_job=18)]
    assert _missing_compile_result_identity_reason(**kwargs) is None


def _make_fresh_p50_retry_fixture(plan: dict[str, object], root: Path) -> None:
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    scheduler_log = (
        root / "diagnostics" / scheduler["host"] / "S1.log" / "scheduler.log"
    )
    scheduler_log.write_text(
        scheduler_log.read_text(encoding="utf-8").replace(
            "[1] 2026-09-05 01:00:06: RELOGIN F1(x86_64): cache=off\n",
            "[1] 2026-09-05 01:00:06: NEW 3 client=C1 versions=[] "
            "/corpus/files/x.ii C++ 0\n"
            "[1] 2026-09-05 01:00:06: put 3 in joblist of F1\n"
            "[1] 2026-09-05 01:00:06: BEGIN: 3 client=C1(x86_64) "
            "server=F1(x86_64)\n"
            "[1] 2026-09-05 01:00:07: END 3 status=0 server=F1\n"
            "[1] 2026-09-05 01:00:08: RELOGIN F1(x86_64): cache=off\n",
        ),
        encoding="utf-8",
    )
    results = root / "C1.results"
    job = results / "workload" / "jobs" / "000001"
    endpoint = next(
        f"{item['address']}:{plan['ports']['instances'][item['name']]}"
        for item in plan["topology"]["instances"]
        if item["name"] == "F1"
    )
    (job / "client-debug.log").write_text(
        "P50 assignment identity bound for job 2 epoch 1 nonce 1 "
        "c_guid 1 tu_seq 1\n"
        f"ICECC[2] 2026-09-05 01:00:04: Have to use host {endpoint} "
        "- Job ID: 2 - env: x86_64\n"
        "P29V1 source committed for P50 CompileFile: 100 exact bytes, "
        "TU sequence 1\n"
        "normalizing P50 client error 14 to Error 106 for a fresh assignment\n"
        "P50 assignment identity bound for job 3 epoch 1 nonce 2 "
        "c_guid 1 tu_seq 2\n"
        f"ICECC[3] 2026-09-05 01:00:06: Have to use host {endpoint} "
        "- Job ID: 3 - env: x86_64\n"
        "P29V1 source committed for P50 CompileFile: 100 exact bytes, "
        "TU sequence 2\n",
        encoding="utf-8",
    )
    result_path = job / "result.tsv"
    fields = result_path.read_text(encoding="utf-8").rstrip("\n").split("\t")
    fields[4] = "3"
    fields[-1] = "1"
    result_path.write_text("\t".join(fields) + "\n", encoding="utf-8")

    sources = [json.loads((results / "source-result.jsonl").read_text())]
    sources.append(
        {
            **sources[0],
            "assignment_nonce": 2,
            "logical_job": 3,
            "tu_seq": 2,
            "wire_job_id": 3,
        }
    )
    _write_jsonl(results / "source-result.jsonl", sources)
    _write_jsonl(
        results / "compile-identity.jsonl",
        [
            {
                "assignment_epoch": 1,
                "assignment_nonce": 2,
                "c_guid": 1,
                "job_id": 3,
                "record": "compile-result-identity",
                "tu_seq": 99,
            }
        ],
    )
    actions = [
        {"action": "COMMIT_ACCEPTED", "c_store_guid": C_GUID, "tu_seq": tu_seq}
        for tu_seq in (1, 2)
    ]
    _write_jsonl(results / "c-action.jsonl", actions)
    _write_jsonl(
        root / "F1.results" / "f-action.jsonl",
        [
            {"action": "SESSION_OPENED"},
            *[
                {
                    "action": "INPUT_COMMITTED",
                    "c_store_guid": C_GUID,
                    "tu_seq": tu_seq,
                }
                for tu_seq in (1, 2)
            ],
        ],
    )
    worker = next(
        item for item in plan["topology"]["instances"] if item["name"] == "F1"
    )
    (
        root / "diagnostics" / worker["host"] / "F1.log" / "iceccd.log"
    ).write_text(
        "P50 CompileFile attached exact P29V1 input for job 2\n"
        "P50 CompileFile attached exact P29V1 input for job 3\n",
        encoding="utf-8",
    )


def _source_transfer_failure_kwargs(
    *, mutation: str | None = None, disconnected: bool = False
) -> dict[str, object]:
    first_endpoint = "10.0.27.101:23003"
    retry_endpoint = (
        first_endpoint if mutation == "same-endpoint" else "10.0.27.56:23004"
    )
    failure = (
        "P29V1 cache source transfer failed closed (status 2, error 4, attempts 0)"
    )
    if disconnected:
        failure = (
            "P50 cache control operation ended disconnected\n"
            "P29V1 cache source transfer failed closed (status 2, error 7, attempts 0)"
        )
    retry = (
        "P50 assignment failed; requesting one fresh strict-P50 remote assignment; "
        f"avoiding failed endpoint {first_endpoint}"
    )
    if mutation == "wrong-failed-endpoint":
        retry = retry.replace(first_endpoint, "10.0.27.99:23999")
    elif mutation == "status-zero":
        failure = failure.replace("status 2", "status 0")
    elif mutation == "missing-retry":
        retry = "retry marker absent"
    elif mutation == "duplicate-failure":
        failure += "\n" + failure
    elif mutation == "reversed-markers":
        failure, retry = retry, failure
    retry_identity_job = 4 if mutation == "wrong-retry-identity" else 3
    first_identity = (
        "P50 assignment identity bound for job 2 epoch 1 nonce 1 "
        "c_guid 1 tu_seq 9\n"
    )
    if mutation == "duplicate-first-identity":
        first_identity += first_identity
    log = (
        first_identity
        + f"ICECC[2] 2026-09-05 01:00:04: Have to use host {first_endpoint} "
        "- Job ID: 2 - env: x86_64\n"
        f"{failure}\n"
        f"{retry}\n"
        f"P50 assignment identity bound for job {retry_identity_job} epoch 1 "
        "nonce 2 c_guid 1 tu_seq 10\n"
        f"ICECC[3] 2026-09-05 01:00:06: Have to use host {retry_endpoint} "
        "- Job ID: 3 - env: x86_64\n"
    )
    assignments = _client_assignments(log, "source-transfer-fixture")
    assignments[0]["worker"] = "F1"
    assignments[1]["worker"] = "F2" if retry_endpoint != first_endpoint else "F1"
    identities = [
        _p50_assignment_identity_evidence(
            log,
            assignment["scheduler_job"],
            after_line=(assignments[index - 1]["line"] if index else 0),
            before_line=assignment["line"] + 1,
        )
        for index, assignment in enumerate(assignments)
    ]
    source_results = (
        {(2, 1, 1): {"status": 3, "attempts": 0, "profile": "P29V1"}}
        if mutation == "noncommitted-source-result"
        else {(2, 1, 1): {"status": 3, "attempts": 0, "profile": "ZSTD_TU"}}
        if mutation == "noncommitted-source-wrong-profile"
        else {(2, 1, 1): {"status": 3, "attempts": 1, "profile": "P29V1"}}
        if mutation == "noncommitted-source-wrong-attempts"
        else {(2, 1, 1): {"status": 0}}
        if mutation == "committed-source-result"
        else {(2, 1, 1): {}}
        if mutation == "malformed-source-result"
        else {}
    )
    return {
        "assignment": assignments[0],
        "assignment_identity": identities[0],
        "attempt_index": 0,
        "compile_identities": {},
        "log_text": log,
        "retry_assignment": assignments[1],
        "retry_identity": identities[1],
        "row_job_id": "C1:A:1:3",
        "source_results": source_results,
    }


@pytest.mark.parametrize("same_endpoint", [False, True])
def test_source_transfer_failure_preserves_normal_legacy_retry(same_endpoint: bool) -> None:
    kwargs = _source_transfer_failure_kwargs(
        mutation="same-endpoint" if same_endpoint else None
    )
    kwargs["log_text"] = kwargs["log_text"].replace(
        "requesting one fresh strict-P50 remote assignment; avoiding failed endpoint "
        + kwargs["assignment"]["endpoint"],
        "requesting one fresh legacy remote assignment",
    )
    record = _source_transfer_failure_observation(**kwargs)
    assert record is not None
    assert record["retry_mode"] == "legacy"
    assert record["scheduler_job"] == 2
    assert record["retry_scheduler_job"] == 3
    assert record["status"] == 2
    assert record["compile_identity_present"] is False


def test_source_transfer_failure_binds_exact_uncommitted_retry_window() -> None:
    record = _source_transfer_failure_observation(
        **_source_transfer_failure_kwargs()
    )

    assert record is not None
    assert record["scheduler_job"] == 2
    assert record["retry_scheduler_job"] == 3
    assert record["worker"] == "F1"
    assert record["failed_endpoint"] == "10.0.27.101:23003"
    assert record["retry_endpoint"] == "10.0.27.56:23004"
    assert record["retry_c_guid"] == record["c_guid"] == 1
    assert record["retry_tu_seq"] == 10
    assert record["profile"] == "P29V1"
    assert record["status"] == 2
    assert record["error"] == 4
    assert record["transfer_attempts"] == 0
    assert record["source_result_present"] is False
    assert record["compile_identity_present"] is False


@pytest.mark.parametrize(
    "mutation",
    [
        "status-zero", "duplicate-failure", "wrong-retry-identity",
        "duplicate-first-identity", "committed-source-result",
        "malformed-source-result", "noncommitted-source-wrong-profile",
        "noncommitted-source-wrong-attempts",
    ],
)
def test_legacy_source_transfer_retry_keeps_identity_guards(mutation: str) -> None:
    kwargs = _source_transfer_failure_kwargs(mutation=mutation)
    kwargs["log_text"] = kwargs["log_text"].replace(
        "requesting one fresh strict-P50 remote assignment; avoiding failed endpoint "
        + kwargs["assignment"]["endpoint"],
        "requesting one fresh legacy remote assignment",
    )
    with pytest.raises(CollectError, match="source-transfer loss"):
        _source_transfer_failure_observation(**kwargs)


def test_source_transfer_disconnect_preserves_daemon_attempt_observation() -> None:
    # A lost local control reply is not a claim that the daemon attempted no
    # transfer. Reproduce the two observer views retained in full A3/n026.
    kwargs = _source_transfer_failure_kwargs(
        mutation="noncommitted-source-wrong-attempts", disconnected=True
    )
    kwargs["log_text"] = kwargs["log_text"].replace(
        "requesting one fresh strict-P50 remote assignment; avoiding failed endpoint "
        + kwargs["assignment"]["endpoint"],
        "requesting one fresh legacy remote assignment",
    )
    record = _source_transfer_failure_observation(**kwargs)
    assert record is not None
    assert record["transfer_attempts"] == 0
    assert record["source_result_attempts"] == 1
    assert record["source_result_status"] == 3
    assert record["control_result_received"] is False


@pytest.mark.parametrize("mutation", [
    "missing", "duplicate", "wrong-status", "wrong-error", "wrong-attempts",
    "wrong-profile", "committed", "bool-attempts", "large-attempts",
])
def test_source_transfer_disconnect_rejects_invalid_evidence(mutation: str) -> None:
    kwargs = _source_transfer_failure_kwargs(
        mutation="noncommitted-source-wrong-attempts", disconnected=True
    )
    log = kwargs["log_text"]
    marker = "P50 cache control operation ended disconnected"
    if mutation == "missing":
        log = log.replace(marker, "no control observation")
    elif mutation == "duplicate":
        log = log.replace(marker, marker + " " + marker)
    elif mutation == "wrong-status":
        log = log.replace("status 2", "status 1")
    elif mutation == "wrong-error":
        log = log.replace("error 7", "error 4")
    elif mutation == "wrong-attempts":
        log = log.replace("attempts 0", "attempts 1")
    else:
        result = kwargs["source_results"][(2, 1, 1)]
        if mutation == "wrong-profile":
            result["profile"] = "ZSTD_TU"
        elif mutation == "committed":
            result["status"] = 0
        elif mutation == "bool-attempts":
            result["attempts"] = True
        else:
            result["attempts"] = 3
    kwargs["log_text"] = log
    with pytest.raises(CollectError, match="source-transfer loss"):
        _source_transfer_failure_observation(**kwargs)


def test_source_transfer_retry_rejects_mixed_retry_claims() -> None:
    kwargs = _source_transfer_failure_kwargs()
    kwargs["log_text"] = kwargs["log_text"].replace(
        "P50 assignment failed; requesting one fresh strict-P50 remote assignment;",
        "P50 assignment failed; requesting one fresh legacy remote assignment "
        "P50 assignment failed; requesting one fresh strict-P50 remote assignment;",
    )
    with pytest.raises(CollectError, match="source-transfer loss"):
        _source_transfer_failure_observation(**kwargs)


def test_source_transfer_failure_accepts_noncommitted_source_result_diagnostic(
) -> None:
    record = _source_transfer_failure_observation(
        **_source_transfer_failure_kwargs(mutation="noncommitted-source-result")
    )

    assert record is not None
    assert record["source_result_present"] is False
    assert record["source_result_status"] == 3


@pytest.mark.parametrize(
    "mutation",
    (
        "wrong-failed-endpoint",
        "status-zero",
        "missing-retry",
        "duplicate-failure",
        "reversed-markers",
        "same-endpoint",
        "wrong-retry-identity",
        "duplicate-first-identity",
        "committed-source-result",
        "malformed-source-result",
        "noncommitted-source-wrong-profile",
        "noncommitted-source-wrong-attempts",
    ),
)
def test_source_transfer_failure_authentication_fails_closed(mutation: str) -> None:
    with pytest.raises(CollectError, match="source-transfer loss"):
        _source_transfer_failure_observation(
            **_source_transfer_failure_kwargs(mutation=mutation)
        )


def _uncommitted_transport_failure_kwargs(
    *, mutation: str | None = None
) -> dict[str, object]:
    first_endpoint = "10.0.27.56:23004"
    retry_endpoint = (
        first_endpoint if mutation == "same-endpoint" else "10.0.27.101:23003"
    )
    normalized = (
        "normalizing P50 client error 2 to Error 106 for a fresh assignment"
    )
    retry = (
        "P50 assignment failed; requesting one fresh strict-P50 remote assignment; "
        f"avoiding failed endpoint {first_endpoint}"
    )
    if mutation == "unsupported-error":
        normalized = normalized.replace("error 2", "error 3")
    elif mutation == "malformed-normalization":
        normalized = normalized.replace("Error 106", "Error 105")
    elif mutation == "duplicate-normalization":
        normalized += "\n" + normalized
    elif mutation == "missing-retry":
        retry = "retry marker absent"
    elif mutation == "wrong-failed-endpoint":
        retry = retry.replace(first_endpoint, "10.0.27.99:23999")
    elif mutation == "reversed-markers":
        normalized, retry = retry, normalized
    retry_identity_job = 4 if mutation == "wrong-retry-identity" else 3
    first_identity = (
        "P50 assignment identity bound for job 2 epoch 1 nonce 1 "
        "c_guid 1 tu_seq 9\n"
    )
    if mutation == "duplicate-first-identity":
        first_identity += first_identity
    profile = (
        "\nP29V1 source committed for P50 CompileFile: 100 exact bytes, "
        "TU sequence 9"
        if mutation == "profile-commit"
        else ""
    )
    log = (
        first_identity
        + f"ICECC[2] 2026-09-05 01:00:04: Have to use host {first_endpoint} "
        "- Job ID: 2 - env: x86_64\n"
        f"{normalized}{profile}\n"
        f"{retry}\n"
        f"P50 assignment identity bound for job {retry_identity_job} epoch 1 "
        "nonce 2 c_guid 1 tu_seq 10\n"
        f"ICECC[3] 2026-09-05 01:00:06: Have to use host {retry_endpoint} "
        "- Job ID: 3 - env: x86_64\n"
    )
    assignments = _client_assignments(log, "transport-loss-fixture")
    assignments[0]["worker"] = "F2"
    assignments[1]["worker"] = "F1" if retry_endpoint != first_endpoint else "F2"
    identities = [
        _p50_assignment_identity_evidence(
            log,
            assignment["scheduler_job"],
            after_line=(assignments[index - 1]["line"] if index else 0),
            before_line=assignment["line"] + 1,
        )
        for index, assignment in enumerate(assignments)
    ]
    source_results = (
        {(2, 1, 1): {"status": 3}}
        if mutation == "noncommitted-source-result"
        else {(2, 1, 1): {"status": 0}}
        if mutation == "committed-source-result"
        else {(2, 1, 1): {}}
        if mutation == "malformed-source-result"
        else {}
    )
    compile_identities = (
        {(2, 1, 1): {}} if mutation == "compile-identity" else {}
    )
    return {
        "assignment": assignments[0],
        "assignment_identity": identities[0],
        "attempt_index": 0,
        "compile_identities": compile_identities,
        "log_text": log,
        "retry_assignment": assignments[1],
        "retry_identity": identities[1],
        "row_job_id": "C1:A:121:179",
        "source_results": source_results,
    }


def test_uncommitted_transport_failure_binds_exact_retry_window() -> None:
    record = _uncommitted_transport_failure_observation(
        **_uncommitted_transport_failure_kwargs()
    )

    assert record is not None
    assert record["scheduler_job"] == 2
    assert record["retry_scheduler_job"] == 3
    assert record["worker"] == "F2"
    assert record["failed_endpoint"] == "10.0.27.56:23004"
    assert record["retry_endpoint"] == "10.0.27.101:23003"
    assert record["retry_c_guid"] == record["c_guid"] == 1
    assert record["original_error"] == 2
    assert record["normalized_error"] == 106
    assert record["profile_commit_present"] is False
    assert record["source_result_present"] is False
    assert record["compile_identity_present"] is False


def test_uncommitted_transport_failure_accepts_noncommitted_source_diagnostic(
) -> None:
    record = _uncommitted_transport_failure_observation(
        **_uncommitted_transport_failure_kwargs(
            mutation="noncommitted-source-result"
        )
    )

    assert record is not None
    assert record["source_result_present"] is False
    assert record["source_result_status"] == 3


@pytest.mark.parametrize(
    "mutation",
    (
        "unsupported-error",
        "malformed-normalization",
        "duplicate-normalization",
        "missing-retry",
        "wrong-failed-endpoint",
        "reversed-markers",
        "same-endpoint",
        "wrong-retry-identity",
        "duplicate-first-identity",
        "committed-source-result",
        "malformed-source-result",
        "compile-identity",
        "profile-commit",
    ),
)
def test_uncommitted_transport_failure_authentication_fails_closed(
    mutation: str,
) -> None:
    with pytest.raises(CollectError, match="transport.loss"):
        _uncommitted_transport_failure_observation(
            **_uncommitted_transport_failure_kwargs(mutation=mutation)
        )


def test_uncommitted_transport_parser_defers_non_transport_retry() -> None:
    kwargs = _uncommitted_transport_failure_kwargs()
    kwargs["log_text"] = str(kwargs["log_text"]).replace(
        "normalizing P50 client error 2 to Error 106 for a fresh assignment",
        "P29V1 cache source transfer failed closed (status 2, error 4, attempts 0)",
    )

    assert _uncommitted_transport_failure_observation(**kwargs) is None


def _unassigned_p50_failure_kwargs(
    *, mutation: str | None = None
) -> dict[str, object]:
    raw: dict[str, object] = {
        "compile_rc": 100,
        "exact": 0,
        "index": 5369,
        "remote": 0,
        "remote_sha": "0" * 64,
        "retries": 0,
        "scheduler_job": "missing-5369",
        "started": 0,
        "finished": 0,
        "turn": "A",
        "worker": "UNKNOWN",
    }
    lines = [
        "ICECC[1] asking for host to use",
        "ICECC[1] local build forced by remote exception: "
        "Error 105 - strict all-P50 assignment has no cache handoff",
        "ICECC[1] remote-only policy refuses local retry",
    ]
    if mutation == "successful":
        raw["compile_rc"] = 0
    elif mutation == "wrong-missing-id":
        raw["scheduler_job"] = "missing-999"
    elif mutation == "remote":
        raw["remote"] = 1
    elif mutation == "retry":
        raw["retries"] = 1
    elif mutation == "wrong-worker":
        raw["worker"] = "F1"
    elif mutation == "wrong-error":
        lines[1] = lines[1].replace("Error 105", "Error 106")
    elif mutation == "missing-refusal":
        lines.pop()
    elif mutation == "duplicate-request":
        lines.insert(1, lines[0])
    elif mutation == "reversed":
        lines[1], lines[2] = lines[2], lines[1]
    elif mutation == "assignment":
        lines.insert(
            1,
            "ICECC[1] Have to use host 10.0.27.56:23004 - Job ID: 1 - "
            "env: x86_64",
        )
    elif mutation == "identity":
        lines.insert(
            1,
            "P50 assignment identity bound for job 1 epoch 1 nonce 1 "
            "c_guid 1 tu_seq 1",
        )
    elif mutation == "profile":
        lines.insert(
            1,
            "P29V1 source committed for P50 CompileFile: 100 exact bytes, "
            "TU sequence 1",
        )
    return {
        "log_text": "\n".join(lines) + "\n",
        "raw": raw,
        "row_job_id": "C1:A:5369:missing-5369",
    }


def test_unassigned_p50_failure_binds_exact_remote_only_refusal() -> None:
    record = _unassigned_p50_failure_observation(
        **_unassigned_p50_failure_kwargs()
    )

    assert record == {
        "client_instance": "C1",
        "compile_rc": 100,
        "failure_line": 2,
        "index": 5369,
        "remote_only_refusal_line": 3,
        "request_line": 1,
        "request_finished_ms": 0,
        "request_started_ms": 0,
        "row_job_id": "C1:A:5369:missing-5369",
        "scheduler_job": "missing-5369",
        "turn": "A",
        "worker": "UNKNOWN",
    }


def _unassigned_scheduler_stream_loss_kwargs(
    *, mutation: str | None = None
) -> dict[str, object]:
    lines = [
        "ICECC[1] asking for host to use",
        "ICECC[1] got exception Error 1 - expected use_cs reply, but got "
        "UNKNOWN instead (this should be an exception!)",
        "ICECC[1] remote-only policy refuses client-error fallback",
    ]
    if mutation == "cache-overlap":
        lines.insert(
            2,
            "ICECC[1] local build forced by remote exception: "
            "Error 105 - strict all-P50 assignment has no cache handoff",
        )
    elif mutation == "wrong-error":
        lines[1] = lines[1].replace("Error 1", "Error 2")
    elif mutation == "missing-refusal":
        lines.pop()
    elif mutation == "reversed":
        lines[1], lines[2] = lines[2], lines[1]
    return {
        "log_text": "\n".join(lines) + "\n",
        "raw": {
            "compile_rc": 100,
            "exact": 0,
            "finished": 1_789_524_585_252,
            "index": 508,
            "remote": 0,
            "remote_sha": "0" * 64,
            "retries": 0,
            "scheduler_job": "missing-508",
            "started": 1_789_524_576_201,
            "turn": "A",
            "worker": "UNKNOWN",
        },
        "row_job_id": "C1:A:508:missing-508",
    }


def test_unassigned_p50_failure_binds_scheduler_stream_loss() -> None:
    record = _unassigned_p50_failure_observation(
        **_unassigned_scheduler_stream_loss_kwargs()
    )

    assert record["failure_reason"] == "scheduler-stream-loss-before-usecs"
    assert record["failure_line"] == 2
    assert record["remote_only_refusal_line"] == 3
    assert record["scheduler_job"] == "missing-508"


@pytest.mark.parametrize(
    "mutation", ("cache-overlap", "wrong-error", "missing-refusal", "reversed")
)
def test_unassigned_scheduler_stream_loss_fails_closed(mutation: str) -> None:
    with pytest.raises(CollectError, match="missing assignment"):
        _unassigned_p50_failure_observation(
            **_unassigned_scheduler_stream_loss_kwargs(mutation=mutation)
        )


@pytest.mark.parametrize(
    "mutation",
    (
        "successful",
        "wrong-missing-id",
        "remote",
        "retry",
        "wrong-worker",
        "wrong-error",
        "missing-refusal",
        "duplicate-request",
        "reversed",
        "assignment",
        "identity",
        "profile",
    ),
)
def test_unassigned_p50_failure_authentication_fails_closed(mutation: str) -> None:
    with pytest.raises(CollectError, match="missing assignment"):
        _unassigned_p50_failure_observation(
            **_unassigned_p50_failure_kwargs(mutation=mutation)
        )


def _abandoned_p50_retry_kwargs(
    *, mutation: str | None = None
) -> dict[str, object]:
    endpoint = "10.0.27.101:23003"
    log = (
        "P50 assignment identity bound for job 5407 epoch 1 nonce 2 "
        "c_guid 1 tu_seq 3\n"
        f"ICECC[1] 2026-09-10 20:52:24: Have to use host {endpoint} "
        "- Job ID: 5407 - env: x86_64\n"
        "P29V1 cache source transfer failed closed "
        "(status 2, error 7, attempts 0)\n"
        "P50 assignment failed; requesting one fresh strict-P50 remote "
        f"assignment; avoiding failed endpoint {endpoint}\n"
        "ICECC[1] local build forced by remote exception: "
        "Error 105 - strict all-P50 assignment has no cache handoff\n"
        "ICECC[1] remote-only policy refuses local retry\n"
    )
    if mutation == "successful":
        compile_rc = 0
    else:
        compile_rc = 100
    if mutation == "second-assignment":
        log += (
            "ICECC[1] 2026-09-10 20:52:34: Have to use host "
            "10.0.27.56:23004 - Job ID: 5408 - env: x86_64\n"
        )
    elif mutation == "profile":
        log += (
            "P29V1 source committed for P50 CompileFile: 100 exact bytes, "
            "TU sequence 3\n"
        )
    elif mutation == "wrong-endpoint":
        log = log.replace(
            f"avoiding failed endpoint {endpoint}",
            "avoiding failed endpoint 10.0.27.99:23999",
        )
    elif mutation == "missing-refusal":
        log = log.replace("ICECC[1] remote-only policy refuses local retry\n", "")
    assignments = _client_assignments(log, "abandoned-retry-fixture")
    assignments[0]["worker"] = "F1"
    identity = _p50_assignment_identity_evidence(
        log,
        assignments[0]["scheduler_job"],
        after_line=0,
        before_line=assignments[0]["line"] + 1,
    )
    return {
        "assignment": assignments[0],
        "assignment_identity": identity,
        "log_text": log,
        "raw": {
            "compile_rc": compile_rc,
            "exact": 0,
            "finished": 20,
            "remote": 1,
            "remote_sha": "0" * 64,
            "retries": 0,
            "started": 10,
        },
        "row_job_id": "C1:A:5338:5407",
    }


def test_abandoned_p50_retry_binds_failed_request_without_usecs() -> None:
    record = _abandoned_p50_retry_request_observation(
        **_abandoned_p50_retry_kwargs()
    )

    assert record is not None
    assert record["scheduler_job"] == 5407
    assert record["failed_endpoint"] == "10.0.27.101:23003"
    assert record["status"] == 2
    assert record["transfer_error"] == 7
    assert record["request_started_ms"] == 10
    assert record["request_finished_ms"] == 20


def _abandoned_p50_result_stream_retry_kwargs(
    *, mutation: str | None = None
) -> dict[str, object]:
    endpoint = "10.0.27.101:23003"
    log = (
        "P50 assignment identity bound for job 427 epoch 1 nonce 2 "
        "c_guid 1 tu_seq 426\n"
        f"ICECC[1] 2026-09-16 02:09:28: Have to use host {endpoint} "
        "- Job ID: 427 - env: x86_64\n"
        "ZSTD_ROUTE source committed for P50 CompileFile: 5849401 exact bytes, "
        "TU sequence 426\n"
        "normalizing P50 client error 14 to Error 106 for a fresh assignment\n"
        "P50 assignment failed; requesting one fresh strict-P50 remote "
        f"assignment; avoiding failed endpoint {endpoint}\n"
        "local build forced by remote exception: "
        "Error 105 - strict all-P50 assignment has no cache handoff\n"
        "remote-only policy refuses local retry\n"
    )
    if mutation == "missing-commit":
        log = log.replace(
            "ZSTD_ROUTE source committed for P50 CompileFile: 5849401 exact bytes, "
            "TU sequence 426\n",
            "",
        )
    elif mutation == "zero-tu":
        log = log.replace("TU sequence 426", "TU sequence 0")
    elif mutation == "wrong-error":
        log = log.replace("client error 14", "client error 3")
    elif mutation == "reordered":
        normalized = (
            "normalizing P50 client error 14 to Error 106 for a fresh assignment\n"
        )
        log = log.replace(normalized, "").replace(
            "ZSTD_ROUTE source committed for P50 CompileFile: 5849401 exact bytes, "
            "TU sequence 426\n",
            normalized
            + "ZSTD_ROUTE source committed for P50 CompileFile: 5849401 exact bytes, "
            "TU sequence 426\n",
        )
    elif mutation == "transfer-overlap":
        log = log.replace(
            "normalizing P50 client error 14 to Error 106 for a fresh assignment\n",
            "P29V1 cache source transfer failed closed "
            "(status 2, error 7, attempts 0)\n"
            "normalizing P50 client error 14 to Error 106 for a fresh assignment\n",
        )
    assignments = _client_assignments(log, "abandoned-result-stream-fixture")
    assignments[0]["worker"] = "F1"
    identity = _p50_assignment_identity_evidence(
        log,
        assignments[0]["scheduler_job"],
        after_line=0,
        before_line=assignments[0]["line"] + 1,
    )
    return {
        "assignment": assignments[0],
        "assignment_identity": identity,
        "log_text": log,
        "raw": {
            "compile_rc": 100,
            "exact": 0,
            "finished": 1_789_524_598_309,
            "remote": 1,
            "remote_sha": "0" * 64,
            "retries": 0,
            "started": 1_789_524_568_146,
        },
        "row_job_id": "C1:A:426:427",
    }


def test_abandoned_p50_retry_binds_committed_result_stream_loss() -> None:
    record = _abandoned_p50_retry_request_observation(
        **_abandoned_p50_result_stream_retry_kwargs()
    )

    assert record is not None
    assert record["failure_reason"] == "result-stream-loss"
    assert record["normalized_error"] == 14
    assert record["profile"] == "ZSTD_ROUTE"
    assert record["raw_bytes"] == 5_849_401
    assert record["tu_seq"] == 426
    assert record["scheduler_job"] == 427
    assert record["failed_endpoint"] == "10.0.27.101:23003"


@pytest.mark.parametrize(
    "mutation",
    ("missing-commit", "zero-tu", "wrong-error", "reordered", "transfer-overlap"),
)
def test_abandoned_p50_result_stream_retry_fails_closed(mutation: str) -> None:
    with pytest.raises(CollectError, match="abandoned strict-P50 retry"):
        _abandoned_p50_retry_request_observation(
            **_abandoned_p50_result_stream_retry_kwargs(mutation=mutation)
        )


@pytest.mark.parametrize(
    "mutation", ("successful", "second-assignment", "profile", "wrong-endpoint", "missing-refusal")
)
def test_abandoned_p50_retry_authentication_fails_closed(mutation: str) -> None:
    with pytest.raises(CollectError, match="abandoned strict-P50 retry"):
        _abandoned_p50_retry_request_observation(
            **_abandoned_p50_retry_kwargs(mutation=mutation)
        )


def test_fresh_p50_retry_binds_only_the_final_result_identity(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    _make_fresh_p50_retry_fixture(plan, root)

    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)

    assert bundle["rows"][0]["session_outcome"] == "committed"
    assert bundle["rows"][0]["exact"] is True
    assert bundle["rows"][0]["retries"] == 1
    assert bundle["observations"]["failed_p50_result_identities"] == {
        "record_count": 1,
        "records": [
            {
                "assignment_epoch": 1,
                "assignment_nonce": 1,
                "attempt_index": 0,
                "reason": "result-stream-loss",
                "result_identity_present": False,
                "row_job_id": "C1:A:1:3",
                "scheduler_job": 2,
                "worker": "F1",
            }
        ],
    }
    assert bundle["observations"]["successful_strict_p50_retry_bindings"] == [
        {
            "failure_reason": "result-stream-loss",
            "final_dispatch_ms": 1_788_570_006_000,
            "final_generation": 1,
            "final_scheduler_job": 3,
            "final_terminal_ms": 1_788_570_007_000,
            "final_worker": "F1",
            "first_dispatch_ms": 1_788_570_004_000,
            "first_generation": 1,
            "first_scheduler_job": 2,
            "first_terminal": "completion",
            "first_terminal_ms": 1_788_570_005_000,
            "first_worker": "F1",
            "job_id": "C1:A:1:3",
        }
    ]
    assert bundle["observations"]["compile_failure_job_ids"] == []


def test_retry_loss_witness_must_be_in_the_same_attempt_window(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    # Build the valid two-attempt fixture, then move Error106 after assignment
    # 2. The final attempt's log cannot excuse attempt 1's missing result.
    _make_fresh_p50_retry_fixture(plan, root)
    job = root / "C1.results" / "workload" / "jobs" / "000001"
    debug = job / "client-debug.log"
    lines = debug.read_text(encoding="utf-8").splitlines()
    error = next(line for line in lines if "normalizing P50 client error" in line)
    lines.remove(error)
    lines.append(error)
    debug.write_text("\n".join(lines) + "\n", encoding="utf-8")

    with pytest.raises(CollectError, match="no result identity or exact loss witness"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def _orphan_recovery_job() -> dict[str, object]:
    return {
        "assignment_claims": [
            {
                "line": 6,
                "scheduler_job": 17,
                "scheduler_record": {
                    "scheduler_job": 17,
                    "terminal": "process-loss-recovery",
                },
                "worker": "F1",
            },
            {
                "line": 22,
                "scheduler_record": {"terminal": "completion"},
                "worker": "F2",
            },
        ],
        "finished": 2_000,
        "orphan_recovery_marker": {
            "line": 10,
            "profile": "P29V1",
            "raw_bytes": 100,
            "tu_seq": 1,
        },
        "retries": 1,
        "row_job_id": "C1:A:1:3",
        "started": 1_000,
    }


@pytest.mark.parametrize("start,end,accepted", ((1500, 2500, True), (2100, 2500, False)))
def test_orphan_kill_interval_overlaps_assignment(start: int, end: int, accepted: bool) -> None:
    raw = _orphan_recovery_job()
    raw["assignment_claims"][0]["scheduler_record"].update(
        worker="F1", dispatch_ms=1000, terminal_ms=1000,
    )
    events = [{"action": "kill -9", "instance": "F1", "fired_ms": end,
               "receipt": {"started_ms": start, "completed_ms": end}}]
    if accepted:
        _validate_orphan_recovery_markers([raw], events)
    else:
        with pytest.raises(CollectError, match="authenticated killed-assignment retry"):
            _validate_orphan_recovery_markers([raw], events)


def test_orphan_marker_is_admitted_only_for_authenticated_killed_retry() -> None:
    raw = _orphan_recovery_job()
    events = [{"action": "kill -9", "fired_ms": 1_500, "instance": "F1"}]

    _validate_orphan_recovery_markers([raw], events)


def _worker_restart_loss_event(*, instance: str = "F1", scheduler_job: int = 17):
    return {
        "action": "restart",
        "instance": instance,
        "receipt": {
            "coordination": {
                "scheduler_rejoin": {"loss_job_ids": [scheduler_job]},
            },
            "schema": "icefarm-worker-restart-v1",
        },
    }


def test_orphan_marker_accepts_authenticated_worker_restart_loss() -> None:
    _validate_orphan_recovery_markers(
        [_orphan_recovery_job()], [_worker_restart_loss_event()]
    )


@pytest.mark.parametrize("mutation", ("wrong-worker", "wrong-job", "bad-schema"))
def test_orphan_marker_refuses_unbound_worker_restart_loss(mutation: str) -> None:
    raw = _orphan_recovery_job()
    event = _worker_restart_loss_event()
    if mutation == "wrong-worker":
        event["instance"] = "F2"
    elif mutation == "wrong-job":
        event["receipt"]["coordination"]["scheduler_rejoin"]["loss_job_ids"] = [18]
    else:
        event["receipt"]["schema"] = "unvalidated-worker-restart"

    with pytest.raises(CollectError, match="authenticated killed-assignment retry"):
        _validate_orphan_recovery_markers([raw], [event])


@pytest.mark.parametrize(
    "mutation",
    ("no-kill", "wrong-worker", "no-process-loss", "marker-after-final"),
)
def test_orphan_marker_refuses_unauthenticated_retry(mutation: str) -> None:
    raw = _orphan_recovery_job()
    events = [{"action": "kill -9", "fired_ms": 1_500, "instance": "F1"}]
    claims = raw["assignment_claims"]
    assert isinstance(claims, list)
    if mutation == "no-kill":
        events = []
    elif mutation == "wrong-worker":
        events[0]["instance"] = "F2"
    elif mutation == "no-process-loss":
        claims[0]["scheduler_record"]["terminal"] = "cancellation"
    else:
        raw["orphan_recovery_marker"]["line"] = 30

    with pytest.raises(CollectError, match="authenticated killed-assignment retry"):
        _validate_orphan_recovery_markers([raw], events)


def test_collection_refuses_duplicate_source_result_json_key(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    path = root / "C1.results" / "source-result.jsonl"
    record = json.loads(path.read_text(encoding="utf-8"))
    encoded = json.dumps(record, sort_keys=True, separators=(",", ":"))
    path.write_text(encoded[:-1] + ',"status":0}\n', encoding="utf-8")

    with pytest.raises(CollectError, match="duplicate JSON key 'status'"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_collection_joins_source_result_by_full_assignment_identity(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    results = root / "C1.results"
    source_path = results / "source-result.jsonl"
    source = json.loads(source_path.read_text(encoding="utf-8"))
    second_source = {**source, "assignment_epoch": 2, "assignment_nonce": 2}
    _write_jsonl(source_path, [source, second_source])
    identity_path = results / "compile-identity.jsonl"
    identity = json.loads(identity_path.read_text(encoding="utf-8"))
    second_identity = {**identity, "assignment_epoch": 2, "assignment_nonce": 2}
    _write_jsonl(identity_path, [identity, second_identity])

    with pytest.raises(CollectError, match="source-result assignment is ambiguous"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_source_attribution_does_not_reuse_a_job_id_after_legacy_s_restart() -> None:
    stale = _source_result_record()
    source_results = {(2, 1, 1): stale}

    assert _source_candidates_for_assignment(source_results, 2, 43) == []
    assert _source_candidates_for_assignment(source_results, 2, 50) == [
        ((2, 1, 1), stale)
    ]


def test_source_attribution_prefers_exact_client_local_legacy_identity() -> None:
    """A coarse current-S timestamp must not alias an older P50 job number."""

    stale = _source_result_record()
    source_results = {(2, 1, 1): stale}
    client_local_legacy = (2, 0, 0, 202, 0)

    assert (
        _source_candidates_for_assignment(
            source_results,
            2,
            50,
            None,
            client_local_legacy,
        )
        == []
    )
    # If the same job-local log also carries a full P50 assignment identity,
    # retain the source candidate so the caller's overlap check fails closed.
    assert _source_candidates_for_assignment(
        source_results,
        2,
        50,
        (2, 1, 1),
        client_local_legacy,
    ) == [((2, 1, 1), stale)]


def test_collection_attributes_client_local_legacy_after_reused_job_id(
    tmp_path: Path,
) -> None:
    """A replacement S may reuse an old P50 job number for legacy work."""

    farm, scenario, plan, root = _raw_collection(tmp_path)
    scheduler = next(
        item for item in plan["topology"]["instances"] if item["role"] == "S"
    )
    scheduler_log = (
        root / "diagnostics" / scheduler["host"] / "S1.log" / "scheduler.log"
    )
    scheduler_log.write_text(
        scheduler_log.read_text(encoding="utf-8").replace(
            "[1] 2026-09-05 01:00:06: RELOGIN F1(x86_64): cache=off\n",
            "[1] 2026-09-05 01:00:06: ICECREAM scheduler 1.4.0 "
            "starting up, port 23000\n"
            "[1] 2026-09-05 01:00:07: NEW 2 client=C1 versions=[] "
            "/corpus/files/x.ii C++ 0\n"
            "[1] 2026-09-05 01:00:07: put 2 in joblist of F1\n"
            "[1] 2026-09-05 01:00:07: BEGIN: 2 client=C1(x86_64) "
            "server=F1(x86_64)\n"
            "[1] 2026-09-05 01:00:08: END 2 status=0 server=F1\n"
            "[1] 2026-09-05 01:00:09: RELOGIN F1(x86_64): cache=off\n",
        ),
        encoding="utf-8",
    )
    results = root / "C1.results"
    job = results / "workload" / "jobs" / "000001"
    endpoint = next(
        f"{item['address']}:{plan['ports']['instances'][item['name']]}"
        for item in plan["topology"]["instances"]
        if item["name"] == "F1"
    )
    debug = job / "client-debug.log"
    debug.write_text(
        "P50 assignment identity bound for job 2 epoch 1 nonce 1 "
        "c_guid 1 tu_seq 99\n"
        + debug.read_text(encoding="utf-8")
        + "normalizing P50 client error 14 to Error 106 for a fresh assignment\n"
        + f"ICECC[3] 2026-09-05 01:00:07: Have to use host {endpoint} "
        "- Job ID: 2 - env: x86_64\n"
        + "legacy wire identity bound for job 2 epoch 0 nonce 0 "
        "c_guid 202 tu_seq 0 origin client-local\n",
        encoding="utf-8",
    )
    result = job / "result.tsv"
    fields = result.read_text(encoding="utf-8").rstrip("\n").split("\t")
    fields[-1] = "1"
    result.write_text("\t".join(fields) + "\n", encoding="utf-8")
    (results / "compile-identity.jsonl").write_text("", encoding="utf-8")
    common = {
        "assignment_epoch": 0,
        "assignment_nonce": 0,
        "c_guid": 202,
        "job_id": 2,
        "schema": "icecream-p50-legacy-wire-v1",
        "tu_seq": 0,
    }
    _write_jsonl(
        results / "c-legacy-wire.jsonl",
        [
            {
                **common,
                "c_to_f_received_bytes": 0,
                "c_to_f_sent_bytes": 400,
                "f_to_c_received_bytes": 200,
                "f_to_c_sent_bytes": 0,
                "role": "C",
            }
        ],
    )
    _write_jsonl(
        root / "F1.results" / "f-legacy-wire.jsonl",
        [
            {
                **common,
                "c_to_f_received_bytes": 400,
                "c_to_f_sent_bytes": 0,
                "f_to_c_received_bytes": 0,
                "f_to_c_sent_bytes": 200,
                "role": "F",
            }
        ],
    )

    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)

    row = bundle["rows"][0]
    assert row["tail_present"] is False
    assert row["c_to_f_bytes"] == 400
    assert row["f_to_c_bytes"] == 200
    assert bundle["observations"]["legacy_wire"]["record_count"] == 1
    assert bundle["observations"]["scheduler_reconciliation"]["generations"] == 2
    assert bundle["observations"]["failed_p50_result_identities"]["records"][0][
        "reason"
    ] == "result-stream-loss"


def test_source_attribution_binds_reused_current_scheduler_job_to_exact_epoch() -> None:
    first = _source_result_record()
    second = {**first, "assignment_epoch": 7, "assignment_nonce": 8}
    source_results = {(2, 1, 1): first, (2, 7, 8): second}
    log = (
        "P50 assignment identity bound for job 2 epoch 7 nonce 8 "
        "c_guid 99 tu_seq 0"
    )
    identity = _p50_assignment_identity_marker(log, 2)

    assert identity == (2, 7, 8)
    assert _source_candidates_for_assignment(
        source_results, 2, 50, identity
    ) == [((2, 7, 8), second)]


def test_source_attribution_prefers_full_wire_identity_at_subsecond_s_upgrade() -> None:
    """A whole-second client timestamp must not beat a P50 wire identity."""

    source = _source_result_record()
    source["assignment_epoch"] = 7
    source["assignment_nonce"] = 8
    source_results = {(2, 7, 8): source}
    identity = _p50_assignment_identity_marker(
        "P50 assignment identity bound for job 2 epoch 7 nonce 8 "
        "c_guid 99 tu_seq 5",
        2,
    )

    # The assignment line says HH:MM:SS.000 while the transition receipt can
    # say HH:MM:SS.260.  The old version is consequently only a coarse-time
    # classification; the exact P50 identity proves the post-upgrade S.
    assert _source_candidates_for_assignment(
        source_results, 2, 43, identity
    ) == [((2, 7, 8), source)]


def test_source_attribution_never_uses_job_number_only_at_subsecond_s_upgrade() -> None:
    stale = _source_result_record()
    source_results = {(2, 1, 1): stale}

    # Without a job-local wire identity an apparent legacy assignment must
    # not inherit a same-number P50 result from another scheduler generation.
    assert _source_candidates_for_assignment(source_results, 2, 43, None) == []
    assert _source_candidates_for_assignment(
        source_results, 2, 43, (2, 7, 8)
    ) == []


def test_legacy_wire_attribution_requires_exact_job_local_binding_marker() -> None:
    stale = {
        "assignment_epoch": 0,
        "assignment_nonce": 0,
        "c_guid": 202,
        "tu_seq": 0,
    }
    records = {(24, 0, 0, 202, 0): stale}

    # A pre-restart P50 source job with the same numeric scheduler ID has no
    # legacy binding marker and must not inherit the post-restart record.
    assert _legacy_wire_candidates_for_assignment(records, 24, None, {}) == []
    assert _legacy_wire_candidates_for_assignment(
        records, 24, (24, 0, 0, 202, 0), {}
    ) == [((24, 0, 0, 202, 0), stale)]


def test_legacy_wire_attribution_rejects_marker_without_exact_result(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    job = next((root / "C1.results" / "workload").glob("jobs/*/client-debug.log"))
    job.write_text(
        job.read_text(encoding="utf-8")
        + "legacy wire identity bound for job 2 epoch 0 nonce 0 "
        "c_guid 202 tu_seq 0 origin client-local\n",
        encoding="utf-8",
    )

    with pytest.raises(CollectError, match="binding marker has no exact result witness"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_collection_refuses_a_commit_without_exact_wire_byte_counts(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    path = root / "C1.results" / "source-result.jsonl"
    record = json.loads(path.read_text(encoding="utf-8"))
    record["c_to_f_bytes"] = 0
    _write_jsonl(path, [record])

    with pytest.raises(CollectError, match="committed source-result has no wire bytes"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_collection_refuses_a_commit_without_a_raw_digest(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    path = root / "C1.results" / "source-result.jsonl"
    record = json.loads(path.read_text(encoding="utf-8"))
    record["raw_digest"] = "0" * 32
    _write_jsonl(path, [record])

    with pytest.raises(CollectError, match="committed source-result has no raw digest"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_collection_refuses_a_commit_without_mutex_service_time(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    path = root / "C1.results" / "source-result.jsonl"
    record = json.loads(path.read_text(encoding="utf-8"))
    record["source_mutex_service_ns"] = 0
    _write_jsonl(path, [record])

    with pytest.raises(
        CollectError, match="committed source-result has no mutex service time"
    ):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def _make_legacy_wire_fixture(root: Path) -> None:
    client = root / "C1.results"
    (client / "source-result.jsonl").unlink()
    debug = next((client / "workload").glob("jobs/*/client-debug.log"))
    debug_lines = [
        line
        for line in debug.read_text(encoding="utf-8").splitlines()
        if "source committed for P50 CompileFile" not in line
    ]
    debug.write_text(
        "\n".join(debug_lines)
        + "\nlegacy wire identity bound for job 2 epoch 1 nonce 1 "
        "c_guid 1 tu_seq 99 origin scheduler\n",
        encoding="utf-8",
    )
    worker_log = next((root / "diagnostics").glob("*/F1.log/iceccd.log"))
    worker_log.write_text("legacy compile completed\n", encoding="utf-8")
    common = {
        "assignment_epoch": 1,
        "assignment_nonce": 1,
        "c_guid": 1,
        "job_id": 2,
        "schema": "icecream-p50-legacy-wire-v1",
        "tu_seq": 99,
    }
    _write_jsonl(
        client / "c-legacy-wire.jsonl",
        [
            {
                **common,
                "c_to_f_received_bytes": 0,
                "c_to_f_sent_bytes": 400,
                "f_to_c_received_bytes": 200,
                "f_to_c_sent_bytes": 0,
                "role": "C",
            }
        ],
    )
    _write_jsonl(
        root / "F1.results" / "f-legacy-wire.jsonl",
        [
            {
                **common,
                "c_to_f_received_bytes": 400,
                "c_to_f_sent_bytes": 0,
                "f_to_c_received_bytes": 0,
                "f_to_c_sent_bytes": 200,
                "role": "F",
            }
        ],
    )


def test_collection_conserves_identity_bound_legacy_wire_bytes(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    _make_legacy_wire_fixture(root)

    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)

    assert bundle["rows"][0]["tail_present"] is False
    assert bundle["rows"][0]["c_to_f_bytes"] == 400
    assert bundle["rows"][0]["f_to_c_bytes"] == 200
    assert bundle["observations"]["legacy_wire"] == {
        "record_count": 1,
        "records": [
            {
                "c_to_f_bytes": 400,
                "client_instance": "C1",
                "f_to_c_bytes": 200,
                "job_id": "C1:A:1:2",
                "turn": "A",
                "worker_instance": "F1",
            }
        ],
    }
    assert bundle["observations"]["turns"]["A"]["c_to_f_bytes"] == 400
    assert bundle["observations"]["turns"]["A"]["f_to_c_bytes"] == 200


def test_collection_conserves_absent_assignment_legacy_wire_bytes(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    _make_legacy_wire_fixture(root)
    (root / "C1.results" / "compile-identity.jsonl").unlink()
    for path in (
        root / "C1.results" / "c-legacy-wire.jsonl",
        root / "F1.results" / "f-legacy-wire.jsonl",
    ):
        record = json.loads(path.read_text(encoding="utf-8"))
        record["assignment_epoch"] = 0
        record["assignment_nonce"] = 0
        record["tu_seq"] = 0
        _write_jsonl(path, [record])
    debug = next(
        (root / "C1.results" / "workload").glob("jobs/*/client-debug.log")
    )
    debug.write_text(
        debug.read_text(encoding="utf-8").replace(
            "epoch 1 nonce 1 c_guid 1 tu_seq 99 origin scheduler",
            "epoch 0 nonce 0 c_guid 1 tu_seq 0 origin client-local",
        ),
        encoding="utf-8",
    )

    bundle = collect_bundle(farm, scenario, plan, sync_remote=False)

    assert bundle["rows"][0]["tail_present"] is False
    assert bundle["rows"][0]["c_to_f_bytes"] == 400
    assert bundle["rows"][0]["f_to_c_bytes"] == 200
    assert bundle["observations"]["legacy_wire"]["record_count"] == 1


def test_absent_assignment_legacy_wire_keys_include_local_guid(
    tmp_path: Path,
) -> None:
    path = tmp_path / "c-legacy-wire.jsonl"
    records = []
    for guid in (101, 202):
        records.append(
            {
                "assignment_epoch": 0,
                "assignment_nonce": 0,
                "c_guid": guid,
                "c_to_f_received_bytes": 0,
                "c_to_f_sent_bytes": 400,
                "f_to_c_received_bytes": 200,
                "f_to_c_sent_bytes": 0,
                "job_id": 2,
                "role": "C",
                "schema": "icecream-p50-legacy-wire-v1",
                "tu_seq": 0,
            }
        )
    _write_jsonl(path, records)

    parsed = _legacy_wire_results(path, "C")

    assert set(parsed) == {(2, 0, 0, 101, 0), (2, 0, 0, 202, 0)}
    text = (
        "legacy wire identity bound for job 2 epoch 0 nonce 0 "
        "c_guid 202 tu_seq 0 origin client-local"
    )
    assert _legacy_wire_binding_marker(text, 2) == (2, 0, 0, 202, 0)
    scheduler_compat = (
        "legacy wire identity bound for job 2 epoch 0 nonce 0 "
        "c_guid 101 tu_seq 0 origin scheduler"
    )
    assert _legacy_wire_binding_marker(scheduler_compat, 2) == (
        2,
        0,
        0,
        101,
        0,
    )


def test_legacy_wire_binding_marker_rejects_origin_authority_mismatch() -> None:
    with pytest.raises(CollectError, match="binding marker is invalid"):
        _legacy_wire_binding_marker(
            "legacy wire identity bound for job 2 epoch 7 nonce 8 "
            "c_guid 202 tu_seq 0 origin client-local",
            2,
        )


@pytest.mark.parametrize(("epoch", "nonce"), ((0, 1), (1, 0)))
def test_collection_refuses_partial_assignment_legacy_wire_identity(
    tmp_path: Path, epoch: int, nonce: int
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    _make_legacy_wire_fixture(root)
    path = root / "C1.results" / "c-legacy-wire.jsonl"
    record = json.loads(path.read_text(encoding="utf-8"))
    record["assignment_epoch"] = epoch
    record["assignment_nonce"] = nonce
    _write_jsonl(path, [record])

    with pytest.raises(CollectError, match="legacy-wire identity is invalid"):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_collection_refuses_nonconserving_legacy_wire_bytes(tmp_path: Path) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    _make_legacy_wire_fixture(root)
    path = root / "F1.results" / "f-legacy-wire.jsonl"
    record = json.loads(path.read_text(encoding="utf-8"))
    record["c_to_f_received_bytes"] += 1
    _write_jsonl(path, [record])

    with pytest.raises(
        CollectError, match="legacy-wire bytes/identity do not conserve"
    ):
        collect_bundle(farm, scenario, plan, sync_remote=False)


def test_pre50_login_without_cache_fields_is_normalized_as_legacy(
    tmp_path: Path,
) -> None:
    evidence = tmp_path / "evidence"
    log = evidence / "diagnostics" / "scheduler-host" / "S1.log" / "scheduler.log"
    log.parent.mkdir(parents=True)
    log.write_text(
        "[1] RELOGIN F1(x86_64): [environment(x86_64), ]\n",
        encoding="utf-8",
    )
    plan = {
        "topology": {
            "instances": [
                {"host": "scheduler-host", "name": "S1", "role": "S", "version": 43},
                {"host": "worker-host", "name": "F1", "role": "F", "version": 43},
            ]
        }
    }

    logins, revisions, ports = _parse_logins(evidence, plan)

    assert logins == [{"cache_profiles": [], "instance": "F1", "protocol": 43}]
    assert revisions == {}
    assert ports == {}


def test_login_parser_uses_advertised_cache_protocol_not_format_token(
    tmp_path: Path,
) -> None:
    evidence = tmp_path / "evidence"
    log = evidence / "diagnostics" / "scheduler-host" / "S1.log" / "scheduler.log"
    log.parent.mkdir(parents=True)
    log.write_text(
        "[1] RELOGIN F2(x86_64): [environment(x86_64), ] "
        "cache=10.0.0.2:24000 cache_wire=v1 cache_protocol=2 "
        "cache_profiles=p29v1 zstd_tu zstd_route\n",
        encoding="utf-8",
    )
    plan = {
        "topology": {
            "instances": [
                {"host": "scheduler-host", "name": "S1", "role": "S", "version": 50},
                {"host": "worker-host", "name": "F2", "role": "F", "version": 50},
            ]
        }
    }

    logins, revisions, ports = _parse_logins(evidence, plan)

    assert logins == [
        {
            "cache_protocol": 2,
            "cache_profiles": ["P29V1", "ZSTD_TU", "ZSTD_ROUTE"],
            "instance": "F2",
            "protocol": 50,
        }
    ]
    assert revisions == {"F2": 2}
    assert ports == {"F2": [24000]}


def test_warm_hint_override_diagnostic_is_parsed_and_validated(tmp_path: Path) -> None:
    evidence = tmp_path / "evidence"
    log = evidence / "diagnostics" / "scheduler-host" / "S1.log" / "scheduler.log"
    log.parent.mkdir(parents=True)
    log.write_text(
        "[1] P50_WARM_HINT_OVERRIDE job=17 warm=1 compatible_free=2 idle_excluded=1\n",
        encoding="utf-8",
    )
    plan = {
        "topology": {
            "instances": [
                {"host": "scheduler-host", "name": "S1", "role": "S"},
                {"host": "worker-a", "name": "F1", "role": "F"},
                {"host": "worker-b", "name": "F2", "role": "F"},
            ]
        }
    }
    assert _warm_hint_overrides(evidence, plan) == {
        "count": 1,
        "events": [
            {
                "compatible_free": 2,
                "idle_excluded": 1,
                "job_id": 17,
                "line": 1,
                "warm": 1,
            }
        ],
        "job_ids": [17],
    }

    log.write_text(
        "P50_WARM_HINT_OVERRIDE job=17 warm=2 compatible_free=2 idle_excluded=1\n",
        encoding="utf-8",
    )
    with pytest.raises(CollectError, match="inconsistent"):
        _warm_hint_overrides(evidence, plan)


def test_role_logs_are_bound_to_the_exact_instance_on_a_shared_host(
    tmp_path: Path,
) -> None:
    evidence = tmp_path / "evidence"
    f1 = evidence / "diagnostics" / "worker-host" / "F1.log" / "iceccd.log"
    f2 = evidence / "diagnostics" / "worker-host" / "F2.log" / "iceccd.log"
    f1.parent.mkdir(parents=True)
    f2.parent.mkdir(parents=True)
    f1.write_text("F1\n", encoding="utf-8")
    f2.write_text("F2\n", encoding="utf-8")

    assert (
        _one_role_log(evidence, {"host": "worker-host", "name": "F1", "role": "F"})
        == f1
    )
    assert (
        _one_role_log(evidence, {"host": "worker-host", "name": "F2", "role": "F"})
        == f2
    )
    assert (
        _one_role_log(evidence, {"host": "worker-host", "name": "F3", "role": "F"})
        is None
    )

    f2.unlink()
    f2.symlink_to(f1)
    with pytest.raises(CollectError, match="unsafe F2 log"):
        _one_role_log(evidence, {"host": "worker-host", "name": "F2", "role": "F"})


def test_event_log_must_cover_and_bind_the_declared_timeline(tmp_path: Path) -> None:
    farm = load_farm_spec(farm_fixture.example_farm_path())
    scenario = load_scenario_spec(INTEGRATION / "scenarios" / "S00-smoke.json", farm)
    scenario.data["timeline"] = [
        {"action": "restart", "instance": "F1", "trigger": "job 2"}
    ]
    evidence = tmp_path / "evidence"
    event_path = evidence / "events" / "events.json"
    event_path.parent.mkdir(parents=True)
    event_path.write_text(
        json.dumps(
            {
                "events": [
                    {
                        "action": "restart",
                        "event_epoch": 1,
                        "event_index": 0,
                        "fired_ms": 1234,
                        "instance": "F1",
                        "last_dispatched_job": 2,
                        "trigger": "job 2",
                        "workload_dispatch_count": 2,
                    }
                ]
            }
        ),
        encoding="utf-8",
    )
    assert _event_log(evidence, scenario)[0]["event_epoch"] == 1

    event_path.write_text('{"events": []}', encoding="utf-8")
    with pytest.raises(CollectError, match="complete scenario timeline"):
        _event_log(evidence, scenario)


def test_endpoint_version_is_resolved_at_dispatch_from_transition_receipts() -> None:
    instance = {"name": "F1", "role": "F", "version": 43}
    events = [
        {
            "action": "restart",
            "fired_ms": 500,
            "instance": "F1",
        },
        {
            "action": "upgrade",
            "fired_ms": 1000,
            "instance": "F1",
            "receipt": {"after": {"image": "p50s4-fixture"}},
        },
        {
            "action": "env_set",
            "fired_ms": 1500,
            "instance": "F1",
        },
        {
            "action": "downgrade",
            "fired_ms": 2000,
            "instance": "F1",
            "receipt": {"after": {"image": "p43-fixture"}},
        },
    ]

    assert _instance_version_at(instance, events, 999) == 43
    assert _instance_version_at(instance, events, 1000) == 50
    assert _instance_version_at(instance, events, 1999) == 50
    assert _instance_version_at(instance, events, 2000) == 43

    malformed = copy.deepcopy(events)
    malformed[1]["receipt"] = {"after": {}}
    with pytest.raises(CollectError, match="after image"):
        _instance_version_at(instance, malformed, 1000)


def test_scheduler_generation_resolves_same_second_transition_epoch() -> None:
    events = [
        {
            "action": "upgrade",
            "fired_ms": 1260,
            "instance": "S1",
        }
    ]

    assert _scheduler_dispatch_epoch(events, "S1", 1000, 1) == 0
    assert _scheduler_dispatch_epoch(events, "S1", 1000, 2) == 1
    assert _scheduler_dispatch_epoch(events, "S1", 1260, 2) == 1


def test_scheduler_generation_epoch_fails_closed_beyond_same_second() -> None:
    events = [
        {
            "action": "upgrade",
            "fired_ms": 2260,
            "instance": "S1",
        }
    ]

    with pytest.raises(CollectError, match="disagrees with the dispatch timestamp"):
        _scheduler_dispatch_epoch(events, "S1", 1000, 2)
    with pytest.raises(CollectError, match="exceeds declared"):
        _scheduler_dispatch_epoch(events, "S1", 1000, 3)
    with pytest.raises(CollectError, match="precedes the dispatch event epoch"):
        _scheduler_dispatch_epoch(events, "S1", 3000, 1)


def test_active_scheduler_generation_uses_replacement_start_boundary() -> None:
    events = [
        {
            "action": "scheduler-loss-active",
            "fired_ms": 1_789_042_945_000,
            "instance": "S1",
            "receipt": {
                "after": {"started_at": "2026-09-10T12:21:36.251650806Z"}
            },
        }
    ]

    assert _scheduler_dispatch_epoch(events, "S1", 1_789_042_942_000, 2) == 1
    with pytest.raises(CollectError, match="disagrees with the dispatch timestamp"):
        _scheduler_dispatch_epoch(events, "S1", 1_789_042_896_000, 2)

    events[0]["receipt"]["after"]["started_at"] = "not-a-timestamp"
    with pytest.raises(CollectError, match="disagrees with the dispatch timestamp"):
        _scheduler_dispatch_epoch(events, "S1", 1_789_042_942_000, 2)


def test_client_transition_environment_adds_and_removes_explicit_mode() -> None:
    assert _transition_target_env(
        {"env": {}},
        {"action": "upgrade", "image": "new"},
        "C",
        "p50s4-fixture",
    ) == {"ICECC_P50_MODE": "on"}
    assert _transition_target_env(
        {
            "env": {
                "ICECC_P50_FAULT_INJECTION": "P29_INTERNER_FAIL_ONCE",
                "ICECC_P50_MODE": "on",
            }
        },
        {"action": "downgrade", "image": "old"},
        "C",
        "p43-fixture",
    ) == {}


@pytest.mark.parametrize(
    "ancestor", ("instances", "client", "results", "workload", "jobs")
)
def test_checkpoint_result_path_rejects_symlinked_ancestors(
    tmp_path: Path, ancestor: str
) -> None:
    evidence = tmp_path / "evidence"
    result_root = evidence / "instances" / "C1" / "results"
    result = result_root / "workload" / "A" / "jobs" / "000001"
    result.mkdir(parents=True)
    (result / "result.tsv").write_text("row\n", encoding="utf-8")
    assert _checkpoint_result_path(
        evidence, "C1", "A", "jobs/000001/result.tsv"
    ) == result / "result.tsv"

    targets = {
        "instances": evidence / "instances",
        "client": evidence / "instances" / "C1",
        "results": result_root,
        "workload": result_root / "workload",
        "jobs": result_root / "workload" / "A" / "jobs",
    }
    target = targets[ancestor]
    real = tmp_path / f"real-{ancestor}"
    target.rename(real)
    target.symlink_to(real, target_is_directory=True)
    assert _checkpoint_result_path(
        evidence, "C1", "A", "jobs/000001/result.tsv"
    ) is None


class _LiveCollection:
    def __init__(
        self,
        plan: dict[str, object],
        source_root: Path,
        *,
        stopped: frozenset[str] = frozenset(),
        f_init: bool | None = True,
    ) -> None:
        self.plan = plan
        self.source_root = source_root
        self.stopped = stopped
        self.f_init = f_init
        topology = plan["topology"]
        assert isinstance(topology, dict)
        instances = topology["instances"]
        assert isinstance(instances, list)
        self.ids = {
            instance["name"]: f"{index + 1:064x}"
            for index, instance in enumerate(instances)
        }
        self.names = {value: key for key, value in self.ids.items()}

    def _name_from_argv(self, command: PlannedCommand) -> str:
        for container_id, name in self.names.items():
            if container_id in command.argv or any(
                item.startswith(container_id + ":") for item in command.argv
            ):
                return name
        for name in self.ids:
            if f"icefarm-{self.plan['run_id']}-{name}" in command.argv:
                return name
        raise AssertionError(f"command has no fixture container: {command}")

    def invoke(self, command: PlannedCommand) -> CommandResult:
        if command.phase == "collect.authenticate":
            name = self._name_from_argv(command)
            return CommandResult(
                0,
                json.dumps(
                    {
                        "Config": {
                            "Labels": {
                                "icefarm.instance": name,
                                "icefarm.run": self.plan["run_id"],
                            }
                        },
                        "Id": self.ids[name],
                        "HostConfig": (
                            {
                                "Init": self.f_init,
                                "Ulimits": [
                                    {"Name": "nofile", "Soft": 65536, "Hard": 65536}
                                ],
                            }
                            if name == "F1" and self.f_init is not None
                            else {}
                        ),
                        "State": {"Running": name not in self.stopped},
                    }
                ),
                "",
            )
        if command.phase == "collect.top":
            name = self._name_from_argv(command)
            assert name not in self.stopped
            content = (
                "1 icecc-cache-service cache\n" if name == "F1" else "1 init idle\n"
            )
            return CommandResult(0, content, "")
        if command.phase == "collect.stats":
            assert self._name_from_argv(command) not in self.stopped
            return CommandResult(0, "{}\n", "")
        if command.phase == "collect.stop":
            assert self._name_from_argv(command) not in self.stopped
            return CommandResult(0, self._name_from_argv(command) + "\n", "")
        if command.phase == "diagnostics.inspect":
            name = self._name_from_argv(command)
            document = {
                "HostConfig": (
                    {
                        "Init": self.f_init,
                        "Ulimits": [
                            {"Name": "nofile", "Soft": 65536, "Hard": 65536}
                        ],
                    }
                    if name == "F1" and self.f_init is not None
                    else {}
                )
            }
            argv = (
                decode_ssh_payload(command.argv)
                if command.argv and command.argv[0] == "ssh"
                else command.argv
            )
            if argv[-5:] != (
                "container",
                "inspect",
                "--format",
                "{{json .}}",
                self.ids[name],
            ):
                return CommandResult(0, json.dumps([document]), "")
            return CommandResult(0, json.dumps(document), "")
        if command.phase == "diagnostics.logs":
            return CommandResult(0, "container output\n", "")
        if command.phase == "diagnostics.sync-log":
            destination = Path(command.argv[-1])
            source = self.source_root / "diagnostics" / command.host / destination.name
            if source.is_dir():
                shutil.copytree(source, destination, dirs_exist_ok=True)
            else:
                destination.mkdir(parents=True, exist_ok=True)
            return CommandResult(0, "", "")
        if command.phase == "collect.results":
            name = self._name_from_argv(command)
            shutil.copytree(
                self.source_root / f"{name}.results",
                Path(command.argv[-1]),
                dirs_exist_ok=True,
            )
            return CommandResult(0, "", "")
        raise AssertionError(f"unexpected collection command: {command.phase}")


def test_live_network_diagnostics_precede_stop(tmp_path: Path, monkeypatch) -> None:
    from types import SimpleNamespace
    from farmharness.integration import lifecycle

    farm, scenario, plan, root = _raw_collection(tmp_path)
    plan["worker_endpoint_contract"] = "icefarm-live-bridge-endpoint-v1"
    monkeypatch.setattr(lifecycle, "_netem_bindings", lambda _: (SimpleNamespace(instance="F1"),))

    class ShapedCollection(_LiveCollection):
        def invoke(self, command):
            if command.phase == "diagnostics.tc":
                assert self._name_from_argv(command) == "F1"
                assert "F1" not in self.stopped, "tc cannot exec after worker stop"
                return CommandResult(0, "qdisc netem 100mbit delay 2ms\n", "")
            result = super().invoke(command)
            if command.phase == "collect.stop":
                self.stopped = self.stopped | {self._name_from_argv(command)}
            return result

    recorder = RecordingTransport(ShapedCollection(plan, root))
    _snapshot_live_evidence(farm, plan, tmp_path / "snapshot", recorder)
    host = next(item["host"] for item in plan["topology"]["instances"] if item["name"] == "F1")
    live = json.loads((tmp_path / "snapshot" / "diagnostics" / host / "F1.live-inspect").read_text())
    assert live["State"]["Running"] is True
    phases = [command.phase for command in recorder.commands]
    assert phases.count("diagnostics.tc") == 1
    assert phases.index("diagnostics.tc") < phases.index("collect.stop")
    assert phases.index("diagnostics.sync-log") > max(
        i for i, phase in enumerate(phases) if phase == "collect.stop"
    )


def test_live_collection_authenticates_samples_then_freezes_before_copy(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    delegate = _LiveCollection(plan, root)
    recorder = RecordingTransport(delegate)

    bundle = collect_bundle(farm, scenario, plan, recorder=recorder)

    assert bundle["rows"][0]["session_outcome"] == "committed"
    phases = [command.phase for command in recorder.commands]
    first_stop = phases.index("collect.stop")
    assert all(
        phases.index(phase) < first_stop
        for phase in ("collect.authenticate", "collect.top", "collect.stats")
    )
    assert phases.index("diagnostics.inspect") > max(
        index for index, phase in enumerate(phases) if phase == "collect.stop"
    )
    assert phases.index("collect.results") > phases.index("diagnostics.sync-log")
    stopped = [
        delegate._name_from_argv(command)
        for command in recorder.commands
        if command.phase == "collect.stop"
    ]
    assert stopped == ["C1", "F1", "S1"]
    stop_commands = [
        command for command in recorder.commands if command.phase == "collect.stop"
    ]
    assert plan["timeouts"]["collect_s"] > 30
    assert all(
        command.timeout_s == plan["timeouts"]["collect_s"]
        for command in stop_commands
    )
    assert all(
        command.argv[-5:-1] == ("container", "stop", "--time", "10")
        for command in stop_commands
    )
    f_inspect = next(
        command
        for command in recorder.commands
        if command.phase == "diagnostics.inspect"
        and delegate._name_from_argv(command) == "F1"
    )
    inspect_argv = (
        decode_ssh_payload(f_inspect.argv)
        if f_inspect.argv[0] == "ssh"
        else f_inspect.argv
    )
    assert inspect_argv[-5:] == (
        "container",
        "inspect",
        "--format",
        "{{json .}}",
        delegate.ids["F1"],
    )


@pytest.mark.parametrize("f_init", (False, None))
def test_live_collection_refuses_false_or_missing_f_init(
    tmp_path: Path, f_init: bool | None
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    delegate = _LiveCollection(plan, root, f_init=f_init)
    with pytest.raises(CollectError, match="Init=true"):
        collect_bundle(farm, scenario, plan, recorder=RecordingTransport(delegate))


def test_live_collection_accepts_only_event_authenticated_stopped_instance(
    tmp_path: Path,
) -> None:
    farm, scenario, plan, root = _raw_collection(tmp_path)
    del scenario
    delegate = _LiveCollection(plan, root, stopped=frozenset({"F1"}))
    recorder = RecordingTransport(delegate)
    destination = tmp_path / "snapshot"

    _snapshot_live_evidence(
        farm,
        plan,
        destination,
        recorder,
        stopped_instances=frozenset({"F1"}),
    )

    sampled = {
        delegate._name_from_argv(command)
        for command in recorder.commands
        if command.phase in {"collect.top", "collect.stats", "collect.stop"}
    }
    assert "F1" not in sampled
    f1 = next(item for item in plan["topology"]["instances"] if item["name"] == "F1")
    assert json.loads(
        (destination / "diagnostics" / f1["host"] / "F1.stats").read_text()
    ) == {"container_running": False, "reason": "authenticated-kill-event"}


def test_authenticated_rejoin_line_uses_exact_window():
    window = b"login F1\nRELOGIN F1\nassignment\n"
    prefix = b"RELOGIN F1\nold assignment\n"
    receipt = {"offset": len(prefix), "bytes": len(window),
               "sha256": hashlib.sha256(window).hexdigest(),
               "cache_line": "RELOGIN F1"}
    assert _authenticated_rejoin_line(prefix + window, receipt) == 4


@pytest.mark.parametrize("fault", [None, "reordered", "worker", "generation", "receipt"])
def test_worker_rejoin_boundaries_bind_order_and_receipts(tmp_path, monkeypatch, fault):
    from types import SimpleNamespace
    from farmharness.integration import collect

    raw = b"ICECREAM scheduler 1 starting up, port 1\n"
    events = []
    for index in range(3):
        window = f"login F1 {index}\nRELOGIN F1 {index}\n".encode()
        rejoin = {"offset": len(raw), "bytes": len(window),
                  "sha256": hashlib.sha256(window).hexdigest(),
                  "cache_line": f"RELOGIN F1 {index}"}
        raw += window
        events.append({"action": "restart", "instance": "F1", "event_epoch": index + 1,
                       "receipt": {"after": {"container_id": "a" * 64, "started_at": str(index)},
                                   "coordination": {"scheduler_rejoin": rejoin}}})
    if fault == "reordered":
        events[1]["receipt"]["coordination"] = events[0]["receipt"]["coordination"]
    elif fault == "worker":
        events[1]["instance"] = "F2"
    elif fault == "generation":
        raw += b"ICECREAM scheduler 1 starting up, port 1\n"
    path = tmp_path / "scheduler.log"
    path.write_bytes(raw)
    monkeypatch.setattr(collect, "_one_role_log", lambda *_: path)
    validated = []

    def validate(receipt, event, scenario, index, **kwargs):
        if fault == "receipt":
            raise CollectError("invalid restart receipt")
        validated.append(index)

    monkeypatch.setattr(collect, "_validate_worker_restart_receipt", validate)
    scenario = SimpleNamespace(data={"expect": {"engagement": "s70-b4-worker-bounces"}})
    plan = {"topology": {"instances": [{"name": "S1", "role": "S"}]}}
    if fault:
        with pytest.raises(CollectError):
            collect._worker_rejoin_boundaries(None, scenario, plan, tmp_path, events)
    else:
        result = collect._worker_rejoin_boundaries(None, scenario, plan, tmp_path, events)
        assert validated == [0, 1, 2]
        assert [r["scheduler_rejoin_line"] for r in result] == [3, 5, 7]
        assert [r["event_epoch"] for r in result] == [1, 2, 3]


@pytest.mark.parametrize("fault", ["hash", "offset", "size", "missing", "duplicate", "partial", "bool"])
def test_authenticated_rejoin_line_rejects_invalid_boundary(fault):
    window = b"login F1\nRELOGIN F1\n"
    prefix = b"old\n"
    if fault == "duplicate":
        window += b"RELOGIN F1\n"
    if fault == "partial":
        window = window[:-1]
    receipt = {"offset": len(prefix), "bytes": len(window),
               "sha256": hashlib.sha256(window).hexdigest(),
               "cache_line": "RELOGIN F1"}
    if fault == "hash":
        receipt["sha256"] = "0" * 64
    elif fault == "offset":
        receipt["offset"] = 2
    elif fault == "size":
        receipt["bytes"] += 1
    elif fault == "missing":
        receipt["cache_line"] = "RELOGIN F2"
    elif fault == "bool":
        receipt["offset"] = True
    with pytest.raises(CollectError):
        _authenticated_rejoin_line(prefix + window, receipt)
