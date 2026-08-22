# Protocol-50 assignment identity

Protocol 50 completes the existing ordered Protocol-49 scheduler-to-worker
assignment contract across the submitter/client path. It does not introduce a
second job identifier: `UseCS.job_id` and `CompileJob.jobID()` remain the one
serialized `wire_id` at every hop.

## Wire shape and compatibility

On a negotiated Protocol-50 link, `UseCS` and `CompileFile` append exactly four
32-bit network-order words after their Protocol-49 payload:

1. `assignment_epoch_hi`
2. `assignment_epoch_lo`
3. `assignment_nonce_hi`
4. `assignment_nonce_lo`

The two epoch words and two nonce words are combined into the same nonzero
64-bit epoch and nonce used by `AssignPrepare`, `AssignReady`,
`RevokeBeforeStart`, and `RevokeResult`. A present identity is therefore the
full `(epoch, existing job_id, nonce)` tuple. Zero epoch plus zero nonce means
identity absent. Any partial identity, or any present identity with a zero
existing job ID, is an invalid payload and closes the receiving channel before
the message can reach scheduling, relay, environment, queue, or compiler work.

Protocols 43, 48, and 49 serialize their retained layouts byte for byte and
decode these new carrier fields as all-zero absent identity. Protocol
negotiation applies independently on each link. The scheduler stamps the tuple
only after the worker has installed the matching `PREPARE`; the submitter
daemon relays that decoded object; the client copies the same existing job ID,
epoch, and nonce into `CompileFile`; and the fulfillment daemon compares the
entire tuple before queueing.

For the eight old/new scheduler/client/fulfillment combinations, only
50/50/50 can deliver a full identity. All other seven combinations preserve
the existing job ID but carry wholly absent epoch and nonce. In particular,
an old scheduler with a new client and fulfillment daemon cannot accidentally
activate strict admission: its scheduler link supplies no Protocol-49 strict
configuration, so the worker remains in its legacy-compatible mode.

## Operator modes

The default remains `legacy`. `advisory` and `enforcing-compat` retain their
Protocol-49 meanings. Enforcing compatibility deliberately accepts either an
exact nonce-bearing Protocol-50 claim or a nonce-less legacy claim for the
currently live prepared wire ID.

`strict-nonce` is now operational, but only when explicitly selected with
`--assignment-fence-mode strict-nonce`. It is never selected by observing a
peer version. A strict remote assignment is eligible only when both scheduler
links, to the submitter and fulfillment daemon, negotiated Protocol 50. Older
connected daemons receive a Legacy configuration projection and are excluded
from strict remote selection; the global operator choice remains strict for
compatible peers. This prevents a mixed remote path from silently weakening
the chosen mode.

In strict mode the fulfillment daemon accepts only a full exact tuple against
its current Reserved record. A stale epoch, wire ID, nonce, partial tuple, or
absent tuple is rejected without consuming the live record. A subsequent exact
claim therefore remains admissible. Local `NoCS`/`CLIENTWORK` decisions do not
use a fulfillment daemon or a remote assignment record and remain exempt.

## Scope boundary

This change carries and validates assignment identity only. It adds no cache
advertisement or capability, no cache input selection or attachment, no M3
compiler attachment, no pipelining, no release-version change, and no second
wire ID.

## Deletion-sensitive gates

The focused gates map one-to-one onto the four production legs:

- `p49scheduler` requires the strict `UseCS` tuple to equal the earlier
  authorized `PREPARE`; deleting the scheduler stamp fails it.
- `p49daemon` sends a full `UseCS` through an actual submitter daemon and
  requires the client-side frame to preserve it; deleting the relay copy fails
  it.
- `p50assignment-remote.sh` selects the strict row of the actual
  `remoteice-quick.sh` compile harness through
  `icecc` and `build_remote_int`; deleting the client copy makes fulfillment
  reject the nonce-less `CompileFile`, so the required remote-path evidence
  fails. The gate is capability-aware, and required runs turn capability skips
  into failures.
- `p49daemon` sends stale nonce, epoch, and wire-ID claims to the real
  fulfillment daemon and requires rejection followed by admission of the exact
  claim against the same live record; deleting any full-tuple comparison fails
  the corresponding discriminator.
