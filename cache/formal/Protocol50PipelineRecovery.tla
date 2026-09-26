------------------------- MODULE Protocol50PipelineRecovery -------------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
Bounded abstract model for the planned R2 ordered-link pipeline/recovery.
The immutable job identity stands for exact TU/job/raw/transaction witnesses;
codec bytes are not modeled.  A relationship is a fixed (C,F,profile) tuple,
but its admission identity is the profile-free C/F pair.  Relationship ordinal
is deliberately distinct from a C-wide TU identity.

Per relationship:
  A = largest contiguous receipt verified by C
  K = largest input atomically published by F
  P = largest locally staged transaction at C
  Q = receipt floor processed by F from an in-order COMMIT_ACK
  S = largest complete bundle sent by C
The one pending F worker is fenced by history epoch, while physical link
generation is separate and fences reconnect callbacks.  RESET keeps A/K/Q,
truncates P/S to K, advances only history epoch, then restages live raw jobs.
***************************************************************************)

CONSTANTS CStores, FStores, TargetC, TargetF,
          Window, MaxRawCCount, MaxRawCBytes,
          RecoveryCase, ThirdJobCase,
          MutantLastReceiptOnly, MutantAckBeyondK,
          MutantStaleWorker, MutantNonIdempotentReset,
          MutantCancelHole, MutantWrongJobReceipt,
          MutantDoubleCancelRelease, MutantResetBeforeWorkerFence

ASSUME /\ CStores # {}
       /\ FStores # {}
       /\ TargetC \in CStores
       /\ TargetF \in FStores
       /\ Window \in Nat \ {0}
       /\ MaxRawCCount \in Nat \ {0}
       /\ MaxRawCBytes \in Nat \ {0}
       /\ RecoveryCase \in BOOLEAN
       /\ ThirdJobCase \in BOOLEAN
       /\ MutantLastReceiptOnly \in BOOLEAN
       /\ MutantAckBeyondK \in BOOLEAN
       /\ MutantStaleWorker \in BOOLEAN
       /\ MutantNonIdempotentReset \in BOOLEAN
       /\ MutantCancelHole \in BOOLEAN
       /\ MutantWrongJobReceipt \in BOOLEAN
       /\ MutantDoubleCancelRelease \in BOOLEAN
       /\ MutantResetBeforeWorkerFence \in BOOLEAN

Links == CStores \X FStores
TargetLink == <<TargetC, TargetF>>
AllJobs == { [c |-> c, f |-> f, slot |-> n] :
             c \in CStores, f \in FStores, n \in 1..3 }
Jobs == { j \in AllJobs : j.slot = 1 \/
          (<<j.c, j.f>> = TargetLink /\
           (j.slot = 2 \/ (ThirdJobCase /\ j.slot = 3))) }
Rel(j) == <<j.c, j.f>>
JobsFor(r) == {j \in Jobs : Rel(j) = r}
NoJob == [c |-> CHOOSE c \in CStores : TRUE,
          f |-> CHOOSE f \in FStores : TRUE, slot |-> 0]
NoWorker == [ord |-> 0, hist |-> 0, job |-> NoJob,
             lgen |-> 0, rel |-> 0, fgen |-> 0, token |-> 0]
NoOrd == 0
MaxOrd == 3
ReceiptUniverse ==
    { [ord |-> n, job |-> j, relationshipEpoch |-> re,
        fGeneration |-> fg, historyEpoch |-> he] :
      n \in 1..MaxOrd, j \in Jobs, re \in 0..1,
      fg \in 0..1, he \in 0..2 }
JobStates == {"Queued", "Reserved", "Staged", "Sent", "Working",
              "Committed", "Observed", "Cancelled", "CancelledCommitted",
              "Released", "Rejected"}
LiveLinkPhases == {"Live", "Recovering", "ResetRequested",
                   "ResetAckPending", "ResetConfirmPending"}
RawUnit(j) == IF j.slot = 1 THEN 1 ELSE 2
RawHeldState(s) == s \in {"Reserved", "Staged", "Sent", "Working", "Committed"}

VARIABLES jobState, cancelled, compilerAllowed, ordinal, ordJob,
          relEpoch, fGeneration, linkGeneration, historyEpoch,
          phase, A, K, P, Q, ackSent, S, receipts, replyAvailable,
          recoveryComplete, worker, workerEpoch, lateWorkers,
          resetOp, resetReply, resetConfirmed,
          acceptedFCancel, cancelCreditReleaseCount,
          badStaleMutation, badHole, badAck

vars == <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
          relEpoch, fGeneration, linkGeneration, historyEpoch,
          phase, A, K, P, Q, ackSent, S, receipts, replyAvailable,
          recoveryComplete, worker, workerEpoch, lateWorkers,
          resetOp, resetReply, resetConfirmed,
          acceptedFCancel, cancelCreditReleaseCount,
          badStaleMutation, badHole, badAck>>

ReceiptFor(r, n) == CHOOSE x \in ReceiptUniverse :
    /\ x.ord = n
    /\ x.job = ordJob[r][n]
    /\ x.relationshipEpoch = relEpoch[r]
    /\ x.fGeneration = fGeneration[r]
    /\ x.historyEpoch = historyEpoch[r]
NextReceiptJob(r) ==
    (CHOOSE rec \in replyAvailable[r] : rec.ord = A[r] + 1).job

