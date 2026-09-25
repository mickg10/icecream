# P50 transfer concurrency: implementation and acceptance contract

Baseline: Sorbet 1.5.0, commit `3ae4c67a8647467020a114fb2b2cb1fc05121281`.
This document specifies a staged change; a stage is implemented only when
its product changes and the named tests have passing evidence. It is not a
record of successful execution. Current wire definitions remain in
[P50_PROTOCOL.md](../cache/P50_PROTOCOL.md),
[P29V1_FORMAT.md](../cache/codec/P29V1_FORMAT.md), and
[ZSTD_FORMATS.md](../cache/codec/ZSTD_FORMATS.md).

## 1. Scope and topology notation

`C<n>F<m>/W<w>` means n C-cache stores, m F-cache stores, and at most w
unacknowledged source transactions per C/F relationship. Compiler processes
behind either cache are not extra cache stores. Compilation is not included
in the source transaction window: source service ends at exact input commit,
not at compiler-result return.

The initial implementation is **A: independent R1 relationships, W1**.
It must support C1F2, C1F3 and C1F4 without a process-wide transfer gate,
while preserving one active operation per C/F incarnation across profiles.
The user confirmed both topology directions: the formal/test matrix also
covers C2F1, C3F1 and C4F1. Bare `2/1`, `3/1` and `4/1` are not accepted
evidence labels: use the full topology and W.

Subsequent stages are specified here but are not implicitly enabled by A:

| Stage | Change | Wire contract | Completion requirement |
| --- | --- | --- | --- |
| A | Independent links, bounded source admission, nonblocking retry setup | Unchanged R1; W1 | Sections 2-5 and A gates below |
| B | Read/intern ahead; optionally prepare predicted FILL early | R1 for preparation only | Byte parity, bounded staging, cancellation tests |
| C | Persistent link with separate per-job ownership | Explicitly negotiated extension | Job binding, idle/reconnect and mixed-version gates |
| D | Ordered speculative TU pipeline, configurable W up to 30 | Negotiated extension | Prefix/entropy recovery, window/byte bounds, D gates |

Shipping A must not wait for a speculative protocol redesign. Conversely,
passing A must never be described as persistent connections or W30 support.
Primary work is design/review; Luna implementation, product-test and formal
lanes execute the changes and tests. No release tag, push, remote deployment
or new full-farm qualification follows automatically from these gates.

## 2. Stage A architecture

### 2.1 Identities and ownership

Keep the existing C-wide preparation authority, monotonically allocated
TU sequence, immutable input record, interner and append-only Block catalogue.
Matcher, F-known membership, relative sequence, history nonce, entropy state
and commit state remain profile/relationship-local.

Use two levels of admission identity:

1. Selected endpoint `(host, cache_port)` before F identity is known.
2. Exact `(C_STORE_GUID, F_STORE_GUID, F_STORE_GENERATION)` after SOURCE_ARMED.

The second gate excludes profile because F's namespace currently has one
active session for a C store across profiles. Profile remains in the codec
route key. Endpoint aliases naming the same store must converge before
CacheWire activation. A TCP descriptor is never an identity. Same address
with a changed generation is never permission to reuse the old history.

Keep address admission through the operation, or an equivalent reservation
that prevents a same-address successor racing the current operation. An alias
may establish F identity, but it must wait before replacing the active F
session. In the current transition this means before SESSION_HELLO activates
the namespace; an already completed ordinary CACHE_SESSION/ready exchange
does not itself activate that namespace. Pending aliases must not consume
every execution credit and prevent an unrelated F from running.

All reads and mutations of `P50CRouteOwner`, preparation state, relationship
maps and endpoint-incarnation binding execute on the existing owner executor.
The old global mutex made worker-side preflight accesses safe by excluding
overlap; merely deleting it does not preserve that property. Worker threads
may own input bytes, blocking file reads and connection setup, not route state.
Owner callbacks must never wait on a mutex/condition whose release requires
another owner callback.

### 2.2 Admission and resources

Expose these local RuntimeConfig controls; they are not new wire fields:

| Field | Initial default | Meaning |
| --- | --- | --- |
| `max_active_source_transfers` | 4 | Concurrent admitted source operations per C sidecar |
| `max_aggregate_source_raw_bytes` | 2 GiB | Sum of reserved source-vector lengths |

Validate nonzero/addressable limits at construction. The aggregate default
retains room for one existing maximum-size input; it is not permission to
allocate four maximum-size inputs. Existing encoded, route, interner and F
pending-input limits remain independently enforced. Raw reservation does not
claim to account for all process RSS: encoded buffers, copies and retained
dictionary state must be reported separately.

Admission must follow these rules:

- Check source descriptor type/length with fstat before allocating its vector.
- Reserve bytes with overflow-safe arithmetic before the read. A file-size
  change cannot cause allocation/read beyond that reservation.
- Waiters for a busy address/incarnation do not take every global execution
  credit. Queue metadata remains bounded by existing control admission.
- Count/byte pressure waits under the original deadline; it is not permanent
  route-table exhaustion and must not require sidecar replacement.
- Retain existing pre-open refusals for known exhausted endpoint/route tables.
  Pending reservations count against table limits. For an unknown identity,
  resolve it before installing an over-capacity route or activating CacheWire.
- Release each reservation exactly once on success, read/setup/route error,
  queue expiry, stop and rejected incarnation. A delayed callback cannot
  release a successor's reservation.
- Do not retire/reset an old incarnation while one of its operations still
  owns mutable state. Drain, settle or reject replacement; do not guess.

A short mutex protecting admission bookkeeping is acceptable. No such mutex
may span file I/O, connection setup, CacheWire, materialization or commit wait.
Avoid a thread per queued TU or unbounded asynchronous task admission.

### 2.3 Setup, retries and lifetime

Initial source reads and connection/arm work can run on the already bounded
control workers. Retry setup currently calls a synchronous ConnectedFdFactory
from the endpoint coroutine; move production retry work to asynchronous I/O
or a fixed, bounded setup executor. Retain simple fixture factories if useful.

An offloaded factory receives the original deadline and only immutable
request/identity data. Transfer its resulting descriptor exactly once; close
it if its recipient has expired or been replaced. No detached task may retain
a pointer to a destroyed runtime. Runtime shutdown must drain/cancel owned
work while its completion executor is still alive. Keep the existing bounded
stop/settlement rule: do not publish a successor while an old coroutine still
owns the relationship. Timeout must never restart the absolute deadline.

Ordinary connection setup must also be safe for concurrent calls: use per-call
resolver results, not shared `gethostbyname` storage. Offloading a blocking
resolver protects the owner executor but does not make arbitrary name-service
lookups cancellable. Record that limitation; numeric scheduler-selected
addresses avoid it. Once stop or whole-C replacement is observed, no retry may
open another source session. Prefer normal settlement of active work; if it
cannot settle within the existing cancellation grace, supervised process exit
is an allowed bounded outcome, not a successful graceful shutdown.

Initial setup still runs on a control worker. A blocking system resolver there
can hold that worker and the in-process service's join; the retry-pool grace
does not make this path cancellable. The daemon-managed sidecar's outer
supervisor escalates from TERM to KILL under its configured bounds and requires
exact process-group absence/reap before replacement. Direct, embedded service
use has no equivalent intrinsic resolver bound. Numeric scheduler-selected
addresses and ordinary arm/deadline handling are the tested local path; do not
describe A13 as proving cancellation of arbitrary resolver or filesystem calls.

Route preparation and answer-NEED remain owner-affine in A. Thus A overlaps
network/setup waits; it does not claim parallel C encoding. A long CPU task
or blocked retry must not be confused with the same failure in measurements.

### 2.4 Files and simplification

Primary product edits belong in `cache/p50_cache_service.{h,cpp}` and the
existing sender/route-owner implementation if an asynchronous factory is
needed. Reuse existing messages, state owners and resource limits. Do not
create a parallel transport stack, duplicate dictionaries, a generic scheduler
framework or a collection of tiny single-purpose headers.
The shared ordinary-connect helper may need a focused thread-safety correction;
that is not permission to redesign ordinary transport.

