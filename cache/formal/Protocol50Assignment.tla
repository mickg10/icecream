------------------------- MODULE Protocol50Assignment -------------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
A deliberately small assignment-ownership model for the next P49/P50 product
slice.

It separates:

  * scheduler incarnation and UseCS publication;
  * the worker's wire-id keyed reservation/tombstone table;
  * legacy, nonce-bearing, and unknown/advisory claims; and
  * compiler-child attempts after one assignment claim has already won.

The model is intentionally not a scheduler, capacity, or timing model.  It
answers the protocol questions that otherwise cause ABA and restart bugs:

  * ENFORCING_COMPAT requires an F-installed prepare but legacy claims remain only
    compatibility-safe, not generation-exact;
  * STRICT_NONCE requires the nonce-bearing claim and is generation-exact;
  * a REVOKED result is a release proof only while its epoch-scoped tombstone
    remains installed;
  * CLAIMED_OR_LATER includes claimed-but-not-yet-running work, so a compiler
    child may restart without a new scheduler assignment or another claim.
***************************************************************************)

CONSTANTS Assignments, Epochs, WireIds, Nonces,
          NoAssignment, NoEpoch, InitEpoch, RestartAssignment,
          A0, A1, E0, E1, W0, N0, N1,
          EpochOf, WireOf, NonceOf, AssignmentAt,
          MutantAllowStrictLegacy,
          MutantPublishBeforeReady,
          MutantPublishAfterRevoke,
          MutantReleaseWithoutRevoke,
          MutantPrepareOverTombstone,
          MutantReuseEpochAfterLoss

EpochOfDef ==
    [a \in Assignments |-> IF a = A0 THEN E0 ELSE E1]

WireOfDef ==
    [a \in Assignments |-> W0]

NonceOfDef ==
    [a \in Assignments |-> IF a = A0 THEN N0 ELSE N1]

AssignmentAtDef ==
    [e \in Epochs |->
        [w \in WireIds |-> IF e = E0 THEN A0 ELSE A1]]

ASSUME /\ Assignments # {}
       /\ Epochs # {}
       /\ WireIds # {}
       /\ Nonces # {}
       /\ NoAssignment \notin Assignments
       /\ NoEpoch \notin Epochs
       /\ InitEpoch \in Epochs
       /\ RestartAssignment \in Assignments
       /\ A0 \in Assignments
       /\ A1 \in Assignments
       /\ A0 # A1
       /\ E0 \in Epochs
       /\ E1 \in Epochs
       /\ E0 # E1
       /\ W0 \in WireIds
       /\ N0 \in Nonces
       /\ N1 \in Nonces
       /\ N0 # N1
       /\ EpochOf \in [Assignments -> Epochs]
       /\ WireOf \in [Assignments -> WireIds]
       /\ NonceOf \in [Assignments -> Nonces]
       /\ AssignmentAt \in
              [Epochs -> [WireIds -> (Assignments \cup {NoAssignment})]]
       /\ \A e \in Epochs, w \in WireIds :
              LET a == AssignmentAt[e][w]
              IN a = NoAssignment \/
                    /\ a \in Assignments
                    /\ EpochOf[a] = e
                    /\ WireOf[a] = w
       /\ \A a, b \in Assignments :
              /\ EpochOf[a] = EpochOf[b]
              /\ WireOf[a] = WireOf[b]
              /\ NonceOf[a] = NonceOf[b]
              => a = b
       /\ MutantAllowStrictLegacy \in BOOLEAN
       /\ MutantPublishBeforeReady \in BOOLEAN
       /\ MutantPublishAfterRevoke \in BOOLEAN
       /\ MutantReleaseWithoutRevoke \in BOOLEAN
       /\ MutantPrepareOverTombstone \in BOOLEAN
       /\ MutantReuseEpochAfterLoss \in BOOLEAN

Modes == {"Legacy", "Advisory", "EnforcingCompat", "StrictNonce"}
Phases == {"Absent", "Reserved", "Claimed", "Started", "Terminal"}
ClaimKinds == {"None", "Unfenced", "Legacy", "Nonce"}
RevokeResults == {"None", "Revoked", "ClaimedOrLater"}
LiveClaimPhases == {"Claimed", "Started", "Terminal"}

