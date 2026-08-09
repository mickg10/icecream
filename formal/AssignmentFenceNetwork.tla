------------------------- MODULE AssignmentFenceNetwork -------------------------
(***************************************************************************
Finite network model for the strict enforcing assignment-fence protocol.

The abstract core proves assignment ownership without transport.  This module
adds bounded FIFO streams and distinguishes:

  queued frame     -- accepted into a channel output buffer;
  flushed frame    -- consumed from that FIFO by the peer;
  protocol result  -- consumed and applied by the receiving state machine.

Two ownership tokens remain separate:

  schedulerReservation[a]  S still owns logical assignment a;
  workerSlot[a]            F still consumes physical capacity for a.

F may free workerSlot when REVOKE linearizes, but S may clear
schedulerReservation only after consuming the complete REVOKED result.  A
live-session FIFO may stop draining forever; safety has no drain-fairness
assumption.  Liveness configurations may add weak fairness for the exact drain
and processing actions, or take the explicit session-loss transition.

The four mutant switches remove one load-bearing premise each:

  MutantUseCSBeforeReady       expose UseCS before READY is consumed;
  MutantReleaseOnFEnqueue      S releases when F queues REVOKED;
  MutantDefaultAllowUnknown    a compacted legacy id can start after release;
  MutantF2SBypass              S consumes a later F->S frame before its FIFO
                              predecessor.
***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Assignments, Workers,
          LegacyId, TokenRequired,
          Capacity, WorkerOf, PolicyOf,
          MaxS2F, MaxF2S, MaxS2D,
          MutantUseCSBeforeReady,
          MutantReleaseOnFEnqueue,
          MutantDefaultAllowUnknown,
          MutantF2SBypass

ASSUME /\ IsFiniteSet(Assignments)
       /\ Assignments # {}
       /\ IsFiniteSet(Workers)
       /\ Workers # {}
       /\ LegacyId # TokenRequired
       /\ Capacity \in [Workers -> (Nat \ {0})]
       /\ WorkerOf \in [Assignments -> Workers]
       /\ PolicyOf \in [Assignments -> {LegacyId, TokenRequired}]
       /\ MaxS2F \in Nat \ {0}
       /\ MaxF2S \in Nat \ {0}
       /\ MaxS2D \in Nat \ {0}
       /\ MutantUseCSBeforeReady \in BOOLEAN
       /\ MutantReleaseOnFEnqueue \in BOOLEAN
       /\ MutantDefaultAllowUnknown \in BOOLEAN
       /\ MutantF2SBypass \in BOOLEAN

SchedulerPhases ==
    {"Absent", "PrepareQueued", "Prepared", "Ready", "UseCSQueued",
     "DeliveryUncertain", "Claimed", "RevokeQueued", "RevokedAtF",
     "Started", "Terminal"}
FStates == {"None", "Reserved", "Claimed", "Started", "Revoked"}
ReleaseCauses == {"None", "Revoked", "Done", "SessionLoss"}
S2FKinds == {"PREPARE", "REVOKE"}
F2SKinds == {"READY", "REVOKED", "STARTED", "BEGIN", "DONE"}
S2DKinds == {"USECS"}
AllKinds == S2FKinds \cup F2SKinds \cup S2DKinds

Msg(kind, assignment, seq) ==
    [kind |-> kind, assignment |-> assignment, seq |-> seq]

MessageType ==
    [kind : AllKinds, assignment : Assignments, seq : Nat]

RemoveAt(s, index) ==
    [i \in 1..(Len(s) - 1) |-> IF i < index THEN s[i] ELSE s[i + 1]]

SeqElems(s) == {s[i] : i \in 1..Len(s)}

VARIABLES phase,
          fState,
          schedulerReservation,
          workerSlot,
          released,
          releaseCause,
          terminalCount,
          readySeen,
          usecsDelivered,
          claimMade,
          claimExact,
          startCount,
          startAfterRelease,
          revokedEnqueued,
          revokedConsumed,
          beginConsumed,
          s2f,
          f2s,
          s2d,
          nextF2SSeq,
          lastF2SConsumed,
          sfLive,
          sdLive

vars ==
    <<phase, fState, schedulerReservation, workerSlot, released,
      releaseCause, terminalCount, readySeen, usecsDelivered, claimMade,
      claimExact, startCount, startAfterRelease, revokedEnqueued,
      revokedConsumed, beginConsumed, s2f, f2s, s2d, nextF2SSeq,
      lastF2SConsumed, sfLive, sdLive>>

WorkerOccupancy(w) ==
    Cardinality({a \in Assignments : workerSlot[a] /\ WorkerOf[a] = w})

ChosenF2SIndex ==
    IF MutantF2SBypass /\ Len(f2s) >= 2 THEN 2 ELSE 1

ChosenF2S == f2s[ChosenF2SIndex]

Init ==
    /\ phase = [a \in Assignments |-> "Absent"]
    /\ fState = [a \in Assignments |-> "None"]
    /\ schedulerReservation = [a \in Assignments |-> FALSE]
    /\ workerSlot = [a \in Assignments |-> FALSE]
    /\ released = [a \in Assignments |-> FALSE]
    /\ releaseCause = [a \in Assignments |-> "None"]
    /\ terminalCount = [a \in Assignments |-> 0]
    /\ readySeen = [a \in Assignments |-> FALSE]
    /\ usecsDelivered = [a \in Assignments |-> FALSE]
    /\ claimMade = [a \in Assignments |-> FALSE]
    /\ claimExact = [a \in Assignments |-> FALSE]
    /\ startCount = [a \in Assignments |-> 0]
    /\ startAfterRelease = [a \in Assignments |-> 0]
    /\ revokedEnqueued = [a \in Assignments |-> FALSE]
    /\ revokedConsumed = [a \in Assignments |-> FALSE]
    /\ beginConsumed = [a \in Assignments |-> FALSE]
    /\ s2f = <<>>
    /\ f2s = <<>>
    /\ s2d = <<>>
    /\ nextF2SSeq = 1
    /\ lastF2SConsumed = 0
    /\ sfLive = TRUE
    /\ sdLive = TRUE

SPrepare(a) ==
    /\ a \in Assignments
    /\ sfLive
    /\ phase[a] = "Absent"
    /\ Len(s2f) < MaxS2F
    /\ phase' = [phase EXCEPT ![a] = "PrepareQueued"]
    /\ schedulerReservation' = [schedulerReservation EXCEPT ![a] = TRUE]
    /\ s2f' = Append(s2f, Msg("PREPARE", a, 0))
    /\ UNCHANGED <<fState, workerSlot, released, releaseCause,
                    terminalCount, readySeen, usecsDelivered, claimMade,
                    claimExact, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    f2s, s2d, nextF2SSeq, lastF2SConsumed, sfLive, sdLive>>

FReceivePrepare ==
    /\ sfLive
    /\ Len(s2f) > 0
    /\ s2f[1].kind = "PREPARE"
    /\ LET a == s2f[1].assignment
       IN /\ phase[a] = "PrepareQueued"
          /\ fState[a] = "None"
          /\ WorkerOccupancy(WorkerOf[a]) < Capacity[WorkerOf[a]]
          /\ Len(f2s) < MaxF2S
          /\ phase' = [phase EXCEPT ![a] = "Prepared"]
          /\ fState' = [fState EXCEPT ![a] = "Reserved"]
          /\ workerSlot' = [workerSlot EXCEPT ![a] = TRUE]
          /\ f2s' = Append(f2s, Msg("READY", a, nextF2SSeq))
    /\ s2f' = Tail(s2f)
    /\ nextF2SSeq' = nextF2SSeq + 1
    /\ UNCHANGED <<schedulerReservation, released, releaseCause,
                    terminalCount, readySeen, usecsDelivered, claimMade,
                    claimExact, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    s2d, lastF2SConsumed, sfLive, sdLive>>

SReceiveReady ==
    /\ sfLive
    /\ Len(f2s) > 0
    /\ ChosenF2S.kind = "READY"
    /\ LET a == ChosenF2S.assignment
       IN /\ phase[a] = "Prepared"
          /\ phase' = [phase EXCEPT ![a] = "Ready"]
          /\ readySeen' = [readySeen EXCEPT ![a] = TRUE]
    /\ lastF2SConsumed' = ChosenF2S.seq
    /\ f2s' = RemoveAt(f2s, ChosenF2SIndex)
    /\ UNCHANGED <<fState, schedulerReservation, workerSlot, released,
                    releaseCause, terminalCount, usecsDelivered, claimMade,
                    claimExact, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    s2f, s2d, nextF2SSeq, sfLive, sdLive>>

SQueueUseCS(a) ==
    /\ a \in Assignments
    /\ sdLive
    /\ Len(s2d) < MaxS2D
    /\ (phase[a] = "Ready"
        \/ (MutantUseCSBeforeReady
            /\ phase[a] \in {"PrepareQueued", "Prepared"}))
    /\ phase' = [phase EXCEPT ![a] = "UseCSQueued"]
    /\ s2d' = Append(s2d, Msg("USECS", a, 0))
    /\ UNCHANGED <<fState, schedulerReservation, workerSlot, released,
                    releaseCause, terminalCount, readySeen, usecsDelivered,
                    claimMade, claimExact, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    s2f, f2s, nextF2SSeq, lastF2SConsumed, sfLive, sdLive>>

DReceiveUseCS ==
    /\ sdLive
    /\ Len(s2d) > 0
    /\ s2d[1].kind = "USECS"
    /\ LET a == s2d[1].assignment
       IN /\ phase[a] = "UseCSQueued"
          /\ phase' = [phase EXCEPT ![a] = "DeliveryUncertain"]
          /\ usecsDelivered' = [usecsDelivered EXCEPT ![a] = TRUE]
    /\ s2d' = Tail(s2d)
    /\ UNCHANGED <<fState, schedulerReservation, workerSlot, released,
                    releaseCause, terminalCount, readySeen, claimMade,
                    claimExact, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    s2f, f2s, nextF2SSeq, lastF2SConsumed, sfLive, sdLive>>

FClaimExact(a) ==
    /\ a \in Assignments
    /\ phase[a] = "DeliveryUncertain"
    /\ fState[a] = "Reserved"
    /\ phase' = [phase EXCEPT ![a] = "Claimed"]
    /\ fState' = [fState EXCEPT ![a] = "Claimed"]
    /\ claimMade' = [claimMade EXCEPT ![a] = TRUE]
    /\ claimExact' = [claimExact EXCEPT ![a] = TRUE]
    /\ UNCHANGED <<schedulerReservation, workerSlot, released, releaseCause,
                    terminalCount, readySeen, usecsDelivered, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, s2f, f2s, s2d, nextF2SSeq,
                    lastF2SConsumed, sfLive, sdLive>>

FClaimLegacy(a) ==
    /\ a \in Assignments
    /\ phase[a] = "DeliveryUncertain"
    /\ fState[a] = "Reserved"
    /\ (PolicyOf[a] = LegacyId \/ MutantDefaultAllowUnknown)
    /\ phase' = [phase EXCEPT ![a] = "Claimed"]
    /\ fState' = [fState EXCEPT ![a] = "Claimed"]
    /\ claimMade' = [claimMade EXCEPT ![a] = TRUE]
    /\ claimExact' = [claimExact EXCEPT ![a] = FALSE]
    /\ UNCHANGED <<schedulerReservation, workerSlot, released, releaseCause,
                    terminalCount, readySeen, usecsDelivered, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, s2f, f2s, s2d, nextF2SSeq,
                    lastF2SConsumed, sfLive, sdLive>>

FStart(a) ==
    /\ a \in Assignments
    /\ fState[a] = "Claimed"
    /\ ~released[a]
    /\ Len(f2s) < MaxF2S
    /\ fState' = [fState EXCEPT ![a] = "Started"]
    /\ startCount' = [startCount EXCEPT ![a] = @ + 1]
    /\ f2s' = Append(f2s, Msg("BEGIN", a, nextF2SSeq))
    /\ nextF2SSeq' = nextF2SSeq + 1
    /\ UNCHANGED <<phase, schedulerReservation, workerSlot, released,
                    releaseCause, terminalCount, readySeen, usecsDelivered,
                    claimMade, claimExact, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    s2f, s2d, lastF2SConsumed, sfLive, sdLive>>

SReceiveBegin ==
    /\ sfLive
    /\ Len(f2s) > 0
    /\ ChosenF2S.kind = "BEGIN"
    /\ LET a == ChosenF2S.assignment
       IN /\ fState[a] = "Started"
          /\ phase' = [phase EXCEPT ![a] = "Started"]
          /\ beginConsumed' = [beginConsumed EXCEPT ![a] = TRUE]
    /\ lastF2SConsumed' = ChosenF2S.seq
    /\ f2s' = RemoveAt(f2s, ChosenF2SIndex)
    /\ UNCHANGED <<fState, schedulerReservation, workerSlot, released,
                    releaseCause, terminalCount, readySeen, usecsDelivered,
                    claimMade, claimExact, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, s2f, s2d,
                    nextF2SSeq, sfLive, sdLive>>

SQueueRevoke(a) ==
    /\ a \in Assignments
    /\ sfLive
    /\ schedulerReservation[a]
    /\ phase[a] \in {"Prepared", "Ready", "UseCSQueued",
                      "DeliveryUncertain", "Claimed"}
    /\ Len(s2f) < MaxS2F
    /\ phase' = [phase EXCEPT ![a] = "RevokeQueued"]
    /\ s2f' = Append(s2f, Msg("REVOKE", a, 0))
    /\ UNCHANGED <<fState, schedulerReservation, workerSlot, released,
                    releaseCause, terminalCount, readySeen, usecsDelivered,
                    claimMade, claimExact, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    f2s, s2d, nextF2SSeq, lastF2SConsumed, sfLive, sdLive>>

FReceiveRevokeNotStarted ==
    /\ sfLive
    /\ Len(s2f) > 0
    /\ s2f[1].kind = "REVOKE"
    /\ LET a == s2f[1].assignment
       IN /\ fState[a] \in {"Reserved", "Claimed"}
          /\ Len(f2s) < MaxF2S
          /\ phase' = [phase EXCEPT
                 ![a] = IF MutantReleaseOnFEnqueue
                        THEN "Terminal" ELSE "RevokedAtF"]
          /\ fState' = [fState EXCEPT ![a] = "Revoked"]
          /\ workerSlot' = [workerSlot EXCEPT ![a] = FALSE]
          /\ schedulerReservation' =
                 IF MutantReleaseOnFEnqueue
                 THEN [schedulerReservation EXCEPT ![a] = FALSE]
                 ELSE schedulerReservation
          /\ released' =
                 IF MutantReleaseOnFEnqueue
                 THEN [released EXCEPT ![a] = TRUE]
                 ELSE released
          /\ releaseCause' =
                 IF MutantReleaseOnFEnqueue
                 THEN [releaseCause EXCEPT ![a] = "Revoked"]
                 ELSE releaseCause
          /\ terminalCount' =
                 IF MutantReleaseOnFEnqueue
                 THEN [terminalCount EXCEPT ![a] = @ + 1]
                 ELSE terminalCount
          /\ revokedEnqueued' = [revokedEnqueued EXCEPT ![a] = TRUE]
          /\ f2s' = Append(f2s, Msg("REVOKED", a, nextF2SSeq))
    /\ s2f' = Tail(s2f)
    /\ nextF2SSeq' = nextF2SSeq + 1
    /\ UNCHANGED <<readySeen, usecsDelivered, claimMade, claimExact,
                    startCount, startAfterRelease, revokedConsumed,
                    beginConsumed, s2d, lastF2SConsumed, sfLive, sdLive>>

