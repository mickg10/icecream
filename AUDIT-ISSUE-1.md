# Audit: Issue #1 and branch `fix/submitter-requeue-on-transient-timeout`

Date: 2026-08-03.  Scope: issue #1 ("Scheduler nukes entire submitter on
transient send timeout"), the proposed fix branch
`fix/submitter-requeue-on-transient-timeout` (commit `ffa6ef9`), and a
corrected implementation on branch `fix/scheduler-deferred-dispatch-send`
(this branch).  All claims below were verified against code and, where marked
CONFIRMED, demonstrated by a failing/passing test in this repository.

## Verdict up front

* The **issue's root-cause analysis is accurate** (every code reference
  checks out) and the bug is real: reproduced end-to-end, the base scheduler
  destroys a submitter and all its in-flight jobs after one 30s send timeout.
* The **fix branch must not ship**.  It contains three independent defects,
  two of which make the failure mode *worse* than the bug it fixes, and one
  of which is an unrelated, silently smuggled wire-protocol regression that
  kills every monitor connection.
* A corrected fix (deferred non-blocking dispatch sends + POLLOUT-driven
  flushing, branch `fix/scheduler-deferred-dispatch-send`) passes all
  red/green gates: no submitter teardown, no protocol corruption, no
  scheduler stall at all (worst control-reply latency 0.8s vs 33.7s base,
  74.4s fix branch).

## 1. Issue claims verification (base = `webgui-observability`, 61adb28)

| Claim | Verdict |
|---|---|
| `send_msg` failure to a submitter is answered only by `handle_end` (empty_queue, `scheduler.cpp` NoCS/UseCS branches) | CONFIRMED |
| `handle_end` on a DAEMON deletes the submitter and **all** its queued + in-flight jobs (`JobDone 255`) | CONFIRMED (integration: 1172–1303 jobs destroyed per run) |
| `flush_writebuf` polls 30s and calls `set_error()` on timeout, poisoning the channel (`instate=ERROR`, `eof=true`) | CONFIRMED (unit test `contract` on base: channel `at_eof()` after timeout) |
| Client-side error strings and daemon `clear_children()` chain | CONFIRMED by reading `client/remote.cpp:242`, `client/main.cpp:676`, `daemon/main.cpp:5616` |
| The 30s timeout is reachable in production | CONFIRMED **with caveats** — see §4.  Note the option-history detail: `TCP_USER_TIMEOUT` (added 2020, commit 3528f52) is an ancestor of the 1.4 release, so the issue's 1.4.90 farm HAD the 9s kernel timeout armed.  The 30s application timeout is reached anyway because the wedged submitters were *slowly draining*, not stopped: a peer that keeps ACKing and freeing dribbles of buffer never trips `TCP_USER_TIMEOUT` (it requires zero forward progress), while still not freeing enough space within 30s for the blocking send to complete.  The harness's full-stop clog is an extreme stand-in for that, and strips the option so the kernel does not pre-empt the application path under test. |

## 2. Fix branch audit — three defects

### F1 (critical): `clear_writebuf()` tears partially transmitted messages

`flush_writebuf` does partial `send()`s in a loop; on a transient timeout the
remaining bytes of a **half-sent** message stay in `msgbuf`
(`msgofs > 0`).  At the moment a connection jams, a partial send is the
*common* case (the jam happens mid-message at the buffer boundary).
`clear_writebuf()` then discards the unsent remainder, leaving the peer's
framing layer waiting inside a truncated frame; the next "fresh" dispatch
message is consumed as the tail of the torn frame.

CONFIRMED twice:

* Unit (`backpressure contract`, fix branch): after timeout + clear + resend,
  the peer receives **nothing** — the re-dispatched USE_CS never arrives.
