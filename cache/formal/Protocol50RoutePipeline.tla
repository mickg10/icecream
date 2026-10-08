------------------------------ MODULE Protocol50RoutePipeline ------------------------------
(***************************************************************************
 Focused generalization of the Protocol-50 route cache/F-input hand-off to
 cover C-side route pipelining (PipelineDepth = K successors in flight).

 Scope and relationship to Protocol50.tla / Protocol50MultiRoute.tla:
   - This is a deliberately small, bounded "representative episode" model:
     rel_seq ranges over 0..MaxRelSeq within each epoch, and epoch ranges
     over 0..MaxEpoch so the state space stays finite even though a Fault
     can restart the rel_seq range from 0 under a fresh epoch.  It is not
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
 cold store, folded into one generic Fault action per design doc section
 1.1(a)): every fault is a FULL cold reset on BOTH sides, never a partial
 rollback or a resync.  On the F side, CRoute::reset_history
 (cache/p50_slice0.cpp) calls abandon_active() then p29v1_.reset() --
 the entire P29V1 F-side codec view is destroyed and rebuilt from
 scratch.  On the C/F-endpoint side, Impl::reset_history
 (cache/p50_endpoint.cpp) replaces the whole Route entry via
 space.routes.insert_or_assign(session.profile deref, Route{...}) with
 .dialogue = nullptr -- the shared dialogue pointer is dropped, not
 resynchronized in place.  C never adopts F's durable cursor; both sides
 simply restart at rel_seq 0 under a new epoch.  Concretely, Fault here:
   - bumps the shared epoch counter (bounded by MaxEpoch, for finiteness);
   - resets fRouteCursor, cConfirmedCursor and cPredictedCursor all to 0
     (NOT to each other's value -- there is no resync, only a cold reset);
   - clears cQueue, fParked and fActive;
   - drops every fStateSent entry tagged with the epoch that just ended
     (the F-side codec view those sends belonged to no longer exists).
 appliedLog is the one piece of state Fault does NOT touch: it is
 permanent cross-epoch ground truth (storage-layer committed history),
 tagged per entry with the epoch it was applied under, and keeps
 accumulating across every fault.  net (in-flight BEGIN/ACK wire
 messages, introduced below to model cross-epoch stragglers) is also
 left untouched by Fault -- a message sent before a fault can still be
 sitting on the wire afterward, and the whole point of modeling it is to
 check that both sides correctly reject/ignore it once it arrives late
 under the new epoch, rather than clearing it away for free.

 Cross-epoch stragglers (design doc section 1.1(a)/(b), the real hazard
 once K > 1): a BEGIN sent just before a fault, or a commit ack sent just
 before a fault, can still be in flight when the fault lands.  net models
 this explicitly as a small set of tagged in-flight messages that Fault
 does not clear.  F rejects (silently discards) any BEGIN whose epoch
 does not match its current epoch (history_nonce mismatch); C's retire
 path ignores any ack whose epoch does not match its current epoch.
 NoCrossEpochRetire checks exactly the C-side half of that: every retire
 is backed by ground truth recorded under the SAME epoch as the queue
 head being retired, never a stale ack left over from a prior epoch that
 happens to share the same (reset-to-0) rel_seq numbering.  net is kept
 small by construction: CBegin/FCommit each refuse to deposit a second
 message of their kind for the current epoch while one is already
 in flight, so at most one BEGIN and one ACK per epoch are ever
 outstanding at a time (older, stale-epoch messages may still coexist
 until explicitly rejected/dropped -- that coexistence is the mechanism
 under test).

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

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS PipelineDepth, MaxRelSeq, MaxEpoch

ASSUME PipelineDepth \in Nat /\ PipelineDepth >= 1
ASSUME MaxRelSeq \in Nat /\ MaxRelSeq >= PipelineDepth
ASSUME MaxEpoch \in Nat /\ MaxEpoch >= 1

\* NoTx is deliberately the SAME record shape as a real [relSeq, epoch]
\* transaction (not a bare scalar sentinel like -1).  TLC's record
\* equality implementation throws "Attempted to check equality of
\* record ... with non-record ..." whenever one side of = (or \in
\* against a record-set, or # once a record exists) is a genuine
\* record and the other is a plain integer -- it does not fall back to
\* "unequal because different shapes" the way untyped set theory would.
\* Every comparison in this spec is written generically as
\* fActive = NoTx / fActive # NoTx / victim = NoTx etc., so giving NoTx
\* the same field set makes all of them record-vs-record and sidesteps
\* the crash everywhere at once, including inside fParked \cup {NoTx}.
NoTx == [relSeq |-> -1, epoch |-> -1]
RelSeqs == 0..MaxRelSeq
Epochs == 0..MaxEpoch

VARIABLES
    cQueue,            \* C's FIFO of uncommitted successors, oldest-first
    cConfirmedCursor,  \* next rel_seq C expects to retire (true-confirmed)
    cPredictedCursor,  \* next rel_seq C will locally begin (speculative)
    fActive,           \* F's single ACTIVE_TX slot: NoTx or [relSeq,epoch]
    fParked,           \* F's parked-successor set (size <= K-1)
    fRouteCursor,       \* F's next-expected rel_seq ("next_rel")
    fStateSent,        \* {relSeq,epoch} pairs for which F has issued STATE/BEGIN
                       \* (epoch-tagged so a stale pre-fault send of a
                       \* relSeq that gets legitimately reused after the
                       \* fault isn't mistaken for still being open)
    epoch,             \* shared fault-epoch counter
    appliedLog,        \* ground truth: <<epoch,relSeq>> F has actually
                       \* applied, in order, across every epoch; never
                       \* reset by Fault
    net,               \* in-flight wire messages: {[kind, relSeq, epoch]},
                       \* kind \in {"BEGIN","ACK"}; never reset by Fault
    cRetiredLog        \* <<epoch,relSeq>> pairs C has retired, in order;
                       \* used only to state NoCrossEpochRetire

vars == <<cQueue, cConfirmedCursor, cPredictedCursor, fActive, fParked,
          fRouteCursor, fStateSent, epoch, appliedLog, net, cRetiredLog>>

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
    /\ net = {}
    /\ cRetiredLog = <<>>

\* True while a BEGIN/ACK for the current epoch is already on the wire --
\* used to bound net to at most one in-flight message of each kind per
\* epoch, so the model stays finite without ruling out the straggler
\* scenario (an older-epoch message can still coexist unconsumed).
NetHasCurrent(kind) == \E m \in net : m.kind = kind /\ m.epoch = epoch

(***************************************************************************
 CBegin: C speculatively begins the next successor, as long as its
 uncommitted-successor FIFO has room (at most K outstanding), and sends
 its BEGIN onto the wire.
***************************************************************************)
CBegin ==
    /\ Len(cQueue) < PipelineDepth
    /\ cPredictedCursor <= MaxRelSeq
    /\ ~NetHasCurrent("BEGIN")
    /\ cQueue' = Append(cQueue, [relSeq |-> cPredictedCursor, epoch |-> epoch])
    /\ net' = net \cup {[kind |-> "BEGIN", relSeq |-> cPredictedCursor, epoch |-> epoch]}
    /\ cPredictedCursor' = cPredictedCursor + 1
    /\ UNCHANGED <<cConfirmedCursor, fActive, fParked, fRouteCursor,
                   fStateSent, epoch, appliedLog, cRetiredLog>>

(***************************************************************************
 FArriveProceed: a same-epoch BEGIN arrives exactly at F's route cursor --
 today's unchanged path.  No parking, STATE/BEGIN issued immediately.
***************************************************************************)
FArriveProceed(relSeq) ==
    /\ [kind |-> "BEGIN", relSeq |-> relSeq, epoch |-> epoch] \in net
    /\ fActive = NoTx
    /\ relSeq = fRouteCursor
    /\ fActive' = [relSeq |-> relSeq, epoch |-> epoch]
    /\ fStateSent' = fStateSent \cup {[relSeq |-> relSeq, epoch |-> epoch]}
    /\ net' = net \ {[kind |-> "BEGIN", relSeq |-> relSeq, epoch |-> epoch]}
    /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, fParked,
                   fRouteCursor, epoch, appliedLog, cRetiredLog>>

(***************************************************************************
 FArrivePark: a same-epoch BEGIN's rel_seq is ahead of F's route cursor
 (its predecessor has not committed yet).  F parks it -- no STATE/BEGIN
 processing happens until the predecessor commits.
***************************************************************************)
FArrivePark(relSeq) ==
    /\ [kind |-> "BEGIN", relSeq |-> relSeq, epoch |-> epoch] \in net
    /\ relSeq > fRouteCursor
    /\ relSeq <= fRouteCursor + PipelineDepth - 1
    /\ ~(\E p \in fParked : p.relSeq = relSeq)
    /\ fParked' = fParked \cup {[relSeq |-> relSeq, epoch |-> epoch]}
    /\ net' = net \ {[kind |-> "BEGIN", relSeq |-> relSeq, epoch |-> epoch]}
    /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, fActive,
                   fRouteCursor, fStateSent, epoch, appliedLog, cRetiredLog>>

(***************************************************************************
 FRejectStaleBegin: F silently discards a BEGIN whose epoch does not match
 its current epoch (history_nonce mismatch) -- the clean-model counterpart
 of a real reset_history call having already rebuilt the codec view out
 from under this message.  Keeps net from accumulating undeliverable
 stale BEGINs forever.
***************************************************************************)
FRejectStaleBegin ==
    \E m \in net :
        /\ m.kind = "BEGIN"
        /\ m.epoch # epoch
        /\ net' = net \ {m}
        /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, fActive,
                       fParked, fRouteCursor, fStateSent, epoch, appliedLog,
                       cRetiredLog>>

(***************************************************************************
 FCommit: F commits its current ACTIVE_TX, appends to the durable
 cross-epoch applied log, and sends an ACK onto the wire.  Release
 trigger: if a parked successor is waiting at exactly committed+1 (the
 correct, and only, release point), it is released into fActive and its
 STATE/BEGIN is issued now, for the first time.
***************************************************************************)
FCommit ==
    /\ fActive # NoTx
    /\ ~NetHasCurrent("ACK")
    /\ LET committed == fActive.relSeq
           releaseCandidate == {p \in fParked : p.relSeq = committed + 1}
           victim == IF releaseCandidate # {}
                     THEN CHOOSE p \in releaseCandidate : TRUE
                     ELSE NoTx
       IN
           /\ appliedLog' = Append(appliedLog, <<epoch, committed>>)
           /\ fRouteCursor' = committed + 1
           /\ fActive' = victim
           /\ fParked' = IF victim = NoTx THEN fParked ELSE fParked \ {victim}
           /\ fStateSent' = IF victim = NoTx
                             THEN fStateSent
                             ELSE fStateSent \cup {[relSeq |-> victim.relSeq, epoch |-> epoch]}
           /\ net' = net \cup {[kind |-> "ACK", relSeq |-> committed, epoch |-> epoch]}
    /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, epoch, cRetiredLog>>

(***************************************************************************
 CRetireCommit: C retires commits strictly in rel_seq order -- only the
 FIFO head may be retired, and only once it is both in the current epoch
 and backed by a same-epoch ACK actually on the wire.  Consuming the ACK
 (removing it from net) is what makes NoCrossEpochRetire meaningful: the
 epoch check here is the one M7 deliberately removes.
***************************************************************************)
CRetireCommit ==
    /\ Len(cQueue) > 0
    /\ LET head == Head(cQueue)
       IN
           /\ head.epoch = epoch
           /\ \E m \in net : m.kind = "ACK" /\ m.relSeq = head.relSeq /\ m.epoch = epoch
           /\ cQueue' = Tail(cQueue)
           /\ cConfirmedCursor' = head.relSeq + 1
           /\ net' = net \ {[kind |-> "ACK", relSeq |-> head.relSeq, epoch |-> epoch]}
           /\ cRetiredLog' = Append(cRetiredLog, <<epoch, head.relSeq>>)
    /\ UNCHANGED <<cPredictedCursor, fActive, fParked, fRouteCursor,
                   fStateSent, epoch, appliedLog>>

(***************************************************************************
 CDropStaleAck: C ignores (discards) any ack whose epoch does not match
 its current epoch.  Keeps net from accumulating undeliverable stale ACKs
 forever; the clean-model counterpart of the C/F-endpoint's dialogue
 pointer having already been replaced out from under this message.
***************************************************************************)
CDropStaleAck ==
    \E m \in net :
        /\ m.kind = "ACK"
        /\ m.epoch # epoch
        /\ net' = net \ {m}
        /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, fActive,
                       fParked, fRouteCursor, fStateSent, epoch, appliedLog,
                       cRetiredLog>>

(***************************************************************************
 Fault: generic stand-in for abort / disconnect / route terminal /
 HISTORY_RESET / cold store, per design doc section 1.1(a).  A full cold
 reset on both sides, never a partial rollback and never a resync: every
 per-epoch cursor goes back to 0, the parked/queue/active state is wiped,
 and the current epoch's fStateSent entries are dropped along with it
 (the codec view those sends belonged to no longer exists).  appliedLog
 (durable ground truth) and net (in-flight wire messages, oblivious to
 either side's local reset) are the two pieces of state this
 deliberately leaves untouched -- that is exactly what lets a message
 sent just before this Fault still be sitting in net afterward, tagged
 with the now-superseded epoch, to exercise FRejectStaleBegin /
 CDropStaleAck / NoCrossEpochRetire.  Bounded by MaxEpoch so the state
 space stays finite.
***************************************************************************)
Fault ==
    /\ epoch < MaxEpoch
    /\ epoch' = epoch + 1
    /\ cQueue' = <<>>
    /\ cPredictedCursor' = 0
    /\ cConfirmedCursor' = 0
    /\ fRouteCursor' = 0
    /\ fParked' = {}
    /\ fActive' = NoTx
    /\ fStateSent' = {s \in fStateSent : s.epoch # epoch}
    /\ UNCHANGED <<appliedLog, net, cRetiredLog>>

Next ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArrivePark(relSeq))
    \/ FRejectStaleBegin
    \/ FCommit
    \/ CRetireCommit
    \/ CDropStaleAck
    \/ Fault

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

-----------------------------------------------------------------------------
(* Invariants *)

TypeOK ==
    /\ Len(cQueue) <= PipelineDepth
    /\ \A i \in 1..Len(cQueue) : cQueue[i].relSeq \in RelSeqs /\ cQueue[i].epoch \in Epochs
    /\ cConfirmedCursor \in Nat
    /\ cPredictedCursor \in Nat
    /\ fActive = NoTx \/ (fActive.relSeq \in RelSeqs /\ fActive.epoch \in Epochs)
    /\ \A p \in fParked : p.relSeq \in RelSeqs /\ p.epoch \in Epochs
    /\ Cardinality(fParked) <= PipelineDepth - 1
    /\ fRouteCursor \in Nat
    /\ \A s \in fStateSent : s.relSeq \in RelSeqs /\ s.epoch \in Epochs
    /\ epoch \in Epochs
    /\ \A k \in 1..Len(appliedLog) : appliedLog[k][1] \in Epochs /\ appliedLog[k][2] \in RelSeqs
    /\ \A m \in net : m.kind \in {"BEGIN", "ACK"} /\ m.relSeq \in RelSeqs /\ m.epoch \in Epochs
    /\ \A k \in 1..Len(cRetiredLog) : cRetiredLog[k][1] \in Epochs /\ cRetiredLog[k][2] \in RelSeqs

\* The applied relSeqs tagged with a given epoch, as a plain set (order is
\* not needed here: within one epoch appliedLog only ever gains entries
\* via FCommit appending the current fRouteCursor value before advancing
\* it by exactly 1, and epoch itself only ever increases, so entries for
\* a past epoch value form one contiguous run with no interleaving from
\* any other epoch).
AppliedRelSeqsInEpoch(e) ==
    {appliedLog[k][2] : k \in {j \in 1..Len(appliedLog) : appliedLog[j][1] = e}}

\* relSeqs whose STATE/BEGIN has been sent *in the current epoch* but not
\* yet applied *in the current epoch* -- F's notion of "currently open."
\* Must be scoped to the current epoch: a Fault bumps epoch and drops the
\* ending epoch's fStateSent entries, but the same numeric relSeq can
\* legitimately be re-begun under a later epoch, so an unscoped
\* membership test would wrongly conflate the two.
OpenOnF == {r \in RelSeqs : (\E s \in fStateSent : s.relSeq = r /\ s.epoch = epoch) /\
                              r \notin AppliedRelSeqsInEpoch(epoch)}

SingleActiveTxOnF ==
    /\ Cardinality(OpenOnF) <= 1
    /\ (fActive # NoTx => OpenOnF = {fActive.relSeq})
    /\ (fActive = NoTx => OpenOnF = {})

\* Per-epoch form of "F's route cursor tracks how much it has applied":
\* appliedLog itself spans every epoch and is never reset, so the count
\* must be scoped to the current epoch to compare against fRouteCursor,
\* which Fault resets to 0 every time.
RouteCursorTracksApplied == fRouteCursor = Cardinality(AppliedRelSeqsInEpoch(epoch))

ActiveIsNextExpected == fActive # NoTx => fActive.relSeq = fRouteCursor

\* Within each epoch (including every past one still recorded in
\* appliedLog), the applied relSeqs are exactly 0,1,2,...,n-1 -- i.e. a
\* contiguous run starting at 0, with no gap or duplicate.
InOrderApply ==
    \A e \in Epochs :
        LET s == AppliedRelSeqsInEpoch(e)
        IN s = 0..(Cardinality(s) - 1)

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
\* expects (catches "STATE sent at park time").  Scoped to the current
\* epoch: an older-epoch fStateSent entry (if any briefly survive a
\* fault) is no longer meaningful against a freshly-reset fRouteCursor.
StateSentNeverAheadOfCursor ==
    \A s \in fStateSent : s.epoch = epoch => s.relSeq <= fRouteCursor

\* Symmetric to QueueCurrentEpochOnly, for F's parking map (catches a
\* missing bulk release on fault).
ParkedCurrentEpochOnly == \A p \in fParked : p.epoch = epoch

\* The K=1 collapse argument: at PipelineDepth = 1, parking can never
\* fire, so fParked stays empty for the whole run.
K1Equivalence == PipelineDepth = 1 => fParked = {}

\* Every C-side retire is backed by F having actually applied that exact
\* (epoch, relSeq) pair -- never a stale ack left over from a different
\* epoch happening to share the same (reset-to-0) relSeq numbering.  This
\* is the invariant M7 (CRetireCommitMutantNoEpochCheck) is designed to
\* violate.
NoCrossEpochRetire ==
    \A k \in 1..Len(cRetiredLog) :
        \E j \in 1..Len(appliedLog) : appliedLog[j] = cRetiredLog[k]

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
    /\ [kind |-> "BEGIN", relSeq |-> relSeq, epoch |-> epoch] \in net
    /\ relSeq > fRouteCursor
    /\ relSeq <= fRouteCursor + PipelineDepth - 1
    /\ fActive' = [relSeq |-> relSeq, epoch |-> epoch]
    /\ fStateSent' = fStateSent \cup {[relSeq |-> relSeq, epoch |-> epoch]}
    /\ net' = net \ {[kind |-> "BEGIN", relSeq |-> relSeq, epoch |-> epoch]}
    /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, fParked,
                   fRouteCursor, epoch, appliedLog, cRetiredLog>>

NextM1 ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArriveParkMutantEarlyActivate(relSeq))
    \/ FRejectStaleBegin
    \/ FCommit
    \/ CRetireCommit
    \/ CDropStaleAck
    \/ Fault
SpecM1 == Init /\ [][NextM1]_vars /\ WF_vars(NextM1)

\* M2: release of n+2 on n's commit -- on commit, release an arbitrary
\* parked successor instead of specifically committed+1.  Needs
\* PipelineDepth = 3 to have two parked entries to pick the wrong one from.
\* victim is explored nondeterministically over every member of fParked
\* (plus NoTx when nothing is parked) rather than picked once via CHOOSE,
\* so TLC's search is guaranteed to visit the wrong-victim branch instead
\* of silently settling on whichever element CHOOSE happens to return.
\* Expected violation: ActiveIsNextExpected.
FCommitMutantEarlyRelease(victim) ==
    /\ fActive # NoTx
    /\ victim \in (fParked \cup {NoTx})
    /\ ~NetHasCurrent("ACK")
    /\ LET committed == fActive.relSeq
       IN
           /\ appliedLog' = Append(appliedLog, <<epoch, committed>>)
           /\ fRouteCursor' = committed + 1
           /\ fActive' = victim
           /\ fParked' = IF victim = NoTx THEN fParked ELSE fParked \ {victim}
           /\ fStateSent' = IF victim = NoTx
                             THEN fStateSent
                             ELSE fStateSent \cup {[relSeq |-> victim.relSeq, epoch |-> epoch]}
           /\ net' = net \cup {[kind |-> "ACK", relSeq |-> committed, epoch |-> epoch]}
    /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, epoch, cRetiredLog>>

NextM2 ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArrivePark(relSeq))
    \/ FRejectStaleBegin
    \/ (\E victim \in (fParked \cup {NoTx}) : FCommitMutantEarlyRelease(victim))
    \/ CRetireCommit
    \/ CDropStaleAck
    \/ Fault
