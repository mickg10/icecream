-------------------------- MODULE Protocol50JobLifecycle --------------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
Protocol-50 compiler-attempt and retained-input lifecycle.

The cache-transaction model proves that INPUT_COMMITTED publishes exact input.
This model starts at that boundary and deliberately does not duplicate
BODY/NEED/FILL, route history, or object publication.

Key separation:

  retained input:  one exact TU at one F-cache incarnation
  attempt:         one P50 or legacy compiler consumer
  logical job:     owns result arbitration and the restart lease

A compiler/clone restart after INPUT_COMMITTED creates a new attempt.  It does
not create a second cache commit on the same F.  Authorization is the atomic
handoff at which the compiler owns an independent complete immutable input
source.  A cache-sidecar restart may therefore cancel a waiting P50 attempt,
but cannot invalidate an already-authorized compiler.  A late result from a
cancelled/losing attempt can arrive but cannot win.
***************************************************************************)

CONSTANTS F0, F1, A0, A1,
          NoF, NoAttempt,
          P50Mode, LegacyMode, NoMode,
          New, Waiting, Running, Finished, Cancelled,
          NoResult, OkResult,
          ExactDigest, NoDigest,
          MutantCommitWithoutLease,
          MutantAuthorizeWithoutOwnedInput,
          MutantReleaseLeaseOnAttemptCancel

ASSUME /\ F0 # F1
       /\ A0 # A1
       /\ NoF \notin {F0, F1}
       /\ NoAttempt \notin {A0, A1}
       /\ P50Mode # LegacyMode
       /\ NoMode \notin {P50Mode, LegacyMode}
       /\ ExactDigest # NoDigest
       /\ MutantCommitWithoutLease \in BOOLEAN
       /\ MutantAuthorizeWithoutOwnedInput \in BOOLEAN
       /\ MutantReleaseLeaseOnAttemptCancel \in BOOLEAN

Fs == {F0, F1}
Attempts == {A0, A1}
Modes == {P50Mode, LegacyMode}
AttemptStates == {New, Waiting, Running, Finished, Cancelled}
Results == {NoResult, OkResult}
Digests == {ExactDigest, NoDigest}

VARIABLES inputCommitted, inputDigest, inputLease,
          fCacheEpoch, environmentReady, jobOpen,
          attemptState, attemptMode, attemptF, attemptEpoch,
          attemptEligible, attemptAuthorized, attemptOwnsInput,
          legacyInputReady, result, acceptedAttempt

vars == <<inputCommitted, inputDigest, inputLease,
          fCacheEpoch, environmentReady, jobOpen,
          attemptState, attemptMode, attemptF, attemptEpoch,
          attemptEligible, attemptAuthorized, attemptOwnsInput,
          legacyInputReady, result, acceptedAttempt>>

Init ==
    /\ inputCommitted = [f \in Fs |-> FALSE]
    /\ inputDigest = [f \in Fs |-> NoDigest]
    /\ inputLease = [f \in Fs |-> FALSE]
    /\ fCacheEpoch = [f \in Fs |-> 0]
    /\ environmentReady = [f \in Fs |-> FALSE]
    /\ jobOpen = TRUE
    /\ attemptState = [a \in Attempts |-> New]
    /\ attemptMode = [a \in Attempts |-> NoMode]
    /\ attemptF = [a \in Attempts |-> NoF]
    /\ attemptEpoch = [a \in Attempts |-> 0]
    /\ attemptEligible = [a \in Attempts |-> FALSE]
    /\ attemptAuthorized = {}
    /\ attemptOwnsInput = {}
    /\ legacyInputReady = [a \in Attempts |-> FALSE]
    /\ result = [a \in Attempts |-> NoResult]
    /\ acceptedAttempt = NoAttempt