Remove obsolete assertions that require a process-wide source mutex, replacing
them with executable ownership/progress tests. Preserve unrelated lifecycle,
deadline, exact-byte and old-peer checks. No P43 or CacheWire R1 bytes change.

### 2.5 Coexistence and rollout

A changes local admission, not advertisements or codec formats. A new C sidecar
must work with an existing revision-1 F sidecar; an existing C can continue
using an upgraded F. P43 selection and ordinary source transfer remain
unchanged. Same C/F across different profiles still shares one admission gate.
Do not infer a new pipeline capability merely from the package version.

Restarting the sidecar discards its local admission tables and establishes the
normal new store identity. A stale callback from the old runtime must never
enter the replacement runtime. The local active-transfer setting of one is
useful for comparison, but does not roll back trace schema or other code
changes; an actual old-peer compatibility test still needs the old binary.
Stages C/D require a separately selected extension and coexistence tests, not
a flag that sends several TUs to an unmodified R1 peer.

## 3. Later pipeline contract

### 3.1 Preparation and predicted NEED

P29 BODY construction already computes the missing-Region list. Actual NEED
must equal that list in exact order. Preparing FILL ahead of NEED therefore
need not invent F cache residency. Pin the route's system-source reuse choice
first, retain size checks and validate actual NEED before local commit.

Sending predicted FILL before receiving NEED is a separate B optimization,
not a consequence of early preparation. It needs one concurrent reader and
one writer to avoid full-duplex buffer stalls. Prove actual FILL validation,
NEED mismatch behavior and lost-commit reconciliation before enabling it.
Do not silently omit mandatory R1 NEED/TU_END messages.

### 3.2 Persistent ordered link

C must establish job/assignment/source identity per TU, separately from link
lifetime. A cached TU and a compiler attempt remain different objects. Each
job retains its own deadline and settlement even when its transport is shared.
Negotiate persistence before any irreversible use of the new mode. An R1
peer still expects one TU and EOF; never send a successor to it. Capability
advertisement/selection must work through S as well as C/F; the current exact
revision validation is not an extensible feature negotiation mechanism.

Use one reader and one queued writer per link. Writer ownership spans a whole
outer frame, including partial writes. Never hold it across a round trip.
Serializing callbacks alone does not prevent two suspended writes interleaving.
For strictly ordered complete TU streams, generic lane IDs are not required.
Arbitrary interleaving of TU fragments requires identification absent in R1.

### 3.3 Speculative prefix and recovery

Preserve one ordered dictionary history per link. Let A be C's observed commit
prefix, K F's actual commit prefix, and P C's prepared prefix. Within an exact
incarnation/history, require `A <= K <= P` and matching identities/digests for
every promoted prefix. F applies and commits only a contiguous prefix.

Retain bounded per-TU records for `(assignment, request, TU_SEQ, REL_SEQ,
history, raw identity, transaction identity, bytes/reconstruction input,
dictionary delta, deadline, disposition)` until reconciled. A prefix witness
alone is not a per-job completion list. Lost replies cannot turn F-committed
work into permission to execute another compiler result.

P29 has a single matcher rollback journal and continuing control/literal
compression streams today. D needs staged successor state and an explicit
entropy recovery rule, not just a deque around the current API. After a
disconnect, reconcile K before disposing of or rebuilding any suffix. Retained
continuation bytes must not be replayed into reset compression contexts.
An initially conservative implementation may reconcile, reset the route and
rebuild the uncommitted suffix; it must preserve already committed inputs/jobs.

Cancel queued, unencoded work without touching history. Once later TUs depend
on an encoded TU, cancellation cannot punch a hole in the stream: finish the
cache transaction without authorizing the cancelled compiler attempt, or
settle/reset the affected suffix. Partial-frame failure resets the transport;
it cannot be repaired by removing the rest of that frame from a write queue.

W is a ceiling, not a memory promise. Reserve raw/encoded bytes, staged state
and output capacity independently. Existing 64 control workers and the F
codec pool's two workers/eight outstanding jobs are separate bottlenecks.
Run W=1,2,4,8,16,30 and select depth from evidence, not a presumed 30x gain.

## 4. Exact product-test schema

### 4.1 Common contract

Use real SidecarRuntime/sender/endpoint paths with local TCP peers and exact
input bytes. A fake ordinary peer may control arm/transition timing, but
codec/commit assertions must exercise the actual endpoint. Test fixtures use
independent C/F identities and explicit barriers/promises, not sleeps as proof
of ordering. A bounded wait is a watchdog, not the correctness oracle.

Each case identifies: stage, case ID, topology, profile(s), input fixture and
digest, barriers/fault trigger, exact assertions, maximum duration and cleanup.
Every successful TU checks C identity, input identity, exact TU sequence
(zero is valid and is the initial sequence),
raw length/digest, selected profile and exact reconstructed bytes. Distinct
requests within one C get distinct TU sequences; an exact retained request
keeps its identity. Each relationship advances only its own route cursor.

For concurrency, require observable overlapping CacheWire operations, not
merely overlapping arm calls or multiple threads. For isolation, require a
healthy commit while the unrelated stalled operation is still held. Always
release fixture barriers and join owned threads on both success and failure.

### 4.2 Stage A required matrix

| ID | Setup / controlled event | Required observation |
| --- | --- | --- |
| A01-N | C1F2/3/4, W1, independent F identities; hold each before commit | All N reach TX_BEGIN/BODY before releasing first commit; N exact commits |
| A02-N | C2/3/4F1, W1, independent C identities | Concurrent namespaces, exact bytes/cursors; no C crosses another namespace |
| A03 | Same C/F/profile, two requests, first held | Second cannot activate/replace first; both eventually commit in order |
| A04 | Same C/F, different profiles, first held | Same serialization as A03; profile changes cannot bypass the gate |
| A05 | F-A holds initial SOURCE_ARMED; F-B healthy | B commits before A is released or times out |
| A06 | A enters a held retry connection/arm; B healthy | Owner processes B to commit while A's retry remains blocked |
| A07 | Different endpoint aliases report same F GUID/generation | At most one active C namespace session; no replacement of live predecessor |
| A08 | Same address reports a new incarnation while old work exists | No old-history reuse or late publication; replacement waits/settles or rejects |
| A09 | Count cap N with N admitted held transfers; N+1 and unrelated waiter | Cap never exceeded; busy-link waiters do not consume all credits; release admits waiting independent work |
| A10 | Raw sizes at aggregate cap, then one byte over remaining capacity | Reservation precedes vector allocation; bounded wait/expiry before read/open; release permits retry |
| A11-kind | Success, read error, open error, route error, expiry and stop | Exactly-once release of credits/descriptors; subsequent healthy work remains admissible |
| A12 | Same-link queued request has earlier deadline than held predecessor | Expires at original deadline; no late open/arm/commit for expired request |
| A13 | Stop with queued, opening and active requests | Owned operations terminate/drain within configured bounds; no later state publication |
| A14 | Distinct routes share a Block minted by an aborted route | No false F-known reference; other route defines required content and reconstructs exactly |
| A15 | Existing one-TU peer / mixed P43 path | R1 messages and EOF behavior unchanged; existing profile and ordinary compatibility tests pass |
| A16 | C-wide preparation failure while another relationship is active | No new/retry setup after terminal owner state; settle retained exact witnesses or stop safely without destroying state still referenced by a coroutine |

A01/A03/A05/A06 run for P29V1, ZSTD_TU and ZSTD_ROUTE. A01 includes a repeated
and edited second TU on each route, proving warm continuation and isolation.
If a case only covers one profile, its receipt must say so; it does not cover
the omitted cells. Memory/queue tests use small configured limits and small
inputs, not GiB allocations. Use a 10-second fixture watchdog unless a
specific existing runtime bound requires more; record overrides explicitly.
Deadline tests allow scheduler tolerance, but must also prove absence of
post-expiry admission/publication. No absolute microbenchmark threshold is
an acceptance condition for the portable correctness suite.

