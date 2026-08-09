----------------------- MODULE AssignmentFenceCoreProof -----------------------
(***************************************************************************
Small TLAPS proof core for the fixed abstract assignment fence.

TLC remains responsible for the finite capacity cutoffs.  This proof instead
covers the unbounded logical ownership/identity facts that must not depend on a
particular model size:

  * schedulerReservation and workerSlot are distinct phase-derived tokens;
  * release and the one-hot terminal token agree with Terminal;
  * every claim has prior preparation evidence;
  * token-required claims retain exact correlation evidence;
  * Revoked is exclusive with accepted-claim evidence; and
  * two simultaneously live scheduler identities cannot share FullIdOf.

The inductive strengthening is deliberately smaller than CoreInvariant.  The
previous proof asked automatic backends to rediscover finite-cardinality
lemmas inside every action obligation; the first real TLAPS run proved 83 of 96
obligations and exposed that structure as the remaining failure.  CapacityBound
continues to be checked by both pinned TLC toolchains and is not relabeled as a
TLAPS theorem.
***************************************************************************)
EXTENDS AssignmentFenceCore, TLAPS

FixedMode ==
    /\ ~MutantLegacyCanClaimToken
    /\ ~MutantReleaseClaimed

PreparedEverExact ==
    \A a \in Assignments : preparedEver[a] <=> phase[a] # "Absent"

ReleasedExact ==
    \A a \in Assignments : released[a] <=> phase[a] = "Terminal"

TerminalCountExact ==
    \A a \in Assignments :
        terminalCount[a] = IF phase[a] = "Terminal" THEN 1 ELSE 0

ClaimEvidenceShape ==
    \A a \in Assignments :
        /\ phase[a] \in {"Absent", "Prepared", "Revoked"}
              => claimant[a] = NoClaimant /\ ~exactClaim[a]
        /\ phase[a] \in {"Claimed", "Started"}
              => claimant[a] # NoClaimant
        /\ exactClaim[a] => claimant[a] # NoClaimant

TokenClaimExact ==
    \A a \in Assignments :
        PolicyOf[a] = TokenRequired /\ claimant[a] # NoClaimant
        => exactClaim[a]

OwnershipInvariant ==
    /\ WorkerSlotCoherence
    /\ SchedulerReservationCoherence
    /\ PreparedEverExact
    /\ ReleasedExact
    /\ TerminalCountExact
    /\ ClaimEvidenceShape
    /\ TokenClaimExact
    /\ LiveFullIdUnique

ProofSafety ==
    /\ ReleaseSafety
    /\ WorkerSlotCoherence
    /\ SchedulerReservationCoherence
    /\ PreparedBeforeClaim
    /\ TokenRequiredExactness
    /\ ClaimRevokeExclusive
    /\ TerminalAtMostOnce
    /\ LiveFullIdUnique

LEMMA OwnershipImpliesSafety ==
    OwnershipInvariant => ProofSafety
BY SMT DEF OwnershipInvariant, ProofSafety, ReleaseSafety,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, PreparedBeforeClaim,
           TokenRequiredExactness, ClaimRevokeExclusive,
           TerminalAtMostOnce, ClaimEvidencePhases

LEMMA InitEstablishesOwnership ==
    ASSUME FixedMode, Init
    PROVE  OwnershipInvariant
BY SMT DEF FixedMode, Init, OwnershipInvariant,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, LiveFullIdUnique,
           LiveWorkerPhases, LiveSchedulerPhases

(***************************************************************************
Prepare is the only action that introduces a new live scheduler identity.  Its
explicit guard is exactly the uniqueness premise needed here.
***************************************************************************)
LEMMA PreparePreservesFullIdUniqueness ==
    ASSUME FixedMode,
           OwnershipInvariant,
           NEW a \in Assignments,
           Prepare(a)
    PROVE  LiveFullIdUnique'
BY SMT DEF FixedMode, OwnershipInvariant, Prepare,
           LiveFullIdUnique, SchedulerReservationCoherence,
           LiveSchedulerPhases

LEMMA PreparePreservesOwnership ==
    ASSUME FixedMode,
           OwnershipInvariant,
           NEW a \in Assignments,
           Prepare(a)
    PROVE  OwnershipInvariant'
BY PreparePreservesFullIdUniqueness,
   SMT DEF FixedMode, OwnershipInvariant, Prepare,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact,
           LiveWorkerPhases, LiveSchedulerPhases

LEMMA ClaimLegacyPreservesOwnership ==
    ASSUME FixedMode,
           OwnershipInvariant,
           NEW current \in Assignments,
           NEW arriving \in Assignments,
           ClaimLegacy(current, arriving)
    PROVE  OwnershipInvariant'
BY SMT DEF FixedMode, OwnershipInvariant, ClaimLegacy,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, LiveFullIdUnique,
           LiveWorkerPhases, LiveSchedulerPhases

LEMMA ClaimTokenPreservesOwnership ==
    ASSUME FixedMode,
           OwnershipInvariant,
           NEW current \in Assignments,
           NEW arriving \in Assignments,
           ClaimToken(current, arriving)
    PROVE  OwnershipInvariant'
