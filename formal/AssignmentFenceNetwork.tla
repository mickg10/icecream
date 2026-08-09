------------------------- MODULE AssignmentFenceNetwork -------------------------
(***************************************************************************
Finite FIFO network model for strict enforcing assignment fencing.

The abstract core has no transport.  This module separates:

  queued frame     -- accepted into a sender output FIFO;
  delivered frame  -- consumed from that FIFO by the peer;
  protocol result  -- validated and applied by the receiving state machine.

Four bounded streams are explicit:

  s2f  scheduler -> fulfillment daemon: PREPARE, REVOKE
  f2s  fulfillment daemon -> scheduler: READY, REVOKED, OWNED, BEGIN, DONE
  s2d  scheduler -> submitter daemon/client: USECS
  c2f  delayed client -> fulfillment daemon: concrete assignment claim

The C->F claim carries the assignment correlation fields.  F claim/revoke
transitions inspect only F-visible state plus the received claim; they never
consult the scheduler's instantaneous phase.  This admits both distributed
orders that the proof must cover:

  S queues REVOKE; F consumes the already-delivered claim first -> OWNED
  F installs the fence first; delayed concrete claim arrives -> rejected

Two conserved tokens remain distinct:

  schedulerReservation[a]  S still owns logical assignment a;
  workerSlot[a]            F still consumes physical capacity for a.

F may free workerSlot when REVOKE linearizes, but S clears
schedulerReservation only after consuming the complete REVOKED result.  Safety
has no drain-fairness assumption.  The liveness configuration adds weak
fairness only for exact live-link drain/processing actions; connection loss is
an explicit alternative terminal transition.

Mutants remove one load-bearing premise each:

  MutantUseCSBeforeReady       expose UseCS before READY is consumed;
  MutantReleaseOnFEnqueue      S releases when F merely queues REVOKED;
  MutantDefaultAllowUnknown    after terminal-record compaction, consume a
                              retained delayed legacy claim by starting it;
  MutantF2SBypass              S consumes a later F->S frame before its FIFO
                              predecessor.
***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Assignments, Workers,
          LegacyId, TokenRequired,
          Capacity, WorkerOf, PolicyOf,
          WireOf, FullIdOf, TokenOf, NoFullId, NoToken,
          MaxS2F, MaxF2S, MaxS2D, MaxC2F,
          MutantUseCSBeforeReady,
          MutantReleaseOnFEnqueue,
          MutantDefaultAllowUnknown,
          MutantF2SBypass

ASSUME /\ IsFiniteSet(Assignments)
       /\ Assignments # {}
       /\ IsFiniteSet(Workers)
       /\ Cardinality(Workers) >= 2
       /\ LegacyId # TokenRequired
       /\ Capacity \in [Workers -> (Nat \ {0})]
       /\ WorkerOf \in [Assignments -> Workers]
       /\ PolicyOf \in [Assignments -> {LegacyId, TokenRequired}]
       /\ WireOf \in [Assignments -> (Nat \ {0})]
       /\ FullIdOf \in [Assignments -> (Nat \ {0})]
       /\ TokenOf \in [Assignments -> (Nat \ {0})]
       /\ NoFullId = 0
       /\ NoToken = 0
       /\ \A a, b \in Assignments : a # b => FullIdOf[a] # FullIdOf[b]
       /\ \A a, b \in Assignments : a # b => TokenOf[a] # TokenOf[b]
       /\ MaxS2F \in Nat \ {0}
       /\ MaxF2S \in Nat \ {0}
       /\ MaxS2D \in Nat \ {0}
       /\ MaxC2F \in Nat \ {0}
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
F2SKinds == {"READY", "REVOKED", "OWNED", "BEGIN", "DONE"}
S2DKinds == {"USECS"}
AllKinds == S2FKinds \cup F2SKinds \cup S2DKinds

Msg(kind, assignment, seq) ==
    [kind |-> kind, assignment |-> assignment, seq |-> seq]

MessageType ==
    [kind : AllKinds, assignment : Assignments, seq : Nat]

ClaimMsg(a, exact) ==
    [assignment |-> a,
     wire |-> WireOf[a],
     full |-> IF exact THEN FullIdOf[a] ELSE NoFullId,
     token |-> IF exact THEN TokenOf[a] ELSE NoToken,
     exact |-> exact]

ClaimType ==
    [assignment : Assignments,
     wire : Nat,
     full : Nat,
     token : Nat,
     exact : BOOLEAN]

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
          claimedWire,
          claimedFull,
          claimedToken,
          claimRejected,
          startCount,
          startAfterRelease,
          revokedEnqueued,
          revokedConsumed,
          beginConsumed,
          claimAfterRevokeQueued,
          claimRejectedAfterFence,
          s2f,
          f2s,
          s2d,
          c2f,
          nextF2SSeq,
          lastF2SConsumed,
          sfLive,
          sdLive

vars ==
    <<phase, fState, schedulerReservation, workerSlot, released,
      releaseCause, terminalCount, readySeen, usecsDelivered, claimMade,
      claimExact, claimedWire, claimedFull, claimedToken, claimRejected,
      startCount, startAfterRelease, revokedEnqueued, revokedConsumed,
      beginConsumed, claimAfterRevokeQueued, claimRejectedAfterFence,
      s2f, f2s, s2d, c2f, nextF2SSeq, lastF2SConsumed, sfLive, sdLive>>

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
    /\ claimedWire = [a \in Assignments |-> 0]
    /\ claimedFull = [a \in Assignments |-> 0]
    /\ claimedToken = [a \in Assignments |-> 0]
    /\ claimRejected = [a \in Assignments |-> 0]
    /\ startCount = [a \in Assignments |-> 0]
    /\ startAfterRelease = [a \in Assignments |-> 0]
    /\ revokedEnqueued = [a \in Assignments |-> FALSE]
    /\ revokedConsumed = [a \in Assignments |-> FALSE]
    /\ beginConsumed = [a \in Assignments |-> FALSE]
    /\ claimAfterRevokeQueued = FALSE
    /\ claimRejectedAfterFence = FALSE
    /\ s2f = <<>>
    /\ f2s = <<>>
    /\ s2d = <<>>
    /\ c2f = <<>>
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
                    claimExact, claimedWire, claimedFull, claimedToken,
                    claimRejected, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    claimAfterRevokeQueued, claimRejectedAfterFence,
                    f2s, s2d, c2f, nextF2SSeq, lastF2SConsumed,
                    sfLive, sdLive>>

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
                    claimExact, claimedWire, claimedFull, claimedToken,
                    claimRejected, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    claimAfterRevokeQueued, claimRejectedAfterFence,
                    s2d, c2f, lastF2SConsumed, sfLive, sdLive>>

(***************************************************************************
READY can already be in F->S when S queues REVOKE.  Consume it in FIFO order.
It advances Prepared->Ready only when still current; otherwise it is a stale
observation and cannot resurrect state.
***************************************************************************)
SReceiveReady ==
    /\ sfLive
    /\ Len(f2s) > 0
    /\ ChosenF2S.kind = "READY"
    /\ LET a == ChosenF2S.assignment
       IN /\ phase' =
                IF phase[a] = "Prepared"
                THEN [phase EXCEPT ![a] = "Ready"]
                ELSE phase
          /\ readySeen' = [readySeen EXCEPT ![a] = TRUE]
    /\ lastF2SConsumed' = ChosenF2S.seq
    /\ f2s' = RemoveAt(f2s, ChosenF2SIndex)
    /\ UNCHANGED <<fState, schedulerReservation, workerSlot, released,
                    releaseCause, terminalCount, usecsDelivered, claimMade,
                    claimExact, claimedWire, claimedFull, claimedToken,
                    claimRejected, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    claimAfterRevokeQueued, claimRejectedAfterFence,
                    s2f, s2d, c2f, nextF2SSeq, sfLive, sdLive>>

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
                    claimMade, claimExact, claimedWire, claimedFull,
                    claimedToken, claimRejected, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, claimAfterRevokeQueued,
                    claimRejectedAfterFence, s2f, f2s, c2f,
                    nextF2SSeq, lastF2SConsumed, sfLive, sdLive>>

(***************************************************************************
A complete UseCS frame cannot be retracted from an independent stream.  Its
client claim is materialized in c2f and may remain delayed across SQueueRevoke,
F fencing, S release, and terminal-record compaction.
***************************************************************************)
DReceiveUseCS ==
    /\ sdLive
    /\ Len(s2d) > 0
    /\ s2d[1].kind = "USECS"
    /\ Len(c2f) < MaxC2F
    /\ LET a == s2d[1].assignment
           exact == PolicyOf[a] = TokenRequired
       IN /\ phase' =
                IF phase[a] = "UseCSQueued"
                THEN [phase EXCEPT ![a] = "DeliveryUncertain"]
                ELSE phase
          /\ usecsDelivered' = [usecsDelivered EXCEPT ![a] = TRUE]
          /\ c2f' = Append(c2f, ClaimMsg(a, exact))
    /\ s2d' = Tail(s2d)
    /\ UNCHANGED <<fState, schedulerReservation, workerSlot, released,
                    releaseCause, terminalCount, readySeen, claimMade,
                    claimExact, claimedWire, claimedFull, claimedToken,
                    claimRejected, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    claimAfterRevokeQueued, claimRejectedAfterFence,
                    s2f, f2s, nextF2SSeq, lastF2SConsumed, sfLive, sdLive>>

FClaimExact ==
    /\ sfLive
    /\ Len(c2f) > 0
    /\ LET m == c2f[1]
           a == m.assignment
       IN /\ m.exact
          /\ m.wire = WireOf[a]
          /\ m.full = FullIdOf[a]
          /\ m.token = TokenOf[a]
          /\ fState[a] = "Reserved"
          /\ phase' =
                IF phase[a] = "DeliveryUncertain"
                THEN [phase EXCEPT ![a] = "Claimed"]
                ELSE phase
          /\ fState' = [fState EXCEPT ![a] = "Claimed"]
          /\ claimMade' = [claimMade EXCEPT ![a] = TRUE]
          /\ claimExact' = [claimExact EXCEPT ![a] = TRUE]
          /\ claimedWire' = [claimedWire EXCEPT ![a] = m.wire]
          /\ claimedFull' = [claimedFull EXCEPT ![a] = m.full]
          /\ claimedToken' = [claimedToken EXCEPT ![a] = m.token]
          /\ claimAfterRevokeQueued' =
                (claimAfterRevokeQueued \/ phase[a] = "RevokeQueued")
    /\ c2f' = Tail(c2f)
    /\ UNCHANGED <<schedulerReservation, workerSlot, released, releaseCause,
                    terminalCount, readySeen, usecsDelivered, claimRejected,
                    startCount, startAfterRelease, revokedEnqueued,
                    revokedConsumed, beginConsumed, claimRejectedAfterFence,
                    s2f, f2s, s2d, nextF2SSeq, lastF2SConsumed,
                    sfLive, sdLive>>

FClaimLegacy ==
    /\ sfLive
    /\ Len(c2f) > 0
    /\ LET m == c2f[1]
           a == m.assignment
       IN /\ ~m.exact
          /\ m.wire = WireOf[a]
          /\ PolicyOf[a] = LegacyId
          /\ fState[a] = "Reserved"
          /\ phase' =
                IF phase[a] = "DeliveryUncertain"
                THEN [phase EXCEPT ![a] = "Claimed"]
                ELSE phase
          /\ fState' = [fState EXCEPT ![a] = "Claimed"]
          /\ claimMade' = [claimMade EXCEPT ![a] = TRUE]
          /\ claimExact' = [claimExact EXCEPT ![a] = FALSE]
          /\ claimedWire' = [claimedWire EXCEPT ![a] = m.wire]
          /\ claimedFull' = [claimedFull EXCEPT ![a] = m.full]
          /\ claimedToken' = [claimedToken EXCEPT ![a] = m.token]
          /\ claimAfterRevokeQueued' =
                (claimAfterRevokeQueued \/ phase[a] = "RevokeQueued")
    /\ c2f' = Tail(c2f)
    /\ UNCHANGED <<schedulerReservation, workerSlot, released, releaseCause,
                    terminalCount, readySeen, usecsDelivered, claimRejected,
                    startCount, startAfterRelease, revokedEnqueued,
                    revokedConsumed, beginConsumed, claimRejectedAfterFence,
                    s2f, f2s, s2d, nextF2SSeq, lastF2SConsumed,
                    sfLive, sdLive>>

FRejectClaimFenced ==
    /\ sfLive
    /\ Len(c2f) > 0
    /\ LET m == c2f[1]
           a == m.assignment
       IN /\ m.wire = WireOf[a]
          /\ fState[a] = "Revoked"
          /\ claimRejected' = [claimRejected EXCEPT ![a] = @ + 1]
          /\ claimRejectedAfterFence' = TRUE
    /\ c2f' = Tail(c2f)
    /\ UNCHANGED <<phase, fState, schedulerReservation, workerSlot,
                    released, releaseCause, terminalCount, readySeen,
                    usecsDelivered, claimMade, claimExact, claimedWire,
                    claimedFull, claimedToken, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    claimAfterRevokeQueued, s2f, f2s, s2d,
                    nextF2SSeq, lastF2SConsumed, sfLive, sdLive>>

FRejectUnknownClaim ==
    /\ sfLive
    /\ Len(c2f) > 0
    /\ LET m == c2f[1]
           a == m.assignment
       IN /\ fState[a] = "None"
          /\ released[a]
          /\ ~MutantDefaultAllowUnknown
          /\ claimRejected' = [claimRejected EXCEPT ![a] = @ + 1]
    /\ c2f' = Tail(c2f)
    /\ UNCHANGED <<phase, fState, schedulerReservation, workerSlot,
                    released, releaseCause, terminalCount, readySeen,
                    usecsDelivered, claimMade, claimExact, claimedWire,
                    claimedFull, claimedToken, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    claimAfterRevokeQueued, claimRejectedAfterFence,
                    s2f, f2s, s2d, nextF2SSeq, lastF2SConsumed,
                    sfLive, sdLive>>

(***************************************************************************
The finite-tombstone mutant consumes an actual retained old claim frame after
record compaction.  It is not a spontaneous start action.
***************************************************************************)
FDefaultAllowUnknownClaim ==
    /\ sfLive
    /\ MutantDefaultAllowUnknown
    /\ Len(c2f) > 0
    /\ LET m == c2f[1]
           a == m.assignment
       IN /\ ~m.exact
          /\ m.wire = WireOf[a]
          /\ PolicyOf[a] = LegacyId
          /\ fState[a] = "None"
          /\ released[a]
          /\ WorkerOccupancy(WorkerOf[a]) < Capacity[WorkerOf[a]]
          /\ fState' = [fState EXCEPT ![a] = "Started"]
          /\ workerSlot' = [workerSlot EXCEPT ![a] = TRUE]
          /\ claimMade' = [claimMade EXCEPT ![a] = TRUE]
          /\ claimExact' = [claimExact EXCEPT ![a] = FALSE]
          /\ claimedWire' = [claimedWire EXCEPT ![a] = m.wire]
          /\ claimedFull' = [claimedFull EXCEPT ![a] = m.full]
          /\ claimedToken' = [claimedToken EXCEPT ![a] = m.token]
          /\ startCount' = [startCount EXCEPT ![a] = @ + 1]
          /\ startAfterRelease' = [startAfterRelease EXCEPT ![a] = @ + 1]
    /\ c2f' = Tail(c2f)
    /\ UNCHANGED <<phase, schedulerReservation, released, releaseCause,
                    terminalCount, readySeen, usecsDelivered, claimRejected,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    claimAfterRevokeQueued, claimRejectedAfterFence,
                    s2f, f2s, s2d, nextF2SSeq, lastF2SConsumed,
                    sfLive, sdLive>>

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
                    claimMade, claimExact, claimedWire, claimedFull,
                    claimedToken, claimRejected, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    claimAfterRevokeQueued, claimRejectedAfterFence,
                    s2f, s2d, c2f, lastF2SConsumed, sfLive, sdLive>>

SReceiveBegin ==
    /\ sfLive
    /\ Len(f2s) > 0
    /\ ChosenF2S.kind = "BEGIN"
    /\ LET a == ChosenF2S.assignment
       IN /\ startCount[a] > 0
          /\ phase' =
                IF released[a]
                THEN phase
                ELSE [phase EXCEPT ![a] = "Started"]
          /\ beginConsumed' = [beginConsumed EXCEPT ![a] = TRUE]
    /\ lastF2SConsumed' = ChosenF2S.seq
    /\ f2s' = RemoveAt(f2s, ChosenF2SIndex)
    /\ UNCHANGED <<fState, schedulerReservation, workerSlot, released,
                    releaseCause, terminalCount, readySeen, usecsDelivered,
                    claimMade, claimExact, claimedWire, claimedFull,
                    claimedToken, claimRejected, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    claimAfterRevokeQueued, claimRejectedAfterFence,
                    s2f, s2d, c2f, nextF2SSeq, sfLive, sdLive>>

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
                    claimMade, claimExact, claimedWire, claimedFull,
                    claimedToken, claimRejected, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, claimAfterRevokeQueued,
                    claimRejectedAfterFence, f2s, s2d, c2f,
                    nextF2SSeq, lastF2SConsumed, sfLive, sdLive>>

FReceiveRevokeReserved ==
    /\ sfLive
    /\ Len(s2f) > 0
    /\ s2f[1].kind = "REVOKE"
    /\ LET a == s2f[1].assignment
       IN /\ fState[a] = "Reserved"
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
                    claimedWire, claimedFull, claimedToken, claimRejected,
                    startCount, startAfterRelease, revokedConsumed,
                    beginConsumed, claimAfterRevokeQueued,
                    claimRejectedAfterFence, s2d, c2f, lastF2SConsumed,
                    sfLive, sdLive>>

(***************************************************************************
Once F has accepted the concrete claim, REVOKE cannot reclaim the slot.  F
returns OWNED; S retains schedulerReservation and waits for BEGIN/DONE or
session loss.  This covers the claim-before-revoke-consumption race.
***************************************************************************)
FReceiveRevokeOwned ==
    /\ sfLive
    /\ Len(s2f) > 0
    /\ s2f[1].kind = "REVOKE"
    /\ LET a == s2f[1].assignment
       IN /\ fState[a] \in {"Claimed", "Started"}
          /\ Len(f2s) < MaxF2S
          /\ f2s' = Append(f2s, Msg("OWNED", a, nextF2SSeq))
    /\ s2f' = Tail(s2f)
    /\ nextF2SSeq' = nextF2SSeq + 1
    /\ UNCHANGED <<phase, fState, schedulerReservation, workerSlot,
                    released, releaseCause, terminalCount, readySeen,
                    usecsDelivered, claimMade, claimExact, claimedWire,
                    claimedFull, claimedToken, claimRejected, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, claimAfterRevokeQueued,
                    claimRejectedAfterFence, s2d, c2f, lastF2SConsumed,
                    sfLive, sdLive>>

(***************************************************************************
The process may finish and queue DONE before a previously queued REVOKE reaches
F.  F still consumes the request and returns OWNED/already-started.  The result
trails BEGIN/DONE in the same FIFO and is stale if DONE already terminalized S.
***************************************************************************)
FReceiveRevokeAfterDone ==
    /\ sfLive
    /\ Len(s2f) > 0
    /\ s2f[1].kind = "REVOKE"
    /\ LET a == s2f[1].assignment
       IN /\ fState[a] = "None"
          /\ startCount[a] > 0
          /\ Len(f2s) < MaxF2S
          /\ f2s' = Append(f2s, Msg("OWNED", a, nextF2SSeq))
    /\ s2f' = Tail(s2f)
    /\ nextF2SSeq' = nextF2SSeq + 1
    /\ UNCHANGED <<phase, fState, schedulerReservation, workerSlot,
                    released, releaseCause, terminalCount, readySeen,
                    usecsDelivered, claimMade, claimExact, claimedWire,
                    claimedFull, claimedToken, claimRejected, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, claimAfterRevokeQueued,
                    claimRejectedAfterFence, s2d, c2f, lastF2SConsumed,
                    sfLive, sdLive>>

SReceiveRevoked ==
    /\ sfLive
    /\ Len(f2s) > 0
    /\ ChosenF2S.kind = "REVOKED"
    /\ LET a == ChosenF2S.assignment
       IN /\ revokedEnqueued[a]
          /\ phase' =
                IF released[a]
                THEN phase
                ELSE [phase EXCEPT ![a] = "Terminal"]
          /\ schedulerReservation' =
                IF released[a]
                THEN schedulerReservation
                ELSE [schedulerReservation EXCEPT ![a] = FALSE]
          /\ released' =
                IF released[a]
                THEN released
                ELSE [released EXCEPT ![a] = TRUE]
          /\ releaseCause' =
                IF released[a]
                THEN releaseCause
                ELSE [releaseCause EXCEPT ![a] = "Revoked"]
          /\ terminalCount' =
                IF released[a]
                THEN terminalCount
                ELSE [terminalCount EXCEPT ![a] = @ + 1]
          /\ revokedConsumed' = [revokedConsumed EXCEPT ![a] = TRUE]
    /\ lastF2SConsumed' = ChosenF2S.seq
    /\ f2s' = RemoveAt(f2s, ChosenF2SIndex)
    /\ UNCHANGED <<fState, workerSlot, readySeen, usecsDelivered,
                    claimMade, claimExact, claimedWire, claimedFull,
                    claimedToken, claimRejected, startCount,
                    startAfterRelease, revokedEnqueued, beginConsumed,
                    claimAfterRevokeQueued, claimRejectedAfterFence,
                    s2f, s2d, c2f, nextF2SSeq, sfLive, sdLive>>

SReceiveOwned ==
    /\ sfLive
    /\ Len(f2s) > 0
    /\ ChosenF2S.kind = "OWNED"
    /\ LET a == ChosenF2S.assignment
       IN /\ claimMade[a] \/ startCount[a] > 0
          /\ phase' =
                IF released[a]
                THEN phase
                ELSE [phase EXCEPT
                        ![a] = IF fState[a] = "Started"
                               THEN "Started" ELSE "Claimed"]
    /\ lastF2SConsumed' = ChosenF2S.seq
    /\ f2s' = RemoveAt(f2s, ChosenF2SIndex)
    /\ UNCHANGED <<fState, schedulerReservation, workerSlot, released,
                    releaseCause, terminalCount, readySeen, usecsDelivered,
                    claimMade, claimExact, claimedWire, claimedFull,
                    claimedToken, claimRejected, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, claimAfterRevokeQueued,
                    claimRejectedAfterFence, s2f, s2d, c2f,
                    nextF2SSeq, sfLive, sdLive>>

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
                    claimExact, claimedWire, claimedFull, claimedToken,
                    claimRejected, startCount, startAfterRelease,
                    revokedEnqueued, revokedConsumed, beginConsumed,
                    claimAfterRevokeQueued, claimRejectedAfterFence,
                    s2f, s2d, c2f, lastF2SConsumed, sfLive, sdLive>>

SReceiveDone ==
    /\ sfLive
    /\ Len(f2s) > 0
    /\ ChosenF2S.kind = "DONE"
    /\ LET a == ChosenF2S.assignment
       IN /\ phase' =
                IF released[a]
                THEN phase
                ELSE [phase EXCEPT ![a] = "Terminal"]
          /\ schedulerReservation' =
                IF released[a]
                THEN schedulerReservation
                ELSE [schedulerReservation EXCEPT ![a] = FALSE]
          /\ released' =
                IF released[a]
                THEN released
                ELSE [released EXCEPT ![a] = TRUE]
          /\ releaseCause' =
                IF released[a]
                THEN releaseCause
                ELSE [releaseCause EXCEPT ![a] = "Done"]
          /\ terminalCount' =
                IF released[a]
                THEN terminalCount
                ELSE [terminalCount EXCEPT ![a] = @ + 1]
    /\ lastF2SConsumed' = ChosenF2S.seq
    /\ f2s' = RemoveAt(f2s, ChosenF2SIndex)
    /\ UNCHANGED <<fState, workerSlot, readySeen, usecsDelivered,
                    claimMade, claimExact, claimedWire, claimedFull,
                    claimedToken, claimRejected, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, claimAfterRevokeQueued,
                    claimRejectedAfterFence, s2f, s2d, c2f,
                    nextF2SSeq, sfLive, sdLive>>

LoseSFSession ==
    /\ sfLive
    /\ LET live ==
             {a \in Assignments : schedulerReservation[a] \/ workerSlot[a]}
       IN /\ \A a \in live : terminalCount[a] = 0
          /\ phase' =
                 [a \in Assignments |->
                    IF a \in live THEN "Terminal" ELSE phase[a]]
          /\ fState' = [a \in Assignments |-> "None"]
          /\ schedulerReservation' =
                 [a \in Assignments |->
                    IF a \in live THEN FALSE ELSE schedulerReservation[a]]
          /\ workerSlot' = [a \in Assignments |-> FALSE]
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
    /\ c2f' = <<>>
    /\ UNCHANGED <<readySeen, usecsDelivered, claimMade, claimExact,
                    claimedWire, claimedFull, claimedToken, claimRejected,
                    startCount, startAfterRelease, revokedEnqueued,
                    revokedConsumed, beginConsumed, claimAfterRevokeQueued,
                    claimRejectedAfterFence, s2d, nextF2SSeq,
                    lastF2SConsumed, sdLive>>

ReconnectSF ==
    /\ ~sfLive
    /\ \A a \in Assignments : ~workerSlot[a] /\ fState[a] = "None"
    /\ sfLive' = TRUE
    /\ UNCHANGED <<phase, fState, schedulerReservation, workerSlot,
                    released, releaseCause, terminalCount, readySeen,
                    usecsDelivered, claimMade, claimExact, claimedWire,
                    claimedFull, claimedToken, claimRejected, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, claimAfterRevokeQueued,
                    claimRejectedAfterFence, s2f, f2s, s2d, c2f,
                    nextF2SSeq, lastF2SConsumed, sdLive>>

LoseSDSession ==
    /\ sdLive
    /\ sdLive' = FALSE
    /\ s2d' = <<>>
    /\ UNCHANGED <<phase, fState, schedulerReservation, workerSlot,
                    released, releaseCause, terminalCount, readySeen,
                    usecsDelivered, claimMade, claimExact, claimedWire,
                    claimedFull, claimedToken, claimRejected, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, claimAfterRevokeQueued,
                    claimRejectedAfterFence, s2f, f2s, c2f,
                    nextF2SSeq, lastF2SConsumed, sfLive>>

ReconnectSD ==
    /\ ~sdLive
    /\ sdLive' = TRUE
    /\ UNCHANGED <<phase, fState, schedulerReservation, workerSlot,
                    released, releaseCause, terminalCount, readySeen,
                    usecsDelivered, claimMade, claimExact, claimedWire,
                    claimedFull, claimedToken, claimRejected, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, claimAfterRevokeQueued,
                    claimRejectedAfterFence, s2f, f2s, s2d, c2f,
                    nextF2SSeq, lastF2SConsumed, sfLive>>

Next ==
    \/ \E a \in Assignments : SPrepare(a)
    \/ FReceivePrepare
    \/ SReceiveReady
    \/ \E a \in Assignments : SQueueUseCS(a)
    \/ DReceiveUseCS
    \/ FClaimExact
    \/ FClaimLegacy
    \/ FRejectClaimFenced
    \/ FRejectUnknownClaim
    \/ FDefaultAllowUnknownClaim
    \/ \E a \in Assignments : FStart(a)
    \/ SReceiveBegin
    \/ \E a \in Assignments : SQueueRevoke(a)
    \/ FReceiveRevokeReserved
    \/ FReceiveRevokeOwned
    \/ FReceiveRevokeAfterDone
    \/ SReceiveRevoked
    \/ SReceiveOwned
    \/ \E a \in Assignments : FComplete(a)
    \/ SReceiveDone
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
    /\ claimedWire \in [Assignments -> Nat]
    /\ claimedFull \in [Assignments -> Nat]
    /\ claimedToken \in [Assignments -> Nat]
    /\ claimRejected \in [Assignments -> Nat]
    /\ startCount \in [Assignments -> Nat]
    /\ startAfterRelease \in [Assignments -> Nat]
    /\ revokedEnqueued \in [Assignments -> BOOLEAN]
    /\ revokedConsumed \in [Assignments -> BOOLEAN]
    /\ beginConsumed \in [Assignments -> BOOLEAN]
    /\ claimAfterRevokeQueued \in BOOLEAN
    /\ claimRejectedAfterFence \in BOOLEAN
    /\ s2f \in Seq(MessageType)
    /\ f2s \in Seq(MessageType)
    /\ s2d \in Seq(MessageType)
    /\ c2f \in Seq(ClaimType)
    /\ nextF2SSeq \in Nat \ {0}
    /\ lastF2SConsumed \in Nat
    /\ sfLive \in BOOLEAN
    /\ sdLive \in BOOLEAN

BufferBound ==
    /\ Len(s2f) <= MaxS2F
    /\ Len(f2s) <= MaxF2S
    /\ Len(s2d) <= MaxS2D
    /\ Len(c2f) <= MaxC2F

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

StartHasCausalChain ==
    \A a \in Assignments :
        startCount[a] > 0
        => /\ readySeen[a]
           /\ usecsDelivered[a]
           /\ claimMade[a]

ClaimCorrelation ==
    \A a \in Assignments :
        claimMade[a]
        => /\ claimedWire[a] = WireOf[a]
           /\ claimExact[a]
              => /\ claimedFull[a] = FullIdOf[a]
                 /\ claimedToken[a] = TokenOf[a]
           /\ ~claimExact[a]
              => /\ claimedFull[a] = NoFullId
                 /\ claimedToken[a] = NoToken

ReleaseAfterRevokedConsume ==
    \A a \in Assignments :
        releaseCause[a] = "Revoked" => revokedConsumed[a]

TokenRequiredExactness ==
    \A a \in Assignments :
        PolicyOf[a] = TokenRequired /\ claimMade[a] => claimExact[a]

RevokedFenceHasNoStart ==
    \A a \in Assignments : fState[a] = "Revoked" => startCount[a] = 0

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
    /\ StartHasCausalChain
    /\ ClaimCorrelation
    /\ ReleaseAfterRevokedConsume
    /\ TokenRequiredExactness
    /\ RevokedFenceHasNoStart
    /\ NoStartAfterRelease
    /\ TerminalAtMostOnce
    /\ TerminalCoherence

(***************************************************************************
Intentional reachability-witness invariants.  Their dedicated configurations
expect counterexamples proving both real claim/revoke orders are present.
***************************************************************************)
NoClaimBeforeRevokeConsume == ~claimAfterRevokeQueued
NoRejectAfterFence == ~claimRejectedAfterFence

(***************************************************************************
Weak fairness is intentionally limited to live-link FIFO drain/processing.
SafetyInvariant is checked against Spec without fairness.  Connection loss is
an explicit alternative transition.
***************************************************************************)
DrainS2F ==
    FReceivePrepare
    \/ FReceiveRevokeReserved
    \/ FReceiveRevokeOwned
    \/ FReceiveRevokeAfterDone
DrainF2S ==
    SReceiveReady
    \/ SReceiveBegin
    \/ SReceiveRevoked
    \/ SReceiveOwned
    \/ SReceiveDone
DrainS2D == DReceiveUseCS
DrainC2F ==
    FClaimExact
    \/ FClaimLegacy
    \/ FRejectClaimFenced
    \/ FRejectUnknownClaim
    \/ FDefaultAllowUnknownClaim

FencedLivenessSpec ==
    /\ Spec
    /\ WF_vars(DrainS2F)
    /\ WF_vars(DrainF2S)
    /\ WF_vars(DrainS2D)
    /\ WF_vars(DrainC2F)

NoPermanentQueuedFrame ==
    /\ [](Len(s2f) > 0 /\ sfLive => <> (Len(s2f) = 0 \/ ~sfLive))
    /\ [](Len(f2s) > 0 /\ sfLive => <> (Len(f2s) = 0 \/ ~sfLive))
    /\ [](Len(s2d) > 0 /\ sdLive => <> (Len(s2d) = 0 \/ ~sdLive))
    /\ [](Len(c2f) > 0 /\ sfLive => <> (Len(c2f) = 0 \/ ~sfLive))

FirstWorker == CHOOSE w \in Workers : TRUE
OtherWorker == CHOOSE w \in Workers : w # FirstWorker
MCCapacity ==
    [w \in Workers |-> IF w = FirstWorker THEN 1 ELSE 2]
MCWorkerOf ==
    [a \in Assignments |-> IF a = "a0" THEN FirstWorker ELSE OtherWorker]
MCPolicyOf ==
    [a \in Assignments |-> IF a = "a0" THEN TokenRequired ELSE LegacyId]
MCWireOf ==
    [a \in Assignments |-> IF a = "a0" THEN 1 ELSE 2]
MCFullIdOf ==
    [a \in Assignments |-> IF a = "a0" THEN 101 ELSE 102]
MCTokenOf ==
    [a \in Assignments |-> IF a = "a0" THEN 201 ELSE 202]

=============================================================================
