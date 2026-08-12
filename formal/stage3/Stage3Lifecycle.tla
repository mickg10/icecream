--------------------------- MODULE Stage3Lifecycle ---------------------------
EXTENDS Naturals, FiniteSets

(***************************************************************************
Finite counterexample-search model for the Stage 3 lifecycle.

This is a review draft, not an accepted refinement proof.  It intentionally
keeps semantic transport in Stage3SendRefinement and retains completed job IDs
for the bounded run so delayed duplicates cannot resurrect an entry.
***************************************************************************)

CONSTANTS Clients, Entries, Generations, Jobs, Assignments, Nonces,
          Capacity, NoGeneration, NoJob, NoAssignment, NoNonce

ASSUME /\ IsFiniteSet(Clients) /\ Clients # {}
       /\ IsFiniteSet(Entries) /\ Entries # {}
       /\ IsFiniteSet(Generations) /\ Generations # {}
       /\ IsFiniteSet(Jobs) /\ Jobs # {}
       /\ IsFiniteSet(Assignments) /\ Assignments # {}
       /\ IsFiniteSet(Nonces) /\ Nonces # {}
       /\ Capacity \in Nat
       /\ NoGeneration \notin Generations
       /\ NoJob \notin Jobs
       /\ NoAssignment \notin Assignments
       /\ NoNonce \notin Nonces

SessionStates == {"Absent", "Active", "Closing", "Closed"}
EntryPhases ==
  {"Idle", "WaitDecision", "RemoteDelivered", "LocalWaitCapacity",
   "LocalBound", "LocalDelivered", "LocalStarted", "Completed"}
Kinds == {"None", "Remote", "Local", "NoCS"}
Terminals == {"None", "Done", "NoCS", "Cleanup"}
SchedulerPhases == {"None", "Assigned", "Begun", "Done", "Cancelled"}

LivePhases == EntryPhases \ {"Idle", "Completed"}
ChargedPhases == {"LocalDelivered", "LocalStarted"}
DonePhases == {"RemoteDelivered", "LocalStarted"}
PairSet == Clients \X Entries

VARIABLES session, sessionGen, requestOpen, requestGen, requested,
          phase, kind, job, assignment, nonce,
          decisionSessionGen, decisionRequestGen, ownerGen,
          slotCharged, beginCommitted, terminal, schedulerPhase,
          completedJobs, staleRejected, illegalDoneRejected

vars ==
  <<session, sessionGen, requestOpen, requestGen, requested,
    phase, kind, job, assignment, nonce,
    decisionSessionGen, decisionRequestGen, ownerGen,
    slotCharged, beginCommitted, terminal, schedulerPhase,
    completedJobs, staleRejected, illegalDoneRejected>>

Requested(c, e) == e \in requested[c]

ChargedPairs ==
  {p \in PairSet : slotCharged[p[1]][p[2]]}

TokenAt(p) ==
  LET c == p[1]
      e == p[2]
  IN <<decisionSessionGen[c][e], decisionRequestGen[c][e],
       assignment[c][e], job[c][e], nonce[c][e]>>

LivePairsForJob(j) ==
  {p \in PairSet :
     LET c == p[1]
         e == p[2]
     IN phase[c][e] \in LivePhases /\ job[c][e] = j}

LivePairsForToken(t) ==
  {p \in PairSet :
     LET c == p[1]
         e == p[2]
     IN /\ phase[c][e] \in LivePhases
        /\ kind[c][e] # "None"
        /\ TokenAt(p) = t}

FreshToken(sg, rg, a, j, n) ==
  /\ j \in Jobs \ completedJobs
  /\ a \in Assignments
  /\ n \in Nonces
  /\ LivePairsForJob(j) = {}
  /\ LivePairsForToken(<<sg, rg, a, j, n>>) = {}

