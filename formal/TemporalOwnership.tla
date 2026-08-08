---------------------------- MODULE TemporalOwnership ----------------------------
(***************************************************************************
A compact TLA+ skeleton for the Icecream scheduler ownership protocol.

The executable Python model in this directory contains the concrete issue-2,
Hall-slack, observer, and identifier-ABA witnesses. This module isolates the
state machine and the safety/liveness boundary suitable for TLC/Apalache.
***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Jobs, Keys, Workers, Credit, WorkerCapacity, NoWorker, SupportsRevoke

ASSUME /\ Jobs # {}
       /\ Keys # {}
       /\ Workers # {}
       /\ NoWorker \notin Workers
       /\ Credit \in Nat \ {0}
       /\ WorkerCapacity \in Nat \ {0}
       /\ SupportsRevoke \in BOOLEAN

Locations == {"Absent", "Staged", "Queued", "Dispatched", "Started", "Terminal"}

VARIABLES loc, key, worker, debt, cancelled, tombstone

vars == <<loc, key, worker, debt, cancelled, tombstone>>

TypeOK ==
    /\ loc \in [Jobs -> Locations]
    /\ key \in [Jobs -> Keys]
    /\ worker \in [Jobs -> (Workers \cup {NoWorker})]
    /\ debt \in [Jobs -> BOOLEAN]
    /\ cancelled \subseteq Keys
    /\ tombstone \subseteq Jobs

Init ==
    /\ loc = [j \in Jobs |-> "Absent"]
    /\ key \in [Jobs -> Keys]
    /\ worker = [j \in Jobs |-> NoWorker]
    /\ debt = [j \in Jobs |-> FALSE]
    /\ cancelled = {}
    /\ tombstone = {}

Stage(j, k) ==
    /\ loc[j] = "Absent"
    /\ k \notin cancelled
    /\ loc' = [loc EXCEPT ![j] = "Staged"]
    /\ key' = [key EXCEPT ![j] = k]
    /\ UNCHANGED <<worker, debt, cancelled, tombstone>>

Activate(j) ==
    /\ loc[j] = "Staged"
    /\ key[j] \notin cancelled
    /\ loc' = [loc EXCEPT ![j] = "Queued"]
    /\ UNCHANGED <<key, worker, debt, cancelled, tombstone>>

Dispatch(j, f) ==
    /\ loc[j] = "Queued"
    /\ key[j] \notin cancelled
    /\ f \in Workers
    /\ Cardinality({x \in Jobs : debt[x]}) < Credit
    /\ Cardinality({x \in Jobs : worker[x] # NoWorker}) < WorkerCapacity
    /\ loc' = [loc EXCEPT ![j] = "Dispatched"]
    /\ worker' = [worker EXCEPT ![j] = f]
    /\ debt' = [debt EXCEPT ![j] = TRUE]
    /\ UNCHANGED <<key, cancelled, tombstone>>

Begin(j) ==
    /\ loc[j] = "Dispatched"
    /\ loc' = [loc EXCEPT ![j] = "Started"]
    /\ debt' = [debt EXCEPT ![j] = FALSE]
    /\ UNCHANGED <<key, worker, cancelled, tombstone>>

Done(j) ==
    /\ loc[j] \in {"Dispatched", "Started"}
    /\ loc' = [loc EXCEPT ![j] = "Terminal"]
    /\ worker' = [worker EXCEPT ![j] = NoWorker]
    /\ debt' = [debt EXCEPT ![j] = FALSE]
    /\ UNCHANGED <<key, cancelled, tombstone>>

Cancel(k) ==
    /\ k \in Keys
    /\ cancelled' = cancelled \cup {k}
    /\ loc' = [j \in Jobs |->
          IF key[j] = k /\ loc[j] \in {"Staged", "Queued"}
          THEN "Terminal"
          ELSE loc[j]]
    /\ UNCHANGED <<key, worker, debt, tombstone>>

CancelBeforeStart(j) ==
    /\ SupportsRevoke
    /\ loc[j] = "Dispatched"
    /\ loc' = [loc EXCEPT ![j] = "Terminal"]
    /\ worker' = [worker EXCEPT ![j] = NoWorker]
    /\ debt' = [debt EXCEPT ![j] = FALSE]
    /\ tombstone' = tombstone \cup {j}
    /\ UNCHANGED <<key, cancelled>>

LateArrivalRejected(j) ==
    /\ j \in tombstone
    /\ UNCHANGED vars

Next ==
    \/ \E j \in Jobs, k \in Keys : Stage(j, k)
    \/ \E j \in Jobs : Activate(j)
    \/ \E j \in Jobs, f \in Workers : Dispatch(j, f)
    \/ \E j \in Jobs : Begin(j)
    \/ \E j \in Jobs : Done(j)
    \/ \E k \in Keys : Cancel(k)
    \/ \E j \in Jobs : CancelBeforeStart(j)
    \/ \E j \in Jobs : LateArrivalRejected(j)

DebtCoherence ==
    \A j \in Jobs : debt[j] <=> loc[j] = "Dispatched"

ReservationCoherence ==
    \A j \in Jobs :
       (worker[j] # NoWorker) <=> loc[j] \in {"Dispatched", "Started"}

CreditBound ==
    Cardinality({j \in Jobs : debt[j]}) <= Credit

CapacityBound ==
    Cardinality({j \in Jobs : worker[j] # NoWorker}) <= WorkerCapacity

CancelCut ==
    \A j \in Jobs : key[j] \in cancelled => loc[j] \notin {"Staged", "Queued"}

TombstoneSafety ==
    \A j \in tombstone : loc[j] = "Terminal" /\ worker[j] = NoWorker

Safety == TypeOK /\ DebtCoherence /\ ReservationCoherence /\
          CreditBound /\ CapacityBound /\ CancelCut /\ TombstoneSafety

Spec == Init /\ [][Next]_vars

(***************************************************************************
This property is expected to fail in a legacy model that permits stuttering
forever in Dispatched. It becomes provable only with explicit fairness/
failure assumptions or with a fair CancelBeforeStart action on new F.
***************************************************************************)
EventualSettlement ==
    \A j \in Jobs : [](debt[j] => <>~debt[j])

=============================================================================
