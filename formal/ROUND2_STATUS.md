# Round 2 formal checkpoint status

Base: `c0e0406896f6cd7a487d885b58d3837a088393ba`
Working branch: `bigoracle/assignment-fence-core-round2`

This is a review candidate for `mickg10/local-oracle`, not accepted formal evidence. No TLC or TLAPS result is claimed here.

## Conserved-token correction

The core separates:

```text
schedulerReservation[a]  -- S still accounts for a
workerSlot[a]             -- F still consumes a physical slot for a
```

After F linearizes `REVOKED`, `workerSlot[a]` is false while `schedulerReservation[a]` remains true until S consumes the result. A one-token model cannot represent the safe interval between those two linearization points.

## Deterministic executable reference

```sh
python3 formal/assignment_fence_reference.py
python3 formal/assignment_fence_reference.py --json
```

Local construction run used Python 3.13 and produced:

```text
PASS                        AssignmentFenceCoreFixed (55 states)
EXPECTED_COUNTEREXAMPLE     P50MixedFleetMutant (11 states)
EXPECTED_COUNTEREXAMPLE     ReleaseClaimedMutant (21 states)
PASS                        P49DefaultDenyAfterCompaction (5 states)
EXPECTED_COUNTEREXAMPLE     P49FiniteTombstoneDefaultAllow (6 states)
PASS                        UseCSExactHandoff (4 states)
EXPECTED_COUNTEREXAMPLE     UseCSExactHandoffMutant (4 states)
PASS                        FSessionExactQuiescence (4 states)
EXPECTED_COUNTEREXAMPLE     FSessionWrongChildReap (4 states)
```

Source SHA-256 from that run:

```text
assignment_fence_reference.py  fec6759dd726e2bcfdc36633335abc26092699858822a1de10234f704eccd899
AssignmentFenceCore.tla        6eaf32e92fee37de9d82c5989e8b0f1a887ec9b272ef5de3f620ab5463c523bb
```

The reference checker is deliberately independent and finite. It is useful for trace names, mutation discrimination and C++ harness generation, but it does not satisfy issue #4's TLC/TLAPS requirement. `mickg10/local-oracle` must syntax-check the TLA+ module, run the fixed and mutant configurations on the required toolchains, and replace this status with retained tool output before the protocol design is accepted.
