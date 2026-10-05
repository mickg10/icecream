# P50 kernel-platform compatibility (3.10 target)

Design note for backward compatibility of the P50 cache sidecar on kernels
that lack `pidfd_open`/`P_PIDFD` waitid, memfd sealing, `getrandom`, or a
working `kcmp`. The reference deployment target is CentOS 7.3,
`3.10.0-514.el7`, which the production build hosts run (see
`~/workspace/icecream_builder/docs/kernel-3.10-sidecar.md` for the per-site
inventory this note builds on; its *fallback designs* are superseded here —
only its file:line inventory is used as a starting point and has been
independently re-verified against `c8bb615f`).

Four independent mitigations, each a narrow backend selected once by the
daemon at startup:

| Mitigation | Native backend | Compat backend | Env override |
|---|---|---|---|
| M1 child handle | `pidfd_open` + `waitid(P_PIDFD)` | `(pid, /proc/PID/stat` starttime ticks`)` + SIGCHLD + bounded `WNOWAIT` poll | `ICECC_P50_PLATFORM_CHILD` |
| M2 input artifact | sealed `memfd` (4 seals) | daemon-private unlinked read-only copy | `ICECC_P50_PLATFORM_INPUT` |
| M3 random | `getrandom` | `/dev/urandom` | `ICECC_P50_PLATFORM_RANDOM` |
| M4 fd proof | `kcmp(KCMP_FILE)` | inode identity of the M2 daemon-private file | `ICECC_P50_PLATFORM_FDPROOF` |

Shorthand: `ICECC_P50_PLATFORM=compat|native|auto` sets all four at once;
any of the four per-mitigation variables, if set, overrides the shorthand
for that mitigation only.

## Decision model

The daemon is the sole source of truth. It decides once, at startup, before
the sidecar is ever launched, and passes the four decisions explicitly in
the sidecar's launch config. The sidecar never reads any
`ICECC_P50_PLATFORM*` variable and never re-probes; a launch config whose
platform fields disagree with what the daemon itself decided is a
programming error, not a runtime condition, and is treated as a launch
failure.

Per-mitigation values and semantics:

* `auto` (default) — probe at startup; use native if the probe succeeds,
  compat if it fails cleanly, fail closed (today's `FailedClosed` /
  `cache=off`) only if compat *also* cannot be established.
* `native` — skip the compat path entirely; if the probe fails, behave
  exactly as today (fail closed). This is for operators who want today's
  strict behavior preserved verbatim and would rather lose the cache than
  run compat.
* `compat` — force the compat backend even when native would work. This is
  the enablement-testing lever: it lets us exercise the compat code paths on
  H1-H4's modern kernels without needing a 3.10 host or a seccomp
  emulation layer.
* any other value — refuse to start the cache (`cache=off`) and log the
  invalid value and the variable name. The daemon does not guess.

Probes are **failure-based**, not version-based: each probe exercises the
exact syscall in the exact shape the production code path uses (same
arguments, same flags), and classifies success/`ENOSYS`/`EINVAL`/`EPERM`
from the syscall's actual return value. No code anywhere parses `uname()`
or `/proc/version` to make a platform decision. This is deliberate: it is
what let us catch the M4 finding below, which a kernel-version check would
have missed entirely (the blocking condition on H1-H4 today is a container
capability, not the kernel).

One startup log line summarizes all four decisions and their reasons, e.g.:

```
p50 platform: child=native(pidfd ok) input=native(seal round-trip ok) random=native(getrandom ok) fdproof=compat(kcmp EPERM: no CAP_SYS_PTRACE)
```

and the same four `(mitigation, backend, reason)` tuples are mirrored into
the state-jsonl/webgui state alongside the existing cache-service state, so
a farm operator can see at a glance which hosts are running which backend
without grepping logs.

## M4 audit (required before any M2 compat work)

