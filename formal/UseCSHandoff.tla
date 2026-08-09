-------------------------- MODULE UseCSHandoff --------------------------
(***************************************************************************
Protocol-neutral submitter-daemon handoff correctness.

The daemon receives an exact scheduler assignment id before attempting to
forward UseCS to its local client. A complete frame creates delivery
uncertainty. Any failure before the final byte instead enters
ExactAbortPending and must either deliver an exact terminal settlement or lose
the scheduler session. A local client id is never a substitute for the exact
scheduler id.

FailureAt records a real deterministic byte cut. With FrameBytes = 4 the
acceptance matrix reaches cuts before byte 1, after one header byte, in the
body, and one byte short of completion.
***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS FrameBytes, NoFailure,
          MutantStoreIdAfterWrite,
          MutantUseLocalAliasOnFailure,
          MutantDropAbortOnTransportFailure

ASSUME /\ FrameBytes \in Nat \ {0}
       /\ NoFailure \notin Nat
       /\ MutantStoreIdAfterWrite \in BOOLEAN
       /\ MutantUseLocalAliasOnFailure \in BOOLEAN
       /\ MutantDropAbortOnTransportFailure \in BOOLEAN

Phases ==
    {"Idle", "Forwarding", "DeliveryUncertain",
     "ExactAbortPending", "Terminal"}
TerminalCauses ==
    {"None", "ExactAbort", "AliasAbort", "SchedulerSessionLoss", "Dropped"}
FailureValues == 0..(FrameBytes - 1) \cup {NoFailure}

VARIABLES phase,
          exactIdKnown,
          bytesWritten,
          frameCommitted,
          failureObservedAt,
          abortQueued,
          abortConsumed,
          schedulerLive,
          localAliasUsed,
          terminalCount,
          terminalCause,
          abortFailureInjected

vars ==
    <<phase, exactIdKnown, bytesWritten, frameCommitted,
      failureObservedAt, abortQueued, abortConsumed, schedulerLive,
      localAliasUsed, terminalCount, terminalCause,
      abortFailureInjected>>

Init ==
    /\ phase = "Idle"
    /\ exactIdKnown = FALSE
    /\ bytesWritten = 0
    /\ frameCommitted = FALSE
    /\ failureObservedAt = NoFailure
    /\ abortQueued = FALSE
    /\ abortConsumed = FALSE
    /\ schedulerLive = TRUE
    /\ localAliasUsed = FALSE
    /\ terminalCount = 0
    /\ terminalCause = "None"
    /\ abortFailureInjected = FALSE

BeginForward ==
    /\ phase = "Idle"
    /\ phase' = "Forwarding"
    /\ exactIdKnown' = ~MutantStoreIdAfterWrite
    /\ UNCHANGED <<bytesWritten, frameCommitted, failureObservedAt,
                    abortQueued, abortConsumed, schedulerLive,
                    localAliasUsed, terminalCount, terminalCause,
                    abortFailureInjected>>

WriteClientFrameByte ==
    /\ phase = "Forwarding"
    /\ bytesWritten < FrameBytes
    /\ LET nextBytes == bytesWritten + 1
       IN /\ bytesWritten' = nextBytes
          /\ frameCommitted' = (nextBytes = FrameBytes)
          /\ phase' =
                IF nextBytes = FrameBytes
                THEN "DeliveryUncertain"
                ELSE "Forwarding"
          /\ exactIdKnown' =
                (exactIdKnown
                 \/ (MutantStoreIdAfterWrite /\ nextBytes = FrameBytes))
    /\ UNCHANGED <<failureObservedAt, abortQueued, abortConsumed,
                    schedulerLive, localAliasUsed, terminalCount,
                    terminalCause, abortFailureInjected>>

FailClientWrite ==
    /\ phase = "Forwarding"
    /\ bytesWritten < FrameBytes
    /\ phase' = "ExactAbortPending"
    /\ failureObservedAt' = bytesWritten
    /\ UNCHANGED <<exactIdKnown, bytesWritten, frameCommitted,
                    abortQueued, abortConsumed, schedulerLive,
                    localAliasUsed, terminalCount, terminalCause,
                    abortFailureInjected>>

QueueExactAbort ==
    /\ phase = "ExactAbortPending"
    /\ exactIdKnown
    /\ schedulerLive
    /\ ~abortQueued
    /\ abortQueued' = TRUE
    /\ UNCHANGED <<phase, exactIdKnown, bytesWritten, frameCommitted,
                    failureObservedAt, abortConsumed, schedulerLive,
                    localAliasUsed, terminalCount, terminalCause,
                    abortFailureInjected>>

QueueLocalAliasAbort ==
    /\ phase = "ExactAbortPending"
    /\ ~exactIdKnown
    /\ schedulerLive
    /\ ~abortQueued
    /\ MutantUseLocalAliasOnFailure
    /\ abortQueued' = TRUE
    /\ localAliasUsed' = TRUE
    /\ UNCHANGED <<phase, exactIdKnown, bytesWritten, frameCommitted,
                    failureObservedAt, abortConsumed, schedulerLive,
                    terminalCount, terminalCause, abortFailureInjected>>

ConsumeAbort ==
    /\ phase = "ExactAbortPending"
    /\ abortQueued
    /\ schedulerLive
    /\ phase' = "Terminal"
    /\ abortConsumed' = TRUE
    /\ terminalCount' = terminalCount + 1
    /\ terminalCause' =
          IF localAliasUsed THEN "AliasAbort" ELSE "ExactAbort"
    /\ UNCHANGED <<exactIdKnown, bytesWritten, frameCommitted,
                    failureObservedAt, abortQueued, schedulerLive,
                    localAliasUsed, abortFailureInjected>>

AbortTransportFailure ==
    /\ phase = "ExactAbortPending"
    /\ exactIdKnown
    /\ schedulerLive
    /\ ~abortQueued
    /\ ~abortFailureInjected
    /\ abortFailureInjected' = TRUE
    /\ phase' = "Terminal"
    /\ terminalCount' = terminalCount + 1
    /\ IF MutantDropAbortOnTransportFailure
          THEN /\ schedulerLive' = TRUE
               /\ terminalCause' = "Dropped"
          ELSE /\ schedulerLive' = FALSE
               /\ terminalCause' = "SchedulerSessionLoss"
    /\ UNCHANGED <<exactIdKnown, bytesWritten, frameCommitted,
                    failureObservedAt, abortQueued, abortConsumed,
                    localAliasUsed>>

Next ==
    \/ BeginForward
    \/ WriteClientFrameByte
    \/ FailClientWrite
    \/ QueueExactAbort
    \/ QueueLocalAliasAbort
    \/ ConsumeAbort
    \/ AbortTransportFailure

Spec == Init /\ [][Next]_vars

AbortProgress ==
    QueueExactAbort \/ QueueLocalAliasAbort
    \/ ConsumeAbort \/ AbortTransportFailure

FairSpec ==
    /\ Spec
    /\ WF_vars(AbortProgress)

TypeOK ==
    /\ phase \in Phases
    /\ exactIdKnown \in BOOLEAN
    /\ bytesWritten \in 0..FrameBytes
    /\ frameCommitted \in BOOLEAN
    /\ failureObservedAt \in FailureValues
    /\ abortQueued \in BOOLEAN
    /\ abortConsumed \in BOOLEAN
    /\ schedulerLive \in BOOLEAN
    /\ localAliasUsed \in BOOLEAN
    /\ terminalCount \in 0..1
    /\ terminalCause \in TerminalCauses
    /\ abortFailureInjected \in BOOLEAN

FrameCommitExact ==
    frameCommitted <=> bytesWritten = FrameBytes

FailureIsPrecommit ==
    failureObservedAt # NoFailure => ~frameCommitted

ExactIdentityBeforeForward ==
    phase # "Idle" => exactIdKnown

NoLocalAliasSettlement == ~localAliasUsed

NoSilentAbortLoss == terminalCause # "Dropped"

ExactTerminalSettlement ==
    terminalCause = "ExactAbort" => abortConsumed /\ exactIdKnown

SessionLossIsExplicit ==
    terminalCause = "SchedulerSessionLoss" => ~schedulerLive

TerminalCoherence ==
    (phase = "Terminal") <=> (terminalCount = 1)

SafetyInvariant ==
    /\ TypeOK
    /\ FrameCommitExact
    /\ FailureIsPrecommit
    /\ ExactIdentityBeforeForward
    /\ NoLocalAliasSettlement
    /\ NoSilentAbortLoss
    /\ ExactTerminalSettlement
    /\ SessionLossIsExplicit
    /\ TerminalCoherence

FailedFrameEventuallySettles ==
    [](failureObservedAt # NoFailure => <> (phase = "Terminal"))

NoFailureAt0 == failureObservedAt # 0
NoFailureAt1 == failureObservedAt # 1
NoFailureAt2 == failureObservedAt # 2
NoFailureAt3 == failureObservedAt # 3

=============================================================================