* Integration (`schedbp`, fix branch): the drained stream produces
  `corrupt USE_CS: client_id=0` followed by
  `received a too large message (size 172228608), ignoring` — the client
  daemon parses a length prefix out of mid-message bytes, `set_error()`s and
  kills the scheduler connection itself.  Net effect in production: the same
  client-visible storm the fix set out to eliminate, plus undefined garbage
  messages before the connection dies.

A latent sibling defect: with `msgofs > 0` and the channel alive, any
*other* `send_msg` (no `clear_writebuf`) appends via `writefull()` at
`msgbuf + msgtogo`, which **overlaps the pending region** — the base code's
implicit invariant "append only when `msgofs == 0`" was broken by keeping the
channel alive without restoring it.

### F2 (critical): requeue livelock — repeated 30s scheduler wedges

The requeue path (`requeue_job_request` → `enqueue_job_request` → `return
true`) feeds the scheduler's `while (empty_queue(...)) continue;` loop
(scheduler.cpp main loop).  `get_first_job_request()` picks by score with
lowest-id tie-break, so the requeued (oldest) job is re-picked immediately;
each retry against the still-jammed socket burns another **full 30s blocking
poll** inside the single-threaded scheduler, and `poll()` — the only place
dead daemons are detected and monitors/controls are serviced — is starved for
the whole time.  The branch's own comment recognizes the poll-starvation
hazard but guards only `at_eof()` channels; a merely-slow (alive) peer never
trips it.

CONFIRMED at the syscall level (strace of the fix-branch scheduler):

```
21:52:04.938910 poll([{fd=10, events=POLLOUT}], 1, 30000) = 0 (Timeout)
21:52:34.969798 poll([{fd=10, events=POLLOUT}], 1, 30000) = 0 (Timeout)
21:53:05.002381 poll([{fd=10, events=POLLOUT}], 1, 30000) = 1 (...)
```

Two consecutive 30s polls back to back — the loop only broke because the test
opened the window at t=75s.  Measured worst control-reply latency: **74.4s**
(vs base 33.7s — the "fix" more than doubles the outage, and with a longer
jam it cycles indefinitely).  Collateral: once a wedge exceeds
`MAX_SCHEDULER_PING` (36s), `prune_servers()` kills control/monitor
connections whose `last_talk` aged during the wedge (observed in one run).

### F3 (critical, unrelated, smuggled): monitor wire-tag regression in `comm.h`

The branch changes `Msg::value_` from `protected` to `private` and deletes
the `value_ = MON_GET_CS;` / `value_ = MON_JOB_DONE;` "overwrite"
assignments in `MonGetCSMsg`/`MonJobDoneMsg` constructors.  `Msg::
send_to_channel()` writes `value_` as the wire tag, so `MonGetCSMsg` now goes
out tagged `GET_CS` **with a Mon-shaped payload** (protocol ≥ 29 writes
Msg-header + filename + lang + job_id + clientid).  The receiving factory
builds a `GetCSMsg`, whose `fill_from_channel` misparses and fails the
message-length check:

```
internal error - message (GET_CS) not read correctly, message size 34 read 32
setting error state for channel ...
```

CONFIRMED by unit test `backpressure montags` (green on base and corrected
branch, red on the fix branch): **the first job-request notification kills
the monitor's channel** — any icemon dies instantly on a busy scheduler.
This hunk has nothing to do with the issue and must be dropped.

Minor: `requeue_job_request(Job*, CompileServer* /*use_cs*/)` ignores its
second parameter.

## 3. The corrected fix (this branch)

Design principle: **never abandon bytes on a live channel, and never block
the scheduler on one daemon's socket.**

