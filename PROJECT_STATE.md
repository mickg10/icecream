# Icecream: current state and cleanup boundary

Updated 2026-09-22 UTC. This is the current operational checkpoint; older
progress reports are history, not evidence that the current product passed.

## Docker developer bootstrap — 2026-09-22, q4 end-to-end PASS

Added `make dev-bootstrap` and `make qa`, using the root `farm.json` (override
with `FARM=...`) and mandatory `ICEFARM_TMPDIR`. The SDK supplies native build
dependencies, Python/pytest and Java. Current/P43 scheduler, client and worker
roles share product images; P43 is rebuilt from the pinned real 1.4.0 commit.
This does not replace the qualified-image identities or external farm below.

q4 passed the complete `make qa` command with Ubuntu 24.04, four jobs and a
16 GiB limit, using the SDK built locally on nas642. Build/install, real P43
build, runtime-image packaging and all five mixed cases passed. Native:
169 passed, six skipped; separate root cache-service checks: two passed.
Python: 1,486 passed, seven skipped in 54.43 seconds. Total about 11m37s,
excluding SDK preparation/transfer. Tested source snapshot:
`3ce12ffb4c2e619537e5c3dbc78cb6e2c5038ee59da4e30d26d748ec759a8ead`.
Remote result:
`/home/mickg10/scratch/icecream/bootstrap-trial.3Ih5my/icecream-qa-5cxfzjy1/result.json`.
Local copy, native/Python logs and mixed evidence:
`/tanksmall/scratch/tmp/icecream-q4-final-5cxfzjy1/`.

Verified on nas642: SDK build; current build/install (177 s with two jobs);
P43 build/install; five mixed-version/profile remote-compilation cases;
real repository A/B selection and missing-repository refusal; SDK save/load.
The last frozen full NAS run (`icecream-qa-orefyqq0`) remains FAIL: 168 native
passed, six skipped, one endpoint-script failure; Python 1,486 passed, seven
skipped in 236.82 seconds. The endpoint failure was an auxiliary `git diff`
check in a gitless source snapshot, after runtime checks passed. Its correction
passed separately on NAS (`icecream-targeted.K4ePDp`) and in the full q4 run.
Do not relabel that older NAS result as a full-command PASS.
The subsequent NAS root cache-service and sanitizer subset also passed (two
passed, zero skipped) with the runner's shared short temporary-path mount and
Make-provided environment. An earlier ad hoc direct-binary reproduction used
different setup and failed; it was not evidence of a product regression.
That targeted Make rerun reused the NAS build tree, replacing its two
cache-service test records; the original aggregate logs/result remain FAIL.
Output ownership was restored to the invoking host user afterward. The
`p49daemon` failed-fork check also returned the expected exit 2 without any
kill/wait calls (`/tanksmall/scratch/tmp/p49-fork-guard.1/`).

The SDK now runs ordinary build/tests as `nobody`, with a separate required
root cache-service/leak-check gate. Root-running the entire suite was incorrect
for tests modeling ordinary P50 users. Fresh Ubuntu also exposed a GCC 13
test-header macro issue and several root fixture/startup-error bugs; fixes are
test-only. Runtime logs, source hashes and stage statuses are retained under
the selected scratch directory. Normal exits restore output ownership.

Earlier isolated runs exposed private fmt corpus paths, missing Java,
long Unix socket names, test-only machine-specific scratch paths, and Docker
blocking the descriptor-identity operation. These were corrected. The unchanged descriptor test
passes with `SYS_PTRACE` in a private-PID QA container. Both compressed-golden
checks now pass using per-frame decoded equality across zstd versions;
same-build byte checks and historical vectors are unchanged. The runner aliases the same
explicit scratch mount at `/tmp` and verifies its identity, avoiding fallback
storage while keeping socket paths short.
Native skips cover optional live/remote probes; the required five-case mixed
Docker gate runs independently. Python skips cover unavailable user/PID namespaces,
one retained production corpus and five Git-history checks in the gitless
snapshot. All five history-dependent tests subsequently passed in the actual
NAS Git checkout (3.40 seconds; scratch `icecream-history-tests.eYBq6c`).
The remaining skips are explicit limits, not tests silently counted as PASS.
The older failed diagnostic logs remain retained, not rewritten as green gates.
NAS mixed proof (three explicit strict P50 profiles plus both P43 roles):
`/tanksmall/scratch/tmp/mixed-p50-profiles-1790097199/summary.json`, five PASS.
Focused codec proof:
`/tanksmall/scratch/tmp/icecream-codec-runtime2.LR7d7U/run.log`.
Repository-switching proof:
`/tanksmall/scratch/tmp/icecream-registry-switch.iPijec/registry-result.json`.

