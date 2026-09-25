# Protocol-50 formal lane

Use `make protocol50-formal` from the repository root with `ICEFARM_TMPDIR`
set. It uses the shared `uv sync --locked` environment. For direct commands
below, enter it with `sh dev/python.sh --exec bash` from the repository root;
Java and the pinned TLC jar remain separate prerequisites.

This directory is the canonical formal home for Protocol 50 because it lives beside the product identities, executable state machine, action trace, and trace checker.

The retained [S6 transplant record](S6_V6_TRANSPLANT_AUTHORITY.md) is historical
evidence, not a current completion claim. Its table's 40-hex blob identifiers
are Git SHA-1 object IDs despite the original `SHA-256` column label. Historical
authority records are preserved byte-for-byte; current execution scope is below.

The core cache lane has five small models with different ownership boundaries;
the aggregate runner additionally includes assignment, global-trace, and
portable ZSTD_ROUTE/FInput lanes described below:

```text
Protocol50.tla
    cache transaction, immutable objects, exact Need, transaction pins,
    eviction, current-session/current-operation fencing, replay,
    and durable-commit reconciliation

Protocol50JobLifecycle.tla
    retained exact input, compiler/job restart, cache-versus-legacy attempts,
    cache-sidecar restart, compiler authorization, and one-result arbitration

Protocol50Reconnect.tla
    fail-closed classification of initial cold state, exact replay,
    lost-final-ack reconciliation, F-store replacement, route mismatch,
    same-GUID namespace loss, and the one-reset-per-session boundary

Protocol50MultiRoute.tla
    the same PreparedTU attempted on two independent F relationships,
    preservation of the source route's pending/durable liability,
    and separation of result acceptance from cache-route reconciliation

Protocol50IncarnationBridge.tla
    verified F_STORE_GUID replacement while an old transaction is in flight
    or after a durable but unaccepted commit, preservation of C retry identity,
    independently owned compiler input, cold replay, and scoped progress

Protocol50TransferConcurrency.tla
    bounded per-(C,F-generation) admission and owner publication, independent
    link overlap/progress, per-C/per-F count and byte credits, unique C-wide
    TU allocation, timeout/reconciliation, generation replacement, and
    stale-callback fencing

Protocol50PipelineRecovery.tla
    bounded W2 ordered speculative-send/commit/receipt/ACK cursors, durable
    receipt reconciliation, reset confirmation, per-job raw reservations,
    link/history/relationship callback fencing, and cancellation suffix rebuild

Protocol50ReplacementReplay.tla
    lost RESET_CONFIRM followed by same-operation RESET replay, terminal
    same-F logical-relationship replacement/use, explicit F-store restart
    distinction, stale-old-relationship rejection, and unaffected-link progress

Protocol50PipelineWindowAccounting.tla
    separate W30 accounting projection for raw, encoded, receipt, and
    speculative-history byte budgets; not a codec or product-pipeline proof
```

## Bounded transfer-concurrency lane

The `transfer-concurrency` lane is deliberately separate from the legacy
formal rows and is selected by the existing `make protocol50-formal`
aggregator. Its six topology configurations cover C2F1/C3F1/C4F1 and
C1F2/C1F3/C1F4. Each has a clean safety run and a separate reachability
witness that succeeds only when every distinct `(C,F)` relationship is in
`Running` simultaneously. A blocked-running-link progress row, byte-pressure
row, generation-replacement row, and six named negative controls exercise the
independent-link and stale-identity failure modes. Expected negative outcomes
are accepted only when the runner sees their exact invariant/temporal
diagnostic; parse failures, timeouts, and generic nonzero exits are failures.