EmptyRecord == [w \in WireIds |-> NoAssignment]
EmptyPhase == [w \in WireIds |-> "Absent"]
EmptyClaimant == [w \in WireIds |-> NoAssignment]
EmptyClaimKind == [w \in WireIds |-> "None"]
EmptyRevokeResult == [a \in Assignments |-> "None"]
EmptyAttemptCount == [a \in Assignments |-> 0]

VARIABLES currentEpoch, usedEpochs, workerEpoch, mode,
          record, phase, claimant, claimKind,
          prepared, ready, published, tombstone,
          revokeResult, released, authorized,
          attemptCount, attemptRunning, resultAccepted,
          badPublish

vars == <<currentEpoch, usedEpochs, workerEpoch, mode,
          record, phase, claimant, claimKind,
          prepared, ready, published, tombstone,
          revokeResult, released, authorized,
          attemptCount, attemptRunning, resultAccepted,
          badPublish>>

CurrentAssignment(w) ==
    IF workerEpoch = NoEpoch
    THEN NoAssignment
    ELSE AssignmentAt[workerEpoch][w]

Init ==
    /\ currentEpoch = InitEpoch
    /\ usedEpochs = {InitEpoch}
    /\ workerEpoch = NoEpoch
    /\ mode = "Legacy"
    /\ record = EmptyRecord
    /\ phase = EmptyPhase
    /\ claimant = EmptyClaimant
    /\ claimKind = EmptyClaimKind
    /\ prepared = {}
    /\ ready = {}
    /\ published = {}
    /\ tombstone = {}
    /\ revokeResult = EmptyRevokeResult
    /\ released = {}
    /\ authorized = {}
    /\ attemptCount = EmptyAttemptCount
    /\ attemptRunning = {}
    /\ resultAccepted = {}
    /\ badPublish = FALSE

Configure(m) ==
    /\ workerEpoch = NoEpoch
    /\ currentEpoch # NoEpoch
    /\ m \in Modes
    /\ workerEpoch' = currentEpoch
    /\ mode' = m
    /\ record' = EmptyRecord
    /\ phase' = EmptyPhase
    /\ claimant' = EmptyClaimant
    /\ claimKind' = EmptyClaimKind
    /\ prepared' = {}
    /\ ready' = {}
    /\ tombstone' = {}
    /\ attemptRunning' = {}
    /\ UNCHANGED <<currentEpoch, usedEpochs, published,
                    revokeResult, released, authorized,
                    attemptCount, resultAccepted, badPublish>>

LoseSchedulerLink ==
    /\ workerEpoch # NoEpoch
    /\ currentEpoch' =
           IF MutantReuseEpochAfterLoss THEN currentEpoch ELSE NoEpoch
    /\ workerEpoch' = NoEpoch
    /\ mode' = "Legacy"
    /\ record' = EmptyRecord
    /\ phase' = EmptyPhase
    /\ claimant' = EmptyClaimant
    /\ claimKind' = EmptyClaimKind
    /\ prepared' = {}
    /\ ready' = {}
    /\ tombstone' = {}
    /\ attemptRunning' = {}
    /\ UNCHANGED <<usedEpochs, published, revokeResult,
                    released, authorized,
                    attemptCount, resultAccepted, badPublish>>

RestartScheduler(e) ==
    /\ workerEpoch = NoEpoch
    /\ e \in Epochs \ usedEpochs
    /\ currentEpoch' = e
    /\ usedEpochs' = usedEpochs \cup {e}
    /\ ready' = {}
    /\ UNCHANGED <<workerEpoch, mode, record, phase, claimant,
                    claimKind, prepared, published, tombstone,
                    revokeResult, released, authorized,
                    attemptCount, attemptRunning, resultAccepted,
                    badPublish>>

Prepare(a) ==
    LET w == WireOf[a]
    IN /\ workerEpoch = currentEpoch
       /\ EpochOf[a] = currentEpoch
       /\ AssignmentAt[currentEpoch][w] = a
       /\ mode \in {"Advisory", "EnforcingCompat", "StrictNonce"}
       /\ (a \notin tombstone \/ MutantPrepareOverTombstone)
       /\ \/ /\ record[w] = NoAssignment
              /\ phase[w] = "Absent"
              /\ record' = [record EXCEPT ![w] = a]
              /\ phase' = [phase EXCEPT ![w] = "Reserved"]
          \/ /\ mode = "Advisory"
              /\ record[w] = a
              /\ phase[w] \in LiveClaimPhases
              /\ a \notin prepared
              /\ record' = record
              /\ phase' = phase
       /\ prepared' = prepared \cup {a}
       /\ UNCHANGED <<currentEpoch, usedEpochs, workerEpoch, mode,
                       claimant, claimKind, ready, published, tombstone,
                       revokeResult, released, authorized,
                       attemptCount, attemptRunning, resultAccepted,
                       badPublish>>