q4 is reachable through its actual configured account,
`mickg10@tt-quietbox4`; the earlier `mickg` check used the wrong account.
It has 32 CPUs, about 249 GiB RAM, and one 3.6 TiB root filesystem with
3.2 TiB free; DockerRootDir is `/var/lib/docker` on that same filesystem.
An explicit `/home/mickg10/scratch/icecream/bootstrap-trial.3Ih5my` directory
was used on that large disk; no mount or Docker configuration changed.
SDK save/load succeeded across nas642's ZFS and q4's containerd/overlayfs.
The archive SHA-256 is
`3716f7ec66cfe71079e5ec259f2de8efcf8cac7adb35c82825857b233f1e162b`.
Those stores report different image IDs (config versus manifest digest), both
present in the same archive; filesystem layer digests match. GitHub access
there lacks working credentials. The verified Git-bundle checkout is
`/home/mickg10/src/icecream-bootstrap-3Ih5my`; the edited snapshot is transferred
separately. This offline checkout trial is not a direct GitHub clone.
The q4 QA pass above uses that offline checkout and exact source snapshot.
This does not prove direct GitHub access there or Ubuntu 22.04 coverage.

## Cleanup implementation — 2026-09-22

Owner approved the readability/correctness cleanup and requested Luna agents
for testing with minimal foreground work. The owner subsequently authorized
committing the verified cleanup and publishing `sorbet_v1.5` to GitHub for a
fresh-checkout trial on research6. Verify the remote branch identity separately
from the test results below; authorization alone is not a successful push.
The larger plan remains open in
`CLEANUP_PLAN.md`; do not mistake this batch for completion of all 15 items.

- Failed disk-fill coordination now attempts abort/release for every client,
  including partial pause/resume failures; both primary and cleanup errors are
  retained. Normal S95 fill-before-pause ordering is unchanged.
- Supported package aggregates omit Fedora 28; its explicit refusal checks stay.
  Compose now bootstraps Autotools, includes Boost/xxhash dependencies, and
  defaults image builds to two jobs. No Docker build was executed for this batch.
- The 887-line manifest worker is an editable packaged shell file, byte-identical
  to its previous Python literal. Existing command identity is preserved.
- Added `make test-harness-fast` and `make test-harness-thorough`. Ordinary pytest
  still includes all tests. Only the expensive synthetic 1,000-row namespace
  revalidation test is excluded by the fast entrypoint.
- Moved the historical S8 guide from `doc/` to `research/farmharness/docs/`,
  updated runnable paths and distribution metadata. No historical content was
  discarded. Other research files retain replay/test consumers.

Luna verification: **1,455 passed, 1 deselected in 89.52 s** for the final fast
selection; JUnit: `/tanksmall/scratch/tmp/sorbet-cleanup-fast.xml`.
The separately selected thorough test passed: **1 passed, 1,455 deselected in
445.29 s**; JUnit: `/tanksmall/scratch/tmp/sorbet-cleanup-thorough.xml`.
Focused event, workload and distribution checks also pass; Bash syntax and Ruff
pass for the agent-owned changes. No farm deployment, new qualification run or
image relabeling occurred. The previously qualified image/campaign below
remains the authority.

Next: enforce explicit scratch storage at Make entrypoints, publish the branch,
and test the fresh checkout on research6. That separate remote build/test trial
is now owner-authorized; it is not a rerun of the existing qualification farm.
Selected-artifact validation and owned-process cancellation remain pending.

Research6 inspection: `~/src` and DockerRootDir are on the large data filesystem
(about 4.1 TiB free), not the root filesystem. Isolated trial directory:
`/home/mickg/src/icecream-checkout-test.1WHUxU`, with private `logs/` and `tmp/`.
The host reports 20 logical CPUs, about 378 GiB RAM and Docker 29.8.0.
GitHub HTTPS access there currently cannot obtain credentials; SSH to GitHub
on ports 22 and 443 timed out. No credentials were copied or host packages
installed. Source publication and remote download access are separate gates.

