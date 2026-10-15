# P50 kernel-platform compatibility (3.10 target)

Design note for backward compatibility of the P50 cache sidecar on kernels
that lack `pidfd_open`/`P_PIDFD` waitid, memfd sealing, or `getrandom`. The
reference deployment target is CentOS 7.3, `3.10.0-514.el7`, which the
production build hosts run (see
`~/workspace/icecream_builder/docs/kernel-3.10-sidecar.md` for the per-site
inventory this note builds on; its *fallback designs* are superseded here —
only its file:line inventory is used as a starting point and has been
independently re-verified against `c8bb615f`).

**Scope note (2026-10-05):** an earlier draft of this design carried a
fourth mitigation, M4 (`kcmp` fd proof), with its own env override
(`ICECC_P50_PLATFORM_FDPROOF`) and farm-matrix baseline requirement. That
mitigation has been dropped entirely after the investigation recorded below
showed `kcmp` is not on the production F path at all — there is nothing for
a 3.10 fallback to replace. The three mitigations below (M1, M2, M3) are
the complete scope of this branch.

Three independent mitigations, each a narrow backend selected once by the
daemon at startup:

| Mitigation | Native backend | Compat backend | Env override |
|---|---|---|---|
| M1 child handle | `pidfd_open` + `waitid(P_PIDFD)` | `waitid(P_PID, pid, WNOHANG\|WNOWAIT)` against the direct child's own zombie, discovered by the daemon's existing tick-driven reap sweep (no SIGCHLD handler added) | `ICECC_P50_PLATFORM_CHILD` |
| M2 input artifact | sealed `memfd` (4 seals) | daemon-private unlinked read-only copy | `ICECC_P50_PLATFORM_INPUT` |
| M3 random | `getrandom` | `/dev/urandom` | `ICECC_P50_PLATFORM_RANDOM` |

Shorthand: `ICECC_P50_PLATFORM=compat|native|auto` sets all three at once;
any of the three per-mitigation variables, if set, overrides the shorthand
for that mitigation only.

## Decision model

The daemon is the sole source of truth. It decides once, at startup, before
the sidecar is ever launched, and passes the three decisions explicitly in
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
or `/proc/version` to make a platform decision. This discipline is what
surfaced the kcmp finding below in the first place: a version check would
have reported "fine, kernel is new enough" while the production code was
never calling kcmp regardless of kernel version.

One startup log line summarizes all three decisions and their reasons, e.g.:

```
p50 platform: child=native(pidfd ok) input=native(seal round-trip ok) random=native(getrandom ok)
```

and the same three `(mitigation, backend, reason)` tuples are mirrored into
the state-jsonl/webgui state alongside the existing cache-service state, so
a farm operator can see at a glance which hosts are running which backend
without grepping logs.

## Why kcmp (M4) was dropped, not fallback-ed

This section records the investigation that led main to drop the fourth
mitigation outright, since the finding is central to the design and would
otherwise look like an unexplained scope cut.

The task's original audit requirement was: if any `kcmp` use site's
guarantee is not equivalent under inode identity once M2 compat is in play,
stop and report before weakening anything. `daemon/p50_fork_fd_hygiene.cpp`
(c8bb615f) has **two structurally different lease-minting functions**,
confirmed by grepping every caller of each:

* `mint_fork_source_lease()` (lines 383-422) — **called only from
  `unittests/p50_fork_fd_hygiene_test.cpp`**, never from `daemon/main.cpp`.
  Its `rejected` condition includes
  `proof_control != OpenFileComparison::Same`, where `proof_control =
  compare_open_file_description(owner.owned_proof_fd_, owner.control_fd_)`
  — i.e. it **hard-gates on kcmp succeeding**. If kcmp is `Unavailable`,
  this function unconditionally returns `std::nullopt`. This is the
  function `kernel-3.10-sidecar.md`'s "every proof fails -> the P50 source
  token is never delivered" claim describes, and it is accurate *for this
  function* — but this function is test-only.
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
  path** (`daemon/main.cpp:8135-8142`): `job->usesP50Input() &&
  !compiler_input_source.has_value()` logs `"P50 attached input failed
  compiler-fork validation"`, closes `compiler_input_fd`, and calls
  `handle_end(client, 146)` — the job fails with exit 146. But this path is
  reached only on a `capture_source_identity`/seal failure or a
  `source_identity_matches` mismatch (both M2-shaped failures), **never**
  because of kcmp, since `mint_attached_input_source` never calls it. kcmp
  Unavailable, by itself, leads to nothing on the real F path — it has no
  wire into this function at all.

**Finding, confirmed by main independently against c8bb615f:** kcmp is not
a 3.10 blocker on the production path at all, because the production path
never calls it. There is no fallback to design for M4, because there is no
production use of the primitive it would be falling back from. This is why
the mitigation is dropped rather than given a compat backend: a compat
backend for a check that never runs would be dead code with no caller to
justify it.

