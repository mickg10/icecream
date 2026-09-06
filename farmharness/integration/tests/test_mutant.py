from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

from farmharness.integration import farmtest
from farmharness.integration.collect import _h3_control_failure_observations
from farmharness.integration.farm_spec import FarmSpecError, load_farm_spec
from farmharness.integration.images import image_bindings
from farmharness.integration.mutant import (
    DAEMON_MUTANT_RECIPE_SCHEMA,
    MUTANT_TRACE_SCHEMA,
    MutantError,
    derive_daemon_mutant,
    derive_scheduler_mutant,
    parse_h3_client_rejections,
    validate_h3_trace,
)
from farmharness.integration.scenario_spec import load_scenario_spec
from farmharness import newgen_farm_env


INTEGRATION = Path(__file__).resolve().parents[1]


def _farm():
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    # Static plan/collector tests need a resolved current-F image but must not
    # claim that the not-yet-built successor has a measured live closure.
    farm.data["authority"]["images"]["p50s4-89917385"]["closure_sha256"] = "a" * 64
    return farm


def _base():
    farm = _farm()
    return farm.data["authority"]["images"]["p50s4-89917385"]


def _trace():
    return [
        {
            "assignment_epoch": 7,
            "assignment_nonce": 11,
            "client_instance": "C1",
            "cache_port": 43123,
            "cache_profile_mask": 1,
            "cache_protocol": 1,
            "emission": "protocol-50-tail-sent",
            "schema": MUTANT_TRACE_SCHEMA,
            "scheduler_instance": "S1",
            "scheduler_job": 1,
            "tail_bytes": 12,
            "tail_hex": "0000a8730000000100000001",
            "worker_instance": "F1",
        }
    ]


def test_mutant_recipe_is_deterministic_and_hash_bound() -> None:
    first = derive_scheduler_mutant("p50s4-89917385", _base())
    second = derive_scheduler_mutant("p50s4-89917385", _base())
    assert first == second
    assert first["kind"] == "scheduler-mutant"
    assert first["base_archive_sha256"] == _base()["archive_sha256"]


def test_daemon_mutant_recipe_binds_sealed_p50_base_and_patch() -> None:
    first = derive_daemon_mutant("p50s4-89917385", _base())
    second = derive_daemon_mutant("p50s4-89917385", _base())
    assert first == second
    assert first["kind"] == "daemon-mutant"
    assert first["recipe_schema"] == DAEMON_MUTANT_RECIPE_SCHEMA
    assert first["base_commit"] == "8991738525d2d34aaafeabeb6ef6790daa79fdfe"
    assert first["base_archive_sha256"] == (
        "1304508377aa9e61484085925b75823acfe52d0b3bbc6921e0de3f84a7973287"
    )


def test_s90_revision_two_recipe_is_exactly_derived_from_sealed_b42() -> None:
    farm = _farm()
    images = farm.data["authority"]["images"]
    candidate = images["p50s90-f-revision-2-candidate"]
    derived = derive_daemon_mutant(
        "p50s4-b42d65e8",
        images["p50s4-b42d65e8"],
        INTEGRATION / "mutants" / "daemon-wire-revision-2.patch",
        label="p50s90-f-revision-2-candidate",
    )

    assert candidate["cache_wire_revision"] == 2
    assert {
        key: candidate[key]
        for key in derived
    } == {
        **derived,
        "patch_path": "mutants/daemon-wire-revision-2.patch",
    }


def test_daemon_mutant_candidate_is_build_selectable_without_runtime_authority() -> None:
    farm = load_farm_spec(INTEGRATION / "farm.example.json")
    [binding] = image_bindings(farm, ["p50s30-f-refusal-mutant-candidate"])
    assert binding.kind == "daemon-mutant"
    assert binding.base_commit == "8991738525d2d34aaafeabeb6ef6790daa79fdfe"
    assert binding.base_archive_sha256 == (
        "1304508377aa9e61484085925b75823acfe52d0b3bbc6921e0de3f84a7973287"
    )
    assert binding.patch_sha256 == (
        "4b1963c4081e2d0f20800db53363940c1ebea128ccdb3665691b270224a7306c"
    )
    assert binding.recipe_sha256 == (
        "ccce2ac2cded430786714d324908ff9374afcd695d625e4fbc61d69184294e56"
    )
    assert binding.expected_closure is None
    assert binding.expected_id is None