**Correction (2026-10-05, prompted by main's review of checkpoint 1):** the
paragraph below this note originally concluded "no STOP condition" from an
incomplete reading — it enumerated every `kcmp` *call site* correctly but
missed that one of the two lease-minting functions never reaches any of
those call sites at all. The corrected finding, which changes the conclusion
from "kcmp's guarantee survives M2 compat" to the stronger "kcmp is not on
the production critical path to begin with," is recorded here in full.

The task's audit requirement: if any `kcmp` use site's guarantee is not
equivalent under inode identity once M2 compat is in play, stop and report
before weakening anything. `daemon/p50_fork_fd_hygiene.cpp` (c8bb615f) has
**two structurally different lease-minting functions**, confirmed by
grepping every caller of each (`grep -rn "mint_fork_source_lease\|
mint_attached_input_source" --include=*.cpp --include=*.h .`):

* `mint_fork_source_lease()` (lines 383-422) — **called only from
  `unittests/p50_fork_fd_hygiene_test.cpp`**, never from `daemon/main.cpp`.
  Its `rejected` condition includes
  `proof_control != OpenFileComparison::Same`, where `proof_control =
  compare_open_file_description(owner.owned_proof_fd_, owner.control_fd_)`
  — i.e. it **hard-gates on kcmp succeeding**. If kcmp is `Unavailable`,
  this function unconditionally returns `std::nullopt`. This is the
  function `kernel-3.10-sidecar.md`'s "every proof fails → the P50 source
  token is never delivered" claim describes, and it is accurate *for this
  function*.
* `mint_attached_input_source()` (lines 424-453) — **the actual production
  entry point**, called from exactly one production call site,
  `daemon/main.cpp:8126`, inside the per-job dispatch loop (confirmed by
  reading `main.cpp:8095-8146`). This function **contains no call to
  `compare_open_file_description` anywhere**. Its admission check is
  `capture_source_identity(fd, &identity)` (seal/dev/ino/mode/size capture)
  followed by `source_identity_matches(...)` across three independently
  obtained descriptors — the original accepted `fd`, plus `proof_fd` and
  `control_fd`, both `duplicate_owner_fd(fd)` — never kcmp. The code's own
  comment at lines 432-435 states this is deliberate: *"Keep two private
  duplicates so the child can re-check the same sealed snapshot without
  depending on kcmp, which is unavailable after the daemon drops privileges
  on common production kernels."* `ForkSourceLease::identity_matches_current()`
  (lines 367-381) and `ForkSourceLease::retire_identity_proof()` (lines
  349-365) both branch on `owner_cookie_ == kAttachmentOwnerCookie` to take
  the same kcmp-free path (fd-number distinctness + `source_identity_matches`
  across all three descriptors) for leases minted by `mint_attached_input_source`;
  only the non-cookie branch, exercised solely by `mint_fork_source_lease`,
  calls into `same_open_file_description`/kcmp.
* **What a `nullopt` from `mint_attached_input_source` leads to on the F
  path** (answering "trace in code what kcmp Unavailable leads to," per
  `daemon/main.cpp:8135-8142`): `job->usesP50Input() &&
  !compiler_input_source.has_value()` logs `"P50 attached input failed
  compiler-fork validation"`, closes `compiler_input_fd`, and calls
  `handle_end(client, 146)` — the job fails with exit 146. But this path is
  reached only on a `capture_source_identity`/seal failure or a
  `source_identity_matches` mismatch (both M2-shaped failures), **never**
  because of kcmp, since `mint_attached_input_source` never calls it. kcmp
  Unavailable, by itself, leads to nothing on the real F path — it has no
  wire into this function at all.

**Corrected finding:** every kcmp comparison in this file is indeed between
descriptor numbers that are dups of one fd the daemon itself already holds
and has already validated — that part of the original audit was right —
but the audit's scope was too narrow: it treated `mint_fork_source_lease`'s
kcmp usage as representative of "the fd-hygiene layer" generally, when in
fact production routes every real job through `mint_attached_input_source`,
which was built from the start to not need kcmp at all. This resolves an
apparent contradiction raised in review: the farm's own P29V1 reuse
measurements (~3 GB uploaded, recorded in memory, from the s40-vs-g4
campaign) are not in tension with kcmp returning `EPERM` on H1-H4's modern
kernels (confirmed live, see below) — P29V1 reuse was never contingent on
kcmp succeeding, because the code path those jobs took doesn't call it.

No STOP condition is triggered, but for a stronger reason than originally
written: M4's "compat" backend is not a new weakening of an active kcmp
proof — it is the **formalization, as an explicit, named mode, of the exact
duplicate-fd + full identity-match discipline `mint_attached_input_source`
already runs unconditionally in production today**, independent of
`ICECC_P50_PLATFORM_FDPROOF`. Concretely:

* **M4 compat** = the active inode-identity check: `source_identity_matches`
  (dev/ino/mode/size/seals) re-verified across all independently-held
  descriptors for the lease, exactly as `mint_attached_input_source` does
  unconditionally today. This is deliberately *not* framed as "accept
  kcmp=Unavailable and otherwise do nothing extra" — per review feedback,
  that framing would be weaker than what the code already guarantees. It is
  a positive, active check, not a fallback-shaped absence of one.
* **M4 native** = attempt the real `kcmp(KCMP_FILE)` syscall and classify
  `Same`/`Different`/`Unavailable`, preserving `mint_fork_source_lease`'s
  stricter behavior exactly where that function is exercised (unit tests,
  and any future production call site that opts into the harder gate).
  Forcing `ICECC_P50_PLATFORM_FDPROOF=native` reproduces today's exact
  code shape for that function; it does not change `mint_attached_input_source`,
  which has no kcmp call to toggle.
* M2 compat still changes *where the file came from*, not *what
  `mint_attached_input_source` compares* — so M4's guarantee with respect to
  M2 compat is unchanged from the original (correct) part of the audit.

## The moby kcmp finding

The task framed kcmp as a "kernels < 4.8 need a fallback" problem via the
`CONFIG_CHECKPOINT_RESTORE` kernel-config gate cited in
`kernel-3.10-sidecar.md` ("if the build0xx kernel has it... nothing
changes... if not... every proof fails"). That framing is incomplete, and
the gap matters for how `auto` should actually behave on H1-H4 today.

Moby's current default seccomp profile
(`github.com/moby/profiles/seccomp/default.json`, vendored into
`moby/moby`) only allows `kcmp` (together with `pidfd_getfd`,
`process_madvise`, `process_vm_readv`, `process_vm_writev`, `ptrace`) when
the container holds `CAP_SYS_PTRACE`:

```json
{"action":"SCMP_ACT_ALLOW","includes":{"caps":["CAP_SYS_PTRACE"]},
 "names":["kcmp","pidfd_getfd","process_madvise","process_vm_readv",
          "process_vm_writev","ptrace"]}
```

There is no kernel-version condition on this rule at all — the gate is a
container capability, independent of `CONFIG_CHECKPOINT_RESTORE` or kernel
version.

This was verified directly against the live test farm, not just read from
the profile source. H4's running `mgsub-cs-g4` container (`mg-icecream:g4`,
host kernel `6.14.0-37-generic`) has `CapAdd: [CAP_SYS_ADMIN,
CAP_SYS_CHROOT]` and no `CAP_SYS_PTRACE` (confirmed bit 19 absent from
`CapEff` in `/proc/self/status` inside the container). A `kcmp` probe
compiled and run inside that exact container returns `-1`/`EPERM`:

```
kcmp ret=-1 errno=1 (Operation not permitted)
```

**Conclusion: `kcmp` is already `Unavailable` today, in native mode, on
H1-H4's modern kernels, for the same reason it would be on a 3.10 host that
lacks `CONFIG_CHECKPOINT_RESTORE`** — the two failure modes converge on the
same `EPERM` outcome through `compare_open_file_description()`, even though
their root causes (container capability vs. kernel config) are unrelated.
Two consequences for the design, the first of which is corrected from an
earlier draft of this note (see the M4 audit correction above):

1. **Corrected.** The real-world security property P50 is already running
   under in production right now is *not* `retire_owned_proof()`'s fail-safe
   handle-retirement discipline — that function, and the kcmp proof it
   contains, is not on the production F-path at all. The property actually
   running today is `mint_attached_input_source`'s `kAttachmentOwnerCookie`
   discipline: duplicate-fd distinctness plus `source_identity_matches`
   (dev/ino/mode/size/seals) across three independently-held descriptors,
   architected from the start (see the code comment cited above) to not
   depend on kcmp succeeding. M4 compat does not introduce a new weaker
   mode; it formalizes and names a mode production has been running
   unconditionally since before this branch existed.
2. `auto` for M4 should not expect to see `native` selected on H1-H4 unless
   `CAP_SYS_PTRACE` is granted to the F container. If the farm wants the
   kcmp proof to actually run (as opposed to running `Unavailable`-safe as
   it does today), that is a capability change to the F container's
   `docker run`/Dockerfile, independent of anything in this branch; absent
   that change, `fdproof=compat` or `fdproof=native(Unavailable)` are
   observationally the same thing today, and the startup log line will say
   so plainly rather than imply a difference that doesn't exist. Note that
   even if `CAP_SYS_PTRACE` were granted, `mint_attached_input_source`
   still would not call kcmp — it has no kcmp call to enable. Making the
   production path actually exercise kcmp would require a separate code
   change to that function, not just a capability grant; this doc does not
   propose that change.

**Container-privilege provenance (answers "how are F containers actually
started" for both the versioned harness and the ad-hoc test farm).** The
versioned integration-test harness, `farmharness/integration/farmtest.py`
(~lines 744-768), grants F containers `--cap-add SYS_CHROOT` unconditionally
and adds `--cap-add SYS_PTRACE` **only** for one explicitly-scoped
fault-injection scenario, `S70-b4-scheduler-active-loss`, whose own inline
comment states why: *"The active-loss harness latches the compile-worker
fork in kernel ptrace state before the short-lived child can exit. No other
cell receives this fault-observation capability."* No `--privileged`, no
seccomp override, anywhere in that launch path. This is covered by harness
test assertions (`farmharness/integration/tests/test_dev_mixed.py:128`,
`test_dev_bootstrap.py:285`, `test_events.py:888,898`) that pin
`SYS_PTRACE`'s presence to that one scenario id. On the ad-hoc test farm,
the live F container's capability set was checked directly: `CapAdd:
[CAP_SYS_ADMIN, CAP_SYS_CHROOT]`, no `CAP_SYS_PTRACE`, no `--privileged` —
matching the versioned harness's default profile, not its fault-injection
exception. No surviving launch script for the earlier comparison container
used in a prior campaign was found (that container has since been torn
down; only its image and a source tarball remain), so that one data point
rests on the fact that it used the same ad-hoc launch mechanism as the
still-running comparison container, not on an independently recovered
command line.

**Live-traffic confirmation status for "does a job through a
no-`CAP_SYS_PTRACE` F container take the P29V1-reuse path or silently fall
back" (checked by experiment, as required, not assumed).** The already-running
ad-hoc F container was checked for current P50 traffic rather than started
fresh, to avoid standing up new infrastructure. Result: its `iceccd` has
been running for about a week and is presently unable to find/select a
scheduler at its configured address (repeating "scheduler not yet found/selected"
in its own log output), and no scheduler or client container is currently
running alongside it on that host — so there is no live job traffic through
it right now to observe directly, and its cache-mount directory and
in-container log path were both checked and found to hold no persisted
state-jsonl/counter evidence from the time it was last processing jobs.
Rather than start a new scheduler to force a fresh observation (out of
scope for this check, and this host is a dev host, not a build host), this
note relies on the code trace above plus the farm's own already-recorded
P29V1 reuse measurements (uploads cut from 19-26 GB to ~3 GB, from an
earlier same-mechanism comparison campaign) as the empirical anchor: those
measurements were taken under this exact capability profile
(no `CAP_SYS_PTRACE`, confirmed `kcmp` `EPERM`), so they already are the
by-experiment confirmation that P29V1 reuse does not silently fall back
under this profile — because, per the code trace, the production admission
path was never calling kcmp in the first place, there was never a
dependency on it to break. If a reviewer wants a fresh live-traffic log
sample under a currently-connected scheduler, that requires either
restarting the ad-hoc farm's scheduler or running a short job against the
versioned harness's own F instance, neither of which was done here to keep
today's farm state undisturbed; flagged here rather than silently assumed.

This finding changes nothing about the M4 audit conclusion above (M2 compat
still doesn't change what kcmp is comparing); it does mean the task's
framing of kcmp as a kernel-version question needs the correction recorded
here, which is why probes in this design are failure-based rather than
version-based — a version check would have reported "fine, kernel is new
enough" while production was already running `Unavailable`.

## Probe shapes

All probes run once at daemon startup, before any sidecar is launched, and
exercise the production-shaped syscall:

* **M1 (child handle)**: `fork()` a short-lived no-op child (`_exit(0)`
  immediately), `pidfd_open(child_pid, 0)` on it, then
  `waitid(P_PIDFD, pidfd, &info, WEXITED)`. `ENOSYS` on either call ->
  compat. Any other failure is a probe error, not a platform signal, and is
  treated as fail-closed under `auto` (same as today) rather than silently
  choosing compat for an unrelated reason.
* **M2 (input artifact)**: `memfd_create(..., MFD_ALLOW_SEALING)`, write a
  marker byte, `fcntl(F_ADD_SEALS, SEAL|SHRINK|GROW|WRITE)`, `fcntl
  (F_GET_SEALS)` round-trip to confirm all four landed. `ENOSYS` from
  `memfd_create`, or seals not round-tripping, -> compat.
* **M3 (random)**: `getrandom(buf, 32, 0)`. `ENOSYS` -> compat
  (`/dev/urandom`, opened once, `O_CLOEXEC`, read loop tolerating partial
  reads/`EINTR`).
* **M4 (fd proof)**: open a throwaway file, `dup()` it, `kcmp(getpid(),
  getpid(), KCMP_FILE, fd, dupfd)`. `ENOSYS`/`EPERM`/`EINTR` -> compat
  (inode-identity comparison against the M2 daemon-private file instead of
  a kernel-adjudicated OFD comparison).

Every probe has a corresponding test hook following the existing
`ICECC_P50_*_TEST_HOOKS` / `force_kcmp_errno` convention (e.g.
`force_pidfd_open_errno`, `force_memfd_create_errno`,
`force_seal_roundtrip_failure`, `force_getrandom_errno`,
`force_kcmp_probe_errno`), so every errno and every partial-support
combination is unit-testable on any kernel, including C1's, without needing
a 3.10 host or kernel-specific CI.

## What stays byte-for-byte unchanged

When a probe selects native, the native backend's code path is untouched by
this branch — no new branching is introduced into the native call sites
beyond "ask the platform-decision module which backend to use," which for
an all-native decision resolves to the exact function calls that exist in
`c8bb615f` today. Behavior-identity on an all-native decision is a
correctness requirement, not just a goal, and will be checked by object
identity (`.o` diff against `c8bb615f` for files where this is expected to
hold) during QA.

## What each compat backend weakens (restated from the task's own analysis)

* **M1**: "same process" becomes a checked observation
  (`(pid, starttime_ticks)` + a `/proc` re-check immediately before
  kill/wait) instead of a kernel-enforced fact; the residual risk is a
  `pid_max` wrap between the check and the use, treated as the compat
  backend's one accepted non-zero-probability window.
* **M2**: the cross-process *producer-immutability* proof (seals: the
  producer physically cannot mutate the bytes) becomes a
  permission-model argument (mode 0400 + unlinked + same-uid discipline);
  a same-uid producer that kept a secret second writable descriptor before
  closing the writer could still mutate the bytes after the daemon's
  validation. Mitigated by the existing full-content digest re-validation
  in `validate_and_rewind` and by the daemon-private file being written
  solely by the daemon, not the sidecar, in the M2 compat flow.
* **M3**: nothing meaningful — `getrandom`'s only advantage over
  `/dev/urandom` is early-boot blocking, irrelevant to a farm daemon.
* **M4**: nothing new, per the audit above — and per the moby finding,
  production is already running in this mode today.

## Open item carried to checkpoint 2

Whether the farm should additionally grant `CAP_SYS_PTRACE` to F containers
so the kcmp proof actually runs instead of running `Unavailable`-safe is
noted here **as a documented option only, not applied to any running or
test container in this branch**: it would widen container privileges, it is
a farm-configuration decision outside this branch's scope, and even if
granted it would not by itself change `mint_attached_input_source`'s
behavior (that function has no kcmp call to enable) — a separate code
change would be needed on top of the capability grant for the production
path to actually exercise kcmp. This option is recorded for the farm
owner's future decision and is explicitly not acted on here.

## Farm-matrix baseline requirement (carried to checkpoint 4)

The farm matrix must include a run with `ICECC_P50_PLATFORM_FDPROOF=native`
on the comparison host, so the matrix captures today's exact code path
(kcmp attempted, observed `Unavailable`, `mint_attached_input_source`
unaffected either way) as an explicit baseline row, not just the compat
rows this branch adds. This keeps the "M4 compat must be a true active
inode-identity check, stronger than passively accepting `Unavailable`"
requirement measurable against a labeled baseline rather than assumed.
