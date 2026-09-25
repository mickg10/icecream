------------------------- MODULE Protocol50ActiveCancel -------------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
Focused bounded lifecycle model for an F-accepted cancellation racing a
materialization worker and a later RESET/replay.  Job slots are immutable job
identities; ordinals are the mutable wire positions assigned to those jobs.
This model complements, and does not replace, Protocol50PipelineRecovery's
full-action topology rows.

The witness has four jobs: job 1 commits before recovery, job 2 is canceled
while its worker is active, and jobs 3/4 survive the RESET snapshot.  The old
worker's physical charge is independent of the logical route and remains held
until that worker completes stale.  The replacement epoch may begin work
before that late worker releases its charge.
***************************************************************************)

CONSTANT MutantDropReplayBacklog
ASSUME MutantDropReplayBacklog \in BOOLEAN

Jobs == 1..4
NoJob == 0
NoWorker == 0
JobStates == {"Reserved", "Sent", "Working", "Committed", "Observed",
              "Unavailable"}
Phases == {"Live", "ResetPending", "ConfirmPending", "Recovering",
           "Retired"}

VARIABLES state, cOrdinal, ordinalJob, FReservations, FPublished,
          worker, workerCharge, lateWorkers, acceptedCancel, callerLost,
          rawCredit, creditReleaseCount, A, K, S, history, phase,
          resetAck, resetAckSeen, resetConfirmSent, resetConfirmApplied,
          resetConfirmEcho, resetConfirmed, replayBacklog, replayInFlight,
          interruptedReplay, badPublication

vars == <<state, cOrdinal, ordinalJob, FReservations, FPublished,
          worker, workerCharge, lateWorkers, acceptedCancel, callerLost,
          rawCredit, creditReleaseCount, A, K, S, history, phase,
          resetAck, resetAckSeen, resetConfirmSent, resetConfirmApplied,
          resetConfirmEcho, resetConfirmed, replayBacklog, replayInFlight,
          interruptedReplay, badPublication>>

SuffixAtReset == {j \in Jobs : cOrdinal[j] > K}
SnapshotReplay == FReservations \cap SuffixAtReset
SnapshotUnavailable == SuffixAtReset \ SnapshotReplay

Init ==
    /\ state = [j \in Jobs |-> "Reserved"]
    /\ cOrdinal = [j \in Jobs |-> j]
    /\ ordinalJob = [n \in Jobs |-> n]
    /\ FReservations = Jobs
    /\ FPublished = {}
    /\ worker = NoWorker
    /\ workerCharge = {}
    /\ lateWorkers = {}
    /\ acceptedCancel = {}
    /\ callerLost = {}
    /\ rawCredit = {<<1, "owned">>, <<2, "owned">>,
                    <<3, "owned">>, <<4, "owned">>}
    /\ creditReleaseCount = [j \in Jobs |-> 0]
    /\ A = 0
    /\ K = 0
    /\ S = 0
    /\ history = 0
    /\ phase = "Live"
    /\ resetAck = [valid |-> FALSE, prefix |-> 0, priorP |-> 0,
                   replay |-> {}, unavailable |-> {}, history |-> 0,
                   witness |-> <<>>]
    /\ resetAckSeen = "NotSeen"
    /\ resetConfirmSent = FALSE
    /\ resetConfirmApplied = FALSE
    /\ resetConfirmEcho = FALSE
    /\ resetConfirmed = "NotConfirmed"
    /\ replayBacklog = {}
    /\ replayInFlight = {}
    /\ interruptedReplay = {}
    /\ badPublication = FALSE

