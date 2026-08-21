------------------- MODULE FSessionQuiescenceAbsolute -------------------
(***************************************************************************
The whole-session deadline is absolute. Reaping a direct child or beginning a
process-group absence check may not restart it. This strengthening is kept as
a named operator so the per-child-deadline mutant checks exactly the bound it
weakens.
***************************************************************************)
EXTENDS FSessionQuiescence

AbsoluteWholeSessionDeadline == totalTicks <= MaxDeadline

=============================================================================
