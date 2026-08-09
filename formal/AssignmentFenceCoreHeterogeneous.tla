----------------- MODULE AssignmentFenceCoreHeterogeneous -----------------
(***************************************************************************
Two-worker/capacity-heterogeneous cutoff for the abstract core.  The protocol
actions and invariants are inherited unchanged from AssignmentFenceCore; only
the finite model values differ.  This prevents acceptance from depending on a
one-worker collapse of WorkerOf/Capacity.
***************************************************************************)
EXTENDS AssignmentFenceCore

FirstWorker == CHOOSE w \in Workers : TRUE
OtherWorker == CHOOSE w \in Workers : w # FirstWorker

MCHeteroCapacity ==
    [w \in Workers |-> IF w = FirstWorker THEN 1 ELSE 2]

MCHeteroWorkerOf ==
    [a \in Assignments |-> IF a = "a0" THEN FirstWorker ELSE OtherWorker]

=============================================================================
