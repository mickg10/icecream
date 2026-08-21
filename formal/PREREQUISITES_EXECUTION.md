# Protocol-neutral prerequisite execution

The authoritative domain entry point is:

```text
formal/run_prerequisite_checks_v2.py
```

It delegates to the published generation-4 proofless runner and accepts only:

```text
formal/prerequisite-formal-checks-v4.json
```

The manifest contains 24 rows and an explicit empty `proofs` array. A complete
run therefore produces 48 TLC results and 24 stable/differential comparisons;
it must not require or accept dummy TLAPM/backend arguments.

## Publication preflight

```sh
git fetch mickg10 refs/heads/bigoracle/protocol-prerequisites-formal
sha=$(git rev-parse FETCH_HEAD)
git cat-file -e "$sha^{commit}"
git show "$sha:formal/PREREQUISITES_FINAL_HANDOFF.md" >/dev/null
printf '%s\n' "$sha"
```

Create a clean detached worktree at that exact SHA. Do not execute by the
moving branch name.

## No-tool preflight

```sh
export PYTHONDONTWRITEBYTECODE=1
python3 formal/prerequisite_static_check_v2.py \
  --manifest formal/prerequisite-formal-checks-v4.json \
  --repo . \
  --formal-dir formal
python3 formal/prerequisite_static_check_v2_test.py
python3 formal/trace_to_harness_test.py
python3 formal/tlc_text_trace_test.py
python3 formal/normalize_tlc_trace_test.py
python3 formal/run_formal_checks_v4_test.py
python3 formal/run_tlc_only_checks_v4_test.py
git status --porcelain --untracked-files=all
```

Every command must return zero and the final status must be empty. The static
checker verifies exactly 24 ordered IDs, the 5/6/7/6 group split, finite-state
arguments, local `EXTENDS` closure, complete constant instantiation, direct
property selection, explicit deadlock policy, absence of constraints/symmetry,
and discriminating trace/harness manifests.

## Pinned dual-TLC run

Use an absent or empty artifact directory outside the checkout:

```sh
python3 formal/run_prerequisite_checks_v2.py \
  --manifest formal/prerequisite-formal-checks-v4.json \
  --repo . \
  --artifacts /absolute/path/outside/checkout/prerequisites-$sha \
  --expected-git-sha "$sha" \
  --stable-jar /cache/tla2tools-1.7.4.jar \
  --stable-sha256 936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88 \
  --differential-jar /cache/tla2tools-1.8.0.jar \
  --differential-sha256 e22f8ffb4bacdea0a871f444dd94fe5fb0d8013b3388ae39e82e26f852c735d5
```

The paths may differ, but their locally computed hashes must match the pinned
values. The runner probes actual TLC capabilities; stable 1.7.4 uses retained
text traces and the differential toolchain uses native JSON when supported.

## Acceptance result

A green prerequisite run requires:

- 24 selected checks;
- 48 TLC result records;
- 24 semantic stable/differential matches;
- one worker for every row;
- complete state-space exhaustion for every passing row;
- the exact named property and a manifest-valid nonempty trace for every
  expected counterexample;
- positive peak RSS and retained commands/log hashes for every TLC process;
- no source-tree mutation; and
- no TLAPS result or proof-tool inventory in this proofless layer.

Expected counterexamples may retain unexplored queue states because TLC stops
at the direct violation. That is not accepted for a passing row.

## Product replay

After the formal matrix is green, map every emitted harness step through
`PREREQUISITE_MODEL_MAP.md`. Existing deterministic seams should be reused.
Missing seams must land as the smallest test-only hook commit before product
behavior changes. In particular:

- no sleep-only replacement for a model barrier;
- no incomplete management response interpreted as absence or zero;
- no protocol PREPARE/READY/REVOKE implementation in this prerequisite layer;
- no capacity advertisement before old compiler process groups are proven
  absent; and
- no local client alias substituted for a known exact scheduler assignment.

## Reporting

Post the exact detached SHA, commands, Java/TLC banners and jar hashes,
generated/distinct states, depth, elapsed time, positive peak RSS, process
exits, raw/normalized trace hashes, essential event subsequences, semantic
harness steps, and the final comparison count. On red, post the first exact
failing row and its retained `result.json`, `tlc.log`, trace, and preflight log.