### Container-privilege provenance (why kcmp returns EPERM today anyway)

Even though kcmp is irrelevant to the production path, it was still worth
confirming why `kcmp` itself returns `EPERM` on H1-H4 today, since the
original task framed it as a kernel-version gap and that framing needed
correcting on its own terms. Moby's current default seccomp profile
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
version. This was verified directly against the live test farm, not just
read from the profile source. H4's running ad-hoc F container
(`mg-icecream:g4`, host kernel `6.14.0-37-generic`) has `CapAdd:
[CAP_SYS_ADMIN, CAP_SYS_CHROOT]` and no `CAP_SYS_PTRACE` (confirmed bit 19
absent from `CapEff` in `/proc/self/status` inside the container). A `kcmp`
probe compiled and run inside that exact container returns `-1`/`EPERM`:

```
kcmp ret=-1 errno=1 (Operation not permitted)
```

The versioned integration-test harness, `farmharness/integration/farmtest.py`
(~lines 744-768), grants F containers `--cap-add SYS_CHROOT` unconditionally
and adds `--cap-add SYS_PTRACE` **only** for one explicitly-scoped
fault-injection scenario, `S70-b4-scheduler-active-loss`, whose own inline
comment states why: *"The active-loss harness latches the compile-worker
fork in kernel ptrace state before the short-lived child can exit. No other
cell receives this fault-observation capability."* No `--privileged`, no
seccomp override, anywhere in that launch path. This is covered by harness
test assertions (`farmharness/integration/tests/test_dev_mixed.py:128`,
`test_dev_bootstrap.py:285`, `test_events.py:888,898`) that pin
`SYS_PTRACE`'s presence to that one scenario id. No surviving launch script
for the earlier (now torn down) comparison container from a prior campaign
was found, but it used the same ad-hoc launch mechanism as the still-running
comparison container checked above.

**Documented option, not applied:** granting `CAP_SYS_PTRACE` to F
containers would make `kcmp` itself succeed, but it would not change
`mint_attached_input_source`'s behavior by itself — that function has no
kcmp call to enable. Making the production path exercise kcmp would need a
separate code change on top of the capability grant, and this doc does not
propose that change. The grant is recorded here purely as a documented
option for the farm owner's future decision; it widens container
privileges, it is a farm-configuration decision outside this branch's
scope, and it is **not applied to any running or test container** in this
work.

### Seccomp-emulation requirement for the 3.10 surface

Even with M4 dropped, `farmharness/seccomp/linux-3.10-surface.json` must
still deny `kcmp` (`EPERM`, matching the real moby/CAP_SYS_PTRACE gate
rather than `ENOSYS`, so the emulation matches a real container's failure
mode and not a bare missing-syscall kernel). The reason to keep this in the
profile despite dropping the mitigation: it is the cleanest proof, in the
detection test, that nothing on the real F path regresses when kcmp is
unavailable — a seccomp profile that *allowed* kcmp would not exercise that
proof, since the real farm never allows it either.

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

Every probe has a corresponding test hook following the existing
`ICECC_P50_*_TEST_HOOKS` / `force_kcmp_errno` convention (e.g.
`force_pidfd_open_errno`, `force_memfd_create_errno`,
`force_seal_roundtrip_failure`, `force_getrandom_errno`), so every errno
and every partial-support combination is unit-testable on any kernel,
including C1's, without needing a 3.10 host or kernel-specific CI.

## What stays byte-for-byte unchanged

When a probe selects native, the native backend's code path is untouched by
this branch — no new branching is introduced into the native call sites
beyond "ask the platform-decision module which backend to use," which for
an all-native decision resolves to the exact function calls that exist in
`c8bb615f` today. Behavior-identity on an all-native decision is a
correctness requirement, not just a goal, and will be checked by object
identity (`.o` diff against `c8bb615f` for files where this is expected to
hold) during QA.

## What each compat backend weakens

* **M1**: the guarantee is not `still_same_process()` -- it is the zombie
  anchor. Our own direct child's zombie pins both its PID and its PGID
  until we explicitly reap it, so there is **no** PID-reuse window while
  that invariant holds, independent of any `/proc` re-check. The invariant
  has exactly two preconditions, both enforced, not merely hoped for:
  (a) every kill/killpg through the handle happens strictly before the
  consuming reap -- after the consume the handle is dead and a signal
  through it is a hard error; (b) nothing anywhere blindly reaps this
  child out from under the registry (no `wait(`, `waitpid(-1`,
  `waitid(P_ALL`, `waitid(P_PGID`, and no `SIG_IGN`/`SA_NOCLDWAIT` on
  `SIGCHLD`, in `daemon/` or `cache/`), checked both by a standing guard
  test and by a `sigaction(SIGCHLD)` check at decide-time (an installed
  `IGN` or `NOCLDWAIT` disposition sends the daemon `FailClosed`, not
  compat). `still_same_process()` (`(pid, starttime_ticks)` + a `/proc`
  re-check) is retained only as a defense-in-depth assert on top of the
  zombie anchor, not as the primary guarantee; it is not what closes the
  `pid_max`-wrap case, because the zombie anchor already closes it. The
  residual risk this leaves is a *future* blind reap introduced by code
  that does not yet exist -- the guard test above exists specifically to
  catch that before it ships, not to accept it as a standing window.