def test_daemon_mutant_patch_is_post_hello_refusal_and_docker_bound() -> None:
    patch = (INTEGRATION / "mutants" / "daemon-session-refuse.patch").read_text()
    assert "SessionHello hello = decode_as<SessionHello>(hello_frame);" in patch
    assert "S30 mutant F refuses P50 session" in patch
    assert patch.index("SessionHello hello") < patch.index("S30 mutant F refuses")
    dockerfile = (INTEGRATION / "docker" / "Dockerfile.daemon-mutant").read_text()
    assert '"${MUTANT_PATCH_SHA256}" /tmp/daemon-mutant.patch' in dockerfile
    assert "git apply --check /tmp/daemon-mutant.patch" in dockerfile


def test_daemon_mutant_uses_only_authority_bound_daemon_role_override() -> None:
    farm = _farm()
    image = derive_daemon_mutant("p50s4-89917385", _base())
    image["role_overrides"] = {"daemon": {"sha256": "d" * 64}}
    assert newgen_farm_env._binary_hash(farm.data["authority"], 50, "F", image) == "d" * 64
    assert newgen_farm_env._binary_hash(farm.data["authority"], 50, "S", image) == farm.data["authority"]["role_stores"]["50"]["scheduler"]["sha256"]


def test_farm_refuses_tampered_mutant_recipe(tmp_path: Path) -> None:
    document = json.loads((INTEGRATION / "farm.example.json").read_text())
    document["authority"]["images"]["p50s4-h3-tail-mutant"]["patch_sha256"] = "0" * 64
    path = tmp_path / "farm.json"
    path.write_text(json.dumps(document), encoding="utf-8")
    with pytest.raises(FarmSpecError, match="mutant"):
        load_farm_spec(path)


def test_farm_refuses_malformed_mutant_role_override(tmp_path: Path) -> None:
    document = json.loads((INTEGRATION / "farm.example.json").read_text())
    document["authority"]["images"]["p50s4-h3-tail-mutant"]["role_overrides"] = {
        "scheduler": {"sha256": "not-a-digest"}
    }
    path = tmp_path / "farm.json"
    path.write_text(json.dumps(document), encoding="utf-8")
    with pytest.raises(FarmSpecError, match="role_overrides|sha256"):
        load_farm_spec(path)


def test_trace_requires_scheduler_assignment_witness_and_exact_tail() -> None:
    jobs = [{"scheduler_job": 1, "client": "C1", "worker": "F1"}]
    result = validate_h3_trace(
        _trace(),
        client_instances={"C1"},
        scheduler_instance="S1",
        jobs=jobs,
        worker_instances={"F1"},
    )
    assert result["authenticated"] is True
    assert result["record_count"] == 1

    bad = copy.deepcopy(_trace())
    bad[0]["tail_bytes"] = 8
    with pytest.raises(MutantError, match="12-byte"):
        validate_h3_trace(
            bad, client_instances={"C1"}, scheduler_instance="S1", jobs=jobs
            , worker_instances={"F1"}
        )


def test_client_rejection_requires_exact_unread_tail() -> None:
    texts = (
        "internal error - message not read correctly, message size 63 read 51",
        "internal error - message (USE_CS) not read correctly, message size 84 read 72",
    )
    for text in texts:
        result = parse_h3_client_rejections(text, client_instance="C1")
        assert result[0]["unread_bytes"] == 12
    with pytest.raises(MutantError, match="unread"):
        parse_h3_client_rejections(
            "internal error - message (USE_CS) not read correctly, message size 84 read 80",
            client_instance="C1",
        )


