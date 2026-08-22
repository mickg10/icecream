--------------------- MODULE Protocol50AssignmentDelivery ---------------------
EXTENDS TLC

(***************************************************************************
Focused refinement model for two assignment-delivery interleavings abstracted
by Protocol50Assignment.tla:

  1. successive assignments may reuse one scheduler wire id in one epoch but
     carry different assignment nonces. Delayed READY/REVOKE traffic must be
     bound to the exact assignment, not looked up by wire id at delivery time;
  2. in ADVISORY mode a legacy claim may win before PREPARE arrives. The late
     PREPARE may enrich and acknowledge that claim but may never regress it to
     RESERVED.

OldA and NewA represent two generations with the same epoch and wire id but
distinct nonces. Therefore PREPARE, READY, REVOKE request, and REVOKE result
must all carry the complete (epoch, wire_id, nonce) identity. CompileFile
already carries wire_id plus the protocol-50 (epoch, nonce) capability.
***************************************************************************)

CONSTANTS OldA, NewA, NoA,
          MutantReadyByWireOnly,
          MutantRevokeRequestByWireOnly,
          MutantRevokeResultByWireOnly,
          MutantLatePrepareRegresses

ASSUME /\ OldA # NewA
       /\ NoA \notin {OldA, NewA}
       /\ MutantReadyByWireOnly \in BOOLEAN
       /\ MutantRevokeRequestByWireOnly \in BOOLEAN
       /\ MutantRevokeResultByWireOnly \in BOOLEAN
       /\ MutantLatePrepareRegresses \in BOOLEAN

Assignments == {OldA, NewA}
Phases == {"Absent", "Reserved", "Claimed", "Started", "Terminal", "Revoked"}
ClaimKinds == {"None", "Unfenced", "Nonce"}
RevokeResults == {"Revoked", "ClaimedOrLater"}
NoResult == "NoResult"

VARIABLES current,
          phase, claimKind,
          workerPrepared, workerRevoked, workerClaimedOrLater,
          schedulerRevokeSent,
          readyInFlight, readyAccepted,
          revokeRequestInFlight,
          revokeResultInFlight, revokeResultKind,
          revokeResultsDelivered,
          claimCrossedRevoke,
          published,
          revokedReleased, claimedRetained,
          ordinarySettled, released

vars == <<current,
          phase, claimKind,
          workerPrepared, workerRevoked, workerClaimedOrLater,
          schedulerRevokeSent,
          readyInFlight, readyAccepted,
          revokeRequestInFlight,
          revokeResultInFlight, revokeResultKind,
          revokeResultsDelivered,
          claimCrossedRevoke,
          published,
          revokedReleased, claimedRetained,
          ordinarySettled, released>>

Init ==
    /\ current = OldA
    /\ phase = [a \in Assignments |-> "Absent"]
    /\ claimKind = [a \in Assignments |-> "None"]
    /\ workerPrepared = {}
    /\ workerRevoked = {}
    /\ workerClaimedOrLater = {}
    /\ schedulerRevokeSent = {}
    /\ readyInFlight = NoA
    /\ readyAccepted = NoA
    /\ revokeRequestInFlight = NoA
    /\ revokeResultInFlight = NoA
    /\ revokeResultKind = NoResult
    /\ revokeResultsDelivered = {}
    /\ claimCrossedRevoke = {}
    /\ published = {}
    /\ revokedReleased = {}
    /\ claimedRetained = {}
    /\ ordinarySettled = {}
    /\ released = {}

PrepareCurrent ==
    LET a == current
    IN /\ phase[a] = "Absent"
       /\ a \notin workerPrepared
       /\ readyInFlight = NoA
       /\ phase' = [phase EXCEPT ![a] = "Reserved"]
       /\ workerPrepared' = workerPrepared \cup {a}
       /\ readyInFlight' = a
       /\ UNCHANGED <<current, claimKind,
                       workerRevoked, workerClaimedOrLater,
                       schedulerRevokeSent, readyAccepted,
                       revokeRequestInFlight,
                       revokeResultInFlight, revokeResultKind,
                       revokeResultsDelivered, claimCrossedRevoke,
                       published, revokedReleased, claimedRetained,
                       ordinarySettled, released>>