Init ==
    /\ jobState = [j \in Jobs |-> "Queued"]
    /\ cancelled = {}
    /\ compilerAllowed = {}
    /\ ordinal = [j \in Jobs |-> NoOrd]
    /\ ordJob = [r \in Links |-> [n \in 1..MaxOrd |-> NoJob]]
    /\ relEpoch = [r \in Links |-> 0]
    /\ fGeneration = [r \in Links |-> 0]
    /\ linkGeneration = [r \in Links |-> 0]
    /\ historyEpoch = [r \in Links |-> 0]
    /\ phase = [r \in Links |-> "Closed"]
    /\ A = [r \in Links |-> 0]
    /\ K = [r \in Links |-> 0]
    /\ P = [r \in Links |-> 0]
    /\ Q = [r \in Links |-> 0]
    /\ ackSent = [r \in Links |-> 0]
    /\ S = [r \in Links |-> 0]
    /\ receipts = [r \in Links |-> {}]
    /\ replyAvailable = [r \in Links |-> {}]
    /\ recoveryComplete = [r \in Links |-> FALSE]
    /\ worker = [r \in Links |-> NoWorker]
    /\ workerEpoch = [r \in Links |-> 0]
    /\ lateWorkers = [r \in Links |-> {}]
    /\ resetOp = [r \in Links |-> 0]
    /\ resetReply = [r \in Links |-> FALSE]
    /\ resetConfirmed = [r \in Links |-> "NotSent"]
    /\ acceptedFCancel = {}
    /\ cancelCreditReleaseCount = [j \in Jobs |-> 0]
    /\ badStaleMutation = FALSE
    /\ badHole = FALSE
    /\ badAck = FALSE

RawHeld(j) == jobState[j] \in {"Reserved", "Staged", "Sent", "Working",
                                "Committed"}
CountForC(c) == Cardinality({j \in Jobs : j.c = c /\ RawHeld(j)})
RECURSIVE SumRaw(_)
SumRaw(js) == IF js = {} THEN 0
              ELSE LET j == CHOOSE x \in js : TRUE
                   IN RawUnit(j) + SumRaw(js \ {j})
BytesForC(c) == SumRaw({j \in Jobs : j.c = c /\ RawHeld(j)})
WorkerOK(w) ==
    /\ w.ord \in 1..MaxOrd
    /\ w.hist \in 0..2
    /\ w.job \in Jobs
    /\ w.lgen \in 0..8
    /\ w.rel \in 0..1
    /\ w.fgen \in 0..1
    /\ w.token \in 0..2

TypeOK ==
    /\ jobState \in [Jobs -> JobStates]
    /\ cancelled \subseteq Jobs
    /\ acceptedFCancel \subseteq Jobs
    /\ cancelCreditReleaseCount \in [Jobs -> 0..2]
    /\ compilerAllowed \subseteq Jobs
    /\ ordinal \in [Jobs -> 0..MaxOrd]
    /\ relEpoch \in [Links -> 0..1]
    /\ fGeneration \in [Links -> 0..1]
    /\ linkGeneration \in [Links -> 0..8]
    /\ historyEpoch \in [Links -> 0..2]
    /\ phase \in [Links -> {"Closed", "Live", "Recovering",
                              "ResetRequested", "ResetAckPending",
                              "ResetConfirmPending", "Retired"}]
    /\ A \in [Links -> 0..MaxOrd]
    /\ K \in [Links -> 0..MaxOrd]
    /\ P \in [Links -> 0..MaxOrd]
    /\ Q \in [Links -> 0..MaxOrd]
    /\ ackSent \in [Links -> 0..(MaxOrd + 1)]
    /\ S \in [Links -> 0..MaxOrd]
    /\ receipts \in [Links -> SUBSET ReceiptUniverse]
    /\ replyAvailable \in [Links -> SUBSET ReceiptUniverse]
    /\ recoveryComplete \in [Links -> BOOLEAN]
    /\ \A r \in Links : worker[r] = NoWorker \/ WorkerOK(worker[r])
    /\ workerEpoch \in [Links -> 0..2]
    /\ \A r \in Links : \A w \in lateWorkers[r] : WorkerOK(w)
    /\ resetOp \in [Links -> 0..2]
    /\ resetReply \in [Links -> BOOLEAN]
    /\ resetConfirmed \in [Links -> {"NotSent", "RetrySent", "Confirmed"}]
    /\ badStaleMutation \in BOOLEAN
    /\ badHole \in BOOLEAN
    /\ badAck \in BOOLEAN


Reserve(j) ==
    LET r == Rel(j)
    IN /\ jobState[j] = "Queued"
       /\ CountForC(j.c) < MaxRawCCount
       /\ BytesForC(j.c) + RawUnit(j) <= MaxRawCBytes
       /\ phase[r] # "Retired"
       /\ jobState' = [jobState EXCEPT ![j] = "Reserved"]
       /\ UNCHANGED <<cancelled, compilerAllowed, ordinal, ordJob,
                       relEpoch, fGeneration, linkGeneration, historyEpoch,
                       phase, A, K, P, Q, ackSent, S, receipts,
                       replyAvailable, recoveryComplete, worker, workerEpoch,
                       lateWorkers, resetOp, resetReply, resetConfirmed,
                       badStaleMutation, badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