Ready(a) ==
    LET w == WireOf[a]
    IN /\ record[w] = a
       /\ (phase[w] = "Reserved" \/
           (mode = "Advisory" /\ phase[w] \in LiveClaimPhases))
       /\ a \in prepared
       /\ ready' = ready \cup {a}
       /\ UNCHANGED <<currentEpoch, usedEpochs, workerEpoch, mode,
                       record, phase, claimant, claimKind, prepared,
                       published, tombstone, revokeResult, released,
                       authorized, attemptCount, attemptRunning,
                       resultAccepted, badPublish>>

PublishUseCS(a) ==
    /\ workerEpoch = currentEpoch
    /\ EpochOf[a] = currentEpoch
    /\ AssignmentAt[currentEpoch][WireOf[a]] = a
    /\ (a \notin tombstone \/ MutantPublishAfterRevoke)
    /\ (mode \notin {"EnforcingCompat", "StrictNonce"} \/
        a \in ready \/ MutantPublishBeforeReady)
    /\ published' = published \cup {a}
    /\ badPublish' =
           (badPublish \/
            (mode \in {"EnforcingCompat", "StrictNonce"} /\ a \notin ready) \/
            a \in tombstone)
    /\ UNCHANGED <<currentEpoch, usedEpochs, workerEpoch, mode,
                    record, phase, claimant, claimKind, prepared,
                    ready, tombstone, revokeResult, released,
                    authorized, attemptCount, attemptRunning,
                    resultAccepted>>

(***************************************************************************
Unknown claims are admitted only in LEGACY/ADVISORY.  They are attributed to
whatever assignment currently owns that wire id in workerEpoch.  This is the
precise old-client ABA weakness: `arriving` may be an older published UseCS.
***************************************************************************)
UnknownClaim(arriving) ==
    LET w == WireOf[arriving]
        target == CurrentAssignment(w)
    IN /\ workerEpoch # NoEpoch
       /\ mode \in {"Legacy", "Advisory"}
       /\ arriving \in published
       /\ target # NoAssignment
       /\ record[w] = NoAssignment
       /\ phase[w] = "Absent"
       /\ target \notin tombstone
       /\ record' = [record EXCEPT ![w] = target]
       /\ phase' = [phase EXCEPT ![w] = "Claimed"]
       /\ claimant' = [claimant EXCEPT ![w] = arriving]
       /\ claimKind' = [claimKind EXCEPT ![w] = "Unfenced"]
       /\ authorized' = authorized \cup {target}
       /\ UNCHANGED <<currentEpoch, usedEpochs, workerEpoch, mode,
                       prepared, ready, published, tombstone,
                       revokeResult, released, attemptCount,
                       attemptRunning, resultAccepted, badPublish>>

KnownLegacyClaim(arriving) ==
    LET w == WireOf[arriving]
        target == record[w]
    IN /\ workerEpoch # NoEpoch
       /\ arriving \in published
       /\ target # NoAssignment
       /\ phase[w] = "Reserved"
       /\ WireOf[target] = WireOf[arriving]
       /\ target \notin tombstone
       /\ (mode \in {"Advisory", "EnforcingCompat"} \/
           (mode = "StrictNonce" /\ MutantAllowStrictLegacy))
       /\ phase' = [phase EXCEPT ![w] = "Claimed"]
       /\ claimant' = [claimant EXCEPT ![w] = arriving]
       /\ claimKind' = [claimKind EXCEPT ![w] = "Legacy"]
       /\ authorized' = authorized \cup {target}
       /\ UNCHANGED <<currentEpoch, usedEpochs, workerEpoch, mode,
                       record, prepared, ready, published, tombstone,
                       revokeResult, released, attemptCount,
                       attemptRunning, resultAccepted, badPublish>>