AdvisoryClaimCurrent ==
    LET a == current
    IN /\ phase[a] = "Absent"
       /\ phase' = [phase EXCEPT ![a] = "Claimed"]
       /\ claimKind' = [claimKind EXCEPT ![a] = "Unfenced"]
       /\ claimCrossedRevoke' =
              IF revokeRequestInFlight = a
              THEN claimCrossedRevoke \cup {a}
              ELSE claimCrossedRevoke
       /\ UNCHANGED <<current, workerPrepared,
                       workerRevoked, workerClaimedOrLater,
                       schedulerRevokeSent, readyInFlight, readyAccepted,
                       revokeRequestInFlight,
                       revokeResultInFlight, revokeResultKind,
                       revokeResultsDelivered,
                       published, revokedReleased, claimedRetained,
                       ordinarySettled, released>>

PrepareAfterUnfencedClaim(a) ==
    /\ a \in Assignments
    /\ phase[a] = "Claimed"
    /\ claimKind[a] = "Unfenced"
    /\ a \notin workerPrepared
    /\ readyInFlight = NoA
    /\ workerPrepared' = workerPrepared \cup {a}
    /\ readyInFlight' = a
    /\ phase' = [phase EXCEPT
                    ![a] = IF MutantLatePrepareRegresses
                           THEN "Reserved" ELSE @]
    /\ UNCHANGED <<current, claimKind,
                    workerRevoked, workerClaimedOrLater,
                    schedulerRevokeSent, readyAccepted,
                    revokeRequestInFlight,
                    revokeResultInFlight, revokeResultKind,
                    revokeResultsDelivered, claimCrossedRevoke,
                    published, revokedReleased, claimedRetained,
                    ordinarySettled, released>>

SwitchToNew ==
    /\ current = OldA
    /\ current' = NewA
    /\ UNCHANGED <<phase, claimKind, workerPrepared,
                    workerRevoked, workerClaimedOrLater,
                    schedulerRevokeSent, readyInFlight, readyAccepted,
                    revokeRequestInFlight,
                    revokeResultInFlight, revokeResultKind,
                    revokeResultsDelivered, claimCrossedRevoke,
                    published, revokedReleased, claimedRetained,
                    ordinarySettled, released>>

DeliverReady ==
    LET origin == readyInFlight
        accepted == IF MutantReadyByWireOnly
                    THEN current
                    ELSE IF origin = current THEN origin ELSE NoA
    IN /\ origin \in Assignments
       /\ readyInFlight' = NoA
       /\ readyAccepted' = accepted
       /\ UNCHANGED <<current, phase, claimKind,
                       workerPrepared,
                       workerRevoked, workerClaimedOrLater,
                       schedulerRevokeSent,
                       revokeRequestInFlight,
                       revokeResultInFlight, revokeResultKind,
                       revokeResultsDelivered, claimCrossedRevoke,
                       published, revokedReleased, claimedRetained,
                       ordinarySettled, released>>

PublishCurrent ==
    /\ readyAccepted = current
    /\ published' = published \cup {current}
    /\ UNCHANGED <<current, phase, claimKind,
                    workerPrepared,
                    workerRevoked, workerClaimedOrLater,
                    schedulerRevokeSent,
                    readyInFlight, readyAccepted,
                    revokeRequestInFlight,
                    revokeResultInFlight, revokeResultKind,
                    revokeResultsDelivered, claimCrossedRevoke,
                    revokedReleased, claimedRetained,
                    ordinarySettled, released>>

SendRevokeCurrent ==
    /\ revokeRequestInFlight = NoA
    /\ current \notin schedulerRevokeSent
    /\ schedulerRevokeSent' = schedulerRevokeSent \cup {current}
    /\ revokeRequestInFlight' = current
    /\ UNCHANGED <<current, phase, claimKind,
                    workerPrepared,
                    workerRevoked, workerClaimedOrLater,
                    readyInFlight, readyAccepted,
                    revokeResultInFlight, revokeResultKind,
                    revokeResultsDelivered, claimCrossedRevoke,
                    published, revokedReleased, claimedRetained,
                    ordinarySettled, released>>

