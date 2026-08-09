# Mixed-version compatibility final handoff

This marker freezes the public execution branch:

```text
bigoracle/compatibility-formal
```

Fetch the branch, resolve its exact head, verify this marker at that commit, and
execute from a clean detached worktree. Do not execute by the moving branch
name.

## Sole authoritative input

```text
formal/MixedVersionCompatibility.tla
formal/MixedVersionCompatibilityChecked.tla
formal/compatibility-formal-checks-v1.json
formal/compatibility_static_check.py
formal/compatibility_codec_fixture.py
formal/compatibility_codec_fixture_test.py
formal/run_compatibility_checks.py
formal/COMPATIBILITY_MODEL_MAP.md
formal/COMPATIBILITY_EXECUTION.md
formal/COMPATIBILITY_FINAL_HANDOFF.md
formal/trace-manifests/Compatibility*.manifest.template.json
```

The reusable generation-4 proofless runner inherited from
`bigoracle/formal-runner-v4` is part of the execution closure. The domain
wrapper rejects every manifest except
`formal/compatibility-formal-checks-v1.json`.

## Matrix contract

Exactly 31 rows are ordered as:

```text
6   fixed topology safety checks
16  fixed-model policy/lifecycle/restart/projection witnesses
9   one-premise compatibility mutants
```

A complete run yields 62 TLC result records and 31 semantic
stable/differential comparison records. The manifest has `"proofs": []`; no
TLAPM or backend argument is required or accepted.

Fixed topologies:

```text
S'FC
S'F'C
S'F'C'
S'FC'
S'F[F']C[C']
S'F'[CC']
```

Per-assignment policies:

```text
old F             -> Legacy
new F + old C     -> FencedLegacy
new F + new C     -> Token
```

A new C assigned to old F projects to Legacy. A prior Token assignment cannot
change a later old-peer assignment.

## Guarantee boundary

The branch deliberately proves different guarantees for different identities:

- `Legacy`: frozen old behavior; no arbitrary-delay exact restart claim.
- `FencedLegacy`: exact new-worker fencing in the live epoch; old-client restart
  identity remains an explicit limitation.
- `Token`: exact full-id/nonce rejection of a prior-epoch delayed claim.

The two old-client restart rows are expected limitation witnesses, not silently
weakened passing theorems. Removing either row is a contract violation.

## Byte-level companion

The deterministic generated codec fixture requires:

- exact frozen old assignment bytes;
- byte-identical new-client Legacy projection to the old encoder;
- complete old and new socketpair round trips;
- rejection of token/full-id fields in old frames;
- rejection of partial prefixes, partial bodies, and trailing bytes;
- one complete frame consumed at a time from a stream; and
- rejection of new terminal shapes by an old decoder.

This fixture is not a substitute for the real C++ message classes. The final
product gate must map the same obligations to old and candidate binaries over
socketpairs and mixed processes.

## Required preflight and run

Follow `COMPATIBILITY_EXECUTION.md`. Before either TLC jar starts:

```sh
export PYTHONDONTWRITEBYTECODE=1
python3 formal/compatibility_codec_fixture_test.py
python3 formal/compatibility_static_check.py \
  --manifest formal/compatibility-formal-checks-v1.json \
  --repo . \
  --formal-dir formal
python3 formal/compatibility_static_check_test.py
```

Then run `formal/run_compatibility_checks.py` with both pinned TLC jars and an
empty artifact directory outside the checkout. Every passing row must exhaust
its state space. Every expected counterexample must fail its directly named
property and validate its essential trace/harness sequence on both toolchains.

## Result discipline

No static, TLC, production codec, mixed-binary, performance, or rollout result
is claimed by this marker. The first executing role must post:

- the resolved exact branch SHA;
- complete no-tool preflight output;
- Java and both TLC banners/hashes;
- all 62 process exits, generated/distinct states, depths, elapsed times, and
  positive peak RSS values;
- raw and normalized trace hashes plus all 25 essential event/harness results;
- all 31 semantic comparison records; and
- the first exact red row with retained artifacts if the run does not complete.

After the formal matrix is green, execute the real C++ codec and mixed-version
matrix in `COMPATIBILITY_MODEL_MAP.md`. No PREPARE/READY/REVOKE behavior is
authorized solely by publication of this handoff.
