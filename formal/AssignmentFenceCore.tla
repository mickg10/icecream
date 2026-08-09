--------------------------- MODULE AssignmentFenceCore ---------------------------
(***************************************************************************
Abstract worker-side assignment ownership for Icecream protocol 49/50.

This core intentionally has no sockets. It separates two conserved tokens:

  schedulerReservation[a]  S still accounts for assignment a and may not
                           reuse its logical reservation;
  workerSlot[a]            F still consumes one physical compile slot for a.

After F linearizes REVOKED, workerSlot is free but schedulerReservation remains
until S consumes the REVOKED result. Collapsing those tokens makes the revoke
transition impossible to state accurately.

MutantLegacyCanClaimToken and MutantReleaseClaimed are model switches used by
negative configurations. They must be FALSE in the fixed specification.
***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Assignments, Workers, NoWorker, NoClaimant,
          LegacyId, TokenRequired,
          Capacity, PolicyOf, WorkerOf, WireOf, FullIdOf, TokenOf,
          MutantLegacyCanClaimToken, MutantReleaseClaimed

ASSUME /\ Assignments # {}
       /\ Workers # {}
       /\ NoWorker \notin Workers
       /\ NoClaimant \notin Assignments
       /\ LegacyId # TokenRequired
       /\ Capacity \in [Workers -> (Nat \ {0})]
       /\ PolicyOf \in [Assignments -> {LegacyId, TokenRequired}]
       /\ WorkerOf \in [Assignments -> Workers]
       /\ WireOf \in [Assignments -> Nat]
       /\ FullIdOf \in [Assignments -> Nat]
       /\ TokenOf \in [Assignments -> Nat]
       /\ MutantLegacyCanClaimToken \in BOOLEAN
       /\ MutantReleaseClaimed \in BOOLEAN

Phases == {"Absent", "Prepared", "Claimed", "Started", "Revoked", "Terminal"}
LiveWorkerPhases == {"Prepared", "Claimed", "Started"}
LiveSchedulerPhases == LiveWorkerPhases \cup {"Revoked"}
ClaimedPhases == {"Claimed", "Started"}
ClaimEvidencePhases == {"Claimed", "Started", "Terminal"}

VARIABLES phase,
          claimant,
          exactClaim,
          schedulerReservation,
          workerSlot,
          released,
          terminalCount,
          preparedEver

vars == <<phase, claimant, exactClaim, schedulerReservation, workerSlot,
          released, terminalCount, preparedEver>>

TypeOK ==
    /\ phase \in [Assignments -> Phases]
    /\ claimant \in [Assignments -> (Assignments \cup {NoClaimant})]
    /\ exactClaim \in [Assignments -> BOOLEAN]
    /\ schedulerReservation \in [Assignments -> BOOLEAN]
    /\ workerSlot \in [Assignments -> BOOLEAN]
    /\ released \in [Assignments -> BOOLEAN]
    /\ terminalCount \in [Assignments -> Nat]
    /\ preparedEver \in [Assignments -> BOOLEAN]

Init ==
    /\ phase = [a \in Assignments |-> "Absent"]
    /\ claimant = [a \in Assignments |-> NoClaimant]
    /\ exactClaim = [a \in Assignments |-> FALSE]
    /\ schedulerReservation = [a \in Assignments |-> FALSE]
    /\ workerSlot = [a \in Assignments |-> FALSE]
    /\ released = [a \in Assignments |-> FALSE]
    /\ terminalCount = [a \in Assignments |-> 0]
    /\ preparedEver = [a \in Assignments |-> FALSE]

WorkerOccupancy(w) == Cardinality({a \in Assignments : workerSlot[a] /\ WorkerOf[a] = w})

Prepare(a) ==
    /\ a \in Assignments
    /\ phase[a] = "Absent"
    /\ WorkerOccupancy(WorkerOf[a]) < Capacity[WorkerOf[a]]
    /\ \A b \in Assignments :
          b # a /\ schedulerReservation[b] => FullIdOf[b] # FullIdOf[a]
    /\ phase' = [phase EXCEPT ![a] = "Prepared"]
    /\ schedulerReservation' = [schedulerReservation EXCEPT ![a] = TRUE]
    /\ workerSlot' = [workerSlot EXCEPT ![a] = TRUE]
    /\ preparedEver' = [preparedEver EXCEPT ![a] = TRUE]
    /\ UNCHANGED <<claimant, exactClaim, released, terminalCount>>

ClaimLegacy(current, arriving) ==
    /\ current \in Assignments
    /\ arriving \in Assignments
    /\ phase[current] = "Prepared"
    /\ ~released[current]
    /\ WireOf[current] = WireOf[arriving]
    /\ (PolicyOf[current] = LegacyId \/ MutantLegacyCanClaimToken)
    /\ phase' = [phase EXCEPT ![current] = "Claimed"]
    /\ claimant' = [claimant EXCEPT ![current] = arriving]
    /\ exactClaim' = [exactClaim EXCEPT
          ![current] = (FullIdOf[current] = FullIdOf[arriving]
                        /\ TokenOf[current] = TokenOf[arriving])]
    /\ UNCHANGED <<schedulerReservation, workerSlot, released,
                    terminalCount, preparedEver>>

