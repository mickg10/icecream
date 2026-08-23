# S4 role artifacts: P43 vs trunk hash-bound runtime roots

Built on q3 (tt-quietbox3) inside the pinned container
`icecream/farm-node:ubuntu22-gcc11-boost174`. Binaries are not committed to
git; roots live on each host under `~/role-artifacts/` as CONTENT-ADDRESSED,
immutable directories (see "Content-addressed immutable roots" below),
published there from q3's tar on demand (see "Distribution").

**The machine-readable authority is `farmharness/role-manifests/p43.json`
and `farmharness/role-manifests/p50.json`.** This document is narrative
only. `farm.py`'s `preflight()`/`verify_role_version()`/`distribute()`/
`immutable_root()`/`launch_image()` all read the JSON, never this file --
if the two ever disagree, the JSON wins and this file is stale and should
be corrected.

**p50 is PROVISIONAL, not a final release authority.** It is a foundation
build of trunk at commit `43297d53`, used to exercise the P43-vs-trunk
mechanism end to end. Do not treat its hashes as "the" P50 release
identity, and do not promote it without a rebuild -- see
`role-manifests/p50.json`'s `rebuild_required` block for exactly what has
to converge first (S1b release-identity bump to 1.5.90, and S2 assignment
handoff) and what regenerating the manifest for a real release entails.

## Source provenance

| Set | Commit | Tag/description | Verification |
|---|---|---|---|
| p43 | `cd74801e0fa4e83e3ae254ca1d7fe98642f36b89` | tag `1.4`, "Update version to 1.4 for release" (2022-03-04) | `git fetch origin +refs/tags/1.4:refs/tags/p43-authority` resolved to this exact SHA; fetched fresh via `git archive` and extracted into `~/p43-build/source` on q3. Re-verified against this repo's own history: real commit, tag `1.4` points at it, reachable via `origin/1.4-branch`. |
| p50 | `43297d535232d8becb58866d5fb6cc73fa3b033d` | trunk foundation (**provisional**), "Add inert protocol-50 cache endpoint advertisement" | reused existing `~/ad-build` on q3 -- verified first via `git archive 43297d53... \| ssh q3 tar -x` into a scratch dir, then `diff -rq` against `~/ad-build/source` filtered for the (expected) one-sided autogen-generated extras: **0 differing/missing tracked files**. Re-verified: real commit, not an ancestor of this branch's own tip (expected -- separate development line), reachable via `origin/implementer/issue16-p50-cache-advertisement-provisional` and the `provisional/s1b-release-identity`/`provisional/s2-assignment-handoff` branches. |

## Build outcomes

**p43**: `autogen.sh` -> `configure` -> `make -j24`, all exit 0. No source or
build-recipe tweaks were needed (only benign `docbook2x is missing` warnings
for man-page generation, unrelated to the role executables). autogen.sh
emitted the expected AC_TRY_COMPILE/AC_PROG_LIBTOOL obsolescence warnings for
this AC_PREREQ([2.63])-era configure.ac -- warnings only, no errors.

**p50**: reused `~/ad-build` (built earlier, confirmed BUILD-EXIT=0 in its
own `build.log`) after the tree-diff verification above.

## The manifests (`farmharness/role-manifests/{p43,p50}.json`)

Each manifest binds, for its set: the exact source commit SHA and how it was
verified; the pinned container image and its `RepoDigest` (read live via
`docker image inspect` on q3, never hardcoded); the tar's path/sha256/size
and how it unpacks; and a `binaries` array covering every executable/script
in the root **plus the host-side `MANIFEST.tsv` itself** (so a tampered TSV
is caught the same way as a tampered binary) -- each entry has its relative
path, the role it serves (`S`/`F`/`C`/`null`), sha256, octal mode, byte
size, and (where one exists) the exact version-probe command and expected
output string. **The tar's sha256 is also what derives every host's runtime
path** -- see immediately below.

The tracked hashes (cross-checked by SSHing to q3 and hashing the real
files -- not copied from any prior report; zero mismatches found against
either this document or the on-disk `MANIFEST.tsv`):