FReceiveRevokeStarted ==
    /\ sfLive
    /\ Len(s2f) > 0
    /\ s2f[1].kind = "REVOKE"
    /\ LET a == s2f[1].assignment
       IN /\ fState[a] = "Started"
          /\ Len(f2s) < MaxF2S
          /\ f2s' = Append(f2s, Msg("STARTED", a, nextF2SSeq))
    /\ s2f' = Tail(s2f)
    /\ nextF2SSeq' = nextF2SSeq + 1
    /\ UNCHANGED <<phase, fState, schedulerReservation, workerSlot,
                    released, releaseCause, terminalCount, readySeen,
                    usecsDelivered, claimMade, claimExact, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, s2d, lastF2SConsumed, sfLive, sdLive>>

SReceiveRevoked ==
    /\ sfLive
    /\ Len(f2s) > 0
    /\ ChosenF2S.kind = "REVOKED"
    /\ LET a == ChosenF2S.assignment
       IN /\ ~released[a]
          /\ revokedEnqueued[a]
          /\ phase' = [phase EXCEPT ![a] = "Terminal"]
          /\ schedulerReservation' =
                 [schedulerReservation EXCEPT ![a] = FALSE]
          /\ released' = [released EXCEPT ![a] = TRUE]
          /\ releaseCause' = [releaseCause EXCEPT ![a] = "Revoked"]
          /\ terminalCount' = [terminalCount EXCEPT ![a] = @ + 1]
          /\ revokedConsumed' = [revokedConsumed EXCEPT ![a] = TRUE]
    /\ lastF2SConsumed' = ChosenF2S.seq
    /\ f2s' = RemoveAt(f2s, ChosenF2SIndex)
    /\ UNCHANGED <<fState, workerSlot, readySeen, usecsDelivered,
                    claimMade, claimExact, startCount, startAfterRelease,
                    revokedEnqueued, beginConsumed, s2f, s2d,
                    nextF2SSeq, sfLive, sdLive>>

