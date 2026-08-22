---------------------- MODULE Protocol50AssignmentOrdering ----------------------
EXTENDS Naturals, Sequences, TLC

(***************************************************************************
A one-assignment actor-local ordering lemma for Protocol 49/50.

The main Protocol50Assignment model owns assignment identity, epoch reuse,
claim compatibility, revocation proof, and compiler-child restart.  This
module covers only two transport interleavings that need separate actor-local
state:

  1. ADVISORY UseCS/claim may reach F before PREPARE.  Consuming the delayed
     matching PREPARE may validate/enrich the claim but must never regress F
     from CLAIMED to RESERVED.

  2. S may enter cancellation after PREPARE is queued while READY is already
     in flight.  Consuming that stale READY must not publish UseCS.  PREPARE
     and REVOKE remain ordered on the same S->F stream.

No timing, capacity, cache-source, environment, or compiler-pipe state is
modeled here.
***************************************************************************)

CONSTANTS MutantRegressClaimOnPrepare,
          MutantPublishAfterCancel,
          MutantRevokeClaimAsRevoked

ASSUME /\ MutantRegressClaimOnPrepare \in BOOLEAN
       /\ MutantPublishAfterCancel \in BOOLEAN
       /\ MutantRevokeClaimAsRevoked \in BOOLEAN

Modes == {"Advisory", "Enforcing"}
SPhases == {"Idle", "PrepareQueued", "Ready", "Cancelling",
            "Released", "Owned"}
FPhases == {"Absent", "Reserved", "Claimed", "Revoked"}
S2FMessages == {"Prepare", "Revoke"}
F2SMessages == {"Ready", "Revoked", "ClaimedOrLater"}
ReleaseProofs == {"None", "Revoked", "ClaimedOrLater"}

VARIABLES mode, sPhase, fPhase, s2f, f2s,
          usecsPublished, claimAccepted, claimRejected,
          cancelBeforePublication, prepareConsumed, readyObserved,
          revokeConsumed, releaseProof,
          badRegression, badPublish, badRevoked

vars == <<mode, sPhase, fPhase, s2f, f2s,
          usecsPublished, claimAccepted, claimRejected,
          cancelBeforePublication, prepareConsumed, readyObserved,
          revokeConsumed, releaseProof,
          badRegression, badPublish, badRevoked>>

Init ==
    /\ mode \in Modes
    /\ sPhase = "Idle"
    /\ fPhase = "Absent"
    /\ s2f = <<>>
    /\ f2s = <<>>
    /\ usecsPublished = FALSE
    /\ claimAccepted = FALSE
    /\ claimRejected = FALSE
    /\ cancelBeforePublication = FALSE
    /\ prepareConsumed = FALSE
    /\ readyObserved = FALSE
    /\ revokeConsumed = FALSE
    /\ releaseProof = "None"
    /\ badRegression = FALSE
    /\ badPublish = FALSE
    /\ badRevoked = FALSE

QueuePrepare ==
    /\ sPhase = "Idle"
    /\ sPhase' = "PrepareQueued"
    /\ s2f' = Append(s2f, "Prepare")
    /\ UNCHANGED <<mode, fPhase, f2s, usecsPublished,
                    claimAccepted, claimRejected,
                    cancelBeforePublication, prepareConsumed,
                    readyObserved, revokeConsumed, releaseProof,
                    badRegression, badPublish, badRevoked>>

PublishUseCS ==
    /\ ~usecsPublished
    /\ \/ /\ mode = "Advisory"
           /\ sPhase \in {"PrepareQueued", "Ready"}
       \/ /\ mode = "Enforcing"
           /\ sPhase = "Ready"
       \/ /\ MutantPublishAfterCancel
           /\ sPhase = "Cancelling"
    /\ usecsPublished' = TRUE
    /\ badPublish' = badPublish \/ cancelBeforePublication
    /\ UNCHANGED <<mode, sPhase, fPhase, s2f, f2s,
                    claimAccepted, claimRejected,
                    cancelBeforePublication, prepareConsumed,
                    readyObserved, revokeConsumed, releaseProof,
                    badRegression, badRevoked>>

