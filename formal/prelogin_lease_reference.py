#!/usr/bin/env python3
"""Deterministic trace oracle for PreloginLease.tla.

The script validates the intended red traces and exact accounting transitions.
TLC is authoritative for state-space and liveness checking.
"""
from __future__ import annotations

from dataclasses import asdict, dataclass, field
import argparse
import json
from typing import Optional


L = 1
Q = 2
LEASE = 2


@dataclass
class Model:
    state: dict[str, str] = field(
        default_factory=lambda: {"p0": "Outside", "p1": "Outside", "p2": "Outside"}
    )
    accepted_at: dict[str, int] = field(default_factory=dict)
    deadline: dict[str, int] = field(default_factory=dict)
    now: int = 0
    phase: str = "Accept"
    accepted_this_turn: int = 0
    control_done: bool = False
    socket_accepted: int = 0
    admitted: int = 0
    completed: int = 0
    expired: int = 0
    peer_closed: int = 0
    rejected: int = 0

    def current(self) -> int:
        return sum(value == "Prelogin" for value in self.state.values())

    def arrive(self, peer: str) -> None:
        self.state[peer] = "Pending"

    def accept(self, peer: str, *, no_cap: bool = False, no_quantum: bool = False) -> None:
        assert self.phase == "Accept"
        assert self.state[peer] == "Pending"
        assert no_quantum or self.accepted_this_turn < Q
        self.socket_accepted += 1
        self.accepted_this_turn += 1
        if no_cap or self.current() < L:
            self.state[peer] = "Prelogin"
            self.accepted_at[peer] = self.now
            self.deadline[peer] = self.now + LEASE
            self.admitted += 1
        else:
            self.state[peer] = "Rejected"
            self.rejected += 1

    def finish_accept(self) -> None:
        self.phase = "Service"

    def service(self) -> None:
        assert self.phase == "Service"
        self.control_done = True
        self.phase = "Accept"
        self.accepted_this_turn = 0

    def login(self, peer: str) -> None:
        assert self.state[peer] == "Prelogin"
        self.state[peer] = "Logged"
        self.completed += 1

    def close(self, peer: str) -> None:
        assert self.state[peer] == "Prelogin"
        self.state[peer] = "Closed"
        self.peer_closed += 1

    def fragment(self, peer: str, *, refresh: bool = False) -> None:
        assert self.state[peer] == "Prelogin"
        if refresh:
            self.deadline[peer] = self.now + LEASE

    def tick(self, *, no_lease: bool = False) -> None:
        self.now += 1
        if no_lease:
            return
        for peer, state in list(self.state.items()):
            if state == "Prelogin" and self.deadline[peer] <= self.now:
                self.state[peer] = "Closed"
                self.expired += 1

    def accounting_ok(self) -> bool:
        return (
            self.socket_accepted == self.admitted + self.rejected
            and self.admitted
            == self.completed + self.expired + self.peer_closed + self.current()
        )

    def safety_error(self) -> Optional[str]:
        if self.current() > L:
            return "PreloginBound"
        if self.accepted_this_turn > Q:
            return "AcceptQuantum"
        for peer, state in self.state.items():
            if state == "Prelogin" and self.now >= self.accepted_at[peer] + LEASE:
                return "WholeHandshakeLease"
        if not self.accounting_ok():
            return "AccountingConservation"
        return None


@dataclass
class Result:
    name: str
    outcome: str
    property: Optional[str]
    trace: list[str]
    final: dict


def fixed() -> Result:
    m = Model()
    trace: list[str] = []
    for peer in ("p0", "p1", "p2"):
        m.arrive(peer)
        trace.append(f"Arrive({peer})")
    m.accept("p0")
    trace.append("Accept(p0):admit")
    m.accept("p1")
    trace.append("Accept(p1):reject-at-cap")
    m.finish_accept()
    trace.append("FinishAccept")
    m.service()
    trace.append("ServiceControl")
    m.login("p0")
    trace.append("Login(p0)")
    assert m.safety_error() is None
    assert m.control_done
    return Result("PreloginLeaseFixed", "PASS", None, trace, asdict(m))


def no_cap() -> Result:
    m = Model()
    trace = ["Arrive(p0)", "Arrive(p1)", "Accept(p0)", "Accept(p1)"]
    m.arrive("p0")
    m.arrive("p1")
    m.accept("p0", no_cap=True)
    m.accept("p1", no_cap=True)
    error = m.safety_error()
    assert error == "PreloginBound"
    return Result("PreloginNoCapMutant", "EXPECTED_COUNTEREXAMPLE", error, trace, asdict(m))


def no_quantum() -> Result:
    m = Model()
    trace = []
    for peer in ("p0", "p1", "p2"):
        m.arrive(peer)
        trace.append(f"Arrive({peer})")
    for peer in ("p0", "p1", "p2"):
        m.accept(peer, no_quantum=True)
        trace.append(f"Accept({peer})")
    error = m.safety_error()
    assert error == "AcceptQuantum"
    return Result("PreloginNoQuantumMutant", "EXPECTED_COUNTEREXAMPLE", error, trace, asdict(m))


def no_quantum_lasso() -> Result:
    m = Model()
    trace = ["<loop>"]
    # The same peer closes and reconnects while the accept phase never yields.
    for _ in range(3):
        m.arrive("p0")
        trace.append("Arrive(p0)")
        m.accept("p0", no_quantum=True)
        trace.append("Accept(p0)")
        m.close("p0")
        trace.append("PeerClose(p0)")
    trace.append("</loop>")
    assert m.phase == "Accept"
    assert not m.control_done
    return Result(
        "PreloginNoQuantumControlLiveness",
        "EXPECTED_LASSO",
        "ControlProgress",
        trace,
        asdict(m),
    )


def no_lease() -> Result:
    m = Model()
    trace = ["Arrive(p0)", "Accept(p0)", "Tick", "Tick"]
    m.arrive("p0")
    m.accept("p0")
    m.tick(no_lease=True)
    m.tick(no_lease=True)
    error = m.safety_error()
    assert error == "WholeHandshakeLease"
    return Result("PreloginNoLeaseMutant", "EXPECTED_COUNTEREXAMPLE", error, trace, asdict(m))


def refresh_lease() -> Result:
    m = Model()
    trace = ["Arrive(p0)", "Accept(p0)", "Tick", "FragmentRefresh(p0)", "Tick"]
    m.arrive("p0")
    m.accept("p0")
    m.tick()
    m.fragment("p0", refresh=True)
    m.tick()
    error = m.safety_error()
    assert error == "WholeHandshakeLease"
    return Result("PreloginRefreshLeaseMutant", "EXPECTED_COUNTEREXAMPLE", error, trace, asdict(m))


def run_all() -> list[Result]:
    return [fixed(), no_cap(), no_quantum(), no_quantum_lasso(), no_lease(), refresh_lease()]


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