OpenLink(r) ==
    /\ phase[r] = "Closed"
    /\ linkGeneration[r] < 8
    /\ \E j \in JobsFor(r) : jobState[j] = "Reserved"
    /\ phase' = [phase EXCEPT ![r] = "Live"]
    /\ linkGeneration' = [linkGeneration EXCEPT ![r] = @ + 1]
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, historyEpoch, A, K, P, Q,
                    ackSent, S, receipts, replyAvailable, recoveryComplete,
                    worker, workerEpoch, lateWorkers, resetOp, resetReply,
                    resetConfirmed, badStaleMutation, badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

Stage(r, j) ==
    /\ phase[r] = "Live"
    /\ jobState[j] = "Reserved"
    /\ Rel(j) = r
    /\ P[r] - A[r] < Window
    /\ P[r] < MaxOrd
    /\ P' = [P EXCEPT ![r] = @ + 1]
    /\ ordinal' = [ordinal EXCEPT ![j] = P[r] + 1]
    /\ ordJob' = [ordJob EXCEPT ![r] = [@ EXCEPT ![P[r] + 1] = j]]
    /\ jobState' = [jobState EXCEPT ![j] = "Staged"]
    /\ UNCHANGED <<cancelled, compilerAllowed, relEpoch, fGeneration,
                    linkGeneration, historyEpoch, phase, A, K, Q, ackSent,
                    S, receipts, replyAvailable, recoveryComplete, worker,
                    workerEpoch, lateWorkers, resetOp, resetReply,
                    resetConfirmed, badStaleMutation, badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

SendBundle(r) ==
    /\ phase[r] = "Live"
    /\ S[r] < P[r]
    /\ S[r] - A[r] < Window
    /\ LET n == S[r] + 1
           j == ordJob[r][n]
       IN /\ j \in Jobs
          /\ jobState[j] = "Staged"
          /\ jobState' = [jobState EXCEPT ![j] = "Sent"]
          /\ S' = [S EXCEPT ![r] = @ + 1]
    /\ UNCHANGED <<cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    phase, A, K, P, Q, ackSent, receipts, replyAvailable,
                    recoveryComplete, worker, workerEpoch, lateWorkers,
                    resetOp, resetReply, resetConfirmed, badStaleMutation,
                    badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

StartFWorker(r) ==
    /\ phase[r] \in {"Live", "Recovering"}
    /\ worker[r] = NoWorker
    /\ K[r] < S[r]
    /\ LET n == K[r] + 1
           j == ordJob[r][n]
       IN /\ j \in Jobs
          /\ jobState[j] = "Sent"
          /\ worker' = [worker EXCEPT ![r] =
                [ord |-> n, hist |-> historyEpoch[r], job |-> j,
                 lgen |-> linkGeneration[r], rel |-> relEpoch[r],
                 fgen |-> fGeneration[r], token |-> workerEpoch[r]]]
          /\ jobState' = [jobState EXCEPT ![j] = "Working"]
    /\ UNCHANGED <<cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    phase, A, K, P, Q, ackSent, S, receipts, replyAvailable,
                    recoveryComplete, workerEpoch, lateWorkers, resetOp,
                    resetReply, resetConfirmed, badStaleMutation, badHole,
                    acceptedFCancel, cancelCreditReleaseCount, badAck>>

PublishFInput(r) ==
    /\ worker[r] # NoWorker
    /\ worker[r].hist = historyEpoch[r]
    /\ worker[r].lgen = linkGeneration[r]
    /\ worker[r].rel = relEpoch[r]
    /\ worker[r].fgen = fGeneration[r]
    /\ worker[r].token = workerEpoch[r]
    /\ worker[r].ord = K[r] + 1
    /\ K[r] - Q[r] < Window
    /\ LET j == worker[r].job
       IN /\ j \notin acceptedFCancel
          /\ jobState[j] = "Working"
          /\ K' = [K EXCEPT ![r] = @ + 1]
          /\ receipts' = [receipts EXCEPT ![r] = @ \cup {ReceiptFor(r, K[r] + 1)}]
          /\ replyAvailable' = [replyAvailable EXCEPT
                                  ![r] = @ \cup {ReceiptFor(r, K[r] + 1)}]
          /\ jobState' = [jobState EXCEPT ![j] = "Committed"]
          /\ worker' = [worker EXCEPT ![r] = NoWorker]
          /\ recoveryComplete' = [recoveryComplete EXCEPT ![r] = FALSE]
    /\ UNCHANGED <<cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    phase, A, P, Q, ackSent, S,
                    workerEpoch, lateWorkers, resetOp, resetReply,
                    resetConfirmed, badStaleMutation, badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