SReceiveStarted ==
    /\ sfLive
    /\ Len(f2s) > 0
    /\ ChosenF2S.kind = "STARTED"
    /\ LET a == ChosenF2S.assignment
       IN /\ fState[a] = "Started"
          /\ phase' = [phase EXCEPT ![a] = "Started"]
    /\ lastF2SConsumed' = ChosenF2S.seq
    /\ f2s' = RemoveAt(f2s, ChosenF2SIndex)
    /\ UNCHANGED <<fState, schedulerReservation, workerSlot, released,
                    releaseCause, terminalCount, readySeen, usecsDelivered,
                    claimMade, claimExact, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    s2f, s2d, nextF2SSeq, sfLive, sdLive>>

FComplete(a) ==
    /\ a \in Assignments
    /\ fState[a] = "Started"
    /\ Len(f2s) < MaxF2S
    /\ fState' = [fState EXCEPT ![a] = "None"]
    /\ workerSlot' = [workerSlot EXCEPT ![a] = FALSE]
    /\ f2s' = Append(f2s, Msg("DONE", a, nextF2SSeq))
    /\ nextF2SSeq' = nextF2SSeq + 1
    /\ UNCHANGED <<phase, schedulerReservation, released, releaseCause,
                    terminalCount, readySeen, usecsDelivered, claimMade,
                    claimExact, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    s2f, s2d, lastF2SConsumed, sfLive, sdLive>>