TypeOK ==
    /\ state \in [Jobs -> JobStates]
    /\ cOrdinal \in [Jobs -> 0..4]
    /\ ordinalJob \in [1..4 -> Jobs \cup {NoJob}]
    /\ FReservations \subseteq Jobs
    /\ FPublished \subseteq Jobs
    /\ worker \in Jobs \cup {NoWorker}
    /\ workerCharge \subseteq Jobs
    /\ lateWorkers \subseteq Jobs
    /\ acceptedCancel \subseteq Jobs
    /\ callerLost \subseteq Jobs
    /\ rawCredit \subseteq {<<j, "owned">> : j \in Jobs}
    /\ creditReleaseCount \in [Jobs -> 0..1]
    /\ A \in 0..4
    /\ K \in 0..4
    /\ S \in 0..4
    /\ history \in 0..2
    /\ phase \in Phases
    /\ resetAck.valid \in BOOLEAN
    /\ resetAck.prefix \in 0..4
    /\ resetAck.priorP \in 0..4
    /\ resetAck.replay \subseteq Jobs
    /\ resetAck.unavailable \subseteq Jobs
    /\ resetAck.history \in 0..2
    /\ resetAck.witness \in {<<>>, <<1,2,3,4>>}
    /\ resetAckSeen \in {"NotSeen", "Lost", "Replayed"}
    /\ resetConfirmSent \in BOOLEAN
    /\ resetConfirmApplied \in BOOLEAN
    /\ resetConfirmEcho \in BOOLEAN
    /\ resetConfirmed \in {"NotConfirmed", "EchoLost", "Confirmed",
                            "ConfirmedAfterEchoLoss"}
    /\ replayBacklog \subseteq Jobs
    /\ replayInFlight \subseteq Jobs
    /\ interruptedReplay \subseteq Jobs
    /\ badPublication \in BOOLEAN

ReserveAll ==
    /\ phase = "Live"
    /\ S = 0
    /\ S' = S
    /\ UNCHANGED <<state, cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    callerLost, rawCredit, creditReleaseCount, A, K, history,
                    phase, resetAck, resetAckSeen, resetConfirmSent,
                    resetConfirmApplied, resetConfirmEcho, resetConfirmed,
                    replayBacklog, replayInFlight, interruptedReplay,
                    badPublication>>

SendNext ==
    /\ phase = "Live"
    /\ S < 4
    /\ LET j == ordinalJob[S + 1]
       IN /\ j \in FReservations
          /\ state' = [state EXCEPT ![j] = "Sent"]
    /\ S' = S + 1
    /\ UNCHANGED <<cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    callerLost, rawCredit, creditReleaseCount, A, K, history,
                    phase, resetAck, resetAckSeen, resetConfirmSent,
                    resetConfirmApplied, resetConfirmEcho, resetConfirmed,
                    replayBacklog, replayInFlight, interruptedReplay,
                    badPublication>>

StartWorker ==
    /\ phase \in {"Live", "Recovering"}
    /\ worker = NoWorker
    /\ K < S
    /\ LET j == ordinalJob[K + 1]
       IN /\ j \in FReservations
          /\ state[j] = "Sent"
          /\ worker' = j
          /\ workerCharge' = workerCharge \cup {j}
          /\ state' = [state EXCEPT ![j] = "Working"]
    /\ UNCHANGED <<cOrdinal, ordinalJob, FReservations, FPublished,
                    lateWorkers, acceptedCancel, callerLost, rawCredit,
                    creditReleaseCount, A, K, S, history, phase, resetAck,
                    resetAckSeen, resetConfirmSent, resetConfirmApplied,
                    resetConfirmEcho, resetConfirmed, replayBacklog,
                    replayInFlight, interruptedReplay, badPublication>>

PublishWorker ==
    /\ worker # NoWorker
    /\ worker \notin acceptedCancel
    /\ state[worker] = "Working"
    /\ FReservations' = FReservations \ {worker}
    /\ FPublished' = FPublished \cup {worker}
    /\ state' = [state EXCEPT ![worker] = "Committed"]
    /\ K' = K + 1
    /\ worker' = NoWorker
    /\ workerCharge' = workerCharge \ {worker}
    /\ UNCHANGED <<cOrdinal, ordinalJob, lateWorkers, acceptedCancel,
                    callerLost, rawCredit, creditReleaseCount, A, S, history,
                    phase, resetAck, resetAckSeen, resetConfirmSent,
                    resetConfirmApplied, resetConfirmEcho, resetConfirmed,
                    replayBacklog, replayInFlight, interruptedReplay,
                    badPublication>>

ObserveCommit ==
    /\ A < K
    /\ LET j == ordinalJob[A + 1]
       IN /\ j \in FPublished
          /\ A' = A + 1
          /\ state' = [state EXCEPT ![j] = "Observed"]
          /\ rawCredit' = rawCredit \ {<<j, "owned">>}
          /\ creditReleaseCount' =
                 [creditReleaseCount EXCEPT ![j] = @ + 1]
    /\ UNCHANGED <<cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    callerLost, K, S, history,
                    phase, resetAck, resetAckSeen, resetConfirmSent,
                    resetConfirmApplied, resetConfirmEcho, resetConfirmed,
                    replayBacklog, replayInFlight, interruptedReplay,
                    badPublication>>

