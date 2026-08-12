# G4 formal preflight and first checkpoint

## Exact source

The formal child must have this immutable ancestor:

```text
d206c7a012b173bd5ad92b5da3ed5a92abcc316c
```

Only these paths may differ from that base:

```text
GNUmakefile
formal/G4Lifecycle.tla
formal/G4_MINIMAL_MODEL.md
formal/G4_PREFLIGHT.md
formal/G4_TRACE_SCHEMA.md
formal/check_g4_trace.py
formal/run_g4_formal.py
formal/test_check_g4_trace.py
```

The runner refuses actual SANY/TLC on a dirty tree, a non-descendant, or a branch that changes another path. It never downloads tools.

## Pinned tools

```text
TLC 1.7.4
path: /tanksmall/scratch/icecream-formal-tools/tlc-1.7.4/tla2tools.jar
sha256: 936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88

TLC 2026.07.31 prerelease
path: /tanksmall/scratch/icecream-formal-tools/tlc-1.8.0-pre/tla2tools.jar
sha256: e22f8ffb4bacdea0a871f444dd94fe5fb0d8013b3388ae39e82e26f852c735d5
```

The prerelease is not described as a final 1.8.0 distribution.

## Local source gates

```bash
make formal_trace_tests
make formal_falsify
make formal_preflight
```

Acceptance:

- `formal_trace_tests` reports exactly 16 tests and 16 passes;
- controls 04 and 05 reject for their named Begin-order and NoCS-lane reasons;
- falsification printouts report zero escaped mutants;
- preflight reports balanced delimiters, unique operator definitions, all required tokens, and no hosted workflow.

## First tool-bearing checkpoint

```bash
export TLA2TOOLS_174=/tanksmall/scratch/icecream-formal-tools/tlc-1.7.4/tla2tools.jar
export FORMAL_ARTIFACT_PARENT=/tanksmall/scratch/icecream-formal-results
export FORMAL_TMP_PARENT=/tanksmall/scratch/icecream-formal-tmp
make formal_stage
```

The target runs in this order:

1. 16 trace controls;
2. source preflight;
3. SANY with the digest-pinned 1.7.4 jar;
4. only `fixed-modern-c1d1-cap1` with one TLC worker.

Required result:

```text
SANY exit                         0
TLC exit                          0
generated states                  > 0
distinct states                   > 0
states left on queue              0
successful completion marker      present
CoreSafety                        intact
NoUnexpectedDeadlock              intact
```

The artifact root retains:

- exact generated config;
- exact command;
- SANY log;
- complete TLC log;
- `/usr/bin/time -v` output;
- state/depth/queue/time result JSON;
- formal source and tool digests;
- Git head, base, changed-path set, and clean status.

A failed checkpoint is posted as failed. It is never relabeled as partial success.

## Later matrix

The runner contains named fixed and one-premise mutant rows, but it deliberately does not run the matrix automatically before the first checkpoint is independently accepted.


The first TLC artifact also contains `source-hashes.json`, `commands.json`, the exact generated config and its digest, the verified jar digest, Java version, full SANY/TLC logs, `/usr/bin/time -v` output, parsed state/depth/queue counts, and peak RSS.