The model treats profile as a route selector, not as part of the relationship
identity. It represents admission credits as count and abstract raw-byte
units, with both C-owned and F-owned limits; it does not model codec internals.
The retained lost-ack witness and physical reservation are one bounded
reconciliation abstraction here, so this is not a claim that production must
hold physical bytes for the entire ambiguity window. C/F generation
replacement, callbacks, timeout and cancellation are finite abstractions, not
an unbounded restart proof. Ordinary `CACHE_SESSION`/ready and namespace
activation at `SESSION_HELLO`, blocking DNS, OS shutdown, and the full
compressed-codec lifecycle remain outside this model. In particular, the
lane proves Stage-A `W=1` reservation/owner behavior only; it does not prove
the future ordered `W>1` pipeline or a `30 TU` depth.
The finite sequence allocator starts at 1 because 0 is its unallocated
sentinel; this is an offset abstraction of production `TU_SEQ`, which starts
at 0. Only C-wide uniqueness and monotonic allocation are claimed.

The row runner is `run_transfer_concurrency_tlc.sh`; it uses the pinned jar
and writes distinct TLC metadata under caller-provided `TLC_STATE_ROOT`.
The aggregate result bundles preserve the actual jar/module/config hashes and
per-row logs for audit.

## Bounded C/D ordered-pipeline and recovery increment

`Protocol50PipelineRecovery.tla` is a later, separate abstraction; it does not
replace or upgrade the Stage-A `W=1` results above and is not selected by the
existing `make protocol50-formal` aggregate. Run it with
`run_pipeline_recovery_tlc.sh` and a fresh absolute, writable, nonsymlink
`TLC_STATE_ROOT` (the retained run here uses `/tanksmall/scratch/tmp`).
Its clean W2 rows cover C2F1/C3F1/C4F1 and C1F2/C1F3/C1F4, with two jobs on
the target relationship and one on each other relationship. Separate
reachability controls demonstrate a fully sent target-window saturation and
another-link publication in both topology directions. A separate three-job
W2 witness demonstrates reuse of one released window credit. A one-link fault
row bounds lost commit replies, recovery receipt delivery, sequential reset
and retry transitions, cancellation of an unsent suffix, and stale worker
fencing. It does not model replay requests for an earlier reset result or a
lost RESET_CONFIRM. Named mutants include missing
receipts, a wrong-job receipt identity, an ACK beyond F's published prefix,
stale worker publication, non-idempotent reset retry, and a cancellation hole.
Each must produce its exact invariant failure; parse errors, timeouts, and
generic nonzero exits are not accepted as controls.

The model separates relationship incarnation, physical link generation,
codec/history epoch, the C-verified receipt floor `A`, F-published input floor
`K`, C-staged ordinal `P`, transmitted cumulative ACK floor, and the `Q` that
advances only when F processes that ACK. Receipts are bounded by the finite
ordinal set and tied to the `(C,F,job)` ordinal map. Worker tokens carry link,
relationship, F-store and history generations. Raw reservations are charged
to C before link open and retained through unresolved commit; per-link
`P-A`/`K-Q` bounds express the configured speculative and receipt windows.
Each F receipt and recovery reply carries an immutable ordinal/job/relationship
epoch/F-generation/history-epoch witness. The model checks preservation of the
ordinal/job mapping; the wrong-job mutant corrupts the recovered record and is
detected by `RecoveryResponseIdentityExact`. It does not model a defensive
wire decoder rejecting malformed recovered identities, nor full epoch/digest
validation at the receiving implementation boundary. C sends from its local
staged/sent floors and does not consult F's `Q`; F alone enforces its
receipt-capacity floor. The fault row permits two bounded disconnect/reset
cycles and a terminal relationship retirement; it does not model creation/use
of a replacement relationship incarnation or unbounded reconnects. Reset
operation identity/count remains in the bounded state, but prior-result replay
semantics and a lost `RESET_CONFIRM` are covered by the separate focused model
below.

