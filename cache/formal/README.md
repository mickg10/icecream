# Protocol-50 formal lane

This directory is the canonical formal home for Protocol 50 because it lives beside the product identities, executable state machine, action trace, and trace checker.

There are three small models with different ownership boundaries:

```text
Protocol50.tla
    cache transaction, immutable objects, exact Need, transaction pins,
    eviction, current-session/current-operation fencing, replay,
    and durable-commit reconciliation

Protocol50JobLifecycle.tla
    retained exact input, compiler/job restart, cache-versus-legacy attempts,
    cache-sidecar restart, compiler authorization, and one-result arbitration

Protocol50IncarnationBridge.tla
    verified F_STORE_GUID replacement while an old transaction is in flight
    or after a durable but unaccepted commit, preservation of C retry identity,
    cold-route/history-independent replay, and the job effects of replacement
```

The boundaries are:

```text
Protocol50 --INPUT_COMMITTED--> Protocol50JobLifecycle

Protocol50 + Protocol50JobLifecycle
    --verified F_STORE_GUID replacement before acceptance-->
Protocol50IncarnationBridge
```

This is one protocol design, not competing architectures. The split keeps the already-large cache state space from absorbing compiler-attempt and incarnation-recovery state.

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

## History-nonce freshness

A production `HISTORY_NONCE` is not merely “different from the current value.” It is a monotonic, non-reused u64 within one `(C_STORE_GUID, F_STORE_GUID)` incarnation. A route reset with a reused or lower nonce is rejected. Exhaustion requires an identity-incarnation change rather than wrap.

This closes a same-session ABA case that operation digests alone cannot distinguish when an identical TU is encoded identically after an old history branch is recreated:

```text
branch N runs transaction X
branch changes away from N
N is reused in the same C/F incarnation
identical X is created at REL_SEQ 0
late callback from old branch N aliases new X
```

The large cache model intentionally keeps `Nonces == 0..1` as an old/new-branch abstraction and does not attempt unbounded nonce allocation. M2/product gates must enforce the concrete monotonic rule and test:

- equal nonce rejected;
- lower/previously used nonce rejected;
- next higher nonce accepted only outside an active transaction;
- C_GUID or F_STORE_GUID replacement starts a new nonce domain.

A history reset should be the first mutating route-control operation after session reconciliation. Further reset need closes the session and re-enters reconciliation rather than repeatedly recycling branch identity in place.

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

“Authorized” means the compiler owns an independent complete input source. A compiler still waiting for bytes through a sidecar-owned pipe is not restart-independent.

## F-incarnation bridge

Ordinary abort is forbidden after F durably commits and before C accepts. Verified F-store replacement is the explicit exception. The same cold-retry rule also applies when the old transaction was still in flight and had not committed:

```text
F proves a new F_STORE_GUID
any old partial overlay or durable-but-unaccepted commit is unreachable
C retains the immutable PreparedTU/retry identity
waiting P50 attachment is cancelled
already-authorized compiler, when one exists, remains valid
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

F_STORE_INCAR_REPLACED
ESTABLISH_COLD_ROUTE
REISSUE_HISTORY_INDEPENDENT
COMMIT_RETRY
ACCEPT_RETRY_COMMIT
```

A large implementation trace need not contain every modeled action, but every mutating implementation event must refine one modeled transition.

The C++ and Python trace checkers must reject:

- abort while F still owns a pending overlay;
- abort after durable F commit and before normal/lost acceptance;
- F mutation from a stale session serial;
- current-session callback carrying an earlier operation identity, including a different transaction digest at the same route cursor;
- reused or non-increasing `HISTORY_NONCE` in one C/F incarnation;
- begin at exhausted `REL_SEQ`.

## Running TLC

Use a pinned TLA+ tools version and record the exact version and jar hash. The canonical command is:

```sh
TLA2TOOLS_JAR=/path/to/tla2tools.jar \
TLC_WORKERS=1 \
make protocol50-formal
```

`run_tlc.sh` requires:

- the cache safety model to pass;
- the job safety model to pass;
- incarnation safety and scoped recovery progress to pass;
- abort-after-commit, terminal-REL_SEQ, missing-input-lease, and lost-retry-identity mutants to fail through their named discriminating invariants.

There is intentionally no hosted GitHub Actions gate. The cache model has previously required about 8 GiB and roughly 24 minutes on one reviewer host. Acceptance therefore requires local runs from the exact PR head, published generated/distinct/depth/runtime statistics, log hashes, and independent reproduction.

No TLC success is claimed for a changed model until those exact-head runs are posted.

## Deliberate exclusions

The formal lane does not model compression bytes, frame parsing, preprocessing time, scheduler policy, bandwidth, compiler-environment transfer internals, P29 grammar construction, or GRZ parsing. Those remain parser, codec, simulator, and farm-test responsibilities.