LoseCaller(j) ==
    /\ j \in Jobs
    /\ j \notin callerLost
    /\ callerLost' = callerLost \cup {j}
    /\ UNCHANGED <<state, cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    rawCredit, creditReleaseCount, A, K, S, history, phase,
                    resetAck, resetAckSeen, resetConfirmSent,
                    resetConfirmApplied, resetConfirmEcho, resetConfirmed,
                    replayBacklog, replayInFlight, interruptedReplay,
                    badPublication>>

AcceptFActiveCancel ==
    /\ phase = "Live"
    /\ worker = 2
    /\ state[2] = "Working"
    /\ 2 \in callerLost
    /\ 2 \in FReservations
    /\ acceptedCancel' = acceptedCancel \cup {2}
    /\ FReservations' = FReservations \ {2}
    /\ worker' = NoWorker
    /\ lateWorkers' = lateWorkers \cup {2}
    /\ phase' = "Recovering"
    /\ state' = [state EXCEPT ![2] = "Sent"]
    /\ UNCHANGED <<cOrdinal, ordinalJob, FPublished, workerCharge,
                    callerLost, rawCredit, creditReleaseCount, A, K, S,
                    history, resetAck, resetAckSeen,
                    resetConfirmSent, resetConfirmApplied, resetConfirmEcho,
                    resetConfirmed, replayBacklog, replayInFlight,
                    interruptedReplay, badPublication>>

CompleteLateWorker ==
    /\ 2 \in lateWorkers
    /\ lateWorkers' = lateWorkers \ {2}
    /\ workerCharge' = workerCharge \ {2}
    /\ IF 2 \in FPublished
          THEN badPublication' = TRUE
          ELSE badPublication' = badPublication
    /\ UNCHANGED <<state, cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, acceptedCancel, callerLost, rawCredit,
                    creditReleaseCount, A, K, S, history, phase, resetAck,
                    resetAckSeen, resetConfirmSent, resetConfirmApplied,
                    resetConfirmEcho, resetConfirmed, replayBacklog,
                    replayInFlight, interruptedReplay>>

ApplyReset ==
    /\ phase = "Recovering"
    /\ A = 1
    /\ K = 1
    /\ worker = NoWorker
    /\ 2 \in lateWorkers
    /\ S = 4
    /\ cOrdinal = [j \in Jobs |-> j]
    /\ ordinalJob = [n \in 1..4 |-> n]
    /\ resetAck' = [valid |-> TRUE, prefix |-> K, priorP |-> S,
                    replay |-> SnapshotReplay,
                    unavailable |-> SnapshotUnavailable,
                    history |-> history + 1,
                    witness |-> <<1,2,3,4>>]
    /\ history' = history + 1
    /\ phase' = "ResetPending"
    /\ resetAckSeen' = "NotSeen"
    /\ resetConfirmSent' = FALSE
    /\ resetConfirmApplied' = FALSE
    /\ resetConfirmEcho' = FALSE
    /\ resetConfirmed' = "NotConfirmed"
    /\ UNCHANGED <<state, cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    callerLost, rawCredit, creditReleaseCount, A, K, S,
                    replayBacklog, replayInFlight, interruptedReplay,
                    badPublication>>

LoseResetAck ==
    /\ phase = "ResetPending"
    /\ resetAck.valid
    /\ resetAckSeen = "NotSeen"
    /\ resetAckSeen' = "Lost"
    /\ UNCHANGED <<state, cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    callerLost, rawCredit, creditReleaseCount, A, K, S,
                    history, phase, resetAck, resetConfirmSent,
                    resetConfirmApplied, resetConfirmEcho, resetConfirmed,
                    replayBacklog, replayInFlight, interruptedReplay,
                    badPublication>>

RetryResetAck ==
    /\ phase = "ResetPending"
    /\ resetAck.valid
    /\ resetAckSeen = "Lost"
    /\ resetAckSeen' = "Replayed"
    /\ UNCHANGED <<state, cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    callerLost, rawCredit, creditReleaseCount, A, K, S,
                    history, phase, resetAck, resetConfirmSent,
                    resetConfirmApplied, resetConfirmEcho, resetConfirmed,
                    replayBacklog, replayInFlight, interruptedReplay,
                    badPublication>>