CommitExactInput(f) ==
    /\ f \in Fs
    /\ jobOpen
    /\ inputCommitted' = [inputCommitted EXCEPT ![f] = TRUE]
    /\ inputDigest' = [inputDigest EXCEPT ![f] = ExactDigest]
    /\ inputLease' =
        IF MutantCommitWithoutLease
        THEN inputLease
        ELSE [inputLease EXCEPT ![f] = TRUE]
    /\ UNCHANGED <<fCacheEpoch, environmentReady, jobOpen,
                    attemptState, attemptMode, attemptF, attemptEpoch,
                    attemptEligible, attemptAuthorized, attemptOwnsInput,
                    legacyInputReady, result, acceptedAttempt>>

StartAttempt(a, f, mode) ==
    /\ a \in Attempts
    /\ f \in Fs
    /\ mode \in Modes
    /\ jobOpen
    /\ acceptedAttempt = NoAttempt
    /\ attemptState[a] = New
    /\ attemptState' = [attemptState EXCEPT ![a] = Waiting]
    /\ attemptMode' = [attemptMode EXCEPT ![a] = mode]
    /\ attemptF' = [attemptF EXCEPT ![a] = f]
    /\ attemptEpoch' = [attemptEpoch EXCEPT ![a] = fCacheEpoch[f]]
    /\ attemptEligible' = [attemptEligible EXCEPT ![a] = TRUE]
    /\ inputLease' =
        IF mode = P50Mode
        THEN [inputLease EXCEPT ![f] = TRUE]
        ELSE inputLease
    /\ UNCHANGED <<inputCommitted, inputDigest, fCacheEpoch,
                    environmentReady, jobOpen, attemptAuthorized,
                    attemptOwnsInput, legacyInputReady, result,
                    acceptedAttempt>>

MakeEnvironmentReady(f) ==
    /\ f \in Fs
    /\ ~environmentReady[f]
    /\ environmentReady' = [environmentReady EXCEPT ![f] = TRUE]
    /\ UNCHANGED <<inputCommitted, inputDigest, inputLease, fCacheEpoch,
                    jobOpen, attemptState, attemptMode, attemptF,
                    attemptEpoch, attemptEligible, attemptAuthorized,
                    attemptOwnsInput, legacyInputReady, result,
                    acceptedAttempt>>

MakeLegacyInputReady(a) ==
    /\ a \in Attempts
    /\ attemptState[a] = Waiting
    /\ attemptMode[a] = LegacyMode
    /\ attemptEligible[a]
    /\ legacyInputReady' = [legacyInputReady EXCEPT ![a] = TRUE]
    /\ UNCHANGED <<inputCommitted, inputDigest, inputLease, fCacheEpoch,
                    environmentReady, jobOpen, attemptState, attemptMode,
                    attemptF, attemptEpoch, attemptEligible,
                    attemptAuthorized, attemptOwnsInput, result,
                    acceptedAttempt>>

StartCompiler(a) ==
    /\ a \in Attempts
    /\ attemptState[a] = Waiting
    /\ attemptEligible[a]
    /\ jobOpen
    /\ attemptF[a] \in Fs
    /\ environmentReady[attemptF[a]]
    /\ IF attemptMode[a] = P50Mode
          THEN /\ attemptEpoch[a] = fCacheEpoch[attemptF[a]]
               /\ inputLease[attemptF[a]]
               /\ inputCommitted[attemptF[a]]
               /\ inputDigest[attemptF[a]] = ExactDigest
          ELSE /\ attemptMode[a] = LegacyMode
               /\ legacyInputReady[a]
    /\ attemptState' = [attemptState EXCEPT ![a] = Running]
    /\ attemptAuthorized' = attemptAuthorized \cup {a}
    /\ attemptOwnsInput' =
        IF MutantAuthorizeWithoutOwnedInput
        THEN attemptOwnsInput
        ELSE attemptOwnsInput \cup {a}
    /\ UNCHANGED <<inputCommitted, inputDigest, inputLease, fCacheEpoch,
                    environmentReady, jobOpen, attemptMode, attemptF,
                    attemptEpoch, attemptEligible, legacyInputReady,
                    result, acceptedAttempt>>

