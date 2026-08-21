---------------- MODULE MixedVersionCompatibilityChecked ----------------
(***************************************************************************
Checked safety surface for MixedVersionCompatibility.

A cancellation may terminalize before UseCS is delivered, so claimIdentity is
legitimately None in that terminal state. Once an identity exists it must match
the per-assignment policy. This operator avoids incorrectly requiring a claim
identity for pre-delivery cancellation while retaining every other base
invariant.
***************************************************************************)
EXTENDS MixedVersionCompatibility

ClaimIdentityIfPresentMatchesPolicy ==
    claimIdentity = "None" \/ claimIdentity = IdentityOf(policy)

CompatibilitySafetyInvariant ==
    /\ TypeOK
    /\ PerAssignmentPolicyCorrect
    /\ GuaranteeMatchesPolicy
    /\ ClaimIdentityIfPresentMatchesPolicy
    /\ OldWorkerSeesOnlyOldVocabulary
    /\ OldClientSeesNoTokenField
    /\ NoGlobalCapabilityLeak
    /\ NoStartAfterRelease
    /\ TokenRejectsStaleRestartClaim
    /\ OldPeerHasNoNewHandshakeWait
    /\ OldFrameFullyConsumed
    /\ OldPeerSeesNoNewTerminalField
    /\ PolicyChosenBeforeDispatch
    /\ OldTraceRefinement
    /\ TerminalAtMostOnce
    /\ ReleaseHasTerminalToken

=============================================================================
