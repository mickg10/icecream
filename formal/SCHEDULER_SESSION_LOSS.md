# Scheduler-session loss formal gate (G4)

Product source mapped by this gate:

```text
mickg10/icecream
fix/stage3-scheduler-policy
b393e9d83db5b367a89cdef2d23c2d04d5576f19
```

This is a protocol-neutral model of the daemon's scheduler-session lifecycle.
It does not model assignment-fence wire messages and does not authorize a
protocol-version change.

## Linearization points

```text
DISCONNECTED
    -> LOGIN_ATTEMPT       scheduler channel selected; Login sent
    -> ACTIVE              ConfCS consumed by iceccd
    -> LOSS_PENDING        active channel is observed lost
    -> DISCONNECTED        generation-scoped cleanup settles once

ACTIVE / LOGIN_ATTEMPT / DISCONNECTED
    -> SHUTDOWN            orderly daemon termination; not unexpected loss
```

A non-null scheduler pointer is not sufficient evidence of an established
session. A channel failure before `ConfCS` preserves schedulerless local work
and does not increment the established-session cleanup counter.

## Checked safety

- `ActiveRequiresConf`: ACTIVE implies a live channel, received ConfCS, and a
  committed generation.
- `AttemptFailurePreserved`: a failed Login attempt neither clears local work
  nor charges established-session cleanup.
- `NoPostLossWork`: after active-session loss, no later event from that poll
  turn executes before cleanup and turn exit.
- `CleanupCountCoherent`: each generation contributes at most one cleanup.
- `RetiredOnlyIfCleaned`: a loss cannot be retired because a numeric sentinel
  accidentally matches its generation.
- `NoUnexpectedShutdownCleanup`: orderly shutdown is not classified as loss.
- `NoGenerationReuse`: generation exhaustion fails closed instead of wrapping.

## Progress

`LossEventuallySettled` assumes weak fairness only for `FinishLoss`, the
product-controlled cleanup action. It assumes no scheduler reply, reconnect,
client progress, compiler termination, or peer-failure fairness.

## Witnesses and mutants

The matrix contains fixed safety/liveness, three reachability witnesses, and
six one-premise mutants:

```text
active-after-ConfCS witness
failed-Login-preserves-local-work witness
active-loss-cleanup witness
pointer-means-active mutant
same-poll-later-event mutant
duplicate-cleanup mutant
numeric-sentinel mutant
orderly-shutdown-as-loss mutant
generation-wrap mutant
```

Every expected counterexample is normalized and validated by a declarative
trace manifest on both TLA+ Tools 1.7.4 and 1.8.0. Stable/differential event
sequences and emitted C++ barrier steps must agree.

## Product action map

| TLA+ action | Product location / intended transition |
|---|---|
| `BeginLogin` | `Daemon::reconnect()`: channel acquired and Login sent; enter LOGIN_ATTEMPT |
| `ReceiveConf` | `Daemon::handle_cs_conf()`: commit ACTIVE and the new generation |
| `FailLoginAttempt` | Login send/read failure before ConfCS; close attempt only |
| `ActiveChannelLoss` | any checked send/read/poll boundary after ACTIVE |
| `SameTurnWork` | later pre-poll/post-poll client, listener, child, or environment event |
| `FinishLoss` | `finish_scheduler_loss_if_needed()`: settle before `clear_children()` |
| `OrderlyShutdown` | `working_loop()` shutdown branch |

The source at the mapped product SHA already contains the loss helper, explicit
settled-valid state, diagnostics, and the immediate pre-poll boundary. It does
not yet implement ConfCS-committed activation or generation-wrap refusal; those
remain product deltas to be checked against this model.

## Evidence command

The read-only workflow `.github/workflows/scheduler-session-loss-formal.yml`
runs the complete matrix with pinned jars, one worker, positive RSS evidence,
nonzero state counts, complete fixed state spaces, validated traces, and
stable/differential semantic comparison.
