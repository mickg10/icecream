#!/usr/bin/env python3
"""Generate the finite strict/pipelined assignment-fence acceptance layer."""

from __future__ import annotations

import json
import textwrap
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
TRACE_DIR = HERE / "trace-manifests"

MODEL = r'''
---------------- MODULE AssignmentFenceAdmissionModes ----------------
(***************************************************************************
Finite comparison of strict READY-before-UseCS and zero-extra-RTT
pipelined enforcing admission.

Pipelining never means default allow.  An assignment claim that reaches
the fulfillment daemon before PREPARE occupies only a bounded pending
record.  It cannot start a compiler or create an environment side effect
until the matching PREPARE is consumed.  PREPARE and REVOKE share one
ordered scheduler-to-daemon queue.
***************************************************************************)
EXTENDS Naturals, Sequences, TLC

CONSTANTS StrictEnforcing, PipelinedEnforcing,
          GeneralScenario, ClaimBeforePrepareScenario, RevokeRaceScenario,
          Mode, Scenario, MaxPending,
          MutantDefaultAllowUnknown,
          MutantSideEffectBeforePrepare,
          MutantUnboundedPending

ASSUME /\ StrictEnforcing # PipelinedEnforcing
       /\ Mode \in {StrictEnforcing, PipelinedEnforcing}
       /\ GeneralScenario # ClaimBeforePrepareScenario
       /\ GeneralScenario # RevokeRaceScenario
       /\ ClaimBeforePrepareScenario # RevokeRaceScenario
       /\ Scenario \in {GeneralScenario,
                         ClaimBeforePrepareScenario,
                         RevokeRaceScenario}
       /\ MaxPending \in Nat \ {0}
       /\ MutantDefaultAllowUnknown \in BOOLEAN
       /\ MutantSideEffectBeforePrepare \in BOOLEAN
       /\ MutantUnboundedPending \in BOOLEAN

ControlKinds == {"PREPARE", "REVOKE"}

ScenarioSequence ==
    CASE Scenario = ClaimBeforePrepareScenario ->
             <<"QueuePrepare", "ExposeUseCS", "ClientClaim",
               "ConsumePrepare", "Start">>
      [] Scenario = RevokeRaceScenario ->
             <<"QueuePrepare", "ExposeUseCS", "ConsumePrepare",
               "QueueRevoke", "ClientClaim", "Start">>
      [] OTHER -> <<>>

ScenarioLength == Len(ScenarioSequence)

Allowed(label) ==
    Scenario = GeneralScenario
    \/ (scenarioStep < ScenarioLength
        /\ ScenarioSequence[scenarioStep + 1] = label)

AdvanceStep ==
    IF Scenario = GeneralScenario THEN scenarioStep ELSE scenarioStep + 1

CapInc(n) == IF n < 2 THEN n + 1 ELSE n

VARIABLES s2f,
          prepareQueuedEver,
          prepared,
          readyQueued,
          readySeen,
          usecsExposed,
          claimArrived,
          pendingCount,
          matchedClaim,
          revoked,
          started,
          sideEffect,
          terminalQueued,
          terminalConsumed,
          released,
          compacted,
          startAfterRelease,
          rejectedUnknown,
          scenarioStep

vars ==
    <<s2f, prepareQueuedEver, prepared, readyQueued, readySeen,
      usecsExposed, claimArrived, pendingCount, matchedClaim, revoked,
      started, sideEffect, terminalQueued, terminalConsumed, released,
      compacted, startAfterRelease, rejectedUnknown, scenarioStep>>

Init ==
    /\ s2f = <<>>
    /\ prepareQueuedEver = FALSE
    /\ prepared = FALSE
    /\ readyQueued = FALSE
    /\ readySeen = FALSE
    /\ usecsExposed = FALSE
    /\ claimArrived = FALSE
    /\ pendingCount = 0
    /\ matchedClaim = FALSE
    /\ revoked = FALSE
    /\ started = FALSE
    /\ sideEffect = FALSE
    /\ terminalQueued = FALSE
    /\ terminalConsumed = FALSE
    /\ released = FALSE
    /\ compacted = FALSE
    /\ startAfterRelease = FALSE
    /\ rejectedUnknown = 0
    /\ scenarioStep = 0

QueuePrepare ==
    /\ Allowed("QueuePrepare")
    /\ ~prepareQueuedEver
    /\ Len(s2f) < 2
    /\ s2f' = Append(s2f, "PREPARE")
    /\ prepareQueuedEver' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<prepared, readyQueued, readySeen, usecsExposed,
                    claimArrived, pendingCount, matchedClaim, revoked,
                    started, sideEffect, terminalQueued, terminalConsumed,
                    released, compacted, startAfterRelease,
                    rejectedUnknown>>

ExposeUseCS ==
    /\ Allowed("ExposeUseCS")
    /\ ~usecsExposed
    /\ IF Mode = StrictEnforcing THEN readySeen ELSE prepareQueuedEver
    /\ usecsExposed' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, claimArrived, pendingCount, matchedClaim,
                    revoked, started, sideEffect, terminalQueued,
                    terminalConsumed, released, compacted,
                    startAfterRelease, rejectedUnknown>>

ClientClaim ==
    /\ Allowed("ClientClaim")
    /\ usecsExposed
    /\ ~claimArrived
    /\ LET immediatelyMatched == prepared /\ ~revoked
           mayPend == pendingCount < MaxPending \/ MutantUnboundedPending
       IN /\ claimArrived' = TRUE
          /\ matchedClaim' =
                IF immediatelyMatched THEN TRUE ELSE matchedClaim
          /\ pendingCount' =
                IF immediatelyMatched
                THEN pendingCount
                ELSE IF mayPend THEN pendingCount + 1 ELSE pendingCount
          /\ rejectedUnknown' =
                IF immediatelyMatched \/ mayPend
                THEN rejectedUnknown
                ELSE CapInc(rejectedUnknown)
          /\ sideEffect' =
                sideEffect
                \/ (MutantSideEffectBeforePrepare /\ ~prepared)
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, revoked, started,
                    terminalQueued, terminalConsumed, released,
                    compacted, startAfterRelease>>

UnknownClaim ==
    /\ Allowed("UnknownClaim")
    /\ Mode = PipelinedEnforcing
    /\ usecsExposed
    /\ ~prepared
    /\ pendingCount <= MaxPending
    /\ LET accepted == pendingCount < MaxPending \/ MutantUnboundedPending
       IN /\ pendingCount' =
                IF accepted THEN pendingCount + 1 ELSE pendingCount
          /\ rejectedUnknown' =
                IF accepted THEN rejectedUnknown
                ELSE CapInc(rejectedUnknown)
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived, matchedClaim,
                    revoked, started, sideEffect, terminalQueued,
                    terminalConsumed, released, compacted,
                    startAfterRelease>>

ConsumePrepare ==
    /\ Allowed("ConsumePrepare")
    /\ Len(s2f) > 0
    /\ s2f[1] = "PREPARE"
    /\ LET matchPending == claimArrived /\ pendingCount > 0
       IN /\ s2f' = Tail(s2f)
          /\ prepared' = TRUE
          /\ readyQueued' = TRUE
          /\ matchedClaim' = matchedClaim \/ matchPending
          /\ pendingCount' =
                IF matchPending THEN pendingCount - 1 ELSE pendingCount
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<prepareQueuedEver, readySeen, usecsExposed,
                    claimArrived, revoked, started, sideEffect,
                    terminalQueued, terminalConsumed, released,
                    compacted, startAfterRelease, rejectedUnknown>>

ConsumeReady ==
    /\ Allowed("ConsumeReady")
    /\ readyQueued
    /\ ~readySeen
    /\ readySeen' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    usecsExposed, claimArrived, pendingCount,
                    matchedClaim, revoked, started, sideEffect,
                    terminalQueued, terminalConsumed, released,
                    compacted, startAfterRelease, rejectedUnknown>>

QueueRevoke ==
    /\ Allowed("QueueRevoke")
    /\ prepareQueuedEver
    /\ ~released
    /\ "REVOKE" \notin {s2f[i] : i \in 1..Len(s2f)}
    /\ Len(s2f) < 2
    /\ s2f' = Append(s2f, "REVOKE")
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived,
                    pendingCount, matchedClaim, revoked, started,
                    sideEffect, terminalQueued, terminalConsumed,
                    released, compacted, startAfterRelease,
                    rejectedUnknown>>

ConsumeRevoke ==
    /\ Allowed("ConsumeRevoke")
    /\ Len(s2f) > 0
    /\ s2f[1] = "REVOKE"
    /\ prepared
    /\ LET fenceWins == ~matchedClaim /\ ~started
       IN /\ s2f' = Tail(s2f)
          /\ revoked' = revoked \/ fenceWins
          /\ terminalQueued' = terminalQueued \/ fenceWins
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived,
                    pendingCount, matchedClaim, started, sideEffect,
                    terminalConsumed, released, compacted,
                    startAfterRelease, rejectedUnknown>>

Start ==
    /\ Allowed("Start")
    /\ prepared
    /\ matchedClaim
    /\ ~revoked
    /\ ~released
    /\ ~started
    /\ started' = TRUE
    /\ sideEffect' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived,
                    pendingCount, matchedClaim, revoked,
                    terminalQueued, terminalConsumed, released,
                    compacted, startAfterRelease, rejectedUnknown>>

ConsumeTerminal ==
    /\ Allowed("ConsumeTerminal")
    /\ terminalQueued
    /\ ~terminalConsumed
    /\ terminalConsumed' = TRUE
    /\ released' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived,
                    pendingCount, matchedClaim, revoked, started,
                    sideEffect, terminalQueued, compacted,
                    startAfterRelease, rejectedUnknown>>

Compact ==
    /\ Allowed("Compact")
    /\ released
    /\ ~compacted
    /\ compacted' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived,
                    pendingCount, matchedClaim, revoked, started,
                    sideEffect, terminalQueued, terminalConsumed,
                    released, startAfterRelease, rejectedUnknown>>

DefaultAllowUnknown ==
    /\ Allowed("DefaultAllowUnknown")
    /\ MutantDefaultAllowUnknown
    /\ compacted
    /\ usecsExposed
    /\ ~matchedClaim
    /\ started' = TRUE
    /\ sideEffect' = TRUE
    /\ startAfterRelease' = TRUE
    /\ scenarioStep' = AdvanceStep
    /\ UNCHANGED <<s2f, prepareQueuedEver, prepared, readyQueued,
                    readySeen, usecsExposed, claimArrived,
                    pendingCount, matchedClaim, revoked,
                    terminalQueued, terminalConsumed, released,
                    compacted, rejectedUnknown>>

Next ==
    \/ QueuePrepare
    \/ ExposeUseCS
    \/ ClientClaim
    \/ UnknownClaim
    \/ ConsumePrepare
    \/ ConsumeReady
    \/ QueueRevoke
    \/ ConsumeRevoke
    \/ Start
    \/ ConsumeTerminal
    \/ Compact
    \/ DefaultAllowUnknown

Spec == Init /\ [][Next]_vars

FairSpec ==
    Spec
    /\ WF_vars(ConsumePrepare)
    /\ WF_vars(ConsumeRevoke)
    /\ WF_vars(ConsumeTerminal)

TypeOK ==
    /\ s2f \in Seq(ControlKinds)
    /\ Len(s2f) <= 2
    /\ prepareQueuedEver \in BOOLEAN
    /\ prepared \in BOOLEAN
    /\ readyQueued \in BOOLEAN
    /\ readySeen \in BOOLEAN
    /\ usecsExposed \in BOOLEAN
    /\ claimArrived \in BOOLEAN
    /\ pendingCount \in 0..(MaxPending + 1)
    /\ matchedClaim \in BOOLEAN
    /\ revoked \in BOOLEAN
    /\ started \in BOOLEAN
    /\ sideEffect \in BOOLEAN
    /\ terminalQueued \in BOOLEAN
    /\ terminalConsumed \in BOOLEAN
    /\ released \in BOOLEAN
    /\ compacted \in BOOLEAN
    /\ startAfterRelease \in BOOLEAN
    /\ rejectedUnknown \in 0..2
    /\ scenarioStep \in 0..ScenarioLength

StrictReadyBeforeUseCS ==
    Mode = StrictEnforcing => (usecsExposed => readySeen)

PrepareQueuedBeforeUseCS == usecsExposed => prepareQueuedEver

MatchedClaimAuthorized ==
    matchedClaim => claimArrived /\ prepared /\ ~revoked

StartAuthorized ==
    started /\ ~startAfterRelease
    => prepared /\ matchedClaim /\ ~revoked /\ ~released

SideEffectAuthorized ==
    sideEffect /\ ~startAfterRelease
    => prepared /\ matchedClaim /\ ~revoked

NoSideEffectBeforePrepare == sideEffect => prepared
PendingBound == pendingCount <= MaxPending
ReleaseAfterTerminalConsume == released => terminalConsumed
RevokedExcludesClaim == revoked => ~matchedClaim /\ ~started
TerminalRequiresFence == terminalQueued => revoked
NoStartAfterRelease == ~startAfterRelease

SafetyInvariant ==
    /\ TypeOK
    /\ StrictReadyBeforeUseCS
    /\ PrepareQueuedBeforeUseCS
    /\ MatchedClaimAuthorized
    /\ StartAuthorized
    /\ SideEffectAuthorized
    /\ NoSideEffectBeforePrepare
    /\ PendingBound
    /\ ReleaseAfterTerminalConsume
    /\ RevokedExcludesClaim
    /\ TerminalRequiresFence
    /\ NoStartAfterRelease

PendingEventuallyResolved ==
    []((claimArrived /\ pendingCount > 0)
       => <>(matchedClaim \/ revoked \/ released))

ScenarioComplete == scenarioStep = ScenarioLength

NoClaimBeforePrepareWitness ==
    ~(Scenario = ClaimBeforePrepareScenario
      /\ ScenarioComplete
      /\ started)

NoRevokeRaceWitness ==
    ~(Scenario = RevokeRaceScenario
      /\ ScenarioComplete
      /\ started)

=============================================================================
'''

