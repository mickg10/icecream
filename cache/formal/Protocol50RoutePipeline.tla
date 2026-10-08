------------------------------ MODULE Protocol50RoutePipeline ------------------------------
(***************************************************************************
 Focused generalization of the Protocol-50 route cache/F-input hand-off to
 cover C-side route pipelining (PipelineDepth = K successors in flight).

 Scope and relationship to Protocol50.tla / Protocol50MultiRoute.tla:
   - This is a deliberately small, bounded "representative episode" model:
     rel_seq ranges over 0..MaxRelSeq and the run terminates once every
     rel_seq in that range has been begun, applied, and retired.  It is not
     a steady-state model and does not replace the existing route/commit
     modules; it isolates exactly the new pipelining behavior described in
     research/designs/P50_ROUTE_PIPELINING.md section 1 (C side) and
     section 2 (F side revised parking design).
   - Digest values themselves (TRANSACTION_DIGEST / STATE_DIGEST) are a
     C++-level concern (see cache/formal/check_trace.py, which is the
     C++ implementer's step, not this one).  Here "the C digest chain
     stays consistent" is restated as a rel_seq contiguity/anchoring
     property (DigestChainConsistency below): in this abstraction rel_seq
     fully determines chain membership, so contiguity is the model-level
     stand-in for "the digest chain has no gap or duplicate."
   - fActive is intentionally a single scalar slot (NoTx or one record),
     matching the real F, which never holds more than one ACTIVE_TX.
     fParked is the new K-1-capacity holding area for successors whose
     BEGIN arrived ahead of the route cursor.

 Fault semantics (abort / disconnect / route terminal / HISTORY_RESET /
 cold store, folded into one generic Fault action per the design's
 invariant 6, "all outstanding FIFOs are cleared in entirety" and the
 review note that faults never do a partial rollback): a Fault discards
 every outstanding/speculative/parked successor and bumps a shared epoch
 counter so that no stale prediction can ever be mistaken for a live one.
 It does NOT erase already-applied ground truth (appliedLog, fRouteCursor,
 cConfirmedCursor, fStateSent) -- that history is durable and is exactly
 what C resynchronizes from afterward.  A true HISTORY_RESET (restarting
 rel_seq at 0 under a new history nonce) is out of scope for this focused
 model; the existing Protocol50Reconnect.tla / Protocol50.tla already
 cover that boundary and are unaffected by pipelining.

 K=1 collapse (central backward-compatibility argument, design doc
 invariants 2 and 12): when PipelineDepth = 1, FArrivePark's guard
 (relSeq > fRouteCursor /\ relSeq <= fRouteCursor + PipelineDepth - 1)
 becomes relSeq > fRouteCursor /\ relSeq <= fRouteCursor, which is never
 satisfiable.  Parking therefore never fires, fParked stays empty for the
 whole run, and the model structurally collapses onto today's single-slot
 behavior.  K1Equivalence below checks exactly that collapse; it stands in
 for a literal refinement proof against Protocol50.tla, per the explicit
 license to do so in the implementation brief.
***************************************************************************)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS PipelineDepth, MaxRelSeq

ASSUME PipelineDepth \in Nat /\ PipelineDepth >= 1
ASSUME MaxRelSeq \in Nat /\ MaxRelSeq >= PipelineDepth

NoTx == -1
RelSeqs == 0..MaxRelSeq

VARIABLES
    cQueue,            \* C's FIFO of uncommitted successors, oldest-first
    cConfirmedCursor,  \* next rel_seq C expects to retire (true-confirmed)
    cPredictedCursor,  \* next rel_seq C will locally begin (speculative)
    fActive,           \* F's single ACTIVE_TX slot: NoTx or [relSeq,epoch]
    fParked,           \* F's parked-successor set (size <= K-1)
    fRouteCursor,       \* F's next-expected rel_seq ("next_rel")
    fStateSent,        \* relSeqs for which F has ever issued STATE/BEGIN
    epoch,             \* shared fault-epoch counter
    appliedLog         \* ground truth: relSeqs F has actually applied, in order

vars == <<cQueue, cConfirmedCursor, cPredictedCursor, fActive, fParked,
          fRouteCursor, fStateSent, epoch, appliedLog>>

Init ==
    /\ cQueue = <<>>
    /\ cConfirmedCursor = 0
    /\ cPredictedCursor = 0
    /\ fActive = NoTx
    /\ fParked = {}
    /\ fRouteCursor = 0
    /\ fStateSent = {}
    /\ epoch = 0
    /\ appliedLog = <<>>

InCQueue(relSeq) ==
    \E k \in 1..Len(cQueue) : cQueue[k].relSeq = relSeq /\ cQueue[k].epoch = epoch

(***************************************************************************
 CBegin: C speculatively begins the next successor, as long as its
 uncommitted-successor FIFO has room (at most K outstanding).
***************************************************************************)
CBegin ==
    /\ Len(cQueue) < PipelineDepth
    /\ cPredictedCursor <= MaxRelSeq
    /\ cQueue' = Append(cQueue, [relSeq |-> cPredictedCursor, epoch |-> epoch])
    /\ cPredictedCursor' = cPredictedCursor + 1
    /\ UNCHANGED <<cConfirmedCursor, fActive, fParked, fRouteCursor,
                   fStateSent, epoch, appliedLog>>

(***************************************************************************
 FArriveProceed: the connection carrying relSeq arrives exactly at F's
 route cursor -- today's unchanged path.  No parking, STATE/BEGIN issued
 immediately.
***************************************************************************)
FArriveProceed(relSeq) ==
    /\ InCQueue(relSeq)
    /\ fActive = NoTx
    /\ relSeq = fRouteCursor
    /\ fActive' = [relSeq |-> relSeq, epoch |-> epoch]
    /\ fStateSent' = fStateSent \cup {relSeq}
    /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, fParked,
                   fRouteCursor, epoch, appliedLog>>

(***************************************************************************
 FArrivePark: the connection's BEGIN rel_seq is ahead of F's route cursor
 (its predecessor has not committed yet).  F parks it -- no STATE/BEGIN
 processing happens until the predecessor commits.
***************************************************************************)
FArrivePark(relSeq) ==
    /\ InCQueue(relSeq)
    /\ relSeq > fRouteCursor
    /\ relSeq <= fRouteCursor + PipelineDepth - 1
    /\ ~(\E p \in fParked : p.relSeq = relSeq)
    /\ fParked' = fParked \cup {[relSeq |-> relSeq, epoch |-> epoch]}
    /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, fActive,
                   fRouteCursor, fStateSent, epoch, appliedLog>>

(***************************************************************************
 FCommit: F commits its current ACTIVE_TX.  Release trigger: if a parked
 successor is waiting at exactly committed+1 (the correct, and only,
 release point), it is released into fActive and its STATE/BEGIN is
 issued now, for the first time.
***************************************************************************)
FCommit ==
    /\ fActive # NoTx
    /\ LET committed == fActive.relSeq
           releaseCandidate == {p \in fParked : p.relSeq = committed + 1}
           victim == IF releaseCandidate # {}
                     THEN CHOOSE p \in releaseCandidate : TRUE
                     ELSE NoTx
       IN
           /\ appliedLog' = Append(appliedLog, committed)
           /\ fRouteCursor' = committed + 1
           /\ fActive' = victim
           /\ fParked' = IF victim = NoTx THEN fParked ELSE fParked \ {victim}
           /\ fStateSent' = IF victim = NoTx
                             THEN fStateSent
                             ELSE fStateSent \cup {victim.relSeq}
    /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, epoch>>

(***************************************************************************
 CRetireCommit: C retires commits strictly in rel_seq order -- only the
 FIFO head may be retired, and only once it is both in the current epoch
 and backed by true ground-truth confirmation (appliedLog membership).
***************************************************************************)
CRetireCommit ==
    /\ Len(cQueue) > 0
    /\ LET head == Head(cQueue)
       IN
           /\ head.epoch = epoch
           /\ \E k \in 1..Len(appliedLog) : appliedLog[k] = head.relSeq
           /\ cQueue' = Tail(cQueue)
           /\ cConfirmedCursor' = head.relSeq + 1
    /\ UNCHANGED <<cPredictedCursor, fActive, fParked, fRouteCursor,
                   fStateSent, epoch, appliedLog>>

(***************************************************************************
 Fault: generic stand-in for abort / disconnect / route terminal /
 HISTORY_RESET / cold store.  Invalidates every outstanding C successor
 and every F-parked successor in one step by bumping epoch; ground truth
 already applied is left untouched.
***************************************************************************)
Fault ==
    /\ \/ fActive # NoTx
       \/ fParked # {}
       \/ cQueue # <<>>
       \/ cPredictedCursor # fRouteCursor
    /\ epoch' = epoch + 1
    /\ cQueue' = <<>>
    /\ cPredictedCursor' = fRouteCursor
    /\ fParked' = {}
    /\ fActive' = NoTx
    /\ UNCHANGED <<cConfirmedCursor, fRouteCursor, fStateSent, appliedLog>>

Next ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArrivePark(relSeq))
    \/ FCommit
    \/ CRetireCommit
    \/ Fault

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