```
p43/obj/scheduler/icecc-scheduler   c205c1347064503b3ab7c56af4584ffb0806f52e2c5786d343883db871c44525   ICECREAM scheduler 1.4.0
p43/obj/daemon/iceccd               62bcc05ad91461212cd9616d960905c97c839b316aac3a9e1e1080608a193441   ICECREAM daemon 1.4.0
p43/obj/client/icecc                c3dc54eadca303f21eaa1f90c265a38a1038378be08f887b24bb3bbea7dabeae   ICECC 1.4.0
p43/obj/client/icecc-create-env     aab94b6ea8f41335de807f814d24a56e827ce5efaf8b06797a365699e83b36cb   (script, no version marker)
p43/MANIFEST.tsv                    12e24679c29975a0092239b542986ca9acffd378e1f8378dcbd6b3d77c1160f8   n/a
p43-root.tar                        6da186b6c4c10f2f15d9e5440156ed890112523f53e905f9dd0d2f314540a3ae   12646400 bytes

p50/obj/scheduler/icecc-scheduler   6ccb91b0dab7596adf2344cedbd56f5eb606815410e85ad1b32cd56a24e78792   ICECREAM scheduler 1.4.92
p50/obj/daemon/iceccd               c0a1df52caee16e5b10d5101f89c4919d3b5df6e073993ed8b239b256ae9d88a   ICECREAM daemon 1.4.92
p50/obj/client/icecc                2c8691ea61c188b0b3d84808a9b577aa2c854b7c4ea132cd398353578bab6853   ICECC 1.4.92
p50/obj/client/icecc-create-env     ee7d30b240c38bccf66d4afcdd45993f115a01d4a2fb4e9143d38596609d2ba4   (script, no version marker)
p50/MANIFEST.tsv                    97e4a68d21db7c50981989dae9dc04232548b2772c71a4b0700ea3fec25fe53a   n/a
p50-root.tar                        c81e6f1f6f32e29a49b5c5623a5c91ccc3197eba160475993659b46d975450db   18442240 bytes
```

Pinned image (both sets): `icecream/farm-node:ubuntu22-gcc11-boost174` @
`sha256:bdb55d4287a473e3ebfbaa7715a50ee670659777278b8d84c350724e6fa8de58`.

Version probes: `icecc --version` and (for the scheduler, which rejects
`--version` as an unrecognized option in both eras but still emits its
startup banner on that path) the `ICECREAM scheduler ...` line from that
same invocation. `iceccd` has no `--version` flag in either era; its
identity is read via `strings obj/daemon/iceccd | grep 'ICECREAM daemon'`.
`icecc-create-env` is a shell script with no embedded version marker in
either era -- its sha256 is still the identity anchor.

All three linked-and-versioned executables' `ldd` output resolves entirely
to system libraries under `/lib/x86_64-linux-gnu/` in both sets -- nothing
else from the build tree is dlopened, so the four listed files plus
MANIFEST.tsv are the complete tracked root.

## Content-addressed immutable roots

Every host's runtime path for a set is `immutable_root(binary_set)` =
`~/role-artifacts/<set>-<tar.sha256>` -- e.g.
`~/role-artifacts/p43-6da186b6c4c10f2f15d9e5440156ed890112523f53e905f9dd0d2f314540a3ae`,
derived from the COMMITTED MANIFEST's own `tar.sha256`, never a fixed
per-set path. This means:

- Two different builds of the same set are always two different paths --
  nothing is ever edited or repaired inside a published directory, because
  a new build simply publishes under a new name (the manifest's tar hash
  changed) and the committed manifest is updated to point at it.
- A directory at a content-addressed path either already has EXACTLY the
  content its name asserts, or it does not exist -- this is verified on
  every access (`preflight()`, `publish_immutable_root()`), never just
  trusted from the name alone, since nothing at the OS level stops a
  determined actor with the same credentials from writing into it (see
  "read-only hardening" below for what this system DOES do about that).
