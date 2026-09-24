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