ObserveResetAck ==
    /\ phase = "ResetPending"
    /\ resetAckSeen = "Replayed"
    /\ resetAck.valid
    /\ resetAck.prefix = A
    /\ resetAck.priorP = 4
    /\ resetAck.history = history
    /\ resetAck.witness = <<1, 2, 3, 4>>
    /\ resetAck.replay = {3, 4}
    /\ resetAck.unavailable = {2}
    /\ phase' = "ConfirmPending"
    /\ state' = [state EXCEPT ![2] = "Unavailable",
                              ![3] = "Reserved",
                              ![4] = "Reserved"]
    /\ creditReleaseCount' = [creditReleaseCount EXCEPT ![2] = @ + 1]
    /\ rawCredit' = rawCredit \ {<<2, "owned">>}
    /\ cOrdinal' = [j \in Jobs |-> CASE j = 1 -> 1
                                           [] j = 2 -> 0
                                           [] j = 3 -> 2
                                           [] j = 4 -> 3]
    /\ ordinalJob' = [n \in 1..4 |-> CASE n = 1 -> 1
                                              [] n = 2 -> 3
                                              [] n = 3 -> 4
                                              [] n = 4 -> NoJob]
    /\ S' = K
    /\ replayBacklog' = resetAck.replay
    /\ replayInFlight' = {}
    /\ interruptedReplay' = {}
    /\ UNCHANGED <<FReservations, FPublished, worker, workerCharge,
                    lateWorkers, acceptedCancel, callerLost, A, K, history,
                    resetAck, resetAckSeen, resetConfirmSent,
                    resetConfirmApplied, resetConfirmEcho, resetConfirmed,
                    badPublication>>

SendResetConfirm ==
    /\ phase = "ConfirmPending"
    /\ resetConfirmed \in {"NotConfirmed", "EchoLost"}
    /\ resetConfirmSent' = TRUE
    /\ resetConfirmEcho' = FALSE
    /\ UNCHANGED <<state, cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    callerLost, rawCredit, creditReleaseCount, A, K, S,
                    history, phase, resetAck, resetAckSeen,
                    resetConfirmApplied, resetConfirmed, replayBacklog,
                    replayInFlight, interruptedReplay, badPublication>>

ApplyResetConfirm ==
    /\ phase = "ConfirmPending"
    /\ resetConfirmSent
    /\ resetConfirmApplied' = TRUE
    /\ resetConfirmEcho' = TRUE
    /\ UNCHANGED <<state, cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    callerLost, rawCredit, creditReleaseCount, A, K, S,
                    history, phase, resetAck, resetAckSeen,
                    resetConfirmSent, resetConfirmed, replayBacklog,
                    replayInFlight, interruptedReplay, badPublication>>

LoseResetConfirmEcho ==
    /\ phase = "ConfirmPending"
    /\ resetConfirmApplied
    /\ resetConfirmEcho
    /\ resetConfirmEcho' = FALSE
    /\ resetConfirmSent' = FALSE
    /\ resetConfirmed' = "EchoLost"
    /\ UNCHANGED <<state, cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    callerLost, rawCredit, creditReleaseCount, A, K, S,
                    history, phase, resetAck, resetAckSeen,
                    resetConfirmApplied, replayBacklog,
                    replayInFlight, interruptedReplay, badPublication>>

ObserveResetConfirmEcho ==
    /\ phase = "ConfirmPending"
    /\ resetConfirmEcho
    /\ resetConfirmApplied
    /\ resetConfirmed' = IF resetConfirmed = "EchoLost"
                            THEN "ConfirmedAfterEchoLoss"
                            ELSE "Confirmed"
    /\ phase' = "Recovering"
    /\ resetConfirmEcho' = FALSE
    /\ UNCHANGED <<state, cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    callerLost, rawCredit, creditReleaseCount, A, K, S,
                    history, resetAck, resetAckSeen, resetConfirmSent,
                    resetConfirmApplied, replayBacklog, replayInFlight,
                    interruptedReplay, badPublication>>

