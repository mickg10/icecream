# S4 role artifacts: P43 vs trunk hash-bound runtime roots

Built on q3 (tt-quietbox3) inside the pinned container
`icecream/farm-node:ubuntu22-gcc11-boost174`. Binaries are not committed to
git; roots live on each host as CONTENT-ADDRESSED, immutable directories
under `$HOME/role-artifacts/store/<set>/<tar-sha256>/` (see "Content-
addressed immutable roots" below), published there from q3's tar on demand
(see "Distribution").

**This revision (round 4) closes the S4 selection/publication MECHANISM
checkpoint** -- a zero-mutation whole-plan barrier, a mutation-started
tracker replacing the old pre-emptive teardown flag, in-command
"attestation" superseding the post-start in-container check, a rewritten
8-step locked publication transaction, a genuinely concurrent
production-`main()`-driven race gate, hash-pinned harness-script
sourcing from the checkout (never an ambient path), and a strengthened
cell-verdict combination in `main()`/`dump_worker_evidence()` that closes
a false-green hole (a cell whose client script failed, or whose JOIN
bijection was broken, used to still exit 0). Also, as part of this same
round: research7's real image-digest and disk-space gaps (plus a
transient docker-socket-permission window found live while verifying the
fix) are now fixed and confirmed live. **Final S4 exit still waits on the
converged 1.5.90 rebuild** (see "p50 is PROVISIONAL" immediately below,
unchanged by this revision).

**Explicit scope boundary (BO, final-S4-exit note):** this round closes
the immutable selection/publication MECHANISM checkpoint only; the
plan-v11 identity-ledger join and the default-caret companion harness
remain final-S4-exit work, deliberately not carried here. The
cell-verdict fix above (exact cardinality/set-equality bijection over
the CURRENT JOINROW key) is correct and required for THIS checkpoint --
BO's final-S4-exit note explicitly builds on it -- but it is not itself
the final bijection authority: BO's point is that a JobID-only JOINROW
key plus a workaround-forced replay cannot serve as the eight-cell
bijection authority that final S4 exit needs; that authority is a
separate, not-yet-built row key and companion harness.

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
  determined actor with the same credentials from writing into it.
- **An existing final name that fails verification is a hard, unrepaired
  failure.** `publish_immutable_root()` (what `distribute` calls) never
  edits an existing published directory -- if one exists and is wrong,
  that violates the one invariant content-addressing rests on. The only
  valid recovery is explicit: `rm -rf` that exact path on that host, then
  re-run `distribute`, which republishes fresh under the SAME
  content-addressed name.

## Publish mechanism: the 8-step locked transaction (`publish_immutable_root()` / `_publish_script()`)

Round 3's publish script has been rewritten to close every gap LO/BO
flagged on it. The ENTIRE stage -> verify -> harden -> bind sequence still
runs as ONE remote bash script, but now under a **SINGLE CANONICAL** lock
-- `$HOME/role-artifacts/.publish.lock`, one file per HOST, not one per
`binary_set` (round 3's `_publish_script()` locked per-set; a p43 publish
and a p50 publish on the same host could still race each other under that
design -- fixed to a single well-known, auditable lock path that
serializes ALL publications on a host, since publish transactions are
fast/rare enough that the extra concurrency a per-set lock would allow
never actually matters). Acquired via `exec 9>lockfile && flock -x -w120
9`, held for the script's full duration.

Inside the lock, in order:

1. **Hash the source tar and compare to `manifest.tar.sha256` -- on EVERY
   host, including q3.** Round 3 only did this on the hub-relay path for
   non-q3 hosts; q3 extracted its own local tar unchecked. Now uniform:
   for non-q3 hosts, the hub-relayed tar bytes are uploaded to a UNIQUE
   incoming filename BEFORE the lock is ever acquired (a plain file
   write, nothing to protect there); the locked transaction then consumes
   that local file exactly like q3 consumes its own resident tar, so the
   in-lock script is byte-identical on every host.
2. If the final name already exists: verify it (full inventory + hash +
   HARDENED mode) and exit -- already-current if clean, a hard unrepaired
   failure if not. Never proceeds past here in that case.
3. Extract into a FRESH TEMP SIBLING (never the final name directly), via
   `tar --same-permissions -xf`.
4. **Exact inventory check**: every non-directory entry found under the
   temp tree corresponds to exactly one manifest path and vice versa --
   catches a tar smuggling something the manifest never listed (round 3
   only checked that listed paths existed, never that nothing EXTRA was
   present).
5. Every tracked path must be a regular, non-symlink file, with the
   correct sha256 at the PRE-hardening (original) mode.
6. `chmod -R a-w` the temp tree -- **REQUIRED to succeed** (round 3 ran
   this AFTER the rename with the failure silently ignored via `chmod
   ...; true` -- LO/BO both flagged this as fail-open: a crashed/skipped
   chmod left a writable final root that the OLD verification still
   accepted, because it tolerated either mode).
7. Re-verify the temp tree, now requiring the HARDENED (write-stripped)
   mode EXACTLY -- confirms the chmod actually took effect, file by file,
   before anything is renamed into its permanent name.