* `services/comm.h/.cpp`
  - New `SendFlags` bit `SendDeferrable`: on backpressure (EAGAIN, or poll
    timeout when combined with `SendBlocking`) the serialized bytes stay
    queued, the channel stays healthy, and `send_msg` reports success.
    Non-deferrable behaviour is byte-for-byte unchanged (monitors still get
    dropped on EAGAIN; all other callers keep the 30s+poison semantics).
  - `has_pending_write()` / `flush_pending()`: non-blocking continuation;
    `flush_pending()` returns false only when the connection actually died.
  - `flush_writebuf` now **compacts unconditionally on exit**, restoring the
    global invariant `msgofs == 0` that `writefull()`/`send_msg()`'s
    append-at-`msgtogo` and length-patching depend on.  This also closes the
    F1-sibling append-corruption hazard for any future caller.
  - No `clear_writebuf()`.  Pending bytes are flushed in order, so a
    partially transmitted message is always completed — tearing is
    impossible by construction.
* `scheduler/scheduler.cpp`
  - `empty_queue()` sends UseCS/NoCS with `SendNonBlocking|SendDeferrable`;
    a false return now really means "connection dead" and keeps the old
    `handle_end` path.
  - Main loop: daemon channels with pending bytes get `POLLOUT` interest;
    on writability (or error) the pending queue is flushed with
    `flush_pending()`, and failure tears the daemon down exactly like a
    read-side EOF.
  - No requeue machinery, no blocking anywhere in the dispatch path.
