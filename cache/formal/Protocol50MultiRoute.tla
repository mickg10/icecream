----------------------- MODULE Protocol50MultiRoute -----------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
A deliberately small composition model for scheduler reroute.

The same immutable PreparedTU may be attempted at F0 and F1, but each F owns
an independent CRoute/cache liability.  Routing a replacement to F1 is a fork
of work, not a move of F0's route state.  Likewise, accepting one compiler
result closes the logical job but cannot silently resolve either cache route.

The cache-transaction details inside each route remain in Protocol50.tla;
compiler input ownership and attempt restart remain in
Protocol50JobLifecycle.tla.  This bridge checks only the ownership boundary
that the current M1 reconnect helper conflates.
***************************************************************************)

CONSTANTS F0, F1, TU, NoTU,
          MutantMoveInsteadOfFork,
          MutantResultDropsLiability

ASSUME /\ F0 # F1
       /\ TU # NoTU
       /\ MutantMoveInsteadOfFork \in BOOLEAN
       /\ MutantResultDropsLiability \in BOOLEAN

Fs == {F0, F1}
RouteStates == {"Idle", "Active", "Durable", "Resolved", "Retired"}
AttemptStates == {"None", "Running", "Finished", "Cancelled"}

VARIABLE s
vars == <<s>>

Init ==
    s = [route                    |-> [f \in Fs |-> "Idle"],
         boundTu                  |-> [f \in Fs |-> NoTU],
         obligation               |-> [f \in Fs |-> FALSE],
         incarnationChanged       |-> [f \in Fs |-> FALSE],
         attempt                  |-> [f \in Fs |-> "None"],
         resultAvailable          |-> [f \in Fs |-> FALSE],
         acceptedResults          |-> {},
         badCrossRouteRetirement  |-> FALSE,
         badResultRetirement      |-> FALSE]

StartRoute(f) ==
    /\ f \in Fs
    /\ s.route[f] = "Idle"
    /\ s' = [s EXCEPT
                 !.route[f] = "Active",
                 !.boundTu[f] = TU,
                 !.obligation[f] = TRUE]

ForkReplacement(source, destination) ==
    /\ source \in Fs
    /\ destination \in Fs
    /\ source # destination
    /\ s.route[source] \in {"Active", "Durable"}
    /\ s.obligation[source]
    /\ s.route[destination] = "Idle"
    /\ IF MutantMoveInsteadOfFork
          THEN s' = [s EXCEPT
                         !.route[destination] = "Active",
                         !.boundTu[destination] = TU,
                         !.obligation[destination] = TRUE,
                         !.route[source] = "Idle",
                         !.boundTu[source] = NoTU,
                         !.obligation[source] = FALSE,
                         !.badCrossRouteRetirement = TRUE]
          ELSE s' = [s EXCEPT
                         !.route[destination] = "Active",
                         !.boundTu[destination] = TU,
                         !.obligation[destination] = TRUE]

CommitInput(f) ==
    /\ f \in Fs
    /\ s.route[f] = "Active"
    /\ s.obligation[f]
    /\ s' = [s EXCEPT !.route[f] = "Durable"]

AcceptCacheCommit(f) ==
    /\ f \in Fs
    /\ s.route[f] = "Durable"
    /\ s.obligation[f]
    /\ s' = [s EXCEPT
                 !.route[f] = "Resolved",
                 !.obligation[f] = FALSE]

ObserveIncarnationChange(f) ==
    /\ f \in Fs
    /\ s.route[f] \in {"Active", "Durable"}
    /\ s.obligation[f]
    /\ ~s.incarnationChanged[f]
    /\ s' = [s EXCEPT !.incarnationChanged[f] = TRUE]

RetireVerifiedIncarnation(f) ==
    /\ f \in Fs
    /\ s.route[f] \in {"Active", "Durable"}
    /\ s.obligation[f]
    /\ s.incarnationChanged[f]
    /\ s' = [s EXCEPT
                 !.route[f] = "Retired",
                 !.obligation[f] = FALSE]

StartCompiler(f) ==
    /\ f \in Fs
    /\ s.route[f] = "Durable"
    /\ s.attempt[f] = "None"
    /\ s' = [s EXCEPT !.attempt[f] = "Running"]

FinishCompiler(f) ==
    /\ f \in Fs
    /\ s.attempt[f] = "Running"
    /\ s' = [s EXCEPT
                 !.attempt[f] = "Finished",
                 !.resultAvailable[f] = TRUE]

CancelCompiler(f) ==
    /\ f \in Fs
    /\ s.attempt[f] = "Running"
    /\ s' = [s EXCEPT !.attempt[f] = "Cancelled"]

AcceptResult(f) ==
    LET hadUnresolved == \E other \in Fs : s.obligation[other]
    IN /\ f \in Fs
       /\ s.attempt[f] = "Finished"
       /\ s.resultAvailable[f]
       /\ s.acceptedResults = {}
       /\ IF MutantResultDropsLiability
             THEN s' = [s EXCEPT
                            !.acceptedResults = {f},
                            !.obligation = [other \in Fs |-> FALSE],
                            !.badResultRetirement =
                                s.badResultRetirement \/ hadUnresolved]
             ELSE s' = [s EXCEPT !.acceptedResults = {f}]

Next ==
    \/ \E f \in Fs : StartRoute(f)
    \/ \E source \in Fs, destination \in Fs :
           ForkReplacement(source, destination)
    \/ \E f \in Fs : CommitInput(f)
    \/ \E f \in Fs : AcceptCacheCommit(f)
    \/ \E f \in Fs : ObserveIncarnationChange(f)
    \/ \E f \in Fs : RetireVerifiedIncarnation(f)
    \/ \E f \in Fs : StartCompiler(f)
    \/ \E f \in Fs : FinishCompiler(f)
    \/ \E f \in Fs : CancelCompiler(f)
    \/ \E f \in Fs : AcceptResult(f)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ s.route \in [Fs -> RouteStates]
    /\ s.boundTu \in [Fs -> {TU, NoTU}]
    /\ s.obligation \in [Fs -> BOOLEAN]
    /\ s.incarnationChanged \in [Fs -> BOOLEAN]
    /\ s.attempt \in [Fs -> AttemptStates]
    /\ s.resultAvailable \in [Fs -> BOOLEAN]
    /\ s.acceptedResults \subseteq Fs
    /\ s.badCrossRouteRetirement \in BOOLEAN
    /\ s.badResultRetirement \in BOOLEAN

EveryStartedRouteBindsThePreparedTU ==
    \A f \in Fs :
        s.route[f] # "Idle" => s.boundTu[f] = TU

UnresolvedRouteHasExactlyOneLiability ==
    \A f \in Fs :
        s.obligation[f] <=> s.route[f] \in {"Active", "Durable"}

RetirementHasIncarnationProof ==
    \A f \in Fs :
        s.route[f] = "Retired" => s.incarnationChanged[f]

AtMostOneAcceptedResult ==
    Cardinality(s.acceptedResults) <= 1

AcceptedResultWasFinished ==
    \A f \in s.acceptedResults :
        /\ s.attempt[f] = "Finished"
        /\ s.resultAvailable[f]

CrossRouteForkPreservesSource ==
    ~s.badCrossRouteRetirement

ResultDoesNotRetireCache ==
    ~s.badResultRetirement

=============================================================================
