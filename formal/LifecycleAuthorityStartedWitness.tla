----------------- MODULE LifecycleAuthorityStartedWitness -----------------
(***************************************************************************
Reachability witness for the conditional lifecycle liveness theorem.

`StartedEventuallyTerminates` is intentionally conditional: it does not claim
that the environment eventually begins a job. This direct witness prevents the
accepted liveness row from passing merely because no execution reaches a
started assignment.
***************************************************************************)
EXTENDS LifecycleAuthority

NoStartedState == ~begunEver

=============================================================================
