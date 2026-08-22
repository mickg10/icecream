# Protocol 49 ordered scheduler-to-worker assignments

Protocol 49 adds a disabled-by-default ordered assignment contract on the
persistent scheduler-to-worker connection.  It closes the
dispatched-but-not-claimed ownership gap without changing the client-facing
assignment shape. Protocol 50 is the separate successor which carries the
same identity through `UseCS` and `CompileFile`; see
`p50-assignment-identity.md`.

## Activation and the four modes

`icecc-scheduler --assignment-fence-mode MODE` freezes one mode for its
scheduler epoch and sends it in `ConfCSMsg`:

- `legacy` / `Legacy` (0) is the default.  It sends no prepare, waits for no
  ready, sends no ordered revoke, and preserves the inherited behavior.
- `advisory` / `Advisory` (1) sends `PREPARE`, but does not wait for `READY`
  before sending the unchanged `UseCS`.  A nonce-less legacy claim that wins
  before `PREPARE` is recorded as claimed and is later bound to the complete
  identity without regressing its phase.
- `enforcing-compat` / `EnforcingCompat` (2) waits for matching `READY` before
  sending `UseCS`.  A nonce-less legacy claim is admitted only through the
  current live prepared wire-id record.  The older
  `--assignment-fence-strict` spelling is an alias for this mode.
- `strict-nonce` / `StrictNonce` (3) is reserved by Protocol 49 and becomes
  operational only on an explicitly selected all-Protocol-50 remote path. A
  daemon whose scheduler link is below 50 refuses it; a Protocol-50 scheduler
  projects Legacy configuration to that old peer and excludes it from strict
  remote assignments.

Local assignments remain on the existing `NoCS`/local path.  A remote choice
whose worker link negotiated protocol 48 also uses the exact legacy path,
regardless of the scheduler's configured protocol-49 mode.  Protocol-48
`ConfCS` and `UseCS` bytes are unchanged.

## Identity and wire vocabulary

Every protocol-49 scheduler-worker control names the complete tuple

```text
(scheduler_epoch: u64, wire_id: u32, assignment_nonce: u64)
```

Epoch and nonce are compared only for equality.  Zero epoch and zero nonce are
reserved and rejected.  The scheduler generates one full-width random u64
epoch for its process and never changes it during that process.  Epoch
uniqueness is therefore probabilistic, not a claim that random collision is
impossible.  Nonces are allocated monotonically without reuse in the epoch;
exhaustion is terminal rather than wrapping.  Each u64 is encoded as two
big-endian u32 words, high word first.

The four message values are appended after the protocol-48 vocabulary:

| Value | Direction | Payload after the message type |
|---:|---|---|
| `0x49f00000` `ASSIGN_PREPARE` | scheduler to worker | epoch, wire id, nonce, submitter host id, zero flags |
| `0x49f00001` `ASSIGN_READY` | worker to scheduler | epoch, wire id, nonce |
| `0x49f00002` `REVOKE_BEFORE_START` | scheduler to worker | epoch, wire id, nonce |
| `0x49f00003` `REVOKE_RESULT` | worker to scheduler | epoch, wire id, nonce, `Revoked` or `ClaimedOrLater` |

This explicit fork-private numeric block cannot silently acquire the same
values as a future upstream enum append.  All encoders, decoders, and send
sites are negotiated-version gated.  An old worker receives no new control
and incurs no ready wait.

### Resolved design-document deviation

The earlier `P49_P50_DESIGN.md` draft placed the nonce only in `PREPARE` and
abbreviated `READY`, `REVOKE`, and the terminal result.  That shape cannot
distinguish a delayed control after the reusable 32-bit wire id has been
released and allocated again within one scheduler epoch.

This implementation therefore carries the full tuple in all four controls.
That is the identity required by `ASSIGNMENT_FENCE_THEORY.md` and the exact
lookup requirement in `UPSTREAM_LANDING_PLAN.md`, both at immutable evidence
revision `5a2a1a42f5e87ea5cc2497130cf7b8f4d871fc43`.  Protocol 50 is still
distinct because it propagates the tuple through client-facing messages;
protocol 49 uses it only on the scheduler-worker control connection.

The actor-local ordering and destructive-retirement refinement is aligned
with the independently approved formal revision
`8287fd92` (`Protocol50AssignmentOrdering.tla`).  The formal destructive link
loss represents a verified epoch retirement; an ordinary product reconnect
that retains the complete bounded table is a state-preserving rebind.

## Scheduler state machine

For a remote prepared assignment in Advisory, EnforcingCompat, or the
Protocol-50 StrictNonce mode, the
scheduler:

1. selects and freezes the worker, legacy `UseCS` projection, policy, epoch,
   wire id, and nonce;
2. indexes the full identity and queues `PREPARE` on the worker stream;
3. debits the submitter's dispatch credit when that `PREPARE` is accepted by
   the channel;
4. skips that worker for new prepared assignments while its channel has
   deferred output;