At least A05 or A01 must fail against the old global-gate behavior. A negative
control that restores global serialization must fail the named progress/overlap
assertion, not merely crash or time out without a diagnostic. Preserve the
existing mutation controls for stale identity, invalid bytes and lost commits.

### 4.3 Additional B/C/D gates

- B: predicted NEED equals actual NEED on cold/warm/edited inputs; large NEED
  plus large FILL drains both directions; mismatch has no local commit; loss
  before/after F commit reconciles exactly; byte parity with serial encoding.
- C: many consecutive jobs reuse one link; no job borrows another assignment
  or deadline; idle EOF and peer replacement handled; new/old mode selection
  is explicit; no fallback after partial new-mode admission.
- D: each W=1,2,4,8,16,30 preserves exact serial decoding; bound counts and
  bytes; lose the connection at every transaction boundary and during a frame;
  lose multiple commit replies; verify K and rebuild only the unresolved suffix.
- D cancellation: first/middle/last TU; cancel before encoding, during BODY,
  during FILL and after F commit; no holes or stale successor publication.
- D restart matrix: C', F', compiler F and S' independently and in paired
  orderings; preserved cache history never substitutes for assignment identity.

## 5. Formal gate and evidence schema

The existing core model's one C-active/one F-pending assumptions do not prove
cross-link concurrency. Add a focused model beside it, integrated with the
existing aggregate manifest/runner. Do not replace all established models or
claim their prior runs cover the new code.

Model A explicitly for C2F1/W1, C3F1/W1, C4F1/W1 and C1F2/W1, C1F3/W1,
C1F4/W1. Require simultaneous-live-link reachability in every topology, not
just a larger constant with only one enabled global operation. Model bounded
requests, store generations, delayed callbacks, lost commit replies, admission
count/byte reservations and release; abstract codec bytes by exact identities.

Required properties:

1. One active session per exact C/F incarnation across profiles.
2. Independent C namespace/route state and unique C-wide request allocation.
3. Count and byte reservations equal owned operations; neither exceeds cap.
4. Stale callback cannot publish or release a newer operation's reservation.
5. Committed-but-unobserved work retains a reconciliation witness.
6. Expiry/stop prevents new admission and late publication, without erasing
   evidence required to settle an already committed input.
7. An unrelated admitted healthy link can complete while another link stalls,
   under stated fairness and available resource assumptions.

Include expected-failure controls for wrong gate identity, stale completion,
incorrect credit release and lost-commit disposal. A global-serialization
control must defeat the concurrency witness/progress property. Distinguish
reachability witnesses (intentional violation of a 'never reached' predicate)
from actual erroneous implementations. Each must fail through its exact named
invariant/property, not an unrelated parser error, timeout or memory limit.

Run smallest topology first, then 3 and 4. Bound heap/workers and direct TLC
state files to unique directories under ICEFARM_TMPDIR. No host `/tmp`
fallback. A timeout/resource exhaustion is incomplete evidence, never PASS.
State-space reduction must preserve the interleavings being claimed.

### 5.1 Required receipt fields

Retain the existing native logs and formal aggregate receipt formats. Their
combined evidence index must contain these fields (JSON types shown):

```json
{
  "schema": "icecream-p50-concurrency-evidence-v1",
  "stage": "A",
  "source_commit": "40 lowercase hexadecimal characters",
  "source_snapshot_sha256": "64 lowercase hexadecimal characters",
  "dirty": true,
  "cases": [{
    "id": "A01-4",
    "kind": "product",
    "topology": {"c_stores": 1, "f_stores": 4, "window_per_link": 1},
    "profiles": ["P29V1"],
    "command": ["exact executable", "exact arguments"],
    "exit_code": 0,
    "status": "PASS",
    "elapsed_ns": 0,
    "assertions": ["all_four_body_before_first_commit", "exact_raw_bytes"],
    "logs": [{"path": "relative retained log", "sha256": "64 lowercase hex"}]
  }]
}
```

The example is a shape, not a passing receipt. Fields cannot be invented from
test names: assertions must be bound to executed assertions or checked traces.
Status is PASS, FAIL, SKIP or INCOMPLETE; the latter two never satisfy a required
cell. IDs/topology/profile cells must be unique. Missing required cells keep
the overall stage incomplete. Expected-failure rows additionally name the
expected and observed diagnostic; nonzero exit alone is insufficient.
Native evidence includes binary digest and build/toolchain identity. Formal
evidence includes TLC jar/module/config digests, command, explored states,
completion marker and each expected-failure diagnostic. Record source changes
after a build instead of attributing a stale binary to the latest checkout.
When one executable checks several cells, its command/log may be shared by
those cells. Record `elapsed_scope: "command"` if only that executable's
duration was measured; do not invent individual cell timings or sum the
repeated command duration as total test time.

### 5.2 Performance experiment, separate from correctness

Use the existing source-result trace to measure admission wait/service; add
separate source-read, setup/retry, preparation, NEED wait and F materialization
durations if needed. Do not silently relabel old `source_mutex_*` fields.
Stage A emits `icecream-p50-source-result-v3` with the existing field layout:
`source_mutex_wait_ns` is the sum of address/incarnation/credit admission wait
intervals, and `source_mutex_service_ns` is total operation elapsed time minus
those intervals. The historical field names are compatibility labels, not
claims that a global mutex is still held. Setup, reading and network service
are included in service. The collector accepts exact v2 and v3 layouts and
rejects unknown versions; v2 retains its original global-gate interpretation.
Neither version's sum of service durations is aggregate CPU time or build wall
time. In particular, v3 service intervals can overlap across relationships.
Measure baseline and candidate on identical input order, profiles, machine,
compiler slots, link shaping and cache state. Use Firefox, RocksDB and a third
available corpus, each cold/warm/edited, with input manifests/digests retained.

Report source TU/s, build wall time, healthy-link delay while another stalls,
queue p50/p95/p99, raw/wire bytes, owner CPU, peak memory, active counts and
connection count. Whole compile duration is not source service time. A modeled
compile trace is not an execution measurement. Shared NIC capacity is not
multiplied by the number of links. Do not sum overlapping wait durations into
a claimed wall-time improvement.

For planning only, A's source-stage bound changes from `1/s` to at most `4/s`,
subject to C CPU, NIC and F capacity. For a persistent link, latency L and
bottleneck service b imply throughput at most `min(W/L, 1/b)`. Select W from
measured saturation; record the implemented mode and never label A as W30.
Here s is the measured mean isolated source-operation service time, not whole
compile time. More explicitly, with N independent busy relationships, owner
CPU time c per TU, mean wire bytes q and shared link capacity B, a planning
ceiling is `min(N/s, 1/c, B/q, aggregate F capacity)`. None of these bounds is
a measured gain. If source transfer accounts for fraction f of build wall
time, a source-only speedup k gives at most `1 / (1 - f + f/k)` build speedup
under that simplified workload model.

## 6. Reproduction and candidate publication

Run the normal checkout gate from the repository root on a Linux Docker host
with Git, Make and the pinned uv installed:

```sh
export ICEFARM_TMPDIR=/absolute/existing/writable/scratch
make qa
```

This uses the checked-in `farm.json`; see [developer QA](../dev/README.md)
for a different resource preset or an offline SDK image. It builds and installs
the source, runs native checks (including the service and sender concurrency
cases), the separate root service checks, the Python suite, and all five local
mixed Docker cases. The wrapper retains a unique result directory. Do not
reuse a staged source directory for an edited checkout or attribute an older
binary's results to newer source. A selected native test invocation alone is
not this complete gate.

For selective rechecks in an existing SDK build, preserve the runner's
unprivileged staging ownership, writable offline uv environment, scratch
binds and `SYS_PTRACE` capability. If overriding its entrypoint, use Docker
`--init` so deliberately orphaned test children are reaped. Do not copy
unusable worktree Git metadata into a source-only snapshot. Retain failed
attempts as failed: passing targeted corrections may complement a full run
when all required cells and exact source/binary identities are accounted for,
but do not turn that original command into a successful `make qa` invocation.