Init ==
  /\ session = [c \in Clients |-> "Absent"]
  /\ sessionGen = [c \in Clients |-> NoGeneration]
  /\ requestOpen = [c \in Clients |-> FALSE]
  /\ requestGen = [c \in Clients |-> NoGeneration]
  /\ requested = [c \in Clients |-> {}]
  /\ phase = [c \in Clients |-> [e \in Entries |-> "Idle"]]
  /\ kind = [c \in Clients |-> [e \in Entries |-> "None"]]
  /\ job = [c \in Clients |-> [e \in Entries |-> NoJob]]
  /\ assignment =
       [c \in Clients |-> [e \in Entries |-> NoAssignment]]
  /\ nonce = [c \in Clients |-> [e \in Entries |-> NoNonce]]
  /\ decisionSessionGen =
       [c \in Clients |-> [e \in Entries |-> NoGeneration]]
  /\ decisionRequestGen =
       [c \in Clients |-> [e \in Entries |-> NoGeneration]]
  /\ ownerGen = [c \in Clients |-> [e \in Entries |-> NoGeneration]]
  /\ slotCharged = [c \in Clients |-> [e \in Entries |-> FALSE]]
  /\ beginCommitted = [c \in Clients |-> [e \in Entries |-> FALSE]]
  /\ terminal = [c \in Clients |-> [e \in Entries |-> "None"]]
  /\ schedulerPhase = [c \in Clients |-> [e \in Entries |-> "None"]]
  /\ completedJobs = {}
  /\ staleRejected = {}
  /\ illegalDoneRejected = {}

OpenSession(c, sg) ==
  /\ c \in Clients /\ sg \in Generations
  /\ session[c] = "Absent"
  /\ session' = [session EXCEPT ![c] = "Active"]
  /\ sessionGen' = [sessionGen EXCEPT ![c] = sg]
  /\ UNCHANGED <<requestOpen, requestGen, requested, phase, kind, job,
                 assignment, nonce, decisionSessionGen,
                 decisionRequestGen, ownerGen, slotCharged,
                 beginCommitted, terminal, schedulerPhase,
                 completedJobs, staleRejected, illegalDoneRejected>>

AcceptRequest(c, rg, es) ==
  /\ c \in Clients /\ rg \in Generations /\ es \in SUBSET Entries
  /\ session[c] = "Active" /\ ~requestOpen[c]
  /\ requestOpen' = [requestOpen EXCEPT ![c] = TRUE]
  /\ requestGen' = [requestGen EXCEPT ![c] = rg]
  /\ requested' = [requested EXCEPT ![c] = es]
  /\ phase' =
       [phase EXCEPT ![c] = [e \in Entries |->
          IF e \in es THEN "WaitDecision" ELSE "Idle"]]
  /\ UNCHANGED <<session, sessionGen, kind, job, assignment, nonce,
                 decisionSessionGen, decisionRequestGen, ownerGen,
                 slotCharged, beginCommitted, terminal, schedulerPhase,
                 completedJobs, staleRejected, illegalDoneRejected>>

RecordDecision(c, e, a, j, n, sg, rg, k) ==
  /\ c \in Clients /\ e \in Entries
  /\ a \in Assignments /\ j \in Jobs /\ n \in Nonces
  /\ k \in {"Remote", "Local"}
  /\ session[c] = "Active" /\ Requested(c, e)
  /\ phase[c][e] = "WaitDecision"
  /\ sg = sessionGen[c] /\ rg = requestGen[c]
  /\ FreshToken(sg, rg, a, j, n)
  /\ phase' = [phase EXCEPT ![c][e] =
       IF k = "Remote" THEN "RemoteDelivered" ELSE "LocalWaitCapacity"]
  /\ kind' = [kind EXCEPT ![c][e] = k]
  /\ job' = [job EXCEPT ![c][e] = j]
  /\ assignment' = [assignment EXCEPT ![c][e] = a]
  /\ nonce' = [nonce EXCEPT ![c][e] = n]
  /\ decisionSessionGen' = [decisionSessionGen EXCEPT ![c][e] = sg]
  /\ decisionRequestGen' = [decisionRequestGen EXCEPT ![c][e] = rg]
  /\ schedulerPhase' = [schedulerPhase EXCEPT ![c][e] = "Assigned"]
  /\ UNCHANGED <<session, sessionGen, requestOpen, requestGen, requested,
                 ownerGen, slotCharged, beginCommitted, terminal,
                 completedJobs, staleRejected, illegalDoneRejected>>

RemoteDecision(c, e, a, j, n, sg, rg) ==
  RecordDecision(c, e, a, j, n, sg, rg, "Remote")

LocalDecision(c, e, a, j, n, sg, rg) ==
  RecordDecision(c, e, a, j, n, sg, rg, "Local")