SReceiveDone ==
    /\ sfLive
    /\ Len(f2s) > 0
    /\ ChosenF2S.kind = "DONE"
    /\ LET a == ChosenF2S.assignment
       IN /\ schedulerReservation[a]
          /\ ~released[a]
          /\ phase' = [phase EXCEPT ![a] = "Terminal"]
          /\ schedulerReservation' =
                 [schedulerReservation EXCEPT ![a] = FALSE]
          /\ released' = [released EXCEPT ![a] = TRUE]
          /\ releaseCause' = [releaseCause EXCEPT ![a] = "Done"]
          /\ terminalCount' = [terminalCount EXCEPT ![a] = @ + 1]
    /\ lastF2SConsumed' = ChosenF2S.seq
    /\ f2s' = RemoveAt(f2s, ChosenF2SIndex)
    /\ UNCHANGED <<fState, workerSlot, readySeen, usecsDelivered,
                    claimMade, claimExact, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    s2f, s2d, nextF2SSeq, sfLive, sdLive>>

LateUnknownLegacyStart(a) ==
    /\ a \in Assignments
    /\ MutantDefaultAllowUnknown
    /\ PolicyOf[a] = LegacyId
    /\ released[a]
    /\ fState[a] = "None"
    /\ WorkerOccupancy(WorkerOf[a]) < Capacity[WorkerOf[a]]
    /\ fState' = [fState EXCEPT ![a] = "Started"]
    /\ workerSlot' = [workerSlot EXCEPT ![a] = TRUE]
    /\ startAfterRelease' = [startAfterRelease EXCEPT ![a] = @ + 1]
    /\ UNCHANGED <<phase, schedulerReservation, released, releaseCause,
                    terminalCount, readySeen, usecsDelivered, claimMade,
                    claimExact, startCount, revokedEnqueued, revokedConsumed,
                    beginConsumed, s2f, f2s, s2d, nextF2SSeq,
                    lastF2SConsumed, sfLive, sdLive>>