Run the additional focused formal lane separately, with Java and the pinned
TLC jar described in [formal setup](../cache/formal/README.md):

```sh
export TLA2TOOLS_JAR=/absolute/path/to/tla2tools.jar
TLC_STATE_ROOT=$(mktemp -d "$ICEFARM_TMPDIR/p50-concurrency-tlc.XXXXXX")
export TLC_STATE_ROOT
ROW_TIMEOUT_SECONDS=120 sh cache/formal/run_transfer_concurrency_tlc.sh \
  > "$TLC_STATE_ROOT/lane.log" 2>&1
```

The runner checks the jar digest, uses two TLC workers and a 2 GiB heap, and
refuses reused row outputs. Inspect its exit status and exact expected-failure
diagnostics. Its 21 rows are the six topology safety checks, six simultaneous
running witnesses, isolated healthy-link progress, byte pressure, bounded
generation replacement, and six negative controls. A new focused lane pass
does not assert a fresh run of every pre-existing formal model.

For the candidate branch `sorbet_1.5_pipeline`, commit these directions before
the implementation. Before upload, freeze the source, complete the required
Stage A cells and compatibility checks, and retain the section 5.1 evidence
index with exact source/binary/tool identities. Record results and limitations
in [PROJECT_STATE.md](../PROJECT_STATE.md). Commit the tested implementation,
push only the candidate branch, and verify its remote commit. Do not create a
release tag, replace the release branch, or describe this as W30, a measured
corpus speedup, or renewed external S* qualification.

## 7. Persistent links and W30: implementation specification

**Status: planned, not implemented.** This section extends stages C/D against
the Stage A candidate `7e16cd0e10babfbc9e21c47a2edf2956cb8c1715`.
It does not change the R1 format documents or establish a new passing gate.
The deliverable is persistent source connections and a configurable ordered
window including W30 for P29V1, ZSTD_TU and ZSTD_ROUTE. W30 is an upper bound,
not thirty concurrent compilers, thirty codec workers, or a memory allowance.
Full Chromium capture is a separate resource-controlled deliverable (§10).

### 7.1 Decisions and explicit non-goals

1. Introduce an explicitly negotiated CacheWire R2. Use an ordinary-protocol
   51 extension for discovery/selection; retain package version 1.5.0.
   Existing P43 and ordinary-50/R1 peers keep their current bytes and behavior.
2. Maintain one live physical source link per exact C/F incarnation, with
   one selected profile. A profile switch drains or reconciles the old link,
   then replaces it. Three profile sockets must not bypass the shared gate.
3. Reuse the compiler's already-open ordinary F connection for the R2 job's
   ARM/ARMED exchange. The source data link is separate and persistent.
   Do not create an extra ordinary TCP connection for every R2 source job.
4. Use ordered complete TU bundles. There is one writer and one independent
   response reader; no generic stream IDs, fragment multiplexing, or parallel
   mutation of a dependent P29/ZSTD_ROUTE codec.
5. Prepare a bounded speculative suffix at C; F decodes and publishes in
   order. Compiler admission still requires the exact individual input commit.
6. On interruption, reconcile every committed receipt, reset both histories,
   and rebuild the uncommitted suffix. Do not clone thirty entropy states or
   replay old continuation bytes into a new history.
7. Do not combine this change with a matcher redesign, a compression-level
   sweep, scheduler policy changes, or a rewrite of all sidecar dispatch.
   Keep additions in existing ownership modules unless a distinct lifetime
   makes a new module materially clearer.

The existing-compiler-socket design is supported by code inspection, not yet
by a passing test: `client/remote.cpp` opens `cserver` before source transfer;
`daemon/main.cpp` returns a completed environment-install Client to UNKNOWN;
ARM requires UNKNOWN; an already-armed Client can later attach CompileFile.
Milestone C1 must verify both installed and already-present environments.
If that path proves impossible, record the precise failed invariant before
substituting per-job control sockets; such a substitute must not be reported
as removal of all extra per-job connection setup.

### 7.2 Compatibility and selection

| Scheduler / endpoints | Required selection and behavior |
| --- | --- |
| Any supported P43 path | Existing non-P50 behavior; no new tail or frame |
| Ordinary-50 scheduler, new C and F | Existing R1 selection, W1, EOF completion |
| New scheduler, either cache endpoint R1-only | R1, W1; no R2 probe bytes |
| New scheduler, both endpoints R2-capable | Exact common revision/profile/features; negotiated W |
| Both R2-capable, operator requests legacy | R1, W1; preserve legacy qualification path |
| No common profile/revision | Existing explicit scheduling/fallback policy, never implicit wire guessing |

The ordinary-51 capability tail must describe supported cache revisions,
profiles and maximum window, not overload a profile bit to mean persistence.
UseCS returns the selected tuple. R2 HELLO independently validates that tuple
against the live endpoint and its limits. Compute the window ceiling as
`min(C_requested, C_cap, F_cap, 30)` initially; byte admission may yield fewer
in-flight TUs. Do not estimate variable-size TU memory by a fixed average.

Audit every `== PROTOCOL_VERSION`, `>= PROTOCOL_VERSION`, revision validator,
Login/UseCS serializer and message factory. Introduce named feature-version
thresholds where appropriate: bumping the maximum must not turn existing
P50 messages into protocol-51-only messages. Preserve the ordinary-50 fixed
tail and R1 EOF contract with golden bytes and old-binary tests. No extension
bytes may be read by an old decoder. New local lease/control records likewise
need a version and exact length, not an unannounced struct-layout change.

### 7.3 Job binding without a per-job data connection

The following ordering is mandatory. Names below describe new operations,
not APIs already available in the checkout.

1. C wrapper obtains a versioned local source-control lease. Extend the
   current `P50CacheControlIdentity` response with the exact C store GUID,
   store generation and derivation version from the daemon's READY lease;
   retain launch generation/attempt and fd ownership checks.
2. On its original compiler connection, the wrapper sends the R2 ARM with
   the exact scheduler assignment, C store identity, selected F identity,
   profile, source request, compiler attempt and local operation identity.
   Do not reuse ordinary-50 ARM's hard-coded R1 validation for this record.
3. F claims the assignment and installs the WAIT owner, then dispatches a
   typed asynchronous reservation to its sidecar. Reserve a bounded logical
   relationship/job row even if the physical data link is not connected yet.
   This avoids a cycle in which ARMED requires a link that requires ARMED.
4. On reservation completion, F rechecks Client/channel lease, READY lease,
   assignment, generation and absolute source deadline before sending ARMED.
   The reply carries the exact reservation handle and relationship epoch.
   If the Client disappeared or expired, retire the reservation; never ACK a
   replacement Client with the same numeric descriptor or request slot.
5. The wrapper passes the actual full ARMED reply and source fd to C's
   sidecar. The sidecar validates its own store/launch identity and selected
   F incarnation, validates source size and reserves count/raw bytes before
   reading or initiating HELLO/JOB_BIND. It creates or reuses the one R2 data link for this logical
   relationship. First HELLO uses the reservation as an exact creation bind;
   F resolves it to its already-installed row rather than creating a job.
6. Each JOB_BIND names the reservation, current physical-link generation,
   exact assignment/attempt/source request and TU identity. F matches and
   consumes the reservation once for that TU. The per-job budget comes from
   the installed F owner, not an arbitrary sender-provided duration.
7. After immutable input publication and exact TX_COMMIT, the wrapper may
   send CompileFile on the same ordinary connection. No detached wrapper
   Client must be searched for or moved for this R2 path. Retain that code for
   R1. Cache commit alone never starts a compiler or extends an assignment.

Logical reservation identity survives physical reconnect only through the
explicit recovery protocol; physical link generation does not. Idle links
must not keep a finished job alive, and finishing the first job must not close
the link used by later jobs. Ordinary connection loss, job cancellation and
S restart retire affected reservations through owner-dispatched cleanup.
Committed inputs may remain cache data without granting compiler admission.
An unrelated valid job on the same link is not cancelled merely because the
first job ended. Bound unconsumed reservations and expire abandoned ones.