DeliverRevokeRequest ==
    LET origin == revokeRequestInFlight
        target == IF MutantRevokeRequestByWireOnly THEN current ELSE origin
        claimWon == phase[target] \in {"Claimed", "Started", "Terminal"}
        alreadyRevoked == phase[target] = "Revoked"
        result == IF claimWon THEN "ClaimedOrLater" ELSE "Revoked"
    IN /\ origin \in Assignments
       /\ revokeResultInFlight = NoA
       /\ workerRevoked' =
              IF claimWon THEN workerRevoked
              ELSE workerRevoked \cup {target}
       /\ workerClaimedOrLater' =
              IF claimWon
              THEN workerClaimedOrLater \cup {target}
              ELSE workerClaimedOrLater
       /\ phase' = [phase EXCEPT
                       ![target] = IF claimWon \/ alreadyRevoked
                                  THEN @ ELSE "Revoked"]
       /\ revokeRequestInFlight' = NoA
       \* The result echoes the request's full identity.  A wire-only mutant
       \* may select the wrong record, but it does not erase the nonce carried
       \* by the control message.
       /\ revokeResultInFlight' = origin
       /\ revokeResultKind' = result
       /\ UNCHANGED <<current, claimKind, workerPrepared,
                       schedulerRevokeSent, readyInFlight, readyAccepted,
                       revokeResultsDelivered, claimCrossedRevoke,
                       published, revokedReleased, claimedRetained,
                       ordinarySettled, released>>

DeliverRevokeResult ==
    LET origin == revokeResultInFlight
        accepted == IF MutantRevokeResultByWireOnly THEN current ELSE origin
        result == revokeResultKind
    IN /\ origin \in Assignments
       /\ result \in RevokeResults
       /\ revokeResultInFlight' = NoA
       /\ revokeResultKind' = NoResult
       /\ revokeResultsDelivered' = revokeResultsDelivered \cup {origin}
       /\ revokedReleased' =
              IF result = "Revoked"
              THEN revokedReleased \cup {accepted}
              ELSE revokedReleased
       /\ claimedRetained' =
              IF result = "ClaimedOrLater"
              THEN claimedRetained \cup {accepted}
              ELSE claimedRetained
       /\ released' =
              IF result = "Revoked"
              THEN released \cup {accepted}
              ELSE released
       /\ UNCHANGED <<current, phase, claimKind,
                       workerPrepared,
                       workerRevoked, workerClaimedOrLater,
                       schedulerRevokeSent, readyInFlight, readyAccepted,
                       revokeRequestInFlight, claimCrossedRevoke,
                       published, ordinarySettled>>

Start(a) ==
    /\ a \in Assignments
    /\ phase[a] = "Claimed"
    /\ phase' = [phase EXCEPT ![a] = "Started"]
    /\ UNCHANGED <<current, claimKind, workerPrepared,
                    workerRevoked, workerClaimedOrLater,
                    schedulerRevokeSent, readyInFlight, readyAccepted,
                    revokeRequestInFlight,
                    revokeResultInFlight, revokeResultKind,
                    revokeResultsDelivered, claimCrossedRevoke,
                    published, revokedReleased, claimedRetained,
                    ordinarySettled, released>>

Finish(a) ==
    /\ a \in Assignments
    /\ phase[a] = "Started"
    /\ phase' = [phase EXCEPT ![a] = "Terminal"]
    /\ UNCHANGED <<current, claimKind, workerPrepared,
                    workerRevoked, workerClaimedOrLater,
                    schedulerRevokeSent, readyInFlight, readyAccepted,
                    revokeRequestInFlight,
                    revokeResultInFlight, revokeResultKind,
                    revokeResultsDelivered, claimCrossedRevoke,
                    published, revokedReleased, claimedRetained,
                    ordinarySettled, released>>

OrdinarySettle(a) ==
    /\ a \in claimedRetained
    /\ phase[a] = "Terminal"
    /\ claimedRetained' = claimedRetained \ {a}
    /\ ordinarySettled' = ordinarySettled \cup {a}
    /\ released' = released \cup {a}
    /\ UNCHANGED <<current, phase, claimKind, workerPrepared,
                    workerRevoked, workerClaimedOrLater,
                    schedulerRevokeSent, readyInFlight, readyAccepted,
                    revokeRequestInFlight,
                    revokeResultInFlight, revokeResultKind,
                    revokeResultsDelivered, claimCrossedRevoke,
                    published, revokedReleased>>