Make integration, local-harness and formal entrypoints now require an explicit
`ICEFARM_TMPDIR`: existing, absolute, writable, and not the filesystem root
(including aliases). There is no `/tmp/i` default for these targets. Child
temporary-directory settings inherit the chosen path; native object files still
require an out-of-tree build directory, and Docker storage is checked separately.
The research6 Python 3.10 probe also confirmed that `datetime.UTC` was unavailable;
active harness code and its test now use `timezone.utc`, matching the documented
Python 3.10 baseline. Native build dependencies and pytest are absent on that
host; those are additional fresh-machine prerequisites, not hidden installations.

## Release branch — sorbet_v1.5

The owner requested a local commit of the complete Icecream work on
`sorbet_v1.5`, with package version **1.5.0**. This replaces the earlier
proposed branch name `sorbet_v1.50`. The owner has now authorized a branch push
for fresh-checkout testing; no release tag is authorized or claimed.
The source version, source/binary release tests, installed-identity checks and
release manifest now agree on 1.5.0. P50 protocol/profile IDs and pinned P43
image versions are unchanged.

The full campaign below qualifies image `p50s4-diag-b269fad9`, before this
release-version change. Preserve that identity; do not relabel its artifacts
as a newly built 1.5.0 image. On 2026-09-22 an independent, read-only recursive
replay of the exact execution digest reproduced **PASS** for all seven
included suites. The S95 harness changes and archived-test removals
are included in the release preparation. Two older resource-retry fixtures
were corrected to require the fault during the first compile attempt, with
negative cases for an attempt completed before or started after the fault.

Release preparation also repairs stale resolver fixtures that still named
excluded host research6. Make-surface tests now isolate their default-temp
expectations from the caller's environment and separately check the supported
operator override across all seven farm targets. No product protocol or
acceptance threshold changed for these fixture corrections.

## Permanent checkout and Git storage

The release checkout is now at
`/tanksmall/MICKG2/mickg/src/mickg10/icecream-worktrees/sorbet_v1.5`.
It is a standalone checkout with its own `.git`, not a linked worktree of
the older `mickg10/icecream` checkout. That older checkout, its untracked
`capability/` directory and its registered worktrees are preserved unchanged.
Do not use the mistakenly created `~/src/mickg/` location.

The old `/tmp/s70-harness-recovery-20260913` name remains a compatibility
symlink for retained evidence and build paths, not the primary storage.
Git objects previously borrowed from scratch have been copied into this
checkout and its alternate-store reference removed. The missing historical
parent `a92c5287a06346a74e5508191302287b8cd73ee0` was recovered from the
upstream repository; full Git object validation passes without scratch
alternates. Origin now names `https://github.com/mickg10/icecream.git`, not
a temporary local checkout. No references or historical objects were pruned.
New working checkouts belong under `mickg10/icecream-worktrees`; disposable
builds, caches and bulk test artifacts can remain on scratch.

Fresh ordinary native targets built successfully under
`/tanksmall/scratch/tmp/sorbet-v1.5-build.C7PA1i` with `make -j2`.
Client, scheduler, daemon and generated package metadata report **1.5.0**;
both source and executable release-identity checks pass. This local build
used the existing dependency prefixes and `--without-libcap-ng` because that
optional library is absent on the host; it is not a newly qualified farm
image or a package-installation test. Package dependency-contract checks,
Autotools generation, changed-file Ruff and whitespace checks pass.

The complete local integration suite passed **1,442 tests in 540.19 s**:

```sh
PYTHONDONTWRITEBYTECODE=1 ICEFARM_TMPDIR=/tanksmall/scratch/tmp \
TMPDIR=/tanksmall/scratch/tmp PYTHONPATH=. python3 -B -m pytest \
  -q -p no:cacheprovider --durations=10 \
  --junitxml=/tanksmall/scratch/tmp/sorbet-v1.5-build.C7PA1i/integration-tests.xml \
  farmharness/integration/tests
```

The first sweep exposed 12 fixture/environment failures (1,423 passed);
the final sweep above includes their repairs and seven new temp-override
cases. One retained Firefox namespace/authority revalidation test consumed
450.91 s of the final run. A future fast/thorough split should separate that
retained-corpus check, not weaken its correctness requirements. After the
final path correction, index and complete working-diff hashes matched their
pre-move values. No full farm rerun, deployment, tag or push occurred.

