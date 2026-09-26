# Sorbet 1.5.0: validation and remaining work

Updated 2026-09-26. Package version is **1.5.0**; the release branch is
`sorbet_v1.5`. The repository is public. The Docker bootstrap implementation
was published as `de027cefc31d79d062c3158400951916a9aa5d63`.
A pushed branch is not a published release tag or a newly qualified farm image.

Use [README.md](README.md) and [dev/README.md](dev/README.md) for setup.
This file records evidence boundaries and open work, not a chronological
agent log. Earlier diagnostic reports remain in Git history and their
retained artifact directories.

## Developer QA

### P29 pre-FILL cancellation foundation

Commit `491fb851` adds explicit serializer/route cancellation before FILL
starts, preserving continuing entropy state and earlier speculative witnesses.
The existing broad abandon/release behavior is unchanged. A fill-started flag
rejects this narrow operation even when FILL throws before becoming ready.
This is the codec foundation, not yet the integrated source-request cancel path.

Luna ran `p50slice0` and `codec_wire` in the SDK; both exited 0. The route
test retains an unacknowledged predecessor, cancels the next BODY preparation,
then checks successor REL_SEQ/history continuity, exact F output and ordered
receipt drainage. The direct serializer test uses real/predicted NEED parity
and an injected dictionary exception during FILL. Existing 11-TU golden
comparison passes (44,745,525 raw bytes, 126,016 regions).
Logs under `/tanksmall/scratch/tmp/p51-codec-abandon-build/runtime/`:
`p50slice0-final.log` is empty on success;
`codec-wire-final.log` SHA256 is
`4deb001c42d451bf11482ba47d5a1381c2ce159748640ab71eb3322fbbf8d1a2`.
Final codec test source SHA256:
`bb01e00543e74301c76e3afec6e4bd89179fe6c476f75138c19a8ee438af6647`;
binary SHA256:
`b439fc3b1f7e17565ed552366fcffe8b584d663e57d07e72f6a7d98e9d7f38fc`.
Authority validation, request-specific monitoring and the 31-request service
cancellation cases remain separate qualification work.

### Source admission pressure

Commit `983c64ab` implements typed CapacityBusy before source read/route work,
with a response budget of at most 100 ms clipped to the original deadline.
The compiler wrapper retries only completed, validated Busy responses using
the retained immutable source, fresh control leases with the same C identity,
the original request/deadline and one F ARM. The operation cap remains 120.

The registered cache-service test passed; bounded wrapper tests exercise a
cap of one, not W30 occupancy. After integration, the merged wrapper passed
syntax, adoption fixtures, incompatible-mode rejection, a 16-TU exact-output
positive batch (six Busy responses for one job), changed-C identity rejection,
and terminal non-Busy/no-retry behavior. Compiled product sources matched the
tested donor. Logs in `/tanksmall/scratch/tmp/p51-w30-capacity-runtime/`:

- `p50cacheservice-registered-target.log`: `5afeafe7d3636ee88257bc6c63836cea38a69599630ee83e43e0c89fdc7d5d31`
- `merged-wrapper-adoption-selftest.log`: `da18cf0f6767cb9e33757b84c542d07bc64da2a76258dd6a07b1868d0aef3080`
- `merged-wrapper-capacity-positive.log`: `34bd63456d9f68fefc8701c36a77379bcf696d4ca20014dd2316e7b07b9c4a3b`
- `merged-wrapper-capacity-identity-negative.log`: `e92bcd6f9dda1dd5d8a910ab5974d0884b36926c1b13b9985289e1e0410c4f63`
- `merged-wrapper-capacity-nonbusy-negative.log`: `4718d13c388c4b11b887d78832cdfb87a04ca819e61167f9d53946e97c37e028`

The cap-one service admission test now repeats the exact refused request 64
times with fresh control pairs and CLOEXEC source duplicates. Every temporary
attempt peaks at baseline + 3 descriptors and returns to baseline (17 in the
recorded run), with one held operation, zero raw bytes and no source reads.
After settlement and exact-request readmission, operation/raw credits are zero;
the remaining 13 descriptors are an identity-matched subset of the original
set. Snapshot enumeration errors fail the test. This is service-level coverage,
not wrapper/daemon fresh-lease resource instrumentation or W30 occupancy.
Donors `6cf2363b` and `a63902a7` passed the focused
`p50cacheservice --p51-capacity-overflow` selector on the final source.
Log: `/tanksmall/scratch/tmp/p51-capacity-plateau-build/runtime/p51-capacity-busy-plateau-strict-snapshot.log`,
SHA256 `06ea43b036c5a85110223b11ba0b8e6eb7f23aba36b6627456323e841eb60cf7`.
Source SHA256 `30337168820eaa429fa143355350b0725221f259a12dd066c921f21f69b70803`;
binary SHA256 `97570d969ae033667f220a1a82aa9ee5efed38b77feda01998b980fc606dbb10`.
An earlier fixed-count cleanup assertion failed because settlement closes
fixture-owned descriptors; that failed diagnostic log remains retained.

Commit `24ffd753` adds the real-wrapper capacity-expiry gate, passing separately
for P29V1, ZSTD_TU and ZSTD_ROUTE. Hooks are compile-time test-only and armed
after warmup. One exact assignment receives Busy, then waits for a deliberately
withheld fresh control lease until its unchanged original deadline. It has
one ARM, no terminal transfer result and no F input-attachment/compiler-start
record through the post-release observation window. The ordinary wrapper may
retry with a new assignment: the test verifies its distinct identity/deadline,
one ARM and exact output, then a separate unrelated compile succeeds after
capacity release. A failed assertion that required the whole invocation to
fail was corrected; successful reassignment is not late success of the expired
operation. Earlier failed setup/parser runs are not passing evidence.
Logs in `/tanksmall/scratch/tmp/p51-capacity-deadline-build/`:

- `expiry-p29-final2.log`: `421befa554a9daaf252c1354052bb3ba16cfd6b6fd88c99dad95cbd81375b57f`
- `expiry-zstd_tu-final.log`: `9abe817e21abb6ac6a7a74bc2694b6b82674fb361855377876a983ee262b1953`
- `expiry-zstd_route-final.log`: `2ef2a5e2d6a1ad569e6cd2bd4c3ef528ae60a524d65a4c26a78a4d4b0109a694`

The registered service pass preceded the final default-off terminal-error
hook; focused service and wrapper checks passed afterward. Wrapper-level retry
resource plateau, actual
four-link W30/reply-settlement overlap and latest-candidate full QA remain open.

### Lost RECOVER response retry

Commit `880fb9c8` extends the scenario below to both W2 and W30 for all three
profiles. It verifies A=0/P=30 witnesses at W30, retained K=1, the exact
normalized response and committed row, successful remaining 29 callers,
RESET confirmation and zero preparation credits. The W30 sanitizer fixture
uses a 60-second absolute deadline; this is not a performance measurement.
The focused selector and neighboring changed-RESET_ACK selector pass under
ASAN/UBSAN. W30 log:
`/tanksmall/scratch/tmp/p51-d05-recover-w30-build/runtime/lost-recover-response-w2-w30-asan.log`,
SHA256 `950b21f90b61847a6005b8f498611ab46506bdf25d482962d90a26bbb8ab75a7`.
This extends the explicit response-loss coverage, not the entire D05 matrix.

The original W2 evidence follows:

The registered sender suite now includes `--lost-recover-response` and the
same case in its default run. For each of P29V1, ZSTD_TU and ZSTD_ROUTE, the
fixture commits one of two jobs, constructs the recovery interval, and drops
the first RECEIPTS response before C receives it. The next connection must
repeat the logical recovery request and witness, preserve the exact receipt
interval, commit only the remaining suffix, and release preparation entries.
Physical transport generation changes; the normalized logical state must not.

Donor `ae2e4507`, imported as `1aa64b41`, changes only the test fixture.
Both the new selector and the neighboring changed-RESET_ACK selector exit 0
under ASAN/UBSAN on the same binary. Logs under
`/tanksmall/scratch/tmp/p51-d05-recover-loss-build/`:

- `lost-recover-response-asan-r5.log`: `380c62507d2152c013b57a80a680476dfcedb5f71317f65945c07a9835a2c90e`
- `changed-reset-ack-asan-r1.log`: `831589a20c5885fc7d32b38616c6d9771cbccf7b9f3f0dbac26eef0901bbc5ea`

Test source SHA256 `f9c2f154f1127b829b65f13f19083052d75def1cf63b6c2e3ae8ffd2138df825`;
binary SHA256 `bfbdfcdec2ccbeb8478b9a0e1b2e04bff111779a886d53d9beedeb86250642bc`.
Earlier selector-wiring and fixture-assertion failures are not passing evidence.
This closes the explicit two-job lost-response scenario across profiles, not
the full D05 window/topology matrix or a new response-corruption test.

### QA snapshot and library portability

Generated `*.a` and `*.so` files are now ignored so QA's untracked-file
snapshot cannot import stale build libraries through VPATH. The supervisor
sanitizer script links `-lxxhash` rather than a Debian-specific absolute path.
These two files match the corresponding changes in donor `43451abf`.
Luna verified both ignore patterns. The adapter source test also clears
inherited cleanup traps around its background child launch and inside the
compound interruption child; this prevents a Bash child from deleting the
parent's runtime directory. The shell file matches donor `13fcef2b`, SHA256
`8e8c1dd44007db6e0074aa9af0d095f3a33e05de1d94f5f91da884d10bccd1d8`.

Both Bash and dash executions pass the complete adapter source/mutation gate
(46.18s and 44.42s). The supervisor sanitizer script also passes (13.78s).
Runs use `nobody`, a writable short `/tmp`, an init reaper, SYS_PTRACE, and the
configured service, preload-helper and linker variables. Earlier runs without
SYS_PTRACE failed process-identity observation and remain failed setup attempts.
Logs under `/tanksmall/scratch/tmp/p51-candidate-portability-runtime-a027d14e/`:

- `adapter-bash-ptrace.log`: `61ad660885d57099cdc77d91a82aefa5f60cf37e5a955d5e33fd86637b423ef0`
- `adapter-dash-ptrace.log`: `71d02409607fcadb6e234af1d080e2d73fa95661b8ff8735465912be4dc56273`
- `sanitize-ptrace.log`: `eec5eafaf4d458f6c5dbc70d40f2ec205ab6c2bfb62756d204b8bc589973c1be`

The old/fixed Bash trap reproducer and fixed dash check are retained at
`/tanksmall/scratch/tmp/p51-candidate-runtime-a027d14e/a027-trap-fixture.log`,
SHA256 `66bfbe0d0dce7ce7480d355bdee2ccd56c8c85aee727c16c1e7df4ac659df65d`.
These focused gates do not constitute a complete candidate QA run.

### Sender sanitizer lifetime correction and clean candidate build

The R2 recovery and shared-failure test fixtures now declare their sender
after its Asio context, so retained receipt timers are destroyed before the
context's timer service. The original sanitizer runs failed during teardown
even where the logical checks printed PASS; those runs remain failed.
Donor `6b538e2a`, imported as `55635641`, changes only these two fixtures.
The service's context already outlives its route owner by member declaration
order; this fix does not change product source.

The corrected ASAN/UBSAN binary passes nine focused selectors: W30 accounting,
lost-COMMIT recovery, changed RESET_ACK replay, lost RESET_CONFIRM,
positive-after-rejection, retirement during recovery, deadline recovery ACK,
completion-log accounting, and observer-failure recovery. Every selector
exits 0. Product objects were rebuilt with instrumentation from `e98a8ba3`;
the imported test source matches the tested source byte-for-byte.
Log `/tanksmall/scratch/tmp/p51-r2-accounting-asan-e98/asan-focused-fixed-lifetime.log`,
SHA256 `cb2df548eec25b490785d13088b998268b55d6cb083321f05b23e9f8d3ffaf04`.
Binary SHA256 `4fddf93667ac03d67c22007d61cf0ce92ae32c6c5fa278e8694c301dadf24c95`;
test source SHA256 `66a202f79a89845ca6ab0c4f4bb25c72d79ed1be713bdbaf0524704b7f325976`.
These focused passes do not close the complete D17 lifecycle matrix.

Separately, an isolated checkout of `a027d14e` completes an out-of-tree
`make -j2`, the source-contract preflight, and direct execution of the
registered accounting regression (three profiles, 32 inputs, two passes,
W4). Logs under `/tanksmall/scratch/tmp/p51-candidate-runtime-a027d14e/`:

- `a027-out-build.log`: `d1d33ebc9ddbb092d6ef914210361c0c77883e149b0d5870a7ed1ac64c4abd25`
- `a027-source-preflight.log`: `fee2d042dc88fc89c9138612382ee23c8b1900ecefbc2eacd385fd9f4e92aa0f`
- `a027-bench-direct.log`: `856f00b7e40c6044a2349ebc07ec555c83cea66c2d5bffffc400f29077a16cfd`

The root recursive `make check TESTS=p50transferwindowbench` also passes with
`ICEFARM_TMPDIR=/tmp UV_CACHE_DIR=/tmp/uv-cache`, taking 395.83 seconds to
build all test-program prerequisites and execute the one selected test.
`a027-root-check-retry.log` in the same log directory has SHA256
`7c5987706e74596f4173a38d438d6ea691c6f7b0fb96300be5ada21e7fb9b526`.
The registered `.trs` receipt under
`/tanksmall/scratch/tmp/p51-candidate-build-a027d14e/unittests/`
is PASS, SHA256 `7f175f2f5d04511903d382671ba96ca623e127f57730d53966587cf2c2689229`.
The earlier subdirectory-only invocation failed because it did not build
the cache test archive; the first root invocation lacked the required scratch
variable. Neither is a passing run. This build predates the fixture correction
above and does not qualify private capacity changes or full candidate QA.

### Retry-safe preprocessed capture

The opt-in `ICECC_P50_PREPROCESSED_CAPTURE` observer now accepts an existing
regular capture only when its bytes exactly match the retry source. It never
overwrites prior evidence. Bounded comparisons cover short reads and large or
empty inputs; normal behavior with the variable absent is unchanged.
The registered `p50_preprocessed_capture` test passes standalone with strict
compiler warnings, including different-content/length and nonregular-file
rejection. Reviewed donor `13390d08`; source hashes match the tested donor.
Retained log `/tanksmall/scratch/tmp/p51-capture-observer-tests/test.log`, SHA256
`b553f8d5fc26035bd1025f6c28bf502550ca9828b968bbe161f9a4b642d02939`.
This corrects an observer failure that masked retries in the private C1F2
W30 gate; it does not itself qualify that recovery scenario or full QA.

### Scoped two-worker W30 recovery

The local C1F2 gate passes P29V1, ZSTD_TU and ZSTD_ROUTE with the rebuilt capture-safe
client. It holds exactly 30 decoded A receipts after one active committed
compiler, stops A and closes only A's scheduler connection, and verifies an
exact B output with B's socket/store unchanged while A remains stopped.
After A resumes, its old group settles before readmission. All 30 held
callers and the active victim succeed with fresh assignment identities and
byte-identical independent local objects; a separate fresh A probe also passes.
This is scoped F-A session loss with an injected reconnect hold, not an
ordinary-loss latency result or the complete restart matrix.
Earlier capture-failing C1F2 pass markers are not qualification.

Select `ICECC_P50_SUITE=C1F2/31` and `ICECC_P50_C1F2_F_LOSS_W30=1` in
`unittests/p50compilee2e-run.sh`, with its 31-input batch manifest, WARM=0 and
PASSES=1. Use an isolated local container with NET_ADMIN, SYS_PTRACE, `ss`,
and distinct daemon/sidecar and wrapper accounts. The test closes only the
validated owned scheduler socket. It is not an external-farm test.

Logs under `/tanksmall/scratch/tmp/p51-d07-compile.YLS0NG/tmp`:

- `c1f2-current-P29V1-r2.log`: `cd969fcd934a3e6b63a8f58cc5687d5dd2d7f65bca6116f1da9d69a49d4fc0af`
- `c1f2-current-ZSTD_TU.log`: `cc12c0b8716bdcca08afc94f38833b789b4524a3ea72886aeb2a2824e1a43e16`
- `c1f2-current-ZSTD_ROUTE.log`: `b7ee9715b24b2d3208b8596ea1fa2a0b0e2e5767a2a7850f42473a63f9966238`

P29V1 and ZSTD_TU executed runner SHA256
`93edd4b0fe2e4245926cce1c627c3fce1a95e41566e0326965867a313bc4f048`,
client `c50e5d67c267ecff4e1e057939913a95e92760c0724ebbb2cfa1e84aa1ce646c`,
and daemon `85da3cf6261cf830852861181f8ecea582f2864df6c94a76d803c32238754069`.
Imported runner `567a68d02a8d808de52d6180fdd88f11802c251e5b1ab0b54712ab7524bb655b`
only tightens the unobserved active-failure branch and factors the exact
Error24 classifier, with a passing Error11-negative fixture. Fixture log
`/tanksmall/scratch/tmp/p51-capture-observer-tests/w30-terminal-classifier-test.log`
has SHA256 `fc5567af25f59b94eccc415ab3ddccde211be71995ead3d95b42bf42c0addaca`.
ZSTD_ROUTE executed the imported `567a68d0…` runner with the same client and
daemon binaries and exited 0. Its log records all 30 held callers completing,
an exact fresh-assignment object for the active victim, and exact fresh A
recovery after the old group is gone. Root independently checked the retained
log and its hash. A generated-input wrapper entry now passes ZSTD_ROUTE.
This evidence does not establish a complete current-candidate build closure.

### Generated-input wrapper entry for scoped W30 recovery

The opt-in `ICECC_P51_WRAPPER_C1F2_W30=1` path in
`unittests/p51wrappercompile-run.sh` generates 31 small manifest inputs and
invokes the existing C1F2/31 gate. It checks all 30 held callers, B progress,
cleanup before fresh A recovery, exact A/B link counts by phase, and exact
fresh recovery output. The source-contract preflight also follows the capture
path's `printf` construction and retains deletion-sensitive assertions.

The generated-input wrapper passed ZSTD_ROUTE on the frozen cleanup daemon,
capture-safe client and positive helper, in the isolated 2-CPU/8-GiB SDK
container. It observed 30 held A commits, exact B progress, old A group gone
before re-login, a fresh exact A object, and one B adoption. A had three
measured adoptions: initial, after cleanup/reconnect, and before the fresh
probe following 137 seconds without a compiler-input attach. The last is
consistent with the endpoint's 60-second idle deadline; absence of compiler
input attach does not prove there were no control frames. This qualifies the
wrapper entry and that scoped profile run, not all-profile wrapper execution
or the full current-candidate build closure.

Donor `39c9e211`; imported commit `09d93aa8`. Outer log:
`/tanksmall/scratch/tmp/p51-d07-compile.YLS0NG/tmp/p51-wrapper-c1f2-route-20260926-r4.log`,
SHA256 `dd363652b9d6e86c21cab2c8dfd66f459a615de82cfc89870eedf5022628f72e`.
Executed wrapper SHA256
`7b647270483214117c84b42b0152736645920dd38f7d9ab6b349e98e65ed1d04`;
source-preflight SHA256
`c9fe2263537f7777b2190a7a664ff8deec3e0eaca14a74e32929413176b6932d`;
runner `567a68d02a8d808de52d6180fdd88f11802c251e5b1ab0b54712ab7524bb655b`,
daemon `85da3cf6261cf830852861181f8ecea582f2864df6c94a76d803c32238754069`,
client `c50e5d67c267ecff4e1e057939913a95e92760c0724ebbb2cfa1e84aa1ce646c`.

### Active compiler loss and fresh-job recovery

The opt-in local wrapper gate
`ICECC_P51_WRAPPER_WORKER_SESSION_LOSS=1` passes for P29V1, ZSTD_TU and
ZSTD_ROUTE. It stops one exact compiler child, identified by job/epoch/nonce,
PID, process group and process start time, then replaces the global scheduler.
It observes TERM/KILL and group settlement before F reconnects, independently
checks group absence, and compares a separate fresh R2 job's object with local
compilation. The committed victim fails with Error 24 and no object after one
bounded retry attempt. This is W1 worker-loss cleanup, not transparent victim
replay, W30 compiler occupancy, or an unaffected-sibling test. Ordinary
scheduler-restart process-stability requirements remain unchanged.

Compiler cleanup now advances in the ordinary daemon loop instead of blocking
input-lifecycle delivery. Exact process-group ownership and capacity accounting
remain with the child registry; reconnect and new admission wait for settlement.
Accounting/identity faults remain sticky; a cleanup timeout can recover only
after exact settlement. Direct scheduler connection completion is now polled,
with failed connects using the existing reconnect backoff.

The stricter mode additionally sets `ICECC_P51_WRAPPER_EXPECT_STABLE_F=1`.
All three profiles pass with Applied/AlreadyApplied CancelAttempt before group
settlement, the original F sidecar PID/store GUID, one R2 adoption, zero R1
readiness, and exact fresh-job output. Reviewed donor `5ce1bdef5c97de8269dc7eefd6f919605dca6768`;
the seven imported source/test files match the recorded run's hashes.
Final log `/tanksmall/scratch/tmp/p51-quiescence-runtime/w1-strict-stable-f-final2.log`,
SHA256 `78ce5ff7f65877e943951a9309746b805945d242047595845ec025fe8d61c1db`;
daemon SHA256 `85da3cf6261cf830852861181f8ecea582f2864df6c94a76d803c32238754069`.
The production ownership-state transition helper also passes its regression.

Separate checks passed refused-endpoint recovery (three-second test backoff)
and empty orderly shutdown on the preceding binary, before the final ownership
gate refactor. Logs under the same directory: `discovery-final-summary.log`
SHA256 `c750b26f4be91eb29fedcf231e5c3cd636cd86a32dffd7887a6495476d782c15`,
and `empty-shutdown-final-run2.log`
SHA256 `e474d8855c9dbb1cebd33538886242b85833c72f4e6b64cb19842938dc4c9223`.
Final-binary reruns also pass: `discovery-commit-summary.log`
SHA256 `9832b0d77381d39282f82b67d29aa3c935d11b57055ff4e0024306d4c397b30a`,
and `empty-shutdown-commit.log`
SHA256 `f3e56299ba3af69af4fab6ab17a14aab8170484b810b287b70e44a1bd6f46187`.
The latter exits normally in 3 ms. Neither proves production backoff timing.
The additional `ICECC_P51_WRAPPER_STAGGERED_QUIESCENCE=1` mode passes all three
profiles on that final daemon binary. It observes two exact compiler groups:
one stopped and one waitable, retaining both PID/PGID/start-time identities
until cleanup. Both settle exactly once; all new local socket accept events
follow the later group settlement. A real ordinary client queued on F during
grace then completes through local fallback, without P50 input. This proves
local admission resumes, not remote P43 compilation. The original F sidecar
remains alive, lifecycle replies precede settlement, and a separate fresh R2
job still produces the exact object.

Reviewed test-only donors: `62cd0c48` and `6b84c6a2`. Final log under the same
directory: `staggered-admission-review-final-all3.log`, SHA256
`e679ba6649a8bfd8be5f38ce649abfe3d8fcfff893a23d0d3b173ef2b684f808`.
Imported runner SHA256:
`00697a610c74158ba14c44de09f9efb885cf98f5916e5440122016cb90317dc5`;
wrapper SHA256:
`1f5bf728b9ef368a5fe04503d003746a30217e274f2868e420136bc11747eb80`.
Broader P43 recovery and the complete W30 matrix remain unqualified. This is
not a full-candidate regression pass; the reused build's complete source
closure is not established.

Run the existing `unittests/p51wrappercompile-run.sh` with its documented
source/build, daemon-account and worker-address environment and explicit task
scratch. The new mode generates only a heavy victim and a fresh probe.
Earlier replacement-mode donor: `c4a7cb12`. Its all-three log:
`/tanksmall/scratch/tmp/p51-d07-compile.YLS0NG/tmp/p51-wloss-donor-cleanup-all3-run1.log`,
SHA256 `497d4605f8de458a454ac20c1dbf716ab0fbb7d6a05927f46c495d0b674acaeb`.
Qualified daemon binary SHA256:
`8aa26f5256843267a97ae2fcaf5344e3bb57ba2a0a9c8514e7c865c04963d119`.
Its daemon source matches the imported optional identity marker, but the
reused build's complete production source closure was not independently
verified. This evidence qualifies the scenario on that binary, not a combined
current-candidate regression. The final harness differs from the executed
cleanup run only in failure wording (bounded observations, not a one-second
wall-clock promise).

### Local control connection backlog recovery

AF_UNIX connect EAGAIN now preserves the pathname and schedules a 5 ms retry,
rather than treating an unconnected socket as an in-progress connection.
The poll owner omits that descriptor during the wait, including ERR/HUP,
and wakes at the earlier of retry time and the original absolute deadline.
Both initial connection entry points are covered; connected operation paths
retain their existing behavior. There is no new wire message or backlog-size
workaround.

The daemon-control suite passes with the fix. Its regression fills the accept
queue, checks an early turn performs no retry, drains the queue and completes
HELLO/control/descriptor handoff/ACK on the same operation. A never-drained
case expires at the original deadline. Restoring the old EAGAIN branch makes
the new regression fail at `!sender.wants_poll()`.

Follow-on local-transport and supervisor source/runtime checks pass, as do
the daemon-control source checks and the complete sidecar-adapter source
suite, including its freshly linked service lifecycle baseline and executable
mutants. Adapter source-suite log SHA256:
`13ba9fbbb01a131f0f9973db27cbea63c64c476ebe798ea6696711067629e2ca`,
retained as `p50daemonsidecaradapter-source.log` in the donor directory below.
The direct adapter runtime also passes after relinking its generated test
executable. The earlier exit 30 was an artifact mismatch: the service trace
correctly reported UnknownRecord, but the test executable predated the rebuilt
adapter archive and reported Applied. Rebuilding that target, without source
changes, produces UnknownRecord and exit zero. The failed run remains retained;
source hashes alone did not establish executable freshness.
Fresh adapter-test SHA256:

### W30 sender accounting lifetime

Per-job R2 wire accounting now retires at exact validated receipt time, before
the sender returns that receipt's window slot to another caller. The retained
snapshot travels with its pending receipt to the result/replay path, and
accounting-registration failure marks the evidence unavailable rather than
silently treating it as valid. The old caller-finalization timing allowed
more than 30 completed-but-not-finalized rows while the protocol window was
already accepting more work.

Donor `edcf2c4d`; imported candidate commit `e98a8ba3`. The registered
`p50transferwindowbench` Automake test exercises all three profiles with 32
deterministic inputs, two passes, W4, exact receipt and socket-byte
reconciliation, and a bounded missing-participant check. It passed on the
canonical configured SDK build. A matched negative control with only
receipt-time retirement disabled fails because a successful result lacks
valid per-job wire accounting. The 20-repeat corpus-shaped gated run is a
regression stress test, not a speed benchmark; the ordinary CLI benchmark no
longer enables that gate. Focused recovery selectors also pass on the tested
sender binary. The clean current patch still needs ASAN and full candidate
build-closure qualification.