- **An existing final name that fails verification is a hard, unrepaired
  failure.** `publish_immutable_root()` (what `distribute` calls) never
  edits an existing published directory -- if one exists and is wrong,
  that violates the one invariant content-addressing rests on (tampering,
  or a bug -- either way worth a human looking, never a silent auto-fix).
  The only valid recovery is explicit: `rm -rf` that exact path on that
  host, then re-run `distribute`, which republishes fresh under the SAME
  content-addressed name (verified live in the corrupt-then-refuse mutant
  test below).

**Publish mechanism** (`publish_immutable_root()`): extract a
manifest-verified copy of q3's tar into a FRESH TEMP SIBLING under
`~/role-artifacts/` (e.g. `<final-name>.tmp-<pid>-<ms>`), verify every
manifest entry against that temp copy, and only then atomically rename it
into the final immutable name with `mv -Tn` (a single `rename(2)` on the
same filesystem that refuses to clobber an existing destination).
**Verified live: GNU coreutils `mv -Tn` against an existing destination
exits 0 but performs no move** -- so this function always re-verifies the
FINAL name afterward rather than trusting the exit code; either publisher
racing to create the identical content ends up correct either way. On q3
itself the tar is extracted locally (q3 is the tar's source of record, so
no hub relay is needed there); on research6/research7/q2 the hub relays
the tar bytes from q3 (confirmed live: q3 cannot SSH directly to
research6 -- host-key verification fails -- so the hub, which reaches
every host, does the relay).

**Read-only hardening**: a successfully published tree is chmod'd
`a-w` (recursively) as a defense-in-depth signal that nothing should
write here again -- not the real guarantee (the owning user can always
`chmod` their own files back, same as any Unix permission; this is what
every mutant test below does to corrupt a published root on purpose), but
enough to make an ACCIDENTAL write from anywhere else fail loudly.
**Caught live while building this**: `chmod -R a-w` turns `755` into
`555` and `664` into `444` (write bit cleared per octal digit), which
initially made `verify_role_files()`'s own mode check permanently and
incorrectly redden every subsequent verification of a tree it had just
hardened. Fixed: the mode check now accepts EITHER the manifest's
recorded mode OR that mode with every write bit stripped (`_write_stripped()`)
-- a strictly SAFER state than the manifest asserts, never a more
permissive one, so this is not a weakening of the check.

`/work` and `/probe` bind mounts are now **READ-ONLY** (`:ro`) everywhere
a role's root is mounted into a container -- `docker_run_detached()` (the
real cluster launch), `run_client()`, and `verify_role_version()`'s probe.
Verified live, twice: an isolated `docker run --rm -v ...:ro` write attempt
and a write attempt from inside a REAL, launched, long-running container
both fail with "Read-only file system". The running scheduler/daemon/
client processes only ever READ their own binary from `/work`; all
logs/state go to the separate `/scratch` mount, which stays writable.

## Two-phase resolution: pure identity binding, then honestly-separate launch validation

Both oracles converged on the same finding from a different angle: the
prior `preflight(host, binary_set, role)` did root-presence + per-file
hash/mode + image digest *and* the role's version-probe `docker run --rm`
in one call, which meant a role that passed its own full check had
ALREADY launched a (harmless, `--rm`, non-cluster-mutating) container --
so a later role's refusal in the same `resolve_launch_plan()` pass could
be preceded by earlier roles' probe containers having genuinely run for
real. LO traced exactly this (2 containers before a late-F refusal),
which silently contradicted a "zero docker actions before refusal" claim.

Fixed by splitting into two honestly-labeled, separately-invoked functions:

- **`preflight(host, binary_set)`** -- PURE resolution: root presence,
  every manifest file's sha256 AND mode, the pinned image's live
  `RepoDigest`. Genuinely **zero `docker run`** (`docker image inspect` is
  a read-only metadata query, not a container launch), and no longer even
  role-specific (a host+set's root/image identity doesn't depend on which
  role will use it).