NoCSDecision(c, e, a, j, n, sg, rg) ==
  /\ c \in Clients /\ e \in Entries
  /\ a \in Assignments /\ j \in Jobs /\ n \in Nonces
  /\ session[c] = "Active" /\ Requested(c, e)
  /\ phase[c][e] = "WaitDecision"
  /\ sg = sessionGen[c] /\ rg = requestGen[c]
  /\ FreshToken(sg, rg, a, j, n)
  /\ phase' = [phase EXCEPT ![c][e] = "Completed"]
  /\ kind' = [kind EXCEPT ![c][e] = "NoCS"]
  /\ job' = [job EXCEPT ![c][e] = j]
  /\ assignment' = [assignment EXCEPT ![c][e] = a]
  /\ nonce' = [nonce EXCEPT ![c][e] = n]
  /\ decisionSessionGen' = [decisionSessionGen EXCEPT ![c][e] = sg]
  /\ decisionRequestGen' = [decisionRequestGen EXCEPT ![c][e] = rg]
  /\ terminal' = [terminal EXCEPT ![c][e] = "NoCS"]
  /\ schedulerPhase' = [schedulerPhase EXCEPT ![c][e] = "Cancelled"]
  /\ completedJobs' = completedJobs \cup {j}
  /\ UNCHANGED <<session, sessionGen, requestOpen, requestGen, requested,
                 ownerGen, slotCharged, beginCommitted,
                 staleRejected, illegalDoneRejected>>

RejectStaleDecision(c, e, sg, rg) ==
  /\ c \in Clients /\ e \in Entries
  /\ session[c] = "Active" /\ Requested(c, e)
  /\ \/ sg # sessionGen[c]
     \/ rg # requestGen[c]
  /\ <<c, e>> \notin staleRejected
  /\ staleRejected' = staleRejected \cup {<<c, e>>}
  /\ UNCHANGED <<session, sessionGen, requestOpen, requestGen, requested,
                 phase, kind, job, assignment, nonce,
                 decisionSessionGen, decisionRequestGen, ownerGen,
                 slotCharged, beginCommitted, terminal, schedulerPhase,
                 completedJobs, illegalDoneRejected>>

BindLocal(c, e) ==
  /\ c \in Clients /\ e \in Entries
  /\ session[c] = "Active" /\ phase[c][e] = "LocalWaitCapacity"
  /\ decisionSessionGen[c][e] = sessionGen[c]
  /\ decisionRequestGen[c][e] = requestGen[c]
  /\ phase' = [phase EXCEPT ![c][e] = "LocalBound"]
  /\ ownerGen' = [ownerGen EXCEPT ![c][e] = sessionGen[c]]
  /\ UNCHANGED <<session, sessionGen, requestOpen, requestGen, requested,
                 kind, job, assignment, nonce, decisionSessionGen,
                 decisionRequestGen, slotCharged, beginCommitted,
                 terminal, schedulerPhase, completedJobs,
                 staleRejected, illegalDoneRejected>>

DeliverLocal(c, e) ==
  /\ c \in Clients /\ e \in Entries
  /\ session[c] = "Active" /\ phase[c][e] = "LocalBound"
  /\ ownerGen[c][e] = sessionGen[c]
  /\ Cardinality(ChargedPairs) < Capacity
  /\ phase' = [phase EXCEPT ![c][e] = "LocalDelivered"]
  /\ slotCharged' = [slotCharged EXCEPT ![c][e] = TRUE]
  /\ UNCHANGED <<session, sessionGen, requestOpen, requestGen, requested,
                 kind, job, assignment, nonce, decisionSessionGen,
                 decisionRequestGen, ownerGen, beginCommitted,
                 terminal, schedulerPhase, completedJobs,
                 staleRejected, illegalDoneRejected>>

CommitBegin(c, e) ==
  /\ c \in Clients /\ e \in Entries
  /\ session[c] = "Active" /\ phase[c][e] = "LocalDelivered"
  /\ ownerGen[c][e] = sessionGen[c] /\ slotCharged[c][e]
  /\ schedulerPhase[c][e] = "Assigned"
  /\ phase' = [phase EXCEPT ![c][e] = "LocalStarted"]
  /\ beginCommitted' = [beginCommitted EXCEPT ![c][e] = TRUE]
  /\ schedulerPhase' = [schedulerPhase EXCEPT ![c][e] = "Begun"]
  /\ UNCHANGED <<session, sessionGen, requestOpen, requestGen, requested,
                 kind, job, assignment, nonce, decisionSessionGen,
                 decisionRequestGen, ownerGen, slotCharged, terminal,
                 completedJobs, staleRejected, illegalDoneRejected>>

