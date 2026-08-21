-------------------------- MODULE Protocol50JobLifecycle --------------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
This is a deliberately small extension of Protocol50.tla.  Protocol50.tla
models the cache transaction and lost-commit window.  This module models the
orthogonal job-attempt layer that consumes an already exact input.

The important separation is:

  cache input identity:   (C_STORE_GUID, TU_SEQ) at one F
  compile attempt:        one P50 or legacy consumer of that input

A restarted compiler job may attach again to the same committed input.  A
cancelled or stale attempt may still return a late result, but it can never win
result arbitration.  Restarting an F invalidates its committed input and all
waiting/running attempts on that F; it does not affect another F.
***************************************************************************)

CONSTANTS F0, F1, A0, A1,
          NoF, NoAttempt,
          P50Mode, LegacyMode, NoMode,
          New, Waiting, Running, Finished, Cancelled,
          NoResult, OkResult,
          ExactDigest, NoDigest

Fs == {F0, F1}
Attempts == {A0, A1}
Modes == {P50Mode, LegacyMode}
AttemptStates == {New, Waiting, Running, Finished, Cancelled}
Results == {NoResult, OkResult}
Digests == {ExactDigest, NoDigest}

AttachedP50(f) ==
    \E a \in Attempts:
        /\ attemptF[a] = f
        /\ attemptMode[a] = P50Mode
        /\ eligible[a]
        /\ attemptState[a] \in {Waiting, Running}

VARIABLES inputCommitted, inputDigest, fEpoch, environmentReady,
          attemptState, attemptMode, attemptF, attemptEpoch,
          eligible, legacyInputReady, result, acceptedAttempt

vars == <<inputCommitted, inputDigest, fEpoch, environmentReady,
          attemptState, attemptMode, attemptF, attemptEpoch,
          eligible, legacyInputReady, result, acceptedAttempt>>

Init ==
    /\ inputCommitted = [f \in Fs |-> FALSE]
    /\ inputDigest = [f \in Fs |-> NoDigest]
    /\ fEpoch = [f \in Fs |-> 0]
    /\ environmentReady = [f \in Fs |-> FALSE]
    /\ attemptState = [a \in Attempts |-> New]
    /\ attemptMode = [a \in Attempts |-> NoMode]
    /\ attemptF = [a \in Attempts |-> NoF]
    /\ attemptEpoch = [a \in Attempts |-> 0]
    /\ eligible = [a \in Attempts |-> FALSE]
    /\ legacyInputReady = [a \in Attempts |-> FALSE]
    /\ result = [a \in Attempts |-> NoResult]
    /\ acceptedAttempt = NoAttempt

CommitExactInput(f) ==
    /\ f \in Fs
    /\ inputCommitted' = [inputCommitted EXCEPT ![f] = TRUE]
    /\ inputDigest' = [inputDigest EXCEPT ![f] = ExactDigest]
    /\ UNCHANGED <<fEpoch, environmentReady,
                    attemptState, attemptMode, attemptF, attemptEpoch,
                    eligible, legacyInputReady, result, acceptedAttempt>>

StartAttempt(a, f, mode) ==
    /\ a \in Attempts
    /\ f \in Fs
    /\ mode \in Modes
    /\ attemptState[a] = New
    /\ attemptState' = [attemptState EXCEPT ![a] = Waiting]
    /\ attemptMode' = [attemptMode EXCEPT ![a] = mode]
    /\ attemptF' = [attemptF EXCEPT ![a] = f]
    /\ attemptEpoch' = [attemptEpoch EXCEPT ![a] = fEpoch[f]]
    /\ eligible' = [eligible EXCEPT ![a] = TRUE]
    /\ UNCHANGED <<inputCommitted, inputDigest, fEpoch, environmentReady,
                    legacyInputReady, result, acceptedAttempt>>

MakeEnvironmentReady(f) ==
    /\ f \in Fs
    /\ environmentReady' = [environmentReady EXCEPT ![f] = TRUE]
    /\ UNCHANGED <<inputCommitted, inputDigest, fEpoch,
                    attemptState, attemptMode, attemptF, attemptEpoch,
                    eligible, legacyInputReady, result, acceptedAttempt>>

MakeLegacyInputReady(a) ==
    /\ a \in Attempts
    /\ attemptState[a] = Waiting
    /\ attemptMode[a] = LegacyMode
    /\ legacyInputReady' = [legacyInputReady EXCEPT ![a] = TRUE]
    /\ UNCHANGED <<inputCommitted, inputDigest, fEpoch, environmentReady,
                    attemptState, attemptMode, attemptF, attemptEpoch,
                    eligible, result, acceptedAttempt>>

StartCompiler(a) ==
    /\ a \in Attempts
    /\ attemptState[a] = Waiting
    /\ eligible[a]
    /\ attemptF[a] \in Fs
    /\ attemptEpoch[a] = fEpoch[attemptF[a]]
    /\ environmentReady[attemptF[a]]
    /\ IF attemptMode[a] = P50Mode
          THEN /\ inputCommitted[attemptF[a]]
               /\ inputDigest[attemptF[a]] = ExactDigest
          ELSE /\ attemptMode[a] = LegacyMode
               /\ legacyInputReady[a]
    /\ attemptState' = [attemptState EXCEPT ![a] = Running]
    /\ UNCHANGED <<inputCommitted, inputDigest, fEpoch, environmentReady,
                    attemptMode, attemptF, attemptEpoch, eligible,
                    legacyInputReady, result, acceptedAttempt>>