def test_scheduler_patch_records_only_post_send_nonzero_projection() -> None:
    patch = (INTEGRATION / "mutants" / "scheduler-tail.patch").read_text()
    assert "outcome" not in patch
    assert "p50_select_cache_profile(" in patch
    assert "cache_advertisement_is_valid_present(" in patch
    assert "reply_epoch = scheduler_assignment_epoch" in patch
    assert "reply_nonce = fresh_assignment_nonce()" in patch
    assert patch.index("+    const bool sent") < patch.index("protocol-50-tail-sent")


def test_mutant_build_authenticates_patch_inside_docker() -> None:
    dockerfile = (INTEGRATION / "docker" / "Dockerfile.scheduler-mutant").read_text()
    assert "ARG MUTANT_PATCH_SHA256" in dockerfile
    assert '"${MUTANT_PATCH_SHA256}" /tmp/scheduler-tail.patch' in dockerfile
    assert "git apply --check /tmp/scheduler-tail.patch" in dockerfile
    assert "git apply /tmp/scheduler-tail.patch" in dockerfile


def test_h3_failure_collection_binds_dispatch_rejection_and_failed_workload(
    tmp_path: Path,
) -> None:
    farm = _farm()
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "H3-mutant-scheduler.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="h3-failure")
    scheduler_log = tmp_path / "diagnostics" / "tt-quietbox3" / "S1.log"
    scheduler_log.mkdir(parents=True)
    (scheduler_log / "scheduler.log").write_text(
        "\n".join(
            [
                "[S1] 2026-09-05 00:00:00: ICECREAM scheduler test starting up, port 23000",
                "[S1] 2026-09-05 00:00:01: NEW 1 client=C1",
                "[S1] 2026-09-05 00:00:02: put 1 in joblist of F1",
                "[S1] 2026-09-05 00:00:03: BEGIN: 1",
                "[S1] 2026-09-05 00:00:04: END 1 status=1",
            ]
        ),
        encoding="utf-8",
    )
    client_log = tmp_path / "diagnostics" / "tt-quietbox3" / "C1.log"
    client_log.mkdir(parents=True)
    (client_log / "client-daemon.log").write_text(
        "internal error - message (USE_CS) not read correctly, message size 84 read 72\n",
        encoding="utf-8",
    )
    scheduler_result = tmp_path / "instances" / "S1" / "results"
    scheduler_result.mkdir(parents=True)
    (scheduler_result / "h3-mutant.jsonl").write_text(
        json.dumps(_trace()[0]) + "\n", encoding="utf-8"
    )
    workload_result = (
        tmp_path / "instances" / "C1" / "results" / "workload" / "A" / "jobs" / "000001"
    )
    workload_result.mkdir(parents=True)
    (workload_result / "result.tsv").write_text(
        "1\tA\t0\tfoo.cpp\tmissing-1\tUNKNOWN\t100\t200\t1\t"
        + "0" * 64
        + "\t"
        + "1" * 64
        + "\t0\t0\t0\n",
        encoding="ascii",
    )
    observations = _h3_control_failure_observations(
        farm,
        scenario,
        plan,
        tmp_path,
        {"workload": {"status": "COMPLETE_WITH_JOB_FAILURES", "clients": [{"jobs": 1, "failures": 1}]}},
        [],
    )
    failure = observations["h3_control_failure"]
    assert failure["authenticated"] is True
    assert failure["successful_product_rows"] == 0
    assert len(failure["emission"]["records"]) == 1
    assert len(failure["rejections"]) == 1


def test_h3_scenario_loads_and_plan_injects_runner_trace() -> None:
    farm = _farm()
    scenario = load_scenario_spec(
        INTEGRATION / "scenarios" / "H3-mutant-scheduler.json", farm
    )
    plan = farmtest.build_plan(farm, scenario, run_id="h3-test")
    scheduler = next(
        command
        for command in plan["commands"]
        if command["phase"] == "up.start-s"
    )
    assert "ICECC_P50_H3_MUTANT=1" in scheduler["argv"]
    assert "ICECC_P50_H3_TRACE=/results/h3-mutant.jsonl" in scheduler["argv"]
    assert all(command["host"] != "localhost" for command in plan["commands"])