`Protocol50ReplacementReplay.tla` is an auxiliary, focused single-link
lifecycle model with abstract sibling-progress tokens rather than a full
multi-link concurrency model or an enlargement of the W2 transfer state
space. The six topology rows vary the peer set and demonstrate sibling
reachability; the existing `Protocol50PipelineRecovery.tla` remains the
multi-relationship concurrency/recovery model. The auxiliary model captures the lost-confirm
sequence explicitly: F applies RESET and returns a result, C's first confirm
is lost, a new physical connection replays the same reset operation, F returns
the cached result without advancing history again, and the second confirm is
processed. It then retires the old logical relationship on the same F store,
creates and uses a strictly new logical identity, and rejects an offer for the
old identity. An F-store restart is modeled as a distinct transition that
advances the store generation before a replacement can be used; same-F
relationship retirement must leave that generation unchanged. Other links in
the configured topology have explicit progress steps. Six safety rows and six
separate finite reachability-witness rows cover C2F1/C3F1/C4F1 and
C1F2/C1F3/C1F4. Three mutants must violate the exact reset-result, stale-offer,
or same-F/store-generation invariant. Run `run_pipeline_replacement_tlc.sh`
with the pinned TLC jar and a fresh `TLC_STATE_ROOT`.

These are bounded state-space checks and finite reachability traces, not a
fairness-based liveness proof. The sibling links are abstract unaffected
progress tokens, not complete concurrent codec pipelines. The model does not
establish unbounded reset/reconnect behavior, wire framing, or product
refinement.

Run both pipeline-level bounded lanes together with
`make protocol50-pipeline-formal` (set `TLA2TOOLS_JAR` and a fresh absolute
`TLC_STATE_ROOT`). This target is intentionally separate from the much larger
`make protocol50-formal` aggregate.

W30 uses a second, intentionally accounting-only model: it checks the cursor
and symbolic raw/encoded/receipt/speculative-journal caps at `W=30`. Its fixed
per-slot byte quantities are explicit assumptions, not measured codec output
or actual dictionary snapshots; history accounting represents metadata for
one working codec state, not cloned entropy snapshots per TU. The W2 model omits profile-specific codec
transitions, grammar/key assignment, parallel codec-context cloning, real job
or TU allocation, wire framing, and implementation refinement. In
particular, it does not qualify a product W2/W30 pipeline or prove production
memory sizes. The profiles P29V1, ZSTD_TU and ZSTD_ROUTE share the abstract
ordered-commit skeleton here; codec correctness remains with their own models
and tests.

The boundaries are:

```text
SESSION_STATE/reconnect report
    -> Protocol50Reconnect
    -> replay, witnessed lost-ack acceptance, verified cold replacement,
       idle reset, or terminal protocol inconsistency

Protocol50 --INPUT_COMMITTED--> Protocol50JobLifecycle

scheduler replacement to another independent F
    -> Protocol50MultiRoute
    -> fork the same PreparedTU onto a second CRoute
       without retiring the first route's liability

Protocol50 + Protocol50JobLifecycle
    --verified F_STORE_GUID replacement before acceptance-->
Protocol50IncarnationBridge
```

This is one protocol design, not competing architectures. The split keeps the already-large cache state space from absorbing compiler-attempt, reconnect-classification, and multi-route ownership state.

The older experimental model under `formal/protocol50/` on the capability branch is withdrawn. It contained the separate `ComputeNeed`/`PinClosure` race and did not implement the callback/restart semantics its review claimed.

## Cache-model rules

The cache model includes:

- one C-side active operation and one F-side pending overlay;
- two possible F destinations, exercised one relationship at a time;
- immutable `Key64 -> content` binding;
- atomic Need computation that pins every already-present required object;
- immediate pinning of each requested object when it is completely installed;
- eviction only when an object is not transaction-pinned;
- complete overlay/pin release on disconnect or session replacement;
- history reset only when C and F are idle and no durable commit awaits C;
- whole-current-TU replay;
- an F-durable/C-unacknowledged commit witness;
- rejection of `TX_ABORTED` in that durable-commit window;
- rejection of `TX_BEGIN` at terminal bounded `REL_SEQ`;
- session-token fencing and same-session operation identity
  `(F, HISTORY_NONCE, REL_SEQ, TU, TX_DIGEST_VARIANT)` for delayed callbacks.

The production transaction digest binds profile, component descriptors, raw digest, and route pre-state. The bounded model gives only one cursor/TU tuple a second digest variant. That is enough to exercise the same-session ABA case:

```text
transaction A starts at one route cursor
A is explicitly aborted before durable commit
transaction B re-encodes the same TU at the same cursor
late callback from A arrives while B is pending
```

