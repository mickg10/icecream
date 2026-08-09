------------------------ MODULE LifecycleAuthority ------------------------
(***************************************************************************
STARTED-job lifecycle authority, independent of p49 unbegun reclamation.

The current protocol permits an attached submitter and the assigned worker to
race to terminalize a started job.  Once the submitter is detached, only the
assigned worker or loss of that worker session may terminalize it.

Mutants reproduce:
  * the nullable submitter guard that accepts a false submitter terminal after
    detach;
  * duplicate JobBegin resetting timestamps/emitting a second monitor Begin;
  * dropping the stable detached identity from management observability.
***************************************************************************)
EXTENDS Naturals, TLC

CONSTANTS AssignedSubmitter, OtherSubmitter, NoSubmitter,
          AssignedGeneration, OtherGeneration,
          AssignedWorker, OtherWorker, NoWorker,
          CountCap,
          MutantDetachedSubmitterTerminates,
          MutantDuplicateBeginMutates,
          MutantDropDetachedIdentity

Phases == {"Waiting", "Started", "Terminal"}
Origins == {"None", "WorkerDone", "WorkerLost", "SubmitterDone"}

ASSUME /\ AssignedSubmitter # OtherSubmitter
       /\ NoSubmitter # AssignedSubmitter
       /\ NoSubmitter # OtherSubmitter
       /\ AssignedGeneration \in Nat \ {0}
       /\ OtherGeneration \in Nat \ {0}
       /\ AssignedGeneration # OtherGeneration
       /\ AssignedWorker # OtherWorker
       /\ NoWorker # AssignedWorker
       /\ NoWorker # OtherWorker
       /\ CountCap \in Nat \ {0}
       /\ MutantDetachedSubmitterTerminates \in BOOLEAN
       /\ MutantDuplicateBeginMutates \in BOOLEAN
       /\ MutantDropDetachedIdentity \in BOOLEAN

CapInc(n) == IF n < CountCap THEN n + 1 ELSE n

VARIABLES phase,
          attachedSubmitter,
          liveSubmitterGeneration,
          detached,
          identityVisible,
          begunEver,
          dispatchDebit,
          creditCount,
          monitorBeginCount,
          startRevision,
          duplicateBeginCount,
          invalidBeginCount,
          terminalCount,
          terminalOrigin,
          terminalSubmitterGeneration,
          terminalWorker,
          rejectedDetachedSubmitter,
          rejectedWrongWorker,
          rejectedWrongSubmitter

vars ==
    <<phase, attachedSubmitter, liveSubmitterGeneration, detached,
      identityVisible, begunEver, dispatchDebit, creditCount,
      monitorBeginCount, startRevision, duplicateBeginCount,
      invalidBeginCount, terminalCount, terminalOrigin,
      terminalSubmitterGeneration, terminalWorker,
      rejectedDetachedSubmitter, rejectedWrongWorker,
      rejectedWrongSubmitter>>

Init ==
    /\ phase = "Waiting"
    /\ attachedSubmitter = AssignedSubmitter
    /\ liveSubmitterGeneration = AssignedGeneration
    /\ detached = FALSE
    /\ identityVisible = TRUE
    /\ begunEver = FALSE
    /\ dispatchDebit = TRUE
    /\ creditCount = 0
    /\ monitorBeginCount = 0
    /\ startRevision = 0
    /\ duplicateBeginCount = 0
    /\ invalidBeginCount = 0
    /\ terminalCount = 0
    /\ terminalOrigin = "None"
    /\ terminalSubmitterGeneration = 0
    /\ terminalWorker = NoWorker
    /\ rejectedDetachedSubmitter = 0
    /\ rejectedWrongWorker = 0
    /\ rejectedWrongSubmitter = 0

BeginFromAssignedWorker ==
    /\ phase = "Waiting"
    /\ phase' = "Started"
    /\ begunEver' = TRUE
    /\ dispatchDebit' = FALSE
    /\ creditCount' = creditCount + 1
    /\ monitorBeginCount' = monitorBeginCount + 1
    /\ startRevision' = startRevision + 1
    /\ UNCHANGED <<attachedSubmitter, liveSubmitterGeneration, detached,
                    identityVisible, duplicateBeginCount, invalidBeginCount,
                    terminalCount, terminalOrigin,
                    terminalSubmitterGeneration, terminalWorker,
                    rejectedDetachedSubmitter, rejectedWrongWorker,
                    rejectedWrongSubmitter>>

DuplicateBeginFromAssignedWorker ==
    /\ phase = "Started"
    /\ duplicateBeginCount' = CapInc(duplicateBeginCount)
    /\ monitorBeginCount' =
          IF MutantDuplicateBeginMutates THEN monitorBeginCount + 1
          ELSE monitorBeginCount
    /\ startRevision' =
          IF MutantDuplicateBeginMutates THEN startRevision + 1
          ELSE startRevision
    /\ UNCHANGED <<phase, attachedSubmitter, liveSubmitterGeneration,
                    detached, identityVisible, begunEver, dispatchDebit,
                    creditCount, invalidBeginCount, terminalCount,
                    terminalOrigin, terminalSubmitterGeneration,
                    terminalWorker, rejectedDetachedSubmitter,
                    rejectedWrongWorker, rejectedWrongSubmitter>>