BY SMT DEF FixedMode, OwnershipInvariant, ClaimToken,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, LiveFullIdUnique,
           LiveWorkerPhases, LiveSchedulerPhases

LEMMA StartPreservesOwnership ==
    ASSUME FixedMode,
           OwnershipInvariant,
           NEW a \in Assignments,
           Start(a)
    PROVE  OwnershipInvariant'
BY SMT DEF FixedMode, OwnershipInvariant, Start,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, LiveFullIdUnique,
           LiveWorkerPhases, LiveSchedulerPhases

LEMMA RevokePreservesOwnership ==
    ASSUME FixedMode,
           OwnershipInvariant,
           NEW a \in Assignments,
           Revoke(a)
    PROVE  OwnershipInvariant'
BY SMT DEF FixedMode, OwnershipInvariant, Revoke,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, LiveFullIdUnique,
           LiveWorkerPhases, LiveSchedulerPhases

LEMMA ConsumeRevokedPreservesOwnership ==
    ASSUME FixedMode,
           OwnershipInvariant,
           NEW a \in Assignments,
           ConsumeRevoked(a)
    PROVE  OwnershipInvariant'
BY SMT DEF FixedMode, OwnershipInvariant, ConsumeRevoked,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, LiveFullIdUnique,
           LiveWorkerPhases, LiveSchedulerPhases

LEMMA CompletePreservesOwnership ==
    ASSUME FixedMode,
           OwnershipInvariant,
           NEW a \in Assignments,
           Complete(a)
    PROVE  OwnershipInvariant'
BY SMT DEF FixedMode, OwnershipInvariant, Complete,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, LiveFullIdUnique,
           LiveWorkerPhases, LiveSchedulerPhases, ClaimedPhases

LEMMA WorkerLostPreservesOwnership ==
    ASSUME FixedMode,
           OwnershipInvariant,
           NEW a \in Assignments,
           WorkerLost(a)
    PROVE  OwnershipInvariant'
BY SMT DEF FixedMode, OwnershipInvariant, WorkerLost,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, LiveFullIdUnique,
           LiveWorkerPhases, LiveSchedulerPhases

LEMMA NextPreservesOwnership ==
    ASSUME FixedMode, OwnershipInvariant, Next
    PROVE  OwnershipInvariant'
<1>1. CASE \E a \in Assignments : Prepare(a)
  <2>1. PICK a \in Assignments : Prepare(a) BY <1>1
  <2>2. QED BY PreparePreservesOwnership, <2>1
<1>2. CASE \E c \in Assignments, a \in Assignments : ClaimLegacy(c, a)
  <2>1. PICK c \in Assignments, a \in Assignments : ClaimLegacy(c, a)
    BY <1>2
  <2>2. QED BY ClaimLegacyPreservesOwnership, <2>1
<1>3. CASE \E c \in Assignments, a \in Assignments : ClaimToken(c, a)
  <2>1. PICK c \in Assignments, a \in Assignments : ClaimToken(c, a)
    BY <1>3
  <2>2. QED BY ClaimTokenPreservesOwnership, <2>1
<1>4. CASE \E a \in Assignments : Start(a)
  <2>1. PICK a \in Assignments : Start(a) BY <1>4
  <2>2. QED BY StartPreservesOwnership, <2>1
<1>5. CASE \E a \in Assignments : Revoke(a)
  <2>1. PICK a \in Assignments : Revoke(a) BY <1>5
  <2>2. QED BY RevokePreservesOwnership, <2>1
<1>6. CASE \E a \in Assignments : ConsumeRevoked(a)
  <2>1. PICK a \in Assignments : ConsumeRevoked(a) BY <1>6
  <2>2. QED BY ConsumeRevokedPreservesOwnership, <2>1
<1>7. CASE \E a \in Assignments : Complete(a)
  <2>1. PICK a \in Assignments : Complete(a) BY <1>7
  <2>2. QED BY CompletePreservesOwnership, <2>1
<1>8. CASE \E a \in Assignments : WorkerLost(a)
  <2>1. PICK a \in Assignments : WorkerLost(a) BY <1>8
  <2>2. QED BY WorkerLostPreservesOwnership, <2>1
<1>9. QED BY <1>1, <1>2, <1>3, <1>4, <1>5, <1>6, <1>7, <1>8
              DEF Next

LEMMA StutteringPreservesOwnership ==
    ASSUME OwnershipInvariant, UNCHANGED vars
    PROVE  OwnershipInvariant'
BY SMT DEF OwnershipInvariant, vars

THEOREM FixedCoreOwnershipSafety ==
    ASSUME FixedMode
    PROVE  Spec => []ProofSafety
<1>1. Init => OwnershipInvariant
  BY InitEstablishesOwnership
<1>2. OwnershipInvariant /\ [Next]_vars => OwnershipInvariant'
  <2>1. CASE Next
    BY NextPreservesOwnership
  <2>2. CASE UNCHANGED vars
    BY StutteringPreservesOwnership
  <2>3. QED BY <2>1, <2>2
<1>3. OwnershipInvariant => ProofSafety
  BY OwnershipImpliesSafety
<1>4. QED BY <1>1, <1>2, <1>3, PTL DEF Spec

=============================================================================
