-------------------- MODULE AssignmentFenceNetworkCompaction --------------------
(***************************************************************************
Small extension of AssignmentFenceNetwork for the finite-tombstone theorem.

The base network retains F's REVOKED record.  Bounded long-run state requires
that a terminal record can eventually be compacted.  The delayed concrete
claim remains in c2f:

  enforcing/default-deny -> FRejectUnknownClaim consumes and rejects it;
  default-allow mutant   -> FDefaultAllowUnknownClaim consumes and starts it,
                            violating NoStartAfterRelease.

This is an extension of the canonical network state, not an independent
protocol model.
***************************************************************************)
EXTENDS AssignmentFenceNetwork

CompactRevoked(a) ==
    /\ a \in Assignments
    /\ phase[a] = "Terminal"
    /\ released[a]
    /\ fState[a] = "Revoked"
    /\ ~workerSlot[a]
    /\ fState' = [fState EXCEPT ![a] = "None"]
    /\ UNCHANGED <<phase, schedulerReservation, workerSlot, released,
                    releaseCause, terminalCount, readySeen, usecsDelivered,
                    claimMade, claimExact, claimedWire, claimedFull,
                    claimedToken, claimRejected, startCount,
                    startAfterRelease, revokedEnqueued, revokedConsumed,
                    beginConsumed, claimAfterRevokeQueued,
                    claimRejectedAfterFence, s2f, f2s, s2d, c2f,
                    nextF2SSeq, lastF2SConsumed, sfLive, sdLive>>

CompactionNext ==
    Next \/ \E a \in Assignments : CompactRevoked(a)

CompactionSpec ==
    Init /\ [][CompactionNext]_vars

CompactionFencedLivenessSpec ==
    /\ CompactionSpec
    /\ WF_vars(DrainS2F)
    /\ WF_vars(DrainF2S)
    /\ WF_vars(DrainS2D)
    /\ WF_vars(DrainC2F)

=============================================================================
