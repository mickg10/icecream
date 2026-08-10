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

InductiveInvariant ==
    /\ TypeOK
    /\ OwnershipInvariant

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
    PROVE  InductiveInvariant
<1>1. TypeOK
  BY SMT DEF Init, TypeOK, Phases
<1>2. OwnershipInvariant
  BY SMT DEF FixedMode, Init, OwnershipInvariant,
             WorkerSlotCoherence, SchedulerReservationCoherence,
             PreparedEverExact, ReleasedExact, TerminalCountExact,
             ClaimEvidenceShape, TokenClaimExact, LiveFullIdUnique,
             LiveWorkerPhases, LiveSchedulerPhases
<1>3. QED BY <1>1, <1>2 DEF InductiveInvariant

(***************************************************************************
Prepare is the only action that introduces a new live scheduler identity.  Its
explicit guard is exactly the uniqueness premise needed here.
***************************************************************************)
LEMMA PreparePreservesFullIdUniqueness ==
    ASSUME FixedMode,
           InductiveInvariant,
           NEW a \in Assignments,
           Prepare(a)
    PROVE  LiveFullIdUnique'
BY SMT DEF FixedMode, InductiveInvariant, TypeOK, OwnershipInvariant, Prepare,
           LiveFullIdUnique, SchedulerReservationCoherence,
           LiveSchedulerPhases

LEMMA PreparePreservesOwnership ==
    ASSUME FixedMode,
           InductiveInvariant,
           NEW a \in Assignments,
           Prepare(a)
    PROVE  InductiveInvariant'
<1>1. TypeOK'
  BY SMT DEF InductiveInvariant, TypeOK, Prepare, Phases
