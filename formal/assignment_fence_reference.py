#!/usr/bin/env python3
"""Deterministic finite reference checks; TLC/TLAPS remain authoritative."""
from __future__ import annotations
from collections import deque
from dataclasses import asdict, dataclass
import argparse, json
from typing import Callable, Iterable, NamedTuple

ABSENT, PREPARED, CLAIMED, STARTED, REVOKED, TERMINAL = (
    "Absent", "Prepared", "Claimed", "Started", "Revoked", "Terminal")
TOKEN_REQUIRED, LEGACY_ID = "TokenRequired", "LegacyId"
POLICY = (TOKEN_REQUIRED, LEGACY_ID)
WIRE, FULL_ID, TOKEN, WORKER, CAPACITY = (7, 7), (1001, 2002), (111, 222), (0, 0), (1,)


def put(t: tuple, i: int, v):
    x = list(t); x[i] = v; return tuple(x)


@dataclass(frozen=True)
class Core:
    phase: tuple[str, ...]
    claimant: tuple[int | None, ...]
    exact: tuple[bool, ...]
    s_res: tuple[bool, ...]
    f_slot: tuple[bool, ...]
    released: tuple[bool, ...]
    terminals: tuple[int, ...]
    prepared: tuple[bool, ...]


def core_init() -> Core:
    n = len(POLICY)
    return Core((ABSENT,)*n, (None,)*n, (False,)*n, (False,)*n,
                (False,)*n, (False,)*n, (0,)*n, (False,)*n)


class Edge(NamedTuple):
    name: str
    state: Core


def core_next(s: Core, legacy_token=False, release_claimed=False) -> Iterable[Edge]:
    n = len(POLICY)
    for a in range(n):
        w = WORKER[a]
        occ = sum(s.f_slot[x] and WORKER[x] == w for x in range(n))
        if s.phase[a] == ABSENT and occ < CAPACITY[w]:
            yield Edge(f"Prepare({a})", Core(put(s.phase,a,PREPARED), s.claimant, s.exact,
                put(s.s_res,a,True), put(s.f_slot,a,True), s.released, s.terminals,
                put(s.prepared,a,True)))
    for c in range(n):
        if s.phase[c] != PREPARED or s.released[c]: continue
        for arriving in range(n):
            if WIRE[c] != WIRE[arriving]: continue
            if POLICY[c] == LEGACY_ID or legacy_token:
                exact = FULL_ID[c] == FULL_ID[arriving] and TOKEN[c] == TOKEN[arriving]
                yield Edge(f"ClaimLegacy({c},{arriving})", Core(put(s.phase,c,CLAIMED),
                    put(s.claimant,c,arriving), put(s.exact,c,exact), s.s_res, s.f_slot,
                    s.released, s.terminals, s.prepared))
            if POLICY[c] == TOKEN_REQUIRED and FULL_ID[c] == FULL_ID[arriving] and TOKEN[c] == TOKEN[arriving]:
                yield Edge(f"ClaimToken({c},{arriving})", Core(put(s.phase,c,CLAIMED),
                    put(s.claimant,c,arriving), put(s.exact,c,True), s.s_res, s.f_slot,
                    s.released, s.terminals, s.prepared))
    for a in range(n):
        if s.phase[a] == CLAIMED and not s.released[a]:
            yield Edge(f"Start({a})", Core(put(s.phase,a,STARTED), s.claimant, s.exact,
                s.s_res, s.f_slot, s.released, s.terminals, s.prepared))
        if s.phase[a] == PREPARED:
            # F frees its physical slot; S keeps its logical reservation until ack.
            yield Edge(f"Revoke({a})", Core(put(s.phase,a,REVOKED), s.claimant, s.exact,
                s.s_res, put(s.f_slot,a,False), s.released, s.terminals, s.prepared))
        if s.phase[a] == REVOKED or (release_claimed and s.phase[a] == CLAIMED):
            p = s.phase if s.phase[a] == CLAIMED else put(s.phase,a,TERMINAL)
            yield Edge(f"ConsumeRevoke({a})", Core(p, s.claimant, s.exact,
                put(s.s_res,a,False), put(s.f_slot,a,False), put(s.released,a,True),
                put(s.terminals,a,s.terminals[a]+1), s.prepared))
        if s.phase[a] in (CLAIMED, STARTED):
            for action in ("Complete", "WorkerLost"):
                yield Edge(f"{action}({a})", Core(put(s.phase,a,TERMINAL), s.claimant,
                    s.exact, put(s.s_res,a,False), put(s.f_slot,a,False),
                    put(s.released,a,True), put(s.terminals,a,s.terminals[a]+1), s.prepared))


def core_error(s: Core) -> str | None:
    wp, sp = {PREPARED,CLAIMED,STARTED}, {PREPARED,CLAIMED,STARTED,REVOKED}
    for a,p in enumerate(s.phase):
        if s.released[a] and p not in {REVOKED,TERMINAL}: return f"ReleaseSafety[{a}]"
        if s.f_slot[a] != (p in wp): return f"WorkerSlotCoherence[{a}]"
        if s.s_res[a] != (p in sp): return f"SchedulerReservationCoherence[{a}]"
        if p in {CLAIMED,STARTED} and not s.prepared[a]: return f"PreparedBeforeClaim[{a}]"
        if POLICY[a] == TOKEN_REQUIRED and p in {CLAIMED,STARTED,TERMINAL} and s.claimant[a] is not None and not s.exact[a]:
            return f"TokenRequiredExactness[{a}]"
        if s.terminals[a] > 1: return f"TerminalAtMostOnce[{a}]"
        if p == REVOKED and s.claimant[a] is not None: return f"ClaimRevokeExclusive[{a}]"
    for w,cap in enumerate(CAPACITY):
        if sum(s.f_slot[a] and WORKER[a] == w for a in range(len(POLICY))) > cap:
            return f"CapacityBound[{w}]"
    return None


