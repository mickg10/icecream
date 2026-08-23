# S4 role artifacts: P43 vs trunk hash-bound runtime roots

Built on q3 (tt-quietbox3) inside the pinned container
`icecream/farm-node:ubuntu22-gcc11-boost174`. Binaries are not committed to
git; roots live on each host as CONTENT-ADDRESSED, immutable directories
under `$HOME/role-artifacts/store/<set>/<tar-sha256>/` (see "Content-
addressed immutable roots" below), published there from q3's tar on demand
(see "Distribution").

**The machine-readable authority is `farmharness/role-manifests/p43.json`
and `farmharness/role-manifests/p50.json`.** This document is narrative
only. `farm.py`'s `preflight()`/`distribute()`/`immutable_root()`/
`launch_image()` all read the JSON, never this file -- if the two ever
disagree, the JSON wins and this file is stale and should be corrected.

**p50 is PROVISIONAL, not a final release authority (unchanged by this
revision).** It is a foundation build of trunk at commit `43297d53`, used
to exercise the P43-vs-trunk mechanism end to end. Do not treat its hashes
as "the" P50 release identity, and do not promote it without a rebuild --
see `role-manifests/p50.json`'s `rebuild_required` block for exactly what
has to converge first (S1b release-identity bump to 1.5.90, and S2
assignment handoff) and what regenerating the manifest for a real release
entails; the eventual converged 1.5.90 set will be one designated build
published under its own content-addressed root, not a flip of this flag.

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
size. **The tar's sha256 is also what derives every host's runtime path.**

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

All three linked-and-versioned executables' `ldd` output resolves entirely
to system libraries under `/lib/x86_64-linux-gnu/` in both sets -- nothing
else from the build tree is dlopened, so the four listed files plus
MANIFEST.tsv are the complete tracked root.

## Content-addressed immutable roots

Every host's runtime path for a set is `immutable_root(binary_set)` =
`$HOME/role-artifacts/store/<set>/<tar-sha256>` -- e.g.
`$HOME/role-artifacts/store/p43/6da186b6c4c10f2f15d9e5440156ed890112523f53e905f9dd0d2f314540a3ae`,
derived from the COMMITTED MANIFEST's own `tar.sha256`, never a fixed or
mutable per-set path, never a symlink/alias. This means:

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

**Publish mechanism** (`publish_immutable_root()` / `_publish_script()`):
the ENTIRE stage -> verify -> bind sequence runs as ONE remote bash
script, under a **HOST-CANONICAL** `flock()` acquired via `exec 9>lockfile
&& flock -x -w120 9` held for the script's full duration -- not a
hub-local lock (which would only serialize invocations sharing one
checkout's lockfile path; a genuinely concurrent publisher from a
different checkout, or `distribute` invoked from cron, would not see
that). The script extracts a manifest-verified copy of q3's tar into a
FRESH TEMP SIBLING, verifies every manifest entry against that temp copy,
and only then atomically renames it into the final immutable name with
`mv -Tn` (a single `rename(2)` on the same filesystem that refuses to
clobber an existing destination). **Verified live: GNU coreutils `mv -Tn`
against an existing destination exits 0 but performs no move** -- the
script always re-verifies the FINAL name afterward rather than trusting
the exit/rename outcome. **Verified live under genuine concurrency**: two
`publish_immutable_root()` calls issued from separate Python threads,
overlapping in wall-clock time, against the SAME absent host+set --
exactly one performed the real extraction, the other correctly observed
`already-current` (rather than racing its own redundant extraction),
final state verified clean. On q3 itself the tar is extracted locally (q3
is the tar's source of record, no hub relay needed); on
research6/research7/q2 the hub relays the tar bytes from q3 as the
script's stdin (confirmed live: q3 cannot SSH directly to research6 --
host-key verification fails).

**A real, `$HOME`-vs-`~` bug caught and fixed while building this**: bash
does NOT tilde-expand `~` inside double quotes (only an UNQUOTED leading
`~` is expanded) -- an early version of the publish script assigned
`ROOT="~/role-artifacts/..."`, which silently created a directory
**literally named `~`** under the SSH session's cwd instead of resolving
to the home directory; every later step in that same script stayed
internally consistent with the wrong path, so the script still reported
success. Fixed by using `$HOME` (ordinary parameter expansion, which DOES
work inside double quotes) everywhere a path is assigned to a quoted
variable in the generated script -- matching the store-layout path
literally as `$HOME/role-artifacts/store/...`, which is why that exact
form is used rather than `~`.

**Read-only hardening**: a successfully published tree is chmod'd `a-w`
(recursively) as a defense-in-depth signal that nothing should write here
again -- not the real guarantee (the owning user can always `chmod` their
own files back, same as any Unix permission; every mutant test below does
exactly that to corrupt a published root on purpose), but enough to make
an ACCIDENTAL write from anywhere else fail loudly. **A second real bug
caught live**: `chmod -R a-w` turns `755` into `555` and `664` into `444`
(write bit cleared per octal digit), which initially made the mode check
permanently and incorrectly redden every subsequent verification of a
tree it had just hardened. Fixed: the mode check now accepts EITHER the
manifest's recorded mode OR that mode with every write bit stripped
(`_write_stripped()`) -- a strictly SAFER state than the manifest asserts,
never a more permissive one, so this is not a weakening of the check.

`/work` and `/probe` bind mounts are **READ-ONLY** (`:ro`) everywhere a
role's root is mounted into a container. Verified live, multiple ways: an
isolated `docker run --rm -v ...:ro` write attempt and a write attempt
from inside a REAL, long-running launched container both fail with "Read-
only file system"; a mutant that deliberately drops `:ro` (see "Race gate
mutants" below) correctly makes that same write SUCCEED, proving the
check has teeth in both directions.

**A third real bug caught live, this one in the TEST harness rather than
farm.py itself**: an early version of the dropped-`:ro` mutant test
mounted the REAL, PRODUCTION `immutable_root("p43")` on q3 (rather than
an isolated test copy) and, having deliberately dropped `:ro` to prove
the write succeeds, appended `"TAMPER\n"` (7 bytes) directly into q3's
real `obj/daemon/iceccd`. Root-caused via the corrupted file's exact
+7-byte size discrepancy (`4319447` vs the manifest's `4319440`,
`len("TAMPER\n") == 7`) and its very-recent mtime; confirmed no other
host/set was affected; repaired via the sanctioned `rm -rf` + re-publish
recovery, reverified clean. Fixed by publishing into a dedicated,
uniquely-prefixed isolated store (matching the pattern the other two race-
gate mutants already used correctly) before mounting it -- the test now
never touches a real production content-addressed root under any
circumstance, including its own deliberate-failure paths.

## Genuinely, entirely read-only resolution; identity verified from inside the actual launched container

Earlier in this successor's history, `preflight()` also ran a throwaway
`docker run --rm` version-probe container per role as part of resolution
-- LO traced this directly (2 probe containers could run for real before a
LATER role's refusal), which silently contradicted a "zero docker actions
before refusal" claim even though the probes never touched the cluster.

BO's stronger, simpler recommendation, adopted here: **the version-probe
containers are removed entirely.** An exact executable hash (already
checked, host-side, by `preflight()`) is a strictly stronger identity
proof than a probe's printed banner ever was. The residual value a probe
had -- confirming what a container ACTUALLY sees through its bind mount,
not just what SSH sees on the host side -- is now provided by
`verify_launched_container_identity()`, called from `up()`/`run_client()`
immediately after each REAL container starts: it hashes the role's own
tracked executable FROM INSIDE that running container (`docker exec ...
sha256sum`) and compares against the manifest, before that container's
registration/test output is trusted for anything. This is a genuinely
stronger check than the removed probes were (it verifies the SPECIFIC,
actually-in-use container instance, not a separate throwaway one mounted
the same way) and it eliminates the entire "is resolution honestly
docker-free" bookkeeping problem, because `preflight()` now has zero
`docker run` of any kind, full stop -- there is no second phase left to
be honest or dishonest about.

`resolve_launch_plan()` is accordingly back to ONE pass: `resolve_role()`
-> `preflight()` for S, every F, and C (when used), in order. Raises on
the first failure; by construction nothing has happened for ANY role at
that point, including ones that already resolved. `up()`/`run_client()`
never resolve or preflight anything themselves -- they only ever consume
an already-validated `LaunchPlan`.

### Race gate: closing the window between resolution and the actual container start

Resolving a plan up front and only then mutating closes the ORDERING
defect (a bad later role can no longer follow an already-launched earlier
one -- reproduced live via a deliberate mutant, see "Verification gate"
below) but, on its own, leaves a narrow window open: something could
touch the SELECTED immutable root between the moment resolution captured
its path and the moment `up()`/`run_client()` actually mount it. Two
independent defenses close this:

1. **Immutability itself**: `publish_immutable_root()` never edits an
   existing final name -- a concurrent, legitimate `distribute` call
   racing right after a resolution captured a path can only ever no-op
   (already-current) or publish a DIFFERENT new name; it structurally
   cannot touch the one already selected. Verified live, twice: a
   concurrent `distribute` immediately after resolution leaves the
   resolved root byte-identical; a "wrong-hash publication attempt"
   (calling `publish_immutable_root()` against a manifest tampered to
   disagree about what should already be at the plan's own resolved path)
   correctly refuses as a read-only verification failure, never a write.
2. **`revalidate_before_mutation(host, binary_set)`**: called immediately
   before the FIRST cluster-mutating action for each role, inside
   `up()`/`run_client()` -- re-runs `preflight()` (cheap, zero docker
   actions). This is what catches an OUT-OF-BAND tamper (something
   bypassing `distribute` entirely and writing directly into the
   immutable path) landing in the window: even though the root's NAME
   didn't change, its verified CONTENT is re-checked right before it
   would be trusted. Verified live: corrupting a real, published root in
   place (after chmod'ing it back writable -- the owning user always can)
   makes `revalidate_before_mutation()` refuse by naming the exact file
   and hash mismatch, immediately before any container action.

The race-gate test uses an ISOLATED test store (a distinctly-prefixed
path, still content-addressed by the SAME real hash) so the REAL
`publish_immutable_root()`/`preflight()`/`resolve_role()`/
`docker_run_detached()`/`verify_launched_container_identity()` functions
run entirely unmodified against it -- only the path prefix is test-owned,
never the functions. It launches ONE real, minimal, uniquely-named
container (never `farm-sched`/`farm-worker`/`farm-client`) and hashes the
executable **from INSIDE** it against the manifest -- the strongest
available check, since it verifies what the actual consuming process
sees through the bind mount. The container is always torn down and its
absence reverified before the test exits.

**Race-gate mutants** (each must redden a NAMED check, isolated stores
only -- see the bug write-up above for why "isolated" is non-negotiable
here):

- **rm-rf-final+extract-in-place instead of atomic rename**: simulates
  the UNSAFE pattern `publish_immutable_root()` does NOT use (rm -rf the
  final name, extract straight into it, no temp sibling, no pre-rename
  verification) by feeding a deliberately truncated tar directly into an
  isolated final path -- `preflight()` correctly reddens against the
  resulting broken intermediate state, which the real temp-sibling +
  atomic-rename design structurally never exposes (only ever-fully-
  verified content appears at a final name).
- **mutable-alias resolution instead of content-addressed**: a fixed
  (non-hash-derived) path resolver is proven, structurally, to always
  return the SAME name regardless of the manifest's tar hash -- exactly
  the collision hazard a fresh path per content hash exists to prevent
  (two different builds could not otherwise avoid colliding at one name).
- **dropped `:ro`**: with the mount forced writable, a write through it
  that must fail (and does, on unmutated code) now succeeds -- run only
  against the isolated store described above.

## Distribution: idempotent, explicit, publish-only (never repair-in-place)

```
python3 farm.py distribute --sets p43,p50 --hosts q3,research6,research7,q2
```

The **only** code path allowed to write role-artifacts onto a host. It is
never called automatically by `up()`/`run_client()`/`resolve_role()`/
`revalidate_before_mutation()` -- bringing files onto a host is always a
deliberate, visible operator action, never a side effect of trying to
launch a cluster. **q3 is included, not skipped** -- it also needs its own
content-addressed copy published from its local tar, exactly like every
other host, so no code path ever has to special-case where a root's bytes
actually live.

For each (set, host) pair it checks the content-addressed
`immutable_root(binary_set)` against the manifest, under the host-
canonical lock described above. Already-published-and-verified is a pure
no-op (`already-current`; a second run of the command above is verified
idempotent). Absent is published fresh (`published (root was absent)`).
**Existing-but-failing-verification is a hard failure, on purpose** -- see
above; `distribute` will never silently "fix" a corrupted immutable path,
only report it and point at the manual `rm -rf` + re-run recovery
(verified live, real corrupt-then-refuse-then-explicit-recovery cycle on
research6).

## Migration from the old mutable per-host layout

Before this successor, every host had a FIXED path per set
(`~/role-artifacts/{p43,p50}-root`) that `distribute` repaired in place on
drift; a subsequent revision moved to a FLAT content-addressed layout
(`~/role-artifacts/<set>-<hash>`) before landing on the current NESTED
`$HOME/role-artifacts/store/<set>/<hash>` layout (matching the exact form
BO specified). **No code path in this file resolves through either older
layout anymore** -- `immutable_root()` always derives the current store
path from the committed manifest's `tar.sha256`. The old directories
(`LEGACY_MUTABLE_ROOT` in `farm.py`, kept only as a documented historical
reference; the intermediate flat layout is undocumented in code, since
nothing ever referenced it as a constant) may still physically exist on
q3/research6/research7/q2 as harmless orphaned data from before each
migration -- nothing reads them, and this task did not delete them (out
of caution scope: no reason to touch more host state than the current
layout needs). The current layout has been published (via `distribute`)
and verified on q3/research6/q2 for both sets, confirmed idempotent on a
second run.

**Known open gaps on research7, not fixed by this task (out of its stated
scope), independently blocking it:**

1. **Image digest** (unchanged from prior revisions): the pinned tag
   `icecream/farm-node:ubuntu22-gcc11-boost174` resolves to a DIFFERENT
   content digest on research7 (`sha256:fe001a6138f017608b8846b43bf268a76a9d7a5b66c3364ba3f881da2ff0c54b`,
   empty `RepoDigests`) than the pinned one on q3/research6/q2.
2. **Disk space** (newly discovered while building THIS revision):
   research7's root filesystem is essentially full (observed as low as
   ~1.5MB free of ~56GB), independently preventing extraction of the new
   store layout there even once gap 1 is fixed.

`preflight` correctly and reproducibly refuses on research7 for both
sets, all three roles -- by image digest where the root happens to be
absent for gap-2 reasons too (both are real, live-confirmed, not
synthetic). Fixing either (rebuild/re-pull the image identically; free
disk space) needs a follow-up outside this task's caution scope (no image
changes, no disk cleanup on shared hosts).

## Selection mechanism

`farmharness/farm.py` has `immutable_root(binary_set)` (content-addressed,
see above) and `role_tree(binary_set)` (`None` -> the original hardcoded
`TREE`, unchanged; otherwise delegates to `immutable_root()`). `main()`
exposes `--binary-set-S/-C/-F {p43,p50}`, each defaulting to `None` so
omitting all three reproduces prior behavior exactly (no manifest lookup,
no preflight -- nothing existing changes); it passes all three straight
into `resolve_launch_plan()` (see above) before `up()` or `run_client()`
ever run. `role_tree()`/`HOSTS`/`KNOWN_SETS`/`preflight()`/`distribute()`/
`resolve_launch_plan()` are all safely importable without triggering a
live deploy (`main()` stays behind `if __name__ == "__main__":`).

## Verification gate (`farmharness/artifact_selection_test.sh`)

Runs, in order:

- **fresh-archive no-ambient gate:** `git archive HEAD -- farmharness`
  into a brand-new, otherwise-empty directory, `farm.py` imported from
  THAT location, both manifests loaded via `farm.load_manifest()` from
  there, schema-key presence checked, S/F/C role coverage confirmed, and
  `immutable_root()`'s store-layout derivation from `tar.sha256`
  confirmed -- makes "committed code and committed manifests must
  actually agree" structurally impossible to miss.
- a static AST anchor: `resolve_role()` called exactly 3x in
  `resolve_launch_plan()`, 0x in `up()`/`run_client()`;
  `revalidate_before_mutation()` exactly 2x in `up()` (S + the per-worker
  F call site), 1x in `run_client()`; `verify_launched_container_identity()`
  exactly 2x in `up()`, 1x in `run_client()`; `docker_run_detached()`
  exactly 1x in `run_client()` (the client is now started detached, like
  the scheduler/worker, not as a one-shot `--rm` container, so the
  in-container identity check can run before any test evidence is
  trusted) -- a secondary, source-level anchor; the primary proof is the
  behavioral rows below.
- **no-network TRANSPORT-SPY gates:** four scenarios driving
  `farm.main()` itself (the real CLI entry point) against a strict,
  manifest-derived FAKE `farm.sh()` (never a fake `preflight()` -- that
  runs for real, consuming the fake transport's responses, so what's
  under test is production code's ORCHESTRATION, not a hand-written
  stand-in): invalid initial S; invalid second F of three (third never
  attempted); invalid C on a host distinct from S/F; BO's final-worker-
  of-three variant. Every scenario now expects exactly 0 actions of ANY
  kind (no separate probe-container dimension left to track, since
  `preflight()` alone is the entire resolution phase).
- **ordering-violation mutant**: `resolve_launch_plan()`'s F-role
  resolution is deleted and `up()` is mutated to resolve each worker
  interleaved with its own launch (reintroducing the exact 02622ab6
  defect) -- proves the transport-spy harness's "0 actions" claim is a
  real, failable property: under the mutation, research6's real docker
  actions are recorded as having happened before research7's later
  resolution failure is even discovered (11 actions recorded, including
  the `finally`-block teardown), matching BO's explicit ask that deleting
  the pure-validation ordering must make the action list nonempty.
- `distribute` run twice against all 4 hosts for both sets -- q3/
  research6/q2 publish then report `already-current`; research7 fails
  both times for its two independent, real, live-reconfirmed gaps.
- a genuinely concurrent host-canonical-lock test: two
  `publish_immutable_root()` calls from separate threads, proven to
  overlap in wall-clock time, against the same absent host+set -- exactly
  one publishes, the other correctly observes `already-current`, final
  state verified clean.
- **24-cell matrix:** all 4 hosts x 2 sets x 3 roles (24 cells) through
  PRODUCTION `preflight()`, one row per cell recording the manifest
  source SHA, the resolved immutable root, the pinned image digest, the
  role's tracked file path/sha256/mode, and PASS/refusal reason (18 PASS
  on q3/research6/q2, 6 correctly FAIL on research7). "Banner probes are
  not cells" -- there is no probe dimension left to matrix over.
- research7's two gaps reconfirmed live and explicitly explained (not
  accepted as a false pass, and not conflated with each other).
