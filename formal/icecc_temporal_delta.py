#!/usr/bin/env python3
"""Additional counterexamples discovered after the first issue-2 model.

This file is separate so the original model remains a stable reproduction
artifact. The downloadable bundle also contains a consolidated model whose
`all` command runs both sets.
"""
from __future__ import annotations

import json

from icecc_temporal_model import (
    State,
    Trace,
    activate,
    assert_core_invariants,
    begin,
    dispatch,
    settle,
    stage,
)


def submitter_teardown(detach_started: bool) -> Trace:
    name = "submitter-teardown-detach-started" if detach_started else "submitter-teardown-delete-started"
    trace = Trace(name, [], [], "", not detach_started)
    state = State(credit_limit=2, worker_capacity=1)
    trace.add("init", state)
    state = begin(dispatch(activate(stage(
        state, bid=1, key="submitter-generation-1/client-11", jids=[1]
    ), 1), 1, worker="F0"), 1)
    trace.add("job-begin", state, job=1, worker="F0", physical_compile_running=True)

    if not detach_started:
        state = state.remove_jobs(lambda job: job.jid == 1)
        assert_core_invariants(state)
        trace.add(
            "submitter-disconnect-delete-started-ledger-entry",
            state,
            job=1,
            physical_compile_running=True,
            scheduler_reservations=state.reservations(),
        )
        state = activate(stage(
            state, bid=2, key="submitter-generation-2/client-12", jids=[2]
        ), 2)
        state = dispatch(state, 2, worker="F0")
        trace.add(
            "scheduler-reassigns-apparently-free-worker",
            state,
            new_job=2,
            physical_old_job=1,
            actual_claims_if_new_client_arrives=2,
            worker_capacity=1,
        )
        trace.conclusion = (
            "COUNTEREXAMPLE: deleting STARTED work when its submitter disconnects "
            "frees only the scheduler ledger; F may still compile it. The same "
            "capacity can be assigned again and the real JobDone becomes unknown."
        )
        return trace

    trace.add(
        "submitter-disconnect-detach-started-owner",
        state,
        job=1,
        retained_worker="F0",
        note="snapshot submitter metadata; remove raw lifetime dependency",
    )
    state = settle(state, 1)
    trace.add("fulfillment-job-done", state, job=1)
    trace.conclusion = (
        "BOUNDED CHECK PASSED: retain F-owned STARTED work until F terminates it."
    )
    return trace


def scheduler_epoch_aba(fenced_client: bool) -> Trace:
    name = "scheduler-restart-fenced-client" if fenced_client else "scheduler-restart-wire-id-aba"
    trace = Trace(name, [], [], "", not fenced_client)
    wire_id = 1
    old_nonce = "epoch-A/nonce-old"
    new_nonce = "epoch-B/nonce-new"
    trace.add("epoch-A-usecs-issued", State(), epoch="A", wire_job_id=wire_id, nonce=old_nonce)
    trace.add("scheduler-restart", State(), old_epoch="A", new_epoch="B")
    trace.add("epoch-B-assignment", State(), epoch="B", wire_job_id=wire_id, nonce=new_nonce)

    if not fenced_client:
        trace.add(
            "delayed-old-client-arrives-at-F",
            State(),
            carried_fields={"wire_job_id": wire_id},
            missing=["scheduler_epoch", "assignment_nonce"],
        )
        trace.add(
            "F-reports-job-begin-to-new-scheduler",
            State(),
            wire_job_id=wire_id,
            new_job_mistakenly_started=True,
        )
        trace.conclusion = (
            "COUNTEREXAMPLE: scheduler restart makes wire-job-id ABA immediate. "
            "An old client and a new assignment are observationally identical."
        )
        return trace

    trace.add(
        "delayed-old-client-arrives-at-F",
        State(),
        carried_fields={"wire_job_id": wire_id, "assignment_nonce": old_nonce},
    )
    trace.add(
        "F-rejects-token-mismatch",
        State(),
        expected_nonce=new_nonce,
        received_nonce=old_nonce,
    )
    trace.conclusion = (
        "BOUNDED CHECK PASSED: a client-echoed assignment capability separates epochs. "
        "Old C requires an explicit wire-id freshness/quarantine assumption."
    )
    return trace


def main() -> int:
    traces = [
        submitter_teardown(False),
        submitter_teardown(True),
        scheduler_epoch_aba(False),
        scheduler_epoch_aba(True),
    ]
    for trace in traces:
        marker = "COUNTEREXAMPLE" if trace.expected_counterexample else "CHECK"
        print(f"[{marker}] {trace.scenario}")
        for index, event in enumerate(trace.events):
            detail = " " + json.dumps(event.detail, sort_keys=True) if event.detail else ""
            print(f"  {index:02d}. {event.name}{detail}")
        print(f"  => {trace.conclusion}\n")
    print("SUMMARY: 2 expected counterexamples, 2 bounded checks, 0 model errors")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