The callback must be rejected because its digest identity differs, even though F, nonce, `REL_SEQ`, and `TU_SEQ` match. Limiting the second variant to one tuple avoids doubling the full state space.

The atomic Need/pin rule prevents:

```text
Need observes object X present
X is omitted from Need
X is evicted before materialization
no Fill for X exists
```

## Reconnect decision table

Reconnect is a decision boundary, not generic “reset on mismatch” logic:

```text
never established + namespace absent:
    initial cold establishment

same F_STORE_GUID + exact route cursor:
    continue or replay the retained whole transaction

same F_STORE_GUID + exact retained last commit one REL_SEQ ahead:
    accept the witnessed lost final acknowledgement

changed F_STORE_GUID:
    verified destructive replacement; preserve PreparedTU and cold-reissue

established + same F_STORE_GUID + namespace absent:
    terminal protocol inconsistency; do not retire unresolved C state

same-incarnation mismatch while C retains active or durable work:
    terminal/reconnect; do not reset over the unresolved transaction

idle same-incarnation mismatch:
    one HISTORY_RESET may establish a fresh branch
```

`Protocol50Reconnect.tla` checks that cold retirement has GUID-change or initial-cold proof, that unresolved active work is not discarded, and that one reconciled session performs at most one reset. Mutants independently enable each unsafe shortcut.

The practical rule is deliberately small: if a second reset seems necessary in one live session, close it and re-enter reconciliation. No history-repair log or frame-resume protocol is required.

## History-nonce and session freshness

A production `HISTORY_NONCE` is not merely “different from the current value.” It is a monotonic, non-reused u64 within one `(C_STORE_GUID, F_STORE_GUID)` incarnation. A route reset with a reused or lower nonce is rejected. Exhaustion requires an identity-incarnation change rather than wrap.

Likewise, an F session serial is nonzero and strictly increasing within one F-store incarnation. Every asynchronous F mutation revalidates the current session serial and the complete operation identity before touching state.

This closes two ABA cases:

```text
branch N runs transaction X
branch changes away from N
N is reused in the same C/F incarnation
identical X is created at REL_SEQ 0
late callback from old branch N aliases new X
```

```text
session S owns operation X
session S2 replaces S
late mutation from S arrives with an otherwise matching transaction identity
```

The large cache model intentionally keeps `Nonces == 0..1` as an old/new-branch abstraction. Its `HISTORY_RESET` action composes with the reconnect model's one-reset-per-session gate; the core model alone is not an unbounded nonce-allocation proof. Product and trace gates enforce the concrete monotonic rule.

## Job-model rules

The job model includes:

- `INPUT_COMMITTED` acquiring the logical-job restart lease before any attachment;
- exact P50 input retained while its logical job remains open;
- `ATTEMPT_ID` outside cache transaction identity;
- replacement attachment without retransmission or a second history commit;
- P50 and legacy as whole-attempt modes, never spliced;
- monotonic compiler authorization after exact input ownership is handed off;
- a cache-sidecar restart cancelling waiting P50 attempts but not an already-authorized compiler;
- legacy attempts unaffected by a cache-only restart;
- late losing results observable but unable to win;
- exactly one accepted result for the logical job.

“Authorized” is now an executable invariant, not only prose. It means the compiler owns an independent, complete, immutable input source. A compiler still waiting for bytes through a sidecar-owned pipe is not restart-independent. A direct mutant authorizes without ownership and must violate `AuthorizedAttemptOwnsIndependentInput`.

A valid compiler restart therefore remains simple:

```text
attempt A dies before or after compiler authorization
logical job remains open
replacement attempt B receives a new independent read cursor
B reads the same retained InputRecord
no retransmission and no second cache-history commit
only one result may be accepted
```

## Multi-route bridge

A scheduler replacement on another F is not an F-store incarnation change. The route map is keyed by physical endpoint/lane identity; the `F_STORE_GUID` is the incarnation token returned by that endpoint.

```text
PreparedTU T
    -> CRoute(F0), possibly active or durable/unacknowledged
    -> CRoute(F1), a new independent transaction for the same TU_SEQ
```