FinishCompiler(a) ==
    /\ a \in Attempts
    /\ attemptState[a] = Running
    /\ a \in attemptAuthorized
    /\ a \in attemptOwnsInput
    /\ attemptState' = [attemptState EXCEPT ![a] = Finished]
    /\ result' = [result EXCEPT ![a] = OkResult]
    /\ UNCHANGED <<inputCommitted, inputDigest, inputLease, fCacheEpoch,
                    environmentReady, jobOpen, attemptMode, attemptF,
                    attemptEpoch, attemptEligible, attemptAuthorized,
                    attemptOwnsInput, legacyInputReady, acceptedAttempt>>

CancelAttempt(a) ==
    /\ a \in Attempts
    /\ attemptState[a] \in {Waiting, Running}
    /\ attemptState' = [attemptState EXCEPT ![a] = Cancelled]
    /\ attemptEligible' = [attemptEligible EXCEPT ![a] = FALSE]
    /\ inputLease' =
        IF MutantReleaseLeaseOnAttemptCancel /\
           attemptMode[a] = P50Mode /\ attemptF[a] \in Fs
        THEN [inputLease EXCEPT ![attemptF[a]] = FALSE]
        ELSE inputLease
    /\ UNCHANGED <<inputCommitted, inputDigest, fCacheEpoch,
                    environmentReady, jobOpen, attemptMode, attemptF,
                    attemptEpoch, attemptAuthorized, attemptOwnsInput,
                    legacyInputReady, result, acceptedAttempt>>

LateResult(a) ==
    /\ a \in Attempts
    /\ attemptState[a] = Cancelled
    /\ a \in attemptAuthorized
    /\ a \in attemptOwnsInput
    /\ result[a] = NoResult
    /\ result' = [result EXCEPT ![a] = OkResult]
    /\ UNCHANGED <<inputCommitted, inputDigest, inputLease, fCacheEpoch,
                    environmentReady, jobOpen, attemptState, attemptMode,
                    attemptF, attemptEpoch, attemptEligible,
                    attemptAuthorized, attemptOwnsInput, legacyInputReady,
                    acceptedAttempt>>

AcceptResult(a) ==
    /\ a \in Attempts
    /\ jobOpen
    /\ acceptedAttempt = NoAttempt
    /\ attemptState[a] = Finished
    /\ attemptEligible[a]
    /\ a \in attemptAuthorized
    /\ a \in attemptOwnsInput
    /\ result[a] = OkResult
    /\ acceptedAttempt' = a
    /\ jobOpen' = FALSE
    /\ inputLease' = [f \in Fs |-> FALSE]
    /\ attemptEligible' =
        [x \in Attempts |-> IF x = a THEN TRUE ELSE FALSE]
    /\ UNCHANGED <<inputCommitted, inputDigest, fCacheEpoch,
                    environmentReady, attemptState, attemptMode, attemptF,
                    attemptEpoch, attemptAuthorized, attemptOwnsInput,
                    legacyInputReady, result>>

CancelJob ==
    /\ jobOpen
    /\ acceptedAttempt = NoAttempt
    /\ jobOpen' = FALSE
    /\ inputLease' = [f \in Fs |-> FALSE]
    /\ attemptState' =
        [a \in Attempts |->
            IF attemptState[a] \in {Waiting, Running}
            THEN Cancelled ELSE attemptState[a]]
    /\ attemptEligible' = [a \in Attempts |-> FALSE]
    /\ UNCHANGED <<inputCommitted, inputDigest, fCacheEpoch,
                    environmentReady, attemptMode, attemptF, attemptEpoch,
                    attemptAuthorized, attemptOwnsInput, legacyInputReady,
                    result, acceptedAttempt>>

EvictCommittedInput(f) ==
    /\ f \in Fs
    /\ inputCommitted[f]
    /\ ~inputLease[f]
    /\ inputCommitted' = [inputCommitted EXCEPT ![f] = FALSE]
    /\ inputDigest' = [inputDigest EXCEPT ![f] = NoDigest]
    /\ UNCHANGED <<inputLease, fCacheEpoch, environmentReady, jobOpen,
                    attemptState, attemptMode, attemptF, attemptEpoch,
                    attemptEligible, attemptAuthorized, attemptOwnsInput,
                    legacyInputReady, result, acceptedAttempt>>

