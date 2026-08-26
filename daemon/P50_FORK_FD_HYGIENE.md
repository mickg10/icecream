# Exact first-fork descriptor hygiene

`iceccd` compiler workers are long-lived fork children. Before
`reset_debug()` or `work_it()` they must prove an exact inherited descriptor
set:

* legacy jobs keep exactly the result/statistics pipe and the exact `Client`
  channel;
* a P50 job may add exactly one accepted sealed source descriptor, bound to its
  nonzero `InputFdRequest::request_id` DeliveryId;
* every keep descriptor is distinct, owned by this process, of the expected
  type (FIFO, socket, regular file), and CLOEXEC-safe;
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

## ControlOperation boundary (HOLD)

The completed local cache `ControlOperation` is consumed inside the cache
adapter and is not retained in `Client`. The daemon carries only the exact
lease observation and descriptor. The `TOCOMPILE` edge therefore has a
fail-closed precondition: a future positive bridge that stores a completed
operation must erase it before this edge and before fork. That bridge is
explicitly HOLD; this slice makes no production daemon-to-sidecar bridge claim
and does not move synchronous transport/FD helpers into `iceccd`.
