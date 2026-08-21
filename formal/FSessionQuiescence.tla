------------------------ MODULE FSessionQuiescence ------------------------
(***************************************************************************
Protocol-neutral fulfillment-daemon session cleanup.

A scheduler-session replacement may advertise capacity only after every
compiler from the prior session is physically quiescent. Reaping an unrelated
state-writer child, observing ECHILD for the direct compiler while descendants
survive, resetting unexplained scalar residue, using one timeout per cleanup
stage, or advertising early are all distinct failure modes.

The model uses one compiler process group and one unrelated writer child. The
writer is intentionally outside compiler occupancy, reproducing the legal
waitpid(-1) W-first trace without requiring an unbounded process table.
***************************************************************************)
EXTENDS Naturals, TLC

CONSTANTS MaxDeadline,
          MutantWaitAny,
          MutantECHILDQuiesces,
          MutantPerChildDeadline,
          MutantResetResidue,
          MutantAdvertiseEarly

ASSUME /\ MaxDeadline \in Nat \ {0}
       /\ MutantWaitAny \in BOOLEAN
       /\ MutantECHILDQuiesces \in BOOLEAN
       /\ MutantPerChildDeadline \in BOOLEAN
       /\ MutantResetResidue \in BOOLEAN
       /\ MutantAdvertiseEarly \in BOOLEAN

Phases ==
    {"Connected", "Lost", "Cleaning", "Quiesced",
     "Advertised", "FailedClosed"}
ChildStates == {"Alive", "Waitable", "Reaped"}

VARIABLES phase,
          compilerDirect,
          writerDirect,
          compilerGroupAlive,
          scalarKids,
          totalTicks,
          localTicks,
          deadlineReset,
          reapedWrongChild,
          residueReset,
          advertised

vars ==
    <<phase, compilerDirect, writerDirect, compilerGroupAlive,
      scalarKids, totalTicks, localTicks, deadlineReset,
      reapedWrongChild, residueReset, advertised>>

Init ==
    /\ phase = "Connected"
    /\ compilerDirect = "Alive"
    /\ writerDirect = "Alive"
    /\ compilerGroupAlive = TRUE
    /\ scalarKids = 1
    /\ totalTicks = 0
    /\ localTicks = 0
    /\ deadlineReset = FALSE
    /\ reapedWrongChild = FALSE
    /\ residueReset = FALSE
    /\ advertised = FALSE

MakeWriterWaitable ==
    /\ writerDirect = "Alive"
    /\ writerDirect' = "Waitable"
    /\ UNCHANGED <<phase, compilerDirect, compilerGroupAlive,
                    scalarKids, totalTicks, localTicks, deadlineReset,
                    reapedWrongChild, residueReset, advertised>>

LoseSchedulerSession ==
    /\ phase = "Connected"
    /\ phase' = "Lost"
    /\ UNCHANGED <<compilerDirect, writerDirect, compilerGroupAlive,
                    scalarKids, totalTicks, localTicks, deadlineReset,
                    reapedWrongChild, residueReset, advertised>>

StartCleanup ==
    /\ phase = "Lost"
    /\ phase' = "Cleaning"
    /\ compilerDirect' = "Waitable"
    /\ UNCHANGED <<writerDirect, compilerGroupAlive, scalarKids,
                    totalTicks, localTicks, deadlineReset,
                    reapedWrongChild, residueReset, advertised>>

ReapExactCompiler ==
    /\ phase = "Cleaning"
    /\ compilerDirect = "Waitable"
    /\ compilerDirect' = "Reaped"
    /\ scalarKids' = 0
    /\ UNCHANGED <<phase, writerDirect, compilerGroupAlive,
                    totalTicks, localTicks, deadlineReset,
                    reapedWrongChild, residueReset, advertised>>

ReapWrongWriter ==
    /\ phase = "Cleaning"
    /\ MutantWaitAny
    /\ writerDirect = "Waitable"
    /\ scalarKids = 1
    /\ writerDirect' = "Reaped"
    /\ scalarKids' = 0
    /\ reapedWrongChild' = TRUE
    /\ UNCHANGED <<phase, compilerDirect, compilerGroupAlive,
                    totalTicks, localTicks, deadlineReset,
                    residueReset, advertised>>

ScalarSaysQuiesced ==
    /\ phase = "Cleaning"
    /\ MutantWaitAny
    /\ scalarKids = 0
    /\ phase' = "Quiesced"
    /\ UNCHANGED <<compilerDirect, writerDirect, compilerGroupAlive,
                    scalarKids, totalTicks, localTicks, deadlineReset,
                    reapedWrongChild, residueReset, advertised>>

ObserveECHILDAsQuiesced ==
    /\ phase = "Cleaning"
    /\ MutantECHILDQuiesces
    /\ compilerDirect = "Reaped"
    /\ compilerGroupAlive
    /\ phase' = "Quiesced"
    /\ UNCHANGED <<compilerDirect, writerDirect, compilerGroupAlive,
                    scalarKids, totalTicks, localTicks, deadlineReset,
                    reapedWrongChild, residueReset, advertised>>

CompilerProcessGroupExits ==
    /\ phase = "Cleaning"
    /\ compilerDirect = "Reaped"
    /\ compilerGroupAlive
    /\ compilerGroupAlive' = FALSE
    /\ UNCHANGED <<phase, compilerDirect, writerDirect, scalarKids,
                    totalTicks, localTicks, deadlineReset,
                    reapedWrongChild, residueReset, advertised>>