FinishCompiler(a) ==
    /\ a \in Attempts
    /\ attemptState[a] = Running
    /\ attemptState' = [attemptState EXCEPT ![a] = Finished]
    /\ result' = [result EXCEPT ![a] = OkResult]
    /\ UNCHANGED <<inputCommitted, inputDigest, fEpoch, environmentReady,
                    attemptMode, attemptF, attemptEpoch, eligible,
                    legacyInputReady, acceptedAttempt>>

CancelAttempt(a) ==
    /\ a \in Attempts
    /\ attemptState[a] \in {Waiting, Running}
    /\ attemptState' = [attemptState EXCEPT ![a] = Cancelled]
    /\ eligible' = [eligible EXCEPT ![a] = FALSE]
    /\ UNCHANGED <<inputCommitted, inputDigest, fEpoch, environmentReady,
                    attemptMode, attemptF, attemptEpoch,
                    legacyInputReady, result, acceptedAttempt>>

LateResult(a) ==
    /\ a \in Attempts
    /\ attemptState[a] = Cancelled
    /\ result[a] = NoResult
    /\ result' = [result EXCEPT ![a] = OkResult]
    /\ UNCHANGED <<inputCommitted, inputDigest, fEpoch, environmentReady,
                    attemptState, attemptMode, attemptF, attemptEpoch,
                    eligible, legacyInputReady, acceptedAttempt>>

AcceptResult(a) ==
    /\ a \in Attempts
    /\ acceptedAttempt = NoAttempt
    /\ attemptState[a] = Finished
    /\ eligible[a]
    /\ result[a] = OkResult
    /\ acceptedAttempt' = a
    /\ UNCHANGED <<inputCommitted, inputDigest, fEpoch, environmentReady,
                    attemptState, attemptMode, attemptF, attemptEpoch,
                    eligible, legacyInputReady, result>>

EvictCommittedInput(f) ==
    /\ f \in Fs
    /\ inputCommitted[f]
    /\ ~AttachedP50(f)
    /\ inputCommitted' = [inputCommitted EXCEPT ![f] = FALSE]
    /\ inputDigest' = [inputDigest EXCEPT ![f] = NoDigest]
    /\ UNCHANGED <<fEpoch, environmentReady,
                    attemptState, attemptMode, attemptF, attemptEpoch,
                    eligible, legacyInputReady, result, acceptedAttempt>>

RestartF(f) ==
    /\ f \in Fs
    /\ fEpoch' = [fEpoch EXCEPT ![f] = 1 - @]
    /\ inputCommitted' = [inputCommitted EXCEPT ![f] = FALSE]
    /\ inputDigest' = [inputDigest EXCEPT ![f] = NoDigest]
    /\ environmentReady' = [environmentReady EXCEPT ![f] = FALSE]
    /\ attemptState' =
        [a \in Attempts |->
            IF attemptF[a] = f /\ attemptState[a] \in {Waiting, Running}
            THEN Cancelled ELSE attemptState[a]]
    /\ eligible' =
        [a \in Attempts |->
            IF attemptF[a] = f /\ attemptState[a] \in {Waiting, Running}
            THEN FALSE ELSE eligible[a]]
    /\ UNCHANGED <<attemptMode, attemptF, attemptEpoch,
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
    \/ \E f \in Fs : EvictCommittedInput(f)
    \/ \E f \in Fs : RestartF(f)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ inputCommitted \in [Fs -> BOOLEAN]
    /\ inputDigest \in [Fs -> Digests]
    /\ fEpoch \in [Fs -> 0..1]
    /\ environmentReady \in [Fs -> BOOLEAN]
    /\ attemptState \in [Attempts -> AttemptStates]
    /\ attemptMode \in [Attempts -> Modes \cup {NoMode}]
    /\ attemptF \in [Attempts -> Fs \cup {NoF}]
    /\ attemptEpoch \in [Attempts -> 0..1]
    /\ eligible \in [Attempts -> BOOLEAN]
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
             /\ ~eligible[a]
        ELSE /\ attemptMode[a] \in Modes
             /\ attemptF[a] \in Fs

CompilerUsesExactlyOneReadyInputMode ==
    \A a \in Attempts :
        IF attemptState[a] = Running
        THEN /\ eligible[a]
             /\ attemptEpoch[a] = fEpoch[attemptF[a]]
             /\ environmentReady[attemptF[a]]
             /\ IF attemptMode[a] = P50Mode
                   THEN /\ inputCommitted[attemptF[a]]
                        /\ inputDigest[attemptF[a]] = ExactDigest
                   ELSE /\ attemptMode[a] = LegacyMode
                        /\ legacyInputReady[a]
        ELSE TRUE

AcceptedResultIsUniqueAndValid ==
    IF acceptedAttempt = NoAttempt
    THEN TRUE
    ELSE /\ acceptedAttempt \in Attempts
         /\ attemptState[acceptedAttempt] = Finished
         /\ eligible[acceptedAttempt]
         /\ result[acceptedAttempt] = OkResult

CancelledAttemptCannotWin ==
    \A a \in Attempts : attemptState[a] = Cancelled => acceptedAttempt # a

Invariants ==
    /\ TypeOK
    /\ CommittedInputIsExact
    /\ AttemptIdentityIsImmutable
    /\ CompilerUsesExactlyOneReadyInputMode
    /\ AcceptedResultIsUniqueAndValid
    /\ CancelledAttemptCannotWin

=============================================================================