BeginFromWrongWorker ==
    /\ phase \in {"Waiting", "Started"}
    /\ invalidBeginCount' = CapInc(invalidBeginCount)
    /\ rejectedWrongWorker' = CapInc(rejectedWrongWorker)
    /\ UNCHANGED <<phase, attachedSubmitter, liveSubmitterGeneration,
                    detached, identityVisible, begunEver, dispatchDebit,
                    creditCount, monitorBeginCount, startRevision,
                    duplicateBeginCount, terminalCount, terminalOrigin,
                    terminalSubmitterGeneration, terminalWorker,
                    rejectedDetachedSubmitter, rejectedWrongSubmitter>>

DetachStartedSubmitter ==
    /\ phase = "Started"
    /\ ~detached
    /\ attachedSubmitter = AssignedSubmitter
    /\ detached' = TRUE
    /\ attachedSubmitter' = NoSubmitter
    /\ liveSubmitterGeneration' = 0
    /\ identityVisible' =
          IF MutantDropDetachedIdentity THEN FALSE ELSE identityVisible
    /\ UNCHANGED <<phase, begunEver, dispatchDebit, creditCount,
                    monitorBeginCount, startRevision, duplicateBeginCount,
                    invalidBeginCount, terminalCount, terminalOrigin,
                    terminalSubmitterGeneration, terminalWorker,
                    rejectedDetachedSubmitter, rejectedWrongWorker,
                    rejectedWrongSubmitter>>

DoneFromAttachedSubmitter ==
    /\ phase \in {"Waiting", "Started"}
    /\ ~detached
    /\ attachedSubmitter = AssignedSubmitter
    /\ liveSubmitterGeneration = AssignedGeneration
    /\ phase' = "Terminal"
    /\ dispatchDebit' = FALSE
    /\ creditCount' = creditCount + IF dispatchDebit THEN 1 ELSE 0
    /\ terminalCount' = terminalCount + 1
    /\ terminalOrigin' = "SubmitterDone"
    /\ terminalSubmitterGeneration' = AssignedGeneration
    /\ terminalWorker' = NoWorker
    /\ UNCHANGED <<attachedSubmitter, liveSubmitterGeneration, detached,
                    identityVisible, begunEver, monitorBeginCount,
                    startRevision, duplicateBeginCount, invalidBeginCount,
                    rejectedDetachedSubmitter, rejectedWrongWorker,
                    rejectedWrongSubmitter>>

DoneFromDetachedSubmitter ==
    /\ phase = "Started"
    /\ detached
    /\ attachedSubmitter = NoSubmitter
    /\ IF MutantDetachedSubmitterTerminates
          THEN /\ phase' = "Terminal"
               /\ terminalCount' = terminalCount + 1
               /\ terminalOrigin' = "SubmitterDone"
               /\ terminalSubmitterGeneration' = OtherGeneration
               /\ terminalWorker' = NoWorker
               /\ rejectedDetachedSubmitter' = rejectedDetachedSubmitter
          ELSE /\ UNCHANGED <<phase, terminalCount, terminalOrigin,
                              terminalSubmitterGeneration, terminalWorker>>
               /\ rejectedDetachedSubmitter' =
                     CapInc(rejectedDetachedSubmitter)
    /\ UNCHANGED <<attachedSubmitter, liveSubmitterGeneration, detached,
                    identityVisible, begunEver, dispatchDebit, creditCount,
                    monitorBeginCount, startRevision, duplicateBeginCount,
                    invalidBeginCount, rejectedWrongWorker,
                    rejectedWrongSubmitter>>

DoneFromWrongSubmitter ==
    /\ phase \in {"Waiting", "Started"}
    /\ ~detached
    /\ rejectedWrongSubmitter' = CapInc(rejectedWrongSubmitter)
    /\ UNCHANGED <<phase, attachedSubmitter, liveSubmitterGeneration,
                    detached, identityVisible, begunEver, dispatchDebit,
                    creditCount, monitorBeginCount, startRevision,
                    duplicateBeginCount, invalidBeginCount, terminalCount,
                    terminalOrigin, terminalSubmitterGeneration,
                    terminalWorker, rejectedDetachedSubmitter,
                    rejectedWrongWorker>>

DoneFromAssignedWorker ==
    /\ phase = "Started"
    /\ phase' = "Terminal"
    /\ terminalCount' = terminalCount + 1
    /\ terminalOrigin' = "WorkerDone"
    /\ terminalWorker' = AssignedWorker
    /\ terminalSubmitterGeneration' = 0
    /\ UNCHANGED <<attachedSubmitter, liveSubmitterGeneration, detached,
                    identityVisible, begunEver, dispatchDebit, creditCount,
                    monitorBeginCount, startRevision, duplicateBeginCount,
                    invalidBeginCount, rejectedDetachedSubmitter,
                    rejectedWrongWorker, rejectedWrongSubmitter>>