## Qualification complete — full campaign PASS

Complete current-product campaign `20260921T200145Z-7ab937` finished **PASS**
with full coverage on the pinned q2/q3/q5 authority. Execution digest:
`62a9b251a06acf8e5e206d842abee58f173fe5c59292e8e6527a729063c5ae45`.
Every included suite passed: smoke, ladder, S70 resilience, S80 twobuild
(12/12), shaped S80 100-Mbit, S90 revision skew, and S95 disk full (2/2).
Terminal report:
`/tanksmall/scratch/ictmp/experiments/icecream/integration/suites/20260921T200145Z-7ab937/SUITE.md`.
No qualification process remains running.

## Qualification history

Current-product qualification on q2/q3/q5 has passed fresh S70 7/7
(`20260921T121013Z-1e6d67`), shaped S80 100-Mbit 1/1
(`20260921T132345Z-261e59`) and S90 2/2 (`20260921T133733Z-d84e68`), all with
full coverage. Two immutable S95 disk-full attempts
(`20260921T135329Z-f284c1` and `20260921T142742Z-93fe3e`) proved that the
scheduler avoided whichever bounded cache was filled because it reported load
1000; each still completed all 200 exact remote compiles but lacked a
post-event assignment on the affected worker. The repair binds S95's existing
preferred-worker facility to its declared disk-fill target, while compatible
P50 resource retry remains free to select the alternate worker. Focused tests
pass 685/685.

After preserving three diagnostic S95 failures, the final repair scopes the
worker preference to trigger job 12, injects ENOSPC while its selected compile
is active, and gives the bounded cache 512 MiB so the compiler environment
does not make the target unschedulable before the fault. Focused tests pass
686/686. Fresh S95 suite `20260921T162144Z-8df672` then passed **2/2** with
full coverage, including product result `20260921T162144Z-619b06`.

The first complete campaign `20260921T163403Z-37dfef` preserved a FAIL at the
S50 aggregate fairness gate after every smoke/S10/S20/S30/S40 and every S50
product cell passed: one sequential timing ratio was 1.069 versus the unchanged
1.05 limit. A clean matched S50 rerun `20260921T194000Z-9afbfa` passed all five
cells and aggregate ratios 1.000, 1.000 and 1.0465, identifying transient timing
noise; no threshold was weakened.

The second complete `full.json` campaign ran as PID/PGID 2049038 (exec
session 83181), launched 2026-09-21T20:01:45Z with the pinned authority,
`ZSTD_ROUTE=3`, scratch-backed temporaries and `--stop-on-fail`. Parent suite
root is `/tanksmall/scratch/ictmp/experiments/icecream/integration/suites/20260921T200145Z-7ab937`
(smoke child `20260921T200145Z-9c01cc`). It finished PASS and its controller
exited; the independent replay above reproduces the full result.

## Follow-up: obsolete GRZ tests removed; native simulator built

After local commit `84656930`, the owner requested obsolete archived GRZ
tests be removed and the native simulator built. Five tests were removed:
four held-out preflight tests requiring the retired P29/GRZ runner contract,
and the matrix test requiring `GRZ_RESIDUAL` to be READY. Git history retains
them. These follow-up test/documentation edits are included in the local
release preparation; no push.

Built with `TMPDIR=/tanksmall/scratch/tmp sh cache/sim/build_p50sim.sh`
before editing the clean committed tree. Source: `cache/sim/p50sim.cpp`;
binary: `cache/sim/.p50sim.bin` (about 1.4 MiB); authenticated build receipt:
`cache/sim/.p50sim-build.json`. Receipt source is commit
`846569304e9fc8393e7beada6bdcaf15e071463e`; verified binary SHA256:
`ce164809ffca711cb449f4cf4cd129642791dba7b808f38efcec57efa3879ec7`.
Generated binary/receipt are ignored build artifacts, not source changes.