Positive 20-repeat log:
`/tanksmall/scratch/tmp/w30-adapter-runtime-user/windowbench-fixed-r2-32-p0-w4-finalizer-repeat20.log`,
SHA256 `88de71cf8904ea3bb9f87ff6a8f43c28aba479b401a5968de1ddc9574d93fc23`.
Matched negative log:
`/tanksmall/scratch/tmp/w30-adapter-runtime-user/windowbench-finalizer-negative-r2-32-p0-w4.log`,
SHA256 `7ee729dad791d37e74b43f8b1522e4d484dcc79f03fbb25244307a143c054be8`.
Registered Automake result: `.trs`
`/tanksmall/scratch/tmp/p51-transfer-window-config-20260926/work/build/unittests/p50transferwindowbench.trs`,
SHA256 `7f175f2f5d04511903d382671ba96ca623e127f57730d53966587cf2c2689229`;
log
`/tanksmall/scratch/tmp/p51-transfer-window-config-20260926/work/build/unittests/p50transferwindowbench.log`,
SHA256 `fad9b89182b80563e854a62f79e315d6299de27ab802af6a30088dd9f29d0e2f`.
Normal ungated 32-input CLI smoke log
`/tanksmall/scratch/tmp/w30-adapter-runtime-user/windowbench-cli-ungated-r2-32-p0-w4.log`,
SHA256 `9ac3d9e974cdb34c76e62ba65aa0db3f35453177a394a1164a1dc15fa277d840`;
focused recovery suite log
`/tanksmall/scratch/tmp/w30-adapter-runtime-user/windowbench-recovery-focused-r1.log`,
SHA256 `f6133521fdf5207a97679e001fbc17c0fff9567fe3fbbd41a4ca21837eece7d2`.
`293dda5d0ae5390299fd2631706ef10d4c7a7a2965ef315b174bba4ed0f3cfe1`;
service SHA256:
`67acb6a1676167da90827e857afdea05912b5b0ec9c2e9c01cb93b0cfc280fff`.
The combined service run below also passes after its fixture ordering correction.

Qualified donor: `536dc696`. Test binary SHA256:
`8ddc058a9675cdeaf0312740efd841cb174ad8246176bec2c56396ab822047d5`.
Retained log: `/tanksmall/scratch/tmp/p51-w30-backlog-luna/p50daemoncontrol-88b339e2.log`
(filename retained across the final test-only hardening), SHA256
`f62010246ac009cb99e2eb8bec6d87ea28bce14f32b8ff729aaf0854e45f48ee`.

### Reset boundaries and terminal reconnect

The default service suite now includes all 93 profile × K=0..30 boundary
cases and three K=31 terminal reconnect cases. At K=0, thirty distinct bundles
are observed sent while F's first materializer is held; the 31st admitted
source has no wire ordinal yet. For K=1..30, an exactly completed prefix
precedes the held suffix. The owned F socket is cut and the exact RESET_ACK
must arrive before worker release. All 31 requests return exact results and
attached input bytes. Only K=0 asserts a full 30-item outstanding window;
the other cases exercise the remaining suffix after the stated prefix.

At K=31, the test completes 31 inputs, cuts the settled link and submits a
32nd exact probe. F's outbound LinkState proves a new physical generation;
all 32 results/attachments and unique receipts are checked. If reset occurs,
its K=31/P=32 acknowledgement is checked. No held worker is fabricated for
this terminal boundary. An uninterrupted baseline supplies per-request
byte/digest parity without assuming asynchronous TU order or identical
history-dependent transaction digests across episodes.

The fixture waits at most five seconds for zero operation/raw credits after
requesting asynchronous stop; stop itself is not a runtime join. A prior
immediate snapshot raced that cleanup. No production code changed.
Selectors: `--d14-w30-boundary-matrix`, `--d14-w30-terminal-all-profiles`,
`--d14-w30-boundary PROFILE K`, plus the existing K0 and smoke selectors.

Focused matrix (93/93) and terminal (3/3) selectors exit zero on base
`0bc9df39` plus this patch. TU SHA256:
`bcf1272c755f007b23aa10e2a8989bf3b73054ba9a19ce41e63166325dfb46e6`;
binary SHA256:
`503ff3976e857426fa8de2e08a653a4cb539176ddb13e6203f2f10a2a34d2453`.
Evidence under
`/tanksmall/scratch/tmp/p51-d14-f004-scratch.yDlJDu/icecream-qa-h514rgg9/current/artifacts/`:

| Log | SHA256 |
|---|---|
| `d14-matrix-drain-r1.log` | `8ddc27a69766cd4f8868b6cf62d2dc32bca5a862491dc9f17666693042408452` |
| `d14-terminal-drain-r1.log` | `c772a8cdfde60b0bc5c1d96a7a5f6f555c95402b5172720dc42b9ad6db725006` |

The combined generated service-test target now **passes**, including all
93 matrix rows and three terminal reconnect rows, on the updated backlog-fix
production files plus a D07 fixture correction. The positive-owner scenario
previously mistook a flag still set by held request 1 for request 2 readiness;
four preparation workers could then give the successor ordinal 2. It now
observes the second complete bundle and exact ordinals [1,2] before submitting
the successor. A scope guard releases the materializer gate and joins the
waiter during assertion unwinding. No production ordering rule was changed.

Final source SHA256:
`de30d6be6fb1f876cdddd5eb20e550b3fcaf9260ad4cd89f99797786b36a1987`;
test binary SHA256:
`7a038ce2da19f72b4f2bf0b189d34d41bca715014b6a80ab83241fbde26a71e0`.
Evidence under
`/tanksmall/scratch/tmp/p51-d11-metadata-expiry-c6fce0e7/work/artifacts/`:
`p50cacheservice-full-eb3-d07-r1.log` SHA256
`2232e7d5d36b009fab9be967aa4fbc9d0b96ea78f997b74da5277dc8259bd2ef`;
matching `.trs` SHA256
`7f175f2f5d04511903d382671ba96ca623e127f57730d53966587cf2c2689229`.
The focused positive-owner selector also passes twice across all profiles.
Production control/adapter sources were persisted and verified inside the
container before rebuilding affected archives and service/test binaries.

The earlier exit-134 abort remains retained: its exact failing assertion was
not captured, so the demonstrated fixture race is not claimed as a proven
explanation of that particular historical abort. Reused-source diagnostic
runs and manual runs missing harness environment are not substituted for
the final result. This gate does not qualify arbitrary restart combinations
or real compiler cleanup.

### Concurrent source-credit admission

The enhanced default service fixture passes for P29V1, ZSTD_TU and
ZSTD_ROUTE with negotiated W30. With a 16-byte aggregate budget, a 14-byte
source remains held at its read-completion barrier and an older 3-byte
request is observed waiting for credit. A 17-byte request receives typed
SourceTooLarge without an observed read or F connection. A newly submitted
2-byte request then bypasses the still-blocked waiter and completes with
exact F bytes, digest and result. Its read barrier makes the 16-byte peak
observable before release. Closing the queued peer retires that request
while the holder retains its 14 bytes; releasing the holder drains source
operation/raw counters, checked again after 50 ms.

This is one fitting-request bypass, not 30 active transfers or proof of all
fairness cases. The separate existing bounded-bypass fixture tests the
30-bypass limit. The new selector is
`--p51-d12-oversize-fit-credit-cancel`; the default service suite also runs
all three profiles. No production code changed.

Qualification used base `6296bc4a` plus the test patch. Imported test source
SHA256: `0962dd3735744b51609b36a129c3a4faf00311c168b0dd769aeeb3c19f6e65f7`.
Focused selector and full ordinary service suite both exit zero on binary
`2df66ec2839829f08cf46e99e7b3402556cba809d3b237136cb681825cdb85b5`;
the generated `.trs` reports PASS. Retained root:
`/tanksmall/scratch/tmp/p51-d11-metadata-expiry-c6fce0e7/work/`.
`artifacts/d12-focused-r4.log` SHA256:
`d34de63ff4b50ea82945daefcab1db9f56d66f9b556b65e83394ec31702e2840`;
`build/unittests/p50cacheservice.log` SHA256:
`d78c5657f9669f2618d2270135bbcdeb9434634d6e9fc908e7254e1ab9f2a180`.
Earlier polling-only peak checks failed because the fitting transfer could
finish before sampling; those fixture failures remain retained separately.

### R2 cancellation before input publication

The opt-in positive-daemon fixture now tests one exact unpublished assignment
for each of P29V1, ZSTD_TU and ZSTD_ROUTE. It arms an R2 reservation without
transferring source, pauses its test-owned F sidecar before CompileFile,
observes the exact WAITP50INPUT attachment request, closes the client peers,
and resumes the sidecar. The bound cancellation reply must report accepted
for the exact job/epoch/nonce/request/reservation. Input settlement reports
unknown-record, no accepted attachment is observed, and both daemons exit
cleanly. The F sidecar PID/start-time identity remains unchanged.

This is a single-assignment prepublication check, not W30 occupancy or an
active compiler cancellation/sibling-output gate. The fixture has no real
compiler environment; its no-start evidence combines sampled child lists
over 500 ms, absence of compiler-start logging and no accepted attachment.
It does not claim continuous process tracing. Conflicting test modes are
rejected rather than silently selecting another gate. The production change
only adds identity-rich cancellation tracing under `ICECC_P50_DEBUG_ATTACH`.

Qualification used a private `63be94b0` snapshot plus the two-file patch;
the imported files are byte-identical. Source SHA256 values:
`daemon/main.cpp`:
`166c895ec93d5622eaf2bf729b732b4d5a57b4ff8ccc30885c9d54d2c1e3a216`;
`unittests/p50daemonpositive.cpp`:
`4875ec0c30f237375e41c514f47c0cb0f89fc0588b1cc236457aa7e02418d50d`.
Test binary SHA256:
`f76db034b12d3f88be989f5372b821cfdc87800aa8b1287d96245a9c2b32b43e`.
All three focused runs exit zero. Logs are retained under
`/tanksmall/scratch/tmp/p51-d07-compile.YLS0NG/`:

| Log | SHA256 |
|---|---|
| `cancel-before-start-r5-P29V1.log` | `04717362adc2ebd59ffb96f45ba6436d7cf86b14aba4deea91092b1ead8f2e35` |
| `cancel-before-start-r5-ZSTD_TU.log` | `e3fa66769de1106e4bea15d928cb717fee90869625277e161552441fc8a080bc` |
| `cancel-before-start-r5-ZSTD_ROUTE.log` | `35961f97fd8a436d7d015aa166889059767ef0981e9cf91ec406f386a2314c38` |

The SDK image was
`sha256:19ef868afec561456949471bd951f7d75aa689eb10af11579eba7f1a966dacb4`,
limited to two CPUs and 8 GiB with NET_ADMIN. In an isolated root SDK
container with its standard `icecc` service account,
select `ICECC_TEST_POSITIVE_DAEMON=1`, `ICECC_P51_MODE=on`,
`ICECC_P50_DEBUG_ATTACH=1`,
`ICECC_TEST_P51_CANCEL_BEFORE_START=1` and
`ICECC_TEST_P51_PROFILE=P29V1` (or either ZSTD profile), then invoke the built
`unittests/p50daemonpositive` with the built `daemon/iceccd` and
`cache/icecc-cache-service` paths. Supply the scratch environment used by the
normal positive-daemon harness. This focused qualification is not full-tip QA.

The existing opt-in `p50daemonpositive-run.sh` now includes all three
prepublication cases after its baseline, pending-disconnect, replacement and
vertical cases. That complete wrapper exits zero against the qualified
binary above. Wrapper SHA256:
`7abcb90b282088bbe4e492b83e78d790f44f6657312d53e2e1b1cc48c9a43db9`;
retained `positive-wrapper-r2.log` in the same scratch directory:
`7aa99730213955782774470a73743e66e27cc70a535a5849b2c77a18d1b740f2`.
Specialized positive-daemon wrappers clear the new selector so inherited
settings cannot silently replace their intended test. Those cleanup edits
were shell-syntax checked; their individual runtime matrices were not rerun.

### Full-QA failures and focused corrections

Canonical QA on frozen `f0049371` is terminal with overall FAIL: 176 native tests,
168 passed, two failed, six skipped, zero errors. Both failures are the
service-fixture issues detailed below. Python completed with 1,595 passed and
eight skipped in 655.40 seconds. The current QA step took 3,219.2 seconds and
returned 1 (the outer make returned 2). This snapshot predates both corrections;
passing focused repairs do not reclassify the frozen full run as passing.
Under the retained run, `current/artifacts/python-pytest.log` SHA256 is
`8b305d178e2c325cd0db99793649bf5127716ed1eeaa0864307fae32762952fe`;
`result.json` SHA256 is
`f4d7926f79ef34ebcfd6e9174b4f806e5a0c7f97cfe55d4637a6c2d69cd9d819`.
This run also predates the compiler-fingerprint QA speedup in `e2c11349`.
Native-stage log SHA256 is
`ce7c49fb7714be2acb9d59a74e0240f8fe0553cd39db75a884d405c7f14b6e9a`
at `current/artifacts/native-check.log` beneath the retained full-QA run below.
The six skips remain uncovered by this default run; separate opt-in gate
evidence must be consulted rather than counting them as passes.

AddressSanitizer reports
`stack-use-after-scope` in `d11_output_cap_client`: its `capped_armed` reference
binds a temporary array at the coroutine call, which expires before
`io_context::run()` resumes the coroutine. This is a test-helper lifetime bug,
not evidence of a production endpoint defect. Earlier ordinary service passes
do not establish sanitizer qualification. The helper now owns the array in
its coroutine frame. Focused and full service sanitizer reruns pass on the
isolated c6fce0e7-based overlay described below.

Retained log:
`/tanksmall/scratch/tmp/p51-f004-qa-scratch.bxOlfG/icecream-qa-vjyj7uml/current/build/unittests/p50cacheservice-sanitize.log`,
SHA256 `9370973a6739d6f4328e8672c647841989af3a689db029474e20e0aff2ae1f88`.
The original full-QA snapshot and terminal failure evidence are retained
unchanged. It must not be reported as passing.

The lifetime correction passes all six output-cap cases (three profiles,
W1/W30) and the full service suite under the repository's ASan/UBSan/LSan
script compile configuration. Both test exits are zero and the generated
`.trs` reports PASS. The private script runs the selector and full suite
against the same instrumented binary; its compile block is unchanged and
the generated Automake environment supplies dependencies. Prebuilt linked
libraries retain their original build configuration; this is not a claim
that every linked object is instrumented.
Artifacts are under
`/tanksmall/scratch/tmp/p51-d11-metadata-expiry-c6fce0e7/work/build/unittests/`:

- `p50cacheservice-sanitize-output-cap.log`: SHA256
  `ce26188d74d07f7254e62c1bf1ace088f824f794f49ab2e9d57f224cd463e2f2`.
- `p50cacheservice-sanitize-full.log`: SHA256
  `3a3d1bc9530cb26a5a6070568af508fec36ddf2e4f592cfdad908a1f4fd33a53`.
- Instrumented binary SHA256:
  `05cf41866a42263d89c1f37ce1592a6035c419147f17bd2c56f67308938fc66b`.
- Qualified test-source SHA256:
  `34b6990fb1db0884d205404c0b7aabbb412f2b17b6d42e0df9387ebb02b79d0b`.

This sanitizer overlay also contains the metadata-expiry test and predates
the recovery-ordering correction below. The later combined ordinary service
qualification is documented under autonomous metadata expiry. Earlier private
invocation/compile/link setup failures did not execute tests and are retained.

The same run also fails the ordinary service suite at
`duplicate_f_cancelled == !positive_recovery_owner`. The positive-recovery
fixture submits its first two requests before proving which reached F's first
worker gate. Its trace is consistent with asynchronous preparation reversing
those requests (`recovery-owner=3/9102`, `pre-replay-ordinal=1`, no recovered
positive first receipt). The fixture now waits for the sole first submission
to reach F's worker gate and C's complete ordinal-1 write before submitting
siblings; all existing outcome assertions are retained.
The adjacent `p50cacheservice.log` has SHA256
`78fad9576fef2f048d1d4358a59dd2d82c90cda2f3056d5b6c9cb7d1eaf8882d`.

The corrected `--d07-positive-recovery-owner` selector passes three runs of
all three profiles on an isolated f004 product build. Each run reports the
exact first recovery owner, one recovered first receipt, matching resets,
exact survivor inputs and released credits. Build and all three test exits
are zero. This qualifies the fixture ordering correction, not full QA or D14.
Artifacts are in
`/tanksmall/scratch/tmp/p51-d14-f004-scratch.yDlJDu/icecream-qa-h514rgg9/current/artifacts/`:

- `d07-positive-r4-run-1.log`: SHA256
  `fbd529f9da535cc71a5861a69fafb6242fe18ffc4e46c412eb20c00790d7a6d6`.
- `d07-positive-r4-run-2.log`: SHA256
  `0e6f19753865346b83e62eb11e1a0900083870574a8f8c82d1ae0a9105b3a6ff`.
- `d07-positive-r4-run-3.log`: SHA256
  `5ab44dabfbea7692bf36a3c64c9bd2d31b73e1c013a961d1d16a309e874d131d`.

Qualified test-source SHA256:
`a23efa0f5d96b0c5bc373401068896bac471f799790663ebecaae183c2b9c158`;
binary SHA256:
`f3e1d35a47a5506a95d478bbe7078f7978928a372b0945d12f71b332acf9d9fa`.
Earlier private build failures (ownership, unrelated experimental D14 code,
missing generated build files) are retained separately and are not test results.

### Autonomous metadata expiry

The 120-reservation test now observes retirement of the exact expiring ID
before making another owner request. It distinguishes explicit cancellation,
checks only those two rows retire, refills both slots and rejects overflow at
120 again. ZSTD_TU/W30 is the configured profile/window; this is metadata
capacity and timer coverage, not 120 active transfers or process-memory proof.

The focused test passes. Deleting only the timer callback's sweep (leaving
request-side sweeping intact) fails with exit 1 at the exact expiry-observation
wait, not an outer timeout. Restoring that call and combining both fixture
corrections above yields a passing full ordinary service suite: actual exit 0,
`.trs` PASS. Production source is unchanged.
Artifacts are below `/tanksmall/scratch/tmp/p51-d11-metadata-expiry-c6fce0e7/work/`:

- `artifacts/metadata-expiry-positive-r1.log`: SHA256
  `bb76ebbd39d00763069eb0dd50ec3ff54463e7f5bc784037980d964d9c13c038`.
- `artifacts/metadata-expiry-timer-mutant-r1.log`: SHA256
  `d3ad93f483658fade9d5a9001dc86b7ae72dd64e698f5dfca3fbdd3d865c63cf`.
- `build/unittests/p50cacheservice.log`: SHA256
  `a32f095f7524179dc29384a82d053e5e9bb5b4bdd392951f314af6ebf492fa32`.
- Qualified test-source SHA256:
  `aa2a170e610b38e50da6642e9dbc3403a7467ed2f03976124ba0f032f97bf381`.
- Binary SHA256:
  `d3f3174e629f97160fc346781d0961b185ddd6f9916aa4d1c6afffc4a9c89479`.

### Compiler fingerprint validation speed

Root-header corpus promotion now hashes each distinct resolved compiler once
per validation invocation, instead of once per A/B row. Every row still checks
its declared hash/path/arguments. File identity (device, inode, size, mtime and
ctime) must remain stable during hashing and on subsequent rows; no cache is
shared between validation calls.

The unchanged original 1,000-row authority test passes in 21.98 seconds with
the optimized validator, versus the recorded 486.65-second baseline (22.1x in
these runs, 95.5% less elapsed time). No corpus rows or original negative cases
were removed. Expanded coverage verifies two distinct compiler paths, one hash
each, conflicting later-row hashes, between-call changes, and file-identity
changes both during hashing and on a later cached row.

Private exact645-based qualification artifacts are under
`/tanksmall/scratch/tmp/p51-retained-perf-opt/runtime/`:

- Unchanged test: `original1000-optimized-r1.log`, SHA256
  `2deb78289bff6bf3a69da5fa8284c32a657dea9ae363025ef505925801928903`.
- Expanded node: 1 passed in 28.30 seconds, `focused-r7.log`, SHA256
  `a015c3279547043401d9fe1f211e14ad0dc3473d87495b75c49aafd47cca28df`.
- Authority module: 19 passed, two skipped in 55.30 seconds, `module-r1.log`,
  SHA256 `5cbf39a29eabb40ddc6261b5c3e75e2ad6a7a19c4874a0d7b68a0b5a30bfe3ff`.
  Skips are unavailable user/PID namespace capability and absent retained
  production trace/compile-command fixtures; neither is counted as covered.
- Direct promotion module: seven passed in 0.14 seconds,
  `direct-promotion-r1.log`, SHA256
  `aa1b23380f91af78302c0ea3ea91977d2b98fe2026f5245da828e71d0b979b81`.

This is validation-time improvement, not a measured distributed build speedup.
The concurrent full-QA run on frozenf004 predates this optimization.

### Independent F ACK and terminal accounting

The opt-in R2 trace now includes F-validated cumulative ACKs and one terminal
snapshot per accepted physical link after socket I/O finishes. Diagnostic
epoch/K/Q state is captured separately from operational state, including
RESET confirmation and failed echo handling; duplicate confirmations do not
rewind the snapshot. Observer failures do not change protocol outcomes.
No new wire record or shutdown wait is introduced. The collector reads worker
trace files and joins them to exact C/F/logical/physical identities, excluding
epoch from the physical-link join because RESET keeps the same socket.
Unmatched or ambiguous worker events are rejected.

Qualification used exact645 plus the 14-file accounting/QA overlay in
`/tanksmall/scratch/tmp/p51-f-emitter-qa.nWoAgu/`, SDK image
`7fb2663633c0557ebc8efdc5e9fffff4a8e727ab7899ce5209753f6c30f1bfd0`:

- Full `p50cacheservice.log` and `p50endpoint.log` targets pass, exit 0.
  Log SHA256 values are `72500016f3c80a0bc8fafab165a3ddb9d0db45ed1efbc03370dcad3ca7a0357d`
  and `db39be26649ceb0999017d9db08425397eb697875f059b2846e3911d216290a9`.
- Actual two-C/one-F producer-to-collector gate passes: distinct C owner
  threads, concurrent bundle rendezvous, per-link K=Q=1, terminal closure,
  and byte/job conservation. Retained `artifacts/live-two-c-source-trace.jsonl`
  SHA256 is `43fde9f7d933d6a53d2a737d5dca51686bd447408ded8f71c49050a8f55a5693`.
- Final collector/report, R2 trace, specs/plan and developer-runner tests:
  484 passed in 6.83 seconds, exit 0, including the live service test.
  The final batch result was returned by execution session 29346; it is not
  represented as a separately retained hashed batch log.
- Qualified service binary SHA256:
  `870dd377170af095889e4234ec7b2ab5188279d84650ce2a1c096bd0d0a5f409`.

Canonical QA supplies its own built service binary to the live Python test
after successful native checks and clears inherited binary paths otherwise.
Python-only runs can still skip that live node. The merged service test also
retains the independently qualified encoded-cap fixture; their combined
current-tip full QA remains pending. Earlier setup and fixture-framing
failures remain retained and are not product verdicts.

Tracing uses synchronous file append under the existing trace mutex when
explicitly enabled. Record tracing mode in performance results; these tests
do not establish negligible instrumentation overhead or farm performance.

### Canonical persistent-wrapper gate

`make -C /work/build/unittests p51wrappercompile-check` completed with exit 0
on the exact645 bootstrap source/build pair. All six cells pass:
P29V1/ZSTD_TU/ZSTD_ROUTE with 2 and 100 distinct compiler invocations. Every
remote object is compared byte-for-byte with the local reference; each cell
observes exactly one adopted persistent R2 link and no R1 session-ready event.
This proves wrapper-path persistence and object parity, not 30 simultaneously
active transfers (this fixture uses one execution slot).

Retained root: `/tanksmall/scratch/tmp/p51-645977c8-mixed-build/c01-run/`.
`outer-r5.log` SHA256:
`6dee114cf1e933bc0b30b040cc6275f4710384903228b6e98c9e96309a5e24e3`.
The 100-job profile logs under `p51-wrapper-fixture.esX52X/` are:

| Profile | Log SHA256 |
| --- | --- |
| P29V1 | `ee95adbe635c01c4a474c8af15c4ceed959a8f4b8573e8a0d746648d55565ea3` |
| ZSTD_TU | `34de02517dacd7610bb4b93072b0529f62e32bd2034c37169048343d7258add8` |
| ZSTD_ROUTE | `2df67ceb3ea885d102f70c5d4ba127412aef4753eff24256c0370891e406f721` |

The SDK was `7fb2663633c0557ebc8efdc5e9fffff4a8e727ab7899ce5209753f6c30f1bfd0`,
with a private Docker bridge, 2 CPUs/8 GiB, and the named `nobody`/`nogroup`
test account. Authored files were unchanged against the saved513e source
snapshot; only generated Autotools support differed. Earlier account/mount
setup failures remain retained and are not passing or failing product tests.

### Current-runtime local R2 and concurrent mixed process checks

Exact `645977c868e9ddc6429b2c0f7d266cf00508f251` built and installed through
the supported bootstrap (exit 0). Product image
`icecream-dev:current-513e258c70a86593`, image ID
`255a28bc27928a3ec346cf0ded510bacf6692ed1d1d14892256bb312c5799943`,
binds source snapshot
`513e258c70a865936763804b2c2395603861a55e20cb7410e2f9493d7c748138`.
The three `dev/mixed.py --p51-r2 --only-p51-r2` cases pass for P29V1,
ZSTD_TU and ZSTD_ROUTE, with remote output, selected profile, source lease
and R2 link adoption checked.

Three separate `--concurrent-mixed --concurrent-profile PROFILE` runs also
pass. Each uses three client roles (P43, R1, R2), one shared current scheduler,
separate R1/R2 workers, a private local Docker network, jobs=3 and memory=8
GiB. They verify distinct overlapping remote compiler processes and exact
role outputs. The P43 image is pinned to `cd74801e0fa4e83e` with image ID
`9140ad2c1a1afb2086bdfcc483d0b0d5d98bf1954168e0889d2e3ee05bc45050`.
Artifacts under `/tanksmall/scratch/tmp/p51-645977c8-mixed-build/mixed-runs/`
have these `summary.json` SHA256 values:

| Run | SHA256 |
| --- | --- |
| `r2-all` | `92ed86afe43e518f61516ccf38eaaa6e3b771750641717a6b7dad7566524ff0c` |
| `concurrent-p29` | `c4bdbda0c2f5ccbbb55f02deecd622ab6c227c2a701b92c8f8d8f9aa4f4a6a20` |
| `concurrent-zstd-tu` | `2883adc66b1de24217ebd15db015f6b2bb5ff9f88d5bd4353d474c1a289639a6` |
| `concurrent-zstd-route` | `30fa648462fb3b679835575c49ba1d95e02bb7c4e75e2839be193be2af7ab938` |

This closes those local process cases for the 645 runtime, not cross-host
farm qualification, every legacy scheduler direction, full W30 saturation,
or the full C01/D18 grids. Later encoded-cap additions are test-only;
unpublished F accounting changes are not included in these images.

### Independent R2 encoded-byte cap

The fixture now independently covers raw-byte and decoder-window caps too,
reusing the same service/client setup. All six ZSTD_TU cells pass:
encoded/raw/window limits at W1 and W30. Each asserts that the other dimensions
can admit two inputs, refuses C2/C3 while C1 retains its charge, then verifies
ACK settlement returns pending credits to zero and an equal-sized refill
commits. Separate deletion controls fail on observed target counters:
raw=2,048 > cap1,536; window=268,435,456 > cap134,217,728. The test-only second
materialization hold keeps excess credit observable; cleanup releases both
holds. No production behavior changed.

This extension was qualified on exact86577d3a plus its service-test diff,
SDK `7fb2663633c0557ebc8efdc5e9fffff4a8e727ab7899ce5209753f6c30f1bfd0`.
Source SHA256 `2d9d3378d496860074daba34425a793472482812c7bf832aed003cee4d708549`;
normal binary `bea3e7cd088d3498989a2600eccd8a37f2710164f69c19bec6407e1a5f80bf4f`.
Artifacts under
`/tanksmall/scratch/tmp/p51-d11-raw-window-cap-scratch/icecream-qa-ek5ujsf7/current/artifacts/`:

| Evidence | SHA256 |
| --- | --- |
| `final-encoded-matrix-r1.log` (exit 0) | `0a5d3a3fc73d45b77d7d04d13d348ebfc7b12df55406b2eee7789ad3834c6208` |
| `final-raw-window-matrix-r1.log` (exit 0) | `be2ab3ac84829dbd5fd5c6cd1da69986728bf74421d61878f63f70d1ca8135f0` |
| `raw-mutant-w1-r2.log` (expected exit 1) | `3add76444abf77e4010d19c7674b5d41d5469b78e15d648c29b791682d6b7f3c` |
| `window-mutant-w1-r1.log` (expected exit 1) | `43dbe99ca326ff249643aad0ec5902f475348f7461fc57596338ff8cc72a06fb` |
| `default-service-r3.log` (full suite, .trs PASS) | `0846064ea9f876d06cbfe213974d6f92cb0f9c51355d59e2ffd3112fa9642e50` |

Earlier scratch/managed-Python setup failures are retained and excluded.
The full-QA run on frozenf004 does not include this test extension. Metadata,
peak memory, other profiles' independent caps and 30 active decoders remain
outside this six-cell result.

The real F service fixture now covers aggregate pending encoded-byte admission
at W1 and W30 for ZSTD_TU. One 1,034-byte encoded input is held against a
1,536-byte cap; two independent C inputs are refused without an extra charge
or publication. After the first input settles, pending encoded/raw/window
credits return to zero and an equal-sized input commits. Removing the encoded
cap check produces an actual 2,068-byte charge and the intended failed
invariant; restoring it passes both cells again. No production code changed.

Qualification used exact `6f5acde0` plus this test, SDK image
`7fb2663633c0557ebc8efdc5e9fffff4a8e727ab7899ce5209753f6c30f1bfd0`.
The full canonical `make -C /work/build/unittests p50cacheservice.log` also
passes (exit 0). Artifacts are under
`/tanksmall/scratch/tmp/icecream-qa-8a9s2j3u/current/`:

- Test source SHA256: `44be987746d844d110103152557886d1f6cd8934935e823501358882c577035c`.
- Binary: `2fd422eabaf2d0a9b51e5277d44a7e33b2e911e06fc49e99fd4a469960ccf778`.
- `artifacts/encoded-r7-w30.log`: `9be61021de67a293a2a36a799a56d2e45d4ecedbc35a9e71b7af2853e8364245`.
- `artifacts/encoded-mutant-w1.log` (expected exit 1): `0d3d9d8e7a10698bdbbf851230e1ff57aaedc4240bfca87961387e39793b6af2`.
- `build/unittests/p50cacheservice.log`: `efb9ed1efe6490f90d6252a500628b7e7db04b614200e914c137641681c6efb5`.

This does not prove 30 simultaneous decoders, independent raw/metadata caps,
other profiles' encoded-cap behavior, or peak memory. Those D11 gates remain
open. Earlier setup/stale-binary failures are retained, not counted as product
failures or successful negative controls.

### Canonical QA baseline and remaining native repairs

The frozen `422932c9` canonical `make qa` run completed with overall exit 1:
native checks reported 176 total, 158 passed, 12 failed, six skipped (exit 2);
Python reported 1,589 passed, seven skipped in 660.35 seconds (exit 0).
Artifacts are under
`/tanksmall/scratch/tmp/p51-422932c-fullqa/icecream-qa-bjv8fovy/current/`.
Native `build/unittests/test-suite.log` SHA256:
`eae5dc779409863a5a46504842f7bc3c0700dedc870a81f6dfc170321eff9984`.
Python `artifacts/python-pytest.log` SHA256:
`d09546052c9ea89a6352ea3a860cf3fbd7a190ec9af5296714793e8b873b34ba`.
This is a passing full Python suite, not passing full QA or current-tip QA.

Three native failures have the source-check repairs documented below.
The other nine were stale ordinary-protocol/trace-placement assertions,
incorrect P50-handler extraction, stale protocol mutation anchors, and
missing libraries in manually linked tests. The consolidated repair keeps
R1 ARM/ARMED protocol predicates independently checked, requires the current
R2 trace call and exact identity ordering, and links the configured service
dependencies. The daemon fixture separately exercises an exact protocol-51
client and a raw client negotiating protocol 50; both retain bounded close
checks. Protocol deletion controls must fail their intended runtime assertion,
not compilation or setup. All focused repair gates pass on the private
`f23c1a85`-based nine-file candidate: source checks, the real-daemon fixture,
cache-session protocol mutants, six daemon-control runtime mutants, and
input-lifecycle/daemon-control ASan, UBSan and LSan runs. The manual links use
the frozen 422 SDK build's service library; this is not a fresh whole-product
sanitizer build. Earlier missing-link-flag and unused-parameter mutation
attempts were setup/control failures, not product failures.

Final cache-session mutation log:
`/tanksmall/scratch/tmp/p51-qagates.3dtjD3/artifacts/p50cachesessionwire-mutants.log`,
exit 0, SHA256
`72c18228ff91b97642d8184bb3d887d73a0c94bd7286681585b988c6177679f2`.
Raw protocol-50/51 daemon fixture:
`/tanksmall/scratch/tmp/p51-daemon50-case/run.log`, all checks pass, SHA256
`30d0d84f7a9cc9967a638c63140d3173c04820c469b6bb24a76b1c6ec92d85ee`.
The remaining four-script group exited 0 in the SDK execution transcript
(session 35337); no separate durable group-log hash is claimed here.
These repairs do not reclassify the frozen canonical run as passing.

Native skipped coverage still needs separate execution: `remoteice-quick`
and `p50assignment-remote` require CAP_SYS_CHROOT; source-arm and positive
daemon runs require their explicit opt-ins; compile/completion end-to-end
runs require a non-loopback worker/scheduler address. Source checks are not
substitutes for those process tests. Final combined-image QA and mixed
P43/R1/R2 runs remain required.

### R2 output-cap publication cleanup

An F output-cap refusal after publication authorization could leave its
reservation marked publishing, preventing cancellation and deadline expiry.
The persistent endpoint also retained a previous job's completion marker,
which could suppress lifecycle cleanup for the next failed job. Cleanup now
releases only the exact unpublished binding's transient publication latch,
retains consumed proof for recovery, and resets per-job result identity at
accepted JOB_BIND. Existing exact-link deactivation still handles reconnects
that replay an older RESET offer; an epoch mismatch is not a blanket reason
to skip physical-link cleanup.

Actual F-service tests pass for P29V1/ZSTD_TU/ZSTD_ROUTE at W1 and W30:
two inputs fill the output cap, a third fails, cancellation succeeds, RESET
marks that suffix unavailable, another C cannot refill before input release,
and releasing the exact input admits a refill. A separate P29V1/W1 case
observes original-deadline retirement without RESET/reconnect or another
request triggering a sweep. Both prior inputs remain byte-exact attachable,
another C's reservation survives, and lifecycle ownership returns to its
two-input baseline. These cover output capacity, not every D11 resource cap.

Frozen private source base: `96733809` plus this change. Artifacts:
`/tanksmall/scratch/tmp/p51-d11-output-cap-current.20260925a/work/artifacts/`.
The canonical targeted `make -C unittests p50cacheservice.log` suite passes,
exit 0; log `default-service-final-automake-r1.log` SHA256
`e4a2bff352874f6a8b25450eb2b4c65cec79b30fa38cc9891bbc75572245c6d4`.
Binary SHA256:
`3753225696efba51f9b6f623d4bc57e1617bfef233b7d4fd966856176161a3df`.
Focused matrix log `cap-matrix-final-r1.log` SHA256
`b8875625094928c65d4c15bb812db3ff629380d52ca5a3d7e7be0573aeb1e1dd`;
expiry log `expiry-focused-r1.log` SHA256
`d0c8250c632a15e9c95209a8ffa711a6ebe8c8a21c25ae95d939bb8b51477542`.
Both focused runs exit 0. The direct default invocation that omitted
`ICECC_TEST_READY_CLOSE_SHIM` failed and is retained separately, not counted
as a product pass. The full targeted `p50endpoint.log` suite also passes:
`default-endpoint-final-r1.log` SHA256
`d7e05f509bbe292156bdc2809ecc448bb1f43381974900c77ec641e1744c11a7`,
binary SHA256
`27ceeea07884319cf6a5d4cab158509b87c707b4cab296f28e3452c5aaaef0d8`.
The private
service run predates the separately tested sender-accounting change;
combined current-tip QA and the remaining full W30 plan are still required.

### R1/R2 source-check repairs

Canonical QA on `422932c9` exposed three stale supplemental source checks:
assignment identity matching counted a longer identifier, CACHE_SESSION
still expected a single ordinary protocol instead of the R1 bridge range,
and dispatch still expected the pre-R2 discriminator spelling. The repaired
scripts check the current R1/R2 admission predicates, keep descriptor transfer
checks scoped to their functions, and retain a separate destructor ownership
check. All three scripts pass; deletion controls reject missing assignment
identity, protocol bounds, destructor clearing, either protocol helper and
the combined dispatch rejection. These source checks supplement, not replace,
behavioral tests. The original full QA run remains separate and unqualified.

Evidence: `/tanksmall/scratch/tmp/p51-422-sourcefix/logs/`.
Passing logs: `assignment.log` SHA256
`70df45b2969f2484f32dddacce7ff6fc50ddde72f42ab5d4aed08999e61a6d6b`;
`cachesession.log`
`ab40b318682d6932e8e659e02ae1a713fb113afafdd042b11911854624c6117b`;
`daemon.log`
`2cb671ddcdb04f2127e829f3d1815da6bb9725519bf1b3d3b2c21ae15e8705a1`.

### Full Python integration follow-up

The unfiltered integration suite on runtime `a93b7595` completed with
1,579 passed, ten failed and seven skipped in 655.74 seconds (exit 1).
Nine failures came from a snapshot fixture writing a fixed filename inside
the read-only checkout; one came from an unsorted formal distribution
manifest. The fixture now uses pytest's per-test temporary directory, and
the manifest is sorted without changing membership. The two affected modules
pass all 47 tests against the `96733809` source plus these corrections, using
the same read-only source mount (4.57 seconds, exit 0).

Retained logs under `/tanksmall/scratch/tmp/p51-a93-combined-qa/tmp/`:
`integration-python-tests.log`, SHA256
`33698ec375fab14153ab32db0cd3fbeba454d58fcd7f747f20cdf167b8cee1a8`;
`967-python-fixture-fixes-r2.log`, SHA256
`aead754f45c88346a4f78d225eb05606491bb78555accf780b47da7c82ebc20c`.
The focused rerun is not a full-suite pass. Full supported QA and mixed R2
qualification remain pending. The unfiltered suite includes thorough tests;
the retained-header authority test's runtime is being measured separately.

### Consumed-reservation recovery model

The focused `cache/formal/run_consumed_proof_tlc.sh` gate passes 15 rows:
six bounded topology safety checks (both 2/3/4-to-1 directions), six directed
recovery/reset witnesses, and three expected invariant failures for clearing
consumed proof, selecting another C's row on the same F, and rearming credit
twice. Only the target and one sibling have full reservation state; other
links have abstract progress witnesses. This is not exhaustive W30 execution
or a proof of C++ refinement. Cross-F reservation tables are separate.

Model SHA256 `5ef74df6bdcb48d391ec9768db24d5ec2fc0f6c35a04dd1dcbaed38026533277`.
Retained focused log:
`/tanksmall/scratch/tmp/pipeline-consumed-proof.9yrVeS/consumed-proof-focused-r1.log`,
SHA256 `21dbde965884f06bf33f17f0322b164b9bcbba61c270208ebb93fe0f19698cd9`.
It records `CONSUMED-PROOF-TLC PASS rows=15`, exit 0. The aggregate recovery
runner now includes these cases (43 rows), but no final aggregate pass is
claimed from the earlier run whose outer exit status was not retained.

### Opt-in R2 sender wire accounting

Normal sender retirement now emits its final interval only after the socket
is closed and writer ownership, active requests, receipt readers and ACK
pumps have quiesced. Diagnostic-only task counters do not change operational
reader/pump flags. Repeated retirement is idempotent; observer delivery occurs
outside the transfer mutex. No extra ACK, shutdown protocol or blocking
destructor was added, and disabled diagnostics skip this tracking.

The final full sender suite passes on sender SHA256
`66316a6c888523f89f778c2043310939e097ebf21d2bc0a5d8d993ad17afaa5b`
and test SHA256
`71ee09dc8fb1cb0131628895a3b19fc62856ad21eaad82786593aa064a493f05`.
Evidence root: `/tanksmall/scratch/tmp/p51-physical-retire-build-r1/`.
Full log `work/artifacts/sender-full-r2.log`, SHA256
`36f1ba06baf87dfd00721f33f031ce5b113e0074dfed40bcf509b1fd7e31bb8c`.
Tests cover all three W30 profiles, final interval totals against F socket
observations, repeated retirement, and a held-reader/blocked-writer case
that forbids a terminal interval immediately after retirement is requested.
Removing finalization calls fails at the bounded terminal-event assertion
(expected exit 134): `work/artifacts/retirement-delete-mutant.log`, SHA256
`a32312d55b936f88ffd147965fbba14ea29cd35f3f97e06314449d83d848f6e6`.
This is sender qualification, not independent F ACK/release evidence or
combined service/farm qualification of the new change.

The combined runtime at `a93b7595` passes the full default service suite from
a clean build, including the repeated W30 recovery cases. Later `2847508c`
changes only formal files/docs. Clean suite log SHA256:
`6e53bf55002a5e0ee79affcbaf4eb341f94f62d3c1900e31ac110ea9e74d1c3d`;
test binary `98148a9c04fe1b0a8934ba180def6f983d33d02bb695cb89d7c98afa27d051d2`;
service source `4ea97c7e106e45a949bdc01597b91b8970d70f8dc174bff739b3b2f322aa1713`.

The same clean binary passes the diagnostics-on ZSTD_TU W30 cancellation/
recovery selector and actual collector conservation: 32 source rows, 30
interval events, 30,153 C-to-F bytes (18,727 job / 11,426 shared), and 4,321
F-to-C bytes (3,596 job / 725 shared). Zero unavailable job snapshots; C raw
credits return to zero and FDs to baseline 16. Physical completeness remains
false and settled relationships zero. Final trace:
`/tanksmall/scratch/tmp/p51-a93-combined-qa/tmp/d17-zstd-tu-final.jsonl`, SHA256
`f28d18700b9c0cc06832445aec11f19f4ea222d3c8be6f7750ddd6149a8ce50a`;
the adjacent `.exit` records 0. Collector/tracing Python modules pass 332 tests.
This qualifies these gates, not the entire W30 plan or farm deployment.

Earlier combined attempts are retained separately: an archive-mtime/reused-
object mismatch linked an old service object with a new route-owner layout
and aborted; the clean build then exposed a dedicated scratch-directory
permission error after UID drop. Cleaning the build and setting only that
scratch directory to mode 1777 preceded the passing run. Neither failed run
is counted as product qualification.

The sender now supports a bounded interval observer, enabled only when the
existing `ICECC_P50_DIAGNOSTICS=1` setting and an observer are both present.
The normal disabled path does not allocate the R2 accounting maps. Frozen
job identities include C/F/logical link/TU/raw digest (TU zero is valid).
Job counters are cumulative; interval counters are additive and include
shared control traffic. Bundle attempts include replay attempts as a subset.
RESET confirmation is distinct from an actual completed ACK write.

The observer delivers traffic after the caller's result, including delayed
ACKs. False returns/exceptions invalidate interval measurements without
changing transfer outcomes. Duplicate completed results retain their exact
key but omit duplicate measurement payloads. Retirement is per physical
link, not a global completeness flag. The service now installs this observer
when diagnostics and `ICECC_P50_SOURCE_RESULT_TRACE` are both configured.
R2 emits v5 source results and additive interval events; R1 retains its prior
format. The collector distinguishes unavailable data, repeated references,
cumulative job snapshots and shared link traffic. Increasing snapshots are
aggregated once per exact key using their greatest value, independent of row
order. This does not prove complete farm bandwidth accounting or a performance
improvement. Sender ordinary-stop terminal events are now qualified above;
combined service qualification and independent F settlement witnesses remain
pending.

The emitter's focused ZSTD_TU cancellation/recovery selector passed after
factoring both interval forms through one serializer. Retained log:
`/tanksmall/scratch/tmp/p51-v5-trace-private/interval-factor-r2.log`, SHA256
`86bb6097a3909ec070baab3f2f43c3f16d3824480afd918daa9a21a05749d3bf`.
Trace: `/tanksmall/scratch/tmp/p51-v5-trace-isolated-r1/work/tmp/r2-interval-factor-r2.jsonl`,
SHA256 `fc3f92e70751cd91b76adf7964517c7bd5f2e780b22a637ab76b9cfcd083eca6`.
The earlier actual trace conserved 29,537 C-to-F and 4,321 F-to-C bytes across
16 intervals and 32 job keys, including cancellation; closed/settled remained
false. These are small fixture measurements, not farm results. The imported
emitter preserves the newer receipt-ledger test hook; its combined service
qualification with the recovery fix is recorded above.
The focused Python group passed 474 tests before the final cumulative-snapshot
aggregation correction. After that correction, the collector/report module
passed all 307 tests, including both snapshot orders and legacy additive rows.
Collector SHA256: `0ea7dd0ced12e8094484361c5c12230f01fdc1976221061a5f7ddf5d1388c6fa`;
collector test SHA256: `19bf7cf067b46e8bc7391f3bc67f18ee3de97973ffa97c5ebb29d159ff16c9cc`.

Frozen endpoint and sender full suites pass. Focused runs also cover all
three profiles at W30, lost-COMMIT and repeated replay, failed-terminal keys,
duplicate references after recovery, false/throw callbacks, and fresh jobs
after RESET without fabricated ACK-write checkpoints. The final sender run
includes the last reference-key correction and narrow GCC13 coroutine warning
workaround. Endpoint files did not change after their full-suite pass.
Combined qualification with the current service/D11 files **failed** on
`1232033d`: the default service suite passed three repeated W30 cycles each
for P29V1 and ZSTD_TU, then failed ZSTD_ROUTE cycle 1 with repeated F
TerminalError results and 30 caller receive timeouts. Cause is under
investigation; this does not establish whether the failure is a pre-existing
race or a new regression. Do not treat this candidate as deployment-qualified.
The retained combined log is
`/tanksmall/scratch/tmp/p51-service-combined.7PdVFL/tmp/service-default-1232033d-r1-test.log`,
SHA256 `f6411136f01a25fa80e1feae9934dcc27c614d815032375b45b062781bac072e`.
Use that preserved copy: later executions overwrite the build-directory log.

The recovery fix addresses a pre-existing inconsistency in
`settle_p51_interrupted_job_on_owner`: a surviving consumed reservation loses
its binding/ordinal/generation, but RECOVER requires that proof while it is
still consumed. The same global scan also lacks C/relationship/epoch filtering.
Both defects are present before accounting (`017dd68f`). Deterministic
pre-fix regressions reproduce cleared survivor proof and acceptance of a
mismatched relationship identity. The fix preserves survivor proof until
RESET, checks exact link and reservation ownership, and leaves credit rearming
to RESET. The cross-C test observes exact cancelled-row retirement and one
outstanding survivor reservation after RESET/CONFIRM.

On base `1232033d` plus this fix, both focused regressions and the full default
service suite pass, including all nine repeated W30 cycles and receipt-cap
cases. Full log SHA256 `104b1ee85e5368b88e85ef7c74e93fd30a3080b06c5cc7ff48f94318fa39e12e`;
service source `b0d9fdcdfd1054f91b0e5eb174b3928fc6bce6209f2a9e52094a320508c8571a`;
test source `151ec52c101253f6883972349e8657fba84f2b55d989f3e71866dcee49bf9898`;
test binary `ff6da433ca39625adfbc17ceab39d4d60cbc8a9272b0d69b7f21cd7ace9cac5e`.
Evidence root is `/tanksmall/scratch/tmp/p51-service-combined.7PdVFL/`.
Removing only the reservation ownership filters makes the cross-C test fail
at its no-early-retirement assertion, with survivor-proof preservation still
enabled. Mutant log SHA256:
`adc77db9cecf8923bc81264493b9471892f96fa9c1baa7a17bd107cfc1c828b6`.
The qualified binary is preserved as `tmp/p50cacheservice-qualified-fix`;
the build-directory binary was subsequently used for the negative control
and must not be mistaken for the passing binary.
This pre-integration evidence does not close the rest of the W30 plan;
combined service qualification is recorded above. The original intermittent failure
remains retained separately; these deterministic regressions establish real
defects without claiming every earlier timeout had the same cause.

Evidence root: `/tanksmall/scratch/tmp/p51-r2-accounting-6faf9b21/`.
SDK `icecream-dev:sdk-ubuntu24.04-be1f3d5a7160`, image
`sha256:7fb2663633c0557ebc8efdc5e9fffff4a8e727ab7899ce5209753f6c30f1bfd0`,
GCC 13.3.0; containers limited to 2 CPUs/8 GiB. Production and test-hooks
endpoint archives were explicitly rebuilt before qualification. A stale
archive link failure and earlier private accounting failures are retained,
not counted as passes.

- Full endpoint log `tmp-clean/accounting-endpoint-full-r1.log`: `fff820dbbf0d72e64f5b238b2e5c95c8e8c3f2afc5fe3b1be7caafde46d5f5bd`.
- Final sender rebuild, focused selectors and full suite log `tmp-clean/accounting-reference-final-r2.log`: `cec94304f3852c51b49121c03ebab4f358208cc3c50c24b49e7cda85e796b315`.
- Final sender binary: `a71c7f5200e50a25c43b33665b327bf99e864b9c481b32216a902bb243912628`.
- Endpoint source/header: `6f1e77e3f200cd012e5086f8dabd4eb85ed8a6a7ff2b00f8bf6333e23a79773b` / `cd3e60ff51741f83a98575263b8132f745e4f5de45d094eb14f1b5468ca3f0a2`.
- Sender source/header: `6d3cf6fbb41a07b1349ebc0c359a82697d5b4bba7ad1395553997527757b84e0` / `c2961614b14b804ed70a06fb7a7fe3725d45e41083c500e0929fd19fdfbe477e`.
- Endpoint/sender test sources: `5fe62d35afa46fedae8d4ad2c9ba7474d935ca47a138e7dcd493e2d07a0132ab` / `d1b293851c0304f7a6d2c1c1f7f00a3b792e3579df87fed2a370de4e99a1687d`.

### Real F receipt-ledger pressure

The default service suite includes W1 and W30 receipt-pressure cases for
P29V1, ZSTD_TU and ZSTD_ROUTE. A controlled client endpoint talks over one
TCP connection to a real F SidecarRuntime, reads exact commit receipts and
withholds ACKs. An owner-thread, test-only snapshot checks K/Q, the selected
window, exact retained receipt ordinals, pending ordinal, outstanding
reservations and published input-record count. At capacity, a direct probe
of the service admission guard rejects the exact next prepared binding
without changing those counters. After a real wire ACK, that same binding
commits on the same connection. The test waits for F to process the final ACK,
checks that receipt rows drain, then sends orderly CLOSE.

This qualifies the service receipt cap/refill, not the independent raw,
encoded, output or metadata limits, peak memory, or all of D11. The probe
does not send an invalid over-window JOB_BIND on the wire. Earlier endpoint
tests cover the separate endpoint window guard.

Frozen source: base `c02dac8c`, three changed service/header/test files, SDK
`icecream-dev:p51-multilink-retry-r6-iptables` image
`sha256:ab5df547b92ed5998d46ab828e24bdde81cf86f5e7716797ac65a6ed90039eee`.
Runs used 2 CPUs, 8 GiB, no container network and explicit scratch mounts.
Artifacts: `/tanksmall/scratch/tmp/p51-d11-receipt-eEWKvO/artifacts/`.
Both `p50cacheservice --d11-real-receipt-ledger` (six cells) and the default
Automake `make -C /work/build/unittests p50cacheservice.log` pass, exit 0.

- Six-cell log SHA256: `4d2aee46c5f3c1563d45f77ceefea820ff6a38541014d85a87bec94f3834b396`.
- Default test log: `d27eb8eed0e962f9f7670ccea54a09f4cd1d15acf4757141a82bde0b1a524b0a`.
- Default `.trs` (PASS): `7f175f2f5d04511903d382671ba96ca623e127f57730d53966587cf2c2689229`.
- Binary: `c8bfc0d001501cdca048c1036ec414d244db503d1206d55aa5abc090bb74eeb2`.
- Service header: `8bae8b40b3f1d15af23bb4a536778b075dddd66cc3ca59d88562c629418769e4`.
- Service source: `43a16329cf5d0ff9fd3d3946288a4995f9f72d784814ef8b7908b37492e2bd68`.
- Test source: `85ed8df58528bf68ce71e92aeb0fc84747426cf2bfe8aa6651bee40bcfc0f0cc`.