CONFIGS: dict[str, tuple[str, str, str, str, str, bool, bool, bool]] = {
    "P49StrictWithinEpoch.cfg": ("StrictEnforcing", "General", "Spec", "INVARIANT", "SafetyInvariant", False, False, False),
    "P49PipelinedWithinEpoch.cfg": ("PipelinedEnforcing", "General", "Spec", "INVARIANT", "SafetyInvariant", False, False, False),
    "P49PipelinedClaimBeforePrepare.cfg": ("PipelinedEnforcing", "ClaimBeforePrepare", "Spec", "INVARIANT", "SafetyInvariant", False, False, False),
    "P49PipelinedRevokeRace.cfg": ("PipelinedEnforcing", "RevokeRace", "Spec", "INVARIANT", "SafetyInvariant", False, False, False),
    "P49PipelinedClaimBeforePrepareWitness.cfg": ("PipelinedEnforcing", "ClaimBeforePrepare", "Spec", "INVARIANT", "NoClaimBeforePrepareWitness", False, False, False),
    "P49PipelinedRevokeRaceWitness.cfg": ("PipelinedEnforcing", "RevokeRace", "Spec", "INVARIANT", "NoRevokeRaceWitness", False, False, False),
    "P49PipelinedDefaultAllowMutant.cfg": ("PipelinedEnforcing", "General", "Spec", "INVARIANT", "NoStartAfterRelease", True, False, False),
    "P49PipelinedSideEffectBeforePrepare.cfg": ("PipelinedEnforcing", "General", "Spec", "INVARIANT", "NoSideEffectBeforePrepare", False, True, False),
    "P49PipelinedUnboundedPending.cfg": ("PipelinedEnforcing", "General", "Spec", "INVARIANT", "PendingBound", False, False, True),
    "P49PipelinedPendingLiveness.cfg": ("PipelinedEnforcing", "ClaimBeforePrepare", "FairSpec", "PROPERTY", "PendingEventuallyResolved", False, False, False),
}