ClaimToken(current, arriving) ==
    /\ current \in Assignments
    /\ arriving \in Assignments
    /\ phase[current] = "Prepared"
    /\ ~released[current]
    /\ PolicyOf[current] = TokenRequired
    /\ WireOf[current] = WireOf[arriving]
    /\ FullIdOf[current] = FullIdOf[arriving]
    /\ TokenOf[current] = TokenOf[arriving]
    /\ phase' = [phase EXCEPT ![current] = "Claimed"]
    /\ claimant' = [claimant EXCEPT ![current] = arriving]
    /\ exactClaim' = [exactClaim EXCEPT ![current] = TRUE]
    /\ UNCHANGED <<schedulerReservation, workerSlot, released,
                    terminalCount, preparedEver>>

Start(a) ==
    /\ a \in Assignments
    /\ phase[a] = "Claimed"
    /\ ~released[a]
    /\ phase' = [phase EXCEPT ![a] = "Started"]
    /\ UNCHANGED <<claimant, exactClaim, schedulerReservation, workerSlot,
                    released, terminalCount, preparedEver>>

Revoke(a) ==
    /\ a \in Assignments
    /\ phase[a] = "Prepared"
    /\ phase' = [phase EXCEPT ![a] = "Revoked"]
    /\ workerSlot' = [workerSlot EXCEPT ![a] = FALSE]
    /\ UNCHANGED <<claimant, exactClaim, schedulerReservation, released,
                    terminalCount, preparedEver>>

ConsumeRevoked(a) ==
    /\ a \in Assignments
    /\ (phase[a] = "Revoked"
        \/ (MutantReleaseClaimed /\ phase[a] = "Claimed"))
    /\ phase' = [phase EXCEPT
          ![a] = IF phase[a] = "Revoked" THEN "Terminal" ELSE phase[a]]
    /\ schedulerReservation' = [schedulerReservation EXCEPT ![a] = FALSE]
    /\ workerSlot' = [workerSlot EXCEPT ![a] = FALSE]
    /\ released' = [released EXCEPT ![a] = TRUE]
    /\ terminalCount' = [terminalCount EXCEPT ![a] = @ + 1]
    /\ UNCHANGED <<claimant, exactClaim, preparedEver>>

Complete(a) ==
    /\ a \in Assignments
    /\ phase[a] \in ClaimedPhases
    /\ phase' = [phase EXCEPT ![a] = "Terminal"]
    /\ schedulerReservation' = [schedulerReservation EXCEPT ![a] = FALSE]
    /\ workerSlot' = [workerSlot EXCEPT ![a] = FALSE]
    /\ released' = [released EXCEPT ![a] = TRUE]
    /\ terminalCount' = [terminalCount EXCEPT ![a] = @ + 1]
    /\ UNCHANGED <<claimant, exactClaim, preparedEver>>