The retained initial focused failure sampled Q immediately after C wrote
the ACK, before F necessarily processed it. It is not a product failure or
a passing gate; the corrected test observes F's bounded eventual Q instead.

A test-only follow-up also checks the client's negotiated LinkState window
and profile. Its six-cell selector passes on test source
`25c63cf22d226053f476899aa2ede43e14f4a0b87f264eae6dabfd6feca51947`,
binary `b21026d6ef6a141fbd23aed51aa8d71c205370f84b6c75d8d84c08f818982f83`.
The selector log has the same SHA256 above. This follow-up did not rerun the
full service suite; that full-suite result belongs to the preceding frozen
source. One failed launcher invocation (exit 126) is retained separately.

### Repeated W30 recovery on persistent runtimes

The default service suite now runs three cancellation/reset cycles for each
of P29V1, ZSTD_TU and ZSTD_ROUTE, retaining the same C/F runtimes and route
owner across each profile's cycles. At every cut it requires 30 active source
operations, 30 distinct complete C bundle writes, exact held raw credits
(1,045 bytes), and a held materializer. Each cycle verifies the selected
caller fails, 29 survivors attach exact inputs, and a fresh transfer succeeds.
Cancellation positions are submission indices, not claimed wire ordinals.
Source operation/raw credits return to zero and descriptors return to the
warmed baseline of 16; connections progress from two to four per profile.

The focused nine-cell run, full default service suite, and full sanitizer
service suite pass. Only the existing sender observation hook is forwarded;
the patch changes no wire framing or production admission limits. This is
small-input source-transfer coverage, not compiled-output, bulk-RSS, or full
F-side retained-memory proof; the broader D17 requirement remains open.

Evidence root: `/tanksmall/scratch/tmp/p51-d17-fd-repeat-r1/artifacts/`.
`repeated-r1.log` SHA256:
`4391bda37a254f48f460acbe239fe2814d80e5b1b8b15257558a30f27db988cc`.
`defaultservice-r3.test.log` SHA256:
`9ffae07802bfc2fa4c868c6737f6da92411d029d846acc458c7be6b026acd5a0`.
Sanitizer log SHA256:
`bc17a489f71ddd406836a4dd74835bfaee500754651950064907034a3ba4f2a4`.
Both default and sanitizer Automake results are PASS/exit 0. Test TU SHA256:
`67891838fe3b315c14125948df7f205d7093cbe0f6c580b709e75d31749739ca`;
ordinary binary SHA256:
`1accce4b61cd01528c93a80b9705a3a79c91615d2a21043adb4576012f781bc6`.
SDK image reported by the test runner:
`sha256:ab5df547b92ed5998d46ab828e24bdde81cf86f5e7716797ac65a6ed90039eee`,
limited to two CPUs/8 GiB with scratch-backed temporary storage.

ASan/UBSan/LSan instrument the test TU, service, route owner and sender;
linked endpoint/protocol/transport/service archives are not instrumented.
The successful sanitizer script deletes its temporary executable, so no
sanitizer-binary hash is retained. Script SHA256:
`9ecbd66389dda492eec120bdf08b2bf5598c448c9e7ce46d67b090cb2fc4e5c2`.
Earlier fixture-identity failures and the combined prerequisite-build/test
timeout are retained but excluded. Focused reruns use the individual
Automake `.log` target: `check-TESTS` builds all `check_PROGRAMS` first.

### Task-count test after thread exit

The daemon task-count test now allows up to two monotonic seconds for the
joined test thread's `/proc/self/task` entry to disappear, polling every 1 ms.
Its live-thread detection assertion and the production single-task fork check
are unchanged. This addresses a post-join test race reported by the other
candidate branch; it does not relax the daemon's runtime requirement.

Luna's strict SDK compile and 10 sequential plus four concurrent focused runs
pass, as does `p50daemontaskcount-source.sh`. Evidence is retained under
`/tanksmall/scratch/tmp/p50-task-count-poll-dc3ac75a/artifacts/`;
`repeat-parallel.log` SHA256 is
`5b7d4843b2a8e3e730cb85a5e3f0f9b9e7c87b2dd921c5a806f383d02525d003`.
Test source SHA256 is
`9892bb87a94cf5144fb6b7b5c597ba6b3bb333893f0608620120192001f01e43`.

### Supported Docker process-gate entrypoint

`ICEFARM_TMPDIR=/existing/scratch make dev-gate GATE=p51-arm-expiry`
now builds a unique checkout snapshot and runs an allowlisted process gate
inside a bounded disposable container. The runner supplies the `icecc` test
identity, private bridge, NET_ADMIN, offline locked Python environment and
short scratch-backed `/tmp`. It rejects skips and incomplete pass markers,
retains artifacts, and removes only its labeled container/network. Selectors
also exist for W30 cache restart and scheduler restart. The scheduler and
ordered-chain qualifications below use this entrypoint; the original
18-cell cache-restart selector has not been newly qualified through it.

Luna's 40 focused bootstrap tests pass, including cleanup timeouts and the
runtime result policy. On nas642, direct `dev/bootstrap.py gate --gate
p51-arm-expiry --farm farm.json` passes: build 186.04 seconds, three-profile
gate 52.10 seconds, both exit 0, with no remaining owned gate resources.
The unconfigured `make dev-gate` route was checked by dry-run, not a second
full build. Evidence: `/tanksmall/scratch/tmp/icecream-qa-_n481ial/result.json`,
SHA256 `21219676658d615ecbdfaf443e2c749c9f0eafd461541a6f19aac35648877368`;
source snapshot `b36f1b3d3b4ca13163208cd00b24bab728a20a7b1f52be946ecb0d349e0efced`.
Outer gate log SHA256
`f936e4d695c63fb042add6be78a137b1bc1bd2718404cc7e98ee35b503960a5a`.
The SDK is `icecream-dev:sdk-ubuntu24.04-be1f3d5a7160`, with 2 CPUs/8 GiB;
later README-only wording clarifies the required short mount path.

### Active ordered W30 restart chains

The supported selectors `p51-scheduler-f-restart-w30` and
`p51-restart-chain-w30`, plus the original `p51-scheduler-restart-w30`, pass
for P29V1, ZSTD_TU and ZSTD_ROUTE on one exact private snapshot:
`69b03d877d3f7291b1eedad8c0cb5148ad98f0c810d8fad02d18be3d5d8878a5`
(base `6a28ea27`, prior to the independent source-FD change).

S→F holds 30 old-scheduler receipts, restarts S, then holds the next 30
receipts while replacing F. The second window is deliberately discarded;
all 30 callers settle non-success without a harness timeout or signal exit.
Their measured bound starts before assignment and is conservative relative
to the 60-second source-arm budget. A fresh window produces 30 exact compiled
objects joined through assignment epoch/nonce, TU and C/F store identities.
F→C uses C2F2, holds W30 across each affected restart, proves unaffected
C2/F2 progress, rejects the old F assignment and retains all 30 valid old-C
source attachments. All 30 fresh post-C source inputs attach exactly, within
their original absolute deadlines. This second gate proves source attachment,
not compiled object output. The S-only selector retains its 30+30 behavior.

All three Docker runs report build/gate exit 0 and PASS; 42 bootstrap tests
also pass. Retained `result.json` directories and outer gate-log SHA256:

| Gate | Directory under `/tanksmall/scratch/tmp/` | Gate seconds | Log SHA256 |
| --- | --- | ---: | --- |
| S→F | `icecream-qa-85db__9o` | 148.493 | `06e270c75c893c7c80f28b58e4917b7c5b47d12a3de0a35ada043fe84df7993a` |
| F→C | `icecream-qa-vtlbaqk6` | 140.966 | `57f94afe6d9ad9e9389e0f39a820faafc8ff268831c34381c861676b80b78856` |
| S-only | `icecream-qa-8ggjc_2j` | 136.818 | `c2d06713ed1b6d7cc073abdbf2baa255eaa6c7a6f06e191be08f55608845a6d5` |

Separate clean builds took 193.806, 183.292 and 182.649 seconds respectively
at 2 CPUs/8 GiB. Earlier failed S→F runs are excluded: one incorrectly applied
the discarded-F-window assertion to the released S window; another compared
public/internal profile labels without their explicit mapping. This evidence
does not close all D09/compiler-loss cases or replace final combined-candidate
qualification with subsequent product changes.

### Source descriptor lifetime

R1/R2 source-transfer callers now release their owned source descriptor after
the exact handoff ACK; later reply processing cannot close a reused descriptor
number. R2 source reading releases the service copy on success, read failure
or cancellation, before scheduling route work. This does not change wire
bytes, deadlines, queue limits or input ownership. The R1 service-side source
descriptor lifetime is unchanged. Queued/pre-handoff source backing is not
covered by the 2 GiB source-vector credit; no memory-peak or disk-leak claim
follows from this change.

Luna qualified the five-file patch on base `6a28ea27` in
`/tanksmall/scratch/tmp/p51-fd-lifetime-build.wu30SL/work`.
The selected Automake run passes `p50daemoncontrol` and `p50cacheservice`
(2/2, no skips/failures). Cases include held R1/R2 replies, descriptor-number
reuse through teardown, cancellation before ACK, transport loss before ACK,
exact R2 committed bytes, active-read peer cancellation and a deterministic
read failure. Reverting the early ACK close fails the intended
`sender_fd_closed` assertion; restoring it passes. Setup errors and the first
stale-library negative-control attempt are retained but excluded as evidence.

Default service log SHA256:
`2e3a6b08a317bc11fbd45f8eba79f0d4adb4fc4299ada425a246fabf7ddf2848`;
default control log:
`69f1a03b01104868280cf52a6a9e27be1f3d83ed15a9168dbb967912a4d4f1ae`.
The negative-control and restored logs are respectively
`artifacts/control-mutant-r2.log` (exit 134) and
`artifacts/control-restored-r1.log` (exit 0). The final header comment is a
documentation-only clarification after testing. The outer default run spent
about nine minutes building unrelated check programs; focused reruns should
build the required archives/binaries explicitly and use `check-TESTS`.

### R2 source-trace measurement gap

Source audit of both `ef29049c` and product `03d108a3` finds that R2 results
leave `c_to_f_bytes`/`f_to_c_bytes` at their default zero and assign
`attempts=1` even on recovery paths. `bind_wire_evidence()` is used only by
the serialized R1 sender; its `ClientByteTotals` log explicitly excludes R2
completions. Follow-up call-path inspection and a trace-enabled R2 run show
that the v3 service emitter is called only by the R1 transfer path: actual
R2 queued completion emits no source-result row. The earlier claim that it
prints R2 defaults was incorrect. Neither missing R2 rows nor internal zero
defaults prove zero traffic or absence of replay; they cannot qualify R2
bandwidth acceptance.

The v4 repair now emits explicit R1/R2 mode and completion-stage labels.
R2 post-read dispatch completion produces a row, with unmeasured attempts,
wire bytes and R1-only timing fields set to null, not zero. Pre-dispatch
refusals remain outside this trace. Availability flags are independent; an
early R1 failure can lack attempt counts while retaining measured timing.
The collector validates these rows but refuses numeric acceptance when
required measurements are unavailable. Trace formatting cannot affect the
reply, and optional fallback hashing runs only with tracing enabled.

Luna's full collector module passes 292 tests; the production-flow source
guard and deletion controls pass. The full service suite, including the
current Runtime expiry tests, reports PASS in the clean private build at
`/tanksmall/scratch/tmp/p51-r2-accounting-6faf9b21/build-clean/unittests/`.
`p50cacheservice.log` SHA256
`3b4989353566846488d73d3cbac40af72ce4ebfbe70ed572c7c36a5b5c2d18bc`.
Actual emitted R2 JSON was parsed and correctly refused as numeric evidence;
JSONL SHA256 `ebeb0a7c9a9434549d9fada50672d4709d01023d3e66866244b7f18a80dd85f8`.
The full synthetic suite reuses assignment IDs across independent fixtures,
so its combined trace correctly fails duplicate-ID validation. Unchanged
R1 row 174 was separately parsed and accepted with measured counters:
`tmp-clean/source-result-r1-positive.jsonl` SHA256
`9c049b4833dcc342e72039cb40085d3cbf9013983b57b513618b2fcf5e793a51`.
The post-suite combined-trace parser failure is not a service-suite failure;
duplicate validation was not weakened.

Exact R2 per-job traffic/replay counts and separately identified shared
recovery/control traffic still require implementation and conservation
tests. Overlapping per-job differences of shared counters would double count.
The emitter repair does not close bandwidth acceptance or the W30 goal.

### R2 source-deadline expiry coverage

The default endpoint suite now exercises four expiry stages for each of
P29V1, ZSTD_TU and ZSTD_ROUTE: a mocked consumer rejecting an expired
reservation before bind; an already-expired returned lease rejected by the
endpoint; expiry while the decode worker is held; and expiry immediately
before publication authorization. Assertions require no committed input or
receipt, no publication authorization, and drained raw/encoded/decoder and
detached-history accounting. Held-worker coverage also checks owner-loop
progress and retained charges until the worker releases. The before-bind
mock does not prove real service reservation expiry or absence of late ARMED.
Publication already authorized before expiry is outside these rejection cases.

Luna's focused 12-cell run and default endpoint suite both exit 0 on the
same binary, based on `36ee0fe0` with only the endpoint test TU changed.
Evidence root: `/tanksmall/scratch/tmp/d08-expiry-private/logs/`.
`d08-r2-deadline-stages.log` SHA256
`981379dfa1eb2f7d46409aa2d5470c1006a9cf0d521728e24bf91fb90424b60a`;
`d08-r2-default-endpoint.log` SHA256
`21266fbcadf3d7a6430c561221e178580d4fb70bf8ebc44f865ba3466df0455b`.
Test source SHA256
`0d6c34730ad0acd7816cfa38132579be859b3587a7de2333e3d346094d8f9773`;
binary `47b0e0e00e39a3107857834809ceb4e35f9f22bf164c76e2119d30b966168cdf`.
SDK image `icecream-dev:sdk-ubuntu24.04-a9fa596a15e7`, 2 CPUs/8 GiB.
This adds regression coverage, not a product change or full D08/C03 closure.

The default service suite additionally tests real `SidecarRuntime` expiry
across all three profiles. With the owner executor held, a reservation call
expires without returning an ARM result; after releasing/draining the owner,
the single-slot capacity remains usable. A successfully reserved but expired
request is denied by actual initial HELLO lookup and JOB_BIND consumption.
A fresh reservation then passes those same lookup/consume checks, ruling out
an independently invalid binding as the rejection cause. This is Runtime
result/lease coverage, not daemon wire ARMED or Client/READY race coverage.

Luna's strengthened focused selector and full default service suite both
exit 0 against product `03d108a3`, with only this test TU changed. Evidence
root: `/tanksmall/scratch/tmp/p51-c03-sidecar-expiry-r1/`.
`build/unittests/p50cacheservice.log` SHA256
`57d5ec2625c0e0c3665e934359496965013b1f9551cfec867798b9f1c93cf64e`;
`.trs` reports PASS; `tmp/service-full-r3.exit` records 0. Test source SHA256
`6d6c0e5a802b9f98b1037fc65349ea0ced7d8beb1e6cdbba343dfe3487d5bdaf`.
The initial full attempt failed scratch-directory permissions before the new
test; the following make invocation did not rerun the test and is excluded.
The passing run used corrected task-owned scratch permissions and an actual
test execution. No product deadlines or limits changed.

The opt-in `p50daemonpositive-p51-arm-expiry-check` additionally exercises
the daemon's final ARM validation on the actual wire, for all three profiles.
A test-only, exact-request pause stops the daemon after Goodbye has been
written and before final validation, following a successful reservation.
The fixture observes the stopped process before its original two-second
budget expires, resumes it after expiry, requires End followed by EOF with
no ARMED, and then proves a fresh ARM succeeds on the same daemon. This
covers that expiry boundary, not every Client/READY replacement race.

The named gate passes against private base `6ff64c41` with these changes.
Evidence root: `/tanksmall/scratch/tmp/p51-c03-wire-arm-r1/tmp/`;
`arm-expiry-target-r3.exit` records 0; its log SHA256 is
`31558b8e383e69a0e15d25dcdd92ae7d42ccbad8630d3e5d7c1fb6477d2dd712`.
Daemon source SHA256
`0effbc06a96df2d6c8bc40e379e224b293ade6c8a45ef123a75d669580ee57cf`;
test TU `e39ecfb75706aefdf64852a14f82136585a43fc95564c7353b95cf7ec848bbb0`.
The runner requires a disposable root container, an `icecc` account and
explicit scratch; each profile has a 45-second timeout plus ten-second kill
grace and unique retained logs. Missing-scratch and missing-opt-in attempts
are excluded, including a default-wrapper skip (77), not counted as passes.
The corrected default daemon wrapper also exits 0 on the same binaries:
`default-positive-r3.log` SHA256
`cbe0d6fe7eb5644291d7711cb2ff402387e657a2e8dd731022b8ccbc80ff166d`.
Daemon binary SHA256
`8127cd31311f7f44a933a6808fa4b64dec733dc64b08bb9aff02780cd8532b09`;
test binary `cae2b77cf12c44afbaa77d219faa1c806a8059f07f06591ba28b79121558975d`.

### Shared recovery failure handling

A failed recovery attempt now marks its current physical generation unusable
and closes its socket even when the coordinating caller has already recovered
an exact positive result. Cleanup belongs to the shared recovery routine,
so admission, receipt-wait and credit-recovery callers use the same rule.
It preserves positive results, retained suffixes, original deadlines and the
stable recovery floor; it does not increase retry limits or change the wire.

The default service suite includes four named cancellation/recovery scenarios
across all three profiles. The positive-coordinator case loses the first
COMMIT reply, cancels a middle reservation, then disconnects before survivor
replay. It checks the exact coordinator/receipt, both RESET snapshots, three
connections, exact survivor attachments, original deadlines and released C
operation/raw credits. The committed-attempt case verifies retirement and
replacement admission without claiming real compiler-process quiescence.
Branch-entry and actual recovery-owner test observations are separate hooks.

Luna's ordinary production build, full sender (37 named cases), full route
owner (including W30 topology cells), and full service suite pass. Sender and
route-owner exit files explicitly record 0 under
`/tanksmall/scratch/tmp/p51-d07-positive-owner.PfF4BU/tmp/`:
`final-sender-hook-r2.log` SHA256
`0d8e964c6710b72270eb3ce89190220cba5edfe402cd9334cd4cda220fbf54ec`;
`final-routeowner-hook-r2.log` SHA256
`a107b00f7c09c646f5e8145bff1710d8134507dcb4dd595eb3dab600e66c8e06`.
Full service Automake PASS/exit 0:
`/tanksmall/scratch/tmp/p51-d07-active-current.n5c5bZ/build/unittests/p50cacheservice.log`,
SHA256 `a041296f9ce70a44e48b3eae7be3d70ee714342def4349f7beb5755840550326`.
Service test source SHA256
`2d38ed7a4ddaca821760e678ccc5c6fafcfcbe3dab30ba276071a3a76aef59f3`.
Tested sender source `27f5980cbfc61e31f4bc48b3160cf1ace0f526f1d1e1b8851c6ca9c2b6797033`
differs from published `1ad6d998a2fa0749420358c636b545a9e2c9b01743306b48cd88890f81be5a95`
only in the two preserved R1 explanatory comments.

Excluded earlier attempts remain in the same artifact roots: stale sender
fixture (`final-sender.log`), changed observer semantics (`final-sender-r2.log`),
and missing uv before service execution. The fixtures were synchronized,
the original observer assertion retained, and pinned uv supplied before the
passing runs. Mixed-version/current-image QA, restart chains and the broader
W30 acceptance matrix remain open; these passes do not close the whole plan.

### Consecutive P29 recovery resets

`CRoute::reset_v1_route` now permits a fresh-nonce reset when its codec state
is absent after a previous reset. A disconnect before the first survivor
rebuild previously made this second reset throw. The reset still clears
history through the normal path; it is not a skipped reset or a retry with
an unchanged nonce. The direct speculative-suffix regression now performs
two consecutive resets without an intervening transaction, rejects a repeated
nonce, then rebuilds and commits exact surviving inputs.

Luna's full `p50slice0` run exits 0:
`/tanksmall/scratch/tmp/p51-d07-positive-owner.PfF4BU/tmp/p29-consecutive-reset.log`,
SHA256 `00ccd965068d1462a75009dd842069a4b60435bec2e99eee0f3a73692c3f6240`.
Product source SHA256:
`6ddb4fba49f07351fc7ee4ee5d11aee35684aebff5b3e8961a5e9cffc7d7e0dd`;
test source `9c115067c354957874e891280c28246b36edb9b6b06db7c898ad3dcdd38fde0f`.
The earlier private positive-coordinator service case passed all three profiles
with the sender repair subsequently qualified above. Its diagnostic log is
`positive-owner-allprofiles-r1.log` in that directory, SHA256
`6276413c24540853548e0f31d29bc69d25f30ddbf7a476c01168e5fc048beec5`.
The standalone P29 reset repair did not fix every interrupted-replay entry
path; shared sender failure handling and combined qualification are recorded above.

### W30 cache restart topology expansion

The restart runner now covers C1F2, C1F3, C1F4, C2F1, C3F1 and C4F1
for P29V1, ZSTD_TU and ZSTD_ROUTE. All 18 cells report PASS on the frozen
`f1a2e343` product: 30 discarded old receipts yield noncommitted old callers,
30 fresh transfers commit and attach exact inputs, and every unaffected
sibling completes an attachment between successful SIGSTOP and SIGCONT of
the affected parent. Original caller deadlines are checked with the existing
2-second cleanup grace. This does not establish compiler-process quiescence,
scheduler restart chains, or qualification of later product changes.

Evidence: `/tanksmall/scratch/tmp/p51-restart-w30.RWA5X5/restart-matrix-r3.log`,
SHA256 `d55e17377458e16c9a52837ff4127ddd76dd2c9f212dfdf648cd7d0423f163f3`.
Each PASS requires the individual case exit status to be zero; the aggregate
wrapper exit field was not retained. The transcript ends normally after all
18 markers. Expected old-window `P51_KIND8_FAIL` diagnostics are not test
assertion failures. Source SHA256:
`ed4eb1f897cb9a62ab8021e4f208124708f40b93a8f33da7ee9e7ad5b78a5007`;
runner `83285ef40166ed7ab1ae519c3a6e95feeebfdea37cb2c1ef7fbfaf2cbfa03f36`;
test binary `eef63506601b43a47bfbae3e1a6d456cd1c9f7eb3b8ef212e2b00c1da3164c58`.
The SDK container was limited to 2 CPUs and 8 GiB. Adjacent runners unset
the new topology selector; their shell syntax checks pass separately.

### Retirement-prepared input admission

`InputLifecycleRegistry::begin_attachment` now rejects a lease while
`attempt_retiring` is set. Previously PREPARE reported a quiesced retained
attempt but its old owner could still obtain an input attachment. The added
registry regression failed on the original product specifically at
`PREPARE left the retiring owner attachable before COMMIT` (exit 1), then
passed the ASan/UBSan/LSan lifecycle runner with the guard (exit 0). These
focused outputs were captured in tool transcripts, not separate log files.

A private real R2 fixture passes P29V1, ZSTD_TU and ZSTD_ROUTE: old-owner
admission denied after PREPARE and after replacement COMMIT, successor denied
before COMMIT, exact successor attachment after reset/replay, and one exact
publication and receipt. Full service Automake PASS, process exit 0:
`/tanksmall/scratch/tmp/p51-d07-active-current.n5c5bZ/build/unittests/p50cacheservice.log`,
SHA256 `ef9489479353001b1d6eb77d6a1227f3d2d674a9b1c81ea11cbe37c37fd48676`.
The lifecycle archive, service test and standalone service were explicitly
rebuilt. Private source is `/tanksmall/scratch/tmp/p51-d07-late-commit.r1/source`;
service test TU SHA256
`1b44ad920c543de037e669efc08bee301a507cdba933b9fee00eeafcce6b1b0f`.
Published registry source SHA256
`7b5c3bd6690edee2a947eb82880c4737558f28bae041bd26c7c62008abe439ec`;
registry test SHA256
`2550e2c68aae19a1d86a871a9c1184a0edabdd7b4dfc4cf9f6bb81d8bbfc0cca`.
The larger service fixture is now integrated with the newer replay variants
and passes the combined default service suite described above. This establishes
input-attachment admission, not real compiler-process quiescence.

### Replacement-trigger diagnostics

The opt-in first-latch replacement record is qualified independently of the
worker/history repair. Focused tests cover completed-ledger capacity, expired
unresolved witnesses, precise reason propagation through subsequent route-owner
refusals, first-cause retention, and exact diagnostic opt-in. Default sender
and route-owner suites also pass. No wire format, cap, or replacement policy
changes are intended; no farm speedup is claimed.

Evidence root: `/tanksmall/scratch/tmp/p51-replacement-trigger-r3.Zzu7jW/tmp/`.
The final successful build is `build-r2.log`, SHA256
`d1a8d12604351b61c35fb12f949d524a752514708d1031371fa448715028a907`.
All six entries in `test-statuses.txt` are zero, SHA256
`10d990347707be7d6015fbe51471f4ad199aeccb38e589e94e572a0be26b3f6c`;
`source-hashes.txt` identifies the nine tested source files. Earlier compile
failures in this and prior scratch snapshots are excluded. The endpoint used
was product `27d67566…`, before the worker repair.

### Endpoint mutation gate

All 13 compiled endpoint mutations now fail with exit 1 at their registered
assertions, with passing normal and isolated codec-queue baselines. Timeouts,
signals, compile failures, and unrelated assertions are rejected. The deadline
fixture requires completion before its independent watchdog; codec admission
and slot-release mutations run in isolation, preserving counter increments
when testing only the queue limit. Per-case logs and a status/hash manifest
are retained on success as well as failure.

Evidence: `/tanksmall/scratch/tmp/p50-profile-mask-r1.5HGnlt/logs/endpoint-mutants-final-r1.log`,
SHA256 `b42db9fc714db39847dbaa33528a6e160428c3a8ee9ddc46d5288b07f8ca5fd6`.
Per-case evidence is under `tmp/p50-endpoint-mutants.28cVMc/` in that root.
The tested endpoint product SHA256 is
`27d67566887c6e785b2d203f3e66ed8a0494617a3b2c32aa1e68eb77b9612815`;
the test TU is `0ad245fb9ac05b68d74660a3d8c9919262c994e111605bc36c3155ed01a60f5e`.
This qualifies the repaired gate against the published endpoint, not the
in-progress worker/history repair. The earlier permissive runner's apparent
pass did not prove intended assertions and is superseded by this evidence.