QueueCancel ==
    LET cancelledBeforePublish ==
            cancelBeforePublication \/ ~usecsPublished
    IN /\ sPhase \in {"PrepareQueued", "Ready"}
       /\ sPhase' = "Cancelling"
       /\ s2f' = Append(s2f, "Revoke")
       /\ cancelBeforePublication' = cancelledBeforePublish
       /\ UNCHANGED <<mode, fPhase, f2s, usecsPublished,
                       claimAccepted, claimRejected, prepareConsumed,
                       readyObserved, revokeConsumed, releaseProof,
                       badRegression, badPublish, badRevoked>>

ConsumePrepare ==
    LET regresses == fPhase = "Claimed" /\ MutantRegressClaimOnPrepare
        nextF == IF fPhase = "Absent" \/ regresses
                 THEN "Reserved"
                 ELSE "Claimed"
    IN /\ Len(s2f) > 0
       /\ Head(s2f) = "Prepare"
       /\ fPhase \in {"Absent", "Claimed"}
       /\ s2f' = Tail(s2f)
       /\ f2s' = Append(f2s, "Ready")
       /\ fPhase' = nextF
       /\ prepareConsumed' = TRUE
       /\ badRegression' = badRegression \/ regresses
       /\ UNCHANGED <<mode, sPhase, usecsPublished,
                       claimAccepted, claimRejected,
                       cancelBeforePublication, readyObserved,
                       revokeConsumed, releaseProof,
                       badPublish, badRevoked>>

ConsumeReady ==
    /\ Len(f2s) > 0
    /\ Head(f2s) = "Ready"
    /\ f2s' = Tail(f2s)
    /\ sPhase' = IF sPhase = "PrepareQueued" THEN "Ready" ELSE sPhase
    /\ readyObserved' = TRUE
    /\ UNCHANGED <<mode, fPhase, s2f, usecsPublished,
                    claimAccepted, claimRejected,
                    cancelBeforePublication, prepareConsumed,
                    revokeConsumed, releaseProof,
                    badRegression, badPublish, badRevoked>>

AcceptClaim ==
    /\ usecsPublished
    /\ ~claimAccepted
    /\ ~claimRejected
    /\ fPhase \in {"Absent", "Reserved"}
    /\ (mode = "Advisory" \/ fPhase = "Reserved")
    /\ fPhase' = "Claimed"
    /\ claimAccepted' = TRUE
    /\ UNCHANGED <<mode, sPhase, s2f, f2s, usecsPublished,
                    claimRejected, cancelBeforePublication,
                    prepareConsumed, readyObserved, revokeConsumed,
                    releaseProof, badRegression, badPublish, badRevoked>>

RejectClaimAfterRevocation ==
    /\ usecsPublished
    /\ ~claimAccepted
    /\ ~claimRejected
    /\ fPhase = "Revoked"
    /\ claimRejected' = TRUE
    /\ UNCHANGED <<mode, sPhase, fPhase, s2f, f2s,
                    usecsPublished, claimAccepted,
                    cancelBeforePublication, prepareConsumed,
                    readyObserved, revokeConsumed, releaseProof,
                    badRegression, badPublish, badRevoked>>

ConsumeRevoke ==
    LET claimWon == fPhase = "Claimed"
        wrongRevoked == claimWon /\ MutantRevokeClaimAsRevoked
        nextF == IF claimWon /\ ~wrongRevoked THEN "Claimed" ELSE "Revoked"
        reply == IF claimWon /\ ~wrongRevoked
                 THEN "ClaimedOrLater"
                 ELSE "Revoked"
    IN /\ Len(s2f) > 0
       /\ Head(s2f) = "Revoke"
       /\ fPhase \in {"Absent", "Reserved", "Claimed"}
       /\ s2f' = Tail(s2f)
       /\ f2s' = Append(f2s, reply)
       /\ fPhase' = nextF
       /\ revokeConsumed' = TRUE
       /\ badRevoked' = badRevoked \/ wrongRevoked
       /\ UNCHANGED <<mode, sPhase, usecsPublished,
                       claimAccepted, claimRejected,
                       cancelBeforePublication, prepareConsumed,
                       readyObserved, releaseProof,
                       badRegression, badPublish>>

