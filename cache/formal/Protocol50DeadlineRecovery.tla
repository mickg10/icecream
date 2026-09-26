------------------------- MODULE Protocol50DeadlineRecovery -------------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
Deadline/reset overlay for one target C-F relationship, one same-link
successor admission, and one isolated sibling relationship. CStores/FStores
select topology labels; the sibling is an abstract independent pair, not a
resource-accurate multi-link network.

Caller and F-reservation deadlines are independent.  An already-admitted F
worker may publish after caller expiry while its F lease is live, but only
before the reset fence.  Snapshot requires the worker to be quiescent and
captures the exact committed prefix. Activation fences the old worker before
snapshot. Confirm requires the C-observed ACK and unchanged prefix, then
converts eligible uncommitted consumed reservations back to replay candidates. A lost ACK can
leave the source row retained; later exact F cancellation may succeed or
fail.  C-expired rows are never newly replayed.  Fenced local retirement is
an explicitly guarded candidate, not assumed protocol behavior.

Safety is checked under arbitrary peer delay. Weak fairness alone does not
prove completion before finite deadlines; no unconditional commit-liveness
claim is made here. `UrgentExpiryRecovery` freezes logical time at first
expiry until recovery confirmation and local successor admission (or terminal
cleanup in the retirement mutant). This is an urgency abstraction, not a
measured response-time bound. Topology constants label six requested shapes;
only one target and one abstract independent sibling are modeled.
***************************************************************************)

CONSTANTS CStores, FStores, TargetC, TargetF,
          CallerDeadline, FReservationDeadline, CleanupBudget,
          DisableFencedRetirement, MutantRenewCleanup, MutantReplayExpired,
          MutantGateSibling, MutantDropRecoveryNotification,
          UrgentExpiryRecovery, AllowResetWitnessLoss, AllowResetRetry,
          AllowOldWorkerStart, MutantWriteAfterExpiry

ASSUME /\ CStores # {} /\ FStores # {}
       /\ TargetC \in CStores /\ TargetF \in FStores
       /\ Cardinality(CStores) \in 1..4
       /\ Cardinality(FStores) \in 1..4
       /\ CallerDeadline \in 1..4
       /\ FReservationDeadline \in 1..4
       /\ CleanupBudget \in 1..3
       /\ DisableFencedRetirement \in BOOLEAN
       /\ MutantRenewCleanup \in BOOLEAN
       /\ MutantReplayExpired \in BOOLEAN
       /\ MutantGateSibling \in BOOLEAN
       /\ MutantDropRecoveryNotification \in BOOLEAN
       /\ UrgentExpiryRecovery \in BOOLEAN
       /\ AllowResetWitnessLoss \in BOOLEAN
       /\ AllowResetRetry \in BOOLEAN
       /\ AllowOldWorkerStart \in BOOLEAN
       /\ MutantWriteAfterExpiry \in BOOLEAN

Jobs == {"victim", "sibling"}
NoWorker == "None"
Phases == {"Live", "ResetRequested", "Fencing", "AckPending",
           "Confirmed", "ReplayResolved", "CleanupDone"}
FLeaseStates == {"Live", "Consumed", "Cancelled", "Expired"}
EarliestDeadline == IF CallerDeadline < FReservationDeadline
                    THEN CallerDeadline ELSE FReservationDeadline

VARIABLES now, cExpired, fExpired, linkLost, recoveryNotified,
          phase, fLease, reservationConsumed,
          oldWorker, workerFenced, resetConfirmed, published, committedPrefix,
          prefixAtSnapshot, resetReplay, resetUnavailable, resetWitness,
          witnessPresent, ackConstructed, ackObserved, replaySent,
          victimBound, victimWritten, badExpiredReplay,
          cRowRetired, fCancelAccepted, fCancelAttempts, creditReleaseCount,
          cleanupActive, cleanupAnchor, cleanupDeadline, episodeDone,
          siblingProgress, successorAdmitted

vars == <<now, cExpired, fExpired, linkLost, recoveryNotified,
          phase, fLease, reservationConsumed,
          oldWorker, workerFenced, resetConfirmed, published, committedPrefix,
          prefixAtSnapshot, resetReplay, resetUnavailable, resetWitness,
          witnessPresent, ackConstructed, ackObserved, replaySent,
          victimBound, victimWritten, badExpiredReplay,
          cRowRetired, fCancelAccepted, fCancelAttempts, creditReleaseCount,
          cleanupActive, cleanupAnchor, cleanupDeadline, episodeDone,
          siblingProgress, successorAdmitted>>

