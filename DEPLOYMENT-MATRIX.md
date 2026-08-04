# Mixed-version deployment analysis: o (released 1.4.x) vs x (this branch)

Date: 2026-08-04.  Question: which mixed deployments of the released icecream
("**o**" = the `1.4-branch` line, protocol **43**) and this branch ("**x**" =
`webgui-observability` + the PR #2 backpressure fix, protocol **48**) are
viable across the three roles — **s** = scheduler (1 host), **f** = build farm
(k hosts, remote-capable iceccd), **c** = submitting hosts (m hosts, `icecc`
clients + local `--no-remote` iceccd), with m >> k.

Everything below is grounded either in code read on both trees or in an
empirical run (labelled R1–R5) with the `unittests/schedbp` harness and real
binaries built from both branches.  Empirical runs use the issue-report
conditions (shrunken scheduler send buffers, TCP_USER_TIMEOUT stripped);
"PASS" = all five contract assertions (jobs delivered exactly once & intact,
submitter + CS + control connections survive, scheduler responsive <5s).

## 0. The one fact that decides most of the matrix

**Issue #1 is entirely scheduler-side.**  The nuke happens in the scheduler's
`empty_queue()` send path; the submitter daemon and client are pure victims and
contain no code that can defend against it.  Empirically:

| run | submitter daemons | scheduler | result |
|---|---|---|---|
| R1 | **o** (protocol 43) | **x** | **PASS** — 4000/4000 intact, 0.8s worst latency |
| R2 | o | o (released 1.4) | RED — submitter nuked at 30s, 1408/4000 jobs destroyed, 33.1s scheduler wedge |
| R3 | **x** (protocol 48) | o | RED — identical signature, 33.0s wedge |
| R4 | x | x | PASS |

R2 confirms the released 1.4 line carries the bug (not just 1.4.90 masters).
R3 is the decisive one: **upgrading every daemon and client buys zero
protection while the scheduler is o.**  Corollary: the fix benefit is binary
in s, and only in s.

Note on which channel the bug lives on: the "submitter" the scheduler nukes is
the **c-host's local iceccd** (the daemon that forwards its clients' GetCS
requests), not the farm daemons.  So the fix protects the s↔c-daemon channels;
the f daemons were never the victims.  This is why f's version is irrelevant
to the bug (R1/R3), and it reframes what f@x is actually *for* (§3).

## 1. Wire compatibility foundation (applies to every mix)

* Every channel independently negotiates min(peer versions); all fields added
  in 44–48 are per-hop version-gated with clean defaults on both send and
  receive (verified in `GetCSMsg`, `UseCSMsg`, `JobLocalBeginMsg`,
  `JobDoneMsg`, `LoginMsg` serialization on both trees).  MIN_PROTOCOL_VERSION
  is 21 on both.  Nothing in a 43↔48 pairing can fail to parse; features
  simply degrade to the min of the hops they traverse.
* Protocol deltas that matter here: upstream 44–46 added
  fulljob/local_reason/cmdline (JobLocalBegin) and command_summary (GetCS);
  the branch itself adds 47 = JOB_TIMING (client → local daemon) and 48 =
  local_flags.  Version map: o=43, upstream master=44(–46 gates), x=48.
* Environment tarballs: `daemon/environment.cpp` is untouched by the branch —
  identical formats and defaults; both o and x daemons advertise
  `env_xz env_zstd` (seen live in the R5 daemon log), and the xz/zstd feature
  bits only constrain server choice when a client sets ICECC_ENV_COMPRESSION.
  No env-transfer hazard in any direction.
* Both candidates arm identical kernel liveness settings (keepalive 27s/3s/3;
  TCP_USER_TIMEOUT 9s — 1.4 `services/comm.cpp:979`, x `services/comm.cpp:1058`),
  so mixing versions does not change connection-death behavior.
* What x@s actually changes beyond the fix: the branch's scheduler also
  carries a **scheduling-policy delta** — runtime-estimate-weighted queue
  ordering (`estimate_job_queue_score`, EWMA per-file runtimes with an aging
  bonus; absent upstream).  It feeds off `JobDoneMsg` real_msec stats, which
  protocol-43 daemons already send, so the smarter ordering is fully live even
  with an all-o fleet.  Be aware you are changing dispatch order, not just
  fixing a bug, when you swap s.