BeginNoCommitFailure(c, e) ==
  /\ c \in Clients /\ e \in Entries
  /\ session[c] = "Active" /\ phase[c][e] = "LocalDelivered"
  /\ ~beginCommitted[c][e]
  /\ session' = [session EXCEPT ![c] = "Closing"]
  /\ UNCHANGED <<sessionGen, requestOpen, requestGen, requested, phase,
                 kind, job, assignment, nonce, decisionSessionGen,
                 decisionRequestGen, ownerGen, slotCharged,
                 beginCommitted, terminal, schedulerPhase,
                 completedJobs, staleRejected, illegalDoneRejected>>

AcceptDone(c, e) ==
  /\ c \in Clients /\ e \in Entries
  /\ session[c] = "Active" /\ Requested(c, e)
  /\ phase[c][e] \in DonePhases /\ terminal[c][e] = "None"
  /\ IF phase[c][e] = "LocalStarted"
        THEN beginCommitted[c][e] /\ kind[c][e] = "Local"
        ELSE kind[c][e] = "Remote"
  /\ phase' = [phase EXCEPT ![c][e] = "Completed"]
  /\ slotCharged' = [slotCharged EXCEPT ![c][e] = FALSE]
  /\ terminal' = [terminal EXCEPT ![c][e] = "Done"]
  /\ schedulerPhase' = [schedulerPhase EXCEPT ![c][e] = "Done"]
  /\ completedJobs' = completedJobs \cup {job[c][e]}
  /\ UNCHANGED <<session, sessionGen, requestOpen, requestGen, requested,
                 kind, job, assignment, nonce, decisionSessionGen,
                 decisionRequestGen, ownerGen, beginCommitted,
                 staleRejected, illegalDoneRejected>>

RejectIllegalDone(c, e) ==
  /\ c \in Clients /\ e \in Entries
  /\ session[c] = "Active" /\ Requested(c, e)
  /\ terminal[c][e] = "None" /\ phase[c][e] \notin DonePhases
  /\ <<c, e>> \notin illegalDoneRejected
  /\ illegalDoneRejected' = illegalDoneRejected \cup {<<c, e>>}
  /\ UNCHANGED <<session, sessionGen, requestOpen, requestGen, requested,
                 phase, kind, job, assignment, nonce,
                 decisionSessionGen, decisionRequestGen, ownerGen,
                 slotCharged, beginCommitted, terminal, schedulerPhase,
                 completedJobs, staleRejected>>

BeginClose(c) ==
  /\ c \in Clients /\ session[c] = "Active"
  /\ session' = [session EXCEPT ![c] = "Closing"]
  /\ UNCHANGED <<sessionGen, requestOpen, requestGen, requested, phase,
                 kind, job, assignment, nonce, decisionSessionGen,
                 decisionRequestGen, ownerGen, slotCharged,
                 beginCommitted, terminal, schedulerPhase,
                 completedJobs, staleRejected, illegalDoneRejected>>

CleanupEntry(c, e) ==
  /\ c \in Clients /\ e \in Entries
  /\ session[c] = "Closing" /\ Requested(c, e)
  /\ terminal[c][e] = "None"
  /\ phase' = [phase EXCEPT ![c][e] = "Completed"]
  /\ slotCharged' = [slotCharged EXCEPT ![c][e] = FALSE]
  /\ terminal' = [terminal EXCEPT ![c][e] = "Cleanup"]
  /\ schedulerPhase' = [schedulerPhase EXCEPT ![c][e] = "Cancelled"]
  /\ completedJobs' =
       IF job[c][e] = NoJob THEN completedJobs
       ELSE completedJobs \cup {job[c][e]}
  /\ UNCHANGED <<session, sessionGen, requestOpen, requestGen, requested,
                 kind, job, assignment, nonce, decisionSessionGen,
                 decisionRequestGen, ownerGen, beginCommitted,
                 staleRejected, illegalDoneRejected>>

FinishClose(c) ==
  /\ c \in Clients /\ session[c] = "Closing"
  /\ \A e \in requested[c] : terminal[c][e] # "None"
  /\ \A e \in Entries : ~slotCharged[c][e]
  /\ session' = [session EXCEPT ![c] = "Closed"]
  /\ UNCHANGED <<sessionGen, requestOpen, requestGen, requested, phase,
                 kind, job, assignment, nonce, decisionSessionGen,
                 decisionRequestGen, ownerGen, slotCharged,
                 beginCommitted, terminal, schedulerPhase,
                 completedJobs, staleRejected, illegalDoneRejected>>