EmitReplay(j) ==
    /\ phase = "Recovering"
    /\ j \in replayBacklog
    /\ replayInFlight = {}
    /\ cOrdinal[j] > 0
    /\ cOrdinal[j] = K + 1
    /\ ordinalJob[K + 1] = j
    /\ S = K
    /\ replayInFlight' = {j}
    /\ S' = S + 1
    /\ state' = [state EXCEPT ![j] = "Sent"]
    /\ UNCHANGED <<cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    callerLost, rawCredit, creditReleaseCount, A, K, history,
                    phase, resetAck, resetAckSeen, resetConfirmSent,
                    resetConfirmApplied, resetConfirmEcho, resetConfirmed,
                    replayBacklog, interruptedReplay, badPublication>>

StartReplayWorker(j) ==
    /\ phase = "Recovering"
    /\ j \in replayInFlight
    /\ worker = NoWorker
    /\ j \in FReservations
    /\ state[j] = "Sent"
    /\ worker' = j
    /\ workerCharge' = workerCharge \cup {j}
    /\ state' = [state EXCEPT ![j] = "Working"]
    /\ UNCHANGED <<cOrdinal, ordinalJob, FReservations, FPublished,
                    lateWorkers, acceptedCancel, callerLost, rawCredit,
                    creditReleaseCount, A, K, S, history, phase, resetAck,
                    resetAckSeen, resetConfirmSent, resetConfirmApplied,
                    resetConfirmEcho, resetConfirmed, replayBacklog,
                    replayInFlight, interruptedReplay, badPublication>>

PublishReplay ==
    /\ phase = "Recovering"
    /\ worker # NoWorker
    /\ worker \in replayInFlight
    /\ worker \notin acceptedCancel
    /\ state[worker] = "Working"
    /\ FReservations' = FReservations \ {worker}
    /\ FPublished' = FPublished \cup {worker}
    /\ state' = [state EXCEPT ![worker] = "Committed"]
    /\ K' = K + 1
    /\ worker' = NoWorker
    /\ workerCharge' = workerCharge \ {worker}
    /\ UNCHANGED <<cOrdinal, ordinalJob, lateWorkers, acceptedCancel,
                    callerLost, rawCredit, creditReleaseCount, A, S, history,
                    phase, resetAck, resetAckSeen, resetConfirmSent,
                    resetConfirmApplied, resetConfirmEcho, resetConfirmed,
                    replayBacklog, replayInFlight, interruptedReplay,
                    badPublication>>

ObserveReplayCommit ==
    /\ phase = "Recovering"
    /\ A < K
    /\ LET j == ordinalJob[A + 1]
       IN /\ j \in replayInFlight
          /\ j \in FPublished
          /\ A' = A + 1
          /\ state' = [state EXCEPT ![j] = "Observed"]
          /\ rawCredit' = rawCredit \ {<<j, "owned">>}
          /\ creditReleaseCount' =
                 [creditReleaseCount EXCEPT ![j] = @ + 1]
          /\ replayBacklog' = replayBacklog \ {j}
          /\ replayInFlight' = {}
    /\ UNCHANGED <<cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    callerLost, K, S, history,
                    phase, resetAck, resetAckSeen, resetConfirmSent,
                    resetConfirmApplied, resetConfirmEcho, resetConfirmed,
                    interruptedReplay, badPublication>>

InterruptReplay ==
    /\ phase = "Recovering"
    /\ replayInFlight # {}
    /\ worker = NoWorker
    /\ LET j == CHOOSE x \in replayInFlight : TRUE
       IN /\ phase' = "Recovering"
          /\ S' = K
          /\ replayInFlight' = {}
          \* The retained backlog abstracts recovery bookkeeping across this
          \* interruption; a second RECOVER/RESET exchange is not modeled here.
          /\ replayBacklog' = IF MutantDropReplayBacklog
                                THEN replayBacklog \ {4}
                                ELSE replayBacklog
          /\ interruptedReplay' = interruptedReplay \cup {j}
          /\ state' = [state EXCEPT ![j] = "Reserved"]
    /\ UNCHANGED <<cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, workerCharge, lateWorkers, acceptedCancel,
                    callerLost, rawCredit, creditReleaseCount, A, K, history,
                    resetAck, resetAckSeen, resetConfirmSent,
                    resetConfirmApplied, resetConfirmEcho, resetConfirmed,
                    badPublication>>