LoseCommitReply(r, n) ==
    /\ RecoveryCase
    /\ phase[r] \in {"Live", "Recovering"}
    /\ \E rec \in replyAvailable[r] : rec.ord = n
    /\ replyAvailable' = [replyAvailable EXCEPT
          ![r] = {rec \in @ : rec.ord # n}]
    /\ recoveryComplete' = [recoveryComplete EXCEPT ![r] = FALSE]
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    phase, A, K, P, Q, ackSent, S, receipts,
                    worker, workerEpoch, lateWorkers,
                    resetOp, resetReply, resetConfirmed, badStaleMutation,
                    badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

ObserveCommitReply(r) ==
    /\ phase[r] = "Live"
    /\ A[r] < K[r]
    /\ \E rec \in replyAvailable[r] : rec.ord = A[r] + 1
    /\ LET rec == CHOOSE x \in replyAvailable[r] : x.ord = A[r] + 1
           j == rec.job
       IN /\ j \in Jobs
          /\ A' = [A EXCEPT ![r] = @ + 1]
          /\ jobState' = [jobState EXCEPT ![j] =
                IF j \in cancelled THEN "CancelledCommitted" ELSE "Observed"]
          /\ compilerAllowed' = IF j \in cancelled THEN compilerAllowed
                                ELSE compilerAllowed \cup {j}
    /\ replyAvailable' = [replyAvailable EXCEPT
          ![r] = {rec \in @ : rec.ord # A[r] + 1}]
    /\ cancelCreditReleaseCount' = [j \in Jobs |->
          cancelCreditReleaseCount[j] +
            IF j = NextReceiptJob(r) /\ j \in cancelled /\
               jobState[j] = "Committed"
            THEN IF MutantDoubleCancelRelease THEN 2 ELSE 1 ELSE 0]
    /\ UNCHANGED <<cancelled, ordinal, ordJob, relEpoch, fGeneration,
                    linkGeneration, historyEpoch, phase, K, P, Q, ackSent,
                    S, receipts, recoveryComplete, worker, workerEpoch,
                    lateWorkers, resetOp, resetReply, resetConfirmed,
                    badStaleMutation, badHole, acceptedFCancel, badAck>>

SendCommitAck(r) ==
    /\ phase[r] \in {"Live", "Recovering"}
    /\ ackSent[r] < A[r]
    /\ ackSent' = [ackSent EXCEPT ![r] =
          IF MutantAckBeyondK THEN K[r] + 1 ELSE A[r]]
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    phase, A, K, P, Q, S, receipts, replyAvailable,
                    recoveryComplete, worker, workerEpoch, lateWorkers,
                    resetOp, resetReply, resetConfirmed, badStaleMutation,
                    badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

ProcessCommitAck(r) ==
    /\ phase[r] \in {"Live", "Recovering"}
    /\ Q[r] < ackSent[r]
    /\ ackSent[r] <= A[r]
    /\ ackSent[r] <= K[r]
    /\ Q' = [Q EXCEPT ![r] = ackSent[r]]
    /\ receipts' = [receipts EXCEPT
          ![r] = {rec \in @ : rec.ord > ackSent[r]}]
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    phase, A, K, P, ackSent, S, replyAvailable,
                    recoveryComplete, worker, workerEpoch, lateWorkers,
                    resetOp, resetReply, resetConfirmed, badStaleMutation,
                    badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

DropLink(r) ==
    /\ RecoveryCase
    /\ phase[r] = "Live"
    /\ linkGeneration[r] \in {1, 2}
    /\ phase' = [phase EXCEPT ![r] = "Recovering"]
    /\ linkGeneration' = [linkGeneration EXCEPT ![r] = @ + 1]
    /\ recoveryComplete' = [recoveryComplete EXCEPT ![r] = FALSE]
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, historyEpoch, A, K, P, Q,
                    ackSent, S, receipts, replyAvailable, worker, workerEpoch,
                    lateWorkers, resetOp, resetReply, resetConfirmed,
                    badStaleMutation, badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

RecoverReceipts(r) ==
    /\ phase[r] = "Recovering"
    /\ IF MutantLastReceiptOnly
          THEN replyAvailable' = [replyAvailable EXCEPT ![r] =
                 IF A[r] < K[r] THEN {CHOOSE rec \in receipts[r] : rec.ord = K[r]}
                 ELSE {}]
          ELSE replyAvailable' = [replyAvailable EXCEPT ![r] =
                 @ \cup {rec \in receipts[r] : rec.ord > A[r]}]
    /\ recoveryComplete' = [recoveryComplete EXCEPT ![r] = TRUE]
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    phase, A, K, P, Q, ackSent, S, receipts, worker,
                    workerEpoch, lateWorkers, resetOp, resetReply,
                    resetConfirmed, badStaleMutation, badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

CorruptRecoveryIdentity(r) ==
    /\ RecoveryCase
    /\ MutantWrongJobReceipt
    /\ phase[r] = "Recovering"
    /\ recoveryComplete[r]
    /\ \E rec \in replyAvailable[r] :
          \E wrong \in JobsFor(r) : wrong # rec.job
    /\ LET rec == CHOOSE x \in replyAvailable[r] :
                    \E wrong \in JobsFor(r) : wrong # x.job
           wrong == CHOOSE j \in JobsFor(r) : j # rec.job
           forged == [rec EXCEPT !.job = wrong]
       IN replyAvailable' = [replyAvailable EXCEPT
              ![r] = (@ \ {rec}) \cup {forged}]
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    phase, A, K, P, Q, ackSent, S, receipts,
                    recoveryComplete, worker, workerEpoch, lateWorkers,
                    resetOp, resetReply, resetConfirmed, badStaleMutation,
                    badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

