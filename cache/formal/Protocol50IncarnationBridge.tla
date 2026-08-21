---------------------- MODULE Protocol50IncarnationBridge ----------------------
EXTENDS Naturals, TLC

(***************************************************************************
Small composition model for the cold-incarnation recovery boundary that
neither Protocol50.tla nor Protocol50JobLifecycle.tla should absorb:

    C retains one immutable PreparedTU / active transaction identity
    F_STORE_GUID changes destructively either:
      * while the old transaction is still in flight, or
      * after F durably committed exact input but before C accepted it
    C establishes a cold route and retries history-independently

Verified F identity replacement is the explicit exception to the ordinary rule
that a durable unaccepted commit may not be abandoned.  It is not represented
as unrestricted TX_ABORTED.

The bridge also records the job-side distinction:

  * a waiting P50 attachment is cancelled by cache-incarnation loss;
  * a compiler already authorized with independent exact input may finish.
***************************************************************************)

CONSTANTS OldGuid, NewGuid, TU, NoTU, MutantLoseRetryOnReplacement

ASSUME /\ OldGuid # NewGuid
       /\ TU # NoTU
       /\ MutantLoseRetryOnReplacement \in BOOLEAN

RecoveryPhases ==
    {"OldInFlight", "AwaitingOldAck", "NeedRoute", "ReadyToSend",
     "RetryInFlight", "RetryDurable", "RetryAccepted", "DoneOld"}
Acceptances == {"None", "Old", "Retry"}
RetryPendingPhases ==
    {"NeedRoute", "ReadyToSend", "RetryInFlight", "RetryDurable"}
ReplacementEligiblePhases == {"OldInFlight", "AwaitingOldAck"}

VARIABLE s
vars == <<s>>

Init ==
    s = [fGuid                    |-> OldGuid,
         cSeenGuid                |-> OldGuid,
         prepared                 |-> TRUE,
         cActive                  |-> TU,
         oldCommitDurable         |-> FALSE,
         oldInputPresent          |-> FALSE,
         waitingP50               |-> TRUE,
         compilerAuthorized       |-> FALSE,
         compilerFinished         |-> FALSE,
         resultAccepted           |-> FALSE,
         replacementObserved      |-> FALSE,
         authorizedAtReplacement  |-> FALSE,
         retryHistoryIndependent  |-> FALSE,
         retryInputPresent        |-> FALSE,
         recovery                 |-> "OldInFlight",
         cacheAccepted            |-> "None"]

CommitOldInput ==
    /\ s.recovery = "OldInFlight"
    /\ s.fGuid = OldGuid
    /\ s.cActive = TU
    /\ s' = [s EXCEPT
                 !.oldCommitDurable = TRUE,
                 !.oldInputPresent = TRUE,
                 !.recovery = "AwaitingOldAck"]

AuthorizeCompiler ==
    /\ s.fGuid = OldGuid
    /\ s.oldInputPresent
    /\ s.waitingP50
    /\ ~s.compilerAuthorized
    /\ s' = [s EXCEPT
                 !.compilerAuthorized = TRUE,
                 !.waitingP50 = FALSE]

AcceptOldCommit ==
    /\ s.recovery = "AwaitingOldAck"
    /\ s.oldCommitDurable
    /\ s.cActive = TU
    /\ ~s.replacementObserved
    /\ s' = [s EXCEPT
                 !.cacheAccepted = "Old",
                 !.cActive = NoTU,
                 !.waitingP50 = FALSE,
                 !.recovery = "DoneOld"]

F_STORE_INCAR_REPLACED ==
    /\ s.recovery \in ReplacementEligiblePhases
    /\ s.fGuid = OldGuid
    /\ s.cSeenGuid = OldGuid
    /\ s.cActive = TU
    /\ s.cacheAccepted = "None"
    /\ IF s.recovery = "AwaitingOldAck"
          THEN /\ s.oldCommitDurable
               /\ s.oldInputPresent
          ELSE /\ ~s.oldCommitDurable
               /\ ~s.oldInputPresent
    /\ s' = [s EXCEPT
                 !.fGuid = NewGuid,
                 !.cSeenGuid = NewGuid,
                 !.oldCommitDurable = FALSE,
                 !.oldInputPresent = FALSE,
                 !.waitingP50 = FALSE,
                 !.replacementObserved = TRUE,
                 !.authorizedAtReplacement = s.compilerAuthorized,
                 !.cActive =
                    IF MutantLoseRetryOnReplacement THEN NoTU ELSE TU,
                 !.recovery = "NeedRoute"]

ESTABLISH_COLD_ROUTE ==
    /\ s.recovery = "NeedRoute"
    /\ s.fGuid = NewGuid
    /\ s.cSeenGuid = NewGuid
    /\ s.prepared
    /\ s.cActive = TU
    /\ s' = [s EXCEPT !.recovery = "ReadyToSend"]

REISSUE_HISTORY_INDEPENDENT ==
    /\ s.recovery = "ReadyToSend"
    /\ s.fGuid = NewGuid
    /\ s.cSeenGuid = NewGuid
    /\ s.prepared
    /\ s.cActive = TU
    /\ s' = [s EXCEPT
                 !.retryHistoryIndependent = TRUE,
                 !.recovery = "RetryInFlight"]

COMMIT_RETRY ==
    /\ s.recovery = "RetryInFlight"
    /\ s.retryHistoryIndependent
    /\ s.fGuid = NewGuid
    /\ s.cActive = TU
    /\ s' = [s EXCEPT
                 !.retryInputPresent = TRUE,
                 !.recovery = "RetryDurable"]

