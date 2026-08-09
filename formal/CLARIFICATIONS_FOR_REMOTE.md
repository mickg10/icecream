# Clarification questions for the remote oracle (p49/p50)

Context: these questions arise from reading `formal/AUDIT_DELTA.md`,
`formal/FencedAssignment.tla`, `formal/TemporalOwnership.tla`,
`formal/README.md`, `formal/FARM_EXECUTION_PROGRAM.md`, and
`formal/properties.ltl` on `audit/temporal-ownership-model` against the
current fork wire (`services/comm.{h,cpp}`, PROTOCOL_VERSION 48), the
scheduler (`scheduler/scheduler.cpp`), and the daemon (`daemon/main.cpp`).
Each question states where the AUDIT_DELTA is ambiguous or underspecified and
the concrete decision we need.  Design context and our default proposals are
in [`P49_P50_DESIGN.md`](P49_P50_DESIGN.md).

The farm-inventory input referenced below was obtained from issue #2 on
mickg10/icecream (13 workers / 792 remote slots, ~741 submitting hosts, one
scheduler, cold build ~2,200 TUs).

---

**Q1 — Epoch allocation and persistence across a scheduler restart with no
stable storage.**  Assignments and tombstones are epoch-scoped, but the
scheduler persists nothing: `new_job_id` is a process-local counter
(`scheduler/scheduler.cpp:113`, `:727`) and there is no state directory.  Is
a 64-bit epoch drawn once per process as
`(start_time_seconds << 32) | random32`, compared for **equality only**,
acceptable — i.e., may the design treat the epoch as an unordered unique
token — or does any part of your model (stale-epoch fast-reject at F,
monitoring, trace ordering, the `Restart(e)` action) rely on epoch
monotonicity across restarts, which would force either persistent storage or
a trusted clock?  Note that `FencedAssignment.tla`'s `Restart(e)` requires
only `e # currentEpoch` and so permits returning to a previously used epoch;
if uniqueness (not mere change) is the intended assumption, please state it
as an explicit model assumption.

**Q2 — Does ASSIGN_PREPARE/READY add a round trip to the hot dispatch path,
and what is the latency/throughput budget at farm scale?**
FARM_EXECUTION_PROGRAM Phase 4 promises "Normal dispatch has no extra round
trip; only exceptional reclamation does," but AUDIT_DELTA's revised p49
mandates "S exposes UseCS only after READY" — one full S→F→S round trip plus
one F event-loop wakeup on **every** remote dispatch.  On this farm (792
slots, ~741 submitters, hundreds of dispatches/second during a cold 2,200-TU
build; F event loops that also fork compilers and can defer reads under
load), what added dispatch p95/p99 latency and what jobs/sec regression is
acceptable before READY-gating is considered too expensive?  Is our ADVISORY
mode (PREPARE pipelined with UseCS, fence not load-bearing, zero added
latency) an acceptable long-term operating point, or is ENFORCING
(READY-gated) the required end state on this farm?

**Q3 — The "waits briefly for a causally earlier prepare" rule conflates the
strict and pipelined variants.**  Under strict READY-before-UseCS a
legitimate claim can never precede its prepare (UseCS is withheld until F
acked), so the wait rule is unnecessary there; under a pipelined form it is
load-bearing but unspecified.  Confirm our resolution — no park-and-wait at F
in any mode (ENFORCING rejects unknown ids outright; ADVISORY admits them as
legacy claims) — or, if the wait must exist, specify: the wait duration, what
the waiting connection holds (an fd only, or a compile slot), whether
environment transfer proceeds while waiting, and what the client observes on
rejection given that the C-F alphabet has no negative acknowledgement (today
a connection close makes the client fall back to a local compile).

**Q4 — Tombstone lifetime, GC bounds, and memory cost under churn.**
"REVOKED installs a tombstone before its ack" has no stated retention bound.
Please specify: (a) may F drop all tombstones and assignment records on a
scheduler-epoch change and on scheduler-link loss, matching the daemon's
existing clear-on-loss behavior (`daemon/main.cpp:6254-6256`)?  (b) within an
epoch, what is the tombstone TTL floor — formally the maximum client delay is
unbounded (your indistinguishability lemma), so any TTL is a stated
assumption; we propose 15 minutes, configurable; (c) is a count cap with
eviction telemetry acceptable at the observed cancellation scales (40k-member
batch cancellations, 21+ concurrent groups), or must the bound be
proof-carrying (no eviction below the freshness window)?  For scale: a
tombstone is ~24 bytes; 40k revocations across 13 workers is ~74 KB per
worker — is bounding by count even necessary, or only by TTL?