ResetClosed(c) ==
  /\ c \in Clients /\ session[c] = "Closed"
  /\ session' = [session EXCEPT ![c] = "Absent"]
  /\ sessionGen' = [sessionGen EXCEPT ![c] = NoGeneration]
  /\ requestOpen' = [requestOpen EXCEPT ![c] = FALSE]
  /\ requestGen' = [requestGen EXCEPT ![c] = NoGeneration]
  /\ requested' = [requested EXCEPT ![c] = {}]
  /\ phase' = [phase EXCEPT ![c] = [e \in Entries |-> "Idle"]]
  /\ kind' = [kind EXCEPT ![c] = [e \in Entries |-> "None"]]
  /\ job' = [job EXCEPT ![c] = [e \in Entries |-> NoJob]]
  /\ assignment' =
       [assignment EXCEPT ![c] = [e \in Entries |-> NoAssignment]]
  /\ nonce' = [nonce EXCEPT ![c] = [e \in Entries |-> NoNonce]]
  /\ decisionSessionGen' =
       [decisionSessionGen EXCEPT ![c] = [e \in Entries |-> NoGeneration]]
  /\ decisionRequestGen' =
       [decisionRequestGen EXCEPT ![c] = [e \in Entries |-> NoGeneration]]
  /\ ownerGen' = [ownerGen EXCEPT ![c] = [e \in Entries |-> NoGeneration]]
  /\ slotCharged' = [slotCharged EXCEPT ![c] = [e \in Entries |-> FALSE]]
  /\ beginCommitted' =
       [beginCommitted EXCEPT ![c] = [e \in Entries |-> FALSE]]
  /\ terminal' = [terminal EXCEPT ![c] = [e \in Entries |-> "None"]]
  /\ schedulerPhase' =
       [schedulerPhase EXCEPT ![c] = [e \in Entries |-> "None"]]
  /\ staleRejected' = staleRejected \ ({c} \X Entries)
  /\ illegalDoneRejected' = illegalDoneRejected \ ({c} \X Entries)
  /\ UNCHANGED completedJobs

Next ==
  \/ \E c \in Clients, sg \in Generations : OpenSession(c, sg)
  \/ \E c \in Clients, rg \in Generations, es \in SUBSET Entries :
       AcceptRequest(c, rg, es)
  \/ \E c \in Clients, e \in Entries, a \in Assignments,
        j \in Jobs, n \in Nonces, sg \in Generations, rg \in Generations :
       RemoteDecision(c, e, a, j, n, sg, rg)
  \/ \E c \in Clients, e \in Entries, a \in Assignments,
        j \in Jobs, n \in Nonces, sg \in Generations, rg \in Generations :
       LocalDecision(c, e, a, j, n, sg, rg)
  \/ \E c \in Clients, e \in Entries, a \in Assignments,
        j \in Jobs, n \in Nonces, sg \in Generations, rg \in Generations :
       NoCSDecision(c, e, a, j, n, sg, rg)
  \/ \E c \in Clients, e \in Entries,
        sg \in (Generations \cup {NoGeneration}),
        rg \in (Generations \cup {NoGeneration}) :
       RejectStaleDecision(c, e, sg, rg)
  \/ \E c \in Clients, e \in Entries : BindLocal(c, e)
  \/ \E c \in Clients, e \in Entries : DeliverLocal(c, e)
  \/ \E c \in Clients, e \in Entries : CommitBegin(c, e)
  \/ \E c \in Clients, e \in Entries : BeginNoCommitFailure(c, e)
  \/ \E c \in Clients, e \in Entries : AcceptDone(c, e)
  \/ \E c \in Clients, e \in Entries : RejectIllegalDone(c, e)
  \/ \E c \in Clients : BeginClose(c)
  \/ \E c \in Clients, e \in Entries : CleanupEntry(c, e)
  \/ \E c \in Clients : FinishClose(c)
  \/ \E c \in Clients : ResetClosed(c)

SafetySpec == Init /\ [][Next]_vars

Fairness ==
  /\ \A c \in Clients, e \in Entries : WF_vars(CleanupEntry(c, e))
  /\ \A c \in Clients : WF_vars(FinishClose(c))

Spec == SafetySpec /\ Fairness