def write_config(path: Path, values: tuple[str, str, str, str, str, bool, bool, bool]) -> None:
    mode, scenario, spec, check_kind, prop, default_allow, side_effect, unbounded = values
    path.write_text(
        textwrap.dedent(
            f'''\
            SPECIFICATION {spec}

            CONSTANTS
                StrictEnforcing = "StrictEnforcing"
                PipelinedEnforcing = "PipelinedEnforcing"
                GeneralScenario = "General"
                ClaimBeforePrepareScenario = "ClaimBeforePrepare"
                RevokeRaceScenario = "RevokeRace"
                Mode = "{mode}"
                Scenario = "{scenario}"
                MaxPending = 2
                MutantDefaultAllowUnknown = {str(default_allow).upper()}
                MutantSideEffectBeforePrepare = {str(side_effect).upper()}
                MutantUnboundedPending = {str(unbounded).upper()}

            {check_kind} {prop}
            CHECK_DEADLOCK FALSE
            '''
        ),
        encoding="utf-8",
    )


def manifest(name: str, prop: str, config: str, min_states: int, events: list[dict[str, Any]], sequence: list[str], final_all: list[dict[str, Any]], harness: list[dict[str, Any]], **metadata: Any) -> None:
    document = {
        "schema": 1,
        "scenario": name,
        "property": prop,
        "expected_result": "counterexample",
        "min_states": min_states,
        "event_mode": "exclusive",
        "events": events,
        "required_subsequence": sequence,
        "final_all": final_all,
        "harness_steps": harness,
        "metadata": {"module": "AssignmentFenceAdmissionModes", "config": config, **metadata},
    }
    (TRACE_DIR / f"{name}.manifest.template.json").write_text(
        json.dumps(document, indent=2) + "\n", encoding="utf-8"
    )


