----------------------- MODULE AssignmentFenceCoreProof -----------------------
(***************************************************************************
TLAPS proof candidate for the fixed abstract assignment-fence core.

This module proves an inductive invariant stronger than CoreInvariant.  The
strengthening makes the ownership conservation explicit:

  * preparedEver is exactly the complement of Absent;
  * released is exactly Terminal;
  * terminalCount is the one-hot terminal token;
  * workerSlot and schedulerReservation are phase-derived tokens;
  * claimant/exact-claim evidence has a phase-consistent shape;
  * every token-required claim is exact, even after its terminal transition.

The action lemmas deliberately name every core transition.  There are no
OMITTED proofs.  This file remains construction evidence until the local oracle
runs tlapm, reports its exact version/backends, and retains the successful
obligation summary.  If a backend needs a smaller finite-set lemma for
CapacityBound, refine that lemma rather than replacing the proof with a TLC
state-space result.
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

StrengthenedInvariant ==
    /\ TypeOK
    /\ WorkerSlotCoherence
    /\ SchedulerReservationCoherence
    /\ PreparedEverExact
    /\ ReleasedExact
    /\ TerminalCountExact
    /\ ClaimEvidenceShape
    /\ TokenClaimExact
    /\ CapacityBound
    /\ LiveFullIdUnique

LEMMA StrengthenedImpliesCore ==
    StrengthenedInvariant => CoreInvariant
BY SMT DEF StrengthenedInvariant, CoreInvariant, ReleaseSafety,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, PreparedBeforeClaim,
           TokenRequiredExactness, ClaimRevokeExclusive,
           TerminalAtMostOnce, ClaimEvidencePhases

LEMMA InitEstablishesStrengthened ==
    ASSUME FixedMode, Init
    PROVE  StrengthenedInvariant
BY Isa DEF FixedMode, Init, StrengthenedInvariant, TypeOK,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, CapacityBound,
           WorkerOccupancy, LiveFullIdUnique, LiveWorkerPhases,
           LiveSchedulerPhases

(***************************************************************************
Prepare is the only action that increases physical occupancy or introduces a
new live full identity.  Its capacity and uniqueness guards are therefore the
load-bearing premises of these two lemmas.
***************************************************************************)
LEMMA PreparePreservesCapacity ==
    ASSUME FixedMode,
           StrengthenedInvariant,
           NEW a \in Assignments,
           Prepare(a)
    PROVE  CapacityBound'
BY Isa DEF FixedMode, StrengthenedInvariant, Prepare, CapacityBound,
           WorkerOccupancy, WorkerSlotCoherence, LiveWorkerPhases

LEMMA PreparePreservesFullIdUniqueness ==
    ASSUME FixedMode,
           StrengthenedInvariant,
           NEW a \in Assignments,
           Prepare(a)
    PROVE  LiveFullIdUnique'
BY SMT DEF FixedMode, StrengthenedInvariant, Prepare,
           LiveFullIdUnique, SchedulerReservationCoherence,
           LiveSchedulerPhases

LEMMA PreparePreservesStrengthened ==
    ASSUME FixedMode,
           StrengthenedInvariant,
           NEW a \in Assignments,
           Prepare(a)
    PROVE  StrengthenedInvariant'
BY PreparePreservesCapacity, PreparePreservesFullIdUniqueness,
   SMT DEF FixedMode, StrengthenedInvariant, Prepare, TypeOK,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, LiveWorkerPhases,
           LiveSchedulerPhases

LEMMA ClaimLegacyPreservesStrengthened ==
    ASSUME FixedMode,
           StrengthenedInvariant,
           NEW current \in Assignments,
           NEW arriving \in Assignments,
           ClaimLegacy(current, arriving)
    PROVE  StrengthenedInvariant'
BY Isa DEF FixedMode, StrengthenedInvariant, ClaimLegacy, TypeOK,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, CapacityBound,
           WorkerOccupancy, LiveFullIdUnique, LiveWorkerPhases,
           LiveSchedulerPhases

LEMMA ClaimTokenPreservesStrengthened ==
    ASSUME FixedMode,
           StrengthenedInvariant,
           NEW current \in Assignments,
           NEW arriving \in Assignments,
           ClaimToken(current, arriving)
    PROVE  StrengthenedInvariant'
BY Isa DEF FixedMode, StrengthenedInvariant, ClaimToken, TypeOK,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, CapacityBound,
           WorkerOccupancy, LiveFullIdUnique, LiveWorkerPhases,
           LiveSchedulerPhases

LEMMA StartPreservesStrengthened ==
    ASSUME FixedMode,
           StrengthenedInvariant,
           NEW a \in Assignments,
           Start(a)
    PROVE  StrengthenedInvariant'
BY Isa DEF FixedMode, StrengthenedInvariant, Start, TypeOK,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, CapacityBound,
           WorkerOccupancy, LiveFullIdUnique, LiveWorkerPhases,
           LiveSchedulerPhases

LEMMA RevokePreservesStrengthened ==
    ASSUME FixedMode,
           StrengthenedInvariant,
           NEW a \in Assignments,
           Revoke(a)
    PROVE  StrengthenedInvariant'
BY Isa DEF FixedMode, StrengthenedInvariant, Revoke, TypeOK,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, CapacityBound,
           WorkerOccupancy, LiveFullIdUnique, LiveWorkerPhases,
           LiveSchedulerPhases

LEMMA ConsumeRevokedPreservesStrengthened ==
    ASSUME FixedMode,
           StrengthenedInvariant,
           NEW a \in Assignments,
           ConsumeRevoked(a)
    PROVE  StrengthenedInvariant'
BY Isa DEF FixedMode, StrengthenedInvariant, ConsumeRevoked, TypeOK,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, CapacityBound,
           WorkerOccupancy, LiveFullIdUnique, LiveWorkerPhases,
           LiveSchedulerPhases

LEMMA CompletePreservesStrengthened ==
    ASSUME FixedMode,
           StrengthenedInvariant,
           NEW a \in Assignments,
           Complete(a)
    PROVE  StrengthenedInvariant'
BY Isa DEF FixedMode, StrengthenedInvariant, Complete, TypeOK,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, CapacityBound,
           WorkerOccupancy, LiveFullIdUnique, LiveWorkerPhases,
           LiveSchedulerPhases, ClaimedPhases

LEMMA WorkerLostPreservesStrengthened ==
    ASSUME FixedMode,
           StrengthenedInvariant,
           NEW a \in Assignments,
           WorkerLost(a)
    PROVE  StrengthenedInvariant'
BY Isa DEF FixedMode, StrengthenedInvariant, WorkerLost, TypeOK,
           WorkerSlotCoherence, SchedulerReservationCoherence,
           PreparedEverExact, ReleasedExact, TerminalCountExact,
           ClaimEvidenceShape, TokenClaimExact, CapacityBound,
           WorkerOccupancy, LiveFullIdUnique, LiveWorkerPhases,
           LiveSchedulerPhases

LEMMA NextPreservesStrengthened ==
    ASSUME FixedMode, StrengthenedInvariant, Next
    PROVE  StrengthenedInvariant'
<1>1. CASE \E a \in Assignments : Prepare(a)
  <2>1. PICK a \in Assignments : Prepare(a) BY <1>1
  <2>2. QED BY PreparePreservesStrengthened, <2>1
<1>2. CASE \E c \in Assignments, a \in Assignments : ClaimLegacy(c, a)
  <2>1. PICK c \in Assignments, a \in Assignments : ClaimLegacy(c, a)
    BY <1>2
  <2>2. QED BY ClaimLegacyPreservesStrengthened, <2>1
<1>3. CASE \E c \in Assignments, a \in Assignments : ClaimToken(c, a)
  <2>1. PICK c \in Assignments, a \in Assignments : ClaimToken(c, a)
    BY <1>3
  <2>2. QED BY ClaimTokenPreservesStrengthened, <2>1
<1>4. CASE \E a \in Assignments : Start(a)
  <2>1. PICK a \in Assignments : Start(a) BY <1>4
  <2>2. QED BY StartPreservesStrengthened, <2>1
<1>5. CASE \E a \in Assignments : Revoke(a)
  <2>1. PICK a \in Assignments : Revoke(a) BY <1>5
  <2>2. QED BY RevokePreservesStrengthened, <2>1
<1>6. CASE \E a \in Assignments : ConsumeRevoked(a)
  <2>1. PICK a \in Assignments : ConsumeRevoked(a) BY <1>6
  <2>2. QED BY ConsumeRevokedPreservesStrengthened, <2>1
<1>7. CASE \E a \in Assignments : Complete(a)
  <2>1. PICK a \in Assignments : Complete(a) BY <1>7
  <2>2. QED BY CompletePreservesStrengthened, <2>1
<1>8. CASE \E a \in Assignments : WorkerLost(a)
  <2>1. PICK a \in Assignments : WorkerLost(a) BY <1>8
  <2>2. QED BY WorkerLostPreservesStrengthened, <2>1
<1>9. QED BY <1>1, <1>2, <1>3, <1>4, <1>5, <1>6, <1>7, <1>8
              DEF Next

LEMMA StutteringPreservesStrengthened ==
    ASSUME StrengthenedInvariant, UNCHANGED vars
    PROVE  StrengthenedInvariant'
BY SMT DEF StrengthenedInvariant, vars

THEOREM FixedCoreSafety ==
    ASSUME FixedMode
    PROVE  Spec => []CoreInvariant
<1>1. Init => StrengthenedInvariant
  BY InitEstablishesStrengthened
<1>2. StrengthenedInvariant /\ [Next]_vars => StrengthenedInvariant'
  <2>1. CASE Next
    BY NextPreservesStrengthened
  <2>2. CASE UNCHANGED vars
    BY StutteringPreservesStrengthened
  <2>3. QED BY <2>1, <2>2
<1>3. StrengthenedInvariant => CoreInvariant
  BY StrengthenedImpliesCore
<1>4. QED BY <1>1, <1>2, <1>3, PTL DEF Spec

COROLLARY FixedCoreNamedProperties ==
    ASSUME FixedMode
    PROVE  Spec =>
             [](/\ TypeOK
                /\ ClaimRevokeExclusive
                /\ ReleaseSafety
                /\ TokenRequiredExactness
                /\ CapacityBound
                /\ TerminalAtMostOnce
                /\ LiveFullIdUnique)
BY FixedCoreSafety, PTL DEF CoreInvariant

=============================================================================
