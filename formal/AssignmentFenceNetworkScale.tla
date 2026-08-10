-------------------- MODULE AssignmentFenceNetworkScale --------------------
(***************************************************************************
Scale-only mapping operators for targeted assignment-fence TLC campaigns.

This wrapper imports the accepted AssignmentFenceNetwork transition system
without modifying it.  The configurations deliberately avoid SYMMETRY, VIEW,
state constraints, and action constraints.  They exercise the smallest larger
supports that add real coverage:

  S1: three assignments contend for a capacity-two worker;
  S2: four assignments contend across two capacity-two workers;
  L1: three assignments/two workers under the existing live-link fairness.

The canonical proof/network acceptance head remains separate.  These mappings
are exploratory/enforcement-gate inputs until their exact runs are retained.
***************************************************************************)
EXTENDS AssignmentFenceNetwork

ScaleIndex(a) ==
    CASE a = "a0" -> 0
      [] a = "a1" -> 1
      [] a = "a2" -> 2
      [] a = "a3" -> 3
      [] OTHER -> 4

ScaleWireOf ==
    [a \in Assignments |-> ScaleIndex(a) + 1]

ScaleFullIdOf ==
    [a \in Assignments |-> ScaleIndex(a) + 101]

ScaleTokenOf ==
    [a \in Assignments |-> ScaleIndex(a) + 201]

(***************************************************************************
S1: the accepted network assumes at least two workers.  w1 is an idle control
worker; all three assignments intentionally contend on w0 at capacity two.
***************************************************************************)
S1Capacity ==
    [w \in Workers |-> IF w = "w0" THEN 2 ELSE 1]

S1WorkerOf ==
    [a \in Assignments |-> "w0"]

S1PolicyOf ==
    [a \in Assignments |-> IF a = "a0" THEN TokenRequired ELSE LegacyId]

(***************************************************************************
S2: two assignments per worker, capacity two on each, alternating policy.
***************************************************************************)
S2Capacity ==
    [w \in Workers |-> 2]

S2WorkerOf ==
    [a \in Assignments |->
        IF a \in {"a0", "a1"} THEN "w0" ELSE "w1"]

S2PolicyOf ==
    [a \in Assignments |->
        IF a \in {"a0", "a2"} THEN TokenRequired ELSE LegacyId]

(***************************************************************************
L1: a shared capacity-two worker plus one independent worker.  This is the
smallest larger liveness support with both contention and cross-worker work.
***************************************************************************)
L1Capacity ==
    [w \in Workers |-> IF w = "w0" THEN 2 ELSE 1]

L1WorkerOf ==
    [a \in Assignments |-> IF a \in {"a0", "a1"} THEN "w0" ELSE "w1"]

L1PolicyOf ==
    [a \in Assignments |-> IF a = "a0" THEN TokenRequired ELSE LegacyId]

=============================================================================
