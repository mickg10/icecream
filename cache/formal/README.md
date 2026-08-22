# Protocol-50 formal lane

This directory is the canonical formal home for Protocol 50 because it lives beside the product identities, executable state machine, action trace, and trace checker.

There are five small models with different ownership boundaries:

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
```

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

“Authorized” is now an executable invariant, not only prose. It means the compiler owns an independent, complete, immutable input source. A compiler still waiting for bytes through a sidecar-owned pipe is not restart-independent. A direct mutant authorizes without ownership and must violate `AuthorizedCompilerOwnsIndependentInput`.

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

The product topology for this effort is one shared C-side authority/GUID used by all local C producer processes. The formal cache model therefore has one C namespace and independent F replicas. A separate “two unrelated C GUIDs assign the same Key64 differently” model is not part of the current product contract. Executable namespace-isolation tests may remain as robustness tests.

Local Prepare must be idempotent across a lost local reply. A producer-session/request token binds the raw length and digest: retrying the same token and identity returns the same PreparedTU/TU_SEQ; reusing the token for different input is rejected. This is local request replay, not a new remote P50 message.

## Implementation correspondence

Current M1 traces cover the implemented subset. Future mutating boundaries should use these names or an explicitly documented one-to-one mapping:

```text
SESSION_OPENED
SESSION_REPLACED
SESSION_DISCONNECTED
HISTORY_RESET
TX_BEGIN
TX_ABORTED
DICT_COMPLETE
NEED_RECORDED
BODY_COMPLETE
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

## Running TLC

Use a pinned TLA+ tools version and record the exact version and jar hash. The canonical command is:

```sh
TLA2TOOLS_JAR=/path/to/tla2tools.jar \
TLC_WORKERS=1 \
make protocol50-formal
```

`run_tlc.sh` prints the jar/module/config hashes and exact Java command for every row. It requires complete safety runs for the cache, job, reconnect, multi-route, and incarnation models plus both scoped progress rows. It also requires directly named invariant failure for:

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

The formal lane does not model compression bytes, frame parsing, preprocessing time, scheduler policy, bandwidth, compiler-environment transfer internals, P29 grammar construction, or GRZ parsing. Those remain parser, codec, simulator, and farm-test responsibilities.
