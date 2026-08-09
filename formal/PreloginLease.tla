---------------------------- MODULE PreloginLease ----------------------------
(***************************************************************************
Bounded accepted-but-not-logged-in scheduler connections.

The accept loop is modeled in microsteps so the no-quantum mutant can keep the
loop in its accept phase while a control request remains pending.  Time is a
small saturating logical clock for exhaustive TLC runs; the implementation maps
it to absolute monotonic milliseconds.

Cumulative counters saturate at CountCap only to keep liveness model checking
finite; implementation counters remain wide monotonic telemetry.

Mutants are independent:
  MutantNoCap          admits beyond L;
  MutantNoQuantum      keeps accepting after Q in one scheduler turn;
  MutantNoLease        never expires an incomplete handshake;
  MutantRefreshLease   refreshes the whole-handshake deadline on fragments.
***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Peers, L, Q, LeaseTicks, MaxTime, CountCap,
          MutantNoCap, MutantNoQuantum, MutantNoLease, MutantRefreshLease

ASSUME /\ IsFiniteSet(Peers)
       /\ Peers # {}
       /\ L \in Nat \ {0}
       /\ Q \in Nat \ {0}
       /\ LeaseTicks \in Nat \ {0}
       /\ MaxTime >= LeaseTicks + 1
       /\ CountCap \in Nat \ {0}
       /\ MutantNoCap \in BOOLEAN
       /\ MutantNoQuantum \in BOOLEAN
       /\ MutantNoLease \in BOOLEAN
       /\ MutantRefreshLease \in BOOLEAN

ConnStates == {"Outside", "Pending", "Prelogin", "Logged", "Closed", "Rejected"}
LoopPhases == {"Accept", "Service"}

Min2(a, b) == IF a <= b THEN a ELSE b
CapAdd(value, delta) == Min2(CountCap, value + delta)

VARIABLES connState,
          acceptedAt,
          deadline,
          now,
          loopPhase,
          acceptedThisTurn,
          controlPending,
          controlDone,
          socketAcceptedTotal,
          preloginAdmittedTotal,
          preloginCompletedTotal,
          preloginExpiredTotal,
          preloginPeerClosedTotal,
          preloginRejectedTotal

vars ==
    <<connState, acceptedAt, deadline, now, loopPhase, acceptedThisTurn,
      controlPending, controlDone, socketAcceptedTotal, preloginAdmittedTotal,
      preloginCompletedTotal, preloginExpiredTotal, preloginPeerClosedTotal,
      preloginRejectedTotal>>

PreloginSet == {p \in Peers : connState[p] = "Prelogin"}
PendingSet == {p \in Peers : connState[p] = "Pending"}

Init ==
    /\ connState = [p \in Peers |-> "Outside"]
    /\ acceptedAt = [p \in Peers |-> 0]
    /\ deadline = [p \in Peers |-> 0]
    /\ now = 0
    /\ loopPhase = "Accept"
    /\ acceptedThisTurn = 0
    /\ controlPending = TRUE
    /\ controlDone = FALSE
    /\ socketAcceptedTotal = 0
    /\ preloginAdmittedTotal = 0
    /\ preloginCompletedTotal = 0
    /\ preloginExpiredTotal = 0
    /\ preloginPeerClosedTotal = 0
    /\ preloginRejectedTotal = 0

Arrive(p) ==
    /\ p \in Peers
    /\ connState[p] \in {"Outside", "Logged", "Closed", "Rejected"}
    /\ connState' = [connState EXCEPT ![p] = "Pending"]
    /\ UNCHANGED <<acceptedAt, deadline, now, loopPhase, acceptedThisTurn,
                    controlPending, controlDone, socketAcceptedTotal,
                    preloginAdmittedTotal, preloginCompletedTotal,
                    preloginExpiredTotal, preloginPeerClosedTotal,
                    preloginRejectedTotal>>

CanAccept ==
    /\ loopPhase = "Accept"
    /\ PendingSet # {}
    /\ (MutantNoQuantum \/ acceptedThisTurn < Q)

Accept(p) ==
    /\ p \in Peers
    /\ connState[p] = "Pending"
    /\ CanAccept
    /\ LET admit == MutantNoCap \/ Cardinality(PreloginSet) < L
       IN /\ connState' =
                [connState EXCEPT ![p] = IF admit THEN "Prelogin" ELSE "Rejected"]
          /\ acceptedAt' =
                IF admit THEN [acceptedAt EXCEPT ![p] = now] ELSE acceptedAt
          /\ deadline' =
                IF admit
                THEN [deadline EXCEPT ![p] = Min2(MaxTime, now + LeaseTicks)]
                ELSE deadline
          /\ preloginAdmittedTotal' =
                CapAdd(preloginAdmittedTotal, IF admit THEN 1 ELSE 0)
          /\ preloginRejectedTotal' =
                CapAdd(preloginRejectedTotal, IF admit THEN 0 ELSE 1)
    /\ socketAcceptedTotal' = CapAdd(socketAcceptedTotal, 1)
    /\ acceptedThisTurn' = Min2(Q + 1, acceptedThisTurn + 1)
    /\ UNCHANGED <<now, loopPhase, controlPending, controlDone,
                    preloginCompletedTotal, preloginExpiredTotal,
                    preloginPeerClosedTotal>>

AcceptStep == \E p \in Peers : Accept(p)

FinishAccept ==
    /\ loopPhase = "Accept"
    /\ ~CanAccept
    /\ loopPhase' = "Service"
    /\ UNCHANGED <<connState, acceptedAt, deadline, now, acceptedThisTurn,
                    controlPending, controlDone, socketAcceptedTotal,
                    preloginAdmittedTotal, preloginCompletedTotal,
                    preloginExpiredTotal, preloginPeerClosedTotal,
                    preloginRejectedTotal>>

ServiceControl ==
    /\ loopPhase = "Service"
    /\ loopPhase' = "Accept"
    /\ acceptedThisTurn' = 0
    /\ controlDone' = (controlDone \/ controlPending)
    /\ UNCHANGED <<connState, acceptedAt, deadline, now, controlPending,
                    socketAcceptedTotal, preloginAdmittedTotal,
                    preloginCompletedTotal, preloginExpiredTotal,
                    preloginPeerClosedTotal, preloginRejectedTotal>>

Login(p) ==
    /\ p \in Peers
    /\ connState[p] = "Prelogin"
    /\ connState' = [connState EXCEPT ![p] = "Logged"]
    /\ preloginCompletedTotal' = CapAdd(preloginCompletedTotal, 1)
    /\ UNCHANGED <<acceptedAt, deadline, now, loopPhase, acceptedThisTurn,
                    controlPending, controlDone, socketAcceptedTotal,
                    preloginAdmittedTotal, preloginExpiredTotal,
                    preloginPeerClosedTotal, preloginRejectedTotal>>

PeerClose(p) ==
    /\ p \in Peers
    /\ connState[p] = "Prelogin"
    /\ connState' = [connState EXCEPT ![p] = "Closed"]
    /\ preloginPeerClosedTotal' = CapAdd(preloginPeerClosedTotal, 1)
    /\ UNCHANGED <<acceptedAt, deadline, now, loopPhase, acceptedThisTurn,
                    controlPending, controlDone, socketAcceptedTotal,
                    preloginAdmittedTotal, preloginCompletedTotal,
                    preloginExpiredTotal, preloginRejectedTotal>>

Fragment(p) ==
    /\ p \in Peers
    /\ connState[p] = "Prelogin"
    /\ deadline' =
          IF MutantRefreshLease
          THEN [deadline EXCEPT ![p] = Min2(MaxTime, now + LeaseTicks)]
          ELSE deadline
    /\ UNCHANGED <<connState, acceptedAt, now, loopPhase, acceptedThisTurn,
                    controlPending, controlDone, socketAcceptedTotal,
                    preloginAdmittedTotal, preloginCompletedTotal,
                    preloginExpiredTotal, preloginPeerClosedTotal,
                    preloginRejectedTotal>>

Tick ==
    LET next == Min2(MaxTime, now + 1)
        victims ==
          IF MutantNoLease
          THEN {}
          ELSE {p \in Peers :
                  connState[p] = "Prelogin" /\ deadline[p] <= next}
    IN /\ now' = next
       /\ connState' =
            [p \in Peers |-> IF p \in victims THEN "Closed" ELSE connState[p]]
       /\ preloginExpiredTotal' =
            CapAdd(preloginExpiredTotal, Cardinality(victims))
       /\ UNCHANGED <<acceptedAt, deadline, loopPhase, acceptedThisTurn,
                       controlPending, controlDone, socketAcceptedTotal,
                       preloginAdmittedTotal, preloginCompletedTotal,
                       preloginPeerClosedTotal, preloginRejectedTotal>>

Next ==
    \/ \E p \in Peers : Arrive(p)
    \/ AcceptStep
    \/ FinishAccept
    \/ ServiceControl
    \/ \E p \in Peers : Login(p)
    \/ \E p \in Peers : PeerClose(p)
    \/ \E p \in Peers : Fragment(p)
    \/ Tick

Spec == Init /\ [][Next]_vars

FairSpec ==
    /\ Spec
    /\ WF_vars(AcceptStep)
    /\ WF_vars(FinishAccept)
    /\ WF_vars(ServiceControl)

TypeOK ==
    /\ connState \in [Peers -> ConnStates]
    /\ acceptedAt \in [Peers -> 0..MaxTime]
    /\ deadline \in [Peers -> 0..MaxTime]
    /\ now \in 0..MaxTime
    /\ loopPhase \in LoopPhases
    /\ acceptedThisTurn \in 0..(Q + 1)
    /\ controlPending \in BOOLEAN
    /\ controlDone \in BOOLEAN
    /\ socketAcceptedTotal \in 0..CountCap
    /\ preloginAdmittedTotal \in 0..CountCap
    /\ preloginCompletedTotal \in 0..CountCap
    /\ preloginExpiredTotal \in 0..CountCap
    /\ preloginPeerClosedTotal \in 0..CountCap
    /\ preloginRejectedTotal \in 0..CountCap

PreloginBound ==
    Cardinality(PreloginSet) <= L

AcceptQuantum ==
    acceptedThisTurn <= Q

WholeHandshakeLease ==
    \A p \in Peers :
        connState[p] = "Prelogin"
        => now < acceptedAt[p] + LeaseTicks

AccountingConservation ==
    /\ socketAcceptedTotal =
          Min2(CountCap, preloginAdmittedTotal + preloginRejectedTotal)
    /\ preloginAdmittedTotal =
          Min2(CountCap,
               preloginCompletedTotal
               + preloginExpiredTotal
               + preloginPeerClosedTotal
               + Cardinality(PreloginSet))

SafetyInvariant ==
    /\ TypeOK
    /\ PreloginBound
    /\ AcceptQuantum
    /\ WholeHandshakeLease
    /\ AccountingConservation

SafetyWithoutQuantum ==
    /\ TypeOK
    /\ PreloginBound
    /\ WholeHandshakeLease
    /\ AccountingConservation

ControlProgress == <> controlDone

=============================================================================
