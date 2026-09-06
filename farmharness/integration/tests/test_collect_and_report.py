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
    _canary_assignment_claims,
    _checkpoint_result_path,
    _assignment_preference,
    _control_observations,
    _event_log,
    _instance_version_at,
    _one_role_log,
    _p29_interner_faults,
    _retained_log_witness,
    _parse_logins,
    _snapshot_live_evidence,
    _transition_target_env,
    _validate_orphan_recovery_markers,
    _warm_hint_overrides,
    collect_bundle,
    load_verified_bundle,
)
from farmharness.integration.farm_spec import load_farm_spec
from farmharness.integration.images import RecordingTransport
from farmharness.integration.lifecycle import bundle_root
from farmharness.integration.remote import CommandResult, PlannedCommand
from farmharness.integration.report import ReportError, report_bundle, verify_bundle
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness.integration.schema_validation import canonical_bytes
from farmharness.integration.verdict import _assignment_preference_errors


INTEGRATION = Path(__file__).resolve().parents[1]
SHA = "a" * 64
C_GUID = "1" * 32


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
    path.write_bytes(prefix + (fresh + "\n").encode())

    assert _retained_log_witness(tmp_path, scheduler, len(prefix), fresh)
    assert not _retained_log_witness(tmp_path, scheduler, len(prefix), stale)
    assert not _retained_log_witness(tmp_path, scheduler, path.stat().st_size + 1, fresh)
    assert not _retained_log_witness(
        tmp_path, {"host": "h1", "name": "F1", "role": "F"}, 0, fresh
    )
    assert not _retained_log_witness(tmp_path, scheduler, len(prefix), "absent")


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
                "schema": "icecream-p50-source-result-v1",
                "source_mutex_service_ns": 2_000_000,
                "source_mutex_wait_ns": 1_000,
                "status": 0,
                "system_source_reuse": False,
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
        "\n".join(debug_lines) + "\n",
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
    ) -> None:
        self.plan = plan
        self.source_root = source_root
        self.stopped = stopped
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
            return CommandResult(0, "[]\n", "")
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
