# G4 lifecycle formal stage

Formal branch: `bigoracle/g4-minimal-formal`

This branch is formal-only. It adds no hosted workflow and changes no product
or integration fixture. The model must be rebased and rerun on the exact final
product/fixture head before any closure claim.

## Corrections over `c02399d`

The rewritten model addresses the local-oracle review in issue comment
`5260295110`:

1. reconnect is reachable through cleanup, client reset, reconnect, and a
   second scheduler generation;
2. ConfCS arrival (`ConfArrives`) is external and has no fairness assumption;
   daemon handling (`HandleConf`) is separate and weakly fair once enabled;
3. at most one local decision is `LocalBound` globally;
4. bind, delivery, begin, and normal completion require the active current
   session/request generation;
5. non-owned and send-failure paths initiate whole-session cleanup; the fixed
   cleanup terminalizes every live entry and releases every charged slot;
6. priority selection is over the actual eligible-client set and supports one
   to three clients; the wrong id-first policy is a one-premise mutant;
7. client/decision cardinalities and local capacity are configuration
   constants supporting 1--3 clients, 1--3 decisions, and 1--2 slots;
8. `check_g4_trace.py` is an executable model-to-product transition checker,
   not a prose-only map.

## Formal boundary

The model covers:

```text
scheduler login and activation
scheduler generation and reconnect
per-client request generation
count 0, accepted batch, and overflow rejection
remote, local, and NoCS decisions
stale-decision rejection
one bound local lane
local slot charge and release
JobBegin-before-LocalStarted
wrong/duplicate JobDone rejection
normal completion
session loss and whole-request cleanup
request close/reset
eligible-client priority selection
```

It deliberately does not assume compiler completion, network repair, ConfCS
arrival, or a permanently live client. The fixed liveness properties are only:

```text
legacy LoginAttempt eventually activates, because that transition is daemon-owned
arrived modern ConfCS is eventually handled, because handling is daemon-owned
pending session cleanup eventually releases all live entries and slots
```

`ConfArrives` and `Connect` have no fairness assumption.

## Invariants

`CoreSafety` contains:

```text
TypeOK
correct legacy/Conf activation evidence
expected <= MaxBatch
accepted <= expected
accepted equals the number of non-Absent entries
slot occupancy <= Capacity
at most one LocalBound entry
slot charge exactly matches LocalDelivered/LocalStarted
LocalStarted => BeginCommitted
BeginCommitted has Started/Terminal shape
Terminal iff terminal_count >= 1
terminal_count <= 1
accepted entries carry nonzero owner generation and request generation
live entries under an active session match both current generations
completed full token is never live
Idle requests own no counts or decisions
Closed requests own no live decisions or slots
clean Disconnected state owns no live decision or slot
selected local client is lexicographic min `(niceness, client_id)` of eligible clients
count 0 leaves request state empty
completion occurs only under active matching authority
```

## Local runner

No tool download occurs inside the runner.

```bash
export TLA2TOOLS_174=/absolute/path/tla2tools-1.7.4.jar
export TLA2TOOLS_180=/absolute/path/pinned-tla2tools-1.8.0.jar
make formal_stage
```

Equivalent direct invocation:

```bash
python3 formal/run_g4_formal.py \
  --jar 1.7.4="$TLA2TOOLS_174" \
  --jar 1.8.0="$TLA2TOOLS_180"
```

The runner verifies pinned SHA-256 digests, requires a clean exact Git commit,
runs SANY, writes every generated config, uses one TLC worker, retains complete
logs/time output/result JSON, requires nonzero state counts, and distinguishes
fixed passes from named mutant/witness violations.

Useful preflight commands that need no TLA jar:

```bash
python3 formal/run_g4_formal.py --check-only
python3 formal/run_g4_formal.py --list
python3 -m unittest -v formal/test_check_g4_trace.py
```

## Matrix

Default fixed rows:

```text
legacy: 1 client, 1 decision, 1 slot
modern: 1 client, 1 decision, 1 slot
modern: 2 clients, 2 decisions, 1 slot
```

An explicit larger row covers 3 clients, 3 decisions, and 2 slots and is
selected with `--include-large` after the small rows are stable.

Named negative/witness rows:

```text
second scheduler generation is reachable
legacy waits incorrectly for ConfCS
LocalStarted occurs before BeginCommitted
cleanup settles only the selected decision
batch accepts MaxBatch + 1
id-first selection replaces lexicographic selection
more than one local decision becomes bound
a stale generation mutates the ledger
completion occurs while disconnected
capacity guard uses <= rather than <
a duplicate terminal increments terminal_count
```

Small TLC completion is counterexample-search evidence. Topology-general safety
still needs the later inductive/TLAPS argument and the final model-to-code map.

## Transition-log conformance

`G4_TRACE_SCHEMA.md` defines a no-wire-change JSONL format.
`check_g4_trace.py` replays exact projected pre/action/post records and rejects
unknown actions/states, impossible edges, generation regressions, ledger
regressions, slot mismatch, second bound entry, capacity overflow, duplicate
terminal, partial cleanup, sequence gaps, dropped records, and digest failure.

The current checker has synthetic unit controls. Product trace emission and
replay of every deterministic integration scenario remain open until the
fixture is frozen and the exact final head is named.

## Evidence status

The model and runner are source-complete for review, but this environment does
not contain either pinned tla2tools jar and has no outbound DNS for downloading
them. Therefore no TLC/SANY result is claimed by this commit. The locally
executed evidence is limited to:

```text
runner static check / matrix generation
Python compilation
transition-checker unit controls
```

The next accepted formal evidence must come from the local runner with exact
jar digests and retained logs, followed by a rebase/rerun on the final product
head.
