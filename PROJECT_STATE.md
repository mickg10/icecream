# Sorbet 1.5.0: validation and remaining work

Updated 2026-09-24. Package version is **1.5.0**; the release branch is
`sorbet_v1.5`. The repository is public. The Docker bootstrap implementation
was published as `de027cefc31d79d062c3158400951916a9aa5d63`.
A pushed branch is not a published release tag or a newly qualified farm image.

Use [README.md](README.md) and [dev/README.md](dev/README.md) for setup.
This file records evidence boundaries and open work, not a chronological
agent log. Earlier diagnostic reports remain in Git history and their
retained artifact directories.

## Developer QA

### Independent-link pipeline candidate

`sorbet_1.5_pipeline` is a candidate, not a release or a new S* farm
qualification. Its first commit, `7fb131be`, contains the
[implementation and exact acceptance contract](doc/p50-transfer-concurrency.md).
Stage A removes C-wide source serialization in favor of bounded independent
C/F relationships: four active operations by default, one per exact F
incarnation across profiles, and 2 GiB of reserved source-vector lengths.
Retry setup no longer blocks the route-owner executor. CacheWire R1 and P43
bytes remain unchanged. Encoding remains owner-affine; persistent connections
and W30 are later, unimplemented stages. No corpus speedup is claimed.

The focused formal lane passed all 21 rows for both topology directions at
2, 3 and 4 stores, including simultaneous-running witnesses and six negative
controls. The final current-runner reproduction took 35.30s. The bounded
model is not a proof of arbitrary C++ execution or W30. Its retained results
are at `/tanksmall/scratch/tmp/p50-transfer-concurrency-final-SNpfoF/`.

The separate staged-source Python run passed **1,511 tests with 2 skips in
577.44s**, under the offline Ubuntu 24.04 SDK, Python 3.12.12 and pytest
8.4.2. Its source digest was
`3da3354b8ba4c131e432f6d1e24be68cf5fd85ca49208c029d3c390366f4c5f3`;
evidence is at `/tanksmall/scratch/tmp/p50-full-python-qa-2Uz5Ek/`.
The skips were an unavailable user/PID namespace capability and an absent
retained production fixture. The existing thorough Firefox authority test
took 490.370s of this run. A subsequent report-wording change passed its
focused **14-test** suite in 0.08s; it clarifies that summed per-operation
timings are not build wall time or CPU time, without changing scoring.

The four selected native targets passed under the strict Ubuntu 24.04 SDK
(GCC 13.3.0), including the concurrency, recovery and bounded-stop cases.
Restoring the global admission gate also failed the intended healthy-link
progress assertion in the separate negative control. Full native/root and
mixed-Docker acceptance are still being completed. The candidate has not been
uploaded. The release-baseline results below do not satisfy those new
candidate gates.

### P50 implementation cleanup

The local cleanup separates seven test-only reference components from live
code, removes retired adapter/dispatcher APIs and unused daemon state, and
moves control-operation encoding out of the header without changing its
wire format. Always-built cache archives decrease from 13 to 7. P50 C++
files in the production directories decrease from 89 to 78; reference code
remains under `unittests/support/` and is built only for tests.

The clean offline Ubuntu 24.04 SDK build/install passed on nas642 with four
jobs and a 16 GiB limit. Ordinary native checks passed **169 tests with 6
optional skips**; the separate root cache-service checks passed **2/2**.
The source snapshot digest was
`f7cf4d99249d8b550bd7b2ec3a5ec6ebcea7580006d6598cba717e2b05a37d6f`.
Evidence root:
`/tanksmall/scratch/tmp/icecream-deslop-final.dh5IUo/icecream-qa-8uj642kz/`.

That complete `make qa` attempt remains **FAIL**: Python reported 1,499
passes, 7 skips and one failure in its real-uv stale-lock negative test.
The negative fixture depended on registry metadata absent from the offline
SDK cache. It now uses a dependency-free project and an initially empty
cache, still requires the real stale-lock diagnostic, and verifies that
`--locked` leaves the lockfile unchanged. A separate fixture's scratch setup
is now independent of whether its helper created the directory first.