CompleteLateWorkerAfterReset ==
    /\ phase \in {"ResetPending", "ConfirmPending", "Recovering"}
    /\ 2 \in lateWorkers
    /\ lateWorkers' = lateWorkers \ {2}
    /\ workerCharge' = workerCharge \ {2}
    /\ IF 2 \in FPublished
          THEN badPublication' = TRUE
          ELSE badPublication' = badPublication
    /\ UNCHANGED <<state, cOrdinal, ordinalJob, FReservations, FPublished,
                    worker, acceptedCancel, callerLost, rawCredit,
                    creditReleaseCount, A, K, S, history, phase, resetAck,
                    resetAckSeen, resetConfirmSent, resetConfirmApplied,
                    resetConfirmEcho, resetConfirmed, replayBacklog,
                    replayInFlight, interruptedReplay>>

Next ==
    \/ SendNext
    \/ StartWorker
    \/ PublishWorker
    \/ ObserveCommit
    \/ \E j \in Jobs : LoseCaller(j)
    \/ AcceptFActiveCancel
    \/ CompleteLateWorker
    \/ ApplyReset
    \/ LoseResetAck
    \/ RetryResetAck
    \/ ObserveResetAck
    \/ SendResetConfirm
    \/ ApplyResetConfirm
    \/ LoseResetConfirmEcho
    \/ ObserveResetConfirmEcho
    \/ \E j \in Jobs : EmitReplay(j)
    \/ \E j \in Jobs : StartReplayWorker(j)
    \/ PublishReplay
    \/ ObserveReplayCommit
    \/ InterruptReplay
    \/ CompleteLateWorkerAfterReset
    \/ UNCHANGED vars

Spec == Init /\ [][Next]_vars

PendingReplayHasNotBeenLost ==
    (phase \in {"ConfirmPending", "Recovering"}) =>
        \A j \in resetAck.replay :
            j \in FPublished \/ j \in replayBacklog \/ j \in replayInFlight

PositivePrefixPreserved ==
    resetAck.valid =>
        /\ 1 \in FPublished
        /\ state[1] = "Observed"
        /\ \A j \in FPublished : j \in {1} \/ j \in resetAck.replay

UnavailableDispositionExact ==
    resetAck.valid =>
        /\ resetAck.replay \cap resetAck.unavailable = {}
        /\ resetAck.replay \cup resetAck.unavailable = {2, 3, 4}
        /\ 1 \notin resetAck.unavailable

AcceptedCancelCannotPublish ==
    /\ ~badPublication
    /\ 2 \in acceptedCancel => 2 \notin FPublished

CallerLossIsNotFCancel ==
    (2 \in callerLost /\ state[2] = "Working" /\
       2 \notin acceptedCancel) => 2 \in FReservations

WorkerPhysicalChargeRetained ==
    /\ worker # NoWorker => worker \in workerCharge
    /\ lateWorkers \subseteq workerCharge

UnavailableCreditReleasedExactlyOnce ==
    creditReleaseCount[2] <= 1
    /\ (state[2] = "Unavailable" => creditReleaseCount[2] = 1)

ActiveCancelRecoveryWitnessNotReached ==
    ~(/\ resetAck.valid
      /\ resetAck.prefix = 1
      /\ resetAck.replay = {3, 4}
      /\ resetAck.unavailable = {2}
      /\ resetAckSeen = "Replayed"
      /\ 2 \in callerLost
      /\ resetConfirmed = "ConfirmedAfterEchoLoss"
      /\ phase = "Recovering"
      /\ cOrdinal[1] = 1
      /\ cOrdinal[2] = 0
      /\ cOrdinal[3] = 2
      /\ cOrdinal[4] = 3
      /\ state[1] = "Observed"
      /\ state[2] = "Unavailable"
      /\ state[3] = "Observed"
      /\ state[4] = "Observed"
      /\ creditReleaseCount[2] = 1
      /\ K = 3
      /\ A = 3
      /\ FPublished = {1, 3, 4}
      /\ replayBacklog = {}
      /\ replayInFlight = {}
      /\ interruptedReplay = {3}
      /\ worker = NoWorker
      /\ workerCharge = {2}
      /\ lateWorkers = {2})

InterruptedReplayRetainsFullBacklog ==
    interruptedReplay # {} =>
        \A j \in resetAck.replay :
            j \in FPublished \/ j \in replayBacklog \/ j \in replayInFlight

ActiveCancelSettlesWithoutResurrection ==
    /\ state[2] = "Unavailable" => 2 \notin FReservations
    /\ state[2] = "Observed" => 2 \in FPublished

=============================================================================