- a same-version wrong-hash mutant, two ways: an isolated scratch copy
  (corrupt it, point `preflight()` at it via a monkeypatched
  `immutable_root()`, confirm refusal, discard the copy), and a real
  end-to-end cycle on research6's actual PUBLISHED immutable copy
  (corrupt one real file in place, confirm `preflight()` reddens naming
  that exact file, confirm `distribute` REFUSES to auto-repair it, confirm
  `preflight()` is still red, then perform the only valid recovery
  (explicit `rm -rf` + re-`distribute`), confirm green again, confirm a
  further `distribute` call is then a clean no-op). research6 is left
  fully hash-clean afterward, reverified.
- **the race gate**, isolated test store (see above): concurrent-
  `distribute` non-interference, wrong-hash-publication-attempt refusal,
  in-container identity confirmation on a real launch, and the three
  named mutants above.
- **a real end-to-end launch**: `resolve_launch_plan()` -> `up()` on a
  single host (q3, as both scheduler and worker) -> in-container identity
  verification wired into the real path -> successful registration ->
  `down()`, with guaranteed teardown and post-teardown container-absence
  reverification regardless of outcome.
- a skipped-preflight mutant: `resolve_launch_plan()`'s source is
  temporarily edited to neutralize the scheduler-role `resolve_role()`
  call, and a mocked (docker/ssh-safe -- no real container is ever
  started or stopped; real, read-only identity checks pass through)
  dynamic invocation of `resolve_launch_plan()` + `up()` confirms the
  `PREFLIGHT-OK` marker for that role disappears (while the untouched
  worker role's marker still appears) and that execution still reaches
  the launch actions completely unguarded. farm.py is restored byte-exact
  (`cmp`-verified) before the script continues, and the marker's return
  is reconfirmed afterward.
- the original 9 selection-mechanism assertions (identity baseline +
  selection-flip mutation, per role, per set -- unchanged; `:ro` on the
  probe mount for consistency with production).

Every source mutation in this file (ordering-violation, dropped-`:ro`,
skipped-preflight) uses the same pattern: snapshot farm.py, mutate,
verify the redden, restore via `cp`, verify byte-exact restoration via
`cmp -s`, all guarded by a shell `trap` so a mid-test failure still
restores the file. Every host-side mutation (variant B, the race gate,
its mutants) either targets an isolated, uniquely-prefixed test path that
is torn down regardless of outcome, or -- where it deliberately targets
real shared data (variant B, on research6) -- has its own explicit,
verified recovery built into the same test, and is followed by a
dedicated post-mutant repair-verification row before the script
continues into any other section that assumes a clean host.

Run: `./artifact_selection_test.sh` (needs SSH reachability to q3,
research6, research7, q2; `ARTIFACT_TEST_HOST` overrides the host used for
the identity/mutant probes, default `q3`; never starts or stops the
actual farm-sched/farm-worker/farm-client cluster except the one narrow,
guaranteed-torn-down real end-to-end launch test, and the race gate's
uniquely-named, always-torn-down containers).