-----------------------------------------------------------------------------
(* Invariants *)

TypeOK ==
    /\ cQueue \in Seq([relSeq: RelSeqs, epoch: Nat])
    /\ Len(cQueue) <= PipelineDepth
    /\ cConfirmedCursor \in Nat
    /\ cPredictedCursor \in Nat
    /\ fActive = NoTx \/ fActive \in [relSeq: RelSeqs, epoch: Nat]
    /\ fParked \subseteq [relSeq: RelSeqs, epoch: Nat]
    /\ Cardinality(fParked) <= PipelineDepth - 1
    /\ fRouteCursor \in Nat
    /\ fStateSent \subseteq RelSeqs
    /\ epoch \in Nat
    /\ appliedLog \in Seq(RelSeqs)

\* relSeqs whose STATE/BEGIN has been sent but not yet applied -- F's
\* notion of "currently open."
OpenOnF == {r \in RelSeqs : r \in fStateSent /\
                              ~(\E k \in 1..Len(appliedLog) : appliedLog[k] = r)}

SingleActiveTxOnF ==
    /\ Cardinality(OpenOnF) <= 1
    /\ (fActive # NoTx => OpenOnF = {fActive.relSeq})
    /\ (fActive = NoTx => OpenOnF = {})

RouteCursorTracksApplied == fRouteCursor = Len(appliedLog)

ActiveIsNextExpected == fActive # NoTx => fActive.relSeq = fRouteCursor

InOrderApply == \A k \in 1..Len(appliedLog) : appliedLog[k] = k - 1

QueueChainConsistent ==
    \A k \in 1..(Len(cQueue) - 1) : cQueue[k + 1].relSeq = cQueue[k].relSeq + 1

QueueAnchoredAtConfirmed ==
    Len(cQueue) > 0 => Head(cQueue).relSeq = cConfirmedCursor

\* Model-level stand-in for "the C digest chain stays consistent": rel_seq
\* contiguity/anchoring, since rel_seq determines chain membership here.
DigestChainConsistency == QueueChainConsistent /\ QueueAnchoredAtConfirmed

\* No stale C-side successor survives a fault.
QueueCurrentEpochOnly == \A k \in 1..Len(cQueue) : cQueue[k].epoch = epoch

\* No STATE/BEGIN is ever issued for a relSeq ahead of what F currently
\* expects (catches "STATE sent at park time").
StateSentNeverAheadOfCursor == \A r \in fStateSent : r <= fRouteCursor

\* Symmetric to QueueCurrentEpochOnly, for F's parking map (catches a
\* missing bulk release on fault).
ParkedCurrentEpochOnly == \A p \in fParked : p.epoch = epoch

