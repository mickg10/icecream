----------------- MODULE LifecycleAuthorityStartedWitness -----------------
(***************************************************************************
Reachability witness for the conditional lifecycle liveness theorem.

`StartedEventuallyTerminates` is intentionally conditional: it does not claim
that the environment eventually begins a job. This direct witness prevents the
accepted liveness row from passing merely because no execution reaches a
started assignment.

The witness deliberately records one rejected wrong-worker Begin before the
assigned worker's valid Begin. That supplies an observable three-state trace
without constraining the product safety model or relying on TLC stuttering.
***************************************************************************)
EXTENDS LifecycleAuthority

WitnessAssignedBegin ==
    /\ invalidBeginCount > 0
    /\ BeginFromAssignedWorker

WitnessNext == BeginFromWrongWorker \/ WitnessAssignedBegin

WitnessSpec == Init /\ [][WitnessNext]_vars

NoStartedState == ~begunEver

=============================================================================