Victim == "victim"
Sibling == "sibling"
SiblingExists == Cardinality(CStores) > 1 \/ Cardinality(FStores) > 1

Init ==
    /\ now = 0
    /\ cExpired = FALSE /\ fExpired = FALSE
    /\ linkLost = FALSE
    /\ recoveryNotified = FALSE
    /\ phase = "Live"
    /\ fLease = "Live"
    /\ reservationConsumed = FALSE
    /\ oldWorker = NoWorker
    /\ workerFenced = FALSE
    /\ resetConfirmed = FALSE
    /\ published = FALSE
    /\ committedPrefix = 0
    /\ prefixAtSnapshot = 0
    /\ resetReplay = FALSE /\ resetUnavailable = FALSE
    /\ resetWitness = FALSE /\ witnessPresent = FALSE
    /\ ackConstructed = FALSE /\ ackObserved = FALSE
    /\ replaySent = FALSE
    /\ victimBound = FALSE /\ victimWritten = FALSE
    /\ badExpiredReplay = FALSE
    /\ cRowRetired = FALSE
    /\ fCancelAccepted = FALSE /\ fCancelAttempts = 0
    /\ creditReleaseCount = 0
    /\ cleanupActive = FALSE /\ cleanupAnchor = 0
    /\ cleanupDeadline = 0 /\ episodeDone = FALSE
    /\ siblingProgress = FALSE
    /\ successorAdmitted = FALSE

TypeOK ==
    /\ now \in 0..7
    /\ cExpired \in BOOLEAN /\ fExpired \in BOOLEAN
    /\ linkLost \in BOOLEAN
    /\ recoveryNotified \in BOOLEAN
    /\ phase \in Phases /\ fLease \in FLeaseStates
    /\ reservationConsumed \in BOOLEAN
    /\ oldWorker \in {NoWorker, "Running"}
    /\ workerFenced \in BOOLEAN /\ resetConfirmed \in BOOLEAN
    /\ published \in BOOLEAN
    /\ committedPrefix \in 0..1 /\ prefixAtSnapshot \in 0..1
    /\ resetReplay \in BOOLEAN /\ resetUnavailable \in BOOLEAN
    /\ resetWitness \in BOOLEAN /\ witnessPresent \in BOOLEAN
    /\ ackConstructed \in BOOLEAN /\ ackObserved \in BOOLEAN
    /\ replaySent \in BOOLEAN
    /\ victimBound \in BOOLEAN /\ victimWritten \in BOOLEAN
    /\ badExpiredReplay \in BOOLEAN
    /\ cRowRetired \in BOOLEAN
    /\ fCancelAccepted \in BOOLEAN /\ fCancelAttempts \in 0..1
    /\ creditReleaseCount \in 0..1
    /\ cleanupActive \in BOOLEAN
    /\ cleanupAnchor \in 0..7 /\ cleanupDeadline \in 0..7
    /\ episodeDone \in BOOLEAN /\ siblingProgress \in BOOLEAN
    /\ successorAdmitted \in BOOLEAN

Tick ==
    /\ now < 7
    /\ (~UrgentExpiryRecovery \/ ~(cExpired \/ fExpired)
        \/ (resetConfirmed /\ (successorAdmitted \/ episodeDone
                                 \/ DisableFencedRetirement)))
    /\ now' = now + 1
    /\ cExpired' = (cExpired \/ (now + 1 >= CallerDeadline))
    /\ fExpired' = (fExpired \/ (now + 1 >= FReservationDeadline))
    /\ fLease' = IF now + 1 >= FReservationDeadline /\ fLease = "Live"
                    THEN "Expired" ELSE fLease
    /\ IF ~cleanupActive /\ ~episodeDone
          /\ now + 1 >= EarliestDeadline
          THEN /\ cleanupActive' = TRUE
               /\ cleanupAnchor' = EarliestDeadline
               /\ cleanupDeadline' = EarliestDeadline + CleanupBudget
          ELSE UNCHANGED <<cleanupActive, cleanupAnchor, cleanupDeadline>>
    /\ UNCHANGED <<linkLost, recoveryNotified, phase, reservationConsumed, oldWorker, workerFenced, resetConfirmed,
                    published, committedPrefix, prefixAtSnapshot,
                    resetReplay, resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, replaySent, victimBound,
                    victimWritten, badExpiredReplay, cRowRetired,
                    fCancelAccepted, fCancelAttempts, creditReleaseCount,
                    episodeDone, siblingProgress, successorAdmitted>>