MarkExactlyQuiesced ==
    /\ phase = "Cleaning"
    /\ compilerDirect = "Reaped"
    /\ ~compilerGroupAlive
    /\ phase' = "Quiesced"
    /\ UNCHANGED <<compilerDirect, writerDirect, compilerGroupAlive,
                    scalarKids, totalTicks, localTicks, deadlineReset,
                    reapedWrongChild, residueReset, advertised>>

ResetDeadlineForGroupCheck ==
    /\ phase = "Cleaning"
    /\ MutantPerChildDeadline
    /\ compilerDirect = "Reaped"
    /\ compilerGroupAlive
    /\ localTicks > 0
    /\ ~deadlineReset
    /\ localTicks' = 0
    /\ deadlineReset' = TRUE
    /\ UNCHANGED <<phase, compilerDirect, writerDirect,
                    compilerGroupAlive, scalarKids, totalTicks,
                    reapedWrongChild, residueReset, advertised>>

TickCleanupDeadline ==
    /\ phase = "Cleaning"
    /\ localTicks < MaxDeadline
    /\ totalTicks < (2 * MaxDeadline + 1)
    /\ LET nextLocal == localTicks + 1
           nextTotal == totalTicks + 1
       IN /\ localTicks' = nextLocal
          /\ totalTicks' = nextTotal
          /\ phase' =
                IF nextLocal = MaxDeadline
                   /\ (~MutantPerChildDeadline \/ deadlineReset)
                THEN "FailedClosed"
                ELSE "Cleaning"
    /\ UNCHANGED <<compilerDirect, writerDirect, compilerGroupAlive,
                    scalarKids, deadlineReset, reapedWrongChild,
                    residueReset, advertised>>

ResetUnexplainedResidue ==
    /\ phase = "Cleaning"
    /\ MutantResetResidue
    /\ (compilerDirect # "Reaped" \/ compilerGroupAlive)
    /\ scalarKids' = 0
    /\ residueReset' = TRUE
    /\ phase' = "Quiesced"
    /\ UNCHANGED <<compilerDirect, writerDirect, compilerGroupAlive,
                    totalTicks, localTicks, deadlineReset,
                    reapedWrongChild, advertised>>

AdvertiseCapacity ==
    /\ phase = "Quiesced"
    /\ phase' = "Advertised"
    /\ advertised' = TRUE
    /\ UNCHANGED <<compilerDirect, writerDirect, compilerGroupAlive,
                    scalarKids, totalTicks, localTicks, deadlineReset,
                    reapedWrongChild, residueReset>>

AdvertiseBeforeQuiescence ==
    /\ MutantAdvertiseEarly
    /\ phase \in {"Lost", "Cleaning"}
    /\ phase' = "Advertised"
    /\ advertised' = TRUE
    /\ UNCHANGED <<compilerDirect, writerDirect, compilerGroupAlive,
                    scalarKids, totalTicks, localTicks, deadlineReset,
                    reapedWrongChild, residueReset>>

Next ==
    \/ MakeWriterWaitable
    \/ LoseSchedulerSession
    \/ StartCleanup
    \/ ReapExactCompiler
    \/ ReapWrongWriter
    \/ ScalarSaysQuiesced
    \/ ObserveECHILDAsQuiesced
    \/ CompilerProcessGroupExits
    \/ MarkExactlyQuiesced
    \/ ResetDeadlineForGroupCheck
    \/ TickCleanupDeadline
    \/ ResetUnexplainedResidue
    \/ AdvertiseCapacity
    \/ AdvertiseBeforeQuiescence

Spec == Init /\ [][Next]_vars

CleanupProgress ==
    StartCleanup \/ ReapExactCompiler
    \/ CompilerProcessGroupExits \/ MarkExactlyQuiesced
    \/ TickCleanupDeadline

FairSpec ==
    /\ Spec
    /\ WF_vars(CleanupProgress)

TypeOK ==
    /\ phase \in Phases
    /\ compilerDirect \in ChildStates
    /\ writerDirect \in ChildStates
    /\ compilerGroupAlive \in BOOLEAN
    /\ scalarKids \in 0..1
    /\ totalTicks \in 0..(2 * MaxDeadline + 1)
    /\ localTicks \in 0..MaxDeadline
    /\ deadlineReset \in BOOLEAN
    /\ reapedWrongChild \in BOOLEAN
    /\ residueReset \in BOOLEAN
    /\ advertised \in BOOLEAN

ExactCompilerReapedBeforeQuiesce ==
    phase \in {"Quiesced", "Advertised"} => compilerDirect = "Reaped"

CompilerGroupAbsentBeforeQuiesce ==
    phase \in {"Quiesced", "Advertised"} => ~compilerGroupAlive

WholeSessionDeadlineBound ==
    totalTicks <= MaxDeadline
    \/ phase \in {"Quiesced", "Advertised", "FailedClosed"}

NoResidueReset == ~residueReset

PriorSessionQuiescedBeforeAdvertise ==
    advertised => compilerDirect = "Reaped" /\ ~compilerGroupAlive

SafetyInvariant ==
    /\ TypeOK
    /\ ExactCompilerReapedBeforeQuiesce
    /\ CompilerGroupAbsentBeforeQuiesce
    /\ WholeSessionDeadlineBound
    /\ NoResidueReset
    /\ PriorSessionQuiescedBeforeAdvertise

CleanupEventuallySettles ==
    [](phase \in {"Lost", "Cleaning"}
       => <> (phase \in {"Quiesced", "FailedClosed", "Advertised"}))

=============================================================================