KnownNonceClaim(arriving) ==
    LET w == WireOf[arriving]
        target == record[w]
    IN /\ workerEpoch # NoEpoch
       /\ mode \in {"Advisory", "EnforcingCompat", "StrictNonce"}
       /\ arriving \in published
       /\ target # NoAssignment
       /\ phase[w] = "Reserved"
       /\ WireOf[target] = WireOf[arriving]
       /\ EpochOf[target] = EpochOf[arriving]
       /\ NonceOf[target] = NonceOf[arriving]
       /\ target \notin tombstone
       /\ phase' = [phase EXCEPT ![w] = "Claimed"]
       /\ claimant' = [claimant EXCEPT ![w] = arriving]
       /\ claimKind' = [claimKind EXCEPT ![w] = "Nonce"]
       /\ authorized' = authorized \cup {target}
       /\ UNCHANGED <<currentEpoch, usedEpochs, workerEpoch, mode,
                       record, prepared, ready, published, tombstone,
                       revokeResult, released, attemptCount,
                       attemptRunning, resultAccepted, badPublish>>

RevokeBeforeClaim(a) ==
    LET w == WireOf[a]
    IN /\ workerEpoch = currentEpoch
       /\ EpochOf[a] = currentEpoch
       /\ AssignmentAt[currentEpoch][w] = a
       /\ a \notin tombstone
       /\ \/ /\ record[w] = NoAssignment
              /\ phase[w] = "Absent"
          \/ /\ record[w] = a
              /\ phase[w] = "Reserved"
       /\ record' = [record EXCEPT ![w] = NoAssignment]
       /\ phase' = [phase EXCEPT ![w] = "Absent"]
       /\ claimant' = [claimant EXCEPT ![w] = NoAssignment]
       /\ claimKind' = [claimKind EXCEPT ![w] = "None"]
       /\ tombstone' = tombstone \cup {a}
       /\ revokeResult' = [revokeResult EXCEPT ![a] = "Revoked"]
       /\ UNCHANGED <<currentEpoch, usedEpochs, workerEpoch, mode,
                       prepared, ready, published, released, authorized,
                       attemptCount, attemptRunning, resultAccepted,
                       badPublish>>

RevokeAlready(a) ==
    /\ a \in tombstone
    /\ revokeResult' = [revokeResult EXCEPT ![a] = "Revoked"]
    /\ UNCHANGED <<currentEpoch, usedEpochs, workerEpoch, mode,
                    record, phase, claimant, claimKind, prepared,
                    ready, published, tombstone, released, authorized,
                    attemptCount, attemptRunning, resultAccepted,
                    badPublish>>

RevokeClaimedOrLater(a) ==
    LET w == WireOf[a]
    IN /\ record[w] = a
       /\ phase[w] \in LiveClaimPhases
       /\ revokeResult' =
              [revokeResult EXCEPT ![a] = "ClaimedOrLater"]
       /\ UNCHANGED <<currentEpoch, usedEpochs, workerEpoch, mode,
                       record, phase, claimant, claimKind, prepared,
                       ready, published, tombstone, released, authorized,
                       attemptCount, attemptRunning, resultAccepted,
                       badPublish>>

ReleaseAssignment(a) ==
    /\ a \in Assignments
    /\ (revokeResult[a] = "Revoked" \/ MutantReleaseWithoutRevoke)
    /\ released' = released \cup {a}
    /\ UNCHANGED <<currentEpoch, usedEpochs, workerEpoch, mode,
                    record, phase, claimant, claimKind, prepared,
                    ready, published, tombstone, revokeResult,
                    authorized, attemptCount, attemptRunning,
                    resultAccepted, badPublish>>

StartCompilerAttempt(a) ==
    LET w == WireOf[a]
    IN /\ record[w] = a
       /\ phase[w] \in {"Claimed", "Started"}
       /\ a \in authorized
       /\ a \notin attemptRunning
       /\ a \notin resultAccepted
       /\ attemptCount[a] < 2
       /\ phase' = [phase EXCEPT ![w] = "Started"]
       /\ attemptCount' = [attemptCount EXCEPT ![a] = @ + 1]
       /\ attemptRunning' = attemptRunning \cup {a}
       /\ UNCHANGED <<currentEpoch, usedEpochs, workerEpoch, mode,
                       record, claimant, claimKind, prepared, ready,
                       published, tombstone, revokeResult, released,
                       authorized, resultAccepted, badPublish>>

FailCompilerAttempt(a) ==
    /\ a \in attemptRunning
    /\ attemptRunning' = attemptRunning \ {a}
    /\ UNCHANGED <<currentEpoch, usedEpochs, workerEpoch, mode,
                    record, phase, claimant, claimKind, prepared,
                    ready, published, tombstone, revokeResult,
                    released, authorized, attemptCount,
                    resultAccepted, badPublish>>

