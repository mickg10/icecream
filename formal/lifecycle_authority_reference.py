#!/usr/bin/env python3
"""Deterministic trace oracle for LifecycleAuthority.tla.

This fixes the expected authority and duplicate-Begin traces.  TLC and the C++
integration barriers remain authoritative.
"""
from __future__ import annotations

from dataclasses import asdict, dataclass
import argparse
import json
from typing import Optional

WAITING = "Waiting"
STARTED = "Started"
TERMINAL = "Terminal"


@dataclass
class Lifecycle:
    phase: str = WAITING
    detached: bool = False
    identity_visible: bool = True
    begun: bool = False
    dispatch_debit: bool = True
    credit_count: int = 0
    monitor_begin_count: int = 0
    start_revision: int = 0
    terminal_count: int = 0
    terminal_origin: str = "None"
    terminal_submitter_generation: int = 0
    duplicate_begin_count: int = 0
    rejected_detached_submitter: int = 0

    def begin(self, *, mutate_duplicate: bool = False) -> None:
        if self.phase == WAITING:
            self.phase = STARTED
            self.begun = True
            self.dispatch_debit = False
            self.credit_count += 1
            self.monitor_begin_count += 1
            self.start_revision += 1
        elif self.phase == STARTED:
            self.duplicate_begin_count += 1
            if mutate_duplicate:
                self.monitor_begin_count += 1
                self.start_revision += 1

    def detach(self, *, drop_identity: bool = False) -> None:
        assert self.phase == STARTED
        self.detached = True
        if drop_identity:
            self.identity_visible = False

    def detached_submitter_done(self, *, allow: bool = False) -> None:
        assert self.phase == STARTED and self.detached
        if allow:
            self.phase = TERMINAL
            self.terminal_count += 1
            self.terminal_origin = "SubmitterDone"
            self.terminal_submitter_generation = 202
        else:
            self.rejected_detached_submitter += 1

    def worker_done(self) -> None:
        assert self.phase == STARTED
        self.phase = TERMINAL
        self.terminal_count += 1
        self.terminal_origin = "WorkerDone"

    def worker_lost(self) -> None:
        assert self.phase == STARTED
        self.phase = TERMINAL
        self.terminal_count += 1
        self.terminal_origin = "WorkerLost"

    def attached_submitter_done(self) -> None:
        assert self.phase in (WAITING, STARTED) and not self.detached
        if self.dispatch_debit:
            self.credit_count += 1
        self.dispatch_debit = False
        self.phase = TERMINAL
        self.terminal_count += 1
        self.terminal_origin = "SubmitterDone"
        self.terminal_submitter_generation = 101


def invariant_error(state: Lifecycle) -> Optional[str]:
    expected_begin_count = 1 if state.begun else 0
    if (
        state.monitor_begin_count != expected_begin_count
        or state.start_revision != expected_begin_count
    ):
        return "BeginLinearizedOnce"
    if (
        state.detached
        and state.phase == TERMINAL
        and state.terminal_origin not in ("WorkerDone", "WorkerLost")
    ):
        return "DetachedTerminalAuthority"
    if state.terminal_origin == "SubmitterDone" and (
        state.detached or state.terminal_submitter_generation != 101
    ):
        return "SubmitterGenerationAuthority"
    if state.detached and not state.identity_visible:
        return "StableDetachedIdentity"
    if state.terminal_count > 1:
        return "TerminalAtMostOnce"
    if state.dispatch_debit != (state.phase == WAITING):
        return "DispatchDebitReleasedOnce"
    if state.credit_count != (0 if state.phase == WAITING else 1):
        return "DispatchDebitReleasedOnce"
    return None


@dataclass
class Result:
    name: str
    outcome: str
    property: Optional[str]
    trace: list[str]
    final: dict


def fixed_case() -> Result:
    state = Lifecycle()
    trace = ["Begin", "DuplicateBegin(no-op)", "Detach", "FalseSubmitterDone(reject)", "WorkerDone"]
    state.begin()
    state.begin()
    state.detach()
    state.detached_submitter_done()
    state.worker_done()
    assert invariant_error(state) is None
    return Result("LifecycleAuthorityFixed", "PASS", None, trace, asdict(state))


def false_terminal_case() -> Result:
    state = Lifecycle()
    state.begin()
    state.detach()
    state.detached_submitter_done(allow=True)
    error = invariant_error(state)
    assert error == "DetachedTerminalAuthority"
    return Result(
        "DetachedSubmitterTerminalMutant",
        "EXPECTED_COUNTEREXAMPLE",
        error,
        ["Begin", "Detach", "FalseSubmitterDone(accept)"],
        asdict(state),
    )


def duplicate_begin_case() -> Result:
    state = Lifecycle()
    state.begin()
    state.begin(mutate_duplicate=True)
    error = invariant_error(state)
    assert error == "BeginLinearizedOnce"
    return Result(
        "DuplicateBeginMutationMutant",
        "EXPECTED_COUNTEREXAMPLE",
        error,
        ["Begin", "DuplicateBegin(mutates)"],
        asdict(state),
    )


def identity_case() -> Result:
    state = Lifecycle()
    state.begin()
    state.detach(drop_identity=True)
    error = invariant_error(state)
    assert error == "StableDetachedIdentity"
    return Result(
        "DetachedIdentityDropMutant",
        "EXPECTED_COUNTEREXAMPLE",
        error,
        ["Begin", "Detach(drop identity)"],
        asdict(state),
    )


def attached_abort_case() -> Result:
    state = Lifecycle()
    state.attached_submitter_done()
    assert invariant_error(state) is None
    return Result(
        "AttachedSubmitterExactAbort",
        "PASS",
        None,
        ["SubmitterDoneWhileWaiting"],
        asdict(state),
    )


def worker_loss_case() -> Result:
    state = Lifecycle()
    state.begin()
    state.detach()
    state.worker_lost()
    assert invariant_error(state) is None
    return Result(
        "DetachedWorkerLoss",
        "PASS",
        None,
        ["Begin", "Detach", "WorkerLost"],
        asdict(state),
    )


def run_all() -> list[Result]:
    return [
        fixed_case(),
        false_terminal_case(),
        duplicate_begin_case(),
        identity_case(),
        attached_abort_case(),
        worker_loss_case(),
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    results = run_all()
    if args.json:
        print(json.dumps([asdict(result) for result in results], indent=2, sort_keys=True))
    else:
        for result in results:
            print(f"{result.outcome:27} {result.name}")
            if result.property:
                print(f"  property: {result.property}")
            print("  trace: " + " -> ".join(result.trace))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
