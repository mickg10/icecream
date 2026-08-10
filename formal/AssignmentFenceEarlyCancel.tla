--------------------- MODULE AssignmentFenceEarlyCancel ---------------------
(***************************************************************************
Actor-local strict assignment-fence model for the early-cancel race.

The scheduler (S) and fulfillment daemon (F) own separate state.  F consumes
only frames at the head of S->F and never reads S's instantaneous phase.
This is the essential distributed case:

  S queues PREPARE;
  S queues REVOKE before F consumes PREPARE;
  F consumes PREPARE, emits READY, then consumes REVOKE and emits REVOKED;
  S treats READY as stale and releases only after consuming REVOKED.

Two resources are intentionally distinct:

  schedulerReservation  -- S still accounts for the logical assignment;
  workerSlot            -- F still occupies physical compile capacity.

F may clear workerSlot when REVOKE is consumed.  S must keep
schedulerReservation until the complete REVOKED result is consumed.

Each mutant removes one premise:

  MutantFReadsSchedulerPhase
      F refuses the queued PREPARE after S has moved to RevokeQueued.

  MutantStaleReadyResurrects
      S lets a delayed READY resurrect a canceled assignment.

  MutantReleaseOnEnqueue
      S releases after F queues REVOKED rather than after consuming it.
***************************************************************************)
EXTENDS Naturals, Sequences, TLC

CONSTANTS MaxS2F, MaxF2S,
          MutantFReadsSchedulerPhase,
          MutantStaleReadyResurrects,
          MutantReleaseOnEnqueue

ASSUME /\ MaxS2F \in Nat \ {0}
       /\ MaxF2S \in Nat \ {0}
       /\ MutantFReadsSchedulerPhase \in BOOLEAN
       /\ MutantStaleReadyResurrects \in BOOLEAN
       /\ MutantReleaseOnEnqueue \in BOOLEAN

SPhases == {"Absent", "PrepareQueued", "Ready", "UseCSQueued",
            "RevokeQueued", "Terminal"}
FPhases == {"None", "Prepared", "Revoked"}
S2FKinds == {"PREPARE", "REVOKE"}
F2SKinds == {"READY", "REVOKED"}

VARIABLES sPhase,
          fPhase,
          schedulerReservation,
          workerSlot,
          cancelRequested,
          earlyCancel,
          readySeen,
          usecsExposed,
          revokedReplyQueued,
          revokedReplyConsumed,
          released,
          terminalCount,
          s2f,
          f2s

vars == <<sPhase, fPhase, schedulerReservation, workerSlot,
          cancelRequested, earlyCancel, readySeen, usecsExposed,
          revokedReplyQueued, revokedReplyConsumed, released,
          terminalCount, s2f, f2s>>

Init ==
    /\ sPhase = "Absent"
    /\ fPhase = "None"
    /\ schedulerReservation = FALSE
    /\ workerSlot = FALSE
    /\ cancelRequested = FALSE
    /\ earlyCancel = FALSE
    /\ readySeen = FALSE
    /\ usecsExposed = FALSE
    /\ revokedReplyQueued = FALSE
    /\ revokedReplyConsumed = FALSE
    /\ released = FALSE
    /\ terminalCount = 0
    /\ s2f = <<>>
    /\ f2s = <<>>

SQueuePrepare ==
    /\ sPhase = "Absent"
    /\ Len(s2f) < MaxS2F
    /\ sPhase' = "PrepareQueued"
    /\ schedulerReservation' = TRUE
    /\ s2f' = Append(s2f, "PREPARE")
    /\ UNCHANGED <<fPhase, workerSlot, cancelRequested, earlyCancel,
                    readySeen, usecsExposed, revokedReplyQueued,
                    revokedReplyConsumed, released, terminalCount, f2s>>

SQueueCancel ==
    /\ ~cancelRequested
    /\ schedulerReservation
    /\ sPhase \in {"PrepareQueued", "Ready"}
    /\ Len(s2f) < MaxS2F
    /\ LET isEarly == /\ sPhase = "PrepareQueued"
                       /\ fPhase = "None"
                       /\ Len(s2f) > 0
                       /\ s2f[1] = "PREPARE"
       IN /\ sPhase' = "RevokeQueued"
          /\ cancelRequested' = TRUE
          /\ earlyCancel' = earlyCancel \/ isEarly
    /\ s2f' = Append(s2f, "REVOKE")
    /\ UNCHANGED <<fPhase, schedulerReservation, workerSlot, readySeen,
                    usecsExposed, revokedReplyQueued, revokedReplyConsumed,
                    released, terminalCount, f2s>>

(***************************************************************************
F consumes the frame it received.  The fixed action does not inspect sPhase.
The mutant adds precisely that forbidden oracle and deadlocks the early-cancel
queue at PREPARE after S has already moved to RevokeQueued.
***************************************************************************)
FConsumePrepare ==
    /\ Len(s2f) > 0
    /\ s2f[1] = "PREPARE"
    /\ fPhase = "None"
    /\ (~MutantFReadsSchedulerPhase \/ sPhase = "PrepareQueued")
    /\ Len(f2s) < MaxF2S
    /\ fPhase' = "Prepared"
    /\ workerSlot' = TRUE
    /\ s2f' = Tail(s2f)
    /\ f2s' = Append(f2s, "READY")
    /\ UNCHANGED <<sPhase, schedulerReservation, cancelRequested,
                    earlyCancel, readySeen, usecsExposed,
                    revokedReplyQueued, revokedReplyConsumed, released,
                    terminalCount>>

