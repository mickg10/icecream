# Icecream: current state and cleanup boundary

Updated 2026-09-21 UTC. This is the current operational checkpoint; older
progress reports are history, not evidence that the current product passed.

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

- Working checkout: `/tmp/s70-harness-recovery-20260913`, base `f98b69ac`.
  Cleanup work is local to `root/finish-cleanup-20260921`; no push or
  release is claimed. Do not use the older, dirty integration checkout as
  the current source of truth or erase its changes.
- Current ordinary product: `b269fad98c296431b1f638a7fc9a0e61b500f195`,
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
| H1-H5 / S00 | `20260920-full-f98b69ac-a7-n001/SUITE.md` | PASS; H1-H4 are expected negative controls |
| S10-S50, including S30 mutant | `20260920-full-f98b69ac-a7-n008/SUITE.md` | Each child through S50 PASS; parent ladder later failed S60 |
| Original full campaign | `20260920-full-f98b69ac-a7/SUITE.md` | FAIL at S60-12; immutable, not relabelled |
| Fresh complete S60 | `20260920T220730Z-d97226/SUITE.md` | 14/14 PASS, separate selected rerun |
| S70 | `20260919T200400Z-s70-c4f6756e/SUITE.md` | Historical PASS; current-product rerun outstanding |
| S80 two-build | `20260920T225502Z-4e8652/SUITE.md` and `PERFORMANCE.md` | 12/12 PASS, all four headline clauses PASS |
| Shaped S80 | `20260919T234000Z-shaped-c4f6756e-a2/SUITE.md` | Historical PASS; current-product rerun outstanding |
| S90 | `20260920T000000Z-s90-c4f6756e/SUITE.md` | Historical PASS; current-product rerun outstanding |
| S95 disk-full | `20260918T222230Z-59bec2/SUITE.md` | ERROR; H5 passed but disk-full did not qualify |

S80's outer `full` report (`20260920T225502Z-524d64`) explicitly has
`Coverage: Selected`, `Selection: twobuild`. It is **not** a full-suite PASS.
P29 wins both turns under the specified measured-wall-plus-link-cost score at
1 Gbit/s and 100 Mbit/s; it is not the fastest measured raw wall time in every
comparison. The earlier claim that all S* qualification was complete is
withdrawn. Project status: **IN PROGRESS**, not blocked on external review.

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

`/tmp/i` points to `/tanksmall/scratch/tmp`. Keep the current checkout in
place during this pass; do not create another build tree on the root disk.

## Remaining work

1. Review the local cleanup/reorganization commit before publishing. Local
   validation is complete with the explicit historical-test limitations
   below; scenario semantics, deadlines and acceptance laws are unchanged.
2. Investigate the retained S60-12 failure despite the later green rerun;
   preserve the distinction between a transient cause and a proven fix.
3. Complete current-product S70, shaped S80, S90 and S95, then obtain the
   required complete full-suite qualification; do not assemble selected
   passes into a fictitious full-run PASS.
4. Before landing/pushing, resolve the missing Git object reported when
   enumerating other refs (`a92c5287a06346a74e5508191302287b8cd73ee0`).
   Current HEAD ancestry is readable. Do not run garbage collection or
   remove alternate object stores as a cleanup shortcut.

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