StartOldWorker ==
    /\ AllowOldWorkerStart
    /\ phase = "Live" /\ ~cExpired /\ ~fExpired
    /\ fLease = "Live" /\ oldWorker = NoWorker
    /\ ~reservationConsumed /\ ~published
    /\ reservationConsumed' = TRUE
    /\ oldWorker' = "Running"
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, phase, fLease, workerFenced, resetConfirmed,
                    published, committedPrefix, prefixAtSnapshot,
                    resetReplay, resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, replaySent, victimBound,
                    victimWritten, badExpiredReplay, cRowRetired,
                    fCancelAccepted, fCancelAttempts, creditReleaseCount,
                    cleanupActive, cleanupAnchor, cleanupDeadline, episodeDone,
                    siblingProgress, successorAdmitted>>

FinishOldWorker ==
    /\ oldWorker = "Running" /\ ~workerFenced /\ ~fExpired
    /\ fLease = "Live"
    /\ oldWorker' = NoWorker
    /\ published' = TRUE
    /\ committedPrefix' = 1
    /\ fLease' = "Consumed"
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, phase,
                    reservationConsumed, workerFenced, resetConfirmed, prefixAtSnapshot,
                    resetReplay, resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, replaySent, victimBound,
                    victimWritten, badExpiredReplay, cRowRetired,
                    fCancelAccepted, fCancelAttempts, creditReleaseCount,
                    cleanupActive, cleanupAnchor, cleanupDeadline, episodeDone,
                    siblingProgress, successorAdmitted>>

StopOldWorkerUnpublished ==
    /\ oldWorker = "Running" /\ ~published
    /\ phase = "Fencing"
    /\ oldWorker' = NoWorker
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, phase, fLease,
                    reservationConsumed, workerFenced, resetConfirmed, published,
                    committedPrefix, prefixAtSnapshot, resetReplay,
                    resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, replaySent, victimBound,
                    victimWritten, badExpiredReplay, cRowRetired,
                    fCancelAccepted, fCancelAttempts, creditReleaseCount,
                    cleanupActive, cleanupAnchor, cleanupDeadline, episodeDone,
                    siblingProgress, successorAdmitted>>

RequestReset ==
    /\ (((cExpired \/ fExpired) /\ recoveryNotified) \/ linkLost)
    /\ phase = "Live"
    /\ phase' = "ResetRequested"
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified,
                    fLease, reservationConsumed,
                    oldWorker, workerFenced, resetConfirmed, published, committedPrefix,
                    prefixAtSnapshot, resetReplay, resetUnavailable,
                    resetWitness, witnessPresent, ackConstructed, ackObserved,
                    replaySent, victimBound, victimWritten, badExpiredReplay,
                    cRowRetired, fCancelAccepted, fCancelAttempts,
                    creditReleaseCount, cleanupActive, cleanupAnchor,
                    cleanupDeadline, episodeDone, siblingProgress, successorAdmitted>>

LoseLink ==
    /\ phase = "Live" /\ ~linkLost
    /\ (~UrgentExpiryRecovery \/ cExpired \/ fExpired)
    /\ linkLost' = TRUE
    /\ UNCHANGED <<now, cExpired, fExpired, recoveryNotified, phase, fLease,
                    reservationConsumed, oldWorker, workerFenced, resetConfirmed, published,
                    committedPrefix, prefixAtSnapshot, resetReplay,
                    resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, replaySent, victimBound,
                    victimWritten, badExpiredReplay, cRowRetired,
                    fCancelAccepted, fCancelAttempts, creditReleaseCount,
                    cleanupActive, cleanupAnchor, cleanupDeadline, episodeDone,
                    siblingProgress, successorAdmitted>>

ObserveRecoveryTrigger ==
    /\ (cExpired \/ fExpired) /\ ~recoveryNotified
    /\ ~MutantDropRecoveryNotification
    /\ recoveryNotified' = TRUE
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, phase, fLease,
                    reservationConsumed, oldWorker, workerFenced, resetConfirmed, published,
                    committedPrefix, prefixAtSnapshot, resetReplay,
                    resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, replaySent, victimBound,
                    victimWritten, badExpiredReplay, cRowRetired,
                    fCancelAccepted, fCancelAttempts, creditReleaseCount,
                    cleanupActive, cleanupAnchor, cleanupDeadline, episodeDone,
                    siblingProgress, successorAdmitted>>