ObserveRecoveredReceipt(r) ==
    /\ phase[r] = "Recovering"
    /\ recoveryComplete[r]
    /\ A[r] < K[r]
    /\ \E rec \in replyAvailable[r] : rec.ord = A[r] + 1
    /\ LET rec == CHOOSE x \in replyAvailable[r] : x.ord = A[r] + 1
           j == rec.job
       IN /\ j \in Jobs
          /\ A' = [A EXCEPT ![r] = @ + 1]
          /\ jobState' = [jobState EXCEPT ![j] =
                IF j \in cancelled THEN "CancelledCommitted" ELSE "Observed"]
          /\ compilerAllowed' = IF j \in cancelled THEN compilerAllowed
                                ELSE compilerAllowed \cup {j}
    /\ replyAvailable' = [replyAvailable EXCEPT
          ![r] = {rec \in @ : rec.ord # A[r] + 1}]
    /\ recoveryComplete' = [recoveryComplete EXCEPT ![r] = FALSE]
    /\ cancelCreditReleaseCount' = [j \in Jobs |->
          cancelCreditReleaseCount[j] +
            IF j = NextReceiptJob(r) /\ j \in cancelled /\
               jobState[j] = "Committed"
            THEN IF MutantDoubleCancelRelease THEN 2 ELSE 1 ELSE 0]
    /\ UNCHANGED <<cancelled, ordinal, ordJob, relEpoch, fGeneration,
                    linkGeneration, historyEpoch, phase, K, P, Q, ackSent,
                    S, receipts, worker, workerEpoch,
                    lateWorkers, resetOp, resetReply, resetConfirmed,
                    badStaleMutation, badHole, acceptedFCancel, badAck>>

FencePendingWorker(r) ==
    /\ RecoveryCase
    /\ phase[r] = "Recovering"
    /\ worker[r] # NoWorker
    /\ workerEpoch[r] < 2
    /\ lateWorkers[r] = {}
    /\ LET w == worker[r]
       IN /\ lateWorkers' = [lateWorkers EXCEPT ![r] = @ \cup {w}]
          /\ worker' = [worker EXCEPT ![r] = NoWorker]
          /\ workerEpoch' = [workerEpoch EXCEPT ![r] = @ + 1]
          /\ jobState' = [jobState EXCEPT ![w.job] = "Sent"]
    /\ UNCHANGED <<cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    phase, A, K, P, Q, ackSent, S, receipts, replyAvailable,
                    recoveryComplete, resetOp, resetReply, resetConfirmed,
                    badStaleMutation, badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

DeliverStaleWorker(r, w) ==
    /\ RecoveryCase
    /\ w \in lateWorkers[r]
    /\ lateWorkers' = [lateWorkers EXCEPT ![r] = @ \ {w}]
    /\ IF MutantStaleWorker
          THEN /\ K' = [K EXCEPT ![r] = @ + 1]
               /\ badStaleMutation' = TRUE
               /\ receipts' = [receipts EXCEPT ![r] =
                     @ \cup {ReceiptFor(r, K[r] + 1)}]
          ELSE /\ UNCHANGED <<K, receipts>>
               /\ badStaleMutation' = badStaleMutation
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    phase, A, P, Q, ackSent, S, replyAvailable,
                    recoveryComplete, worker, workerEpoch, resetOp,
                    resetReply, resetConfirmed, badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

RequestReset(r) ==
    /\ RecoveryCase
    /\ phase[r] = "Recovering"
    /\ A[r] = K[r]
    /\ Q[r] = K[r]
    /\ (worker[r] = NoWorker \/ MutantResetBeforeWorkerFence)
    /\ resetOp[r] < 2
    /\ phase' = [phase EXCEPT ![r] = "ResetRequested"]
    /\ resetOp' = [resetOp EXCEPT ![r] = @ + 1]
    /\ resetConfirmed' = [resetConfirmed EXCEPT ![r] = "NotSent"]
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    A, K, P, Q, ackSent, S, receipts, replyAvailable,
                    recoveryComplete, worker, workerEpoch, lateWorkers,
                    resetReply, badStaleMutation, badHole,
                    acceptedFCancel, cancelCreditReleaseCount, badAck>>

ApplyReset(r) ==
    /\ RecoveryCase
    /\ phase[r] = "ResetRequested"
    /\ resetOp[r] \in 1..2
    /\ A[r] = K[r]
    /\ Q[r] = K[r]
    /\ worker[r] = NoWorker
    /\ phase' = [phase EXCEPT ![r] = "ResetAckPending"]
    /\ historyEpoch' = [historyEpoch EXCEPT ![r] = @ + 1]
    /\ P' = [P EXCEPT ![r] = K[r]]
    /\ S' = [S EXCEPT ![r] = K[r]]
    /\ ordJob' = [ordJob EXCEPT ![r] =
          [n \in 1..MaxOrd |-> IF n <= K[r] THEN ordJob[r][n] ELSE NoJob]]
    /\ ordinal' = [j \in Jobs |->
          IF Rel(j) = r /\ ordinal[j] > K[r] THEN NoOrd ELSE ordinal[j]]
    /\ jobState' = [j \in Jobs |->
          IF Rel(j) = r /\ ordinal[j] > K[r]
          THEN IF j \in cancelled THEN "Released" ELSE "Reserved"
          ELSE jobState[j]]
    /\ cancelCreditReleaseCount' = [j \in Jobs |->
          cancelCreditReleaseCount[j] +
            IF Rel(j) = r /\ ordinal[j] > K[r] /\ j \in cancelled /\
               RawHeldState(jobState[j])
            THEN IF MutantDoubleCancelRelease THEN 2 ELSE 1 ELSE 0]
    /\ resetReply' = [resetReply EXCEPT ![r] = TRUE]
    /\ replyAvailable' = [replyAvailable EXCEPT ![r] = {}]
    /\ UNCHANGED <<cancelled, compilerAllowed, relEpoch, fGeneration,
                    linkGeneration, A, K, Q, ackSent, receipts,
                    recoveryComplete, worker, workerEpoch, lateWorkers,
                    resetOp, resetConfirmed, badStaleMutation, badHole, acceptedFCancel, badAck>>