- **`verify_role_version(host, binary_set, role)`** -- LAUNCH-phase: runs
  the role's executable inside the pinned image, mounted `:ro` from the
  already-identity-verified root, and confirms the expected version
  string. This DOES invoke `docker run --rm` -- an honest, throwaway,
  non-cluster-mutating action, but a docker action nonetheless, so it is
  never folded into `preflight()`'s zero-docker-actions claim.

`resolve_launch_plan()` runs these as two full passes over the WHOLE plan
(S, every F, C), never interleaved per role:

1. **Pass 1** -- `resolve_role()` -> `preflight()` for every role, in
   order. Raises on the first failure; by construction nothing has
   happened for ANY role yet, including ones that already resolved.
2. **Pass 2** -- `verify_launch_version()` -> `verify_role_version()` for
   every role, in order, only reached once EVERY role in pass 1 succeeded.
   A role that fails here, after earlier roles' probes already ran, still
   leaves zero CLUSTER-mutating actions taken -- `up()`/`run_client()` are
   never reached either way; only the labeling of what already happened
   changed, from a false "resolution never touches docker" claim to an
   honest "resolution never touches docker; a separate, later,
   explicitly-launch-classified step does, and only after resolution
   fully succeeded."

`resolve_role()`/`preflight()` never publish anything. If the selected
set's root is absent (or fails verification) on a host and `distribute`
was never run there, `preflight` refuses with a message naming the exact
problem and suggesting the `distribute` command -- that refusal, not a
silent auto-fetch, is the fail-closed behavior the spec requires.

### Race gate: closing the window between resolution and the actual container start

Resolving a plan up front and only then mutating closes the ORDERING
defect (a bad later role can no longer follow an already-launched earlier
one) but, on its own, leaves a narrow window open: something could touch
the SELECTED immutable root between the moment resolution captured its
path and the moment `up()`/`run_client()` actually mount it. Two
independent defenses close this:

1. **Immutability itself**: `publish_immutable_root()` never edits an
   existing final name (see above) -- a concurrent, legitimate
   `distribute` call racing right after a resolution captured a path can
   only ever no-op (already-current) or publish a DIFFERENT new name; it
   structurally cannot touch the one already selected. Verified live:
   running `distribute` again immediately after a resolution leaves the
   resolved root byte-identical.
2. **`revalidate_before_mutation(host, binary_set)`**: called immediately
   before the FIRST cluster-mutating action for each role, inside
   `up()`/`run_client()` -- re-runs `preflight()` (cheap, zero docker
   actions of its own) one more time. This is what catches an
   OUT-OF-BAND tamper (something bypassing `distribute` entirely and
   writing directly into the immutable path) landing in the window: even
   though the root's NAME didn't change, its verified CONTENT is
   re-checked right before it would be trusted. Verified live: corrupting
   a real, published root in place (after chmod'ing it back writable --
   the owning user always can) makes `revalidate_before_mutation()`
   refuse by naming the exact file and hash mismatch, immediately before
   any container action; recovery is the same explicit `rm -rf` +
   re-`distribute` as any other content-addressing violation.

The race-gate test also launches ONE real, minimal, uniquely-named
container (never `farm-sched`/`farm-worker`/`farm-client`) with the
resolved immutable root mounted, and hashes the executable **from INSIDE
the running container** (`docker exec ... sha256sum`) against the
manifest -- the strongest available check, since it verifies what the
actual consuming process sees through the bind mount, not just what SSH
sees on the host side -- and separately confirms `:ro` is enforced from
inside that same real container, not only in an isolated probe. The
container is always torn down and its absence reverified before the test
exits.

## Distribution: idempotent, explicit, publish-only (never repair-in-place)

```
python3 farm.py distribute --sets p43,p50 --hosts q3,research6,research7,q2
```

The **only** code path allowed to write role-artifacts onto a host. It is
never called automatically by `up()`/`run_client()`/`resolve_role()`/
`revalidate_before_mutation()` -- bringing files onto a host is always a
deliberate, visible operator action, never a side effect of trying to
launch a cluster. **q3 is included, not skipped** -- it now also needs its
own content-addressed copy published from its local tar, exactly like
every other host, so no code path ever has to special-case where a root's
bytes actually live.