* **M2**: the cross-process *producer-immutability* proof (seals: the
  producer physically cannot mutate the bytes) becomes a permission-model
  argument. The compat identity check that `mint_attached_input_source`'s
  `source_identity_matches` must accept for a daemon-private file is, in
  full: device, inode, `nlink == 0` (the file is unlinked — nothing else
  can be holding it open by path), mode exactly `0400`, the captured size,
  and the fd open `O_RDONLY` — i.e. every field the sealed-memfd path
  checks except the seal bitmask itself, which compat replaces with the
  `nlink == 0` + mode-`0400` + `O_RDONLY` combination as the strongest
  available substitute for "nothing else can still write to this." A
  same-uid producer that kept a secret second writable descriptor open
  before dropping to this state could still mutate the bytes after the
  daemon's validation; this residual risk is unchanged from the seals case
  in every other respect and is mitigated the same way the seals path
  already is: the existing full-content digest re-validation in
  `validate_and_rewind`, plus the daemon (not the sidecar) being the sole
  writer of the compat file.
* **M3**: nothing meaningful — `getrandom`'s only advantage over
  `/dev/urandom` is early-boot blocking, irrelevant to a farm daemon.

## M1 reap-discovery mechanism (tick sweep, not a SIGCHLD self-pipe)

An earlier draft of this design proposed a dedicated SIGCHLD handler
(self-pipe pattern) as the compat substitute for `waitid(P_PIDFD, ...)`
readiness notification. That design is **superseded**: a SIGCHLD handler
in the main daemon process would put `EINTR` on every blocking syscall in
the daemon, which the native path never has to tolerate today. Instead,
compat reap discovery reuses the daemon's existing tick-driven sweep,
`CentralChildReaperRegistry::publish_pending()` /
`reap_one_unobserved()` (`cache/p50_sidecar_lifecycle.cpp`), which already
polls every registered owner once per daemon tick with a non-blocking,
non-consuming `waitid(..., WNOHANG|WNOWAIT)` probe. For a compat-backend
entry, that probe is `waitid(P_PID, pid, &info, WEXITED|WNOHANG|WNOWAIT)`
against the direct child's own zombie — the same zombie that anchors the
identity guarantee above — zeroing `siginfo_t` first and treating
`si_pid == 0` as "no change, try again next tick," per the kernel
behavior of `waitid()`/`wait4()`, which (unlike `waitpid()`'s documented
surface) accept `WNOWAIT` without `EINVAL`. The actual consuming reap is
the same call shape without `WNOWAIT`.

**Dependency this surfaced, not yet closed as of this writing:** the
sidecar reap-dispatch block in `daemon/main.cpp` (including the call to
`publish_pending()`) is itself gated on `cache_adapter->outer_pidfd() >=
0`. Under compat, if `outer_pidfd_` were left at its native `-1`
sentinel, this entire path — not just the pidfd-specific native probe —
would never run for the sidecar, and reap discovery would never happen at
all. The design direction to close this: give `outer_pidfd_` a
non-negative surrogate value under compat (the child's own `pid_t` cast
to `int`, which is always positive for a valid pid and satisfies every
existing `>= 0`/equality check already written against it, with no new
allocator or side-table), tag `Entry` and `ReapEvent` with a minimal
`ChildHandleBackend backend` field so `reap_one()`'s two call sites into
the pidfd-status probe can dispatch between the native `waitid(P_PIDFD,
...)` and the compat `waitid(P_PID, ...)` shape above.  (A third item of
the original direction — reject the direct-worker registration lane
outright under compat — is void as of the 2026-10-14 retirement: that
lane's dedicated registration API and the DirectWorker/AttemptLeaf
machinery it fed were removed from the tree, leaving only the reserved
`ReaperOwnerKind::DirectWorker` arm, which `register_owner()` already
refuses for the Compat backend.)
This is implementation in progress; it is recorded here before being
coded, not after, per this branch's reporting discipline for any point
where a compat path's guarantee needs a corrected mechanism to hold.

Still open, to be closed before this section is considered final:
measured bound on the daemon's per-tick poll timeout while a compat
sidecar is registered (so the discovery latency has a stated worst case,
not just "next tick"); confirmation that sidecar death also surfaces
through the control socket (EOF/EPIPE) independently of the reap sweep,
so a dead sidecar drives the adapter to `FailClosed` as a single state
transition rather than failing jobs one at a time while the sweep catches
up; and an H4-measured exit-to-detection latency figure once the surrogate
`outer_pidfd_` wiring lands and compiles there.