LoseResetAck(r) ==
    /\ RecoveryCase
    /\ phase[r] = "ResetAckPending"
    /\ resetReply[r]
    /\ resetConfirmed[r] = "NotSent"
    /\ resetReply' = [resetReply EXCEPT ![r] = FALSE]
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    phase, A, K, P, Q, ackSent, S, receipts, replyAvailable,
                    recoveryComplete, worker, workerEpoch, lateWorkers,
                    resetOp, resetConfirmed, badStaleMutation, badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

RetryReset(r) ==
    /\ RecoveryCase
    /\ phase[r] = "ResetAckPending"
    /\ resetOp[r] \in 1..2
    /\ ~resetReply[r]
    /\ resetConfirmed[r] = "NotSent"
    /\ resetReply' = [resetReply EXCEPT ![r] = TRUE]
    /\ resetConfirmed' = [resetConfirmed EXCEPT ![r] = "RetrySent"]
    /\ IF MutantNonIdempotentReset
          THEN historyEpoch' = [historyEpoch EXCEPT ![r] = @ + 1]
          ELSE UNCHANGED historyEpoch
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration,
                    phase, A, K, P, Q, ackSent, S, receipts,
                    replyAvailable, recoveryComplete, worker, workerEpoch,
                    lateWorkers, resetOp, badStaleMutation,
                    badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

ObserveResetAck(r) ==
    /\ RecoveryCase
    /\ phase[r] = "ResetAckPending"
    /\ resetReply[r]
    /\ phase' = [phase EXCEPT ![r] = "ResetConfirmPending"]
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    A, K, P, Q, ackSent, S, receipts, replyAvailable,
                    recoveryComplete, worker, workerEpoch, lateWorkers,
                    resetOp, resetReply, resetConfirmed, badStaleMutation,
                    badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

SendResetConfirm(r) ==
    /\ RecoveryCase
    /\ phase[r] = "ResetConfirmPending"
    /\ resetConfirmed[r] # "Confirmed"
    /\ resetConfirmed' = [resetConfirmed EXCEPT ![r] = "Confirmed"]
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    phase, A, K, P, Q, ackSent, S, receipts,
                    replyAvailable, recoveryComplete, worker, workerEpoch,
                    lateWorkers, resetOp, resetReply, badStaleMutation,
                    badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

ProcessResetConfirm(r) ==
    /\ RecoveryCase
    /\ phase[r] = "ResetConfirmPending"
    /\ resetConfirmed[r] = "Confirmed"
    /\ phase' = [phase EXCEPT ![r] = "Live"]
    /\ resetReply' = [resetReply EXCEPT ![r] = FALSE]
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    A, K, P, Q, ackSent, S, receipts, replyAvailable,
                    recoveryComplete, worker, workerEpoch, lateWorkers,
                    resetOp, resetConfirmed, badStaleMutation, badHole,
                    acceptedFCancel, cancelCreditReleaseCount, badAck>>

CancelJob(j) ==
    /\ RecoveryCase
    /\ jobState[j] \in {"Queued", "Reserved", "Staged", "Sent",
                          "Working", "Committed"}
    /\ j \notin cancelled
    /\ (jobState[j] # "Staged" \/ ordinal[j] <= S[Rel(j)]
        \/ linkGeneration[Rel(j)] < 8)
    /\ LET r == Rel(j)
           stagedUnsent == jobState[j] = "Staged" /\ ordinal[j] > S[r]
       IN /\ jobState' = [jobState EXCEPT ![j] =
                IF @ \in {"Queued", "Reserved"} THEN "Cancelled" ELSE @]
          /\ phase' = [phase EXCEPT ![r] =
                IF stagedUnsent THEN "Recovering" ELSE @]
          /\ linkGeneration' = [linkGeneration EXCEPT ![r] =
                IF stagedUnsent THEN @ + 1 ELSE @]
          /\ ordinal' = IF MutantCancelHole /\ stagedUnsent
                THEN [ordinal EXCEPT ![j] = NoOrd] ELSE ordinal
          /\ ordJob' = IF MutantCancelHole /\ stagedUnsent
                THEN [ordJob EXCEPT ![r] =
                     [@ EXCEPT ![ordinal[j]] = NoJob]] ELSE ordJob
          /\ badHole' = badHole \/ (MutantCancelHole /\ stagedUnsent)
          /\ cancelCreditReleaseCount' = [x \in Jobs |->
                cancelCreditReleaseCount[x] +
                  IF x = j /\ jobState[j] = "Reserved"
                  THEN IF MutantDoubleCancelRelease THEN 2 ELSE 1 ELSE 0]
    /\ cancelled' = cancelled \cup {j}
    /\ compilerAllowed' = compilerAllowed \ {j}
    /\ UNCHANGED <<relEpoch, fGeneration, historyEpoch,
                    A, K, P, Q, ackSent, S, receipts, replyAvailable,
                    recoveryComplete, worker, workerEpoch, lateWorkers,
                    resetOp, resetReply, resetConfirmed, badStaleMutation,
                    acceptedFCancel, badAck>>

