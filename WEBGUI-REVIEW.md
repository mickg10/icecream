# Review: iceccd web GUI / observability surface

**Scope.** The `webgui-observability` work as it exists in `daemon/main.cpp` (merge-base
`37cb407940a18aee5b444cc6d9d75dd2aaccf86d`), plus `tests/webgui/`, the `--webgui` /
`--webgui-port` / `--webgui-addr` flags, and the `ICECC_WEB_HOSTPORT` override. The entire
web surface is fork-added: `git show origin/master:daemon/main.cpp | grep -c webgui` returns
0. All line numbers below were checked against the tree under review, whose `daemon/main.cpp`
is byte-identical to the branch tip.

---

## 1. Verdict

**This code is safe to run bound to `127.0.0.1` on a trusted, non-multi-user build node, and
it is not safe to bind to a routable address as it stands.** Two properties of the deployment
drive that conclusion. First, `iceccd` runs as root on farm nodes because it needs chroot for
remote jobs; `grep -c setuid daemon/main.cpp` returns 0, so the daemon proper never drops
privilege and everything the web server touches is touched as root. Second, the daemon is a
single `poll()` loop that also carries compile-job dispatch and the scheduler channel, so any
CPU burn, stall or fd pressure introduced on the web path is taken directly out of the node's
compile throughput. That failure class is not hypothetical here: issue #1 on this repository
was a blocking send in the scheduler path that wedged the same loop.

Two things materially limit the blast radius today, and the report should be read with them in
mind. The GUI is opt-in (`webgui_enabled = false`, daemon/main.cpp:1049; `setup_web_listen_fd`
returns immediately without creating a socket when it is off, daemon/main.cpp:1289-1292), and
the default bind is loopback (`webgui_addr = "127.0.0.1"`, daemon/main.cpp:1051). A stock
`iceccd` is therefore unaffected by everything in section 2. Nothing in the packaging turns
the GUI on.

The problem is that the branch's own deployment story points away from that safe default.
`DEPLOYMENT-MATRIX.md` sells farm-node web GUIs as a reason to run this branch, and
`ICECC_WEB_HOSTPORT` maps an empty host component to `0.0.0.0` (daemon/main.cpp:6188-6189), so
the ordinary-looking value `ICECC_WEB_HOSTPORT=":8768"` silently publishes an unauthenticated,
uncapped, un-timed-out HTTP server from a root process onto every interface of the build
network. In that configuration a single unprivileged peer with no credentials and no compile
job can take the node out of the farm. Findings W1, W2 and W3 were each reproduced against the
branch binary.

Concretely: keep it on loopback, keep it off on shared or multi-tenant nodes, and fix W1
through W3 before any non-loopback bind is offered as a supported configuration. Independently
of any of that, W4 means the telemetry the feature exists to produce is wrong today on every
submitting node, with no attacker involved.

---

## 2. Findings

Severities below are the post-verification ones. Where an initial reviewer's rating was
adjusted during refutation, both are shown and the reason is given.