8. **Fail-loud atomic rename** (`mv -T`, never `mv -Tn`/no-clobber) of the
   temp sibling onto the final name. Safe without the `-n` guard: by
   construction, the final name's absence was already confirmed (step 2)
   under this SAME continuously-held lock, so a rename failure here is
   now a genuine, surprising error worth failing loudly on, not a benign
   already-done race. Re-verifies once more on the final path
   (inventory/type/hash/hardened-mode) before declaring `PUBLISH-OK`.

**`verify_role_files()` (shared by `preflight()`, the barrier, and the
publish script's own checks) now requires the HARDENED (write-stripped)
mode EXACTLY, dropping round 3's "accept either the manifest mode or its
write-stripped variant" leniency** -- that leniency existed because round
3's chmod ran after the rename with its failure ignored, so a legitimately
published root could be caught mid-way in a not-yet-hardened state; now
that hardening is REQUIRED and happens strictly BEFORE the rename, nothing
should ever be renamed into its final name without already being hardened,
so the check can safely be exact again. **Verified live as a genuine
regression, not just a design argument**: a fresh, dedicated mutant
(described below) reverts to either-mode-acceptance and shows it silently
accepts a real, chmod-regressed (writable) published file.

### Two real bugs caught live while building this transaction (both fixed at the source)

**Umask-truncated permissions on extraction.** A local sandbox dry run
against the REAL p43 tar (pulled from q3, sha256-verified) found that
`MANIFEST.tsv` -- stored in the tar at mode `664`, matching the manifest
exactly -- extracted as mode `644` under the new strict pre-hardening
check. Root cause: plain `tar -x`, run as a non-root user (every publish
target host extracts as the SSH login user, never root), applies the
extracting process's UMASK on top of the archive's stored mode bits
instead of reproducing them exactly (confirmed: local shell `umask` was
`0022`, and `664 & ~022 == 644`). This would have hit ANY real host, not
just the sandbox. Fixed: `tar --same-permissions -xf`, which forces GNU
tar to reproduce the archive's exact stored mode regardless of umask.

**Partial-upload cleanup gap.** Live testing against a near-full disk
(research7, see below) surfaced that a failed non-q3 upload (`cat >
~/role-artifacts/incoming-*.tar`, e.g. from `ENOSPC` partway through)
left a real, partial multi-megabyte file behind: the locked script's own
cleanup trap only runs for the SUCCESSFUL-upload case (it can't run if
the upload itself is what failed), so nothing else was cleaning this up.
Fixed: `publish_immutable_root()` now does a best-effort `rm -f` of the
incoming filename immediately on a push failure, before returning the
error -- a failed cleanup here never masks the original, more actionable
push error.

## In-command ATTESTATION: identity proven before the role process can even start

Round 3's design hashed a role's tracked executable from inside the
ALREADY-RUNNING container, immediately after start
(`verify_launched_container_identity()`, a `docker exec ... sha256sum`).
BO's definitive round-4 blueprint supersedes this with a stronger,
in-band mechanism: **the identity check is baked directly into the
container's own launch command**, so the real role binary structurally
cannot execute at all if it fails.

`_attestation_prefix(binary_set, token)` builds a bash snippet from
`manifest["binaries"]` (every tracked file, not just one role-specific
probe path): for each, it checks (from INSIDE the container, through its
own `/work` bind mount) that the file is a regular, non-symlink file with
the exact expected sha256. If every check passes, it prints a unique
`ARTIFACT-ATTEST-OK-<token>` marker and falls through to the real role
command; if ANY check fails, it prints `ARTIFACT-ATTEST-FAIL` and calls
`exit 97`, which -- because this text runs as the container's own PID 1
(`docker run -d ... bash -c 'PREFIX; REAL_CMD'`) -- terminates the entire
container immediately. For S and F, the prefix is prepended directly to
the scheduler/daemon's own `docker run` command. For C, the client
container starts as an idle holder (`sleep 1800`, nothing to attest
there yet); the prefix instead gates the SEPARATE `docker exec ... bash
-c 'PREFIX; farm_client.sh ...'` invocation that actually runs the test,
so `farm_client.sh` -- the real action for C -- is exactly as
structurally unreachable-without-attestation as the scheduler/daemon
`exec` is for S/F. `run_client()` additionally, independently, checks
that its OWN expected marker actually appears in the captured output
before trusting anything in it -- discovered live to be a genuine SECOND,
redundant layer of protection specific to C (see mutant results below).

`wait_for_attestation()` polls `docker logs` for the marker purely to
turn a dead-on-arrival container into a clear, fast `RuntimeError` rather
than something that silently limps along -- it is a proof-COLLECTION
step, not part of the gate itself: the gate is the container's own
launch command, already decided by the time `docker run`/`docker exec`
returns.

**Verified live, end to end, on real hosts, using the real production
functions** (never a fake/reimplementation): a clean isolated store
launches successfully, marker appears, the real command actually
executes; a corrupted isolated store (one tracked file tampered) is
refused, container exits 97 within about a second, the real command
NEVER runs. **Three independent deletion mutants** (see "Verification
gate" below) each prove their own callsite's attestation is genuinely
load-bearing: with S's (or F's, or C's) attestation prefix+wait removed
at that ONE production callsite, the real scheduler/daemon process is
observed ACTUALLY RUNNING (`docker exec ... pgrep`, RC=0) against a
corrupted store that the unmutated code correctly refuses.

`verify_launched_container_identity()` and its supporting
`_ROLE_PROBE_PATH` map are retired entirely -- attestation is a strictly
broader, earlier, in-band replacement.

## The whole-plan barrier and the mutation-started tracker

**`revalidate_entire_plan(worker_hosts, plan, client_host=None)`**: a
zero-mutation re-`preflight()` of EVERY role in an already-resolved
`LaunchPlan` -- S, every F, C when used -- in ONE pass, called in `main()`
immediately after `resolve_launch_plan()` succeeds and strictly before
any mutation. `resolve_launch_plan()` alone proves every role valid AT
THE MOMENT IT, INDIVIDUALLY, was resolved; nothing re-checks the WHOLE
plan again as one atomic step before the first mutation, which leaves a
tamper on ANY role (not just the one about to be mutated) undiscovered
until that specific role's own turn -- by which point earlier roles may
already have been torn down and relaunched. This barrier closes that gap.
`revalidate_before_mutation()` (round 3's per-role, immediately-before-
its-own-mutation check) remains, unchanged, as defense-in-depth: it
catches a tamper landing AFTER the barrier clears but before that
specific role's own mutation (up()'s S->F loop, and run_client(), each
take real wall-clock time). Neither replaces the other.

**`MUTATIONS` (a small module-level tracker) replaces `main()`'s old
`down_needed = True`**, which used to be set immediately after
`resolve_launch_plan()` succeeded -- i.e. BEFORE the barrier or `up()`
had performed a single real action, so a barrier-revalidation failure
still left `down_needed` True and `down()` would tear down a
possibly-unrelated, pre-existing, exact-name running cluster (LO/BO's
finding). `MUTATIONS.mark()` is called from inside the four actual
mutating primitives themselves (`docker_rm`, `scratch_prepare`,
`docker_run_detached`, `push_file`) -- never from any call site above
them -- so it is structurally impossible for it to become True without a
real mutating action having actually run. `main()` now checks
`MUTATIONS.started` (not a static flag) in its `finally` block.

**Verified live, both ways**: a no-network transport-spy scenario proves
the barrier independently catches a tamper landing strictly BETWEEN the
two preflight passes (a STATEFUL fake returns the correct hash the first
time a target file is queried, during `resolve_launch_plan()`, and a
wrong one every time after, during the barrier's own pass) -- 0 actions
recorded, AND a real, pre-existing, unrelated `farm-sched`-named
container on q3 is confirmed to survive completely untouched (direct
proof `down()` was never called, not merely that `up()`'s own
`docker_rm` didn't run). A dedicated barrier-neutralization mutant (see
below) confirms the barrier is genuinely load-bearing: with it
neutralized, the SAME tamper is no longer caught before mutation begins
-- `"DOWN: tearing down cluster"` appears (which only ever happens once
`MUTATIONS.started` is True), proving S was mutated before the tampered
role was ever reached.

## The race-gate seam and a REAL, process-level concurrent race gate

`_race_gate_pause()` is an inert-by-default seam, called once from
`main()` between `resolve_launch_plan()` and `revalidate_entire_plan()`.
Gated entirely by two environment variables (`FARM_RACE_READY_FILE`,
`FARM_RACE_CONTINUE_FILE`) that nothing in `farm.py` itself ever sets --
in every normal invocation both are unset and the function returns
immediately, zero behavior change. When set (only ever by an external
test harness, in a subprocess's environment), it touches the ready file
the instant it's called, then blocks (bounded, polling) until the
continue file appears.

This backs a genuinely concurrent race-gate test: a tiny standalone
driver script monkeypatches `immutable_root()` to an isolated,
uniquely-prefixed store and then calls the REAL `farm.main()` -- launched
as a genuinely separate OS process (`subprocess.Popen`), not a thread and
not a reimplementation. The foreground test waits for the ready file
(main() has resolved its plan and is paused at the seam), then
concurrently: (a) calls the REAL `publish_immutable_root()` against the
SAME host+set the background plan resolved (simulating a legitimate
operator running `distribute` at the same moment -- must no-op
harmlessly), and (b) performs a genuine wrong-hash tamper (corrupts
`MANIFEST.tsv`) on that SAME resolved host+root -- never a different,
coincidentally-identical-looking one (the "same-root rule": both sides
compute the identical `isolated_root()` formula from the same manifest,
so they agree by construction without needing IPC). It then releases the
continue file and waits for the background process to exit.

**Two rows, both real, both passing**: the FINAL F (2nd of 2 workers) is
tampered while S resolves clean; separately, C is tampered while S and F
both resolve clean. Both refuse via `revalidate_entire_plan()` naming the
exact host+role, with **zero new containers** on any involved host and
**`"DOWN: tearing down cluster"` never appearing** in the captured
output (direct proof `down()` was never called). A barrier-neutralization
mutant (`revalidate_entire_plan()`'s body replaced with an immediate
`return`) re-runs the final-F row and confirms the row FLIPS: the exact
same refusal message no longer appears, and `"DOWN: tearing down
cluster"` DOES appear -- proving mutation genuinely began (S, and likely
the clean first worker) before the tampered role was ever reached.

research7 is not used as a race-gate target in this revision (its gaps
are now fixed -- see "research7's gaps: FIXED 2026-08-24" below -- this
section simply wasn't re-plumbed for it once the fix landed). q2 stands in
as the second worker instead.

## Harness script integrity: farm_client.sh/replay.py sourced from the checkout, hash-pinned

A round-4 finding (BO): `run_client()`'s `SCRIPTS`/`HUB_DIR` mechanism
used to source `farm_client.sh` and `replay.py` -- the compatibility/
JOIN-replay half of the test harness, pushed into the client container's
`/scratch` before every test -- from a hardcoded AMBIENT session-scratch
path, entirely outside the reviewed/committed SHA. Content agreed
(confirmed: the ambient copies were byte-identical to the committed ones,
both frozen at commit `f5d13fd9`), but that agreement was never verified,
only assumed -- an execution-provenance gap: the compatibility/JOIN cells
executed bytes that were never actually part of any reviewed commit.

Fixed (the MINIMUM shape BO offered, chosen over the stronger
container-tmpfs-staging alternative to keep this round's scope bounded --
see "Deferred: stronger harness-staging shape" below):

- `HUB_DIR` is now `os.path.dirname(os.path.abspath(__file__))` -- the
  checkout's own `farmharness/` directory, never an ambient path.
- `FROZEN_HARNESS_HASHES` pins both scripts' sha256, exactly as
  `role-manifests/*.json` pins every tracked role binary's.
- `verify_harness_scripts()` -- pure, local, read-only -- hashes both
  files from `HUB_DIR` and compares against the frozen values, called as
  the very FIRST thing `run_client()` does (before
  `revalidate_before_mutation()`, before `push_file()`, before any
  docker primitive). Raises `RuntimeError`, fail-closed, no fallback, on
  any mismatch or missing file.

**Verified live, both the direct claim and the integration claim BO
asked for**: `verify_harness_scripts()` itself refuses a tampered
checkout copy of `replay.py`; and calling the REAL `run_client()`
against that same tamper refuses BEFORE touching the target host at
all -- confirmed via a real pre/post `docker ps` diff (0 containers
appear) and `MUTATIONS.started` staying `False` (a real mutating
primitive never runs). `replay.py` restored byte-exact afterward
(`cmp`-verified). Separately, a full real end-to-end cluster+client
cycle (fmt, 51 translation units, single-host q3) confirms the fix
doesn't regress the real path: byte-exact-remote 51/51, clean teardown.

**Deferred: stronger harness-staging shape.** BO's preferred, stronger
design -- build a deterministic two-file tar in memory on the hub, feed
it directly over SSH into the foreground `docker run -i`, extract into
CONTAINER-PRIVATE tmpfs (never mutable host scratch), verify type/name/
size/hash INSIDE the container, emit a run-token `HARNESS-STAGED-OK`
marker before executing -- was not shipped this round (explicitly
offered as optional, to be adopted "if it fits round-4 scope"; the
MINIMUM shape closes the actual provenance gap found, and the stronger
shape's added value is defense-in-depth against a same-window,
same-checkout host-side tamper of the ALREADY-staged file after
`verify_harness_scripts()` passed but before `push_file()` completes --
a narrower, lower-probability window than the one this round's fix
closes). Left as an explicit follow-up.

## Cell-verdict fix: `main()`/`dump_worker_evidence()` are no longer false-green-capable

A third, independent round-4 finding (BO): `main()` used to discard
`dump_worker_evidence()`'s own boolean return value entirely (never
assigned, never combined into `ok`) and never inspected `run_client()`'s
`r.returncode` either. That meant a cell whose client script itself
failed -- a nonzero exit after successful worker registration -- or
whose JOIN bijection was genuinely broken, still let the whole `farm.py`
process exit 0. A false green: `CLUSTER: REGISTERED-OK` plus a bad or
missing `CELL: PASS` could still report overall success.

Fixed exactly per BO's spec, in `main()`:

```python
client_ok = (r.returncode == 0)
join_ok = dump_worker_evidence(workers, r.stdout, r.returncode, a.binary_set_c)
ok = ok and client_ok and join_ok
```

And `dump_worker_evidence()` itself (`_dump_worker_evidence_impl()`,
wrapped by a never-raising outer function -- malformed or missing
evidence must produce a `False` verdict, never an uncaught exception)
now independently re-derives and requires ALL of:

- exactly one parseable `CELL: project TUs=N mode=M` header line;
- exactly one terminal `CELL: PASS` line (a `CELL: FAIL`, or a missing
  terminal line entirely, is a hard fail here, never silently ignored);
- the JOINROW count equals `N * (2 if mode == "sequence" else 1)` --
  `replay.py`'s own `sequence` mode genuinely replays every TU twice
  (once cold, once warm), so this is the correct expected cardinality,
  not an arbitrary guess;
- every row's `accepted`/`exact`/`mode` fields are exactly `1`/`1`/`remote`;
- client-reported job IDs are unique and none is `-1` (a JOINROW with
  `jobid=-1` means `replay.py`'s own assignment-line regex never matched
  anything for that TU -- genuinely missing evidence, not a valid id);
- **EXACT set equality** between the client's reported job IDs and the
  union of every F host's own requested-job-id log entries -- this
  replaces the previous subset check (`covered <= f_union`), which is
  the actual bug BO found: it silently accepted extra or stale
  F-reported job IDs that the client never claimed;
- F-reported completions (`grep -c 'Remote compilation completed with
  exit code 0'` per host, summed) equal the JOINROW count exactly.

One machine-readable terminal line, `CELL-VERDICT: join_ok=... client_rc=...
expected_rows=... actual_rows=... set_equal=... f_completions=...
product=... harness=... run=...`, is printed on every path (including the
never-raise wrapper's `except` branch) and gates the process exit via the
`main()` combination above.

**Six named controls**, each independently making the production
invocation nonzero, all verified in `artifact_selection_test.sh`:

1. client exits 7 after registration -- `client_ok=False` -> exit 2.
2. `CELL: FAIL` with otherwise-plausible rows -- `terminal_ok=False` ->
   `join_ok=False`.
3. one JOINROW removed -- cardinality mismatch -> `join_ok=False`.
4. one extra F-reported job ID injected, client-side ID list otherwise
   untouched -- exposes the OLD subset-check bug directly: under the old
   `<=` comparison this would have PASSED; under the new `==` comparison
   it correctly fails.
5. `join_ok` deliberately dropped from the `main()` combination (a real
   source mutation of the exact three-line block above) -- a genuinely
   bad JOIN (`CELL: FAIL`) is proven to incorrectly resurrect exit 0,
   i.e. this reproduces BO's original false-green finding on demand, then
   is restored byte-exact.
6. `client_rc` (`client_ok`) deliberately dropped the same way -- a
   genuinely bad client return code (7) is proven to incorrectly
   resurrect exit 0, then is restored byte-exact.

Verified both ways: fast synthetic unit coverage of
`dump_worker_evidence()` and the `main()`-level combination (controls
1-6, via monkeypatched transport/primitives, isolating this claim from
the ones already separately proven elsewhere -- attestation, the
barrier, harness-script integrity), AND a real end-to-end confirmation
(genuine q3 single-host S+F+C cluster, real `run_client()` against the
real fmt project, real `dump_worker_evidence()` against the real
resulting `worker.log`s) confirming the strengthened parser/bijection
logic recognizes a genuine pass against PRODUCTION output, not just
hand-built synthetic JOINROW lines -- byte-exact-remote 51/51,
`client_ok=True`, `join_ok=True`, clean teardown.

The frozen replay blobs (`farm_client.sh`/`replay.py`, commit `f5d13fd9`)
needed no change for this fix -- it is entirely `farm.py`-side (the
consumer of their output), not a change to what they emit.

## Distribution: idempotent, explicit, publish-only (never repair-in-place)

```
python3 farm.py distribute --sets p43,p50 --hosts q3,research6,research7,q2
```

The **only** code path allowed to write role-artifacts onto a host. It is
never called automatically by `up()`/`run_client()`/`resolve_role()`/
`revalidate_before_mutation()`/`revalidate_entire_plan()` -- bringing
files onto a host is always a deliberate, visible operator action, never
a side effect of trying to launch a cluster. **q3 is included, not
skipped** -- it also needs its own content-addressed copy published from
its local tar, exactly like every other host, so no code path ever has
to special-case where a root's bytes actually live.

For each (set, host) pair it checks the content-addressed
`immutable_root(binary_set)` against the manifest, under the
single-canonical-lock 8-step transaction described above.
Already-published-and-verified is a pure no-op (`already-current`; a
second run of the command above is verified idempotent). Absent is
published fresh (`published (root was absent)`).
**Existing-but-failing-verification is a hard failure, on purpose** --
`distribute` will never silently "fix" a corrupted immutable path, only
report it and point at the manual `rm -rf` + re-run recovery.

## Migration from the old mutable per-host layout

Before this successor, every host had a FIXED path per set
(`~/role-artifacts/{p43,p50}-root`) that `distribute` repaired in place on
drift; a subsequent revision moved to a FLAT content-addressed layout
(`~/role-artifacts/<set>-<hash>`) before landing on the current NESTED
`$HOME/role-artifacts/store/<set>/<hash>` layout. **No code path in this
file resolves through either older layout anymore** --
`immutable_root()` always derives the current store path from the
committed manifest's `tar.sha256`. The old directories
(`LEGACY_MUTABLE_ROOT` in `farm.py`, kept only as a documented historical
reference) may still physically exist on q3/research6/research7/q2 as
harmless orphaned data from before each migration -- nothing reads them,
and this task did not delete them.

**research7's gaps: FIXED 2026-08-24 (disk + digest ref; owner-directed
repair).** Historical finding, kept as provenance -- both were real,
independently blocking, and NOT fixed by this task at the time each was
found:

1. **Image digest** (found in an earlier revision): the pinned tag
   `icecream/farm-node:ubuntu22-gcc11-boost174` resolved to a DIFFERENT
   content digest on research7 than the pinned one on q3/research6/q2.
   Root cause turned out to be the STORE ARCHITECTURE, not the image
   bytes: q3/research6/q2 run docker's containerd image store (where
   `Id == manifest digest`); research7 ran the classic overlay2 store,
   which never minted a digest ref at all. Fixed via a
   `repositories.json` digest-ref injection (owner-directed).
2. **Disk space** (found while building round 3): research7's root
   filesystem was essentially full (observed as low as ~1.5MB free of
   ~56GB), independently preventing extraction of the store layout there
   even once gap 1 was fixed. Fixed by snapshotting old revisions and
   clearing stale icematrix/builder cache (owner-directed); confirmed
   live during round 4 at ~5.4GB free.
3. **A third, freshly-introduced gap, found and fixed live during this
   same verification**: immediately after the two repairs above landed,
   `/var/run/docker.sock` on research7 was `root:root` mode `0660` (a
   side effect of the `snap docker restart` used in the repair), which
   denied `farm.py`'s own non-root production SSH path ANY docker access
   at all -- independent of whether the image content itself was now
   correct. **Root cause turned out to be TRANSIENT, not permanent**: a
   `snap docker restart` recreates the socket as `root:root`, and the
   snap's own post-start hook restores `root:docker` after a short
   delay -- this was caught mid-window, not a real regression needing a
   manual fix. **The correct response to this specific symptom is to
   retry after the hook settles, never to `chgrp` the socket manually**
   (a manual chgrp would fight the snap's own hook and could leave the
   socket in an inconsistent state on the NEXT restart). By the time this
   was reconfirmed, the hook had already completed on its own.

All three are now confirmed fixed **live, through farm.py's own
production functions** (not merely relayed): `image_digest_remote()`
returns the pinned digest exactly; `df` shows ~5.4GB free; a real
`publish_immutable_root()` + `preflight()` cycle both pass clean.
research7 now participates fully in the 24-cell matrix and `distribute`
(all 4 hosts expected green). It is still not used as a race-gate/tamper
target in this revision's isolated-store tests (q2 continues to serve
that role) -- not because it's broken, simply because those sections
were already built and validated against q2 before this fix landed, and
re-plumbing an already-proven-working section for no functional gain
wasn't worth the risk this late in the round.

**A minor operational footgun found live**: if a `docker run -v
SRC:/work:ro` is ever issued against a `SRC` that doesn't exist on the
target host (confirmed during debugging a test-script bug, never in
production code), Docker auto-creates `SRC` as an empty directory as a
side effect of the mount -- and because the Docker DAEMON runs as root,
that auto-created directory is ROOT-OWNED, un-removable by the normal
SSH login user without `sudo`. Every real code path in `farm.py`
publishes a root (as the login user) before ever mounting it, so this
should not occur in production; noted here because a test-script bug
during round 4 hit it directly (fixed in the test, recovered via `sudo
-n rm -rf` on the affected host).

## Selection mechanism

`farmharness/farm.py` has `immutable_root(binary_set)` (content-addressed,
see above) and `role_tree(binary_set)` (`None` -> the original hardcoded
`TREE`, unchanged; otherwise delegates to `immutable_root()`). `main()`
exposes `--binary-set-S/-C/-F {p43,p50}`, each defaulting to `None` so
omitting all three reproduces prior behavior exactly (no manifest lookup,
no preflight -- nothing existing changes); it passes all three straight
into `resolve_launch_plan()` before the race-gate seam,
`revalidate_entire_plan()`, `up()`, and `run_client()` ever run.
`role_tree()`/`HOSTS`/`KNOWN_SETS`/`preflight()`/`distribute()`/
`resolve_launch_plan()` are all safely importable without triggering a
live deploy (`main()` stays behind `if __name__ == "__main__":`).

## Verification gate (`farmharness/artifact_selection_test.sh`)

Runs, in order:

- **fresh-archive no-ambient gate:** `git archive HEAD -- farmharness`
  into a brand-new, otherwise-empty directory, `farm.py` imported from
  THAT location, both manifests loaded via `farm.load_manifest()` from
  there, schema-key presence checked, and `immutable_root()`'s
  store-layout derivation from `tar.sha256` confirmed.
- a static AST anchor covering the round-4 flow: `resolve_role()` exactly
  3x in `resolve_launch_plan()`, 0x in `up()`/`run_client()`;
  `revalidate_before_mutation()` exactly 2x in `up()`, 1x in
  `run_client()`; `_attestation_prefix()`/`wait_for_attestation()` each
  exactly 2x in `up()`, 1x in `run_client()`; `docker_run_detached()`
  exactly 1x in `run_client()`; `verify_harness_scripts()` exactly 1x in
  `run_client()`; `revalidate_entire_plan()`/`_race_gate_pause()`/
  `resolve_launch_plan()` each exactly 1x in `main()`. A second anchor
  confirms `MUTATIONS.mark()` is called from exactly the 4 real mutating
  primitives and nowhere else.
- **HARNESS SCRIPT INTEGRITY**: `HUB_DIR` is confirmed to be the
  checkout's own directory (never an ambient path); a baseline confirms
  both harness scripts hash-match their frozen values; a tamper mutant
  (the checkout copy of `replay.py`) proves BOTH `verify_harness_scripts()`
  itself AND the real `run_client()` integration point refuse before
  touching any host (0 containers, `MUTATIONS.started` stays `False`),
  restored byte-exact afterward.
- **CELL-VERDICT fix**: synthetic coverage of the strengthened
  `dump_worker_evidence()` (baseline, `CELL: FAIL`, row-removed, and the
  extra-F-id case that specifically proves the subset-vs-exact-equality
  bug fix) and of `main()`'s `client_ok`/`join_ok`/`ok` combination
  (good path, bad client return code); two further named controls as
  real source mutations of that exact combination line (dropping
  `join_ok`, then `client_rc`), each proven to resurrect the original
  false-green bug on demand and then restored byte-exact; PLUS a real
  end-to-end confirmation (genuine q3 cluster, real fmt client run) that
  the strengthened parser recognizes a genuine pass against real
  `replay.py`/`worker.log` output, not just hand-built synthetic lines.
- **no-network TRANSPORT-SPY gates:** the original four
  `resolve_launch_plan()`-level scenarios (invalid initial S; invalid
  second F of three; invalid C on a distinct host; final-worker-of-three),
  each expecting exactly 0 actions -- PLUS a NEW barrier-specific
  scenario (a stateful fake, correct on a target's first preflight query,
  wrong on every query after) proving `revalidate_entire_plan()`
  independently catches a tamper landing strictly between the two passes,
  0 actions, and a real pre-existing container surviving untouched.
- **ordering-violation mutant**: unchanged from round 3 -- proves the
  "0 actions" claim is genuinely failable.
- `distribute` run twice against all 4 hosts for both sets; a genuinely
  concurrent host-canonical-lock test (two threads, proven wall-clock
  overlap, exactly one publishes).
- **24-cell matrix**: all 4 hosts x 2 sets x 3 roles through PRODUCTION
  `preflight()` -- all 24 PASS (research7's gaps are fixed).
- research7's three gaps (image digest, disk space, and a
  freshly-introduced docker-socket-permission issue found live while
  verifying the first two) confirmed FIXED, through production functions.
- a same-version wrong-hash mutant, two ways (isolated copy; real
  end-to-end corrupt-refuse-recover cycle on research6).
- **6 PUBLICATION mutants**, each a real source mutation of
  `_publish_script()`/`verify_role_files()`, isolated stores only, each
  independently reddening a NAMED check and fully restored (`cmp`-verified):
  1. q3 tar-hash check removed -- a tar with extra unhashed trailing
     bytes is extracted and published anyway.
  2. exact-inventory check removed -- a tar with a manifest-unlisted
     extra member is published anyway.
  3. type/symlink check removed (combined with neutralizing the mode
     check's incidental non-dereferencing symlink rejection, since the
     shipped code has real redundancy here -- a symlink's own `stat`
     mode is always 777 on Linux, already failing the mode check even
     without the explicit type check) -- a symlink replacing a tracked
     file gets past the stage the type check exists to gate.
  4. `verify_role_files()` reverted to either-mode-acceptance -- a real,
     live, chmod-regressed (writable) published file is silently
     accepted.
  5. canonical lock removed -- two genuinely concurrent publishers racing
     the same absent target no longer reliably produce the clean
     (published, already-current) pairing; at least one hits a real
     failure instead.
  6. atomic rename-via-temp-sibling replaced by in-place extraction -- an
     interrupted publish (a tar hash-consistent with itself but
     truncated) leaves a broken, partially-extracted directory AT the
     final immutable name, something the real design makes structurally
     impossible.
- an `immutable_root` -> legacy/mutable-alias mutant: structurally proves
  a fixed (non-hash-derived) resolver always returns the same name
  regardless of tar hash -- the exact collision hazard content-addressing
  exists to prevent.
- a mutable-IMG mutant: `up()`'s S launch reverted to the bare, floating
  `IMG` tag instead of the plan's resolved, digest-pinned `plan.s_img` --
  the captured `docker run` command's image reference changes from the
  pinned digest to the mutable tag, exactly the pinning guarantee
  content-addressed resolution exists to carry through to the one place
  it actually matters (the real launch command).
- the dropped-`:ro` mutant (unchanged from round 3): with the mount
  forced writable, a write through it that must fail now succeeds, run
  only against an isolated store.
- **ATTESTATION baseline + 3 independent deletion mutants**: a clean
  isolated store launches and runs for real; a corrupted one is refused
  before the real command ever starts. Removing S's (or F's, C's) own
  attestation prefix+wait at that ONE production callsite (source
  mutation, `revalidate_before_mutation()` monkeypatched to a no-op
  purely to isolate this layer from that separately-proven one) lets the
  real scheduler/daemon process be observed ACTUALLY RUNNING against a
  corrupted store; for C, farm_client.sh is reached with no gate at all
  (though `run_client()`'s own independent post-hoc marker check --
  itself a real, extra, redundant defense specific to C -- still catches
  the missing marker, a genuine finding about C's layered protection).
- **the REAL CONCURRENT RACE GATE**: a genuinely separate background
  process driving the actual `main()` entry point, paused at the
  race-gate seam, raced by production `distribute()` + a real wrong-hash
  tamper against the exact host+root it resolved -- two rows (final-F,
  separately C), both refusing cleanly with zero mutations and `down()`
  never called -- plus a barrier-neutralization mutant proving the row
  flips (mutation genuinely begins) when the barrier is removed.
- a real end-to-end launch (`resolve_launch_plan()` -> `up()` with real
  attestation -> registration -> `down()`, single host q3).
- a skipped-preflight mutant (unchanged in spirit from round 3; its
  dynamic-invocation fake now also stubs `wait_for_attestation()`
  directly, since attestation itself is validated by its own dedicated
  section above and this test's own claim is specifically about the
  `PREFLIGHT-OK` marker + action-reachability).
- the original 9 selection-mechanism assertions (identity baseline +
  selection-flip mutation, per role, per set).

Every source mutation in this file uses the same pattern: snapshot
farm.py, mutate, verify the redden, restore via `cp`, verify byte-exact
restoration via `cmp -s`, all guarded by a shell `trap` so a mid-test
failure still restores the file. Every host-side mutation either targets
an isolated, uniquely-prefixed test path torn down regardless of
outcome, or -- where it deliberately targets real shared data (the
same-version wrong-hash variant B, on research6) -- has its own explicit,
verified recovery built into the same test, followed by a dedicated
post-mutant repair-verification row before the script continues into any
section that assumes a clean host.

Run: `./artifact_selection_test.sh` (needs SSH reachability to q3,
research6, research7, q2; `ARTIFACT_TEST_HOST` overrides the host used
for the identity/mutant probes, default `q3`; never starts or stops the
actual farm-sched/farm-worker/farm-client cluster except the narrow,
guaranteed-torn-down real end-to-end launch test and the race gate's
uniquely-named, always-torn-down containers/driver processes).

## Production vs. test-scaffolding locus, per key claim

Explicitly called out per LO/BO's repeated requirement ("that distinction
is the entire HOLD"):

| Claim | Runs in |
|---|---|
| Publication 8-step transaction (tar-hash, inventory, type, hardening, lock, atomic rename) | **PRODUCTION**: `_publish_script()`/`publish_immutable_root()`, called directly by `distribute()` and every publication test |
| `verify_role_files()` hardened-mode-exact check | **PRODUCTION**: shared by `preflight()`, the barrier, and the publish script |
| Whole-plan barrier | **PRODUCTION**: `revalidate_entire_plan()`, called from `main()` |
| Per-role defense-in-depth | **PRODUCTION**: `revalidate_before_mutation()`, called from `up()`/`run_client()` |
| Mutation tracking / down()-suppression | **PRODUCTION**: `MUTATIONS` object + `main()`'s `finally` block |
| In-command attestation | **PRODUCTION**: `_attestation_prefix()` embedded directly in `docker_run_detached()`'s/`run_client()`'s own launch commands, `wait_for_attestation()` |
| Race-gate seam | **PRODUCTION**: `_race_gate_pause()`, called from `main()` (inert unless test env vars are set) |
| Harness-script integrity (HUB_DIR, hash pinning) | **PRODUCTION**: `verify_harness_scripts()`, called as the first line of `run_client()` |
| Cell-verdict combination (`client_ok`/`join_ok`/`ok`) | **PRODUCTION**: the three-line combination in `main()`, immediately after `run_client()` returns |
| Cell-verdict bijection checks (header/terminal/cardinality/rows/uniqueness/exact-set-equality/completions) | **PRODUCTION**: `dump_worker_evidence()`/`_dump_worker_evidence_impl()`, called from `main()` with the real `run_client()` return value |
| Race-gate concurrency orchestration (subprocess launch, timing, tamper injection) | **TEST SCAFFOLDING**: the driver script + the foreground orchestration in `artifact_selection_test.sh` -- but it drives the real `main()` process end to end, never reimplementing or faking any of the production functions above |
| Isolated-store path prefixing | **TEST SCAFFOLDING** (a monkeypatch of `immutable_root()` only -- every function that consumes the result runs unmodified) |
| Source mutations (18 named mutants total) | **TEST SCAFFOLDING** temporarily edits `farm.py`'s own source, runs it in a fresh process, then restores byte-exact -- the code being exercised IS production code, just deliberately, temporarily broken to prove a specific check is load-bearing |