DoneFromWrongWorker ==
    /\ phase = "Started"
    /\ rejectedWrongWorker' = CapInc(rejectedWrongWorker)
    /\ UNCHANGED <<phase, attachedSubmitter, liveSubmitterGeneration,
                    detached, identityVisible, begunEver, dispatchDebit,
                    creditCount, monitorBeginCount, startRevision,
                    duplicateBeginCount, invalidBeginCount, terminalCount,
                    terminalOrigin, terminalSubmitterGeneration,
                    terminalWorker, rejectedDetachedSubmitter,
                    rejectedWrongSubmitter>>

AssignedWorkerLost ==
    /\ phase = "Started"
    /\ phase' = "Terminal"
    /\ terminalCount' = terminalCount + 1
    /\ terminalOrigin' = "WorkerLost"
    /\ terminalWorker' = AssignedWorker
    /\ terminalSubmitterGeneration' = 0
    /\ UNCHANGED <<attachedSubmitter, liveSubmitterGeneration, detached,
                    identityVisible, begunEver, dispatchDebit, creditCount,
                    monitorBeginCount, startRevision, duplicateBeginCount,
                    invalidBeginCount, rejectedDetachedSubmitter,
                    rejectedWrongWorker, rejectedWrongSubmitter>>

Next ==
    \/ BeginFromAssignedWorker
    \/ DuplicateBeginFromAssignedWorker
    \/ BeginFromWrongWorker
    \/ DetachStartedSubmitter
    \/ DoneFromAttachedSubmitter
    \/ DoneFromDetachedSubmitter
    \/ DoneFromWrongSubmitter
    \/ DoneFromAssignedWorker
    \/ DoneFromWrongWorker
    \/ AssignedWorkerLost

Spec == Init /\ [][Next]_vars

WorkerTerminalStep == DoneFromAssignedWorker \/ AssignedWorkerLost

FairSpec == Spec /\ WF_vars(WorkerTerminalStep)

TypeOK ==
    /\ phase \in Phases
    /\ attachedSubmitter \in {AssignedSubmitter, NoSubmitter}
    /\ liveSubmitterGeneration \in {0, AssignedGeneration}
    /\ detached \in BOOLEAN
    /\ identityVisible \in BOOLEAN
    /\ begunEver \in BOOLEAN
    /\ dispatchDebit \in BOOLEAN
    /\ creditCount \in 0..1
    /\ monitorBeginCount \in Nat
    /\ startRevision \in Nat
    /\ duplicateBeginCount \in 0..CountCap
    /\ invalidBeginCount \in 0..CountCap
    /\ terminalCount \in 0..1
    /\ terminalOrigin \in Origins
    /\ terminalSubmitterGeneration \in {0, AssignedGeneration, OtherGeneration}
    /\ terminalWorker \in {NoWorker, AssignedWorker}
    /\ rejectedDetachedSubmitter \in 0..CountCap
    /\ rejectedWrongWorker \in 0..CountCap
    /\ rejectedWrongSubmitter \in 0..CountCap

BeginLinearizedOnce ==
    /\ monitorBeginCount = IF begunEver THEN 1 ELSE 0
    /\ startRevision = IF begunEver THEN 1 ELSE 0

DispatchDebitReleasedOnce ==
    /\ dispatchDebit <=> phase = "Waiting"
    /\ creditCount = IF phase = "Waiting" THEN 0 ELSE 1

TerminalAtMostOnce == terminalCount <= 1

TerminalCoherence ==
    /\ (phase = "Terminal") <=> (terminalCount = 1)
    /\ (terminalCount = 0) <=> (terminalOrigin = "None")

DetachedTerminalAuthority ==
    detached /\ phase = "Terminal"
    => terminalOrigin \in {"WorkerDone", "WorkerLost"}

SubmitterGenerationAuthority ==
    terminalOrigin = "SubmitterDone"
    => /\ ~detached
       /\ terminalSubmitterGeneration = AssignedGeneration

WorkerAssignmentAuthority ==
    terminalOrigin \in {"WorkerDone", "WorkerLost"}
    => terminalWorker = AssignedWorker

StableDetachedIdentity == detached => identityVisible

DetachedImpliesBegun == detached => begunEver

SafetyInvariant ==
    /\ TypeOK
    /\ BeginLinearizedOnce
    /\ DispatchDebitReleasedOnce
    /\ TerminalAtMostOnce
    /\ TerminalCoherence
    /\ DetachedTerminalAuthority
    /\ SubmitterGenerationAuthority
    /\ WorkerAssignmentAuthority
    /\ StableDetachedIdentity
    /\ DetachedImpliesBegun

StartedEventuallyTerminates ==
    [](phase = "Started" => <> (phase = "Terminal"))

=============================================================================