### Worker-lifetime repair and qualification

The earlier endpoint code shared a mutable codec dialogue between the codec pool
worker and owner-side cancellation/replacement/reset. The completion-state
mutex does not protect codec state; rejecting an old completion after decode
cannot prevent an earlier concurrent mutation. This affects the common
materialization path used by R1 and R2. No concurrent-memory-error run is
claimed yet.

A focused R1 adopted-endpoint regression deterministically demonstrates the
related accounting defect: while the worker is held at the existing
pre-materialization hook, cancellation completes and all three pending-byte
counters drop to zero. The intended assertion fails with exit 1:
`held=1 pending_encoded=0 pending_raw=0 decoder_window=0`.
Log: `/tanksmall/scratch/tmp/p51-d06-credit-red.TqMBA5/logs/credit-pin-red-r3.log`,
SHA256 `9fbad87a3ded117f4c295c55fad261579d3f5c855b9e1de27990b710b88537e2`.
Earlier compile and too-small-window fixture failures are not product evidence.

The repair implements exclusive worker codec ownership, exact-current
completion restoration, and byte reservations that outlive the endpoint and
its io_context when necessary. Its connection to measured farm time or F RSS
is unproven.

The repair candidate passes the focused pending-credit regression and
detached-resident model tests, including same-C eviction/re-admission,
aggregate/per-C/total caps, pinned-object rejection, and lease destruction
after model destruction. Refreshed evidence:
`/tanksmall/scratch/tmp/p51-d06-detached.m9ntmb/logs/focus-r5.log`, SHA256
`1a6d7a350f4136b6247677d19246f99459f27274d728bb3e5f5f5593eb0a460e`.
Both `SLICE0_EXIT` and `D06_FOCUS_EXIT` are zero. This is not yet evidence
for real P29/ZSTD_ROUTE held-worker reset/disconnect recovery, healthy
successor progress, full regression closure, or concurrent-memory checks.

A later development-build test reaches a real second materialization worker
after an exact first commit for both P29V1 and ZSTD_ROUTE, resets F while that
worker is held, and observes no second publication. Pending raw/encoded/window
credits remain charged across reset; P29 retains 65536 detached resident bytes,
while ZSTD_ROUTE retains 65536 separately charged history bytes. All measured
credits and namespace/input counts drain to zero after worker release.
Evidence: `/tanksmall/scratch/tmp/p51-held-worker.Y4GvlM/logs/focused-r10-o0.log`,
SHA256 `94be1b9831384688729f459d6f878b8b07fb4323fc70f4793657665500197d8a`.
This used an O0 development build and an older sender/owner baseline, with
endpoint SHA256 `63d5cf58b009859dda5404be0e72cce0f3ebf596b770a61965e7aa65c8f0b11e`.
It does not qualify disconnect or overlapping healthy-successor behavior, nor
the final combined normal-flags build. Earlier fixture failures are excluded.

The refreshed O0 successor case additionally proves reset-induced old-peer
EOF and a fresh-link commit with exact input attachment while the old worker
remains held, for both profiles. After release, all transient byte charges
drain; the successor's namespace, revision, and input record remain intact.
Evidence: `/tanksmall/scratch/tmp/p51-held-worker-final.6pP4qv/logs/focused-successor-r7-o0.log`,
SHA256 `362462d34cbfa087fbc6dc97183dcab1b2fcfeed292b8662720cbb321630c997`.
This snapshot uses published `5199ed83` plus the worker repair and test
overlays, including shared callback-completion lifetime handling. It does
not prove independent peer-close-only recovery.

The subsequent optimized rebuild and combined endpoint, sender, route-owner,
and resource-model suites all exited zero under a strict sequential runner.
The endpoint suite includes both held-worker reset/successor profiles; the
sender and route-owner suites include existing W30 recovery and both-direction
topology cases. All eight authored repair/test files match the tested snapshot.
Evidence: `/tanksmall/scratch/tmp/p51-held-worker-final.6pP4qv/logs/full-normal-r1.log`,
SHA256 `2e15ccc1331e74f92a7f35d1b9b47622c0824c2aa776d3db189fdaef99bc84b2`.
Build flags include `-O2 -std=c++23`; protocol and endpoint archives were rebuilt.
This qualifies the worker repair against these suites, not the entire W30
plan, independent peer-close-only coverage, or concurrent-memory checking.

Independent peer-close-only coverage now passes for P29V1 and ZSTD_ROUTE:
the peer closes after sending the full bundle while the real worker is held,
without calling `reset_store`. An owner-context timer progresses and pending
raw/encoded/window charges remain held. After worker release, the endpoint
returns `Disconnected`; any published input must remain exact, no receipt ACK
is accepted, and all transient byte/history charges must drain to zero.
In the observed runs publication won and one exact input remained attachable.
This does not prove immediate EOF detection during decode: the R2 owner waits
for materialization, explicit cancellation, or its deadline.

The strengthened focused run and full optimized endpoint suite pass on test
TU SHA256 `fed6b9c5adbdd424ff2d4d52a197fad25a8c88b1a21fa2126d28f9131c206ac1`.
Logs under `/tanksmall/scratch/tmp/p51-held-worker-final.6pP4qv/logs/`:
`peer-close-focused-r2.log`, SHA256
`905bd1e112d64c5475c2840b6939c977d850fe9089c9e911f8c94f66db88c6b2`;
`endpoint-full-r2.log`, SHA256
`84b4000ef3def62123d075e3ce90afda31e2d59b25757c48a35079a9b67c7919`.
Earlier peer-close tests printed but did not assert final zero charges; this
evidence supersedes them.

Focused ASan/UBSan qualification also passes: reset/successor and independent
peer-close selectors each ran three times for both P29V1 and ZSTD_ROUTE, plus
the InputRecord sanitizer subtest. Both Automake results are PASS with no
ASan/UBSan findings. Endpoint, endpoint-test and input sources were instrumented,
as were rebuilt protocol50 and localtransport archives; `services/libicecc.a`
remained uninstrumented. This is not TSAN or full-service instrumentation.
Preserved logs in the same directory:
`asan-reset-selector-r3.log`, SHA256
`4e30cae6f1b361c07c64c125019a55a577ba6e57262f102f3fb38dbd04374f62`;
`asan-peer-close-selector-r1.log`, SHA256
`3a885437919d42c510198ea207c8cadbc915273c83c07331e21fb64f0083469c`.
An earlier invocation missing `ICEFARM_TMPDIR` failed setup and is not runtime
evidence.

Focused TSAN executions also exit zero for reset/successor and peer-close,
each covering P29V1 and ZSTD_ROUTE with no reported data race. Intentional-race
preflights on host and SDK container first confirmed the runtime detects a
race. Endpoint/test sources and protocol50/localtransport were instrumented;
`services/libicecc.a` was not. GCC warns that `atomic_thread_fence` is not
supported by TSAN, limiting happens-before modeling. These executions are
not proof of whole-service race freedom. Logs in the same directory:
`tsan-reset-build-run-r1.log`, SHA256
`04b63d11b1e773164c400b484fdf772273b8fbd0b39ea076cb42922207fc98e8`;
`tsan-peer-close-run-r1.log`, SHA256
`6c101ca1426433b8482513d648567f4c56b4dc8671e93b10610eeeedb626eb07`.

### Active cancellation and mixed-load evidence gaps

Queued cancellation now passes first/middle/last submission positions 0/15/30
through actual C raw-credit admission. Each case cancels one of 31 waiting
requests, separately releases its admission-blocking holder, and commits and
attaches the other 30 exact inputs over one persistent connection. The exact
F reservation retires once; no endpoint recovery marker exists at this stage.
Duplicate cancellation does not retire it again, and C operation/raw credits
drain to zero. The test uses ZSTD_TU with W30 configured; it does not establish
simultaneous W30 occupancy, compiler quiescence, or wire-ordinal contiguity
from the fresh global TU sequence assertion.

Evidence root: `/tanksmall/scratch/tmp/p51-d07-queued-r0.1HJjqY/tmp/`.
All three selectors exit zero against the replacement-trigger product snapshot
(before the worker repair); test TU SHA256
`b5b4466ac36da8dbac3e6682e4a4ef11a348fff8d687482a51851d1e256f9b2a`.
Logs: `d07-first-handshake-r2.log` SHA256
`b0cf86d385a406ae40aaccae7fc6fa5d37e0b34d168832df22596fd9effa39fe`,
`d07-middle-handshake-r1.log` SHA256
`6c8bdbd6e384c24998ffc13408f10c1eecc627d7c3214123504205ff0800c531`,
and `d07-last-handshake-r1.log` SHA256
`1d1e1c71d32d91ede83bd73f068cf925029efe2ef4d5d6ce42dd32f7b286c2d2`.
Earlier runs omitted the ordinary connection handshake or required a recovery
marker for an unbound request; those fixture failures are excluded.

This does not substitute for partial/full-transfer cancellation.
A deterministic active-transfer probe now reproduces the recovery mismatch
against product `06716e64`: the first input commits exactly; F holds the second
job's worker after its full bundle, with the successor's complete ordinal-3
bundle buffered. Exact F cancellation is accepted before publication, while
both original deadlines have about 30 seconds remaining. Neither job receives
a result before its deadline; eight physical links are accepted. C operation
and raw credits eventually drain, but successor liveness fails. Evidence:
`/tanksmall/scratch/tmp/p51-d07-active-head.DkM6x9/tmp/active-r1.log`, SHA256
`3afa9c86129243af332a39753998bbd57edb745f11e8e535b67133d7220e985d`.
The observational probe exits zero because it verifies setup and records
outcomes, not because cancellation recovery passes. Its sequential receives
could conceal a successor response behind the other job's deadline.
The corrected regression receives both results independently and fails
specifically at `successor_result.has_value()` (exit 1): both results are
absent at their original deadlines, eight links are accepted, and the exact
cancelled reservation retires once. Log in the same directory:
`active-concurrent-red-r1.log`, SHA256
`200242364ecedb15c819ccb187da39c87ed2d2357db477d2c77437313429987a`.
This supersedes the observational probe for the successor-liveness finding.
The candidate repair passes this focused real C/F regression with
ZSTD_TU and W30 configured: the cancelled caller receives a non-success result,
the exact successor commits within 3 ms, two physical links are accepted,
and C operation/raw credits drain. Explicit process status is zero. Evidence:
`/tanksmall/scratch/tmp/p51-d07-active-current.n5c5bZ/tmp/active-cancel-recovery-r2.log`,
SHA256 `7a376f0f9db6dae84122ec81c534071070aba509c55ce165b0bbf43d2691a9fb`.
This establishes recovery for the three-job scenario, not actual W30
occupancy, all profiles, or interrupted replay of multiple survivors.
The strengthened scenario subsequently passes P29V1, ZSTD_TU and ZSTD_ROUTE:
explicit cancelled-caller error, exact cancelled input absent, one reservation
retirement, two total links, exact successor bytes and original deadlines,
with all tracked C operations/raw credits drained. Log:
`/tanksmall/scratch/tmp/p51-d07-active-current.n5c5bZ/tmp/active-cancel-profiles-r1.log`,
SHA256 `77c54b7e18628c95f4dd80541e48c415665b81b4cd797cdf6d51303e76b79afe`.
This still does not prove actual W30 occupancy or compiler-process quiescence.
The repair implements exact suffix dispositions and contiguous survivor replay.
The independent full endpoint suite passes (exit 0). Focused sender tests for
lost RESET_CONFIRM, lost confirmation echo, and changed reset results pass
all three profiles. Their initial fixture wrongly required F to have observed a staged
successor binding; the corrected check derives the expected exact binding
from its ARM and additionally compares any binding F actually consumed.
Evidence under `/tanksmall/scratch/tmp/p50-recovery-fixture.Mlzk8L/`:
`endpoint-full-r1.log`, SHA256
`383619d14070302b6e0ee1966764ec4f695328f529a4547329b8cd6944c5de59`;
`sender-focused-r2.log`, SHA256
`127bcb3d2724b2fea9be8d9067b9e9094cdbb0294fd59bece2fef3132b3fd222`.
The first full sender run failed its separate lost-commit/RESET_ACK scenario:
the fixture advertised new reset identity with Q=0 despite K=1; real RESET
sets Q=K. The corrected focused scenario passes, including repeated
materialization interruption (`lost-commit-r2.log`, SHA256
`6f4b04216ca18291c91c09649153556b76164af298ef3621edd549e490757163`).
The next full run exposed the same missing-F-observed-binding fixture error
in another shared-recovery branch. After correcting that branch, all six
focused shared-failure cases (2/30 callers across all three profiles) pass:
`shared-failure-r2.log`, SHA256
`4345d5accbca4cbf930796f34922c6e78592f1a4d184e4f388fdda97d8d485a9`.
The complete sender default suite then passes with process exit 0:
`sender-full-r3.log`, SHA256
`b0c7849dabd97866f378a14064cef7b7736ca6938e596b91ce1765b17e458545`;
sender test source SHA256
`ad041631ea928b273e6b3269a85f9f8bd42c5d60303377f1bbb6b0c1fc87f413`.
Final consolidated qualification includes the corrected service recovery
fixtures and bounded-count ordinal loops. Wire, full service and full sender
suites pass with process exit 0. Both the standalone service and the embedded
test service were rebuilt. Final evidence root:
`/tanksmall/scratch/tmp/p51-d07-active-default-run.r2/`.

| Gate | Log beneath that root | SHA256 |
|---|---|---|
| Wire, including maximum-ordinal interval encoding | `tmp/wire-r2.log` | `a72639a77d1abf06080e2f547f03d62832e4cb86ab05cd58aad78ae69068f28f` |
| Full service; Automake PASS, including all-profile active cancellation | `build/unittests/p50cacheservice.log` | `57ed292f59d92808784f878797fc9f649dee4b5f0ad8f8bd7410afbc54ab66cf` |
| Full sender | `tmp/sender-full-r4.log` | `03e3f5b7b9fc36fb2948a44f6c63448e972fb469e1e63f8de460120546ca740d` |

Final service source SHA256 is
`7da582be35371a142449b1823efa518db42a93fa33195526d8b9271b2c3099d9`;
sender source is
`aae9f740b20a12b8ee87dd5ec4eb0c34b600a16582592d2e8d3ec4c34656e19b`.
Endpoint source is unchanged from its passing full suite. The ordinal loops
iterate at most P-K entries, with addition only inside a nonempty bounded
interval; UINT64_MAX remains the sender's exhausted sentinel. Wire tests at
that boundary are codec checks, not an execution of astronomical job counts.
Earlier fixture failures and the read-only-source Automake refresh setup
failure remain preserved and are not counted as passes.

Focused repeated active-cancellation recovery on product `832ad154` passes
ASan/UBSan/LSan (Automake PASS, exit 0). A private test wrapper warms up all
three profiles, then repeats the all-profile batch twice in one process.
Each measured batch returns to the warmed file-descriptor baseline of 5;
each scenario drains tracked operation and raw-byte credits. Evidence:
`/tanksmall/scratch/tmp/p51-d07-asan.r1/build/unittests/p50cacheservice-sanitize.log`,
SHA256 `16c433b6ac4b4608f6aa4bfb2789fdaa7d83e46cc7a83c631434f3081dbaa176`.
The service, sender, route owner and endpoint/protocol closure are instrumented;
the prebuilt `services/libicecc` dependency is not. This is neither whole-program
instrumentation nor a measured RSS/heap plateau or complete D17 qualification.
The first build stopped on a range-loop-copy warning promoted to an error;
the successful build retains the warning with only that warning exempted from
`-Werror`. Its failed build log is preserved, not counted as a runtime result.
The same sanitizer lane subsequently passes the endpoint held-worker
reset/successor and independent peer-close cases three times each for P29V1
and ZSTD_ROUTE, plus InputRecord lifecycle checks (Automake PASS, process
exit 0). Retained-worker counters remain charged until completion and then
drain. Endpoint log under the same root:
`build/unittests/p50endpoint-sanitize.log`, SHA256
`f40e07600d2e6aab4508bdbb9d3b879f08bc0e9286f88c57df90cae46d2bb1d9`.
The same uninstrumented services-library limitation applies.

The real C/F four-job interrupted-replay regression now passes all three
profiles and is registered in the default service suite. It observes the
complete original bundles in order, cancels the held second job, and cuts
the recovered link immediately after replaying job 3, before job 4 is staged.
The third connection reports recovery A=1/P=2 (observed K=2); both surviving
inputs attach with exact bytes within their original deadlines, the canceled
input remains absent, three links are accepted, and tracked credits drain.
W30 is configured; this four-job test does not prove W30 occupancy.

Evidence root: `/tanksmall/scratch/tmp/p51-d07-replay-2pwjOp/`.

| Gate | Log beneath that root | SHA256 |
|---|---|---|
| Restored-source all-profile focused run, exit 0 | `tmp/replay-interrupt-release-all.log` | `71eecb234fc6ed87f0b8ca676f0d77d7df940789fc6d2bc2e5bbbd13d5006fb9` |
| Full service, Automake PASS | `build/unittests/p50cacheservice.log` | `1e84070559d2f6a28fe897636ccebe2491f9ad774c6f430a8d8afccbc98ee5a0` |
| Deliberate witnesses-only backlog truncation, exit 1 | `tmp/replay-interrupt-negcontrol-final.log` | `c44582edfbcb227e00a066649351183948fe4b7789554f8dd8e4b3c41fd19709` |

The negative control passes its setup, exact first-survivor attachment,
cancellation, reset and credit assertions, then fails specifically because
job 4 has no result by its original deadline. The normal sender was restored
and rebuilt before the passing runs. Earlier negative probes with delayed
attachments, and full-suite scratch-permission setup failures, are retained
but excluded. Final service test source SHA256 is
`89fcf9ab16050666c50484a22f4ec640a75f31d3fa062d0230839c54c80dfe3f`;
sender SHA256 is
`e9a2a386f5abb7b6eb496fdf0e6b4a59009e4073ba84cffdd07726930244f300`.
The sender change extends the existing optional disconnect test callback to
replayed bundles; normal callers leave it unset. No wire layout changes.
This does not establish recovery when the coordinating caller is already
positively committed before a later replay failure; that schedule remains
under investigation. Sanitizer results above predate this test extension.

All D07 cancellation positions/stages,
compiler-process quiescence, and the broader external/performance gates remain
open. This qualification is not completion of the full W30 plan. The candidate
R2 recovery layout changes require upgrading both ends and draining old links;
P43 and CacheWire R1 records are unchanged.

In the earlier failing product, F reset removes cancelled reservations,
but C's sender replays every unresolved suffix witness using its
old relationship ordinal. The required behavior is exact settlement followed
by contiguous rebuilding of still-live jobs, preserving their TU identities.
Global TU sequence values are not proof of relationship-ordinal contiguity.

The concurrent local P43/R1/R2 P29V1 gate has a passing real Docker run:
`/tanksmall/scratch/tmp/p51-d18-measured.lhh4dw/run/summary.json`, SHA256
`f8f424b4ef3b25271b9d9c934cdfa16355bf794d3c90ad50c596286c10e93dd8`.
One enforcing-compat scheduler serves separate R1 and R2 workers. Measured
jobs P43=5, R1=4, R2=6 overlap as distinct PID/starttime identities; each
compiled program produces its expected output. R1/R2 exact input attachments
and R2 source lease are matched to those jobs, excluding warmup records.
Final parsing adds exact numeric boundaries and same-worker child-PID
completion checks, validated against retained logs and fake-Docker negatives.
The live run precedes that parsing-only tightening.
The final harness suite passes 16 tests; retained output is
`logs/final-fake-pytest.log` beneath the same run root, SHA256
`f7bc33a514baf72434f389df4733d2317a625dae8ec47a9f0fcd67f77fa6e818`.
`logs/retained-log-parser-validation.json` records the final parser's checks
against the real logs; `logs/image-provenance.txt` retains full image labels.

Tested image IDs: current
`sha256:e7a6a390c406a67257ae44fc06ba4c79ccd3dc9409f0784bd5090ec54c955595`,
P43 `sha256:9140ad2c1a1afb2086bdfcc483d0b0d5d98bf1954168e0889d2e3ee05bc45050`.
The current image predates worker repair `ef29049c`; this run does not qualify
that newer product. Scope is warm-environment, same-host, P29V1 compatibility,
not W30 occupancy, all-profile mixed traffic, or external-farm performance.

The concurrent harness now accepts `--concurrent-profile` with P29V1
(default), ZSTD_TU or ZSTD_ROUTE. The selected profile controls both worker
and current-client configuration and the exact attachment/commit evidence
checks; P43 remains unchanged. Run profiles sequentially in distinct output
directories under the same resource limit. The 23-test fake-Docker suite
passes (agent-observed terminal exit 0, 100.73 s), including matching-profile
positives, mismatched-profile rejection and invalid CLI combinations.
Command in private snapshot `/tanksmall/scratch/tmp/p51-restart-w30.RWA5X5`:
`ICEFARM_TMPDIR=/tanksmall/scratch/tmp/p51-restart-w30.RWA5X5/uvtmp sh dev/python.sh --exec pytest -q -p no:cacheprovider farmharness/integration/tests/test_dev_mixed.py`.
The run's stdout was not saved to a file; no log hash is claimed.
Tested harness SHA256:
`e0763fe3b02705f22e45bcdd925f3fe9b637802b63902109f4a8ab44ecbb6e57`;
test source SHA256:
`ab5c1dd7f952f700fae80cbc4416084c8b255d1a32f57ca162392468f86367e7`.
This qualifies harness behavior, not live ZSTD mixed traffic. Current-image
real Docker runs for all three profiles remain required.

### Clean-checkout build and mixed-version compatibility

Exact candidate `03d108a3cc70d7e17f91ad06457ecde05d7fb63a` now builds
through the supported Docker bootstrap and passes local concurrent P43/R1/R2
QA separately for P29V1, ZSTD_TU and ZSTD_ROUTE. Luna ran the profiles
sequentially with `--concurrent-mixed --jobs 3 --memory-gb 8` and distinct
output directories/bridge networks. Each case requires three distinct remote
compiler PID/start-time identities surviving an overlap bracket, exact
compiled-program outputs, and measured-job R1/R2 input attachments. This
qualifies local coexistence, not W30 occupancy or external mixed-farm load.

Evidence root: `/tanksmall/scratch/tmp/p51-d18-final-03d108a3/`.
Bootstrap `icecream-qa-vgud5l6a/result.json` reports PASS and build exit 0
(182.317 s), SHA256
`9797370cf12303359a669089a18aa88472e517f04086733f42412ab07fa1734d`.
Source snapshot `da71b6c3eee83b696bf2fb3494b674c1d27d5633535777c6faf61d114fb5c4ec`;
runtime image `sha256:f11625d02b85b6cbf15c7abb2d263b0269a43f88aa7b6a3a65890cb2996af515`.
Pinned P43 source `cd74801e0fa4e83e3ae254ca1d7fe98642f36b89`, image
`sha256:9140ad2c1a1afb2086bdfcc483d0b0d5d98bf1954168e0889d2e3ee05bc45050`.

| Concurrent profile | Summary under evidence root | SHA256 |
| --- | --- | --- |
| P29V1 | `d18-p29v1/summary.json` | `81859d97aed4adc8c8b1b2ff9268c49b140d76557f2767fb5cbf4c4968c53100` |
| ZSTD_TU | `d18-zstd-tu/summary.json` | `f8df424cdf2d3e13c04e641ba2caae03e413c6f6b05f690f51c89e6fc2551cb2` |
| ZSTD_ROUTE | `d18-zstd-route/summary.json` | `8480cb3aa31d97134adabf09627988c860e93c495dad196b9e04b0e19793615c` |

These retained summaries contain legacy `r1_remote_p29v1` and
`r2_remote_p29v1` field names even for ZSTD. The actual selected-profile
checks and attachment/commit assertions are profile-sensitive; the names
are a reporting defect, not evidence that the ZSTD runs used P29. The writer
now uses profile-neutral `r1_remote`/`r2_remote`; `selected_profile` continues
to identify the tested codec. All 23 fake-Docker tests pass in 110.10 s,
including neutral-field assertions for all three profiles. Log under the same
root: `logs-d18-metadata-fake-tests-r2.log`, SHA256
`edad21199a563116451e3c70294e3dcadefdce24937b871f87e45dcb4138ce85`.
The first suite attempt passed 22 tests but hit the existing 20-second CLI
timeout in the nine-case old-scheduler compatibility test; that test passed
alone in 19.54 s, then the complete suite passed without timeout changes.
The reporting-only correction does not alter the retained real-run evidence.
Source-commit log events lack job IDs, so their correlation remains limited
to the measured per-role log.

Earlier sequential compatibility evidence follows; it is not evidence for
the newer candidate's other gates.

Published `fdf03e25520d9db25746b29da7847e935b747b7f` builds and installs
successfully from a clean detached worktree using the normal bootstrap
workflow (two jobs, 8 GiB). Source snapshot SHA256:
`9b896e4827e30ede3e95968b05bad32c1f962bf362635745022e69f75c5b986d`.
Result: `/tanksmall/scratch/tmp/p51-combined-qa/icecream-qa-ij9l89uh/result.json`,
SHA256 `e1bf4fdc409c7311fab336b9ad15517ab2f68160b98a87c90b5fbd9f8d60f116`.

The resulting runtime image
`sha256:e7a6a390c406a67257ae44fc06ba4c79ccd3dc9409f0784bd5090ec54c955595`
passes all eight local mixed-version cases: R1 P29V1/ZSTD_TU/ZSTD_ROUTE,
P43 worker, P43 client, and R2 P29V1/ZSTD_TU/ZSTD_ROUTE. Each case verifies
actual remote compilation. The old image uses pinned source
`cd74801e0fa4e83e3ae254ca1d7fe98642f36b89`.
Summary: `/tanksmall/scratch/tmp/p51-combined-qa/mixed-fdf03e25-r2/summary.json`,
SHA256 `45f8ad6d289c21cc834a85bb5dcce1ab7e2afb8e092ca643ed124d4e7474469c`.

These are sequential compatibility cases, not simultaneous mixed-load,
W30 occupancy, restart, sanitizer, or cross-host qualification. Earlier
source-copy bootstrap failures omitted tracked files from a synthetic Git
index; they are retained failures and are not evidence against this clean
checkout or a substitute for its successful result.

### Actual scheduler-process W30 restart harness

The clean published `fdf03e25` build now also passes the combined local
integration checks, each with exit 0:

| Gate | Passing scope | Log SHA256 |
| --- | --- | --- |
| Multi-link W30 | C1F2/3/4 and C2/3/4F1, all three profiles: 18 cells | `62ccb660064ac05dc704c0aee63835bb22b3d9104b219021d6974c826acdfc6f` |
| C/F and S restart W30 | C1F2 F-cache and C2F1 C-cache replacement, all profiles: six cells; actual S replacement, all profiles: three cells | `164644e6b7393e23572ff2ca966e5c16d70f8571048c8bc71dbcc397f2d6e4c6` |

Logs are `multilink-fdf03e25.log` and `restarts-fdf03e25.log` under
`/tanksmall/scratch/tmp/p51-combined-qa/`. Test executable SHA256:
`268b3a208e4fa5e0a918d12d20ffc076ed737dbeec646c00a39ccef25d209824`.
All runs use private bridge containers with two CPUs and 8 GiB.
Restart checks verify old-call settlement, fresh W30 commits and exact input
attachment; C/F replacement also checks an unaffected sibling. This qualifies
the current ledger/recovery product with the harness. It does not cover the
external-farm matrix, every larger-topology restart combination, arbitrary
interleavings, or the remaining fragmentation/multiple-lost-receipt cases.

The following older-snapshot evidence separately establishes the harness
and its timeout-cleanup behavior:

The opt-in `p51schedulerrestart-w30-check` harness passes C1F1/W30 for
P29V1, ZSTD_TU and ZSTD_ROUTE. It holds 30 old receipts, replaces the actual
scheduler process while C/F daemon and cache PIDs remain stable, verifies
the old callers settle fail-closed under remote-only policy, then holds and
releases 30 fresh receipts and compares every fresh remote object with its
local result. Old-call settlement is not a transparent-retry guarantee.
The three-second test reconnect cadence does not qualify production latency.
Run requirements are in [developer QA](dev/README.md).

Evidence root: `/tanksmall/scratch/tmp/p51-real-s-cleanup/`.
`runtime-run-r22.log` SHA256
`0fe45db54ca538767bfbe1f92a42c78c9ed81d265fd159da1baf55ddfdcda2fc`
contains all three passing profile markers; its exit file is 0.
`trap-timeout-r2-container.log` SHA256
`0133d9f85b454f89806d63e5d61ddf17d3f92902e5a5d2bc6675402eedbae405`
proves actual timeout status 124 after 30 receipts were held, fixture failure,
a fresh abort marker, and removal of the temporary redirection rule.
Timeout uses foreground signal delivery and a 20-second cleanup grace.

This qualifies the harness on an older frozen product snapshot, **not** the
new ledger fix or current combined candidate. Snapshot sender source SHA256:
`b17e1de52cf94241272c22f224668d7192d8bd098bca93ce792d895b38f69837`;
endpoint source:
`bc83d20be683c0b1574a900a16b7fffcb13b64af2a12a080ffd9afdca8c3071d`;
service source:
`54640beb7759d7e02124a661808539431b6163b873ce95fc7417724b2616e8b9`.
Final helper binary SHA256:
`aadee66274bbbf36bed4fab9ba521c82e295d857e0222479ff157a83200e3ec9`.
The published runner differs from that older successful test copy only by
one wording-only correction to a failure message.

### Blocked writer with concurrent receipt processing

The ordinary window/connector fixtures now use default socket buffers;
dedicated blocked-writer coverage below retains its 4096-byte settings.
No input sizes, occupancy assertions or serial-negative controls changed.
Measured on frozen accounting-product snapshots:

| Fixture | Both buffers restricted | Sender default only | Both default |
| --- | ---: | ---: | ---: |
| 18 window cells + three serial controls | 179.80 s | 178.79 s | 0.40 s |
| Three W30 connector-failure cells | 63.83 s | 62.49 s | 0.13 s |
| Full sender suite, including blocked writer | 288.01 s | not measured | 44.91 s |

All reported runs exit 0. This is test-runtime improvement, not product/farm
throughput evidence. Evidence root:
`/tanksmall/scratch/tmp/p51-wbuf-both-default.uxKAJJ/`.
Full log `logs/sender-full-both-default.log` SHA256:
`cd2cdc84002acbace4ae6801cea46a40f04ece272653e2e5f51247dc17ac830b`.
Test source SHA256:
`77e1bb18660a4bd7aa8ddb1b04eeb17d82b5d15b1fe8da1e131a936a41821a1c`.
Binary SHA256:
`735de2163a4f0c923d9b4837cbe39d11594e39f9ecad19d22395428f44707103`.

The Linux default sender suite now includes a kernel-backpressure fixture
for P29V1/ZSTD_TU/ZSTD_ROUTE. With W2, F publishes the first input then pauses
its reader. A 512 KiB second input fills C's small send buffer. The test
observes queued bytes and no `POLLOUT`, an incomplete second bundle and a
live C event-loop heartbeat, then releases C's receipt reader and requires
the first receipt to validate while the second write is still blocked.
Peer shutdown and sender retirement must settle both callers within bounded
waits, preserving the first committed result and exactly one F publication.

This is partial D16 evidence: kernel backpressure, not an observed `send()`
EAGAIN return, complete service-process shutdown, or a worker/descriptor/credit
leak matrix. The focused selector passes all three profiles. The full default
sender suite also exits 0 on the accounting product snapshot, taking 288.01 s
wall time (1.46 s user, 1.76 s system). This older timing includes the
ordinary window fixtures' tiny-buffer delay, removed by the comparison above.

Evidence root: `/tanksmall/scratch/tmp/p51-d16-qa.WkgfTa/`.
Full log `logs/sender-full-r1.log` SHA256:
`a46d4c84d70ebe849dc3e9ef16379cb5ade11feb4075f2764c457686c5cf3d20`.
Frozen test source SHA256:
`9be181ec4d05f9ec231f6ecdfa93362ab598ff8687426c3ce6d800b1a734b9fc`.
Full-suite executable SHA256:
`54850049c4c1042b9520cb03bff60f18ddee0bdda99cb2f497384a330c450da3`.
An earlier fixture incorrectly required the first caller to finish before
retiring the blocked writer; that failed run remains retained. The passing
test checks receipt-reader progress under pressure and preserved positive
settlement after retirement, without claiming earlier caller completion.

### Bounded persistent-link completion accounting

The product sender now retains fixed-size R1 byte counters instead of an
ever-growing vector of asynchronous I/O completions. R2 records are excluded
from those counters because R1 and R2 can share a route owner. Detailed
endpoint/test logging keeps its prior behavior; overflow makes byte evidence
unavailable rather than wrapping. This changes neither the wire format nor
completion identity checks. No farm speedup is established by this change.

The frozen accounting-only candidate passes the full sender, endpoint and
route-owner suites (all exit 0), including the 18 positive window cells,
three serial negative controls, exact R1 network accounting, and zero retained
completion records after persistent R2 traffic. The endpoint suite includes
the six interrupted BODY cases below. This is not the full D17 sanitizer,
descriptor and process-memory growth matrix.

Evidence root: `/tanksmall/scratch/tmp/p51-window-matrix-fdf.1A9DCS/`.
Combined log `accounting-full-regression-r4.log` SHA256:
`faecdee4ced7c63bde8fc881192d9e011314ed499c4acde0fc6600af82c55cd4`.
Frozen sender test source SHA256:
`7a10073069371bb49344c2baf639e6eedc369804b746d9134666ece1ca46e9a4`.
Earlier setup failures (missing test archives and an accidentally copied
mid-edit test source) remain retained failures, not passing runs.
The pre-existing sender source-boundary failure is now repaired by moving
unchanged `CACHE_PROFILE_*` constants to an installed lightweight header,
re-exported by `comm.h` and directly included by the sender. A consumer
compiles against staged installed headers, with static assertions for bits
1/2/4 and mask 7. Both focused retry/replay baselines pass; deleting the retry
or completed-result lookup fails its intended assertion, not compilation or
a timeout. Mutation runs are bounded and unexpected failures retain evidence.
The sender mutant link now uses Automake's configured cap-ng libraries.
The endpoint source gate uses SDK-provided grep instead of silently skipping
its forbidden-name check when ripgrep is absent.

Evidence root: `/tanksmall/scratch/tmp/p50-profile-mask-r1.5HGnlt/tmp/`.
Sender gate `p50zstdsender-source-r2.log` SHA256:
`6eca83cb2d6b23d035f76c906219cb9714c538cf3c93296b72a7b25c36a852c3`.
Endpoint source gate `p50endpoint-source-r2.log` SHA256:
`ee86f5e0b298984d6868a8f9dc5fbbb7b02ba041d9a355130d7ae0c63651764c`.
Both exit 0. The full endpoint suite with accounting and all 25 frame cuts
also exits 0; `p50endpoint-full.log` SHA256:
`9e4542a2fe5d096bc3c3c6398ab8591855a2c0200a5adb138f8e18a89453cc1d`.
Earlier missing-build-prerequisite and cap-ng link failures remain retained.
The separate compiled endpoint-mutant suite has not been newly qualified.

### Interrupted R2 frame recovery

The default endpoint suite now extends the BODY relay to 25 interruption
cases across P29V1/ZSTD_TU/ZSTD_ROUTE: one payload-prefix cut each in JOB_BIND,
TU_BEGIN and TU_END; BODY header offsets 1/2/3 and early/middle payload cuts;
and one P29-only R2_FILL payload cut. HELLO remains intact. Each case checks
the exact fully forwarded frame sequence (no complete END), no initial
publication, retained-witness RESET/rebuild/replay, exact attached input and
one final commit/ACK. Truncated JOB_BIND consumes no initial reservation;
the replay consumes one, versus two total when the first binding was complete.

Focused and full endpoint runs both exit 0 on the same frozen binary. This
snapshot predates the completion-accounting change; the combined qualification
is recorded above. These 25 cases are not exhaustive D03 byte offsets,
HELLO/recovery/ACK interruption, or actual EAGAIN coverage.

Evidence root: `/tanksmall/scratch/tmp/p51-d03-frame-cuts-r2/`.
Test source SHA256:
`3435b3d0d181ef7bf7677c0f801690eecbb48713c5d56a8fb22acae5245ddcbc`.
Binary SHA256:
`fb484a40bab20889fe7ccb6e1020c569a492693316a13b2e990619511e83af5f`.
Focused build/run log `logs/container-r2.log` SHA256:
`6474f220652aafaa9390a2185d4fe40603f0a753f65b9b6ea680966a6d05fb26`.
Full log `logs/full-endpoint-r2.log` SHA256:
`9c920e93126c383a0cedbfb9a89145f84ab6c5113df950eda0181ba5d02731db`.
The earlier six-BODY-only qualification below records its own product and
binary identities; inherited scratch logs are not evidence for the new run.

#### Earlier BODY-only qualification

A test-only TCP relay cuts the first R2 BODY after an early or middle payload
prefix. All six combinations with P29V1/ZSTD_TU/ZSTD_ROUTE pass: no partial
input is published, the original witness survives, RECOVER/RESET/CONFIRM
reconciles the empty committed prefix, and replay produces exactly one
materialization, commit and acknowledgment with exact attached bytes.
The focused selector and full endpoint suite both exit 0 on the same binary.
No production fault-injection hook was added.

The relay forwards HELLO/JOB_BIND/BEGIN before the partial BODY; its trace
checks that exact sequence. Cut counts are payload-relative, with the
four-byte header offset reported separately. This is not exhaustive D03:
other record boundaries, HELLO/ACK fragmentation and actual EAGAIN remain
separate. The original results below use the preceding product snapshot;
the accounting qualification above also reruns these cases in the full suite.

Evidence root: `/tanksmall/scratch/tmp/p51-d03-body-recovery-r3/`.
Test source SHA256:
`57499065ad567cb7b41e89df4a28570f08fbf7cfe477685bb4b8e433b7357f17`.
Binary SHA256:
`fd09467d27ab7f51533e736c5f8ed3b02b22ef3a6e8fbd990313d1f77d5536b9`.
Focused build/run log SHA256:
`91c8402b188b7db2d0e6f3eea85d732f901b435ece7c76ee8959818c737cf5ac`.
Full log `logs/full-endpoint-r3.log` SHA256:
`b8fac637e6649a14497839cf22ff5f7da7501926824c272fe9e90ea9dbfe50d0`.
Earlier retained failures were fixture issues: the expected proxy trace omitted
HELLO, then a diagnostic tried to stream an optional value directly. The
passing build still emits existing compiler/aggregate-initializer warnings.

### Successful R2 bytewise fragmentation

The real client/server R2 fixture passes P29V1, ZSTD_TU and ZSTD_ROUTE
with one-byte writes for C's JOB_BIND/BEGIN/BODY/FILL/END bundle and F's
LINK_STATE/TX_COMMIT. It verifies exact attached bytes, one publication and
acknowledgment, complete record ordering, and transaction-fragment byte totals.
Both the focused selector and full endpoint suite exit 0 on the same binary.
Run `ICECC_P50_R2_FRAGMENT_SUCCESS_FOCUS=1 "$BUILD/unittests/p50endpoint"`.

This is successful fragmentation, not D03 completion: HELLO/ACK fragmentation,
interrupted frames, recovery at each boundary, and actual EAGAIN remain separate.
The fixture cancels its watchdog after both coroutines finish rather than
adding a fixed delay to each profile.

Evidence root: `/tanksmall/scratch/tmp/p51-d03-fragment-success.Us1E3p/`.
Test source SHA256:
`a86c9a435cef8ecb4a4164c15f414fda172f9e7de258c0c9732c2b2e38b454e2`.
Binary SHA256:
`cd54fe2ed02bb6ceb6e791367d95debccd4bde828aaebc14fec94cfb1ec1ce11`.
Focused build/run log `logs/container-r2.log` SHA256:
`bfe7095113495479c97c99f80d831ade5db7353154e693c9176c3992fb306a78`.
Full suite log `logs/full-container.log` SHA256:
`3f4cbba461c06ba9793022c53c933421fb34c49e660b67a0393eea7d032445af`.
The earlier compile failure was an unqualified test-only `digest128` name;
its original log is retained.

### Lost committed receipts with F kept alive

The D04 integration fixture passes all nine cells: drop 1, 2 or 30 fully
emitted COMMIT replies across P29V1, ZSTD_TU and ZSTD_ROUTE. It closes only
the proxied connection, keeps the same F sidecar process alive, and verifies
each recovered result against the held receipt's exposed TU/raw identity.
Every row observes two physical links, one publication and one accepted
input attachment per job. The nine-row command exits 0. This uses the
`fdf03e25` product with the updated test helper; no product change is needed.

This is retained-input attachment evidence, not actual compiler execution.
It does not cover an uncommitted suffix; the fixture labels that exclusion.
The local result does not expose every opaque wire digest, so complete
receipt validation remains the endpoint's responsibility. The held receipt
set additionally checks unique contiguous ordinals and relationship fields.

Test source SHA256:
`2709c366a8241ef664c6343ff290336dd811e8db1f8043378b55713b5e195343`.
Binary SHA256:
`c62749bf042be34c34198f7df773e614124ae2aaf0c19e5a7c37ca36502dd7b7`.
All nine logs are under `/tanksmall/scratch/tmp/p51-d04-qa-results/`, named
`d04-{1,2,30}-{P29V1,ZSTD_TU,ZSTD_ROUTE}.log`. W30 log hashes:

| Profile | SHA256 |
| --- | --- |
| P29V1 | `5217698de0bcce35415f3ba8f704e6f19f686d0ac990a3c7d2fc91b4f9b56982` |
| ZSTD_TU | `4269d62c49e5b4abaa0589da1a6a0c6825b3fd15b6b9a0a9e6c14abc0a6bc44a` |
| ZSTD_ROUTE | `d41afc2bcf91b7614f56ddf0d481d16075a6f04488bddff2301f723aa2d42376` |

The first focused run recovered successfully but failed its counting checks:
the fixture had not precreated the stderr capture files. That failed log is
retained as `d04-count1-tu.log`; the corrected fixture creates owned capture
files before daemon launch, then checks markers after attachment settlement.

### Sender window matrix and serial control

The real sender passes W1/2/4/8/16/30 across P29V1, ZSTD_TU and
ZSTD_ROUTE (18 cells). Each queues W+1 jobs, holds receipt processing,
observes exactly W complete bundles before the first receipt, then drains
and verifies every source digest and unique committed TU. Reservation-to-TU
mapping permits legitimate admission reordering rather than assuming
submission order equals wire order. One connection serves each normal cell.

The serial negative control queues 31 jobs with W1 across all three profiles:
it observes only one complete bundle while all 31 callers remain unsettled,
so the same demanded-W30 occupancy witness is false. It then drains all jobs
and checks exact results. This is an occupancy control, not a speed claim.
Both `p50zstdsender --window-matrix` and the full sender suite pass (exit 0)
on the same binary; the matrix is also included in the default suite.

Evidence root: `/tanksmall/scratch/tmp/p51-window-matrix-fdf.1A9DCS/`.
Test source SHA256:
`4d2d5194d7c9c84d32b7e5025c1f3dc7794511c949a3724398de315b8815ecd5`.
Binary SHA256:
`2fa21a149e0e6a767e269effda87f1f45152e472c07820b85c1a4974336a36bc`.

| Log | SHA256 |
| --- | --- |
| `window-matrix-r2.log` | `ba82922475809f95e576f663a1a635714abf14a40ad87338e852d577ca9e8190` |
| `full-sender.log` | `d4dfdb6c2e1bf9cb7e147e53ff72666a5125ee8bde6562dd99610224c2f8c367` |

This covers sender occupancy/refill, not all D03 fragmentation offsets,
multiple lost receipts, concurrent mixed-version load or farm speedup.

The fixture now bounds result waits by each cohort's original deadline.
The focused matrix passes on this updated test source, SHA256
`341aa8af3683144f605928f87472bb316658059147b186bf0c71ea775effa716`;
binary SHA256 `8479e1059b194e6821e70fce949ab8e9a50f7f4802d62245db338b5946e67e3e`.
Evidence: `/tanksmall/scratch/tmp/p51-source-gate.p9YKIe/window-matrix-updated-r4.log`,
SHA256 `e4ba3ef8340aa589e4fb158d9e20b5164c736a67b34b3e9dbb96d1d3062af59c`,
exit 0. Timestamped test execution took about 179 seconds with a 360-second
outer watchdog; per-job deadlines were unchanged. The preceding 180-second
outer-watchdog run timed out and is not a pass. Two earlier command/setup
failures and that timeout remain alongside the successful log. The full-suite
result above belongs to the previous test source, not this focused rerun.

### Focused sender sanitizer qualification

The completion-ledger capacity and expired-witness selectors, incorrect
RESET_CONFIRM echo selector, and first-connector-failure W30 selector pass
with address, undefined-behavior and leak sanitizers (each exit 0). The last
two selectors cover all three profiles. Sender, route-owner, endpoint and
test translation units were instrumented; static protocol, local-transport
and icecc dependencies were not. This is not whole-product instrumentation.

The first expiry run found a test teardown lifetime error: its I/O context
was destroyed before a sender retaining a pending timer. Declaring the
context before the sender fixes that fixture. The failing run is retained;
this finding is not evidence of a reproduced production teardown defect.
The passing private test source predates the window-matrix additions:
SHA256 `46ba531698b3fea2740d53d71a8c74aa5cb172c77b0f83a35ecce71d6634d236`.
Instrumented binary SHA256:
`4f4c8d79aad7b3106ed38406a9f31661c8f76c33985e6fcec203e7b1b040b2cc`.

Logs under `/tanksmall/scratch/tmp/p51-ledger-cap-green.PNHvSI/sanitize-logs/`:

| `sender-asan-r2-` log suffix | SHA256 |
| --- | --- |
| `completed-ledger-cap-w2.log` | `549d80d08350ab50557e836819ce4fe3b3547a36c7424b4aaf4e4911f9a86f60` |
| `completed-ledger-expired-witness.log` | `3f854d6e4064770f9294b3e3a73e2e6eceeefe83d581a0156405fdb066de15e8` |
| `mismatched-reset-confirm-echo.log` | `b0165c784022586e2e38cf266d454a32be87722574b9904852ed61092cd4f97c` |
| `connector-first-failure-w30.log` | `9ec0f3f276998b1ffd04e523885a2449374593e62ec063e89f3057231430b18b` |

### Bounded sender completion ledger

R2 reserves completion-ledger capacity before admitting a bundle, counting
both completed entries and outstanding reservations. An unresolved retained
request keeps its slot until reconciliation or route retirement. Expired
unresolved requests cause explicit replacement-required results rather than
extending the original deadline or leaving fresh callers on an unusable route.
Validated positive results remain available for exact replay.

The W2/one-entry regression passes: only one bundle commits and is acknowledged;
its caller receives Committed, while the other receives replacement-required
Unavailable with zero transfer attempts. Exact replay opens no new connection.
The expiry regression covers both an already-waiting and a later caller,
observes completion after the original deadline, and verifies zero connection
attempts after expiry. The incorrect RESET_CONFIRM echo regression passes for
all three profiles: callers remain pending until the exact echo is received.

Focused checks and the full `p50zstdsender`, `p50routeowner`, and `p50endpoint`
executables pass on the same frozen source; each runtime log records exit 0.
Evidence root: `/tanksmall/scratch/tmp/p51-ledger-cap-green.PNHvSI/logs/`.

| Evidence file | SHA256 |
| --- | --- |
| `completed-ledger-expiry-r9.log` | `b960592e68bea2c968cc28f943666264aa024de2a725f6edecd3f53f97efbffd` |
| `completed-ledger-cap-r10.log` | `3f527b68dcaa27c84e2faa0248d1aa876c1967553a429845c20e148a3f9cf7c3` |
| `completed-ledger-full-r11-build.log` | `6fd152f607e97b340f01b0c0efa6349068ebcf4d74e9d19a1171fb6aa7a06369` |
| `completed-ledger-full-r11-p50zstdsender.log` | `520ccc76353dd17b75a7e31e1101aa7d03f2e1935fdbdffaba94126c49506ed0` |
| `completed-ledger-full-r11-p50routeowner.log` | `0fd9f9d5702ab20a17ce848b69311edb602a70a7420fd3a952e09f3a84d4425b` |
| `completed-ledger-full-r11-p50endpoint.log` | `6a57fa491804b8908db482ea9e46ce6c09c7d8615b2097b74d77dad891ebf2fe` |

Frozen sender source SHA256 is
`4abc800fb0bc10f8de1824edc7bbcab2417604700b6189bd2505d3c81b7d046e`;
endpoint source is
`c29acf56492ea57ee946203632e96edc4ddc5191c1827a761bb9c5cc6a25c906`;
sender test source is
`cbbb1c59bd7339bbeb4596e2037ff3b167e5a934de93e548e9229112d85ba271`.
This qualifies the sender/endpoint change, not the final daemon-restart,
mixed-version farm, or performance matrix. It does not establish the cause
of the separately observed farm delivery backlog.

### Opt-in source-free diagnostics

The R2 client-phase extension emits `P50_REMOTE_PHASE` for one remote
invocation, including separate preprocessing, C lease, F ARM, control-transfer,
and compiler-result intervals. Its private SDK snapshot uses the published
`33cff860` dependency baseline plus the diagnostic client/header and formatter
test changes, without the in-progress worker-lifetime repair. The formatter
runtime test and normal product build pass. Real local C1F1 ZSTD_TU remote
compiles produce byte-identical objects with diagnostics enabled and disabled;
the disabled run emits none of the diagnostic record prefixes.

Evidence under `/tanksmall/scratch/tmp/p50-remote-diag-r1/`:

| Evidence file | SHA256 |
| --- | --- |
| `r2-success-smoke-r4.log` | `790f34ff0bdd0ec46960596a78a3ef469bb866d70364db9556da7038fdd691e5` |
| `r2-disabled-smoke-r3.log` | `fc36b5e910e38e1907a209f86276f03f3229392c75e2a6bc49c474e5af2e0b9c` |
| `r2-source-failure-smoke-r4.log` | `81616940ed7076b3a76b6a50d7987c6ed31dcb17257b94e8f728349e51da9932` |

The attempted source-failure smoke initially failed during sidecar startup:
its generated Unix socket path exceeded the platform limit. That setup
failure is not a source-phase diagnostic test. The corrected short-path run
reaches F's deliberately shortened source deadline: the first phase record
reports `f_arm_to_armed`, exception, original error 5, and null unreached
control/result intervals. Exit 100 is the expected strict remote-only compile
failure, not a successful compile. The subsequent retry's error 11 is an
opt-in fixture capture collision: the first attempt already created
`ICECC_P50_PREPROCESSED_CAPTURE`, and retry's `O_EXCL` creation refuses the
same destination. Without that capture variable the normal retry does not
take this branch. This run does not qualify successful retry recovery.
Earlier negative runs either omitted opt-in or exposed the
outer exception overwriting the original error with 106. The final client
build and negative rerun qualify the error-preservation correction; the
enabled/disabled successful compiles preceded that catch-only correction.
These tiny synthetic compiles are not farm throughput evidence.

`ICECC_P50_DIAGNOSTICS=1` enables bounded retry-decision records and service
timing/resource summaries; see [the schema and timing scope](doc/p50-cache-diagnostics.md).
The strict SDK build, real completion-flow loss/retry gate with JSON assertions,
full service executable, and metrics opt-in/off gate pass. Evidence directory:
`/tanksmall/scratch/tmp/p51-diagnostics-qa.r1/logs/`.

| Evidence file | SHA256 |
| --- | --- |
| `strict-build-r1.log` | `ea821d8cb2251dba43f773e67adb332c17156a71a7d41e5585baf00bc4e890f0` |
| `completionflow-diagnostics-r3.log` | `50f257bb95046462cfd8659bd69436426ca2b5c998ab7f2bc1785b8e280861d7` |
| `full-service-diagnostics-r1.log` | `9dad4e03a5bcba2bfe247d1baf624670fa3866e41d242e2157b32faec9af89dc` |
| `service-metrics-assert-r2.log` | `dda8f831108c817eb328f1154364b10b280cf1f1b81e268d3de354396114df18` |

The three runtime logs terminate with `INNER_EXIT=0`; the service check invokes
the full executable, not an Automake result wrapper. Completion-flow ran in a
private Docker bridge namespace. The built service source differs from the
published source only by a timing-scope comment. This qualifies the diagnostics
change, not later sender-ledger changes or the full W30 restart matrix.

### Pipeline formal entry point

`make protocol50-pipeline-formal` passes on published `32763111`, with explicit
`ICEFARM_TMPDIR`, pinned `TLA2TOOLS_JAR`, and fresh absolute `TLC_STATE_ROOT`.
The aggregate includes 19 recovery/accounting rows and 18 replacement rows.
The replacement lane comprises six safety
rows, six replacement-use reachability witnesses, two distinct recovery
witnesses (confirm not applied vs. confirm applied with its echo lost), and
four expected mutant counterexamples. The model distinguishes C's confirm
write, F's confirm application, and C's observation of the exact echo; the
reset operation identity remains retained until echo observation.

