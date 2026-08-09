----------------- MODULE LifecycleAuthorityStartedWitness -----------------
(***************************************************************************
Reachability witness for the conditional lifecycle liveness theorem.

`StartedEventuallyTerminal` is intentionally conditional: it does not claim
that the environment eventually submits or begins a job.  This direct witness
prevents the accepted liveness row from passing merely because `Started` was
unreachable in the finite model.
***************************************************************************)
EXTENDS LifecycleAuthority

NoStartedState == beginCount = 0

=============================================================================
