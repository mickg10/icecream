# S4 role artifacts: P43 vs trunk hash-bound runtime roots

Built on q3 (tt-quietbox3) inside the pinned container
`icecream/farm-node:ubuntu22-gcc11-boost174`. Binaries are not committed to
git; roots and tars live on q3 under `~/role-artifacts/` and are distributed
from there to other hosts on demand (see "Distribution" below).

**The machine-readable authority is `farmharness/role-manifests/p43.json`
and `farmharness/role-manifests/p50.json`.** This document is narrative
only. `farm.py`'s `preflight()`/`distribute()`/`launch_image()` all read the
JSON, never this file -- if the two ever disagree, the JSON wins and this
file is stale and should be corrected.

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
| p43 | `cd74801e0fa4e83e3ae254ca1d7fe98642f36b89` | tag `1.4`, "Update version to 1.4 for release" (2022-03-04) | `git fetch origin +refs/tags/1.4:refs/tags/p43-authority` resolved to this exact SHA; fetched fresh via `git archive` and extracted into `~/p43-build/source` on q3. Re-verified 2026-08-23 against this repo's own history: real commit, tag `1.4` points at it, reachable via `origin/1.4-branch`. |
| p50 | `43297d535232d8becb58866d5fb6cc73fa3b033d` | trunk foundation (**provisional**), "Add inert protocol-50 cache endpoint advertisement" | reused existing `~/ad-build` on q3 -- verified first via `git archive 43297d53... \| ssh q3 tar -x` into a scratch dir, then `diff -rq` against `~/ad-build/source` filtered for the (expected) one-sided autogen-generated extras: **0 differing/missing tracked files**. Re-verified 2026-08-23: real commit, not an ancestor of this branch's own tip (expected -- separate development line), reachable via `origin/implementer/issue16-p50-cache-advertisement-provisional` and the `provisional/s1b-release-identity`/`provisional/s2-assignment-handoff` branches. |

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
output string.

The current hashes (cross-checked 2026-08-23 by SSHing to q3 and hashing the
real files -- not copied from any prior report; zero mismatches found):