* The web GUI is **daemon-side** (`ICECC_WEB_HOSTPORT`, ~4k lines in
  daemon/main.cpp).  s@x adds no web UI; farm observability arrives with f@x,
  and per-developer-host observability (including protocol-47 compile timing)
  arrives only with c@x (client and its local daemon live on the same host and
  upgrade together).

## 2. Config-by-config verdicts

### A. `[c@o f@o s@x]` — upgrade only the scheduler
**Viable: yes.  This is the highest benefit-per-risk step available.**
Empirical basis: R1 (PASS).  The fix is fully effective for the existing
fleet; the estimate-based queue ordering activates immediately (fed by o
daemons' stats); one host changed, revert = swap the binary back (daemons
re-login automatically).  You do not get any web GUI from this step.  Residual
known weakness: the client-side local-fallback fragility (`client/main.cpp`
TODO "daemon internal state gets confused") exists in both o and x clients —
with s@x it simply stops being triggered by scheduler-side nukes.

### B. `[fs@x c@o]` — scheduler + farm
**Viable: yes.**  Everything in A, plus: farm-node web GUIs, upstream daemon
improvements on f hosts, f↔s at protocol 48.  Be clear about what it does
*not* add: nothing for Issue #1 (the victims are c-host daemons — §0), and no
compile-timing observability (protocol 47 is client→local-daemon; c is still
o).  The c@o ↔ f@x compile path (COMPILE_FILE/RESULT at negotiated 43) is the
most-exercised compatibility direction in icecream deployments and is fully
version-gated; my harness does not exercise COMPILE_FILE, so run one real
compile through an upgraded farm node as a smoke test when you take this step.
Rollout cost: k hosts, safely done in waves (mixed f@o/f@x is per-host
independent — each farm daemon is its own scheduler client).

### C. `[cfs@x]` — everything
**Viable: yes — the target state.**  Adds the full observability stack:
compile timing (47) into the c-host daemon web GUIs, local_reason/cmdline/
local_flags for local-build accounting, plus storm-hardening everywhere.
Cost: m hosts.  Mixed c@o/c@x during the wave is fine (each c host is
independent); upgrade a host's client and its local daemon together (one
package) — a skewed host degrades gracefully (AF_UNIX channel negotiates,
timing silently off) but there is no reason to run skewed deliberately.

### D. `[c@x f@o s@o]` — clients first
**Viable in the "nothing breaks" sense (R3: no new failure modes vs all-o) —
but strictly dominated.  Do not choose this order.**  It touches the largest
host population (m) first, delivers zero Issue-1 protection (R3 RED, identical
signature to R2), and the only feature it lights up is per-c-host web GUIs
with local timing.  Its one legitimate occurrence is accidental: if OS-image
or package cadence rolls client hosts before infrastructure.  If that happens,
nothing needs to be rolled back — just prioritize s@x.

### E. `[f@o s@o+x]` — both schedulers co-running (with either c version)
**Viable: yes — and this is the recommended *vehicle* for step A rather than a
binary swap.**  Election mechanics, all verified:

* Preference is (higher protocol, then earlier start).  x=48 always beats
  o=43.
* R5a (empirical): the released-1.4 scheduler correctly parses the
  protocol-48 announce and logs
  `"has announced itself as a preferred scheduler, disconnecting all
  connections"` — it evicts its daemons so they re-home.
* R5b (empirical): the x scheduler receives the 43 announce and does nothing.
  The preference is strictly one-directional; no flapping is possible while
  both live.
* Daemon side (code, both trees ≥33): re-discovery collects all answers and
  picks highest version, tie → longest-running (`DiscoverSched::
  try_get_scheduler`).  Announces repeat every 120s, so any daemon that lands
  on o during an x outage is evicted back within ≤120s of x's return.

Net behavior: bring s@x up next to s@o → the whole fleet migrates to x within
one announce cycle; if s@x dies, daemons fall back to s@o automatically
(builds continue, with the old bug, until x returns).  That is a free
warm-standby HA arrangement with automatic preference for the fixed
scheduler, and an automatic rollback path for new-code risk.  Operational
requirements: **same netname** on both (different netnames = two isolated
farms, no election), and do **not** run s@o with
`--persistent-client-connection` (it suppresses the eviction, freezing
daemons on the buggy scheduler).  Each cutover instant (either direction)
costs the in-flight jobs of daemons homed on the losing scheduler — the same
cost as any scheduler restart; introduce s@x at a quiet moment.  Monitors are
also evicted and must re-discover.