* Review-driven hardening (added after the adversarial review):
  - `MsgChannel::pending_write_age()` tracks how long deferred output has
    waited (armed on the deferral transition, cleared on full drain, with a
    once-per-episode trace line), and `prune_servers()` removes a daemon
    whose deferred output has gone unaccepted for **30s** — the same bound
    the old blocking send enforced, now platform-independent (it no longer
    relies on the `#ifdef`'d `TCP_USER_TIMEOUT`) and enforced without
    wedging the scheduler.  This also caps the widened client-cancel race
    (a job cancelled while its UseCS sits deferred stays matched to its
    server until delivery or this bound; the daemon's JobDone(107) on late
    delivery cleans up the short-lived phantom).
  - `ENOTCONN` is fenced out of the deferrable classification: a
    never-connected socket polls writable, so deferring on it would queue
    bytes and spin silently.  Only genuine backpressure (EAGAIN/EWOULDBLOCK,
    or the blocking-poll timeout) defers.

Bounding: pending bytes per submitter are bounded by its own in-flight job
requests (~60B per dispatch reply); a daemon that never drains keeps its
TCP_USER_TIMEOUT/keepalive semantics (kernel declares it dead) or is cleaned
up by the existing read-side paths.

## 4. Environment findings (matter for interpreting the issue)

* **`TCP_USER_TIMEOUT` = 9s** (`MsgChannel` ctor, `services/comm.cpp:1059`)
  is present in every release since 2020 (commit 3528f52 predates the 1.4
  tag — the issue's 1.4.90 farm had it armed).  On a modern Linux kernel it
  kills a *fully* stalled (zero-window, zero-progress) TCP peer ~9s in, on
  every variant.  It does NOT fire for the production failure mode: a
  slowly-draining peer keeps ACKing and freeing dribbles of buffer, which
  resets the kernel timer while still never freeing enough space within 30s
  — that is how the farm reached the application-level timeout with the
  option armed.  The harness's full-stop clog is an extreme stand-in for
  that slow drain, so it strips the option to keep the kernel from
  pre-empting the application path under test.  The option is also
  `#ifdef`'d (absent on some platforms), inert on the AF_UNIX
  client↔daemon channels, and on Linux < 5.11 did not abort ACKed
  zero-window probing — which is why the corrected fix adds its own
  application-level bound (§3) rather than relying on it.
* **Kernel buffer autotuning**: with default (megabyte-scale) buffers,
  dispatch replies (~60B each) essentially never jam a connection; the
  pathology needs thousands of queued jobs and/or reduced buffers.  The
  integration harness pins the scheduler's `SO_SNDBUF` to 4KiB via an
  `LD_PRELOAD` shim to make the jam deterministic.
* **Loopback zero-window pathology**: with `SO_RCVBUF` below one loopback
  MSS (~64KiB), a zero-window connection never reopens its window (silly
  window syndrome avoidance) — early harness versions produced false reds
  this way.  The harness documents and avoids this (64KiB receiver buffer).
* The scheduler accepts new connections at most once per second
  (`next_listen`), so connection-setup latency is a poor liveness signal —
  the harness probes over a pre-established text-port control channel.

## 5. Red/green matrix

Unit (`unittests/backpressure`, AF_UNIX socketpair, ~30s on old branches due
to the hardcoded poll timeout).  Beyond the two differentiating groups below,
the suite also pins the new API's semantics: `multiqueue` (fixed branch
only; many messages appended behind a jammed one arrive intact and in order
-- the observable form of the msgofs==0 compaction invariant; run at both
8KiB and 2KiB socket buffers, the latter landing the partial write inside
chop_output()'s no-compact window where review proved the unconditional
compaction is load-bearing), `flusherrors` (fixed branch only;
flush_pending() returns false only for a genuinely dead peer and leaves the
channel errored; empty-buffer, already-errored, and deferred-age edges), and
`nondeferrable` (every variant; a SendNonBlocking send without
SendDeferrable still fails fast and poisons the channel, which the monitor
path relies on).  The integration harness gained a split contract to match
the new 30s bound: normal mode (25s transient stall) asserts nothing is
lost and nothing wedges; `stall` mode (75s, never drains) asserts the
scheduler cuts the dead submitter loose at ~30s — measured 31s — without
wedging, which doubles as the positive proof that the backpressure scenario
really engaged:

| test | base | fix branch | corrected |
|---|---|---|---|
| `contract` — backpressured dispatch send must not kill the channel; USE_CS must arrive intact after drain | **FAIL** (channel poisoned) | **FAIL** (stream torn; USE_CS never arrives) | PASS |
| `montags` — Mon* messages keep MON_* wire tags | PASS | **FAIL** (monitor channel killed) | PASS |

Integration (`unittests/schedbp` + `sndbuf_shim.so`, real scheduler, fake
daemons, 4000 jobs, 75s full-stop clog, issue-report conditions):

| assertion | base | fix branch | corrected |
|---|---|---|---|
| submitter survives | **FAIL** (nuked at 30s) | **FAIL** (torn stream kills channel) | PASS |
| every job answered exactly once, intact | **FAIL** (2754–2828/4000) | **FAIL** (2628–2758/4000 + corrupt USE_CS) | PASS (4000/4000) |
| compile server survives | PASS | PASS | PASS |
| scheduler responsive (<5s control replies) | **FAIL** (33.7s) | **FAIL** (74.4s, livelock) | PASS (0.8s) |
| control connection survives | PASS | PASS | PASS |

Reproduce:

```
# unit (wired into make check)
make -C unittests check
# integration
gcc -shared -fPIC -O2 unittests/sndbuf_shim.c -ldl -o /tmp/sndbuf_shim.so
g++ -std=c++17 -g -O1 -Iservices -I. unittests/schedbp.cpp \
    services/.libs/libicecc.a -llzo2 -lzstd -larchive -ldl -lpthread -o /tmp/schedbp
/tmp/schedbp scheduler/icecc-scheduler /tmp/sndbuf_shim.so 4000 75
```

## 6. Recommendations

1. Do not merge `fix/submitter-requeue-on-transient-timeout`.
2. Adopt `fix/scheduler-deferred-dispatch-send` (this branch) for the
   scheduler dispatch path.
3. Separately consider making `TCP_USER_TIMEOUT` (currently hardcoded 9s)
   configurable: it kills a fully-stalled TCP peer after ~9s on every
   variant (a smaller, kernel-level cousin of the blast radius), and its
   platform variance is why the corrected fix carries its own 30s
   application-level bound.
4. If other scheduler→daemon sends (rare: pings to old protocols, conf
   pushes) ever show the same pattern, they can adopt
   `SendNonBlocking|SendDeferrable` — the main-loop flush machinery is
   already in place.
