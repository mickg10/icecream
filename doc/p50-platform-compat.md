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

The task's audit requirement: if any `kcmp` use site's guarantee is not
equivalent under inode identity once M2 compat is in play, stop and report
before weakening anything. Every `kcmp(KCMP_FILE)` call site in
`daemon/p50_fork_fd_hygiene.cpp` was enumerated:

* `compare_open_file_description()` (syscall wrapper, `ENOSYS`/`EPERM`/
  `EINTR` → `Unavailable`).
* `retire_owned_proof()` — compares the daemon's own `proof_fd`/`control_fd`
  dups against each other.
* `mint_attached_input_source()` — `proof_fd`/`control_fd` are both
  `duplicate_owner_fd(fd)` of a single already-accepted input `fd` the
  daemon holds.
* `mint_fork_source_lease()` — three-way compare among
  `owner.owned_proof_fd_`, `owner.control_fd_`, and `fd`, all tracing back to
  that same single daemon-held `fd`.

**Finding: every kcmp comparison in this file is between descriptor numbers
that are dups or `/proc/self/fd` reopens of one fd the daemon itself already
holds and has already validated** — whether that fd originated as the
sidecar's sealed memfd (native) or the daemon's own M2-compat private file
(compat), the comparison is identical in kind: "are these two descriptor
numbers, both mine, still backed by the open-file description I think they
are, or has one been silently swapped via close+dup3 since I validated it."
M2 compat changes *where the file came from*, not *what is being compared*.
No STOP condition is triggered: M4's guarantee is unchanged by M2 compat,
and the two mitigations can be implemented and toggled independently.

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
Two consequences for the design:

1. The real-world security property P50 is already running under in
   production right now is `retire_owned_proof()`'s fail-safe handle
   retirement discipline, not the kcmp proof — the proof has already been
   silently `Unavailable` on every H1-H4 job today. M4 compat does not
   introduce a new weaker mode; it formalizes and names a mode the farm is
   already in.
2. `auto` for M4 should not expect to see `native` selected on H1-H4 unless
   `CAP_SYS_PTRACE` is granted to the F container. If the farm wants the
   kcmp proof to actually run (as opposed to running `Unavailable`-safe as
   it does today), that is a capability change to the F container's
   `docker run`/Dockerfile, independent of anything in this branch; absent
   that change, `fdproof=compat` or `fdproof=native(Unavailable)` are
   observationally the same thing today, and the startup log line will say
   so plainly rather than imply a difference that doesn't exist.

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
so the kcmp proof actually runs instead of running `Unavailable`-safe is a
farm-configuration question outside this branch's scope; it is flagged here
rather than acted on, since changing F container capabilities is not part
of the compat work and was not asked for.