RestartFCache(f) ==
    /\ f \in Fs
    /\ fCacheEpoch' = [fCacheEpoch EXCEPT ![f] = 1 - @]
    /\ inputCommitted' = [inputCommitted EXCEPT ![f] = FALSE]
    /\ inputDigest' = [inputDigest EXCEPT ![f] = NoDigest]
    /\ inputLease' = [inputLease EXCEPT ![f] = FALSE]
    /\ attemptState' =
        [a \in Attempts |->
            IF attemptF[a] = f /\ attemptMode[a] = P50Mode /\
               attemptState[a] = Waiting
            THEN Cancelled ELSE attemptState[a]]
    /\ attemptEligible' =
        [a \in Attempts |->
            IF attemptF[a] = f /\ attemptMode[a] = P50Mode /\
               attemptState[a] = Waiting
            THEN FALSE ELSE attemptEligible[a]]
    /\ UNCHANGED <<environmentReady, jobOpen, attemptMode, attemptF,
                    attemptEpoch, attemptAuthorized, attemptOwnsInput,
                    legacyInputReady, result, acceptedAttempt>>

Next ==
    \/ \E f \in Fs : CommitExactInput(f)
    \/ \E a \in Attempts, f \in Fs, mode \in Modes : StartAttempt(a, f, mode)
    \/ \E f \in Fs : MakeEnvironmentReady(f)
    \/ \E a \in Attempts : MakeLegacyInputReady(a)
    \/ \E a \in Attempts : StartCompiler(a)
    \/ \E a \in Attempts : FinishCompiler(a)
    \/ \E a \in Attempts : CancelAttempt(a)
    \/ \E a \in Attempts : LateResult(a)
    \/ \E a \in Attempts : AcceptResult(a)
    \/ CancelJob
    \/ \E f \in Fs : EvictCommittedInput(f)
    \/ \E f \in Fs : RestartFCache(f)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ inputCommitted \in [Fs -> BOOLEAN]
    /\ inputDigest \in [Fs -> Digests]
    /\ inputLease \in [Fs -> BOOLEAN]
    /\ fCacheEpoch \in [Fs -> 0..1]
    /\ environmentReady \in [Fs -> BOOLEAN]
    /\ jobOpen \in BOOLEAN
    /\ attemptState \in [Attempts -> AttemptStates]
    /\ attemptMode \in [Attempts -> Modes \cup {NoMode}]
    /\ attemptF \in [Attempts -> Fs \cup {NoF}]
    /\ attemptEpoch \in [Attempts -> 0..1]
    /\ attemptEligible \in [Attempts -> BOOLEAN]
    /\ attemptAuthorized \subseteq Attempts
    /\ attemptOwnsInput \subseteq Attempts
    /\ legacyInputReady \in [Attempts -> BOOLEAN]
    /\ result \in [Attempts -> Results]
    /\ acceptedAttempt \in Attempts \cup {NoAttempt}

CommittedInputIsExact ==
    \A f \in Fs : inputCommitted[f] <=> inputDigest[f] = ExactDigest

AttemptIdentityIsImmutable ==
    \A a \in Attempts :
        IF attemptState[a] = New
        THEN /\ attemptMode[a] = NoMode
             /\ attemptF[a] = NoF
             /\ ~attemptEligible[a]
             /\ a \notin attemptAuthorized
             /\ a \notin attemptOwnsInput
        ELSE /\ attemptMode[a] \in Modes
             /\ attemptF[a] \in Fs

AuthorizedAttemptIsWellFormed ==
    \A a \in attemptAuthorized :
        /\ attemptMode[a] \in Modes
        /\ attemptF[a] \in Fs
        /\ attemptState[a] \in {Running, Finished, Cancelled}

AuthorizedAttemptOwnsIndependentInput ==
    attemptAuthorized \subseteq attemptOwnsInput

OwnedInputWasAuthorized ==
    attemptOwnsInput \subseteq attemptAuthorized

RunningAndFinishedWereAuthorized ==
    \A a \in Attempts :
        attemptState[a] \in {Running, Finished} =>
            /\ a \in attemptAuthorized
            /\ a \in attemptOwnsInput

