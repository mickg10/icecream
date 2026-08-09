# Formal runner generation 4

This branch separates two execution contracts without weakening either one.

## Proof-bearing manifests

`run_formal_checks.py` / `run_formal_checks_v4.py` retain the full contract:

- exact clean git revision;
- artifact tree outside the checkout;
- two different pinned TLC jars;
- exact TLAPM hash;
- exact Isabelle, Zenon, Z3, and LS4 hashes and build identities;
- positive peak-RSS evidence;
- one TLC worker for authoritative liveness;
- direct named-property counterexamples with normalized trace manifests;
- semantic stable/differential agreement; and
- an all-obligations-proved TLAPS result.

## Proofless manifests

`run_tlc_only_checks_v4.py` is the common orchestration for matrices whose
manifest contains an explicit empty `proofs` array. It reuses the same v4 TLC,
trace, clean-tree, RSS, and differential code, but it has no TLAPM/backend
arguments and does not probe or accept dummy proof inventory.

The two domain wrappers reject manifest substitution:

```text
run_prerequisite_checks_v4.py
  prerequisite-formal-checks-v4.json
  prerequisite_static_check_v2.py

run_compatibility_checks_v4.py
  compatibility-formal-checks-v1.json
  compatibility_static_check.py
```

A domain branch supplies its exact manifest and static checker. The reusable
runner branch intentionally does not duplicate those model-specific inputs.

## Static preflight ordering

For a proofless run the order is:

1. resolve the exact wrapper-approved manifest path;
2. verify exact git identity and a clean checkout;
3. require an absent/empty artifact directory outside the checkout;
4. validate an explicit empty proof set and stable-then-differential ordering;
5. run the domain static checker;
6. run all shared trace/runner self-tests;
7. recheck the clean exact checkout;
8. hash and preflight both TLC jars and Java;
9. execute each selected row on stable then differential TLC; and
10. compare theorem-relevant state and trace evidence.

The domain static checker is responsible for the full local `EXTENDS` closure,
constant/config completeness, direct property selection, prohibited
constraints or symmetry, trace-manifest structure, and harness-step references.

## Direct no-tool tests

```sh
export PYTHONDONTWRITEBYTECODE=1
python3 formal/trace_to_harness_test.py
python3 formal/tlc_text_trace_test.py
python3 formal/normalize_tlc_trace_test.py
python3 formal/run_formal_checks_v4_test.py
python3 formal/run_tlc_only_checks_v4_test.py
```

These tests do not establish a TLC or TLAPS result. They validate orchestration,
parsers, backend identity handling, matrix contracts, precedence linting,
semantic differential comparison, and proofless argument separation.

## Evidence

Every proofless artifact directory contains:

```text
run-metadata.json
preflight/<static-check>.log
self-tests/*.log
<check>/<toolchain>/tlc.log
<check>/<toolchain>/time.txt
<check>/<toolchain>/result.json
<check>/<toolchain>/raw and normalized trace evidence when applicable
summary.json or failure.json
```

A timeout, parser/runtime exception, missing positive RSS, wrong named
violation, rejected trace, stable/differential disagreement, nonempty passing
queue, or unexpected checkout mutation remains red.
