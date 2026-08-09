#!/usr/bin/env python3
"""Bounded executable model for Icecream temporal ownership.

This is an explicit-state witness generator, not a replacement for TLC/Spin.
It models the minimum S/D/C/F state needed to expose four protocol defects:

1. Legacy cancellation must either release unsafely or retain forever.
2. A p49 S<->F prepare/revoke protocol is not exact for an old C: after an
   S restart, an old id-only claim can alias a new assignment with the same id.
3. A submitter-side JobDone cannot be authoritative after F has claimed/started.
4. waitpid(-1) plus a scalar current_kids counter can reap an unrelated child.

The p50 mode adds an exact (scheduler_epoch, job_id, nonce) claim as C's first
message to F and fences F's old session before a new scheduler epoch. The
model explores all enabled interleavings up to a bounded depth and checks the
ownership invariants after every transition.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, replace
from enum import Enum
from typing import Callable, Iterable, Optional, Sequence


class Mode(str, Enum):
    LEGACY = "legacy-id-only"
    P49 = "p49-prepare-revoke-old-client"
    P50 = "p50-exact-claim"


class ReleasePolicy(str, Enum):
    RELEASE = "release"
    RETAIN = "retain"
    FENCED = "fenced"


@dataclass(frozen=True, order=True)
class Assignment:
    label: str
    epoch: int
    job_id: int
    nonce: int

    @property
    def exact(self) -> tuple[int, int, int]:
        return (self.epoch, self.job_id, self.nonce)

    def short(self) -> str:
        return f"{self.label}[e={self.epoch},id={self.job_id},n={self.nonce}]"


@dataclass(frozen=True, order=True)
class Claim:
    origin: Assignment
    exact_wire: Optional[tuple[int, int, int]]
    id_wire: int


@dataclass(frozen=True, order=True)
class Execution:
    origin: Assignment
    authorization: Assignment


@dataclass(frozen=True, order=True)
class SFMessage:
    kind: str
    assignment: Assignment


@dataclass(frozen=True, order=True)
class FSMessage:
    kind: str
    assignment: Assignment


@dataclass(frozen=True)
class State:
    epoch: int = 1
    nonce_counter: int = 0
    record: Optional[Assignment] = None
    reservation: Optional[Assignment] = None

    s_to_f: tuple[SFMessage, ...] = ()
    s_to_c: tuple[Assignment, ...] = ()
    client_caps: tuple[Assignment, ...] = ()
    c_to_f: tuple[Claim, ...] = ()
    f_to_s: tuple[FSMessage, ...] = ()

    f_prepared: frozenset[Assignment] = frozenset()
    f_claimed: Optional[Execution] = None
    f_started: Optional[Execution] = None
    f_tombstones: frozenset[Assignment] = frozenset()
    revoked_acked: frozenset[Assignment] = frozenset()

    dispatched_a: bool = False
    dispatched_b: bool = False
    restarted: bool = False
    cancel_requested: bool = False
    submitter_done_sent: bool = False
    client_dropped: bool = False


@dataclass(frozen=True)
class Config:
    mode: Mode
    cancel_policy: ReleasePolicy
    terminal_policy: ReleasePolicy
    allow_cancel: bool = True
    allow_restart: bool = True
    allow_submitter_done: bool = True
    fence_f_on_restart: bool = True


@dataclass(frozen=True)
class Step:
    name: str
    state: State


def remove_at(items: Sequence, index: int) -> tuple:
    return tuple(items[:index]) + tuple(items[index + 1 :])


def dispatch(st: State, cfg: Config, label: str) -> Optional[State]:
    if st.record is not None or st.reservation is not None:
        return None
    if label == "A":
        if st.dispatched_a:
            return None
    elif label == "B":
        if st.dispatched_b or not st.restarted:
            return None
    else:
        raise ValueError(label)

    nonce_counter = st.nonce_counter + 1
    nonce = nonce_counter if cfg.mode == Mode.P50 else 0
    a = Assignment(label=label, epoch=st.epoch, job_id=1, nonce=nonce)
    s_to_f = st.s_to_f
    if cfg.mode != Mode.LEGACY:
        s_to_f += (SFMessage("prepare", a),)

    return replace(
        st,
        nonce_counter=nonce_counter,
        record=a,
        reservation=a,
        s_to_f=s_to_f,
        s_to_c=st.s_to_c + (a,),
        dispatched_a=st.dispatched_a or label == "A",
        dispatched_b=st.dispatched_b or label == "B",
    )


def successors(st: State, cfg: Config) -> Iterable[tuple[str, State]]:
    for label in ("A", "B"):
        nxt = dispatch(st, cfg, label)
        if nxt is not None:
            yield (f"S dispatches {label}", nxt)

    for i, offer in enumerate(st.s_to_c):
        yield (
            f"C receives UseCS for {offer.short()}",
            replace(
                st,
                s_to_c=remove_at(st.s_to_c, i),
                client_caps=st.client_caps + (offer,),
            ),
        )

    for i, cap in enumerate(st.client_caps):
        exact = cap.exact if cfg.mode == Mode.P50 else None
        claim = Claim(origin=cap, exact_wire=exact, id_wire=cap.job_id)
        yield (
            f"C sends claim for payload {cap.short()}",
            replace(
                st,
                client_caps=remove_at(st.client_caps, i),
                c_to_f=st.c_to_f + (claim,),
            ),
        )

    # S->F is FIFO; S->C and C->F are independent of it.
    if st.s_to_f:
        msg = st.s_to_f[0]
        tail = st.s_to_f[1:]
        if msg.kind == "prepare":
            prepared = st.f_prepared
            if msg.assignment not in st.f_tombstones:
                prepared = prepared | {msg.assignment}
            yield (
                f"F receives PREPARE {msg.assignment.short()}",
                replace(st, s_to_f=tail, f_prepared=prepared),
            )
        elif msg.kind == "revoke":
            a = msg.assignment
            prepared = st.f_prepared - {a}
            tombstones = st.f_tombstones | {a}
            claimed = st.f_claimed
            started = st.f_started
            if started is not None and started.authorization == a:
                ack = FSMessage("started", a)
            else:
                if claimed is not None and claimed.authorization == a:
                    claimed = None
                ack = FSMessage("revoked", a)
            yield (
                f"F receives REVOKE {a.short()} and replies {ack.kind.upper()}",
                replace(
                    st,
                    s_to_f=tail,
                    f_prepared=prepared,
                    f_claimed=claimed,
                    f_tombstones=tombstones,
                    f_to_s=st.f_to_s + (ack,),
                ),
            )
        else:
            raise AssertionError(msg.kind)

    for i, claim in enumerate(st.c_to_f):
        auth: Optional[Assignment] = None
        if cfg.mode == Mode.LEGACY:
            if st.record is not None and st.record.job_id == claim.id_wire:
                auth = st.record
            else:
                auth = claim.origin
        elif cfg.mode == Mode.P49:
            matches = sorted(
                (a for a in st.f_prepared if a.job_id == claim.id_wire),
                key=lambda a: (a.epoch, a.nonce, a.label),
                reverse=True,
            )
            if matches:
                auth = matches[0]
        elif cfg.mode == Mode.P50:
            matches = [a for a in st.f_prepared if a.exact == claim.exact_wire]
            if matches:
                auth = matches[0]
        else:
            raise AssertionError(cfg.mode)

        new_claimed = st.f_claimed
        if auth is not None and auth not in st.f_tombstones and st.f_started is None:
            new_claimed = Execution(origin=claim.origin, authorization=auth)
        yield (
            f"F receives claim carrying "
            f"{'exact '+str(claim.exact_wire) if claim.exact_wire else 'id '+str(claim.id_wire)}",
            replace(
                st,
                c_to_f=remove_at(st.c_to_f, i),
                f_claimed=new_claimed,
            ),
        )

    if st.f_claimed is not None and st.f_started is None:
        yield (
            f"F starts payload {st.f_claimed.origin.short()} under "
            f"authorization {st.f_claimed.authorization.short()}",
            replace(st, f_started=st.f_claimed, f_claimed=None),
        )

    if st.f_to_s:
        msg = st.f_to_s[0]
        tail = st.f_to_s[1:]
        record = st.record
        reservation = st.reservation
        revoked_acked = st.revoked_acked
        if msg.kind == "revoked":
            revoked_acked = revoked_acked | {msg.assignment}
            if record == msg.assignment:
                record = None
            if reservation == msg.assignment:
                reservation = None
        elif msg.kind != "started":
            raise AssertionError(msg.kind)
        yield (
            f"S receives {msg.kind.upper()} for {msg.assignment.short()}",
            replace(
                st,
                f_to_s=tail,
                record=record,
                reservation=reservation,
                revoked_acked=revoked_acked,
            ),
        )

    if cfg.allow_cancel and st.record is not None and not st.cancel_requested:
        a = st.record
        if cfg.cancel_policy == ReleasePolicy.RELEASE:
            yield (
                f"submitter cancels {a.short()}; S releases immediately",
                replace(
                    st,
                    record=None,
                    reservation=None,
                    cancel_requested=True,
                ),
            )
        elif cfg.cancel_policy == ReleasePolicy.RETAIN:
            yield (
                f"submitter cancels {a.short()}; S retains indefinitely",
                replace(st, cancel_requested=True),
            )
        elif cfg.cancel_policy == ReleasePolicy.FENCED:
            yield (
                f"submitter cancels {a.short()}; S emits REVOKE and retains",
                replace(
                    st,
                    s_to_f=st.s_to_f + (SFMessage("revoke", a),),
                    cancel_requested=True,
                ),
            )
        else:
            raise AssertionError(cfg.cancel_policy)

    if cfg.allow_submitter_done and st.record is not None and not st.submitter_done_sent:
        a = st.record
        if cfg.terminal_policy == ReleasePolicy.RELEASE:
            yield (
                f"submitter JobDone for {a.short()} is treated as terminal",
                replace(
                    st,
                    record=None,
                    reservation=None,
                    submitter_done_sent=True,
                ),
            )
        elif cfg.terminal_policy in (ReleasePolicy.RETAIN, ReleasePolicy.FENCED):
            sf = st.s_to_f
            if st.f_started is None and cfg.mode != Mode.LEGACY:
                sf += (SFMessage("revoke", a),)
            yield (
                f"submitter JobDone for {a.short()} is non-authoritative",
                replace(st, submitter_done_sent=True, s_to_f=sf),
            )
        else:
            raise AssertionError(cfg.terminal_policy)

    if cfg.allow_restart and st.dispatched_a and not st.restarted:
        # A correct F lease-session fence quiesces old-session work before F
        # advertises capacity to the new S epoch. A wrapper that already
        # received UseCS can still retain that capability and reconnect later.
        yield (
            "scheduler restarts; F changes lease session; delivered C capabilities survive",
            replace(
                st,
                epoch=st.epoch + 1,
                record=None,
                reservation=None,
                s_to_f=(),
                s_to_c=(),
                c_to_f=(),
                f_to_s=(),
                f_prepared=frozenset(),
                f_claimed=None,
                f_started=None if cfg.fence_f_on_restart else st.f_started,
                f_tombstones=frozenset(),
                restarted=True,
                cancel_requested=False,
                submitter_done_sent=False,
            ),
        )

    if not st.client_dropped and (st.client_caps or st.c_to_f):
        yield (
            "client disappears without ever reaching F",
            replace(st, client_caps=(), c_to_f=(), client_dropped=True),
        )


def invariant_violations(st: State) -> list[str]:
    out: list[str] = []
    exe = st.f_started
    if exe is not None:
        if exe.origin.exact != exe.authorization.exact:
            out.append(
                "payload/authorization alias: "
                f"payload {exe.origin.short()} ran as {exe.authorization.short()}"
            )
        if st.reservation is None or st.reservation.exact != exe.authorization.exact:
            out.append(
                "physical execution lacks its matching S reservation: "
                f"F runs {exe.authorization.short()}, S reserves "
                f"{st.reservation.short() if st.reservation else 'nothing'}"
            )
        if exe.authorization in st.revoked_acked:
            out.append(
                f"F executes {exe.authorization.short()} after REVOKED was acknowledged"
            )
    for a in st.revoked_acked:
        if a in st.f_prepared:
            out.append(f"revoked assignment {a.short()} remains prepared")
        if st.f_claimed is not None and st.f_claimed.authorization == a:
            out.append(f"revoked assignment {a.short()} remains claimed")
    return out


def bfs_counterexample(
    cfg: Config,
    *,
    max_depth: int,
    violation_filter: Optional[Callable[[str], bool]] = None,
) -> tuple[Optional[list[Step]], int]:
    initial = State()
    queue: deque[tuple[State, list[Step]]] = deque([(initial, [])])
    seen = {initial}
    explored = 0

    while queue:
        st, path = queue.popleft()
        explored += 1
        if len(path) >= max_depth:
            continue
        for name, nxt in successors(st, cfg):
            new_path = path + [Step(name, nxt)]
            violations = invariant_violations(nxt)
            chosen = [
                v for v in violations if violation_filter is None or violation_filter(v)
            ]
            if chosen:
                return new_path + [Step("VIOLATION: " + " | ".join(chosen), nxt)], explored
            if nxt not in seen:
                seen.add(nxt)
                queue.append((nxt, new_path))

    return None, explored


def print_path(title: str, path: Sequence[Step]) -> None:
    print(f"\n=== {title} ===")
    for i, step in enumerate(path, 1):
        print(f"{i:2d}. {step.name}")


def take_named(st: State, cfg: Config, prefix: str) -> State:
    matches = [(name, nxt) for name, nxt in successors(st, cfg) if name.startswith(prefix)]
    if len(matches) != 1:
        raise AssertionError(
            f"expected one transition beginning {prefix!r}, got {[m[0] for m in matches]}"
        )
    return matches[0][1]


def legacy_release_late_thaw_witness() -> None:
    cfg = Config(
        mode=Mode.LEGACY,
        cancel_policy=ReleasePolicy.RELEASE,
        terminal_policy=ReleasePolicy.RELEASE,
        allow_restart=False,
        allow_submitter_done=False,
    )
    st = State()
    trace: list[Step] = []

    st = dispatch(st, cfg, "A")  # type: ignore[assignment]
    assert st is not None
    trace.append(Step("S dispatches A and reserves the only F slot", st))
    st = take_named(st, cfg, "C receives UseCS")
    trace.append(Step("C receives UseCS, then is arbitrarily delayed before contacting F", st))
    st = take_named(st, cfg, "submitter cancels")
    trace.append(Step("S releases because no JobBegin has arrived", st))
    st = take_named(st, cfg, "C sends claim")
    trace.append(Step("The delayed C thaws and sends the old id-only claim", st))
    st = take_named(st, cfg, "F receives claim")
    trace.append(Step("Legacy F accepts the claim; it has no S-issued fence", st))
    st = take_named(st, cfg, "F starts payload")
    trace.append(Step("F starts the compile after S has released its reservation", st))

    violations = invariant_violations(st)
    assert any("lacks its matching" in v for v in violations)
    trace.append(Step("VIOLATION: " + " | ".join(violations), st))
    print_path("Legacy release branch: late-thaw safety counterexample", trace)


def legacy_retain_deadlock_witness() -> None:
    cfg = Config(
        mode=Mode.LEGACY,
        cancel_policy=ReleasePolicy.RETAIN,
        terminal_policy=ReleasePolicy.RETAIN,
        allow_restart=False,
        allow_submitter_done=False,
    )
    st = State()
    trace: list[Step] = []

    st = dispatch(st, cfg, "A")  # type: ignore[assignment]
    assert st is not None
    trace.append(Step("S dispatches A and reserves the only F slot", st))

    offer = st.s_to_c[0]
    st = replace(st, s_to_c=(), client_caps=(offer,))
    trace.append(Step("C receives UseCS", st))

    st = replace(st, cancel_requested=True)
    trace.append(Step("C disappears; S retains because a late C could still start", st))

    st = replace(st, client_caps=(), client_dropped=True)
    trace.append(Step("No actor remains that can produce JobBegin or JobDone", st))

    assert st.reservation is not None
    print_path("Legacy retain branch: bounded liveness counterexample", trace)
    print(
        "Result: the reservation is safe but permanently unreclaimable without an "
        "additional F-side fence/lease action."
    )


def wrong_waitpid_witness() -> None:
    print("\n=== F child accounting: waitpid(-1) witness ===")
    children = ["compiler:A", "state-writer"]
    current_kids = 1
    reaped = "state-writer"
    children.remove(reaped)
    current_kids -= 1
    print(" 1. live children = {compiler:A, state-writer}; current_kids = 1")
    print(" 2. state-writer exits first; waitpid(-1) reaps state-writer")
    print(" 3. clear_children decrements current_kids to 0")
    print(f" 4. remaining live child = {children[0]}")
    print(
        "Result: F can reconnect and advertise capacity while the old compiler "
        "still executes. A typed pid->(kind, session, assignment) registry is required."
    )


def main() -> int:
    legacy_release_late_thaw_witness()
    legacy_retain_deadlock_witness()

    p49_old = Config(
        mode=Mode.P49,
        cancel_policy=ReleasePolicy.FENCED,
        terminal_policy=ReleasePolicy.FENCED,
        allow_cancel=False,
        allow_restart=True,
        allow_submitter_done=False,
    )
    path, explored = bfs_counterexample(
        p49_old,
        max_depth=11,
        violation_filter=lambda v: "payload/authorization alias" in v,
    )
    assert path is not None
    print_path(
        f"p49 + old C: cross-epoch ABA counterexample ({explored} states searched)",
        path,
    )

    p50 = Config(
        mode=Mode.P50,
        cancel_policy=ReleasePolicy.FENCED,
        terminal_policy=ReleasePolicy.FENCED,
        allow_cancel=True,
        allow_restart=True,
        allow_submitter_done=True,
    )
    path, explored = bfs_counterexample(p50, max_depth=13)
    assert path is None, "p50 bounded model unexpectedly found a counterexample"
    print("\n=== p50 exact claim: bounded check ===")
    print(
        f"PASS: no ownership invariant violation in {explored} reachable states "
        "through depth 13. This is bounded evidence, not an unbounded proof."
    )

    p49_bad_terminal = Config(
        mode=Mode.P49,
        cancel_policy=ReleasePolicy.FENCED,
        terminal_policy=ReleasePolicy.RELEASE,
        allow_cancel=False,
        allow_restart=False,
        allow_submitter_done=True,
    )
    path, explored = bfs_counterexample(
        p49_bad_terminal,
        max_depth=9,
        violation_filter=lambda v: "lacks its matching" in v,
    )
    assert path is not None
    print_path(
        f"Submitter JobDone after F start: authority counterexample ({explored} states searched)",
        path,
    )

    p49_good_terminal = replace(
        p49_bad_terminal,
        terminal_policy=ReleasePolicy.FENCED,
    )
    path, explored = bfs_counterexample(p49_good_terminal, max_depth=10)
    assert path is None
    print("\n=== F-authoritative terminal rule: bounded check ===")
    print(
        f"PASS: no ownership invariant violation in {explored} reachable states "
        "through depth 10."
    )

    wrong_waitpid_witness()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