Current P5FS is a dedicated per-operation Unix connection, not an existing
multi-job bus. Add narrowly typed reserve/cancel/recovery operations through
the existing owner-dispatch pattern. A control worker must not mutate route
maps. Never block the F event loop awaiting sidecar or network work.

### 7.4 Wire grammar and digest contract

Before enabling R2, specify exact integer widths, byte order, bounded lengths,
message numbers, enums, optional-field rules and digest domains in
`cache/P50_PROTOCOL.md`, with golden encode/decode fixtures. The following is
the required semantic schema; it deliberately does not allocate unreviewed
wire numbers in a design document.

| Record | Required content / rule |
| --- | --- |
| HELLO / STATE | Selected revision/features/profile/window; C/F store identities; logical relationship epoch; physical link generation; codec history nonce; peer byte/frame limits; recovery status |
| JOB_BIND | Exact installed reservation plus assignment/attempt/source request, TU sequence, profile, raw length/digest; current link generation |
| BEGIN / BODY / optional FILL / END | History nonce, contiguous route sequence, TU, pre-state digest, encoded lengths/digests, raw identity, whole-transaction digest; explicit complete-bundle boundary |
| TX_COMMIT | Exact TU/job-binding digest, history/route identity, transaction/raw/post-state digests, monotonic relationship commit ordinal |
| COMMIT_ACK | C's contiguous verified receipt floor, relationship/recovery epoch; no gaps or acknowledgement beyond F's committed prefix |
| RECOVER / RECEIPTS | C's verified floor and retained suffix identity; F's complete bounded receipt interval, not only last commit |
| RESET / RESET_ACK | Idempotent recovery operation ID, settled prefix, old epoch, fresh history nonce and new epoch; no input-record deletion |
| RESET_CONFIRM | Exact recovery operation ID and new epoch; C confirms receipt of RESET_ACK before new TU bundles |
| CLOSE / ERROR | Typed reason and relevant epoch/TU; no successful settlement inferred from EOF |

Use the existing bounded outer-frame shape only if it remains sufficient;
large TU bodies span bounded frames, not a single unchecked allocation.
One writer owns a complete frame across partial writes and owns the whole
TU bundle through END. Control records are inserted only between bundles.
The response reader remains live while the writer is blocked; decoding and
receiving ACKs must not require the same held lock. ACK-floor records have
priority at the next bundle boundary to prevent receipt-capacity deadlock.

The R2 transaction digest must cover the canonical job binding, profile,
history/sequence identity, raw identity and all encoded BODY/FILL bytes with
explicit lengths. Link-generation binding must also be validated; distinguish
stable logical transaction identity from reconnect-specific transport fields.
Use a new domain where semantics differ from R1. Specify exactly which fields
are stable under replay and which are regenerated after history reset.
Duplicate binding, extra END, trailing bytes, missing frames, excessive
lengths, wrong profile and mismatched digest are explicit errors before
publication. No successful compiler reply is inferred from a source prefix.

#### 7.4.1 Planned link-rejection completion

This extension is pending implementation and qualification; it is not a claim
about the current wire codec. Real F-process restart testing exposed silent
rejection of an old F identity followed by excessive reconnect attempts.
Land shared retry pacing first, independently of this wire extension.

Proposed frame 25, `R2_LINK_REJECT`, has exactly 18 payload bytes: a big-endian
u16 reason at offset 0 and a Digest128 at offset 2. Reasons are `1` StoreReplaced
and `2` ReservationMissing; other values, truncation and trailing bytes are
invalid. The digest is XXH3-128 over ASCII `R2-link-offer-v1` (no NUL), followed
by the canonical 181-byte LINK_HELLO payload, without its outer frame header.
Use the existing Digest128 byte representation. This binds the complete offer,
including reservation, relationship/epoch, physical generation, C/F identities,
C control incarnation, profile, window and limits.

F may emit this record only in reply to a decoded, structurally valid LINK_HELLO:
StoreReplaced means the offered F store identity differs from the current one;
ReservationMissing means the current store cannot find the exact offered lease.
The lookup must distinguish definite absence from stale physical generation,
control-incarnation mismatch, or other invalid-offer checks: the existing
optional lease lookup returns no value for all of these and is not by itself
evidence for ReservationMissing. Ambiguous lookup failure retains bounded EOF
handling until a precise reason is available.
Malformed input does not receive an invented identity-bound rejection. C accepts
the rejection only when the reason and digest match the outstanding offer. It
retires that old relationship only, wakes its waiters, and reports a non-success
outcome for unfinished work. It must preserve any already validated exact COMMIT
and must not replace the entire C sidecar or disturb a healthy sibling link.
Fresh assignments to a new F identity remain admissible. No rejection grants
publication, compilation or receipt credit. R1 framing and behavior are unchanged.

Store replacement and logical-link retirement are different operations.
ReservationMissing must not retire the entire F store identity: a fresh
assignment may target that same store. Match the rejected relationship, epoch
and physical generation before changing its owner. Old same-key preparation
must be quiescent and cleaned before a replacement uses that preparation key;
late cleanup must never erase the replacement's state. A fresh ARM is not
proof of a fresh logical relationship, since F may reuse its existing row.
For a genuinely new F identity, fenced old work may drain in a bounded retired
table while the new route is admitted. Retired state continues to count against
resource limits until cleanup; temporary drain pressure must not disable all
of C's unrelated routes or extend the original request deadline.

Older R2 peers and transport EOF remain distinguishable only by bounded retry,
not by an assumed store replacement. Retry pacing is relationship-wide: failed
recovery attempts back off 5, 10, 20, 40, 80, 160, 320, then at most 500 ms;
all callers share the next eligible retry instant. Successful reconciliation
resets the delay; ordinary socket setup alone does not. Waits release writer
ownership, honor the original caller deadline, and wake on retirement. W30
callers must not multiply the reconnect rate.

Required tests: codec golden/round-trip and malformed reasons/lengths; changed
offer fields and stale physical-generation replies cannot retire a newer link;
actual F restart yields bounded old-call completion and a successful fresh
assignment; W1/W30 immediate-EOF peers demonstrate an aggregate attempt bound;
unaffected-link progress and validated-positive-result preservation remain true.
Do not count retry pacing alone as prompt typed replacement detection.

### 7.5 Predicted P29 NEED and speculative codec state

`p29_wire.h` already computes the sender's missing-region list; its NEED
validator checks the expected list/order/terminator. R2 can construct this
canonical NEED locally and use the same FILL encoder, eliminating the wire
NEED round trip. This is not permission to omit F validation. F independently
derives the required data and checks FILL and reconstructed raw identity.
Preserve the case where manifest blocks exist but the missing-region count
is zero: it can still require a NEED/FILL dialogue in the current codec.
Test empty input, zero-missing blocks, nonempty missing regions, repeated
regions, system-source reuse and long entropy continuation independently.

Create an explicit `Staged` preparation state. Advancing speculative history
is not `accept_commit` and must not make an input available to a compiler.
Today the preparation authority has one `uncommitted_route_entry`, and P29
has one pending transaction/journal. Replace that restriction deliberately:
finish a staged TU's local speculative transition before preparing the next,
retain the raw source and immutable transaction metadata until reconciliation,
and keep the speculative codec state separate from the acknowledged floor.
On suffix failure reset/rebuild, rather than retaining W full codec clones.
ZSTD_ROUTE needs the same distinction for dependent history. ZSTD_TU does
not require dependent history, but follows identical job/receipt ordering.

Within one relationship epoch define:

* A: last contiguous commit receipt verified by C.
* K: last input atomically published and receipted by F.
* P: last complete locally staged transaction at C.
* Q: last receipt floor explicitly acknowledged to F by C.

