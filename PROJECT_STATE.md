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

### Persistent-link implementation in progress

The production C sender passes its W30 regression against the direct F
endpoint for ZSTD_TU, P29V1 and ZSTD_ROUTE: 30 complete bundles and 30 F commits before C processes
receipts, then cumulative ACK and ordinal-31 refill on one connection. Exact
and conflicting duplicate-request checks pass, along with the existing sender
tests. The deliberately small socket buffers take about 22 seconds within the
original 30-second deadline; this is not a throughput benchmark.
The clean candidate is based on `70e12c53`, changes only the test, and contains
no recovery-only endpoint APIs. Each materialized input is checked against
the submitted bytes, profile, sequence and digest. Evidence:
`/tanksmall/scratch/tmp/p50-sender-w30-profiles-70e12/artifacts/run-r2.log`
(SHA256 `fb0013bea2a5bdc04f9be4102629e1782a01802fd52264a1752d735667bf75d7`).
Test binary SHA256:
`a4e0fafa6f29202c865eded6c5ab1e7f15e2ce9c5a6cb35be8c2727d82b1196c`.
This does not qualify wrapper/daemon integration, recovery, restarts,
multi-link topologies, or mixed farm operation.

The direct topology regression also passes C1F2, C1F3, C1F4,
C2F1, C3F1 and C4F1 for each of P29V1, ZSTD_TU and ZSTD_ROUTE
against the committed production sender/endpoint (18 cells).
The shared-C cases use one preparation authority; shared-F cases use one
endpoint/store. Every link reaches 30 sends and 30 F commits before any ACK,
then reaches 31 commits and cumulative ACK after refill. Aggregate pre-ACK
occupancy is 60, 90 or 120. Exact input bytes and digests are checked.
Evidence: `/tanksmall/scratch/tmp/p50-w30-topology-20260924/artifacts/run-profiles-r1.log`
(SHA256 `00f8889e16998f67a60bece78ec32ef10271dba2880789b4b53e9de160697d29`).
This does not prove service-worker saturation, recovery, restart behavior,
or mixed farm operation.

The dormant recovery record codecs now pass the focused `p50wire` gate:
exact payload sizes and round trips, truncation/trailing-byte rejection,
invalid subkinds/ordinals/intervals/epochs, and transcript sensitivity to
changed witness bytes. Evidence:
`/tanksmall/scratch/tmp/p50-recovery-closure.avtx9r/logs/p50wire-recovery-r1.log`
(SHA256 `e52b1bd8ba96eb0b174bb3b3a7d708581b3f4107ecf52f886a8f2b686d7782a1`).
That wire test proves record encoding/decoding only.

ZSTD_ROUTE recovery preparation now advances its bounded speculative history
and REL_SEQ after each successfully rebuilt suffix entry. The regression
checks three retained entries against fresh sequential preparation: exact
encoded bytes, pre-state digests, REL 0/1/2 and unchanged raw identity. An
out-of-order rebuild is rejected without consuming the cursor. Focused
evidence: `logs/zstd-route-recovery-authority-r2.log` under the recovery root
(SHA256 `d41cc3deb4f51de335528dee08dd5eab7b8b029760eb300c7268c98a8b4aafbc`).
The full endpoint suite also passes in
`logs/route-recovery-endpoint-and-sender-full-r1.log`; its endpoint binary is
`b2c85b8b6c6991066dd6a6186f0368471cbf6b917ff08a43a370287974dfc61c`.
The subsequent sender suite in that combined run fails at the first-bundle
wait after its ZSTD_TU W30 case; the combined run is not a passing sender gate.