The lifecycle topology rows vary abstract sibling-progress tokens, not full
concurrent pipelines. These finite checks do not prove liveness, codec/parser
correctness, memory safety, or arbitrary W30 interleavings. Existing runtime
and broader formal requirements remain separate.
Evidence: `/tanksmall/scratch/tmp/pipeline-formal-32763111.WlFSPr/aggregate.log`,
SHA256 `3fd0a274ee594508e80a4868956e8ddd5b6d87eaa441c16dbaaa8418036c8927`,
terminal `PIPELINE-FORMAL-TLC PASS lanes=2` and `AGGREGATE_EXIT=0`.
The log records exact module/config hashes and each intended counterexample.
Pinned jar SHA256 is
`936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88`; focused
module SHA256 is
`411207b2620a3ccfb7e4ddf7c560cc9c78a5ad511f836a83f06d33e428775a95`.

### Opt-in R2 mixed Docker gate

`dev/mixed.py --p51-r2` adds actual remote-compile cases for P29V1,
ZSTD_TU and ZSTD_ROUTE; all three pass with R2 source leases and worker
link adoption. Evidence:
`/tanksmall/scratch/tmp/p51-mixed-r2.vFSVL7/mixed-r2-current-evidence-fix/summary.json`
(SHA256 `f76bf3f1499539c3da8924ded573a41b00c61f968b6f74f61b2803bc693f4655`).
Runtime image ID:
`sha256:9a39ad53e551bf9d5c07a6b3d758afb258cd8f943032583c2ca379e90feddc3f`.
Its source manifest is `3299b4fe9f7e1070124802a4b1dd690cd18c04fe5ba6a97f99b78146a6b331d4`,
from the private coherent wrapper build, not a qualification of current HEAD.
These are sequential one-program cases, not W30 occupancy, reuse, or
simultaneous mixed-version coexistence tests. Subsequent harness refinements
(old-scheduler-only selection and scheduler logfile ownership) passed the
10 focused harness tests; their stdout was not separately retained.

The pinned old protocol-50 scheduler fallback initially failed with
`invalid message payload (GET_CS)`. The corrected daemon now projects R2
requests and advertisements to cache absence on that older scheduler hop,
while retaining the original wrapper request during deferred scheduling.
Its strict SDK build and actual old-scheduler remote compile pass. Evidence:
`/tanksmall/scratch/tmp/p51-old50-compat-20260925T005555Z/old50-rerun/summary.json`
(SHA256 `945989e7bc8fa86428835072174f6571b2bf0833b064add4c7e68ec197821610`).
The scheduler is from commit `94e9b44025887412c70c1c46c35fc588d6dec776`,
binary SHA256 `ecb988d58ba66ee176512b5065ab03352b994da6cd8adf2846ca5db2f2d73da2`.
The corrected daemon source hash is
`c2421f21416cf153d89e1b39246afec74e68a93194a30fa5905f199aba0883aa`;
derived runtime image ID is
`sha256:700edb9e1d3ca7bd0f49361e27a99a5350e2dc2885d704d1a550a40ac0b1c55e`.
This proves ordinary remote fallback without R2 selection for that pinned
peer. The same image also passes current-scheduler R2 remote compilation
for all three profiles, each with R2 selection, source lease and link adoption:
`current51-r2/summary.json` under the same evidence root, SHA256
`2afa36c916b64b8e50471ce050475770e4d803f7fe06e3f69461b0b96dfef404`.
Scheduler-transition regressions remain pending. These separate runs do not
prove simultaneous coexistence or qualify the combined recovery changes.
The harness can isolate this case; see [dev/README.md](dev/README.md).

### Persistent-link implementation in progress

RESET confirmation loss is fixed: F echoes the exact applied RESET_CONFIRM,
and C waits for that echo before releasing its stable reset retry identity.
The previous implementation failed the deterministic lost-confirmation test
by attempting a different reset against F's still-unconfirmed prior result.
Both confirmation-loss and applied-confirmation/echo-loss cases now pass for
P29V1, ZSTD_TU and ZSTD_ROUTE, with exact once-only commits and the same reset
identity across reconnection. Full sender and endpoint executables pass too
(`SENDER_AND_ENDPOINT_EXIT=0`):
`/tanksmall/scratch/tmp/p51-lost-confirm-sdk.EK4iRP/logs/full-sender-endpoint-r1.log`,
SHA256 `4f2aa1af974c33cd2ce82e61ad175aa03641ae6929daac61ac5f8c75950c42a3`.
Endpoint source SHA256:
`4e59b1419dec2260a993ae4f379799961e2e93019291c53f3aecb79489985ccb`;
sender test SHA256:
`4334876affbbe37ac558a154034ec5d0bcfe758480002c4044a4863b4cd99d28`.
This changes the unreleased windowed recovery handshake: deploy matching C/F
binaries. It adds no normal-transfer round trip and does not alter P43.
Formal echo-loss/early-retry-discard modeling remains a follow-up, not a
claim established by these native tests.

Active-read cancellation now checks the control peer without consuming bytes
before each 64 KiB source read. A deterministic read-worker barrier proves
peer closure is observed inside the read loop, before any F connection, and
both operation and raw-byte credits return to zero. This does not interrupt
an already-blocked disk read. The focused selector and full native service
suite pass; log SHA256 `ae29bf3deb0a93e45c3618af47f3c63b9e04a0ee40c310a37de8ed5b5e288006`
at `/tanksmall/scratch/tmp/p51-e208-qualification/build/unittests/p50cacheservice.log`.
The non-test-hook standalone service builds too (binary SHA256
`4384ea3d6dd1ffb657610df03f9b68a30f6db1dd618249eb5532494863c0c89c`).
Updated ASan/UBSan/leak execution also passes after explicitly forcing the
Make target; its log includes the active-read cancellation witness:
`/tanksmall/scratch/tmp/p51-e208-qualification/build/unittests/p50cacheservice-sanitize.log`,
SHA256 `c153b289ddfccf3c0fcfd00c8f2f85dbb46c0ff129dc680bebae95b09b67a65c`.
The instrumentation boundary remains service/test/owner/sender, not the
endpoint/protocol static libraries or standalone cache process. An earlier
up-to-date Make result reused the old log and was not counted for this change.

The `e2080067` service sanitizer suite passes (Automake exit 0):
`/tanksmall/scratch/tmp/p51-e208-qualification/logs/service-sanitize-before-read-cancel.log`,
SHA256 `d2e4ead2d193e078cedf76731e91127fda40d06c0285005caec0babc5a7c2194`.
Address/undefined-behavior/leak instrumentation covers the service test,
service, route owner and sender. Endpoint/protocol static libraries and the
standalone cache process were not instrumented; this is not whole-stack
sanitizer coverage. The corrected run used the `services/libicecc.la` Make
target after an earlier setup attempt requested nonexistent `libicecc.a`.
The qualification directory was subsequently reused for the active-read
cancellation candidate; this archived log describes the earlier e208 source,
not the directory's current source/build contents.

The full sender suite also passes on the `e2080067` product source, including
the request-scoped recovery observation and its deterministic regression:
`/tanksmall/scratch/tmp/p51-e208-qualification/build/unittests/p50zstdsender.log`,
SHA256 `923e7cfe506a4aba524e98ffea2c27a5066e633733ebbd4ecf846b7eca05a94c`,
Automake PASS / exit 0. Binary SHA256:
`ae8ecfe8e01dcd6a3b9748857c2385f77e4f8b566a48f6e73ab0a519cd48164c`.
Source/test hashes match the `35ba9dd9` regression recorded below. This is
native test coverage, not sanitizer or farm performance evidence.

Combined service/route-owner qualification passes in
`/tanksmall/scratch/tmp/p51-admission-combined-r2/build/unittests/`:
`p50cacheservice.log` SHA256
`fee5e6cec4d00286f97a15e9925b3f35362c289c96dec42c054b1baa9d53e603`
and `p50routeowner.log` SHA256
`ecc2a108e04741d81c37bbed83df1f314b2fed08b70a79c6fd8ccac8613a07fe`,
both with Automake PASS results. Client archive and standalone cache service
were explicitly rebuilt; subdirectory test builds alone do not rebuild those
dependencies. Scratch must be writable to the test's unprivileged identity.
The service binary is
`dc96b0437bdbb0590b8b5376d8fc66224af3fd11b678a415a5060192545092a0`.
This covers the nonblocking byte-credit queue, bounded 30-admission bypass,
peer-close/deadline cleanup, same-F idle-history replacement and concurrent
successor callers. The request is copied before moving its containing control
operation, independent of compiler argument evaluation order. Active source
reads still check peer closure only after reading; early read cancellation is
not established by these tests.

The tested service source differs from published source
`54640beb7759d7e02124a661808539431b6163b873ce95fc7417724b2616e8b9`
only by one explanatory comment. Service test source is
`29fbc2f96ddee697e504c4b17f19cc7ea3d6af3c3bcfa702a5f4b289749afd60`;
owner source is `aa4a27be1a0da5485a07713273fb4519b3d8f9c2f1448ad7fd8a5294fa288c80`.
These combined runs use sender source `b42a6fb0` from before the request-scoped
test observation added in `35ba9dd9`; the latter is qualified separately below.
They are not a full native/farm/sanitizer or performance qualification.

The synthetic scheduler-epoch W30 fixture passes for P29V1, ZSTD_TU and
ZSTD_ROUTE on a frozen combined service/owner build. It retains the same
C/F daemon processes, cache identities and receipt-gated TCP connection:
30 old receipts are held, old results match those exact witnesses, an old
assignment's attachment is rejected after the epoch change, and 30 fresh
receipts (ordinals 31–60) are held before release and exact attachment.
An independent scheduler pair continues to transfer during the transition.
This is not an actual shared scheduler-process restart or a test of lost
old receipts: the gate releases those receipts after the session change.

Evidence root: `/tanksmall/scratch/tmp/p51-synth-sched-r0.sHxOQZ/runtime/`.
Logs and SHA256:

- `current-p29-run-r4.log`: `2033a1f1a85567308d8dde258bc9538e2d20b3dab53d53c43f8464c3591e6863`
- `current-tu-run-r1.log`: `b9030d48fee72b3ce56eee024b939553c2c28d9054fdb5a28716f7d48ab4e674`
- `current-route-run-r1.log`: `2b0155cd97df869e2fd00b7a0ff1de6bdeccd6187702e8d69ca1beef266c0d53`

Fixture source SHA256:
`50704d4d61f9877f483a7d25c5e0066f7e089e737f66ddb3575ffc147b9cc860`;
service source `d0bf04b5749e2fe6503745c06455f9b964a2df34bbe6b1f01b7160f626ee484b`;
owner source `aa4a27be1a0da5485a07713273fb4519b3d8f9c2f1448ad7fd8a5294fa288c80`.
This build predates the request-copy-before-control-move correction in
service admission; it does not qualify that later change.

The reproducible opt-in entry point is
`make -C unittests p50daemonpositive-p51-synthetic-scheduler-w30-check`
from an already built tree, inside a disposable root container with usable
NET_ADMIN/iptables, an unprivileged `icecc` identity, and writable
`ICEFARM_TMPDIR` accessible to that identity. Mount scratch at a short path
such as `/work/tmp` for Unix socket limits. The named target passed all
three profile markers on the frozen build above; aggregate log
`synthetic-scheduler-named-target-run.log` in the same evidence root has
SHA256 `f4c3b6d7fa4941576ee37baf0b14902a2bf6f08c8be7cef08469f356a36c2645`.
The outer container exit code was not retained, so the individual exit-0
profile runs remain the direct process-status evidence.

The sender now rebases an older same-relationship ARM onto its verified
post-RESET epoch and physical connection. A future-epoch offer on a healthy
link is rejected before preparation, without disrupting valid work. During
uncertain recovery, classification waits for the verified recovered epoch;
rejecting a caller must not mark a successfully recovered link as failed.
The focused post-RESET probe passes for P29V1, ZSTD_TU and ZSTD_ROUTE: reject
the future offer, then commit the original older ARM with exact bytes and
cumulative ACK without another connector call. The full sender suite also
passes on that frozen closure (retained container
`p51-postreset-allprofiles-r2`, exit 0). Evidence under
`/tanksmall/scratch/tmp/p51-route-reap.6Pp7wd/logs/`:
`post-reset-offer-rebase-allprofiles-r1.log`, SHA256
`7418d0d67ce14cd527ec8527ab142c1ce41a23620ecbb71b2329de4e97ae6898`;
`sender-full-after-postreset-r1.log`, SHA256
`1b05610e9ea5860c78e3e42cf7294867cd429870af1ff4798dfaf7b2fda05e1e`.
Sender source SHA256:
`b42a6fb0361f70d1d29a9fb52748e6728d31f5e40a12135f67332118cb5c926c`;
test source: `f305c2e34e1442679d853a41684b8e108f4ccdaac61a75fbfe13a61c7cca089e`.
This closure includes endpoint idle-history bookkeeping changes; it is not
qualification of the combined service/route-owner changes. The focused probe
does not exercise a future offer during a lost RESET reply; the separate
`--future-arm-during-recovery` ZSTD_TU selector now covers that case. A
request-keyed observation proves the future request enters recovery before
reconnect completes. It is rejected locally while both original jobs commit
exactly once, cumulative ACK reaches 2, and connector count remains 3.
Frozen `future-arm-during-recovery-r5.log` in the same evidence directory
has SHA256 `877c62b9cdfc8d9efe2d38bdc99bc0ea7f374127556fd6045ae6c4dd988e48f8`,
exit 0. Sender source/test SHA256:
`b17e1de52cf94241272c22f224668d7192d8bd098bca93ce792d895b38f69837` /
`281674764746adcb196acafe8495415e4e9a5403fbd2094593acd54d8968d417`.
This selector does not establish equivalent coverage for both other profiles.
Concurrent replacement callers have a separate focused result below.
The matching endpoint unit suite passes separately, exit 0:
`post-reset-endpoint-unit-r1.log` in the same directory, SHA256
`a37e440c8ae7d87432f0190baa5f2f6659e8d5506b942437238e2d39fade9532`.
Endpoint source/header SHA256:
`bc83d20be683c0b1574a900a16b7fffcb13b64af2a12a080ffd9afdca8c3071d` /
`58e25a4aaa3424ccbbe3d5c5c232e00271debafc9a34db9a13ccb2ec94312d2b`.
The endpoint now tracks the exact R2 relationship owning each codec history
and exposes quiescent history retirement without deleting committed inputs.
The focused owner test `p50routeowner --same-successor-concurrency` passes:
two callers for the same successor wait behind the old ACK pump, then both
commit exact bytes through one replacement connection. It asserts two total
connectors (old plus successor), one old-history retirement, and executor-owned
teardown. Evidence: `same-successor-owner-r2.log` under the same log directory,
SHA256 `3fc256f9e8d6ed800cc4e9ef911f5930f55baf4887f149b2ced723d1a7a28ae2`,
exit 0. Owner source/header SHA256:
`aa4a27be1a0da5485a07713273fb4519b3d8f9c2f1448ad7fd8a5294fa288c80` /
`2c411e5573aec2ac455b0d233e858068bfcd654f2c8d2ec9c89946b1b9e1e1d6`;
test source `4aa3a205399b89e01fbb11549f35f64cd447ce483ffd121ede4496694ea21e41`;
binary `ee11355bb64d0874e08eb1fff5e70b0d0ccb52c7ae15ac2bda87efc1d026db4e`.
This is one ZSTD_TU concurrency case, not qualification of the combined
service/owner closure or all profiles/topologies. The combined native suites
are recorded above; broader farm/topology qualification remains open.

The R2 sender now retries failed initial connection setup for the original
caller, retaining its prepared TU, assignment, and absolute deadline. It uses
the shared bounded backoff, checks retirement before using a returned fd,
and does not invoke recovery for a link that never opened. First-attempt
failure followed by W30 and refill passes for all three profiles with two
connector calls. Always-failing setup expires with bounded attempts and one
preparation; retirement wakes the retry wait. The full sender suite passes:
`/tanksmall/scratch/tmp/p51-connect-retry-r0/build/p51-connect-retry-full-r2.log`,
SHA256 `e417952ea5423d56901020fb583c90042253845b8f25fe3a7a7bac66a1c438de`,
`SENDER_FULL_EXIT=0` and retained container exit 0. Binary:
`9327ac45d9436675db13a9af2369d72c0d1610603d1e0040d31e289e4c1c6c05`;
sender source: `60b7981db89d4eac4d855a9fa33b06a9762046a1a7257534ef28633f46d22c20`;
test source: `d383676b5a861cc2cc7976293c4b6959fca3a9da352d985a32e29de74c1a8157`.
This sender-suite result alone does not qualify daemon multi-link operation,
the admission-queue refactor, or same-F idle-history retirement changes.

The real-daemon multi-link gate now passes all 18 cells: C1F2/3/4 and
C2/3/4F1, each with P29V1, ZSTD_TU and ZSTD_ROUTE. Each link reaches 30
outstanding jobs; the largest cases check 120 exact input attachments and
healthy-link progress while one relationship's receipts remain held.
The C4F1/ZSTD_ROUTE cell needed six setup attempts to establish four links
and still completed all 120 jobs. Evidence:
`/tanksmall/scratch/tmp/p51-multilink-retry-r6/tmp/matrix-r6.log`, SHA256
`bfb21a840767a763e6c89e51e4e85df413d6e3ed1399d74a14060764cff764c3`;
`matrix-r6.exit` records `MULTILINK_MATRIX_EXIT=0`, and the retained
`p51-multilink-retry-r6-matrix` container exited 0 without OOM.
Fixture source SHA256:
`40039cd641a61c1b4a517f02b2ffebc691b96005adf1f6b8a7b0d7d35580963f`;
fixture binary:
`00afb791ca3feabfad1bffd341b808ad7b1d260eae270c28426b1fddc3e07603`.
This frozen build combines the earlier r5 product baseline with the committed
initial-connection retry sender; its service source is
`7a05a98f1e74489b3ca064e4f051e35ea9d8bc33b83b1fc5f46333d526ae094f`.
It predates typed missing-reservation handling and the aggregate-oversize
error distinction, and excludes the pending admission/history changes.
It qualifies this matrix fixture and retry integration, not final HEAD,
restart behavior, cross-host throughput, or the complete W30 release gate.

The same frozen binaries also pass the two cache-process replacement cases:
ZSTD_TU C1F2/F-cache and C2F1/C-cache. Each holds one affected job after F
commit but before C observes the receipt, replaces that cache process, checks
healthy-sibling progress and rejects the discarded old completion, then
attaches a new assignment using the replacement store identity. Evidence:
`/tanksmall/scratch/tmp/p51-multilink-retry-r6/tmp/restart-r6-run.log`, SHA256
`870c09d1537b67c03535be3c5a6c0dbb0639473d18f034735eccffcfbbd6b170`;
`restart-r6-run.exit` records `RESTART_GATE_EXIT=0`. This is not a W30-occupied
restart, scheduler/ordinary-daemon restart, all-profile restart matrix, or
qualification of the pending combined source. An earlier invocation failed
before testing because the copied script was not executable; the successful
run invokes it through `sh`, as the Make target does.

The existing default daemon regression wrapper passes on those same frozen
binaries (`DEFAULT_DAEMON_EXIT=0`):
`/tanksmall/scratch/tmp/p51-multilink-retry-r6/tmp/default-daemon-r6-run.log`,
SHA256 `41ad7ecfd4d0ff57c9174334651d5ec8283dd03460132fd6751c3e1653210b13`.
This checks that the added opt-in fixture modes do not replace the default
daemon tests; it is not a full native/farm suite result.

The expanded cache-replacement gate now passes all six W30 cells:
C1F2/F-cache and C2F1/C-cache, each with P29V1, ZSTD_TU and ZSTD_ROUTE.
Each holds exactly 30 old commits before C observes them, replaces only the
affected cache process, checks that all 30 original callers settle without
accepting the discarded receipts, and preserves healthy-sibling progress.
A fresh window then reaches 30 held commits before receipt release and
attaches all 30 exact inputs on the replacement store identity. Evidence:
`/tanksmall/scratch/tmp/p51-restart-w30-r0/tmp/restart-w30-r0-matrix.log`,
SHA256 `55a1c7a88b31090d33f49970eb0f9a1a33d7962460580f01f1e8d66b218999dd`;
the adjacent exit file records `RESTART_W30_MATRIX_EXIT=0`.
Fixture source SHA256:
`5b29ed1a6c7cf6c2847afe309fd0e4b4a5d62a7a57410647e4ba120ab4200bbd`;
fixture binary:
`f6307c11f33d5cf1c2a30ec6773f057d5c0464aeb4f9f15d0a95245b949b91d5`.
This uses the frozen r6 product binaries described above, not the pending
admission/history/recovery changes. It proves bounded caller settlement,
not a measured per-caller finish-time comparison against the original
deadline. Scheduler/ordinary-daemon/compiler replacement and final combined
qualification remain separate obligations.
The follow-up timing-instrumented fixture also passes all six cells. It
records each caller's actual original deadline and completion time, rather
than treating the outer watchdog as its deadline. All 360 old/fresh callers
finish before their original deadlines (the assertion permits 2 seconds of
cleanup grace). Evidence:
`/tanksmall/scratch/tmp/p51-restart-w30-r1/tmp/restart-w30-r1-matrix-deadlines.log`,
SHA256 `14e7299400eb26c267f4fcb9caf29a478b68938c62670d0a4db0718ae1fc7c752`;
the adjacent exit file records `RESTART_W30_DEADLINE_MATRIX_EXIT=0`.
Fixture source SHA256:
`bb08a7c00df4b9a3e0490742d9250cd3d05750ff36671a7af6259dd54a88ee53`;
binary: `d78391f71e30ab12c4b149985b736b29daa8ff8c4e98bc04d867f27af5ecc148`.
This still uses the same frozen product binaries, not the pending combined
implementation; it measures prompt restart settlement, not deadline expiry.
The existing two-case, one-job restart mode also passes with this generalized
fixture (`restart-w30-r0-focused.log` in the same directory). Its `set -e`
command then failed on an outdated source guard copied into the private tree;
the current guard and shell syntax checks pass separately in
`restart-w30-source-guard.log`, SHA256
`d26a269dd2f5e49571a6c3eb67127c5d1cc4d1ece08dc5e3f37a6cb12a457b36`.

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

The sender also passes the full suite with repeated shared-link
loss for 2 and 30 callers across all three profiles: the original COMMIT
reply and then RESET_ACK are lost; a third connection replays the exact reset
and finishes the retained suffix. Each input is published once and cumulative
ACK reaches the final ordinal. This is not a second failure after a fully
confirmed reset. Retirement checks cover stopping recovery without another
connection and preserving an already validated positive commit, including
exact replay and rejection of conflicting input.
Evidence: `/tanksmall/scratch/tmp/p51-retire-final-freeze-GuhC4u/logs/build-and-full-sender3.log`
(SHA256 `2496323fdb73f2fec83a8642f1b805433acfacc5e0dc84d6ed5266f05b59cd29`),
exit 0 in 94.33 seconds; freshly linked binary SHA256
`40a70201ded065de7cff6e2d795ded27c0055d468e0a3c009e21abdc9e6f992d`.
The build records matching source hashes before and after execution. Earlier
attempts with unretained test source or failed builds followed by execution
of a copied binary are not qualification evidence for these changes.
The preceding diagnostic run timed out (exit 124). The recovery fixture now
posts retirement to the sender owner and waits for server completion before
stopping its event loop; previously it could wait forever on a future whose
executor had already stopped. This does not resolve the separately recorded
first-bundle timeout or qualify process restarts and real-daemon integration.

The focused `p50zstdsender --deadline-recovery-ack` selector also passes:
expiry during the recovery connector returns DeadlineExceeded on the original
deadline (two connection attempts, no RESET, exact F commit retained), while
expiry after an exact validated receipt preserves Committed without opening
another connection. ACK drain after expiry is not required. This is a
test-only addition; the full sender suite was not rerun for it. Evidence:
`/tanksmall/scratch/tmp/p51-deadline-tests-r2-20260924T221518Z/logs/deadline-recovery-ack-r2.log`
(SHA256 `c2cb8fe2e92a84bab3ef1406825743adc492fe530c0c4e43c591c6377749a7c9`),
exit 0; binary SHA256
`51c6950de873a46f873b2832e6635f7c68d4e1835f60bb2018c6b4664b82d3de`.

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
The integration overlay also passes a real-daemon ZSTD_TU W30 gate against
the committed recovery core plus its narrowly scoped GCC 13 receipt-reader
allocator-warning guard. A relay holds 30 distinct complete COMMIT frames
while all 30 callers remain pending, then releases them; all exact inputs and
all 30 original compiler-channel attachments are accepted. This is a small
synthetic concurrency gate, not a compiler-output or throughput benchmark.
Evidence: `/tanksmall/scratch/tmp/p51-vertical-clean-candidate/run-28b-w30-r1.log`
(SHA256 `fd7ffa8a726b164c4dddfb827039327e17602bdf771834521a191bdaa6fc92ca`).
Its strict build passes in `build-28b-r4.log` under the same directory
(SHA256 `6418bb7ee60b658afd539110c57f122f2cd2524fb8cfcade70db37906f65acd1`).
The integration overlay now passes that real-daemon W30 gate for P29V1,
ZSTD_TU and ZSTD_ROUTE, with temporary diagnostics removed. Each profile
holds 30 complete COMMIT frames before releasing any receipts, then checks
30 exact results and original-channel input attachments. Evidence:
`/tanksmall/scratch/tmp/p51-vertical-unified-0ed20/logs/run-all-profiles-w30-clean-r1.log`
(SHA256 `2061531188b1b9ce1d4e1e50398a5cacfbce714383054493d7cd6a5b5b6d4d4e`),
exit 0; test binary SHA256
`325712e79d9efc8d32ac938f3ea66314398eca35f7a18dc2ba146d9c09108cdc`.
This snapshot uses the pre-retirement-change sender core, not the later
`94e9b440` sender. The route-only failure was a product bug: two reservation
checks cast the advertisement bitmask (ZSTD_ROUTE = 4) to the profile ID
(ZSTD_ROUTE = 3). Explicit mapping fixes both link lookup and job admission.
The focused service regression covers all three profiles and verifies that
wrong-profile rejection leaves the correct reservation consumable.
The opt-in integration is included on the candidate branch. With the two
daemon-cleanup fixes, the explicit
`make -C unittests p50daemonpositive-p51-w30-check` target passes all three
30-job profiles in an isolated root container with NET_ADMIN, iptables,
an unprivileged `icecc` identity and writable scratch. It is not part of the
default native test set. Final target evidence under the same root:
`logs/run-w30-make-target-r1.log`
(SHA256 `c49b6edaccfc7fe711944b1f7333059a453d7eda29fa4ea4ef207f51596068a5`).
The standard daemon runner also passes default, pending-disconnect, P51
cancellation/replacement and one-job vertical modes:
`logs/run-daemon-standard-direct-r1.log`
(SHA256 `d43259eed25ae938a10663e598df5480420a0ff275ba95cf51da025f6a06b9e2`).
These runs retain the pinned pre-retirement sender core described above.
Allocation failure in orphaned-ARM retention was reviewed but not directly
injected.