ActivateReset ==
    /\ phase = "ResetRequested"
    /\ phase' = "Fencing"
    /\ workerFenced' = TRUE
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, fLease,
                    reservationConsumed, oldWorker, resetConfirmed, published, committedPrefix,
                    prefixAtSnapshot, resetReplay, resetUnavailable,
                    resetWitness, witnessPresent, ackConstructed, ackObserved,
                    replaySent, victimBound, victimWritten, badExpiredReplay,
                    cRowRetired, fCancelAccepted, fCancelAttempts,
                    creditReleaseCount, cleanupActive, cleanupAnchor,
                    cleanupDeadline, episodeDone, siblingProgress, successorAdmitted>>

ConstructResetAck ==
    /\ phase = "Fencing" /\ workerFenced /\ oldWorker = NoWorker
    /\ prefixAtSnapshot' = committedPrefix
    /\ resetReplay' = (fLease = "Live" /\ ~published)
    /\ resetUnavailable' = ~(fLease = "Live" /\ ~published)
    /\ resetWitness' = (fLease = "Live" /\ ~published)
    /\ witnessPresent' = TRUE
    /\ ackConstructed' = TRUE
    /\ ackObserved' = FALSE
    /\ phase' = "AckPending"
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, fLease,
                    reservationConsumed, oldWorker, workerFenced, resetConfirmed, published,
                    committedPrefix, replaySent, victimBound, victimWritten,
                    badExpiredReplay, cRowRetired, fCancelAccepted,
                    fCancelAttempts, creditReleaseCount, cleanupActive,
                    cleanupAnchor, cleanupDeadline, episodeDone, siblingProgress, successorAdmitted>>

ObserveResetAck ==
    /\ phase = "AckPending" /\ ackConstructed /\ ~ackObserved
    /\ ackObserved' = TRUE
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, phase, fLease,
                    reservationConsumed, oldWorker, workerFenced, resetConfirmed, published,
                    committedPrefix, prefixAtSnapshot, resetReplay,
                    resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, replaySent, victimBound, victimWritten,
                    badExpiredReplay, cRowRetired, fCancelAccepted,
                    fCancelAttempts, creditReleaseCount, cleanupActive,
                    cleanupAnchor, cleanupDeadline, episodeDone, siblingProgress, successorAdmitted>>

LoseResetWitness ==
    /\ AllowResetWitnessLoss
    /\ phase \in {"AckPending", "Confirmed", "ReplayResolved"}
    /\ witnessPresent
    /\ witnessPresent' = FALSE
    /\ resetWitness' = FALSE
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, phase, fLease,
                    reservationConsumed, oldWorker, workerFenced, resetConfirmed, published,
                    committedPrefix, prefixAtSnapshot, resetReplay,
                    resetUnavailable, ackConstructed, ackObserved,
                    replaySent, victimBound, victimWritten, badExpiredReplay,
                    cRowRetired, fCancelAccepted, fCancelAttempts,
                    creditReleaseCount, cleanupActive, cleanupAnchor,
                    cleanupDeadline, episodeDone, siblingProgress, successorAdmitted>>

ConfirmReset ==
    /\ phase = "AckPending" /\ ackObserved
    /\ oldWorker = NoWorker
    /\ committedPrefix = prefixAtSnapshot
    /\ phase' = "Confirmed"
    /\ reservationConsumed' = IF resetReplay THEN FALSE ELSE reservationConsumed
    /\ workerFenced' = FALSE
    /\ resetConfirmed' = TRUE
    /\ victimBound' = FALSE /\ victimWritten' = FALSE
    /\ replaySent' = FALSE
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, fLease, oldWorker, published,
                    committedPrefix, prefixAtSnapshot, resetReplay,
                    resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, badExpiredReplay,
                    cRowRetired, fCancelAccepted, fCancelAttempts,
                    creditReleaseCount, cleanupActive, cleanupAnchor,
                    cleanupDeadline, episodeDone, siblingProgress, successorAdmitted>>