**Q5 — The exact old-C wire-id freshness / quarantine assumption for S'F'C.**
The matrix row says "exactness bounded by old-C id freshness" and the prose
says persistence/randomization "is not a proof against arbitrary delay," but
the rule itself is never stated.  We propose: per-epoch assignment-table
flush at F, plus `new_job_id` starting at a uniformly random 32-bit value per
epoch (skipping 0), giving residual collision probability per delayed claim
of `ids_prepared_on_that_worker_during_the_delay / 2^32`, with **no**
quarantine window (unenforceable without persistence).  Is this the freshness
rule you intended, and what maximum old-C delay should the acceptance traces
assume when they force id reuse with the tiny test allocator (Phase 5)?

**Q6 — The S'F'[CC'] row overstates nonce protection: per-claim vs
per-assignment exactness.**  F cannot know an assignment's client generation
in advance, so nonce-less claims must remain admissible in a mixed fleet.  A
delayed old-C claim presenting a colliding wire id can therefore still claim
an assignment whose real client is a C' — the "C' exact nonce mode" guarantee
is a property of *claims*, not of *assignments*.  Your own model shows this:
`FencedAssignment.tla`'s `NonceSpec` omits `LegacyClaim` from `Next`; a mixed
next-state relation (`NonceClaim ∨ LegacyClaim`) readmits the
ExactGenerationSafety counterexample for nonce-bearing assignments.  Do you
(a) accept the weaker per-claim statement for the mixed row (and restate the
matrix accordingly), and (b) want a strict admission mode — reject nonce-less
claims, operator-enabled once no old C remains — added to the design so the
exact row becomes reachable, and if so, where is it negotiated (we propose a
`CS_CONF` fence_mode value)?  Please also add the mixed-fleet configuration
to the TLA+ checking matrix.

**Q7 — Interaction with the deferred-output/backpressure path and the
dispatch-credit clamp.**  p49 puts PREPARE/REVOKE on the S→F channel, which
today carries almost no S-originated traffic and participates in the
deferred-send machinery (30 s `ICECC_DEFERRED_SEND_TIMEOUT_MSEC` bound;
submitters mid-backlog are ineligible for dispatch via
`submitter_accepts_dispatch()`, `scheduler/scheduler.cpp:813-818`).  Two
decisions: (a) should a *worker* with an armed deferred-output backlog be
ineligible for new PREPAREs (a worker-side analogue of the submitter gate),
i.e. should `pick_server` skip it, or is queueing PREPAREs into the deferred
buffer acceptable up to the 30 s bound?  (b) we move the dispatch-credit
debit to the dispatch decision (PREPARE send) so the oldest-unconfirmed
stall bound spans the PREPARED window — confirm, or argue for debit at UseCS
send (which would leave ENFORCING-mode PREPARED assignments outside the
credit clamp entirely).

**Q8 — REVOKE_RESULT vocabulary vs F's claimed-but-unstarted phase.**  F has
a real CLAIMED state with no running compile: an accepted `CompileFile` parks
in the `TOCOMPILE` queue behind the load gate (`daemon/main.cpp:5464`,
`current_load >= 1000` blocks starts indefinitely).  The sketch's alphabet
{REVOKED, STARTED} forces F to answer STARTED for a compile that has not
begun (and likewise for one that already completed).  Options: (a) keep the
two-value alphabet and define STARTED as "claimed or later" (our conservative
default — the scheduler retains ownership and the claimant drives it to
JobBegin/JobDone or dies); (b) add a CLAIMED result for observability only;
(c) make claimed-but-unstarted compiles revocable (disconnect the claimant,
which falls back to a local compile).  Which do you want, given (c) trades a
client-visible disconnection for tighter reclamation?

**Q9 — Should revocation-without-fence upgrade the stall-retention policy?**
The scheduler currently retains a stalled unconfirmed dispatch by design
(`scheduler/scheduler.cpp:1646-1698`) because, per your lemma, timeout-only
reclaim is unsafe without revocation.  With PR-1's REVOKE (no PREPARE/READY
yet), the id under revocation was allocated in the current epoch and cannot
be stale, and a REVOKED result proves no compile started.  Is
revoke-after-stall-bound acceptable **at PR-1** (LEGACY mode, tombstone on an
id F never saw prepared) for >= 49 workers, or do you want the policy flip
held until the fence (ENFORCING) also lands?  We propose shipping it
flag-guarded, default off, and ask which default the acceptance matrix should
pin.

