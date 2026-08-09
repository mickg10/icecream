# Protocol-neutral prerequisite final handoff

This marker freezes the public execution branch:

```text
bigoracle/protocol-prerequisites-formal
```

Fetch the branch, resolve its exact head, verify this marker at that commit, and
execute from a clean detached worktree. Do not execute by the moving branch
name.

## Sole authoritative domain input

```text
formal/run_prerequisite_checks_v2.py
formal/prerequisite-formal-checks-v4.json
formal/prerequisite_static_check_v2.py
formal/PREREQUISITES_EXECUTION.md
formal/PREREQUISITE_MODEL_MAP.md
formal/PREREQUISITES_FINAL_HANDOFF.md
```

The reusable proofless runner files inherited from
`bigoracle/formal-runner-v4` are part of the execution closure, but the domain
wrapper rejects every manifest path except
`formal/prerequisite-formal-checks-v4.json`.

## Matrix contract

Exactly 24 rows are ordered as:

```text
5  shared scheduler-visible job-ID checks
6  STARTED lifecycle authority checks
7  exact UseCS handoff/byte-cut checks
6  old fulfillment-session quiescence checks
```

A complete run yields exactly 48 TLC result records and 24 semantic
toolchain-comparison records. The manifest has `"proofs": []`; TLAPM and
backend arguments are neither required nor accepted by the authoritative
entry point.

The four UseCS cut witnesses are fixed-model reachability checks at byte counts
0, 1, 2, and 3 of a four-byte abstract frame. The abstraction distinguishes
product injection positions; it does not claim the wire frame is four bytes.

The old-session model separately represents the direct compiler child, its
process group, an unrelated writer child, compiler occupancy, and one absolute
cleanup deadline. It includes direct mutants for wait-any wrong-child reap,
ECHILD with a live descendant group, per-stage deadline reset, unexplained
residue reset, and early replacement-session advertisement.

## Required preflight

Follow `PREREQUISITES_EXECUTION.md`. Before either TLC jar starts, the static
checker must report PASS for all 24 rows and all direct Python self-tests must
return zero with an empty `git status --porcelain --untracked-files=all`.

The static checker rejects:

- a substituted manifest;
- any proof row;
- missing or reordered matrix IDs;
- a group count other than 5/6/7/6;
- a missing local `EXTENDS` module;
- incomplete or extra constant assignments;
- absent or multiple SPECIFICATION/deadlock policies;
- constraints, action constraints, symmetry, or VIEW reduction;
- an expected counterexample checking anything except its named property;
- missing finite-state reasoning;
- empty or inconsistent trace events, final predicates, metadata, or harness
  anchors; and
- stable/differential order other than stable then differential.

## Result discipline

No static, TLC, product, compatibility, performance, or cluster result is
claimed by this marker. The first executing role must post:

- the resolved exact branch SHA;
- no-tool preflight output;
- Java and both TLC banners/hashes;
- all 48 process exits, generated/distinct states, depths, elapsed times, and
  positive peak RSS values;
- raw and normalized trace hashes plus essential event/harness sequences;
- all 24 comparison records; and
- the first exact red row with retained artifacts if the run does not complete.

Only after this matrix is green should the emitted barriers be replayed through
the concrete seams in `PREREQUISITE_MODEL_MAP.md`. Missing deterministic seams
may be added as isolated test hooks. This handoff does not authorize any
PREPARE/READY/REVOKE protocol behavior.