Focused rerun: **126 PASS / 10 FAIL**, including **all 12 native simulator
tests PASS** and all four remaining held-out preflight tests PASS. Eight
archived predictive tests still request unsupported `P29` instead of the
current `P29V1`; two route-matrix tests remain gated by historical build
receipt/source authority (including retired libbsc receipt fields).
The earlier assumption that a missing binary explained all 30 failures was
incomplete: building it uncovered these additional historical mismatches.
No product profile alias, GRZ support or weakened receipt checks were added.
Final JUnit: `/tanksmall/scratch/ictmp/research-layout-native.FIcHXR/native-built-final.xml`.
Command used canonical scratch TMPDIR, `PYTHONDONTWRITEBYTECODE=1`,
`PYTHONPATH=.:research/farmharness`, and `pytest -q -p no:cacheprovider`
over the heldout-preflight, method-matrix-simulator, multitu-predictive-producer
test files and `cache/sim/test_p50sim.py`. Validation has finished; no farm
workload started. S* qualification was still outstanding at that historical
checkpoint; the later full campaign above completed it.

## Research-layout work resumed and completed locally

The owner explicitly resumed the reorganization on 2026-09-21. All 113
relocated files are accounted for, with executable bits preserved. Historical
reports, experimental codecs, vendor provenance, statistical tools,
measurements and S4–S8 research/replay tooling now live under `research/`.
Current imports, script paths, documentation and distribution manifests were
updated. See [research/README.md](research/README.md) for the layout.

The production OnlineS1 header moved byte-for-byte to `cache/codec/`;
production include sites changed, not codec algorithms. Active acceptance
tooling and its resolver remain in `farmharness/`. This checkpoint accompanies
the local cleanup/reorganization commit; no push is authorized or claimed;
this is not new product-image or full-farm qualification. Validation and the
archived-test limitations are recorded below.

### Earlier stop: retained caution, not an active pause

The owner stopped work after discovering an inappropriate artifact-inventory
scan: `rg` was reading `/proc/*/{cwd,exe,fd/*}` as file contents rather than
comparing `readlink` targets. The helper was interrupted and its shell process
group 1953802 was terminated. A subsequent `ps -C rg` returned no processes.
Do not restart this scan. The earlier inventory's claimed absence of process references is
not reliable evidence; relocation byte hashes and retained old-path links are
independently verified, but no broader impact claim is made.

The interrupted helper was not restarted. The parent finished the partial
S4–S8 moves, corrected their imports/constructed paths, and validated locally.

## Source and authority

- Working checkout:
  `/tanksmall/MICKG2/mickg/src/mickg10/icecream-worktrees/sorbet_v1.5`, release branch
  `sorbet_v1.5`, based on cleanup commit `84656930`. No push or published
  release is claimed. Do not use the older, dirty integration checkout as
  the current source of truth or erase its changes.
- Qualified farm product: `b269fad98c296431b1f638a7fc9a0e61b500f195`,
  image `p50s4-diag-b269fad9`. Current catalogue base: `f98b69ac`.
- Farm: **tt-quietbox2, tt-quietbox3, tt-quietbox5 only**. q4, research6,
  and research7 are excluded. Historical reports naming them are not orders.
- Farm authority:
  `/tanksmall/scratch/ictmp/experiments/icecream/integration/full-e2556e79-authority-a7-no-r6.json`.
- Plan of record: `/tmp/report_on_how_to_finish_and_simplify.md`.
  Mutable spec: `/tmp/integration_full_spec.md`; check for updates during
  continuing work. External DeepImplementer communicates through
  <https://github.com/mickg10/icecream/issues/16> and is advisory, never a gate.
- Project handoff: `/tmp/resume_icecream_impleemnter.md` (spelling retained).
  `/tmp/resume.md` currently belongs to the unrelated r33 project. Do not
  overwrite it or create a workspace-wide resume file.

## Qualification, not just the latest green report

Evidence below is relative to
`/tanksmall/scratch/ictmp/experiments/icecream/integration/suites/`.

| Gate | Evidence | Current disposition |
|---|---|---|
| H1-H5 / S00 | `20260921T200145Z-9c01cc/SUITE.md` | PASS; H1-H4 are expected negative controls |
| S10-S60, including S30 mutant | `20260921T200145Z-c0bd78/SUITE.md` | Every child PASS, including all 14 S60 transitions |
| S70 | `20260921T200146Z-edc759/SUITE.md` | 7/7 PASS |
| S80 two-build | `20260921T200146Z-84647e/SUITE.md` and `PERFORMANCE.md` | 12/12 PASS, all four headline clauses PASS |
| Shaped S80 | `20260921T200146Z-2ea5be/SUITE.md` | 1/1 PASS |
| S90 | `20260921T200146Z-5a6376/SUITE.md` | 2/2 PASS |
| S95 disk-full | `20260921T200146Z-893b97/SUITE.md` | 2/2 PASS |
| Complete full campaign | `20260921T200145Z-7ab937/SUITE.md` | Full coverage PASS; exact recursive replay reproduced PASS |