A fresh Git archive of combined commit `b1790ab1` subsequently passes the
full sender suite, service suite, and explicit three-profile real-daemon W30
target. Its generated build helpers were recreated with `autogen.sh` before
the successful build. Evidence under
`/tanksmall/scratch/tmp/p51-b1790-qa.1w2Xvi/logs/`:
`run-sender-service-r1.log`
(SHA256 `1bdee288b20d6c9bb0cbecf0663010098d435082dc9ab156638eb97c4a31be38`)
and `run-w30-target-r1.log`
(SHA256 `001c3bd76ae1bf1e3d32721f24ea16cd847a9c5561cd14c527f408ece4c4865b`),
both exit 0. A subsequent standard daemon runner on the exact combined commit
passes its default invocation but fails in the pending-disconnect invocation:
the later source-arm ACK and dependent CACHE_SESSION checks fail. Evidence:
`run-daemon-standard-r1.log` under the same root (exit 1, SHA256
`454cf61e8a85f6d2642a1e2a554420a6848d800dec04952cba8f4953e9f4e6a4`).
The runner stops there, before P51 cancellation/replacement and vertical modes.
The daemon rejects the next source ARM with `advertisement=0`. The cause was
adapter retirement after an expected `UnknownRecord` response to cancelling
an input that never arrived. The adapter now completes that exact CancelAttempt
as a replayable no-op; other missing-record lifecycle operations still retire
the relationship. A second defect delayed queued lifecycle work until its
deadline on an otherwise idle connection; queued actionable work now requests
an immediate outer-loop turn.

Both behavior changes pass all four standard-daemon invocations in
`run-daemon-standard-r3.log` (exit 0, SHA256
`69f42f1be793d512a748f51ab8ce5f66a61bda856360149cac899b58f78ea645`).
That run used daemon SHA256
`3ac8a459c3e253564acc26545431cfb75ddea915f92b0a29dbb43c466738e130`.
The final source adds explanatory comments and passes the adapter unit suite:
quiet-connection cancellation, exact replay, a second cancellation, and
missing-record CloseLogicalInputLease still withdrawing the relationship.
Evidence: `build-adapter-cancel-final-r1.log` under the same logs root (exit 0,
SHA256 `d33099d66f79c6e7327f4e3f429f0b2ef794a462b7bbf4142e66cb849ba67a6e`).
Its daemon SHA256 is
`4e7c38fe58dd8a701686dab316383bef5e89d6f1614f4afb5243eccd1a0b4414`;
the final-source standard-daemon rerun also passes all four invocations:
`run-daemon-standard-r4.log` (inner and container exit 0, SHA256
`9745f9ecd05391c8e76b14deac41102fda73add12d59ca09482f3b5f4877b86b`).
These four invocations
include a one-job vertical transfer, not the separate 30-job receipt gate.

The same final daemon also passes the explicit three-profile W30 target:
P29V1, ZSTD_TU and ZSTD_ROUTE each observe `jobs=30 peak_held_commits=30`,
then exact input identities and accepted original-channel compiler attachments.
Evidence: `run-w30-target-r2.log` in the same logs root (exit 0, SHA256
`71c9668635d4d51a7d80722f08a935017ead1521016007a3d37d5988640ed64d`).
The daemon, service and fixture binaries are unchanged from the final standard
runner. These are small-input concurrency tests, not throughput measurements
or proof of process-restart recovery.

The actual wrapper now passes 31 sequential remote compilations for each of
P29V1, ZSTD_TU and ZSTD_ROUTE, with byte-identical local/remote objects and one
persistent data link per measured batch. Environment warmup and deliberate
pre-batch sidecar rotations are excluded from that connection count.
The harness uses short container paths backed by host scratch storage and
preserves empty optional compile-database columns with explicit TSV sentinels;
shell whitespace collapsing previously misread a source file as a database.
Evidence: `/tanksmall/scratch/tmp/p51-wrapper-rerun3.log` (exit 0, SHA256
`461efdd7131395e1f24c38353d6169bf30b8c75a9755caf25825a9b2b8d09909`).
Per-profile logs, offsets and corpus are retained in
`/tanksmall/scratch/tmp/p51-wrapper-rerun.9klM9c/evidence-31/`.
The coherent test snapshot includes the combined integration/cancellation
changes and private diagnostic logging. Client/daemon/service binary SHA256:
`cdf21ff1ef1767595a00815de301f4ede2d77d68198d77d65240ae6acb950361`,
`967a582c54a40079d927abd1d1fac74d416af7da0c353f21493ce6f6070aaedb`,
`007c22e2f6aa206dc8bed5c5fd81b2fadc02d28de66a19b26adcdfdf449e3fac`.
This proves sequential link reuse, not 30 simultaneous wrapper invocations;
the separate receipt gate proves W30 concurrency.

The same coherent snapshot also passes C01's 2-job and 100-job batches for
all three profiles, each with one measured persistent link and exact remote
objects. Logs: `/tanksmall/scratch/tmp/p51-wrapper-2.log` (container exit 0,
SHA256 `eb29c28332dbdd2489540772a21eec5fd49b601db675cd34fb4afdbcbba7140c`)
and `/tanksmall/scratch/tmp/p51-wrapper-100.log` (all required per-profile and
final PASS markers; numeric container exit was not retained, SHA256
`d69ba781b3e6d790b1e27cd980aad0697244b6c8732d5391187b73e7abe56b4e`).
Their artifact roots are `/tanksmall/scratch/tmp/p51-wrapper-2.CfxV3i/evidence-2`
and `/tanksmall/scratch/tmp/p51-wrapper-100.ZL1Fq1/evidence-100`.
The opt-in `make -C unittests p51wrappercompile-check` runs both counts across
all profiles, requiring a built tree, disposable test container, writable
`ICEFARM_TMPDIR` and `ICECC_P50_C1F1_WORKER_SCHEDULER_HOST`. Generated-Makefile
dry-run validation passes in `/tanksmall/scratch/tmp/p51-wrapper-make-dryrun.log`
(SHA256 `a7c4b81fb6363b5c18e17a582d325823b329fb8c74e80d9e9af0435606c87919`).
The entry point itself has not yet been executed end-to-end; the underlying
script runs above provide runtime evidence, and the dry run checks invocation.

The shared R2 reconnect gate now backs off from 5 ms to a 500 ms cap across
all callers on a relationship, without extending their original deadlines.
Only successful reconciliation resets the delay. Focused W1/W30 tests observe
10 aggregate connections in about 1.4 seconds against a replacement F that
accepts TCP and rejects the stale HELLO. Retirement interrupts a registered
499 ms retry wait in under 1 ms in the recorded run. Shared/repeated recovery
for all three profiles, deadline handling and positive-receipt preservation
also pass. Evidence: `logs/backoff-stage1-final.log` under
`/tanksmall/scratch/tmp/p51-b1790-qa.1w2Xvi/` (SHA256
`27f172874d9e75d7825805161b408e8dad697b6f6be18a239b7dd7a2e0966816`).
The full sender suite passes in `logs/backoff-full-sender.log` under that root
(SHA256 `fb5ce5afccce501205bf5265951ee2a67b4eb4c463304cb2a7afdc8c84f94974`),
both with container exit 0. Sender source SHA256 is
`c10bb81e817ba1ab890b791c55e565b16ab559fc99bf3b9bd71c2cd2d6ce42ec`.
The wakeup regression is registered in the default suite. After that
registration-only edit, rebuild and the focused selector pass in
`logs/backoff-registration-r1.log` (SHA256
`38a0c1e8579b73ba495e29a2884efc48364bc259bf6af6c8675b322037dc867e`).
A scratch-only mutation removing the retirement wake fails the intended
200 ms latency assertion, exit 134, in `logs/backoff-negative-no-wake.log`
(SHA256 `0c311eb2ca1e11e9c94b815187013a8ad81f8210a1acc12f092c07f35fb3a412`).
That deliberately failing mutant is not product source. Final test source
SHA256 is `dcb61c5d0c819e39da74246e2f1618daa1ebad21d4439546f7fdc592796c9aef`.
These runs use the private build snapshot with the exact sender overlay,
not a clean final combined candidate. Retry pacing is not a repair for the
separate old-route retirement failure below and does not add typed rejection.

The type-25 R2 link-rejection codec passes `p50wire`: exact 18-byte shape,
both reasons, golden domain-bound HELLO digest, identity sensitivity and
malformed/trailing-byte rejection. Evidence: `logs/link-reject-codec-r2.log`
under the sender build root above (SHA256
`43c54a68ea739a6af92fd2dcf26c590ea2ad5207bf856e7c02c4422e95c489ac`),
container exit 0; test binary SHA256
`a0f2eefca1d71db084ce76ab531a20cf816ff138376f74e76fbcdc81a53c74a7`.
That run is codec-only evidence. Subsequent endpoint/owner and sender tests
cover StoreReplaced emission and exact-offer rejection handling:

- Endpoint stale-generation rejection and full route-owner suite pass,
  including all 18 direct W30 topology/profile cells and idle background-pump
  retirement cleanup. Evidence:
  `/tanksmall/scratch/tmp/p51-route-reap.6Pp7wd/focused-build-run.log`, SHA256
  `98fce46abb7c5256b110692fee0303ae466cff073ed571a2ccbf2dfd12d8a497`.
  Earlier failed build/test attempts remain in that log; its final run passed.
  Route-owner binary: `c2b4b82e5ddef678e5189211f7f09e6e8ba8e11558581440c86aaee35aaa987d`.
- The combined sender suite passes all-profile W30/recovery, initial and
  two-caller shared rejection, typed reconnect rejection, and ordinary EOF
  backoff/retirement. Sender binary:
  `c9ae15df33977bb2639058d4c451fe3413b38a9a5493edbd2787d8518ab2770a`.
  The agent retained a concise tool-output excerpt, **not a raw run log**, at
  `/tanksmall/scratch/tmp/p51-typed-reject.YJYEqc-r2/logs/sender-full-r4-tool-transcript.txt`
  (SHA256 `7d31b32757ae0c93527a2f98c8fc8c6c5c7c649f4e1fdfc374217fd429a45525`).
  The fail-fast command reached `SENDER_FULL_PASS`; a separate outer exit-code
  field was not retained. Tested sender source:
  `a69768b8739b1ff9598c26339d32e5bb31e322b65f08cceba710e24ca48ebf4e`.

The owner run used the preceding sender snapshot; the full sender run used
the same endpoint/owner sources plus the shared-caller rejection fix.
These are not final combined service/restart qualification. The expanded
sender gates now pass with 30 callers sharing one rejection/connector,
wrong-digest and stale-physical-offer rejection negatives, and preservation
of a prior committed result plus exact replay after a sibling's rejection.
Evidence: `logs/rejection-expanded-r3.log` under the typed-reject root above
(SHA256 `d196519b077efdb8b3fadadf2b199b62556bc48351bf07a74bddafbd4c0d3ef7`),
`DOCKER_EXIT=0`. Test source SHA256:
`f20ba1e76cdfcf522a94c78c2c327cc9be61d2cd48b097ecac285f22ff4f5ebb`;
binary: `f0962b18b36238fc33591a74ad71cee801106389c4f95ea3b7028cc55ed85103`.
These focused selectors use the preceding committed product snapshot, not
the subsequent typed lookup. The ZSTD_TU positive-result gate now also passes
for the same original caller: F waits for C to validate COMMIT, then resets
the socket before C can send its ACK. The reconnect receives a typed
ReservationMissing, but the original result and exact replay stay Committed
and retain that rejection for route cleanup. The sibling-rejection case is
retained and also passes. Evidence under the same typed-reject logs root:
`same-caller-barrier-runtime-r3.log`, SHA256
`76473bf6db529f2d2695a7720b5e1e006ca6be48edea918c72bcc548c9ab1a17`,
`DOCKER_EXIT=0`. Test source SHA256:
`0d78c2e18a965a18320ba883cff4d994d898e3768e6b5f1e7fdfce6b641781a7`;
binary: `94b1fcfbae8defe700156708cf838b47e1d7f45ee5b1c5764ed08ab13a5f78fe`.
This focused sender test is not a real service missing-reservation recovery
or an all-profile qualification of this specific failure boundary.

The real multi-link matrix exposed a daemon admission mismatch: C1F3/P29V1
delivered only 65 of 90 source leases, explicitly refusing 25 at the old
64-pending-setup cap. Its three links reached 30/30/5 receipts. The daemon
now uses a named 120-entry bound for P51 lease/ARM setup and orphan/cancel
cleanup queues; the separate 64 control-worker limit is unchanged. The
previously failing C1F3/P29V1 case now passes with 90 jobs, W30 per link,
healthy-link progress while one link is held, and all exact input attachments.
Evidence: `/tanksmall/scratch/tmp/p51-multilink.WhiHmZ/c1f3-cap-r1.log`
(SHA256 `1b732880eb8019a76f5e55a6d1510690ee7d8cf9951991bdc039dfdac52f9fd4`),
`C1F3_CAP_EXIT=0`. Daemon source:
`6b7b9b908548fafde2189762019076cfe50283115a6fc57b71c3458a6d796ddf`;
binary: `8d1671d6fc7ea20831c33d4d07079b5c763a79e52c9ac3329cb107b895930d8f`.
The private fixture is still under qualification. This result does not prove
the full 18-cell matrix, 120-job cancellation bursts, or runtime overflow
handling; compile-time predicate checks only cover the 119/120 boundary.

An isolated C4F1/P29V1 rerun also passes: four physical links, 120 jobs,
30 unique commits per link, healthy-link progress while one receipt stream
is held, and exact input attachments. Retained container
`p51-c4f1-p29-diag3` exited 0 with all recorded cgroup memory events zero.
Evidence: `tmp/diag-run/test.log` under the same multi-link evidence root,
SHA256 `3fe091d9aad6a8fbd8a589e499a6206935cf300a6b9815f1241120f608aade8b`.
This used the same daemon binary listed above, without the new handoff
diagnostics. It does not explain the earlier C4F1 setup disconnect after
15 passing matrix cells, nor qualify the complete matrix or repeated runs.

The R2 source service now reports `SourceTooLarge` (0x5003 in the existing
result field) when a valid source exceeds the aggregate raw-byte budget;
it no longer labels that immediate refusal as deadline expiry. The test
checks the exact returned code, no F connection for that source, fitting
source credit/socket admission, and stop-time credit release. This does not
yet prove per-job cancellation while waiting or freedom from preparation-pool
head-of-line blocking. An independent `--aggregate-fit-exact` selector now
proves that the same runtime rejects a 17-byte source against a 16-byte
aggregate cap, then transfers a fitting 12-byte source through the ordinary
P51 link-session handshake and real R2 endpoint. It checks materialized bytes,
commit digest, and released raw credit. Evidence:
`/tanksmall/scratch/tmp/p51-route-reap.6Pp7wd/logs/aggregate-fit-exact-r1.log`,
SHA256 `71ee0b557778e1991c3187ce9849a3168aa8c3051bdbe2b7f384fba922f21944`,
exit 0; binary `33466055777984a1005c060994220804a30c643f0112475abe91d8514f121c95`.
The executed test TU was `ef1cb58db72da34022a696b832c14f4b62f425e1b6dee12c0b6b780652621328`;
it also contained separate, unqualified reconnect work, excluded from this
test's commit. This focused pass does not qualify that work or the full suite.

A real C route-owner/F service test also passes for an initial reservation
evicted before its first link: typed Missing, fresh same-F relationship with
the expected committed byte count/digest, and a separate C owner's persistent
sibling link committing before and after. This is not established-link
eviction/reconnect or compiler attachment coverage. Both the focused selector
and full service test executable passed in the same run:
`/tanksmall/scratch/tmp/p51-route-reap.6Pp7wd/logs/same-f-real-transfer-r4.log`,
SHA256 `0fdbab6e7d87dfd80dbee900f15c045b923a46d4c850ec175ea2cb10e012e66b`,
exit 0. Test binary SHA256:
`b023fc9247c4183754862d60eddc3dfa1ec02acde3148a0e45ba182ce01345ec`;
test source: `c20d74a5ef3d9bde697cfa2d6e676bc9fc50a0ff10ffb629953d6ae0236fd936`;
service source: `4fe79a375e23b2a5bfdc5e38af2d5ba87bb5879067c21b4e070f59dfbb145a82`.
The run rebuilt the service test, not the separate external service executable.

The runtime now distinguishes definite reservation absence from an invalid
offer in one owner-thread lookup. Only an absent initial reservation or
absent reconnect relationship produces ReservationMissing; mismatched fields
and ambiguous empty fixture lookups remain Invalid. The endpoint emits the
exact-offer-bound rejection for definite absence. Focused endpoint tests and
the full service suite pass; direct service assertions cover missing initial
and reconnect entries, wrong profile, and exact valid lookup for all profiles.
Evidence under `/tanksmall/scratch/tmp/p51-route-reap.6Pp7wd/logs/`:
`typed-missing-endpoint.log` (SHA256
`e7bc950fcc722a5a120da3338f0165caaa2a0a6dde131e465570e6b4d19c3dd7`),
and `typed-missing-service-complete.log` (SHA256
`15778027ffb7d6be70629e69af497a0157cc64de6807cf79d8b1417979233320`).
Earlier incomplete invocations omitted required test environment settings;
they are not passes. This does not yet prove a real same-F missing-reservation
followed by fresh assignment and healthy sibling progress. An additional
owner/lifecycle unit now checks that an evicted idle relationship can be
reassigned on the same F incarnation, a live sibling keeps its relationship,
and a mismatched logical offer is Invalid. It directly records synthetic
commit/ACK state: it does not transfer or materialize source bytes. Full
service-suite evidence: `logs/same-f-owner-rerun-r2.log` under that root
(SHA256 `73bfc5bcbb6b06e6a3987b5d251edcc60712b5ae3e58f02642d1007fcf1e1382`),
exit 0. Test source: `7d396847bcf882b060b493827a80db4f511bdd046ed2ae0c0b9e000de6764759`;
binary: `9af5ccdc625da05076e4046bd4b55c609f98a5d9bd1604133aec11a1becffa01`.
The real multi-link daemon fixture remains separate work.

The current recovery service passes the scoped ZSTD_TU restart rerun for
C1F2/F-cache and C2F1/C-cache: an established healthy sibling attaches while
the affected parent is stopped, the unobserved old receipt is not reported
as committed, and a fresh assignment attaches on the replacement incarnation.
Evidence: `/tanksmall/scratch/tmp/p51-route-reap.6Pp7wd/restart-final.log`
(SHA256 `0c171ee36f1b6ddf7cfe53ede08ab8df44e8d66d199b04edef961d7b884e322c`),
`RESTART_GATE_EXIT=0`. Service binary SHA256:
`fbb662685cfaba230551ed6d7907ef9ea29ab4a16071a3582d87899720cd2a1f`;
fixture binary: `6aa9a4d2f706f1a8d6a4ec52eb535fcbd255712b1828634b5a81d4384967944c`.
The private fixture source is
`fbccf0a1233dd3f76f9191897e3c927c1fd7f7962a857fea3e27840d12667750`;
the private adapter adds configuration diagnostics only. Initial runs failed
before exercising restart because the daemon's unprivileged identity could
not traverse the private build root; the successful run corrected that
directory's traversal permission. This is one affected job plus sibling
progress, not W30 restart occupancy, all profiles, scheduler restart, or
qualification of the in-development multi-link fixture.

The in-development real C2F1/C-cache restart fixture passes for ZSTD_TU:
the independent C2/F1 link attaches exact input while C1's parent is stopped,
the C1 sidecar PID changes, the discarded old receipt is not reported as a
commit, and a fresh assignment attaches exact input under the new C identity.
Evidence: `/tanksmall/scratch/tmp/p51-restart-review.Blc6Qk/restart-C2F1.raw.log`
(SHA256 `ae54386653f5034a45a786ee9a214f6b96eac59364fd5adf7eab8b061f2c6d76`).
The diagnostic fixture source hash is
`c83b5cbba6dc3f3100159f6d30c5bff0b55708b2da3adebeb087f06330d559c7`;
test binary hash is
`56cb2ea9aca1c6a617cad94399e458a27033fe1fa04b66e07900830ffb6d26f8`.
This is not a clean upstream qualification, W30 restart test, or all-profile
restart result; the fixture remains under review.

The initial C1F2/F-cache restart fixture **failed**: healthy sibling
progress succeeds, but the obsolete F identity triggers thousands of retries
and the fresh assignment returns Error without input identity or attachment.
Evidence: `restart-attach-diagnostic4.raw.log` under the same root
(SHA256 `2cb1fd3a7cdf28e5fa3b4e39c599849e958fd302ef5f585dbf32a00df4c197f1`).
Its final summary's `fresh=1` indicates the selected scenario, not a passing
fresh-transfer assertion; the log contains explicit FAILED assertions.
Shared retry pacing is covered above; typed R2 rejection remains pending.
Fresh replacement attachment now has a confirmed failure path: the old F route
retains one preparation; `reset_f_store_exact` cannot reset that live route,
so endpoint-identity binding rejects the fresh assignment with error 4 and
zero transfer attempts. Diagnostic evidence: `restart-reset-diagnostic.raw.log`
under the same root (SHA256
`ea84ba3e0a41f9859e1e946ee3b50d206d1202f3470725b3f4858ba53972264e`).
The in-development old-incarnation retirement fix now passes that C1F2
ZSTD_TU case with the committed retry-backoff sender: the pre-existing healthy
sibling attaches while the affected parent is stopped, the discarded old
receipt is not reported committed, and a fresh F identity commits 47 exact
bytes and attaches to CompileFile. Evidence:
`/tanksmall/scratch/tmp/p51-retirement-final.0kjizw/restart-C1F2-stage1.log`
(SHA256 `1a37f804a3c81d9b9ddfc50083475d7d739d344a9965f31224c04dc05c5b295d`),
outer exit 0; test binary
`176ac72fa805f5535dd9fb2666555427d49f76613f8b8942c2e0d2e495fd991c`.
The old operation made 63 bounded connection attempts during its original
35-second deadline. This private snapshot predates typed-rejection integration;
it is not qualification of the current combined worktree, all profiles,
or 30 concurrent jobs during restart. Landing the tested retirement fix and
qualifying the combined implementation remain pending.
Retry backoff alone cannot repair this admission failure. The observed
cancellation withdrawal refers to the dead old incarnation, not its successor.
Next: finish
wrapper C01, real C/F/S restart gates, and mixed-version qualification. Do not
count same-process socket recovery as a process-restart test.

A separate coherent integration snapshot passes 55 native P50 targets,
including ordinary 43/49/50/51, R1/R2 and profile-selection boundary tests:
`/tanksmall/scratch/tmp/p51-compat-qa.W5ISl6/native-p50-suite-r4.log`
(SHA256 `020755ec900944ee44f1ccf6edb95959005a8da75a4f12ca53ebf07c6921feb2`).
The subsequent profile-mapping service regression also passes in
`profile-mask-service-r1.log` under that root
(SHA256 `ababdabd15ab1df014beb1a969cd8ab3a50d89bd860a546ce357552238925da0`).
Full service concurrency, recovery, restarts, wrapper and mixed-farm gates
remain open. The Stage A results below do not qualify these changes.
No Chromium build/download has been started for this implementation step.

The separate async-service candidate passes the full `p50cacheservice` suite
against the committed sender/endpoint. Added cases cover local reply deadline
and peer closure, slot reuse, the global 120-reservation limit, cancellation
and post-deadline slot reuse, publication/reset lifecycle, and shutdown while an accepted
connection stalls before ordinary protocol admission completes. That last
case verifies both operation and raw-byte credits return to zero. A second
case completes ordinary protocol admission, observes the link-session request,
then withholds its reply; shutdown also returns both credits in that phase.
The optional cancellation-aware handshake polls do not extend the original deadline.
A third case observes R2 LINK_HELLO and withholds LINK_STATE. Whole-runtime
shutdown retires the C sender on its owner executor and returns both credits.
These tests do not yet prove cancellation during body/recovery or 120 active
transfers. This older 120-reservation test did not prove autonomous timer expiry:
its post-wait reservation request itself calls the owner sweep. A non-sweeping
observation before any new request, with a timer-disabled negative control,
is now qualified under "Autonomous metadata expiry" above. This does not invalidate the
separate unpublished-job expiry callback evidence documented above.
This service snapshot still uses the earlier committed sender and
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

The recovery runner now passes 26 rows, including six explicit reduced-action
staged-unsent cancellation/reindex witnesses for C2/3/4F1 and C1F2/3/4, and a
double-release mutant rejected by its exact invariant. Each new witness
requires committed work on every sibling link. These are reachability checks,
not exhaustive active-cancellation safety or proof of the C++ repair.
The ordinary specification retains its full action relation. Final log:
`/tanksmall/scratch/tmp/pipeline-cancel-reindex-spec.RFAWnp/run.log`, SHA256
`08c08c0fe54dc93108a20c09c1d0c8594a477935d673f281390540d410c2758b`;
recovery model SHA256
`7ff25e0a84825e25c25601b19664ddef0d5abd16501e40c92550a6571b7b734b`.
Earlier model bookkeeping/parser failures and timed-out searches are not
passing evidence. Active sent/working cancellation and immutable unavailable
results across lost reset replies remain separate model work.

A subsequent 28-row run adds a directed four-job active-cancel witness and
an un-emitted-backlog-loss mutant. The witness preserves the positive prefix,
accepts F cancellation while an old worker remains charged, loses/retries
RESET_ACK and the confirmation echo, reindexes two survivors and retains them
through a replay interruption. The mutant loses the un-emitted survivor and
fails the intended invariant. This is a one-link scenario model, not general
snapshot/deadline validation; the second disconnect abstracts backlog
retention, not another complete wire recovery. Aggregate evidence:
`/tanksmall/scratch/tmp/pipeline-active-cancel-aggregate.4WcR1I/aggregate.log`,
SHA256 `cc15de6356af65cf622b25ac864653faae5416bf8a940b4c73026ae6b132fc04`;
active model SHA256
`6c1251c1e3d2b7e9725923db54f248b3725375709fa01cf37096afa29f4d900f`.
The candidate repair's real C/F evidence and remaining runtime gaps are
recorded above under active cancellation; the model does not replace those
tests or complete the broader W30 qualification.

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
