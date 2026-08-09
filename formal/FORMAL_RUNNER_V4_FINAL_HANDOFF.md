# Formal runner v4 final handoff

This marker freezes the reusable orchestration branch:

```text
bigoracle/formal-runner-v4
```

Resolve and record the exact branch head after fetching this marker. Execute by
the detached commit SHA, never by the moving branch name.

## Authoritative files

```text
formal/run_formal_checks_v4.py
formal/run_formal_checks_v4_test.py
formal/run_tlc_only_checks_v4.py
formal/run_tlc_only_checks_v4_test.py
formal/run_prerequisite_checks_v4.py
formal/run_compatibility_checks_v4.py
formal/FORMAL_RUNNER_V4.md
formal/FORMAL_RUNNER_V4_FINAL_HANDOFF.md
```

`run_formal_checks_v4.py` remains the proof-bearing path. The two domain
wrappers use `run_tlc_only_checks_v4.py`, require an exact domain manifest and
static checker, and deliberately expose no TLAPM or backend arguments when the
manifest has zero proof rows.

## Publication preflight

```sh
git fetch mickg10 refs/heads/bigoracle/formal-runner-v4
sha=$(git rev-parse FETCH_HEAD)
git cat-file -e "$sha^{commit}"
git show "$sha:formal/FORMAL_RUNNER_V4_FINAL_HANDOFF.md" >/dev/null
printf '%s\n' "$sha"
```

## Direct no-tool preflight

From a clean detached worktree at the resolved SHA:

```sh
export PYTHONDONTWRITEBYTECODE=1
python3 formal/trace_to_harness_test.py
python3 formal/tlc_text_trace_test.py
python3 formal/normalize_tlc_trace_test.py
python3 formal/run_formal_checks_v4_test.py
python3 formal/run_tlc_only_checks_v4_test.py
git status --porcelain --untracked-files=all
```

Every command must return zero and the final status must be empty. This is
runner evidence only. It is not a model, TLC, TLAPS, compatibility, product, or
performance result.

## Domain integration

A prerequisite branch must provide exactly:

```text
formal/prerequisite-formal-checks-v4.json
formal/prerequisite_static_check_v2.py
```

and invoke `run_prerequisite_checks_v4.py`.

A compatibility branch must provide exactly:

```text
formal/compatibility-formal-checks-v1.json
formal/compatibility_static_check.py
```

and invoke `run_compatibility_checks_v4.py`.

Each wrapper rejects a substituted manifest path, a nonempty proof set, a
different toolchain order, an in-checkout artifact directory, a dirty or wrong
revision, and an empty `--only` selection.

No formal result is claimed by this handoff. The first executing role must post
the resolved SHA, complete direct-test output, and the first exact failure if
any preflight is red.