ACCEPT_RETRY_COMMIT ==
    /\ s.recovery = "RetryDurable"
    /\ s.retryInputPresent
    /\ s.retryHistoryIndependent
    /\ s.fGuid = NewGuid
    /\ s.cSeenGuid = NewGuid
    /\ s.cActive = TU
    /\ s' = [s EXCEPT
                 !.cacheAccepted = "Retry",
                 !.cActive = NoTU,
                 !.recovery = "RetryAccepted"]

FinishAuthorizedCompiler ==
    /\ s.compilerAuthorized
    /\ ~s.compilerFinished
    /\ s' = [s EXCEPT !.compilerFinished = TRUE]

AcceptCompilerResult ==
    /\ s.compilerAuthorized
    /\ s.compilerFinished
    /\ ~s.resultAccepted
    /\ s' = [s EXCEPT !.resultAccepted = TRUE]

Next ==
    \/ CommitOldInput
    \/ AuthorizeCompiler
    \/ AcceptOldCommit
    \/ F_STORE_INCAR_REPLACED
    \/ ESTABLISH_COLD_ROUTE
    \/ REISSUE_HISTORY_INDEPENDENT
    \/ COMMIT_RETRY
    \/ ACCEPT_RETRY_COMMIT
    \/ FinishAuthorizedCompiler
    \/ AcceptCompilerResult

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ s.fGuid \in {OldGuid, NewGuid}
    /\ s.cSeenGuid \in {OldGuid, NewGuid}
    /\ s.prepared \in BOOLEAN
    /\ s.cActive \in {TU, NoTU}
    /\ s.oldCommitDurable \in BOOLEAN
    /\ s.oldInputPresent \in BOOLEAN
    /\ s.waitingP50 \in BOOLEAN
    /\ s.compilerAuthorized \in BOOLEAN
    /\ s.compilerFinished \in BOOLEAN
    /\ s.resultAccepted \in BOOLEAN
    /\ s.replacementObserved \in BOOLEAN
    /\ s.authorizedAtReplacement \in BOOLEAN
    /\ s.retryHistoryIndependent \in BOOLEAN
    /\ s.retryInputPresent \in BOOLEAN
    /\ s.recovery \in RecoveryPhases
    /\ s.cacheAccepted \in Acceptances

DurableCommitHasReconciliationWitness ==
    ~s.oldCommitDurable \/
        /\ s.fGuid = OldGuid
        /\ s.cSeenGuid = OldGuid
        /\ s.prepared
        /\ s.cActive = TU
        /\ s.recovery = "AwaitingOldAck"
        /\ s.cacheAccepted = "None"

ReplacementPreservesRetryIdentity ==
    ~s.replacementObserved \/
    s.recovery \notin RetryPendingPhases \/
        /\ s.prepared
        /\ s.cActive = TU
        /\ s.fGuid = NewGuid
        /\ s.cSeenGuid = NewGuid

ReplacementClearsOldIncarnationState ==
    s.replacementObserved =>
        /\ ~s.oldCommitDurable
        /\ ~s.oldInputPresent
        /\ s.fGuid = NewGuid
        /\ s.cSeenGuid = NewGuid

ReplacementCancelsWaitingP50 ==
    s.replacementObserved => ~s.waitingP50

AuthorizedCompilerSurvivesReplacement ==
    s.replacementObserved /\ s.authorizedAtReplacement =>
        s.compilerAuthorized

AcceptedCompilerResultWasAuthorized ==
    s.resultAccepted =>
        /\ s.compilerAuthorized
        /\ s.compilerFinished

OldCommitCannotBeAcceptedAfterReplacement ==
    s.replacementObserved => s.cacheAccepted # "Old"

RetryAcceptanceUsesNewIncarnation ==
    s.cacheAccepted = "Retry" =>
        /\ s.fGuid = NewGuid
        /\ s.cSeenGuid = NewGuid
        /\ s.retryHistoryIndependent
        /\ s.retryInputPresent
        /\ s.cActive = NoTU
        /\ s.recovery = "RetryAccepted"

RetryStateIsHistoryIndependent ==
    s.recovery \in {"RetryInFlight", "RetryDurable", "RetryAccepted"} =>
        s.retryHistoryIndependent

RecoveryInit ==
    s = [fGuid                    |-> NewGuid,
         cSeenGuid                |-> NewGuid,
         prepared                 |-> TRUE,
         cActive                  |-> TU,
         oldCommitDurable         |-> FALSE,
         oldInputPresent          |-> FALSE,
         waitingP50               |-> FALSE,
         compilerAuthorized       |-> FALSE,
         compilerFinished         |-> FALSE,
         resultAccepted           |-> FALSE,
         replacementObserved      |-> TRUE,
         authorizedAtReplacement  |-> FALSE,
         retryHistoryIndependent  |-> FALSE,
         retryInputPresent        |-> FALSE,
         recovery                 |-> "NeedRoute",
         cacheAccepted            |-> "None"]

RecoveryNext ==
    \/ ESTABLISH_COLD_ROUTE
    \/ REISSUE_HISTORY_INDEPENDENT
    \/ COMMIT_RETRY
    \/ ACCEPT_RETRY_COMMIT

RecoverySpec ==
    /\ RecoveryInit
    /\ [][RecoveryNext]_vars
    /\ WF_vars(ESTABLISH_COLD_ROUTE)
    /\ WF_vars(REISSUE_HISTORY_INDEPENDENT)
    /\ WF_vars(COMMIT_RETRY)
    /\ WF_vars(ACCEPT_RETRY_COMMIT)

RetryEventuallyAccepted ==
    <> (s.cacheAccepted = "Retry")

=============================================================================