Starting F1 must not mutate F0's cursor, matcher, transaction, or reconciliation witness. A compiler result from either F may win the logical job, but result acceptance does not resolve either cache relationship. Each route is cleared only by its own normal/lost commit acceptance, explicit pre-durable abort, or verified same-endpoint incarnation replacement.

`Protocol50MultiRoute.tla` checks that every started route remains bound to the same immutable PreparedTU, every unresolved route retains exactly one liability, retirement has incarnation proof, and at most one result is accepted. Two direct mutants deliberately:

```text
move the source CRoute to F1 instead of forking an F1 CRoute
accept a compiler result and clear all outstanding cache liabilities
```

Both must violate their named invariants. This bridge models ownership only; it does not model scheduler policy, queueing, or bandwidth.

## F-incarnation bridge

Ordinary abort is forbidden after F durably commits and before C accepts. Verified F-store replacement is the explicit exception. The same cold-retry rule also applies when the old transaction was still in flight and had not committed:

```text
F proves a new F_STORE_GUID
any old partial overlay or durable-but-unaccepted commit is unreachable
C retains the immutable PreparedTU/retry identity
waiting P50 attachment is cancelled
already-authorized compiler survives only if it owns exact input independently
new cold route is established
same PreparedTU is reissued history-independently
new commit is accepted
```

The bridge model checks both pre-durable and post-durable replacement timing and includes a scoped temporal property:

```text
under a stable new incarnation and weak fairness of
route establishment, reissue, commit, and acceptance,
the retained retry is eventually accepted
```

It does **not** claim unconditional liveness under infinitely recurring failures.

## Bounded session-token scope

`Protocol50.tla` uses two non-reused session tokens. This is intentional: the bound represents an original session and one replacement so stale-vs-current callback fencing can be explored. Token exhaustion is not a product state or an unbounded reconnect proof. Production session serials are u64 and remain non-reused within an `F_STORE_GUID` incarnation.

## One shared C namespace

The legacy `Protocol50.tla` cache-state model has one C namespace and independent F replicas; it does not model profile codec/key assignment. The separate bounded `Protocol50TransferConcurrency.tla` lane now checks both multi-C/one-F and one-C/multi-F admission topologies, including C-wide TU uniqueness, without modeling codec internals or claiming cross-C key-namespace equivalence.

Local Prepare must be idempotent across a lost local reply. A producer-session/request token binds the raw length and digest: retrying the same token and identity returns the same PreparedTU/TU_SEQ; reusing the token for different input is rejected. This is local request replay, not a new remote P50 message.

## Implementation correspondence

The current action traces cover the implemented subset and provide names for
correspondence checks; they are not a claim of full implementation refinement.
Future mutating boundaries should use these names or an explicitly documented
one-to-one mapping:

```text
SESSION_OPENED
SESSION_REPLACED
SESSION_DISCONNECTED
HISTORY_RESET
TX_BEGIN
TX_ABORTED
BODY_COMPLETE
NEED_RECORDED
OBJECT_APPLIED
INPUT_MATERIALIZED
INPUT_COMMITTED
COMMIT_ACCEPTED
ACTIVE_REPLAYED
LOST_COMMIT_ACCEPTED
EVICT_OBJECT
STALE_SESSION_CALLBACK
STALE_OPERATION_CALLBACK

COMMITTED_INPUT_RETAINED
ATTEMPT_STARTED
ENVIRONMENT_READY
LEGACY_INPUT_READY
COMPILER_AUTHORIZED
COMPILER_FINISHED
ATTEMPT_CANCELLED
LATE_RESULT
RESULT_ACCEPTED
JOB_CANCELLED
COMMITTED_INPUT_EVICTED
F_CACHE_RESTARTED

ROUTE_FORKED
F_STORE_INCAR_REPLACED
ESTABLISH_COLD_ROUTE
REISSUE_HISTORY_INDEPENDENT
COMMIT_RETRY
ACCEPT_RETRY_COMMIT
```

A large implementation trace need not contain every modeled action, but every mutating implementation event must refine one modeled transition.