WorkerLost(a) ==
    /\ a \in Assignments
    /\ phase[a] \in ClaimedPhases
    /\ phase' = [phase EXCEPT ![a] = "Terminal"]
    /\ schedulerReservation' = [schedulerReservation EXCEPT ![a] = FALSE]
    /\ workerSlot' = [workerSlot EXCEPT ![a] = FALSE]
    /\ released' = [released EXCEPT ![a] = TRUE]
    /\ terminalCount' = [terminalCount EXCEPT ![a] = @ + 1]
    /\ UNCHANGED <<claimant, exactClaim, preparedEver>>

Next ==
    \/ \E a \in Assignments : Prepare(a)
    \/ \E c \in Assignments, a \in Assignments : ClaimLegacy(c, a)
    \/ \E c \in Assignments, a \in Assignments : ClaimToken(c, a)
    \/ \E a \in Assignments : Start(a)
    \/ \E a \in Assignments : Revoke(a)
    \/ \E a \in Assignments : ConsumeRevoked(a)
    \/ \E a \in Assignments : Complete(a)
    \/ \E a \in Assignments : WorkerLost(a)

ReleaseSafety ==
    \A a \in Assignments :
        released[a] => phase[a] \in {"Revoked", "Terminal"}

WorkerSlotCoherence ==
    \A a \in Assignments : workerSlot[a] <=> phase[a] \in LiveWorkerPhases

SchedulerReservationCoherence ==
    \A a \in Assignments :
        schedulerReservation[a] <=> phase[a] \in LiveSchedulerPhases

PreparedBeforeClaim ==
    \A a \in Assignments :
        phase[a] \in ClaimEvidencePhases /\ claimant[a] # NoClaimant
        => preparedEver[a]

TokenRequiredExactness ==
    \A a \in Assignments :
        PolicyOf[a] = TokenRequired
        /\ phase[a] \in ClaimEvidencePhases
        /\ claimant[a] # NoClaimant
        => exactClaim[a]

ClaimRevokeExclusive ==
    \A a \in Assignments : phase[a] = "Revoked" => claimant[a] = NoClaimant

CapacityBound ==
    \A w \in Workers : WorkerOccupancy(w) <= Capacity[w]

TerminalAtMostOnce ==
    \A a \in Assignments : terminalCount[a] <= 1

LiveFullIdUnique ==
    \A a, b \in Assignments :
        a # b /\ phase[a] \in LiveSchedulerPhases /\ phase[b] \in LiveSchedulerPhases
        => FullIdOf[a] # FullIdOf[b]

CoreInvariant ==
    /\ TypeOK
    /\ ReleaseSafety
    /\ WorkerSlotCoherence
    /\ SchedulerReservationCoherence
    /\ PreparedBeforeClaim
    /\ TokenRequiredExactness
    /\ ClaimRevokeExclusive
    /\ CapacityBound
    /\ TerminalAtMostOnce
    /\ LiveFullIdUnique

Spec == Init /\ [][Next]_vars

SettlementStep(a) == Revoke(a) \/ ConsumeRevoked(a) \/ Complete(a) \/ WorkerLost(a)

FairSpec ==
    /\ Spec
    /\ \A a \in Assignments : WF_vars(SettlementStep(a))

EventualSettlement ==
    \A a \in Assignments :
        [](phase[a] \in LiveSchedulerPhases => <> (phase[a] = "Terminal"))

MCCapacity == [w \in Workers |-> 1]
MCPolicyOf == [a \in Assignments |-> IF a = "a0" THEN TokenRequired ELSE LegacyId]
MCWorkerOf == [a \in Assignments |-> CHOOSE w \in Workers : TRUE]
MCWireOf == [a \in Assignments |-> 7]
MCFullIdOf == [a \in Assignments |-> IF a = "a0" THEN 1001 ELSE 2002]
MCTokenOf == [a \in Assignments |-> IF a = "a0" THEN 111 ELSE 222]

=============================================================================