<1>2. OwnershipInvariant'
  <2>1. WorkerSlotCoherence'
    <3>1. SUFFICES ASSUME NEW b \in Assignments
                    PROVE  workerSlot'[b]
                           <=> phase'[b] \in LiveWorkerPhases
      BY DEF WorkerSlotCoherence
    <3>2. (b = a) => (workerSlot'[b]
                       <=> phase'[b] \in LiveWorkerPhases)
      <4>1. "Prepared" \in LiveWorkerPhases
        BY DEF LiveWorkerPhases
      <4>2. (b = a) => workerSlot'[b] = TRUE
        BY Isa DEF InductiveInvariant, TypeOK, Prepare
      <4>3. (b = a) => phase'[b] = "Prepared"
        BY Isa DEF InductiveInvariant, TypeOK, Prepare
      <4>4. QED BY <4>1, <4>2, <4>3, SMT
    <3>3. (b # a) => (workerSlot'[b]
                       <=> phase'[b] \in LiveWorkerPhases)
      BY SMT DEF InductiveInvariant, OwnershipInvariant, Prepare,
                 WorkerSlotCoherence, LiveWorkerPhases
    <3>4. QED BY <3>2, <3>3, SMT
  <2>2. SchedulerReservationCoherence'
    <3>1. SUFFICES ASSUME NEW b \in Assignments
                    PROVE  schedulerReservation'[b]
                           <=> phase'[b] \in LiveSchedulerPhases
      BY DEF SchedulerReservationCoherence
    <3>2. (b = a) => (schedulerReservation'[b]
                       <=> phase'[b] \in LiveSchedulerPhases)
      <4>1. "Prepared" \in LiveSchedulerPhases
        BY DEF LiveSchedulerPhases, LiveWorkerPhases
      <4>2. (b = a) => schedulerReservation'[b] = TRUE
        BY Isa DEF InductiveInvariant, TypeOK, Prepare
      <4>3. (b = a) => phase'[b] = "Prepared"
        BY Isa DEF InductiveInvariant, TypeOK, Prepare
      <4>4. QED BY <4>1, <4>2, <4>3, SMT
    <3>3. (b # a) => (schedulerReservation'[b]
                       <=> phase'[b] \in LiveSchedulerPhases)
      BY SMT DEF InductiveInvariant, OwnershipInvariant, Prepare,
                 SchedulerReservationCoherence, LiveSchedulerPhases
    <3>4. QED BY <3>2, <3>3, SMT
  <2>3. PreparedEverExact'
    <3>1. SUFFICES ASSUME NEW b \in Assignments
                    PROVE  preparedEver'[b] <=> phase'[b] # "Absent"
      BY DEF PreparedEverExact
    <3>2. (b = a) => (preparedEver'[b]
                       <=> phase'[b] # "Absent")
      BY Isa DEF InductiveInvariant, TypeOK, OwnershipInvariant, Prepare,
                 PreparedEverExact, Phases
    <3>3. (b # a) => (preparedEver'[b]
                       <=> phase'[b] # "Absent")
      BY SMT DEF InductiveInvariant, OwnershipInvariant, Prepare,
                 PreparedEverExact
    <3>4. QED BY <3>2, <3>3, SMT
  <2>4. ReleasedExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, Prepare, ReleasedExact
  <2>5. TerminalCountExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, Prepare,
               TerminalCountExact
  <2>6. ClaimEvidenceShape'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, Prepare,
               ClaimEvidenceShape
  <2>7. TokenClaimExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, Prepare, TokenClaimExact
  <2>8. LiveFullIdUnique' BY PreparePreservesFullIdUniqueness
  <2>9. QED BY <2>1, <2>2, <2>3, <2>4, <2>5, <2>6, <2>7, <2>8
              DEF OwnershipInvariant
<1>3. QED BY <1>1, <1>2 DEF InductiveInvariant

LEMMA ClaimLegacyPreservesOwnership ==
    ASSUME FixedMode,
           InductiveInvariant,
           NEW current \in Assignments,
           NEW arriving \in Assignments,
           ClaimLegacy(current, arriving)
    PROVE  InductiveInvariant'
<1>1. TypeOK'
  BY SMT DEF InductiveInvariant, TypeOK, ClaimLegacy, Phases
<1>2. OwnershipInvariant'
  <2>1. WorkerSlotCoherence'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimLegacy,
               WorkerSlotCoherence, LiveWorkerPhases
  <2>2. SchedulerReservationCoherence'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimLegacy,
               SchedulerReservationCoherence, LiveSchedulerPhases,
               LiveWorkerPhases
  <2>3. PreparedEverExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimLegacy,
               PreparedEverExact
  <2>4. ReleasedExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimLegacy, ReleasedExact
  <2>5. TerminalCountExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimLegacy,
               TerminalCountExact
  <2>6. ClaimEvidenceShape'
    <3>1. SUFFICES ASSUME NEW b \in Assignments
                    PROVE  /\ phase'[b] \in {"Absent", "Prepared", "Revoked"}
                                  => claimant'[b] = NoClaimant /\ ~exactClaim'[b]
                           /\ phase'[b] \in {"Claimed", "Started"}
                                  => claimant'[b] # NoClaimant
                           /\ exactClaim'[b] => claimant'[b] # NoClaimant
      BY DEF ClaimEvidenceShape
    <3>2. (b = current) =>
           (/\ phase'[b] \in {"Absent", "Prepared", "Revoked"}
                  => claimant'[b] = NoClaimant /\ ~exactClaim'[b]
            /\ phase'[b] \in {"Claimed", "Started"}
                  => claimant'[b] # NoClaimant
            /\ exactClaim'[b] => claimant'[b] # NoClaimant)
      <4>1. (b = current) => phase'[b] = "Claimed"
        BY CoreConstants, Isa
           DEF InductiveInvariant, TypeOK, ClaimLegacy, Phases
      <4>2. (b = current) => claimant'[b] = arriving
        BY CoreConstants, Isa
           DEF InductiveInvariant, TypeOK, ClaimLegacy
      <4>3. arriving # NoClaimant
        BY CoreConstants, SMT DEF ClaimLegacy
      <4>4. QED BY <4>1, <4>2, <4>3, SMT
    <3>3. (b # current) =>
           (/\ phase'[b] \in {"Absent", "Prepared", "Revoked"}
                  => claimant'[b] = NoClaimant /\ ~exactClaim'[b]
            /\ phase'[b] \in {"Claimed", "Started"}
                  => claimant'[b] # NoClaimant
            /\ exactClaim'[b] => claimant'[b] # NoClaimant)
      BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimLegacy,
                 ClaimEvidenceShape
    <3>4. QED BY <3>2, <3>3, SMT
  <2>7. TokenClaimExact'
    <3>1. SUFFICES ASSUME NEW b \in Assignments
                    PROVE  PolicyOf[b] = TokenRequired
                           /\ claimant'[b] # NoClaimant => exactClaim'[b]
      BY DEF TokenClaimExact
    <3>2. (b = current) =>
           (PolicyOf[b] = TokenRequired
            /\ claimant'[b] # NoClaimant => exactClaim'[b])
      BY CoreConstants, SMT DEF FixedMode, ClaimLegacy
    <3>3. (b # current) =>
           (PolicyOf[b] = TokenRequired
            /\ claimant'[b] # NoClaimant => exactClaim'[b])
      BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimLegacy,
                 TokenClaimExact
    <3>4. QED BY <3>2, <3>3, SMT
  <2>8. LiveFullIdUnique'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimLegacy,
               LiveFullIdUnique, LiveSchedulerPhases, LiveWorkerPhases
  <2>9. QED BY <2>1, <2>2, <2>3, <2>4, <2>5, <2>6, <2>7, <2>8
              DEF OwnershipInvariant
<1>3. QED BY <1>1, <1>2 DEF InductiveInvariant

LEMMA ClaimTokenPreservesOwnership ==
    ASSUME FixedMode,
           InductiveInvariant,
           NEW current \in Assignments,
           NEW arriving \in Assignments,
           ClaimToken(current, arriving)
    PROVE  InductiveInvariant'
<1>1. TypeOK'
  BY SMT DEF InductiveInvariant, TypeOK, ClaimToken, Phases
<1>2. OwnershipInvariant'
  <2>1. WorkerSlotCoherence'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimToken,
               WorkerSlotCoherence, LiveWorkerPhases
  <2>2. SchedulerReservationCoherence'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimToken,
               SchedulerReservationCoherence, LiveSchedulerPhases,
               LiveWorkerPhases
  <2>3. PreparedEverExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimToken,
               PreparedEverExact
  <2>4. ReleasedExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimToken, ReleasedExact
  <2>5. TerminalCountExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimToken,
               TerminalCountExact
  <2>6. ClaimEvidenceShape'
    BY CoreConstants, SMT
       DEF InductiveInvariant, TypeOK, OwnershipInvariant, ClaimToken,
           ClaimEvidenceShape, Phases
  <2>7. TokenClaimExact'
    BY CoreConstants, SMT
       DEF InductiveInvariant, TypeOK, OwnershipInvariant, ClaimToken,
           TokenClaimExact, Phases
  <2>8. LiveFullIdUnique'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, ClaimToken,
               LiveFullIdUnique, LiveSchedulerPhases, LiveWorkerPhases
  <2>9. QED BY <2>1, <2>2, <2>3, <2>4, <2>5, <2>6, <2>7, <2>8
              DEF OwnershipInvariant
<1>3. QED BY <1>1, <1>2 DEF InductiveInvariant

LEMMA StartPreservesOwnership ==
    ASSUME FixedMode,
           InductiveInvariant,
           NEW a \in Assignments,
           Start(a)
    PROVE  InductiveInvariant'
<1>1. TypeOK'
  BY SMT DEF InductiveInvariant, TypeOK, Start, Phases
<1>2. OwnershipInvariant'
  BY SMT DEF FixedMode, InductiveInvariant, OwnershipInvariant, Start,
             WorkerSlotCoherence, SchedulerReservationCoherence,
             PreparedEverExact, ReleasedExact, TerminalCountExact,
             ClaimEvidenceShape, TokenClaimExact, LiveFullIdUnique,
             LiveWorkerPhases, LiveSchedulerPhases
<1>3. QED BY <1>1, <1>2 DEF InductiveInvariant

LEMMA RevokePreservesOwnership ==
    ASSUME FixedMode,
           InductiveInvariant,
           NEW a \in Assignments,
           Revoke(a)
    PROVE  InductiveInvariant'
<1>1. TypeOK'
  BY SMT DEF InductiveInvariant, TypeOK, Revoke, Phases
<1>2. OwnershipInvariant'
  <2>1. WorkerSlotCoherence'
    BY SMT DEF InductiveInvariant, TypeOK, OwnershipInvariant, Revoke,
               WorkerSlotCoherence, LiveWorkerPhases, Phases
  <2>2. SchedulerReservationCoherence'
    BY SMT DEF InductiveInvariant, TypeOK, OwnershipInvariant, Revoke,
               SchedulerReservationCoherence, LiveSchedulerPhases,
               LiveWorkerPhases, Phases
  <2>3. PreparedEverExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, Revoke,
               PreparedEverExact
  <2>4. ReleasedExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, Revoke, ReleasedExact
  <2>5. TerminalCountExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, Revoke,
               TerminalCountExact
  <2>6. ClaimEvidenceShape'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, Revoke,
               ClaimEvidenceShape
  <2>7. TokenClaimExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, Revoke, TokenClaimExact
  <2>8. LiveFullIdUnique'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, Revoke,
               LiveFullIdUnique, LiveSchedulerPhases, LiveWorkerPhases
  <2>9. QED BY <2>1, <2>2, <2>3, <2>4, <2>5, <2>6, <2>7, <2>8
              DEF OwnershipInvariant
<1>3. QED BY <1>1, <1>2 DEF InductiveInvariant

LEMMA ConsumeRevokedPreservesOwnership ==
    ASSUME FixedMode,
           InductiveInvariant,
           NEW a \in Assignments,
           ConsumeRevoked(a)
    PROVE  InductiveInvariant'
<1>1. TypeOK'
  BY SMT DEF InductiveInvariant, TypeOK, ConsumeRevoked, Phases
<1>2. OwnershipInvariant'
  <2>1. WorkerSlotCoherence'
    BY SMT DEF FixedMode, InductiveInvariant, TypeOK, OwnershipInvariant,
               ConsumeRevoked, WorkerSlotCoherence, LiveWorkerPhases, Phases
  <2>2. SchedulerReservationCoherence'
    BY SMT DEF FixedMode, InductiveInvariant, TypeOK, OwnershipInvariant,
               ConsumeRevoked, SchedulerReservationCoherence,
               LiveSchedulerPhases, LiveWorkerPhases, Phases
  <2>3. PreparedEverExact'
    BY SMT DEF FixedMode, InductiveInvariant, OwnershipInvariant,
               ConsumeRevoked, PreparedEverExact
  <2>4. ReleasedExact'
    <3>1. SUFFICES ASSUME NEW b \in Assignments
                    PROVE  released'[b] <=> phase'[b] = "Terminal"
      BY DEF ReleasedExact
    <3>2. (b = a) => (released'[b] <=> phase'[b] = "Terminal")
      <4>1. (b = a) => released'[b] = TRUE
        BY Isa DEF InductiveInvariant, TypeOK, ConsumeRevoked
      <4>2. (b = a) => phase'[b] = "Terminal"
        BY Isa DEF FixedMode, InductiveInvariant, TypeOK, ConsumeRevoked,
                   Phases
      <4>3. QED BY <4>1, <4>2, SMT
    <3>3. (b # a) => (released'[b] <=> phase'[b] = "Terminal")
      BY SMT DEF InductiveInvariant, OwnershipInvariant, ConsumeRevoked,
                 ReleasedExact
    <3>4. QED BY <3>2, <3>3, SMT
  <2>5. TerminalCountExact'
    BY SMT DEF FixedMode, InductiveInvariant, TypeOK, OwnershipInvariant,
               ConsumeRevoked, TerminalCountExact, Phases
  <2>6. ClaimEvidenceShape'
    BY SMT DEF FixedMode, InductiveInvariant, OwnershipInvariant,
               ConsumeRevoked, ClaimEvidenceShape
  <2>7. TokenClaimExact'
    BY SMT DEF FixedMode, InductiveInvariant, OwnershipInvariant,
               ConsumeRevoked, TokenClaimExact
  <2>8. LiveFullIdUnique'
    BY SMT DEF FixedMode, InductiveInvariant, OwnershipInvariant,
               ConsumeRevoked, LiveFullIdUnique, LiveSchedulerPhases,
               LiveWorkerPhases
  <2>9. QED BY <2>1, <2>2, <2>3, <2>4, <2>5, <2>6, <2>7, <2>8
              DEF OwnershipInvariant
<1>3. QED BY <1>1, <1>2 DEF InductiveInvariant

LEMMA CompletePreservesOwnership ==
    ASSUME FixedMode,
           InductiveInvariant,
           NEW a \in Assignments,
           Complete(a)
    PROVE  InductiveInvariant'
<1>1. TypeOK'
  BY SMT DEF InductiveInvariant, TypeOK, Complete, Phases
<1>2. OwnershipInvariant'
  <2>1. WorkerSlotCoherence'
    BY SMT DEF InductiveInvariant, TypeOK, OwnershipInvariant, Complete,
               WorkerSlotCoherence, LiveWorkerPhases, ClaimedPhases, Phases
  <2>2. SchedulerReservationCoherence'
    BY SMT DEF InductiveInvariant, TypeOK, OwnershipInvariant, Complete,
               SchedulerReservationCoherence, LiveSchedulerPhases,
               LiveWorkerPhases, ClaimedPhases, Phases
  <2>3. PreparedEverExact'
    <3>1. SUFFICES ASSUME NEW b \in Assignments
                    PROVE  preparedEver'[b] <=> phase'[b] # "Absent"
      BY DEF PreparedEverExact
    <3>2. (b = a) => (preparedEver'[b] <=> phase'[b] # "Absent")
      <4>1. (b = a) => phase'[b] = "Terminal"
        BY Isa DEF InductiveInvariant, TypeOK, Complete, Phases
      <4>2. preparedEver[a] = TRUE
        BY SMT DEF InductiveInvariant, OwnershipInvariant, Complete,
                   PreparedEverExact, ClaimedPhases, Phases
      <4>3. preparedEver' = preparedEver
        BY SMT DEF Complete
      <4>4. (b = a) => preparedEver'[b] = TRUE
        BY <4>2, <4>3, SMT
      <4>5. QED BY <4>1, <4>4, SMT
    <3>3. (b # a) => (preparedEver'[b] <=> phase'[b] # "Absent")
      BY SMT DEF InductiveInvariant, OwnershipInvariant, Complete,
                 PreparedEverExact
    <3>4. QED BY <3>2, <3>3, SMT
  <2>4. ReleasedExact'
    BY SMT DEF InductiveInvariant, TypeOK, OwnershipInvariant, Complete,
               ReleasedExact, ClaimedPhases, Phases
  <2>5. TerminalCountExact'
    BY SMT DEF InductiveInvariant, TypeOK, OwnershipInvariant, Complete,
               TerminalCountExact, ClaimedPhases, Phases
  <2>6. ClaimEvidenceShape'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, Complete,
               ClaimEvidenceShape, ClaimedPhases
  <2>7. TokenClaimExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, Complete,
               TokenClaimExact
  <2>8. LiveFullIdUnique'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, Complete,
               LiveFullIdUnique, LiveSchedulerPhases, LiveWorkerPhases,
               ClaimedPhases
  <2>9. QED BY <2>1, <2>2, <2>3, <2>4, <2>5, <2>6, <2>7, <2>8
              DEF OwnershipInvariant
<1>3. QED BY <1>1, <1>2 DEF InductiveInvariant

LEMMA WorkerLostPreservesOwnership ==
    ASSUME FixedMode,
           InductiveInvariant,
           NEW a \in Assignments,
           WorkerLost(a)
    PROVE  InductiveInvariant'
<1>1. TypeOK'
  BY SMT DEF InductiveInvariant, TypeOK, WorkerLost, Phases
<1>2. OwnershipInvariant'
  <2>1. WorkerSlotCoherence'
    BY SMT DEF InductiveInvariant, TypeOK, OwnershipInvariant, WorkerLost,
               WorkerSlotCoherence, LiveWorkerPhases, Phases
  <2>2. SchedulerReservationCoherence'
    BY SMT DEF InductiveInvariant, TypeOK, OwnershipInvariant, WorkerLost,
               SchedulerReservationCoherence, LiveSchedulerPhases,
               LiveWorkerPhases, Phases
  <2>3. PreparedEverExact'
    BY SMT DEF InductiveInvariant, TypeOK, OwnershipInvariant, WorkerLost,
               PreparedEverExact, LiveWorkerPhases, Phases
  <2>4. ReleasedExact'
    BY SMT DEF InductiveInvariant, TypeOK, OwnershipInvariant, WorkerLost,
               ReleasedExact, LiveWorkerPhases, Phases
  <2>5. TerminalCountExact'
    BY SMT DEF InductiveInvariant, TypeOK, OwnershipInvariant, WorkerLost,
               TerminalCountExact, LiveWorkerPhases, Phases
  <2>6. ClaimEvidenceShape'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, WorkerLost,
               ClaimEvidenceShape, LiveWorkerPhases
  <2>7. TokenClaimExact'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, WorkerLost,
               TokenClaimExact
  <2>8. LiveFullIdUnique'
    BY SMT DEF InductiveInvariant, OwnershipInvariant, WorkerLost,
               LiveFullIdUnique, LiveSchedulerPhases, LiveWorkerPhases
  <2>9. QED BY <2>1, <2>2, <2>3, <2>4, <2>5, <2>6, <2>7, <2>8
              DEF OwnershipInvariant
<1>3. QED BY <1>1, <1>2 DEF InductiveInvariant

LEMMA NextPreservesOwnership ==
    ASSUME FixedMode, InductiveInvariant, Next
    PROVE  InductiveInvariant'
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
    ASSUME InductiveInvariant, UNCHANGED vars
    PROVE  InductiveInvariant'
<1>1. phase' = phase BY Isa DEF vars
<1>2. claimant' = claimant BY Isa DEF vars
<1>3. exactClaim' = exactClaim BY Isa DEF vars
<1>4. schedulerReservation' = schedulerReservation BY Isa DEF vars
<1>5. workerSlot' = workerSlot BY Isa DEF vars
<1>6. released' = released BY Isa DEF vars
<1>7. terminalCount' = terminalCount BY Isa DEF vars
<1>8. preparedEver' = preparedEver BY Isa DEF vars
<1>9. TypeOK'
  BY <1>1, <1>2, <1>3, <1>4, <1>5, <1>6, <1>7, <1>8, SMT
     DEF InductiveInvariant, TypeOK
<1>10. OwnershipInvariant'
  BY <1>1, <1>2, <1>3, <1>4, <1>5, <1>6, <1>7, <1>8, SMT
     DEF InductiveInvariant, OwnershipInvariant, WorkerSlotCoherence,
         SchedulerReservationCoherence, PreparedEverExact, ReleasedExact,
         TerminalCountExact, ClaimEvidenceShape, TokenClaimExact,
         LiveFullIdUnique
<1>11. QED BY <1>9, <1>10 DEF InductiveInvariant

LEMMA StepPreservesOwnership ==
    ASSUME FixedMode, InductiveInvariant, [Next]_vars
    PROVE  InductiveInvariant'
<1>1. CASE Next
  BY <1>1, NextPreservesOwnership
<1>2. CASE UNCHANGED vars
  BY <1>2, StutteringPreservesOwnership
<1>3. QED BY <1>1, <1>2 DEF vars

THEOREM FixedCoreOwnershipSafety ==
    ASSUME FixedMode
    PROVE  Spec => []ProofSafety
<1>1. Init => InductiveInvariant
  BY InitEstablishesOwnership
<1>2. InductiveInvariant /\ [Next]_vars => InductiveInvariant'
  BY StepPreservesOwnership
<1>3. InductiveInvariant => ProofSafety
  BY OwnershipImpliesSafety DEF InductiveInvariant
<1>4. QED BY <1>1, <1>2, <1>3, PTL DEF Spec

=============================================================================