AcceptFPrepublicationCancel(j) ==
    /\ RecoveryCase
    /\ j \in cancelled
    /\ j \notin acceptedFCancel
    /\ jobState[j] = "Staged"
    /\ ordinal[j] > S[Rel(j)]
    /\ phase[Rel(j)] = "Recovering"
    /\ acceptedFCancel' = acceptedFCancel \cup {j}
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    relEpoch, fGeneration, linkGeneration, historyEpoch,
                    phase, A, K, P, Q, ackSent, S, receipts,
                    replyAvailable, recoveryComplete, worker, workerEpoch,
                    lateWorkers, resetOp, resetReply, resetConfirmed,
                    cancelCreditReleaseCount, badStaleMutation, badHole, badAck>>

RetireRelationship(r) ==
    /\ RecoveryCase
    /\ phase[r] = "Recovering"
    /\ worker[r] = NoWorker
    /\ A[r] = K[r]
    /\ Q[r] = K[r]
    /\ fGeneration[r] = 0
    /\ relEpoch[r] = 0
    /\ phase' = [phase EXCEPT ![r] = "Retired"]
    /\ relEpoch' = [relEpoch EXCEPT ![r] = @ + 1]
    /\ fGeneration' = [fGeneration EXCEPT ![r] = @ + 1]
    /\ UNCHANGED <<jobState, cancelled, compilerAllowed, ordinal, ordJob,
                    linkGeneration, historyEpoch, A, K, P, Q, ackSent, S,
                    receipts, replyAvailable, recoveryComplete, worker,
                    workerEpoch, lateWorkers, resetOp, resetReply,
                    resetConfirmed, badStaleMutation, badHole, acceptedFCancel, cancelCreditReleaseCount, badAck>>

TargetJob(slot) == CHOOSE j \in Jobs : Rel(j) = TargetLink /\ j.slot = slot

GeneralNext ==
    \/ \E j \in Jobs : Reserve(j)
    \/ \E r \in Links : OpenLink(r)
    \/ \E r \in Links : \E j \in JobsFor(r) : Stage(r, j)
    \/ \E r \in Links : SendBundle(r)
    \/ \E r \in Links : StartFWorker(r)
    \/ \E r \in Links : PublishFInput(r)
    \/ \E r \in Links, n \in 1..MaxOrd : LoseCommitReply(r, n)
    \/ \E r \in Links : ObserveCommitReply(r)
    \/ \E r \in Links : SendCommitAck(r)
    \/ \E r \in Links : ProcessCommitAck(r)
    \/ \E r \in Links : DropLink(r)
    \/ \E r \in Links : RecoverReceipts(r)
    \/ \E r \in Links : CorruptRecoveryIdentity(r)
    \/ \E r \in Links : ObserveRecoveredReceipt(r)
    \/ \E r \in Links : FencePendingWorker(r)
    \/ \E r \in Links : \E w \in lateWorkers[r] : DeliverStaleWorker(r, w)
    \/ \E r \in Links : RequestReset(r)
    \/ \E r \in Links : ApplyReset(r)
    \/ \E r \in Links : LoseResetAck(r)
    \/ \E r \in Links : RetryReset(r)
    \/ \E r \in Links : ObserveResetAck(r)
    \/ \E r \in Links : SendResetConfirm(r)
    \/ \E r \in Links : ProcessResetConfirm(r)
    \/ \E j \in Jobs : CancelJob(j)
    \/ \E j \in Jobs : AcceptFPrepublicationCancel(j)
    \/ \E r \in Links : RetireRelationship(r)
    \/ UNCHANGED vars