LoseSFSession ==
    /\ sfLive
    /\ LET live ==
             {a \in Assignments : schedulerReservation[a] \/ workerSlot[a]}
       IN /\ \A a \in live : terminalCount[a] = 0
          /\ phase' =
                 [a \in Assignments |->
                    IF a \in live THEN "Terminal" ELSE phase[a]]
          /\ fState' =
                 [a \in Assignments |->
                    IF a \in live THEN "None" ELSE fState[a]]
          /\ schedulerReservation' =
                 [a \in Assignments |->
                    IF a \in live THEN FALSE ELSE schedulerReservation[a]]
          /\ workerSlot' =
                 [a \in Assignments |->
                    IF a \in live THEN FALSE ELSE workerSlot[a]]
          /\ released' =
                 [a \in Assignments |->
                    IF a \in live THEN TRUE ELSE released[a]]
          /\ releaseCause' =
                 [a \in Assignments |->
                    IF a \in live THEN "SessionLoss" ELSE releaseCause[a]]
          /\ terminalCount' =
                 [a \in Assignments |->
                    IF a \in live THEN terminalCount[a] + 1
                    ELSE terminalCount[a]]
    /\ sfLive' = FALSE
    /\ s2f' = <<>>
    /\ f2s' = <<>>
    /\ s2d' = <<>>
    /\ UNCHANGED <<readySeen, usecsDelivered, claimMade, claimExact,
                    startCount, startAfterRelease, revokedEnqueued,
                    revokedConsumed, beginConsumed, nextF2SSeq,
                    lastF2SConsumed, sdLive>>