The direct production sender recovery regression now passes for ZSTD_TU:
the original caller completes after a lost commit reply, failed connector,
and lost RESET_ACK; a separate case completes after two interrupted
materializations and two resets. Exact input identity is preserved, with one
successful materialization. The same binary passes the full sender suite,
including W30 for all three profiles. Evidence:
`/tanksmall/scratch/tmp/p50-recovery-closure.avtx9r/logs/p50zstdsender-full-r11.log`
(SHA256 `81c06e890ba6a090933b082cd7dd6364c937619eeff3c691fd55838210e4931c`).
Binary SHA256:
`d1f3319e4c25d78e503495b90fab60d37c5b8f233ceb3aff3644dd7dc5afbcc0`.
The same source closure passes `p50slice0` (including exact interrupted-install
retry and resource accounting) and `p50endpoint`. Endpoint log:
`/tanksmall/scratch/tmp/p50-recovery-closure.avtx9r/logs/recovery-endpoint-r3.log`
(SHA256 `a9c299f9195e67f9c64f8afa1b52ac4201f88d7e2635fff03a448ed80f953b10`).
Both 2 and 30 concurrent ZSTD_TU callers pass shared-link recovery: all initial
bundles are sent before the first commit reply is lost, F retains prefix 1,
and one reconnect recovers that receipt and replays only the unfinished
suffix (29 jobs in the W30 case). All original callers complete; each exact
input is committed once. These fixtures use distinct 128-byte inputs and do
not measure large-payload recovery throughput.
The full sender suite, including this regression and bounded teardown, passes.
Evidence: `logs/p50zstdsender-full-shared-failure-r3.log` under the same root
(SHA256 `efdfe14176d8da8f328faaf8c210366f055583655e2f42ea6713c399d21cd2e7`).
The expanded direct sender suite now passes both 2 and 30 concurrent callers
for each of ZSTD_TU, P29V1 and ZSTD_ROUTE. The replacement reader, ACK writer
and original callers are fenced by physical-link generation so an old failure
cannot close or clear its successor's state. Original callers share the same
recovery loop, rather than requiring a new request to trigger progress.
Every shared-failure cell verifies all inputs committed exactly once, ACK
through the final ordinal, and exactly two connections. Inputs are small
generated source fragments, not a corpus throughput test.
The full suite also passes healthy W30 for all three profiles and the prior
single-caller recovery cases. Evidence:
`logs/sender-w30-diagnostics-r2.log` under the same recovery root
(SHA256 `392024009e370355a9e9d7ffdefac92d7a9ae0b6ac1d9b1d85df4d78cc1578f9`);
binary SHA256 `e339bf425be0901c75e13f0d7f7c8a0392e4c5c6ac00eac6862a06d7a8dec3be`.
This run took 93.96 seconds. An earlier first-bundle timeout did not reproduce
in the subsequent full runs; its cause remains unresolved and bounded failure
diagnostics are retained. This does not qualify repeated shared-link failures,
service integration, cancellation/expiry, or process restart behavior.

The P51 source-control codec now restores the ARM identity before validating
ARMED metadata and accepts a selected window from 1 through the offered
window. Focused request/reply round trips pass for offer 30 with selections
1 and 30; invalid selections, revision and request identity are rejected by
the encoder. Evidence:
`/tanksmall/scratch/tmp/p51-service-own.ntIYnn/codec-r3.log`
(SHA256 `36fa0b4edd65ca7554ec1e323b4019176eec77ce4eab0aa15569fea4512a9979`).
Additional codec cases reject mutated reply windows 0/31 and revision 3 at
decode time, and round-trip a committed empty TU. Evidence:
`/tanksmall/scratch/tmp/p51-service-own.ntIYnn/tmp/p50daemoncontrol-codec.log`
(SHA256 `7224f20d8c478b7d52c38a684f8f4f0d8d7dbd46e99202317b8ac6fa48ef6c34`).

The annotated tag `sorbet_1.5_pipeline_plan` freezes the detailed C/D plan at
`6dfd606de89bd60abb9315b398b53df37c878640`; the tag is pushed and its remote
target was verified. It is a plan checkpoint, not a W30 release.