The executable trace gates reject:

- abort while F still owns a pending overlay;
- abort after durable F commit and before normal/lost acceptance;
- F mutation from a stale session serial;
- reused or non-increasing session serials;
- a second `HISTORY_RESET` in one session;
- current-session callback carrying an earlier operation identity, including a different transaction digest at the same route cursor;
- reused or non-increasing `HISTORY_NONCE` in one C/F incarnation;
- begin or commit at exhausted `REL_SEQ`;
- loss of the durable-commit reconciliation witness.

## P50 assignment identity through the client

`Protocol50AssignmentIdentity.tla` refines the accepted assignment and
delivery models across the existing S -> C -> F claimant path. It treats
`job_id` as the established wire id, carries epoch and nonce only when every
corresponding hop is P50, rejects partial identities, and checks the explicit
P43/P50 compatibility matrix, StrictNonce promise, local exemption,
reconnect, revoke, and ordinary-settlement suffix. See
`Protocol50AssignmentIdentity.md` and run its focused fail-closed matrix with
`run_assignment_identity_tlc.sh`.

## Running TLC

Use the pinned TLA+ tools artifact whose SHA-256 is declared in
`formal_aggregate_manifest.json`.  The canonical, source-release-portable
command is:

```sh
ICEFARM_TMPDIR=/absolute/path/to/scratch \
TLA2TOOLS_JAR=/path/to/tla2tools.jar \
make protocol50-formal
```

The target runs the core, assignment-delivery, assignment-identity, global,
bounded transfer-concurrency, and bounded ZSTD_ROUTE/FInput lanes selected by
that manifest.  It allocates a
unique retained directory below `P50_FORMAL_RESULTS_ROOT` (or
`$ICEFARM_TMPDIR/icecream-formal`), authenticates the shared jar before any
lane starts, hashes every selected input, and records commands, logs, state
counts, and a machine-readable aggregate verdict.  Missing dependencies,
timeouts, missing row markers, and `SKIP` are never PASS.

The authenticated manifest defaults the aggregate to eight TLC workers.  The
selected V6 composition graph does not drain inside its 300-second row bound
with one worker, but completes at depth 87 with eight.  `TLC_WORKERS` remains
an explicit override for constrained or larger hosts, and its exact value is
retained in the evidence bundle; too little execution capacity fails closed.

The ZSTD_ROUTE/FInput selection uses the packaged portable execution
authority and does not claim the complete 87-row historical matrix.  Direct
historical review still invokes `run_zstd_route_finput_composition_tlc.sh`
without `S6_PORTABLE_EXECUTION=1`, retaining its correction-spec, Git
ancestry, single-parent, and clean-worktree gates.

The core `run_tlc.sh` lane prints the jar/module/config hashes and exact Java
command for every row. It requires complete safety runs for the cache, job,
reconnect, multi-route, and incarnation models plus both scoped progress rows.
It also requires directly named invariant failure for:

- abort after durable commit;
- begin at terminal `REL_SEQ`;
- same-cursor callback with the wrong transaction digest;
- committed input without its logical-job lease;
- attempt cancellation releasing that lease while the job remains open;
- compiler authorization without independent input ownership;
- same-GUID namespace loss incorrectly treated as cold replacement;
- route reset that discards unresolved active work;
- a second history reset in one session;
- scheduler replacement moving rather than forking an independent CRoute;
- result acceptance discarding unresolved cache liabilities;
- lost retry identity on F-store replacement;
- compiler authorization without independent exact-input ownership after replacement.

There is intentionally no hosted GitHub Actions gate. The cache model has previously required about 8 GiB and roughly 24 minutes on one reviewer host. Acceptance therefore requires local runs from the exact PR head, published generated/distinct/depth/runtime statistics, log hashes, and independent reproduction.

No TLC success is claimed for a changed model until those exact-head runs are posted.

## Deliberate exclusions

The formal lane does not model compression bytes, frame parsing, preprocessing
time, scheduler policy, bandwidth, compiler-environment transfer internals, or
P29V1 grammar construction. Those remain parser, codec, simulator, and
farm-test responsibilities.