(***************************************************************************
Focused reachability driver for the six cancel/reindex witness configs.
Those configs set RecoveryCase and ThirdJobCase together.  The reduced action
set keeps all independent links live through one committed job, while ordering
the target link to a settled prefix, two staged suffix jobs, and one
prepublication cancellation.  It is a bounded witness harness, not a second
exhaustive safety model; the standard topology rows still use GeneralNext.
Only staged-unsent cancellation is modeled here.  F-accepted cancellation of
Sent/Working jobs and its publication race are deliberately excluded; these
rows must not be read as evidence about active materialization cancellation.
***************************************************************************)
SendPrefixOrSiblingBundle(r) ==
    /\ (r # TargetLink \/ S[r] = 0)
    /\ SendBundle(r)

StageForCancelReindex(r, j) ==
    /\ ((r # TargetLink) \/ (j.slot = 1 /\ P[r] = 0)
       \/ (j.slot > 1 /\ P[r] > 0))
    /\ Stage(r, j)

CancelReindexNext ==
    \/ \E j \in Jobs : Reserve(j)
    \/ \E r \in Links : OpenLink(r)
    \/ \E r \in Links : \E j \in JobsFor(r) : StageForCancelReindex(r, j)
    \/ \E r \in Links : SendPrefixOrSiblingBundle(r)
    \/ \E r \in Links : StartFWorker(r)
    \/ \E r \in Links : PublishFInput(r)
    \/ \E r \in Links : ObserveCommitReply(r)
    \/ \E r \in Links : SendCommitAck(r)
    \/ \E r \in Links : ProcessCommitAck(r)
    \/ (LET r == TargetLink
            middle == TargetJob(2)
        IN /\ A[r] = 1 /\ K[r] = 1 /\ Q[r] = 1
           /\ P[r] = 3 /\ S[r] = 1
           /\ jobState[middle] = "Staged"
           /\ ordinal[middle] > S[r]
           /\ CancelJob(middle))
    \/ AcceptFPrepublicationCancel(TargetJob(2))
    \/ RequestReset(TargetLink)
    \/ ApplyReset(TargetLink)
    \/ ObserveResetAck(TargetLink)
    \/ SendResetConfirm(TargetLink)
    \/ ProcessResetConfirm(TargetLink)

Next == GeneralNext

Spec == Init /\ [][Next]_vars
CancelReindexSpec == Init /\ [][CancelReindexNext]_vars

CursorAndWindowBounds ==
    \A r \in Links :
        /\ Q[r] <= A[r]
        /\ A[r] <= K[r]
        /\ K[r] <= S[r]
        /\ S[r] <= P[r]
        /\ P[r] - A[r] <= Window
        /\ K[r] - Q[r] <= Window
        /\ ackSent[r] <= A[r]
        /\ ackSent[r] <= K[r]

ReceiptLedgerExact ==
    \A r \in Links :
        /\ \A rec \in receipts[r] :
             /\ rec.ord \in 1..MaxOrd
             /\ Q[r] < rec.ord
             /\ rec.ord <= K[r]
             /\ rec.job \in JobsFor(r)
             /\ rec.job = ordJob[r][rec.ord]
             /\ rec.relationshipEpoch = relEpoch[r]
             /\ rec.fGeneration = fGeneration[r]
             /\ rec.historyEpoch <= historyEpoch[r]
        /\ {rec.ord : rec \in receipts[r]} =
             {n \in 1..MaxOrd : Q[r] < n /\ n <= K[r]}

RecoveryResponseContainsAllReceipts ==
    \A r \in Links : recoveryComplete[r] =>
        {n \in 1..MaxOrd : A[r] < n /\ n <= K[r]} \subseteq
            {rec.ord : rec \in replyAvailable[r]}

RecoveryResponseIdentityExact ==
    \A r \in Links : \A rec \in replyAvailable[r] :
        rec \in receipts[r]

ReceiptIdentityContiguous ==
    \A r \in Links : \A n \in 1..K[r] :
        /\ ordJob[r][n] \in JobsFor(r)
        /\ ordinal[ordJob[r][n]] = n

NoOrdinalHoleThroughPrepared ==
    \A r \in Links : \A n \in 1..P[r] : ordJob[r][n] \in JobsFor(r)

ReservationCaps ==
    /\ \A c \in CStores : CountForC(c) <= MaxRawCCount
    /\ \A c \in CStores : BytesForC(c) <= MaxRawCBytes

CompilerRequiresVerifiedCommit ==
    compilerAllowed \subseteq {j \in Jobs : jobState[j] = "Observed"}

StaleWorkerCannotPublish == ~badStaleMutation
CancellationCannotPunchHole == ~badHole
CancellationCreditReleasedAtMostOnce ==
    \A j \in Jobs : cancelCreditReleaseCount[j] <= 1
AcceptedFCancelCreditReleasedOnce ==
    \A j \in acceptedFCancel : jobState[j] = "Released" =>
        cancelCreditReleaseCount[j] = 1
AcceptedFCancelNeverResurrects ==
    \A j \in acceptedFCancel : jobState[j] \in {"Staged", "Released"}
AckNeverExceedsCommittedPrefix == \A r \in Links : ackSent[r] <= K[r]
FullWindowWitnessNotReached ==
    \A r \in Links : S[r] - A[r] < Window
NoIndependentProgressWhileTargetFull ==
    ~\E other \in Links :
        other # TargetLink
        /\ S[TargetLink] - A[TargetLink] = Window
        /\ K[other] > 0
ResetOperationIdempotent ==
    \A r \in Links : historyEpoch[r] <= resetOp[r]

ResetRequiresPendingWorkerFence ==
    \A r \in Links :
        phase[r] \in {"ResetRequested", "ResetAckPending", "ResetConfirmPending"}
        => worker[r] = NoWorker

LastConfirmedResetResult(r) ==
    IF resetOp[r] = 2 /\ resetConfirmed[r] # "Confirmed"
    THEN 1
    ELSE IF resetConfirmed[r] = "Confirmed" THEN resetOp[r] ELSE 0

ThirdJobRefillNotReached ==
    \A r \in Links : ~(P[r] = 3 /\ A[r] >= 1 /\ S[r] = 3)

CancelSuffixReindexWitnessNotReached ==
    LET r == TargetLink
        prefix == TargetJob(1)
        cancelledMiddle == TargetJob(2)
        survivingSuffix == TargetJob(3)
    IN ~(/\ RecoveryCase
         /\ ThirdJobCase
         /\ phase[r] = "Live"
         /\ \A other \in Links \ {r} : K[other] >= 1
         /\ resetOp[r] = 1
         /\ historyEpoch[r] = 1
         /\ A[r] = 1
         /\ K[r] = 1
         /\ Q[r] = 1
         /\ P[r] = 2
         /\ S[r] = 1
         /\ ordinal[prefix] = 1
         /\ ordJob[r][1] = prefix
         /\ jobState[prefix] = "Observed"
         /\ cancelledMiddle \in cancelled
         /\ cancelledMiddle \in acceptedFCancel
         /\ jobState[cancelledMiddle] = "Released"
         /\ ordinal[cancelledMiddle] = NoOrd
         /\ cancelCreditReleaseCount[cancelledMiddle] = 1
         /\ survivingSuffix \notin cancelled
         /\ jobState[survivingSuffix] = "Staged"
         /\ ordinal[survivingSuffix] = 2
         /\ ordJob[r][2] = survivingSuffix)

=============================================================================
