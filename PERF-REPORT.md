# Performance review: scheduler + end-to-end distributed build

Date: 2026-08-04, branch `fix/scheduler-deferred-dispatch-send`.  Methods:
`perf record -F 1999 -g` on the scheduler under a synthetic dispatch flood
(the `unittests/schedbp` harness), dispatch-rate timelines from scheduler
logs, and a real two-machine farm (configuration C: 400 template-heavy TUs,
~1.2s each) with a syscall-level phase trace of a single cross-machine
remote compile.

## 1. Where time goes end-to-end (real farm, per remote TU)

Phase trace of one TU compiled on the remote farm host (strace `-ttt` on the
client, job confirmed dispatched cross-machine), total 1.250s:

| phase | time | share |
|---|---|---|
| client startup + local-daemon handshake | 16 ms | 1.3% |
| **scheduler round (GetCS → UseCS)** | **4 ms** | **0.3%** |
| local preprocess + upload (163 KiB preprocessed) | 196 ms | 15.7% |
| remote compile on the farm host | 1031 ms | 82.5% |
| object download (4 KiB) + write + exit | 6 ms | 0.5% |

Interpretation: the compiler dominates, as it should.  Distribution
overhead is ~0.22s/TU and is almost entirely preprocess+upload, which
pipelines across concurrent jobs.  The scheduler contributes 4 ms per job
in the request path — at build scale it is invisible:

| build (400 TUs) | wall |
|---|---|
| plain local `g++ -j12` (baseline) | 44.2 s |
| farm, cold daemons (env tarball installs included) | 27.4 s |
| farm, warm | 26.7 s |

Environment installation (the one-time ~30 MB tarball per farm daemon) is
fully absorbed by parallelism: cold vs warm differs by < 1 s.  Speedup with
just k=2 farm hosts is 1.65×; the ceiling here is farm CPU (30 slots), not
any icecream component.

## 2. Scheduler under dispatch flood (the hot path that matters)

Wide-open throughput was measured by flooding the scheduler with N
requests from one submitter over loopback with the shim active
(`schedbp N 10`) and profiling from spawn.

**Finding: the branch's estimate-based queue scoring makes each dispatch
O(queue-depth), so a flood is O(n²).**  `get_first_job_request()` rescans
every queued job (scoring each) for every single dispatch.  At the 4,000
jobs of the stress runs this is harmless (worst control-reply latency
0.7 s).  At 20,000 queued jobs it dominated completely:

* Dispatch rate climbs as the queue drains — the O(depth) signature:
  ~600/s at depth 20k → ~3,600/s near empty (before optimization).
* The main loop's `while (empty_queue()) …` drains the entire queue
  before returning to `poll()`, so during the flood the scheduler served
  nothing else: worst control-reply latency **14.1 s**.
* perf (before): `get_first_job_request` 19.5%, and ~22% more burned in
  string machinery it drives — `Job::fileName()` **returned by value**
  (a heap copy per score), the `file_runtime_estimates` map lookup on
  that fresh string, `std::string` construct/allocate, plus
  `time(nullptr)` per scored job (1.8% in vdso).

### Optimizations applied on this branch (commit with this report)

1. `Job::fileName()` returns `const std::string&` (was by-value).
2. `estimate_job_queue_score(job, now)` takes the clock from the caller;
   `get_first_job_request()` reads `time()` once per scan instead of once
   per job.

Effect at 20k flood: flood duration ~13 s → ~10 s, dispatch rate at full
depth ~600/s → ~1,540/s, worst control latency 14.1 s → 10.8 s, and the
string-construction cluster vanished from the profile (after: the scan
itself 23.3% of a smaller total, then the estimates-map lookup and Job
accessors).  The 4,000-job contract improved to 0.7 s worst latency; `make
check` and both schedbp modes stay green.

### Follow-ups (updated after the divergence review)

The drain-before-poll half is now fixed: dispatch runs in batches of 128
and re-polls with a zero timeout between batches, so control traffic is
serviced throughout a flood instead of waiting for the full drain.  The
scan itself is still O(depth) per dispatch; if queues beyond ~10k
outstanding requests become a real operating point, replace the linear
rescan with an incremental structure (max-heap keyed on score with lazy
aging, or a cached per-group maximum invalidated on enqueue).  For the
fleet sizes exercised here (≤ 4k outstanding) the batched behavior is
fine.

## 3. Cost of the backpressure fix itself

Nothing from the fix appears in the profiles at any measurable level:

* The deferral bookkeeping (`pending_write_since`, the `deferred` flag)
  executes only on the EAGAIN/timeout paths and on buffer-drain
  transitions — zero samples attributed.
* The unconditional compaction adds a `memmove` only when a flush exits
  with bytes still pending (i.e., only under backpressure).  At the
  measured operating point that was a few hundred bytes; it is now also
  structurally bounded, because dispatch pauses for a submitter with
  deferred output, capping any channel's backlog at roughly one kernel
  socket capacity.  The `__memmove` samples in
  the flood profile (3.8%) are the pre-existing message-buffer management
  (`chop_input`/`writefull` on 20k inbound requests), present before the
  fix as well.
* `prune_servers()` gained one subtraction and compare per daemon per
  loop iteration.
* The 30 s deferred-age bound adds no work while nothing is deferred
  (`pending_write_since == 0` short-circuit).

## 4. Logging

All measurements ran at `-vvv` (worst case).  Even during the 20k flood,
log I/O (`__GI___libc_write` + fs writeback) totalled ~3% — logging is not
a scheduler bottleneck even at maximum verbosity, though production should
still run at default verbosity for log volume reasons.