(***************************************************************************
READY is current only while S is still waiting for it.  After cancellation it
is consumed as stale.  The mutant resurrects Ready and thereby permits UseCS.
***************************************************************************)
SConsumeReady ==
    /\ Len(f2s) > 0
    /\ f2s[1] = "READY"
    /\ LET nextPhase ==
              IF sPhase = "PrepareQueued"
              THEN "Ready"
              ELSE IF MutantStaleReadyResurrects /\ sPhase = "RevokeQueued"
                   THEN "Ready"
                   ELSE sPhase
       IN sPhase' = nextPhase
    /\ readySeen' = TRUE
    /\ f2s' = Tail(f2s)
    /\ UNCHANGED <<fPhase, schedulerReservation, workerSlot,
                    cancelRequested, earlyCancel, usecsExposed,
                    revokedReplyQueued, revokedReplyConsumed, released,
                    terminalCount, s2f>>

SQueueUseCS ==
    /\ sPhase = "Ready"
    /\ ~usecsExposed
    /\ sPhase' = "UseCSQueued"
    /\ usecsExposed' = TRUE
    /\ UNCHANGED <<fPhase, schedulerReservation, workerSlot,
                    cancelRequested, earlyCancel, readySeen,
                    revokedReplyQueued, revokedReplyConsumed, released,
                    terminalCount, s2f, f2s>>

FConsumeRevoke ==
    /\ Len(s2f) > 0
    /\ s2f[1] = "REVOKE"
    /\ fPhase = "Prepared"
    /\ Len(f2s) < MaxF2S
    /\ fPhase' = "Revoked"
    /\ workerSlot' = FALSE
    /\ revokedReplyQueued' = TRUE
    /\ s2f' = Tail(s2f)
    /\ f2s' = Append(f2s, "REVOKED")
    /\ UNCHANGED <<sPhase, schedulerReservation, cancelRequested,
                    earlyCancel, readySeen, usecsExposed,
                    revokedReplyConsumed, released, terminalCount>>

(***************************************************************************
Direct mutant for the forbidden release linearization point.  The scheduler
has not consumed the result; f2s still contains REVOKED.
***************************************************************************)
SPrematureRelease ==
    /\ MutantReleaseOnEnqueue
    /\ revokedReplyQueued
    /\ ~revokedReplyConsumed
    /\ ~released
    /\ sPhase' = "Terminal"
    /\ schedulerReservation' = FALSE
    /\ released' = TRUE
    /\ terminalCount' = terminalCount + 1
    /\ UNCHANGED <<fPhase, workerSlot, cancelRequested, earlyCancel,
                    readySeen, usecsExposed, revokedReplyQueued,
                    revokedReplyConsumed, s2f, f2s>>

SConsumeRevoked ==
    /\ Len(f2s) > 0
    /\ f2s[1] = "REVOKED"
    /\ ~revokedReplyConsumed
    /\ revokedReplyConsumed' = TRUE
    /\ f2s' = Tail(f2s)
    /\ IF released
          THEN UNCHANGED <<sPhase, schedulerReservation, released,
                           terminalCount>>
          ELSE /\ sPhase' = "Terminal"
               /\ schedulerReservation' = FALSE
               /\ released' = TRUE
               /\ terminalCount' = terminalCount + 1
    /\ UNCHANGED <<fPhase, workerSlot, cancelRequested, earlyCancel,
                    readySeen, usecsExposed, revokedReplyQueued, s2f>>

Next ==
    \/ SQueuePrepare
    \/ SQueueCancel
    \/ FConsumePrepare
    \/ SConsumeReady
    \/ SQueueUseCS
    \/ FConsumeRevoke
    \/ SPrematureRelease
    \/ SConsumeRevoked

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ sPhase \in SPhases
    /\ fPhase \in FPhases
    /\ schedulerReservation \in BOOLEAN
    /\ workerSlot \in BOOLEAN
    /\ cancelRequested \in BOOLEAN
    /\ earlyCancel \in BOOLEAN
    /\ readySeen \in BOOLEAN
    /\ usecsExposed \in BOOLEAN
    /\ revokedReplyQueued \in BOOLEAN
    /\ revokedReplyConsumed \in BOOLEAN
    /\ released \in BOOLEAN
    /\ terminalCount \in Nat
    /\ s2f \in Seq(S2FKinds)
    /\ f2s \in Seq(F2SKinds)
    /\ Len(s2f) <= MaxS2F
    /\ Len(f2s) <= MaxF2S

WorkerSlotCoherence == workerSlot <=> fPhase = "Prepared"
ReleaseAfterRevokedConsumption == released => revokedReplyConsumed
ReservationUntilRevokedConsumption ==
    revokedReplyQueued /\ ~revokedReplyConsumed => schedulerReservation
NoUseCSAfterCancel == cancelRequested => ~usecsExposed
UseCSAfterReady == usecsExposed => readySeen
TerminalExactlyOnce == terminalCount <= 1
ReleasedIsTerminal == released => sPhase = "Terminal" /\ terminalCount = 1

SafetyInvariant ==
    /\ TypeOK
    /\ WorkerSlotCoherence
    /\ ReleaseAfterRevokedConsumption
    /\ ReservationUntilRevokedConsumption
    /\ NoUseCSAfterCancel
    /\ UseCSAfterReady
    /\ TerminalExactlyOnce
    /\ ReleasedIsTerminal

EarlyCancelCompleted ==
    /\ earlyCancel
    /\ revokedReplyConsumed
    /\ released
    /\ ~schedulerReservation
    /\ ~workerSlot
    /\ sPhase = "Terminal"

NoEarlyCancelCompleted == ~EarlyCancelCompleted

=============================================================================