For each (set, host) pair it checks the content-addressed
`immutable_root(binary_set)` against the manifest. Already-published-and-
verified is a pure no-op (`already-current`; a second run of the command
above is verified idempotent). Absent is published fresh
(`published (root was absent)`) via the temp-sibling-extract-verify-
atomic-rename sequence above. **Existing-but-failing-verification is a
hard failure, on purpose** -- see "Content-addressed immutable roots"
above; `distribute` will never silently "fix" a corrupted immutable path,
only report it and point at the manual `rm -rf` + re-run recovery.

## Migration from the old mutable per-host layout

Before this revision, every host had a FIXED path per set
(`~/role-artifacts/{p43,p50}-root`) that `distribute` repaired in place on
drift. **No code path in this file resolves through that layout anymore**
-- `immutable_root()` always derives a content-addressed path from the
committed manifest's `tar.sha256`, and `role_tree()`/`preflight()`/
`resolve_role()`/`distribute` all go through it. The old fixed-path
directories (`LEGACY_MUTABLE_ROOT` in `farm.py`, kept only as a documented
historical reference) may still physically exist on q3/research6/research7/q2
as harmless orphaned data from before this migration -- nothing reads them,
and this task did not delete them (out of caution scope: no reason to touch
more host state than the new layout needs). The new content-addressed
layout has been published (via `distribute`) and verified on all 4 hosts
for both sets, alongside the old one.

**Known open gap, not fixed by this task (out of its stated scope):**
`research7`'s local Docker image under the tag
`icecream/farm-node:ubuntu22-gcc11-boost174` has a **different** content
digest (`sha256:fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b`,
empty `RepoDigests`) than the pinned one on q3/research6/q2. `preflight`
correctly and reproducibly refuses on research7 for both sets, all three
roles, by IMAGE DIGEST -- this is real, pre-existing drift discovered while
building this mechanism, not a synthetic test case, and is exactly the
class of problem gap #5 (mutable-tag hazard) exists to catch. Fixing it
(rebuild or re-pull the image identically on research7) needs a follow-up;
it remains out of this task's caution scope (no image changes on shared
hosts).

## Selection mechanism

`farmharness/farm.py` has `immutable_root(binary_set)` (content-addressed,
see above) and `role_tree(binary_set)` (`None` -> the original hardcoded
`TREE`, unchanged; otherwise delegates to `immutable_root()`). `main()`
exposes `--binary-set-S/-C/-F {p43,p50}`, each defaulting to `None` so
omitting all three reproduces prior behavior exactly (no manifest lookup,
no preflight -- nothing existing changes); it passes all three straight
into `resolve_launch_plan()` (see "Two-phase resolution" above) before
`up()` or `run_client()` ever run. `role_tree()`/`HOSTS`/`KNOWN_SETS`/
`preflight()`/`verify_role_version()`/`distribute()`/`resolve_launch_plan()`
are all safely importable without triggering a live deploy (`main()` stays
behind `if __name__ == "__main__":`).

## Verification gate (`farmharness/artifact_selection_test.sh`)

Runs, in order:

- **fresh-archive no-ambient gate:** `git archive HEAD -- farmharness`
  into a brand-new, otherwise-empty directory, `farm.py` imported from
  THAT location, both manifests loaded via `farm.load_manifest()` from
  there, schema-key presence checked, S/F/C role coverage confirmed, and
  `immutable_root()`'s derivation from `tar.sha256` confirmed -- the row
  that makes "committed code and committed manifests must actually
  agree" structurally impossible to miss again (the exact failure mode an
  earlier revision had: code read one path, manifests were committed at
  another, and every green run only worked because an untracked copy at
  the other path happened to be sitting in the shared worktree).
- a static AST anchor: `resolve_role()` called exactly 3x in
  `resolve_launch_plan()`, 0x in `up()`/`run_client()`;
  `verify_launch_version()` called exactly 3x in `resolve_launch_plan()`;
  `revalidate_before_mutation()` called exactly 2x in `up()` (S + the
  per-worker F call site), 1x in `run_client()` -- a secondary,
  source-level anchor; the primary proof is the behavioral rows below.