### W1. Web connections have no idle timeout, no keepalive and no count cap
**Severity: high** (initially proposed critical by two reviewers; reduced because the feature
is opt-in and loopback by default, the impact is availability-only, and the uncapped/unreaped
pattern already exists upstream on the daemon's client listener at daemon/main.cpp:5693.)
**Where:** daemon/main.cpp:3384-3406, :917-918, :5514-5517, :5662-5672.

`handle_web_accept()` accepts in an unbounded `for (;;)` loop (daemon/main.cpp:3384) and
inserts every accepted socket into `web_connections` (daemon/main.cpp:3405) with no check on
the map's size. The author clearly intended an age-based reaper: `WebConnection::created_msec`
is declared at daemon/main.cpp:917, zero-initialised at :918, and stamped at accept time at
:3404. It is then never read. `grep -n created_msec daemon/main.cpp` returns 451, 452, 556,
917, 918, 3006 and 3404; the first four and 3006 belong to the unrelated `Client` struct, so
the `WebConnection` field has no reader anywhere in the program. No sweep over
`web_connections` exists: the only iterations are `close_web()` (daemon/main.cpp:1411-1413,
called once at shutdown), the poll-array build (:5514) and the revents scan (:5662). Every
`drop_web_connection` call site lives inside `handle_web_connection` and therefore requires a
poll event on that specific fd. The accepted socket also gets no socket-level backstop:
`grep -c "SO_KEEPALIVE\|TCP_USER_TIMEOUT\|SO_RCVTIMEO" daemon/main.cpp` returns 0, and the
accept path sets only `O_NONBLOCK` and `FD_CLOEXEC` (daemon/main.cpp:3396-3400).

A peer that connects and sends nothing therefore generates no further event, is never aged
out, and pins one fd in the root daemon until the daemon exits. The same applies to a peer
that simply vanishes (a closed laptop, a NAT idle drop), so this is not only an attack: it is
also a slow leak under ordinary use, and every open dashboard tab opens a fresh TCP connection
every two seconds because the page polls on `setInterval(refresh, 2000)` (daemon/main.cpp:2274)
while every response carries `Connection: close` (daemon/main.cpp:1477).

Two costs follow. The fd budget is shared with compile clients, remote job sockets and
environment files, so exhausting it stops the node taking compile work. Separately, each
retained connection is re-pushed into the poll set on every main-loop iteration
(daemon/main.cpp:5514-5517) and then located by a fresh linear scan of the whole `pollfds`
vector (daemon/main.cpp:5662-5672), making event dispatch quadratic in the number of held
connections on the one thread that schedules compiles.

Reproduced against the branch binary, launched exactly as the test harness launches it
(`--no-remote -m 0 --webgui --webgui-port <p> --webgui-addr 127.0.0.1`). With 0, 1000, 2000
and 4000 silent connections the daemon's fd count went 9, 1010, 2009, 4009 and the CPU cost of
a single `/api/state` request went 0.33 ms, 2.67 ms, 7.83 ms, 27.8 ms; doubling the connection
count multiplied the cost by roughly 3.5, confirming the quadratic term. Nothing was reclaimed
after 12 seconds of silence, and a socket that had sent nothing was still open after 20. The
part that matters most for this codebase: the cost of a *compile client* connecting on the
daemon's own unix socket, which has nothing to do with the web port, went from 0.20 ms to
3.65 ms across the same sweep. With the soft `nofile` limit lowered to 128, a trivial connect
loop pinned the daemon at 128 fds and a compile client that had previously received the
protocol handshake received nothing on three consecutive attempts.

**Fix.** Use the timestamp that is already being recorded. Add a last-activity stamp beside
`created_msec`, and at the top of `answer_client_requests()` sweep `web_connections`, dropping
any entry that has not completed its request headers within a few seconds or has exceeded a
total lifetime cap. Clamp `poll_timeout_msec` (computed at daemon/main.cpp:5577-5592, which
today accounts only for the scheduler pong and the state-dump deadline) so the sweep actually
runs on an otherwise idle node. Cap `web_connections.size()` in the accept loop at something
a dashboard will never exceed, on the order of 32 to 64, and close anything accepted beyond
it immediately. Finally, record each connection's index into `pollfds` at registration time,
or handle web revents in the same pass that builds the array, so dispatch is linear.

### W2. `EMFILE` on the web listener busy-spins the main loop and defeats its own log throttle
**Severity: high** (initially proposed critical; reduced because it amplifies W1 rather than
firing on its own, the GUI is opt-in and loopback by default, and the accept-respin pattern
itself predates the fork.)
**Where:** daemon/main.cpp:3387-3393, :1447-1448, :1453, :5508-5511, :5658-5659.

When `accept()` on `web_listen_fd` fails with anything other than `EAGAIN`, `EWOULDBLOCK` or
`EINTR`, `handle_web_accept()` calls `note_accept_error("webgui", errno)` and returns
(daemon/main.cpp:3392-3393). It does not dequeue the pending connection, does not disarm the
listener, and applies no backoff. The listener is registered with level-triggered `POLLIN` on
every iteration (daemon/main.cpp:5508-5511) and re-checked every iteration
(daemon/main.cpp:5658-5659), so with a connection still queued, `poll()` returns immediately
forever regardless of the computed timeout. I confirmed against the kernel with a standalone
C program under a reduced `RLIMIT_NOFILE` that Linux does not drop the queued connection on
`EMFILE`: `poll` returns `revents=0x1` and `accept` returns `errno=24` on every pass,
indefinitely.

`note_accept_error()` then amplifies it. The function has a five-second log throttle, but
daemon/main.cpp:1447 sets `const bool force_log = (err == EMFILE || err == ENFILE);` and
:1448 skips the throttle when `force_log` is set, so the one errno that recurs at loop
frequency is exactly the one exempted from rate limiting. It also calls
`collect_fd_snapshot()` (daemon/main.cpp:1453) on every logged occurrence, which opens
`/proc/self/fd` (daemon/main.cpp:141). Measured on the running daemon under `ulimit -n 256`
with idle connections held open: 4.96 seconds of CPU over a 5-second window, and 6,548,485
bytes of identical `accept failed (webgui)` lines, about 1.3 MB/s; a longer run produced
roughly 57 MB in 20 seconds. That is both a CPU starvation of the compile-scheduling loop and
an unbounded disk-fill vector on `/var/log`. The condition does not self-heal, because W1
guarantees the held fds are never reclaimed.

Two corrections to how this was first written up. It is a busy-spin, not a total freeze: the
loop keeps iterating and still services existing client fds and the scheduler channel each
pass, so the node stops taking *new* work because of the fd exhaustion itself rather than
because of the spin. And `collect_fd_snapshot()` does not perform an expensive directory walk
in this state, because `opendir` itself needs an fd and fails; the ironic consequence is that
`open_count` stays -1 and the `open_fds` field the author added is omitted precisely when it
would be most useful. Note also that the merge-base has the same accept-respin defect on the
client listeners (daemon/main.cpp:2114-2126 at the merge-base) with worse logging, so the
class is inherited; what is new here is a second listener that an unauthenticated peer can
drive into the state, and the deliberate `EMFILE`/`ENFILE` exemption from the branch's own new
rate limiter. Commit a0cbc99 ("Harden iceccd against EMFILE") added counters and logging on
this path but no actual mitigation.

**Fix.** On `EMFILE` or `ENFILE`, either stop registering `web_listen_fd` in the poll set for
a backoff interval of a second or so, or keep one spare fd in reserve and use the standard
close-accept-close-reopen trick so the backlog actually drains. Remove the `force_log`
exemption at daemon/main.cpp:1447; the counters at :1438-1441 already record every occurrence,
so throttling the log line loses no information. Do not call `collect_fd_snapshot()` from a
path that can fire at loop frequency.

### W3. The 64 KiB request guard neither closes the connection nor stops reading
**Severity: high** (confirmed at the proposed level.)
**Where:** daemon/main.cpp:3429-3432, :5516, :1482, :3506.

When `conn.inbuf` exceeds 64 KiB the code queues a 413 response and `break`s out of the read
loop (daemon/main.cpp:3429-3432). It does not call `drop_web_connection(fd)`, does not clear
or truncate `inbuf`, and does not disarm `POLLIN`; the poll registration at
daemon/main.cpp:5516 arms `POLLIN` unconditionally for every web connection regardless of
state. `queue_web_response` also assigns `outbuf` wholesale (daemon/main.cpp:1482) rather than
appending or refusing while a response is in flight.

The simple case is benign: with nothing else in flight, the small 413 is written on the next
`POLLOUT` cycle and `close_after_write` drops the connection. The unbounded case needs one
extra step. A peer first elicits a response it never reads, for example `GET /` which returns
roughly 25 KB of HTML, so the kernel send buffer fills. It then keeps sending. Our write of
the 413 now returns `EAGAIN` at daemon/main.cpp:3506 and the connection stays alive with a
non-empty `outbuf`. From then on, every poll wakeup does the same thing: `POLLIN` fires because
the peer is still sending, `read()` appends up to 4096 bytes to `inbuf`, the size check trips,
`queue_web_response` regenerates the 413 into `outbuf` and in doing so discards whatever bytes
of the previous response had already been written (so the wire sees a truncated body followed
by a fresh status line), and the loop breaks with no write possible. The dispatch gate at
daemon/main.cpp:3447 is `conn.outbuf.empty() && ...`, so the connection can neither parse nor
write nor close, while `inbuf` grows at line rate. With W1 removing any deadline and any cap,
this is unbounded memory growth plus a CPU spin in a root process.

**Fix.** On exceeding the limit, queue the 413 and mark the connection for close, or drop it
outright. Stop arming `POLLIN` once a response has been queued: `close_after_write` is always
true (daemon/main.cpp:918, :1483), so no second request can ever be served on that socket and
there is no reason to keep reading. Make `queue_web_response` refuse to overwrite a partially
written `outbuf`.

### W4. Every submitter-side job is recorded as failed, and the real compiler exit code is discarded
**Severity: high** (confirmed at the proposed level.) This one needs no attacker and no
unusual configuration; it is simply wrong on every node that submits jobs.
**Where:** daemon/main.cpp:3010, :3191-3192, :5374-5396, :5405, :5434.

`remember_finished_job` stores the value it was handed as `entry.exitcode`
(daemon/main.cpp:3010), and its only caller is `handle_end` (daemon/main.cpp:5194), which is
invoked with daemon-internal teardown codes. The icecc client sends `EndMsg` only when the
remote compile succeeded (client/main.cpp:663-665), producing `handle_end(client, 119)` at
daemon/main.cpp:5434. Every other termination, including all local jobs and failed remote
jobs, ends with a bare socket close, so `get_msg()` returns null and the daemon calls
`handle_end(client, 118)` at daemon/main.cpp:5405. Neither 118 nor 119 is a compiler exit
status. Only jobs this daemon compiles *for other nodes* carry a real code, via
`handle_compile_done`, which reads the child's status at daemon/main.cpp:5105 and passes it at
:5124 — which is exactly why the bug is invisible on pure worker nodes and blatant on
developer workstations.

Meanwhile the authoritative value is being transmitted and thrown away. The fork's own
`JobTimingMsg` carries the real exit code (client/invocation_timing.cpp:123-133) and
`services/comm.cpp` deserialises it, but `Daemon::handle_job_timing`
(daemon/main.cpp:5374-5396) copies submit timestamp, enqueue, start, finish, waitforcs, local
queue, exec, both job ids and the mode, and never touches `m->exitcode`. The `Client` struct
has no field that could hold it, so the value is unrecoverable after that point.

The consequences are visible in three places. `update_insights_history` counts
`if (entry.exitcode != 0) ++bucket->jobs_failed;` (daemon/main.cpp:3191-3192), so on a
submitting node a fully green build renders a "failed" series equal to the total on the
insights chart. The Recent Jobs table's `exit` column shows 118 or 119 for every row, and
because a genuinely failed remote compile also produces 118 (no `EndMsg` is sent on failure)
it is indistinguishable from a successful local job. And the JSONL state dump writes the same
value as `"exitcode"` (daemon/main.cpp:3056), which the bundled `log_extractor` at the repo
root consumes as `ok = exitcode == 0`, reporting zero successes for a fully successful build.

**Fix.** Store `m->exitcode` in `handle_job_timing` alongside the other timing fields, with a
has-value flag. In `remember_finished_job`, prefer the client-reported code when
`has_timing` is set, and record `handle_end`'s parameter in a separate field such as
`end_reason_code`. Count `jobs_failed` from the real exit code, and surface the teardown code
under its own distinct label in the jobs table and the JSONL so the two are never conflated.

### W5. Responses are fully buffered per connection with no size bound and no write deadline
**Severity: medium** (initially proposed high; reduced because `outbuf` is in fact bounded by
the 4096-byte cmdline cap rather than being unbounded, and because the worst case requires a
full 20000-entry history.)
**Where:** daemon/main.cpp:1473-1482, :1508, :1054, :3499-3505, :628-632.

`queue_web_response` serialises the whole response into `conn.outbuf`
(daemon/main.cpp:1482), and it does so by streaming an already-complete body through a
*second* `ostringstream` (daemon/main.cpp:1473-1481), so the body is materialised twice.
`/api/jobs?limit=N` honours N up to `kMaxLimit = 20000` (daemon/main.cpp:1508), which is
exactly `job_history_capacity` (daemon/main.cpp:1054), and the dashboard's own rows selector
offers 20000 as a choice (daemon/main.cpp:1952). Each entry serialises the full command line
(daemon/main.cpp:3119), capped at 4096 bytes by `client_command_line_for_display`
(daemon/main.cpp:628-632), plus the outfile and roughly two dozen other fields.

All of this runs synchronously inside the single `poll()` loop. A faithful micro-benchmark of
`dump_job_history_json` followed by `queue_web_response` over a full 20000-entry deque
produced an 18.5 MB body in about 130 ms at realistic 300-character command lines, and a 91 MB
body in about 770 ms at the 4096-byte cap. That is pure CPU, unaffected by the socket being
non-blocking, and every millisecond of it is time the node is not dispatching compile jobs,
not reading the scheduler and not reaping children. No adversary is required: an operator
picking 20000 in the dropdown on a busy node is enough, and the page re-issues that request
every two seconds (daemon/main.cpp:2274).

The drain path compounds it. `conn.outbuf.erase(0, n)` (daemon/main.cpp:3503) memmoves the
entire remainder on every successful write, so draining a large body costs quadratic memmove
work in the same loop. And because there is no write deadline, a client that requests a large
body and then stops reading leaves it pinned in `outbuf` indefinitely, which with W1's missing
connection cap is a memory-exhaustion vector against a root process.

**Fix.** Lower `kMaxLimit` to something a UI actually renders, in the low hundreds, and remove
the 5000 and 20000 options from the selector; if deep history is genuinely wanted, paginate
with a sequence cursor. Replace the `erase(0, n)` drain with a `size_t outbuf_off` cursor so
the drain is linear. Pass the body into `queue_web_response` by value and `std::move` it,
writing the status line into a small prefix string rather than streaming the whole body
through a second stream. Add the write/idle deadline from W1 so a stalled reader cannot pin a
buffer forever.

### W6. `ICECC_WEB_HOSTPORT` with an empty host fails open to `0.0.0.0` and silently overrides `--webgui-addr`
**Severity: medium** (initially proposed high; reduced because it requires a specific
ambiguous spelling by the operator, the impact is read-only disclosure, and one claimed
aggravator turned out to be fail-*closed*.)
**Where:** daemon/main.cpp:6188-6189, :6202, :1298-1302, :5994.

Two parsers in this file make opposite trust decisions about the same input. The socket setup
helper `parse_webgui_bind_addr` treats an empty address as loopback: `if (candidate.empty() ||
candidate == "localhost") { candidate = "127.0.0.1"; }` (daemon/main.cpp:1299-1302). The
environment parser treats it as all interfaces: `if (web_host.empty()) { web_host =
"0.0.0.0"; }` (daemon/main.cpp:6188-6189). Because the environment path substitutes before
`parse_webgui_bind_addr` ever runs, storing the result at daemon/main.cpp:6202, the safe
default is unreachable for env-configured daemons.

The value that triggers it looks entirely reasonable. `ICECC_WEB_HOSTPORT=":8768"` reads as
"default address, port 8768", a reading the daemon's own diagnostic invites by advertising the
accepted forms as `<addr>:<port> or <port>` (daemon/main.cpp:6198-6199). `rfind(':')` finds
index 0, the host component is empty, and the daemon binds every interface. The only signal is
an informational line stating the resulting address; nothing warns that the requested scope
was widened. I verified empirically that this binds `0.0.0.0` and answers a request arriving
from the host's LAN address.

The block also runs *after* the getopt loop that sets `d.webgui_addr` from `--webgui-addr`
(daemon/main.cpp:5994), so any colon-containing environment value silently overrides an
explicit command-line bind address. Commit 6dce897's title ("add ICECC_WEB_HOSTPORT override")
suggests that precedence may be deliberate, but it is neither documented nor warned about.

Two corrections to the original write-up, both worth stating so the fix does not overreach.
The bare `<port>` form is safe: daemon/main.cpp:6180-6181 preserves the current address when
no colon is present. And the unconditional `d.webgui_best_effort = true` at
daemon/main.cpp:6173 is fail-*closed*, not fail-open: it causes bind and listen failures to
disable the GUI and continue rather than aborting the daemon, which for an observability
feature on a compile node is correct behaviour. The legitimate complaint about it is narrower,
that best-effort semantics are applied even when the environment value is subsequently
rejected as invalid at daemon/main.cpp:6198.

**Fix.** Have the environment path defer to `parse_webgui_bind_addr` instead of
pre-substituting: on an empty host component, keep the existing `d.webgui_addr`. Require
binding beyond loopback to be spelled out explicitly, and emit a `log_warning` naming the
exposure whenever the resolved bind address is not a loopback literal. Do not let the
environment variable override an explicitly passed `--webgui-addr`, or if that precedence is
intended, document it and log when it takes effect.

### W7. No authentication and no `Host` or `Origin` validation on the HTTP surface
**Severity: medium** (initially proposed high; reduced for the reasons below.)
**Where:** daemon/main.cpp:1486-1503, :3455-3493, :3378-3407, :1465-1483.

`parse_http_request` reads only the first line into method, path and version, and discards the
version unchecked (daemon/main.cpp:1486-1503). No other code in `handle_web_connection`
inspects a single header, and the dispatcher branches purely on the route string
(daemon/main.cpp:3455-3493). `handle_web_accept` fills a `sockaddr_storage` from `accept()`
and never looks at it (daemon/main.cpp:3387). There is no `Host` check, no `Origin` check and
no credential of any kind; `grep -c "Authorization\|Access-Control\|X-Frame-Options\|
Content-Security-Policy\|nosniff" daemon/main.cpp` returns 0, and `queue_web_response` emits
exactly four headers (daemon/main.cpp:1474-1478). Verified live: a request with no `Host` at
all returns 200, a request with two conflicting `Host` headers plus a hostile `Origin` returns
200, and `HTTP/9.9` returns 200. RFC 9112 section 3.2 requires a 400 for an HTTP/1.1 request
that lacks a `Host` field or carries more than one.

The certain exposure is that anyone who can reach the port reads the node's job history,
including every `cmdline` and `outfile` (`dump_job_history_json`, daemon/main.cpp:3079, fields
at :3119). With the default loopback bind that means any local user; with W6's `0.0.0.0` bind
it means the whole build network. The conditional exposure is DNS rebinding: because no `Host`
value is validated, a page a developer opens on the build node can, after re-resolving its own
name to 127.0.0.1, read the same data same-origin. Current Chromium blocks this via
Private Network Access, since the required preflight receives a 405 here; Firefox, Safari and
older Chrome do not.

Being honest about what this is *not*: a stronger version of this claim, that the fork
introduces a high-severity unauthenticated root API, does not survive scrutiny, and I am
recording that because it affects how much weight to put on the finding. Upstream's scheduler
already opens an unauthenticated TCP text listener bound to `scheduler_interface` whose
command set includes the *mutating* `removecs`, `blockcs` and `unblockcs`; by comparison this
surface is GET-only against a fixed route allowlist, with no file serving, no path traversal,
no writes and no exec, so the parent's root euid grants an attacker no additional capability
here. And for *live* clients the daemon obtains those command lines from `/proc` itself, which
on a default `hidepid=0` Linux any local user can read with `ps aux`. The genuine incremental
disclosure is retention: `job_history_capacity` is 20000 (daemon/main.cpp:1054), so `/api/jobs`
serves command lines of long-exited jobs that can no longer be reconstructed from `/proc`.
That is real, and it is what makes the missing `Host` check worth fixing rather than a
conformance nit.

**Fix.** Parse the `Host` header and return 400 when it is missing or duplicated, which is the
conformance fix. Separately, and this is the fix that actually stops rebinding, accept only a
`Host` value matching a loopback literal, `localhost`, or the configured bind address, and
reject anything else. Add a loopback peer-address check when the socket is bound to loopback.
Reject requests carrying a cross-site `Origin` or `Sec-Fetch-Site`. If off-box access is
genuinely wanted, require a shared token before permitting a non-loopback bind. Adding
`X-Content-Type-Options: nosniff`, a restrictive `Content-Security-Policy`, `X-Frame-Options:
DENY` and `Referrer-Policy: no-referrer` to `queue_web_response` is cheap and worth doing while
in there.

---

## 3. Unverified / lower confidence

The items below were raised during review but did not go through the same adversarial
refutation pass as section 2. Each is grounded in a line of code I have confirmed exists and
says what is claimed, but the failure scenarios were not reproduced and the severities are the
proposers' own. Treat them as leads.

**Correctness and conformance in the HTTP path.**

- *A complete, already-buffered request is discarded on EOF.* The `POLLIN` branch treats
  `read()` returning 0 as unconditional teardown and `return`s (daemon/main.cpp:3436-3439)
  before reaching the dispatch block at :3447, so a client that writes a full request and then
  calls `shutdown(SHUT_WR)` never receives an answer. That is the shape of an ordinary shell
  health check such as `printf 'GET /api/state HTTP/1.1\r\nHost: x\r\n\r\n' | nc -q1 ...`,
  which would report the GUI as down while it is fine. Suggested handling is to mark the
  connection read-closed and dispatch anything already buffered, dropping immediately only when
  nothing dispatchable is present.
- *Bare-LF requests hang forever.* `parse_http_request` deliberately tolerates bare LF line
  endings (daemon/main.cpp:1488-1491), but the completeness gate accepts only the literal
  four-byte `\r\n\r\n` (daemon/main.cpp:3447). A bare-LF request is therefore never dispatched
  and, given W1, never closed either: sixteen bytes leaks a root-daemon fd while looking like
  legitimate traffic. Scan for the first of `\r\n\r\n` or `\n\n` so the gate matches the
  parser's own tolerance.
- *Non-UTF-8 bytes produce invalid JSON.* `json_escape` handles the named escapes and C0
  controls but passes every byte at or above 0x20 through verbatim (daemon/main.cpp:192-198),
  while responses are labelled `charset=utf-8`. Linux argv and filenames are arbitrary byte
  strings, so a single Latin-1 filename in a checkout corrupts the command line in the UI
  (browsers decode leniently to U+FFFD) and, more seriously, produces JSONL lines that `jq` and
  Python's `json` both reject, because the same escaper builds the state dump
  (daemon/main.cpp:3056 and neighbours). Validate UTF-8 while scanning and substitute a
  replacement character on malformed sequences.

**Telemetry accuracy.**

- *Insights minute buckets mishandle wall-clock steps.* `update_insights_history` keys buckets
  on wall-clock `end_ts` (daemon/main.cpp:3154) and backfills with an unbounded
  `while (next_minute <= minute_ts)` loop (daemon/main.cpp:3159-3164) that runs *before*
  `prune_insights_history` (daemon/main.cpp:3224). An RTC-less node whose clock steps forward
  from epoch to now would allocate on the order of tens of millions of buckets inside
  `handle_end`, on the poll thread. A backward step takes the opposite path and `return`s at
  daemon/main.cpp:3170-3172 before touching any counter, so jobs silently vanish from the
  insights series while still appearing in `/api/jobs`. Clamp the backfill against the
  retention window, and fold too-old entries into the oldest bucket rather than dropping them.
- *Headline cards report the in-progress partial minute.* `dump_insights_series_json` emits
  buckets through `current_minute` inclusive (daemon/main.cpp:3236-3239) and the insights JS
  reads the final bucket for its "latest jobs/min" and average cards, so the cards read near
  zero just after each rollover and every chart's right edge perpetually dips. Read the last
  *complete* minute, or flag the final bucket as partial and render it differently.
- *The dashboard rate card saturates at the fetched row limit.* The "N/min" figure is computed
  in JS over only the rows returned by `/api/jobs?limit=N` and anchored to the newest recorded
  job rather than to now, so a node exceeding the row limit per minute silently caps, and an
  idle node keeps displaying the last active minute's rate. Compute it server-side from the
  insights buckets.
- *Minute drilldown can disagree with the chart that launched it.* The series buckets are
  retained for 120 minutes while the drilldown enumerates the separate 20000-entry job ring
  with a further 5000-row response clamp (daemon/main.cpp:3306-3310), so on a node exceeding
  roughly 167 jobs per minute a click on an old chart point opens a page whose row count
  contradicts the plotted total, with no truncation indicator. Include the bucket's total in
  the drilldown payload and render "showing N of M".

**Dashboard robustness.**

- *Stale data is presented as live.* `refresh()` passes no `AbortSignal` to its three fetches
  and `setInterval(refresh, 2000)` (daemon/main.cpp:2274) fires with no in-flight guard. The
  only failure handling is `catch (error) { setText("meta", "error: " + error); }`
  (daemon/main.cpp:2258-2260), which touches nothing but the meta line, so every card, both
  tables and the green scheduler dot retain their last successful values. The failure that
  matters is the one this dashboard exists to diagnose: if the poll loop wedges, the kernel
  still completes the handshake from the listen backlog, so the fetches neither resolve nor
  reject and even the meta line is never updated. The operator sees a fully populated, green
  dashboard for a dead node. Add an in-flight guard, a fetch timeout, and a visible staleness
  state driven by time since last success.

**Process and fd hygiene.**

- *Compile children inherit every open web socket.* Accepted web fds get `FD_CLOEXEC`
  (daemon/main.cpp:3400), which only takes effect at `exec`. The local-compile child forked in
  `handle_connection` (daemon/serve.cpp) closes only its own pipe end and lives for the whole
  compile job before the grandchild's close-everything loop in `workit.cpp` runs, so each child
  holds a duplicate of `web_listen_fd` and of every web connection open at fork time. The peer
  therefore sees no FIN when `drop_web_connection` runs, and system-wide fd usage is multiplied
  by the number of concurrent children. Close the web fds explicitly in the child branch
  immediately after fork.

**Test coverage.** These are the most actionable of the unverified items, because they explain
why nothing in section 2 was caught before review.

- *The suite runs in no automated context.* `tests/webgui/run_playwright.sh` is invoked by
  nothing: grepping every `*.am`, `GNUmakefile`, `tests/test.sh` and CI YAML for
  `webgui|playwright|run_playwright` finds only the runner itself (the `package_builder`
  scripts mention `tests/webgui` solely to exclude `node_modules` from tarballs).
  `unittests/Makefile.am` declares `TESTS = testargs backpressure`, and both CI systems run
  `make test`, so `make check`, `make test-strict` and CI all stay green no matter what the
  web GUI does. The runner is also CI-hostile as written, requiring live network for
  `npm install` and `npx playwright install chromium`, and a pre-configured tree. This matches
  the fork's established "tests exist but do not run in CI" pattern.
- *The daemon under test never runs a job, so the data assertions are vacuous.* The daemon is
  spawned with `--no-remote -m 0` (tests/webgui/webgui.spec.cjs:82) and nothing ever connects a
  client, so clients, job history and insights buckets are permanently empty. The assertions
  that would inspect real data are guarded by `if (clientsJson.clients.length > 0)`
  (spec:151) and `if (jobsJson.jobs.length > 0)` (spec:162) and can therefore never execute.
  A `json_escape` regression on a command line containing a quote would not be caught, because
  `response.json()` only ever parses empty arrays. The one genuine value assertion on data is
  `expect(jobsJson.capacity).toBe(20000)` (spec:159).
- *The one UI interaction asserts static template text.* `selectOption("#job-limit", "200")`
  followed by expecting `#jobs-sub` to contain "rows" (spec:133-134) is satisfied by the
  shipped template, which contains `<span id="jobs-sub">0 rows</span>`
  (daemon/main.cpp:1954). Deleting the change handler entirely would not fail the test. The two
  insights pages are checked with `request.get` plus a substring match on strings that are
  literal constants in the served HTML (spec:169, spec:189), so the several hundred lines of
  charting and drilldown JS never execute in any test.
- *No adversarial HTTP coverage at all.* Nothing exercises the 400 path
  (daemon/main.cpp:3451), the 405 path (:3453), the 413 path (:3429), the 404 path (:3493),
  `parse_jobs_limit` clamping, idle or half-open connections, fd pressure, or a client that
  stops reading. Every defect in section 2 lives in exactly that unexercised region.
- *The spawn environment is inherited wholesale.* The daemon is started with
  `env: { ...process.env, ICECC_TEST_SOCKET }` (spec:85). Because `ICECC_WEB_HOSTPORT` is read
  after getopt and overrides the flags (W6), a developer who has that variable exported gets a
  daemon bound somewhere else, a 20-second timeout at spec:95, and no diagnostic, since the log
  in the temp directory is neither printed nor cleaned up. Strip `ICECC_*` from the child
  environment apart from `ICECC_TEST_SOCKET`, and dump the log tail on timeout.

---

## 4. What is genuinely good here

This is a substantial, coherent feature and several parts of it are better than they had to be.

**The failure posture of the listener setup is right.** `setup_web_listen_fd` returns
successfully without creating a socket when the GUI is disabled (daemon/main.cpp:1289-1292),
and `webgui_best_effort` causes bind and listen failures to log a warning and disable the GUI
rather than aborting the daemon. For an observability feature bolted onto a compile daemon
that is the correct trade: a port collision or a permissions problem must never cost the node
its compile capacity. This was initially reported to me as a fail-open aggravator, and on
inspection it is the opposite; I want that on the record because it is a deliberate and
correct design decision.

**The defaults are conservative.** Opt-in (daemon/main.cpp:1049), loopback
(daemon/main.cpp:1051), GET-only with an explicit 405 (daemon/main.cpp:3453), a fixed route
allowlist with a 404 fallthrough (daemon/main.cpp:3455-3493), no file serving and therefore no
path-traversal surface, no mutating endpoints, and `Cache-Control: no-store` on every response
(daemon/main.cpp:1476). Compare this with the pre-existing unauthenticated scheduler text port,
whose command set includes `removecs` and `blockcs`: the fork's new surface is strictly the
more conservative of the two. The failures in section 2 are lifecycle and resource-management
failures, not a careless design.

**The socket handling is non-blocking end to end and the input is bounded in the right
places.** Accepted sockets get `O_NONBLOCK` and `FD_CLOEXEC` (daemon/main.cpp:3396-3400), the
write path breaks correctly on `EAGAIN`, `EWOULDBLOCK` and `EINTR` rather than spinning
(daemon/main.cpp:3506), `POLLERR`, `POLLHUP` and `POLLNVAL` are handled explicitly
(daemon/main.cpp:3416-3418), and there *is* a request size cap (daemon/main.cpp:3429) and a
command-line length cap (daemon/main.cpp:628-632). The author clearly understood that this code
must not block the main loop. W3 is a bug in the cap's follow-through, not an absence of the
idea; W1's `created_msec` is a reaper that was designed and then not finished. That is a very
different situation from a design that never considered these problems, and the fixes are
correspondingly small.

**The self-instrumentation is thoughtful.** `note_accept_error` (daemon/main.cpp:1436-1463)
maintains `accept_errors_total`, a dedicated `accept_emfile_errors` counter, the last errno and
its timestamp, and attaches an fd snapshot with the soft and hard `RLIMIT_NOFILE` values. A
five-second log throttle was written deliberately. This is exactly the telemetry you want when
diagnosing a node that has stopped taking work, and the `EMFILE` counter in particular
anticipates the right failure mode. W2 is that one exemption undermines the throttle the same
function implements; the surrounding instrumentation is a genuine improvement over upstream,
which simply calls `log_perror` twice per failed accept with no throttling at all.

**Durations use the monotonic clock.** Job timings are computed from `monotonic_msec`
(daemon/main.cpp:3006-3008), so measured durations are immune to NTP steps and suspend/resume.
Only the insights bucket *placement* uses wall-clock time, which is the correct choice for
minute-aligned buckets that a human reads. The unverified time-handling item above is about
that placement lacking a clamp, not about the wrong clock being used.

**The client-side timing protocol is a real improvement.** `JobTimingMsg` carries submit,
enqueue, start, finish, waitforcs, local-queue and exec timings plus both job ids and the mode,
which lets the daemon distinguish scheduler wait from queue wait from actual compile time. That
is genuinely useful data that upstream does not collect. W4 is the more frustrating for it: the
plumbing to carry the exit code is already built and working end to end, and the daemon simply
forgets to copy one field.

**The dashboard is self-contained and correctly scoped.** No CDN, no external fonts, no
frameworks, everything embedded and served from the daemon. Timeouts and error paths need work,
but the basic structure is sound and the insights and drilldown pages are a real diagnostic
tool rather than a demo.

**The test harness gets the hard part right.** `tests/webgui/webgui.spec.cjs` isolates the
daemon under test with a dedicated `-n playwright-webgui` netname so it cannot discover or
disturb a real scheduler, uses an ephemeral port, and points the daemon at a private socket
path. That isolation is the part people usually get wrong. What is missing is coverage depth
and any automated invocation, both of which are additive.

---

## 5. Prioritized actions

### Before binding anywhere other than loopback

These four are the gate. All of section 2's availability findings become remotely reachable by
an unauthenticated peer the moment the bind address is not loopback, and W6 makes that happen
by accident.

1. **Add a connection cap and an idle reaper** (W1). Cap `web_connections.size()`, read the
   `created_msec` that is already being recorded, enforce a header-read deadline and a total
   lifetime, and clamp `poll_timeout_msec` so the sweep runs. This single change also removes
   the standing trigger for W2 and bounds the memory pin in W5.
2. **Fix the `EMFILE` path** (W2). Back off or drain rather than respinning on an armed
   listener, and remove the `force_log` exemption at daemon/main.cpp:1447 so the failure cannot
   outrun its own diagnostic.
3. **Make the 64 KiB guard terminal** (W3). Close the connection, or at minimum stop arming
   `POLLIN`, once a response has been queued.
4. **Stop `ICECC_WEB_HOSTPORT` from failing open** (W6). An empty host component must mean the
   current default, never `0.0.0.0`; a non-loopback resolved bind must emit a warning naming
   the exposure; and an explicit `--webgui-addr` should not be silently overridden.

### Before proposing this upstream

5. **Fix the exit-code telemetry** (W4). This is the highest-value correctness fix in the
   report and it is small: store `m->exitcode` in `handle_job_timing`, prefer it in
   `remember_finished_job`, and keep the teardown code in a separate field. Until this lands,
   the insights failure series, the jobs table's exit column and the `log_extractor` output are
   all wrong on any node that submits jobs.
6. **Add `Host` validation and decide the authentication story** (W7). Reject a missing or
   duplicated `Host`, allowlist the value against loopback and the configured bind, check the
   peer address when bound to loopback, and require a token before any non-loopback bind is
   supported. Add the four standard response headers while in `queue_web_response`.
7. **Bound the response path** (W5). Lower `kMaxLimit`, drop the 5000 and 20000 rows options,
   replace the `erase(0, n)` drain with an offset cursor, and move the body into `outbuf`
   instead of streaming it through a second `ostringstream`.
8. **Wire the Playwright suite into something automated**, and give it a daemon that has
   actually run a job. A `check-webgui` target that runs when `npx` is available and skips
   loudly otherwise, plus one real compile driven through `ICECC_TEST_SOCKET` in `beforeAll`,
   would convert the currently vacuous data assertions into real ones and would have caught
   W4 directly.
9. **Add raw-socket tests for the HTTP edge cases**: malformed request, non-GET, oversized
   body, half-open client, many idle connections, and a client that requests a large body and
   stops reading. Each one pins a defect from section 2 as expected behaviour so it cannot
   silently return.
10. **Fix the EOF-discards-a-buffered-request and bare-LF gate bugs**, since both make ordinary
    shell-based health checks report a healthy GUI as dead, and the second one leaks an fd per
    probe until W1 is fixed.

### Optional polish

11. Validate UTF-8 in `json_escape` so the JSONL pipeline cannot be poisoned by one oddly named
    source file.
12. Clamp the insights backfill against the retention window and fold too-old entries into the
    oldest bucket instead of dropping them.
13. Give the dashboard an in-flight guard, fetch timeouts, and a visible staleness state, so a
    wedged daemon does not render as a healthy one.
14. Read the last complete minute for the insights headline cards, or mark the trailing bucket
    partial; compute the dashboard's per-minute rate server-side.
15. Report the series bucket total alongside the drilldown rows so a truncated drilldown is
    visibly truncated.
16. Close inherited web fds in the compile child immediately after fork, so
    `drop_web_connection` actually tears the connection down and the fd telemetry reflects
    reality.