WaitingP50AttemptOwnsRestartLease ==
    \A a \in Attempts :
        IF attemptState[a] = Waiting /\ attemptEligible[a] /\
           attemptMode[a] = P50Mode
        THEN inputLease[attemptF[a]]
        ELSE TRUE

InputLeaseBelongsToOpenJob ==
    (\E f \in Fs : inputLease[f]) => jobOpen

CommittedInputForOpenJobKeepsLease ==
    \A f \in Fs :
        jobOpen /\ inputCommitted[f] => inputLease[f]

AcceptedResultIsUniqueAndValid ==
    IF acceptedAttempt = NoAttempt
    THEN TRUE
    ELSE /\ acceptedAttempt \in Attempts
         /\ ~jobOpen
         /\ attemptState[acceptedAttempt] = Finished
         /\ attemptEligible[acceptedAttempt]
         /\ acceptedAttempt \in attemptAuthorized
         /\ acceptedAttempt \in attemptOwnsInput
         /\ result[acceptedAttempt] = OkResult

CancelledAttemptCannotWin ==
    \A a \in Attempts : attemptState[a] = Cancelled => acceptedAttempt # a

Invariants ==
    /\ TypeOK
    /\ CommittedInputIsExact
    /\ AttemptIdentityIsImmutable
    /\ AuthorizedAttemptIsWellFormed
    /\ AuthorizedAttemptOwnsIndependentInput
    /\ OwnedInputWasAuthorized
    /\ RunningAndFinishedWereAuthorized
    /\ WaitingP50AttemptOwnsRestartLease
    /\ InputLeaseBelongsToOpenJob
    /\ CommittedInputForOpenJobKeepsLease
    /\ AcceptedResultIsUniqueAndValid
    /\ CancelledAttemptCannotWin

(***************************************************************************
Scoped progress theorem for a valid job restart.  A first P50 attempt has died
before authorization; the exact committed InputRecord and job lease remain.
Under weak fairness of the four replacement-attempt steps, A1 attaches to the
same record, acquires independent input ownership, finishes, and wins.  There
is deliberately no second CommitExactInput transition in RestartNext.
***************************************************************************)

RestartInit ==
    /\ inputCommitted = [f \in Fs |-> f = F0]
    /\ inputDigest =
        [f \in Fs |-> IF f = F0 THEN ExactDigest ELSE NoDigest]
    /\ inputLease = [f \in Fs |-> f = F0]
    /\ fCacheEpoch = [f \in Fs |-> 0]
    /\ environmentReady = [f \in Fs |-> f = F0]
    /\ jobOpen = TRUE
    /\ attemptState =
        [a \in Attempts |-> IF a = A0 THEN Cancelled ELSE New]
    /\ attemptMode =
        [a \in Attempts |-> IF a = A0 THEN P50Mode ELSE NoMode]
    /\ attemptF =
        [a \in Attempts |-> IF a = A0 THEN F0 ELSE NoF]
    /\ attemptEpoch = [a \in Attempts |-> 0]
    /\ attemptEligible = [a \in Attempts |-> FALSE]
    /\ attemptAuthorized = {}
    /\ attemptOwnsInput = {}
    /\ legacyInputReady = [a \in Attempts |-> FALSE]
    /\ result = [a \in Attempts |-> NoResult]
    /\ acceptedAttempt = NoAttempt

RestartNext ==
    \/ StartAttempt(A1, F0, P50Mode)
    \/ StartCompiler(A1)
    \/ FinishCompiler(A1)
    \/ AcceptResult(A1)

RestartSpec ==
    /\ RestartInit
    /\ [][RestartNext]_vars
    /\ WF_vars(StartAttempt(A1, F0, P50Mode))
    /\ WF_vars(StartCompiler(A1))
    /\ WF_vars(FinishCompiler(A1))
    /\ WF_vars(AcceptResult(A1))

ReplacementAttemptEventuallyAccepted ==
    <> (acceptedAttempt = A1)

=============================================================================