def write_traces() -> dict[str, str]:
    TRACE_DIR.mkdir(exist_ok=True)
    manifest(
        "P49PipelinedClaimBeforePrepareWitness",
        "NoClaimBeforePrepareWitness",
        "P49PipelinedClaimBeforePrepareWitness.cfg",
        6,
        [
            {"name": "ExposeUseCSBeforePrepareConsumed", "all": [{"path": "usecsExposed", "from": False, "to": True}, {"path": "prepared", "to": False}, {"path": "prepareQueuedEver", "to": True}]},
            {"name": "ParkClaimPending", "all": [{"path": "claimArrived", "from": False, "to": True}, {"path": "pendingCount", "from": 0, "to": 1}, {"path": "matchedClaim", "to": False}, {"path": "sideEffect", "to": False}]},
            {"name": "ConsumePrepareAndMatch", "all": [{"path": "prepared", "from": False, "to": True}, {"path": "pendingCount", "from": 1, "to": 0}, {"path": "matchedClaim", "from": False, "to": True}]},
            {"name": "StartAfterAuthorization", "all": [{"path": "started", "from": False, "to": True}, {"path": "sideEffect", "from": False, "to": True}, {"path": "revoked", "to": False}]},
        ],
        ["ExposeUseCSBeforePrepareConsumed", "ParkClaimPending", "ConsumePrepareAndMatch", "StartAfterAuthorization"],
        [{"path": "started", "eq": True}, {"path": "prepared", "eq": True}, {"path": "matchedClaim", "eq": True}, {"path": "pendingCount", "eq": 0}],
        [
            {"after": "ExposeUseCSBeforePrepareConsumed", "emit": "expose_usecs_after_prepare_enqueue_without_ready", "actor": "scheduler"},
            {"after": "ParkClaimPending", "emit": "assert_claim_parked_without_side_effect", "actor": "fulfillment-daemon"},
            {"after": "ConsumePrepareAndMatch", "emit": "authorize_exact_pending_claim", "actor": "fulfillment-daemon"},
        ],
    )
    manifest(
        "P49PipelinedRevokeRaceWitness",
        "NoRevokeRaceWitness",
        "P49PipelinedRevokeRaceWitness.cfg",
        7,
        [
            {"name": "QueueRevokeBehindPrepare", "all": [{"path": "s2f", "to": ["REVOKE"]}, {"path": "prepared", "to": True}, {"path": "matchedClaim", "to": False}]},
            {"name": "ClaimWinsBeforeRevokeConsume", "all": [{"path": "claimArrived", "from": False, "to": True}, {"path": "matchedClaim", "from": False, "to": True}, {"path": "revoked", "to": False}]},
            {"name": "StartClaimWinner", "all": [{"path": "started", "from": False, "to": True}, {"path": "revoked", "to": False}]},
        ],
        ["QueueRevokeBehindPrepare", "ClaimWinsBeforeRevokeConsume", "StartClaimWinner"],
        [{"path": "started", "eq": True}, {"path": "matchedClaim", "eq": True}, {"path": "revoked", "eq": False}],
        [
            {"after": "QueueRevokeBehindPrepare", "emit": "hold_revoke_in_ordered_control_queue", "actor": "scheduler-channel"},
            {"after": "ClaimWinsBeforeRevokeConsume", "emit": "linearize_claim_before_queued_revoke", "actor": "fulfillment-daemon"},
        ],
    )
    manifest(
        "P49PipelinedDefaultAllowMutant",
        "NoStartAfterRelease",
        "P49PipelinedDefaultAllowMutant.cfg",
        8,
        [
            {"name": "FenceBeforeClaim", "all": [{"path": "revoked", "from": False, "to": True}, {"path": "terminalQueued", "from": False, "to": True}, {"path": "matchedClaim", "to": False}]},
            {"name": "ConsumeTerminalRelease", "all": [{"path": "terminalConsumed", "from": False, "to": True}, {"path": "released", "from": False, "to": True}]},
            {"name": "CompactFenceRecord", "all": [{"path": "compacted", "from": False, "to": True}, {"path": "released", "to": True}]},
            {"name": "DefaultAllowLateUnknown", "all": [{"path": "startAfterRelease", "from": False, "to": True}, {"path": "started", "from": False, "to": True}, {"path": "sideEffect", "from": False, "to": True}]},
        ],
        ["FenceBeforeClaim", "ConsumeTerminalRelease", "CompactFenceRecord", "DefaultAllowLateUnknown"],
        [{"path": "released", "eq": True}, {"path": "compacted", "eq": True}, {"path": "startAfterRelease", "eq": True}],
        [
            {"after": "CompactFenceRecord", "emit": "compact_terminal_assignment_record", "actor": "fulfillment-daemon"},
            {"after": "DefaultAllowLateUnknown", "emit": "assert_late_unknown_claim_started_after_release", "actor": "fulfillment-daemon"},
        ],
    )
    manifest(
        "P49PipelinedSideEffectBeforePrepare",
        "NoSideEffectBeforePrepare",
        "P49PipelinedSideEffectBeforePrepare.cfg",
        4,
        [
            {"name": "ExposePipelinedUseCS", "all": [{"path": "usecsExposed", "from": False, "to": True}, {"path": "prepared", "to": False}]},
            {"name": "PrematureClaimSideEffect", "all": [{"path": "claimArrived", "from": False, "to": True}, {"path": "sideEffect", "from": False, "to": True}, {"path": "prepared", "to": False}, {"path": "matchedClaim", "to": False}]},
        ],
        ["ExposePipelinedUseCS", "PrematureClaimSideEffect"],
        [{"path": "sideEffect", "eq": True}, {"path": "prepared", "eq": False}, {"path": "matchedClaim", "eq": False}],
        [
            {"after": "ExposePipelinedUseCS", "emit": "deliver_claim_before_prepare_consume", "actor": "client-channel"},
            {"after": "PrematureClaimSideEffect", "emit": "assert_environment_side_effect_preceded_authorization", "actor": "fulfillment-daemon"},
        ],
    )
    manifest(
        "P49PipelinedUnboundedPending",
        "PendingBound",
        "P49PipelinedUnboundedPending.cfg",
        6,
        [
            {"name": "GrowPendingTable", "all": [{"path": "pendingCount", "delta": 1}, {"path": "prepared", "to": False}]},
            {"name": "ExceedPendingLimit", "all": [{"path": "pendingCount", "from": 2, "to": 3}, {"path": "prepared", "to": False}]},
        ],
        ["GrowPendingTable", "GrowPendingTable", "ExceedPendingLimit"],
        [{"path": "pendingCount", "eq": 3}],
        [{"after": "ExceedPendingLimit", "emit": "assert_pending_claim_table_exceeded_negotiated_bound", "actor": "fulfillment-daemon"}],
        max_pending=2,
    )
    return {
        "claim": "trace-manifests/P49PipelinedClaimBeforePrepareWitness.manifest.template.json",
        "revoke": "trace-manifests/P49PipelinedRevokeRaceWitness.manifest.template.json",
        "default": "trace-manifests/P49PipelinedDefaultAllowMutant.manifest.template.json",
        "side": "trace-manifests/P49PipelinedSideEffectBeforePrepare.manifest.template.json",
        "pending": "trace-manifests/P49PipelinedUnboundedPending.manifest.template.json",
    }