TypeOK ==
  /\ session \in [Clients -> SessionStates]
  /\ sessionGen \in [Clients -> (Generations \cup {NoGeneration})]
  /\ requestOpen \in [Clients -> BOOLEAN]
  /\ requestGen \in [Clients -> (Generations \cup {NoGeneration})]
  /\ requested \in [Clients -> SUBSET Entries]
  /\ phase \in [Clients -> [Entries -> EntryPhases]]
  /\ kind \in [Clients -> [Entries -> Kinds]]
  /\ job \in [Clients -> [Entries -> (Jobs \cup {NoJob})]]
  /\ assignment \in
       [Clients -> [Entries -> (Assignments \cup {NoAssignment})]]
  /\ nonce \in [Clients -> [Entries -> (Nonces \cup {NoNonce})]]
  /\ decisionSessionGen \in
       [Clients -> [Entries -> (Generations \cup {NoGeneration})]]
  /\ decisionRequestGen \in
       [Clients -> [Entries -> (Generations \cup {NoGeneration})]]
  /\ ownerGen \in
       [Clients -> [Entries -> (Generations \cup {NoGeneration})]]
  /\ slotCharged \in [Clients -> [Entries -> BOOLEAN]]
  /\ beginCommitted \in [Clients -> [Entries -> BOOLEAN]]
  /\ terminal \in [Clients -> [Entries -> Terminals]]
  /\ schedulerPhase \in [Clients -> [Entries -> SchedulerPhases]]
  /\ completedJobs \subseteq Jobs
  /\ staleRejected \subseteq PairSet
  /\ illegalDoneRejected \subseteq PairSet

StartedImpliesBeginCommitted ==
  \A p \in PairSet :
    LET c == p[1]
        e == p[2]
    IN phase[c][e] = "LocalStarted" => beginCommitted[c][e]

ChargedExactlyInChargedPhase ==
  \A p \in PairSet :
    LET c == p[1]
        e == p[2]
    IN slotCharged[c][e] = (phase[c][e] \in ChargedPhases)

CapacityBound == Cardinality(ChargedPairs) <= Capacity

DoneAuthority ==
  \A p \in PairSet :
    LET c == p[1]
        e == p[2]
    IN terminal[c][e] = "Done"
       => \/ kind[c][e] = "Remote"
          \/ kind[c][e] = "Local" /\ beginCommitted[c][e]

CompletedIffTerminal ==
  \A p \in PairSet :
    LET c == p[1]
        e == p[2]
    IN (phase[c][e] = "Completed") = (terminal[c][e] # "None")

LiveJobUnique ==
  \A j \in Jobs : Cardinality(LivePairsForJob(j)) <= 1

LiveTokenUnique ==
  \A p \in PairSet, q \in PairSet :
    LET pc == p[1]
        pe == p[2]
        qc == q[1]
        qe == q[2]
    IN /\ phase[pc][pe] \in LivePhases
       /\ phase[qc][qe] \in LivePhases
       /\ kind[pc][pe] # "None"
       /\ kind[qc][qe] # "None"
       /\ TokenAt(p) = TokenAt(q)
       => p = q

CompletedJobNotLive ==
  \A p \in PairSet :
    LET c == p[1]
        e == p[2]
    IN phase[c][e] \in LivePhases => job[c][e] \notin completedJobs

GenerationAgreement ==
  \A p \in PairSet :
    LET c == p[1]
        e == p[2]
    IN kind[c][e] # "None"
       => /\ decisionSessionGen[c][e] = sessionGen[c]
          /\ decisionRequestGen[c][e] = requestGen[c]

LocalOwnerAgreement ==
  \A p \in PairSet :
    LET c == p[1]
        e == p[2]
    IN phase[c][e] \in {"LocalBound", "LocalDelivered", "LocalStarted"}
       => /\ kind[c][e] = "Local"
          /\ ownerGen[c][e] = sessionGen[c]

SchedulerAgreement ==
  \A p \in PairSet :
    LET c == p[1]
        e == p[2]
    IN /\ phase[c][e] = "RemoteDelivered"
          => schedulerPhase[c][e] = "Assigned"
       /\ phase[c][e] = "LocalStarted"
          => schedulerPhase[c][e] = "Begun"
       /\ terminal[c][e] = "Done"
          => schedulerPhase[c][e] = "Done"
       /\ terminal[c][e] \in {"NoCS", "Cleanup"}
          => schedulerPhase[c][e] = "Cancelled"

ClosedClean ==
  \A c \in Clients :
    session[c] = "Closed"
    => /\ \A e \in requested[c] : terminal[c][e] # "None"
       /\ \A e \in Entries : ~slotCharged[c][e]

ClosingEventuallyClosed ==
  \A c \in Clients : session[c] = "Closing" ~> session[c] = "Closed"

=============================================================================