\* The K=1 collapse argument: at PipelineDepth = 1, parking can never
\* fire, so fParked stays empty for the whole run.
K1Equivalence == PipelineDepth = 1 => fParked = {}

-----------------------------------------------------------------------------
(* Liveness *)

LeadsTo(P, Q) == [](P => <>Q)

ParkedRelSeqs == {p.relSeq : p \in fParked}

\* A parked transaction is eventually released (or discarded by a fault),
\* under the WF_vars(Next) fairness already built into Spec.
ParkedEventuallyResolved ==
    \A r \in RelSeqs : LeadsTo(r \in ParkedRelSeqs, r \notin ParkedRelSeqs)

-----------------------------------------------------------------------------
(* Mutants -- each must FAIL with the named invariant.  Each mutant swaps
   exactly one action for a buggy variant and re-wires Next/Spec around it,
   following the existing suite's "one shared module, differently-named
   Spec per mutant .cfg" convention (see e.g. Protocol50.tla's
   AbortMutant/RelSeqMutant/OperationDigestMutant). *)

\* M1: release before predecessor commits -- a parked successor is
\* activated immediately on arrival instead of waiting for its
\* predecessor's commit.  Expected violation: ActiveIsNextExpected.
FArriveParkMutantEarlyActivate(relSeq) ==
    /\ InCQueue(relSeq)
    /\ relSeq > fRouteCursor
    /\ relSeq <= fRouteCursor + PipelineDepth - 1
    /\ fActive' = [relSeq |-> relSeq, epoch |-> epoch]
    /\ fStateSent' = fStateSent \cup {relSeq}
    /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, fParked,
                   fRouteCursor, epoch, appliedLog>>

NextM1 ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArriveParkMutantEarlyActivate(relSeq))
    \/ FCommit
    \/ CRetireCommit
    \/ Fault
SpecM1 == Init /\ [][NextM1]_vars /\ WF_vars(NextM1)

