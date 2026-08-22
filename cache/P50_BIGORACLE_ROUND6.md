# Protocol-50 BigOracle Round 6 review

## Verdict

```text
B0 C++23 / Boost baseline:              ACCEPT
M0 identities / framing / negotiation:  ACCEPT
M1 in-memory P29 transaction:           ACCEPT WITH ONE CODE FIX
Remote sidecar/socket slice:             UNBLOCK AFTER THAT FIX + BOUNDS GATE
Eviction implementation:                NOT REQUIRED FOR M1; formal rule frozen
Job restart/result arbitration:         FORMAL CONTRACT READY; implement with attachment slice
```

The design is now small enough to implement. Do not add another transport abstraction, frame-resume protocol, acknowledgement window, or history-repair log.

## Immediate M1 defect: incomplete trailing FILL can be ignored

`FStore::append_fill()` may apply the final requested object while `FillStreamDecoder` retains a partial trailing record. `materialize_and_verify()` checks `remaining.empty()` but does not require the FILL parser buffer to be empty. A transaction can therefore commit while silently ignoring malformed trailing FILL bytes unless its caller separately invokes `finish_fill()`.

Required correction:

```cpp
std::vector<ObjectApplied> FStore::append_fill(...)
{
    ...
    for (const FillRecord& record : records)
        result.push_back(apply_object(...));

    if (pending.remaining.empty())
        pending.partial_fill.finish();

    return result;
}

std::vector<uint8_t> FStore::materialize_and_verify(...)
{
    pending.partial_fill.finish(); // defensive binding gate
    ...
}
```

Regression:

```text
one FILL frame = final requested complete object
               + truncated prefix of another object record

expected:
    preceding complete object remains installed
    transaction cannot materialize or commit
    replay recomputes Need from actual F state
```

No wire or formal-state change is needed; this is parser closure at the executable boundary.

## Before remote sockets: enforce local transaction caps

The current negotiated limits bound one frame and one FILL record, but `TX_BEGIN` can declare unbounded DICT, BODY, decoded item counts, and raw output. Before accepting remote bytes, F must enforce configured local caps for:

```text
DICT encoded bytes
BODY encoded bytes
DICT decoded item count
BODY decoded item count
raw/materialized bytes
pending bytes per C namespace
prepared/retained input bytes and record count
```

These do not all need new V1 wire fields. F may reject an over-cap `TX_BEGIN` with terminal `ERROR`; the job path then starts a new legacy attempt. Keep the first protocol small.

## Atomic Need/pin rule for the future eviction slice

Need computation and pin acquisition are one namespace-owner transition:

```text
for every required object:
    PRESENT -> pin now
    ABSENT  -> record in exact Need

on complete requested object application:
    validate -> publish -> pin -> clear missing
```

Disconnect, abort, or session replacement releases every transaction pin exactly once. Eviction may remove only unpinned objects. This prevents an object observed present during Need from being evicted before materialization when no Fill for it exists.

M1 has no eviction, so do not manufacture a cache policy merely to satisfy this future rule. Add one synthetic interleaving test when pin/evict lands.

## Session callback fencing

Every asynchronous mutation must carry and revalidate:

```text
F_STORE_GUID
session token/serial
C_STORE_GUID
HISTORY_NONCE
REL_SEQ
transaction digest where applicable
operation kind
```

This includes close callbacks, TX_COMMIT delivery, HISTORY_RESET completion, object decode, and materialization-worker completion. An old callback may be observed/logged but cannot mutate a replacement session.

## Job restart contract

Cache identity and compiler-attempt identity remain separate:

```text
cache input:
    (C_STORE_GUID, TU_SEQ) at one F

compiler attempt:
    logical job + ATTEMPT_ID + whole input mode
```

`ATTEMPT_ID` is not part of `TX_BEGIN`, object identity, transaction digest, or route history.

`INPUT_COMMITTED` publishes a retained exact input record with independent read cursors. A replacement compiler attempt on the same F reuses it without retransmission or a second history commit. A retry on another F is a new relationship transaction for the same immutable PreparedTU.

The retained-input restart lease lasts until one result is accepted or the logical job is definitively cancelled. This is simpler and safer than out-of-transaction dependency repair.

### Cache-sidecar restart

```text
waiting P50 attachment:
    fails; replacement attempt may be scheduled after re-commit

already-authorized compiler:
    may finish because it owns exact input independently

legacy attempt:
    unaffected by a cache-only restart
```

“Authorized” must mean the compiler owns a complete immutable input source. A compiler still receiving a sidecar-owned pipe is not restart-independent.

Only one result may be accepted. Late losing results are discarded after being accounted.

## Formal suite

Canonical location:

```text
cache/formal/Protocol50.tla
cache/formal/Protocol50JobLifecycle.tla
```

The cache model covers atomic Need/pin, object application, eviction, session-token fencing, replay, and the lost-final-commit window. The job model begins at `INPUT_COMMITTED` and covers retained input, replacement attempts, cache-only F restart, legacy overlap, and result uniqueness.

The older `formal/protocol50/` capability-branch model is withdrawn; it still contained the separate Need/pin race and did not implement the callback/restart semantics its review comment claimed.

A retained TLC run is required before merging the formal correction branch. Publishing a TLA file is not itself verification.

## Edge-case gates by slice

### Immediate M1

- final requested object plus partial trailing FILL record;
- FILL parser failure after earlier complete objects;
- same key/same bytes duplicate and same key/different bytes failure;
- exact Need unchanged by duplicate applications;
- history reset cannot occur while C retains active work;
- stale old session handle cannot complete or close replacement state.

### Remote Zstd calibration slice

- every frame-header split;
- short/excess DICT and BODY;
- zero-length legal components;
- transaction/local memory caps;
- disconnect after every frame boundary;
- old/new peer fallback;
- C2F1 namespace isolation;
- terminal ERROR followed by no accepted frame.

### Attachment/job slice

- job reference before/after INPUT_COMMITTED;
- attempt dies before authorization;
- attempt dies after authorization;
- replacement reads the same retained input from an independent cursor;
- F cache restart with waiting and running attempts;
- cache attempt versus legacy replacement, both result orders;
- only one accepted result;
- logical cancellation releases retained-input lease.

### Eviction slice

- eviction immediately before Need;
- eviction attempting to race atomic Need/pin;
- eviction after pin rejected;
- disconnect releases pins once;
- restart clears incomplete overlay and pins;
- retained complete objects survive failed TU as specified.

## Ordered path

```text
1. Fix trailing-partial-FILL commit and add its regression.
2. Run the existing B0/M0/M1 gates again.
3. Run TLC for both canonical formal models and retain logs/state counts.
4. Begin the dedicated endpoint with P50_ZSTD_TU and local caps.
5. Add retained InputRecord + attempt/result lifecycle.
6. Add real P29.
7. Add reconnect/fallback/eviction.
8. Add GRZ and then farm/simulator convergence.
```

This is implementation work now, not an invitation to reopen the architecture.