SpecM2 == Init /\ [][NextM2]_vars /\ WF_vars(NextM2)

\* M3: bulk release missing on fault -- parked entries survive a fault
\* instead of being cleared.  Expected violation: ParkedCurrentEpochOnly.
\* Everything else about this Fault variant matches the real cold reset
\* exactly (see Fault's comment); fParked surviving is the one
\* deliberate bug.
FaultMutantNoBulkRelease ==
    /\ epoch < MaxEpoch
    /\ epoch' = epoch + 1
    /\ cQueue' = <<>>
    /\ cPredictedCursor' = 0
    /\ cConfirmedCursor' = 0
    /\ fRouteCursor' = 0
    /\ fParked' = fParked
    /\ fActive' = NoTx
    /\ fStateSent' = {s \in fStateSent : s.epoch # epoch}
    /\ UNCHANGED <<appliedLog, net, cRetiredLog>>

NextM3 ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArrivePark(relSeq))
    \/ FRejectStaleBegin
    \/ FCommit
    \/ CRetireCommit
    \/ CDropStaleAck
    \/ FaultMutantNoBulkRelease
SpecM3 == Init /\ [][NextM3]_vars /\ WF_vars(NextM3)

\* M4: STATE sent at park time -- fStateSent is updated when a successor
\* is parked, instead of waiting for release.  Expected violation:
\* StateSentNeverAheadOfCursor.
FArriveParkMutantStateAtParkTime(relSeq) ==
    /\ [kind |-> "BEGIN", relSeq |-> relSeq, epoch |-> epoch] \in net
    /\ relSeq > fRouteCursor
    /\ relSeq <= fRouteCursor + PipelineDepth - 1
    /\ ~(\E p \in fParked : p.relSeq = relSeq)
    /\ fParked' = fParked \cup {[relSeq |-> relSeq, epoch |-> epoch]}
    /\ fStateSent' = fStateSent \cup {[relSeq |-> relSeq, epoch |-> epoch]}
    /\ net' = net \ {[kind |-> "BEGIN", relSeq |-> relSeq, epoch |-> epoch]}
    /\ UNCHANGED <<cQueue, cConfirmedCursor, cPredictedCursor, fActive,
                   fRouteCursor, epoch, appliedLog, cRetiredLog>>