@dataclass
class Result:
    name: str; outcome: str; states: int; property: str|None=None
    trace: list[str]|None=None; final: dict|None=None


def bfs(name: str, init, nxt: Callable, bad: Callable, expect=False) -> Result:
    q, parent = deque([init]), {init: None}
    while q:
        s = q.popleft(); err = bad(s)
        if err:
            tr=[]; c=s
            while parent[c] is not None:
                prev,act=parent[c]; tr.append(act); c=prev
            tr.reverse()
            if not expect: raise AssertionError(f"{name}: {err}: {tr}")
            return Result(name,"EXPECTED_COUNTEREXAMPLE",len(parent),err,tr,asdict(s))
        for act,t in nxt(s):
            if t not in parent: parent[t]=(s,act); q.append(t)
    if expect: raise AssertionError(f"{name}: expected counterexample")
    return Result(name,"PASS",len(parent))


@dataclass(frozen=True)
class Tiny:
    phase: str; a: bool; b: bool; c: bool=False


def tombstone(default_allow: bool) -> Result:
    def nxt(s):
        if s.phase==ABSENT: yield "Prepare", Tiny(PREPARED,False,False)
        if s.phase==PREPARED: yield "FenceAndRevoke", Tiny(REVOKED,True,False)
        if s.phase==REVOKED: yield "ConsumeREVOKED", Tiny(TERMINAL,True,True)
        if s.phase==TERMINAL and s.a: yield "ForgetTombstone", Tiny(TERMINAL,False,True)
        if s.phase==TERMINAL and s.b and not s.a and default_allow:
            yield "DelayedLegacyClaim", Tiny(CLAIMED,False,True)
    return bfs("P49FiniteTombstoneDefaultAllow" if default_allow else "P49DefaultDenyAfterCompaction",
        Tiny(ABSENT,False,False), nxt, lambda s: "ReleaseSafety" if s.b and s.phase==CLAIMED else None,
        default_allow)


@dataclass(frozen=True)
class Handoff:
    phase: str; exact: bool; frame: bool; live: bool; debt: bool; reservation: bool; terminals: int


def handoff(fixed: bool) -> Result:
    def nxt(s):
        if s.phase=="Waiting": yield "ReceiveUseCS", Handoff("Writing",fixed,False,True,True,True,0)
        if s.phase=="Writing":
            if s.exact: yield "FailFrameAndSendExactDone", Handoff(TERMINAL,True,False,False,False,False,1)
            else: yield "FailFrameAndSendAliasCancel", Handoff(TERMINAL,False,False,True,True,True,0)
            yield "CompleteFrame", Handoff("DeliveryUncertain",True,True,s.live,s.debt,s.reservation,s.terminals)
    def bad(s):
        if s.phase==TERMINAL and not s.frame and (s.live or s.debt or s.reservation or s.terminals!=1):
            return "FailedFrameEventuallySettles"
        return None
    return bfs("UseCSExactHandoff" if fixed else "UseCSExactHandoffMutant",
        Handoff("Waiting",False,False,True,True,True,0), nxt, bad, not fixed)


@dataclass(frozen=True)
class Child:
    phase: str; compiler: bool; writer: bool; occupancy: int; advertised: bool


def quiescence(fixed: bool) -> Result:
    def nxt(s):
        if s.phase=="Connected": yield "LoseSchedulerSession", Child("Quiescing",s.compiler,s.writer,s.occupancy,False)
        if s.phase=="Quiescing":
            if fixed: yield "TerminateAndWaitExactCompiler", Child("Quiesced",False,s.writer,0,False)
            elif s.writer: yield "waitpid(-1)-reaps-writer", Child("Quiesced",True,False,0,False)
        if s.phase=="Quiesced": yield "AdvertiseNewSession", Child("Advertised",s.compiler,s.writer,s.occupancy,True)
    return bfs("FSessionExactQuiescence" if fixed else "FSessionWrongChildReap",
        Child("Connected",True,True,1,False), nxt,
        lambda s: "PriorSessionQuiescedBeforeAdvertise" if s.advertised and (s.compiler or s.occupancy) else None,
        not fixed)


def run_all():
    return [
        bfs("AssignmentFenceCoreFixed",core_init(),lambda s: core_next(s),core_error),
        bfs("P50MixedFleetMutant",core_init(),lambda s: core_next(s,legacy_token=True),core_error,True),
        bfs("ReleaseClaimedMutant",core_init(),lambda s: core_next(s,release_claimed=True),core_error,True),
        tombstone(False), tombstone(True), handoff(True), handoff(False),
        quiescence(True), quiescence(False)]


def main() -> int:
    ap=argparse.ArgumentParser(); ap.add_argument("--json",action="store_true"); a=ap.parse_args()
    r=run_all()
    if a.json: print(json.dumps([asdict(x) for x in r],indent=2,sort_keys=True))
    else:
        for x in r:
            print(f"{x.outcome:27} {x.name} ({x.states} states)")
            if x.trace: print("  trace: "+" -> ".join(x.trace)+f"\n  property: {x.property}")
    return 0

if __name__ == "__main__": raise SystemExit(main())