Luna lanes implement product changes and run tests; primary owns architecture,
review and integration. The uncommitted integration candidate now passes a
one-job vertical test with distinct real C/F daemons and sidecars. The test
uses the assigned GetCS/UseCS local channel for the C lease, negotiates R2
window 30, commits exact TU0 input through source-control kind 8, and obtains
an accepted input attachment through CompileFile on the original F channel.
Both daemons exit cleanly. This exercises test-driven ordinary messages, not
the actual compiler wrapper, and does not prove 30 concurrent transfers or a
successful compiler output. Endpoint shutdown reports status 1 after one
commit; this is not evidence of a graceful CacheWire CLOSE exchange.
Evidence: `/tanksmall/scratch/tmp/p51-vertical-logs/p51-vertical-r5.log`
(SHA256 `765640ef4ab1964cd0522a53c844aa8f442bde65bfd4092439c2f21357d2d9cf`).
The candidate corrects source-control decoding and removes Unix-only
credential requirements from the public TCP auxiliary-link admission check;
the exact live connection lease and pristine-channel checks remain required.
Full service concurrency, recovery, restarts, wrapper and mixed-farm gates
remain open. The Stage A results below do not qualify these changes.
No Chromium build/download has been started for this implementation step.

The separate async-service candidate passes the full `p50cacheservice` suite
against the committed sender/endpoint. Added cases cover local reply deadline
and peer closure, slot reuse, the global 120-reservation limit, cancellation
and idle expiry, publication/reset lifecycle, and shutdown while an accepted
connection stalls before ordinary protocol admission completes. That last
case verifies both operation and raw-byte credits return to zero. A second
case completes ordinary protocol admission, observes the link-session request,
then withholds its reply; shutdown also returns both credits in that phase.
The optional cancellation-aware handshake polls do not extend the original deadline.
A third case observes R2 LINK_HELLO and withholds LINK_STATE. Whole-runtime
shutdown retires the C sender on its owner executor and returns both credits.
These tests do not yet prove cancellation during body/recovery or 120 active
transfers. This service snapshot still uses the earlier committed sender and
endpoint, so combined qualification with the latest recovery changes remains open.
Evidence: `/tanksmall/scratch/tmp/p51-service-own.ntIYnn/service-posthello-pass-r3.log`
(SHA256 `5904a3ee47fb2f57d1d79a31a65ab2240072f2771bb824b4a6991b4575b3c930`);
test binary SHA256
`56222f849e95f7b78a7c88770dae3b2ee0b78919d51b37c8fc04ef7c8f661cf1`.

The focused `run_adopted_r2` receiver fixture passed two sequential ZSTD_TU
jobs on one TCP link: one W1 HELLO, distinct one-shot reservations, two
complete transactions with commit/ACK, exact materialized input bytes and an
idle CLOSE. This uses a test peer, not the production C sender or compiler
wrapper. Evidence:
`/tanksmall/scratch/tmp/p50-r2-codec-snapshot-20260924/tmp/c1-r2-endpoint-fixture-run-r3.log`
(SHA256 `9f892ba1282d5459b46db350bcd4fb3f5d9ebf477b0a3dfc2a2ed35373f41e60`).
Test binary SHA256:
`f39626002fc634490f91d1531323402be6864cbc9817c944d8ef51e2540a884b`.
The tested endpoint source snapshot is
`411c78435893110f04683a7e835c8244be4708a81b8350c703d9e23aac243895`;
ongoing integration edits are not covered by that snapshot.

The next dormant implementation checkpoint includes ordinary P51 source
control and descriptor handoff, persistent endpoint plumbing, bounded
reservation/receipt storage, and F-side window admission up to 30. It does
not enable production R2 advertisement or qualify C-side W30, recovery, or
restart behavior. The frozen source is
`/tanksmall/scratch/tmp/p50-r2-w30-snapshot-20260924/source`.
Its services, sidecar, daemon, client and scheduler compiled and linked;
the full make invocation subsequently failed on test-build temporary-file
permissions. Targeted builds and runtime checks were then run after fixing
that environment issue. Source-arm validation (R2 empty input, R1 empty-input
rejection and unsupported-revision rejection) and the full cache-service
suite passed. Evidence:
`/tanksmall/scratch/tmp/p50-r2-w30-snapshot-20260924/tmp/c1-sourcearm-service-run-r1.log`
(SHA256 `99b6041f997bc59b899018934fcb36a03280a926ccafd87b492db4eba05ce829`).
The same snapshot also passed both direct-F endpoint checks: two sequential
exact jobs on one connection and silent-before-HELLO incarnation cancellation.
Log: `tmp/c1-endpoint-current-focus-r1.log` under the snapshot root above
(SHA256 `85adb880dfe4f779bc5c775d544bf766b29ecdc4954ee9d222243d3f4920bb4c`).
Endpoint binary SHA256:
`550d0e63db95e1ea7e723637d5f72c57a72aac99845ca9af8c9cf4c730877871`.
Earlier stale-snapshot failures are not passes; these targeted results close
those checks only. Newer independent C reader/writer and recovery edits are
separate work and must obtain their own build/runtime evidence.