NextM4 ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArriveParkMutantStateAtParkTime(relSeq))
    \/ FRejectStaleBegin
    \/ FCommit
    \/ CRetireCommit
    \/ CDropStaleAck
    \/ Fault
SpecM4 == Init /\ [][NextM4]_vars /\ WF_vars(NextM4)

\* M5: C retiring out of order -- retire an arbitrary confirmed queue
\* entry instead of only ever the FIFO head.  The epoch check on the ack
\* is kept intact (that is M7's bug, not this one); only the "head-only"
\* discipline is broken.  Expected violation: DigestChainConsistency
\* (specifically QueueAnchoredAtConfirmed).
CRetireCommitMutantAnyOrder ==
    /\ Len(cQueue) > 0
    /\ \E k \in 1..Len(cQueue) :
        /\ cQueue[k].epoch = epoch
        /\ \E m \in net : m.kind = "ACK" /\ m.relSeq = cQueue[k].relSeq /\ m.epoch = epoch
        /\ cQueue' = [n \in 1..(Len(cQueue) - 1) |->
                        IF n < k THEN cQueue[n] ELSE cQueue[n + 1]]
        /\ cConfirmedCursor' = cQueue[k].relSeq + 1
        /\ net' = net \ {[kind |-> "ACK", relSeq |-> cQueue[k].relSeq, epoch |-> epoch]}
        /\ cRetiredLog' = Append(cRetiredLog, <<epoch, cQueue[k].relSeq>>)
    /\ UNCHANGED <<cPredictedCursor, fActive, fParked, fRouteCursor,
                   fStateSent, epoch, appliedLog>>