```
p43-root/obj/scheduler/icecc-scheduler   c205c1347064503b3ab7c56af4584ffb0806f52e2c5786d343883db871c44525   ICECREAM scheduler 1.4.0
p43-root/obj/daemon/iceccd               62bcc05ad91461212cd9616d960905c97c839b316aac3a9e1e1080608a193441   ICECREAM daemon 1.4.0
p43-root/obj/client/icecc                c3dc54eadca303f21eaa1f90c265a38a1038378be08f887b24bb3bbea7dabeae   ICECC 1.4.0
p43-root/obj/client/icecc-create-env     aab94b6ea8f41335de807f814d24a56e827ce5efaf8b06797a365699e83b36cb   (script, no version marker)
p43-root/MANIFEST.tsv                    12e24679c29975a0092239b542986ca9acffd378e1f8378dcbd6b3d77c1160f8   n/a
p43-root.tar                             6da186b6c4c10f2f15d9e5440156ed890112523f53e905f9dd0d2f314540a3ae   12646400 bytes

p50-root/obj/scheduler/icecc-scheduler   6ccb91b0dab7596adf2344cedbd56f5eb606815410e85ad1b32cd56a24e78792   ICECREAM scheduler 1.4.92
p50-root/obj/daemon/iceccd               c0a1df52caee16e5b10d5101f89c4919d3b5df6e073993ed8b239b256ae9d88a   ICECREAM daemon 1.4.92
p50-root/obj/client/icecc                2c8691ea61c188b0b3d84808a9b577aa2c854b7c4ea132cd398353578bab6853   ICECC 1.4.92
p50-root/obj/client/icecc-create-env     ee7d30b240c38bccf66d4afcdd45993f115a01d4a2fb4e9143d38596609d2ba4   (script, no version marker)
p50-root/MANIFEST.tsv                    97e4a68d21db7c50981989dae9dc04232548b2772c71a4b0700ea3fec25fe53a   n/a
p50-root.tar                             c81e6f1f6f32e29a49b5c5623a5c91ccc3197eba160475993659b46d975450db   18442240 bytes
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

## Preflight: fail-closed, in the real launch path

`farm.py` has a `preflight(host, binary_set, role)` that runs before ANY
docker action for a selected set, on every host that will run that role.
It checks, in order: the root directory is present on `host`; every
manifest-listed file's sha256 AND mode match exactly (root presence,
per-file hash/mode -- this is what a same-version-but-swapped-binary attack
fails on); the pinned image's live `RepoDigest` on `host` matches the
manifest; and the role's own binary reports the expected version string
when actually executed inside the pinned image. Any failure raises with a
precise, named reason and nothing is stopped or started.

`resolve_role()` calls `preflight()` and, on success, logs a
`PREFLIGHT-OK host=... role=... set=...` line (both to stdout and to a
hub-side launch log) before returning the resolved `/work` bind-source and
a **digest-pinned** `repo@sha256:...` image reference for the actual
`docker run` (not the mutable tag -- the tag stays only as the bootstrap
alias for `binary_set=None`, i.e. unselected/prior behavior, unchanged).

`resolve_role()` does **not** distribute anything itself. If the selected
set's root is absent (or wrong) on a host and `distribute` was never run
there, preflight refuses with a message naming the exact problem and
suggesting the `distribute` command to fix it -- that refusal, not a
silent auto-fetch, is the fail-closed behavior the spec requires.

### Preflight atomicity: resolve the COMPLETE plan before mutating ANYTHING

Both oracles held the mechanism above on one remaining gap: `resolve_role()`
being fail-closed per-role was not enough on its own, because `up()` used to
resolve the scheduler role and immediately launch it, then resolve+launch
each worker in the same loop -- a bad SECOND worker was only discovered
after the scheduler and first worker were already torn down and started for
real. `resolve_launch_plan(worker_hosts, binary_set_s, binary_set_f,
client_host, binary_set_c)` closes this: it resolves S, then every F host
(in order), then C (only when a client is actually going to be used), all in
one pass, and returns a `LaunchPlan` object -- `up()` and `run_client()` no
longer call `resolve_role()` at all; they only ever consume a plan this
function already validated in full. `main()` calls
`resolve_launch_plan()` once, before `up()`/`run_client()`/any mutating
primitive; only if it returns normally does a `down_needed` flag flip True,
and `main()`'s `finally` block now checks that flag -- a plan-validation
refusal never calls `down()` at all (previously, `main()`'s unconditional
`finally` fired four `docker rm -f` calls even when the very first preflight
refused, which could disturb a pre-existing, unrelated, exact-name running
cluster while simply declining to start a new one).

`up()`'s own docker/scratch mutations are factored into three named,
individually monkeypatchable primitives -- `docker_rm()`, a new
`docker_run_detached()` (the one `docker run -d --name ...` call site), and
a new `scratch_prepare()` (the one scratch mkdir/chmod/rm-log call site) --
alongside the pre-existing `push_file()`. `artifact_selection_test.sh` has
a no-network gate group that monkeypatches exactly these four functions
(plus `preflight()` itself, to inject a controlled refusal for one chosen
host/role without touching the network at all, and `sh()`, hard-failed if
ever called, as a belt-and-suspenders check that `up()`/`run_client()` are
truly never reached) and drives `farm.main()` itself -- the real CLI entry
point -- through four scenarios: an invalid initial S, an invalid second F
(of three, with a third F never even attempted), an invalid C (after S and
every F resolved cleanly), and BO's variant where the FINAL F (of three)
fails after all the others already resolved. Every scenario asserts the
recorded mutating-action list is empty. The static AST call-count check
(now `resolve_launch_plan():3`, `up():0`, `run_client():0`) is kept as a
secondary, source-level anchor only -- the primary proof is these
behavioral, no-network rows plus the pre-existing dynamic
skipped-preflight mutant test (which now mutates
`resolve_launch_plan()`'s S-role call site).

## Distribution: idempotent, explicit, repair-capable

```
python3 farm.py distribute --sets p43,p50 --hosts research6,research7,q2
```

The **only** code path allowed to write role-artifacts onto a host. It is
never called automatically by `up()`/`run_client()`/`resolve_role()` --
bringing files onto a host is always a deliberate, visible operator action,
never a side effect of trying to launch a cluster.

For each (set, host) pair it checks what's already there against the
manifest (same per-file sha256+mode check preflight uses). A host that
already matches exactly is left untouched and reported `already-current`
(a second run of the command above is a pure no-op, verified). Anything
else -- root absent, partial, or every-file-verified-wrong (drift,
corruption, tampering since the last run) -- is (re)synced from a
manifest-verified copy of q3's tar, relayed through the hub (q3 cannot SSH
directly to research6/research7/q2 on this network -- confirmed via
host-key-verification failure -- so the hub, which already reaches every
host, pulls the tar from q3 and pushes it to the target), and re-verified
file-by-file immediately after extraction. q3 itself is always skipped (it
is the source of record, not a distribution target).

This means `distribute` is genuinely a repair path, on purpose: since it
only ever runs on explicit operator invocation (never automatically), a
repair it performs is always a visible, intentional action -- never a
silent side effect that could launder tampering. `preflight` staying
separate, read-only, and non-repairing is what keeps both "distribute was
never run" and "distribute was run but the result is now wrong" fail-closed
on the automatic launch path; `distribute` existing is the remediation for
either, invoked by a human, not by `up()`.

## Selection mechanism

`farmharness/farm.py` has `ROLE_SET_DIR = {"p43": "~/role-artifacts/p43-root",
"p50": "~/role-artifacts/p50-root"}` and `role_tree(binary_set)`
(`None` -> the original hardcoded `TREE`, unchanged). `main()` exposes
`--binary-set-S/-C/-F {p43,p50}`, each defaulting to `None` so omitting all
three reproduces prior behavior exactly (no manifest lookup, no preflight
-- nothing existing changes); it passes all three straight into
`resolve_launch_plan()` (see "Preflight atomicity" above) before `up()`
or `run_client()` ever run. `role_tree()`/`HOSTS`/`ROLE_SET_DIR`/
`preflight()`/`distribute()`/`resolve_launch_plan()` are all safely
importable without triggering a live deploy (`main()` stays behind
`if __name__ == "__main__":`).

All four hosts (`q3`, `research6`, `research7`, `q2`) now have
`~/role-artifacts/` populated for both sets (q3 is the original source;
the other three were populated via `distribute`, verified idempotent).

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
it was out of this task's caution scope ("do not touch anything else on
those hosts" beyond role-artifacts and the sanctioned test mutations).

## Verification gate (`farmharness/artifact_selection_test.sh`)

Runs, in order:

- **fresh-archive no-ambient gate (FIRST row):** `git archive HEAD --
  farmharness` into a brand-new, otherwise-empty directory, `farm.py`
  imported from THAT location, both manifests loaded via `farm.load_manifest()`
  from there, schema-key presence checked, and S/F/C role coverage confirmed
  (every role `preflight()` probes for has a `binaries[]` entry with a
  non-empty `version_probe.command`, so a manifest gap can't silently make
  `preflight()` skip a role's identity check). This is the row that makes
  the whole "committed code and committed manifests must actually agree, no
  ambient/untracked file may be able to satisfy the test" defect class
  structurally impossible to miss -- exactly the failure mode `02622ab6` had
  (code read `role-manifests/`, manifests were committed at `artifacts/`,
  and every green run only worked because an untracked copy at the other
  path was still sitting in the shared worktree).
- a static AST check that `resolve_role()` is called exactly the expected
  number of times: 3 in `resolve_launch_plan()` (S + the per-worker F
  comprehension + C), 0 in `up()`, 0 in `run_client()` -- a secondary,
  source-level anchor; see "Preflight atomicity" above for the primary,
  behavioral proof;
- **no-network deletion-sensitive spy gates:** four scenarios (invalid
  initial S, invalid second F of three, invalid C, BO's final-worker-of-three
  variant), each driving `farm.main()` itself against a monkeypatched
  `preflight()` and asserting the recorded `docker_rm`/`docker_run_detached`/
  `scratch_prepare`/`push_file` action list is empty -- see "Preflight
  atomicity" above;
- `distribute` run twice against research6/research7/q2 for both sets --
  first run materializes, second run is a verified no-op;
- the full preflight matrix green on research6/q2 after distribution, and
  the research7 image-digest gap above reproduced and asserted as a real,
  named refusal (not accepted as a false pass);
- a same-version wrong-hash mutant, two ways: an isolated scratch copy
  (corrupt it, point preflight at it directly, confirm refusal, discard the
  copy), and a real end-to-end cycle on research6's actual distributed
  copy (corrupt one real file in place, confirm preflight reddens naming
  that exact file, re-run `distribute` and confirm it repairs and reports
  the repair, confirm preflight goes green again, confirm a further
  `distribute` call is then a clean no-op -- i.e. the repair itself is
  idempotent) -- research6 is left fully hash-clean afterward, reverified;
- a skipped-preflight mutant: `resolve_launch_plan()`'s source is
  temporarily edited to neutralize the scheduler-role `resolve_role()`
  call, and a mocked (docker/ssh-safe -- no real container is ever started
  or stopped) dynamic invocation of `resolve_launch_plan()` + `up()`
  confirms the `PREFLIGHT-OK` marker for that role disappears (while the
  untouched worker role's marker still appears, proving the detection is
  precise) and that execution still reaches the launch actions completely
  unguarded -- exactly the gap this check exists to catch. farm.py is
  restored byte-exact (`cmp`-verified) before the script continues, and the
  marker's return is reconfirmed afterward;
- the original 9 selection-mechanism assertions (identity baseline +
  selection-flip mutation, per role, per set -- unchanged).

Run: `./artifact_selection_test.sh` (needs SSH reachability to q3,
research6, research7, q2; `ARTIFACT_TEST_HOST` overrides the host used for
the identity/mutant probes, default `q3`; never starts or stops the actual
farm-sched/farm-worker/farm-client cluster).