The direct endpoint W30 test passes for ZSTD_TU, P29V1 and ZSTD_ROUTE on a
clean `959fee2c` overlay containing the R2 P29 NEED-transition fix and the
generalized regression test. C writes 30
complete bundles on one TCP link; F commits all 30 before C reads any
receipt. C validates the ordered receipts, sends cumulative ACK 30, then
prepares and sends job 31 on the same connection. Exact input bytes for all
31 jobs, ACK 31 and clean CLOSE are checked. Preparation capacity is 30;
the refill input is not prepared until the first window has drained.

This is **three-profile direct C/F endpoint evidence only**. It does not
qualify the production multi-caller sender, recovery, restart, mixed-version
operation or farm performance. P29 derives its local NEED state after BODY,
without sending a NEED frame; waiting for FILL-dependent completion here
previously prevented P29 commits. Evidence:
`/tanksmall/scratch/tmp/p50-p29-clean-959fee2c/work/artifacts/focused-build-test-r3.log`
(SHA256 `2b916738daac2e9384a256ac8234b8e62cabd56e1b9609936153935228ec2f10`).
Test source SHA256:
`a091cfea40cfdf62af6ffdc86ff3b51aa4afe01d89101c3b8683c54621e6bfba`;
binary SHA256:
`25a54f9566b954bbe4ca0bf650378c8aa1fff324fc297feef07a82fd2e7ef355`.

The first version-gate audit preserves ordinary maximum 50 and existing R1
bytes. Focused `p50sourcearmwire`, `p50cachesessionwire`,
`p50cacheadvertisement` and the source-arm source guard passed in 92.50s;
the affected communication library and linked tests were rebuilt. Evidence:
`/tanksmall/scratch/tmp/p50-stageA-C0C1-20260924.ZJvsRb/tmp/c0-replay.log`
(SHA256 `9babd192070d127c64e4190567875e019354acfd6478839ad224def699a8f3b4`).
This is a focused compatibility gate, not a daemon/client build, full QA or
persistent-link test. An earlier make-check attempt failed because read-only
source staging tried to regenerate build files; direct targets and the source
guard subsequently passed. On actual protocol-51 enablement, legacy R1 must
remain explicitly selectable; interim exact-50 guards are not a permanent
prohibition against the planned new-peer legacy path.

A separate bounded C/D model lane passed 19 rows: eight clean checks and
eleven exact expected counterexamples (six deliberate faults and five
reachability witnesses). It includes both topology directions at 2/3/4,
sent-window and refill witnesses, bounded recovery and a separate W30
accounting projection. Evidence:
`/tanksmall/scratch/tmp/p50-pipeline-recovery-final6/runner.log`
(SHA256 `8b29b46e3cb5258ec00b23ce49ae1e21377008baffca98618d522af4977503ad`).
The model is not a production receiver-validation proof, a complete restart
model, or a runtime W30 test. Full epoch/digest validation, lost-confirmation
recovery and product correspondence remain runtime/model follow-up gates;
see [formal scope](cache/formal/README.md). Persistent runtime integration
remains unqualified; the narrower component gates below do not establish it.