Require `Q <= A <= K <= P`, `P - A <= W`, and `K - Q <= W`.
These are relationship ordinals, not C-wide TU IDs (other routes create gaps
in TU IDs). Allocate an ordered row mapping each ordinal to exact TU/job and
history/REL identity. Bound retained recovery/receipt rows independently
across resets; epoch change is not an excuse to leak the previous ledger.
Distinguish relationship incarnation, physical-link generation and codec
history/recovery epoch explicitly. A codec reset does not renumber the
committed relationship prefix or erase Q/A/K. After settling K, discard the
old speculative suffix and set P to K before rebuilding. Still-live jobs get
contiguous new suffix ordinals, retaining their original TU/job identities;
cancelled uncommitted jobs do not leave holes. A new C/F incarnation starts
a new relationship, not a fabricated continuation of the old counters.
The compiler may proceed for a verified individual commit even if later
transactions remain in flight, provided its ordinary owner is still valid.

F publishes the immutable input record and its receipt as one owner-visible
transition, before sending TX_COMMIT. Store all not-yet-acknowledged receipts;
the R1 single `last_commit` is insufficient. Receipt eviction follows only
an exact COMMIT_ACK or completed retirement policy, never an idle timeout
while an in-flight C can still legitimately recover the epoch.
Enforce this as flow control, not only an assertion: before refilling the
window, the writer emits its latest COMMIT_ACK in full at the next bundle
boundary, ahead of any bundle that would exceed the previous receipt budget.
Track the transmitted ACK floor separately from Q, which advances when F
processes it. F processes these control records in stream order before the
following bundle and never publishes beyond its receipt capacity. On link
loss, the transmitted floor is not proof that F processed it; recovery uses
actual retained receipts and confirmed floors. Both sides must still read
control traffic when payload admission is paused, preventing a full ledger
from blocking the very ACK that frees it.

### 7.6 Interruption, recovery and reset

1. Freeze new preparation/admission for the affected relationship. Fence
   its old socket, writer, reader and worker completions by generation.
   Continue healthy unrelated relationships within global resource caps.
2. Reconnect only to the same verified F incarnation for recovery. Send
   C's A and retained exact suffix identities; F returns every receipt in
   `(A, K]`, bounded by W, including individual job/raw/transaction identity.
3. Validate the entire interval for contiguous order and exact identity
   before advancing A. A last receipt or a numeric prefix alone is not enough.
   Preserve already-published input records and do not compile twice.
4. F settles or explicitly aborts the remaining interrupted pending decode
   and fences late materialization callbacks. Current `reset_history` rejects
   pending/interrupted work: do not bypass that check with a blind reset.
5. Once both sides agree on the settled prefix, execute idempotent RESET.
   Retain the recovery operation/result until C confirms the new epoch.
   If RESET_ACK is lost, repeating RESET returns that same result instead of
   creating another nonce or forgetting the receipt interval.
   C sends RESET_CONFIRM before new bundles. F deduplicates RESET by operation
   ID before checking old-nonce staleness; a duplicate confirmation is harmless.
   Keep one bounded last-reset result per live relationship through the next
   confirmed reset or relationship retirement, so loss of a confirmation does
   not require an unbounded chain of tombstones or erase an ambiguous result.
6. Reset codec/entropy state on both ends without deleting immutable input
   records. Rebuild only the uncommitted suffix from retained raw input in
   order; preserve TU/job identity, use the new nonce and fresh route-relative
   sequence. Revalidate each still-live job reservation/deadline before replay.
7. Resume normal admission only after both sides agree on the new epoch.
   Release each old encoded buffer/credit exactly once; carry raw reservations
   across rebuild rather than counting the same source twice.

If F restarts with a new incarnation, do not claim K survived. Reject old
leases, return the appropriate retry/reschedule result and establish new job
bindings before retransmission. If C restarts, its old callbacks and receipts
cannot become the new process's ownership; retire old reservations, preserve
only inputs that the existing lifecycle permits, and obtain fresh assignments.
If S restarts, cached bytes do not resurrect old scheduling claims. Test S,
C and F independently and in ordered combinations, including the ordinary
compiler connection disappearing while a cache commit is in progress.

Recovery failure is a typed job/link result with bounded cleanup, not an
unbounded reconnect loop. Separate absolute per-job source deadlines from
link idle deadlines and cleanup/recovery time budgets. A keepalive must not
extend a job deadline. A cleanup grace period must not publish expired work.

### 7.7 Cancellation and capacity ownership

| Cancellation point | Required action |
| --- | --- |
| Queued, no route ordinal/state yet | Remove request and release its reservations |
| Staged but wholly unsent | Remove only by truncating/rebuilding the dependent suffix; no sequence hole |
| Frame partly written | Retire physical link; reconcile/reset affected suffix |
| Complete bundle sent, result unknown | Reconcile exact receipt; never assume failure means uncommitted |
| Input committed, job cancelled | Keep allowed cache data; prohibit that job's compiler admission; settle once |
| Deadline expired during decode | Recheck before publication; abort/fence and reconcile; never extend deadline |

A non-expired cancellation may finish a complete cache transaction if that is
the chosen explicit policy, but it must settle the cancelled job separately.
Do not continue after an absolute source deadline merely to avoid resetting
the history. Cancelling first/middle/last entries must preserve the exact
remaining queue and must not leak a credit or consume a later job's receipt.

Reserve count and bytes before allocating or reading a source into memory.
Account separately for raw retention, encoded BODY/FILL, codec working state,
F decoded/materialized input, pending reservations, receipt metadata and
queued socket buffers. Include producer queues and retry buffers in caps.
The existing 2 GiB aggregate source-vector limit is not an RSS limit and must
not be multiplied by 30. Existing 64 control workers and F's two codec workers /
eight outstanding materializations remain distinct constraints to reconcile,
not constants to increase blindly. Stage A's default four active operations
also must not silently prevent the W30 gate from reaching thirty.

Initial rollout keeps R2 opt-in and W1. For R2 tests expose validated window,
raw-byte, encoded-byte, F-output-byte and metadata-count settings through one
configuration path; reject zero/overflow/inconsistent budgets at startup.
Document concrete tested presets before enabling W30 by default. A single TU
that cannot fit must be rejected before staging with an actionable size/cap
error or an explicitly selected bounded legacy path; never switch wire mode
after partial transmission. Fair admission must allow a fitting healthy link
to progress without letting a large waiting TU starve forever.

## 8. Implementation sequence and review boundaries

Each row is an independently reviewable commit boundary, not a request for a
new framework or a separate header per record. Product and test work is done
by Luna agents; the primary reviews ownership, evidence and publication.

| Step | Product work / main files | Exit gate |
| --- | --- | --- |
| C0 | Freeze semantic schema above; exact R2 wire tables and fixtures in protocol docs; audit `services/comm.*` version guards | Old bytes unchanged; all new fields bounded and digest coverage explicit |
| C1 | Versioned local lease; ARM on original `client/remote.cpp` compiler socket; asynchronous F reservation in `daemon/main.cpp`, control-operation/service modules | Environment present/install paths; disconnect/expiry during reservation; same Client later CompileFile; no extra ARM TCP |
| C2 | R2 selection in Login/UseCS and endpoint HELLO; persistent-link table in cache service/endpoint; generation fencing and idle close | Mixed-version matrix; W1 two jobs share one data connection; first job completion does not kill second |
| C3 | Sole writer/reader, exact JOB_BIND and F input lifecycle integration; keep R1 path intact | Real W1 all profiles, compiler ownership/cancellation, repeated warm transfers |
| D0 | Bounded receipt ledger, COMMIT_ACK, idempotent recover/reset | Two or more lost commits reconciled; lost reset reply; interrupted worker fenced |
| D1 | Predicted NEED parity; explicit staged preparation in `p50_endpoint.*`, `p50_slice0.*`, `codec/p29_wire.h`; dependent ZSTD history | Golden codec parity and forced rebuild parity, no fake commit, no compiler exposure |
| D2 | Windowed sender/route owner with separate count/byte admission; `p50_zstd_sender.*`, `p50_route_owner.*` | W2 then W4/W8/W16/W30 real outstanding witnesses; cap tests; cancellation suffix tests |
| D3 | Formal model/config/runner updates plus product fault fixtures | Both topology directions, positive witnesses and named negative controls (§9) |
| D4 | Full native/Python/root/mixed gates, old/new binary upgrades, performance measurements | Frozen evidence identifies exact binary and every required cell; limitations explicit |
| X0–X3 | Chromium pin/preflight, capture/resume, corpus verification, bounded benchmark | §10 completeness and resource gates; not a condition for small correctness tests |