Older failed campaigns and selected-only passes remain immutable history;
none was relabelled or assembled into the full result above.
P29 wins both turns under the specified measured-wall-plus-link-cost score at
1 Gbit/s and 100 Mbit/s; it is not the fastest measured raw wall time in every
comparison. S* qualification is **COMPLETE for the tested farm image**;
release packaging and publication are separate from this acceptance result.

## Cleanup decisions

| Area | Action / reason |
|---|---|
| Default image preparation | Use the seven labels referenced by the current scenarios, not the historical 14-image / 20-source-label lists. Include the current product, P43, H1's intentionally wrong image, and the four fault/skew mutants. Historical labels remain available through explicit `LABELS=`. |
| P50 compression | Keep exactly `P29V1=1`, `ZSTD_TU=2`, `ZSTD_ROUTE=3`; keep `OFF` as the legacy control, not a fourth codec. No new compression experiment or GRZ reintroduction. |
| Legacy compression | Keep LZO and zstd. `services/comm.cpp` uses LZO below protocol 40 and zstd from protocol 40 onward; P43 rollout compatibility depends on ordinary legacy transport. `OFF` is not uncompressed. |
| GRZ/libbsc research | Already absent from the production build/link/profile paths. Exclude the five archived vendor sources/provenance entries from the product source distribution. Keep their Git history, licences, tuple API and frozen test vectors; correct misleading old build instructions. |
| Runtime logs/traces | Keep scheduler assignment/login, C/F cache and source/result traces, mutex timing, transition/teardown and protected-process records. The verifier consumes these; they are not disposable debug output. |
| Completed result bundles | Keep successful AND failed bundles, checksums, image receipts, source/corpus authorities and replay dependencies immutable. No truncation, selective deletion, or rewriting a failed verdict. |
| Archived local build trees | Relocate only explicitly inventoried inactive trees from root-backed `/tmp` to scratch; retain contents and old-path symlinks. Never prune active services, Git objects, current images, or remote farm data. |
| Old S4–S8 research harness | Relocated together under `research/farmharness/`, including tests, schemas, guides and supervisor Dockerfile. Native regression/simulator consumers import the new paths explicitly. The live integration harness and resolver do not import this archive. No wholesale deletion based merely on an old codec name. |

New build/test temporaries must be scratch-backed:

```sh
export ICECREAM_WORK_ROOT=/tanksmall/scratch/ictmp
export ICEFARM_TMPDIR=/tmp/i
export TMPDIR=/tmp/i TMP=/tmp/i TEMP=/tmp/i TEMPDIR=/tmp/i
```

`/tmp/i` points to `/tanksmall/scratch/tmp`. Source and Git metadata belong
under the permanent `~/src/mickg10` layout, not `/tmp` or scratch. Do not
create another build tree on the root disk.

## Remaining work

1. This checkpoint accompanies the owner's local `sorbet_v1.5` / 1.5.0
   release commit. Publishing, new release-image construction and
   CI/bootstrap improvements require their own explicit follow-up.
2. Retain prior failure evidence, including S60-12 and the first S50 fairness
   failure; a later passing campaign does not rewrite their observations.
3. The archived research-test limitations below are not newly passing product
   tests and remain separately documented.
4. New-user farm bootstrap remains separate from S* completion. The current
   farm input is JSON, but `farm.example.json` and catalogue scenarios still
   embed operator hosts, paths and retained image/corpus records. Resolver v1
   is historical replay compatibility; current runs use v2 per-instance
   resolution, generated internally from farm/scenario JSON. Docker
   save/load deployment already exists, with compressed transfer and
   post-load image-content checks. The missing layer is a small operator
   farm JSON plus release-provided artifact metadata, host-role mapping,
   portable corpus/toolchain materialization and resource-bounded local or
   multi-host entry points. Do not claim that copying the current example
   alone bootstraps an arbitrary new machine.

