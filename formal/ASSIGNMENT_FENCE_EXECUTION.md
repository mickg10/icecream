# Assignment-fence proof-bearing execution

## Authoritative input

Run only:

```text
formal/assignment-fence-formal-checks-v1.json
formal/assignment_fence_static_check.py
formal/assignment_fence_fifo_quotient_test.py
formal/run_assignment_fence_tlc_checks_v4.py
formal/run_assignment_fence_checks_v4.py
```

The matrix contains exactly 14 TLC rows in this order:

```text
5  abstract ownership-core rows
9  FIFO-network safety, witnesses, mutants, and bounded liveness rows
```

It also contains exactly one TLAPS row:

```text
core-tlaps-proof -> AssignmentFenceCoreProof.tla
```

A complete run therefore produces 28 TLC process records, 14 semantic
stable/differential comparisons, and one all-obligations-proved TLAPS result.

## No-tool preflight

From a clean detached worktree at the exact candidate SHA:

```sh
export PYTHONDONTWRITEBYTECODE=1
python3 formal/assignment_fence_static_check.py \
  --manifest formal/assignment-fence-formal-checks-v1.json \
  --repo . \
  --formal-dir formal
python3 formal/assignment_fence_static_check_test.py
python3 formal/assignment_fence_fifo_quotient_test.py
python3 formal/trace_to_harness_test.py
python3 formal/tlc_text_trace_test.py
python3 formal/normalize_tlc_trace_test.py
python3 formal/run_formal_checks_v4_test.py
python3 formal/run_tlc_only_checks_v4_test.py
python3 formal/manifest_contract_test.py
git status --porcelain --untracked-files=all
```

Every command must return zero and the final status must be empty.

## Pinned execution

Use the exact stable and differential TLC hashes already accepted by issue #4:

```text
TLC 1.7.4
936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88

TLC 1.8.0 build 2026.07.31.184830
e22f8ffb4bacdea0a871f444dd94fe5fb0d8013b3388ae39e82e26f852c735d5
```

Pin `tlapm`, Isabelle, Zenon, Z3, and LS4 by executable hash exactly as required
by `run_formal_checks_v4.py`; do not substitute placeholders. Then invoke:

```sh
python3 formal/run_assignment_fence_checks_v4.py \
  --manifest formal/assignment-fence-formal-checks-v1.json \
  --repo . \
  --artifacts /absolute/path/outside/checkout \
  --expected-git-sha "$(git rev-parse HEAD)" \
  --stable-jar /path/tla2tools-1.7.4.jar \
  --stable-sha256 936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88 \
  --differential-jar /path/tla2tools-1.8.0.jar \
  --differential-sha256 e22f8ffb4bacdea0a871f444dd94fe5fb0d8013b3388ae39e82e26f852c735d5 \
  --tlapm /path/tlapm \
  --tlapm-sha256 <sha256> \
  --backend isabelle=/path/isabelle=<sha256> \
  --backend zenon=/path/zenon=<sha256> \
  --backend z3=/path/z3=<sha256> \
  --backend ls4=/path/ls4=<sha256>
```

The preliminary dual-TLC phase may be run with
`run_assignment_fence_tlc_checks_v4.py`. It retains the exact proof row in its
metadata and states that TLAPS was not executed. It is not final acceptance.

`--skip-proofs` is rejected by the final domain wrapper.

## Acceptance discipline

- Every passing safety row must exhaust its reachable state space.
- Every expected counterexample must violate only its directly named property.
- Every counterexample must satisfy its event, final-state, and harness manifest
  on both toolchains.
- Stable and differential semantic summaries must match.
- Every liveness row runs with one TLC worker.
- TLAPS must report all obligations proved; partial proof is red.
- Artifacts stay outside the checkout and the exact SHA remains clean.

## Finite-state theory

The F→S abstraction and the exact dual-toolchain state counts are specified in
`ASSIGNMENT_FENCE_FIFO_QUOTIENT.md`. That document is the sole quotient note;
no absolute transport sequence number is part of the protocol theorem.