Next ==
    \/ PrepareCurrent
    \/ AdvisoryClaimCurrent
    \/ \E a \in Assignments : PrepareAfterUnfencedClaim(a)
    \/ SwitchToNew
    \/ DeliverReady
    \/ PublishCurrent
    \/ SendRevokeCurrent
    \/ DeliverRevokeRequest
    \/ DeliverRevokeResult
    \/ \E a \in Assignments : Start(a)
    \/ \E a \in Assignments : Finish(a)
    \/ \E a \in Assignments : OrdinarySettle(a)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ current \in Assignments
    /\ phase \in [Assignments -> Phases]
    /\ claimKind \in [Assignments -> ClaimKinds]
    /\ workerPrepared \subseteq Assignments
    /\ workerRevoked \subseteq Assignments
    /\ workerClaimedOrLater \subseteq Assignments
    /\ schedulerRevokeSent \subseteq Assignments
    /\ readyInFlight \in Assignments \cup {NoA}
    /\ readyAccepted \in Assignments \cup {NoA}
    /\ revokeRequestInFlight \in Assignments \cup {NoA}
    /\ revokeResultInFlight \in Assignments \cup {NoA}
    /\ revokeResultKind \in RevokeResults \cup {NoResult}
    /\ ((revokeResultInFlight = NoA) = (revokeResultKind = NoResult))
    /\ revokeResultsDelivered \subseteq Assignments
    /\ claimCrossedRevoke \subseteq Assignments
    /\ published \subseteq Assignments
    /\ revokedReleased \subseteq Assignments
    /\ claimedRetained \subseteq Assignments
    /\ ordinarySettled \subseteq Assignments
    /\ released \subseteq Assignments

PublishedHasMatchingPrepare ==
    published \subseteq workerPrepared

RevocationTargetsSentAssignment ==
    (workerRevoked \cup workerClaimedOrLater) \subseteq schedulerRevokeSent

RevokeResultHasMatchingWorkerProof ==
    /\ revokedReleased \subseteq workerRevoked
    /\ (claimedRetained \cup ordinarySettled) \subseteq
          workerClaimedOrLater

ReleaseHasMatchingSettlement ==
    /\ released = revokedReleased \cup ordinarySettled
    /\ revokedReleased \cap ordinarySettled = {}

RevokedHasNoClaimEvidence ==
    \A a \in workerRevoked :
        /\ claimKind[a] = "None"
        /\ phase[a] = "Revoked"

ClaimedOrLaterPreservesWorkerState ==
    \A a \in workerClaimedOrLater :
        phase[a] \in {"Claimed", "Started", "Terminal"}

CrossedClaimWinsRevoke ==
    \A a \in claimCrossedRevoke :
        /\ a \notin workerRevoked
        /\ phase[a] \in {"Claimed", "Started", "Terminal"}

StaleNonceResultDoesNotReleaseCurrent ==
    (current = NewA
     /\ OldA \in revokeResultsDelivered
     /\ NewA \notin workerRevoked) =>
        NewA \notin revokedReleased

ClaimedPhaseNeverRegresses ==
    \A a \in Assignments :
        claimKind[a] # "None" =>
            phase[a] \in {"Claimed", "Started", "Terminal"}

RevokeClaimCrossingCompleted ==
    \E a \in Assignments :
        /\ a \in claimCrossedRevoke
        /\ a \in workerClaimedOrLater
        /\ a \in claimedRetained
        /\ a \notin released
        /\ phase[a] \in {"Claimed", "Started", "Terminal"}

StaleNonceResultDelivered ==
    /\ current = NewA
    /\ OldA \in revokeResultsDelivered
    /\ OldA \in revokedReleased
    /\ NewA \notin released

NoRevokeClaimCrossingWitness == ~RevokeClaimCrossingCompleted
NoStaleNonceResultWitness == ~StaleNonceResultDelivered

=============================================================================