ConsumeRevokeResult ==
    /\ Len(f2s) > 0
    /\ Head(f2s) \in {"Revoked", "ClaimedOrLater"}
    /\ sPhase = "Cancelling"
    /\ releaseProof' = Head(f2s)
    /\ sPhase' = IF Head(f2s) = "Revoked" THEN "Released" ELSE "Owned"
    /\ f2s' = Tail(f2s)
    /\ UNCHANGED <<mode, fPhase, s2f, usecsPublished,
                    claimAccepted, claimRejected,
                    cancelBeforePublication, prepareConsumed,
                    readyObserved, revokeConsumed,
                    badRegression, badPublish, badRevoked>>

Next ==
    \/ QueuePrepare
    \/ PublishUseCS
    \/ QueueCancel
    \/ ConsumePrepare
    \/ ConsumeReady
    \/ AcceptClaim
    \/ RejectClaimAfterRevocation
    \/ ConsumeRevoke
    \/ ConsumeRevokeResult

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ mode \in Modes
    /\ sPhase \in SPhases
    /\ fPhase \in FPhases
    /\ s2f \in Seq(S2FMessages)
    /\ f2s \in Seq(F2SMessages)
    /\ usecsPublished \in BOOLEAN
    /\ claimAccepted \in BOOLEAN
    /\ claimRejected \in BOOLEAN
    /\ cancelBeforePublication \in BOOLEAN
    /\ prepareConsumed \in BOOLEAN
    /\ readyObserved \in BOOLEAN
    /\ revokeConsumed \in BOOLEAN
    /\ releaseProof \in ReleaseProofs
    /\ badRegression \in BOOLEAN
    /\ badPublish \in BOOLEAN
    /\ badRevoked \in BOOLEAN

QueueBound == Len(s2f) <= 2 /\ Len(f2s) <= 2

ClaimNeverRegresses ==
    /\ ~badRegression
    /\ (claimAccepted => fPhase = "Claimed")

CancelCutsPublication ==
    /\ ~badPublish
    /\ (cancelBeforePublication => ~usecsPublished)

ClaimWinsRevoke ==
    /\ ~badRevoked
    /\ (claimAccepted => fPhase # "Revoked")

ReleaseHasProof ==
    sPhase = "Released" =>
        /\ releaseProof = "Revoked"
        /\ fPhase = "Revoked"
        /\ ~claimAccepted

OwnedHasClaim ==
    sPhase = "Owned" =>
        /\ releaseProof = "ClaimedOrLater"
        /\ claimAccepted
        /\ fPhase = "Claimed"

EnforcingPublicationObservedReady ==
    mode # "Enforcing" \/ ~usecsPublished \/ readyObserved

EarlyCancelCompleted ==
    /\ mode = "Enforcing"
    /\ sPhase = "Released"
    /\ prepareConsumed
    /\ readyObserved
    /\ revokeConsumed
    /\ ~usecsPublished
    /\ fPhase = "Revoked"

AdvisoryClaimBeforePrepareCompleted ==
    /\ mode = "Advisory"
    /\ claimAccepted
    /\ prepareConsumed
    /\ fPhase = "Claimed"

NoEarlyCancelWitness == ~EarlyCancelCompleted
NoAdvisoryClaimBeforePrepareWitness == ~AdvisoryClaimBeforePrepareCompleted

Safety ==
    /\ TypeOK
    /\ QueueBound
    /\ ClaimNeverRegresses
    /\ CancelCutsPublication
    /\ ClaimWinsRevoke
    /\ ReleaseHasProof
    /\ OwnedHasClaim
    /\ EnforcingPublicationObservedReady

=============================================================================
