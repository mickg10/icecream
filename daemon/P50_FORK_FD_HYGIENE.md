# Exact first-fork descriptor hygiene

`iceccd` compiler workers are long-lived fork children. Before
`reset_debug()` or `work_it()` they must prove an exact inherited descriptor
set:

* legacy jobs keep exactly the result/statistics pipe and the exact `Client`
  channel;
* a P50 job may add exactly one accepted sealed source descriptor, bound to its
  nonzero `InputFdRequest::request_id` DeliveryId through a move-only,
  owner-minted `ForkSourceLease`; a numeric DeliveryId is never authority;
  the lease explicitly separates its borrowed handoff FD from a private proof
  and control pair. Move-assignment and destruction retire only those private
  handles; the caller-owned handoff number is never closed by the lease. The
  mint boundary also disarms a rejected owner if a malformed private slot
  aliases the borrowed handoff, so natural owner destruction cannot close the
  caller descriptor. The test seam's proof observation slot is caller-owned;
  move-assignment preserves both destination and moved-from slot references,
  and the slot may be replaced at any time, including with a same-OFD `dup3`
  handoff;
* source handoffs are sealed immutable memfds: unsealed mutable regular files
  are rejected at mint, so a same-size content mutation cannot pass the
  mint-to-sweep identity checks.  The owned proof is reopened as a distinct
  kernel open-file description and paired with a hidden control handle;
  Linux `kcmp(KCMP_FILE)` proves the private proof/control pair and proves
  that neither private handle shares an OFD with the borrowed handoff at mint.
  If `kcmp` is unavailable or denied during retirement, both private handles
  are still retired while the caller's numeric-slot replacement is untouched;
* every keep descriptor is distinct, owned by this process, of the expected
  type (FIFO, connected stream socket, regular file), with the statistics pipe
  write-only, client socket read/write, source read-only, and all channels
  CLOEXEC-safe;
* Linux uses disjoint real-close `close_range` intervals. The CLOEXEC-only
  mode is not sufficient because the worker remains alive before its compiler
  subprocess starts;
* if the kernel backend is unavailable, the bounded `/proc/self/fd` fallback
  distinguishes enumeration, name-parse, and close failures;
* any failure is reported to the parent as an explicit I/O completion before
  `reset_debug()` or `work_it()`. The child never proceeds with a best-effort
  inventory.

The sweep changes only the child descriptor table. The parent retains its
unrelated control/listener descriptors and the child keeps the ordinary client
channel, so an ordinary client OP_CANCEL/EOF remains observable while the
worker is alive.

## Production attachment boundary

The completed local cache operation is consumed by the cache adapter and is
not retained as numeric authority in `Client`. At the `TOCOMPILE` boundary the
daemon verifies the complete attachment key/owner/request binding, then mints
the lease from the accepted descriptor and request ID
(`daemon/main.cpp:8113-8129`). `handle_connection` receives that move-only
lease (`daemon/serve.cpp:202-207`) and the fork hygiene sweep remains the
fail-closed child boundary. This document does not claim that a bare delivery
ID can mint a lease or that unrelated synchronous transport helpers moved into
`iceccd`.