ResolveReplay ==
    /\ phase = "Confirmed"
    /\ phase' = "ReplayResolved"
    /\ replaySent' = IF MutantReplayExpired
                         THEN resetReplay
                         ELSE resetReplay /\ ~cExpired /\ ~fExpired
                              /\ fLease = "Live"
    /\ victimBound' = replaySent'
    /\ badExpiredReplay' = (badExpiredReplay \/
          (replaySent' /\ (cExpired \/ fExpired)))
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, fLease,
                    reservationConsumed, oldWorker, workerFenced, resetConfirmed, published,
                    committedPrefix, prefixAtSnapshot, resetReplay,
                    resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, victimWritten, cRowRetired,
                    fCancelAccepted, fCancelAttempts, creditReleaseCount,
                    cleanupActive, cleanupAnchor, cleanupDeadline, episodeDone,
                    siblingProgress, successorAdmitted>>

RetryReset ==
    /\ AllowResetRetry
    /\ phase \in {"Confirmed", "ReplayResolved"}
    /\ ~cRowRetired
    /\ phase' = "ResetRequested"
    /\ IF MutantRenewCleanup /\ cleanupActive
          THEN cleanupDeadline' = now + CleanupBudget
          ELSE cleanupDeadline' = cleanupDeadline
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, fLease,
                    reservationConsumed, oldWorker, workerFenced, resetConfirmed, published,
                    committedPrefix, prefixAtSnapshot, resetReplay,
                    resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, replaySent, victimBound,
                    victimWritten, badExpiredReplay, cRowRetired,
                    fCancelAccepted, fCancelAttempts, creditReleaseCount,
                    cleanupActive, cleanupAnchor, episodeDone, siblingProgress, successorAdmitted>>

WriteReplayedVictim ==
    /\ phase = "ReplayResolved" /\ victimBound /\ ~victimWritten
    /\ fLease = "Live" /\ ~workerFenced
    /\ (~(cExpired \/ fExpired) \/ MutantWriteAfterExpiry)
    /\ victimWritten' = TRUE
    /\ badExpiredReplay' = (badExpiredReplay \/ (cExpired \/ fExpired))
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, phase, fLease,
                    reservationConsumed, oldWorker, workerFenced, resetConfirmed, published,
                    committedPrefix, prefixAtSnapshot, resetReplay,
                    resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, replaySent, victimBound,
                    cRowRetired, fCancelAccepted,
                    fCancelAttempts, creditReleaseCount, cleanupActive,
                    cleanupAnchor, cleanupDeadline, episodeDone, siblingProgress, successorAdmitted>>

RetireExpiredFenced ==
    /\ ~DisableFencedRetirement
    /\ phase \in {"Confirmed", "ReplayResolved"}
    /\ (cExpired \/ fExpired) /\ ~cRowRetired
    /\ oldWorker = NoWorker
    /\ committedPrefix = 0 /\ ~victimBound /\ ~victimWritten
    /\ ~replaySent
    /\ cRowRetired' = TRUE
    /\ creditReleaseCount' = creditReleaseCount + 1
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, phase, fLease,
                    reservationConsumed, oldWorker, workerFenced, resetConfirmed, published,
                    committedPrefix, prefixAtSnapshot, resetReplay,
                    resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, replaySent, victimBound,
                    victimWritten, badExpiredReplay, fCancelAccepted,
                    fCancelAttempts, cleanupActive, cleanupAnchor,
                    cleanupDeadline, episodeDone, siblingProgress, successorAdmitted>>

ExactFCancel ==
    /\ phase \in {"Confirmed", "ReplayResolved"}
    /\ (cExpired \/ fExpired) /\ fLease \in {"Live", "Consumed"}
    /\ fCancelAttempts = 0
    /\ fCancelAttempts' = 1
    /\ \E accepted \in {FALSE, fLease = "Live" /\ ~published}:
          /\ fCancelAccepted' = accepted
          /\ IF accepted THEN fLease' = "Cancelled" ELSE fLease' = fLease
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, phase, reservationConsumed,
                    oldWorker, workerFenced, resetConfirmed, published, committedPrefix,
                    prefixAtSnapshot, resetReplay, resetUnavailable,
                    resetWitness, witnessPresent, ackConstructed, ackObserved,
                    replaySent, victimBound, victimWritten, badExpiredReplay,
                    cRowRetired, creditReleaseCount, cleanupActive,
                    cleanupAnchor, cleanupDeadline, episodeDone, siblingProgress, successorAdmitted>>

AdmitSuccessor ==
    /\ resetConfirmed /\ cRowRetired /\ creditReleaseCount = 1
    /\ cExpired /\ ~successorAdmitted
    /\ cleanupActive /\ now <= cleanupDeadline
    /\ successorAdmitted' = TRUE
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, phase,
                    fLease, reservationConsumed, oldWorker, workerFenced,
                    resetConfirmed, published, committedPrefix, prefixAtSnapshot,
                    resetReplay, resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, replaySent, victimBound,
                    victimWritten, badExpiredReplay, cRowRetired,
                    fCancelAccepted, fCancelAttempts, creditReleaseCount,
                    cleanupActive, cleanupAnchor, cleanupDeadline, episodeDone,
                    siblingProgress>>

FinishEpisode ==
    /\ cleanupActive /\ now >= cleanupDeadline
    /\ episodeDone' = TRUE /\ cleanupActive' = FALSE
    /\ phase' = "CleanupDone"
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, fLease,
                    reservationConsumed, oldWorker, workerFenced, resetConfirmed, published,
                    committedPrefix, prefixAtSnapshot, resetReplay,
                    resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, replaySent, victimBound,
                    victimWritten, badExpiredReplay, cRowRetired,
                    fCancelAccepted, fCancelAttempts, creditReleaseCount,
                    cleanupAnchor, cleanupDeadline, siblingProgress, successorAdmitted>>

ProgressSibling ==
    /\ SiblingExists /\ ~siblingProgress
    /\ (~MutantGateSibling \/ phase \in {"Live", "CleanupDone"})
    /\ siblingProgress' = TRUE
    /\ UNCHANGED <<now, cExpired, fExpired, linkLost, recoveryNotified, phase, fLease,
                    reservationConsumed, oldWorker, workerFenced, resetConfirmed, published,
                    committedPrefix, prefixAtSnapshot, resetReplay,
                    resetUnavailable, resetWitness, witnessPresent,
                    ackConstructed, ackObserved, replaySent, victimBound,
                    victimWritten, badExpiredReplay, cRowRetired,
                    fCancelAccepted, fCancelAttempts, creditReleaseCount,
                    cleanupActive, cleanupAnchor, cleanupDeadline, episodeDone, successorAdmitted>>

StopAtHorizon == /\ now = 7 /\ UNCHANGED vars

Next == Tick \/ StartOldWorker \/ FinishOldWorker \/ StopOldWorkerUnpublished
        \/ LoseLink \/ ObserveRecoveryTrigger \/ RequestReset \/ ActivateReset
        \/ ConstructResetAck \/ ObserveResetAck
        \/ LoseResetWitness \/ ConfirmReset \/ ResolveReplay \/ RetryReset
        \/ WriteReplayedVictim \/ RetireExpiredFenced \/ AdmitSuccessor \/ ExactFCancel
        \/ FinishEpisode \/ ProgressSibling \/ StopAtHorizon

Spec == Init /\ [][Next]_vars
FairSpec == Spec /\ WF_vars(ProgressSibling) /\ WF_vars(Tick)
           /\ WF_vars(FinishEpisode)
RecoveryFairSpec == FairSpec
    /\ WF_vars(ObserveRecoveryTrigger)
    /\ WF_vars(RequestReset)
    /\ WF_vars(ActivateReset)
    /\ WF_vars(StopOldWorkerUnpublished)
    /\ WF_vars(ConstructResetAck)
    /\ WF_vars(ObserveResetAck)
    /\ WF_vars(ConfirmReset)
    /\ WF_vars(RetireExpiredFenced)
    /\ WF_vars(AdmitSuccessor)

NoExpiredReplay == ~badExpiredReplay
NoNewBindAfterExpiry == ~badExpiredReplay
CommittedPrefixMonotone == [] [committedPrefix' >= committedPrefix]_vars
ResetPrefixPreserved == committedPrefix >= prefixAtSnapshot
CommittedCancellationRejected == published => ~fCancelAccepted
RetirementRequiresFence == cRowRetired =>
    resetConfirmed
    /\ oldWorker = NoWorker /\ committedPrefix = 0
    /\ ~victimBound /\ ~victimWritten /\ creditReleaseCount = 1
ReleaseAtMostOnce == creditReleaseCount \in 0..1
CleanupNotRenewed == cleanupActive =>
    cleanupDeadline = cleanupAnchor + CleanupBudget
NoExpiredBindEvent == ~badExpiredReplay
SiblingProgressEnabled == ~siblingProgress => ENABLED ProgressSibling
SiblingEventuallyProgress == <> siblingProgress
TerminalCleanupEventually == <> episodeDone
RecoveryConfirmationEventually == <> resetConfirmed
SameLinkSuccessorEventuallyAdmitted == <> successorAdmitted
SuccessorAdmissionRequiresExpiredRowRetired == successorAdmitted => cRowRetired

=============================================================================