Formal work can proceed alongside C0–C3 from the frozen semantics. Do not
implement speculative P29 state before the W1 binding and recovery ownership
are testable. Do not postpone recovery until after a happy-path W30 demo.
Reuse existing tests/runners and place new tests beside their owning module.
Update protocol docs when bytes become implemented; leave this plan's status
explicit until all relevant gates pass. No test result for Stage A qualifies C/D.

## 9. Exact C/D testing and evidence schema

### 9.1 Execution dimensions

Run real sender/endpoint bytes for all three profiles at W=1,2,4,8,16,30 on
C1F1. Run contention/restart/capacity integration at W1 and W30 for each of
C1F2, C1F3, C1F4, C2F1, C3F1 and C4F1. Cover these both with same-machine
isolated Docker peers and the selected external farm; local topology coverage
is not evidence of cross-host latency behavior. Smaller machines run a
declared capped preset, and must not silently skip required candidate cells.

Every case records source commit, dirty-tree status, binary/image hashes,
profile/revision, requested/negotiated/observed W, topology, seed, operation
counts, input manifest hash, limits, monotonic event trace and result path.
Timed tests use synchronization barriers/events, not arbitrary sleeps, to
place the failure. Retain failed evidence; a rerun is a separate attempt.

### 9.2 Required cases

| ID | Setup / injection | Exact acceptance assertion |
| --- | --- | --- |
| C01 | Two then 100 sequential jobs, each profile | One persistent data connection absent deliberate idle/restart; unique job binding and exact raw output each time |
| C02 | ARM via original compiler channel, environment absent/present | Same Client owns ARM and CompileFile; no extra per-job ordinary ARM connection; no detached-owner move |
| C03 | Delay sidecar reservation; close Client, replace READY lease, expire deadline | No stale ARMED; reservation reclaimed once; healthy next Client succeeds |
| C04 | Old/new C/F/S combinations, explicit legacy selection | Exact negotiated tuple; old bytes/EOF preserved; no R2 bytes sent to R1; real remote compile |
| C05 | Duplicate/wrong assignment, TU, profile, raw identity, reservation or epoch | Typed rejection before input publication/compiler admission; unrelated link still works |
| C06 | Finish first job while later job uses link; idle expiration | Job completion leaves live link usable; idle close reconnects cleanly; no deadline extension |
| D01 | Small TUs, block F receipt delivery while allowing input flow | Sender writes W complete distinct bundles before receiving first receipt; observed unacknowledged peak equals W, including 30 |
| D02 | Deliberately serial W1 implementation under D01/W30 | Test fails the outstanding-window witness, not merely a throughput threshold |
| D03 | Every outer-frame boundary and representative byte offsets inside headers/payloads; forced short writes/EAGAIN | No byte interleaving, no partial publication; reconnect yields exact original raws and one settlement/job |
| D04 | Drop 1, 2 and W commit replies after publication | Recover every exact receipt; no lost committed job, no duplicate compilation or replay of committed input |
| D05 | Lose RECOVER response, RESET request/reply and new-epoch confirmation | Idempotent result/nonce; no forgotten receipt interval; bounded retries and cleanup |
| D06 | Reset while F materialization worker is delayed; deliver old completion after new epoch | Old worker cannot publish, mutate new codec or release new owner's credits |
| D07 | Cancel first/middle/last at queued/staged/partial/full/committed states | No sequence hole, exact suffix rebuild, cancelled compiler never runs, every credit released once |
| D08 | Expire source deadline before bind, during decode and before publication | No late publication/ARMED; cleanup bounded; keepalive does not extend job |
| D09 | S, C, F and compiler restart independently; S→F and F→C sequences | Old generation/assignment cannot attach; typed retry/reschedule; healthy relationships progress |
| D10 | Address aliases, changed F generation, profile switch under load | One profile-free incarnation gate; drain/reconcile before switch; stale callbacks harmless |
| D11 | Cap raw, encoded, F output, metadata and receipt ledger independently | Observed counters never exceed configured caps; admission precedes allocation; no duplex/receipt deadlock |
| D12 | One oversize TU plus fitting unrelated traffic; cancellation while waiting for credit | Actionable oversize result; no hidden allocation; fitting work progresses; bounded starvation policy |
| D13 | Empty/zero-missing/nonempty/reused P29 regions; long entropy stream | Predicted NEED agrees with canonical R1 logic; F independently validates; exact output/digest parity |
| D14 | Compare uninterrupted run with forced reset/rebuild at each TU boundary | Identical raw outputs and logical job outcomes for P29 and both ZSTD profiles; history bytes may legitimately differ |
| D15 | Truncate/oversize/trail/digest-corrupt each new record; duplicate END/ACK beyond K | Specific parser/identity error; bounded allocation; no committed-prefix advancement |
| D16 | Disconnect while writer blocked and reader processing ACK; stop service under pressure | No lock cycle; all owned workers/fds/credits settled within stated timeout |
| D17 | Repeated connect/reset/cancel cycles under sanitizers | No use-after-free, double release, descriptor growth or monotonic retained-byte growth |
| D18 | Local and external mixed farm; old P43 and R1 jobs alongside R2 | Real remote results correct; new traffic does not silently force old jobs local |

For D03 enumerate every record type and all distinct state transitions;
exhaust every byte offset for small fixtures, sample reproducibly for large
payloads. D04 must include multiple committed-but-unobserved jobs, which a
single `last_commit` implementation cannot pass. D11 includes withholding
COMMIT_ACK until the F receipt ledger fills. D16 must distinguish peer silence
from a local lock cycle using trace events and a bounded outer watchdog.
Resource assertions use both exact internal accounting and peak process/
cgroup memory; equality between raw-vector counters and RSS is not expected.

### 9.3 Formal gate

Extend the focused model with logical relationship and physical generations,
reservations, A/K/P/Q, ordered per-TU identities, bounded receipts, recovery
epochs, reset-response loss, deadlines/cancellation and symbolic byte credits.
Check prefix order, exact publication, no duplicate settlement, no stale-owner
effects, one physical incarnation gate, count/byte bounds and healthy-link
progress under explicitly stated fairness assumptions. Model S invalidation
separately from F codec restart; compiler admission is not cache publication.

Exhaustively explore small W2/W3 configurations for both C1F2/3/4 and
C2/3/4F1 as state-space limits permit, with exact bounds reported. Run bounded
W4/8/16/30 accounting/reachability configurations and require an actual
full-window witness. Do not label these an exhaustive proof of arbitrary W30
interleavings. If a larger topology needs abstraction, state the abstraction
and retain concrete runtime coverage; do not silently shrink the topology.

Negative controls deliberately permit a sequence hole, discard all but the
last receipt, accept stale generation, release a credit twice, ACK beyond K,
or reset before pending-worker fencing. Each must fail the named invariant,
not timeout or syntax-check. Liveness claims specify eventual peer response
and scheduler fairness; arbitrary endless restarts do not guarantee progress.
TLA+ does not establish codec byte correctness, parser bounds or C++ memory
safety. Those remain product-test gates. Parameterized proof is additional
evidence only if actually discharged, not implied by finite TLC runs.

### 9.4 Performance and publication acceptance

Measure R1/W1, R2/W1 and R2/W2/4/8/16/30 using identical binaries except for
selected mode, identical input order and declared cache state. Use Firefox,
RocksDB and at least one larger available corpus before full Chromium.
Report cold route, repeated warm route, and edited-pair results separately;
do not call repeated identical input an edited source-tree workload.

Record source wall time, full build wall time when measured, first-commit
latency, p50/p95/p99 source latency, useful raw bytes/s, actual socket bytes,
BODY/FILL/framing/control bytes, TCP connection count, retransmission/rebuild
bytes, C/F CPU time and cycles when available, peak memory, queue delay and
observed window histogram. Include loopback and cross-host runs with measured
RTT/NIC constraints. Run at least three repetitions per performance cell;
report spread and retain all runs rather than selecting the best. A W30
correctness witness is mandatory even if bandwidth saturates at W4.