\* M2: release of n+2 on n's commit -- on commit, release an arbitrary
\* parked successor instead of specifically committed+1.  Needs
\* PipelineDepth = 3 to have two parked entries to pick the wrong one from.
\* Expected violation: ActiveIsNextExpected.
FCommitMutantEarlyRelease ==
    /\ fActive # NoTx
    /\ LET committed == fActive.relSeq
           victim == IF fParked # {} THEN CHOOSE p \in fParked : TRUE ELSE NoTx
       IN
           /\ appliedLog' = Append(appliedLog, committed)
           /\ fRouteCursor' = committed + 1
           /\ fActive' = victim
           /\ fParked' = IF victim = NoTx THEN fParked ELSE fParked \ {victim}
           /\ fStateSent' = IF victim = NoTx
                             THEN fStateSent
                             ELSE fStateSent \cup {victim.relSeq}
    /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, epoch>>

NextM2 ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArrivePark(relSeq))
    \/ FCommitMutantEarlyRelease
    \/ CRetireCommit
    \/ Fault
SpecM2 == Init /\ [][NextM2]_vars /\ WF_vars(NextM2)

\* M3: bulk release missing on fault -- parked entries survive a fault
\* instead of being cleared.  Expected violation: ParkedCurrentEpochOnly.
FaultMutantNoBulkRelease ==
    /\ \/ fActive # NoTx
       \/ fParked # {}
       \/ cQueue # <<>>
       \/ cPredictedCursor # fRouteCursor
    /\ epoch' = epoch + 1
    /\ cQueue' = <<>>
    /\ cPredictedCursor' = fRouteCursor
    /\ fParked' = fParked
    /\ fActive' = NoTx
    /\ UNCHANGED <<cConfirmedCursor, fRouteCursor, fStateSent, appliedLog>>

NextM3 ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArrivePark(relSeq))
    \/ FCommit
    \/ CRetireCommit
    \/ FaultMutantNoBulkRelease
SpecM3 == Init /\ [][NextM3]_vars /\ WF_vars(NextM3)

\* M4: STATE sent at park time -- fStateSent is updated when a successor
\* is parked, instead of waiting for release.  Expected violation:
\* StateSentNeverAheadOfCursor.
FArriveParkMutantStateAtParkTime(relSeq) ==
    /\ InCQueue(relSeq)
    /\ relSeq > fRouteCursor
    /\ relSeq <= fRouteCursor + PipelineDepth - 1
    /\ ~(\E p \in fParked : p.relSeq = relSeq)
    /\ fParked' = fParked \cup {[relSeq |-> relSeq, epoch |-> epoch]}
    /\ fStateSent' = fStateSent \cup {relSeq}
    /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, fActive,
                   fRouteCursor, epoch, appliedLog>>

NextM4 ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArriveParkMutantStateAtParkTime(relSeq))
    \/ FCommit
    \/ CRetireCommit
    \/ Fault
SpecM4 == Init /\ [][NextM4]_vars /\ WF_vars(NextM4)

\* M5: C retiring out of order -- retire an arbitrary confirmed queue
\* entry instead of only ever the FIFO head.  Expected violation:
\* DigestChainConsistency (specifically QueueAnchoredAtConfirmed).
CRetireCommitMutantAnyOrder ==
    /\ Len(cQueue) > 0
    /\ \E k \in 1..Len(cQueue) :
        /\ cQueue[k].epoch = epoch
        /\ \E j \in 1..Len(appliedLog) : appliedLog[j] = cQueue[k].relSeq
        /\ cQueue' = [m \in 1..(Len(cQueue) - 1) |->
                        IF m < k THEN cQueue[m] ELSE cQueue[m + 1]]
        /\ cConfirmedCursor' = cQueue[k].relSeq + 1
    /\ UNCHANGED <<cPredictedCursor, fActive, fParked, fRouteCursor,
                   fStateSent, epoch, appliedLog>>

NextM5 ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArrivePark(relSeq))
    \/ FCommit
    \/ CRetireCommitMutantAnyOrder
    \/ Fault
SpecM5 == Init /\ [][NextM5]_vars /\ WF_vars(NextM5)

\* M6: a successor surviving a fault -- cQueue is left untouched by Fault
\* instead of being cleared.  Expected violation: QueueCurrentEpochOnly.
FaultMutantSurvivingSuccessor ==
    /\ \/ fActive # NoTx
       \/ fParked # {}
       \/ cQueue # <<>>
       \/ cPredictedCursor # fRouteCursor
    /\ epoch' = epoch + 1
    /\ cQueue' = cQueue
    /\ cPredictedCursor' = cPredictedCursor
    /\ fParked' = {}
    /\ fActive' = NoTx
    /\ UNCHANGED <<cConfirmedCursor, fRouteCursor, fStateSent, appliedLog>>

NextM6 ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArrivePark(relSeq))
    \/ FCommit
    \/ CRetireCommit
    \/ FaultMutantSurvivingSuccessor
SpecM6 == Init /\ [][NextM6]_vars /\ WF_vars(NextM6)

=============================================================================