The user-suggested `[c@x f@o s@o+x]` variant inherits E's scheduler dynamics
unchanged; per §0/§2D the c@x part contributes local observability only, so
sequence it after the farm rather than before.

### F. `[s@x f@x c@o+x]` — scheduler + farm new, clients mixed
**Viable: yes — and it is not so much a configuration as the *steady state
of wave C*: config B with the client migration in flight.**  Everything
relevant decomposes per-host, so there is nothing emergent to fear:

* Each c host is an independent (client + local daemon) pair with its own
  channels to s and to f nodes; old and new client hosts never interact
  with each other.  The effective feature level of each host's path is
  min-of-hops, and with s@x and f@x every hop except the c-host itself is
  already 48 — so each host's capabilities are decided purely by its own
  version.  c@o hosts behave exactly as they do in config B (empirical
  basis: R1 — protocol-43 submitters against the x scheduler, PASS);
  c@x hosts behave exactly as in config C (R4, PASS).  The mixed fleet is
  the union of two proven pairings.
* Feature heterogeneity during the wave is per-host and graceful: c@x
  hosts get compile timing (47) into their local web GUIs, richer
  local-build accounting (44–46), and command summaries in scheduler logs;
  c@o hosts keep todays behaviour.  Nothing farm- or scheduler-side keys
  on a uniform client version.
* Scheduling fairness is version-blind: the estimate-based queue scoring
  keys on file names and JobDone statistics, which both generations
  supply identically; niceness (43) is supported by both, so there is no
  skew between old and new client hosts.  (Job-size fairness is a
  separate axis: the original capped age bonus could starve short jobs
  under a sustained stream of long ones — fixed by making the age bonus
  unbounded, so every queued request eventually outranks any newcomer.)
* The Issue-1 fix protects **both** generations equally (it is
  scheduler-side, and R1 proves the o-daemon case), including the new 30s
  deferred-output cap (§4), which judges daemons by behaviour, not
  version.
* Upgrade mechanics: move each c host's client and local daemon together
  (one package); a transiently skewed host degrades gracefully (the
  AF_UNIX channel negotiates down; timing simply stays off) but there is
  no reason to run skewed on purpose.

Operationally this is the configuration you will actually live in for
however long the m-host wave takes — days or weeks — and it is safe to
pause indefinitely at any mixture, including as a de-facto end state if
some c hosts can never be upgraded (build appliances, pinned images).
The only cost of lingering is heterogeneous observability: timing and
local-reason data exist only for the upgraded subset, so fleet-wide
dashboards undercount until the wave completes.

## 3. Recommended sequence

1. **E now**: start s@x alongside s@o (same netname, no persistent-clients
   flag).  Fleet auto-migrates; automatic fallback if the new scheduler
   misbehaves.  Watch for a week of real load: scheduler log for
   `flush_pending` teardown lines, absence of the
   `expected use_cs reply, but got UNKNOWN` storms, and dispatch-order
   changes from the estimate-based queue.
2. **B next (waves)**: f@x across the k farm hosts for farm web GUIs +
   upstream daemon fixes; smoke one real remote compile per wave.
3. **C last (waves)**: c@x across the m hosts for timing + full
   observability.  This is also the point the storm-*fragility* (client
   fallback path) stops mattering entirely, because both its trigger and its
   victims are now current.
4. Retire s@o or leave it as permanent warm standby (E is a legitimate end
   state, not just a transition).
5. Skip D as a deliberate configuration; treat it as a harmless accident of
   packaging order if it occurs.

## 4. Bounds worth knowing when running s@x