Any speedup claim names baseline and cache condition. Separate saving setup
latency from saving NEED round trips and from overlapping queued transfer.
Do not promise a 30x gain: dependent codec CPU, input generation, F processing
and NIC capacity remain serial bottlenecks. A performance regression must be
explained before making R2 the default, but cannot be hidden by relaxing
correctness or excluding cancellation/recovery tests.

Freeze a new evidence index covering each case/profile/topology/window and
compatibility binary identity. Optional unavailable Chromium/perf cells say
NOT RUN; required correctness cells cannot pass through skips. Full QA and
focused formal runs belong to the tested source commit, not merely the branch
name. Publish code only after the mandatory gate; preserve Stage A evidence
as evidence for Stage A. Roll out R2/W1, then W2/4/8/16/30 with an explicit
legacy opt-out. Do not create a release tag or overwrite the release branch.

## 10. Full Chromium corpus without unbounded resource use

### 10.1 Define and pin the deliverable

The requested full corpus means every C/C++ compile action in one explicitly
selected Linux x64 `chrome` target build with `use_jumbo_build=false`, including
generated translation units. It does not mean every Chromium platform or
configuration. Name it `chromium-linux-x64-chrome-nonjumbo`; capture exact GN
args and target closure. Do not silently sample it if the machine fills up.

Resolve and record an immutable Chromium commit before fetching, together
with depot_tools commit, DEPS content hash, compiler/tool/sysroot identities,
SDK image digest, OS/architecture and generated GN args. Do not leave a moving
`main` or `latest` as the reproducibility identity. Disable automatic
depot_tools updates after pinning. Use local execution, with no dependence on
remote build credentials. Select debug/release/component settings explicitly
and include them in the corpus identity; changing them creates a new corpus.

Follow the [official Linux build instructions](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/linux/build_instructions.md)
for checkout/build tooling. Their free-space minimum is not an upper bound
for a non-jumbo build plus all preprocessed sources and an archive. Verify
the selected revision's own instructions/tool requirements at pin time.

### 10.2 Preflight and hard bounds

Require an explicit existing `CHROMIUM_WORK_ROOT` on an approved data volume;
never default to `/tmp`, the checkout, home or the root filesystem. Put source,
dependency caches, compiler caches, build output, raw corpus, temporary files,
logs and archives under that managed root. Validate resolved paths and mounts,
including symlinks and container bind mounts, before launching a fetch.

The planning survey found roughly 970 GB free on nas642's scratch volume;
quietbox2 had more CPU/RAM but its reported free storage was on root/home.
These are transient observations, not reservations. Recheck immediately before
execution. Quietbox2 needs an explicitly selected non-root data volume before
heavy capture; do not use its root disk because it appears spacious.

Configure a real filesystem/project quota or equivalently enforceable dedicated
volume limit plus a free-space reserve. Periodic `du` checks alone are not a
hard disk bound. Count checkout/dependencies, transient fetch packs, build,
raw outputs, archive and any concurrent copies; archive creation may briefly
require both raw and compressed copies. If no enforceable disk boundary is
available, report that preflight failure instead of starting the full job.

Set explicit CPU jobs, memory/cgroup ceiling, process limit and reserved host
memory. Initial build and capture run sequentially with conservative jobs;
do not overlap full Chromium build, compression-ceiling jobs and W30 stress.
Cap breach stops the owned process group, records INCOMPLETE and preserves
validated resumable outputs. Do not delete unrelated scratch directories,
kill unrelated compilers or let a failed cap silently reduce corpus coverage.

### 10.3 One managed workflow, not another farm framework

Implement a small `farmharness/integration/chromium_corpus.py` workflow with
tests beside existing corpus tooling; reuse `corpus_archive.py`, lifecycle
handling, `workers/manifest_driver.sh` and the strict TU-manifest contract.
Use the existing farm configuration for machine/container/resource selection.
Do not introduce an independent host-inventory syntax or automatic deployment
service. All Python runs through the repository's pinned uv environment.

Expose two simple Make entry points, **proposed, not yet implemented**:

```sh
CHROMIUM_WORK_ROOT=/approved/data/chromium make corpus-chromium
CHROMIUM_WORK_ROOT=/approved/data/chromium make test-chromium-pipeline
```

The first command validates a checked-in/example configuration completed with
explicit pin, quota and resource settings; absent values fail with instructions.
It preflights, fetches, builds, captures and verifies, resuming only matching
completed phases. The second requires the verified manifest and runs the
bounded selected profile/window matrix using existing farm configuration.
Neither command invents storage defaults, downloads Chromium during `make qa`,
or treats a missing full corpus as a successful Chromium qualification.

Use phase states `planned`, `fetching`, `synced`, `configured`, `target-built`,
`capturing`, `verified`, `archived`. Record command/tool identities, timestamps,
exit status, owned process IDs and cap observations. Lock the run directory
against concurrent writers; a stale lock requires verified process absence.
Support clean interruption/resume, not detached work with no status handle.

### 10.4 Capture correctness and completeness

1. Build the exact target so generated headers/sources exist. Extract compile
   commands for its dependency closure, not every unrelated compilation-db row.
   Preserve the raw compilation database and the selected action inventory.
2. Expand response files deterministically. Preserve compiler, working directory,
   include paths, language, macros, sysroot and forced includes; replace only
   object/dependency-output behavior necessary for preprocessing. Hash the
   original and transformed command and all identified response-file contents.
3. Treat C and C++ separately. Enumerate assembly, Rust, link and other non-C/C++
   actions as declared exclusions. Handle module/PCH actions explicitly; if
   they cannot be represented faithfully, fail full-coverage verification
   rather than silently omitting them. Multiple configurations of one source
   are distinct actions; deduplicate only identical declared action identities.
4. Capture each output to a bounded temporary file, close/flush it, validate
   completion and digest, then atomically rename and journal the completed
   action. A crash must not leave a truncated file marked complete. Include
   generated-input/tool/config fingerprints in resume validation.
5. Verify every selected action has exactly one successful manifest entry;
   report total actions, completed, failed, exclusions by reason, raw bytes,
   content hashes and language counts. Any failed/missing action prevents
   the `verified` state and final full-corpus manifest publication.
6. Archive through existing manifest-aware tooling with size/digest checks.
   Preserve the verified manifest even if archiving hits a limit. Never label
   a partially written archive complete or automatically delete raw data.

Absolute checkout paths can affect preprocessed line directives. Keep a fixed
container mount path and record it; do not quietly normalize source text to
improve compression. A second machine must reproduce the action inventory
and explain any byte differences before claiming bitwise corpus reproducibility.
Keep full input provenance locally; do not upload the corpus to GitHub.

For warm tests first use A→A explicitly labelled identical replay. An edited
A→B corpus requires a second pinned revision/config/build and generic paired
manifest matching; existing Firefox-specific paired handling is not proof
that Chromium edits are covered. Capture B only after budgeting its additional
storage. Compression comparisons use the same complete ordered bytes and
include framing/manifest overhead consistently; no subset-vs-full comparison.

### 10.5 Corpus acceptance tests and execution gate

Unit tests use tiny fake compile databases and response files: generated
sources, duplicate actions, C/C++ separation, command preservation, unsupported
module/PCH, failed preprocess, changed pin/tool/input, truncated output, stale
temporary file, interrupted rename/journal, concurrent invocation and cap
failure. Assert no incomplete corpus can produce a successful full manifest.
Exercise archive and manifest-driver integration with a tiny real corpus.

Then run one real pinned Chromium TU as a tooling smoke, clearly not the full
corpus. Only after preflight, smoke and cap/resume tests pass, start the full
target build/capture in a Luna execution lane. Report phase, completed actions,
bytes, free reserve, peak memory, elapsed time and any stalled command. Full
capture completes only at exact inventory coverage; hardware exhaustion is an
honest INCOMPLETE result with required additional capacity, not a reason to
declare a sample the requested full corpus.
