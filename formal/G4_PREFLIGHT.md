# G4 formal evidence status

This branch is formal-only. No hosted workflow result is used.

## Source preflight

The rewritten `G4Lifecycle.tla` was checked locally before publication for:

```text
module header/terminator
balanced delimiters and strings
presence of all ten one-premise mutant constants
presence of the fixed safety/fairness operators
presence of all lifecycle actions
all 29 next-state variables assigned or explicitly UNCHANGED in each action
15 generated matrix rows
```

That preflight passed on the model text published by this commit. It is not a
substitute for SANY.

## Executable trace controls

`formal/test_check_g4_trace.py` contains 16 controls covering:

```text
valid reconnect replay
unknown action
sequence gap
LocalStarted before BeginCommitted
stale owner generation
priority inversion
second LocalBound
capacity overflow
duplicate terminal
accepted-ledger regression
partial cleanup
non-stale identity mislabeled stale
begin-failure classification
bad footer digest
dropped records
JSONL command-line replay
```

The required exact-commit command is:

```bash
make formal_trace_tests
```

Its output must be retained with the later TLC artifact. No workflow badge or
pre-commit prototype result replaces that exact-commit run.

## TLC blocker in the publication environment

The publication environment has OpenJDK 21 but no pinned `tla2tools.jar` and no
usable shell DNS. The local runner intentionally does not download tools.
Therefore this commit claims no SANY or TLC result.

Required next command on a host with the pinned jars:

```bash
export TLA2TOOLS_174=/absolute/path/tla2tools-1.7.4.jar
export TLA2TOOLS_180=/absolute/path/tla2tools-1.8.0.jar
make formal_stage
```

The runner verifies the pinned SHA-256 digests, refuses a dirty worktree, uses
one TLC worker, and retains generated configs, complete output, state counts,
depth, queue-at-end, time/RSS output, and per-row result JSON.

## Acceptance status

```text
model source: published for review
local runner: published
transition checker/schema/tests: published
SANY: NOT RUN on exact commit
TLC: NOT RUN on exact commit
TLAPS: NOT YET PROVIDED
product trace emission: NOT YET PROVIDED
final-head rebase/replay: BLOCKED until final product/fixture head is named
```
