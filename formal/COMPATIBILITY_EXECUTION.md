# Mixed-version compatibility execution

The authoritative entry point is:

```text
formal/run_compatibility_checks.py
```

It delegates to the published generation-4 proofless runner and accepts only:

```text
formal/compatibility-formal-checks-v1.json
```

The matrix contains 31 rows and an explicit empty `proofs` array. A complete
run produces 62 TLC result records and 31 semantic stable/differential
comparisons. It must not require or accept dummy TLAPM/backend inventory.

## Publication preflight

```sh
git fetch mickg10 refs/heads/bigoracle/compatibility-formal
sha=$(git rev-parse FETCH_HEAD)
git cat-file -e "$sha^{commit}"
git show "$sha:formal/COMPATIBILITY_FINAL_HANDOFF.md" >/dev/null
printf '%s\n' "$sha"
```

Create a clean detached worktree at the exact SHA. Do not execute by the moving
branch name.

## No-tool preflight

```sh
export PYTHONDONTWRITEBYTECODE=1
python3 formal/compatibility_codec_fixture_test.py
python3 formal/compatibility_static_check.py \
  --manifest formal/compatibility-formal-checks-v1.json \
  --repo . \
  --formal-dir formal
python3 formal/compatibility_static_check_test.py
python3 formal/trace_to_harness_test.py
python3 formal/tlc_text_trace_test.py
python3 formal/normalize_tlc_trace_test.py
python3 formal/run_formal_checks_v4_test.py
python3 formal/run_tlc_only_checks_v4_test.py
git status --porcelain --untracked-files=all
```

Every command must return zero and the final status must be empty. The static
checker enforces:

- exactly 31 ordered IDs split 6 fixed / 16 witness / 9 mutant;
- the six required topology labels;
- one worker and stable-then-differential TLC on every row;
- no mutant enabled in a fixed or fixed-model witness configuration;
- exactly one mutant enabled in each mutation configuration;
- explicit finite-state reasoning;
- exact local `EXTENDS` closure and complete constant assignments;
- exactly `SPECIFICATION Spec`, one direct invariant, and one explicit
  deadlock policy per config;
- no constraint, action constraint, symmetry, or VIEW reduction;
- explicit Legacy/FencedLegacy restart limitations rather than false exactness;
- nonempty discriminating events, final predicates, metadata/config ownership,
  and deterministic harness anchors for every counterexample; and
- all codec fixture tests.

## Pinned dual-TLC run

Use an absent or empty artifact directory outside the checkout:

```sh
python3 formal/run_compatibility_checks.py \
  --manifest formal/compatibility-formal-checks-v1.json \
  --repo . \
  --artifacts /absolute/path/outside/checkout/compatibility-$sha \
  --expected-git-sha "$sha" \
  --stable-jar /cache/tla2tools-1.7.4.jar \
  --stable-sha256 936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88 \
  --differential-jar /cache/tla2tools-1.8.0.jar \
  --differential-sha256 e22f8ffb4bacdea0a871f444dd94fe5fb0d8013b3388ae39e82e26f852c735d5
```

Tool paths may differ, but locally computed hashes must match the pinned
values. Stable 1.7.4 uses retained text traces; the differential build uses
native JSON when supported. Both traces are normalized and must produce the
same essential event and harness semantics.

## Acceptance result

A green run requires:

- 31 selected checks;
- 62 TLC process result records;
- 31 semantic stable/differential matches;
- complete state-space exhaustion for all six passing topology rows;
- the directly named property and a manifest-valid nonempty trace for all 25
  expected counterexample rows;
- positive peak RSS and retained command/log hashes for every TLC process;
- no source-tree mutation; and
- zero TLAPS results in this proofless layer.

Expected counterexamples may retain queue states because TLC stops at the
named violation. A passing topology row may not.

## Product codec and mixed-binary gate

The Python codec fixture is not the production serialization proof. After the
formal matrix is green, build exact old and candidate binaries and run the
real message classes through socketpairs and mixed processes. Required rows:

```text
S'FC
S'FC'
S'F'C
S'F'C'
S'F[F']C[C']
S'F'[CC']
```

For each row retain:

- actual negotiated versions on every channel;
- exact bytes and complete-frame consumption at the old boundary;
- proof that no old peer sees a new message or field;
- normal completion, cancellation, relevant revoke, submitter loss, worker
  loss, detach/completion, scheduler restart, delayed claim, and compaction;
- exact terminal and residual accounting;
- complete management responses and process exits; and
- throughput/latency comparison against the immediately preceding upstream
  series commit.

The two old-client restart rows are limitation witnesses, not failures of the
stated design. They prevent reports from claiming exact arbitrary-delay
restart fencing where old client identity cannot provide it. Only the Token
row claims and must demonstrate exact stale-restart rejection.

## Reporting

Post the exact detached SHA, no-tool output, Java/TLC banners and hashes, all 62
process exits, generated/distinct states, depths, elapsed times, positive peak
RSS values, raw/normalized trace hashes, essential events/harness steps, and
all 31 comparison records. On red, post the first exact failing row with its
retained config, static preflight, `result.json`, TLC log, and trace artifacts.