- **no-network TRANSPORT-SPY gates:** five scenarios driving `farm.main()`
  itself (the real CLI entry point) against a strict, manifest-derived
  FAKE `farm.sh()` (never a fake `preflight()`/`verify_role_version()` --
  those run for real, consuming the fake transport's responses, so what's
  actually under test is production code's ORCHESTRATION, not a
  hand-written stand-in). Four identity-level failures (invalid initial
  S; invalid second F of three, third never attempted; invalid C on a
  host distinct from S/F; BO's final-worker-of-three variant) each assert
  ZERO version-probe containers were ever reached (pass 1 aborts before
  pass 2 starts) and zero cluster-mutating actions. A fifth,
  probe-level-failure scenario reproduces LO's exact finding on purpose:
  every role passes identity (pass 1 completes, 0 docker actions), so
  pass 2 legitimately runs 2 real (faked) probe containers for the
  earlier roles before the 3rd role's probe fails -- asserted as exactly
  3 probe containers recorded, and STILL 0 cluster-mutating actions.
- `distribute` run twice against all 4 hosts for both sets -- first run
  publishes, second run is a verified no-op (`already-current` x8).
- **24-cell matrix:** all 4 hosts x 2 sets x 3 roles (24 cells) through
  PRODUCTION `preflight()` + `verify_role_version()` -- q3 included (not
  just the independent `probe()` helper used by the identity-baseline
  rows further down), with the exact expected outcome per real current
  host state asserted per cell (18 PASS on q3/research6/q2, 6 correctly
  FAIL-by-image-digest on research7).
- the research7 image-digest gap reproduced and asserted as a real, named
  refusal (not accepted as a false pass) -- redundant with one cell of the
  matrix above by design, kept as its own explicit, easy-to-find row.
- a same-version wrong-hash mutant, two ways: an isolated scratch copy
  (corrupt it, point `preflight()` at it via a monkeypatched
  `immutable_root()`, confirm refusal, discard the copy), and a real
  end-to-end cycle on research6's actual PUBLISHED immutable copy
  (corrupt one real file in place, confirm `preflight()` reddens naming
  that exact file, confirm `distribute` REFUSES to auto-repair it
  -- the content-addressing invariant, not a repair path -- confirm
  `preflight()` is still red, then perform the only valid recovery
  (explicit `rm -rf` + re-`distribute`), confirm green again, confirm a
  further `distribute` call is then a clean no-op). research6 is left
  fully hash-clean afterward, reverified.
- **the race gate** (see above): concurrent-`distribute` non-interference,
  out-of-band-tamper `revalidate_before_mutation()` refusal with explicit
  recovery, and in-container hash-vs-manifest identity plus `:ro`
  enforcement from inside a real launched container.
- a skipped-preflight mutant: `resolve_launch_plan()`'s source is
  temporarily edited to neutralize the scheduler-role `resolve_role()`
  call, and a mocked (docker/ssh-safe -- no real container is ever started
  or stopped; only real, read-only identity/probe checks pass through)
  dynamic invocation of `resolve_launch_plan()` + `up()` confirms the
  `PREFLIGHT-OK` marker for that role disappears (while the untouched
  worker role's marker still appears, proving the detection is precise)
  and that execution still reaches the launch actions completely
  unguarded. farm.py is restored byte-exact (`cmp`-verified) before the
  script continues, and the marker's return is reconfirmed afterward.
- the original 9 selection-mechanism assertions (identity baseline +
  selection-flip mutation, per role, per set -- unchanged; `:ro` added to
  the probe mount for consistency with production).

Run: `./artifact_selection_test.sh` (needs SSH reachability to q3,
research6, research7, q2; `ARTIFACT_TEST_HOST` overrides the host used for
the identity/mutant probes, default `q3`; never starts or stops the actual
farm-sched/farm-worker/farm-client cluster -- the race gate's one real
container uses a distinct, uniquely-named, always-torn-down identity).