NextM5 ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArrivePark(relSeq))
    \/ FRejectStaleBegin
    \/ FCommit
    \/ CRetireCommitMutantAnyOrder
    \/ CDropStaleAck
    \/ Fault
SpecM5 == Init /\ [][NextM5]_vars /\ WF_vars(NextM5)

\* M6: a successor surviving a fault -- cQueue (and the cPredictedCursor
\* it is anchored to) is left untouched by Fault instead of being
\* cleared.  Everything else about this Fault variant matches the real
\* cold reset exactly (see Fault's comment); cQueue/cPredictedCursor
\* surviving is the one deliberate bug.  Expected violation:
\* QueueCurrentEpochOnly.
FaultMutantSurvivingSuccessor ==
    /\ epoch < MaxEpoch
    /\ epoch' = epoch + 1
    /\ cQueue' = cQueue
    /\ cPredictedCursor' = cPredictedCursor
    /\ cConfirmedCursor' = 0
    /\ fRouteCursor' = 0
    /\ fParked' = {}
    /\ fActive' = NoTx
    /\ fStateSent' = {s \in fStateSent : s.epoch # epoch}
    /\ UNCHANGED <<appliedLog, net, cRetiredLog>>

NextM6 ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArrivePark(relSeq))
    \/ FRejectStaleBegin
    \/ FCommit
    \/ CRetireCommit
    \/ CDropStaleAck
    \/ FaultMutantSurvivingSuccessor
