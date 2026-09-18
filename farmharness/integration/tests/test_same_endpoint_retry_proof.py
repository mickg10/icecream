from copy import deepcopy

import pytest

from farmharness.integration.retry_decision import same_endpoint_decision_valid


def same_worker_bundle(kind="source"):
    from farmharness.integration.tests.test_verdict import (
        _s70_b4_worker_source_transfer_recovery_bundle,
        _s70_b4_worker_uncommitted_transport_recovery_bundle,
    )

    bundle = (
        _s70_b4_worker_source_transfer_recovery_bundle()
        if kind == "source"
        else _s70_b4_worker_uncommitted_transport_recovery_bundle()
    )
    observations = bundle["observations"]
    key = (
        "failed_p50_source_transfers"
        if kind == "source"
        else "failed_p50_uncommitted_transports"
    )
    failure = observations[key]["records"][0]
    binding = observations["successful_strict_p50_retry_bindings"][0]
    binding["final_worker"] = binding["first_worker"]
    failure["retry_endpoint"] = failure["failed_endpoint"]
    failure["source_result_status"] = None
    observations["assignment_lifecycle"][100]["attempts"][1]["worker"] = failure[
        "worker"
    ]
    bundle["rows"][100]["cs"] = failure["worker"]
    host, port = failure["retry_endpoint"].rsplit(":", 1)
    proof = evidence()["same_endpoint_decision"]
    proof.update(
        generation=binding["final_generation"],
        job=failure["retry_scheduler_job"],
        epoch=failure["retry_assignment_epoch"],
        nonce=failure["retry_assignment_nonce"],
        worker=binding["final_worker"],
        failed_host=host,
        selected_host=host,
        failed_port=int(port),
        selected_port=int(port),
        timestamp_ms=binding["final_dispatch_ms"],
        put_ms=binding["final_dispatch_ms"],
    )
    failure["same_endpoint_decision"] = proof
    return bundle


@pytest.mark.parametrize("kind", ["source", "transport"])
def test_full_bundle_accepts_exact_same_worker_retry(kind):
    from farmharness.integration.verdict import evaluate_bundle

    result = evaluate_bundle(same_worker_bundle(kind))
    assert result["status"] == "PASS", result


@pytest.mark.parametrize(
    "mutation", ["missing", "busy", "generation", "dispatch", "nonce", "worker"]
)
@pytest.mark.parametrize("kind", ["source", "transport"])
def test_full_bundle_rejects_unproved_same_worker_retry(mutation, kind):
    from farmharness.integration.verdict import evaluate_bundle

    bundle = same_worker_bundle(kind)
    key = (
        "failed_p50_source_transfers"
        if kind == "source"
        else "failed_p50_uncommitted_transports"
    )
    failure = bundle["observations"][key]["records"][0]
    proof = failure["same_endpoint_decision"]
    if mutation == "missing":
        del failure["same_endpoint_decision"]
    elif mutation == "busy":
        proof["compatible_alternative"] = 1
    elif mutation == "generation":
        proof["generation"] += 1
    elif mutation == "dispatch":
        proof["put_ms"] += 1
    elif mutation == "nonce":
        proof["nonce"] += 1
    else:
        proof["worker"] = "F2"
    assert evaluate_bundle(bundle)["status"] == "FAIL"


def evidence():
    return {
        "failed_endpoint": "10.0.0.1:23004",
        "retry_endpoint": "10.0.0.1:23004",
        "retry_scheduler_job": 9,
        "retry_assignment_epoch": 2,
        "retry_assignment_nonce": 11,
        "same_endpoint_decision": {
            "generation": 1,
            "job": 9,
            "epoch": 2,
            "nonce": 11,
            "failed_host": "10.0.0.1",
            "failed_port": 23004,
            "selected_host": "10.0.0.1",
            "selected_port": 23004,
            "profile": 1,
            "compatible_alternative": 0,
            "line": 5,
            "timestamp_ms": 1000,
            "new_line": 3,
            "put_line": 6,
            "put_ms": 1000,
            "worker": "F2",
            "source_path": "diagnostics/host/S1.log/scheduler.log",
        },
    }


def test_exact_same_endpoint_proof_and_dispatch():
    item = evidence()
    binding = {
        "final_generation": 1,
        "final_scheduler_job": 9,
        "final_dispatch_ms": 1000,
        "first_worker": "F2",
        "final_worker": "F2",
    }
    assert same_endpoint_decision_valid(item, binding)
    for key in binding:
        bad = dict(binding, **{key: "mismatch"})
        assert not same_endpoint_decision_valid(item, bad)


@pytest.mark.parametrize(
    "field,value",
    [
        ("compatible_alternative", 1),
        ("compatible_alternative", False),
        ("job", 8),
        ("epoch", 3),
        ("nonce", 12),
        ("nonce", 2**64),
        ("job", 2**32),
        ("profile", 2),
        ("profile", True),
        ("failed_host", "other"),
        ("selected_port", 23005),
        ("failed_port", 65536),
        ("line", 6),
        ("new_line", 5),
        ("timestamp_ms", 1001),
        ("generation", True),
        ("worker", ""),
        ("source_path", ""),
    ],
)
def test_mismatched_or_busy_alternative_proof_rejected(field, value):
    item = deepcopy(evidence())
    item["same_endpoint_decision"][field] = value
    assert not same_endpoint_decision_valid(item)


def test_missing_or_extra_proof_fields_rejected():
    item = evidence()
    proof = item.pop("same_endpoint_decision")
    assert not same_endpoint_decision_valid(item)
    for key in proof:
        item["same_endpoint_decision"] = {k: v for k, v in proof.items() if k != key}
        assert not same_endpoint_decision_valid(item)
    item["same_endpoint_decision"] = dict(proof, invented=True)
    assert not same_endpoint_decision_valid(item)


@pytest.mark.parametrize("kind", ["source", "transport"])
def test_collector_same_endpoint_requires_exact_scheduler_decision(kind):
    from farmharness.integration.collect import (
        CollectError,
        _source_transfer_failure_observation,
        _uncommitted_transport_failure_observation,
    )
    from farmharness.integration.tests.test_collect_and_report import (
        _source_transfer_failure_kwargs,
        _uncommitted_transport_failure_kwargs,
    )

    fixture, collect = (
        (_source_transfer_failure_kwargs, _source_transfer_failure_observation)
        if kind == "source"
        else (
            _uncommitted_transport_failure_kwargs,
            _uncommitted_transport_failure_observation,
        )
    )
    args = fixture(mutation="same-endpoint")
    identity = args["retry_identity"]
    host, port = args["retry_assignment"]["endpoint"].rsplit(":", 1)
    proof = evidence()["same_endpoint_decision"]
    proof.update(
        job=identity["scheduler_job"],
        epoch=identity["assignment_epoch"],
        nonce=identity["assignment_nonce"],
        failed_host=host,
        selected_host=host,
        failed_port=int(port),
        selected_port=int(port),
        worker="F1",
    )
    record = collect(**args, retry_decisions=[proof])
    assert record["same_endpoint_decision"] == proof
    assert same_endpoint_decision_valid(record)
    for decisions in (
        [],
        [proof, proof],
        [dict(proof, compatible_alternative=1)],
        [dict(proof, nonce=proof["nonce"] + 1)],
    ):
        with pytest.raises(CollectError):
            collect(**args, retry_decisions=decisions)