ReconnectSF ==
    /\ ~sfLive
    /\ \A a \in Assignments : ~workerSlot[a] /\ fState[a] = "None"
    /\ sfLive' = TRUE
    /\ UNCHANGED <<phase, fState, schedulerReservation, workerSlot,
                    released, releaseCause, terminalCount, readySeen,
                    usecsDelivered, claimMade, claimExact, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, s2f, f2s, s2d, nextF2SSeq,
                    lastF2SConsumed, sdLive>>

LoseSDSession ==
    /\ sdLive
    /\ sdLive' = FALSE
    /\ s2d' = <<>>
    /\ UNCHANGED <<phase, fState, schedulerReservation, workerSlot,
                    released, releaseCause, terminalCount, readySeen,
                    usecsDelivered, claimMade, claimExact, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, s2f, f2s, nextF2SSeq,
                    lastF2SConsumed, sfLive>>

ReconnectSD ==
    /\ ~sdLive
    /\ sdLive' = TRUE
    /\ UNCHANGED <<phase, fState, schedulerReservation, workerSlot,
                    released, releaseCause, terminalCount, readySeen,
                    usecsDelivered, claimMade, claimExact, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, s2f, f2s, s2d, nextF2SSeq,
                    lastF2SConsumed, sfLive>>

Next ==
    \/ \E a \in Assignments : SPrepare(a)
    \/ FReceivePrepare
    \/ SReceiveReady
    \/ \E a \in Assignments : SQueueUseCS(a)
    \/ DReceiveUseCS
    \/ \E a \in Assignments : FClaimExact(a)
    \/ \E a \in Assignments : FClaimLegacy(a)
    \/ \E a \in Assignments : FStart(a)
    \/ SReceiveBegin
    \/ \E a \in Assignments : SQueueRevoke(a)
    \/ FReceiveRevokeNotStarted
    \/ FReceiveRevokeStarted
    \/ SReceiveRevoked
    \/ SReceiveStarted
    \/ \E a \in Assignments : FComplete(a)
    \/ SReceiveDone
    \/ \E a \in Assignments : LateUnknownLegacyStart(a)
    \/ LoseSFSession
    \/ ReconnectSF
    \/ LoseSDSession
    \/ ReconnectSD

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ phase \in [Assignments -> SchedulerPhases]
    /\ fState \in [Assignments -> FStates]
    /\ schedulerReservation \in [Assignments -> BOOLEAN]
    /\ workerSlot \in [Assignments -> BOOLEAN]
    /\ released \in [Assignments -> BOOLEAN]
    /\ releaseCause \in [Assignments -> ReleaseCauses]
    /\ terminalCount \in [Assignments -> Nat]
    /\ readySeen \in [Assignments -> BOOLEAN]
    /\ usecsDelivered \in [Assignments -> BOOLEAN]
    /\ claimMade \in [Assignments -> BOOLEAN]
    /\ claimExact \in [Assignments -> BOOLEAN]
    /\ startCount \in [Assignments -> Nat]
    /\ startAfterRelease \in [Assignments -> Nat]
    /\ revokedEnqueued \in [Assignments -> BOOLEAN]
    /\ revokedConsumed \in [Assignments -> BOOLEAN]
    /\ beginConsumed \in [Assignments -> BOOLEAN]
    /\ s2f \in Seq(MessageType)
    /\ f2s \in Seq(MessageType)
    /\ s2d \in Seq(MessageType)
    /\ nextF2SSeq \in Nat \ {0}
    /\ lastF2SConsumed \in Nat
    /\ sfLive \in BOOLEAN
    /\ sdLive \in BOOLEAN