5. accepts `READY` only from that worker and only for the exact tuple in
   `Prepared` phase.

Advisory sends the frozen `UseCS` immediately after step 3. EnforcingCompat
and StrictNonce send it only after step 5. A stale, duplicate, wrong-worker, wrong-phase,
zero, or post-reuse `READY` is a bounded no-op.  In particular, a matching
`READY` received after cancellation cannot publish `UseCS`.

A pre-claim submitter cancellation or disconnect queues `REVOKE` on the same
ordered stream as `PREPARE`.  Sending the revoke is not permission to release
the assignment id or worker slot.  An exact `Revoked` result releases both;
`ClaimedOrLater` retains them until the worker's ordinary completion.  The
name means that the worker consumed the claim or advanced beyond that point;
it does not assert that a compiler process has already begun.  Detaching a
submitter discharges its local dispatch credit before its pointer can vanish,
but does not release the separately owned worker assignment.

`JobBegin` is positive claim evidence even when it crosses an outstanding
revoke.  Once observed, a contradictory delayed `Revoked` cannot release the
assignment; the worker's ordinary completion remains the terminal boundary.
Likewise, a READY exposure failure can recursively tear down the current
worker if its ordered revoke also fails, so the handler reports that deletion
to the connection drain instead of returning a live-pointer indication.

Normal `READY` and terminal lookup is expected O(1) through a full-identity
hash index.  The ordinary control handlers do not scan all jobs or daemons.

## Worker state machine

The daemon event-loop thread is the sole owner of prepare, claim, revoke, and
session-loss transitions.  Authorization is resolved before a job is attached
to a client, queued, touches an environment, or can start a compiler.

In EnforcingCompat, `PREPARE` first installs a `Reserved` record and only then
emits `READY`.  A nonce-less client claim consumes only that current prepared
wire-id record.  Unknown claims reject.

In Advisory, an unknown nonce-less claim is allowed and atomically installs a
claimed nonce-zero placeholder.  A later `PREPARE` for that epoch and wire id
binds its nonce into the same record.  It never changes `Claimed` or
`ClaimedOrLater` back to `Reserved`.

Claim consumption and ordered revoke consumption form the race:

- revoke first retains `Revoked`, closes the compatibility wire id, rejects
  delayed claims, and emits `Revoked`;
- claim first retains worker ownership and emits `ClaimedOrLater`, including
  when the accepted request is still waiting for a compiler slot.

Each full identity has one immutable terminal outcome.  Exact duplicate
controls replay the same outcome.  Stale controls for an older nonce never
mutate a reused wire id.  An exact delayed `PREPARE` after revoke replays the
terminal result instead of emitting `READY`.

Records and compatibility close markers live for the scheduler epoch, not for
one TCP connection.  A transient worker-session loss closes every attributable
live record and retains its full identity; reconnecting with the same nonzero
epoch and mode is a rebind that preserves those outcomes.  It never clears
state.  A mode change within an epoch is refused.

A verified replacement by a different nonzero epoch may clear the old
assignment table.  Before clearing, the daemon records the old epoch as
retired.  That retired epoch can never be reactivated in the same daemon
process.  This is the product refinement of the formal destructive
`LoseSchedulerLink` step: transient retained reconnects stutter/rebind, while
any operation that actually clears the table also retires the old world and
requires a fresh nonzero epoch.

The terminal/live table and retired-epoch history have a production limit of
65,536 entries each.  Normal lookups and insertions are expected O(1).  The
daemon does not evict an identity from a live epoch: reaching the bound marks
that epoch exhausted, closes the scheduler session, rejects further claims,
and refuses same-epoch reactivation.  A fresh epoch is the bounded assignment
reset.  Retired-history exhaustion refuses replacement rather than forgetting
an old epoch.  The smaller limit used by the integration gate is available
only when the daemon is explicitly in test mode.

## Compatibility boundary and gates

Protocol 49 supports new scheduler + new worker + old client.  Because the old
client presents only a wire id, Advisory and EnforcingCompat cannot reject an
arbitrarily delayed prior claim with the exactness of a nonce-bearing client.
Protocol 50 supplies that end-to-end discriminator.  Protocol 49 does
distinguish delayed scheduler-worker controls by their full tuple.

The focused gates use the production message classes and real scheduler and
daemon processes.  They cover literal protocol-48 bytes, negotiated-P48
behavior, all four mode values, Advisory immediate exposure and
claim-before-PREPARE, EnforcingCompat `PREPARE -> READY -> UseCS`, deterministic
StrictNonce activation and mixed-path refusal, debit-at-PREPARE, the deferred-output worker gate,
revoke-first and claim-first races, cancellation versus delayed READY, stale
and duplicate controls, same-wire-id reuse, no early id release, late-claim
rejection, submitter and link teardown, retained same-epoch rebind,
clear-then-old-epoch refusal, claim evidence versus a delayed contradictory
result, current-channel deletion during READY failure, epoch-lifetime records,
bounded exhaustion, and default-disabled inheritance.