**Q10 — Fence activation semantics on F.**  We propose `CS_CONF` to carry
`(epoch, fence_mode)` so protocol 49 defines the vocabulary while activation
is a per-scheduler-session policy.  Confirm two consequences: (a) with
fence_mode=LEGACY (revoke-only operation), F must default-ALLOW unknown ids —
otherwise every job on a 49 link would be rejected before the scheduler
starts preparing; (b) with fence_mode=ENFORCING, default-deny applies to
claims arriving on **all** client links including < 50 ones (the deny is
keyed on the S-F discipline, not the claim's version), with the CLIENTWORK /
local-decision path exempt (`daemon/main.cpp:5556-5563`; local decisions
never debit credit, `scheduler/scheduler.cpp:1931`).

**Q11 — RESERVED-entry lease vs long environment transfers.**  If a prepared
assignment is never claimed and never revoked (scheduler died after READY;
epoch change not yet observed), F's table entry must expire locally.  But a
legitimate claim is often preceded, on the same connection, by a multi-minute
environment transfer that cannot be associated with the assignment until
`CompileFileMsg` arrives (EnvTransferMsg carries no job id).  What lease
floor do you require for RESERVED entries (we propose 10 minutes,
configurable; for comparison MAX_BUSY_INSTALLING is 120 s), and is local
expiry without any F→S notification acceptable — the scheduler's own
settlement paths (bounce, teardown, revocation, epoch change) being the
authoritative ones?

**Q12 — Msg enum allocation policy for fork-private messages.**  The fork
already consumed implicit values through 97 (`JOB_TIMING`); we propose 98-101
for the four p49 messages.  Upstream (protocol 44) could later mint the same
numeric values with different meanings; a >= 49 negotiation implies both ends
are fork builds, and we gate *decode* on the negotiated version so a
44-negotiated link can never construct the new classes
(`services/comm.cpp:1291` switch).  Any objection to sequential allocation,
or do you prefer a fork-private numeric base (e.g. 120+) for everything after
97 to keep a visible upstream/fork boundary?

**Q13 — Pipelined-form proof obligation.**  AUDIT_DELTA reserves "a pipelined
form" pending separate proof.  Our ADVISORY mode is that form, made safe by
construction: the fence is not load-bearing (unknown ids admitted), so the
claim-races-prepare window loses hardening but cannot lose correctness
relative to p48.  Do you agree ADVISORY-as-specified needs no additional
proof beyond p48-refinement (its rejects are a subset of: tombstoned ids and
nonce mismatches, both sound), and that the proof obligation attaches only to
ENFORCING (which we discharge by the causal READY-before-UseCS argument plus
the FencedAssignment model with the mixed Next from Q6)?

**Q14 — Model/typing nits to fix before TLC runs.**  (a)
`FencedAssignment.tla` types `claimedBy \in [Assignments -> (Assignments \cup
{NoNonce})]` — the sentinel is nonce-sorted in an assignment-sorted slot;
suggest a distinct `NoAssignment`.  (b) `AuthorizationSafety` is satisfied by
construction of both claim actions and so checks little; consider replacing
it with "Claimed/Started implies a prepare occurred in the same epoch"
(currently implied only via `phase[current] = "Reserved"`).  (c) Per your
README note, neither TLA+ module has been TLC/Apalache-checked yet; please
run both configs plus a mixed-claim config before we freeze message
semantics, since Q6 predicts a counterexample in the mixed model.