After those test-only corrections, the full offline SDK Python suite passed
**1,505 tests with 2 skips in 567.92s**, using Python 3.12.12. Evidence:
`/tanksmall/scratch/tmp/icecream-sdk-python-final/full.log` and
`/tanksmall/scratch/tmp/icecream-sdk-python-final/work/artifacts/pytest-full-final.xml`.
The corrected test files in that run matched the checkout. Native and mixed
results below use the unchanged product source from the clean build; these
complementary passes do not turn the original `make qa` receipt into a pass.

Separately, all five mixed Docker cases passed using the clean installed
product: P50 P29V1, ZSTD_TU, ZSTD_ROUTE, P43 worker and P43 client. Their
evidence is `mixed-recovered-all5/summary.json` under the evidence root above;
run ID `023e6f9d99de4f5092d30dd302a7397f`. Five Git-history tests also passed in
the real checkout; source-only snapshots skip those checks. Earlier failed
build/diagnostic runs were retained, not relabeled as passing.

### Earlier uv and portability evidence

The local uv conversion pins Python 3.12.12, pytest 8.4.2 and uv 0.9.21.
On nas642, its full integration-harness run passed **1,504 tests in 612.61s**
with `UV_OFFLINE=1`; the log is
`/tanksmall/scratch/tmp/uv-thorough-1790168208/pytest.log`.
The rebuilt Ubuntu 24.04 SDK completed offline, non-root native
build/install in 187.05s. Its result is
`/tanksmall/scratch/tmp/icecream-uv-real2.2JNKOK/icecream-qa-eg2lg4aa/result.json`.
The Compose image also built successfully and ran the managed Python
and pytest offline as both root and `nobody`. Those earlier checks did not
include full native/mixed QA or a new q4 portability trial.

The following complete QA result predates the uv conversion:

The Ubuntu 24.04 SDK was built on nas642 (Docker/ZFS), transferred with
Docker save/load, and used for a complete `make qa` on q4
(Docker/containerd/overlayfs). Four build jobs and a 16 GiB limit were used.

| Gate | Recorded result |
|---|---|
| Current source build/install | PASS |
| Ordinary native checks | 169 passed, 6 skipped |
| Separate root cache-service and sanitizer checks | 2 passed, no skips |
| Python integration-harness checks | 1,486 passed, 7 skipped; 54.43 seconds |
| P43 build from real 1.4 source | PASS |
| Strict P50 P29V1 / ZSTD_TU / ZSTD_ROUTE | 3/3 remote-compilation cases PASS |
| P43 worker / P43 client with current scheduler | 2/2 remote-compilation cases PASS |
| Complete q4 command | PASS; about 11m37s, excluding SDK preparation/transfer |

Tested source snapshot:
`3ce12ffb4c2e619537e5c3dbc78cb6e2c5038ee59da4e30d26d748ec759a8ead`.
The P43 source is `cd74801e0fa4e83e3ae254ca1d7fe98642f36b89`.
SDK archive SHA-256:
`3716f7ec66cfe71079e5ec259f2de8efcf8cac7adb35c82825857b233f1e162b`.

Retained q4 result:
`/home/mickg10/scratch/icecream/bootstrap-trial.3Ih5my/icecream-qa-5cxfzjy1/result.json`.
A local copy with native/Python logs and mixed-case evidence is at
`/tanksmall/scratch/tmp/icecream-q4-final-5cxfzjy1/`.
These are evidence locations on the test machines, not checkout prerequisites.

Six native skips were optional live/remote probes; the five mixed Docker
cases ran separately and required actual remote compilation. Python skipped
one unavailable user/PID namespace test, one optional retained-corpus test,
and five Git-history checks in the source-only snapshot. All five history
checks passed separately in the real NAS checkout.

The q4 trial used an offline Git-bundle checkout plus the exact edited source
snapshot. It did not test a direct GitHub clone. The repository subsequently
became public, but that change does not retroactively validate a clone.
Ubuntu 22.04, Docker Desktop, and a full QA run at the default two-job/8 GiB
setting are not covered by this four-job/16 GiB result.

