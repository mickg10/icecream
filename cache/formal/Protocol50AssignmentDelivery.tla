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

VARIABLES current,
          phase, claimKind,
          workerPrepared, workerRevoked,
          schedulerRevokeSent,
          readyInFlight, readyAccepted,
          revokeRequestInFlight, revokeResultInFlight,
          published, released

vars == <<current,
          phase, claimKind,
          workerPrepared, workerRevoked,
          schedulerRevokeSent,
          readyInFlight, readyAccepted,
          revokeRequestInFlight, revokeResultInFlight,
          published, released>>

Init ==
    /\ current = OldA
    /\ phase = [a \in Assignments |-> "Absent"]
    /\ claimKind = [a \in Assignments |-> "None"]
    /\ workerPrepared = {}
    /\ workerRevoked = {}
    /\ schedulerRevokeSent = {}
    /\ readyInFlight = NoA
    /\ readyAccepted = NoA
    /\ revokeRequestInFlight = NoA
    /\ revokeResultInFlight = NoA
    /\ published = {}
    /\ released = {}

PrepareCurrent ==
    LET a == current
    IN /\ phase[a] = "Absent"
       /\ a \notin workerPrepared
       /\ readyInFlight = NoA
       /\ phase' = [phase EXCEPT ![a] = "Reserved"]
       /\ workerPrepared' = workerPrepared \cup {a}
       /\ readyInFlight' = a
       /\ UNCHANGED <<current, claimKind, workerRevoked,
                       schedulerRevokeSent, readyAccepted,
                       revokeRequestInFlight, revokeResultInFlight,
                       published, released>>

AdvisoryClaimCurrent ==
    LET a == current
    IN /\ phase[a] = "Absent"
       /\ phase' = [phase EXCEPT ![a] = "Claimed"]
       /\ claimKind' = [claimKind EXCEPT ![a] = "Unfenced"]
       /\ UNCHANGED <<current, workerPrepared, workerRevoked,
                       schedulerRevokeSent, readyInFlight, readyAccepted,
                       revokeRequestInFlight, revokeResultInFlight,
                       published, released>>

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
    /\ UNCHANGED <<current, claimKind, workerRevoked,
                    schedulerRevokeSent, readyAccepted,
                    revokeRequestInFlight, revokeResultInFlight,
                    published, released>>

SwitchToNew ==
    /\ current = OldA
    /\ current' = NewA
    /\ UNCHANGED <<phase, claimKind, workerPrepared, workerRevoked,
                    schedulerRevokeSent, readyInFlight, readyAccepted,
                    revokeRequestInFlight, revokeResultInFlight,
                    published, released>>

DeliverReady ==
    LET origin == readyInFlight
        accepted == IF MutantReadyByWireOnly
                    THEN current
                    ELSE IF origin = current THEN origin ELSE NoA
    IN /\ origin \in Assignments
       /\ readyInFlight' = NoA
       /\ readyAccepted' = accepted
       /\ UNCHANGED <<current, phase, claimKind,
                       workerPrepared, workerRevoked,
                       schedulerRevokeSent,
                       revokeRequestInFlight, revokeResultInFlight,
                       published, released>>

PublishCurrent ==
    /\ readyAccepted = current
    /\ published' = published \cup {current}
    /\ UNCHANGED <<current, phase, claimKind,
                    workerPrepared, workerRevoked,
                    schedulerRevokeSent,
                    readyInFlight, readyAccepted,
                    revokeRequestInFlight, revokeResultInFlight,
                    released>>

SendRevokeCurrent ==
    /\ revokeRequestInFlight = NoA
    /\ current \notin schedulerRevokeSent
    /\ schedulerRevokeSent' = schedulerRevokeSent \cup {current}
    /\ revokeRequestInFlight' = current
    /\ UNCHANGED <<current, phase, claimKind,
                    workerPrepared, workerRevoked,
                    readyInFlight, readyAccepted,
                    revokeResultInFlight, published, released>>

DeliverRevokeRequest ==
    LET origin == revokeRequestInFlight
        target == IF MutantRevokeRequestByWireOnly THEN current ELSE origin
    IN /\ origin \in Assignments
       /\ revokeResultInFlight = NoA
       /\ workerRevoked' = workerRevoked \cup {target}
       /\ phase' = [phase EXCEPT ![target] = "Revoked"]
       /\ revokeRequestInFlight' = NoA
       /\ revokeResultInFlight' = target
       /\ UNCHANGED <<current, claimKind, workerPrepared,
                       schedulerRevokeSent, readyInFlight, readyAccepted,
                       published, released>>

DeliverRevokeResult ==
    LET origin == revokeResultInFlight
        accepted == IF MutantRevokeResultByWireOnly THEN current ELSE origin
    IN /\ origin \in Assignments
       /\ released' = released \cup {accepted}
       /\ revokeResultInFlight' = NoA
       /\ UNCHANGED <<current, phase, claimKind,
                       workerPrepared, workerRevoked,
                       schedulerRevokeSent, readyInFlight, readyAccepted,
                       revokeRequestInFlight, published>>

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

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ current \in Assignments
    /\ phase \in [Assignments -> Phases]
    /\ claimKind \in [Assignments -> ClaimKinds]
    /\ workerPrepared \subseteq Assignments
    /\ workerRevoked \subseteq Assignments
    /\ schedulerRevokeSent \subseteq Assignments
    /\ readyInFlight \in Assignments \cup {NoA}
    /\ readyAccepted \in Assignments \cup {NoA}
    /\ revokeRequestInFlight \in Assignments \cup {NoA}
    /\ revokeResultInFlight \in Assignments \cup {NoA}
    /\ published \subseteq Assignments
    /\ released \subseteq Assignments

PublishedHasMatchingPrepare ==
    published \subseteq workerPrepared

RevocationTargetsSentAssignment ==
    workerRevoked \subseteq schedulerRevokeSent

ReleaseHasMatchingWorkerProof ==
    released \subseteq workerRevoked

ClaimedPhaseNeverRegresses ==
    \A a \in Assignments :
        claimKind[a] # "None" =>
            phase[a] \in {"Claimed", "Started", "Terminal"}

=============================================================================
