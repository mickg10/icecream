import copy

import pytest

from farmharness.integration.collect import CollectError, _resource_failure_observation


def evidence():
    first = dict(scheduler_job=10, assignment_epoch=7, assignment_nonce=8,
                 c_guid=9, tu_seq=0, line=1)
    final = dict(first, scheduler_job=11, assignment_nonce=12, tu_seq=1, line=5)
    assignments = [dict(scheduler_job=10, worker="F1", endpoint="a:1", line=2),
                   dict(scheduler_job=11, worker="F2", endpoint="b:2", line=6)]
    log = "\n".join([
        "P50 assignment identity bound for job 10 epoch 7 nonce 8 c_guid 9 tu_seq 0",
        "Have to use host a:1 - Job ID: 10",
        "P50 worker resource failure normalized to Error 106 for job 10 epoch 7 nonce 8 c_guid 9 tu_seq 0",
        "P50 assignment failed; requesting one fresh strict-P50 remote assignment; avoiding failed endpoint a:1",
        "P50 assignment identity bound for job 11 epoch 7 nonce 12 c_guid 9 tu_seq 1",
        "Have to use host b:2 - Job ID: 11",
    ])
    results = {}
    for identity in (first, final):
        result = {k: v for k, v in identity.items() if k != "line"}
        result["job_id"] = result.pop("scheduler_job")
        results[(result["job_id"], result["assignment_epoch"], result["assignment_nonce"])] = result
    return dict(log_text=log, assignments=assignments, identities=[first, final],
                compile_identities=results, row_job_id="C1:A:1:11")


def test_resource_retry_binds_both_received_results():
    result = _resource_failure_observation(**evidence())
    assert result["normalized_error"] == 106
    assert result["first_worker"] == "F1"
    assert result["final_worker"] == "F2"
    assert result["compile_results_present"] is True


@pytest.mark.parametrize("mutation", ["missing_result", "wrong_guid", "wrong_nonce",
    "same_worker", "same_endpoint", "wrong_exclusion", "duplicate", "out_of_order",
    "extra_attempt", "malformed"])
def test_resource_retry_rejects_unbound_evidence(mutation):
    data = copy.deepcopy(evidence())
    if mutation == "missing_result":
        del data["compile_identities"][(10, 7, 8)]
    elif mutation == "wrong_guid":
        data["compile_identities"][(10, 7, 8)]["c_guid"] = 99
    elif mutation == "wrong_nonce":
        data["identities"][0]["assignment_nonce"] = 99
    elif mutation == "same_worker":
        data["assignments"][1]["worker"] = "F1"
    elif mutation == "same_endpoint":
        data["assignments"][1]["endpoint"] = "a:1"
    elif mutation == "wrong_exclusion":
        data["log_text"] = data["log_text"].replace("avoiding failed endpoint a:1", "avoiding failed endpoint b:2")
    elif mutation == "duplicate":
        data["log_text"] += "\n" + data["log_text"].splitlines()[2]
    elif mutation == "out_of_order":
        data["identities"][1]["line"] = 3
    elif mutation == "extra_attempt":
        data["assignments"].append(data["assignments"][1])
    else:
        data["log_text"] = data["log_text"].replace("Error 106 for job", "Error 999 for job")
    with pytest.raises(CollectError):
        _resource_failure_observation(**data)


def test_old_logs_do_not_invent_resource_witness():
    data = evidence()
    data["log_text"] = data["log_text"].replace("P50 worker resource failure normalized", "old diagnostic")
    assert _resource_failure_observation(**data) is None


def resource_bundle():
    from farmharness.integration.tests.test_verdict import _s95_disk_fill_bundle
    bundle = _s95_disk_fill_bundle()
    row = bundle["rows"][2]
    row.update(cs="F2", tail_present=True, tail_profile="P29V1",
               session_outcome="committed", reuse=False)
    obs = bundle["observations"]
    obs["assignment_lifecycle"][2]["attempts"][1]["worker"] = "F2"
    obs["job_lifecycle"][2].update(dispatch_ms=900, first_dispatch_ms=900,
        final_dispatch_ms=1200, terminal_ms=1225, scheduler_generation=1,
        scheduler_dispatch_line=10)
    record = _resource_failure_observation(**evidence())
    record.update(row_job_id="3", first_generation=1, final_generation=1,
        first_dispatch_ms=900, first_terminal_ms=1150,
        final_dispatch_ms=1200, final_terminal_ms=1225)
    record["first_identity"]["scheduler_job"] = 30
    record["final_identity"]["scheduler_job"] = 31
    obs["p50_resource_failures"] = [record]
    return bundle


def test_s95_accepts_authenticated_inflight_resource_retry():
    from farmharness.integration.verdict import evaluate_bundle
    verdict = evaluate_bundle(resource_bundle())
    assert verdict["status"] == "PASS", verdict


@pytest.mark.parametrize("mutation", ["completed_before_fault", "started_after_fault",
    "no_error106", "wrong_worker",
    "wrong_job", "no_cancellation", "duplicate", "no_witness", "bad_timing"])
def test_s95_resource_retry_fails_closed(mutation):
    from farmharness.integration.verdict import evaluate_bundle
    bundle = resource_bundle()
    obs = bundle["observations"]
    record = obs["p50_resource_failures"][0]
    if mutation == "completed_before_fault":
        record["first_terminal_ms"] = bundle["event_log"][0]["fired_ms"] - 1
    elif mutation == "started_after_fault":
        started = bundle["event_log"][0]["fired_ms"] + 1
        record["first_dispatch_ms"] = started
        obs["job_lifecycle"][2]["first_dispatch_ms"] = started
        obs["job_lifecycle"][2]["dispatch_ms"] = started
    elif mutation == "no_error106":
        obs["error106_job_ids"] = []
    elif mutation == "wrong_worker":
        record["first_worker"] = "F2"
    elif mutation == "wrong_job":
        record["first_identity"]["scheduler_job"] = 99
    elif mutation == "no_cancellation":
        obs["assignment_lifecycle"][2]["attempts"][0]["terminal"] = "completion"
    elif mutation == "duplicate":
        obs["p50_resource_failures"].append(copy.deepcopy(record))
    elif mutation == "no_witness":
        obs["p50_resource_failures"] = []
    else:
        record["first_terminal_ms"] = 1500
    verdict = evaluate_bundle(bundle)
    assert verdict["status"] == "FAIL", verdict
    assert next(c for c in verdict["clauses"] if c["id"] == "engagement.expected")["status"] == "FAIL"
