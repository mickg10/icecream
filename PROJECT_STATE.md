# Sorbet 1.5.0: validation and remaining work

Updated 2026-09-25. Package version is **1.5.0**; the release branch is
`sorbet_v1.5`. The repository is public. The Docker bootstrap implementation
was published as `de027cefc31d79d062c3158400951916a9aa5d63`.
A pushed branch is not a published release tag or a newly qualified farm image.

Use [README.md](README.md) and [dev/README.md](dev/README.md) for setup.
This file records evidence boundaries and open work, not a chronological
agent log. Earlier diagnostic reports remain in Git history and their
retained artifact directories.

## Developer QA

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

Real multi-survivor replay interruption, all D07 cancellation positions/stages,
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

### Clean-checkout build and mixed-version compatibility

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