With s@x, dispatch replies to a stalled c-host daemon queue in scheduler
memory instead of wedging the loop: bounded in size by that submitter's
outstanding jobs (~60–100 B per reply; 4000 jobs ≈ 300 KB) — the request
count is client-driven and effectively bounded by the fleet's concurrent
compile jobs.  Time-bounded twice over: kernel TCP_USER_TIMEOUT (9s, both
candidates arm it, but `#ifdef`'d and platform-dependent) for truly dead
peers, and an **application-level 30-second cap** on deferred-output age,
enforced by `prune_servers()` from a monotonic clock (wall-clock steps can
neither disable nor mis-fire it) with the poll timeout re-capped after
dispatch so the deadline cannot silently stretch to the loop's 36s
ceiling.  Additionally, a submitter whose channel holds undelivered
dispatch output receives **no further assignments** until it drains — so
a stalled submitter's slot reservation is bounded by the kernel socket
capacity between the two hosts (tens of KB, i.e. one to a few thousand
replies), not by the depth of its request queue.  Verified by the
harness's stall mode: assignments stop at ~2.6k of 4k queued requests and
teardown lands at t≈31s with the scheduler responsive throughout.

## 5. Reproduction

```
# R1/R2/R3: schedbp compiled against each tree's libicecc (the MSG_IS shim in
# unittests/schedbp.cpp makes it build against 1.4's pre-46 Msg API), pointed
# at either scheduler binary:
schedbp_<tree> <either icecc-scheduler> sndbuf_shim.so 4000 75
# R5: two schedulers, shared netname, ICECC_TESTS=1
# ICECC_TEST_SCHEDULER_PORTS=8767:8769; watch for the
# "preferred scheduler, disconnecting" line on the loser only.
```

## 7. Real-machine stress results (2026-08-04)

Every configuration was exercised on real hardware: nas642 (32-core,
scheduler(s) + unprivileged c-role daemons + one Docker farm container) and
research6 (20-core, native root iceccd run from /dev/shm), both on the same
LAN.  Workload: 400 template-heavy C++ TUs (~1.2s each), real `icecc`
clients, real environment tarball transfer into the farm's chroots, `make
-j40` (F: 2×`-j20` concurrently; G: `-j400`).  Farm-role root came from
Docker on nas642 (default container caps suffice for iceccd's chroot) and
sudo on research6 — no host ever needed root outside a container except
research6's daemon itself.

| config | result | wall | distribution (jobs) | errors |
|---|---|---|---|---|
| A `[c@o f@o s@x]` | PASS 400/400 | 32.3s | r6:235 nas:136 local:29 | none |
| B `[c@o f@x s@x]` | PASS 400/400 | 32.9s | r6:224 nas:145 local:31 | none |
| C `[cfs@x]` | PASS 400/400 | 34.8s | r6:249 nas:128 local:23 | none |
| D `[c@x f@o s@o]` | PASS 400/400 | 30.8s | r6:220 nas:151 local:29 | none |
| E dual s@o+x, kill+restart s@x mid-build | PASS 400/400 | 45.7s | via x:378, rest local during blip | 6 transient reconnect warnings, 0 failures |
| F `[s@x f@x c@o+x]` two concurrent builds | PASS 2×400/400 | 57.9s/58.7s | farm:717 local:83 | none; walls within 1s — no version bias |

Config E detail: all three daemons (including the Docker one) discovered
both schedulers over real LAN broadcast and elected s@x (protocol 48);
killing s@x 25s into the build failed the fleet over to s@o, restarting it
45s in evicted them back — the build absorbed the double migration for
+13s of wall time and zero lost TUs.

G-series (issue-conditions probes with the sndbuf/rcvbuf shim on the
scheduler and the c-daemon, SIGSTOP-freezing the entire submitter daemon
mid-build):

* G1 (12s freeze, s@x): a genuine dispatch deferral engaged and drained on
  thaw — 400/400, scheduler probe worst 0.22s throughout.
* G2 (45s freeze, s@x): 400/400, wall = build + freeze duration, zero
  failed TUs, zero storm — clients simply waited out their frozen local
  daemon and the farm pipeline resumed.  (Note: G2 is *recovery* evidence,
  not deadline evidence — healthy kernel buffers self-limited before
  sustained dispatch backpressure could arm the 30s bound; the bound
  itself is exercised by the schedbp stall mode, where buffers are
  deliberately shrunk.)
* A structural finding worth recording: with healthy kernel buffers a
  *single* frozen submitter cannot jam the dispatch channel for long —
  reply backlog self-limits at roughly 2× farm slots × ~60 B (running
  jobs finish once, their successors' clients never hear the UseCS that
  was swallowed by the frozen daemon, so slots wedge and dispatch stops
  growing).  The sustained 30s-timeout regime of issue #1 therefore
  requires collapsed socket memory (the report's swap-thrashing hosts) or
  very large aggregate slot counts — precisely the conditions the
  `unittests/schedbp` harness models with shrunken buffers, where the
  base-vs-fixed differentiation is proven.  On healthy fleets the fix's
  value shows as G1/G2's zero-loss rides through daemon stalls, plus the
  30s bound as the platform-independent backstop.

tt-quietbox was unreachable during this campaign (no key trust from
nas642/research6 for ttuser/mickg); to include it in a future round, add
nas642's mickg key to the target account's authorized_keys.