On nas642, SDK/build/install, repository A/B selection, missing-repository
refusal, save/load and all five mixed cases passed. The earlier frozen full run
`icecream-qa-orefyqq0` remains **FAIL**: an auxiliary endpoint-script
`git diff` check ran in a gitless snapshot. Its correction passed separately
on NAS and in q4's full run. NAS root cache checks and the failed-fork guard
also passed separately. Those follow-ups are not a replacement full NAS pass.

## External S* qualification

The recorded full farm campaign qualifies source
`b269fad98c296431b1f638a7fc9a0e61b500f195`, image
`p50s4-diag-b269fad9`, on q2/q3/q5. It predates the release-version and
developer-bootstrap changes. Do not relabel it as qualification of newly
built 1.5.0 images.

Campaign: `20260921T200145Z-7ab937`.
Execution digest:
`62a9b251a06acf8e5e206d842abee58f173fe5c59292e8e6527a729063c5ae45`.
A read-only recursive replay reproduced PASS on 2026-09-22.
Evidence roots below are relative to
`/tanksmall/scratch/ictmp/experiments/icecream/integration/suites/`.

| Gate | Evidence directory | Result |
|---|---|---|
| H1–H5 / S00 | `20260921T200145Z-9c01cc` | PASS; H1–H4 are expected negative controls |
| S10–S60 | `20260921T200145Z-c0bd78` | PASS, including all 14 S60 transitions |
| S70 resilience | `20260921T200146Z-edc759` | 7/7 PASS |
| S80 two-build | `20260921T200146Z-84647e` | 12/12 PASS |
| Shaped S80 | `20260921T200146Z-2ea5be` | 1/1 PASS |
| S90 revision skew | `20260921T200146Z-5a6376` | 2/2 PASS |
| S95 disk full | `20260921T200146Z-893b97` | 2/2 PASS |

The campaign authority is
`/tanksmall/scratch/ictmp/experiments/icecream/integration/full-e2556e79-authority-a7-no-r6.json`.
Its host selection is not a universal deployment requirement. q4 was a separate
developer-QA trial, not an added worker in this campaign.

P29's recorded win is under the specified measured-wall-plus-link-cost score
at 1 Gbit/s and 100 Mbit/s, not fastest raw wall time in every comparison.
Prior failed campaigns remain failed; no acceptance threshold was relaxed to
construct this result.

## Formal and historical-tool boundaries

[TLA+/TLC documentation](cache/formal/README.md) describes the selected bounded
models and expected-failure controls. Native/Python/Docker success is not a
fresh full TLC run, and model-check success is not proof of arbitrary C++
execution or an unbounded deployment.

[Research tools](research/README.md) retain replay consumers and frozen codec
fixtures. Their old GRZ/P29 predictive workflows are not current product
profiles. The last recorded focused historical simulator/replay run had
126 passes and 10 failures: eight still requested the obsolete `P29` selector,
and two depended on historical build receipts. They are not newly passing
product tests. Details remain in the pre-cleanup Git history and
`/tanksmall/scratch/ictmp/research-layout-native.FIcHXR/`.

## Remaining engineering work

- Portable multi-host qualification: separate small operator geometry from
  retained image/corpus authorities, with explicit host-role mapping and
  migration tests. Root `farm.json` currently drives local Docker QA only.
- Improve bounded cancellation of owned remote workload processes and observe
  event failures while commands are outstanding.
- Validate only selected corpus artifacts at execution while preserving all
  required checks on those selected inputs.
- Replace scenario-name policy switches with explicit execution policy;
  separate large event/collection/verdict modules and extract remaining
  embedded event scripts without changing historical replay behavior.
- Version receipt contracts and clock domains explicitly, including legacy
  128 MiB versus current 512 MiB assumptions and request/start/completion times.
- Modernize CI around the existing QA entrypoint and resource presets.
  Legacy CI files are not evidence of current 1.5 coverage.
- Produce and qualify release artifacts from the selected release commit;
  follow [ReleaseProcess.md](ReleaseProcess.md) and
  [S1B_EXIT_MANIFEST.md](S1B_EXIT_MANIFEST.md).

Keep source/Git storage separate from disposable build scratch. Preserve
frozen fixture bytes, completed result bundles, image receipts and their
source identities when cleaning documentation.