## Research reorganization validation — 2026-09-21

- **101 focused live-harness tests PASS**: distribution/boundary checks,
  Make targets, image preparation, lineage, S30/S50/S60/ladder catalogues and
  S80 scoring. Tests guard explicit packaging, the production/research header
  boundary, and the active harness's independence from archived imports.
- **Four freshly compiled native codec regressions PASS**: provider, parity
  (11 inputs / 84 streams), intern and wire. Seven runtime-evidence tests,
  the production compile-wiring/deletion contract, layout-census verification,
  and four relocated CLI `--help` checks from outside the checkout PASS.
- All 113 old deleted paths have corresponding relocated regular files and
  preserved executable bits. OnlineS1 and the three archived experimental
  codec headers are byte-identical to HEAD. No changes to `services/`,
  `client/`, `daemon/`, `scheduler/` or `configure.ac`.
- Archived-tool/simulator sweep: **811 PASS, 34 FAIL**. An untouched HEAD
  archive has **807 PASS, 38 FAIL** under the same canonical scratch temporary
  directory. Comparing normalized JUnit test identities finds **zero new
  failing tests**. Four baseline-only failures involve Git identity in the
  archive extraction; they are not claimed as fixes.
- The 34 retained failures are four obsolete P29/GRZ runner-contract checks
  and 30 native-simulator-dependent cases (3 matrix, 15 predictive producer,
  12 simulator); `.p50sim.bin` is absent in both trees. No GRZ dependency was
  restored, no acceptance rule weakened and no historical failure hidden.
  Initial S5 rollback failures with symlinked `/tmp/i` disappear when using
  canonical `/tanksmall/scratch/tmp`; this existing path sensitivity is not
  repaired by a source reorganization.
- Test commands used `PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=.` and
  `TMPDIR=/tanksmall/scratch/tmp`, with `pytest -q -p no:cacheprovider` over
  `research/farmharness/*_test.py cache/sim/test_p50sim.py`. Baseline used
  `farmharness/*_test.py cache/sim/test_p50sim.py` from HEAD's archive.
  Retained JUnit results: `baseline.xml` and `relocated.xml` under
  `/tanksmall/scratch/ictmp/research-layout-native.FIcHXR/`.
- Automake generation PASS (existing `subdir-objects` warnings only);
  focused Ruff and whitespace checks PASS. No farm run or push. The owner
  subsequently authorized a local commit of this cleanup/reorganization.

## Earlier cleanup validation and artifact relocation

- Focused validation: **98 passed in 4.92 seconds** across Make targets,
  distribution manifests, image preparation, current lineage, S30/S50/S60
  and ladder catalogues, and S80 scoring. New default-label tests failed on
  the old lists before the fix. Explicit historical `LABELS=` still works.
- `ruff check --no-cache` on the changed tests and `git diff --check`: PASS.
  `automake --foreign --no-force`: exit 0, with warnings about the existing
  cross-directory source layout (`subdir-objects` is not enabled).
- During that initial cleanup, no C/C++ product source, configure dependencies, protocol/profile IDs,
  scenario, verdict or deadline was changed. No native-product rebuild or
  live farm qualification was performed. The subsequent header/include-only
  reorganization and its native regressions are recorded above.
- Relocated eight preserved archived build trees, **6,195,245,056 bytes
  (5.77 GiB)** previously allocated on the root filesystem. Before/after
  sorted-tar SHA-256 checks match for every tree, including metadata and
  relative symlinks. Root free space rose from **4.1 GiB to 9.8 GiB**
  (96% to 89% used). No contents were discarded.
- Destination and exact recovery receipt:
  `/tanksmall/scratch/ictmp/quarantine/icecream-build-cleanup-20260921.2JG8Rg/`.
  `relocate.sh` records the eight exact paths and expected hashes;
  `relocation.log` records all verified moves. Each original `/tmp` path
  is now a directory symlink to its retained scratch copy.
- Standalone scheduler logs, tarballs, saved executables, golden fixtures,
  result bundles, corpora and all remote Docker objects were left untouched.
  Local services 2564842 and 2816295 were excluded from cleanup. No active
  farm workload was started, no Git object/reference was deleted, and no
  commit or push was made.