FinishCompilerAttempt(a) ==
    LET w == WireOf[a]
    IN /\ a \in attemptRunning
       /\ record[w] = a
       /\ phase[w] = "Started"
       /\ attemptRunning' = attemptRunning \ {a}
       /\ phase' = [phase EXCEPT ![w] = "Terminal"]
       /\ resultAccepted' = resultAccepted \cup {a}
       /\ UNCHANGED <<currentEpoch, usedEpochs, workerEpoch, mode,
                       record, claimant, claimKind, prepared, ready,
                       published, tombstone, revokeResult, released,
                       authorized, attemptCount, badPublish>>

Next ==
    \/ \E m \in Modes : Configure(m)
    \/ LoseSchedulerLink
    \/ \E e \in Epochs : RestartScheduler(e)
    \/ \E a \in Assignments : Prepare(a)
    \/ \E a \in Assignments : Ready(a)
    \/ \E a \in Assignments : PublishUseCS(a)
    \/ \E a \in Assignments : UnknownClaim(a)
    \/ \E a \in Assignments : KnownLegacyClaim(a)
    \/ \E a \in Assignments : KnownNonceClaim(a)
    \/ \E a \in Assignments : RevokeBeforeClaim(a)
    \/ \E a \in Assignments : RevokeAlready(a)
    \/ \E a \in Assignments : RevokeClaimedOrLater(a)
    \/ \E a \in Assignments : ReleaseAssignment(a)
    \/ \E a \in Assignments : StartCompilerAttempt(a)
    \/ \E a \in Assignments : FailCompilerAttempt(a)
    \/ \E a \in Assignments : FinishCompilerAttempt(a)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ currentEpoch \in Epochs \cup {NoEpoch}
    /\ usedEpochs \subseteq Epochs
    /\ workerEpoch \in Epochs \cup {NoEpoch}
    /\ mode \in Modes
    /\ record \in [WireIds -> (Assignments \cup {NoAssignment})]
    /\ phase \in [WireIds -> Phases]
    /\ claimant \in [WireIds -> (Assignments \cup {NoAssignment})]
    /\ claimKind \in [WireIds -> ClaimKinds]
    /\ prepared \subseteq Assignments
    /\ ready \subseteq Assignments
    /\ published \subseteq Assignments
    /\ tombstone \subseteq Assignments
    /\ revokeResult \in [Assignments -> RevokeResults]
    /\ released \subseteq Assignments
    /\ authorized \subseteq Assignments
    /\ attemptCount \in [Assignments -> 0..2]
    /\ attemptRunning \subseteq Assignments
    /\ resultAccepted \subseteq Assignments
    /\ badPublish \in BOOLEAN

NoEpochHasNoWorkerState ==
    workerEpoch # NoEpoch \/
        /\ record = EmptyRecord
        /\ phase = EmptyPhase
        /\ claimant = EmptyClaimant
        /\ claimKind = EmptyClaimKind
        /\ prepared = {}
        /\ ready = {}
        /\ tombstone = {}
        /\ attemptRunning = {}

PhaseRecordCoherence ==
    \A w \in WireIds :
        (record[w] = NoAssignment) <=> (phase[w] = "Absent")

RecordEpochCoherence ==
    \A w \in WireIds :
        record[w] = NoAssignment \/
            /\ workerEpoch # NoEpoch
            /\ record[w] \in Assignments
            /\ EpochOf[record[w]] = workerEpoch
            /\ WireOf[record[w]] = w

ClaimStateCoherence ==
    \A w \in WireIds :
        /\ (phase[w] \in LiveClaimPhases =>
               /\ claimant[w] # NoAssignment
               /\ claimKind[w] # "None"
               /\ record[w] \in authorized)
        /\ (phase[w] = "Reserved" =>
               /\ claimant[w] = NoAssignment
               /\ claimKind[w] = "None")
        /\ (phase[w] = "Absent" =>
               /\ claimant[w] = NoAssignment
               /\ claimKind[w] = "None")

ReadySubsetPrepared == ready \subseteq prepared

EnforcingClaimsWerePrepared ==
    \A w \in WireIds :
        mode \in {"EnforcingCompat", "StrictNonce"} /\
        phase[w] \in LiveClaimPhases => record[w] \in prepared

NonceClaimsExact ==
    \A w \in WireIds :
        claimKind[w] = "Nonce" /\ phase[w] \in LiveClaimPhases =>
            claimant[w] = record[w]