SpecM6 == Init /\ [][NextM6]_vars /\ WF_vars(NextM6)

\* M7: stale ack retires new-epoch head -- CRetireCommit's ack lookup
\* drops the "m.epoch = epoch" check, so a stale ack left in net from a
\* prior (now-superseded) epoch can retire a current-epoch queue head
\* that merely happens to share the same (reset-to-0) relSeq numbering,
\* even though F has not actually applied that relSeq under the current
\* epoch.  The head's own epoch check is kept intact (the head really is
\* current-epoch; it is the ack's epoch that is wrongly ignored).
\* Expected violation: NoCrossEpochRetire.
CRetireCommitMutantNoEpochCheck ==
    \E m \in net :
        /\ m.kind = "ACK"
        /\ Len(cQueue) > 0
        /\ LET head == Head(cQueue)
           IN
               /\ head.epoch = epoch
               /\ m.relSeq = head.relSeq
               /\ cQueue' = Tail(cQueue)
               /\ cConfirmedCursor' = head.relSeq + 1
               /\ net' = net \ {m}
               /\ cRetiredLog' = Append(cRetiredLog, <<epoch, head.relSeq>>)
    /\ UNCHANGED <<cPredictedCursor, fActive, fParked, fRouteCursor,
                   fStateSent, epoch, appliedLog>>

NextM7 ==
    \/ CBegin
    \/ (\E relSeq \in RelSeqs : FArriveProceed(relSeq))
    \/ (\E relSeq \in RelSeqs : FArrivePark(relSeq))
    \/ FRejectStaleBegin
    \/ FCommit
    \/ CRetireCommitMutantNoEpochCheck
    \/ CDropStaleAck
    \/ Fault
SpecM7 == Init /\ [][NextM7]_vars /\ WF_vars(NextM7)

=============================================================================