def add_focused_manifest(trace_paths: dict[str, str]) -> None:
    base_path = HERE / "formal-checks.json"
    base = json.loads(base_path.read_text(encoding="utf-8"))
    selected_ids = [
        "core-fixed", "core-heterogeneous", "core-mixed-token-mutant",
        "core-release-claimed-mutant", "core-revoked-result-liveness",
        "network-fixed", "network-claim-wins-queued-revoke-witness",
        "network-fence-wins-delayed-claim-witness", "network-compaction-fixed",
        "network-usecs-before-ready-mutant", "network-release-on-enqueue-mutant",
        "network-default-allow-after-compaction-mutant", "network-f2s-bypass-mutant",
        "network-fair-liveness",
    ]
    by_id = {row["id"]: row for row in base["checks"]}
    missing = [item for item in selected_ids if item not in by_id]
    if missing:
        raise SystemExit(f"missing inherited rows: {missing}")
    rows = [by_id[item] for item in selected_ids]

    def row(check_id: str, config: str, expected: str, prop: str, *, kind: str = "safety", trace: str | None = None) -> dict[str, Any]:
        result: dict[str, Any] = {
            "id": check_id,
            "module": "AssignmentFenceAdmissionModes",
            "config": config,
            "kind": kind,
            "workers": 1,
            "expected": expected,
            "property": prop,
            "toolchains": ["stable", "differential"],
            "timeout_seconds": 3600 if kind == "liveness" else 1800,
        }
        if trace:
            result["trace_manifest"] = trace
        return result

    rows.extend([
        row("p49-strict-within-epoch", "P49StrictWithinEpoch.cfg", "pass", "SafetyInvariant"),
        row("p49-pipelined-within-epoch", "P49PipelinedWithinEpoch.cfg", "pass", "SafetyInvariant"),
        row("p49-pipelined-claim-before-prepare", "P49PipelinedClaimBeforePrepare.cfg", "pass", "SafetyInvariant"),
        row("p49-pipelined-revoke-race", "P49PipelinedRevokeRace.cfg", "pass", "SafetyInvariant"),
        row("p49-pipelined-claim-before-prepare-witness", "P49PipelinedClaimBeforePrepareWitness.cfg", "counterexample", "NoClaimBeforePrepareWitness", trace=trace_paths["claim"]),
        row("p49-pipelined-revoke-race-witness", "P49PipelinedRevokeRaceWitness.cfg", "counterexample", "NoRevokeRaceWitness", trace=trace_paths["revoke"]),
        row("p49-pipelined-default-allow-mutant", "P49PipelinedDefaultAllowMutant.cfg", "counterexample", "NoStartAfterRelease", trace=trace_paths["default"]),
        row("p49-pipelined-side-effect-before-prepare-mutant", "P49PipelinedSideEffectBeforePrepare.cfg", "counterexample", "NoSideEffectBeforePrepare", trace=trace_paths["side"]),
        row("p49-pipelined-unbounded-pending-mutant", "P49PipelinedUnboundedPending.cfg", "counterexample", "PendingBound", trace=trace_paths["pending"]),
        row("p49-pipelined-pending-liveness", "P49PipelinedPendingLiveness.cfg", "pass", "PendingEventuallyResolved", kind="liveness"),
    ])
    focused = {
        "schema": 1,
        "description": "Proof-bearing assignment-fence gate: unbounded ownership core, finite FIFO network quotient, strict enforcing baseline, and separately negotiated pipelined-enforcing contingency.",
        "checks": rows,
        "proofs": base.get("proofs", []),
    }
    (HERE / "assignment-fence-proof-checks-v1.json").write_text(
        json.dumps(focused, indent=2) + "\n", encoding="utf-8"
    )


def main() -> None:
    (HERE / "AssignmentFenceAdmissionModes.tla").write_text(
        textwrap.dedent(MODEL).lstrip(), encoding="utf-8"
    )
    for name, values in CONFIGS.items():
        write_config(HERE / name, values)
    trace_paths = write_traces()
    add_focused_manifest(trace_paths)


if __name__ == "__main__":
    main()