StrictClaimsExact ==
    \A w \in WireIds :
        mode = "StrictNonce" /\ phase[w] \in LiveClaimPhases =>
            /\ claimKind[w] = "Nonce"
            /\ claimant[w] = record[w]

(***************************************************************************
This is intentionally NOT part of Safety.  It fails in mixed compatibility
mode: a delayed old client can present the same wire id and be attributed to
the current prepared assignment when the claim carries no nonce.
***************************************************************************)
EnforcingCompatClaimsExact ==
    \A w \in WireIds :
        mode = "EnforcingCompat" /\ phase[w] \in LiveClaimPhases =>
            claimant[w] = record[w]

AllClaimsExact ==
    \A w \in WireIds :
        phase[w] \in LiveClaimPhases => claimant[w] = record[w]

ReleaseHasRevocationProof ==
    \A a \in released : revokeResult[a] = "Revoked"

TombstoneIsNotLive ==
    \A a \in tombstone :
        LET w == WireOf[a]
        IN record[w] # a \/ phase[w] = "Absent"

SameEpochRevocationSafety ==
    \A a \in released :
        workerEpoch = EpochOf[a] =>
            \A w \in WireIds :
                claimant[w] # a \/ phase[w] \notin LiveClaimPhases

StrictRevocationSafety ==
    mode # "StrictNonce" \/
        \A w \in WireIds :
            claimant[w] \notin released \/
            phase[w] \notin LiveClaimPhases

AttemptAuthorization ==
    /\ attemptRunning \subseteq authorized
    /\ \A a \in attemptRunning :
           LET w == WireOf[a]
           IN /\ record[w] = a
              /\ phase[w] = "Started"
              /\ attemptCount[a] > 0

ResultAuthorization ==
    /\ resultAccepted \subseteq authorized
    /\ \A a \in resultAccepted : attemptCount[a] > 0

ReadyGate == ~badPublish

Safety ==
    /\ TypeOK
    /\ NoEpochHasNoWorkerState
    /\ PhaseRecordCoherence
    /\ RecordEpochCoherence
    /\ ClaimStateCoherence
    /\ ReadySubsetPrepared
    /\ EnforcingClaimsWerePrepared
    /\ NonceClaimsExact
    /\ StrictClaimsExact
    /\ ReleaseHasRevocationProof
    /\ TombstoneIsNotLive
    /\ SameEpochRevocationSafety
    /\ StrictRevocationSafety
    /\ AttemptAuthorization
    /\ ResultAuthorization
    /\ ReadyGate

(***************************************************************************
Scoped compiler-child restart proof.  The scheduler assignment and accepted
claim already exist.  Attempt 0 has failed after authorization; attempt 1 may
start from the same assignment and finish without another claim or prepare.
***************************************************************************)
RestartInit ==
    LET a == RestartAssignment
        w == WireOf[a]
    IN /\ currentEpoch = EpochOf[a]
       /\ usedEpochs = {EpochOf[a]}
       /\ workerEpoch = EpochOf[a]
       /\ mode = "StrictNonce"
       /\ record = [x \in WireIds |-> IF x = w THEN a ELSE NoAssignment]
       /\ phase = [x \in WireIds |-> IF x = w THEN "Started" ELSE "Absent"]
       /\ claimant = [x \in WireIds |-> IF x = w THEN a ELSE NoAssignment]
       /\ claimKind = [x \in WireIds |-> IF x = w THEN "Nonce" ELSE "None"]
       /\ prepared = {a}
       /\ ready = {a}
       /\ published = {a}
       /\ tombstone = {}
       /\ revokeResult = EmptyRevokeResult
       /\ released = {}
       /\ authorized = {a}
       /\ attemptCount =
              [x \in Assignments |-> IF x = a THEN 1 ELSE 0]
       /\ attemptRunning = {}
       /\ resultAccepted = {}
       /\ badPublish = FALSE

RestartNext ==
    StartCompilerAttempt(RestartAssignment) \/
    FinishCompilerAttempt(RestartAssignment)

RestartSpec ==
    /\ RestartInit
    /\ [][RestartNext]_vars
    /\ WF_vars(StartCompilerAttempt(RestartAssignment))
    /\ WF_vars(FinishCompilerAttempt(RestartAssignment))

RestartEventuallyFinishes ==
    <> (RestartAssignment \in resultAccepted)

=============================================================================