BufferBound ==
    /\ Len(s2f) <= MaxS2F
    /\ Len(f2s) <= MaxF2S
    /\ Len(s2d) <= MaxS2D

MessageDirectionOK ==
    /\ \A m \in SeqElems(s2f) : m.kind \in S2FKinds /\ m.seq = 0
    /\ \A m \in SeqElems(f2s) : m.kind \in F2SKinds /\ m.seq > 0
    /\ \A m \in SeqElems(s2d) : m.kind \in S2DKinds /\ m.seq = 0

F2SStrictlyIncreasing ==
    /\ \A i, j \in 1..Len(f2s) : i < j => f2s[i].seq < f2s[j].seq
    /\ \A m \in SeqElems(f2s) : m.seq > lastF2SConsumed

WorkerSlotCoherence ==
    \A a \in Assignments :
        workerSlot[a] <=> fState[a] \in {"Reserved", "Claimed", "Started"}

CapacityBound ==
    \A w \in Workers : WorkerOccupancy(w) <= Capacity[w]

ReservationReleaseCoherence ==
    \A a \in Assignments :
        /\ released[a] => ~schedulerReservation[a]
        /\ phase[a] # "Absent" /\ phase[a] # "Terminal"
              => schedulerReservation[a]

ReadyBeforeUseCS ==
    \A a \in Assignments : usecsDelivered[a] => readySeen[a]

ReleaseAfterRevokedConsume ==
    \A a \in Assignments :
        releaseCause[a] = "Revoked" => revokedConsumed[a]

TokenRequiredExactness ==
    \A a \in Assignments :
        PolicyOf[a] = TokenRequired /\ claimMade[a] => claimExact[a]

NoStartAfterRelease ==
    \A a \in Assignments : startAfterRelease[a] = 0

TerminalAtMostOnce ==
    \A a \in Assignments : terminalCount[a] <= 1

TerminalCoherence ==
    \A a \in Assignments :
        (phase[a] = "Terminal") <=> (terminalCount[a] = 1)

SafetyInvariant ==
    /\ TypeOK
    /\ BufferBound
    /\ MessageDirectionOK
    /\ F2SStrictlyIncreasing
    /\ WorkerSlotCoherence
    /\ CapacityBound
    /\ ReservationReleaseCoherence
    /\ ReadyBeforeUseCS
    /\ ReleaseAfterRevokedConsume
    /\ TokenRequiredExactness
    /\ NoStartAfterRelease
    /\ TerminalAtMostOnce
    /\ TerminalCoherence

(***************************************************************************
Weak fairness below is intentionally limited to live-link FIFO drains and
peer processing.  SafetyInvariant is checked against Spec without fairness.
Connection loss remains an explicit alternative transition.
***************************************************************************)
DrainS2F == FReceivePrepare \/ FReceiveRevokeNotStarted \/ FReceiveRevokeStarted
DrainF2S == SReceiveReady \/ SReceiveBegin \/ SReceiveRevoked \/ SReceiveStarted \/ SReceiveDone
DrainS2D == DReceiveUseCS

FencedLivenessSpec ==
    /\ Spec
    /\ WF_vars(DrainS2F)
    /\ WF_vars(DrainF2S)
    /\ WF_vars(DrainS2D)

NoPermanentQueuedFrame ==
    /\ [](Len(s2f) > 0 /\ sfLive => <> (Len(s2f) = 0 \/ ~sfLive))
    /\ [](Len(f2s) > 0 /\ sfLive => <> (Len(f2s) = 0 \/ ~sfLive))
    /\ [](Len(s2d) > 0 /\ sdLive => <> (Len(s2d) = 0 \/ ~sdLive))

MCCapacity == [w \in Workers |-> 1]
MCWorkerOf ==
    [a \in Assignments |->
       IF a = "a0" THEN CHOOSE w \in Workers : TRUE
       ELSE CHOOSE w \in Workers : w # CHOOSE x \in Workers : TRUE]
MCPolicyOf ==
    [a \in Assignments |-> IF a = "a0" THEN TokenRequired ELSE LegacyId]

=============================================================================