The dormant P51 ordinary codecs and version-4 local descriptor exchange have
passed a focused component gate. Normal negotiation still has maximum 50;
the tests explicitly construct protocol-51 channels. R1 source-arm,
cache-session and result-disposition records remain selectable there, while
P51-specific records reject protocol 50 and unsupported versions. Version-3
descriptor bytes are retained; version 4 adds the exact C store identity and
tests one-shot request binding, malformed replies and nonblocking send failure
under backpressure. These are component tests, not a working R2 data link.

Evidence under `/tanksmall/scratch/tmp/p50-stageA-C0C1-20260924.ZJvsRb/tmp/`:

| Log | Result and scope | SHA256 |
| --- | --- | --- |
| `c1-wire-runtime-r9.log` | source-arm, fd seam, result disposition and source guard passed; 43.29s including Docker wrapper | `5127a19b8df5eb9598a2d1e208abae2617e3760adcc4e3bdb8411fb40221579b` |
| `c1-sessionwire-r2.log` | cache-session compatibility passed; 4.44s | `dc97d312ac6a216b7351d922d4b357cdd0bf822fa198ceab8c16498d2f37a7d3` |
| `c1-advertisement-r1.log` | advertisement fixtures passed; 64.08s | `df65edff3ac274ca5c6bfda594196fa9d1c099840254687e82a5d4141de4a218` |

The tested communication source hashes are `comm.h`
`7381bce245c7f0429d99110606caa4e298859da3405c4058b568f9fdfb5206e0`
and `comm.cpp`
`0d02256a91d58e55405ce01355e3b6a6a96deb3cc57923cdc92788bbf1b89357`.
Advertisement linked the retained pre-D1 cache library
`0d15cb87e814ae5c86aeaa10049d2dff7ea72d46109199452dfa2b06956c51d0`;
it does not qualify concurrent P29 speculative-preparation edits. Earlier
attempts hit read-only Automake regeneration, a misspelled build target, and
test fixtures requiring updates for explicit 51 coexistence; those attempts
remain failed, not rewritten as a clean full-suite run. The draft asynchronous
daemon integration compiled separately but has no runtime qualification yet
and is not included in this codec checkpoint.

### Reservation-service snapshot gate

The isolated development snapshot passed the complete `p50cacheservice` run
with v7 reservation tests: exact duplicate at capacity, fixed relationship
window/profile, C-incarnation mismatch, and expiry freeing bounded capacity.
The first reservation fixture incorrectly used a paired local C/F identity;
it was corrected to use an independent remote C. A later run exposed an
existing stop-opening fixture race: it launched the queued contender before
observing the first operation's ARM barrier. The corrected ordering preserves
the same timeout and refusal assertions.

Passing log:
`/tanksmall/scratch/tmp/p50-r2-codec-snapshot-20260924/tmp/c1-r2-service-run-r3.log`
(SHA256 `d48bc2d76f24e343213c241244dcfa3467fabaa90a00a838ab7b82071f461a79`).
Test source SHA256:
`7bd472199565f0d1723eb975850fe95b69e183ab72cf7a3d9b5014dd7d188473`;
binary SHA256:
`2cda98e3e59a88ff6b4c9a4eeb1bad575df4aa9c4d8d76a4f9908fd784f13e6b`.
This snapshot gate excludes newer link lookup/consumption callbacks, the
persistent receive loop, C sender/wrapper integration and recovery. Those
development changes remain unqualified and are not covered by this pass.

### CacheWire R2 record component

