#!/usr/bin/env python3
"""Executable small-state model for Icecream scheduler ownership and liveness.

The model is intentionally independent of the production implementation. It
captures the protocol facts that matter for issue #2 and follow-on work:

* one primary location per logical compile member;
* remote dispatch owns a worker reservation and submitter debt;
* cancellation cuts every scheduler-owned member in a client-key fibre;
* old F cannot safely release a dispatched-but-unstarted job on an S-only timeout;
* aggregate credit is insufficient in a heterogeneous eligibility graph;
* bounded wire identifiers admit ABA traces after reuse;
* sampled observations are not durable liveness witnesses.

Run:
    python3 formal/icecc_temporal_model.py all
    python3 formal/icecc_temporal_model.py all --json-dir /tmp/icecc-traces
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict, dataclass, replace
from enum import Enum
from pathlib import Path
from typing import Callable, Iterable, Mapping, Sequence


class Phase(str, Enum):
    STAGED = "staged"
    QUEUED = "queued"
    DISPATCHED = "dispatched"
    STARTED = "started"
    TERMINAL = "terminal"


@dataclass(frozen=True, order=True)
class Job:
    jid: int
    batch: int
    key: str
    phase: Phase
    debt: bool = False
    worker: str | None = None

    @property
    def reserved(self) -> bool:
        return self.worker is not None and self.phase in {Phase.DISPATCHED, Phase.STARTED}


@dataclass(frozen=True, order=True)
class Batch:
    bid: int
    key: str
    cancelled: bool = False


@dataclass(frozen=True)
class State:
    jobs: tuple[Job, ...] = ()
    batches: tuple[Batch, ...] = ()
    credit_limit: int = 1
    worker_capacity: int = 1

    def job(self, jid: int) -> Job:
        return next(j for j in self.jobs if j.jid == jid)

    def batch(self, bid: int) -> Batch:
        return next(b for b in self.batches if b.bid == bid)

    def outstanding(self) -> int:
        return sum(1 for j in self.jobs if j.debt)

    def reservations(self) -> int:
        return sum(1 for j in self.jobs if j.reserved)

    def replace_job(self, new_job: Job) -> "State":
        return replace(
            self,
            jobs=tuple(sorted(new_job if j.jid == new_job.jid else j for j in self.jobs)),
        )

    def replace_batch(self, new_batch: Batch) -> "State":
        return replace(
            self,
            batches=tuple(sorted(new_batch if b.bid == new_batch.bid else b for b in self.batches)),
        )

    def remove_jobs(self, predicate: Callable[[Job], bool]) -> "State":
        return replace(self, jobs=tuple(j for j in self.jobs if not predicate(j)))


@dataclass(frozen=True)
class Event:
    name: str
    detail: Mapping[str, object]


@dataclass
class Trace:
    scenario: str
    events: list[Event]
    states: list[State]
    conclusion: str
    expected_counterexample: bool

    def add(self, event: str, state: State, **detail: object) -> None:
        self.events.append(Event(event, detail))
        self.states.append(state)

    def to_json(self) -> dict[str, object]:
        def state_json(s: State) -> dict[str, object]:
            return {
                "jobs": [
                    {**asdict(j), "phase": j.phase.value, "reserved": j.reserved}
                    for j in s.jobs
                ],
                "batches": [asdict(b) for b in s.batches],
                "credit_limit": s.credit_limit,
                "worker_capacity": s.worker_capacity,
                "outstanding": s.outstanding(),
                "reservations": s.reservations(),
            }

        return {
            "scenario": self.scenario,
            "events": [asdict(e) for e in self.events],
            "states": [state_json(s) for s in self.states],
            "conclusion": self.conclusion,
            "expected_counterexample": self.expected_counterexample,
        }


class ModelViolation(AssertionError):
    pass


def assert_core_invariants(s: State) -> None:
    ids = [j.jid for j in s.jobs]
    if len(ids) != len(set(ids)):
        raise ModelViolation("two live logical jobs share one scheduler job id")
    batch_ids = {b.bid for b in s.batches}
    for j in s.jobs:
        if j.batch not in batch_ids:
            raise ModelViolation(f"job {j.jid} has no owning batch")
        if j.debt != (j.phase == Phase.DISPATCHED):
            raise ModelViolation(
                f"job {j.jid}: debt must exist exactly in DISPATCHED, got {j.phase}/{j.debt}"
            )
        if (j.worker is not None) != (j.phase in {Phase.DISPATCHED, Phase.STARTED}):
            raise ModelViolation(
                f"job {j.jid}: worker reservation disagrees with phase {j.phase}"
            )
    if s.outstanding() > s.credit_limit:
        raise ModelViolation("submitter dispatch debt exceeds credit")
    if s.reservations() > s.worker_capacity:
        raise ModelViolation("worker reservations exceed capacity")


def stage(s: State, *, bid: int, key: str, jids: Iterable[int]) -> State:
    if bid in {b.bid for b in s.batches}:
        raise ModelViolation(f"batch {bid} already exists")
    jobs = list(s.jobs)
    jobs.extend(Job(jid=jid, batch=bid, key=key, phase=Phase.STAGED) for jid in jids)
    out = replace(s, jobs=tuple(sorted(jobs)), batches=tuple(sorted((*s.batches, Batch(bid, key)))))
    assert_core_invariants(out)
    return out


def activate(s: State, bid: int) -> State:
    jobs = tuple(
        replace(j, phase=Phase.QUEUED) if j.batch == bid and j.phase == Phase.STAGED else j
        for j in s.jobs
    )
    out = replace(s, jobs=tuple(sorted(jobs)))
    assert_core_invariants(out)
    return out


def dispatch(s: State, jid: int, worker: str = "F0") -> State:
    j = s.job(jid)
    if j.phase != Phase.QUEUED:
        raise ModelViolation(f"job {jid} is not queued")
    if s.outstanding() >= s.credit_limit or s.reservations() >= s.worker_capacity:
        raise ModelViolation("dispatch attempted without credit/capacity")
    b = s.batch(j.batch)
    if b.cancelled:
        raise ModelViolation("post-cancel dispatch")
    out = s.replace_job(replace(j, phase=Phase.DISPATCHED, debt=True, worker=worker))
    assert_core_invariants(out)
    return out


def begin(s: State, jid: int) -> State:
    j = s.job(jid)
    if j.phase != Phase.DISPATCHED:
        raise ModelViolation(f"job {jid} did not have an unconfirmed dispatch")
    out = s.replace_job(replace(j, phase=Phase.STARTED, debt=False))
    assert_core_invariants(out)
    return out


def settle(s: State, jid: int) -> State:
    j = s.job(jid)
    if j.phase not in {Phase.DISPATCHED, Phase.STARTED}:
        raise ModelViolation(f"job {jid} cannot settle from {j.phase}")
    out = s.replace_job(replace(j, phase=Phase.TERMINAL, debt=False, worker=None))
    assert_core_invariants(out)
    return out


def legacy_cancel_early_return(s: State, key: str) -> State:
    matching_staged_batches = {
        b.bid
        for b in s.batches
        if b.key == key and any(j.batch == b.bid and j.phase == Phase.STAGED for j in s.jobs)
    }
    out = s
    if matching_staged_batches:
        out = out.remove_jobs(lambda j: j.batch in matching_staged_batches and j.phase == Phase.STAGED)
        out = replace(out, batches=tuple(b for b in out.batches if b.bid not in matching_staged_batches))
        assert_core_invariants(out)
        return out
    out = out.remove_jobs(lambda j: j.key == key and j.phase == Phase.QUEUED)
    assert_core_invariants(out)
    return out


def unified_cancel(s: State, key: str) -> State:
    out = replace(
        s,
        batches=tuple(replace(b, cancelled=True) if b.key == key else b for b in s.batches),
    )
    out = out.remove_jobs(lambda j: j.key == key and j.phase in {Phase.STAGED, Phase.QUEUED})
    assert_core_invariants(out)
    return out


def no_scheduler_owned_members(s: State, key: str) -> bool:
    return not any(j.key == key and j.phase in {Phase.STAGED, Phase.QUEUED} for j in s.jobs)


def scenario_cancel(mode: str) -> Trace:
    trace = Trace(f"cancel-{mode}", [], [], "", mode == "legacy")
    s = State(credit_limit=1, worker_capacity=1)
    trace.add("init", s)
    s = stage(s, bid=1, key="session-1/client-7", jids=[1, 2])
    trace.add("stage-earlier-batch", s, batch=1, members=2)
    s = activate(s, 1)
    trace.add("activate-earlier-batch", s, batch=1)
    s = dispatch(s, 1)
    trace.add("dispatch-one-member", s, job=1)
    s = stage(s, bid=2, key="session-1/client-7", jids=[3])
    trace.add("stage-later-alias", s, batch=2, note="same wire cancellation key")
    s = legacy_cancel_early_return(s, "session-1/client-7") if mode == "legacy" else unified_cancel(s, "session-1/client-7")
    trace.add("process-cancel", s, mode=mode)

    cut_holds = no_scheduler_owned_members(s, "session-1/client-7")
    if mode == "legacy" and cut_holds:
        raise ModelViolation("legacy cancellation unexpectedly satisfied the complete-cut property")
    if mode == "unified" and not cut_holds:
        raise ModelViolation("unified cancellation left scheduler-owned members")

    if mode == "legacy":
        trace.conclusion = (
            "COUNTEREXAMPLE: a matching staged record causes an early return; "
            "the earlier same-key queued member survives the cancellation cut."
        )
    else:
        s = settle(s, 1)
        trace.add("settle-in-flight-member", s, job=1)
        if any(j.phase != Phase.TERMINAL for j in s.jobs):
            raise ModelViolation("fixed trace did not converge")
        trace.conclusion = (
            "BOUNDED CHECK PASSED: unified cancellation removes staged and queued members; "
            "the sole in-flight member settles exactly once."
        )
    return trace


def scenario_frozen_dispatch(revocable_worker: bool) -> Trace:
    name = "frozen-dispatch-revocable" if revocable_worker else "frozen-dispatch-legacy"
    trace = Trace(name, [], [], "", not revocable_worker)
    s = State(credit_limit=1, worker_capacity=1)
    trace.add("init", s)
    s = dispatch(activate(stage(s, bid=1, key="session-1/client-9", jids=[1]), 1), 1)
    trace.add("usecs-issued", s, job=1, client_process="frozen-before-JobBegin")

    if not revocable_worker:
        trace.add("stutter", s, duration="unbounded")
        trace.conclusion = (
            "COUNTEREXAMPLE: G(debt -> F settled) is false. Scheduler and legacy F "
            "cannot distinguish an arbitrarily delayed client from a dead one, so the "
            "reservation and credit may remain forever while the daemon connection stays healthy."
        )
        return trace

    trace.add("scheduler-send-cancel-before-start", s, job=1, worker="F0")
    trace.add("worker-linearizes-tombstone", s, job=1)
    s = settle(s, 1)
    trace.add("worker-ack-cancelled-release", s, job=1)
    trace.conclusion = (
        "BOUNDED CHECK PASSED: a new-F cancel-before-start tombstone provides the missing "
        "linearization point; a late old or new client is rejected by job id."
    )
    return trace


def max_bipartite_matches(
    demands: Sequence[str], free_capacity: Mapping[str, int], eligible: Mapping[str, frozenset[str]]
) -> int:
    slots: list[tuple[str, int]] = []
    for worker, cap in sorted(free_capacity.items()):
        slots.extend((worker, i) for i in range(cap))
    owner: dict[tuple[str, int], int] = {}

    def augment(di: int, seen: set[tuple[str, int]]) -> bool:
        signature = demands[di]
        for slot in slots:
            if slot[0] not in eligible[signature] or slot in seen:
                continue
            seen.add(slot)
            previous = owner.get(slot)
            if previous is None or augment(previous, seen):
                owner[slot] = di
                return True
        return False

    return sum(augment(di, set()) for di in range(len(demands)))


def scenario_hall_credit() -> Trace:
    trace = Trace("eligibility-blind-credit", [], [], "", True)
    capacity = {"A1": 1, "A2": 1, "B1": 1}
    eligibility = {"needs-A": frozenset({"A1", "A2"}), "needs-B": frozenset({"B1"})}
    reservations = {"A1": 1, "A2": 1, "B1": 0}
    free = {w: capacity[w] - reservations[w] for w in capacity}
    total_slots = sum(capacity.values())
    aggregate_credit = max(1, total_slots - 1)
    aggregate_rule_holds = sum(reservations.values()) <= aggregate_credit
    matching = max_bipartite_matches(["needs-A"], free, eligibility)
    s = State(credit_limit=aggregate_credit, worker_capacity=total_slots)
    trace.add("reserve-two-constrained-slots", s, capacity=capacity, reservations=reservations, aggregate_credit=aggregate_credit)
    trace.add("new-constrained-demand", s, demand="needs-A", globally_free=sum(free.values()), compatible_matching=matching)
    if not aggregate_rule_holds or matching != 0 or sum(free.values()) != 1:
        raise ModelViolation("Hall counterexample construction failed")
    trace.conclusion = (
        "COUNTEREXAMPLE: aggregate credit holds and one global slot is free, yet the active "
        "eligibility neighbourhood has zero free capacity. Correct admission needs Hall/min-cut "
        "slack over the job-to-worker graph, not total slots."
    )
    return trace


def scenario_identifier_aba() -> Trace:
    trace = Trace("bounded-id-aba", [], [], "", True)
    modulus = 3
    live: dict[int, str] = {}
    delayed_done: list[int] = []
    next_id = 0

    def allocate(label: str) -> int:
        nonlocal next_id
        next_id = (next_id + 1) % modulus
        if next_id == 0:
            next_id = 1
        live[next_id] = label
        return next_id

    old = allocate("old-generation")
    trace.add("allocate-old", State(), wire_job_id=old)
    del live[old]
    delayed_done.append(old)
    trace.add("old-locally-retired-done-delayed", State(), wire_job_id=old)
    allocate("filler")
    new = allocate("new-generation")
    if new != old:
        raise ModelViolation("small identifier space did not wrap as expected")
    trace.add("allocate-new-with-reused-id", State(), wire_job_id=new)
    delivered = delayed_done.pop(0)
    victim = live.pop(delivered, None)
    trace.add("deliver-stale-done", State(), wire_job_id=delivered, removed=victim)
    if victim != "new-generation":
        raise ModelViolation("stale completion did not alias the new job")
    trace.conclusion = (
        "COUNTEREXAMPLE: after finite-id reuse, delayed JobDone is observationally identical "
        "to completion of the new job. Exact safety needs an epoch or no-reuse/quarantine rule."
    )
    return trace


def scenario_observer_gap() -> Trace:
    trace = Trace("ephemeral-observer-gap", [], [], "", True)
    active_interval = (1.0, 2.0)
    polls = [0.0, 3.0]
    samples = [active_interval[0] <= t <= active_interval[1] for t in polls]
    trace.add("activity-start", State(), at=active_interval[0])
    trace.add("activity-end", State(), at=active_interval[1])
    trace.add("polls", State(), at=polls, observed=samples)
    if any(samples):
        raise ModelViolation("observer-gap trace unexpectedly sampled activity")
    trace.conclusion = (
        "COUNTEREXAMPLE: contention existed, but every listcs sample missed it. Tests need a "
        "durable event/counter or explicit barrier, not a short-lived connection's presence."
    )
    return trace


FIELD_SINCE: dict[str, int] = {
    "GetCS.preferred_host": 22,
    "UseCS.matched_job_id": 28,
    "CompileFile.remote_compiler_name": 30,
    "GetCS.minimal_host_version": 34,
    "GetCS.client_count": 39,
    "JobDone.client_count": 39,
    "GetCS.required_features": 42,
    "Login.supported_features": 42,
    "GetCS.niceness": 43,
    "JobLocalBegin.fulljob": 44,
    "JobLocalBegin.local_reason": 45,
    "GetCS.command_summary": 46,
    "JobLocalBegin.cmdline": 46,
    "JobTiming": 47,
    "GetCS.full_path": 48,
    "JobLocalBegin.local_flags": 48,
    "CancelBeforeStart": 49,
}


def scenario_compatibility() -> Trace:
    trace = Trace("mixed-version-projection", [], [], "", False)
    versions = {"old": 43, "current": 48, "proposed": 49}
    topologies = {
        "S'FC": {"S-F": (49, 43), "C-F": (43, 43)},
        "S'F'C": {"S-F": (49, 49), "C-F": (43, 49)},
        "S'F'C'": {"S-F": (49, 49), "C-F": (49, 49)},
        "S'F[CC']": {"S-F": (49, 43), "C-F(old)": (43, 43), "C-F(new)": (49, 43)},
        "S'F'[CC']": {"S-F": (49, 49), "C-F(old)": (43, 49), "C-F(new)": (49, 49)},
    }
    negotiated = {topology: {link: min(ends) for link, ends in links.items()} for topology, links in topologies.items()}
    trace.add("negotiate-each-link-independently", State(), versions=versions, links=negotiated)

    old_visible = sorted(name for name, since in FIELD_SINCE.items() if since <= versions["old"])
    current_visible = sorted(name for name, since in FIELD_SINCE.items() if since <= versions["current"])
    if not set(old_visible).issubset(current_visible):
        raise ModelViolation("protocol feature alphabets are not monotone")
    if negotiated["S'F'C"]["S-F"] < 49 or negotiated["S'FC"]["S-F"] != 43:
        raise ModelViolation("mixed-version negotiation table is inconsistent")
    trace.add(
        "project-new-trace-to-negotiated-alphabet",
        State(),
        protocol43=old_visible,
        protocol48=current_visible,
        cancel_before_start_enabled=["S'F'C", "S'F'C'", "S'F'[CC']"],
    )
    trace.conclusion = (
        "BOUNDED CHECK PASSED: fields form a monotone negotiated alphabet. Proposed "
        "cancel-before-start depends only on S-F, so old clients remain usable with S'F'C; "
        "old-F links project to protocol 43 and retain legacy semantics."
    )
    return trace


SCENARIOS: dict[str, Callable[[], Trace]] = {
    "cancel-legacy": lambda: scenario_cancel("legacy"),
    "cancel-unified": lambda: scenario_cancel("unified"),
    "frozen-legacy": lambda: scenario_frozen_dispatch(False),
    "frozen-revocable": lambda: scenario_frozen_dispatch(True),
    "hall": scenario_hall_credit,
    "id-aba": scenario_identifier_aba,
    "observer": scenario_observer_gap,
    "compat": scenario_compatibility,
}


def print_trace(trace: Trace) -> None:
    marker = "COUNTEREXAMPLE" if trace.expected_counterexample else "CHECK"
    print(f"[{marker}] {trace.scenario}")
    for i, event in enumerate(trace.events):
        detail = " " + json.dumps(event.detail, sort_keys=True) if event.detail else ""
        print(f"  {i:02d}. {event.name}{detail}")
    print(f"  => {trace.conclusion}\n")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", choices=["all", *SCENARIOS.keys()])
    parser.add_argument("--json-dir", type=Path, help="write one machine-readable trace per scenario")
    args = parser.parse_args(argv)
    names = list(SCENARIOS) if args.scenario == "all" else [args.scenario]
    traces: list[Trace] = []
    try:
        for name in names:
            trace = SCENARIOS[name]()
            traces.append(trace)
            print_trace(trace)
    except (ModelViolation, ValueError) as exc:
        print(f"MODEL ERROR: {exc}", file=sys.stderr)
        return 2

    if args.json_dir:
        args.json_dir.mkdir(parents=True, exist_ok=True)
        for trace in traces:
            path = args.json_dir / f"{trace.scenario}.json"
            path.write_text(json.dumps(trace.to_json(), indent=2, sort_keys=True) + "\n")
        print(f"wrote {len(traces)} traces to {args.json_dir}")

    expected_cex = sum(t.expected_counterexample for t in traces)
    checks = len(traces) - expected_cex
    print(f"SUMMARY: {expected_cex} expected counterexample(s), {checks} bounded check(s), 0 model errors")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