The dormant R2 record codecs and exact wire table are implemented. The focused
`p50wire` test passed in the bounded SDK container from the isolated source
snapshot `/tanksmall/scratch/tmp/p50-r2-codec-snapshot-20260924/source`.
It covers fixed sizes/offsets, round trips, truncation/trailing-byte rejection,
distinct R1/R2 BODY/FILL types, empty-source binding, and an independently
constructed digest transcript with byte/length/type/binding mutations.
The tested binary SHA256 is
`11783b4482799f443fcdaa96dacceab3a4ffe41c08516f12a3459e14a5f0ac2d`.
Test source SHA256 is
`ef45bf41966dd246a3795af62d8c394c833988514cb3a1213d0e1ba876e716bd`;
protocol header/source SHA256 values are respectively
`42c04da7acfa7ffd09ca3b64a6d2caf7348203f672168525dfa0f26fed1655ba`
and `0a8e4d806839444a2f56a7c0b4a4655b321206d386067b46f6f3e50c35ec40da`.
The test exits silently on success; its retained runtime log is empty.
Initial combined builds failed on stale dependent archives and a cancellation
codec type error; those attempts are not passing gates. This wire-only gate
does not qualify reservation service behavior, live persistent sessions,
recovery, or W30. Ordinary negotiation remains capped at 50.

### Speculative P29 codec component

The P29 speculative-codec component now has a focused passing gate: 30 TUs
are encoded and materialized by the real F store before C accepts its first
receipt. Predicted NEED equals F's derived NEED; FILL is built from the
prediction. Ordered receipt validation advances a separate confirmed cursor,
rejects wrong or reordered receipts, and releases bounded count/raw credits.
A separate reset fixture commits and acknowledges one TU, stages two unsent
TUs, then rebuilds only that uncommitted suffix with the same TU identities
and a fresh codec history. It rejects an old-history receipt. These are
in-process codec/store tests, not evidence of 30 bundles sent over a persistent
socket or of lost-reply recovery.

`p50slice0`, `codec_wire` and `p50cacheadvertisement` passed together in
66.35 seconds under the existing bounded SDK container. Runtime evidence:
`/tanksmall/scratch/tmp/p50-stageA-C0C1-20260924.ZJvsRb/tmp/c1-d1-runtime-r2.log`
(SHA256 `42d1da015801f149a9858b3c6d39063ea147451234b68151a45804dabf5baf75`).
The initial focused build succeeded but its runtime command used the wrong
working directory for codec fixtures; that attempt remains failed. The
corrected command ran the same binaries with the fixture directory resolved.
Concurrent daemon, local-reservation and preparation-authority integration
changes are excluded from this component gate.

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

The tested implementation is `be02ac858fc01b60ef380b7c06554bbcd91ec2f8`;
`bb1cb0c75b6d960a3beb40e4e24e5b92494411a6` adds only an explicit test-helper
write-result check required by the stricter sanitizer build. Product source
is identical between those commits. Native checks used the Ubuntu 24.04 SDK
with GCC 13.3.0, four CPUs and a 16 GiB memory limit.

| Candidate gate | Result |
| --- | --- |
| Native suite, including corrected rechecks | 169 passed; 6 optional skips |
| Separate root cache-service and sanitizer checks | 2/2 passed; 174.91s |
| Local mixed Docker: P29V1, ZSTD_TU, ZSTD_ROUTE, P43 worker, P43 client | 5/5 passed; actual remote compilation required |

These are complementary runs, not a single clean `make qa` receipt. The first
full native invocation reported 160 passes, 6 skips and 9 failures. Eight
failures came from its noncanonical container/staging setup (child reaping,
the required descriptor-check capability, source permissions and copied Git
metadata); the ninth was the test-helper compile error. All nine failed
targets plus the updated ordinary cache-service test then passed in the
corrected **10/10** rerun in 198.54s. No assertions were relaxed. The original
invocation remains recorded as FAIL. Restoring global serialization also
failed the intended healthy-link progress assertion in its negative control.

Native logs and the combined, source-bound evidence index are retained at
`/tanksmall/scratch/tmp/p50-full-qa-be02ac858/`. Mixed results are at
`/tanksmall/scratch/tmp/p50-mixed-pipeline-jlq0YJ/mixed/summary.json`, run ID
`df02604178a746ffb3840772073b3198`. The mixed image contains the tested product;
its source label uses the developer snapshot digest, not the distinct Git
archive digest. Detailed identities and limits are in the evidence index.
The local mixed run is not a renewed external-farm qualification or a
separately executed old/new-P50-binary rolling-upgrade test. The release-baseline
results below are historical and were not substituted for these candidate gates.

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
